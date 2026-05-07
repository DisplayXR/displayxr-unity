// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0

using UnityEngine;
using UnityEngine.Rendering;

namespace DisplayXR
{
    /// <summary>
    /// Camera-centric stereo rig. Attach to a Camera whose transform represents the
    /// viewer pose. The camera's vertical FOV is inherited as the rendering FOV.
    /// Exposes only: IPD, parallax, and inverse convergence distance.
    /// </summary>
    [AddComponentMenu("DisplayXR/Camera-Centric Rig")]
    [DisallowMultipleComponent]
    [RequireComponent(typeof(Camera))]
    public class DisplayXRCamera : MonoBehaviour
    {
        [Header("Stereo Tunables")]

        [Tooltip("Scales inter-eye distance. 1.0 = natural, <1 = reduced stereo, >1 = exaggerated.")]
        [Range(0f, 3f)]
        public float ipdFactor = 1.0f;

        [Tooltip("Scales eye X/Y offset. 1.0 = natural parallax.")]
        [Range(0f, 3f)]
        public float parallaxFactor = 1.0f;

        [Header("Convergence")]

        [Tooltip("Inverse convergence distance (1/meters). 0 = infinity (parallel projection). " +
                 "Higher values = screen plane closer to camera.")]
        [Range(0f, 10f)]
        public float invConvergenceDistance = 0f;

        [Header("Debug")]

        [Tooltip("Show eye tracking status in the console.")]
        public bool logEyeTracking;

        private DisplayXRFeature m_Feature;
        private float m_CachedCameraFov;
        private Camera m_Camera;
        // True when running under URP/HDRP. BiRP fires Camera.onPreRender; SRP doesn't,
        // so we route through RenderPipelineManager.beginCameraRendering instead.
        private bool m_UsingSRP;

#if UNITY_EDITOR
        void OnValidate()
        {
            if (GetComponent<DisplayXRDisplay>() != null)
                Debug.LogError("[DisplayXR] DisplayXRCamera and DisplayXRDisplay cannot coexist on the same GameObject. Remove one.", this);
        }
#endif

        void OnEnable()
        {
            m_Feature = DisplayXRFeature.Instance;
            m_Camera = GetComponent<Camera>();
            // Cache the camera's FOV BEFORE XR overrides it. Once XR is active,
            // Camera.fieldOfView returns the Kooima FOV we set, creating a
            // feedback loop that collapses the FOV to zero.
            m_CachedCameraFov = m_Camera.fieldOfView;
            // Guard against XR having already overridden the FOV to near-zero
            if (m_CachedCameraFov < 1.0f)
                m_CachedCameraFov = 60.0f;
            m_UsingSRP = GraphicsSettings.currentRenderPipeline != null;
            if (m_UsingSRP)
                RenderPipelineManager.beginCameraRendering += OnSRPBeginCamera;
            else
                Camera.onPreRender += OnCameraPreRender;
            DisplayXRRigManager.Register(m_Camera);
        }

        void OnDisable()
        {
            Debug.Log("[DisplayXR] DisplayXRCamera.OnDisable");
            if (m_UsingSRP)
                RenderPipelineManager.beginCameraRendering -= OnSRPBeginCamera;
            else
                Camera.onPreRender -= OnCameraPreRender;
            DisplayXRRigManager.Unregister(m_Camera);
            m_Feature = null;
        }

        void OnSRPBeginCamera(ScriptableRenderContext ctx, Camera cam) => OnCameraPreRender(cam);

        void OnCameraPreRender(Camera cam)
        {
            if (cam != m_Camera || m_Feature == null) return;

            if (!m_Feature.GetStereoMatrices(out Matrix4x4 leftView, out Matrix4x4 leftProj,
                                              out Matrix4x4 rightView, out Matrix4x4 rightProj))
                return;

            // Convert view matrices from OpenXR convention (right-hand, -Z forward) to
            // Unity world convention (left-hand, +Z forward) by negating column 2.
            leftView = FlipViewZ(leftView);
            rightView = FlipViewZ(rightView);

            cam.SetStereoViewMatrix(Camera.StereoscopicEye.Left, leftView);
            cam.SetStereoViewMatrix(Camera.StereoscopicEye.Right, rightView);
            cam.SetStereoProjectionMatrix(Camera.StereoscopicEye.Left, leftProj);
            cam.SetStereoProjectionMatrix(Camera.StereoscopicEye.Right, rightProj);
        }

