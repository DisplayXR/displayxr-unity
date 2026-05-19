// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
//
// Standalone OpenXR session for editor preview.
// Loads the DisplayXR runtime directly (bypassing Unity's OpenXR loader)
// and manages its own instance/session lifecycle.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#define DISPLAYXR_EXPORT __declspec(dllexport)
#elif defined(__GNUC__)
#define DISPLAYXR_EXPORT __attribute__((visibility("default")))
#else
#define DISPLAYXR_EXPORT
#endif

// ============================================================================
// Log callback (routes native messages to Unity's Debug.Log)
// ============================================================================

/// Signature for the log callback. The string is UTF-8, not null-terminated
/// beyond the provided length (but in practice it is null-terminated).
typedef void (*DisplayXRLogCallback)(const char *message);

/// Register a log callback. When set, all [DisplayXR-SA] messages are routed
/// through this callback instead of (in addition to) stderr.
DISPLAYXR_EXPORT void displayxr_standalone_set_log_callback(DisplayXRLogCallback callback);

// ============================================================================
// Session lifecycle
// ============================================================================

/// (Windows only) Set Unity's graphics device for atlas bridge texture.
/// Must be called BEFORE displayxr_standalone_start().
/// The pointer is auto-detected as either an ID3D12Resource* (if the editor
/// runs on D3D12) or an ID3D11Texture2D* (if the editor runs on D3D11), and
/// the corresponding device is extracted via GetDevice().
/// @param unity_native_tex A native texture pointer from Unity.
DISPLAYXR_EXPORT void displayxr_standalone_set_unity_device(void *unity_native_tex);

/// Get the atlas bridge texture opened on Unity's device (Windows only).
/// Returns ID3D11Texture2D* if Unity is on D3D11, ID3D12Resource* if on D3D12.
/// C# passes this to Texture2D.CreateExternalTexture and uses Graphics.CopyTexture
/// to copy the atlas RT into it each frame.
DISPLAYXR_EXPORT void displayxr_standalone_get_atlas_bridge_texture(
    void **native_ptr, uint32_t *width, uint32_t *height);

/// Get the window-space UI bridge texture opened on Unity's device.
/// Caller passes desired RT dimensions; the backend lazily creates a
/// shared bridge texture matching them on the SA device and opens it on
/// Unity's device. Returns ID3D11Texture2D* / ID3D12Resource* (Windows)
/// or NULL on platforms with unified device model (Metal — no bridge
/// needed, C# uses the original wsui_set_texture path).
/// C# wraps via Texture2D.CreateExternalTexture and copies its wsui RT
/// into it each frame; the plugin's per-frame xrEndFrame hook then copies
/// SA-side bridge to the composition-layer swapchain image.
DISPLAYXR_EXPORT void displayxr_standalone_get_wsui_bridge_texture(
    uint32_t width, uint32_t height,
    void **native_ptr, uint32_t *out_width, uint32_t *out_height);

/// Start a standalone OpenXR session for editor preview.
/// Loads the runtime from the JSON manifest, creates instance/session,
/// and opens a native preview window for compositor output.
/// @param runtime_json_path Path to the OpenXR runtime JSON manifest.
/// @return 1 on success, 0 on failure.
DISPLAYXR_EXPORT int displayxr_standalone_start(const char *runtime_json_path);

/// Stop the standalone session and release all resources.
/// Safe to call even if not running.
DISPLAYXR_EXPORT void displayxr_standalone_stop(void);

/// Whether the standalone session is currently running.
DISPLAYXR_EXPORT int displayxr_standalone_is_running(void);

// ============================================================================
// Frame loop (called each EditorApplication.update tick)
//
// Flow: poll_events → begin_frame → [C# renders cameras] → submit_frame
// ============================================================================

/// Poll OpenXR events (session state changes, etc). Does NOT drive frames.
DISPLAYXR_EXPORT void displayxr_standalone_poll_events(void);

/// Begin a new frame: xrWaitFrame + xrBeginFrame + xrLocateViews.
/// @param should_render Set to 1 if the app should render, 0 if not.
/// @return 1 on success, 0 on failure or session not ready.
DISPLAYXR_EXPORT int displayxr_standalone_begin_frame(int *should_render);

/// Submit a rendered frame with tiled atlas projection layers.
/// Acquires the single atlas swapchain image, blits from the provided atlas
/// texture, releases, and calls xrEndFrame with N tiled projection views.
/// @param atlas_tex Native texture pointer (id<MTLTexture>) for the tiled atlas.
/// @return 1 on success, 0 on failure.
DISPLAYXR_EXPORT int displayxr_standalone_submit_frame_atlas(void *atlas_tex);

