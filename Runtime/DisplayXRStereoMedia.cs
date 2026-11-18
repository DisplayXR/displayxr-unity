// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

using UnityEngine;
using UnityEngine.Experimental.Rendering;
using UnityEngine.Rendering;
#if DISPLAYXR_HAS_VIDEO
using UnityEngine.Video;
#endif

namespace DisplayXR
{
    /// <summary>
    /// Shows a stereo picture or video - side-by-side or top-bottom - so that each eye sees
    /// its own half (#463). Give it a <see cref="texture"/> or a VideoPlayer, pick the
    /// <see cref="layout"/>, and choose how it is presented:
    /// <list type="bullet">
    /// <item><see cref="PresentationMode.Quad"/> - a quad in the scene. Placed on the rig's
    /// zero-parallax plane (<see cref="QuadPlacement.ZeroParallaxPlane"/>) the rig adds no
    /// disparity of its own, so the picture's own disparity is the only one; the quad is
    /// still a scene object, so it has a place in the scene and moves against it as the
    /// viewer's head moves.</item>
    /// <item><see cref="PresentationMode.FullScreen"/> - each eye's half goes straight to that
    /// eye's render target, covering the view. No parallax is added and nothing moves with
    /// the head: a plain stereo photo / video viewer.</item>
    /// </list>
    /// <para>
    /// <b>Mono fallback.</b> When the session is not rendering stereo (no session, or a
    /// one-view rendering mode - which is also what the provider switches to when the panel
    /// goes 2D), or with <see cref="forceMono"/>, both eyes get the left picture.
    /// </para>
    /// <para>
    /// Works on the Built-in, Universal and High Definition pipelines with one shader
    /// (<c>DisplayXR/StereoMedia</c>). See <c>docs~/architecture/stereo-media.md</c> for the
    /// per-pipeline notes and limits.
    /// </para>
    /// <para>
    /// The picture is submitted each frame with <c>Graphics.RenderMesh</c>: the component
    /// adds no renderer and no child object, and never moves or scales the GameObject it
    /// sits on. Nothing is allocated per frame.
    /// </para>
    /// </summary>
    [AddComponentMenu("DisplayXR/Stereo Media")]
    [DisallowMultipleComponent]
    [ExecuteAlways]
    // Just before the provider driver (10000), which samples the rig pose it sends the
    // runtime: by then every app script has moved the rig, so the quad is placed on the
    // plane of the pose that is actually rendered - not last frame's.
    [DefaultExecutionOrder(9999)]
    public class DisplayXRStereoMedia : MonoBehaviour
    {
        /// <summary>How the media is put in front of the viewer.</summary>
        public enum PresentationMode
        {
            /// <summary>A quad in the scene.</summary>
            Quad = 0,
            /// <summary>Covers the view; each eye's half goes to that eye's target directly.</summary>
            FullScreen = 1,
        }

        /// <summary>Where the quad sits (Quad presentation).</summary>
        public enum QuadPlacement
        {
            /// <summary>On the rig's zero-parallax plane, sized to it: the virtual display
            /// plane of a <see cref="DisplayXRDisplay"/>, the convergence plane of a
            /// <see cref="DisplayXRCamera"/>.</summary>
            ZeroParallaxPlane = 0,
            /// <summary>At this GameObject's transform, <see cref="quadSize"/> in its local units.</summary>
            Manual = 1,
        }

        /// <summary>When the full-screen picture is drawn relative to the scene.</summary>
        public enum FullScreenOrder
        {
            /// <summary>Behind the scene: it replaces the skybox / clear colour, and scene
            /// geometry draws over it.</summary>
            Background = 0,
            /// <summary>Over the scene (after transparents).</summary>
            Foreground = 1,
        }

        /// <summary>What the texture's alpha channel means.</summary>
        public enum AlphaMode
        {
            /// <summary>Ignored; the picture is opaque.</summary>
            Opaque = 0,
            /// <summary>Colour is already multiplied by alpha.</summary>
            Premultiplied = 1,
            /// <summary>Colour is not multiplied by alpha (a PNG, for example).</summary>
            Straight = 2,
        }

        [Header("Source")]

        [Tooltip("The stereo picture: a Texture2D or a RenderTexture. Ignored while a Video Player is assigned.")]
        public Texture texture;

#if DISPLAYXR_HAS_VIDEO
        [Tooltip("Optional video source. Its Target Texture is used when its Render Mode is Render Texture; " +
                 "otherwise the component switches it to API Only and shows its internal texture once prepared.")]
        public VideoPlayer videoPlayer;

        [Tooltip("Call Prepare() on the Video Player when this component is enabled, so the first frame and the " +
                 "picture size are known before playback starts.")]
        public bool prepareVideo = true;
#endif

