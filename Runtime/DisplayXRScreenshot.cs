// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Concurrent;
using System.IO;
using UnityEngine;
using UnityEngine.Rendering;

namespace DisplayXR
{
    /// <summary>
    /// Saves the multi-view atlas the runtime composes for this session as a PNG.
    /// Mirrors the C++ test app and Unreal plugin convention:
    ///   <Pictures>/DisplayXR/<appname>-N_atlas_<viewCount>_<cols>x<rows>.png
    /// where N auto-increments and viewCount/cols/rows come from the active
    /// rendering mode (the runtime owns the "_atlas_..." suffix; see
    /// DisplayXR/displayxr-runtime#425). A short white flash gives the user
    /// visual feedback.
    ///
    /// As of #140 (W6 of #396) the capture is runtime-owned: a live session calls
    /// <c>xrCaptureAtlasEXT</c> (XR_EXT_atlas_capture) via
    /// <see cref="DisplayXRProvider.CaptureAtlas"/>, the runtime reads back the
    /// compositor's own atlas image and writes the PNG. The plugin no longer does
    /// an app-side <c>AsyncGPUReadback</c> or a hidden-camera Kooima re-render.
    ///
    /// A live provider session supplies a path prefix to xrCaptureAtlasEXT and the
    /// runtime writes the PNG on its next composed frame.
    ///
    /// Bind <see cref="Capture"/> to any developer-chosen event (key, button,
    /// gamepad). The included sample <c>DisplayXRInputController</c> binds it
    /// to the 'I' key by default.
    /// </summary>
    public static class DisplayXRScreenshot
    {
        /// <summary>Fired on the main thread after the PNG is written.</summary>
        public static event Action<string> OnSaved;

        /// <summary>Fired on the main thread on capture failure.</summary>
        public static event Action<string> OnFailed;

        /// <summary>Path of the most recently saved PNG (null until first success).</summary>
        public static string LastSavedPath { get; private set; }

        /// <summary>
        /// Output directory for saved PNGs. Defaults to
        /// <c>Environment.SpecialFolder.MyPictures</c> + "DisplayXR".
        /// </summary>
        public static string OutputDirectory
        {
            get => s_OutputDirectory ?? DefaultOutputDirectory();
            set => s_OutputDirectory = value;
        }

        private const int kFlashFrames = 8;

        // Frames to wait after issuing a live (runtime-owned) capture before
        // starting the feedback flash. The flash draws into the same eye buffers
        // the runtime composites for xrCaptureAtlasEXT, and the runtime grabs its
        // next composed frame — so starting the flash immediately whites out the
        // saved atlas. Delaying it a few frames lets the clean atlas be captured
        // first.
        private const int kFlashStartDelayFrames = 3;

        private static string s_OutputDirectory;
        private static int s_FlashFramesRemaining;
        private static int s_FlashStartDelay;

        // Main-thread dispatch queue drained by the live flash overlay each frame
        // (OnSaved/OnFailed callbacks marshalled back to the main thread).
        private static readonly ConcurrentQueue<Action> s_MainQueue = new ConcurrentQueue<Action>();

        // Cached material for the flash overlay (HideAndDontSave; reused across captures).
        private static Material s_FlashMat;

        // ================================================================
        // Public API
        // ================================================================

        /// <summary>
        /// Request a screenshot. The capture is requested from the runtime via
        /// xrCaptureAtlasEXT — the runtime reads back its own composited atlas and
        /// writes the PNG on the next composed frame.
        /// </summary>
        public static void Capture()
        {
            DrainMainQueue();
            CaptureLive();
        }

        // ================================================================
        // Live OpenXR session: runtime-owned capture via xrCaptureAtlasEXT
        // ================================================================

