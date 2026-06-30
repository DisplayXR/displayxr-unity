// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0

using System.Runtime.InteropServices;

namespace DisplayXR
{
    /// <summary>
    /// P/Invoke bindings to the custom IUnityXRDisplay Display Provider's native
    /// session (epic #166). These are the <c>dxr_prov_*</c> exports from
    /// <c>native~/displayxr_xrprovider/displayxr_provider_session.cpp</c>, kept
    /// separate from <see cref="DisplayXRNative"/> (the hook/standalone bindings)
    /// so the provider surface is grouped.
    ///
    /// The provider session runs WITHOUT Unity's OpenXR loader — it is the display
    /// subsystem. So <see cref="DisplayXRFeature"/> is not instantiated in provider
    /// mode; the provider driver reads display info / pushes tunables through here
    /// instead. Windows / D3D12 only (the provider is gated to that platform).
    /// </summary>
    public static class DisplayXRProviderNative
    {
        private const string LibName = "displayxr_unity";

        /// <summary>Display geometry surfaced from XR_EXT_display_info (mirrors DxrProvDisplayInfo).</summary>
        [StructLayout(LayoutKind.Sequential)]
        public struct DisplayInfo
        {
            public float widthM, heightM;
            public uint pixelWidth, pixelHeight;
            public float nominalX, nominalY, nominalZ;
            public float scaleX, scaleY;
            public int isValid;
        }

        /// <summary>One enumerated rendering mode (mirrors DxrProvModeInfo).</summary>
        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Ansi)]
        public struct ModeInfo
        {
            public uint modeIndex;
            public uint viewCount;
            public uint tileColumns, tileRows;
            public uint viewWidthPx, viewHeightPx;
            public float viewScaleX, viewScaleY;
            public int hardwareDisplay3D;
            public int isActive;
            public int isRequestable;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 64)] public string name;
        }

        /// <summary>Whether the provider's runtime session is currently running.</summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int dxr_prov_session_is_running();

        /// <summary>
        /// Push the stereo rig tunables (mirrors displayxr_set_tunables, minus the
        /// clipAtDisplayPlane flag the rig descriptor doesn't carry). Scale-as-zoom
        /// must be folded into virtualDisplayHeight / invConvergenceDistance by the
        /// caller (the native side mirrors the standalone and ignores scale).
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void dxr_prov_set_tunables(
            float ipdFactor, float parallaxFactor, float perspectiveFactor,
            float virtualDisplayHeight, float invConvergenceDistance, float fovOverride,
            float nearZ, float farZ, int cameraCentric);

        /// <summary>
        /// Push the rig (scene/parent) pose in Unity world coords; the native side
        /// converts to OpenXR. enabled=0 reverts to identity.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void dxr_prov_set_display_pose(
            float px, float py, float pz,
            float ox, float oy, float oz, float ow,
            int enabled);

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void dxr_prov_get_display_info(out DisplayInfo info);

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void dxr_prov_get_render_rect(out uint width, out uint height);

        // ---- Rendering modes -------------------------------------------------

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern uint dxr_prov_get_mode_count();

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int dxr_prov_get_mode_info(uint index, out ModeInfo info);

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern uint dxr_prov_get_active_mode_index();

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int dxr_prov_request_rendering_mode(uint modeIndex);

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int dxr_prov_request_display_mode(int mode3d);

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int dxr_prov_set_eye_tracking_mode(int manual);

        // ---- Event consumption (atomic read-and-clear) -----------------------

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int dxr_prov_consume_mode_changed(out uint prevIndex, out uint curIndex);

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int dxr_prov_consume_hw_state_changed(out int hw3d);

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int dxr_prov_consume_eye_tracking_changed(out int isTracking, out int activeMode);
    }
}
