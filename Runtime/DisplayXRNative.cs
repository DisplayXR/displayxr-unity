// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0

using System;
using System.Runtime.InteropServices;

namespace DisplayXR
{
    /// <summary>
    /// P/Invoke bindings to the displayxr_unity native plugin.
    /// </summary>
    internal static class DisplayXRNative
    {
        private const string LibName = "displayxr_unity";

        /// <summary>
        /// Install OpenXR hook chain. Called once with the next xrGetInstanceProcAddr.
        /// Returns our hook function pointer.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern IntPtr displayxr_install_hooks(IntPtr nextGetInstanceProcAddr);

        /// <summary>
        /// Set stereo rig tunables from game thread.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void displayxr_set_tunables(
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
        internal static extern void displayxr_get_display_info(
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
        internal static extern void displayxr_get_eye_positions(
            out float lx, out float ly, out float lz,
            out float rx, out float ry, out float rz,
            out int isTracked);

        /// <summary>
        /// Set scene transform (parent camera pose + zoom) applied before Kooima.
        /// Chain: raw eyes → scene transform → tunables → Kooima.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void displayxr_set_scene_transform(
            float posX, float posY, float posZ,
            float oriX, float oriY, float oriZ, float oriW,
            float scaleX, float scaleY, float scaleZ,
            int enabled);

        /// <summary>
        /// Set the window handle for session creation (HWND on Win32, NSView* on macOS).
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void displayxr_set_window_handle(IntPtr handle);

        /// <summary>
        /// Set editor mode flag. When enabled, native code creates its own preview
        /// window instead of auto-detecting the app's window.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void displayxr_set_editor_mode(int enabled);

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
        internal static extern void displayxr_set_transparent_background(int enabled);

        /// <summary>
        /// (runtime-pvt #191 spec v5) Set the chroma-key color for the
        /// runtime's post-weave alpha-conversion shader pass.
        /// COLORREF (0x00BBGGRR). 0 = pass disabled.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void displayxr_set_transparent_chroma_key(uint color);

        /// <summary>
        /// Hint the typed-swapchain substitution about the project color space.
        /// 1 = Linear (UNORM_SRGB siblings); 0 = Gamma (UNORM siblings).
        /// Must be called BEFORE Unity creates its OpenXR swapchains.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void displayxr_set_use_srgb_swapchain(int enabled);

        /// <summary>
        /// Check whether the plugin is running in shell/IPC mode.
        /// Detected via DISPLAYXR_SHELL_SESSION=1 environment variable.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int displayxr_is_shell_mode();

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
        internal static extern int displayxr_is_our_process_foreground();

        /// <summary>
        /// (issue #57) Overlay's current client size in pixels. After scroll-
        /// resize the overlay's rect changes but Unity's Screen.width/height
        /// stay frozen at the off-screen Unity HWND's dimensions — raycast
        /// math must use these values, not Screen.*. Returns (0, 0) if no
        /// overlay HWND.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void displayxr_get_overlay_size(out int width, out int height);

        /// <summary>
        /// Get shell-mode mouse button state from native WM_INPUT tracking.
        /// Buttons: bit 0 = left, bit 1 = right, bit 2 = middle.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void displayxr_get_shell_mouse_state(
            out int buttons, out int mouseX, out int mouseY);

