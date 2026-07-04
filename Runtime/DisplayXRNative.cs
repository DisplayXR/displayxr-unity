// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

using System;
using System.Runtime.InteropServices;

namespace DisplayXR
{
    /// <summary>
    /// P/Invoke bindings to the displayxr_unity native plugin.
    ///
    /// Promoted to public in v1.5.9 so user code (notably the Samples~/
    /// DefaultInputController sample that moved out of Runtime) can call
    /// the same native bindings the plugin uses internally. Method
    /// signatures track the underlying native exports — a native change
    /// is a binding change. Keep that contract in mind if you call these
    /// from app code: prefer the high-level wrappers (DisplayXRProvider,
    /// DisplayXRTransparentOverlay, DisplayXRRigManager) where they
    /// exist; reach for DisplayXRNative only when nothing higher-level
    /// is available.
    /// </summary>
    public static class DisplayXRNative
    {
        private const string LibName = "displayxr_unity";

        /// <summary>
        /// Set stereo rig tunables from game thread.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void displayxr_set_tunables(
            float ipdFactor,
            float parallaxFactor,
            float perspectiveFactor,
            float virtualDisplayHeight,
            float invConvergenceDistance,
            float fovOverride,
            float nearZ,
            float farZ,
            int cameraCentric,
            int clipAtDisplayPlane);