        /// <summary>Negate column 2 (Z) of a view matrix to convert OpenXR → Unity world handedness.</summary>
        static Matrix4x4 FlipViewZ(Matrix4x4 m)
        {
            m.m02 = -m.m02;
            m.m12 = -m.m12;
            m.m22 = -m.m22;
            m.m32 = -m.m32;
            return m;
        }

        void LateUpdate()
        {
            // Only the active rig pushes tunables (prevents multi-rig conflicts)
            var active = DisplayXRRigManager.ActiveCamera;
            if (active != null && active != m_Camera) return;

            if (m_Feature == null)
            {
                m_Feature = DisplayXRFeature.Instance;
                if (m_Feature == null) return;
            }

            // In play mode, XR overwrites cam.fieldOfView with the Kooima FOV
            // each frame — reading it back creates a feedback loop that blows
            // up to ~180°.  Only sync from the camera in edit mode (inspector edits).
            if (!Application.isPlaying)
            {
                float currentFov = m_Camera.fieldOfView;
                if (currentFov >= 1.0f)
                    m_CachedCameraFov = currentFov;
            }

            // Compute half_tan_vfov from the cached camera FOV
            float halfTanVfov = Mathf.Tan(m_CachedCameraFov * 0.5f * Mathf.Deg2Rad);

            // Convert camera-centric params to native tunables
            var tunables = new DisplayXRTunables
            {
                ipdFactor = ipdFactor,
                parallaxFactor = parallaxFactor,
                perspectiveFactor = 1.0f,
                virtualDisplayHeight = 0f,
                invConvergenceDistance = invConvergenceDistance,
                fovOverride = halfTanVfov,
                nearZ = m_Camera.nearClipPlane,
                farZ = m_Camera.farClipPlane,
                cameraCentricMode = true,
            };

            m_Feature.SetTunables(tunables);

            // Push viewport size and screen position for window-relative Kooima.
            // On Windows, native WM_SIZE handler overrides with accurate HWND position.
            m_Feature.SetViewportSize(Screen.width, Screen.height, 0, 0);

            // Send camera world pose + scale to native
            m_Feature.SetSceneTransform(
                transform.position,
                transform.rotation,
                transform.lossyScale,
                enabled: true);

            m_Feature.RefreshEyePositions();

            if (logEyeTracking)
            {
                Debug.Log($"[DisplayXR] Eyes: L={m_Feature.LeftEyePosition}, " +
                          $"R={m_Feature.RightEyePosition}, tracked={m_Feature.IsEyeTracked}" +
                          $" camPos={transform.position} fov={m_CachedCameraFov:F1}" +
                          $" invd={invConvergenceDistance:F4}");
            }
        }

#if UNITY_EDITOR
        void OnDrawGizmosSelected()
        {
            // Draw virtual screen plane at convergence distance (if finite)
            if (invConvergenceDistance < 0.001f) return;

            float convergenceDist = 1.0f / invConvergenceDistance;
            var info = DisplayXRFeature.Instance?.DisplayInfo ?? default;

            float aspect = info.isValid
                ? info.displayWidthMeters / info.displayHeightMeters
                : 16f / 9f;

            float fovDeg = m_CachedCameraFov > 0f ? m_CachedCameraFov
                         : GetComponent<Camera>().fieldOfView;
            float halfH = convergenceDist * Mathf.Tan(fovDeg * 0.5f * Mathf.Deg2Rad);

            float w = halfH * 2f * aspect;
            float h = halfH * 2f;

            Gizmos.color = new Color(1f, 0.6f, 0.2f, 0.3f);
            Gizmos.matrix = transform.localToWorldMatrix;
            Vector3 center = new Vector3(0, 0, convergenceDist);
            Gizmos.DrawCube(center, new Vector3(w, h, 0.002f));

            Gizmos.color = new Color(1f, 0.6f, 0.2f, 0.8f);
            Gizmos.DrawWireCube(center, new Vector3(w, h, 0.002f));
        }
#endif
    }
}
