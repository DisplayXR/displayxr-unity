// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

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

        [Header("Rendering")]

        [Tooltip("Post-process FXAA on the eye render target. Unity drops MSAA on the XR " +
                 "eye RT (submits sampleCount=1 and its resolve loses fractional alpha), so " +
                 "silhouettes alias — most visibly in alpha-native transparent overlays. " +
                 "FXAA restores soft edges post-resolve where MSAA/supersampling can't. " +
                 "Disable if you don't need it.")]
        public bool postProcessAntiAliasing = true;

        [Header("Debug")]

        [Tooltip("Show eye tracking status in the console.")]
        public bool logEyeTracking;

        private DisplayXRFeature m_Feature;
        private Camera m_Camera;
        private DisplayXRPostAA m_PostAA;
        // True when running under URP/HDRP. BiRP fires Camera.onPreRender; SRP doesn't,
        // so we route through RenderPipelineManager.beginCameraRendering instead.
        private bool m_UsingSRP;
        // URP foreground-clip globals consumed by DisplayXR/ForegroundClipURP (the opt-in
        // FullScreenPassRendererFeature). The off-axis projection itself is owned by
        // KooimaProjectionFixFeature on URP, not by this rig.
        private static readonly int s_ForegroundFarId = Shader.PropertyToID("_DXRForegroundFar");
        private static readonly int s_EyePosLId = Shader.PropertyToID("_DXREyePosL");
        private static readonly int s_EyePosRId = Shader.PropertyToID("_DXREyePosR");

        // Set by DisplayXRSplash on the boot-splash rig. Such a rig does NOT join
        // the rig registry — so app scripts that bind to DisplayXRRigManager
        // .ActiveCamera latch onto the app's real rig, not this transient one —
        // but it still owns the projection: it pushes unconditionally and raises
        // DisplayXRRigManager.SplashActive so the app's rigs suspend their push
        // while the splash plays.
        internal bool bootSplashOverlay;

        void OnEnable()
        {
            m_Camera = GetComponent<Camera>();
            m_Feature = DisplayXRFeature.Instance;
            m_UsingSRP = GraphicsSettings.currentRenderPipeline != null;
            if (m_UsingSRP)
                RenderPipelineManager.beginCameraRendering += OnSRPBeginCamera;
            else
                Camera.onPreRender += OnCameraPreRender;
            if (bootSplashOverlay)
                DisplayXRRigManager.SplashActive = true;
            else
                DisplayXRRigManager.Register(m_Camera);

            // Post-process AA: Unity drops MSAA on the XR eye RT, so attach a
            // (hidden, rig-managed) FXAA pass and mirror the toggle onto it.
            m_PostAA = DisplayXRPostAA.Ensure(gameObject);
            m_PostAA.enabled = postProcessAntiAliasing;
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
            if (bootSplashOverlay)
                DisplayXRRigManager.SplashActive = false;
            else
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

            // The view matrix is always overridden: it carries the OpenXR→Unity
            // handedness conversion (FlipViewZ) that BiRP's pose path doesn't apply.
            cam.SetStereoViewMatrix(Camera.StereoscopicEye.Left, leftView);
            cam.SetStereoViewMatrix(Camera.StereoscopicEye.Right, rightView);

            // BiRP: always override the projection with the runtime's render-ready fov,
            // built explicitly (native dxr_projection_from_fov). Unity's XR builds a
            // correct projection from views[i].fov only for a CENTERED frustum; for
            // window-relative off-center the frustum shears and Unity mishandles it,
            // over-separating the eyes (#396 — the native cube app, which builds the
            // projection itself, works off-center; Unity didn't until this override).
            // Centered is unchanged (explicit == Unity's fov projection there, Probe A).
            // The native projection also carries the foreground-clip per-view far (#57).
            if (!m_UsingSRP)
            {
                cam.SetStereoProjectionMatrix(Camera.StereoscopicEye.Left, leftProj);
                cam.SetStereoProjectionMatrix(Camera.StereoscopicEye.Right, rightProj);
            }
            // URP path: projection is owned by KooimaProjectionFixFeature (it re-pushes
            // these same leftProj/rightProj per eye-pass — URP ignores
            // SetStereoProjectionMatrix, #1328435). The rig no longer touches projection
            // or farClipPlane here. It only publishes the per-eye foreground far + eye
            // positions so the opt-in DisplayXR/ForegroundClipURP pass can clip per-eye
            // in screen space (#57/#129). The global is inert if that pass isn't wired,
            // so this is safe for every URP app. leftView/rightView are already
            // FlipViewZ'd above, matching the shader's UNITY_MATRIX_I_V.
            else
            {
                float farL = leftProj.m23 / (leftProj.m22 + 1f);
                float farR = rightProj.m23 / (rightProj.m22 + 1f);
                bool clip = foregroundOnlyClip && farL > cam.nearClipPlane && farR > cam.nearClipPlane;
                Shader.SetGlobalVector(s_ForegroundFarId,
                    clip ? new Vector4(farL, farR, 1f, 0f) : Vector4.zero); // z = enable
                if (clip)
                {
                    Shader.SetGlobalVector(s_EyePosLId, leftView.inverse.GetColumn(3));
                    Shader.SetGlobalVector(s_EyePosRId, rightView.inverse.GetColumn(3));
                }
            }
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
            // Keep the post-process AA pass tracking the toggle (inspector or
            // runtime). Before the active-rig gate so every rig tracks its own flag.
            if (m_PostAA != null) m_PostAA.enabled = postProcessAntiAliasing;

            if (bootSplashOverlay)
            {
                // Splash rig: sole pusher while it plays — bypass the gate.
            }
            else
            {
                // The boot splash owns the projection while it plays.
                if (DisplayXRRigManager.SplashActive) return;
                // Only the active rig pushes tunables (prevents multi-rig conflicts)
                var active = DisplayXRRigManager.ActiveCamera;
                if (active != null && active != m_Camera) return;
            }

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

        /// <summary>
        /// (epic #166 provider) The stereo tunables this display-centric rig would
        /// push this frame, as raw rig fields — no <see cref="DisplayXRFeature"/>
        /// dependency (the custom display provider runs without Unity's OpenXR
        /// loader, so the feature singleton is null in that mode).
        /// virtualDisplayHeight is the raw field (0 = use the physical display
        /// height); the provider driver resolves it and folds scene scale. Mirrors
        /// the hook-path LateUpdate push. Read-only — does not touch native.
        /// </summary>
        public DisplayXRTunables GetProviderTunables()
        {
            var cam = m_Camera != null ? m_Camera : GetComponent<Camera>();
            var t = DisplayXRTunables.Default;
            t.ipdFactor = ipdFactor;
            t.parallaxFactor = parallaxFactor;
            t.perspectiveFactor = perspectiveFactor;
            t.virtualDisplayHeight = virtualDisplayHeight;
            t.invConvergenceDistance = 0f;
            t.fovOverride = 0f;
            t.nearZ = cam != null ? cam.nearClipPlane : 0.3f;
            t.farZ = cam != null ? cam.farClipPlane : 1000f;
            t.cameraCentricMode = false;
            t.clipAtDisplayPlane = foregroundOnlyClip;
            return t;
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

        // Cached per-rig scratch buffer for N-view eye positions; reused
        // each gizmo callback to avoid per-frame allocations.
        Vector3[] m_GizmoEyes;

        void DrawGizmosImpl()
        {
            DisplayXRDisplayInfo info = DisplayXRFeature.Instance != null
                ? DisplayXRFeature.Instance.DisplayInfo
                : DisplayXRGizmoHelpers.ReadDisplayInfoFromNative();

            // Window-relative Kooima: when the SA preview is rendering into
            // a sub-rect of the panel, the virtual display gizmo should
            // match the WINDOW dims, not the physical panel dims.
            DisplayXRGizmoHelpers.ComputeWindowRelativeShift(
                info, DisplayXRGizmoHelpers.TryGetCanvasRect(),
                out float winW_m, out float winH_m, out _, out _);
            bool windowMode = winW_m > 0f && winH_m > 0f;

            float h = virtualDisplayHeight > 0f
                ? virtualDisplayHeight
                : (windowMode ? winH_m : (info.isValid ? info.displayHeightMeters : 0.2f));
            float w = windowMode
                ? h * (winW_m / winH_m)
                : (info.isValid ? info.displayWidthMeters * (h / info.displayHeightMeters) : h * 1.5f);

            // Light-blue virtual display volume (preserves the pre-#111
            // OnDrawGizmosSelected aesthetic).
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

            if (m_GizmoEyes == null)
                m_GizmoEyes = new Vector3[DisplayXRGizmoHelpers.MAX_VIEWS];

            int count = DisplayXRGizmoHelpers.GetDisplayCentricEyesWorld(
                transform, info,
                ipdFactor, parallaxFactor, perspectiveFactor,
                virtualDisplayHeight,
                m_GizmoEyes, out bool isLive);

            var cam = m_Camera != null ? m_Camera : GetComponent<Camera>();
            Color frustumColor = Color.magenta;
            Color eyeColor = isLive ? DisplayXRGizmoHelpers.EyeGlyphLive
                                    : DisplayXRGizmoHelpers.EyeGlyphNominal;

            // Near + far come straight from the camera. The frustum helper
            // clamps far internally so we don't fill the whole scene at
            // farClipPlane = 1000 m.
            float nearDist = cam != null ? cam.nearClipPlane : 0.3f;
            float farDist = cam != null ? cam.farClipPlane : 1000f;

            for (int i = 0; i < count; i++)
            {
                Vector3 eye = m_GizmoEyes[i];
                DisplayXRGizmoHelpers.DrawAsymmetricFrustum(
                    eye, cBL, cBR, cTR, cTL, nearDist, farDist, frustumColor);
                DisplayXRGizmoHelpers.DrawEyeGlyph(
                    eye, transform.rotation, 0.05f, eyeColor);
            }
        }
#endif
    }
}
