// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0

using System.Runtime.InteropServices;

namespace DisplayXR
{
    /// <summary>
    /// Stereo rig tunable parameters passed to the native plugin.
    /// These modify how xrLocateViews output is transformed before Kooima projection.
    /// </summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct DisplayXRTunables
    {
        /// <summary>
        /// Scales inter-eye distance. 1.0 = natural, &lt;1 = reduced stereo, &gt;1 = exaggerated.
        /// </summary>
        public float ipdFactor;

        /// <summary>
        /// Scales eye X/Y offset from display center. 1.0 = natural parallax.
        /// </summary>
        public float parallaxFactor;

        /// <summary>
        /// Scales eye Z only (depth intensity without changing baseline). 1.0 = natural.
        /// </summary>
        public float perspectiveFactor;

        /// <summary>
        /// Virtual display height in meters. 0 = use physical display height.
        /// </summary>
        public float virtualDisplayHeight;

        /// <summary>
        /// Inverse convergence distance (1/meters). 0 = infinity (parallel projection).
        /// Higher values = screen plane closer to camera. Used by camera-centric mode.
        /// </summary>
        public float invConvergenceDistance;

        /// <summary>
        /// Override FOV in radians. 0 = compute from display geometry.
        /// Used by camera-centric mode.
        /// </summary>
        public float fovOverride;

        /// <summary>
        /// Near clip plane distance in meters. Inherited from Camera.nearClipPlane.
        /// </summary>
        public float nearZ;

        /// <summary>
        /// Far clip plane distance in meters. Inherited from Camera.farClipPlane.
        /// </summary>
        public float farZ;

        /// <summary>
        /// Whether to use camera-centric mode parameters (convergenceDistance, fovOverride).
        /// </summary>
        [MarshalAs(UnmanagedType.U1)]
        public bool cameraCentricMode;

        /// <summary>Returns default tunables with all factors at 1.0 (natural).</summary>
        public static DisplayXRTunables Default => new DisplayXRTunables
        {
            ipdFactor = 1.0f,
            parallaxFactor = 1.0f,
            perspectiveFactor = 1.0f,
            virtualDisplayHeight = 0.0f,
            invConvergenceDistance = 0.0f,
            fovOverride = 0.0f,
            nearZ = 0.3f,
            farZ = 1000.0f,
            cameraCentricMode = false,
        };
    }
}
