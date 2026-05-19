// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0

using UnityEngine;
using UnityEngine.Rendering;

namespace DisplayXR
{
    /// <summary>
    /// Display-centric stereo rig. Attach to a Camera whose parent transform represents
    /// the virtual display pose (position and orientation of the display surface).
    /// The camera's FOV is ignored — the display's physical geometry and virtualDisplayHeight
    /// determine the frustum. Eyes move around the display based on tracking.
    /// </summary>
    [AddComponentMenu("DisplayXR/Display-Centric Rig")]
    [DisallowMultipleComponent]
    [RequireComponent(typeof(Camera))]
    public class DisplayXRDisplay : MonoBehaviour
    {
        [Header("Stereo Tunables")]

        [Tooltip("Scales inter-eye distance. 1.0 = natural, <1 = reduced stereo, >1 = exaggerated.")]
        [Range(0f, 3f)]
        public float ipdFactor = 1.0f;

        [Tooltip("Scales eye X/Y offset from display center. 1.0 = natural parallax.")]
        [Range(0f, 3f)]
        public float parallaxFactor = 1.0f;

        [Tooltip("Scales perceived depth. 1.0 = natural perspective.")]
        [Range(0f, 3f)]
        public float perspectiveFactor = 1.0f;

        [Tooltip("Virtual display height in meters. 0 = use physical display height.")]
        public float virtualDisplayHeight = 0f;

        [Tooltip("Foreground-only render: clip each view at its own |eye.z|*m2v " +
                 "(i.e. the per-view distance from eye to display plane). Geometry " +
                 "past the display plane is clipped automatically. N-view safe — " +
                 "each view's projection uses its own eye-Z. Camera.farClipPlane is " +
                 "ignored when this is enabled.")]
        public bool foregroundOnlyClip = false;

        [Header("Debug")]

        [Tooltip("Show eye tracking status in the console.")]
        public bool logEyeTracking;

        private DisplayXRFeature m_Feature;
        private Camera m_Camera;
        // True when running under URP/HDRP. BiRP fires Camera.onPreRender; SRP doesn't,
        // so we route through RenderPipelineManager.beginCameraRendering instead.
        private bool m_UsingSRP;

        void OnEnable()
        {
            m_Camera = GetComponent<Camera>();
            m_Feature = DisplayXRFeature.Instance;
            m_UsingSRP = GraphicsSettings.currentRenderPipeline != null;
            if (m_UsingSRP)
                RenderPipelineManager.beginCameraRendering += OnSRPBeginCamera;
            else
                Camera.onPreRender += OnCameraPreRender;
            DisplayXRRigManager.Register(m_Camera);
#if !UNITY_EDITOR
            if (m_Feature == null)
            {
                Debug.LogWarning("[DisplayXR] DisplayXRFeature not active. " +
                    "Enable it in Project Settings > XR Plug-in Management > OpenXR.");
            }
#endif
        }

        void OnDisable()
        {
            if (m_UsingSRP)
                RenderPipelineManager.beginCameraRendering -= OnSRPBeginCamera;
            else
                Camera.onPreRender -= OnCameraPreRender;
            DisplayXRRigManager.Unregister(m_Camera);
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
            // Without this, Unity objects at +Z end up behind the camera → black screen.
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

#if UNITY_EDITOR
        void OnValidate()
        {
            if (GetComponent<DisplayXRCamera>() != null)
                Debug.LogError("[DisplayXR] DisplayXRDisplay and DisplayXRCamera cannot coexist on the same GameObject. Remove one.", this);
        }
#endif

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

            // Resolve virtualDisplayHeight: 0 means use physical display height
            float vdh = virtualDisplayHeight;
            if (vdh <= 0f && m_Feature.DisplayInfo.isValid)
            {
                vdh = m_Feature.DisplayInfo.displayHeightMeters;
            }

            // Push tunables to native plugin — affects next xrLocateViews
            var tunables = new DisplayXRTunables
            {
                ipdFactor = ipdFactor,
                parallaxFactor = parallaxFactor,
                perspectiveFactor = perspectiveFactor,
                virtualDisplayHeight = vdh,
                invConvergenceDistance = 0f,
                fovOverride = 0f,
                nearZ = m_Camera.nearClipPlane,
                farZ = m_Camera.farClipPlane,
                cameraCentricMode = false,
                clipAtDisplayPlane = foregroundOnlyClip,
            };

            m_Feature.SetTunables(tunables);

            // Push viewport size and screen position for window-relative Kooima.
            // On Windows, native WM_SIZE handler overrides with accurate HWND position.
            m_Feature.SetViewportSize(Screen.width, Screen.height, 0, 0);

            // Push scene transform: parent camera's world pose is the display pose.
            // Transform scale acts as zoom: scale > 1 zooms in (display appears bigger).
            m_Feature.SetSceneTransform(
                transform.position,
                transform.rotation,
                transform.lossyScale,
                enabled: true);

            // Refresh eye positions for debug/UI
            m_Feature.RefreshEyePositions();

            if (logEyeTracking)
            {
                Debug.Log($"[DisplayXR] Display: pos={transform.position} " +
                          $"near={m_Camera.nearClipPlane} far={m_Camera.farClipPlane} " +
                          $"camWorldPos={m_Camera.transform.position} " +
                          $"Eyes: L={m_Feature.LeftEyePosition}, " +
                          $"R={m_Feature.RightEyePosition}, tracked={m_Feature.IsEyeTracked}");
            }
        }

#if UNITY_EDITOR
        // Gating: selection-driven in plain Edit Mode; active-rig-only when
        // a DisplayXR session is alive (preview window running, or Play Mode
        // with the XR loader). The two are mutually exclusive — exactly one
        // entry point draws per frame.
        void OnDrawGizmos()
        {
            if (!DisplayXRGizmoHelpers.IsSessionActive()) return;
            var cam = m_Camera != null ? m_Camera : GetComponent<Camera>();
            if (cam == null || DisplayXRRigManager.ActiveCamera != cam) return;
            DrawGizmosImpl();
        }

