// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

using UnityEngine;
using UnityEngine.Rendering;

namespace DisplayXR
{
    /// <summary>
    /// Camera-centric stereo rig. Attach to a Camera whose transform represents the
    /// viewer pose. The camera's vertical FOV is inherited as the rendering FOV.
    /// Exposes only: IPD, parallax, and inverse convergence distance.
    /// <para>
    /// <b>2D fallback (#256): this rig needs no code at all — drop the convergence and
    /// that is the whole story.</b> Its transform already IS a viewer position and its
    /// FOV is the camera's own, so with no DisplayXR runtime Unity renders the authored
    /// viewpoint unchanged. And the convergence drops itself: <see cref="invConvergenceDistance"/>
    /// leaves managed code ONLY via <see cref="GetProviderTunables"/> → the provider
    /// driver → <c>dxr_prov_set_tunables</c>, i.e. a native tunable consumed at
    /// <c>xrLocateViews</c>. With no session there is no driver (the loader never
    /// creates one), nothing is pushed, and nothing consumes it. Nothing camera-side —
    /// no projection override, no lens shift, no eye/anchor offset — is derived from it
    /// anywhere in managed code; the only other reader is the editor gizmo. The BiRP
    /// foreground-clip pass is likewise attached disabled and only ever enabled inside a
    /// <c>DisplayXRProviderDriver.IsActive</c> branch. So the camera renders as a
    /// completely plain camera with no gating required.
    /// </para>
    /// <para>
    /// Only <see cref="DisplayXRDisplay"/> needs a fallback adjustment: its transform is
    /// the virtual DISPLAY PLANE, so a non-stereo camera sits on that plane and clips
    /// through the content. See <c>DisplayXRDisplay.ApplyTwoDFallbackPullback</c>.
    /// </para>
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

        [Tooltip("Foreground-only render: clip each view at the convergence distance " +
                 "(1/invConvergenceDistance). Geometry past the convergence plane is " +
                 "clipped. Single convergence is shared by all views, so per-view fars " +
                 "match each other. No-op when invConvergenceDistance == 0 (parallel " +
                 "projection has no finite convergence point).\n\n" +
                 "On a transparent overlay the runtime may push this plane BACK when " +
                 "the desktop behind the content carries no horizontal depth cue " +
                 "(rear depth budget — see DisplayXRDepthBudget, and add a " +
                 "DisplayXRContentBounds to the content root so the runtime looks in " +
                 "the right place). Against a runtime without XR_DXR_depth_budget, or " +
                 "an opaque session, the clip stays exactly on the convergence plane.")]
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

        private float m_CachedCameraFov;
        // (#274) The authored FOV, SERIALIZED so it survives a scene load during a live
        // session. Written ONLY outside Play (OnValidate / Reset / the edit-mode sync in
        // LateUpdate), so it can never be polluted by the XR-stamped Camera.fieldOfView.
        // 0 = never captured (component authored before #274 and scene not re-saved since).
        [SerializeField, HideInInspector] private float m_AuthoredFov = 0f;
        // (#274) One-shot: warned that a rig had to fall back to the XR-written camera FOV.
        private static bool s_WarnedUnseededFov;
        private Camera m_Camera;
        private DisplayXRPostAA m_PostAA;
        // True when running under URP/HDRP. BiRP fires Camera.onPreRender; SRP doesn't,
        // so we route through RenderPipelineManager.beginCameraRendering instead.
        private bool m_UsingSRP;
        // BiRP provider foreground clip (#166): a single cam.farClipPlane can't sit on
        // the display/convergence plane for both eyes at once, so the clip runs per-eye
        // in screen space via this hidden OnRenderImage pass (BiRP analog of the URP
        // ForegroundClipURP shader). Null under URP/HDRP.
        private DisplayXRForegroundClipBiRP m_ClipBiRP;
        // URP foreground-clip globals consumed by DisplayXR/ForegroundClipURP (the opt-in
        // FullScreenPassRendererFeature). The off-axis projection itself comes from the
        // provider's full per-eye projection matrix (type=Matrix), not from this rig.
        private static readonly int s_ForegroundFarId = Shader.PropertyToID("_DXRForegroundFar");
        private static readonly int s_EyePosLId = Shader.PropertyToID("_DXREyePosL");
        private static readonly int s_EyePosRId = Shader.PropertyToID("_DXREyePosR");

#if UNITY_EDITOR
        void OnValidate()
        {
            CaptureAuthoredFovFromCamera();
            if (GetComponent<DisplayXRDisplay>() != null)
                Debug.LogError("[DisplayXR] DisplayXRCamera and DisplayXRDisplay cannot coexist on the same GameObject. Remove one.", this);
        }

