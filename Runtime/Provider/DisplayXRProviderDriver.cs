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
        uint  m_SessionSerial;
        int   m_SessionFrame; // frames of the running session (1 = the frame it started), up to 2
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
            // (#459) Take a calibration marker down with the glue (no-op unless one is up).
            DisplayXRGameViewCalibrator.Abort();
            s_calLastValid = false; s_imgRectFrame = -1;
            // (#459) The glue stops (Play ends, dock / undock restarts the subsystem): put
            // Unity's uniform zoom back on every Game view GVFIT stretched.
            RestoreGameViewFit();
            s_gvScaled = s_gvOffPane = false; s_degSince = -1.0; s_degWarned = false;
            DisplayXRDepthBudget.Reset();
            DisplayXRCursorDepth.Reset();
            // No pump will run again: complete every pending desktop action as failed.
            DisplayXRDesktop.OnProviderStopped();
        }

#if UNITY_STANDALONE_LINUX && !UNITY_EDITOR
        // Native-Wayland player: displayxr_resize_overlay cannot resize Unity's window
        // from native code (a Wayland client sizes its own toplevel, and Unity's belongs
        // to its SDL), so the plug-in queues the size and Unity applies it here.
        private static bool s_LinuxResizeMissing;
        private static void ApplyPendingLinuxResize()
        {
            if (s_LinuxResizeMissing) return;
            try
            {
                if (DisplayXRNative.displayxr_linux_take_pending_resize(out int w, out int h) != 0 && w > 0 && h > 0)
                {
                    Debug.Log($"[DisplayXR] Wayland: resizing the window to {w}x{h} (logical px)");
                    Screen.SetResolution(w, h, FullScreenMode.Windowed);
                }
            }
            catch (System.EntryPointNotFoundException)
            {
                s_LinuxResizeMissing = true;
            }
        }