        [Tooltip("What the texel values mean. SRGB (photos, video) is right for nearly all media: where the texture's " +
                 "format does not make the GPU decode it (a non-sRGB render texture a video is decoded into, in a " +
                 "Linear project), the shader does. Linear is for textures that hold linear light.")]
        public DisplayXRStereoEncoding encoding = DisplayXRStereoEncoding.SRGB;

        [Tooltip("The texture is stored upside down (some native / external textures).")]
        public bool flipVertical;

        [Tooltip("The picture's true size in pixels, when the texture is not that size - most often an imported " +
                 "image that Unity resized to a power of two (Non-Power of 2 other than None turns 3840x1080 into " +
                 "4096x1024). Only the shape is used. 0 x 0 = the texture's own size.")]
        public Vector2Int sourceSize;

        [Header("Layout")]

        [Tooltip("How the two eye pictures are packed. Top-Bottom has the left eye on top.")]
        public DisplayXRStereoLayout layout = DisplayXRStereoLayout.SideBySide;

        [Tooltip("Each eye picture was squeezed to half size along the packing axis (half-width side-by-side, " +
                 "half-height top-bottom) and is shown at the aspect of the whole texture.")]
        public bool halfResolution;

        [Tooltip("Width / height of one stored pixel. 1 = square pixels. 0 = take it from the Video Player " +
                 "(square for a plain texture).")]
        [Min(0f)]
        public float pixelAspect = 1f;

        [Tooltip("Moves the whole picture in depth by shifting the two eye pictures apart, in percent of the picture " +
                 "width. Positive = further behind the screen, negative = towards the viewer. Both pictures are " +
                 "cropped by the same amount. 0 = the picture as authored.")]
        [Range(-10f, 10f)]
        public float depthShiftPercent;

        [Tooltip("Show the left picture to both eyes even when the session renders stereo.")]
        public bool forceMono;

        [Header("Presentation")]

        public PresentationMode presentation = PresentationMode.Quad;

        [Tooltip("Fit: whole picture, bars where the frame is not covered. Fill: frame covered, picture cropped. " +
                 "Stretch: frame covered, aspect not kept.")]
        public DisplayXRStereoFit fit = DisplayXRStereoFit.Fit;

        [Tooltip("What the texture's alpha channel means. Opaque ignores it. The others blend the picture over what is " +
                 "behind it and write alpha to the target, as an alpha-native transparent overlay needs.")]
        public AlphaMode alphaMode = AlphaMode.Opaque;

        [Tooltip("Multiplies the picture (use the alpha to fade a non-opaque picture).")]
        public Color tint = Color.white;

        [Header("Quad")]

        public QuadPlacement placement = QuadPlacement.ZeroParallaxPlane;

        [Tooltip("The rig camera: its zero-parallax plane carries the quad, and it is the one camera a full-screen " +
                 "picture is drawn for. Empty = the active rig camera (DisplayXRRigManager.ActiveCamera).")]
        public Camera rigCamera;

        [Tooltip("Size of the frame the picture is fitted into, in this object's local units (Manual placement, and " +
                 "the fallback while no zero-parallax plane is available).")]
        public Vector2 quadSize = new Vector2(1.6f, 0.9f);

        [Header("Full Screen")]

        public FullScreenOrder fullScreenOrder = FullScreenOrder.Background;

        [Tooltip("Displayed width / height of the view the picture is fitted into. 0 = the window the session " +
                 "composites into (falls back to the rig camera's aspect).")]
        [Min(0f)]
        public float viewAspect;

        /// <summary>The texture currently shown (the video's when a Video Player is assigned). May be null.</summary>
        public Texture CurrentTexture => m_State.texture;

        /// <summary>True while each eye gets its own picture; false in the mono fallback.</summary>
        public bool IsStereo => m_State.stereo;

        /// <summary>Displayed width / height of one eye's picture (0 while the size is unknown).</summary>
        public float PictureAspect => m_PictureAspect;

        /// <summary>The material the picture is drawn with (owned by the component; null while disabled).</summary>
        public Material Material => m_Material;

        // ---- internals ---------------------------------------------------------

        static readonly int s_MainTexId = Shader.PropertyToID("_MainTex");
        static readonly int s_ColorId = Shader.PropertyToID("_Color");
        static readonly int s_STLId = Shader.PropertyToID("_DXRMediaSTL");
        static readonly int s_STRId = Shader.PropertyToID("_DXRMediaSTR");
        static readonly int s_ClampLId = Shader.PropertyToID("_DXRMediaClampL");
        static readonly int s_ClampRId = Shader.PropertyToID("_DXRMediaClampR");
        static readonly int s_FrameId = Shader.PropertyToID("_DXRMediaFrame");
        static readonly int s_FullScreenId = Shader.PropertyToID("_DXRMediaFullScreen");
        static readonly int s_ColorXformId = Shader.PropertyToID("_DXRMediaColor");
        static readonly int s_AlphaId = Shader.PropertyToID("_DXRMediaAlpha");
        static readonly int s_MaxLodId = Shader.PropertyToID("_DXRMediaMaxLod");
        static readonly int s_ScriptEyeId = Shader.PropertyToID("_DXRMediaScriptEye");
        static readonly int s_SrcBlendId = Shader.PropertyToID("_SrcBlend");
        static readonly int s_DstBlendId = Shader.PropertyToID("_DstBlend");
        static readonly int s_ZWriteId = Shader.PropertyToID("_ZWrite");
        static readonly int s_ZTestId = Shader.PropertyToID("_ZTest");

