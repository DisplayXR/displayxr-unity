// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

using System.Reflection;
using System.Runtime.InteropServices;
using UnityEngine;

namespace DisplayXR
{
    /// <summary>
    /// Per-frame driver for the custom IUnityXRDisplay Display Provider (epic #166
    /// M2). Created/destroyed by <see cref="DisplayXRDisplayLoader"/> with the
    /// provider subsystem. Each frame it: (1) pushes the active rig's tunables +
    /// pose to the provider session (so a tracked face gets correct depth/parallax
    /// — the provider analog of the hook-path LateUpdate push), and (2) pumps the
    /// provider's mode/hardware/eye-tracking events into <see cref="DisplayXRProvider"/>.
    ///
    /// This driver is the provider's push path; it reuses the rigs'
    /// (<see cref="DisplayXRDisplay"/> / <see cref="DisplayXRCamera"/>) read-only
    /// GetProviderTunables() so the tunable math stays in one place.
    ///
    /// Runs late (DefaultExecutionOrder) so the active rig's LateUpdate has settled.
    /// </summary>
    [DefaultExecutionOrder(10000)]
    public sealed class DisplayXRProviderDriver : MonoBehaviour
    {
        static DisplayXRProviderDriver s_instance;

        /// <summary>
        /// True while the custom display provider is driving rendering. Rigs use this
        /// to disable provider-incompatible features — notably post-process AA, whose
        /// OnRenderImage Blit can't address the provider's 2-slice texture-array eye
        /// RT (garbage / white blocks) in either SPI or MultiPass (#166).
        /// </summary>
        public static bool IsActive { get; private set; }

        bool  m_SessionStarted;
        bool  m_QuitRequested;
        float m_DisplayHeightM;

        /// <summary>Create the singleton driver (idempotent). Called from the loader's Start.</summary>
        public static void EnsureInstance()
        {
            if (s_instance != null) return;
            var go = new GameObject("DisplayXRProviderDriver")
            {
                hideFlags = HideFlags.HideAndDontSave
            };
            DontDestroyOnLoad(go);
            s_instance = go.AddComponent<DisplayXRProviderDriver>();
            IsActive = true;
        }

        /// <summary>Destroy the singleton driver. Called from the loader's Stop.</summary>
        public static void DestroyInstance()
        {
            if (s_instance == null) return;
            Destroy(s_instance.gameObject);
            s_instance = null;
            IsActive = false;
        }

        void OnDestroy()
        {
            // The loader destroys the driver on Stop. If a session was up, that's the
            // falling edge too — LateUpdate won't run again to notice it.
            if (m_SessionStarted)
            {
                m_SessionStarted = false;
                DisplayXRProvider.OnSessionStopped();
            }
            DisplayXRDepthBudget.Reset();
        }

        void LateUpdate()
        {
#if UNITY_EDITOR_OSX || UNITY_STANDALONE_OSX
            // macOS (#204): pump xrPollEvent from the MAIN thread — the runtime's
            // poll drains NSApp events and flushes CATransaction (main-thread-only;
            // AppKit throws off-main). Windows keeps the graphics-thread pump.
            DisplayXRProviderNative.dxr_prov_poll_events();
#endif
            // #223: the shell's workspace close request reaches us as session EXITING. A
            // native OpenXR app just exits its loop on that event; a Unity app must be told —
            // quit when the runtime asks. Checked before the is-running gate (EXITING also
            // clears running). One-shot so the shutdown frames don't re-fire Quit().
            if (!m_QuitRequested && DisplayXRProviderNative.dxr_prov_exit_requested() != 0)
            {
                m_QuitRequested = true;
                Debug.Log("[DisplayXR] runtime requested session exit → Application.Quit()");
                Application.Quit();
                return;
            }
            if (DisplayXRProviderNative.dxr_prov_session_is_running() == 0)
            {
                // Falling edge: session lost / restarting (editor dock-undock) / stopping.
                if (m_SessionStarted)
                {
                    m_SessionStarted = false;
                    DisplayXRProvider.OnSessionStopped();
                    // A stale budget must not outlive its session: dock<->undock
                    // restarts the provider, and a remembered "Open" would leave the
                    // clip plane pushed back over a background nobody has measured.
                    DisplayXRDepthBudget.Reset();
                }
                return;
            }

            if (!m_SessionStarted)
            {
                m_SessionStarted = true;
                DisplayXRProviderNative.dxr_prov_get_display_info(out var di);
                m_DisplayHeightM = di.isValid != 0 ? di.heightM : 0f;
                DisplayXRProvider.OnSessionStarted();
            }

            DisplayXRProvider.PumpEvents();
            // Rear depth budget (#318): republish the last locate's advisory value
            // BEFORE the rigs render, so the foreground clip this frame uses this
            // frame's number. Cheap (one P/Invoke, no allocation) and inert on a
            // runtime without XR_DXR_depth_budget.
            DisplayXRDepthBudget.Poll();
            PushActiveRigTunables();
            PushGameViewRectEditorProbe();
        }

        // --- GameView-glue (Task (a), editor + texture probe only) ---------------
        // Glue the dedicated weave window to the Unity Game view's RENDER area so
        // window-relative Kooima + the weaver's lenticular phase track where the mirror-blit
        // shows the woven output, AND so the forced full-window zone (rendered tile size +
        // the runtime's woven region) is born at the panel's native resolution. Pure
        // reflection into UnityEditor.GameView so the Runtime asmdef needs no UnityEditor
        // reference (Type resolves to null in a built player → inert; gated to editor+probe).
        static System.Type  s_gvType;
        static FieldInfo    s_parentField;       // EditorWindow.m_Parent (HostView)
        static PropertyInfo s_screenPosProp;     // GUIView.screenPosition (Rect, LOGICAL points)
        static PropertyInfo s_pppProp;           // EditorGUIUtility.pixelsPerPoint (DPI scale)
        static MethodInfo   s_mainTargetSizeMethod; // static GetMainGameViewTargetSize (Vector2 px)
        static float        s_cachedPpp;         // stable DPI scale, cached once (>0 = valid)
        static bool         s_haveLastGood;      // last sane render rect (replay fallback)
        static int          s_lgX, s_lgY, s_lgW, s_lgH;
        static bool         s_loggedGlueOnce;
        static bool         s_loggedInitOnce;
        static PropertyInfo s_lowResProp;         // GameView "*lowResolution*" writable bool property
        static FieldInfo    s_lowResField;        // GameView "*lowResolution*"/"m_LowResolution*" bool field
        static bool         s_lowResResolved;     // member lookup done (found or not)
        static bool         s_loggedLowResOnce;

        // Dock-state detection (#740 auto-switch): the GameView instance the render rect
        // came from + the reflection chain View.window (ContainerWindow) → m_ShowMode.
        // ShowMode.MainWindow (4) == the view is docked in the main editor window.
        static object       s_matchedGameView;    // instance TryGetGameViewRenderRect matched
        static PropertyInfo s_hostWindowProp;     // View.window → ContainerWindow
        static FieldInfo    s_showModeField;      // ContainerWindow.m_ShowMode (int)
        static PropertyInfo s_showModeProp;       // fallback: ContainerWindow.showMode
        static bool         s_warnedDockDetect;
        static bool         s_lastAppliedDocked = true;
        /// <summary>Dock state the current session was bound for (watcher compares against this).</summary>
        public static bool LastAppliedDocked => s_lastAppliedDocked;

        // Force the editor Game view to render its mirror RT at FULL (physical) resolution
        // rather than the HiDPI "Low Resolution Aspect Ratios" mode. That toggle (ON by default
        // on a scaled display) makes Unity render the Game view at LOGICAL px (panel_physical /
        // ppp — e.g. 879x374 on a 250% 3840x2160 panel) and upscale for display; the woven
        // lenticular image then gets downsampled 1/ppp and re-upsampled, scrambling the per-
        // column L/R phase → wrong/soft 3D. OFF renders at physical px so the woven canvas maps
        // 1:1 to the panel (no resample). Pure reflection (Runtime asmdef has no UnityEditor ref);
        // inert in a built player. Editor + probe only. Member name varies by Unity version, so
        // it's found by substring; if none matches, the bool members are logged for diagnosis.
        static void ForceGameViewFullResolution()
        {
            if (!ProbeEnabled() || s_gvType == null) return;
            var wins = Resources.FindObjectsOfTypeAll(s_gvType);
            if (wins == null) return;
            if (!s_lowResResolved)
            {
                s_lowResResolved = true;
                var flags = BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic;
                var boolMembers = new System.Collections.Generic.List<string>();
                for (var t = s_gvType; t != null && s_lowResProp == null && s_lowResField == null; t = t.BaseType)
                {
                    foreach (var pr in t.GetProperties(flags | BindingFlags.DeclaredOnly))
                        if (pr.PropertyType == typeof(bool))
                        {
                            if (pr.CanWrite) boolMembers.Add("prop " + pr.Name);
                            if (pr.CanWrite && pr.Name.ToLowerInvariant().Contains("lowresolution")) { s_lowResProp = pr; break; }
                        }
                    if (s_lowResProp != null) break;
                    foreach (var fi in t.GetFields(flags | BindingFlags.DeclaredOnly))
                        if (fi.FieldType == typeof(bool))
                        {
                            boolMembers.Add("field " + fi.Name);
                            if (fi.Name.ToLowerInvariant().Contains("lowresolution")) { s_lowResField = fi; break; }
                        }
                    if (s_lowResField != null) break;
                }
                if (s_lowResProp == null && s_lowResField == null)
                    Debug.LogWarning("[DisplayXR] GameView: no 'lowResolution' member found on " +
                        s_gvType.FullName + " — bool members: " + string.Join(", ", boolMembers));
                else
                    Debug.Log("[DisplayXR] GameView: low-res member = " +
                        (s_lowResProp != null ? "prop " + s_lowResProp.Name : "field " + s_lowResField.Name));
            }
            if (s_lowResProp == null && s_lowResField == null) return;
            foreach (var w in wins)
            {
                try
                {
                    bool cur = s_lowResProp != null ? (bool)s_lowResProp.GetValue(w, null) : (bool)s_lowResField.GetValue(w);
                    if (!cur) continue; // already full-res
                    if (s_lowResProp != null) s_lowResProp.SetValue(w, false, null);
                    else s_lowResField.SetValue(w, false);
                    // Repaint via reflection (EditorWindow is a UnityEditor type the Runtime
                    // asmdef can't reference directly) so the RT is reallocated at full res.
                    var repaint = w.GetType().GetMethod("Repaint",
                        BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic,
                        null, System.Type.EmptyTypes, null);
                    if (repaint != null) repaint.Invoke(w, null);
                    if (!s_loggedLowResOnce)
                    {
                        s_loggedLowResOnce = true;
                        Debug.Log("[DisplayXR] GameView: forced Low Resolution Aspect Ratios OFF " +
                                  "(mirror renders at physical px so the woven lenticular maps 1:1).");
                    }
                }
                catch { /* member shape varies by Unity version; best-effort */ }
            }
        }

        // Diagnostic: log every GameView instance's maximized flag + host screenPosition +
        // the static GetMainGameViewTargetSize, but only when the combined signature changes.
        // Used to understand why a double-click maximize doesn't grow the mirror RT (the
        // maximized view is a separate instance the "main" target-size query doesn't report).
        static string s_lastGvSig;
        static PropertyInfo s_maximizedProp;
        static void LogGameViewInstancesOnChange()
        {
            if (!ProbeEnabled() || s_gvType == null) return;
            var wins = Resources.FindObjectsOfTypeAll(s_gvType);
            if (wins == null) return;
            Vector2 mainSize = Vector2.zero;
            if (s_mainTargetSizeMethod != null && s_mainTargetSizeMethod.Invoke(null, null) is Vector2 sv) mainSize = sv;
            if (s_maximizedProp == null)
                for (var t = s_gvType; t != null && s_maximizedProp == null; t = t.BaseType)
                    s_maximizedProp = t.GetProperty("maximized", BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic);
            var sb = new System.Text.StringBuilder();
            sb.Append($"mainTargetSize=({mainSize.x}x{mainSize.y}) instances={wins.Length}: ");
            foreach (var w in wins)
            {
                bool maxed = false;
                try { if (s_maximizedProp != null) maxed = (bool)s_maximizedProp.GetValue(w, null); } catch { }
                Rect sp = default; bool haveSp = false;
                try
                {
                    var pr = s_parentField != null ? s_parentField.GetValue(w) : null;
                    if (pr != null && s_screenPosProp != null) { sp = (Rect)s_screenPosProp.GetValue(pr, null); haveSp = true; }
                }
                catch { }
                sb.Append(haveSp ? $"[max={maxed} pos=({sp.x},{sp.y} {sp.width}x{sp.height})] " : $"[max={maxed} pos=?] ");
            }
            string sig = sb.ToString();
            if (sig != s_lastGvSig) { s_lastGvSig = sig; Debug.Log("[DisplayXR] GameView instances: " + sig); }
        }

