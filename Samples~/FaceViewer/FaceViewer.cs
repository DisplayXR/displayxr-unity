// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

using UnityEngine;

namespace DisplayXR
{
    /// <summary>
    /// Billboard component: rotates this GameObject so it always faces the
    /// tracked viewer's head. The head position comes from
    /// <see cref="DisplayXRProvider.TryGetViewerHead"/> — the midpoint of the
    /// render-ready per-eye poses the display provider is drawing with — so the
    /// object tracks the real viewer as they move in front of the display (not
    /// just the camera transform).
    ///
    /// Falls back to the active camera's transform position when no viewer data
    /// is available (provider not running, viewer not tracked yet), so the
    /// object still behaves sensibly in 2D mode and plain Play Mode.
    ///
    /// Attach to any world-space object (label quad, avatar, UI panel).
    /// Works in Play Mode and built players.
    /// </summary>
    [AddComponentMenu("DisplayXR/Face Viewer (Billboard)")]
    [DefaultExecutionOrder(100)] // after the rig components' LateUpdate
    public class FaceViewer : MonoBehaviour
    {
        [Tooltip("Only rotate around the world Y axis (upright billboard). " +
                 "Good for labels and standing avatars; disable for full " +
                 "3-axis facing (e.g. floating panels).")]
        public bool yawOnly = false;

        [Tooltip("Turn rate in degrees per second. 0 = snap instantly.")]
        public float turnSpeed = 0f;

        [Tooltip("When true, the object's +Z axis points AWAY from the viewer " +
                 "(correct for Unity UI, TextMesh and Quad primitives, whose " +
                 "visible face looks down -Z). When false, +Z points toward " +
                 "the viewer.")]
        public bool forwardAwayFromViewer = true;

        void LateUpdate()
        {
            var cam = DisplayXRRigManager.ActiveCamera;
            if (cam == null) cam = Camera.main;
            if (cam == null) return;

            if (!TryGetViewerHead(cam, out Vector3 head))
                head = cam.transform.position;

            Vector3 dir = forwardAwayFromViewer
                ? transform.position - head
                : head - transform.position;
            if (yawOnly) dir.y = 0f;
            if (dir.sqrMagnitude < 1e-8f) return;

            Quaternion target = Quaternion.LookRotation(dir.normalized, Vector3.up);
            transform.rotation = turnSpeed <= 0f
                ? target
                : Quaternion.RotateTowards(transform.rotation, target,
                                           turnSpeed * Time.deltaTime);
        }

        /// <summary>
        /// World-space viewer head position (midpoint of the two rendered eyes).
        /// Returns false when the provider isn't running or hasn't delivered real
        /// per-eye views yet (viewer not tracked, 2D mode) — the caller should
        /// fall back to the camera transform.
        ///
        /// <paramref name="cam"/> is unused and kept for source compatibility
        /// with the v2.8.3 signature.
        ///
        /// Do NOT reach for <c>cam.GetStereoViewMatrix()</c> here: Unity's C#-side
        /// stereo matrix cache is only written by <c>SetStereoViewMatrix()</c>,
        /// which the plugin doesn't call in provider mode — it returns the same
        /// mono matrix for both eyes, so the head never moves (#236). The
        /// provider's own per-eye data is the source of truth.
        /// </summary>
        public static bool TryGetViewerHead(Camera cam, out Vector3 head) =>
            DisplayXRProvider.TryGetViewerHead(out head);
    }
}