        void OnDrawGizmosSelected()
        {
            if (DisplayXRGizmoHelpers.IsSessionActive()) return;
            DrawGizmosImpl();
        }

        void DrawGizmosImpl()
        {
            // Prefer the runtime's pushed display info; fall back to a direct
            // native read so the gizmo works in Edit Mode preview where
            // DisplayXRFeature.Instance is null but the preview session has
            // still written into the native ring.
            DisplayXRDisplayInfo info = DisplayXRFeature.Instance != null
                ? DisplayXRFeature.Instance.DisplayInfo
                : DisplayXRGizmoHelpers.ReadDisplayInfoFromNative();

            float h = virtualDisplayHeight > 0f
                ? virtualDisplayHeight
                : (info.isValid ? info.displayHeightMeters : 0.2f);
            float w = info.isValid
                ? info.displayWidthMeters * (h / info.displayHeightMeters)
                : h * 1.5f;

            // Light-blue virtual display volume (restored to match the
            // pre-#111 OnDrawGizmosSelected aesthetic).
            Gizmos.matrix = transform.localToWorldMatrix;
            Gizmos.color = new Color(0.2f, 0.8f, 1.0f, 0.3f);
            Gizmos.DrawCube(Vector3.zero, new Vector3(w, h, 0.002f));
            Gizmos.color = new Color(0.2f, 0.8f, 1.0f, 0.8f);
            Gizmos.DrawWireCube(Vector3.zero, new Vector3(w, h, 0.002f));
            Gizmos.matrix = Matrix4x4.identity;

            float hw = w * 0.5f;
            float hh = h * 0.5f;
            Vector3 cBL = transform.TransformPoint(new Vector3(-hw, -hh, 0f));
            Vector3 cBR = transform.TransformPoint(new Vector3(+hw, -hh, 0f));
            Vector3 cTR = transform.TransformPoint(new Vector3(+hw, +hh, 0f));
            Vector3 cTL = transform.TransformPoint(new Vector3(-hw, +hh, 0f));

            DisplayXRGizmoHelpers.GetEyeWorldPositions(
                transform, info,
                ipdFactor, parallaxFactor, perspectiveFactor,
                cameraCentric: false,
                out Vector3 leftEye, out Vector3 rightEye, out bool isLive);

            var cam = m_Camera != null ? m_Camera : GetComponent<Camera>();
            Color frustumColor = Color.magenta;
            Color eyeColor = isLive ? DisplayXRGizmoHelpers.EyeGlyphLive
                                    : DisplayXRGizmoHelpers.EyeGlyphNominal;

            // Far distance for the truncated pyramid. Camera.farClipPlane
            // defaults to 1000 m which fills the entire scene — clamp to a
            // few display-widths so the gizmo stays legible.
            float farDist = 5f * Mathf.Max(w, h);
            if (cam != null) farDist = Mathf.Min(farDist, cam.farClipPlane);

            DisplayXRGizmoHelpers.DrawAsymmetricFrustum(leftEye, cBL, cBR, cTR, cTL, farDist, frustumColor);
            DisplayXRGizmoHelpers.DrawAsymmetricFrustum(rightEye, cBL, cBR, cTR, cTL, farDist, frustumColor);
            DisplayXRGizmoHelpers.DrawEyeGlyph(leftEye, transform.rotation, 0.015f, eyeColor);
            DisplayXRGizmoHelpers.DrawEyeGlyph(rightEye, transform.rotation, 0.015f, eyeColor);
        }
#endif
    }
}