        /// <summary>
        /// Get display info queried from runtime via XR_EXT_display_info.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void displayxr_get_display_info(
            out float displayWidthM,
            out float displayHeightM,
            out uint pixelWidth,
            out uint pixelHeight,
            out float nominalX,
            out float nominalY,
            out float nominalZ,
            out float scaleX,
            out float scaleY,
            out int isValid);

        /// <summary>
        /// Get raw eye positions from last xrLocateViews call.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void displayxr_get_eye_positions(
            out float lx, out float ly, out float lz,
            out float rx, out float ry, out float rz,
            out int isTracked);

        /// <summary>
        /// Set scene transform (parent camera pose + zoom) applied before Kooima.
        /// Chain: raw eyes → scene transform → tunables → Kooima.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void displayxr_set_scene_transform(
            float posX, float posY, float posZ,
            float oriX, float oriY, float oriZ, float oriW,
            float scaleX, float scaleY, float scaleZ,
            int enabled);

        /// <summary>
        /// Set the window handle for session creation (HWND on Win32, NSView* on macOS).
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void displayxr_set_window_handle(IntPtr handle);

        /// <summary>
        /// (runtime-pvt #191 / displayxr-unity #57) Request the runtime's
        /// transparent-background mode for the next OpenXR session. Sets
        /// transparentBackgroundEnabled on XrWin32WindowBindingCreateInfoEXT
        /// (XR_EXT_win32_window_binding spec_version 4) so the runtime opts the
        /// bound HWND's swapchain into BitBlt (D3D11) or DComp (D3D12)
        /// presentation.
        ///
        /// Must be called BEFORE xrCreateSession runs — typically from
        /// [RuntimeInitializeOnLoadMethod(SubsystemRegistration)].
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void displayxr_set_transparent_background(int enabled);

        /// <summary>
        /// (#34 / #131) Define the 3D canvas sub-rect within the window client
        /// area, in pixels. The runtime weaves the 3D into this rect; the rest
        /// of the window is the 2D surround region. Pass width==0 || height==0
        /// to clear (full-window canvas). Re-applied each frame; safe any time.
        /// Hooked path (built apps) only.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void displayxr_set_canvas_rect(int x, int y, uint width, uint height);

        /// <summary>
        /// (#34 / #131) Read back the active 3D canvas sub-rect (HWND-client
        /// pixels, top-left origin). Returns 1 and fills the out params when a
        /// sub-rect is set; returns 0 (full-window canvas) and leaves them
        /// untouched otherwise. Used by the overlay raycast so cursor→NDC maps
        /// into the sub-rect the 3D actually weaves into (the stereo projection
        /// is computed for that sub-rect), not the full window.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int displayxr_get_canvas_rect_px(
            out int x, out int y, out uint width, out uint height);

        /// <summary>
        /// (XR_EXT_display_zones) Define the 3D-zone rect (client-window pixels,
        /// top-left origin) the runtime frames the Kooima 3D into. The locate
        /// hook chains an XrDisplayZoneEXT in front of the view-rig (zone-scoped
        /// projection) and the xrEndFrame hook chains the same zone on each
        /// projection layer (binding its views into the rect). width&lt;=0 ||
        /// height&lt;=0 clears. Inert unless the runtime advertises
        /// XR_EXT_display_zones and reports caps.supported. Mutually exclusive
        /// with displayxr_set_canvas_rect (a zones frame makes the output rect
        /// inert). Hooked path (built apps) only.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void displayxr_set_3d_zone_rect(int x, int y, int width, int height);

        /// <summary>
        /// (XR_EXT_display_zones) Clear the 3D-zone rect (revert to full-window
        /// framing). Hooked path (built apps) only.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void displayxr_clear_3d_zone();

        /// <summary>
        /// (#131) Get the runtime's weave-target size = the bound HWND client
        /// area, in physical pixels. On Leia SR this differs from the display
        /// panel dims. The 2D surround texture + canvas sub-rect must use THESE
        /// pixels. Returns 0x0 until a window is bound. Hooked path (built apps).
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void displayxr_get_render_target_size(out uint width, out uint height);

        /// <summary>
        /// (displayxr-unity #85) Configure Unity's main render NSWindow for
        /// transparent overlay mode on macOS. enabled=1 flips setOpaque:NO +
        /// backgroundColor=clearColor; enabled=0 restores the saved values.
        /// No-op on non-Apple platforms.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void displayxr_macos_configure_unity_nswindow(int enabled);

        /// <summary>
        /// Move Unity's configured NSWindow by (dx, dy) screen points. Used by
        /// app code that implements borderless-window drag (e.g. right-mouse-
        /// drag-to-move). Cocoa screen coordinates: +x right, +y UP. If the
        /// app's input source is top-left pixel space (e.g. flipped Y), negate
        /// dy. No-op until configure_unity_nswindow has been called. No-op on
        /// non-Apple platforms.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void displayxr_macos_offset_window(int dx, int dy);

        /// <summary>
        /// Cursor-anchored window-drag API (recommended over offset_window for
        /// drag-to-move use cases). Begin captures the cursor's offset within
        /// the window; Update snaps the origin each frame so the cursor stays
        /// pinned to the same window-relative spot. End clears state. Avoids
        /// the scale / feedback issues of app-side cursor deltas on Mac.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void displayxr_macos_begin_window_drag();
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void displayxr_macos_update_window_drag();
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void displayxr_macos_end_window_drag();

        /// <summary>
        /// Toggle Unity's configured NSWindow between borderless (no title bar /
        /// close / minimize / resize chrome) and the saved original style.
        /// Save/restore is symmetric; pass enabled=0 to restore. Idempotent.
        /// No-op on non-Apple platforms. macOS only.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void displayxr_macos_set_window_borderless(int enabled);

        /// <summary>
        /// Hint the typed-swapchain substitution about the project color space.
        /// 1 = Linear (UNORM_SRGB siblings); 0 = Gamma (UNORM siblings).
        /// Must be called BEFORE Unity creates its OpenXR swapchains.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void displayxr_set_use_srgb_swapchain(int enabled);

        /// <summary>
        /// Check whether the plugin is running in shell/IPC mode.
        /// Detected via DISPLAYXR_WORKSPACE_SESSION=1 (legacy
        /// DISPLAYXR_SHELL_SESSION=1 is also honored) environment variable.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int displayxr_is_shell_mode();

        /// <summary>
        /// (issue #57) Returns 1 if the OS foreground window belongs to our
        /// process, 0 otherwise. Use to gate input handlers (WASD, mouse-look)
        /// that should be inactive when the user has clicked through the
        /// transparent overlay to another app — otherwise Unity's RawInput
        /// (which uses RIDEV_INPUTSINK so it receives input regardless of
        /// foreground) would also process the keypress and e.g. move the cube
        /// while the user is typing in Notepad.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int displayxr_is_our_process_foreground();

        /// <summary>
        /// (issue #57) Overlay's current client size in pixels. After scroll-
        /// resize the overlay's rect changes but Unity's Screen.width/height
        /// stay frozen at the off-screen Unity HWND's dimensions — raycast
        /// math must use these values, not Screen.*. Returns (0, 0) if no
        /// overlay HWND.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void displayxr_get_overlay_size(out int width, out int height);

        /// <summary>
        /// Get shell-mode mouse button state from native WM_INPUT tracking.
        /// Buttons: bit 0 = left, bit 1 = right, bit 2 = middle.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void displayxr_get_shell_mouse_state(
            out int buttons, out int mouseX, out int mouseY);

        /// <summary>
        /// (#140 / #396 W6) Capture the runtime's composed multi-view atlas to a
        /// PNG via xrCaptureAtlasEXT (XR_EXT_atlas_capture). The runtime does the
        /// readback with the compositor's own atlas image and writes
        /// "&lt;pathPrefix&gt;_atlas_&lt;viewCount&gt;_&lt;cols&gt;x&lt;rows&gt;.png"
        /// (runtime owns the suffix; see DisplayXR/displayxr-runtime#425) — no
        /// app-side AsyncGPUReadback or hidden-camera re-render. Non-blocking
        /// (latches; PNG lands next composed frame).
        /// </summary>
        /// <param name="pathPrefix">Bare output path prefix (no layout tokens); the
        /// runtime appends "_atlas_&lt;viewCount&gt;_&lt;cols&gt;x&lt;rows&gt;.png".</param>
        /// <param name="stage">1 = projection-only, 0 = post-compose (includes chrome).</param>
        /// <returns>1 on XR_SUCCEEDED, 0 if unresolved / no session / failed.</returns>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int displayxr_capture_atlas(
            [MarshalAs(UnmanagedType.LPStr)] string pathPrefix, int stage);

        /// <summary>
        /// Get the Kooima stereo view and projection matrices computed by the native library.
        /// These are the matched matrix pairs that should be applied directly, bypassing
        /// Unity's matrix reconstruction from (fov, position, orientation).
        /// Matrices are column-major, OpenXR/OpenGL convention.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void displayxr_get_stereo_matrices(
            [MarshalAs(UnmanagedType.LPArray, SizeConst = 16)] float[] leftView,
            [MarshalAs(UnmanagedType.LPArray, SizeConst = 16)] float[] leftProj,
            [MarshalAs(UnmanagedType.LPArray, SizeConst = 16)] float[] rightView,
            [MarshalAs(UnmanagedType.LPArray, SizeConst = 16)] float[] rightProj,
            out int valid);

        /// <summary>
        /// Get readback pixel data from offscreen rendering.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void displayxr_get_readback(
            out IntPtr pixels,
            out uint width,
            out uint height,
            out int ready);

        /// <summary>
        /// Kill xrPollEvent forwarding. Call before session/instance teardown.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void displayxr_stop_polling();

        // ====================================================================
        // Window-space UI overlay (issue #67)
        //
        // Routes a Canvas RenderTexture to a XrCompositionLayerWindowSpaceEXT
        // composition layer. The native side mirrors the Unity texture into
        // an OpenXR overlay swapchain each frame; the runtime composites it
        // on top of the eye projection.
        // ====================================================================

        /// <summary>
        /// Register the Unity-side native texture pointer that the overlay
        /// swapchain should mirror each frame. Pass IntPtr.Zero to deregister.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void displayxr_window_space_ui_set_texture(
            IntPtr nativeTex, int width, int height);

        /// <summary>
        /// Update the layer descriptor (fractional window coords + per-eye
        /// horizontal disparity). Cheap; safe to call every frame.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void displayxr_window_space_ui_set_layer(
            float x, float y, float width, float height, float disparity);

        /// <summary>
        /// Clear the registered texture and mark the layer inactive.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void displayxr_window_space_ui_clear();

        // ====================================================================
        // Local2D overlay (#439/#491) — modern mask-based 2D-over-3D layer
        // (XrCompositionLayerLocal2DEXT, "glass over 3D"). Replaces the legacy
        // 2D-surround handoff for in-canvas 2D content (e.g. a speech bubble).
        // ====================================================================

        /// <summary>
        /// Register the Unity RenderTexture whose contents become the Local2D
        /// layer. Safe to call every frame; a changed pointer/size lazily
        /// recreates the overlay swapchain. Pass IntPtr.Zero to deregister.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void displayxr_local2d_set_texture(
            IntPtr nativeTex, int width, int height);

        /// <summary>
        /// Set the destination rect in client-window PIXELS (post-DPI) — the
        /// XrCompositionLayerLocal2DEXT::rect / mask-tier coordinate space.
        /// Cheap; safe to call every frame.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void displayxr_local2d_set_rect(
            int x, int y, int width, int height);

        /// <summary>
        /// Clear the registered texture and mark the Local2D layer inactive.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void displayxr_local2d_clear();

        /// <summary>
        /// Get the display backing scale factor (Retina).
        /// Returns 2.0 on macOS Retina, 1.0 on non-Retina or non-macOS.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern float displayxr_get_backing_scale_factor();

        // ====================================================================
        // Transparent overlay mode (issue #57) — Windows only
        //
        // Per-pixel-alpha transparent overlay for desktop avatar use cases.
        // The OpenXR session opts into ALPHA_BLEND so Unity emits real alpha
        // to the swapchain; the runtime composes the desktop under each tile
        // pre-weave and alpha-gates post-weave. On Windows the overlay HWND
        // is top-level + WS_EX_NOREDIRECTIONBITMAP so DWM composites it
        // purely from the runtime's DComp visuals.
        // ====================================================================

