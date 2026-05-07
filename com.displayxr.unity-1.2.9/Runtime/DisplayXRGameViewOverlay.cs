// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0

using System.Collections.Generic;
using UnityEngine;

namespace DisplayXR
{
    /// <summary>
    /// Renders the DisplayXR eye tile atlas in the Game View during Play Mode.
    /// Suppresses scene camera rendering while the standalone preview is running
    /// and draws the atlas via OnGUI. Only one instance needed in the scene.
    /// </summary>
    [AddComponentMenu("DisplayXR/Game View Overlay")]
    public class DisplayXRGameViewOverlay : MonoBehaviour
    {
        /// <summary>
        /// Set by DisplayXRPreviewSession (Editor) when the SA session starts/stops.
        /// </summary>
        public static RenderTexture AtlasTexture { get; set; }

        private struct SuppressedCamera
        {
            public Camera camera;
            public int originalCullingMask;
            public CameraClearFlags originalClearFlags;
            public Color originalBackgroundColor;
        }

        private List<SuppressedCamera> m_SuppressedCameras = new List<SuppressedCamera>();
        private bool m_Suppressing;

        void OnDisable()
        {
            RestoreAllCameras();
        }

        void Update()
        {
            bool running = DisplayXRNative.displayxr_standalone_is_running() != 0;

            if (running && !m_Suppressing)
            {
                SuppressSceneRendering();
                m_Suppressing = true;
            }
            else if (!running && m_Suppressing)
            {
                RestoreAllCameras();
                m_Suppressing = false;
            }
        }

        void OnGUI()
        {
            if (DisplayXRNative.displayxr_standalone_is_running() == 0)
                return;

            RenderTexture atlas = AtlasTexture;
            if (atlas == null) return;

            // Get current mode tiling — the actual rendered content region
            DisplayXRNative.displayxr_standalone_get_current_mode_info(
                out uint vc, out uint tc, out uint tr,
                out uint vw, out uint vh,
                out float _, out float _, out int _);

            // Content region = tiles * per-view size (may be smaller than atlas)
            uint contentW = tc * vw;
            uint contentH = tr * vh;
            if (contentW == 0 || contentH == 0)
            {
                contentW = (uint)atlas.width;
                contentH = (uint)atlas.height;
            }

            // UV crop: show only the rendered content, not the full atlas
            float uMax = (float)contentW / atlas.width;
            float vMax = (float)contentH / atlas.height;

            // Letterbox in screen rect
            Rect screenRect = new Rect(0, 0, Screen.width, Screen.height);
            float contentAspect = (float)contentW / contentH;
            float screenAspect = screenRect.width / screenRect.height;

            Rect drawRect;
            if (screenAspect > contentAspect)
            {
                float w = screenRect.height * contentAspect;
                drawRect = new Rect((screenRect.width - w) * 0.5f, 0, w, screenRect.height);
            }
            else
            {
                float h = screenRect.width / contentAspect;
                drawRect = new Rect(0, (screenRect.height - h) * 0.5f, screenRect.width, h);
            }

            // Projection Y is flipped during scene rendering (both platforms) so
            // the atlas RT is Y-inverted. Flip UV.y to display correctly.
            GUI.DrawTextureWithTexCoords(drawRect, atlas, new Rect(0, vMax, uMax, -vMax));

            // Tile grid overlay
            if (tc > 0 && tr > 0)
                DrawTileGrid(drawRect, tc, tr, vw, vh, contentW, contentH);

            // Diagnostic label
            string camName = DisplayXRRigManager.ActiveCameraName ?? "—";
            GUI.Label(new Rect(drawRect.x + 4, drawRect.y + 4, drawRect.width - 8, 20),
                $"Content: {contentW}x{contentH}  Tiles: {tc}x{tr}  View: {vw}x{vh}  Cam: {camName}",
                GUI.skin.label);
        }

        // ================================================================
        // Camera suppression
        // ================================================================

        private void SuppressSceneRendering()
        {
            m_SuppressedCameras.Clear();
            foreach (var cam in Camera.allCameras)
            {
                if (cam == null) continue;
                // Skip our own hidden render rig cameras
                if (cam.gameObject.hideFlags.HasFlag(HideFlags.HideAndDontSave))
                    continue;

                m_SuppressedCameras.Add(new SuppressedCamera
                {
                    camera = cam,
                    originalCullingMask = cam.cullingMask,
                    originalClearFlags = cam.clearFlags,
                    originalBackgroundColor = cam.backgroundColor,
                });
                cam.cullingMask = 0;
                cam.clearFlags = CameraClearFlags.SolidColor;
                cam.backgroundColor = Color.black;
            }
        }

        private void RestoreAllCameras()
        {
            for (int i = 0; i < m_SuppressedCameras.Count; i++)
            {
                var mc = m_SuppressedCameras[i];
                if (mc.camera == null) continue;
                mc.camera.cullingMask = mc.originalCullingMask;
                mc.camera.clearFlags = mc.originalClearFlags;
                mc.camera.backgroundColor = mc.originalBackgroundColor;
            }
            m_SuppressedCameras.Clear();
        }

        // ================================================================
        // Tile grid overlay
        // ================================================================

        private static void DrawTileGrid(Rect drawRect, uint tileCols, uint tileRows,
            uint viewW, uint viewH, uint contentW, uint contentH)
        {
            if (contentW == 0 || contentH == 0) return;

            // Use GUI for runtime (Handles is editor-only)
            Color prev = GUI.color;
            GUI.color = new Color(1f, 1f, 0f, 0.6f);
            Texture2D pixel = Texture2D.whiteTexture;

            // Vertical lines between tile columns
            for (uint c = 1; c < tileCols; c++)
            {
                float xFrac = (float)(c * viewW) / contentW;
                float x = drawRect.x + xFrac * drawRect.width;
                GUI.DrawTexture(new Rect(x - 0.5f, drawRect.y, 1, drawRect.height), pixel);
            }

            // Horizontal lines between tile rows
            for (uint r = 1; r < tileRows; r++)
            {
                float yFrac = (float)(r * viewH) / contentH;
                float y = drawRect.y + yFrac * drawRect.height;
                GUI.DrawTexture(new Rect(drawRect.x, y - 0.5f, drawRect.width, 1), pixel);
            }

            // Tile labels
            for (uint r = 0; r < tileRows; r++)
            {
                for (uint c = 0; c < tileCols; c++)
                {
                    uint viewIdx = r * tileCols + c;
                    float xFrac = (float)(c * viewW) / contentW;
                    float yFrac = (float)(r * viewH) / contentH;
                    float x = drawRect.x + xFrac * drawRect.width + 4;
                    float y = drawRect.y + yFrac * drawRect.height + 20;
                    GUI.Label(new Rect(x, y, 80, 16), $"Eye {viewIdx}");
                }
            }

            GUI.color = prev;
        }
    }
}
