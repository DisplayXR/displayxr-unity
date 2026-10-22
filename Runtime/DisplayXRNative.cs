// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

using System;
using System.Runtime.InteropServices;
using UnityEngine;

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
        /// Get display info queried from runtime via XR_DXR_display_info.
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
        /// (runtime-pvt #191 / displayxr-unity #57) Request the runtime's
        /// transparent-background mode for the next OpenXR session. Sets
        /// transparentBackgroundEnabled on XrWin32WindowBindingCreateInfoDXR
        /// (XR_DXR_win32_window_binding spec_version 4) so the runtime opts the
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
        /// (#189) Read the Kooima canvas the runtime frames the window-relative
        /// off-axis projection into: its rect ON THE PANEL (panel pixels,
        /// top-left origin) and its physical size (meters). Returns 1 and fills
        /// the out params when valid; returns 0 otherwise. Published each frame
        /// by the provider from the XR_DXR_view_rig raw channel; used by the
        /// editor Scene-view eye gizmo to draw window-relative eyes + the
        /// convergence-plane aspect Kooima consumes.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int displayxr_get_kooima_canvas(
            out int rectX, out int rectY, out int rectW, out int rectH,
            out float sizeWMeters, out float sizeHMeters);

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
        /// (issue #323) The size in pixels this app is actually composited at
        /// when the runtime — not the OS window — owns it: the live workspace
        /// tile canvas the provider polls and sizes the eye swapchain from.
        /// Returns 1 with width/height filled when a tile size is known, 0
        /// otherwise (no workspace controller, tile slot not bound yet, older
        /// runtime) — then keep using Screen.*. Under the shell Unity's main
        /// window is minimized and never follows the tile, so Screen.* is
        /// frozen at the launch size. Pixels only; carries no DPI space.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int displayxr_get_render_canvas_size(
            out uint width, out uint height);

        /// <summary>
        /// Get shell-mode mouse button state from native WM_INPUT tracking.
        /// Buttons: bit 0 = left, bit 1 = right, bit 2 = middle.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void displayxr_get_shell_mouse_state(
            out int buttons, out int mouseX, out int mouseY);

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

        // ====================================================================
        // Window-space UI overlay (issue #67)
        //
        // Routes a Canvas RenderTexture to a XrCompositionLayerWindowSpaceDXR
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

        // Per-slot window-space UI (#336 follow-up). Several DisplayXRWindowSpaceUI
        // components can be live at once; each takes a slot so they no longer overwrite
        // each other. The slot-less calls above address slot 0.
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int displayxr_window_space_ui_acquire_slot();

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void displayxr_window_space_ui_release_slot(int slot);

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void displayxr_window_space_ui_set_texture_slot(
            int slot, IntPtr nativeTex, int width, int height);

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void displayxr_window_space_ui_set_layer_slot(
            int slot, float x, float y, float width, float height, float disparity);

        // ====================================================================
        // Local2D overlay (#439/#491) — modern mask-based 2D-over-3D layer
        // (XrCompositionLayerLocal2DDXR, "glass over 3D"). Replaces the legacy
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
        /// XrCompositionLayerLocal2DDXR::rect / mask-tier coordinate space.
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

#endif

        // Window controls (display-zones port on Windows; #332 on Linux, where they
        // act on Unity's own window and the transparent overlay follows it). Same
        // exports and semantics on both.
#if UNITY_STANDALONE_WIN || UNITY_EDITOR_WIN || UNITY_STANDALONE_LINUX
        /// <summary>
        /// (display-zones port) Atomically read + zero the overlay's close-
        /// request flag. Returns 1 once after the user pressed the decorated
        /// overlay's close (X) button or Alt+F4. The plugin swallows the
        /// overlay's WM_CLOSE (it does NOT destroy the overlay HWND, which would
        /// leave cloaked Unity running headless) and raises this flag instead.
        /// Apps poll it each frame and call Application.Quit() so the whole
        /// process shuts down cleanly. Always 0 when no transparent overlay.
        /// On Linux always 0: Unity's own (managed) window takes the WM close
        /// (Alt+F4) and quits by itself, and it has no title bar while cloaked.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int displayxr_consume_overlay_close_request();

        /// <summary>
        /// (display-zones port) Resize the managed overlay window to width x
        /// height px via SetWindowPos (#61-bracketed, clamped to a min size).
        /// The reliable resize path on the SR display — the weaver subclass eats
        /// mouse edge interactions on the non-activating overlay, but
        /// SetWindowPos is never intercepted. Apps drive it from a keyboard
        /// shortcut. No-op when there is no managed overlay window. On Linux it
        /// resizes Unity's window; the weave window and swapchain follow it.
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

        // Click-through region (#57/#131 on Windows, #332 on Linux). Same exports
        // on both: Windows builds a SetWindowRgn on the overlay HWND, Linux an XShape
        // input region on Unity's window (see displayxr_linux.c). On Linux they are
        // no-ops outside transparent mode.
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
#endif

