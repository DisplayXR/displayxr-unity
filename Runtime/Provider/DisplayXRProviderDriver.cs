// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

using System.Reflection;
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

        void LateUpdate()
        {
            if (DisplayXRProviderNative.dxr_prov_session_is_running() == 0)
            {
                m_SessionStarted = false;
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
        static int  s_probeGate = -1;   // -1 unknown, 0 off, 1 on
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

        static bool ProbeEnabled()
        {
            if (!Application.isEditor) return false;
            if (s_probeGate < 0)
                s_probeGate = System.Environment.GetEnvironmentVariable(
                    "DISPLAYXR_PROV_TEXTURE_PROBE") == "1" ? 1 : 0;
            return s_probeGate == 1;
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
                    if (area > bestArea) { bestArea = area; fallbackHost = wr; }
                    // GetMainGameViewTargetSize is in LOGICAL points, same units as
                    // screenPosition, so match panel width to it directly (ppp-independent).
                    if (size.x > 1f)
                    {
                        float score = Mathf.Abs(wr.width - size.x);
                        if (score < bestScore) { bestScore = score; host = wr; haveHost = true; }
                    }
                }
            if (!haveHost) { host = fallbackHost; haveHost = bestArea >= 0f; }

            // Compose the PHYSICAL render rect. GetMainGameViewTargetSize returns the render
            // area in LOGICAL points (excludes the toolbar), so multiply by ppp for physical
            // px — the mirror RT (== our swapchain) is presented 1:1 into the panel, so it must
            // be the panel's physical size to fill. Toolbar height = (host - render) in points,
            // scaled; it offsets the window's top below the Game view toolbar.
            int rw, rh, rx, ry, toolbar;
            if (size.x > 1f && size.y > 1f)
            {
                rw = Mathf.RoundToInt(size.x * ppp);
                rh = Mathf.RoundToInt(size.y * ppp);
                toolbar = Mathf.Max(0, Mathf.RoundToInt((host.height - size.y) * ppp));
            }
            else
            {
                rw = Mathf.RoundToInt(host.width * ppp);
                rh = Mathf.RoundToInt(host.height * ppp);
                toolbar = 0;
            }
            rx = Mathf.Max(0, Mathf.RoundToInt(host.x * ppp));
            ry = Mathf.Max(0, Mathf.RoundToInt(host.y * ppp) + toolbar);

            // Sanity: reject implausible SIZE readings (transient 0x0 during a play/layout
            // transition) and reuse the last-good rect so the zone never borns wrong-size.
            // The zone size comes from GetMainGameViewTargetSize (ppp-independent), so a
            // not-yet-latched ppp only offsets the initial POSITION (the per-frame glue
            // corrects it once ppp latches) — don't gate the zone on ppp.
            bool sane = haveHost && size.x > 1f && size.y > 1f
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

        // Called by the loader BEFORE the subsystem starts: stash the render rect so
        // session_start borns the forced zone at the panel's native resolution.
        public static void TryPushInitialGameViewRect()
        {
            if (!ProbeEnabled()) return;
            if (TryGetGameViewRenderRect(out int px, out int py, out int pw, out int ph, out string dbg))
            {
                DisplayXRProviderNative.dxr_prov_set_initial_gameview_rect(px, py, pw, ph);
                if (!s_loggedInitOnce) { s_loggedInitOnce = true; Debug.Log($"[DisplayXR] GameView initial rect: {dbg}"); }
            }
        }

        int m_LastGlueX = int.MinValue, m_LastGlueY, m_LastGlueW, m_LastGlueH;

        void PushGameViewRectEditorProbe()
        {
            if (!ProbeEnabled()) return;
            if (!TryGetGameViewRenderRect(out int px, out int py, out int pw, out int ph, out string dbg))
                return;
            if (!s_loggedGlueOnce) { s_loggedGlueOnce = true; Debug.Log($"[DisplayXR] GameView glue: {dbg}"); }
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