#endif

        void LateUpdate()
        {
#if UNITY_EDITOR_OSX || UNITY_STANDALONE_OSX
            // macOS (#204): pump xrPollEvent from the MAIN thread — the runtime's
            // poll drains NSApp events and flushes CATransaction (main-thread-only;
            // AppKit throws off-main). Windows, Linux and Android (#418) keep the
            // native graphics-thread pump (GfxPopulateNextFrameDesc) — do NOT add them
            // here, or the events would be pumped from two threads.
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
#if UNITY_STANDALONE_LINUX && !UNITY_EDITOR
            ApplyPendingLinuxResize();
#endif
            // Agent calls to the app's own MCP tools (XR_DXR_mcp_tools), queued by the
            // event pump; their handlers run here, on the main thread.
            DisplayXRMcp.Pump();
            // Desktop-control action results + consent changes (XR_DXR_desktop_control),
            // queued by the event pump; tasks complete / StateChanged fires here.
            DisplayXRDesktop.Pump();
            // Side-step (Linux native Wayland): the app's own window state and its
            // SteppedAside / ReturnedHome events. One status copy; inert elsewhere.
            DisplayXRSideStep.Tick();
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
                    // Same for the depth-aware cursor: never leave the OS cursor hidden
                    // for a session that is gone.
                    DisplayXRCursorDepth.Reset();
                }
                return;
            }

            // A restart the running flag hid: Unity stops the session on an Android pause
            // and starts a new one on resume while this (main) thread is paused, so the
            // flag reads 1 on both sides of the gap. The native start serial does not lie —
            // play the missed falling edge now so everything that re-arms on
            // SessionStarted (display mode, cursor depth, Local2D, the app's own
            // handlers) re-applies to the NEW session (#418 P3b).
            uint serial = 0;
            try { serial = DisplayXRProviderNative.dxr_prov_session_serial(); }
            catch (System.EntryPointNotFoundException) { serial = m_SessionSerial; } // older native
            if (m_SessionStarted && serial != m_SessionSerial)
            {
                Debug.Log($"[DisplayXR] provider session restarted under a paused main thread (serial {m_SessionSerial} -> {serial}) — re-arming SessionStarted");
                m_SessionStarted = false;
                DisplayXRProvider.OnSessionStopped();
                DisplayXRDepthBudget.Reset();
                DisplayXRCursorDepth.Reset();
            }

            if (!m_SessionStarted)
            {
                m_SessionStarted = true;
                m_SessionSerial = serial;
                m_SessionFrame = 0;
                DisplayXRProviderNative.dxr_prov_get_display_info(out var di);
                m_DisplayHeightM = di.isValid != 0 ? di.heightM : 0f;
                DisplayXRProvider.OnSessionStarted();
            }
            // The start-up placement (on by default; Linux native Wayland): once per run,
            // at the session's second frame, after the app's own placement had its turn;
            // every frame after while its decision waits for the window's first show (a
            // static field read otherwise).
            if (m_SessionFrame < 2) ++m_SessionFrame;
            DisplayXRTargetDisplay.TickStartupPlacement(m_SessionFrame);

            DisplayXRProvider.PumpEvents();
            // Rear depth budget (#318): republish the last locate's advisory value
            // BEFORE the rigs render, so the foreground clip this frame uses this
            // frame's number. Cheap (one P/Invoke, no allocation) and inert on a
            // runtime without XR_DXR_depth_budget.
            DisplayXRDepthBudget.Poll();
            // Depth-aware cursor (ADR-046): pointer + depth source in, willDraw out, OS
            // cursor hidden exactly while the runtime draws its own. A few field reads
            // and one state P/Invoke while the app has not opted in.
            DisplayXRCursorDepth.Tick();
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

        // --- (#459) Pixel-exact Game view: shared state ---------------------------------
        // How long a Game-view size must hold before the glue pushes it (the #740 resize
        // debounce); the 1:1 diagnostic and the whole-pixel experiment wait the same time.
        internal const double GameViewSettleSeconds = 0.35;

        // What the matched path of TryGetGameViewRenderRect last saw: the pane as handed to
        // DisplayXRGameViewPlacement, Unity's target size in px (fractional for Free Aspect)
        // and the pane's left border in points. Valid only while s_diagValid.
        static bool  s_diagValid;
        static DisplayXRGameViewPlacement.Pane s_diagPane;
        static float s_diagTargetWPx, s_diagTargetHPx;
        static Vector2 s_diagZoom;   // the matched view's zoom (docked mode; zero = not read)

        // (#459) A Game view that CANNOT be 1:1 this frame (docked / texture mode only):
        //   s_gvScaled   its zoom is not 1 (a fixed size Unity fits into the view, or the
        //                Scale slider): every texel is resampled on the way to the panel;
        //   s_gvOffPane  the computed anchor came out off the pane (replaced by the area).
        // While either holds: the anchor stays inside the pane, the picture is shown as
        // Unity scales it (no usable 3D), the calibration marker is not shown (it cannot be
        // decoded under scaling - it used to flash three times and fail), and ONE warning
        // names the cause and the remedy, once the state has held for kDegradedHoldSeconds
        // (the editor side, DisplayXRGameViewWholePixel, normally fixes the view within a
        // few tenths of a second by taking it over for the Play session; then no warning).
        static bool   s_gvScaled, s_gvOffPane;
        static int    s_gvOffPaneDx, s_gvOffPaneDy;
        static double s_degSince = -1.0;
        static bool   s_degWarned;
        const double  kDegradedHoldSeconds = 1.0;

        /// <summary>(#459) True while the docked Game view cannot be shown texel for pixel
        /// (zoomed, or the anchor fell off the pane) and the glue is in its degraded mode.</summary>
        internal static bool GameViewDegraded => s_gvScaled || s_gvOffPane;

        static void WarnGameViewDegradedOnce()
        {
            if (!GameViewDegraded || !s_lastAppliedDocked) { s_degSince = -1.0; return; }
            double now = Time.realtimeSinceStartupAsDouble;
            if (s_degSince < 0.0) { s_degSince = now; return; }
            if (s_degWarned || now - s_degSince < kDegradedHoldSeconds) return;
            s_degWarned = true;
            var inv = System.Globalization.CultureInfo.InvariantCulture;
            string size = s_diagPane.RtW + "x" + s_diagPane.RtH;
            string msg;
            if (s_gvScaled)
            {
                string zoom = s_diagZoom.x.ToString("0.##", inv) + "x";
                msg = "the Game view shows its " + size + " image scaled to " + zoom +
                      (s_diagZoom.x < 1f ? " (a fixed size larger than the view, which Unity shrinks to fit, or the Scale slider)"
                                         : " (the Scale slider)") +
                      ", so no texel lands on one panel pixel and 3D cannot be right. The picture is shown as Unity " +
                      "scales it, without 3D alignment, and the on-screen calibration is skipped. " +
                      "Remedy: in the Game view's toolbar select Free Aspect and set Scale to 1x (or pick a fixed size " +
                      "that fits the view at 1x). The plug-in normally does this by itself for the Play session and puts " +
                      "your selection back afterwards; it did not here (DISPLAYXR_PROV_GV_TAKEOVER=0, a size or zoom " +
                      "changed by hand during Play, or editor internals it could not reach).";
            }
            else
            {
                msg = "the computed image position (pane +" + s_gvOffPaneDx + ",+" + s_gvOffPaneDy + " for a " + size +
                      " image) is outside the Game view pane, so it was not used: the weave is anchored on the " +
                      "pane's render area instead and 3D is probably wrong. Remedy: select Free Aspect and Scale 1x " +
                      "in the Game view's toolbar; if it already is, please report the 'GameView 1:1' log line.";
            }
            Debug.LogWarning("[DisplayXR] Game view cannot be shown 1:1: " + msg +
                             " See docs~/architecture/editor-game-view-placement.md.");
        }

        /// <summary>The Game view the glue is bound to (a UnityEditor.GameView), or null.</summary>
        internal static object MatchedGameView => s_matchedGameView;

        /// <summary>The matched pane's points-to-px as Unity uses it (1.75 at 175 %), measured
        /// from the pane's own rects, never the global pixelsPerPoint. 0 = not known yet.</summary>
        internal static float MatchedPaneScale =>
            s_diagValid ? DisplayXRGameViewPlacement.SnappedScale(s_diagPane.PtToPx) : 0f;

        // WHOLE-PIXEL size (#459). At 125 / 175 / 225 % a Free Aspect Game view is drawn half
        // a pixel narrower than its render texture, so a column is dropped and the eyes swap
        // past it. Three states, from env DISPLAYXR_PROV_GV_WHOLEPIXEL:
        //   unset  AUTO: off until the on-screen calibration (DisplayXRGameViewCalibrator)
        //          MEASURES an image that is not drawn 1:1 at a fractional scale; then on for
        //          the rest of the editor session (RequestWholePixelAuto).
        //   "1"    on from the start, at every fractional scale (the former opt-in).
        //   "0"    never.
        // When on, the editor-side DisplayXRGameViewWholePixel selects, during Play, a temporary
        // Fixed Resolution size that is a whole number of points AND pixels and snaps the
        // zoom to 1, and tells the driver which view carries it (WholePixelView). On that
        // view the driver then (1) skips GVFIT — the size is drawn texel for pixel at zoom 1,
        // and GVFIT's h / (h - 0.5) stretch would double a row — and (2) places the anchor
        // as a letterbox with no stretch and the pane's real left border. THE EXPERIMENT
        // WINS OVER GVFIT on that one view; every other view, and every frame the size is
        // not selected, keeps GVFIT and the default placement exactly as they were.
        static int s_wholePixelGate = -1;   // -1 unresolved, 0 = "0" never, 1 = "1" always, 2 = auto
        static bool s_wholePixelAuto;       // auto: a calibration asked for it (this editor session)
        const string kSessWholePixelAuto = "DisplayXR.GameViewWholePixel.Auto";
        internal static bool WholePixelEnabled()
        {
            if (!ProbeEnabled()) return false;
            if (s_wholePixelGate < 0)
            {
                string e = System.Environment.GetEnvironmentVariable("DISPLAYXR_PROV_GV_WHOLEPIXEL");
                s_wholePixelGate = e == "1" ? 1 : e == "0" ? 0 : 2;
                // Kept for the editor session, so the next Play starts with the size selected
                // instead of measuring the same broken draw (and flashing the marker) again.
                if (s_wholePixelGate == 2) s_wholePixelAuto = SessionGetBool(kSessWholePixelAuto);
            }
            return s_wholePixelGate == 1 || (s_wholePixelGate == 2 && s_wholePixelAuto);
        }

        /// <summary>(#459) The calibration measured an image that is not drawn texel for pixel
        /// at a fractional scale: turn the whole-pixel size on for the rest of the editor
        /// session, unless DISPLAYXR_PROV_GV_WHOLEPIXEL=0.</summary>
        internal static void RequestWholePixelAuto(string why)
        {
            WholePixelEnabled();   // resolve the gate
            if (s_wholePixelGate == 0)
            {
                Debug.Log("[DisplayXR] whole-pixel Game view: " + why +
                          ", but DISPLAYXR_PROV_GV_WHOLEPIXEL=0 keeps the whole-pixel size off");
                return;
            }
            if (s_wholePixelGate != 2 || s_wholePixelAuto) return;
            s_wholePixelAuto = true;
            SessionSetBool(kSessWholePixelAuto, true);
            Debug.Log("[DisplayXR] whole-pixel Game view: " + why + " - selecting the whole-pixel size " +
                      "(on until the editor quits; DISPLAYXR_PROV_GV_WHOLEPIXEL=0 opts out)");
        }

        /// <summary>The Game view currently showing the whole-pixel size, or null. Set by the
        /// editor-side DisplayXRGameViewWholePixel, cleared when it restores the view.</summary>
        internal static object WholePixelView
        {
            get => s_wholePixelView;
            set { s_wholePixelView = value; if (value == null) s_fitSkipLogged = false; }
        }
        static object s_wholePixelView;

        // (#459) True the first time a key is seen in this EDITOR session. Statics are reset by
        // the domain reload of every Play, so a "warn once" static alone warns once per Play;
        // UnityEditor.SessionState (reached by reflection: the Runtime asmdef has no UnityEditor
        // reference) lasts until the editor quits. Falls back to once per domain.
        static MethodInfo s_sessGetBool, s_sessSetBool;
        static bool s_sessResolved;
        static System.Collections.Generic.HashSet<string> s_onceLocal;
        static void ResolveSessionState()
        {
            if (s_sessResolved) return;
            s_sessResolved = true;
            try
            {
                var ss = System.Type.GetType("UnityEditor.SessionState,UnityEditor");
                if (ss != null)
                {
                    var types = new[] { typeof(string), typeof(bool) };
                    s_sessGetBool = ss.GetMethod("GetBool", BindingFlags.Static | BindingFlags.Public, null, types, null);
                    s_sessSetBool = ss.GetMethod("SetBool", BindingFlags.Static | BindingFlags.Public, null, types, null);
                }
            }
            catch { }
        }

        // A bool kept until the editor quits (false where SessionState is not reachable).
        static bool SessionGetBool(string key)
        {
            ResolveSessionState();
            try { if (s_sessGetBool != null) return (bool)s_sessGetBool.Invoke(null, new object[] { key, false }); }
            catch { }
            return false;
        }

        static void SessionSetBool(string key, bool value)
        {
            ResolveSessionState();
            try { if (s_sessSetBool != null) s_sessSetBool.Invoke(null, new object[] { key, value }); }
            catch { }
        }

        static bool FirstTimeThisEditorSession(string key)
        {
            key = "DisplayXR.Once." + key;
            ResolveSessionState();
            try
            {
                if (s_sessGetBool != null && s_sessSetBool != null)
                {
                    if ((bool)s_sessGetBool.Invoke(null, new object[] { key, false })) return false;
                    s_sessSetBool.Invoke(null, new object[] { key, true });
                    return true;
                }
            }
            catch { }
            if (s_onceLocal == null) s_onceLocal = new System.Collections.Generic.HashSet<string>();
            return s_onceLocal.Add(key);
        }

        // Two references to the same live UnityEngine.Object (a destroyed one matches nothing).
        static bool SameLiveObject(object a, object b)
        {
            var ua = a as UnityEngine.Object; var ub = b as UnityEngine.Object;
            return ua != null && ub != null && ua == ub;
        }

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
        static bool   s_fitSkipLogged;   // (#459) "skipped on the whole-pixel view", once per activation

        // (#459) GVFIT IS ONLY VALID AT ZOOM 1. The row fit is a fraction of a row on a
        // texture drawn at its own size. On a view whose zoom is anything else (a Fixed
        // Resolution larger than the area, which Unity fits; the Scale slider off 1x) writing
        // it made the zoom ANISOTROPIC - (0.279, 1.0002) on hardware: a 4K picture drawn
        // 3.6x too tall, the anchor computed 700 px above the pane, a black weave, and the
        // stretched zoom left in the layout after Play. So: at zoom 1 the fit is written; at
        // any other zoom scale.y is kept equal to scale.x (Unity's own uniform zoom), which
        // also undoes a stretch left by an earlier frame or an older plug-in. Both values
        // come from DisplayXRGameViewPlacement.FitZoomY (pure, unit-tested).
        // WHAT IS PUT BACK, AND WHEN: RestoreGameViewFit sets scale.y = scale.x (and a nudged
        // translation back) on every Game view. It runs when the glue stops (OnDestroy: Play
        // ends, dock / undock restarts the subsystem) and, from the editor side
        // (DisplayXRGameViewWholePixel), on leaving Play, before a domain reload, when the
        // editor quits, and once at editor load (a layout saved by a killed editor).
        // A stretched scale.y also stops Unity following its own default zoom
        // (GameView.UpdateZoomAreaAndParent compares scale.y with the default), so leaving
        // it behind is not cosmetic.
        static System.Collections.Generic.Dictionary<UnityEngine.Object, float> s_fitNudgeOrig;   // view -> translation.y before "nudge:"

        // GameView.m_ZoomArea (a ZoomableArea) of one view, with its m_Scale / m_Translation
        // fields resolved. Null when the editor internals are not found.
        static object ZoomAreaOf(object view)
        {
            if (view == null || s_gvType == null) return null;
            if (s_zoomAreaField == null)
                for (var t = s_gvType; t != null && s_zoomAreaField == null; t = t.BaseType)
                    foreach (var fi in t.GetFields(BindingFlags.Instance | BindingFlags.Public |
                                                   BindingFlags.NonPublic | BindingFlags.DeclaredOnly))
                        if (fi.Name.ToLowerInvariant().Contains("zoomarea")) { s_zoomAreaField = fi; break; }
            if (s_zoomAreaField == null) return null;
            var za = s_zoomAreaField.GetValue(view);
            if (za == null) return null;
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
                if (s_zoomScaleField == null || s_zoomTransField == null)
                    Debug.Log("[DisplayXR] GVFIT zoom fields: scale=" +
                              (s_zoomScaleField != null ? s_zoomScaleField.Name : "MISSING") +
                              " trans=" + (s_zoomTransField != null ? s_zoomTransField.Name : "MISSING"));
            }
            return za;
        }

        /// <summary>(#459) The view's zoom (GameView.m_ZoomArea.m_Scale). False when it cannot be read.</summary>
        internal static bool TryGetGameViewZoom(object view, out Vector2 zoom)
        {
            zoom = default;
            try
            {
                if (s_gvType == null) s_gvType = System.Type.GetType("UnityEditor.GameView,UnityEditor");
                var za = ZoomAreaOf(view);
                if (za == null || s_zoomScaleField == null) return false;
                zoom = (Vector2)s_zoomScaleField.GetValue(za);
                return zoom.x > 0f && zoom.y > 0f;
            }
            catch { return false; }
        }

        /// <summary>
        /// (#459) Undo everything GVFIT wrote, on every Game view: scale.y back to scale.x
        /// (Unity's zoom is uniform), a "nudge:" translation back to what it was. Writes
        /// nothing on a view that is already uniform, so it is safe to call at any time, in
        /// or out of Play, and as often as wanted. Returns how many views it changed.
        /// </summary>
        internal static int RestoreGameViewFit()
        {
            int changed = 0;
            try
            {
                if (s_gvType == null) s_gvType = System.Type.GetType("UnityEditor.GameView,UnityEditor");
                if (s_gvType == null) return 0;   // a built player
                var wins = Resources.FindObjectsOfTypeAll(s_gvType);
                if (wins == null) return 0;
                foreach (var w in wins)
                {
                    try
                    {
                        var za = ZoomAreaOf(w);
                        if (za == null) continue;
                        if (s_zoomScaleField != null)
                        {
                            var sc = (Vector2)s_zoomScaleField.GetValue(za);
                            float y = DisplayXRGameViewPlacement.RestoredZoomY(sc.x);
                            if (sc.x > 0f && sc.y > 0f && Mathf.Abs(sc.y - y) > 1e-6f)
                            { sc.y = y; s_zoomScaleField.SetValue(za, sc); changed++; }
                        }
                        if (s_fitNudgeOrig != null && s_zoomTransField != null &&
                            s_fitNudgeOrig.TryGetValue(w, out float origY))
                        {
                            var tr = (Vector2)s_zoomTransField.GetValue(za);
                            tr.y = origY; s_zoomTransField.SetValue(za, tr); changed++;
                        }
                    }
                    catch { /* one view's internals: try the others */ }
                }
                if (s_fitNudgeOrig != null) s_fitNudgeOrig.Clear();
                s_lastFitLog = null;
            }
            catch { }
            return changed;
        }

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
                    var za = ZoomAreaOf(w);
                    if (s_zoomAreaField == null) return;
                    if (za == null) continue;

                    // (#459) The view showing the whole-pixel size is drawn texel for pixel at
                    // zoom 1 (the editor side snapped it): no fit, and none of the overrides.
                    if (SameLiveObject(w, WholePixelView))
                    {
                        if (!s_fitSkipLogged)
                        {
                            s_fitSkipLogged = true;
                            Debug.Log("[DisplayXR] GVFIT skipped on the whole-pixel Game view " +
                                      "(zoom 1, no row stretch)");
                        }
                        continue;
                    }

                    string applied = null;
                    if (s_fitSpec == "scale" && s_zoomScaleField != null)
                    {
                        // dest is 0.5 px short of the RT: stretch y so dest rows == RT rows.
                        // The texture is found without the GVGEOM logger: the seam fix must
                        // not depend on a diagnostic env.
                        var sc = (Vector2)s_zoomScaleField.GetValue(za);
                        if (!(sc.x > 0f) || !(sc.y > 0f)) continue;
                        bool zoomOne = DisplayXRGameViewPlacement.IsZoomOne(sc.x);
                        RenderTexture rt = FirstGameViewRt(w);
                        // At zoom 1 without a texture (a realloc transient): leave it as it is.
                        if (zoomOne && (rt == null || rt.height <= 1)) continue;
                        // (#459) Valid at zoom 1 only; at any other zoom this is scale.x.
                        float target = DisplayXRGameViewPlacement.FitZoomY(sc.x, rt != null ? rt.height : 0);
                        if (Mathf.Abs(sc.y - target) > 1e-6f)
                        { sc.y = target; s_zoomScaleField.SetValue(za, sc); }
                        applied = zoomOne
                            ? $"scale.y={target:F7} (rt={rt.width}x{rt.height})"
                            : "not applied: the Game view's zoom is not 1 (scale.y kept equal to scale.x)";
                    }
                    else if (s_fitSpec.StartsWith("nudge:") && s_zoomTransField != null)
                    {
                        float pt = float.Parse(s_fitSpec.Substring(6),
                            System.Globalization.CultureInfo.InvariantCulture);
                        var tr = (Vector2)s_zoomTransField.GetValue(za);
                        if (Mathf.Abs(tr.y - pt) > 1e-6f)
                        {
                            // Remember what Unity had, for RestoreGameViewFit.
                            if (s_fitNudgeOrig == null) s_fitNudgeOrig = new System.Collections.Generic.Dictionary<UnityEngine.Object, float>();
                            if (!s_fitNudgeOrig.ContainsKey(w)) s_fitNudgeOrig[w] = tr.y;
                            tr.y = pt; s_zoomTransField.SetValue(za, tr);
                        }
                        applied = $"trans.y={pt:F3}pt";
                    }
                    else if (s_fitSpec == "off")
                    {
                        if (s_zoomScaleField != null)
                        {
                            // (#459) Uniform again: y = x, not 1 (the view may be zoomed).
                            var sc = (Vector2)s_zoomScaleField.GetValue(za);
                            float y = DisplayXRGameViewPlacement.RestoredZoomY(sc.x);
                            if (sc.x > 0f && sc.y > 0f && Mathf.Abs(sc.y - y) > 1e-6f) { sc.y = y; s_zoomScaleField.SetValue(za, sc); }
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

        // (#420) The letterbox rule Unity itself uses (whole-point rounding of the flipped target
        // rect) and the rest of the matched-pane arithmetic live in DisplayXRGameViewPlacement
        // (pure C#, unit-tested in Tests~/GameViewPlacement).

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
        static int s_lastChromeSlackOnly = -1; // matched-pane placement mode (-1 unseen), logged on change
        // physClientW: the matched pane's client width in PHYSICAL px (s_panePhysClient), passed
        // by the matched path so the slack never mixes Unity's DPI space (GetClientRect from
        // managed code) with the physical RT width. -1 = unmatched fallback, read GetClientRect.
        static int RtCentringOffsetX(int rtWidth, int physClientW = -1)
        {
            if (!s_hwndCorrValid || s_hwndCorrHwnd == System.IntPtr.Zero)
                return s_rtCentX < 0 ? 0 : s_rtCentX;
            int clientW;
            if (physClientW > 0) clientW = physClientW;
            else if (GetClientRect(s_hwndCorrHwnd, out var cr)) clientW = cr.right - cr.left;
            else return s_rtCentX < 0 ? 0 : s_rtCentX;
            int slack = clientW - rtWidth;
            if (slack <= 0 || slack > 64)          // transient realloc / implausible match:
                return s_rtCentX < 0 ? 0 : s_rtCentX; // hold last good, never pop to 0
            int cand = DisplayXRGameViewPlacement.CentringX(slack);
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
        // (#420) `scale` must be THIS PANE's points->px (ptToPx in the matched path), not the
        // global EditorGUIUtility.pixelsPerPoint: on a 300 % secondary with a 100 % primary the
        // global value is 1.0 (never latched, it must exceed 1.1), so the margin came out -2 px
        // instead of -5 and the docked anchor sat 3-4 px low, the size of error the eye sees as
        // inverted. physClientH: as RtCentringOffsetX's physClientW, the guard in one DPI space.
        // (= DisplayXRGameViewPlacement.RtBottomMarginPt, 1.6 pt)
        static int s_rtCentY = 0;   // latched (0 is the correct default: no margin until seen)
        static int RtCentringOffsetY(int rtHeight, float scale, int physClientH = -1)
        {
            if (!s_hwndCorrValid || s_hwndCorrHwnd == System.IntPtr.Zero) return s_rtCentY;
            int clientH;
            if (physClientH > 0) clientH = physClientH;
            else if (GetClientRect(s_hwndCorrHwnd, out var cr)) clientH = cr.bottom - cr.top;
            else return s_rtCentY;
            if (clientH <= rtHeight) return s_rtCentY; // transient realloc: hold last good
            int cand = DisplayXRGameViewPlacement.BottomMarginPx(scale);
            if (cand != s_rtCentY)
            {
                Debug.Log("[DisplayXR] #740 RtCentY latch " + s_rtCentY + "->" + cand
                          + " (clientH=" + clientH + " rtH=" + rtHeight + " scale=" + scale + ")");
                s_rtCentY = cand;
            }
            return s_rtCentY;
        }

        // (#459) PlayModeView.GetMainPlayModeView(): the Game view Unity treats as the main one.
        // Null when the API is missing, the main view is not a Game view (e.g. the device
        // simulator) or there is none; the caller then keeps its heuristic.
        static MethodInfo s_mainPlayModeViewMethod;
        static bool s_mainPlayModeViewResolved;
        static int  s_mainViewLogState;   // 0 unseen, 1 = same as the heuristic, 2 = differs
        static object TryGetMainPlayModeGameView()
        {
            if (!s_mainPlayModeViewResolved)
            {
                s_mainPlayModeViewResolved = true;
                try
                {
                    var pmv = System.Type.GetType("UnityEditor.PlayModeView,UnityEditor");
                    if (pmv != null)
                        s_mainPlayModeViewMethod = pmv.GetMethod("GetMainPlayModeView",
                            BindingFlags.Static | BindingFlags.NonPublic | BindingFlags.Public,
                            null, System.Type.EmptyTypes, null);
                }
                catch { }
                if (s_mainPlayModeViewMethod == null && FirstTimeThisEditorSession("NoMainPlayModeView"))
                    Debug.LogWarning("[DisplayXR] GameView glue: PlayModeView.GetMainPlayModeView not found " +
                                     "(editor internals changed?) - with several Game views open the glue " +
                                     "falls back to matching by width.");
            }
            if (s_mainPlayModeViewMethod == null || s_gvType == null) return null;
            try
            {
                var v = s_mainPlayModeViewMethod.Invoke(null, null) as UnityEngine.Object;
                return v != null && s_gvType.IsInstanceOfType(v) ? v : null;
            }
            catch { return null; }
        }

        // (#459) The matched pane's left border in points: EditorWindow.position (the border
        // REMOVED) minus HostView.screenPosition (the border INCLUDED). Both are Unity points
        // of the same window, so the difference needs no scale and no Win32 call. 1 pt between
        // two docked views, 0 at the editor window's left edge, maximized and floating.
        static bool TryGetMatchedLeftBorderPt(Rect host, out float borderPt)
        {
            borderPt = 0f;
            if (s_matchedGameView == null) return false;
            try
            {
                if (s_posProp == null)
                    for (var t = s_matchedGameView.GetType(); t != null && s_posProp == null; t = t.BaseType)
                        s_posProp = t.GetProperty("position",
                            BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic);
                if (s_posProp == null) return false;
                var pos = (Rect)s_posProp.GetValue(s_matchedGameView, null);
                float b = pos.x - host.x;
                if (!(b >= -0.01f && b <= 4f)) return false;   // not a border: implausible reading
                borderPt = Mathf.Max(0f, b);
                return true;
            }
            catch { return false; }
        }

        // (#459) IS THE GAME VIEW DRAWN 1:1? One log line each time the numbers settle. A
        // warning, once per editor session, only for what is certain from values read back
        // from Unity: a column mismatch, the glue's computed texture size or the canvas not
        // being the real texture's, the measured scale not being the window's DPI. The ROW
        // verdict depends on the GVFIT model and is unverified on hardware, so it stays inside
        // the log line. Changes nothing. Three things must all hold
        // for the woven image to reach the panel texel for pixel:
        //   W, H    Unity draws the render texture into a rect of the texture's own size. The
        //           rect is Unity's target size (px, fractional for Free Aspect) x the zoom
        //           area's scale = targetInView x the pane's scale; computed from the target
        //           and the zoom because targetInView goes through
        //           EditorGUIUtility.pixelsPerPoint, which is only valid inside OnGUI. The raw
        //           targetInView / viewInWindow points are logged next to it.
        //   canvas  the size the glue drives the woven canvas to equals the texture's: the
        //           native mirror blit stretches the canvas over the whole texture with a
        //           LINEAR sampler, so one pixel of difference resamples every column.
        // DrawnPx / IsOneToOne / LinesDroppedOrDoubled / BorderPx are pure
        // (DisplayXRGameViewPlacement, unit-tested).
        struct OneToOneKey
        {
            public int RtW, RtH, CanvasW, CanvasH, AnchorDx, AnchorDy;
            public float TargetW, TargetH, ZoomX, ZoomY, Scale, BorderPt;
            public bool WholePixel, HaveZoom, Degraded;
            public bool Same(in OneToOneKey o) =>
                RtW == o.RtW && RtH == o.RtH && CanvasW == o.CanvasW && CanvasH == o.CanvasH
                && AnchorDx == o.AnchorDx && AnchorDy == o.AnchorDy
                && TargetW == o.TargetW && TargetH == o.TargetH && ZoomX == o.ZoomX && ZoomY == o.ZoomY
                && Scale == o.Scale && BorderPt == o.BorderPt && WholePixel == o.WholePixel && HaveZoom == o.HaveZoom
                && Degraded == o.Degraded;
        }
        static OneToOneKey s_o2oPend, s_o2oLogged;
        static bool   s_o2oHavePend, s_o2oHaveLogged;
        static double s_o2oPendSince;
        static bool   s_o2oWarnedW, s_o2oWarnedGlue, s_o2oWarnedCanvas, s_o2oWarnedDpi, s_o2oWarnedUnavailable;
        static PropertyInfo s_targetInViewProp, s_viewInWindowProp;
        static bool   s_o2oRectPropsResolved;

        // GetDpiForWindow answers in the WINDOW's own DPI awareness context, not the calling
        // thread's: the DPI of the monitor it is on for a per-monitor-aware window (Unity's
        // editor windows), the system DPI for a system-aware one, 96 for an unaware one. It
        // is a scale, not a coordinate; it is only LOGGED next to the measured pane scale as
        // a cross-check and never enters any arithmetic with a rect.
        [DllImport("user32.dll")] static extern uint GetDpiForWindow(System.IntPtr hWnd);

        static RenderTexture FirstGameViewRt(object view)
        {
            if (s_rtFields == null)
            {
                s_rtFields = new System.Collections.Generic.List<FieldInfo>();
                var rtFlags = BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic;
                for (var t = s_gvType; t != null; t = t.BaseType)
                    foreach (var fi in t.GetFields(rtFlags | BindingFlags.DeclaredOnly))
                        if (fi.FieldType == typeof(RenderTexture)) s_rtFields.Add(fi);
            }
            // The same pick as ApplyGameViewFit: the first live RenderTexture field.
            foreach (var rfi in s_rtFields)
                if (rfi.GetValue(view) is RenderTexture r && r != null) return r;
            return null;
        }

        // canvasW/H: the size TryGetGameViewRenderRect returned (what the glue drives the
        // woven canvas to once it settles); anchorX/Y: the anchor it returned, before the
        // phase nudge. All physical px of the pane's monitor, like s_panePhysWin.
        static void LogGameViewOneToOneOnChange(int anchorX, int anchorY, int canvasW, int canvasH)
        {
            if (!s_diagValid || s_matchedGameView == null || s_gvType == null) return;
            try
            {
                var rt = FirstGameViewRt(s_matchedGameView);
                if (rt == null || rt.width <= 1 || rt.height <= 1) return;
                var key = new OneToOneKey
                {
                    RtW = rt.width, RtH = rt.height, CanvasW = canvasW, CanvasH = canvasH,
                    AnchorDx = anchorX - s_diagPane.Left, AnchorDy = anchorY - s_diagPane.Top,
                    TargetW = s_diagTargetWPx, TargetH = s_diagTargetHPx, ZoomX = 1f, ZoomY = 1f,
                    Scale = DisplayXRGameViewPlacement.SnappedScale(s_diagPane.PtToPx),
                    BorderPt = s_diagPane.HaveLeftBorder ? s_diagPane.LeftBorderPt : -1f,
                    WholePixel = s_diagPane.WholePixelFixedSize,
                    Degraded = GameViewDegraded,
                };
                if (TryGetGameViewZoom(s_matchedGameView, out Vector2 sc))
                {
                    key.ZoomX = sc.x; key.ZoomY = sc.y; key.HaveZoom = true;
                }

                // Settle: the same hold as the size push, so a splitter drag logs once.
                double now = Time.realtimeSinceStartupAsDouble;
                if (!s_o2oHavePend || !key.Same(s_o2oPend))
                {
                    s_o2oPend = key; s_o2oHavePend = true; s_o2oPendSince = now;
                    return;
                }
                if (now - s_o2oPendSince < GameViewSettleSeconds) return;
                if (s_o2oHaveLogged && key.Same(s_o2oLogged)) return;
                s_o2oLogged = key; s_o2oHaveLogged = true;

                double drawnW = DisplayXRGameViewPlacement.DrawnPx(key.TargetW, key.ZoomX);
                double drawnH = DisplayXRGameViewPlacement.DrawnPx(key.TargetH, key.ZoomY);
                int lostW = DisplayXRGameViewPlacement.LinesDroppedOrDoubled(drawnW, key.RtW);
                int lostH = DisplayXRGameViewPlacement.LinesDroppedOrDoubled(drawnH, key.RtH);
                bool canvasOk = canvasW == key.RtW && canvasH == key.RtH;
                var inv = System.Globalization.CultureInfo.InvariantCulture;
                // The glue's own idea of the texture size (Mathf.RoundToInt of Unity's target,
                // i.e. halves to even) against the texture Unity really allocated (halves away
                // from zero, PlayModeView.RenderView): where they differ the canvas, and the
                // RtW / RtH the placement used, are a pixel off (doc: finding F4).
                int glueW = s_diagPane.RtW, glueH = s_diagPane.RtH;
                bool glueOk = glueW == key.RtW && glueH == key.RtH;
                string Axis(double drawn, int rtPx, int lost, string note) =>
                    drawn.ToString("F2", inv) + " px for a " + rtPx + " px texture (" +
                    (lost == 0 ? "1:1" : (lost < 0 ? "up to " + (-lost) + " dropped" : "up to " + lost + " doubled") + note) + ")";

                var sb = new System.Text.StringBuilder(448);
                sb.Append("W drawn ").Append(Axis(drawnW, key.RtW, lostW, ""));
                // The row verdict under GVFIT is a prediction from the zoom, not yet confirmed on
                // hardware (doc: finding F1): reported here, never raised as a warning.
                sb.Append(", H drawn ").Append(Axis(drawnH, key.RtH, lostH, key.WholePixel ? "" : " - predicted, unverified: F1"));
                sb.Append(" | texture=").Append(key.RtW).Append('x').Append(key.RtH)
                  .Append(" glue computed ").Append(glueW).Append('x').Append(glueH)
                  .Append(glueOk ? " (agree)" : " (DISAGREE)");
                sb.Append(" | target=").Append(key.TargetW.ToString("R", inv)).Append('x').Append(key.TargetH.ToString("R", inv));
                sb.Append(" px zoom=").Append(key.HaveZoom
                    ? "(" + key.ZoomX.ToString("R", inv) + "," + key.ZoomY.ToString("R", inv) + ")" : "? (assumed 1)");
                sb.Append(" scale=").Append(key.Scale.ToString("R", inv));
                uint dpi = 0;
                if (Application.platform == RuntimePlatform.WindowsEditor && s_hwndCorrHwnd != System.IntPtr.Zero)
                {
                    try { dpi = GetDpiForWindow(s_hwndCorrHwnd); } catch { /* pre-1607 Windows */ }
                }
                if (dpi != 0) sb.Append(" (pane dpi ").Append(dpi).Append(" = ").Append((dpi / 96f).ToString("R", inv)).Append(')');
                sb.Append(" canvas=").Append(canvasW).Append('x').Append(canvasH)
                  .Append(canvasOk ? " (= texture)" : " (NOT the texture's size)");
                if (key.BorderPt >= 0f)
                    sb.Append(" | left border ").Append(key.BorderPt.ToString("0.###", inv)).Append(" pt = +")
                      .Append(DisplayXRGameViewPlacement.BorderPx(key.BorderPt, key.Scale)).Append(" px");
                sb.Append(" | anchor at pane +").Append(key.AnchorDx).Append(",+").Append(key.AnchorDy);
                if (!s_o2oRectPropsResolved)
                {
                    s_o2oRectPropsResolved = true;
                    var flags = BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic;
                    for (var t = s_gvType; t != null; t = t.BaseType)
                    {
                        if (s_targetInViewProp == null) s_targetInViewProp = t.GetProperty("targetInView", flags | BindingFlags.DeclaredOnly);
                        if (s_viewInWindowProp == null) s_viewInWindowProp = t.GetProperty("viewInWindow", flags | BindingFlags.DeclaredOnly);
                    }
                }
                try
                {
                    if (s_targetInViewProp != null && s_targetInViewProp.GetValue(s_matchedGameView, null) is Rect tiv)
                        sb.Append(" | targetInView=(").Append(tiv.x.ToString("F3", inv)).Append(',').Append(tiv.y.ToString("F3", inv))
                          .Append(' ').Append(tiv.width.ToString("F3", inv)).Append('x').Append(tiv.height.ToString("F3", inv)).Append(")pt");
                    if (s_viewInWindowProp != null && s_viewInWindowProp.GetValue(s_matchedGameView, null) is Rect viw)
                        sb.Append(" viewInWindow=(").Append(viw.width.ToString("F3", inv)).Append('x').Append(viw.height.ToString("F3", inv)).Append(")pt");
                }
                catch { /* raw rects are a courtesy; the verdict above does not use them */ }
                if (key.WholePixel) sb.Append(" [whole-pixel size]");
                if (key.Degraded)
                    sb.Append(s_gvScaled ? " [zoom is not 1: cannot be 1:1 - picture shown scaled, no calibration]"
                                         : " [computed anchor was off the pane: anchored on the render area, no calibration]");
                // Undocked = PRESENT mode: the runtime presents its own window over the pane, so
                // how Unity draws the texture underneath is not what reaches the panel.
                if (!s_lastAppliedDocked) sb.Append(" [present mode: covered by the weave window, no warning]");
                Debug.Log("[DisplayXR] GameView 1:1: " + sb);
                if (!s_lastAppliedDocked) return;
                // (#459) A zoomed view: the single "cannot be shown 1:1" warning says it all;
                // the per-condition warnings below would each repeat it.
                if (key.Degraded) return;

                // WARNINGS: only for conditions that are certain from values read back from
                // Unity (the real texture, the target, the zoom, the window DPI), and once per
                // EDITOR session, not per Play (the statics below do not survive the domain
                // reload of each Play; the SessionState flag does).
                const string kDoc = " See docs~/architecture/editor-game-view-placement.md.";
                string pct = (key.Scale * 100f).ToString("0.#", inv) + " %";
                if (lostW != 0 && !s_o2oWarnedW)
                {
                    s_o2oWarnedW = true;
                    if (FirstTimeThisEditorSession("O2O.W")) Debug.LogWarning("[DisplayXR] Game view is not drawn 1:1 horizontally: its " + key.RtW +
                        " px wide texture is drawn " + drawnW.ToString("F2", inv) + " px wide at " + pct +
                        ", so columns will be dropped/doubled and the image past them sits a pixel off " +
                        "(on a lenticular panel: the eyes swap there). A Windows scale of 100/150/200/250/300 % " +
                        "avoids it; the whole-pixel size fixes it and is selected automatically once the " +
                        "on-screen calibration measures it (DISPLAYXR_PROV_GV_WHOLEPIXEL=1 selects it from the start)." + kDoc);
                }
                if (!glueOk && !s_o2oWarnedGlue)
                {
                    s_o2oWarnedGlue = true;
                    if (FirstTimeThisEditorSession("O2O.Glue")) Debug.LogWarning(
                        "[DisplayXR] GameView glue: the Game view's texture is " + key.RtW + "x" + key.RtH +
                        " but the glue computed " + glueW + "x" + glueH + " from Unity's target size (" +
                        key.TargetW.ToString("R", inv) + "x" + key.TargetH.ToString("R", inv) +
                        " px, halves rounded to even; Unity rounds them up), so the woven canvas and the " +
                        "anchor's row are sized for a texture one pixel smaller." + kDoc);
                }
                if (!canvasOk && glueOk && !s_o2oWarnedCanvas)   // (with !glueOk the warning above says it)
                {
                    s_o2oWarnedCanvas = true;
                    if (FirstTimeThisEditorSession("O2O.Canvas")) Debug.LogWarning("[DisplayXR] Game view is not drawn 1:1: the woven canvas is driven to " +
                        canvasW + "x" + canvasH + " but the Game view's texture is " + key.RtW + "x" + key.RtH +
                        "; the mirror blit resamples one into the other (linear), which blurs the interlace " +
                        "across the whole view." + kDoc);
                }
                if (dpi != 0 && Mathf.Abs(dpi / 96f - key.Scale) > 0.011f && !s_o2oWarnedDpi)
                {
                    s_o2oWarnedDpi = true;
                    if (FirstTimeThisEditorSession("O2O.Dpi")) Debug.LogWarning("[DisplayXR] GameView glue: the pane's measured scale (" +
                        key.Scale.ToString("R", inv) + ") is not its window DPI (" + dpi + " = " +
                        (dpi / 96f).ToString("R", inv) + "); the placement uses the measured one." + kDoc);
                }
            }
            catch (System.Exception e)
            {
                if (!s_o2oWarnedUnavailable)
                {
                    s_o2oWarnedUnavailable = true;
                    if (FirstTimeThisEditorSession("O2O.Unavailable")) Debug.LogWarning("[DisplayXR] GameView 1:1 check unavailable (editor internals changed?): " + e.Message);
                }
            }
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
            s_diagValid = false;   // (#459) set again by the matched path below
            s_gvScaled = s_gvOffPane = false;
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
            // (#459) Prefer the view Unity itself calls the main play-mode view: it is the one
            // GetMainPlayModeViewTargetSize (the size above) describes, and the one Screen and
            // input refer to. The width heuristic above stays as the fallback (API missing, the
            // main view is not a Game view, or it has no host yet). With a single Game view
            // both pick that view, so the result is unchanged; with several, the heuristic
            // compared host POINTS with a size that is px once Low Resolution is off, i.e. it
            // picked the widest view rather than the main one.
            if (wins != null && s_screenPosProp != null)
            {
                object mainView = TryGetMainPlayModeGameView();
                if (mainView != null)
                {
                    try
                    {
                        var mpr = s_parentField.GetValue(mainView);
                        if (mpr != null)
                        {
                            bool differs = !SameLiveObject(mainView, matchedWin);
                            host = (Rect)s_screenPosProp.GetValue(mpr, null);
                            haveHost = true; matchedWin = mainView;
                            int state = differs ? 2 : 1;
                            if (state != s_mainViewLogState && (differs || s_mainViewLogState == 2))
                                Debug.Log("[DisplayXR] GameView glue: bound to the main play-mode view" +
                                          (differs ? " (the width heuristic picked another of " + wins.Length + " Game views)"
                                                   : " (the width heuristic agrees again)"));
                            s_mainViewLogState = state;
                        }
                    }
                    catch { /* keep the heuristic's pick */ }
                }
            }
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
                    bool haveDraw = TryGetPaneDrawRectPoints(out Rect draw) && host.height > draw.height;
                    if (haveDraw)
                        toolbarPhys = DisplayXRGameViewPlacement.ToolbarPx(host.height, draw.height, ptToPx);
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
                    //
                    // The docked chrome slack and a letterbox are two descriptions of the SAME
                    // slack — apply exactly one. The (#740) RT-centring corrections were measured
                    // against a BOTTOM-ALIGNED base (top = client height - RT height), not
                    // against this toolbar-inset, centred one: stacking them double-counted the
                    // X centring (+3 letterbox, +4 RtCentX at slack 7) and halved the measured
                    // 4 px bottom margin before subtracting it in full — about +3 / -2 px, which
                    // the slanted lens turns into a near half-cycle docked phase flip (#420; bisected
                    // v2.15.0 good -> a7d3fe1 bad). Free Aspect docked = chrome slack only (a few
                    // px): restore the HW-verified #740 placement, in this pane's physical px.
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
                        // (#420) The chrome-slack vs letterbox decision and both placements are
                        // DisplayXRGameViewPlacement (pure C#, unit-tested): see its comments for
                        // the measurements behind each term. Everything is in THIS pane's
                        // physical px and its own ptToPx, never the global ppp.
                        var pane = new DisplayXRGameViewPlacement.Pane
                        {
                            Left = s_panePhysWin.left, Top = s_panePhysWin.top,
                            ClientW = pcw, ClientH = pch, ToolbarPx = toolbarPhys,
                            RtW = rtW, RtH = rtH, TargetHPx = size.y * sizeScale,
                            DockedNonMaximized = s_lastAppliedDocked && !IsMatchedGameViewMaximized(),
                            HaveDrawRect = haveDraw,
                            HostWPt = host.width, DrawWPt = draw.width, DrawHPt = draw.height,
                            HostHPt = host.height,
                            PtToPx = ptToPx,
                            RoundsBottomEdge = SystemInfo.graphicsUVStartsAtTop,
                        };
                        // (#459) The pane's real left border, for the 1:1 diagnostic; the
                        // placement reads it (and drops the GVFIT stretch) only on the view
                        // that shows the opt-in whole-pixel size.
                        pane.HaveLeftBorder = TryGetMatchedLeftBorderPt(host, out pane.LeftBorderPt);
                        pane.WholePixelFixedSize = SameLiveObject(s_matchedGameView, WholePixelView);
                        // (#459) The view's zoom, in docked (texture) mode only: there Unity's
                        // own draw is what reaches the panel, so a zoom other than 1 cannot be
                        // 1:1 and the zoom-1 arithmetic below does not describe it. A floating
                        // view is covered by the runtime's own window (present mode) and keeps
                        // the letterbox placement it always had.
                        s_diagZoom = default;
                        if (s_lastAppliedDocked && TryGetGameViewZoom(s_matchedGameView, out s_diagZoom))
                            pane.ZoomX = s_diagZoom.x;
                        s_diagPane = pane;
                        s_diagTargetWPx = size.x * sizeScale;
                        s_diagTargetHPx = size.y * sizeScale;
                        s_diagValid = true;
                        bool chromeSlackOnly = DisplayXRGameViewPlacement.IsChromeSlackOnly(pane);
                        if ((chromeSlackOnly ? 1 : 0) != s_lastChromeSlackOnly)
                        {
                            s_lastChromeSlackOnly = chromeSlackOnly ? 1 : 0;
                            Debug.Log("[DisplayXR] GameView glue placement: "
                                      + (chromeSlackOnly ? "docked chrome slack (#740 RT-centring)" : "letterbox centring")
                                      + " (pane=" + pcw + "x" + pch + " area=" + rw + "x" + rh
                                      + " rt=" + rtW + "x" + rtH + ")");
                        }
                        // The #740 latches (they hold the last good value through realloc
                        // transients and log changes); each is read only where it applies.
                        // The Y guard takes the drawn height, which never exceeds the area.
                        // (#420) A docked pane on D3D with the draw rect takes its Y from the
                        // row Unity shows the RT's row 0 on (DisplayXRGameViewPlacement.RtRow0Y);
                        // the Y latch is then only the fallback (no draw rect, GL).
                        int centX = chromeSlackOnly ? RtCentringOffsetX(rtW, pcw) : 0;
                        int centY = pane.DockedNonMaximized
                            ? RtCentringOffsetY(Mathf.Min(rtH, rh), ptToPx, pch) : 0;
                        DisplayXRGameViewPlacement.Place(pane, centX, centY, out rx, out ry, out rw, out rh);
                        // (#459) Degraded, not black: a zoomed view was placed by PlaceScaled
                        // (inside the render area); and whatever the cause, an anchor that
                        // comes out off the pane is replaced by the render area.
                        s_gvScaled = DisplayXRGameViewPlacement.IsScaledView(pane.ZoomX);
                        if (s_lastAppliedDocked)
                        {
                            int bx = rx, by = ry;
                            if (!DisplayXRGameViewPlacement.KeepInsidePane(pane, ref rx, ref ry, ref rw, ref rh))
                            { s_gvOffPane = true; s_gvOffPaneDx = bx - pane.Left; s_gvOffPaneDy = by - pane.Top; }
                        }
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
                // (#420) GetMainGameViewTargetSize reports POINTS for the first frames (while
                // "Low Resolution Aspect Ratios" is still on) and px after; say which, so a
                // 1469.333x719.6667 at 150 % does not read as a 1.5x-too-small RT.
                string units = sizeScale != 1f ? $" pt x{sizeScale:F3}" : "";
                dbg = $"mainSize=({size.x}x{size.y}{units}) host(points)=({host.x},{host.y} {host.width}x{host.height}) " +
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

        // ---- (#459) Measured Game-view image position ---------------------------------
        // The computed anchor (TryGetGameViewRenderRect) is right at 250 % and one pixel off
        // at 175 % (the image is centred in a 1122.5 px area and Unity snaps it; +1 measured,
        // +2 computed: inverted 3D). DisplayXRGameViewCalibrator measures it on screen instead
        // and this applies the result. Windows editor, docked texture mode, matched pane only.
        static bool s_calLastValid;          // last frame: computed rect in -> anchor out
        static int  s_calLastInX, s_calLastInY, s_calLastW, s_calLastH, s_calLastOutX, s_calLastOutY;
        static int  s_imgRectFrame = -1;     // Time.frameCount the image rect below is from
        static int  s_imgX, s_imgY, s_imgW, s_imgH;   // in the pane's client area, physical px
        static bool s_imgMeasured;

        static void ApplyGameViewCalibration(ref int px, ref int py, int pw, int ph)
        {
            bool can = Application.platform == RuntimePlatform.WindowsEditor && s_lastAppliedDocked
                       && s_panePhysValid && s_hwndCorrValid && s_hwndCorrHwnd != System.IntPtr.Zero
                       && !BindPaneEnabled() && !ZoneGlueEnabled();
            if (!can)
            {
                s_calLastValid = false;
                DisplayXRGameViewCalibrator.Abort();
                return;
            }
            if (s_diagValid && GameViewDegraded)
            {
                // (#459) Zoomed (or the anchor was off the pane): the marker cannot be decoded
                // under scaling, so it is not shown at all. The computed rect is the answer.
                s_calLastValid = false;
                DisplayXRGameViewCalibrator.Abort();
                s_imgX = px - s_panePhysWin.left; s_imgY = py - s_panePhysWin.top;
                s_imgW = pw; s_imgH = ph; s_imgMeasured = false;
                s_imgRectFrame = Time.frameCount;
                return;
            }
            if (!s_diagValid)
            {
                // A transient reading: the glue handed back its last good rect. Keep the
                // anchor that rect had, so a measured anchor does not jump for one frame.
                // The image rect of that frame is still the one in use: keep it current, so
                // TryGetGameViewImageRect answers what the anchor is placed with (#471).
                if (s_calLastValid && px == s_calLastInX && py == s_calLastInY && pw == s_calLastW && ph == s_calLastH)
                {
                    px = s_calLastOutX; py = s_calLastOutY;
                    if (s_imgRectFrame >= 0) s_imgRectFrame = Time.frameCount;
                }
                return;
            }
            var layout = new DisplayXRGameViewCalibrator.Layout
            {
                Pane = s_hwndCorrHwnd,
                Monitor = MonitorFromWindow(s_hwndCorrHwnd, 2 /* MONITOR_DEFAULTTONEAREST */),
                PaneLeft = s_panePhysWin.left, PaneTop = s_panePhysWin.top,
                PaneW = s_panePhysClient.right - s_panePhysClient.left,
                PaneH = s_panePhysClient.bottom - s_panePhysClient.top,
                X = px, Y = py, W = pw, H = ph,
                Maximized = !s_diagPane.DockedNonMaximized,
                WholePixel = s_diagPane.WholePixelFixedSize,
                Scale = DisplayXRGameViewPlacement.SnappedScale(s_diagPane.PtToPx),
            };
            s_calLastInX = px; s_calLastInY = py; s_calLastW = pw; s_calLastH = ph;
            try { DisplayXRGameViewCalibrator.Tick(layout, ref px, ref py); }
            catch (System.Exception e)
            {
                // A missing native export (an older plug-in binary): computed placement only.
                DisplayXRGameViewCalibrator.Disable();
                if (FirstTimeThisEditorSession("GvCalib.Unavailable"))
                    Debug.LogWarning("[DisplayXR] GameView calibration unavailable, using the computed position: " + e.Message);
                px = s_calLastInX; py = s_calLastInY;
            }
            s_calLastOutX = px; s_calLastOutY = py; s_calLastValid = true;

            bool measured = DisplayXRGameViewCalibrator.AppliedMeasured;
            s_imgX = px - layout.PaneLeft; s_imgY = py - layout.PaneTop;
            s_imgW = measured ? DisplayXRGameViewCalibrator.MeasuredW : pw;
            s_imgH = measured ? DisplayXRGameViewCalibrator.MeasuredH : ph;
            s_imgMeasured = measured;
            s_imgRectFrame = Time.frameCount;
        }

        /// <summary>
        /// (#459) Where the docked editor Game view's image is, for code that binds to it
        /// (#471). <paramref name="clientX"/> / <paramref name="clientY"/>: the image's
        /// top-left in the matched pane window's CLIENT area, in PHYSICAL px of the pane's
        /// monitor (the pane is a borderless child, so its client origin is its window
        /// origin); <paramref name="width"/> / <paramref name="height"/>: its size in px;
        /// <paramref name="measured"/>: true when the position was read off the screen by the
        /// calibration, false when it is the computed one. Without the phase nudge. False
        /// (all zero) unless the glue placed the image this frame or the last: outside the
        /// Windows editor, outside Play, with a floating Game view (present mode), or before
        /// a pane is matched.
        /// </summary>
        internal static bool TryGetGameViewImageRect(out int clientX, out int clientY,
                                                     out int width, out int height, out bool measured)
        {
            clientX = clientY = width = height = 0; measured = false;
            if (!IsActive || s_imgRectFrame < 0 || Time.frameCount - s_imgRectFrame > 1) return false;
            clientX = s_imgX; clientY = s_imgY;
            width = s_imgW; height = s_imgH;
            measured = s_imgMeasured;
            return true;
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
            // (#459) Is the Game view drawn texel for pixel? Log-only; before the nudge so the
            // logged anchor is the computed one.
            LogGameViewOneToOneOnChange(px, py, pw, ph);
            WarnGameViewDegradedOnce();   // (#459) one warning when the view cannot be 1:1
            // (#459) Where the image really is: once the layout has settled a marker is read
            // back from the screen and the anchor goes to the MEASURED origin; the computed
            // one above stays as the fallback. The nudge below still applies on top.
            ApplyGameViewCalibration(ref px, ref py, pw, ph);
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
            const double kResizeSettleSeconds = GameViewSettleSeconds;
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

        // The active camera's rig components, looked up once per ActiveCamera change
        // instead of twice per frame (#451). Re-queried while the camera carries
        // neither, so a rig added to the same camera later is still picked up.
        Camera           m_TunablesCam;
        DisplayXRDisplay m_TunablesDisp;
        DisplayXRCamera  m_TunablesCamRig;

        void PushActiveRigTunables()
        {
            if (DisplayXRRigManager.SplashActive) return;

            Camera cam = DisplayXRRigManager.ActiveCamera;
            if (cam == null) return;

            if (!ReferenceEquals(cam, m_TunablesCam) || (m_TunablesDisp == null && m_TunablesCamRig == null))
            {
                m_TunablesCam    = cam;
                m_TunablesDisp   = cam.GetComponent<DisplayXRDisplay>();
                m_TunablesCamRig = cam.GetComponent<DisplayXRCamera>();
            }

            // Read the active rig's tunables (raw fields; no DisplayXRFeature dep).
            DisplayXRTunables t;
            var disp = m_TunablesDisp;
            var camRig = m_TunablesCamRig;
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
            DisplayXRProvider.RigPlaneRotation = q;
            DisplayXRProvider.RigPlaneValid   = true;
            DisplayXRProviderNative.dxr_prov_set_display_pose(p.x, p.y, p.z, q.x, q.y, q.z, q.w, 1);
        }
    }
}