        // Full-screen geometry never leaves the view; bounds this large keep CPU culling
        // (which uses the camera transform, not the per-eye projection) from dropping it.
        const float k_HugeExtent = 1e5f;

        // Every queue used is OUTSIDE the opaque range, on purpose - see the queue notes in
        // docs~/architecture/stereo-media.md. In short: with depth priming URP draws its
        // opaque queue (<= 2500) with ZTest Equal against a depth prepass, and this shader
        // has no depth pass, so an opaque-queue quad would not be drawn at all. 2501-2649
        // is HDRP's after-post-process opaque range. From 2900 (HDRP's first plain
        // transparent queue) to 3100 (its last) every pipeline draws the pass with the
        // shader's own depth state.
        //
        // After the opaque queue and the skybox, before ordinary transparents: the opaque
        // quad (which writes depth) and the full-screen background.
        const int k_EarlyQueue = (int)RenderQueue.Transparent - 100;
        // The last queue every pipeline still draws with its transparents.
        const int k_LateQueue = (int)RenderQueue.Transparent + 100;

        // Everything the material depends on. Compared field by field each frame; the
        // material is only touched when something differs.
        struct State
        {
            public Texture texture;
            public int width, height;        // the texture as it is on the GPU
            public int aspectWidth, aspectHeight; // the size the picture's shape is taken from
            public int mipCount;
            public bool formatIsSRGB;
            public DisplayXRStereoLayout layout;
            public bool halfResolution;
            public float pixelAspect;
            public float depthShift;
            public bool flipVertical;
            public bool stereo;
            public DisplayXRStereoEncoding encoding;
            public PresentationMode presentation;
            public DisplayXRStereoFit fit;
            public AlphaMode alphaMode;
            public Color tint;
            public FullScreenOrder order;
            public Vector2 frame;      // Quad: frame size in the quad's own units. Full screen: unused.
            public float frameAspect;
            public bool valid;

            public bool Same(in State o) =>
                valid && o.valid &&
                ReferenceEquals(texture, o.texture) && width == o.width && height == o.height &&
                aspectWidth == o.aspectWidth && aspectHeight == o.aspectHeight && mipCount == o.mipCount &&
                formatIsSRGB == o.formatIsSRGB && layout == o.layout && halfResolution == o.halfResolution &&
                pixelAspect == o.pixelAspect && depthShift == o.depthShift && flipVertical == o.flipVertical &&
                stereo == o.stereo && encoding == o.encoding && presentation == o.presentation && fit == o.fit &&
                alphaMode == o.alphaMode && tint == o.tint && order == o.order && frame == o.frame &&
                frameAspect == o.frameAspect;
        }

        State m_State;
        float m_PictureAspect;
        bool m_Visible;
        Vector2 m_Drawn;            // Quad: drawn size in the quad's units. Full screen: fraction of the view.
        Matrix4x4 m_QuadMatrix = Matrix4x4.identity;
        bool m_UsingSRP;

        Mesh m_Mesh;
        Material m_Material;
        bool m_WarnedShader;
        bool m_WarnedNoPlane;

        // Rig lookup, cached by camera (as the provider driver does) so nothing is
        // queried per frame.
        Camera m_PlaneCamera;
        DisplayXRDisplay m_PlaneDisplayRig;
        DisplayXRCamera m_PlaneCameraRig;

        // Stereo state: re-read from the provider's mode table on events, not per frame.
        bool m_ModeDirty = true;
        bool m_ModeStereo = true;
        System.Action m_OnSessionEdge;
        System.Action<uint, uint> m_OnModeChanged;
        System.Action<bool> m_OnHardwareChanged;

#if DISPLAYXR_HAS_VIDEO
        VideoPlayer m_HookedPlayer;
        VideoPlayer.EventHandler m_OnVideoPrepared;
#endif

#if UNITY_EDITOR
        Camera.CameraCallback m_OnEditCamera;
        System.Action<ScriptableRenderContext, Camera> m_OnEditCameraSRP;
#endif