        /// <summary>
        /// Set the viewport (window) size and screen position for window-relative
        /// Kooima projection. Screen position is needed to compute the window-center
        /// eye offset on the physical display.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void displayxr_set_viewport_size(uint width, uint height,
            int screenX, int screenY);

        /// <summary>
        /// Request 2D or 3D display mode.
        /// </summary>
        /// <param name="mode3d">1 for 3D mode, 0 for 2D mode.</param>
        /// <returns>1 on success, 0 on failure or not supported.</returns>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int displayxr_request_display_mode(int mode3d);

        /// <summary>
        /// Get the Kooima stereo view and projection matrices computed by the native library.
        /// These are the matched matrix pairs that should be applied directly, bypassing
        /// Unity's matrix reconstruction from (fov, position, orientation).
        /// Matrices are column-major, OpenXR/OpenGL convention.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void displayxr_get_stereo_matrices(
            [MarshalAs(UnmanagedType.LPArray, SizeConst = 16)] float[] leftView,
            [MarshalAs(UnmanagedType.LPArray, SizeConst = 16)] float[] leftProj,
            [MarshalAs(UnmanagedType.LPArray, SizeConst = 16)] float[] rightView,
            [MarshalAs(UnmanagedType.LPArray, SizeConst = 16)] float[] rightProj,
            out int valid);

        /// <summary>
        /// Get readback pixel data from offscreen rendering.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void displayxr_get_readback(
            out IntPtr pixels,
            out uint width,
            out uint height,
            out int ready);

        /// <summary>
        /// Kill xrPollEvent forwarding. Call before session/instance teardown.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void displayxr_stop_polling();

        /// <summary>
        /// Destroy the editor preview window. Call before XR teardown to
        /// prevent the compositor from blocking on a stale window.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void displayxr_destroy_preview_window();

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
        internal static extern void displayxr_window_space_ui_set_texture(
            IntPtr nativeTex, int width, int height);

        /// <summary>
        /// Update the layer descriptor (fractional window coords + per-eye
        /// horizontal disparity). Cheap; safe to call every frame.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void displayxr_window_space_ui_set_layer(
            float x, float y, float width, float height, float disparity);

        /// <summary>
        /// Clear the registered texture and mark the layer inactive.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void displayxr_window_space_ui_clear();

        // ====================================================================
        // Native log callback (routes native messages to Debug.Log)
        // ====================================================================

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        internal delegate void LogCallback([MarshalAs(UnmanagedType.LPStr)] string message);

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void displayxr_standalone_set_log_callback(LogCallback callback);

        // ====================================================================
        // Standalone preview session (bypasses Unity's OpenXR loader)
        // ====================================================================

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int displayxr_standalone_window_was_closed();

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void displayxr_standalone_get_preview_mouse_state(
            out int buttons, out int wheelDelta);

        /// <summary>
        /// (v1.2.10+) Cursor position in the runtime preview window's content
        /// area as fractional coords (0..1, top-left origin). Returns 1 if
        /// the cursor is inside the content area, 0 otherwise (fx/fy = -1).
        /// Fractional convention matches wsui layer rects so callers can
        /// hit-test directly without needing the window's pixel size.
        /// Editor-preview only — built apps use Input.mousePosition.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int displayxr_standalone_get_preview_mouse_position(
            out float fx, out float fy);

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int displayxr_standalone_window_is_interacting();

        /// <summary>
        /// (Windows only) Set Unity's D3D12 device for atlas bridge texture.
        /// Must be called BEFORE displayxr_standalone_start().
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void displayxr_standalone_set_unity_device(IntPtr unityNativeTex);

        /// <summary>
        /// (Windows D3D12 only) Get the atlas bridge texture opened on Unity's device.
        /// C# uses Graphics.CopyTexture to copy the atlas RT into this each frame.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void displayxr_standalone_get_atlas_bridge_texture(
            out IntPtr nativePtr, out uint width, out uint height);

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int displayxr_standalone_start(
            [MarshalAs(UnmanagedType.LPStr)] string runtimeJsonPath);

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void displayxr_standalone_stop();

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int displayxr_standalone_is_running();

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void displayxr_standalone_poll();

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void displayxr_standalone_get_display_info(
            out float displayWidthM, out float displayHeightM,
            out uint pixelWidth, out uint pixelHeight,
            out float nominalX, out float nominalY, out float nominalZ,
            out float scaleX, out float scaleY,
            out int isValid);

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void displayxr_standalone_get_eye_positions(
            out float lx, out float ly, out float lz,
            out float rx, out float ry, out float rz,
            out int isTracked);

        /// <summary>
        /// Get the display backing scale factor (Retina).
        /// Returns 2.0 on macOS Retina, 1.0 on non-Retina or non-macOS.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern float displayxr_get_backing_scale_factor();

        // ====================================================================
        // Standalone frame loop (split: poll → begin → render → submit)
        // ====================================================================

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void displayxr_standalone_poll_events();

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int displayxr_standalone_begin_frame(out int shouldRender);

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int displayxr_standalone_submit_frame_atlas(IntPtr atlasTex);

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int displayxr_standalone_submit_frame(IntPtr leftTex, IntPtr rightTex);

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void displayxr_standalone_end_frame_empty();

        // ====================================================================
        // Standalone stereo views (Kooima projection via display3d library)
        // ====================================================================

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void displayxr_standalone_compute_views(
            uint viewCount,
            float nearZ, float farZ,
            [MarshalAs(UnmanagedType.LPArray)] float[] viewMatrices,
            [MarshalAs(UnmanagedType.LPArray)] float[] projMatrices,
            out int valid);

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void displayxr_standalone_get_swapchain_size(
            out uint width, out uint height);

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void displayxr_standalone_get_current_mode_info(
            out uint viewCount,
            out uint tileColumns, out uint tileRows,
            out uint viewWidthPixels, out uint viewHeightPixels,
            out float viewScaleX, out float viewScaleY,
            out int hardwareDisplay3D);

        // ====================================================================
        // Standalone tunables + display pose
        // ====================================================================

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void displayxr_standalone_set_tunables(
            float ipdFactor, float parallaxFactor, float perspectiveFactor,
            float virtualDisplayHeight, float invConvergenceDistance, float fovOverride,
            float nearZ, float farZ, int cameraCentric);

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void displayxr_standalone_set_display_pose(
            float posX, float posY, float posZ,
            float oriX, float oriY, float oriZ, float oriW,
            float scaleX, float scaleY, float scaleZ,
            int enabled);

        // ====================================================================
        // Standalone display mode switching
        // ====================================================================

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int displayxr_standalone_request_display_mode(int mode3d);

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int displayxr_standalone_request_rendering_mode(uint modeIndex);

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int displayxr_standalone_get_rendering_mode_name(
            uint arraySlot,
            [MarshalAs(UnmanagedType.LPArray)] byte[] buffer,
            uint bufferSize);

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int displayxr_standalone_get_preview_window_size(
            out uint width, out uint height);

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int displayxr_standalone_is_key_pressed(int ascii);

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int displayxr_standalone_enumerate_rendering_modes(
            uint capacity, out uint count,
            [MarshalAs(UnmanagedType.LPArray)] uint[] modeIndices,
            IntPtr modeNames,
            [MarshalAs(UnmanagedType.LPArray)] uint[] viewCounts,
            [MarshalAs(UnmanagedType.LPArray)] uint[] tileColumns,
            [MarshalAs(UnmanagedType.LPArray)] uint[] tileRows,
            [MarshalAs(UnmanagedType.LPArray)] uint[] viewWidthPixels,
            [MarshalAs(UnmanagedType.LPArray)] uint[] viewHeightPixels,
            [MarshalAs(UnmanagedType.LPArray)] float[] viewScaleX,
            [MarshalAs(UnmanagedType.LPArray)] float[] viewScaleY,
            [MarshalAs(UnmanagedType.LPArray)] int[] hardwareDisplay3D);

        // ====================================================================
        // Transparent overlay mode (issue #57) — Windows only
        //
        // Chroma-key transparent overlay for desktop avatar use cases. The
        // Leia weaver drops alpha and the D3D11 compositor uses
        // DXGI_ALPHA_MODE_IGNORE, so true per-pixel alpha doesn't survive
        // end-to-end. Workaround: render a magic color (default magenta) in
        // transparent regions of both eye views and let the layered window
        // (LWA_COLORKEY) punch those pixels through to the desktop.
        // ====================================================================

