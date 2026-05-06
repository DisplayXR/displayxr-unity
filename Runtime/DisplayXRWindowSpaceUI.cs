// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0

using UnityEngine;

namespace DisplayXR
{
    /// <summary>
    /// Routes a Canvas to a window-space composition layer for 2D UI overlay.
    /// The Canvas content is rendered to a RenderTexture, then submitted as an
    /// XrCompositionLayerWindowSpaceEXT via the native xrEndFrame interceptor.
    ///
    /// The overlay is composited to both eyes with per-eye horizontal shift (disparity),
    /// rendered pre-interlace by the runtime's display processor.
    /// </summary>
    [AddComponentMenu("DisplayXR/Window Space UI")]
    [RequireComponent(typeof(Canvas))]
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
        public Vector2Int resolution = new Vector2Int(512, 256);

        /// <summary>The RenderTexture used to capture the Canvas content.</summary>
        public RenderTexture OverlayTexture { get; private set; }

        private Canvas m_Canvas;
        private Camera m_OverlayCamera;

        // Cached previous values so LateUpdate only re-pushes when something changed.
        private float m_LastX, m_LastY, m_LastW, m_LastH, m_LastDisparity;


        void OnEnable()
        {
            m_Canvas = GetComponent<Canvas>();

            // Create overlay render texture
            OverlayTexture = new RenderTexture(resolution.x, resolution.y, 0,
                RenderTextureFormat.ARGB32)
            {
                name = "DisplayXR_Overlay",
                useMipMap = false,
                autoGenerateMips = false,
            };
            OverlayTexture.Create();

            // Create a dedicated camera for rendering the Canvas
            var camGO = new GameObject("DisplayXR_OverlayCam");
            camGO.transform.SetParent(transform, false);
            camGO.hideFlags = HideFlags.HideAndDontSave;

            m_OverlayCamera = camGO.AddComponent<Camera>();
            m_OverlayCamera.clearFlags = CameraClearFlags.SolidColor;
            m_OverlayCamera.backgroundColor = Color.clear;
            m_OverlayCamera.orthographic = true;
            m_OverlayCamera.orthographicSize = resolution.y * 0.5f;
            m_OverlayCamera.nearClipPlane = 0.1f;
            m_OverlayCamera.farClipPlane = 100f;
            m_OverlayCamera.targetTexture = OverlayTexture;
            m_OverlayCamera.depth = -100; // Render before main camera
            m_OverlayCamera.cullingMask = 1 << gameObject.layer;

            // Set Canvas to render through our overlay camera
            m_Canvas.renderMode = RenderMode.ScreenSpaceCamera;
            m_Canvas.worldCamera = m_OverlayCamera;

            // Push initial layer descriptor + texture pointer to native.
            DisplayXRNative.displayxr_window_space_ui_set_layer(
                positionX, positionY, width, height, disparity);
            DisplayXRNative.displayxr_window_space_ui_set_texture(
                OverlayTexture.GetNativeTexturePtr(), resolution.x, resolution.y);

            m_LastX = positionX; m_LastY = positionY;
            m_LastW = width;     m_LastH = height;
            m_LastDisparity = disparity;

            Debug.Log($"[DisplayXR] WindowSpaceUI enabled: {resolution.x}x{resolution.y}, " +
                      $"pos=({positionX},{positionY}), size=({width},{height})");
        }

        void OnDisable()
        {
            DisplayXRNative.displayxr_window_space_ui_clear();

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
    }
}