        /// <summary>
        /// Editor GameView weave-to-texture is the DEFAULT (v2.8.0+): show the runtime's woven
        /// stereo INSIDE the Unity Game view instead of a separate external window. Editor-only
        /// (a built player uses the app-owned overlay); opt OUT with
        /// <c>DISPLAYXR_PROV_EXTERNAL_WINDOW=1</c> to restore the pre-2.8 external-window path.
        /// This is the single source of truth for the flip — the loader pushes it to native via
        /// <see cref="DisplayXRProviderNative.dxr_prov_set_texture_mode"/>, and the rig glue +
        /// re-host watcher gate on it.
        /// </summary>
        public static bool GameViewTextureModeEnabled()
        {
            if (!Application.isEditor) return false;
            if (s_texModeGate < 0)
                s_texModeGate = System.Environment.GetEnvironmentVariable(
                    "DISPLAYXR_PROV_EXTERNAL_WINDOW") == "1" ? 0 : 1;
            return s_texModeGate == 1;
        }
        static int s_texModeGate = -1;

        // Back-compat alias for the internal glue call sites (was the env-probe gate).
        static bool ProbeEnabled() => GameViewTextureModeEnabled();

        // Zone-glue arrangement (#740/#742, SUPERSEDED experiment — env
        // DISPLAYXR_PROV_GV_ZONEGLUE=1 opts back in): the weave window parked ONCE at the
        // monitor rect with the ZONE rect carrying the pane's screen offset. It did NOT fix
        // the docked phase error (both arrangements failed identically — the SR weaver
        // anchors to the DXGI window association, not the zone rect, #740), so the hybrid
        // (docked texture+child-glue / undocked present) runs on the legacy WINDOW-GLUE push
        // path — now the default (both HW-proven hybrid configs use it).
        static int s_zoneGlueGate = -1;
        static bool ZoneGlueEnabled()
        {
            if (!ProbeEnabled()) return false;
            if (s_zoneGlueGate < 0)
                s_zoneGlueGate = System.Environment.GetEnvironmentVariable(
                    "DISPLAYXR_PROV_GV_ZONEGLUE") == "1" ? 1 : 0;
            return s_zoneGlueGate == 1;
        }

        // BINDPANE experiment (#740, env DISPLAYXR_PROV_GV_BINDPANE=1; takes precedence
        // over zone-glue): bind UNITY'S OWN Game-view pane window (the matched GUIView
        // child) as the weave HWND — the SR SDK then tracks the real content window
        // natively (GA_ROOT = Unity's container, the normal windowed-SR-app shape) and
        // the zone carries the render area's offset within the pane's CLIENT rect.
        // The plugin never moves/restyles the pane (guarded natively too). Known limits
        // (experiment-grade): tab maximize/undock re-host the view in a DIFFERENT
        // GUIView HWND mid-session — the binding can't follow; docked-only test.
        static int s_bindPaneGate = -1;
        static bool BindPaneEnabled()
        {
            if (!ProbeEnabled()) return false;
            if (s_bindPaneGate < 0)
                s_bindPaneGate = System.Environment.GetEnvironmentVariable(
                    "DISPLAYXR_PROV_GV_BINDPANE") == "1" ? 1 : 0;
            return s_bindPaneGate == 1;
        }

        [StructLayout(LayoutKind.Sequential)]
        struct Win32Point { public int x, y; }
        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        struct Win32MonitorInfo
        {
            public int cbSize;
            public Win32Rect rcMonitor;
            public Win32Rect rcWork;
            public uint dwFlags;
        }
        [DllImport("user32.dll")] static extern System.IntPtr MonitorFromPoint(Win32Point pt, uint flags);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        static extern bool GetMonitorInfoW(System.IntPtr hMonitor, ref Win32MonitorInfo info);

        // The monitor rect (physical px) containing the given screen point — the panel the
        // Game view lives on, i.e. where the zone-glue weave window is born. Falls back to
        // (0,0,3840x2160)-style primary via MONITOR_DEFAULTTONEAREST semantics.
        static bool TryGetMonitorRect(int px, int py, out int mx, out int my, out int mw, out int mh)
        {
            mx = my = mw = mh = 0;
            if (Application.platform != RuntimePlatform.WindowsEditor) return false;
            try
            {
                var pt = new Win32Point { x = px, y = py };
                var mon = MonitorFromPoint(pt, 2 /* MONITOR_DEFAULTTONEAREST */);
                if (mon == System.IntPtr.Zero) return false;
                var mi = new Win32MonitorInfo { cbSize = Marshal.SizeOf<Win32MonitorInfo>() };
                if (!GetMonitorInfoW(mon, ref mi)) return false;
                mx = mi.rcMonitor.left; my = mi.rcMonitor.top;
                mw = mi.rcMonitor.right - mi.rcMonitor.left;
                mh = mi.rcMonitor.bottom - mi.rcMonitor.top;
                return mw > 0 && mh > 0;
            }
            catch { return false; }
        }

        // --- Stage-B seam diagnosis: geometry logging (env DISPLAYXR_PROV_GVGEOM=1) ---
        // One "GVGEOM" line whenever any watched value changes. Purpose: quantify the
        // point→pixel rounding disagreement suspected behind the maximize/resize midline
        // phase seam (#727 follow-up). Unity mixes TWO rounding rules on the same HiDPI
        // chain — DockArea RoundToPixelGrid = floor(v*ppp+0.48) vs GameView
        // Mathf.Round(v*ppp) — which can disagree by 1 physical px on non-default
        // (resized/floating/maximized) layout rects at fractional ppp (2.5). A 1-px
        // swapchain-vs-client-height mismatch engages the DirectFlip/MPO panel-fitter
        // stretch, which drops/duplicates one row at ~50% height = the observed seam.
        // Logged per GameView: host rect (logical points), BOTH rounding rules applied,
        // every RenderTexture field's size, zoom-area draw rect / targetInView, and the
        // Win32 truth: the best-matching editor child HWND (the GUIView swapchain
        // window) with its GetWindowRect/GetClientRect, plus deltaH = win32 client
        // height − each rounded host height. deltaH == ±1 only in seamed geometries
        // confirms the model; the fix must drive it to 0.
        static int s_gvGeomGate = -1;
        static string s_lastGeomSig;
        static FieldInfo s_zoomAreaField;
        static PropertyInfo s_zoomDrawRectProp;
        static System.Collections.Generic.List<FieldInfo> s_rtFields;
        static System.Collections.Generic.List<PropertyInfo> s_geomRectProps;
        static bool s_geomMembersResolved;

        static bool GvGeomEnabled()
        {
            if (!ProbeEnabled()) return false;
            if (s_gvGeomGate < 0)
                s_gvGeomGate = System.Environment.GetEnvironmentVariable(
                    "DISPLAYXR_PROV_GVGEOM") == "1" ? 1 : 0;
            return s_gvGeomGate == 1;
        }