/// Legacy submit (backward compatibility — deprecated, submits empty frame).
DISPLAYXR_EXPORT int displayxr_standalone_submit_frame(void *left_tex, void *right_tex);

/// End the current frame with no layers (keeps session alive when not rendering).
DISPLAYXR_EXPORT void displayxr_standalone_end_frame_empty(void);

// ============================================================================
// Stereo view computation (Kooima projection via display3d library)
// ============================================================================

/// Compute Kooima view and projection matrices for N views.
/// Views are processed in stereo pairs. Matrices are column-major.
/// @param view_count  Number of views to compute.
/// @param near_z      Near clip plane distance (meters).
/// @param far_z       Far clip plane distance (meters).
/// @param view_matrices  Output float[view_count * 16] view matrices.
/// @param proj_matrices  Output float[view_count * 16] projection matrices.
/// @param valid          Set to 1 if matrices are valid, 0 if not.
DISPLAYXR_EXPORT void displayxr_standalone_compute_views(
    uint32_t view_count,
    float near_z, float far_z,
    float *view_matrices, float *proj_matrices,
    int *valid);

// ============================================================================
// Tunables + display pose (mirror of hook chain's set_tunables/set_scene_transform)
// ============================================================================

/// Set stereo tunables for the standalone session.
DISPLAYXR_EXPORT void displayxr_standalone_set_tunables(
    float ipd_factor, float parallax_factor, float perspective_factor,
    float virtual_display_height, float inv_convergence_distance, float fov_override,
    float near_z, float far_z, int camera_centric);

/// Set display pose (parent camera transform) for the standalone session.
DISPLAYXR_EXPORT void displayxr_standalone_set_display_pose(
    float pos_x, float pos_y, float pos_z,
    float ori_x, float ori_y, float ori_z, float ori_w,
    float scale_x, float scale_y, float scale_z,
    int enabled);

// ============================================================================
// Queries
// ============================================================================

/// Get display info from the standalone session.
DISPLAYXR_EXPORT void displayxr_standalone_get_display_info(
    float *display_width_m, float *display_height_m,
    uint32_t *pixel_width, uint32_t *pixel_height,
    float *nominal_x, float *nominal_y, float *nominal_z,
    float *scale_x, float *scale_y,
    int *is_valid);

/// Get eye positions from the standalone session.
DISPLAYXR_EXPORT void displayxr_standalone_get_eye_positions(
    float *lx, float *ly, float *lz,
    float *rx, float *ry, float *rz,
    int *is_tracked);

/// Get the display backing scale factor (Retina).
/// Returns 2.0 on macOS Retina, 1.0 on non-Retina or non-macOS.
DISPLAYXR_EXPORT float displayxr_get_backing_scale_factor(void);

/// Returns 1 if the user closed the preview window, 0 otherwise.
/// Resets the flag after reading.
DISPLAYXR_EXPORT int displayxr_standalone_window_was_closed(void);

/// Get mouse button state tracked by the preview window's WndProc.
/// Used by the C# input controller in editor preview mode (where Unity's
/// new Input System doesn't reliably see clicks because the click target
/// is our preview window, not Unity's).
/// @param buttons  Bitmask: 1 = left, 2 = right, 4 = middle (currently held).
/// @param wheel_delta  Accumulated wheel delta since last call (consumed).
DISPLAYXR_EXPORT void displayxr_standalone_get_preview_mouse_state(
    int *buttons, int *wheel_delta);

/// Cursor position in the preview window's content area as FRACTIONAL
/// coords (0..1, top-left origin). Returns 1 if the cursor is inside the
/// content area (out_fx/out_fy filled), 0 otherwise (set to -1.0). The
/// fractional convention matches wsui layer rects, so app-side input
/// routers can hit-test directly without needing the window's pixel size.
DISPLAYXR_EXPORT int displayxr_standalone_get_preview_mouse_position(
    float *out_fx, float *out_fy);

/// Returns 1 if the preview window is being moved or resized.
DISPLAYXR_EXPORT int displayxr_standalone_window_is_interacting(void);

/// Get atlas swapchain dimensions (for creating matching RenderTextures in C#).
DISPLAYXR_EXPORT void displayxr_standalone_get_swapchain_size(
    uint32_t *width, uint32_t *height);

/// Get current rendering mode tiling info.
DISPLAYXR_EXPORT void displayxr_standalone_get_current_mode_info(
    uint32_t *view_count,
    uint32_t *tile_columns, uint32_t *tile_rows,
    uint32_t *view_width_pixels, uint32_t *view_height_pixels,
    float *view_scale_x, float *view_scale_y,
    int *hardware_display_3d);

