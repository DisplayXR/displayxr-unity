// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

using UnityEngine;

namespace DisplayXR
{
    /// <summary>
    /// Drop-in switch for the DisplayXR <b>depth-aware cursor</b>: while this component is
    /// enabled, the mouse cursor lifts off the glass over content that pops out of the
    /// display, drawn in stereo by the runtime; elsewhere it is the plain OS cursor. Put it on
    /// any GameObject (one per scene). The settings mirror <see cref="DisplayXRCursorDepth"/>,
    /// which code can drive directly instead.
    /// </summary>
    [AddComponentMenu("DisplayXR/DisplayXR Depth Cursor")]
    [DisallowMultipleComponent]
    public class DisplayXRDepthCursor : MonoBehaviour
    {
        [Tooltip("Auto (default): Unity's scene depth under the cursor when the render pipeline " +
                 "exposes it, else the runtime's measured disparity. Depth: scene depth only " +
                 "(URP: the camera Depth Texture is switched on while this is enabled; BiRP " +
                 "forward needs the camera to already render depth). Measured: the runtime " +
                 "matches the two rendered views (runtime spec v7). Physics: raycast 3D " +
                 "colliders along the cursor footprint.")]
        public DisplayXRCursorDepth.DepthSource source = DisplayXRCursorDepth.DepthSource.Auto;

        [Tooltip("How far in front of the content the cursor floats, in eye baselines. " +
                 "0 = the runtime default (0.015), shared by every DisplayXR app.")]
        [Min(0f)]
        public float margin = 0f;

        [Tooltip("Hide Cursor.visible while the runtime draws its own cursor.")]
        public bool manageOsCursor = true;

        [Header("Custom sprite (optional)")]
        [Tooltip("Your own cursor art for the LIFTED cursor (runtime spec v5). Empty = the default " +
                 "look (a copy of the OS cursor on Windows/macOS). Any format; straight alpha; up " +
                 "to 1024 px a side. On the glass the OS cursor shows - set it too, e.g. with " +
                 "DisplayXRCursorDepth.SetCursor.")]
        public Texture2D sprite;

        [Tooltip("The click point in sprite pixels from the TOP-left corner (Cursor.SetCursor's convention).")]
        public Vector2 spriteHotspot = Vector2.zero;

        [Tooltip("Sprite height on the panel, metres. 0 = the runtime default (the OS cursor's " +
                 "size); negative = the texture's own pixel height on the panel.")]
        public float spriteHeightMeters = 0f;

        [Header("Physics source")]
        [Tooltip("Layers the footprint raycast considers.")]
        public LayerMask raycastLayers = ~(1 << 2);

        [Tooltip("Maximum footprint raycast distance (world units).")]
        public float raycastMaxDistance = 1000f;

        void OnEnable()
        {
            Apply();
            DisplayXRCursorDepth.Enabled = true;
        }

        void OnDisable()
        {
            DisplayXRCursorDepth.Enabled = false;
            if (m_AppliedSprite != null) DisplayXRCursorDepth.ClearSprite();
            m_AppliedSprite = null;
        }

        Texture2D m_AppliedSprite;
        Vector2 m_AppliedHotspot;
        float m_AppliedHeight;

        // Live-apply inspector edits (cheap: field copies; the native side is told only
        // when something actually changed).
        void Update() => Apply();

        void Apply()
        {
            DisplayXRCursorDepth.Source = source;
            DisplayXRCursorDepth.Margin = margin;
            DisplayXRCursorDepth.ManageOsCursor = manageOsCursor;
            DisplayXRCursorDepth.RaycastLayerMask = raycastLayers;
            DisplayXRCursorDepth.RaycastMaxDistance = raycastMaxDistance;

            // The sprite: pushed only when one of its fields changed (SetSprite re-uploads).
            if (sprite != m_AppliedSprite || spriteHotspot != m_AppliedHotspot || spriteHeightMeters != m_AppliedHeight)
            {
                if (sprite != null) DisplayXRCursorDepth.SetSprite(sprite, spriteHotspot, spriteHeightMeters);
                else if (m_AppliedSprite != null) DisplayXRCursorDepth.ClearSprite();
                m_AppliedSprite = sprite;
                m_AppliedHotspot = spriteHotspot;
                m_AppliedHeight = spriteHeightMeters;
            }
        }
    }
}
