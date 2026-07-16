// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

using System.Runtime.InteropServices;

namespace DisplayXR
{
    /// <summary>
    /// P/Invoke bindings to the custom IUnityXRDisplay Display Provider's native
    /// session (epic #166). These are the <c>dxr_prov_*</c> exports from
    /// <c>native~/displayxr_xrprovider/displayxr_provider_session.cpp</c>, kept
    /// separate from <see cref="DisplayXRNative"/> (the general native bindings)
    /// so the provider surface is grouped.
    ///
    /// The provider session runs WITHOUT Unity's OpenXR loader — it is the display
    /// subsystem. The provider driver reads display info / pushes tunables through
    /// here. Windows / D3D12 only (the provider is gated to that platform).
    /// </summary>
    public static class DisplayXRProviderNative
    {
        private const string LibName = "displayxr_unity";

        /// <summary>Display geometry surfaced from XR_DXR_display_info (mirrors DxrProvDisplayInfo).</summary>
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
        /// Pump xrPollEvent (session-state machine + runtime event latches). On
        /// Windows the native provider pumps from the graphics thread each frame;
        /// on macOS the runtime's poll drains NSApp events + flushes CATransaction,
        /// which are MAIN-thread-only (AppKit throws off-main) — so the driver calls
        /// this every LateUpdate there instead (#204). No-op while no session runs.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void dxr_prov_poll_events();

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
        /// Effective provider render mode: 1 = Single-Pass-Instanced, 0 = MultiPass.
        /// <see cref="DisplayXRPostAA"/> reads it to gate its OnRenderImage FXAA pass:
        /// valid only in MultiPass (each eye is its own single-slice RT); SPI's 2-slice
        /// texture array can't be addressed by Graphics.Blit (garbage / white) (#166).
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int dxr_prov_get_single_pass();

        /// <summary>
        /// (#173) Opt the provider into a DEDICATED standalone weave window instead of
        /// the default app-owned overlay. Set BEFORE the subsystem starts — the loader
        /// calls this with 1 when <c>Application.isEditor</c>, so editor Play Mode gets a
        /// movable window that coexists with the editor (window-relative Kooima + input,
        /// no focus-switch crash) rather than an overlay that covers the whole editor.
        /// Built players never call it (overlay default). Windows-only.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void dxr_prov_set_dedicated_window(int enable);

        /// <summary>
        /// Glue-to-GameView (Task (a), editor + texture probe): reposition the dedicated
        /// weave window so its client rect covers the Unity Game view's on-screen region,
        /// so window-relative Kooima + the weaver's lenticular phase track where the
        /// mirror-blit shows the woven output. x,y = screen px (top-left origin), w,h =
        /// Game view size in px. Pushed each frame by <see cref="DisplayXRProviderDriver"/>.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void dxr_prov_set_gameview_rect(int x, int y, int w, int h);

        /// <summary>
        /// Stash the GameView render rect (physical px) BEFORE the session starts (Task (a)
        /// fill). session_start sizes the weave window to it and borns the forced full-window
        /// zone at that size, so the rendered tile size + the runtime's woven region fill the
        /// panel at native resolution (otherwise the zone freezes at the window creation
        /// default and the mirror srcRect over-samples into black).
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void dxr_prov_set_initial_gameview_rect(int x, int y, int w, int h);

        /// <summary>
        /// Publish the authoritative Game-view panel PHYSICAL px (Phase 1 zone convergence,
        /// #727 follow-up). This is the ONLY reliable physical-px source — info.mirrorRtDesc
        /// reports LOGICAL px on a HiDPI display. The per-frame pump re-drives the forced
        /// full-window zone to this so the compositor canvas == render viewport pixel-exact,
        /// and adapts on a real tab resize (Scale-independent: magnify drives no change).
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void dxr_prov_set_panel_px(int w, int h);

        /// <summary>
        /// Zone-glue arrangement (#740/#742): publish the Game view pane's FULL screen rect
        /// (position + size, physical px). The weave window stays parked at the monitor
        /// origin; the ZONE rect carries the pane's screen offset (the desktop-avatar-proven
        /// contract for placing woven content at a screen sub-rect). Seed BEFORE session
        /// start and push every frame.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void dxr_prov_set_panel_rect(int x, int y, int w, int h);

        /// <summary>
        /// BINDPANE experiment (#740): bind Unity's OWN Game-view pane window (GUIView
        /// child) as the weave HWND — set BEFORE the subsystem starts; the zone rect
        /// (dxr_prov_set_panel_rect) then carries the render area's offset within that
        /// window's client. The plugin never moves/restyles it. Editor + probe only.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void dxr_prov_set_external_weave_hwnd(System.IntPtr hwnd);

        /// <summary>
        /// Bind mode within the editor GameView feature (#740 hybrid): 0 = texture (docked),
        /// 1 = present (undocked). Set before session start, dock-state-driven.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void dxr_prov_set_present_mode(int enable);