        // Built-in pipeline, multi-pass: which eye the pass about to render is for, as a
        // shader global. One subscription shared by all instances.
        static int s_ScriptEyeUsers;
        static Camera.CameraCallback s_OnPreRenderEye;

        static void PublishScriptEye(Camera cam)
        {
            // Camera.stereoActiveEye is Left / Right inside an eye's render callbacks and
            // Mono for a camera that is not rendering stereo - the same signal the BiRP
            // foreground clip reads (in OnRenderImage) to pick its eye.
            Shader.SetGlobalFloat(s_ScriptEyeId,
                cam != null && cam.stereoActiveEye == Camera.MonoOrStereoscopicEye.Right ? 1f : 0f);
        }

        void OnEnable()
        {
            m_State = default;
            m_ModeDirty = true;

            m_OnSessionEdge ??= OnSessionEdge;
            m_OnModeChanged ??= OnModeChanged;
            m_OnHardwareChanged ??= OnHardwareChanged;
            DisplayXRProvider.SessionStarted += m_OnSessionEdge;
            DisplayXRProvider.SessionStopped += m_OnSessionEdge;
            DisplayXRProvider.RenderingModeChanged += m_OnModeChanged;
            DisplayXRProvider.HardwareDisplayStateChanged += m_OnHardwareChanged;

            m_UsingSRP = GraphicsSettings.currentRenderPipeline != null;
            if (!m_UsingSRP)
            {
                s_OnPreRenderEye ??= PublishScriptEye;
                if (s_ScriptEyeUsers++ == 0) Camera.onPreRender += s_OnPreRenderEye;
            }

#if UNITY_EDITOR
            // Outside Play, LateUpdate runs only when something changes while every view
            // repaints on its own, so a draw submitted from LateUpdate would come and go.
            // Submit once per camera render instead.
            m_OnEditCamera ??= DrawForEditCamera;
            m_OnEditCameraSRP ??= DrawForEditCameraSRP;
            if (m_UsingSRP) RenderPipelineManager.beginCameraRendering += m_OnEditCameraSRP;
            else Camera.onPreCull += m_OnEditCamera;
#endif

            EnsureResources();
            Refresh();
        }

        void OnDisable()
        {
            DisplayXRProvider.SessionStarted -= m_OnSessionEdge;
            DisplayXRProvider.SessionStopped -= m_OnSessionEdge;
            DisplayXRProvider.RenderingModeChanged -= m_OnModeChanged;
            DisplayXRProvider.HardwareDisplayStateChanged -= m_OnHardwareChanged;
            if (!m_UsingSRP && --s_ScriptEyeUsers <= 0)
            {
                s_ScriptEyeUsers = 0;
                Camera.onPreRender -= s_OnPreRenderEye;
                Shader.SetGlobalFloat(s_ScriptEyeId, 0f);
            }
#if UNITY_EDITOR
            if (m_UsingSRP) RenderPipelineManager.beginCameraRendering -= m_OnEditCameraSRP;
            else Camera.onPreCull -= m_OnEditCamera;
#endif
#if DISPLAYXR_HAS_VIDEO
            HookVideo(null);
#endif
            DestroyOwned(m_Mesh);
            DestroyOwned(m_Material);
            m_Mesh = null;
            m_Material = null;
            m_State = default;
            m_Visible = false;
        }

        static void DestroyOwned(Object o)
        {
            if (o == null) return;
            if (Application.isPlaying) Destroy(o);
            else DestroyImmediate(o);
        }

        void OnSessionEdge() => m_ModeDirty = true;

        void OnModeChanged(uint previous, uint current) => m_ModeDirty = true;

        // Not a decision by itself: when the panel flips to 2D the provider switches to a
        // one-view rendering mode (and back), and the view count is what is read below. A
        // display with no switchable hardware can report 2D while it renders two views.
        void OnHardwareChanged(bool is3D) => m_ModeDirty = true;

        // Late (see the DefaultExecutionOrder above): the rig has been moved by then.
        void LateUpdate()
        {
            Refresh();
            if (Application.isPlaying) Draw(null);
        }