#if UNITY_STANDALONE_LINUX && !UNITY_EDITOR
        /// <summary>
        /// (#332) Right-drag move of the Linux transparent overlay. Call every
        /// frame with the right button's state; the first pressed call starts
        /// the drag, later ones move Unity's window (the overlay follows) by
        /// how far the pointer has moved, the first released call ends it.
        /// Returns 1 while a drag is in progress. No-op outside transparent mode.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int displayxr_linux_drag_window(int right_pressed);

        /// <summary>
        /// (#332) 1 while the Linux transparent overlay is up and the click-through
        /// exports act; 0 for the opaque child window or without X. Cached state,
        /// cheap to poll every frame.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int displayxr_linux_click_through_active();

        /// <summary>
        /// Native-Wayland player: device pixels per logical pixel of Unity's window.
        /// Unity's Screen and pointer are logical there, while the window-pixel API
        /// (overlay size, PointerPosition) is device pixels, as on X11 and Windows.
        /// 1 everywhere else.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern float displayxr_linux_ui_scale();

        /// <summary>
        /// Native-Wayland player: a window size asked for through
        /// displayxr_resize_overlay, in logical pixels, for Screen.SetResolution
        /// (a Wayland client sizes its own window). Returns 1 when one is pending.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int displayxr_linux_take_pending_resize(out int width, out int height);

        /// <summary>
        /// 1 when the last <see cref="displayxr_get_overlay_position"/> answer was the
        /// window's current position. 0 on a native-Wayland player that could not tell
        /// right now (no DisplayXR GNOME extension, the screen locked, the window not
        /// shown yet): the answer was then the last known position, or (0, 0) if there
        /// was none. (0, 0) is a legitimate position, so check this before saving one.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int displayxr_linux_overlay_position_known();

        /// <summary>
        /// Native-Wayland player: what can be asked of the app's own window right
        /// now, through the DisplayXR GNOME extension (version 15): bit 1 keep
        /// above, bit 2 activate, bit 4 minimize. 0 in X11 mode (the app's own
        /// EWMH calls work there), before the session starts, or without the
        /// extension.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int displayxr_linux_window_state_caps();

        /// <summary>
        /// Native-Wayland player: keep the app's window above other windows (1) or
        /// stop (0). Remembered (also when called before the session starts) and
        /// sent until the extension confirms it: after a refusal (screen locked,
        /// a drag) and whenever the window is shown anew (recreated, the extension
        /// came back). Returns 1 when the extension can do it now, 0 when it cannot
        /// right now (still remembered). Setting the same value again is a no-op.
        /// Asynchronous; the result is logged.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int displayxr_linux_set_keep_above(int above);

        /// <summary>
        /// Native-Wayland player: bring the app's window to the front with keyboard
        /// focus, restoring it if minimized and switching to its workspace. With the
        /// DisplayXR GNOME extension version 17 or newer, GNOME does this only when
        /// the user's latest key press or click went to the app and the user has not
        /// left it since (Alt+Tab, Super+H, the dock, a click elsewhere; the app
        /// minimizing itself with <see cref="displayxr_linux_set_minimized"/> does not
        /// count), or when no window has focus. Otherwise focus stays where it is, a
        /// minimized window stays minimized, GNOME flags the window as wanting
        /// attention if it is covered or minimized, and the log says
        /// "window activate: refused". To show the window without focus, use
        /// <see cref="displayxr_linux_set_minimized"/>(0). Returns 1 when the request
        /// was sent.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int displayxr_linux_activate_window();

        /// <summary>
        /// Native-Wayland player: minimize the app's window (1) or restore it
        /// without focus (0; <see cref="displayxr_linux_activate_window"/> restores
        /// it with focus). Returns 1 when the request was sent.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int displayxr_linux_set_minimized(int minimized);

        /// <summary>
        /// Native-Wayland player: the app's window state as the DisplayXR GNOME
        /// extension reports it, whoever changed it (the app, or the user from the
        /// dock or the window menu): bit 1 kept above, bit 4 minimized. -1 when not
        /// known (X11 mode, before the session, no extension or one older than
        /// version 15).
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int displayxr_linux_window_state();

        /// <summary>
        /// The app's window as an xdg-desktop-portal <c>parent_window</c>:
        /// <c>"wayland:&lt;handle&gt;"</c> for a native-Wayland player (the window
        /// exported with xdg-foreign), <c>"x11:&lt;hex&gt;"</c> for an X11 player. Pass
        /// it to a portal request (RemoteDesktop / ScreenCast Start, Screenshot, ...)
        /// so GNOME opens the dialog on top of the app's window instead of wherever a
        /// window with no parent lands. Writes a NUL-terminated string to
        /// <paramref name="buf"/> and returns 1; 0 when the window is not known:
        /// <list type="bullet">
        /// <item>X11: until an XR session has bound the app's window (the plugin
        /// finds the window then, not before).</item>
        /// <item>Native Wayland: without an XR session, when the compositor has no
        /// <c>zxdg_exporter_v2</c>, while the window is hidden or minimized, and
        /// from the moment the compositor dropped the export (the window was
        /// hidden) until a frame or two after the window is shown again.</item>
        /// </list>
        /// It only reads a value the plugin keeps (no request to the compositor). It may
        /// wait briefly while an XR session is starting; otherwise it returns at once,
        /// from any thread. Ask for it right before each
        /// portal request rather than keeping it: a window shown anew gets a new
        /// handle. Pass it on verbatim: the compositor picks the Wayland handle
        /// (mutter: 32 random printable ASCII characters, spaces and quotes
        /// included). See <see cref="LinuxWindowHandle"/>.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int displayxr_linux_window_handle([Out] byte[] buf, int len);

        /// <summary><see cref="displayxr_linux_window_handle"/> as a string; "" when
        /// the window is not known (pass "" as <c>parent_window</c> then: the
        /// dialog opens unparented, as it would without this call).</summary>
        public static string LinuxWindowHandle()
        {
            var buf = new byte[256];
            if (displayxr_linux_window_handle(buf, buf.Length) == 0)
                return "";
            int n = System.Array.IndexOf(buf, (byte)0);
            return System.Text.Encoding.ASCII.GetString(buf, 0, n < 0 ? buf.Length : n);
        }

        /// <summary>Kinds of <see cref="LinuxFileDropEvent"/>.</summary>
        public const int LinuxFileDropEnter = 1, LinuxFileDropMove = 2, LinuxFileDropLeave = 3, LinuxFileDropDrop = 4;
        /// <summary><see cref="LinuxFileDropEvent.flags"/>: x, y (X root px) are known.</summary>
        public const int LinuxFileDropHasRoot = 1;
        /// <summary><see cref="LinuxFileDropEvent.flags"/>: stageX, stageY (GNOME's
        /// desktop px, mutter's stage) are known.</summary>
        public const int LinuxFileDropHasStage = 2;

        /// <summary>
        /// One file-drop event of a native-Wayland player
        /// (<see cref="displayxr_linux_file_drop_next"/>); the native DxrWlDropEvent.
        /// </summary>
        [StructLayout(LayoutKind.Sequential)]
        public struct LinuxFileDropEvent
        {
            /// <summary>1 enter (files dragged onto the window; paths), 2 move (the
            /// latest position), 3 leave (they left, or the drag was cancelled), 4
            /// drop (paths). Enter comes first, then moves, then leave or drop.</summary>
            public int kind;
            /// <summary><see cref="LinuxFileDropHasRoot"/> | <see cref="LinuxFileDropHasStage"/>.</summary>
            public int flags;
            /// <summary>The pointer in X root px, the screen space of the X11 code and
            /// of <see cref="displayxr_get_overlay_position"/> (HasRoot).</summary>
            public int x, y;
            /// <summary>The pointer in GNOME's desktop px (HasStage; needs the DisplayXR
            /// GNOME extension).</summary>
            public int stageX, stageY;
            /// <summary>The pointer in the window, device px from its top-left (the
            /// units of <see cref="displayxr_get_overlay_size"/>). Always set.</summary>
            public int windowX, windowY;
            /// <summary>Enter / drop: how many paths.</summary>
            public int pathCount;
            /// <summary>Enter / drop: the size of the paths buffer they need.</summary>
            public int pathsBytes;
        }

        /// <summary>
        /// Native-Wayland player: receive files dragged onto the app's window from a
        /// file manager (1), or stop (0). Poll the events with
        /// <see cref="LinuxFileDropNext"/> (or <see cref="displayxr_linux_file_drop_next"/>)
        /// every frame; they arrive while a DisplayXR session runs.
        /// <para><b>What it does to Unity's clipboard:</b> the first time the app loses
        /// the keyboard focus with drops on, the plugin's own Wayland data device
        /// replaces SDL's for the rest of the run (GNOME sends a drag to one data device
        /// per app, and making ours the one unlinks SDL's). From then on
        /// <c>GUIUtility.systemCopyBuffer</c> cannot read what other apps copy (on GNOME
        /// 50 it reads "" or an older text); Unity's own copies still read back. Read and
        /// write the clipboard with <see cref="LinuxClipboardText"/> /
        /// <see cref="SetLinuxClipboardText"/> then (<see cref="LinuxClipboardState"/> tells
        /// when: <see cref="LinuxClipboardStateSdlCut"/>; a <c>systemCopyBuffer</c> write
        /// crashed the test player inside SDL in this state). Turning drops off before the
        /// first focus loss leaves SDL's clipboard as it was; after it, SDL's does not
        /// come back (GNOME never re-links a replaced data device) and the plugin keeps
        /// its own for the clipboard.</para>
        /// Returns 1 when drops are on now; 0 when remembered until the session binds
        /// the window (or when turned off); -1 when unsupported: an X11 / XWayland
        /// player (XDND messages go to Unity's own X connection, which the plugin
        /// cannot read), or a compositor without wl_data_device_manager version 3.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int displayxr_linux_file_drop_enable(int enable);

        /// <summary>
        /// Native-Wayland player: takes the oldest file-drop event into
        /// <paramref name="ev"/>, and for enter / drop its paths into
        /// <paramref name="paths"/> (UTF-8, each NUL-terminated, one more NUL at the
        /// end). Returns the kind; 0 when there is none; -1 when
        /// <paramref name="pathsLen"/> is smaller than <c>ev.pathsBytes</c> (the event
        /// stays queued: call again with that much room). Any thread.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int displayxr_linux_file_drop_next(out LinuxFileDropEvent ev, [Out] byte[] paths, int pathsLen);

        private static readonly object s_FileDropLock = new object();
        private static byte[] s_FileDropBuf = new byte[4096];

        /// <summary><see cref="displayxr_linux_file_drop_next"/> with the paths as
        /// strings (empty for move / leave). False when there is no event.</summary>
        public static bool LinuxFileDropNext(out LinuxFileDropEvent ev, out string[] paths)
        {
            paths = System.Array.Empty<string>();
            lock (s_FileDropLock)
            {
                int kind = displayxr_linux_file_drop_next(out ev, s_FileDropBuf, s_FileDropBuf.Length);
                if (kind < 0)
                {
                    s_FileDropBuf = new byte[ev.pathsBytes];
                    kind = displayxr_linux_file_drop_next(out ev, s_FileDropBuf, s_FileDropBuf.Length);
                }
                if (kind <= 0)
                    return false;
                if (ev.pathsBytes > 0)
                {
                    var list = new System.Collections.Generic.List<string>(ev.pathCount);
                    int start = 0;
                    for (int i = 0; i < ev.pathsBytes; i++)
                    {
                        if (s_FileDropBuf[i] != 0)
                            continue;
                        if (i > start)
                            list.Add(System.Text.Encoding.UTF8.GetString(s_FileDropBuf, start, i - start));
                        start = i + 1;
                    }
                    paths = list.ToArray();
                }
                return true;
            }
        }

        /// <summary>What a <see cref="LinuxClipboardText"/> / <see cref="SetLinuxClipboardText"/>
        /// call came to (the native DXR_CLIP_* codes).</summary>
        public enum LinuxClipboardResult
        {
            /// <summary>Done.</summary>
            Done = 0,
            /// <summary>No plugin clipboard here: no DisplayXR session (or an X11 / XWayland
            /// player, or the editor).</summary>
            Unavailable = -1,
            /// <summary>SDL's clipboard still works (file drops never replaced it): use
            /// <c>GUIUtility.systemCopyBuffer</c>.</summary>
            UseSystemCopyBuffer = -2,
            /// <summary>The app does not have the keyboard focus: Wayland gives the clipboard to
            /// the focused app only. Without the focus, use the runtime's desktop-control
            /// clipboard (<see cref="DisplayXRDesktop.GetClipboardText"/> /
            /// <see cref="DisplayXRDesktop.SetClipboardText"/>, spec v3, under the user's consent).</summary>
            NotFocused = -3,
            /// <summary>Read: the text is on its way; call again (next frame on the main thread).</summary>
            Pending = -4,
            /// <summary>More than <see cref="LinuxClipboardMaxBytes"/>.</summary>
            TooLarge = -6,
            /// <summary>The clipboard's owner sent nothing in time, or another error.</summary>
            Failed = -7,
            /// <summary>Write: a text with a NUL character.</summary>
            Invalid = -8,
        }

        /// <summary>The most text a read or a write takes, in UTF-8 bytes (1 MiB).</summary>
        public const int LinuxClipboardMaxBytes = 1024 * 1024;
        /// <summary><see cref="LinuxClipboardState"/>: the plugin's clipboard serves reads and writes.</summary>
        public const int LinuxClipboardStateAvailable = 1;
        /// <summary><see cref="LinuxClipboardState"/>: the app has the keyboard focus.</summary>
        public const int LinuxClipboardStateFocused = 2;
        /// <summary><see cref="LinuxClipboardState"/>: SDL's data device was replaced (file drops):
        /// <c>GUIUtility.systemCopyBuffer</c> cannot read other apps' copies for the rest of the run.</summary>
        public const int LinuxClipboardStateSdlCut = 4;
        /// <summary><see cref="LinuxClipboardState"/>: the clipboard holds the plugin's last write.</summary>
        public const int LinuxClipboardStateOwned = 8;

        /// <summary>Native-Wayland player: the plugin clipboard's state, <c>LinuxClipboardState*</c> bits.</summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int displayxr_linux_clipboard_state();

        /// <summary>Native-Wayland player: reads the clipboard's text (see <see cref="LinuxClipboardText"/>):
        /// its length in bytes, NUL-terminated in <paramref name="buf"/>, or a negative
        /// <see cref="LinuxClipboardResult"/>; -5 when <paramref name="buf"/> is too small
        /// (<paramref name="needed"/> bytes; the text is kept for the next call). Waits up to
        /// <paramref name="timeoutMs"/>; pass 0 on Unity's main thread.</summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int displayxr_linux_clipboard_read([Out] byte[] buf, int len, int timeoutMs, out int needed);

        /// <summary>Native-Wayland player: puts <paramref name="len"/> bytes of UTF-8 text on the
        /// clipboard (0 empties it): 1, or a negative <see cref="LinuxClipboardResult"/>.</summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int displayxr_linux_clipboard_write(byte[] utf8, int len);

        /// <summary>Native-Wayland player: the plugin clipboard's state, <c>LinuxClipboardState*</c>
        /// bits; 0 with an older native plugin.</summary>
        public static int LinuxClipboardState()
        {
            try { return displayxr_linux_clipboard_state(); }
            catch (EntryPointNotFoundException) { return 0; }
        }

        /// <summary>
        /// Native-Wayland player: the clipboard's text, read through the plugin's own Wayland
        /// data device. For an app with file drops on, whose <c>GUIUtility.systemCopyBuffer</c>
        /// no longer reads other apps' copies (<see cref="displayxr_linux_file_drop_enable"/>);
        /// until that happens it answers <see cref="LinuxClipboardResult.UseSystemCopyBuffer"/>.
        /// <para>By the Wayland rules it reads only while the app has the keyboard focus
        /// (<see cref="LinuxClipboardResult.NotFocused"/> otherwise; for an app that reads the
        /// clipboard without the focus, e.g. after sending Ctrl+C to another app, use the runtime's
        /// <see cref="DisplayXRDesktop.GetClipboardText"/>). "" when the clipboard holds no text.</para>
        /// <para>Never blocks Unity's main thread: there it starts the read and answers
        /// <see cref="LinuxClipboardResult.Pending"/> until the text is in (call again next frame;
        /// usually the next call has it). On another thread it waits up to
        /// <paramref name="timeoutMs"/> (at most 3 s are used). Any thread; the text never reaches a log.</para>
        /// </summary>
        public static LinuxClipboardResult LinuxClipboardText(out string text, int timeoutMs = 1000)
        {
            text = null;
            if (DisplayXRDesktopCaptureTimeout.OnMainThread)
                timeoutMs = 0;
            var buf = new byte[4096]; // per call: a waiting worker never holds up the main thread
            try
            {
                for (;;)
                {
                    int rc = displayxr_linux_clipboard_read(buf, buf.Length, Math.Max(0, timeoutMs), out int needed);
                    if (rc == -5 && needed > buf.Length)
                    {
                        buf = new byte[needed]; // the text waits for this call
                        continue;
                    }
                    if (rc < 0)
                        return (LinuxClipboardResult)rc;
                    text = System.Text.Encoding.UTF8.GetString(buf, 0, rc);
                    return LinuxClipboardResult.Done;
                }
            }
            catch (EntryPointNotFoundException) { return LinuxClipboardResult.Unavailable; }
        }

        /// <summary>
        /// Native-Wayland player: puts <paramref name="text"/> on the clipboard through the plugin's
        /// own Wayland data device ("" or null empties it). Same rules as
        /// <see cref="LinuxClipboardText"/>: only once file drops replaced SDL's data device
        /// (<see cref="LinuxClipboardResult.UseSystemCopyBuffer"/> before: systemCopyBuffer works), and
        /// only while the app has the keyboard focus (without it, use
        /// <see cref="DisplayXRDesktop.SetClipboardText"/>). Returns at once; the text stays on the
        /// clipboard while the DisplayXR session runs (GNOME keeps a copy after that), until something
        /// else is copied. <see cref="LinuxClipboardStateOwned"/> confirms it a frame later.
        /// </summary>
        public static LinuxClipboardResult SetLinuxClipboardText(string text)
        {
            byte[] b = System.Text.Encoding.UTF8.GetBytes(text ?? "");
            try
            {
                int rc = displayxr_linux_clipboard_write(b, b.Length);
                return rc >= 0 ? LinuxClipboardResult.Done : (LinuxClipboardResult)rc;
            }
            catch (EntryPointNotFoundException) { return LinuxClipboardResult.Unavailable; }
        }

