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
        [Tooltip("Measured: the runtime measures the depth under the cursor from the two " +
                 "rendered views - any content, no colliders, no code (runtime spec v7). " +
                 "AppPoint: raycast 3D physics colliders along the cursor footprint, or feed " +
                 "DisplayXRCursorDepth.SetHitPoint every frame (runtime spec v5; needs a " +
                 "DisplayXRDisplay / DisplayXRCamera rig).")]
        public DisplayXRCursorDepth.DepthSource source = DisplayXRCursorDepth.DepthSource.Measured;

        [Tooltip("How far in front of the content the cursor floats, in eye baselines. " +
                 "0 = the runtime default (0.015), shared by every DisplayXR app.")]
        [Min(0f)]
        public float margin = 0f;

        [Tooltip("Hide Cursor.visible while the runtime draws its own cursor.")]
        public bool manageOsCursor = true;

        [Header("AppPoint")]
        [Tooltip("Raycast 3D physics colliders along the cursor footprint when nothing calls " +
                 "SetHitPoint / sets CustomHitTest.")]
        public bool usePhysicsRaycast = true;

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
        }

        // Live-apply inspector edits (cheap: field copies; the native side is told only
        // when something actually changed).
        void Update() => Apply();

        void Apply()
        {
            DisplayXRCursorDepth.Source = source;
            DisplayXRCursorDepth.Margin = margin;
            DisplayXRCursorDepth.ManageOsCursor = manageOsCursor;
            DisplayXRCursorDepth.UsePhysicsRaycast = usePhysicsRaycast;
            DisplayXRCursorDepth.RaycastLayerMask = raycastLayers;
            DisplayXRCursorDepth.RaycastMaxDistance = raycastMaxDistance;
        }
    }
}