        void Reset()
        {
            CaptureAuthoredFovFromCamera();
        }
#endif

        // (#274) Capture the Camera's FOV into the serialized authored value. EDIT MODE
        // ONLY — in Play, Camera.fieldOfView is XR-written and must never be captured.
        private void CaptureAuthoredFovFromCamera()
        {
            if (Application.isPlaying) return;
            var c = m_Camera != null ? m_Camera : GetComponent<Camera>();
            if (c == null) return;
            float f = c.fieldOfView;
            if (f >= 1.0f && f < 179.0f) m_AuthoredFov = f;
        }

        void OnEnable()
        {
            m_Camera = GetComponent<Camera>();
            // Cache the camera's FOV BEFORE XR overrides it. Once XR is active,
            // Camera.fieldOfView returns the Kooima FOV we set, creating a
            // feedback loop that collapses the FOV to zero.
            // (#274) Seed from the SERIALIZED authored value, not from the Camera. On a
            // scene loaded while a session is live, Unity's XR has already stamped the
            // tracking-derived FOV onto Camera.fieldOfView before OnEnable runs, so
            // reading the camera here seeds the cache with a polluted value that then
            // feeds the next projection — the FOV walks ~2x per scene visit (measured:
            // 12.5 -> 24.3 -> ... -> 116.8 against an authored 35). The serialized value
            // is only ever written outside Play, so it cannot be polluted.
            if (m_AuthoredFov >= 1.0f && m_AuthoredFov < 179.0f)
            {
                m_CachedCameraFov = m_AuthoredFov;
            }
            else
            {
                // Never captured: component authored before #274 and the scene not
                // re-saved since. Fall back to the Camera — correct on a first load,
                // WRONG on a live-session scene reload (the pre-#274 behaviour). Say so
                // once, because from here the symptom is "scene shrinks every visit"
                // with nothing pointing at the cause.
                m_CachedCameraFov = m_Camera.fieldOfView;
                if (m_CachedCameraFov < 1.0f)
                    m_CachedCameraFov = 60.0f;
                if (DisplayXRProvider.IsRunning && !s_WarnedUnseededFov)
                {
                    s_WarnedUnseededFov = true;
                    Debug.LogWarning("[DisplayXR] DisplayXRCamera on '" + name + "' has no serialized " +
                        "authored FOV and was enabled while a session is already running, so its FOV " +
                        "was read from a Camera that XR has already overwritten (#274). Open the scene " +
                        "in the editor and save it once to capture the authored value; or set " +
                        "DisplayXRCamera.AuthoredFieldOfView explicitly on scene entry.", this);
                }
            }
            m_UsingSRP = GraphicsSettings.currentRenderPipeline != null;
            if (m_UsingSRP)
                RenderPipelineManager.beginCameraRendering += OnSRPBeginCamera;
            else
                Camera.onPreRender += OnCameraPreRender;
            DisplayXRRigManager.Register(m_Camera);

            // BiRP per-eye foreground clip (provider): attach the hidden OnRenderImage
            // clip pass BEFORE the post-AA pass so FXAA softens the clip silhouette.
            // BiRP only — URP owns the clip in its shader.
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
            Debug.Log("[DisplayXR] DisplayXRCamera.OnDisable");
            if (m_UsingSRP)
                RenderPipelineManager.beginCameraRendering -= OnSRPBeginCamera;
            else
                Camera.onPreRender -= OnCameraPreRender;
            DisplayXRRigManager.Unregister(m_Camera);
        }

        void OnSRPBeginCamera(ScriptableRenderContext ctx, Camera cam) => OnCameraPreRender(cam);

        void OnCameraPreRender(Camera cam)
        {
            if (cam != m_Camera) return;

            // Provider mode (#166): publish the URP ForegroundClipURP globals from
            // the provider's per-eye clip data, mirroring DisplayXRDisplay. BiRP drives
            // the dedicated per-eye OnRenderImage clip pass (DisplayXRForegroundClipBiRP),
            // enabled only while the clip is on; it reads each eye's native
            // convergence-plane far in OnRenderImage so both eyes clip on the plane
            // (a single cam.farClipPlane can't).
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
        // wired, so this is safe when foregroundOnlyClip is off. #166 Phase C.
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
            Shader.SetGlobalVector(s_ForegroundFarId,
                clip ? new Vector4(farL, farR, 1f, 0f) : Vector4.zero);
            if (clip)
            {
                Shader.SetGlobalVector(s_EyePosLId, new Vector4(lx, ly, lz, 0f));
                Shader.SetGlobalVector(s_EyePosRId, new Vector4(rx, ry, rz, 0f));
            }
        }