        [StructLayout(LayoutKind.Sequential)]
        struct Win32Rect { public int left, top, right, bottom; }
        delegate bool Win32EnumProc(System.IntPtr hWnd, System.IntPtr lParam);
        [DllImport("user32.dll")] static extern bool EnumWindows(Win32EnumProc cb, System.IntPtr lParam);
        [DllImport("user32.dll")] static extern bool EnumChildWindows(System.IntPtr parent, Win32EnumProc cb, System.IntPtr lParam);
        [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(System.IntPtr hWnd, out uint pid);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetClassName(System.IntPtr hWnd, System.Text.StringBuilder name, int max);
        [DllImport("user32.dll")] static extern bool GetWindowRect(System.IntPtr hWnd, out Win32Rect r);
        [DllImport("user32.dll")] static extern bool GetClientRect(System.IntPtr hWnd, out Win32Rect r);
        [DllImport("user32.dll")] static extern bool IsWindowVisible(System.IntPtr hWnd);
        [DllImport("user32.dll")] static extern bool IsWindow(System.IntPtr hWnd);

        // --- (#263) Per-monitor DPI reconciliation -------------------------------
        // THE BUG THIS FIXES: the managed glue and the native weave window speak two
        // different DPI coordinate spaces, and nothing converted between them.
        //   * Native  — the weave window is created DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
        //     (displayxr_win32.c), so its SetWindowPos/GetWindowRect are TRUE physical
        //     virtual-desktop pixels.
        //   * Managed — this file never sets an awareness context, so every GetWindowRect /
        //     GetClientRect here reads in Unity's process awareness, where Windows
        //     VIRTUALIZES coordinates on any monitor whose scale differs from the primary's.
        // Virtualized == physical on the primary monitor, which is why every single-monitor
        // (and same-scale dual-monitor) rig worked and this went unseen. On a mixed-DPI pair
        // they diverge by the ratio of the two scale factors, and the weave window lands
        // mispositioned AND wrong-sized (field report: laptop 2560x1440 @200% primary + SR
        // 2560x1600 @150%; the glue computed a pane 1928 px tall for a 1600 px-tall panel).
        //
        // THE FIX: keep every existing computation in Unity's (virtualized) space — all the
        // #727/#740 sub-pixel work stays byte-identical — and convert ONCE at the boundary,
        // just before publishing to native. The conversion factor is MEASURED, not assumed:
        // k = physical pane width / virtualized pane width, both read from the same matched
        // pane HWND. On the primary monitor the two reads are identical, so k == 1.0 exactly
        // and this path is a mathematical no-op — the tested configurations cannot regress.
        [DllImport("user32.dll")] static extern System.IntPtr SetThreadDpiAwarenessContext(System.IntPtr ctx);
        [DllImport("user32.dll")] static extern System.IntPtr MonitorFromWindow(System.IntPtr hWnd, uint flags);
        [DllImport("user32.dll")] static extern int GetSystemMetrics(int index);

        static readonly System.IntPtr kPerMonitorAwareV2 = new System.IntPtr(-4);
        const int SM_XVIRTUALSCREEN = 76, SM_YVIRTUALSCREEN = 77;
        const int SM_CXVIRTUALSCREEN = 78, SM_CYVIRTUALSCREEN = 79;

        // Read a window's rects in the PER-MONITOR-AWARE space (i.e. the space native's
        // weave window lives in), restoring the caller's context immediately. Returns false
        // if the OS predates per-monitor-v2 (Win10 1703-) — callers then keep k = 1, which
        // is exactly the pre-#263 behavior.
        static bool TryGetPhysicalRects(System.IntPtr hwnd, out Win32Rect win, out Win32Rect client)
        {
            win = default; client = default;
            if (hwnd == System.IntPtr.Zero) return false;
            System.IntPtr prev = System.IntPtr.Zero;
            try { prev = SetThreadDpiAwarenessContext(kPerMonitorAwareV2); }
            catch { return false; }            // EntryPointNotFound on older Windows
            if (prev == System.IntPtr.Zero) return false;
            try { return GetWindowRect(hwnd, out win) && GetClientRect(hwnd, out client); }
            finally { SetThreadDpiAwarenessContext(prev); }
        }

        // Physical rect + measured scale of the matched pane, refreshed with the match.
        static Win32Rect s_paneVirtWin, s_panePhysWin, s_panePhysClient;
        static float s_paneScaleK = 1f;
        static bool  s_panePhysValid;
        static string s_lastDpiLog;
        static string s_lastCandidateLog;

        // --- GameView zoom-area sub-pixel fit: THE maximize/resize seam fix (#727 f-up) ---
        // Root cause (measured 2026-07-12, ~50 instrumented captures): the GameView
        // allocates its mirror RT at ceil(dest_px) rows, where dest_px is fractional —
        // the 21pt GameView toolbar at ppp 2.5 makes every render area X.5 px tall — and
        // draws the RT onto that 0.5-px-shorter dest. The resample's nearest-sampling
        // rounding tie crosses at ~50% of the height = a 1..3-row phase break at the RT
        // midline (the lenticular seam). Only docked layouts with client height ≡ 2
        // (mod 5) happened to align cleanly — which is why the default layout looked
        // fine and everything else seamed. FIX: set GameView.m_ZoomArea.m_Scale.y =
        // RTh/(RTh-0.5) each frame so dest rows == RT rows — an exact 1:1 vertical map
        // (x untouched). Verified clean (<1° interlace-phase anomaly, from 113°) in
        // docked (all five height residues), maximized, and floating, with reproducible
        // on/off/on toggles. DEFAULT ON in texture-probe mode. Env overrides
        // (DISPLAYXR_PROV_GV_FIT): "off" disables; "nudge:<pt>" sets m_Translation.y
        // instead; "file" re-reads the knob from %TEMP%\dxr_gv_fit.txt every 30 frames
        // (sweep values without relaunching).
        static string s_fitEnv;          // null = unresolved; default (unset env) = "scale"
        static string s_fitSpec;         // active spec ("scale" / "nudge:0.1" / "off")
        static int    s_fitFrame;
        static FieldInfo s_zoomScaleField, s_zoomTransField;
        static bool   s_zoomFieldsResolved;
        static string s_lastFitLog;

        static void ApplyGameViewFit()
        {
            if (!ProbeEnabled() || s_gvType == null) return;
            if (s_fitEnv == null)
            {
                s_fitEnv = System.Environment.GetEnvironmentVariable("DISPLAYXR_PROV_GV_FIT");
                if (string.IsNullOrEmpty(s_fitEnv)) s_fitEnv = "scale"; // default ON — the seam fix
            }

            // Resolve the active spec (file mode re-reads periodically).
            if (s_fitEnv == "file")
            {
                if ((s_fitFrame++ % 30) == 0)
                {
                    try
                    {
                        string p = System.IO.Path.Combine(System.IO.Path.GetTempPath(), "dxr_gv_fit.txt");
                        s_fitSpec = System.IO.File.Exists(p) ? System.IO.File.ReadAllText(p).Trim() : "off";
                    }
                    catch { s_fitSpec = "off"; }
                }
            }
            else s_fitSpec = s_fitEnv;
            if (string.IsNullOrEmpty(s_fitSpec)) return;

            var wins = Resources.FindObjectsOfTypeAll(s_gvType);
            if (wins == null) return;
            foreach (var w in wins)
            {
                try
                {
                    if (s_zoomAreaField == null)
                        for (var t = s_gvType; t != null && s_zoomAreaField == null; t = t.BaseType)
                            foreach (var fi in t.GetFields(BindingFlags.Instance | BindingFlags.Public |
                                                           BindingFlags.NonPublic | BindingFlags.DeclaredOnly))
                                if (fi.Name.ToLowerInvariant().Contains("zoomarea")) { s_zoomAreaField = fi; break; }
                    if (s_zoomAreaField == null) return;
                    var za = s_zoomAreaField.GetValue(w);
                    if (za == null) continue;
                    if (!s_zoomFieldsResolved)
                    {
                        s_zoomFieldsResolved = true;
                        for (var t = za.GetType(); t != null; t = t.BaseType)
                            foreach (var fi in t.GetFields(BindingFlags.Instance | BindingFlags.Public |
                                                           BindingFlags.NonPublic | BindingFlags.DeclaredOnly))
                            {
                                if (fi.FieldType != typeof(Vector2)) continue;
                                if (s_zoomScaleField == null && fi.Name.ToLowerInvariant().Contains("scale"))
                                    s_zoomScaleField = fi;
                                if (s_zoomTransField == null && fi.Name.ToLowerInvariant().Contains("translation"))
                                    s_zoomTransField = fi;
                            }
                        Debug.Log("[DisplayXR] GVFIT zoom fields: scale=" +
                                  (s_zoomScaleField != null ? s_zoomScaleField.Name : "MISSING") +
                                  " trans=" + (s_zoomTransField != null ? s_zoomTransField.Name : "MISSING"));
                    }

                    string applied = null;
                    if (s_fitSpec == "scale" && s_zoomScaleField != null)
                    {
                        // dest is 0.5 px short of the RT: stretch y so dest rows == RT rows.
                        // Resolve the GameView's RenderTexture fields here if the GVGEOM
                        // logger (which also builds this list) isn't enabled — the seam fix
                        // must not depend on a diagnostic env (it silently no-ops otherwise:
                        // rt stays null and the seam returns on maximized/floating layouts).
                        if (s_rtFields == null)
                        {
                            s_rtFields = new System.Collections.Generic.List<FieldInfo>();
                            var rtFlags = BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic;
                            for (var t = s_gvType; t != null; t = t.BaseType)
                                foreach (var fi in t.GetFields(rtFlags | BindingFlags.DeclaredOnly))
                                    if (fi.FieldType == typeof(RenderTexture)) s_rtFields.Add(fi);
                        }
                        RenderTexture rt = null;
                        if (s_rtFields != null)
                            foreach (var rfi in s_rtFields)
                                if (rfi.GetValue(w) is RenderTexture r && r != null) { rt = r; break; }
                        if (rt == null || rt.height <= 1) continue;
                        float target = rt.height / (rt.height - 0.5f);
                        var sc = (Vector2)s_zoomScaleField.GetValue(za);
                        if (Mathf.Abs(sc.y - target) > 1e-6f)
                        { sc.y = target; s_zoomScaleField.SetValue(za, sc); }
                        applied = $"scale.y={target:F7} (rt={rt.width}x{rt.height})";
                    }
                    else if (s_fitSpec.StartsWith("nudge:") && s_zoomTransField != null)
                    {
                        float pt = float.Parse(s_fitSpec.Substring(6),
                            System.Globalization.CultureInfo.InvariantCulture);
                        var tr = (Vector2)s_zoomTransField.GetValue(za);
                        if (Mathf.Abs(tr.y - pt) > 1e-6f)
                        { tr.y = pt; s_zoomTransField.SetValue(za, tr); }
                        applied = $"trans.y={pt:F3}pt";
                    }
                    else if (s_fitSpec == "off")
                    {
                        if (s_zoomScaleField != null)
                        {
                            var sc = (Vector2)s_zoomScaleField.GetValue(za);
                            if (Mathf.Abs(sc.y - 1f) > 1e-6f) { sc.y = 1f; s_zoomScaleField.SetValue(za, sc); }
                        }
                        if (s_zoomTransField != null)
                        {
                            var tr = (Vector2)s_zoomTransField.GetValue(za);
                            if (Mathf.Abs(tr.y) > 1e-6f) { tr.y = 0f; s_zoomTransField.SetValue(za, tr); }
                        }
                        applied = "off (restored)";
                    }
                    if (applied != null && applied != s_lastFitLog)
                    {
                        s_lastFitLog = applied;
                        Debug.Log("[DisplayXR] GVFIT applied: " + applied);
                    }
                }
                catch (System.Exception e)
                {
                    if (s_lastFitLog != "err")
                    { s_lastFitLog = "err"; Debug.LogWarning("[DisplayXR] GVFIT error: " + e.Message); }
                }
            }
        }

        static void LogGameViewGeometryOnChange()
        {
            if (!GvGeomEnabled() || s_gvType == null || s_parentField == null || s_screenPosProp == null) return;
            if (Application.platform != RuntimePlatform.WindowsEditor) return;
            var wins = Resources.FindObjectsOfTypeAll(s_gvType);
            if (wins == null || wins.Length == 0) return;

            if (!s_geomMembersResolved)
            {
                s_geomMembersResolved = true;
                var flags = BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic;
                s_rtFields = new System.Collections.Generic.List<FieldInfo>();
                s_geomRectProps = new System.Collections.Generic.List<PropertyInfo>();
                for (var t = s_gvType; t != null; t = t.BaseType)
                {
                    foreach (var fi in t.GetFields(flags | BindingFlags.DeclaredOnly))
                    {
                        if (fi.FieldType == typeof(RenderTexture)) s_rtFields.Add(fi);
                        if (s_zoomAreaField == null && fi.Name.ToLowerInvariant().Contains("zoomarea"))
                            s_zoomAreaField = fi;
                    }
                    foreach (var pr in t.GetProperties(flags | BindingFlags.DeclaredOnly))
                        if (pr.PropertyType == typeof(Rect))
                        {
                            var n = pr.Name.ToLowerInvariant();
                            if (n.Contains("targetinview") || n.Contains("viewinwindow"))
                                s_geomRectProps.Add(pr);
                        }
                }
            }

            float ppp = s_cachedPpp > 0f ? s_cachedPpp : 1f;
            float livePpp = 0f;
            try { if (s_pppProp != null && s_pppProp.GetValue(null, null) is float lf) livePpp = lf; } catch { }

            // Win32 truth: every visible window (top-level + descendants) of this process.
            var hwnds = new System.Collections.Generic.List<(System.IntPtr h, string cls, Win32Rect wr, Win32Rect cr)>();
            uint myPid = (uint)System.Diagnostics.Process.GetCurrentProcess().Id;
            Win32EnumProc collect = (h, l) =>
            {
                if (!IsWindowVisible(h)) return true;
                var nameSb = new System.Text.StringBuilder(64);
                GetClassName(h, nameSb, 64);
                // (#263) Never consider OUR OWN weave window. Without this the diagnostic
                // matched "DisplayXRProviderWindow" and printed a meaningless score
                // (observed: score=912.0) in exactly the misaligned state the capture was
                // taken to diagnose — the correction path at ApplyWin32HostPositionCorrection
                // has always had this exclusion; the diagnostic did not, which made the log
                // actively misleading precisely where it mattered.
                string cls0 = nameSb.ToString();
                if (cls0.StartsWith("DisplayXR")) return true;
                GetWindowRect(h, out var wr);
                GetClientRect(h, out var cr);
                hwnds.Add((h, cls0, wr, cr));
                return true;
            };
            Win32EnumProc top = (h, l) =>
            {
                GetWindowThreadProcessId(h, out uint pid);
                if (pid != myPid) return true;
                collect(h, l);
                EnumChildWindows(h, collect, System.IntPtr.Zero);
                return true;
            };
            try { EnumWindows(top, System.IntPtr.Zero); } catch { }

            var sb = new System.Text.StringBuilder();
            sb.Append($"ppp={ppp:F3} live={livePpp:F3} k={s_paneScaleK:F4}" +
                      (s_panePhysValid ? "" : "(unconv)") +
                      $" xrMode={UnityEngine.XR.XRSettings.gameViewRenderMode}");
            foreach (var w in wins)
            {
                Rect sp = default; bool haveSp = false;
                try
                {
                    var pr = s_parentField.GetValue(w);
                    if (pr != null) { sp = (Rect)s_screenPosProp.GetValue(pr, null); haveSp = true; }
                }
                catch { }
                if (!haveSp) { sb.Append(" | host=?"); continue; }

                // The two rounding rules Unity mixes (GameView vs DockArea).
                int rndH = Mathf.RoundToInt(sp.height * ppp);
                int rndW = Mathf.RoundToInt(sp.width * ppp);
                int f48H = Mathf.FloorToInt(sp.height * ppp + 0.48f);
                int f48W = Mathf.FloorToInt(sp.width * ppp + 0.48f);
                sb.Append($" | host=({sp.x:F2},{sp.y:F2} {sp.width:F2}x{sp.height:F2})pt " +
                          $"px(round)={rndW}x{rndH} px(f48)={f48W}x{f48H}");

                foreach (var fi in s_rtFields)
                {
                    try
                    {
                        if (fi.GetValue(w) is RenderTexture rt && rt != null)
                            sb.Append($" {fi.Name}={rt.width}x{rt.height}");
                    }
                    catch { }
                }
                try
                {
                    if (s_zoomAreaField != null)
                    {
                        var za = s_zoomAreaField.GetValue(w);
                        if (za != null)
                        {
                            if (s_zoomDrawRectProp == null)
                                for (var t = za.GetType(); t != null && s_zoomDrawRectProp == null; t = t.BaseType)
                                    s_zoomDrawRectProp = t.GetProperty("drawRect",
                                        BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic);
                            if (s_zoomDrawRectProp != null && s_zoomDrawRectProp.GetValue(za, null) is Rect dr)
                                sb.Append($" zoomDraw=({dr.x:F2},{dr.y:F2} {dr.width:F2}x{dr.height:F2})");
                        }
                    }
                    foreach (var rp in s_geomRectProps)
                        if (rp.GetValue(w, null) is Rect rr)
                            sb.Append($" {rp.Name}=({rr.x:F2},{rr.y:F2} {rr.width:F2}x{rr.height:F2})");
                }
                catch { }

                // Best-matching HWND for this GameView's host rect (physical px).
                float exX = sp.x * ppp, exY = sp.y * ppp, exW = sp.width * ppp, exH = sp.height * ppp;
                float bestScore = float.MaxValue;
                (System.IntPtr h, string cls, Win32Rect wr, Win32Rect cr) best = default;
                foreach (var e in hwnds)
                {
                    float score = Mathf.Abs(e.wr.left - exX) + Mathf.Abs(e.wr.top - exY)
                                + Mathf.Abs((e.wr.right - e.wr.left) - exW)
                                + Mathf.Abs((e.wr.bottom - e.wr.top) - exH);
                    if (score < bestScore) { bestScore = score; best = e; }
                }
                if (best.h != System.IntPtr.Zero)
                {
                    int winW = best.wr.right - best.wr.left, winH = best.wr.bottom - best.wr.top;
                    int cliW = best.cr.right - best.cr.left, cliH = best.cr.bottom - best.cr.top;
                    sb.Append($" hwnd[{best.cls}]=({best.wr.left},{best.wr.top} {winW}x{winH}) client={cliW}x{cliH}" +
                              $" deltaH(round)={cliH - rndH} deltaH(f48)={cliH - f48H} score={bestScore:F1}");
                }
            }

            string sig = sb.ToString();
            if (sig != s_lastGeomSig) { s_lastGeomSig = sig; Debug.Log("[DisplayXR] GVGEOM " + sig); }
        }

        // Docked-layout glue POSITION correction (#727 f-up, task 0B). In a DOCKED layout
        // `m_Parent.screenPosition × ppp` is NOT the true screen rect — the docked HostView
        // chain carries a border/coordinate-space discrepancy (measured 13 px in x at ppp 2.5)
        // — so the weave window was glued off the displayed pixels → a constant lenticular
        // phase error that changes as the editor window moves. Floating and maximized match
        // exactly. Win32 is the truth: find this process's visible HWND whose window rect
        // best matches the expected host rect (the GUIView swapchain child window, same
        // matcher GVGEOM validated) and take POSITION from it; SIZE stays mainSize-derived.
        // The enumeration reruns only when the reflection-derived host rect changes (a
        // steady layout costs nothing; a drag re-reads the moving truth each frame).
        static Rect   s_hwndCorrHostKey;
        static float  s_hwndCorrPppKey;
        static bool   s_hwndCorrValid;
        static int    s_hwndCorrX, s_hwndCorrY;
        static System.IntPtr s_hwndCorrHwnd;   // the matched pane GUIView (BINDPANE binds it)
        static string s_lastHwndCorrLog;
        static int    s_hwndCorrRetryTick;     // throttles re-enum retries while unmatched (#740)

        static void ApplyWin32HostPositionCorrection(Rect host, float ppp, int toolbar,
                                                     ref int rx, ref int ry)
        {
            if (Application.platform != RuntimePlatform.WindowsEditor) return;
            // (#740) Stale-pane detection: a layout reset destroys/recreates the GameView, so
            // the matched pane HWND can die while the reflection host rect (the re-enum key
            // below) stays IDENTICAL — without this the glue would keep publishing a dead HWND
            // (the native follow goes inert and the weave window sticks at its last rect).
            // Invalidate, park the native pane-follow (unsubclasses the old host), and force
            // a re-enumeration so the recreated pane is re-matched.
            if (s_hwndCorrHwnd != System.IntPtr.Zero && !IsWindow(s_hwndCorrHwnd))
            {
                s_hwndCorrValid = false;
                s_hwndCorrHwnd = System.IntPtr.Zero;
                s_hwndCorrHostKey = default; s_hwndCorrPppKey = 0f;
                s_hwndCorrRetryTick = 0;
                Debug.Log("[DisplayXR] GameView glue: matched pane HWND died (layout change) -> re-matching");
                DisplayXRProviderNative.displayxr_set_pane_follow(System.IntPtr.Zero, 0, 0, 0, 0);
            }
            // Re-enumerate on a key change, and RETRY (throttled — EnumWindows isn't free)
            // while unmatched: a failed match mid-layout-transition used to consume the key,
            // so a layout that settled at the same rect never re-tried and stayed stuck.
            bool retryUnmatched = !s_hwndCorrValid && (s_hwndCorrRetryTick++ % 10 == 0);
            if (host != s_hwndCorrHostKey || ppp != s_hwndCorrPppKey || retryUnmatched)
            {
                s_hwndCorrHostKey = host; s_hwndCorrPppKey = ppp;
                s_hwndCorrValid = false;
                // (#263) SCORE ON SIZE, NEVER POSITION.
                //
                // The old scorer summed |dleft|+|dtop|+|dwidth|+|dheight| against
                // host*ppp and accepted only <150. That rejects the CORRECT pane whenever
                // the Game view lives on a non-primary monitor: `ppp` is the pane's own
                // monitor scale, so the SIZE terms are right (delta ~ 0), but the POSITION
                // terms are wrong by the whole cross-monitor offset. Field capture: a pane
                // matching 3840x1949 exactly was thrown out on score=2560 — 17x the
                // threshold — purely on x. We were using the one measurement we cannot
                // compute across a scale boundary as a rejection criterion for the one we
                // can.
                //
                // Size is scale-correct by construction (both sides use the pane's own
                // monitor scale). Position is what we are trying to LEARN, so it must not
                // gate the match — it is read from the winner instead.
                float exW = host.width * ppp, exH = host.height * ppp;
                float bestScore = float.MaxValue; Win32Rect bestWr = default; string bestCls = null;
                System.IntPtr bestH = System.IntPtr.Zero;
                bool bestIsGuiView = false;
                uint myPid = (uint)System.Diagnostics.Process.GetCurrentProcess().Id;
                // GvGeomEnabled() RESOLVES the lazy gate; testing s_gvGeomGate directly
                // reads -1 (unresolved) whenever this runs before the diagnostic has,
                // which is always -- so the candidate log never printed at all. Reported
                // from the field as "zero hits in all five logs".
                var rejected = GvGeomEnabled() ? new System.Text.StringBuilder() : null;
                Win32EnumProc collect = (h, l) =>
                {
                    if (!IsWindowVisible(h)) return true;
                    var nameSb = new System.Text.StringBuilder(64);
                    GetClassName(h, nameSb, 64);
                    string cls = nameSb.ToString();
                    GetWindowRect(h, out var wr);
                    int cw = wr.right - wr.left, ch = wr.bottom - wr.top;
                    // Never match our own weave window: it is glued to the very rect we are
                    // validating, so it scores ~0 against our own (possibly wrong) output.
                    bool skip = cls.StartsWith("DisplayXR");
                    float score = Mathf.Abs(cw - exW) + Mathf.Abs(ch - exH);
                    rejected?.Append($" [{cls} ({wr.left},{wr.top} {cw}x{ch}) size-score={score:F0}{(skip ? " SKIP-own" : "")}]");
                    if (skip) return true;
                    // Prefer a real GUIView over any other same-sized window of ours.
                    bool isGuiView = cls.IndexOf("GUIView", System.StringComparison.OrdinalIgnoreCase) >= 0;
                    bool better = isGuiView != bestIsGuiView ? isGuiView : score < bestScore;
                    if (better) { bestScore = score; bestWr = wr; bestCls = cls; bestH = h; bestIsGuiView = isGuiView; }
                    return true;
                };
                Win32EnumProc top = (h, l) =>
                {
                    GetWindowThreadProcessId(h, out uint pid);
                    if (pid != myPid) return true;
                    collect(h, l);
                    EnumChildWindows(h, collect, System.IntPtr.Zero);
                    return true;
                };
                try { EnumWindows(top, System.IntPtr.Zero); } catch { return; }

                // Tolerance is on SIZE only, so it can be tight: a pane either is the one
                // Unity reported the dimensions of or it is not. 64px absorbs border/DPI
                // rounding without admitting a differently-sized window.
                bool accept = bestCls != null && bestScore < 64f;
                if (rejected != null)
                {
                    string rlog = $"expect {exW:F0}x{exH:F0} -> " +
                                  (accept ? $"MATCH {bestCls} score={bestScore:F0}" : "NO MATCH") +
                                  " | candidates:" + rejected;
                    if (rlog != s_lastCandidateLog) { s_lastCandidateLog = rlog; Debug.Log("[DisplayXR] GVGEOM pane candidates: " + rlog); }
                }
                if (accept)
                {
                    s_hwndCorrValid = true;
                    s_hwndCorrX = bestWr.left; s_hwndCorrY = bestWr.top;
                    s_hwndCorrHwnd = bestH;

                    // Ground truth in the space native's weave window actually lives in.
                    // Everything downstream reads THESE, not points x ppp.
                    s_panePhysValid = TryGetPhysicalRects(bestH, out s_panePhysWin, out s_panePhysClient);
                    if (s_panePhysValid)
                    {
                        int pw = s_panePhysWin.right - s_panePhysWin.left;
                        int vw = bestWr.right - bestWr.left;
                        s_paneScaleK = (vw > 64 && pw > 64) ? (float)pw / vw : 1f;
                        if (!(s_paneScaleK > 0.25f && s_paneScaleK < 4f)) s_paneScaleK = 1f;
                    }
                    else s_paneScaleK = 1f;

                    string log = $"{bestCls} virt=({bestWr.left},{bestWr.top} " +
                                 $"{bestWr.right - bestWr.left}x{bestWr.bottom - bestWr.top}) " +
                                 (s_panePhysValid
                                     ? $"phys=({s_panePhysWin.left},{s_panePhysWin.top} " +
                                       $"{s_panePhysWin.right - s_panePhysWin.left}x" +
                                       $"{s_panePhysWin.bottom - s_panePhysWin.top}) k={s_paneScaleK:F4}"
                                     : "phys=<unavailable>") +
                                 $" size-score={bestScore:F0}";
                    if (log != s_lastHwndCorrLog)
                    { s_lastHwndCorrLog = log; Debug.Log("[DisplayXR] GameView glue matched pane: " + log); }
                }
                else
                {
                    s_panePhysValid = false; s_paneScaleK = 1f;
                }
            }
            if (s_hwndCorrValid)
            {
                // (#263) No Mathf.Max(0, ...) here any more. The Windows virtual desktop is
                // SIGNED — a monitor arranged left of or above the primary has negative
                // coordinates — so clamping to zero silently dragged the weave window onto
                // the primary. (That clamp was NOT what the mixed-DPI field report hit: the
                // toolbar offset kept the pre-clamp value positive across all 2,580 GVGEOM
                // records of that capture. It is a separate latent bug, fixed here because
                // it produces an identical-looking symptom and would cost a whole debugging
                // cycle to rediscover.) The real bound is the virtual desktop, applied once
                // at the publish boundary below.
                rx = s_hwndCorrX;
                ry = s_hwndCorrY + toolbar;
            }
        }

        // (#263) The Game view's own render-area rect WITHIN the pane, in points.
        //
        // This is the authoritative answer to "where inside the pane do the woven pixels
        // actually land", and it is the one Unity itself draws to. It already accounts for
        // BOTH the toolbar strip and any letterbox padding, which is what makes it the
        // right source: the previously-derived `toolbar` term is computed as
        // (pane height - mainSize height), so under a FIXED aspect ratio it silently
        // absorbs the letterbox as well. Measured in the field: 141 px with Free Aspect
        // (the real toolbar) versus 349 px with a fixed 2560x1600 in a 3840x1949 pane --
        // the extra 208 being letterbox, mistaken for chrome. Deriving y and height from
        // that number put the render rect 208 px low and 208 px short, and left the width
        // at the full pane while the RT was only 2560 wide.
        //
        // Returns false when the member cannot be resolved (Unity version drift); callers
        // then keep the mainSize-derived behaviour, which is correct for Free Aspect.
        static bool TryGetPaneDrawRectPoints(out Rect drawPoints)
        {
            drawPoints = default;
            if (s_matchedGameView == null) return false;
            try
            {
                if (s_zoomAreaField == null)
                    for (var t = s_gvType; t != null && s_zoomAreaField == null; t = t.BaseType)
                        foreach (var fi in t.GetFields(BindingFlags.Instance | BindingFlags.NonPublic | BindingFlags.Public))
                            if (fi.Name.ToLowerInvariant().Contains("zoomarea")) { s_zoomAreaField = fi; break; }
                if (s_zoomAreaField == null) return false;
                var za = s_zoomAreaField.GetValue(s_matchedGameView);
                if (za == null) return false;
                if (s_zoomDrawRectProp == null)
                    for (var t = za.GetType(); t != null && s_zoomDrawRectProp == null; t = t.BaseType)
                        s_zoomDrawRectProp = t.GetProperty("drawRect",
                            BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic);
                if (s_zoomDrawRectProp == null) return false;
                if (!(s_zoomDrawRectProp.GetValue(za, null) is Rect dr)) return false;
                if (!(dr.width > 1f) || !(dr.height > 1f)) return false;
                drawPoints = dr;
                return true;
            }
            catch { return false; }
        }

        // (#740) RT-CENTRING offset — the docked-only interlace phase error, measured.
        // Unity draws the Game view's render target CENTRED inside the pane: the pane's
        // client is 1733 px wide while the RT is 1728 (5 px of dock chrome), so the woven
        // pixels land ~3 px right of the pane's left edge — while our invisible weave child
        // (the DP's phase anchor) sat AT the pane's left edge, because this code assumed a
        // zero left inset (only Y ever got a toolbar subtraction). The weaver then phases
        // for X and Unity paints at X+3: a constant interlace phase error that the eye can
        // never see directly, since the anchor paints nothing.
        // Maximized / floating have pane client == RT (no dock chrome) => offset 0, which is
        // exactly why those states were always correct.
        // Measured by cross-correlating the runtime's woven shared texture against the
        // screen (real scene content => non-periodic => unambiguous lag; fresh session so
        // the on-demand dump can't be stale): docked delta = +3 px (corr 0.96-0.98),
        // maximized delta = 0 px (corr 0.996). Round-half-up matches the measurement
        // (5 px of slack -> +3, i.e. Unity gives the extra pixel to the left).
        // (#740) LATCH-LAST-GOOD. The true centring offset is width-INVARIANT (Unity's dock
        // chrome — scrollbar/border — is a constant width, so slack = client_w − rt_w is a
        // constant to within ±1px of sub-pixel rounding wobble as logical points × ppp round
        // independently for the pane client vs the RT). Recomputing every frame therefore
        // JITTERED the published correction by ±1px at some widths (~0.36 cycle — "noticeable
        // but never inverted") and, worse, dropped to 0 on the transient slack≤0 reads during
        // an interactive resize's swapchain realloc (a 3px phase pop if you release right
        // then). So: compute a candidate, accept it only when plausible, and LATCH it — the
        // settled value rides through the churn unchanged. A fixed ≤0.18-cycle residual from
        // the inherent sub-pixel rounding remains (the correct integer genuinely alternates
        // 2/3 with sub-pixel phase); only the runtime canvas_offset can null that fractional
        // part. Stable-but-slightly-off beats jumpy.
        static int s_rtCentX = -1;   // -1 = never latched a plausible value
        static int RtCentringOffsetX(int rtWidth)
        {
            if (!s_hwndCorrValid || s_hwndCorrHwnd == System.IntPtr.Zero)
                return s_rtCentX < 0 ? 0 : s_rtCentX;
            if (!GetClientRect(s_hwndCorrHwnd, out var cr))
                return s_rtCentX < 0 ? 0 : s_rtCentX;
            int clientW = cr.right - cr.left;
            int slack = clientW - rtWidth;
            if (slack <= 0 || slack > 64)          // transient realloc / implausible match:
                return s_rtCentX < 0 ? 0 : s_rtCentX; // hold last good, never pop to 0
            int cand = (slack + 1) / 2;
            if (cand != s_rtCentX)
            {
                Debug.Log("[DisplayXR] #740 RtCentX latch " + s_rtCentX + "->" + cand
                          + " (clientW=" + clientW + " rtW=" + rtWidth + " slack=" + slack + ")");
                s_rtCentX = cand;
            }
            return s_rtCentX;
        }

        // (#740) VERTICAL sibling of the X centring. Our render-Y assumes the RT is
        // BOTTOM-aligned in the pane client (top = clientH − rtH, the toolbar). Unity
        // actually draws it with a small constant BOTTOM MARGIN — measured 4px on this panel
        // (H=660: RT top 113px below pane top, 4px slack below; H=860: same 4px) — so the
        // content sits `margin` px ABOVE our anchor. Through the SLANTED lens this vertical
        // misalignment becomes X-interlace phase (measured 0.288 px-of-phase per row here),
        // and a −4 anchor-Y correction lands the docked phase EXACTLY on the maximized
        // (correct) reference (−0.1849 vs −0.1848). Maximized/floating have no dock chrome →
        // the pane's own client already accounts for it → margin ≈ 0, which is why they were
        // always correct. Returns a NEGATIVE offset (move the anchor up to the content).
        // The bottom margin is constant to the integer; a ≤0.05-cycle sub-pixel residual
        // remains from ppp-rounding of clientH/rtH (fractional — only the runtime
        // canvas_offset can null it; eye-imperceptible). This is the honest, N-view-general
        // replacement for the stereo view-swap stopgap.
        // Measured RT bottom-margin: Unity draws the Game-view RT not flush to the pane
        // client's bottom but ~1.6pt above it (4px at ppp 2.5, HW-verified: a −4 anchor-Y
        // correction lands the docked interlace phase digit-for-digit on the maximized
        // reference). It is NOT derivable from the pane/RT/toolbar rects — those are all
        // self-consistent with bottom-alignment; this is the delta between Unity's real
        // toolbar and the clientH−rtH we assume. Expressed in points × ppp so it tracks
        // HiDPI. If a future Unity/DPI shifts docked phase, re-measure via the woven-texture
        // vs screen cross-correlation (δy) and update this constant.
        const float kRtBottomMarginPt = 1.6f;
        static int s_rtCentY = 0;   // latched (0 is the correct default: no margin until seen)
        static int RtCentringOffsetY(int rtHeight, float ppp)
        {
            if (!s_hwndCorrValid || s_hwndCorrHwnd == System.IntPtr.Zero) return s_rtCentY;
            if (!GetClientRect(s_hwndCorrHwnd, out var cr)) return s_rtCentY;
            int clientH = cr.bottom - cr.top;
            if (clientH <= rtHeight) return s_rtCentY; // transient realloc: hold last good
            int cand = -Mathf.RoundToInt(kRtBottomMarginPt * ppp);
            if (cand != s_rtCentY)
            {
                Debug.Log("[DisplayXR] #740 RtCentY latch " + s_rtCentY + "->" + cand
                          + " (clientH=" + clientH + " rtH=" + rtHeight + " ppp=" + ppp + ")");
                s_rtCentY = cand;
            }
            return s_rtCentY;
        }

        // Compute the visible Game view's RENDER-area rect in physical px. SIZE comes from
        // Unity's own GetMainGameViewTargetSize() (the render-target pixel size it uses to
        // allocate the main Game view — one stable value, no instance juggling). POSITION
        // comes from the HostView screenPosition of the GameView instance whose width MATCHES
        // that size (× a cached stable ppp, offset down by the toolbar). Insane readings are
        // rejected and the last-good rect returned instead, so a transient (0x0 / full-screen)
        // reading during a play/layout transition never borns a wrong-size zone (low-res
        // replay) or jumps the window over the editor (mouse-block). Returns false only if
        // nothing usable is available yet. Editor + probe only.
        static bool TryGetGameViewRenderRect(out int px, out int py, out int pw, out int ph,
                                             out string dbg)
        {
            px = py = pw = ph = 0; dbg = null;
            if (s_gvType == null)
            {
                s_gvType = System.Type.GetType("UnityEditor.GameView,UnityEditor");
                if (s_gvType == null) return false;
            }
            if (s_parentField == null)
                for (var t = s_gvType; t != null && s_parentField == null; t = t.BaseType)
                    s_parentField = t.GetField("m_Parent",
                        BindingFlags.Instance | BindingFlags.NonPublic | BindingFlags.Public);

            // Stable DPI scale, cached once. EditorGUIUtility.pixelsPerPoint flip-flops between
            // the true DPI and 1.0 across frames (context-dependent), so latch the first sane
            // value (>1.1) and reuse it — the editor DPI doesn't change during a session.
            if (s_cachedPpp <= 0f)
            {
                if (s_pppProp == null)
                {
                    var egui = System.Type.GetType("UnityEditor.EditorGUIUtility,UnityEditor");
                    if (egui != null)
                        s_pppProp = egui.GetProperty("pixelsPerPoint",
                            BindingFlags.Static | BindingFlags.Public | BindingFlags.NonPublic);
                }
                if (s_pppProp != null && s_pppProp.GetValue(null, null) is float f && f > 1.1f)
                    s_cachedPpp = f;
            }
            float ppp = s_cachedPpp > 0f ? s_cachedPpp : 1f;

            // SIZE: Unity's main Game view render-target size (physical px, excludes toolbar).
            if (s_mainTargetSizeMethod == null)
                for (var t = s_gvType; t != null && s_mainTargetSizeMethod == null; t = t.BaseType)
                    s_mainTargetSizeMethod = t.GetMethod("GetMainGameViewTargetSize",
                        BindingFlags.Static | BindingFlags.NonPublic | BindingFlags.Public,
                        null, System.Type.EmptyTypes, null)
                        ?? t.GetMethod("GetMainPlayModeViewTargetSize",
                        BindingFlags.Static | BindingFlags.NonPublic | BindingFlags.Public,
                        null, System.Type.EmptyTypes, null);
            Vector2 size = Vector2.zero;
            if (s_mainTargetSizeMethod != null && s_mainTargetSizeMethod.Invoke(null, null) is Vector2 sv)
                size = sv;

            // POSITION: the GameView instance whose physical width best matches `size` (ties
            // position to the same panel the size came from), else the largest-area instance.
            Rect host = default; bool haveHost = false; float bestScore = float.MaxValue, bestArea = -1f;
            Rect fallbackHost = default;
            object matchedWin = null, fallbackWin = null;
            var wins = (s_parentField != null) ? Resources.FindObjectsOfTypeAll(s_gvType) : null;
            if (wins != null)
                foreach (var w in wins)
                {
                    var pr = s_parentField.GetValue(w);
                    if (pr == null) continue;
                    // screenPosition lives on GUIView (the m_Parent/HostView type), not GameView.
                    if (s_screenPosProp == null)
                        for (var t = pr.GetType(); t != null && s_screenPosProp == null; t = t.BaseType)
                            s_screenPosProp = t.GetProperty("screenPosition",
                                BindingFlags.Instance | BindingFlags.NonPublic | BindingFlags.Public);
                    if (s_screenPosProp == null) continue;
                    var wr = (Rect)s_screenPosProp.GetValue(pr, null);
                    float area = wr.width * wr.height;
                    if (area > bestArea) { bestArea = area; fallbackHost = wr; fallbackWin = w; }
                    // GetMainGameViewTargetSize is in LOGICAL points, same units as
                    // screenPosition, so match panel width to it directly (ppp-independent).
                    if (size.x > 1f)
                    {
                        float score = Mathf.Abs(wr.width - size.x);
                        if (score < bestScore) { bestScore = score; host = wr; haveHost = true; matchedWin = w; }
                    }
                }
            if (!haveHost) { host = fallbackHost; haveHost = bestArea >= 0f; matchedWin = fallbackWin; }
            if (haveHost) s_matchedGameView = matchedWin;   // dock-state detection reads this (#740)

            // Compose the PHYSICAL render rect. GetMainGameViewTargetSize returns the render
            // area (excludes the toolbar) but its UNITS vary by project: LOGICAL points in
            // some, physical px in others (memory "GAP A") — a blind ×ppp is 2.5x wrong for the
            // physical-px projects. Disambiguate against the host rect (always LOGICAL points):
            // if size.x is closer to host.width it's logical (×ppp); if closer to
            // host.width*ppp it's already physical (×1). At ppp≈1 the two coincide. This is only
            // the BORN size; native converges to the authoritative mirror-RT panel px on frame 1.
            int rw, rh, rx, ry, toolbar;
            // (#263) Hoisted: the letterbox step below needs it to recover the RT's
            // physical pixel size, and it is otherwise scoped to the branch that sets it.
            float sizeScale = 1f;
            if (size.x > 1f && size.y > 1f)
            {
                sizeScale = ppp; // bring `size` to physical px; default = treat as points
                if (haveHost && host.width > 1f && ppp > 1.1f)
                {
                    // Per-frame disambiguation on purpose — do NOT latch this decision: the
                    // units legitimately flip within a session (mainSize is LOGICAL while
                    // "Low Resolution Aspect Ratios" is on and PHYSICAL once our force turns
                    // it off), and a latch turned the healed post-layout-reset readings into
                    // ×ppp-doubled rects (#740). Truly corrupt readings (Unity's recreated
                    // GameView double-scales after an in-Play layout reset) are rejected by
                    // the pane-client cross-check below, not here.
                    float dLogical  = Mathf.Abs(size.x - host.width);       // size already in points
                    float dPhysical = Mathf.Abs(size.x - host.width * ppp); // size already in px
                    // (#263) STRICT `<`, so an exact tie falls to PHYSICAL. A tie is not
                    // hypothetical: a Game view pinned to a fixed resolution sitting exactly
                    // midway between the pane's point width and its physical width produces
                    // dLogical == dPhysical == 1280 (measured: size 2560, host 1280pt at
                    // ppp 3.0). The old `<=` then multiplied an already-physical 2560x1600
                    // by 3.0 -> 7680x4800, which the monitor clamp truncated to the panel
                    // size — wrong, but plausible-looking. Scaling an already-physical value
                    // is the worse failure of the two, so the tie must not land there.
                    sizeScale = (dLogical < dPhysical) ? ppp : 1f;
                }
                rw = Mathf.RoundToInt(size.x * sizeScale);
                rh = Mathf.RoundToInt(size.y * sizeScale);
                // Toolbar = full panel height − render height, both in physical px (units-safe
                // regardless of `size`'s original units, unlike the old (host−size)×ppp).
                toolbar = Mathf.Max(0, Mathf.RoundToInt(host.height * ppp) - rh);
            }
            else
            {
                rw = Mathf.RoundToInt(host.width * ppp);
                rh = Mathf.RoundToInt(host.height * ppp);
                toolbar = 0;
            }
            // (#263) No Mathf.Max(0, ...) — the virtual desktop is signed. See the note in
            // ApplyWin32HostPositionCorrection; the bound is applied once at the boundary below.
            rx = Mathf.RoundToInt(host.x * ppp);
            ry = Mathf.RoundToInt(host.y * ppp) + toolbar;
            ApplyWin32HostPositionCorrection(host, ppp, toolbar, ref rx, ref ry);
            // (#740) The RT-placement corrections apply ONLY to a docked, non-maximized
            // Game view — the only state with dock chrome. Maximized and floating draw the
            // RT flush (δ already (0,0)); correcting them shifts a correct weave (maximized
            // went inverted when the Y margin was applied there). s_lastAppliedDocked
            // excludes floating/present; the maximized flag excludes the maximized sub-state.
            // (#263) Applied here only for the UNMATCHED fallback — the matched path below
            // re-applies them on top of the pane's own physical rect, so doing it in both
            // places would double-count.
            if (!(s_panePhysValid && s_hwndCorrValid) &&
                s_lastAppliedDocked && !IsMatchedGameViewMaximized())
            {
                rx += RtCentringOffsetX(rw);       // Unity centres the RT horizontally
                ry += RtCentringOffsetY(rh, ppp);  // …and draws it with a bottom margin
            }

            // ============ (#263) TAKE GROUND TRUTH FROM THE MATCHED PANE ============
            // Everything above derived the rect from Unity's points x a single global ppp.
            // That is correct only while the pane sits on the primary monitor; the moment
            // the origin crosses onto a monitor at a different scale, the POSITION is wrong
            // by the whole cross-monitor offset (measured: computed x=5120, true x=2560).
            //
            // When we have matched the pane we do not need any of that arithmetic: its
            // physical rect, read under PER_MONITOR_AWARE_V2, is the space native's weave
            // window lives in. Take BOTH position and size from it.
            //
            // Size matters as much as position here: a field capture with a fixed-resolution
            // Game view matched the pane, positioned it correctly, and STILL rendered wrong
            // — 2560x1600 of content drawn inside a 3840x1949 pane (~67% linear) — because
            // size still came from GetMainGameViewTargetSize. Taking only position would
            // have "fixed" that capture into looking correct while staying wrong.
            if (s_panePhysValid && s_hwndCorrValid)
            {
                int pcw = s_panePhysClient.right - s_panePhysClient.left;
                int pch = s_panePhysClient.bottom - s_panePhysClient.top;
                if (pcw > 64 && pch > 64)
                {
                    // Points -> physical for THIS pane, measured from its own two rects.
                    float ptToPx = (host.width > 1f)
                        ? (s_panePhysWin.right - s_panePhysWin.left) / host.width
                        : ppp;

                    // (#263) THE TOOLBAR, taken from Unity's own draw rect — and ONLY
                    // the toolbar. Field data on three runs pinned down exactly what that
                    // rect does and does not carry:
                    //   host.height - draw.height  ==  the TRUE toolbar (measured 47.00 pt
                    //     = 141 px), and it stays correct under a fixed aspect ratio.
                    //   draw.y                     ==  21.00 pt, which is NOT the toolbar
                    //     and is not an offset we want; using it put every config 78 px
                    //     high (47 - 21 = 26 pt = 78 px at this scale).
                    //   draw.width/height          ==  unchanged by a fixed aspect, so the
                    //     rect carries no letterbox information at all.
                    // So: use it for the toolbar, and never for placement. The previously
                    // derived toolbar (pane height - mainSize height) is what silently
                    // absorbed the letterbox; this one cannot, because mainSize is not in it.
                    int toolbarPhys;
                    if (TryGetPaneDrawRectPoints(out Rect draw) && host.height > draw.height)
                        toolbarPhys = Mathf.RoundToInt((host.height - draw.height) * ptToPx);
                    else
                        toolbarPhys = Mathf.RoundToInt(toolbar * s_paneScaleK);
                    if (toolbarPhys < 0) toolbarPhys = 0;
                    if (toolbarPhys > pch / 2) toolbarPhys = 0;

                    // The pane's render area: the client, inset from the top by the toolbar.
                    rx = s_panePhysWin.left;
                    ry = s_panePhysWin.top + toolbarPhys;
                    rw = pcw;
                    rh = pch - toolbarPhys;

                    // (#263) LETTERBOX. With a fixed aspect ratio selected, Unity fits the
                    // render target into that area preserving ITS aspect and centres it,
                    // leaving grey bars — so the woven pixels occupy less than the render
                    // area. Free Aspect makes the RT track the pane, the ratios match, and
                    // this collapses to a no-op. Derived rather than read, because the draw
                    // rect demonstrably does not carry it.
                    if (size.x > 1f && size.y > 1f && rw > 0 && rh > 0)
                    {
                        // Unity NEVER UPSCALES the render target past 1:1 here — when the
                        // RT is smaller than the render area it is drawn at its native size
                        // and centred, leaving grey bars, rather than stretched to fill.
                        // Hence the min(1, ...): a pure fit-to-area was measured drawing a
                        // 2560-wide RT into a 2893-wide rect, a uniform 1.13x enlargement
                        // that placed correctly and still looked wrong. The scale is applied
                        // to BOTH axes together so the aspect cannot drift.
                        int rtW = Mathf.RoundToInt(size.x * sizeScale);
                        int rtH = Mathf.RoundToInt(size.y * sizeScale);
                        if (rtW > 0 && rtH > 0)
                        {
                            float fit = Mathf.Min(1f, Mathf.Min((float)rw / rtW, (float)rh / rtH));
                            int fitW = Mathf.RoundToInt(rtW * fit);
                            int fitH = Mathf.RoundToInt(rtH * fit);
                            if (fitW > 0 && fitH > 0 && (fitW != rw || fitH != rh))
                            {
                                rx += (rw - fitW) / 2;
                                ry += (rh - fitH) / 2;
                                rw = fitW;
                                rh = fitH;
                            }
                        }
                    }

                    // The RT-centring corrections (#740) are expressed in the same physical
                    // px as the client rect they were measured from, so they still apply.
                    if (s_lastAppliedDocked && !IsMatchedGameViewMaximized())
                    {
                        rx += RtCentringOffsetX(rw);
                        ry += RtCentringOffsetY(rh, ppp);
                    }
                }
            }

            // (#740/#263) Clamp to the monitor the PANE is on. Resolve it from the matched
            // pane HWND rather than from a computed centre point: the centre point is derived
            // from the very rect we are validating, so on a mixed-DPI rig a wrong rect
            // selected the wrong monitor and then "clamped" against it — a second-order
            // failure that would have survived the scale fix. Falls back to the point probe
            // when there is no matched pane yet.
            int monW = 0, monH = 0;
            bool haveMon = false;
            if (s_hwndCorrValid && s_hwndCorrHwnd != System.IntPtr.Zero)
            {
                var mon = MonitorFromWindow(s_hwndCorrHwnd, 2 /* MONITOR_DEFAULTTONEAREST */);
                if (mon != System.IntPtr.Zero)
                {
                    var mi = new Win32MonitorInfo { cbSize = Marshal.SizeOf<Win32MonitorInfo>() };
                    System.IntPtr prevCtx = System.IntPtr.Zero;
                    try { prevCtx = SetThreadDpiAwarenessContext(kPerMonitorAwareV2); } catch { }
                    try
                    {
                        if (GetMonitorInfoW(mon, ref mi))
                        {
                            monW = mi.rcMonitor.right - mi.rcMonitor.left;
                            monH = mi.rcMonitor.bottom - mi.rcMonitor.top;
                            haveMon = monW > 0 && monH > 0;
                        }
                    }
                    finally { if (prevCtx != System.IntPtr.Zero) SetThreadDpiAwarenessContext(prevCtx); }
                }
            }
            if (!haveMon)
                haveMon = TryGetMonitorRect(rx + rw / 2, ry + rh / 2, out _, out _, out monW, out monH);
            if (haveMon)
            {
                if (rw > monW) rw = monW;
                if (rh > monH) rh = monH;
            }

            // (#263) The real positional bound: the virtual desktop, which is SIGNED. This
            // replaces the old Mathf.Max(0, ...) clamps, which pinned any monitor left of or
            // above the primary to the primary's top-left corner.
            {
                int vsx = GetSystemMetrics(SM_XVIRTUALSCREEN), vsy = GetSystemMetrics(SM_YVIRTUALSCREEN);
                int vsw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vsh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
                if (vsw > 0 && vsh > 0)
                {
                    rx = Mathf.Clamp(rx, vsx - rw, vsx + vsw);
                    ry = Mathf.Clamp(ry, vsy - rh, vsy + vsh);
                }
            }

            // (#740) Pane-client cross-check (Win32 truth): the render area can never exceed
            // the matched pane window's client rect. Unity's RECREATED GameView after an
            // in-Play layout reset is corrupted — its view rect reports physical px as points,
            // so Unity itself allocates a ×ppp-doubled target (GVGEOM: viewInWindow 1532x769
            // "pt" inside a 693pt host → m_TargetTexture 3830x1923) and mainSize follows.
            // Those readings are internally consistent, under the monitor bound, and steady —
            // only the pane client exposes them. Reject → last-good (the re-host watcher
            // heals the view via maximize/restore and restarts the subsystem).
            bool paneFit = true;
            // (#263) Compare against the pane client in the SAME space rw/rh now carry:
            // physical when the conversion ran, virtualized otherwise.
            Win32Rect pcr = default; bool havePcr;
            if (s_panePhysValid && s_hwndCorrValid) { pcr = s_panePhysClient; havePcr = true; }
            else havePcr = s_hwndCorrValid && s_hwndCorrHwnd != System.IntPtr.Zero &&
                           GetClientRect(s_hwndCorrHwnd, out pcr);
            if (havePcr)
            {
                int cw = pcr.right - pcr.left, ch = pcr.bottom - pcr.top;
                if (cw > 64 && ch > 64 && (rw > cw + 8 || rh > ch + 8))
                    paneFit = false;
            }

            // Sanity: reject implausible SIZE readings (transient 0x0 during a play/layout
            // transition) and reuse the last-good rect so the zone never borns wrong-size.
            // The zone size comes from GetMainGameViewTargetSize (ppp-independent), so a
            // not-yet-latched ppp only offsets the initial POSITION (the per-frame glue
            // corrects it once ppp latches) — don't gate the zone on ppp.
            bool sane = haveHost && size.x > 1f && size.y > 1f && paneFit
                        && rw >= 256 && rh >= 256 && rw <= 8192 && rh <= 8192;
            if (sane)
            {
                s_haveLastGood = true; s_lgX = rx; s_lgY = ry; s_lgW = rw; s_lgH = rh;
                px = rx; py = ry; pw = rw; ph = rh;
                dbg = $"mainSize=({size.x}x{size.y}) host(points)=({host.x},{host.y} {host.width}x{host.height}) " +
                      $"ppp={ppp:F3} toolbar={toolbar} -> render=({px},{py} {pw}x{ph})";
                return true;
            }
            if (s_haveLastGood)
            {
                px = s_lgX; py = s_lgY; pw = s_lgW; ph = s_lgH;
                dbg = $"INSANE reading (mainSize={size.x}x{size.y} render={rx},{ry} {rw}x{rh} ppp={ppp:F3}) " +
                      $"-> reuse last-good ({px},{py} {pw}x{ph})";
                return true;
            }
            dbg = $"no sane rect yet (mainSize={size.x}x{size.y} host={host.width}x{host.height} ppp={ppp:F3})";
            return false;
        }

        // (#740) DOCK-BORDER probe — the suspected root of the docked-only phase error.
        // Our render-area X is the matched pane HWND's left with a ZERO left inset assumed
        // (we only ever compute a toolbar for Y). But Unity's DockArea INSETS its hosted
        // window by a border, and EditorWindow.position is defined as
        // `m_Parent.borderSize.Remove(m_Parent.screenPosition)` — i.e. position is the
        // content rect with the border REMOVED, while m_Parent.screenPosition (and the
        // GUIView HWND that matches it) still include it. DockArea suppresses the border
        // where the view meets the container edge — which is exactly the maximized and
        // floating cases (correct), versus a mid-layout docked view (wrong). If the content
        // is drawn N px right of our invisible anchor, the weaver phases for the anchor
        // while the pixels land at anchor+N: a pure interlace-phase error, invisible to the
        // eye because the anchor never paints. This logs the border directly.
        static PropertyInfo s_posProp;
        static string s_lastBorderLog;
        static void LogDockBorderOnChange(float ppp, int rx, int ry, int rw, int rh)
        {
            if (s_matchedGameView == null || s_parentField == null || s_screenPosProp == null) return;
            try
            {
                if (s_posProp == null)
                    for (var t = s_matchedGameView.GetType(); t != null && s_posProp == null; t = t.BaseType)
                        s_posProp = t.GetProperty("position",
                            BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic);
                if (s_posProp == null) return;
                var pos = (Rect)s_posProp.GetValue(s_matchedGameView, null);   // points, border REMOVED
                var parent = s_parentField.GetValue(s_matchedGameView);
                if (parent == null) return;
                var host = (Rect)s_screenPosProp.GetValue(parent, null);       // points, border INCLUDED
                float borderXpx = (pos.x - host.x) * ppp;                      // the inset we ignore
                float borderYpx = (pos.y - host.y) * ppp;
                string log = $"gv.position=({pos.x:F1},{pos.y:F1} {pos.width:F1}x{pos.height:F1})pt " +
                             $"host.screenPos=({host.x:F1},{host.y:F1} {host.width:F1}x{host.height:F1})pt " +
                             $"ppp={ppp:F3} | position*ppp=({pos.x * ppp:F1},{pos.y * ppp:F1}) " +
                             $"hwnd=({s_hwndCorrX},{s_hwndCorrY}) ourRender=({rx},{ry} {rw}x{rh}) | " +
                             $"BORDER px: x={borderXpx:F1} y={borderYpx:F1} | " +
                             $"anchor-vs-content X error = {pos.x * ppp - rx:F1}px";
                if (log != s_lastBorderLog)
                {
                    s_lastBorderLog = log;
                    Debug.Log("[DisplayXR] #740 dock-border: " + log);
                }
            }
            catch { }
        }

        // Dock-state detection (#740 auto-switch): is the (matched) Game view docked in the
        // MAIN editor window? Reflection chain: GameView.m_Parent (HostView) → View.window
        // (ContainerWindow) → m_ShowMode; ShowMode.MainWindow (4) ⟺ docked. Chosen over the
        // Win32 GA_ROOT==find_unity_hwnd comparison because find_unity_hwnd prefers
        // GetForegroundWindow — a focused floating Game view would misdetect as docked.
        // Returns false when the chain can't be resolved (editor internals changed) —
        // the caller falls back to docked/texture.
        public static bool TryDetectGameViewDocked(out bool docked)
        {
            docked = true;
            if (s_matchedGameView == null)
                TryGetGameViewRenderRect(out _, out _, out _, out _, out _);
            var gv = s_matchedGameView;
            if (gv == null || s_parentField == null) return false;
            try
            {
                var parent = s_parentField.GetValue(gv);   // HostView/DockArea
                if (parent == null) return false;
                if (s_hostWindowProp == null)
                    for (var t = parent.GetType(); t != null && s_hostWindowProp == null; t = t.BaseType)
                    {
                        var p = t.GetProperty("window",
                            BindingFlags.Instance | BindingFlags.NonPublic | BindingFlags.Public);
                        if (p != null && p.PropertyType.Name == "ContainerWindow") s_hostWindowProp = p;
                    }
                if (s_hostWindowProp == null) return false;
                var container = s_hostWindowProp.GetValue(parent, null);
                if (container == null) return false;
                if (s_showModeField == null && s_showModeProp == null)
                    for (var t = container.GetType(); t != null; t = t.BaseType)
                    {
                        s_showModeField = t.GetField("m_ShowMode",
                            BindingFlags.Instance | BindingFlags.NonPublic | BindingFlags.Public);
                        if (s_showModeField != null) break;
                        s_showModeProp = t.GetProperty("showMode",
                            BindingFlags.Instance | BindingFlags.NonPublic | BindingFlags.Public);
                        if (s_showModeProp != null) break;
                    }
                int mode;
                if (s_showModeField != null)
                    mode = System.Convert.ToInt32(s_showModeField.GetValue(container));
                else if (s_showModeProp != null)
                    mode = System.Convert.ToInt32(s_showModeProp.GetValue(container, null));
                else return false;
                docked = mode == 4; // UnityEditor.ShowMode.MainWindow
                return true;
            }
            catch { return false; }
        }

        // (#740 auto-switch) Fresh dock-state read for the transition watcher: re-match the
        // live GameView instance first (a dock/undock re-hosts the view — possibly into a
        // NEW instance — and a stale stash would read the old container), then detect.
        public static bool TryDetectDockStateFresh(out bool docked)
        {
            docked = true;
            if (!ProbeEnabled()) return false;
            if (!TryGetGameViewRenderRect(out _, out _, out _, out _, out _)) return false;
            return TryDetectGameViewDocked(out docked);
        }

        // (#740 auto-switch) One-shot dock-state override consumed by the NEXT
        // ApplyDockModeForSessionStart — the %TEMP%\dxr_dock_toggle test hook uses it to
        // exercise the full Stop→Start re-bind with a flipped mode without a physical
        // undock (real detection would just re-bind the same mode).
        static int s_forcedDockState = -1; // -1 none, 0 undocked, 1 docked
        public static void ForceNextDockState(bool docked) { s_forcedDockState = docked ? 1 : 0; }

        // (#740 auto-switch) Called by the loader BEFORE the subsystem (re)starts: detect the
        // Game view's dock state and drive the native bind mode — DOCKED → texture mode +
        // child-glue window (in-tab occlusion via mirror-blit; the DP phase_off fix
        // anchors phase via GA_ROOT=container); UNDOCKED → PRESENT mode (runtime presents
        // woven stereo into our visible top-level window over the floating pane; SR
        // self-anchors, phase-snap free). The env vars stay authoritative TEST overrides:
        // when DISPLAYXR_PROV_PRESENT_MODE / DISPLAYXR_PROV_GV_CHILDGLUE are set the setters
        // are skipped (the native side then falls back to the env). Native overrides survive
        // session stop, so this must run before EVERY start, not just the first.
        // Child-glue is handed the matched PANE hwnd — its GA_ROOT container is resolved
        // NATIVELY at window-creation time (attempt #1 pre-captured the container here and
        // Unity destroyed it as Play settled → dead WS_CHILD → mono).
        public static void ApplyDockModeForSessionStart()
        {
            if (!ProbeEnabled()) return;
            if (Application.platform != RuntimePlatform.WindowsEditor) return;
            // Refresh the matched GameView instance + pane HWND (s_hwndCorrHwnd) first.
            TryGetGameViewRenderRect(out _, out _, out _, out _, out _);
            bool docked, forced = s_forcedDockState >= 0;
            if (forced)
            {
                docked = s_forcedDockState == 1; // dxr_dock_toggle test hook (one-shot)
                s_forcedDockState = -1;
            }
            else if (!TryDetectGameViewDocked(out docked))
            {
                docked = true; // occlusion-correct default; present mode stays reachable via env
                if (!s_warnedDockDetect)
                {
                    s_warnedDockDetect = true;
                    Debug.LogWarning("[DisplayXR] GameView dock-state detection unavailable " +
                        "(editor internals changed?) — assuming DOCKED (texture mode).");
                }
            }
            s_lastAppliedDocked = docked;
            s_lastViewSwap = -1; // (#740) force the per-frame view-swap to re-push this session
            bool presentEnv = System.Environment.GetEnvironmentVariable("DISPLAYXR_PROV_PRESENT_MODE") != null;
            bool childEnv   = System.Environment.GetEnvironmentVariable("DISPLAYXR_PROV_GV_CHILDGLUE") != null;
            if (!presentEnv)
                DisplayXRProviderNative.dxr_prov_set_present_mode(docked ? 0 : 1);
            if (!childEnv)
            {
                // The pane handle is passed in BOTH modes: docked → child-glue parent
                // (GA_ROOT resolved natively at creation); undocked → the present window's
                // OWNER hint (z-rides the floating Game view instead of global TOPMOST).
                var pane = s_hwndCorrValid ? s_hwndCorrHwnd : System.IntPtr.Zero;
                DisplayXRProviderNative.displayxr_set_child_glue(docked ? 1 : 0, pane);
            }
            Debug.Log("[DisplayXR] GameView dock state: " + (docked
                ? "DOCKED → texture mode + child-glue" : "UNDOCKED → PRESENT mode")
                + (forced ? " [FORCED test hook]" : "")
                + (presentEnv || childEnv ? " [env override active — mode pinned by env]" : ""));
        }

        // (#740 stereo unswap) The docked texture weave path assigns the two views in the
        // reversed order vs maximized/floating — a discrete flip, geometry-invariant,
        // runtime/SDK-side (#740). HW-proven: forcing the swap makes docked correct and
        // maximized wrong; gating it to docked-non-maximized makes both correct. It's a
        // PER-FRAME copy decision, and maximize/unmaximize does NOT restart the session
        // (same GameView instance, so the RehostWatcher doesn't fire), so this must be
        // re-evaluated every frame — not once at bind. present (undocked) is already correct
        // (s_lastAppliedDocked=false). Env DISPLAYXR_PROV_VIEW_SWAP is the authoritative test
        // override. Stereo-only band-aid until the runtime fixes the maximized-path root
        // cause — must NOT ship on the N>2 quilt path.
        static int s_lastViewSwap = -1;
        static void UpdateViewSwap()
        {
            // Superseded by RtCentringOffsetX/Y (the docked flip was an uncorrected
            // anchor-vs-content offset, now fixed in-geometry — honest, N-view-general, no
            // residual). The view-swap mechanism is kept as an env-only fallback
            // (DISPLAYXR_PROV_VIEW_SWAP): when the env is set the native side pins it and we
            // don't touch it; otherwise it stays OFF.
            if (System.Environment.GetEnvironmentVariable("DISPLAYXR_PROV_VIEW_SWAP") != null) return;
            if (s_lastViewSwap == 0) return;
            s_lastViewSwap = 0;
            DisplayXRProviderNative.dxr_prov_set_view_swap(0);
        }

        // (#740) EditorWindow.maximized for the matched Game view — the discriminant for the
        // stereo unswap (docked non-maximized swaps; maximized does not). Reuses the
        // s_maximizedProp cache resolved by LogGameViewInstancesOnChange.
        static bool IsMatchedGameViewMaximized()
        {
            if (s_matchedGameView == null) return false;
            try
            {
                if (s_maximizedProp == null && s_gvType != null)
                    for (var t = s_gvType; t != null && s_maximizedProp == null; t = t.BaseType)
                        s_maximizedProp = t.GetProperty("maximized",
                            BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic);
                if (s_maximizedProp == null) return false;
                return (bool)s_maximizedProp.GetValue(s_matchedGameView, null);
            }
            catch { return false; }
        }

        // Called by the loader BEFORE the subsystem starts. Window-glue (default): born the
        // weave window at the pane rect; zone at (0,0) window-sized. Zone-glue opt-in
        // (DISPLAYXR_PROV_GV_ZONEGLUE=1, superseded experiment): born at the MONITOR rect
        // (parked for the whole session) with the ZONE carrying the pane's screen offset.
        public static void TryPushInitialGameViewRect()
        {
            if (!ProbeEnabled()) return;
            // Full-res the Game view BEFORE the zone is born so the mirror RT == panel physical px.
            if (TryGetGameViewRenderRect(out int px, out int py, out int pw, out int ph, out string dbg))
            {
                ForceGameViewFullResolution();
                // BINDPANE: hand the runtime Unity's own pane window; the zone rect is the
                // render area's offset within the pane's client (client==window rect for the
                // borderless GUIView child, so screen-space subtraction is exact).
                if (BindPaneEnabled() && s_hwndCorrValid && s_hwndCorrHwnd != System.IntPtr.Zero)
                {
                    DisplayXRProviderNative.dxr_prov_set_external_weave_hwnd(s_hwndCorrHwnd);
                    DisplayXRProviderNative.dxr_prov_set_panel_rect(px - s_hwndCorrX, py - s_hwndCorrY, pw, ph);
                    if (!s_loggedInitOnce)
                    {
                        s_loggedInitOnce = true;
                        Debug.Log($"[DisplayXR] GameView BINDPANE init: pane=0x{s_hwndCorrHwnd.ToInt64():X} " +
                                  $"at ({s_hwndCorrX},{s_hwndCorrY}) zone=({px - s_hwndCorrX},{py - s_hwndCorrY} {pw}x{ph}) | {dbg}");
                    }
                    return;
                }
                if (ZoneGlueEnabled() &&
                    TryGetMonitorRect(px + pw / 2, py + ph / 2, out int mx, out int my, out int mw, out int mh))
                {
                    DisplayXRProviderNative.dxr_prov_set_initial_gameview_rect(mx, my, mw, mh);
                    // Pane rect relative to the monitor origin: window client (0,0) == monitor
                    // (mx,my), so the zone rect is monitor-local (== screen when mx,my == 0,0).
                    DisplayXRProviderNative.dxr_prov_set_panel_rect(px - mx, py - my, pw, ph);
                    if (!s_loggedInitOnce)
                    {
                        s_loggedInitOnce = true;
                        Debug.Log($"[DisplayXR] GameView ZONE-GLUE init: monitor=({mx},{my} {mw}x{mh}) " +
                                  $"zone=({px - mx},{py - my} {pw}x{ph}) | {dbg}");
                    }
                    return;
                }
                DisplayXRProviderNative.dxr_prov_set_initial_gameview_rect(px, py, pw, ph);
                if (!s_loggedInitOnce) { s_loggedInitOnce = true; Debug.Log($"[DisplayXR] GameView initial rect: {dbg}"); }
            }
        }

        int m_LastGlueX = int.MinValue, m_LastGlueY, m_LastGlueW, m_LastGlueH;
        System.IntPtr m_LastFollowPane; // last pane HWND published to pane-follow (#740)
        int m_LastDbgW = int.MinValue, m_LastDbgH; // size-change diagnostic log key (#740)
        int m_PushedW = int.MinValue, m_PushedH;   // last size actually pushed (settle-debounce, #740)
        static int s_freezeSizeGate = -1;          // DISPLAYXR_PROV_FREEZE_SIZE (task 7 sandbox)
        int m_PendW = int.MinValue, m_PendH;       // candidate size awaiting settle
        double m_PendSince;                        // when the candidate appeared

        // (#740) DOCKED phase calibration knob: the docked texture path showed an exact L/R
        // eye swap — the signature of a constant ~one-view-pitch (~2px) residual in the
        // phase_off correction, NOT drift (per-eye fields stay uniform). The docked child
        // weave window is INVISIBLE (pure phase anchor + zone canvas), so shifting its X
        // moves ONLY the interlace phase, never the on-screen content — a clean live knob:
        // write an integer to %TEMP%\dxr_phase_nudge.txt (re-read ~every half second) and
        // sweep seated until the eyes are correct; the found constant then goes to the
        // runtime agent (#740) for a proper DP-side calibration. Docked/texture mode only.
        static int s_phaseNudgeX;
        static int s_phaseNudgeY;
        static int s_phaseNudgeTick;
        static int ReadNudge(string file, int cur, string label)
        {
            try
            {
                string f = System.IO.Path.Combine(System.IO.Path.GetTempPath(), file);
                int v = 0;
                if (System.IO.File.Exists(f))
                    int.TryParse(System.IO.File.ReadAllText(f).Trim(), out v);
                if (v != cur) Debug.Log("[DisplayXR] docked phase nudge " + label + " = " + v + " px");
                return v;
            }
            catch { return cur; }
        }
        static void UpdatePhaseNudge()
        {
            // (#740) X shifts the invisible anchor horizontally (moves phase, not content).
            // Y does the same vertically — through the slanted lens a Y anchor offset ALSO
            // moves the X-interlace phase, so this probes whether the uncorrected docked
            // anchor-vs-content Y offset (measured −4px, the vertical sibling of the +3 RT
            // centring) is the residual half-cycle. dxr_phase_nudge.txt / _y.txt.
            if (!s_lastAppliedDocked) { s_phaseNudgeX = 0; s_phaseNudgeY = 0; return; }
            if ((s_phaseNudgeTick++ % 30) != 0) return;
            s_phaseNudgeX = ReadNudge("dxr_phase_nudge.txt", s_phaseNudgeX, "X");
            s_phaseNudgeY = ReadNudge("dxr_phase_nudge_y.txt", s_phaseNudgeY, "Y");
        }

        void PushGameViewRectEditorProbe()
        {
            if (!ProbeEnabled()) return;
            // (#740 f-up) Pause ALL glue work while the custom host drag is live: Unity's
            // maximized-view layout readings flap by the toolbar height every frame while
            // the container moves (mainSize 1596↔1536 observed) → per-frame zone re-drive →
            // swapchain realloc storm → shimmer/loss of 3D. The native lockstep follow owns
            // the window position during the drag (the original smooth behavior); everything
            // here resumes at mouse-up. POV stays live regardless (the frame pump is
            // independent of this glue).
            if (DisplayXRProviderNative.displayxr_host_drag_active() != 0) return;
            // Keep the Game view at full (physical) resolution — re-enforce in case the user
            // (or a layout change) flips "Low Resolution Aspect Ratios" back on mid-session.
            ForceGameViewFullResolution();
            LogGameViewInstancesOnChange();
            LogGameViewGeometryOnChange();
            ApplyGameViewFit();
            UpdateViewSwap(); // (#740 stereo unswap) per-frame — maximize doesn't restart
            if (!TryGetGameViewRenderRect(out int px, out int py, out int pw, out int ph, out string dbg))
                return;
            if (!s_loggedGlueOnce) { s_loggedGlueOnce = true; Debug.Log($"[DisplayXR] GameView glue: {dbg}"); }
            // (#740) Log the full computation whenever the resulting SIZE changes — a layout
            // reset produced steady wrong ×ppp sizes and the once-only log above hid the
            // inputs (mainSize/host/ppp/toolbar) that explain them.
            if (pw != m_LastDbgW || ph != m_LastDbgH)
            {
                m_LastDbgW = pw; m_LastDbgH = ph;
                Debug.Log($"[DisplayXR] GameView glue size change: {dbg}");
            }
            LogDockBorderOnChange(s_cachedPpp > 0 ? s_cachedPpp : 1f, px, py, pw, ph); // #740
            // (#740) Docked phase calibration: shift the invisible child anchor's X — phase
            // moves, content doesn't (see UpdatePhaseNudge).
            UpdatePhaseNudge();
            px += s_phaseNudgeX;
            py += s_phaseNudgeY;
            // (#740) SIZE settle-debounce: a continuous interactive resize (dock splitter /
            // tab edge drag) changes the pane size EVERY frame → per-frame zone re-drive →
            // per-frame swapchain+bridge realloc — a long splitter drag hung the D3D12
            // device (DXGI_ERROR_DEVICE_HUNG → device removed → editor fatal). Push a
            // CHANGED size only after it has held for kResizeSettleSeconds; POSITION keeps
            // pushing immediately (the window stays glued, briefly at the old size), so a
            // whole resize costs ONE realloc at settle instead of one per frame.
            // DISPLAYXR_PROV_FREEZE_SIZE=1: never push a size change at all — the swapchain
            // stays at its born size for the whole session (positions keep tracking live).
            // Crash-proof sandbox for interactive layout/phase exploration while the
            // realloc-adjacent D3D12 instability (per-frame compositor barrier UB, id 527)
            // is chased runtime-side: content letterboxes/stretches on a resize (cosmetic).
            if (s_freezeSizeGate < 0)
                s_freezeSizeGate = System.Environment.GetEnvironmentVariable(
                    "DISPLAYXR_PROV_FREEZE_SIZE") == "1" ? 1 : 0;
            const double kResizeSettleSeconds = 0.35;
            if (m_PushedW == int.MinValue)
            {
                m_PushedW = pw; m_PushedH = ph; // first frame: adopt (session-start size)
            }
            else if (s_freezeSizeGate == 1)
            {
                pw = m_PushedW; ph = m_PushedH; // frozen at born size
            }
            else if (pw != m_PushedW || ph != m_PushedH)
            {
                double now = Time.realtimeSinceStartupAsDouble;
                if (pw != m_PendW || ph != m_PendH)
                {
                    m_PendW = pw; m_PendH = ph; m_PendSince = now; // (re)start the settle clock
                }
                if (now - m_PendSince < kResizeSettleSeconds)
                {
                    pw = m_PushedW; ph = m_PushedH; // hold the old size while resizing
                }
                else
                {
                    m_PushedW = pw; m_PushedH = ph; // settled: push the new size once
                }
            }
            // (#740) Pane-follow: publish the matched pane HWND + render-vs-pane-window offset
            // so the native WM_TIMER keeps the weave window glued during OS modal drags of a
            // Unity window (which freeze this LateUpdate → the C# glue stops). The timer
            // re-derives the pane's live screen rect and repositions our window. Runs for all
            // glue arrangements; harmless when the window doesn't track (no-op).
            if (s_hwndCorrValid && s_hwndCorrHwnd != System.IntPtr.Zero)
            {
                DisplayXRProviderNative.displayxr_set_pane_follow(
                    s_hwndCorrHwnd, px - s_hwndCorrX, py - s_hwndCorrY, pw, ph);
                // (#740) New pane matched (layout reset replaced the GameView): drop the
                // window-glue debounce so the next dxr_prov_set_gameview_rect push fires
                // even if the new pane landed at the identical rect (the native side also
                // re-glues immediately on the pane-follow retarget).
                if (m_LastFollowPane != s_hwndCorrHwnd)
                {
                    m_LastFollowPane = s_hwndCorrHwnd;
                    m_LastGlueX = int.MinValue;
                }
            }
            // BINDPANE: the bound window IS the pane — publish only the render area's
            // client-relative offset (the pane moves itself; zone x/y stay put unless the
            // in-pane layout changes). Never any window op.
            if (BindPaneEnabled())
            {
                if (s_hwndCorrValid && s_hwndCorrHwnd != System.IntPtr.Zero)
                    DisplayXRProviderNative.dxr_prov_set_panel_rect(px - s_hwndCorrX, py - s_hwndCorrY, pw, ph);
                return;
            }
            // Zone-glue (opt-in experiment, #740/#742): the window is parked at the monitor origin —
            // publish the pane's monitor-local rect to the zone-convergence path every frame
            // (native no-ops once converged; moves = pure zone x/y updates, no window op at
            // all). NEVER call dxr_prov_set_gameview_rect here in this mode.
            if (ZoneGlueEnabled())
            {
                if (TryGetMonitorRect(px + pw / 2, py + ph / 2, out int mx, out int my, out _, out _))
                { px -= mx; py -= my; }
                DisplayXRProviderNative.dxr_prov_set_panel_rect(px, py, pw, ph);
                return;
            }
            // Legacy window-glue path (DISPLAYXR_PROV_GV_ZONEGLUE=0):
            // Phase 1: publish the authoritative panel PHYSICAL px to the native zone-convergence
            // path every frame (the native side no-ops once converged). This is the ONLY reliable
            // physical-px source — info.mirrorRtDesc is LOGICAL px on a HiDPI display. Scale-
            // independent (mainSize is the target, not the zoom), so magnify drives no zone churn;
            // a real tab resize changes pw/ph → native re-drives the zone + reallocs the swapchain.
            DisplayXRProviderNative.dxr_prov_set_panel_px(pw, ph);
            // Debounce: only reposition the weave window when the rect actually changes, so a
            // steady Game view doesn't get SetWindowPos'd every frame (churn / weaver jitter).
            if (px == m_LastGlueX && py == m_LastGlueY && pw == m_LastGlueW && ph == m_LastGlueH)
                return;
            m_LastGlueX = px; m_LastGlueY = py; m_LastGlueW = pw; m_LastGlueH = ph;
            DisplayXRProviderNative.dxr_prov_set_gameview_rect(px, py, pw, ph);
        }

        void PushActiveRigTunables()
        {
            if (DisplayXRRigManager.SplashActive) return;

            Camera cam = DisplayXRRigManager.ActiveCamera;
            if (cam == null) return;

            // Read the active rig's tunables (raw fields; no DisplayXRFeature dep).
            DisplayXRTunables t;
            var disp = cam.GetComponent<DisplayXRDisplay>();
            var camRig = cam.GetComponent<DisplayXRCamera>();
            if (disp != null && disp.enabled) t = disp.GetProviderTunables();
            else if (camRig != null && camRig.enabled) t = camRig.GetProviderTunables();
            else return; // active camera carries no DisplayXR rig — nothing to push

            // Fold scene scale (lossyScale) as zoom into the depth metric the runtime
            // owns — virtualDisplayHeight (display-centric) / convergence (camera-
            // centric) — matching the standalone + cube_handle (vdh / scaleFactor).
            // The native side ignores scale, so we fold it here.
            float scale = cam.transform.lossyScale.y;
            if (scale < 1e-4f) scale = 1e-4f;

            float vdh = t.virtualDisplayHeight;
            float invConv = t.invConvergenceDistance;
            if (t.cameraCentricMode)
            {
                invConv /= scale;
            }
            else
            {
                if (vdh <= 0f) vdh = m_DisplayHeightM; // 0 -> physical height (native also guards)
                if (vdh > 0f) vdh /= scale;
            }

            DisplayXRProviderNative.dxr_prov_set_tunables(
                t.ipdFactor, t.parallaxFactor, t.perspectiveFactor,
                vdh, invConv, t.fovOverride, t.nearZ, t.farZ,
                t.cameraCentricMode ? 1 : 0);

            Vector3 p = cam.transform.position;
            Quaternion q = cam.transform.rotation;
            // Stash the exact Unity-world rig pose we send the runtime as the display
            // plane, so the URP foreground clip references the SAME pose (its plane
            // origin/normal) rather than re-reading the transform independently (#166).
            DisplayXRProvider.RigPlanePos     = p;
            DisplayXRProvider.RigPlaneForward = cam.transform.forward;
            DisplayXRProvider.RigPlaneValid   = true;
            DisplayXRProviderNative.dxr_prov_set_display_pose(p.x, p.y, p.z, q.x, q.y, q.z, q.w, 1);
        }
    }
}