        private static void CaptureLive()
        {
            // The custom display Provider (epic #166) fulfils the capture runtime-side
            // via xrCaptureAtlasEXT.

            // Built-app stereo is two-view (Unity SetStereoViewMatrix L/R); this
            // matches the cols x rows the runtime composes for a standard 3D mode.
            // cols/rows feed only the PNG filename — the runtime owns the
            // "_atlas_<viewCount>_<cols>x<rows>.png" suffix and is the
            // authoritative source of the actual atlas dimensions.
            const int cols = 2;
            const int rows = 1;

            // Build the numbered output PREFIX (bare — no layout tokens, no
            // extension): the runtime appends "_atlas_<viewCount>_<cols>x<rows>.png"
            // (DisplayXR/displayxr-runtime#425), so the prefix must NOT pre-bake the
            // layout or the final name duplicates it (..._2x1_atlas_2_2x1.png).
            // Mirrors the demos' dxr_capture::MakeCaptureAtlasPrefix — number
            // against "<stem>-<N>_atlas_*.png" so repeats accumulate rather than
            // overwrite.
            string outDir = OutputDirectory;
            string stem = SanitizeStem(Application.productName);
            string prefix;
            try
            {
                Directory.CreateDirectory(outDir);
                int n = NextSequenceNumber(outDir, stem, cols, rows);
                prefix = Path.Combine(outDir, $"{stem}-{n}");
            }
            catch (Exception ex)
            {
                Fail($"Could not prepare output directory: {ex.Message}");
                return;
            }

            // projectionOnly: true matches the native test apps / demos default.
            bool ok = DisplayXRProvider.CaptureAtlas(prefix, projectionOnly: true);
            if (!ok)
            {
                Fail("xrCaptureAtlasEXT unavailable or rejected the request "
                     + "(runtime missing XR_EXT_atlas_capture or no live session)");
                return;
            }

            // The runtime writes "<prefix>_atlas_<viewCount>_<cols>x<rows>.png" on its
            // next composed frame. Optimistic success (the call is non-blocking;
            // matches the native apps).
            Succeed(prefix + $"_atlas_{cols * rows}_{cols}x{rows}.png");

            // Defer the feedback flash: it draws into the same eye buffers the
            // runtime composites for the capture, so arming it now whites out the
            // saved atlas. Wait a few frames (the overlay ticks the delay down and
            // arms s_FlashFramesRemaining) so the clean atlas is captured first.
            s_FlashStartDelay = kFlashStartDelayFrames;
            s_FlashFramesRemaining = 0;
            EnsureFlashOverlay();
        }

        // ================================================================
        // Flash overlay (live path)
        // ================================================================

        private static void EnsureFlashOverlay()
        {
            // Attach the overlay component to EVERY registered rig camera. Unity
            // OpenXR doesn't gate per-camera rendering by our active flag — every
            // stereo rig camera in the scene renders to the swapchain each frame,
            // and whichever renders last overwrites earlier ones. Attaching to all
            // of them guarantees the flash lands in the final eye buffer regardless
            // of which rig "wins" the swapchain submission.
            var cams = DisplayXRRigManager.RegisteredCameras;
            if (cams == null || cams.Count == 0) return;
            foreach (var cam in cams)
            {
                if (cam == null) continue;
                if (cam.GetComponent<DisplayXRFlashOverlay>() != null) continue;
                cam.gameObject.AddComponent<DisplayXRFlashOverlay>();
                Debug.Log("[DisplayXR] Flash overlay attached to rig camera: " + cam.name);
            }
        }

        internal static int _GetFlashFramesRemaining() => s_FlashFramesRemaining;
        internal static int _MaxFlashFrames => kFlashFrames;
        internal static void _ConsumeFlashFrame()
        {
            if (s_FlashFramesRemaining > 0) s_FlashFramesRemaining--;
        }

        // Live-path pre-flash delay: ticked once per frame by the overlay. While
        // counting down, the flash stays disarmed so the runtime's atlas capture
        // (which grabs its next composed frame) isn't whited out by the flash.
        // When the delay elapses, the flash is armed for kFlashFrames frames.
        internal static void _TickFlashStartDelay()
        {
            if (s_FlashStartDelay <= 0) return;
            s_FlashStartDelay--;
            if (s_FlashStartDelay == 0) s_FlashFramesRemaining = kFlashFrames;
        }

        // ================================================================
        // White flash overlay
        // ================================================================

        // Used by the live path's DisplayXRFlashOverlay (Camera.onPostRender)
        // to draw into whatever RT the rig camera just wrote to (the OpenXR
        // swapchain image). Don't change RenderTexture.active here.
        internal static void _DrawFlashGL(int framesRemaining)
        {
            float alpha = Mathf.Clamp01((float)framesRemaining / kFlashFrames);
            EnsureFlashMaterial();
            if (s_FlashMat == null) return;

            GL.PushMatrix();
            GL.LoadOrtho();
            s_FlashMat.SetPass(0);
            GL.Begin(GL.QUADS);
            GL.Color(new Color(1f, 1f, 1f, alpha));
            GL.Vertex3(0f, 0f, 0f);
            GL.Vertex3(1f, 0f, 0f);
            GL.Vertex3(1f, 1f, 0f);
            GL.Vertex3(0f, 1f, 0f);
            GL.End();
            GL.PopMatrix();
        }