        void LateUpdate()
        {
            // Keep the post-process AA pass tracking the toggle (inspector or
            // runtime). Before the active-rig gate so every rig tracks its own flag.
            if (m_PostAA != null) m_PostAA.enabled = postProcessAntiAliasing && DisplayXRPostAA.SupportedForCurrentStereoMode();

            // The boot splash owns the projection while it plays.
            if (DisplayXRRigManager.SplashActive) return;

            // Only the active rig pushes tunables (prevents multi-rig conflicts)
            var active = DisplayXRRigManager.ActiveCamera;
            if (active != null && active != m_Camera) return;

            // In edit mode, sync the cached FOV from the camera (inspector edits).
            // In play mode, XR overwrites cam.fieldOfView with the Kooima FOV each
            // frame — reading it back would create a feedback loop that blows up to
            // ~180°, so the cache is frozen at its OnEnable value. GetProviderTunables
            // reads m_CachedCameraFov.
            if (!Application.isPlaying)
            {
                float currentFov = m_Camera.fieldOfView;
                if (currentFov >= 1.0f)
                {
                    m_CachedCameraFov = currentFov;
                    if (currentFov < 179.0f) m_AuthoredFov = currentFov;   // (#274)
                }
            }
        }

        /// <summary>
        /// (#274) The FOV this rig actually projects with — the value you authored on the
        /// Camera, not what <see cref="Camera.fieldOfView"/> reads back.
        ///
        /// <para>
        /// <b>Why this exists.</b> While a session is running the provider hands Unity a
        /// full per-eye projection, and Unity's XR then WRITES <c>Camera.fieldOfView</c>
        /// from it each frame — a tracking-derived number that moves with the viewer's
        /// head (measured in the field at 76.5°–124.5° against an authored 60°). Anything
        /// reading <c>Camera.fieldOfView</c> in Play therefore gets a value that is neither
        /// what you set nor stable: a <c>Screen Space - Camera</c> canvas sizes itself from
        /// it and so renders too large and rescales as the viewer moves, and gameplay or
        /// cinematic code doing FOV maths silently drifts.
        /// </para>
        ///
        /// <para>
        /// <b>It survives scene loads (#274).</b> The value is serialized on the component
        /// and captured only in edit mode, so a scene loaded while a session is already
        /// running seeds from it rather than from the XR-overwritten <c>Camera.fieldOfView</c>.
        /// Scenes authored before this fix have no captured value until they are opened and
        /// saved once in the editor; the rig logs a one-shot warning when it has to fall back.
        /// </para>
        ///
        /// <para>
        /// The rig has always kept the authored value (it must, or reading the camera back
        /// would feed the Kooima FOV into itself and collapse it); it was simply private.
        /// This exposes it, and makes it settable so an app CAN change FOV at runtime —
        /// assigning <c>Camera.fieldOfView</c> while XR is active does not work, because
        /// XR overwrites it and the rig projects from this cache regardless.
        /// </para>
        ///
        /// <para>
        /// <b>A camera with no rig has no such record.</b> Nothing captures its authored
        /// FOV before XR starts, so the original is simply gone. If you need it, snapshot
        /// it yourself before the session comes up — or use
        /// <c>DisplayXRWindowSpaceUI</c> for screen-fixed UI, which is a composition layer
        /// at a fixed window rect and immune to the camera pose and projection entirely.
        /// See <c>docs~/architecture/two-dimensional-scenes.md</c>.
        /// </para>
        /// </summary>
        public float AuthoredFieldOfView
        {
            get
            {
                if (m_CachedCameraFov >= 1.0f) return m_CachedCameraFov;
                var c = m_Camera != null ? m_Camera : GetComponent<Camera>();
                return c != null ? c.fieldOfView : 60f;
            }
            set
            {
                if (!(value >= 1.0f) || value >= 179.0f) return;
                m_CachedCameraFov = value;
                // Outside Play, Camera.fieldOfView is still the authored value and the
                // inspector shows it, so keep the two in step. In Play, writing it would
                // be pointless — XR overwrites it every frame from the projection.
                if (!Application.isPlaying)
                {
                    var c = m_Camera != null ? m_Camera : GetComponent<Camera>();
                    if (c != null) c.fieldOfView = value;
                    // (#274) Keep the serialized authored value in step. Edit mode only:
                    // a runtime FOV change is instance state and must not masquerade as
                    // the authored value.
                    m_AuthoredFov = value;
                }
            }
        }

