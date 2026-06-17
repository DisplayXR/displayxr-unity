// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0

using UnityEngine;
using UnityEngine.Experimental.Rendering;

namespace DisplayXR
{
    /// <summary>
    /// Submits a Unity UI Canvas as an OpenXR XrCompositionLayerLocal2DEXT
    /// composition layer (#439/#491) — the modern, mask-based 2D-over-3D path.
    /// The runtime composites this "glass over 3D": the woven 3D under the
    /// layer's pixel rect goes flat 2D (implicit mask) and the Canvas content is
    /// alpha-composited on top. This is what the native VK avatar demo uses for
    /// its speech bubble, and it replaces the legacy 2D-surround handoff
    /// (<see cref="DisplayXRSurround"/>) for in-canvas 2D content — sidestepping
    /// the surround path's stale-RenderTexture-pointer fragility (the bubble
    /// vanishing when Unity reallocated the surround RT): this path recreates
    /// its overlay swapchain whenever the RT changes.
    ///
    /// Like <see cref="DisplayXRWindowSpaceUI"/> this takes over the Canvas it's
    /// attached to and drives it as a private WorldSpace canvas rendered by a
    /// dedicated orthographic camera into an offscreen RenderTexture. Give it its
    /// own Canvas GameObject.
    ///
    /// Position is authored in fractional window coords [0..1]; the component
    /// converts to the client-window pixel rect the Local2D layer requires using
    /// the live panel pixel size. Scoped to the hooked path (built apps); the
    /// editor/standalone-preview path is a follow-up (no cross-device bridge here).
    /// </summary>
    [AddComponentMenu("DisplayXR/Local 2D")]
    [RequireComponent(typeof(Canvas))]
    [ExecuteAlways]
    public class DisplayXRLocal2D : MonoBehaviour
    {
        [Header("Window Position (fractional 0..1)")]

        [Tooltip("Left edge as fraction of window width.")]
        [Range(0f, 1f)] public float positionX = 0.35f;

        [Tooltip("Top edge as fraction of window height.")]
        [Range(0f, 1f)] public float positionY = 0.05f;

        [Tooltip("Width as fraction of window width.")]
        [Range(0f, 1f)] public float width = 0.3f;

        [Tooltip("Height as fraction of window height.")]
        [Range(0f, 1f)] public float height = 0.18f;

        [Header("Render Settings")]

        [Tooltip("Resolution of the overlay RenderTexture.")]
        public Vector2Int resolution = new Vector2Int(1024, 512);

        /// <summary>The RenderTexture capturing the Canvas content.</summary>
        public RenderTexture OverlayTexture { get; private set; }

        private static readonly Vector3 kCanvasWorldPos = new Vector3(0, 100000f, 0);
        private const int kPrivateLayer = 30;

        private Canvas m_Canvas;
        private RectTransform m_CanvasRect;
        private Camera m_OverlayCamera;

        private RenderMode m_OrigRenderMode;
        private Vector3 m_OrigCanvasPos;
        private Quaternion m_OrigCanvasRot;
        private Vector3 m_OrigCanvasScale;
        private int m_OrigCanvasLayer;
        private Camera m_OrigCanvasWorldCamera;
        private bool m_StateSaved;

        private int m_LastRectX = int.MinValue, m_LastRectY, m_LastRectW, m_LastRectH;