#if UNITY_STANDALONE_WIN || UNITY_EDITOR_WIN
        /// <summary>
        /// Toggle chroma-key transparent overlay mode on the parent (Unity
        /// top-level) HWND. Mutually exclusive with shell mode.
        /// </summary>
        /// <param name="enabled">1 to enable, 0 to restore original styles.</param>
        /// <param name="colorKey">Windows COLORREF (0x00BBGGRR). Typical: 0x00FF00FF (magenta).</param>
        /// <param name="topmost">1 to add WS_EX_TOPMOST while enabled.</param>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void displayxr_set_transparent_overlay(
            int enabled, uint colorKey, int topmost);

        /// <summary>
        /// Set the rectangular hit-test region for transparent overlay mode.
        /// Inside the rect → HTCLIENT; outside → HTTRANSPARENT (click-through).
        /// Coordinates are client-space pixels (top-left origin).
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void displayxr_set_overlay_hit_rect(
            int x, int y, int w, int h);

        /// <summary>
        /// Per-pixel hit-test override for transparent overlay mode. AND-ed
        /// with the rect check in WM_NCHITTEST. Set to 1 when the cursor is
        /// over the cube silhouette (e.g. via Physics.Raycast) and 0 when
        /// in a transparent zone — lets clicks fall through to the desktop
        /// even inside the cube's bounding rect.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void displayxr_set_overlay_hit_active(int active);

        /// <summary>
        /// Read cursor position (overlay-client coords, top-left origin) and
        /// mouse button state. Designed for transparent overlay mode where
        /// Unity's New Input System Mouse.current.position is frozen because
        /// the cloaked Unity HWND isn't OS-foreground (documented Unity
        /// limitation). Returns (-1, -1) for clientX/Y if no overlay HWND.
        /// Buttons: bit 0 = left, 1 = right, 2 = middle.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void displayxr_get_overlay_pointer(
            out int clientX, out int clientY, out int buttons);

        /// <summary>
        /// (v1.2.2) Atomically read + zero the overlay's accumulated mouse-
        /// wheel delta. Returns Win32 raw units (120 per notch; positive =
        /// wheel forward). Always 0 in opaque mode (no transparent overlay).
        /// The plugin no longer self-resizes the overlay on wheel events;
        /// apps consume this value and decide what to do with it.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int displayxr_consume_overlay_wheel_delta();
#endif

    }
}
