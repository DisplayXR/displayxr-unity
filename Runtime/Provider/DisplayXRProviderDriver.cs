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
        // Push the Unity Game view's on-screen rect to native each frame so the dedicated
        // weave window is glued to it → window-relative Kooima + weaver phase track where
        // the mirror-blit shows the woven output. Pure reflection into UnityEditor.GameView
        // so the Runtime asmdef needs no UnityEditor reference (Type resolves to null in a
        // built player → the whole path is inert there and gated to editor + probe anyway).
        static int  s_probeGate = -1;   // -1 unknown, 0 off, 1 on
        System.Type m_GameViewType;
        FieldInfo   m_ParentField;      // EditorWindow.m_Parent (HostView)
        PropertyInfo m_ScreenPosProp;   // GUIView.screenPosition (Rect, LOGICAL points)
        PropertyInfo m_PixelsPerPointProp; // EditorGUIUtility.pixelsPerPoint (DPI scale)
        bool        m_LoggedGlueOnce;

        void PushGameViewRectEditorProbe()
        {
            if (!Application.isEditor) return;
            if (s_probeGate < 0)
            {
                s_probeGate = System.Environment.GetEnvironmentVariable(
                    "DISPLAYXR_PROV_TEXTURE_PROBE") == "1" ? 1 : 0;
            }
            if (s_probeGate == 0) return;

            if (m_GameViewType == null)
            {
                m_GameViewType = System.Type.GetType("UnityEditor.GameView,UnityEditor");
                if (m_GameViewType == null) { s_probeGate = 0; return; } // not in editor
            }

            // Resolve EditorWindow.m_Parent (HostView) from the type (walk the hierarchy
            // for the non-public base field).
            if (m_ParentField == null)
            {
                for (var t = m_GameViewType; t != null && m_ParentField == null; t = t.BaseType)
                    m_ParentField = t.GetField("m_Parent",
                        BindingFlags.Instance | BindingFlags.NonPublic | BindingFlags.Public);
                if (m_ParentField == null) return;
            }

            // Enumerate ALL GameView instances and pick the LARGEST-area one — the visible
            // panel. FindObjectsOfTypeAll can also return smaller/background game views, and
            // gluing to the wrong (small) one makes Unity size the mirror RT smaller than the
            // visible panel → the woven output shows at a fraction of the tab.
            var wins = Resources.FindObjectsOfTypeAll(m_GameViewType);
            if (wins == null || wins.Length == 0) return;
            Rect r = default; float bestArea = -1f; bool found = false;
            foreach (var w in wins)
            {
                var pr = m_ParentField.GetValue(w);
                if (pr == null) continue;
                if (m_ScreenPosProp == null)
                {
                    for (var t = pr.GetType(); t != null && m_ScreenPosProp == null; t = t.BaseType)
                        m_ScreenPosProp = t.GetProperty("screenPosition",
                            BindingFlags.Instance | BindingFlags.NonPublic | BindingFlags.Public);
                    if (m_ScreenPosProp == null) return;
                }
                var wr = (Rect)m_ScreenPosProp.GetValue(pr, null);
                float area = wr.width * wr.height;
                if (!m_LoggedGlueOnce)
                    Debug.Log($"[DisplayXR] GameView candidate: screenPos(points)=({wr.x},{wr.y} {wr.width}x{wr.height}) area={area}");
                if (area > bestArea) { bestArea = area; r = wr; found = true; }
            }
            if (!found) return;

            // screenPosition is in LOGICAL points; the weave window + runtime want PHYSICAL
            // pixels. Scale by the editor's DPI (EditorGUIUtility.pixelsPerPoint) — e.g. 2.5
            // at 250% Windows scaling, where the raw 836x422 points → 2090x1055 px.
            float ppp = 1f;
            if (m_PixelsPerPointProp == null)
            {
                var eguiType = System.Type.GetType("UnityEditor.EditorGUIUtility,UnityEditor");
                if (eguiType != null)
                    m_PixelsPerPointProp = eguiType.GetProperty("pixelsPerPoint",
                        BindingFlags.Static | BindingFlags.Public | BindingFlags.NonPublic);
            }
            if (m_PixelsPerPointProp != null)
            {
                object v = m_PixelsPerPointProp.GetValue(null, null);
                if (v is float f && f > 0f) ppp = f;
            }

            int px = Mathf.RoundToInt(r.x * ppp);
            int py = Mathf.RoundToInt(r.y * ppp);
            int pw = Mathf.RoundToInt(r.width * ppp);
            int ph = Mathf.RoundToInt(r.height * ppp);

            if (!m_LoggedGlueOnce)
            {
                m_LoggedGlueOnce = true;
                Debug.Log($"[DisplayXR] GameView glue: screenPos(points)=({r.x},{r.y} {r.width}x{r.height}) " +
                          $"ppp={ppp} -> physical=({px},{py} {pw}x{ph})");
            }

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
