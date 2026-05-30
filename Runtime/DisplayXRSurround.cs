// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0

using UnityEngine;
using UnityEngine.Experimental.Rendering;

namespace DisplayXR
{
    /// <summary>
    /// Renders a Unity UI Canvas as a full-window <b>2D surround</b> (issue #131):
    /// a flat, post-weave 2D layer that the runtime blits into the non-canvas
    /// region of the window — always at full native panel resolution, unlike the
    /// pre-weave window-space UI layer (<see cref="DisplayXRWindowSpaceUI"/>).
    ///
    /// Use it for crisp 2D content (text bubbles, HUD chrome, frames) that sits
    /// <i>around</i> a 3D canvas sub-rect. Pair with <see cref="canvasRect"/> (or
    /// call <c>DisplayXRNative.displayxr_set_canvas_rect</c> yourself) to shrink
    /// the 3D weave into a sub-region; the surround fills everything outside it.
    ///
    /// Unlike wsui this is genuinely flat (no per-eye disparity) and the surround
    /// texture is full-window, so a UI element at texture pixel (x, y) appears at
    /// window pixel (x, y). The runtime copies the non-canvas pixels 1:1; pixels
    /// the surround texture leaves transparent (alpha 0) present as transparent
    /// in a transparent-overlay app (the bubble shows, the rest shows through).
    ///
    /// Windows D3D12 hooked path (built apps) only for now — the native side
    /// allocates a SHARED RGBA8 texture + ID3D12Fence on Unity's device and
    /// registers it via xrSetSharedTextureSurround2DFenceEXT.
    /// </summary>
    [AddComponentMenu("DisplayXR/2D Surround")]
    [RequireComponent(typeof(Canvas))]
    [ExecuteAlways]
    public class DisplayXRSurround : MonoBehaviour
    {
        [Header("Canvas sub-rect (3D region, window pixels)")]
        [Tooltip("If 'setCanvasRect' is on, the 3D content weaves into this rect " +
                 "(window-client pixels) and the surround fills the rest. " +
                 "Leave setCanvasRect off to manage the canvas rect elsewhere.")]
        public bool setCanvasRect = true;
        public RectInt canvasRect = new RectInt(0, 0, 0, 0);

        [Header("Render")]
        [Tooltip("Surround texture size. 0,0 = use the runtime's display pixel " +
                 "dimensions (must match the window client area, per spec).")]
        public Vector2Int resolution = new Vector2Int(0, 0);

        [Tooltip("Flip the rendered texture vertically. The runtime copies the " +
                 "surround texture 1:1 into the (top-left origin) back buffer; " +
                 "flip if the content appears upside-down on device.")]
        public bool flipY = true;

        /// <summary>The RGBA8 RenderTexture the Canvas is captured into.</summary>
        public RenderTexture SurroundTexture { get; private set; }

        // Park the WorldSpace canvas far from scene content on a private layer
        // that only our dedicated camera renders (same approach as wsui).
        private static readonly Vector3 kCanvasWorldPos = new Vector3(0, 200000f, 0);
        private const int kPrivateLayer = 29;

        private Canvas m_Canvas;
        private RectTransform m_CanvasRect;
        private Camera m_Camera;

        private RenderMode m_OrigRenderMode;
        private Vector3 m_OrigPos;
        private Quaternion m_OrigRot;
        private Vector3 m_OrigScale;
        private int m_OrigLayer;
        private Camera m_OrigWorldCamera;
        private bool m_StateSaved;

        private int m_Width, m_Height;
        private bool m_Registered;

        // Editor (standalone session) path: the SA session runs on its own
        // device, so we copy our RT into a SHARED bridge texture the native side
        // registers with the runtime. In a built app (hooked path) the runtime
        // shares Unity's device and reads our RT directly — no bridge.
        private bool m_Editor;
        private Texture2D m_BridgeTex;

        // The 3D canvas sub-rect in window pixels (defaults to centered half).
        private RectInt EffectiveCanvasRect()
        {
            RectInt r = canvasRect;
            if (r.width <= 0 || r.height <= 0)
                r = new RectInt(m_Width / 4, m_Height / 4, m_Width / 2, m_Height / 2);
            return r;
        }

        void OnEnable()
        {
            m_Canvas = GetComponent<Canvas>();
            m_CanvasRect = m_Canvas.GetComponent<RectTransform>();
            TrySetup();
        }

