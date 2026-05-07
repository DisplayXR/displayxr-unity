// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0

using UnityEngine;
using UnityEngine.Experimental.Rendering;

namespace DisplayXR
{
    /// <summary>
    /// Submits a Unity UI Canvas as an OpenXR XrCompositionLayerWindowSpaceEXT
    /// composition layer.
    ///
    /// IMPORTANT API: this component takes over the Canvas it's attached to and
    /// drives it as a private WorldSpace canvas. Don't share the Canvas with
    /// other UI logic that expects ScreenSpaceOverlay/Camera modes — give wsui
    /// its own Canvas GameObject. The canvas's child UI elements ARE rendered
    /// into the layer; everything else is preserved.
    ///
    /// The overlay is composited per-eye with horizontal disparity, rendered
    /// pre-interlace by the runtime's display processor.
    ///
    /// Why WorldSpace and not ScreenSpaceCamera? URP's RenderGraph has multiple
    /// failure modes when a Canvas in ScreenSpaceCamera mode targets a camera
    /// whose targetTexture is an offscreen RT (canvas-camera coupling races,
    /// Canvas-position-depends-on-camera-position cycles when the camera is
    /// parented under the canvas, RenderGraph dropping offscreen-only cameras
    /// from its loop). WorldSpace + dedicated camera with targetTexture is the
    /// canonical Unity recipe for "render UI to a texture" and works
    /// identically across BiRP / URP / HDRP.
    /// </summary>
    [AddComponentMenu("DisplayXR/Window Space UI")]
    [RequireComponent(typeof(Canvas))]
    [ExecuteAlways]
    public class DisplayXRWindowSpaceUI : MonoBehaviour
    {
        [Header("Window Position (fractional 0..1)")]

        [Tooltip("Left edge position as fraction of window width.")]
        [Range(0f, 1f)]
        public float positionX = 0.02f;

        [Tooltip("Top edge position as fraction of window height.")]
        [Range(0f, 1f)]
        public float positionY = 0.02f;

        [Tooltip("Width as fraction of window width.")]
        [Range(0f, 1f)]
        public float width = 0.3f;

        [Tooltip("Height as fraction of window height.")]
        [Range(0f, 1f)]
        public float height = 0.15f;

        [Header("Depth")]

        [Tooltip("Horizontal shift for stereo depth. 0 = at screen plane, " +
                 "positive = in front, negative = behind.")]
        [Range(-0.05f, 0.05f)]
        public float disparity;

        [Header("Render Settings")]

        [Tooltip("Resolution of the overlay RenderTexture.")]
        public Vector2Int resolution = new Vector2Int(1024, 1024);

        /// <summary>The RenderTexture used to capture the Canvas content.</summary>
        public RenderTexture OverlayTexture { get; private set; }

        // We park the WorldSpace canvas at this fixed position, far from any
        // scene content, so the dedicated camera looking at it sees nothing
        // else that might bleed into our RT.
        private static readonly Vector3 kCanvasWorldPos = new Vector3(0, 100000f, 0);
        // Dedicated layer: we put the canvas + children on this layer and give
        // ONLY our overlay camera that layer in its cullingMask. We pick a
        // mid-range layer that's typically unused (Unity reserves 0-7 for
        // built-ins; 8+ are user layers; 31 is sometimes used for ignore-raycast).
        // 30 has the lowest collision risk in practice — rarely used by host apps.
        private const int kPrivateLayer = 30;

        private Canvas m_Canvas;
        private RectTransform m_CanvasRect;
        private Camera m_OverlayCamera;

        // Saved state, restored in OnDisable.
        private RenderMode m_OrigRenderMode;
        private Vector3 m_OrigCanvasPos;
        private Quaternion m_OrigCanvasRot;
        private Vector3 m_OrigCanvasScale;
        private int m_OrigCanvasLayer;
        private Camera m_OrigCanvasWorldCamera;
        private bool m_StateSaved;

        private float m_LastX, m_LastY, m_LastW, m_LastH, m_LastDisparity;

