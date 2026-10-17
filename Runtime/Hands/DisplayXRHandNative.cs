// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

using System.Runtime.InteropServices;

namespace DisplayXR.Hands
{
    /// <summary>
    /// P/Invoke bindings to the provider's <c>XR_EXT_hand_tracking</c> binding
    /// (<c>native~/displayxr_xrprovider/displayxr_provider_hands.{h,cpp}</c>). The native
    /// side owns the <c>XrHandTrackerEXT</c> handles next to the session and locates the
    /// joints in the same reference space, at the same predicted display time, as the
    /// views — this class only moves 26 poses across the boundary.
    /// </summary>
    internal static class DisplayXRHandNative
    {
        const string LibName = "displayxr_unity";

        /// <summary>Joints per hand (<c>XR_HAND_JOINT_COUNT_EXT</c>); the array lengths below.</summary>
        public const int JointCount = 26;

        /// <summary>Floats per joint in <c>outPoses</c>: px, py, pz, qx, qy, qz, qw.</summary>
        public const int PoseStride = 7;

        /// <summary>
        /// 1 when a session is up, the extension is enabled and the system reports
        /// <c>supportsHandTracking</c>; 0 otherwise. Poll it — the session comes up on the
        /// graphics thread a frame or two into Play.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int DisplayXR_HandTracking_IsSupported();

        /// <summary>Request hand tracking (trackers created now or when a session is up). Remembered across restarts.</summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int DisplayXR_HandTracking_Start();

        /// <summary>Release the trackers; <see cref="DisplayXR_HandTracking_Locate"/> returns 0 until the next Start.</summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void DisplayXR_HandTracking_Stop();

        /// <summary>
        /// Locate one hand's 26 joints in the UNITY frame (left-handed, +Z forward), in
        /// metres relative to the display plane, OpenXR joint order (palm 0, wrist 1,
        /// thumb metacarpal 2 … little tip 25).
        /// </summary>
        /// <param name="hand">0 = left, 1 = right.</param>
        /// <param name="time">XrTime, or 0 for the current frame's predicted display time.</param>
        /// <param name="outPoses">26 × 7 floats (see <see cref="PoseStride"/>).</param>
        /// <param name="outRadii">26 joint radii in metres.</param>
        /// <param name="outFlags">26 XrSpaceLocationFlags: 1 orientation valid, 2 position valid, 4 orientation tracked, 8 position tracked.</param>
        /// <returns>1 when the hand is actively tracked, 0 otherwise (arrays are still filled when the locate itself succeeded).</returns>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int DisplayXR_HandTracking_Locate(int hand, long time,
            [Out] float[] outPoses, [Out] float[] outRadii, [Out] ulong[] outFlags);
    }
}