#endif

        /// <summary>
        /// The DisplayXR runtime's version as the plug-in read it when it created its
        /// OpenXR instance, "major.minor.patch" (e.g. "2.50.0"); "" before the first XR
        /// session (and with an older native plug-in). After a session ends it is still
        /// the version of the runtime that ran it. On Linux it is the release tag in the
        /// runtime's name, since Linux runtimes report 1.6.0 as their OpenXR
        /// runtimeVersion (displayxr-runtime#1790). Any platform, any thread.
        /// </summary>
        public static string RuntimeVersion()
        {
            try
            {
                var buf = new byte[32];
                if (dxr_prov_get_runtime_version(buf, buf.Length) == 0)
                    return "";
                int n = Array.IndexOf(buf, (byte)0);
                return System.Text.Encoding.ASCII.GetString(buf, 0, n < 0 ? buf.Length : n);
            }
            catch (EntryPointNotFoundException) { }
            catch (DllNotFoundException) { }
            return "";
        }

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        private static extern int dxr_prov_get_runtime_version([Out] byte[] buf, int len);

        // ---------------------------------------------------------------------
        // Linux desktop info (read-only): the monitors, the windows, the window at
        // a point, the pointer and the X root scale, for an app's agent on GNOME. Linux player only;
        // elsewhere (Windows, macOS, the editor) every call returns false / empty /
        // 0. It is only what the DisplayXR GNOME extension's read-only members
        // (runtime spec wayland-window-geometry.md §12.4) and mutter's DisplayConfig
        // give any client. No window titles and no UI text, by design: those come
        // from DisplayXRDesktop (ListWindows, GetTextAt) after the user's consent.
        // Consent policy (runtime ADR-056): the monitors and the X root scale are
        // always available. What tells the agent about OTHER apps (the window list,
        // the window at a point, the focused window, the pointer) needs this app's
        // desktop control GRANTED (DisplayXRDesktop.State == Granted): before that,
        // LinuxWindows / LinuxWindowAt / LinuxPointer return false (empty), with one
        // log line, "needs desktop control granted". None of them ever asks.
        // ---------------------------------------------------------------------

        /// <summary>
        /// One monitor of the desktop as GNOME lays it out (a mutter "logical
        /// monitor"). See <see cref="LinuxMonitors"/>.
        /// </summary>
        public struct LinuxMonitor
        {
            /// <summary>
            /// The monitor's rect in GNOME desktop coordinates: mutter's stage
            /// coordinates, top-left origin, y down. These are logical px in mutter's
            /// logical layout mode (fractional scaling; the default on GNOME 50) and
            /// device px in its physical layout mode. The same space as
            /// <see cref="LinuxWindowInfo.Frame"/>, <see cref="LinuxPointer"/> and
            /// DisplayXRDesktop on GNOME; multiply by <see cref="LinuxXRootScale"/>
            /// for X root coordinates.
            /// </summary>
            public RectInt Rect;

            /// <summary>
            /// Mutter's scale for this monitor (1, 1.25, 1.333..., 2, ...). In the
            /// logical layout mode one desktop px on this monitor is Scale device px
            /// (a screenshot's px); in the physical layout mode desktop px are device
            /// px already.
            /// </summary>
            public float Scale;

            /// <summary>The primary monitor (the one with GNOME's top bar).</summary>
            public bool Primary;

            /// <summary>Its connector name as mutter has it ("eDP-1", "HDMI-1", ...).
            /// A monitor mirrored on several connectors reports the first.</summary>
            public string Connector;
        }

        /// <summary>
        /// One window on the desktop, as the DisplayXR GNOME extension reports it. No
        /// title, by design (see the note above <see cref="LinuxMonitor"/>). See
        /// <see cref="LinuxWindows"/>.
        /// </summary>
        public struct LinuxWindowInfo
        {
            /// <summary>
            /// Mutter's window id: unique and stable for the window's lifetime (a
            /// window shown anew gets a new one). An unsigned 64-bit number, often
            /// above int.MaxValue. 0 with a DisplayXR GNOME extension older than
            /// version 16.
            /// </summary>
            public ulong Id;

            /// <summary>
            /// The window's frame (what the user sees as the window, a client-side
            /// title bar included; no shadow) in GNOME desktop coordinates, the space of
            /// <see cref="LinuxMonitor.Rect"/>. Reported for a minimized window too
            /// (where it would be).
            /// </summary>
            public RectInt Frame;

            /// <summary>The owning process; 0 when not known.</summary>
            public int Pid;

            /// <summary>The X11 WM_CLASS or the Wayland app id ("org.gnome.Nautilus",
            /// "firefox", ...); "" when the window has none. Cut at 127 bytes of UTF-8.</summary>
            public string AppId;

            /// <summary>It has the keyboard focus (at most one window does).</summary>
            public bool Focused;

            /// <summary>Minimized (by its app, the user or the dock). Always false with a
            /// DisplayXR GNOME extension older than version 15.</summary>
            public bool Minimized;

            /// <summary>
            /// Index of its monitor (the one mutter places it on: the one holding most
            /// of it) in the array <see cref="LinuxMonitors"/> returns; -1 when it is on
            /// no monitor or the monitor list is not available. Indexes the list as it
            /// is now: call LinuxMonitors and LinuxWindows together.
            /// </summary>
            public int Monitor;
        }

        /// <summary>
        /// Linux player: the desktop's monitors, from mutter's
        /// <c>org.gnome.Mutter.DisplayConfig</c> (GNOME only; X11 and native-Wayland
        /// players alike). In mutter's order; <see cref="LinuxMonitor.Primary"/> marks
        /// the primary. Returns false, with an empty array, when not available: not a
        /// Linux player, not a GNOME session, or no session bus.
        /// Any thread. The answer is cached for 250 ms; a call that refreshes it waits
        /// for a D-Bus round trip to gnome-shell (about a millisecond). A D-Bus call
        /// gives up after 250 ms, and after a timeout nothing is asked for a second
        /// (this list keeps its last answer meanwhile).
        /// </summary>
        public static unsafe bool LinuxMonitors(out LinuxMonitor[] monitors)
        {
            monitors = Array.Empty<LinuxMonitor>();
#if UNITY_STANDALONE_LINUX && !UNITY_EDITOR
            if (s_LinuxDesktopMissing) return false;
            try
            {
                var raw = t_LinuxMonitorBuf ?? (t_LinuxMonitorBuf = new NativeLinuxMonitor[8]);
                for (int attempt = 0; attempt < 3; attempt++)
                {
                    int n;
                    fixed (NativeLinuxMonitor* p = raw)
                        n = displayxr_linux_monitors(p, raw.Length);
                    if (n < 0) return false;
                    if (n > raw.Length) { raw = t_LinuxMonitorBuf = new NativeLinuxMonitor[n + 4]; continue; }
                    var result = new LinuxMonitor[n];
                    for (int i = 0; i < n; i++)
                    {
                        NativeLinuxMonitor m = raw[i];
                        result[i] = new LinuxMonitor
                        {
                            Rect = new RectInt(m.x, m.y, m.width, m.height),
                            Scale = m.scale,
                            Primary = m.primary != 0,
                            Connector = LinuxUtf8Z(m.connector, 32),
                        };
                    }
                    monitors = result;
                    return true;
                }
            }
            catch (EntryPointNotFoundException) { s_LinuxDesktopMissing = true; }
            catch (DllNotFoundException) { s_LinuxDesktopMissing = true; }
#endif
            return false;
        }

        /// <summary>
        /// Linux player: the windows on the desktop, in stacking order, TOP FIRST: index
        /// 0 is the window above all the others, and of two overlapping windows the one
        /// with the lower index covers the other. From the DisplayXR GNOME extension,
        /// which lists mutter's window actors, kept in stacking order (X11 and
        /// native-Wayland players alike; the app's own window included). To find the
        /// window at a point, use <see cref="LinuxWindowAt"/>: it also knows input
        /// regions (a click-through window) and shell UI. With extension version 16 or newer: normal
        /// windows, dialogs and utility windows (a "Save changes?" dialog is listed);
        /// older ones list normal windows only. Never menus, popups, tooltips, docks or
        /// the desktop. No titles (see <see cref="LinuxWindowInfo"/>). Returns false,
        /// with an empty array, when not available: not a Linux player, no DisplayXR
        /// GNOME extension (not installed, disabled, not GNOME), or no session bus.
        /// <b>Needs desktop control granted</b> (<see cref="DisplayXRDesktop.State"/> ==
        /// <see cref="DesktopControlState.Granted"/>): before that, false and an empty
        /// array (see the policy note above <see cref="LinuxMonitor"/>).
        /// Any thread. The answer is cached for 100 ms; a call that refreshes it waits
        /// for a D-Bus round trip or two (about a millisecond each; the monitor list is
        /// refreshed too when it is due). A D-Bus call gives up after 250 ms, and after
        /// a timeout nothing is asked for a second (false meanwhile).
        /// </summary>
        public static unsafe bool LinuxWindows(out LinuxWindowInfo[] windows)
        {
            windows = Array.Empty<LinuxWindowInfo>();
#if UNITY_STANDALONE_LINUX && !UNITY_EDITOR
            if (s_LinuxDesktopMissing || !LinuxDesktopControlGranted("LinuxWindows")) return false;
            try
            {
                var raw = t_LinuxWindowBuf ?? (t_LinuxWindowBuf = new NativeLinuxWindow[64]);
                for (int attempt = 0; attempt < 3; attempt++)
                {
                    int n;
                    fixed (NativeLinuxWindow* p = raw)
                        n = displayxr_linux_windows(p, raw.Length);
                    if (n < 0) return false;
                    if (n > raw.Length) { raw = t_LinuxWindowBuf = new NativeLinuxWindow[n + 16]; continue; }
                    var result = new LinuxWindowInfo[n];
                    for (int i = 0; i < n; i++)
                    {
                        NativeLinuxWindow w = raw[i];
                        result[i] = new LinuxWindowInfo
                        {
                            Id = w.id,
                            Frame = new RectInt(w.x, w.y, w.width, w.height),
                            Pid = w.pid,
                            AppId = LinuxUtf8Z(w.appId, 128),
                            Focused = w.focused != 0,
                            Minimized = w.minimized != 0,
                            Monitor = w.monitor,
                        };
                    }
                    windows = result;
                    return true;
                }
            }
            catch (EntryPointNotFoundException) { s_LinuxDesktopMissing = true; }
            catch (DllNotFoundException) { s_LinuxDesktopMissing = true; }
#endif
            return false;
        }

        /// <summary>
        /// Linux player: the app window a click at <paramref name="position"/> (GNOME
        /// desktop coordinates, the space of <see cref="LinuxMonitor.Rect"/>) would
        /// meet. With DisplayXR GNOME extension version 16 or newer this is mutter's own
        /// pick (GetWindowAt): stacking, input regions (a click-through window is passed
        /// over to the one behind it) and shell UI on top all count, as for a click. It
        /// can be a menu or popup that <see cref="LinuxWindows"/> does not list. With an
        /// older extension it is the topmost not-minimized window whose frame holds the
        /// point (from LinuxWindows). No title, as everywhere here.
        /// Returns false, and a default <paramref name="window"/>, when no app window is
        /// there: the desktop background (or a desktop-icons window), shell UI (the top
        /// bar, the overview), a point on no monitor, the screen locked; and when not
        /// available (not a Linux player, no extension), and while this app's desktop
        /// control is not <see cref="DesktopControlState.Granted"/>.
        /// Any thread. The same point is answered from a 100 ms cache; another point
        /// costs a D-Bus round trip (about a millisecond; a D-Bus call gives up after
        /// 250 ms).
        /// </summary>
        public static unsafe bool LinuxWindowAt(Vector2Int position, out LinuxWindowInfo window)
        {
            window = default;
#if UNITY_STANDALONE_LINUX && !UNITY_EDITOR
            if (s_LinuxDesktopMissing || !LinuxDesktopControlGranted("LinuxWindowAt")) return false;
            try
            {
                NativeLinuxWindow w;
                if (displayxr_linux_window_at(position.x, position.y, &w) != 1) return false;
                window = new LinuxWindowInfo
                {
                    Id = w.id,
                    Frame = new RectInt(w.x, w.y, w.width, w.height),
                    Pid = w.pid,
                    AppId = LinuxUtf8Z(w.appId, 128),
                    Focused = w.focused != 0,
                    Minimized = w.minimized != 0,
                    Monitor = w.monitor,
                };
                return true;
            }
            catch (EntryPointNotFoundException) { s_LinuxDesktopMissing = true; }
            catch (DllNotFoundException) { s_LinuxDesktopMissing = true; }
#endif
            return false;
        }

        /// <summary>
        /// Linux player: where the pointer is, in GNOME desktop coordinates (the space
        /// of <see cref="LinuxMonitor.Rect"/>), wherever it is on the desktop (a
        /// Wayland app is otherwise only told while the pointer is over its own
        /// window). From the DisplayXR GNOME extension, version 16 or newer. Returns
        /// false (and (0, 0)) when not available: not a Linux player, no extension or an
        /// older one, or the screen is locked; and while this app's desktop control is
        /// not <see cref="DesktopControlState.Granted"/>.
        /// Any thread. The answer is cached for 100 ms: after moving the pointer, ask
        /// again at least 100 ms later to see the new position.
        /// </summary>
        public static bool LinuxPointer(out Vector2Int position)
        {
            position = default;
#if UNITY_STANDALONE_LINUX && !UNITY_EDITOR
            if (s_LinuxDesktopMissing || !LinuxDesktopControlGranted("LinuxPointer")) return false;
            try
            {
                if (displayxr_linux_pointer(out int x, out int y) == 0) return false;
                position = new Vector2Int(x, y);
                return true;
            }
            catch (EntryPointNotFoundException) { s_LinuxDesktopMissing = true; }
            catch (DllNotFoundException) { s_LinuxDesktopMissing = true; }
#endif
            return false;
        }

        /// <summary>
        /// Linux player: X root px per GNOME desktop px. X root coordinates are what an
        /// X11 client sees, and what the plug-in's window-position API
        /// (<c>displayxr_get_overlay_position</c> / <c>displayxr_set_overlay_position</c>)
        /// speaks on Linux, on X11 and native-Wayland players alike:
        /// X root = desktop x this. 1 without XWayland scaling (and in an X11
        /// session); an integer N with mutter's xwayland-native-scaling at a scaled
        /// monitor. It is the factor the plug-in itself converts window positions
        /// with: the 3D panel's rect as the runtime reports it (X root px) against the
        /// same monitor's desktop rect; on an X11 player without an XR session, the X
        /// root window's size against the monitors' bounding box.
        /// 0 when not known: not a Linux player, no monitor list, or a native-Wayland
        /// player without the panel's rect (before its XR session reported it, or with
        /// no X server at all); the position API cannot convert then either.
        /// Any thread. Cached for 250 ms.
        /// </summary>
        public static float LinuxXRootScale()
        {
#if UNITY_STANDALONE_LINUX && !UNITY_EDITOR
            if (s_LinuxDesktopMissing) return 0f;
            try
            {
                return displayxr_linux_xroot_scale();
            }
            catch (EntryPointNotFoundException) { s_LinuxDesktopMissing = true; }
            catch (DllNotFoundException) { s_LinuxDesktopMissing = true; }
#endif
            return 0f;
        }

