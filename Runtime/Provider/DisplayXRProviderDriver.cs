// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0

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
    /// The rigs (<see cref="DisplayXRDisplay"/> / <see cref="DisplayXRCamera"/>)
    /// still call <see cref="DisplayXRFeature"/> in their own LateUpdate, but that
    /// path is gated on the OpenXR hook being installed — inert in provider mode.
    /// This driver is the provider's push path; it reuses the rigs' read-only
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
            DisplayXRProviderNative.dxr_prov_set_display_pose(p.x, p.y, p.z, q.x, q.y, q.z, q.w, 1);
        }
    }
}