        /// <summary>
        /// Re-evaluate the source, the layout and the placement now. Runs every frame in
        /// LateUpdate; call it yourself only to have <see cref="CurrentTexture"/>,
        /// <see cref="IsStereo"/> or <see cref="PictureAspect"/> reflect a change before then.
        /// </summary>
        public void Refresh()
        {
            if (m_Material == null || m_Mesh == null) return;

            State s = default;
            s.texture = ResolveTexture(out float sourcePixelAspect);
            if (s.texture != null)
            {
                s.width = s.texture.width;
                s.height = s.texture.height;
                s.mipCount = s.texture.mipmapCount;
                // The FORMAT, not a colour-space guess: it is what decides whether the
                // sampler decodes (see DisplayXRStereoMediaMath.ColorTransform).
                s.formatIsSRGB = GraphicsFormatUtility.IsSRGBFormat(s.texture.graphicsFormat);
            }
            bool stated = sourceSize.x > 0 && sourceSize.y > 0;
            s.aspectWidth = stated ? sourceSize.x : s.width;
            s.aspectHeight = stated ? sourceSize.y : s.height;
            s.layout = layout;
            s.halfResolution = halfResolution;
            s.pixelAspect = pixelAspect > 0f ? pixelAspect : sourcePixelAspect;
            s.depthShift = depthShiftPercent * 0.01f;
            s.flipVertical = flipVertical;
            s.stereo = ComputeStereo();
            s.encoding = encoding;
            s.presentation = presentation;
            s.fit = fit;
            s.alphaMode = alphaMode;
            s.tint = tint;
            s.order = fullScreenOrder;
            s.valid = true;

            if (presentation == PresentationMode.FullScreen)
            {
                m_QuadMatrix = Matrix4x4.identity;
                s.frame = Vector2.one;
                s.frameAspect = ResolveViewAspect(ResolveRigCamera());
            }
            else
            {
                s.frame = PlaceQuad(out m_QuadMatrix);
                s.frameAspect = s.frame.y > 1e-6f ? s.frame.x / s.frame.y : 0f;
            }

            if (s.Same(m_State)) return;
            m_State = s;
            Apply(s);
        }

        // ---- source ------------------------------------------------------------

        Texture ResolveTexture(out float sourcePixelAspect)
        {
            sourcePixelAspect = 1f;
#if DISPLAYXR_HAS_VIDEO
            if (!ReferenceEquals(videoPlayer, m_HookedPlayer)) HookVideo(videoPlayer);
            var vp = videoPlayer;
            if (vp != null)
            {
                // Polled, not only event-driven: a new clip / URL can change the texture
                // and its size without this component being told, and both getters are
                // plain property reads.
                Texture t = vp.renderMode == VideoRenderMode.RenderTexture && vp.targetTexture != null
                    ? vp.targetTexture
                    : vp.texture;
                if (vp.isPrepared)
                {
                    uint den = vp.pixelAspectRatioDenominator;
                    if (den != 0) sourcePixelAspect = (float)vp.pixelAspectRatioNumerator / den;
                }
                return t; // null until prepared (API Only): nothing is drawn meanwhile
            }
#endif
            return texture;
        }

#if DISPLAYXR_HAS_VIDEO
        void HookVideo(VideoPlayer vp)
        {
            if (m_HookedPlayer != null && m_OnVideoPrepared != null)
                m_HookedPlayer.prepareCompleted -= m_OnVideoPrepared;
            m_HookedPlayer = vp;
            if (vp == null) return;

            m_OnVideoPrepared ??= OnVideoPrepared;
            vp.prepareCompleted += m_OnVideoPrepared;

            // Edit mode: look, but leave the player's settings and state alone.
            if (!Application.isPlaying) return;

            // Anything but "Render Texture with a target" would have the player draw the
            // whole side-by-side frame itself (camera planes / material override). API Only
            // decodes into the player's own texture and draws nothing.
            bool hasTarget = vp.renderMode == VideoRenderMode.RenderTexture && vp.targetTexture != null;
            if (!hasTarget && vp.renderMode != VideoRenderMode.APIOnly)
                vp.renderMode = VideoRenderMode.APIOnly;

            bool hasSource = vp.source == VideoSource.Url ? !string.IsNullOrEmpty(vp.url) : vp.clip != null;
            if (prepareVideo && hasSource && !vp.isPrepared)
                vp.Prepare();
        }

        // The texture and its size become known here. Looping and seeking keep the same
        // texture, so they need no handling; a new clip prepares again.
        void OnVideoPrepared(VideoPlayer vp) => m_State.valid = false;
#endif

        // ---- stereo or mono ----------------------------------------------------

        bool ComputeStereo()
        {
            if (forceMono || !Application.isPlaying) return false;
            // The managed session flag: no native call, and false on platforms / projects
            // where the provider never starts.
            if (!DisplayXRProvider.IsSessionReady) return false;

            if (m_ModeDirty)
            {
                // The active mode's view count decides. The mode table can still be empty
                // right after the session starts (it is filled late in a built player) -
                // then assume stereo, which is how the panel boots, and look again next
                // frame.
                uint active = DisplayXRProvider.ActiveModeIndex;
                var modes = DisplayXRProvider.Modes;
                bool found = false;
                for (int i = 0; i < modes.Count; i++)
                {
                    var m = modes[i];
                    if (m.modeIndex != active) continue;
                    m_ModeStereo = m.viewCount >= 2;
                    found = true;
                    break;
                }
                if (!found) m_ModeStereo = true;
                m_ModeDirty = !found;
            }
            return m_ModeStereo;
        }