        /// <summary>
        /// (epic #166 provider) The stereo tunables this camera-centric rig would
        /// push this frame, as raw rig fields. fovOverride = tan(halfVFov) from the
        /// cached camera FOV (cached because XR overrides Camera.fieldOfView).
        /// Mirrors the hook-path LateUpdate push; used by the provider driver.
        /// Read-only — does not touch native.
        /// </summary>
        public DisplayXRTunables GetProviderTunables()
        {
            var cam = m_Camera != null ? m_Camera : GetComponent<Camera>();
            float fovDeg = AuthoredFieldOfView;   // (#274) single source of truth
            var t = DisplayXRTunables.Default;
            t.ipdFactor = ipdFactor;
            t.parallaxFactor = parallaxFactor;
            t.perspectiveFactor = 1.0f;
            t.virtualDisplayHeight = 0f;
            t.invConvergenceDistance = invConvergenceDistance;
            t.fovOverride = Mathf.Tan(fovDeg * 0.5f * Mathf.Deg2Rad);
            t.nearZ = cam != null ? cam.nearClipPlane : 0.3f;
            t.farZ = cam != null ? cam.farClipPlane : 1000f;
            t.cameraCentricMode = true;
            t.clipAtDisplayPlane = foregroundOnlyClip;
            return t;
        }

#if UNITY_EDITOR
        // Selection-driven in plain Edit Mode; active-rig-only when a
        // DisplayXR session is alive (preview window, or Play Mode XR loader).
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

        // Cached per-rig scratch buffer for N-view eye positions.
        Vector3[] m_GizmoEyes;

        void DrawGizmosImpl()
        {
            DisplayXRDisplayInfo info = DisplayXRGizmoHelpers.ReadDisplayInfoFromNative();

            // Window-relative Kooima: convergence volume aspect should
            // match the WINDOW the runtime is rasterizing into, not the
            // physical panel.
            DisplayXRGizmoHelpers.ComputeWindowRelativeShift(
                info, DisplayXRGizmoHelpers.TryGetCanvasRect(),
                out float winW_m, out float winH_m, out _, out _);
            float aspect = (winW_m > 0f && winH_m > 0f)
                ? winW_m / winH_m
                : (info.isValid ? info.displayWidthMeters / info.displayHeightMeters : 16f / 9f);

            var cam = m_Camera != null ? m_Camera : GetComponent<Camera>();
            float fovDeg = m_CachedCameraFov > 0f ? m_CachedCameraFov
                         : (cam != null ? cam.fieldOfView : 60f);

            // Convergence plane: at 1/invd when finite, else a 2 m preview
            // plane so the parallel-projection case still gives the user
            // something to anchor on.
            bool parallel = invConvergenceDistance < 0.001f;
            float convergenceDist = parallel ? 2f : (1f / invConvergenceDistance);
            float halfH = convergenceDist * Mathf.Tan(fovDeg * 0.5f * Mathf.Deg2Rad);
            float h = halfH * 2f;
            float w = h * aspect;
            float hw = w * 0.5f;
            float hh = h * 0.5f;

            // Orange convergence volume (preserves the pre-#111 visual; the
            // parallel-projection case shifts hue so the user can tell it
            // apart from the finite-convergence case).
            Color volFill = parallel
                ? new Color(0.7f, 0.4f, 1f, 0.25f)
                : new Color(1f, 0.6f, 0.2f, 0.3f);
            Color volWire = parallel
                ? new Color(0.7f, 0.4f, 1f, 0.8f)
                : new Color(1f, 0.6f, 0.2f, 0.8f);
            Vector3 center = new Vector3(0, 0, convergenceDist);
            Vector3 size = new Vector3(w, h, 0.002f);
            Gizmos.matrix = transform.localToWorldMatrix;
            Gizmos.color = volFill;
            Gizmos.DrawCube(center, size);
            Gizmos.color = volWire;
            Gizmos.DrawWireCube(center, size);
            Gizmos.matrix = Matrix4x4.identity;

            Vector3 cBL = transform.TransformPoint(new Vector3(-hw, -hh, convergenceDist));
            Vector3 cBR = transform.TransformPoint(new Vector3(+hw, -hh, convergenceDist));
            Vector3 cTR = transform.TransformPoint(new Vector3(+hw, +hh, convergenceDist));
            Vector3 cTL = transform.TransformPoint(new Vector3(-hw, +hh, convergenceDist));

            if (m_GizmoEyes == null)
                m_GizmoEyes = new Vector3[DisplayXRGizmoHelpers.MAX_VIEWS];

            int count = DisplayXRGizmoHelpers.GetCameraCentricEyesWorld(
                transform, info,
                ipdFactor, parallaxFactor,
                m_GizmoEyes, out bool isLive);

            Color frustumColor = Color.magenta;
            Color eyeColor = isLive ? DisplayXRGizmoHelpers.EyeGlyphLive
                                    : DisplayXRGizmoHelpers.EyeGlyphNominal;

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
