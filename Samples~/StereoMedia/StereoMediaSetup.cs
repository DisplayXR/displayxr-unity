// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

using UnityEngine;
#if DISPLAYXR_SAMPLE_HAS_VIDEO
using UnityEngine.Video;
#endif

namespace DisplayXR.Samples
{
    /// <summary>
    /// Builds the Stereo Media sample when you press Play: a <see cref="DisplayXRStereoMedia"/>
    /// showing a generated side-by-side test picture (<see cref="StereoMediaTestPattern"/>) -
    /// or your own stereo video, if <see cref="videoUrl"/> is set - plus a small cube in
    /// front of the screen for reference.
    ///
    /// Put it on any GameObject in a scene with a camera, or use
    /// <b>DisplayXR > Samples > Create Stereo Media Scene</b>, which makes that scene for
    /// you. Like the Basic Scene sample, the content is built at runtime and discarded on
    /// Stop; the <b>Create Scene Content</b> button in the inspector authors it into the
    /// scene instead.
    ///
    /// While playing, select the object and change the <c>DisplayXRStereoMedia</c>
    /// component's settings live: Presentation (Quad / Full Screen), Fit, Depth Shift,
    /// Force Mono.
    /// </summary>
    public class StereoMediaSetup : MonoBehaviour
    {
        [Header("Test Picture")]

        [Tooltip("How the generated picture is packed. The component is set to the same layout.")]
        public DisplayXRStereoLayout patternLayout = DisplayXRStereoLayout.SideBySide;

        [Tooltip("Size of ONE eye's picture in pixels.")]
        public Vector2Int eyeResolution = new Vector2Int(960, 540);

        [Header("Your Own Video (optional)")]

        [Tooltip("A file path or URL of a stereo video, e.g. C:/Videos/clip_sbs.mp4. Empty = the generated test " +
                 "picture. Needs Unity's built-in Video module.")]
        public string videoUrl = "";

        [Tooltip("How the video packs the two eyes.")]
        public DisplayXRStereoLayout videoLayout = DisplayXRStereoLayout.SideBySide;

        [Tooltip("The video squeezes each eye to half size (a 1920x1080 or 3840x2160 side-by-side file usually does).")]
        public bool videoHalfResolution = true;

        public bool loopVideo = true;

        [Header("Presentation")]

        public DisplayXRStereoMedia.PresentationMode presentation = DisplayXRStereoMedia.PresentationMode.Quad;

        public DisplayXRStereoFit fit = DisplayXRStereoFit.Fit;

        [Tooltip("Adds a small cube in front of the screen plane: it overlaps the picture and shifts against it as " +
                 "you move your head in Quad presentation.")]
        public bool addReferenceCube = true;

        void Start()
        {
            // Already authored (Create Scene Content), or set up by hand.
            if (GetComponent<DisplayXRStereoMedia>() != null) return;

            Camera cam = EnsureRig();
            var media = gameObject.AddComponent<DisplayXRStereoMedia>();
            Texture2D pattern = UseVideo ? null
                : StereoMediaTestPattern.Create(eyeResolution.x, eyeResolution.y, patternLayout);
            Configure(media, pattern, cam);
            if (addReferenceCube) CreateReferenceCube(cam, null);
        }

        /// <summary>True when <see cref="videoUrl"/> is set and the Video module is there to play it.</summary>
        public bool UseVideo
        {
            get
            {
#if DISPLAYXR_SAMPLE_HAS_VIDEO
                return !string.IsNullOrWhiteSpace(videoUrl);
#else
                return false;
#endif
            }
        }