        // ---- placement ---------------------------------------------------------

        // Returns the frame size in the quad's own units, and the quad's object-to-world.
        Vector2 PlaceQuad(out Matrix4x4 matrix)
        {
            if (placement == QuadPlacement.ZeroParallaxPlane)
            {
                Camera cam = ResolveRigCamera();
                if (TryGetPlane(cam, out Vector3 center, out Quaternion rotation, out Vector2 size, out Vector3 rigScale))
                {
                    m_WarnedNoPlane = false;
                    // The plane is defined in the rig's local units; give the quad the rig's scale.
                    matrix = Matrix4x4.TRS(center, rotation, rigScale);
                    return size;
                }
                if (!m_WarnedNoPlane && Application.isPlaying)
                {
                    m_WarnedNoPlane = true;
                    Debug.LogWarning("[DisplayXR] Stereo Media: no zero-parallax plane to place the quad on " +
                                     (cam == null
                                         ? "(no rig camera)"
                                         : "(the camera has no enabled DisplayXR rig, or its camera-centric rig converges at infinity)") +
                                     ". Using this object's transform and Quad Size instead.", this);
                }
            }
            matrix = transform.localToWorldMatrix;
            return new Vector2(Mathf.Max(0f, quadSize.x), Mathf.Max(0f, quadSize.y));
        }

        bool TryGetPlane(Camera cam, out Vector3 center, out Quaternion rotation, out Vector2 size, out Vector3 rigScale)
        {
            center = default; rotation = Quaternion.identity; size = default; rigScale = Vector3.one;
            if (cam == null) return false;
            if (!ReferenceEquals(cam, m_PlaneCamera) || (m_PlaneDisplayRig == null && m_PlaneCameraRig == null))
            {
                m_PlaneCamera = cam;
                cam.TryGetComponent(out m_PlaneDisplayRig);
                cam.TryGetComponent(out m_PlaneCameraRig);
            }
            return TryGetZeroParallaxPlane(cam, m_PlaneDisplayRig, m_PlaneCameraRig, ResolveViewAspect(cam),
                                           out center, out rotation, out size, out rigScale);
        }

        /// <summary>
        /// The rig's zero-parallax plane: where a flat picture gets no disparity from the
        /// rig, so only the picture's own disparity is seen.
        /// <list type="bullet">
        /// <item><see cref="DisplayXRDisplay"/>: the rig transform is the virtual display, so
        /// the plane is at the rig origin; its height is <c>virtualDisplayHeight</c>, or the
        /// physical display height when that is 0 (a nominal 0.2 while no session reports
        /// one).</item>
        /// <item><see cref="DisplayXRCamera"/>: the plane is <c>1 / invConvergenceDistance</c>
        /// in front of the camera, as tall as the rig's authored vertical FOV makes it
        /// there. There is none when the rig converges at infinity.</item>
        /// </list>
        /// Only public rig data is used; the projection itself is the runtime's. Returns
        /// false when the camera has no enabled rig or the plane is at infinity.
        /// </summary>
        /// <param name="rigCamera">The camera carrying the rig.</param>
        /// <param name="aspect">Displayed width / height of the view.</param>
        /// <param name="center">World position of the plane's centre.</param>
        /// <param name="rotation">World rotation; the plane faces along -forward.</param>
        /// <param name="size">Width and height in the rig's LOCAL units.</param>
        /// <param name="rigScale">The rig's lossy scale, to give an object <paramref name="size"/> applies to.</param>
        public static bool TryGetZeroParallaxPlane(Camera rigCamera, float aspect, out Vector3 center,
                                                   out Quaternion rotation, out Vector2 size, out Vector3 rigScale)
        {
            center = default; rotation = Quaternion.identity; size = default; rigScale = Vector3.one;
            if (rigCamera == null) return false;
            rigCamera.TryGetComponent(out DisplayXRDisplay display);
            rigCamera.TryGetComponent(out DisplayXRCamera cameraRig);
            return TryGetZeroParallaxPlane(rigCamera, display, cameraRig, aspect, out center, out rotation, out size, out rigScale);
        }