#if UNITY_STANDALONE_LINUX && !UNITY_EDITOR
        // Byte-for-byte mirrors of DisplayXRLinuxMonitor / DisplayXRLinuxWindow in
        // native~/displayxr_linux_desktop_info.h (its unit test pins 56 / 168 bytes).
        [StructLayout(LayoutKind.Sequential)]
        private unsafe struct NativeLinuxMonitor
        {
            public int x, y, width, height;
            public float scale;
            public int primary;
            public fixed byte connector[32];
        }

        [StructLayout(LayoutKind.Sequential)]
        private unsafe struct NativeLinuxWindow
        {
            public ulong id;
            public int x, y, width, height;
            public int pid, monitor, focused, minimized;
            public fixed byte appId[128];
        }

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        private static extern unsafe int displayxr_linux_monitors(NativeLinuxMonitor* buf, int len);

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        private static extern unsafe int displayxr_linux_windows(NativeLinuxWindow* buf, int len);

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        private static extern unsafe int displayxr_linux_window_at(int x, int y, NativeLinuxWindow* window);

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        private static extern int displayxr_linux_pointer(out int x, out int y);

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        private static extern float displayxr_linux_xroot_scale();

        // A native plug-in older than these exports: every call answers "not available".
        private static volatile bool s_LinuxDesktopMissing;

        // The consent gate (runtime ADR-056): what tells the agent about other apps needs
        // this app's desktop control granted. One log line per refusal streak, not per
        // call (agents poll these every frame); a grant re-arms it.
        private static volatile bool s_LinuxGateLogged;

        private static bool LinuxDesktopControlGranted(string what)
        {
            if (DisplayXRDesktop.State == DesktopControlState.Granted)
            {
                s_LinuxGateLogged = false;
                return true;
            }
            if (!s_LinuxGateLogged)
            {
                s_LinuxGateLogged = true;
                Debug.Log($"[DisplayXR] {what}: needs desktop control granted (DisplayXRDesktop.Request); " +
                          "returning nothing until then");
            }
            return false;
        }

        // Per-thread native buffers, reused: per-frame polling allocates only the result.
        [ThreadStatic] private static NativeLinuxMonitor[] t_LinuxMonitorBuf;
        [ThreadStatic] private static NativeLinuxWindow[] t_LinuxWindowBuf;

        private static unsafe string LinuxUtf8Z(byte* p, int cap)
        {
            int n = 0;
            while (n < cap && p[n] != 0) n++;
            return n == 0 ? "" : System.Text.Encoding.UTF8.GetString(p, n);
        }
#endif

    }
}