        private static void EnsureFlashMaterial()
        {
            if (s_FlashMat != null) return;
            var shader = Shader.Find("Hidden/Internal-Colored");
            if (shader == null) return;
            s_FlashMat = new Material(shader) { hideFlags = HideFlags.HideAndDontSave };
            s_FlashMat.SetInt("_SrcBlend", (int)BlendMode.SrcAlpha);
            s_FlashMat.SetInt("_DstBlend", (int)BlendMode.OneMinusSrcAlpha);
            s_FlashMat.SetInt("_Cull", (int)CullMode.Off);
            s_FlashMat.SetInt("_ZWrite", 0);
            s_FlashMat.SetInt("_ZTest", (int)CompareFunction.Always);
        }

        // ================================================================
        // Path / naming
        // ================================================================

        private static string DefaultOutputDirectory()
        {
            string pictures = Environment.GetFolderPath(Environment.SpecialFolder.MyPictures);
            return Path.Combine(pictures, "DisplayXR");
        }

        private static string SanitizeStem(string name)
        {
            if (string.IsNullOrEmpty(name)) return "DisplayXRApp";
            var chars = name.ToCharArray();
            for (int i = 0; i < chars.Length; i++)
            {
                char c = chars[i];
                bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
                       || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
                if (!ok) chars[i] = '_';
            }
            return new string(chars);
        }

        // Number against the final "<stem>-<N>_atlas_<viewCount>_<cols>x<rows>.png"
        // name (the runtime-owned suffix, #425) so the live and editor-preview paths
        // share one counter and repeats accumulate rather than overwrite. Mirrors the
        // demos' dxr_capture::NextCaptureNumSuffix.
        private static int NextSequenceNumber(string dir, string stem, int cols, int rows)
        {
            if (!Directory.Exists(dir)) return 1;
            string prefix = stem + "-";
            string suffix = $"_atlas_{cols * rows}_{cols}x{rows}.png";
            int max = 0;
            foreach (string path in Directory.GetFiles(dir, $"{stem}-*{suffix}"))
            {
                string name = Path.GetFileName(path);
                if (!name.StartsWith(prefix, StringComparison.Ordinal)) continue;
                if (!name.EndsWith(suffix, StringComparison.Ordinal)) continue;
                int midLen = name.Length - prefix.Length - suffix.Length;
                if (midLen <= 0) continue;
                string mid = name.Substring(prefix.Length, midLen);
                if (int.TryParse(mid, out int n) && n > max) max = n;
            }
            return max + 1;
        }

        // ================================================================
        // Plumbing
        // ================================================================

        private static void Succeed(string path)
        {
            LastSavedPath = path;
            Debug.Log($"[DisplayXR] Atlas screenshot saved: {path}");
            try { OnSaved?.Invoke(path); } catch (Exception ex) { Debug.LogException(ex); }
        }

        private static void Fail(string message)
        {
            Debug.LogWarning($"[DisplayXR] Atlas screenshot failed: {message}");
            try { OnFailed?.Invoke(message); } catch (Exception ex) { Debug.LogException(ex); }
        }

        private static void DrainMainQueue()
        {
            while (s_MainQueue.TryDequeue(out var a))
            {
                try { a(); } catch (Exception ex) { Debug.LogException(ex); }
            }
        }

        // Exposed so the live path's per-frame overlay can drain the queue.
        internal static void _DrainMainQueue() => DrainMainQueue();
    }

    // Component attached to the active rig camera that draws the white flash
    // into the camera's render target via a CommandBuffer. Live (non-SA) path.
    // CommandBuffer is required because Camera.OnPostRender (the legacy
    // event/instance message) fires after Unity releases the OpenXR swapchain
    // image, so any draws there land in nothing. CameraEvent.AfterEverything
    // runs as part of Unity's render queue, before swapchain release.
    [DisallowMultipleComponent]
    internal class DisplayXRFlashOverlay : MonoBehaviour
    {
        // BeforeImageEffects fires before Unity's XR plugin snapshots the eye
        // texture for swapchain submit. AfterEverything is too late in OpenXR.
        private const CameraEvent kEvent = CameraEvent.BeforeImageEffects;
        private Camera m_Cam;
        private CommandBuffer m_CB;
        private bool m_Attached;
        private static Material s_BlitMat;
        private static System.Collections.Generic.HashSet<string> s_LoggedCams =
            new System.Collections.Generic.HashSet<string>();
        // Multiple overlays may exist (one per rig camera). Decrement the shared
        // flash counter only once per Unity frame to avoid burning through it
        // N× faster.
        private static int s_LastTickFrame = -1;