        static bool TryGetZeroParallaxPlane(Camera cam, DisplayXRDisplay display, DisplayXRCamera cameraRig, float aspect,
                                            out Vector3 center, out Quaternion rotation, out Vector2 size, out Vector3 rigScale)
        {
            center = default; rotation = Quaternion.identity; size = default; rigScale = Vector3.one;
            if (!(aspect > 0f)) aspect = 16f / 9f;
            Transform t = cam.transform;

            if (display != null && display.enabled)
            {
                float h = display.virtualDisplayHeight;
                if (h <= 0f)
                    h = DisplayXRProvider.IsSessionReady && DisplayXRProvider.TryGetDisplayInfo(out var info) && info.heightM > 0f
                        ? info.heightM
                        : 0.2f;
                center = t.position;
                size = new Vector2(h * aspect, h);
            }
            else if (cameraRig != null && cameraRig.enabled)
            {
                float inv = cameraRig.invConvergenceDistance;
                if (inv < 1e-3f) return false; // parallel projection: the plane is at infinity
                float d = 1f / inv;
                float tanHalf = Mathf.Tan(cameraRig.AuthoredFieldOfView * 0.5f * Mathf.Deg2Rad);
                DisplayXRStereoMediaMath.FrustumPlaneSize(tanHalf, d, aspect, out float w, out float h);
                center = t.TransformPoint(new Vector3(0f, 0f, d));
                size = new Vector2(w, h);
            }
            else
            {
                return false;
            }
            rotation = t.rotation;
            rigScale = t.lossyScale;
            return true;
        }

        // The camera whose plane the quad sits on, and the ONE camera a full-screen picture
        // is drawn for.
        Camera ResolveRigCamera()
        {
            if (rigCamera != null) return rigCamera;
            Camera cam = DisplayXRRigManager.ActiveCamera;
            // Rigs register only in Play; in edit mode use the main camera so the picture
            // previews where it will be.
            if (cam == null && !Application.isPlaying) cam = Camera.main;
            return cam;
        }

        float ResolveViewAspect(Camera cam)
        {
            if (viewAspect > 0f) return viewAspect;
            // The window the session composites into. Not Screen.*: in a transparent-overlay
            // app that is Unity's own cloaked window (#291).
            if (Application.isPlaying && DisplayXRProvider.IsSessionReady &&
                DisplayXRWindowSpaceUI.TryGetWindowPixelSize(out float w, out float h) && w > 0f && h > 0f)
                return w / h;
            if (cam != null && cam.aspect > 0f) return cam.aspect;
            return 16f / 9f;
        }

        // ---- drawing -----------------------------------------------------------

        void EnsureResources()
        {
            if (m_Material == null)
            {
                // Resources.Load first (reliable in player builds); Shader.Find fallback.
                Shader sh = Resources.Load<Shader>("DisplayXRStereoMedia");
                if (sh == null) sh = Shader.Find("DisplayXR/StereoMedia");
                if (sh == null || !sh.isSupported)
                {
                    if (!m_WarnedShader)
                    {
                        m_WarnedShader = true;
                        Debug.LogWarning("[DisplayXR] Stereo Media: shader 'DisplayXR/StereoMedia' is missing or " +
                                         "unsupported; nothing will be drawn.", this);
                    }
                    return;
                }
                m_Material = new Material(sh) { name = "DisplayXR Stereo Media", hideFlags = HideFlags.HideAndDontSave };
            }

            if (m_Mesh == null)
            {
                m_Mesh = new Mesh { name = "DisplayXR Stereo Media Quad", hideFlags = HideFlags.HideAndDontSave };
                m_Mesh.vertices = new[]
                {
                    new Vector3(-0.5f, -0.5f, 0f), new Vector3(0.5f, -0.5f, 0f),
                    new Vector3(0.5f, 0.5f, 0f), new Vector3(-0.5f, 0.5f, 0f),
                };
                m_Mesh.uv = new[]
                {
                    new Vector2(0f, 0f), new Vector2(1f, 0f), new Vector2(1f, 1f), new Vector2(0f, 1f),
                };
                m_Mesh.triangles = new[] { 0, 2, 1, 0, 3, 2 };
            }
        }