/// Get per-view eye positions from the standalone preview's last
/// xrLocateViews call. Unlike displayxr_standalone_get_eye_positions
/// (2-eye L/R helper, kept for back-compat with existing C# bindings),
/// this surfaces all N view positions the preview session received
/// from xrLocateViews — viewCount = 2 for standard 3D, 4 for quad,
/// 8 for lenticular, etc.
///
/// Positions are in LOCAL space (display-relative, OpenXR right-hand
/// −Z forward), packed as float[capacity*3] = (x0,y0,z0, x1,y1,z1, …).
/// Output count is min(located_view_count, capacity).
///
/// @param out_xyz_array   Output buffer, capacity*3 floats.
/// @param capacity        Max views the caller can hold.
/// @param out_count       Actual view count written (may be 0).
/// @param out_tracked     1 if the last locateViews tracked, 0 otherwise.
DISPLAYXR_EXPORT void displayxr_standalone_get_n_eye_positions(
    float *out_xyz_array,
    uint32_t capacity,
    uint32_t *out_count,
    int32_t *out_tracked);

// ============================================================================
// Display mode switching
// ============================================================================

/// Request 2D or 3D display mode.
/// @param mode_3d 1 for 3D, 0 for 2D.
/// @return 1 on success, 0 on failure or not supported.
DISPLAYXR_EXPORT int displayxr_standalone_request_display_mode(int mode_3d);

/// Request a vendor-specific rendering mode by index.
/// @param mode_index Mode index from enumeration (0 = standard).
/// @return 1 on success, 0 on failure or not supported.
DISPLAYXR_EXPORT int displayxr_standalone_request_rendering_mode(uint32_t mode_index);

/// Fetch the runtime-reported display name for the rendering mode at the
/// given enumeration array slot (0..count-1). Bypasses the awkward
/// marshalling of `char[][256]` from managed code: callers pass a flat
/// byte buffer + capacity and parse a null-terminated UTF-8 string out.
/// @param array_slot   Enumeration slot from displayxr_standalone_enumerate_rendering_modes.
/// @param buffer       Output byte buffer (filled with null-terminated UTF-8).
/// @param buffer_size  Capacity of buffer in bytes (including terminator).
/// @return 1 on success (buffer is null-terminated), 0 on failure (buffer[0] = '\0').
DISPLAYXR_EXPORT int displayxr_standalone_get_rendering_mode_name(
    uint32_t array_slot, char *buffer, uint32_t buffer_size);

/// Preview window content-area size in backing pixels (Mac) / client
/// pixels (Win). Returns 1 on success, 0 if the window doesn't exist
/// or the platform isn't supported.
DISPLAYXR_EXPORT int displayxr_standalone_get_preview_window_size(
    uint32_t *out_w, uint32_t *out_h);

/// Polls the OS for whether the given ASCII-coded key is currently
/// pressed (focus-independent). Letters and digits only — pass uppercase
/// ASCII ('V' = 0x56, '0' = 0x30, etc.). For app-side hotkeys that
/// should fire while the standalone preview NSWindow has focus.
/// @return 1 if pressed, 0 otherwise.
DISPLAYXR_EXPORT int displayxr_standalone_is_key_pressed(int ascii);

/// Enumerate available rendering modes with full metadata.
/// Two-call pattern: first call with capacity=0 to get count,
/// then allocate and call again. Extended arrays are optional (may be NULL).
/// @param capacity            Array capacity (0 for count-only query).
/// @param count               Output: number of modes available.
/// @param mode_indices        Output array of mode indices.
/// @param mode_names          Output array of mode name strings (256 chars each).
/// @param view_counts         Output array of view counts per mode (optional).
/// @param tile_columns        Output array of tile column counts (optional).
/// @param tile_rows           Output array of tile row counts (optional).
/// @param view_width_pixels   Output array of per-view pixel widths (optional).
/// @param view_height_pixels  Output array of per-view pixel heights (optional).
/// @param view_scale_x        Output array of horizontal view scales (optional).
/// @param view_scale_y        Output array of vertical view scales (optional).
/// @param hardware_display_3d Output array of hardware 3D flags (optional).
/// @return 1 on success, 0 on failure.
DISPLAYXR_EXPORT int displayxr_standalone_enumerate_rendering_modes(
    uint32_t capacity, uint32_t *count,
    uint32_t *mode_indices, char (*mode_names)[256],
    uint32_t *view_counts,
    uint32_t *tile_columns, uint32_t *tile_rows,
    uint32_t *view_width_pixels, uint32_t *view_height_pixels,
    float *view_scale_x, float *view_scale_y,
    int *hardware_display_3d);

#ifdef __cplusplus
}
#endif