        /// <summary>
        /// (#740) Publish the matched Game-view pane HWND + render-origin-minus-pane-window
        /// offset + size so a native WM_TIMER keeps the weave window glued to the pane during
        /// OS modal drags (which freeze the C# glue). NULL pane disables.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void displayxr_set_pane_follow(System.IntPtr paneHwnd, int offX, int offY, int w, int h);

        /// <summary>
        /// (#740 auto-switch) Child-glue selection for the docked texture path: docked →
        /// (1, matched PANE hwnd) — native resolves the pane's GA_ROOT container at
        /// WINDOW-CREATION time (never pre-capture the container: Unity can destroy it as
        /// Play settles); undocked → (0, IntPtr.Zero). -1 restores the
        /// DISPLAYXR_PROV_GV_CHILDGLUE env gate. Survives session stop — set before EVERY
        /// session (re)start, dock-state-driven.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void displayxr_set_child_glue(int enable, System.IntPtr paneHwnd);

        /// <summary>
        /// (#740 auto-switch) 1 while the dedicated weave window is a live HWND. A
        /// child-glue window dies with its parent container; the recovery watcher polls
        /// this and restarts the subsystem to recreate it under the surviving container.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int displayxr_dedicated_window_alive();

        /// <summary>
        /// (#740 stereo unswap) 1 = submit the two stereo views into opposite swapchain
        /// slots — cancels the docked texture path's view-order flip (a discrete, geometry-
        /// invariant, runtime/SDK-side defect). Set per (re)start = docked-and-not-maximized.
        /// STEREO ONLY; -1 restores the DISPLAYXR_PROV_VIEW_SWAP env gate. Survives session
        /// stop — re-set per start.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void dxr_prov_set_view_swap(int enable);

        /// <summary>
        /// (#740 f-up) 1 while the custom host MOVE drag is in progress. The driver pauses
        /// its per-frame GameView glue pushes during the drag (Unity's maximized-view
        /// layout readings flap by the toolbar height per frame while the container moves
        /// → zone/swapchain realloc storm → shimmer); the native lockstep follow owns the
        /// window position until mouse-up.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int displayxr_host_drag_active();

        /// <summary>
        /// GameView weave-to-texture presentation (Task (a), editor + texture probe): the
        /// runtime-woven shared texture opened on Unity's D3D device. C# wraps it as an
        /// external Texture2D and draws it into the Game view (DisplayXRGameViewPresenter).
        /// Returns IntPtr.Zero until texture mode publishes it.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern System.IntPtr dxr_prov_get_woven_unity_texture(out uint width, out uint height);

        /// <summary>
        /// The woven content's canvas sub-rect within the shared texture (top-left origin px)
        /// plus the full texture dims, so the presenter can build a normalized texCoords rect.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void dxr_prov_get_woven_canvas(
            out int x, out int y, out int cw, out int ch, out uint texw, out uint texh);

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

        /// <summary>Per-zone per-eye foreground clip (#166 multi-zone). Zone 0 = primary,
        /// i>=1 = extra zone i-1. Returns 1 on success.</summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int dxr_prov_get_zone_eye_clip(
            uint zone, uint eye, out float outFar, out float outEx, out float outEy, out float outEz);

        /// <summary>Number of 3D zones (1 primary + active extra zones). Multi-zone
        /// transparent mask (#166): the overlay unions a per-zone silhouette.</summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern uint dxr_prov_get_zone_count();

        /// <summary>Fill zone `zone`'s cyclopean L/R view+proj (float[16] each,
        /// column-major). Returns 1 on success. Zone 0 = primary; i>=1 = extra i-1.</summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int dxr_prov_get_zone_stereo_matrices(
            uint zone,
            [Out] float[] lv, [Out] float[] lp, [Out] float[] rv, [Out] float[] rp);

        /// <summary>Fill zone `zone`'s window-client pixel rect (top-left origin).
        /// Returns 1 on success.</summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int dxr_prov_get_zone_rect_px(
            uint zone, out int x, out int y, out int w, out int h);

        /// <summary>
        /// Push the stereo rig tunables to the provider (minus the clipAtDisplayPlane
        /// flag the rig descriptor doesn't carry). Scale-as-zoom
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

        // ---- Atlas capture (XR_DXR_atlas_capture, #140) ----------------------

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int dxr_prov_capture_atlas(
            [MarshalAs(UnmanagedType.LPStr)] string pathPrefix, int stage);

        // ---- Event consumption (atomic read-and-clear) -----------------------

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int dxr_prov_consume_mode_changed(out uint prevIndex, out uint curIndex);

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int dxr_prov_consume_hw_state_changed(out int hw3d);

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int dxr_prov_consume_eye_tracking_changed(out int isTracking, out int activeMode);
    }
}