        // Build the RT + camera once a valid resolution is known. Retried from
        // LateUpdate if display info isn't available yet at OnEnable.
        private void TrySetup()
        {
            if (SurroundTexture != null) return; // already set up
            if (m_Canvas == null) return;

            ResolveResolution();
            if (m_Width <= 0 || m_Height <= 0)
                return; // no valid dims yet — retry next LateUpdate

            // Save canvas state for restoration.
            m_OrigRenderMode = m_Canvas.renderMode;
            m_OrigPos = m_CanvasRect.position;
            m_OrigRot = m_CanvasRect.rotation;
            m_OrigScale = m_CanvasRect.localScale;
            m_OrigLayer = gameObject.layer;
            m_OrigWorldCamera = m_Canvas.worldCamera;
            m_StateSaved = true;

            // RGBA8 RT (runtime requires R8G8B8A8_UNORM for the surround source).
            var desc = new RenderTextureDescriptor(m_Width, m_Height,
                GraphicsFormat.R8G8B8A8_UNorm, GraphicsFormat.D24_UNorm_S8_UInt)
            {
                msaaSamples = 1,
                useMipMap = false,
                autoGenerateMips = false,
            };
            SurroundTexture = new RenderTexture(desc) { name = "DisplayXR_Surround" };
            SurroundTexture.Create();

            // WorldSpace canvas sized 1 UI unit = 1 window pixel, parked on the
            // private layer. UI elements positioned in window-pixel space map 1:1
            // into the surround texture.
            m_Canvas.renderMode = RenderMode.WorldSpace;
            m_CanvasRect.position = kCanvasWorldPos;
            m_CanvasRect.rotation = Quaternion.identity;
            m_CanvasRect.localScale = new Vector3(0.01f, 0.01f, 0.01f);
            m_CanvasRect.sizeDelta = new Vector2(m_Width, m_Height);
            SetLayerRecursive(gameObject, kPrivateLayer);

            var camGO = new GameObject("DisplayXR_SurroundCam");
            camGO.transform.SetParent(transform, false);
            camGO.hideFlags = HideFlags.HideAndDontSave;
            camGO.transform.position = kCanvasWorldPos + new Vector3(0, 0, 1);
            // flipY: invert the up vector so the captured RT is Y-flipped to match
            // the runtime's top-left back-buffer origin (same trick as wsui).
            camGO.transform.rotation = flipY
                ? Quaternion.LookRotation(Vector3.back, Vector3.down)
                : Quaternion.LookRotation(Vector3.back, Vector3.up);

            m_Camera = camGO.AddComponent<Camera>();
            m_Camera.clearFlags = CameraClearFlags.SolidColor;
            m_Camera.backgroundColor = Color.clear; // transparent outside the bubble
            m_Camera.orthographic = true;
            m_Camera.orthographicSize = m_Height * 0.005f; // half canvas height in world units
            m_Camera.aspect = (float)m_Width / m_Height;
            m_Camera.nearClipPlane = 0.01f;
            m_Camera.farClipPlane = 10f;
            m_Camera.targetTexture = SurroundTexture;
            m_Camera.cullingMask = 1 << kPrivateLayer;
            m_Camera.depth = -2000;
            m_Camera.enabled = false; // rendered manually in LateUpdate

            m_Canvas.worldCamera = m_Camera;

            m_Editor = Application.isEditor;
            RegisterSurround();
            Debug.Log($"[DisplayXR] Surround enabled: {m_Width}x{m_Height} " +
                      $"(RGBA8, {(m_Editor ? "standalone/bridge" : "hooked")})");
        }