        void OnEnable()
        {
            m_Canvas = GetComponent<Canvas>();
            m_CanvasRect = m_Canvas.GetComponent<RectTransform>();

            // Save state for restoration in OnDisable.
            m_OrigRenderMode = m_Canvas.renderMode;
            m_OrigCanvasPos = m_CanvasRect.position;
            m_OrigCanvasRot = m_CanvasRect.rotation;
            m_OrigCanvasScale = m_CanvasRect.localScale;
            m_OrigCanvasLayer = gameObject.layer;
            m_OrigCanvasWorldCamera = m_Canvas.worldCamera;
            m_StateSaved = true;

            // ---- RT with explicit depth-stencil format (URP RenderGraph requirement) ----
            var rtDesc = new RenderTextureDescriptor(resolution.x, resolution.y,
                GraphicsFormat.B8G8R8A8_UNorm, GraphicsFormat.D24_UNorm_S8_UInt)
            {
                msaaSamples = 1,
                useMipMap = false,
                autoGenerateMips = false,
            };
            OverlayTexture = new RenderTexture(rtDesc) { name = "DisplayXR_Overlay" };
            OverlayTexture.Create();

            // ---- Switch the canvas to WorldSpace + park it on the private layer ----
            m_Canvas.renderMode = RenderMode.WorldSpace;
            // worldCamera is assigned to the OverlayCamera below (after creation)
            // so GraphicRaycaster can project screen-cursor input onto the canvas.
            m_CanvasRect.position = kCanvasWorldPos;
            m_CanvasRect.rotation = Quaternion.identity;
            // Use the canvas's existing reference width as the scale baseline.
            // 1 world unit per UI unit at scale 1 → set scale so the RT
            // resolution maps cleanly. Use 1/100 like a typical WorldSpace UI
            // setup (1 cm per UI unit).
            m_CanvasRect.localScale = new Vector3(0.01f, 0.01f, 0.01f);
            // Canvas size in UI units = resolution (so 1 RT pixel = 1 UI unit).
            m_CanvasRect.sizeDelta = resolution;
            SetLayerRecursive(gameObject, kPrivateLayer);

            // ---- Dedicated camera: orthographic, points at canvas, renders ONLY the private layer ----
            var camGO = new GameObject("DisplayXR_OverlayCam");
            camGO.transform.SetParent(transform, false);
            camGO.hideFlags = HideFlags.HideAndDontSave;
            // Canvas plane normal is +Z; camera sits at +Z, looks toward -Z
            // to see the front face of the canvas. We deliberately invert
            // the camera's up vector so the rendered RT is Y-flipped at
            // capture time — this compensates for the bottom-left ↔ top-left
            // origin convention mismatch between Unity's render target
            // pipeline and the swapchain image our native blit feeds the
            // runtime compositor with. Without this the panel reads
            // upside-down in the runtime preview window.
            camGO.transform.position = kCanvasWorldPos + new Vector3(0, 0, 1);
            camGO.transform.rotation = Quaternion.LookRotation(Vector3.back, Vector3.down);

            m_OverlayCamera = camGO.AddComponent<Camera>();
            m_OverlayCamera.clearFlags = CameraClearFlags.SolidColor;
            m_OverlayCamera.backgroundColor = Color.clear;
            m_OverlayCamera.orthographic = true;
            // Ortho size = half-height in world units. Canvas height = resolution.y * 0.01
            // (because of localScale=0.01). So ortho size = resolution.y * 0.005 covers the canvas exactly.
            m_OverlayCamera.orthographicSize = resolution.y * 0.005f;
            m_OverlayCamera.aspect = (float)resolution.x / resolution.y;
            m_OverlayCamera.nearClipPlane = 0.01f;
            m_OverlayCamera.farClipPlane = 10f;
            m_OverlayCamera.targetTexture = OverlayTexture;
            m_OverlayCamera.cullingMask = 1 << kPrivateLayer;
            m_OverlayCamera.depth = -1000;
            // Disable URP auto-render. We Render() manually each frame in
            // LateUpdate — works in BiRP/URP/HDRP and bypasses any
            // pipeline-specific scheduling quirks for offscreen-only cameras.
            m_OverlayCamera.enabled = false;

            // Wire OverlayCamera as the canvas's event camera. GraphicRaycaster
            // needs a camera reference to project screen-cursor input onto a
            // WorldSpace canvas — without it, raycasts return empty hits and
            // app-side input routers (e.g. DisplayXRWsuiMouseRouter) can't drive
            // sliders/buttons. Using OverlayCamera (which already views the
            // canvas full-frame) means cursor coords passed in RT-pixel space
            // map 1:1 to the rendered layout. The camera's flipped up-vector
            // (down) flips the projection's Y to match the runtime's top-left
            // texture origin, so callers should pass a Y-flipped cursor coord
            // to PointerEventData.position to compensate.
            m_Canvas.worldCamera = m_OverlayCamera;

            // ---- Tell native about our texture + initial layer descriptor ----
            DisplayXRNative.displayxr_window_space_ui_set_layer(
                positionX, positionY, width, height, disparity);
            DisplayXRNative.displayxr_window_space_ui_set_texture(
                OverlayTexture.GetNativeTexturePtr(), resolution.x, resolution.y);

            m_LastX = positionX; m_LastY = positionY;
            m_LastW = width;     m_LastH = height;
            m_LastDisparity = disparity;

            Debug.Log($"[DisplayXR] WindowSpaceUI enabled: {resolution.x}x{resolution.y} " +
                      $"(WorldSpace canvas, layer {kPrivateLayer}, dedicated camera)");
        }

        void OnDisable()
        {
            DisplayXRNative.displayxr_window_space_ui_clear();

            // Restore the canvas's original mode + transform + layer.
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
                if (Application.isPlaying)
                    Destroy(m_OverlayCamera.gameObject);
                else
                    DestroyImmediate(m_OverlayCamera.gameObject);
            }

            if (OverlayTexture != null)
            {
                OverlayTexture.Release();
                if (Application.isPlaying)
                    Destroy(OverlayTexture);
                else
                    DestroyImmediate(OverlayTexture);
                OverlayTexture = null;
            }
        }

        void LateUpdate()
        {
            // Manually render the overlay camera into our RT. URP/HDRP only.
            // (BiRP would auto-render via its loop, but Render() is also
            // perfectly valid there — just slightly redundant.)
            if (m_OverlayCamera != null && OverlayTexture != null)
            {
                m_OverlayCamera.Render();
            }

            if (positionX != m_LastX || positionY != m_LastY ||
                width != m_LastW || height != m_LastH ||
                disparity != m_LastDisparity)
            {
                DisplayXRNative.displayxr_window_space_ui_set_layer(
                    positionX, positionY, width, height, disparity);
                m_LastX = positionX; m_LastY = positionY;
                m_LastW = width;     m_LastH = height;
                m_LastDisparity = disparity;
            }
        }

        private static void SetLayerRecursive(GameObject go, int layer)
        {
            go.layer = layer;
            foreach (Transform child in go.transform)
                SetLayerRecursive(child.gameObject, layer);
        }
    }
}