#if UNITY_STANDALONE_WIN || UNITY_EDITOR_WIN
        /// <summary>
        /// Toggle transparent overlay mode on the parent (Unity top-level)
        /// HWND. Cloaks Unity, installs the focus hook, and snaps the top-
        /// level overlay HWND to Unity's rect. Mutually exclusive with shell
        /// mode.
        /// </summary>
        /// <param name="enabled">1 to enable, 0 to restore original styles.</param>
        /// <param name="topmost">1 to add WS_EX_TOPMOST while enabled.</param>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void displayxr_set_transparent_overlay(
            int enabled, int topmost);

        /// <summary>
        /// (avatar simple-window) Style Unity's real main HWND for avatar-style
        /// windowing: strip to borderless WS_POPUP, subclass for the borderless
        /// right-drag (#61-bracketed), and let the hit-mask path drive
        /// SetWindowRgn on this HWND. enabled=0 restores Unity's styles. Call
        /// after the session starts.
        /// </summary>
        /// <param name="enabled">1 to enable, 0 to restore original styles.</param>
        /// <param name="topmost">1 to add WS_EX_TOPMOST while enabled.</param>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void displayxr_set_simple_window(int enabled, int topmost);

        /// <summary>
        /// (avatar simple-window) Toggle window decoration: WS_POPUP (borderless)
        /// &lt;-&gt; WS_OVERLAPPEDWINDOW (title bar + sizing frame for OS
        /// move/resize). The client rect is preserved across the toggle so
        /// content doesn't jump or rescale. Matches the avatar's B key. No-op
        /// unless simple-window mode is active.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void displayxr_toggle_window_decoration();

        /// <summary>
        /// (avatar simple-window) Set decoration state explicitly (1=decorated,
        /// 0=borderless). Idempotent variant of displayxr_toggle_window_decoration.
        /// No-op unless simple-window mode is active.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void displayxr_set_window_decorated(int decorated);

        /// <summary>
        /// Set the rectangular hit-test region of the overlay. Coords
        /// are overlay client-space pixels (top-left origin).
        ///
        /// In transparent WS_POPUP + NOREDIRECTIONBITMAP mode (#57),
        /// this also drives SetWindowRgn — outside the rect the OS
        /// treats our window as if it didn't exist (both rendering and
        /// hit-testing), so input is routed natively to whichever
        /// desktop window is at the cursor with full fidelity (real
        /// DefWindowProc modal SC_MOVE/SC_SIZE/SC_CLOSE loops, native
        /// cursor adaptation, native menu activation, native hover).
        /// Push the cube/avatar silhouette's screen-space AABB each
        /// frame. w &lt;= 0 or h &lt;= 0 clears the region (overlay
        /// catches everywhere — used as init default).
        ///
        /// In opaque WS_CHILD mode (legacy/Game-View overlay), this
        /// updates the rect used by WM_NCHITTEST as a fast
        /// HTCLIENT-vs-HTTRANSPARENT discriminator.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void displayxr_set_overlay_hit_rect(
            int x, int y, int w, int h);

        /// <summary>
        /// Per-pixel hit-test state from the C# raycast. Tracks "is the
        /// cursor over a clickable renderer?" for callers that want to
        /// know — but no longer drives the OS hit-test routing. OS
        /// routing is owned by displayxr_set_overlay_hit_rect (AABB-
        /// region path) or displayxr_set_overlay_hit_mask (per-pixel
        /// silhouette path, takes over once any mask has been pushed).
        /// Kept callable for backward compat.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void displayxr_set_overlay_hit_active(int active);

        /// <summary>
        /// (issue #57 Approach B+) Per-pixel silhouette mask drives
        /// SetWindowRgn for cross-process click-through. <paramref name="mask"/>
        /// is mask_w*mask_h bytes (non-zero = opaque/overlay catches,
        /// zero = transparent/OS routes past to whatever desktop window
        /// is at the cursor), conceptually scaled to overlay client
        /// size dst_w*dst_h. The native side walks the mask row-by-row,
        /// RLE-encodes opaque runs as RECTs, ExtCreateRegion's the
        /// union, and SetWindowRgn's it onto the overlay HWND. Outside
        /// the silhouette the OS treats our window as if it didn't
        /// exist — including concavities the AABB swallows (between
        /// the tiger's legs, around the tail, etc.).
        ///
        /// Push from an AsyncGPUReadback callback each frame. NULL
        /// mask reverts to the AABB-region path. Once any mask is
        /// applied, displayxr_set_overlay_hit_rect stops driving
        /// SetWindowRgn — callers committed to the mask path must
        /// keep it fresh each frame.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void displayxr_set_overlay_hit_mask(
            IntPtr mask, int mask_w, int mask_h, int dst_w, int dst_h);

        /// <summary>
        /// (#131) Register an opaque rect (overlay client pixels, top-left
        /// origin) that must catch clicks even though it lives in the 2D
        /// surround region outside the 3D silhouette — e.g. a high-res text
        /// bubble. It is UNION-ed into the SetWindowRgn region built by
        /// displayxr_set_overlay_hit_mask each frame, so the bubble catches
        /// clicks while the empty surround keeps routing past to the desktop.
        /// Pass w&lt;=0 || h&lt;=0 to clear. Takes effect on the next hit-mask
        /// update. Transparent overlay (hooked) path only.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void displayxr_set_overlay_surround_rect(
            int x, int y, int w, int h);

        /// <summary>
        /// (#131) Per-pixel variant of displayxr_set_overlay_surround_rect:
        /// register the EXACT shape of a 2D surround element (e.g. a comic
        /// bubble with a triangular tail) as an alpha mask (mask_w*mask_h
        /// bytes, non-zero = opaque/catch) mapped over the dst rect (overlay
        /// client px, top-left). RLE-unioned into the SetWindowRgn region each
        /// frame, so the element catches clicks while the empty area beside it
        /// (e.g. the corners next to the tail) keeps routing to the desktop —
        /// which a single bounding rect can't express. The surround is flat
        /// post-weave 2D, so the caller rasterizes the mask directly (no
        /// disparity / per-view math). The plugin copies the bytes. Pass
        /// mask = IntPtr.Zero or any dim &lt;= 0 to clear. Transparent overlay
        /// (hooked) path only.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void displayxr_set_overlay_surround_mask(
            IntPtr mask, int mask_w, int mask_h,
            int dst_x, int dst_y, int dst_w, int dst_h);

        /// <summary>
        /// (#131) Put the transparent overlay into fixed full-screen,
        /// app-managed window mode. enabled=1 sizes the overlay HWND to its
        /// monitor at the aligned origin and DISABLES the native right-drag
        /// move so the app owns all window interaction via virtual rects (3D
        /// canvas sub-rect via displayxr_set_canvas_rect + 2D surround). This
        /// turns move/resize into pure rectangle math (no OS resize, no
        /// phase-snap, no mid-resize content drop). enabled=0 restores native
        /// move. Windows transparent overlay only; no-op elsewhere.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void displayxr_set_overlay_fullscreen(int enabled);

        /// <summary>
        /// (#131) Opt in to a born-fullscreen transparent overlay. Must be called
        /// BEFORE the overlay is created — call as early as possible, e.g. from a
        /// RuntimeInitializeOnLoadMethod(BeforeSplashScreen). The overlay is then
        /// created already covering its monitor, so the app needs no post-creation
        /// resize (which recreates the swapchain = a startup flash) and Unity can
        /// stay WINDOWED (avoiding fullscreen independent-flip, which bypasses DWM
        /// alpha compositing and washes the content out). No-op once the overlay
        /// exists. Windows transparent overlay only.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void displayxr_set_fullscreen_overlay_pref(int enabled);

        /// <summary>
        /// (#131) Set the transparent overlay's mouse cursor shape (applied each
        /// mouse move over the overlay client area). Shapes: 0=arrow, 1=size-WE,
        /// 2=size-NS, 3=size-NWSE, 4=size-NESW, 5=size-all (move). The region
        /// editor uses it for resize affordances on window edges/corners and
        /// region lines. Windows transparent overlay only; no-op elsewhere.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void displayxr_set_overlay_cursor(int shape);

        /// <summary>
        /// Read cursor position (overlay-client coords, top-left origin) and
        /// mouse button state. Designed for transparent overlay mode where
        /// Unity's New Input System Mouse.current.position is frozen because
        /// the cloaked Unity HWND isn't OS-foreground (documented Unity
        /// limitation). Returns (-1, -1) for clientX/Y if no overlay HWND.
        /// Buttons: bit 0 = left, 1 = right, 2 = middle.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void displayxr_get_overlay_pointer(
            out int clientX, out int clientY, out int buttons);

        /// <summary>
        /// (v1.2.2) Atomically read + zero the overlay's accumulated mouse-
        /// wheel delta. Returns Win32 raw units (120 per notch; positive =
        /// wheel forward). Always 0 in opaque mode (no transparent overlay).
        /// The plugin no longer self-resizes the overlay on wheel events;
        /// apps consume this value and decide what to do with it.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int displayxr_consume_overlay_wheel_delta();

        /// <summary>
        /// (display-zones port) Atomically read + zero the overlay's close-
        /// request flag. Returns 1 once after the user pressed the decorated
        /// overlay's close (X) button or Alt+F4. The plugin swallows the
        /// overlay's WM_CLOSE (it does NOT destroy the overlay HWND, which would
        /// leave cloaked Unity running headless) and raises this flag instead.
        /// Apps poll it each frame and call Application.Quit() so the whole
        /// process shuts down cleanly. Always 0 when no transparent overlay.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int displayxr_consume_overlay_close_request();

        /// <summary>
        /// (display-zones port) Resize the managed overlay window to width x
        /// height px via SetWindowPos (#61-bracketed, clamped to a min size).
        /// The reliable resize path on the SR display — the weaver subclass eats
        /// mouse edge interactions on the non-activating overlay, but
        /// SetWindowPos is never intercepted. Apps drive it from a keyboard
        /// shortcut. No-op when there is no managed overlay window.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void displayxr_resize_overlay(int width, int height);

        /// <summary>
        /// (display-zones port) Get/set the managed overlay window's screen-space
        /// top-left, for app-side window-position persistence. Set is #61-bracketed
        /// (size unchanged). No-op when there is no managed overlay window.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void displayxr_get_overlay_position(out int x, out int y);

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void displayxr_set_overlay_position(int x, int y);
#endif

    }
}