        void Apply(in State s)
        {
            if (s.texture == null || s.width <= 0 || s.height <= 0)
            {
                m_Visible = false;
                m_PictureAspect = 0f;
                return;
            }

            var sol = DisplayXRStereoMediaMath.Solve(
                s.aspectWidth, s.aspectHeight, s.layout, s.halfResolution, s.pixelAspect, s.fit, s.frameAspect,
                s.depthShift, s.flipVertical, s.stereo);
            m_PictureAspect = sol.pictureAspect;

            bool fullScreen = s.presentation == PresentationMode.FullScreen;
            bool opaque = s.alphaMode == AlphaMode.Opaque;
            m_Drawn = new Vector2(s.frame.x * sol.frameX, s.frame.y * sol.frameY);

            var m = m_Material;
            m.SetTexture(s_MainTexId, s.texture);
            m.SetColor(s_ColorId, s.tint);
            m.SetVector(s_STLId, new Vector4(sol.left.scaleX, sol.left.scaleY, sol.left.offsetX, sol.left.offsetY));
            m.SetVector(s_STRId, new Vector4(sol.right.scaleX, sol.right.scaleY, sol.right.offsetX, sol.right.offsetY));
            m.SetVector(s_ClampLId, new Vector4(sol.left.clampMinX, sol.left.clampMinY, sol.left.clampMaxX, sol.left.clampMaxY));
            m.SetVector(s_ClampRId, new Vector4(sol.right.clampMinX, sol.right.clampMinY, sol.right.clampMaxX, sol.right.clampMaxY));
            m.SetVector(s_FrameId, new Vector4(m_Drawn.x, m_Drawn.y, 0f, 0f));
            m.SetFloat(s_FullScreenId, fullScreen ? 1f : 0f);
            m.SetFloat(s_ColorXformId, DisplayXRStereoMediaMath.ColorTransform(
                QualitySettings.activeColorSpace == ColorSpace.Linear, s.formatIsSRGB, s.encoding));
            m.SetFloat(s_AlphaId, (float)s.alphaMode);
            m.SetFloat(s_MaxLodId, Mathf.Max(0, s.mipCount - 1));
            m.SetFloat(s_SrcBlendId, (float)BlendMode.One);
            m.SetFloat(s_DstBlendId, (float)(opaque ? BlendMode.Zero : BlendMode.OneMinusSrcAlpha));

            if (fullScreen)
            {
                bool background = s.order == FullScreenOrder.Background;
                m.SetFloat(s_ZWriteId, 0f);
                m.SetFloat(s_ZTestId, (float)(background ? CompareFunction.LessEqual : CompareFunction.Always));
                m.renderQueue = background ? k_EarlyQueue : k_LateQueue;
            }
            else
            {
                m.SetFloat(s_ZWriteId, opaque ? 1f : 0f);
                m.SetFloat(s_ZTestId, (float)CompareFunction.LessEqual);
                m.renderQueue = opaque ? k_EarlyQueue : (int)RenderQueue.Transparent;
            }

            m_Visible = m_Drawn.x > 0f && m_Drawn.y > 0f;
        }

        // Submits the picture for this frame. onlyCamera = null: the cameras the
        // presentation calls for (Play). Otherwise: that one camera, if it qualifies
        // (edit mode, once per camera render).
        //
        // Graphics.RenderMesh rather than a renderer on a child object: nothing is created,
        // parented or toggled, so there is no object to orphan when this GameObject is
        // activated or deactivated, and "which camera" is a parameter of the draw
        // (RenderParams.camera) instead of shared state changed between cameras.
        void Draw(Camera onlyCamera)
        {
            if (!m_Visible || m_Material == null || m_Mesh == null) return;

            bool fullScreen = m_State.presentation == PresentationMode.FullScreen;
            Camera target = onlyCamera;
            Bounds bounds;
            if (fullScreen)
            {
                // Clip-space geometry covers whatever camera renders it, so it is drawn for
                // exactly one: the rig camera. Never the Scene view, a preview, a
                // reflection probe or a second Game camera rendering into a texture.
                Camera rig = ResolveRigCamera();
                if (rig == null || (onlyCamera != null && !ReferenceEquals(onlyCamera, rig))) return;
                target = rig;
                bounds = new Bounds(rig.transform.position, Vector3.one * k_HugeExtent);
            }
            else
            {
                bounds = QuadWorldBounds();
            }

            var rp = new RenderParams(m_Material)
            {
                camera = target, // null = every camera: the quad is an ordinary scene object
                layer = gameObject.layer,
                worldBounds = bounds,
                shadowCastingMode = ShadowCastingMode.Off,
                receiveShadows = false,
                lightProbeUsage = LightProbeUsage.Off,
                reflectionProbeUsage = ReflectionProbeUsage.Off,
                motionVectorMode = MotionVectorGenerationMode.ForceNoMotion,
            };
            Graphics.RenderMesh(rp, m_Mesh, 0, m_QuadMatrix);
        }

        // The quad is a unit square scaled by m_Drawn in the shader, in the XY plane of
        // m_QuadMatrix.
        Bounds QuadWorldBounds()
        {
            Matrix4x4 q = m_QuadMatrix;
            float hx = m_Drawn.x * 0.5f, hy = m_Drawn.y * 0.5f;
            var extents = new Vector3(
                Mathf.Abs(q.m00) * hx + Mathf.Abs(q.m01) * hy,
                Mathf.Abs(q.m10) * hx + Mathf.Abs(q.m11) * hy,
                Mathf.Abs(q.m20) * hx + Mathf.Abs(q.m21) * hy);
            return new Bounds(new Vector3(q.m03, q.m13, q.m23), extents * 2f);
        }

#if UNITY_EDITOR
        void DrawForEditCameraSRP(ScriptableRenderContext context, Camera cam) => DrawForEditCamera(cam);

        void DrawForEditCamera(Camera cam)
        {
            if (Application.isPlaying || cam == null) return;
            if (cam.cameraType != CameraType.Game && cam.cameraType != CameraType.SceneView) return;
            Refresh();
            Draw(cam);
        }
#endif
    }
}