        void OnEnable()
        {
            m_Canvas = GetComponent<Canvas>();
            m_CanvasRect = m_Canvas.GetComponent<RectTransform>();

            m_OrigRenderMode = m_Canvas.renderMode;
            m_OrigCanvasPos = m_CanvasRect.position;
            m_OrigCanvasRot = m_CanvasRect.rotation;
            m_OrigCanvasScale = m_CanvasRect.localScale;
            m_OrigCanvasLayer = gameObject.layer;
            m_OrigCanvasWorldCamera = m_Canvas.worldCamera;
            m_StateSaved = true;

            // RT with explicit depth-stencil (URP RenderGraph requirement),
            // BGRA8 to match the overlay swapchain picker default on Windows.
            var rtDesc = new RenderTextureDescriptor(resolution.x, resolution.y,
                GraphicsFormat.B8G8R8A8_UNorm, GraphicsFormat.D24_UNorm_S8_UInt)
            {
                msaaSamples = 1,
                useMipMap = false,
                autoGenerateMips = false,
            };
            OverlayTexture = new RenderTexture(rtDesc) { name = "DisplayXR_Local2D" };
            OverlayTexture.Create();

            // WorldSpace canvas parked far away on a private layer.
            m_Canvas.renderMode = RenderMode.WorldSpace;
            m_CanvasRect.position = kCanvasWorldPos;
            m_CanvasRect.rotation = Quaternion.identity;
            m_CanvasRect.localScale = new Vector3(0.01f, 0.01f, 0.01f);
            m_CanvasRect.sizeDelta = resolution;
            SetLayerRecursive(gameObject, kPrivateLayer);

            // Dedicated ortho camera. Flipped up-vector Y-flips the RT to match
            // the swapchain image's top-left origin (same trick as wsui).
            var camGO = new GameObject("DisplayXR_Local2DCam");
            camGO.transform.SetParent(transform, false);
            camGO.hideFlags = HideFlags.HideAndDontSave;
            camGO.transform.position = kCanvasWorldPos + new Vector3(0, 0, 1);
            camGO.transform.rotation = Quaternion.LookRotation(Vector3.back, Vector3.down);

            m_OverlayCamera = camGO.AddComponent<Camera>();
            m_OverlayCamera.clearFlags = CameraClearFlags.SolidColor;
            m_OverlayCamera.backgroundColor = Color.clear;
            m_OverlayCamera.orthographic = true;
            m_OverlayCamera.orthographicSize = resolution.y * 0.005f;
            m_OverlayCamera.aspect = (float)resolution.x / resolution.y;
            m_OverlayCamera.nearClipPlane = 0.01f;
            m_OverlayCamera.farClipPlane = 10f;
            m_OverlayCamera.targetTexture = OverlayTexture;
            m_OverlayCamera.cullingMask = 1 << kPrivateLayer;
            m_OverlayCamera.depth = -1000;
            m_OverlayCamera.enabled = false;

            m_Canvas.worldCamera = m_OverlayCamera;

            DisplayXRNative.displayxr_local2d_set_texture(
                OverlayTexture.GetNativeTexturePtr(), resolution.x, resolution.y);

            Debug.Log($"[DisplayXR] Local2D enabled: {resolution.x}x{resolution.y}");
        }

        void OnDisable()
        {
            DisplayXRNative.displayxr_local2d_clear();

            if (m_StateSaved && m_Canvas != null)
            {
                m_Canvas.renderMode = m_OrigRenderMode;
                m_Canvas.worldCamera = m_OrigCanvasWorldCamera;
                if (m_CanvasRect != null)
                {
                    m_CanvasRect.position = m_OrigCanvasPos;
                    m_CanvasRect.rotation = m_OrigCanvasRot;
                    m_CanvasRect.localScale = m_OrigCanvasScale;
                }
                SetLayerRecursive(gameObject, m_OrigCanvasLayer);
                m_StateSaved = false;
            }

            if (m_OverlayCamera != null)
            {
                if (Application.isPlaying) Destroy(m_OverlayCamera.gameObject);
                else DestroyImmediate(m_OverlayCamera.gameObject);
            }

            if (OverlayTexture != null)
            {
                OverlayTexture.Release();
                if (Application.isPlaying) Destroy(OverlayTexture);
                else DestroyImmediate(OverlayTexture);
                OverlayTexture = null;
            }
        }

        void LateUpdate()
        {
            if (m_OverlayCamera == null || OverlayTexture == null) return;

            // Keep the camera aspect at the live pixel-rect aspect so the bubble
            // isn't stretched when the panel resizes.
            if (TryGetPanelPixelSize(out int px, out int py, out int pw, out int ph) &&
                pw > 0 && ph > 0)
            {
                float aspect = (float)pw / ph;
                m_OverlayCamera.orthographicSize = resolution.y * 0.005f;
                m_OverlayCamera.aspect = aspect;
                if (m_CanvasRect != null)
                    m_CanvasRect.sizeDelta = new Vector2(resolution.y * aspect, resolution.y);

                if (px != m_LastRectX || py != m_LastRectY ||
                    pw != m_LastRectW || ph != m_LastRectH)
                {
                    DisplayXRNative.displayxr_local2d_set_rect(px, py, pw, ph);
                    m_LastRectX = px; m_LastRectY = py;
                    m_LastRectW = pw; m_LastRectH = ph;
                }
            }

            m_OverlayCamera.Render();
        }

        // Convert the fractional rect to client-window pixels using the live
        // panel size (built-app: the runtime composites into Unity's window, so
        // Screen.* is the panel).
        private bool TryGetPanelPixelSize(out int x, out int y, out int w, out int h)
        {
            x = y = w = h = 0;
            if (Screen.width <= 0 || Screen.height <= 0) return false;
            float sw = Screen.width, sh = Screen.height;
            x = Mathf.RoundToInt(Mathf.Clamp01(positionX) * sw);
            y = Mathf.RoundToInt(Mathf.Clamp01(positionY) * sh);
            w = Mathf.RoundToInt(Mathf.Clamp01(width) * sw);
            h = Mathf.RoundToInt(Mathf.Clamp01(height) * sh);
            return w > 0 && h > 0;
        }

        private static void SetLayerRecursive(GameObject go, int layer)
        {
            go.layer = layer;
            foreach (Transform child in go.transform)
                SetLayerRecursive(child.gameObject, layer);
        }
    }
}
