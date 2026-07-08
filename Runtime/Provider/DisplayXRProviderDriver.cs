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
        static MemberInfo   s_targetSizeMember;  // GameView.targetSize (Vector2, PHYSICAL px)
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

        // Compute the visible Game view's RENDER-area rect in physical px. Size comes from
        // GameView.targetSize (the render-target pixel size — DPI-independent, excludes the
        // toolbar); position from the HostView screenPosition (points) × pixelsPerPoint,
        // offset down by the toolbar (= host physical height − render height). Returns false
        // outside the editor / before a GameView exists.
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
            if (s_parentField == null) return false;

            var wins = Resources.FindObjectsOfTypeAll(s_gvType);
            if (wins == null || wins.Length == 0) return false;

            // Pick the largest-area GameView (the visible panel).
            object best = null; Rect host = default; float bestArea = -1f;
            foreach (var w in wins)
            {
                var pr = s_parentField.GetValue(w);
                if (pr == null) continue;
                if (s_screenPosProp == null)
                    for (var t = pr.GetType(); t != null && s_screenPosProp == null; t = t.BaseType)
                        s_screenPosProp = t.GetProperty("screenPosition",
                            BindingFlags.Instance | BindingFlags.NonPublic | BindingFlags.Public);
                if (s_screenPosProp == null) return false;
                var wr = (Rect)s_screenPosProp.GetValue(pr, null);
                float area = wr.width * wr.height;
                if (area > bestArea) { bestArea = area; host = wr; best = w; }
            }
            if (best == null) return false;

            // Render size from GameView.targetSize (physical px, excludes toolbar). Property
            // or field depending on Unity version.
            if (s_targetSizeMember == null)
            {
                for (var t = s_gvType; t != null && s_targetSizeMember == null; t = t.BaseType)
                {
                    s_targetSizeMember = (MemberInfo)t.GetProperty("targetSize",
                        BindingFlags.Instance | BindingFlags.NonPublic | BindingFlags.Public)
                        ?? t.GetField("targetSize",
                        BindingFlags.Instance | BindingFlags.NonPublic | BindingFlags.Public);
                }
            }
            Vector2 target = Vector2.zero;
            if (s_targetSizeMember is PropertyInfo tp) { if (tp.GetValue(best, null) is Vector2 v) target = v; }
            else if (s_targetSizeMember is FieldInfo tf) { if (tf.GetValue(best) is Vector2 v) target = v; }
            bool haveTarget = target.x > 1f && target.y > 1f;

            // DPI scale (points -> physical px). EditorGUIUtility.pixelsPerPoint is UNRELIABLE
            // here — it depends on the current GUI context and flip-flops between the true DPI
            // and 1.0 across frames, which made the glued window's POSITION jump every frame
            // (weaver phase / pointer reference jitter → the drag fought itself). Derive it
            // instead from the stable ratio targetSize.x / host.width (both describe the same
            // render width, one in px one in points), falling back to pixelsPerPoint only if
            // targetSize is unavailable.
            float ppp = 1f;
            if (haveTarget && host.width > 1f)
            {
                ppp = target.x / host.width;
            }
            else
            {
                if (s_pppProp == null)
                {
                    var egui = System.Type.GetType("UnityEditor.EditorGUIUtility,UnityEditor");
                    if (egui != null)
                        s_pppProp = egui.GetProperty("pixelsPerPoint",
                            BindingFlags.Static | BindingFlags.Public | BindingFlags.NonPublic);
                }
                if (s_pppProp != null && s_pppProp.GetValue(null, null) is float f && f > 0f) ppp = f;
            }

            int hostPhysH = Mathf.RoundToInt(host.height * ppp);
            pw = haveTarget ? Mathf.RoundToInt(target.x) : Mathf.RoundToInt(host.width * ppp);
            ph = haveTarget ? Mathf.RoundToInt(target.y) : hostPhysH;
            int toolbar = Mathf.Max(0, hostPhysH - ph);
            px = Mathf.RoundToInt(host.x * ppp);
            py = Mathf.RoundToInt(host.y * ppp) + toolbar;

            dbg = $"host(points)=({host.x},{host.y} {host.width}x{host.height}) ppp={ppp:F3} " +
                  $"targetSize=({target.x}x{target.y}) toolbar={toolbar} -> render=({px},{py} {pw}x{ph})";
            return pw > 0 && ph > 0;
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

        void PushGameViewRectEditorProbe()
        {
            if (!ProbeEnabled()) return;
            if (!TryGetGameViewRenderRect(out int px, out int py, out int pw, out int ph, out string dbg))
                return;
            if (!s_loggedGlueOnce) { s_loggedGlueOnce = true; Debug.Log($"[DisplayXR] GameView glue: {dbg}"); }
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