        void OnEnable()
        {
            m_Cam = GetComponent<Camera>();
            m_CB = new CommandBuffer { name = "DisplayXR Flash" };
            // Diagnose: log every distinct camera that renders, so we can
            // verify Main Camera is in fact the one writing to the swapchain.
            Camera.onPreRender += LogRenderingCamera;
        }

        void OnDisable()
        {
            Camera.onPreRender -= LogRenderingCamera;
            DetachCB();
            m_CB?.Release();
            m_CB = null;
        }

        private static void LogRenderingCamera(Camera c)
        {
            if (c == null) return;
            string key = c.name + (c.stereoEnabled ? " [stereo]" : "");
            if (s_LoggedCams.Add(key))
            {
                string tt = c.targetTexture != null ? c.targetTexture.name : "<null>";
                Debug.Log($"[DisplayXR] Rendering camera: {key}, targetEye={c.stereoTargetEye}, targetTex={tt}");
            }
        }

        void LateUpdate()
        {
            // Drain main-thread results so OnSaved/OnFailed fire and the success
            // log appears.
            DisplayXRScreenshot._DrainMainQueue();

            bool decrementThisFrame = Time.frameCount != s_LastTickFrame;
            if (decrementThisFrame) s_LastTickFrame = Time.frameCount;

            // Live path: count down the pre-flash delay once per frame so the
            // runtime captures a clean atlas before the flash draws into the eye
            // buffers. This arms s_FlashFramesRemaining when the delay elapses, so
            // read the count AFTER ticking.
            if (decrementThisFrame) DisplayXRScreenshot._TickFlashStartDelay();

            int frames = DisplayXRScreenshot._GetFlashFramesRemaining();

            if (frames <= 0 || m_Cam == null)
            {
                DetachCB();
                if (frames > 0 && decrementThisFrame) DisplayXRScreenshot._ConsumeFlashFrame();
                return;
            }

            EnsureBlitMaterial();
            if (s_BlitMat == null)
            {
                if (!m_LoggedNoMat)
                {
                    Debug.LogWarning("[DisplayXR] Flash material is null — flash will not render.");
                    m_LoggedNoMat = true;
                }
                if (decrementThisFrame) DisplayXRScreenshot._ConsumeFlashFrame();
                return;
            }

            float alpha = (float)frames / DisplayXRScreenshot._MaxFlashFrames;
            s_BlitMat.SetColor("_Color", new Color(1f, 1f, 1f, alpha));

            m_CB.Clear();
            // Pass Texture2D.whiteTexture so Blit has a valid source; our shader
            // doesn't sample it but `null` source is unreliable across platforms.
            m_CB.Blit(Texture2D.whiteTexture, BuiltinRenderTextureType.CameraTarget, s_BlitMat);

            if (!m_Attached)
            {
                m_Cam.AddCommandBuffer(kEvent, m_CB);
                m_Attached = true;
                Debug.Log($"[DisplayXR] Flash CB attached to {m_Cam.name} at {kEvent}, alpha={alpha:F2}, shader={s_BlitMat.shader.name}, supported={s_BlitMat.shader.isSupported}");
            }

            if (decrementThisFrame) DisplayXRScreenshot._ConsumeFlashFrame();
        }

        private bool m_LoggedNoMat;

        private void DetachCB()
        {
            if (m_Attached && m_Cam != null && m_CB != null)
                m_Cam.RemoveCommandBuffer(kEvent, m_CB);
            m_Attached = false;
        }

        private static void EnsureBlitMaterial()
        {
            if (s_BlitMat != null) return;
            // Loaded from Runtime/Resources/ so Unity bundles it in builds
            // (Shader.Find at runtime would otherwise let Unity's shader
            // stripping drop it from the build).
            var shader = Resources.Load<Shader>("DisplayXRFlash");
            if (shader == null) shader = Shader.Find("Hidden/DisplayXRFlash");
            if (shader == null)
            {
                Debug.LogWarning("[DisplayXR] DisplayXRFlash shader not found — flash will be invisible.");
                return;
            }
            s_BlitMat = new Material(shader) { hideFlags = HideFlags.HideAndDontSave };
        }
    }
}