        /// <summary>
        /// The main camera, with a display-centric rig added if it carries no DisplayXR rig.
        /// A display-centric rig's transform IS the screen, which is the natural frame for
        /// a picture viewer: the quad lands on the screen plane, full size.
        /// </summary>
        public static Camera EnsureRig()
        {
            Camera cam = Camera.main;
            if (cam == null) return null;
            if (cam.GetComponent<DisplayXRDisplay>() == null && cam.GetComponent<DisplayXRCamera>() == null)
            {
                var rig = cam.gameObject.AddComponent<DisplayXRDisplay>();
                rig.virtualDisplayHeight = 1f; // the screen is 1 unit tall in the scene
            }
            return cam;
        }

        /// <summary>
        /// Point <paramref name="media"/> at the picture (or the video) with this
        /// component's settings. Shared by the runtime path and the editor button.
        /// </summary>
        public void Configure(DisplayXRStereoMedia media, Texture pattern, Camera cam)
        {
            media.rigCamera = cam;
            media.presentation = presentation;
            media.fit = fit;
            media.placement = DisplayXRStereoMedia.QuadPlacement.ZeroParallaxPlane;

#if DISPLAYXR_SAMPLE_HAS_VIDEO
            if (UseVideo)
            {
                var player = GetComponent<VideoPlayer>();
                if (player == null) player = gameObject.AddComponent<VideoPlayer>();
                player.source = VideoSource.Url;
                player.url = videoUrl;
                player.isLooping = loopVideo;
                player.playOnAwake = true;
                // The component shows the player's own texture; the player draws nothing itself.
                player.renderMode = VideoRenderMode.APIOnly;
                media.videoPlayer = player;
                media.layout = videoLayout;
                media.halfResolution = videoHalfResolution;
                if (Application.isPlaying) player.Play();
                return;
            }
#endif
            media.texture = pattern;
            media.layout = patternLayout;
            media.halfResolution = false; // the generated picture is full resolution per eye
        }

        /// <summary>
        /// A cube a little in front of the rig's zero-parallax plane, low and to the right,
        /// sized relative to the plane. Returns null when the camera has no such plane.
        /// </summary>
        public static GameObject CreateReferenceCube(Camera cam, Material material)
        {
            if (cam == null) return null;
            float aspect = cam.aspect > 0f ? cam.aspect : 16f / 9f;
            if (!DisplayXRStereoMedia.TryGetZeroParallaxPlane(cam, aspect, out Vector3 center, out Quaternion rotation,
                                                              out Vector2 size, out Vector3 scale))
                return null;

            float w = size.x * scale.x, h = size.y * scale.y;
            var cube = GameObject.CreatePrimitive(PrimitiveType.Cube);
            cube.name = "Reference Cube";
            // -forward is towards the viewer: in front of the screen.
            cube.transform.position = center + rotation * new Vector3(0.36f * w, -0.3f * h, -0.12f * h);
            cube.transform.rotation = rotation * Quaternion.Euler(15f, 30f, 0f);
            cube.transform.localScale = Vector3.one * (0.12f * h);

            var renderer = cube.GetComponent<Renderer>();
            if (material == null)
            {
                material = new Material(FindLitShader());
                Tint(material, new Color(0.9f, 0.5f, 0.1f));
            }
            renderer.sharedMaterial = material;

            if (FindAnyObjectByType<Light>() == null)
            {
                var lightGo = new GameObject("DirectionalLight");
                var light = lightGo.AddComponent<Light>();
                light.type = LightType.Directional;
                lightGo.transform.rotation = rotation * Quaternion.Euler(40f, -30f, 0f);
            }
            return cube;
        }

        /// <summary>The lit shader of whichever render pipeline the project uses.</summary>
        public static Shader FindLitShader()
        {
            Shader s = Shader.Find("Universal Render Pipeline/Lit");
            if (s == null) s = Shader.Find("HDRP/Lit");
            if (s == null) s = Shader.Find("Standard");
            return s;
        }

        public static void Tint(Material material, Color color)
        {
            if (material.HasProperty("_BaseColor")) material.SetColor("_BaseColor", color); // URP / HDRP
            if (material.HasProperty("_Color")) material.SetColor("_Color", color);         // Built-in
        }
    }
}
