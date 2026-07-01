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
        /// Select the stereo render mode BEFORE the session starts (#166 task #8):
        /// 1 = Single-Pass-Instanced (URP+Win+D3D12 — 1 pass × 2 over a 2-slice array),
        /// 0 = MultiPass (BiRP/other — 2 pass × 1, one texture per eye). SPI renders
        /// opaque geometry wrong on BiRP, so the loader gates on the active pipeline.
        /// Must be called before StartSubsystem (GfxStart reads it).
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void dxr_prov_set_single_pass(int enable);

        /// <summary>
        /// Request a transparent background BEFORE the session starts (#166 Phase A):
        /// 1 opts the session into ALPHA_BLEND + transparentBackgroundEnabled so the
        /// runtime's DComp overlay composites the woven 3D over the desktop with
        /// per-pixel alpha. Only takes effect if the runtime advertises ALPHA_BLEND.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void dxr_prov_set_transparent_background(int enable);

        /// <summary>
        /// Set the single 3D-zone rect (client-window px) the runtime frames the
        /// Kooima 3D into (#166 Phase B). Seed BEFORE the session starts (like the
        /// hook SeedLaunchZone) so the swapchain is born zone-sized. w&lt;=0||h&lt;=0 clears.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void dxr_prov_set_3d_zone_rect(int x, int y, int w, int h);

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void dxr_prov_clear_3d_zone();

        /// <summary>
        /// Set the total number of 3D zones (#166 Phase B2): 1 primary + extras. 0/1 →
        /// no extra zones. Clamped to the provider's max (4, Unity's render-pass cap).
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void dxr_prov_set_zone_count(uint totalZones);

        /// <summary>Set 3D zone `index`'s rect (client-window px). index 0 = primary.</summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void dxr_prov_set_zone(uint index, uint zoneId, int x, int y, int w, int h);

        /// <summary>
        /// Lazily create the provider's Local2D overlay swapchain + cross-device bridge
        /// (#166 Phase B) sized to w×h and return the Unity-device handle. Mirrors
        /// dxr_prov_get_wsui_bridge. C# CopyTexture's the canvas RT into it each frame.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void dxr_prov_get_local2d_bridge(
            uint width, uint height,
            out System.IntPtr nativePtr, out uint outWidth, out uint outHeight);

        /// <summary>Set the Local2D dest rect in client-window pixels. w&lt;=0||h&lt;=0 clears.</summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void dxr_prov_set_local2d_rect(int x, int y, int w, int h);

        /// <summary>
        /// Per-eye foreground-clip data (#166 Phase B): the eye's foreground far
        /// (view-space display-plane distance, world units) + the eye WORLD position
        /// (Unity coords). DisplayXRDisplay publishes these to the URP ForegroundClipURP
        /// globals in provider mode (DisplayXRFeature.GetStereoMatrices is inert then).
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void dxr_prov_get_eye_clip(
            uint eye, out float outFar, out float outEx, out float outEy, out float outEz);

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

        // ---- Window-space UI (HUD) bridge (#67/#166) -------------------------
        // Lazily creates the provider's wsui overlay swapchain + cross-device
        // bridge sized to w×h and returns the Unity-device handle of the bridge.
        // C# Graphics.CopyTexture's the canvas RT into it each frame.
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void dxr_prov_get_wsui_bridge(
            uint width, uint height,
            out System.IntPtr nativePtr, out uint outWidth, out uint outHeight);

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