        void OnDisable()
        {
            if (m_Registered)
            {
                try
                {
                    if (m_Editor)
                        DisplayXRNative.displayxr_standalone_surround_clear();
                    else
                    {
                        DisplayXRNative.displayxr_surround_clear();
                        if (setCanvasRect)
                            DisplayXRNative.displayxr_set_canvas_rect(0, 0, 0, 0);
                    }
                }
                catch (System.EntryPointNotFoundException) { }
                m_Registered = false;
            }

            if (m_BridgeTex != null)
            {
                // CreateExternalTexture'd handle doesn't own the native resource
                // (the SA backend keeps it); just drop our wrapper.
                if (Application.isPlaying) Destroy(m_BridgeTex);
                else DestroyImmediate(m_BridgeTex);
                m_BridgeTex = null;
            }

            if (m_StateSaved && m_Canvas != null)
            {
                m_Canvas.renderMode = m_OrigRenderMode;
                m_Canvas.worldCamera = m_OrigWorldCamera;
                if (m_CanvasRect != null)
                {
                    m_CanvasRect.position = m_OrigPos;
                    m_CanvasRect.rotation = m_OrigRot;
                    m_CanvasRect.localScale = m_OrigScale;
                }
                SetLayerRecursive(gameObject, m_OrigLayer);
                m_StateSaved = false;
            }

            if (m_Camera != null)
            {
                if (Application.isPlaying) Destroy(m_Camera.gameObject);
                else DestroyImmediate(m_Camera.gameObject);
                m_Camera = null;
            }
            if (SurroundTexture != null)
            {
                SurroundTexture.Release();
                if (Application.isPlaying) Destroy(SurroundTexture);
                else DestroyImmediate(SurroundTexture);
                SurroundTexture = null;
            }
        }

        void LateUpdate()
        {
            if (SurroundTexture == null)
            {
                TrySetup(); // display info may have become available
                return;
            }
            if (m_Camera == null) return;
            m_Camera.Render();

            if (m_Editor)
            {
                // Standalone: copy our RT into the SHARED bridge the runtime reads.
                if (m_BridgeTex == null) TryAcquireBridge();
                if (m_BridgeTex != null)
                    Graphics.CopyTexture(SurroundTexture, m_BridgeTex);
            }

            // Re-register cheaply each frame (robust to RT/bridge recreation).
            if (!m_Registered) RegisterSurround();
        }

        private void RegisterSurround()
        {
            if (SurroundTexture == null) return;
            RectInt r = EffectiveCanvasRect();
            try
            {
                if (m_Editor)
                {
                    if (m_BridgeTex == null) TryAcquireBridge();
                    if (m_BridgeTex == null) return; // SA session not up yet — retry
                    if (setCanvasRect)
                        DisplayXRNative.displayxr_standalone_surround_set_active(
                            1, r.x, r.y, (uint)r.width, (uint)r.height);
                    else
                        DisplayXRNative.displayxr_standalone_surround_set_active(1, 0, 0, 0, 0);
                    m_Registered = true;
                }
                else
                {
                    // Hooked path: the runtime shares Unity's device; register the
                    // RT directly + set the canvas sub-rect.
                    DisplayXRNative.displayxr_surround_set_texture(
                        SurroundTexture.GetNativeTexturePtr(), (uint)m_Width, (uint)m_Height);
                    if (setCanvasRect)
                        DisplayXRNative.displayxr_set_canvas_rect(r.x, r.y, (uint)r.width, (uint)r.height);
                    m_Registered = true;
                }
            }
            catch (System.EntryPointNotFoundException)
            {
                // Older plugin without surround support — no-op.
            }
        }

        private void TryAcquireBridge()
        {
            if (m_BridgeTex != null) return;
            try
            {
                DisplayXRNative.displayxr_standalone_get_surround_bridge_texture(
                    (uint)m_Width, (uint)m_Height,
                    out System.IntPtr ptr, out uint bw, out uint bh);
                if (ptr != System.IntPtr.Zero && bw > 0 && bh > 0)
                {
                    m_BridgeTex = Texture2D.CreateExternalTexture(
                        (int)bw, (int)bh, TextureFormat.RGBA32, false, true, ptr);
                    m_BridgeTex.name = "DisplayXR_SurroundBridge";
                }
            }
            catch (System.EntryPointNotFoundException) { }
        }

        private void ResolveResolution()
        {
            if (resolution.x > 0 && resolution.y > 0)
            {
                m_Width = resolution.x;
                m_Height = resolution.y;
                return;
            }
            // Use the runtime's display pixel dimensions.
            try
            {
                DisplayXRNative.displayxr_get_display_info(
                    out _, out _, out uint pw, out uint ph,
                    out _, out _, out _, out _, out _, out int isValid);
                if (isValid != 0 && pw > 0 && ph > 0)
                {
                    m_Width = (int)pw;
                    m_Height = (int)ph;
                    return;
                }
            }
            catch (System.EntryPointNotFoundException) { }
            m_Width = m_Height = 0;
        }

        private static void SetLayerRecursive(GameObject go, int layer)
        {
            go.layer = layer;
            foreach (Transform child in go.transform)
                SetLayerRecursive(child.gameObject, layer);
        }
    }
}
