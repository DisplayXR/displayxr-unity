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

        private Camera m_Camera;
        private DisplayXRPostAA m_PostAA;
        // True when running under URP/HDRP. BiRP fires Camera.onPreRender; SRP doesn't,
        // so we route through RenderPipelineManager.beginCameraRendering instead.
        private bool m_UsingSRP;
        // BiRP provider foreground clip (#166): a single cam.farClipPlane can't sit on
        // the display plane for both eyes at once (off-axis the L/R edges diverge), so
        // the clip is done per-eye in screen space by this hidden OnRenderImage pass —
        // the BiRP analog of the URP ForegroundClipURP shader. Null under URP/HDRP.
        private DisplayXRForegroundClipBiRP m_ClipBiRP;
        // URP foreground-clip globals consumed by DisplayXR/ForegroundClipURP (the opt-in
        // FullScreenPassRendererFeature). The off-axis projection itself comes from the
        // provider's full per-eye projection matrix (type=Matrix), not from this rig.
        private static readonly int s_ForegroundFarId = Shader.PropertyToID("_DXRForegroundFar");
        private static readonly int s_EyePosLId = Shader.PropertyToID("_DXREyePosL");
        private static readonly int s_EyePosRId = Shader.PropertyToID("_DXREyePosR");
        private static readonly int s_RigPosId = Shader.PropertyToID("_DXRRigPos");
        private static readonly int s_RigFwdId = Shader.PropertyToID("_DXRRigFwd");

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
            m_UsingSRP = GraphicsSettings.currentRenderPipeline != null;
            if (m_UsingSRP)
                RenderPipelineManager.beginCameraRendering += OnSRPBeginCamera;
            else
                Camera.onPreRender += OnCameraPreRender;
            if (bootSplashOverlay)
                DisplayXRRigManager.SplashActive = true;
            else
                DisplayXRRigManager.Register(m_Camera);

            // BiRP per-eye foreground clip (provider): attach the (hidden, rig-managed)
            // OnRenderImage clip pass BEFORE the post-AA pass so the clip silhouette is
            // in place when FXAA softens it. BiRP only — URP owns the clip in its shader.
            if (!m_UsingSRP)
            {
                m_ClipBiRP = DisplayXRForegroundClipBiRP.Ensure(gameObject);
                m_ClipBiRP.enabled = false; // enabled only under the provider + clip on
            }

            // Post-process AA: Unity drops MSAA on the XR eye RT, so attach a
            // (hidden, rig-managed) FXAA pass and mirror the toggle onto it.
            m_PostAA = DisplayXRPostAA.Ensure(gameObject);
            m_PostAA.enabled = postProcessAntiAliasing && DisplayXRPostAA.SupportedForCurrentStereoMode();
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
            if (cam != m_Camera) return;

            // Provider mode (#166): publish the URP ForegroundClipURP globals from
            // the provider's per-eye clip data. BiRP has no such shader pass built
            // into URP — drive the dedicated per-eye OnRenderImage clip pass instead
            // (DisplayXRForegroundClipBiRP), enabling it only while the clip is on. It
            // reads each eye's native display-plane far in OnRenderImage, so both eyes
            // clip exactly on the plane.
            if (DisplayXRProviderDriver.IsActive)
            {
                if (m_UsingSRP)
                {
                    PublishProviderForegroundClip();
                }
                else if (m_ClipBiRP != null)
                {
                    m_ClipBiRP.clipEnabled = foregroundOnlyClip;
                    m_ClipBiRP.enabled = foregroundOnlyClip;
                }
                return;
            }
        }

        // Provider mode: publish the URP DisplayXR/ForegroundClipURP globals from the
        // provider's per-eye clip data (far + eye world pos), mirroring the hook-path
        // URP branch in OnCameraPreRender. The globals are inert if the clip pass isn't
        // wired, so this is safe when foregroundOnlyClip is off. #166 Phase B.
        void PublishProviderForegroundClip()
        {
            if (!foregroundOnlyClip)
            {
                Shader.SetGlobalVector(s_ForegroundFarId, Vector4.zero);
                return;
            }
            DisplayXRProviderNative.dxr_prov_get_eye_clip(0, out float farL, out float lx, out float ly, out float lz);
            DisplayXRProviderNative.dxr_prov_get_eye_clip(1, out float farR, out float rx, out float ry, out float rz);
            float nz = m_Camera != null ? m_Camera.nearClipPlane : 0.3f;
            bool clip = farL > nz && farR > nz;
            // Plane mode (w=1): the shader derives each eye's far as its perpendicular
            // distance to the display plane (through the rig origin, normal = rig forward),
            // straight from that eye's own render position — per-zone AND per-eye correct.
            // The published-eye-pick fails in multi-zone because a zone's pair can shift
            // more than half the IPD (#166), so its right eye gets the wrong (short) far.
            Shader.SetGlobalVector(s_ForegroundFarId,
                clip ? new Vector4(farL, farR, 1f, 1f) : Vector4.zero);
            if (clip)
            {
                // Plane origin/normal = the exact rig pose the driver sends the runtime
                // as the display plane. The provider now hands Unity a rig-RELATIVE
                // deviceAnchorToEyePose, so the render eye (curEye = UNITY_MATRIX_I_V)
                // carries the rig pose exactly once — matching this single-application
                // cam.transform, so the clip plane tracks the moving rig (#166).
                Vector3 rp  = DisplayXRProvider.RigPlaneValid ? DisplayXRProvider.RigPlanePos     : transform.position;
                Vector3 fwd = DisplayXRProvider.RigPlaneValid ? DisplayXRProvider.RigPlaneForward : transform.forward;
                Shader.SetGlobalVector(s_RigPosId, new Vector4(rp.x, rp.y, rp.z, 0f));
                Shader.SetGlobalVector(s_RigFwdId, new Vector4(fwd.x, fwd.y, fwd.z, 0f));
                Shader.SetGlobalVector(s_EyePosLId, new Vector4(lx, ly, lz, 0f));
                Shader.SetGlobalVector(s_EyePosRId, new Vector4(rx, ry, rz, 0f));
            }
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
            if (m_PostAA != null) m_PostAA.enabled = postProcessAntiAliasing && DisplayXRPostAA.SupportedForCurrentStereoMode();

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
        }

        /// <summary>
        /// (epic #166 provider) The stereo tunables this display-centric rig would
        /// push this frame, as raw rig fields — no native dependency (the custom
        /// display provider runs without Unity's OpenXR loader).
        /// virtualDisplayHeight is the raw field (0 = use the physical display
        /// height); the provider driver resolves it and folds scene scale.
        /// Read-only — does not touch native.
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
            DisplayXRDisplayInfo info = DisplayXRGizmoHelpers.ReadDisplayInfoFromNative();

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
