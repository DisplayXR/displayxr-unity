// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
//
// OpenXR function interception for Unity's OpenXR Feature hook mechanism.

#pragma once

#include <openxr/openxr.h>

#ifdef __cplusplus
extern "C" {
#endif

// Platform export macros
#if defined(_WIN32)
#define DISPLAYXR_EXPORT __declspec(dllexport)
#elif defined(__GNUC__)
#define DISPLAYXR_EXPORT __attribute__((visibility("default")))
#else
#define DISPLAYXR_EXPORT
#endif

/// Logging helper — writes to displayxr.log and OutputDebugString on Windows.
void displayxr_log(const char *fmt, ...);

/// Called by Unity's OpenXR Feature via HookGetInstanceProcAddr.
/// Stores the next xrGetInstanceProcAddr in the chain and returns our interceptor.
/// @param next The next xrGetInstanceProcAddr function pointer in the chain.
/// @return Our interceptor function pointer (cast to PFN_xrVoidFunction for C# IntPtr).
DISPLAYXR_EXPORT XrResult displayxr_hook_xrGetInstanceProcAddr(XrInstance instance,
                                                             const char *name,
                                                             PFN_xrVoidFunction *function);

/// Install the hook chain. Called once from C# with the next-in-chain function pointer.
/// @param next_gipa The next xrGetInstanceProcAddr in Unity's hook chain.
/// @return Our hook function pointer as PFN_xrVoidFunction (for C# IntPtr).
DISPLAYXR_EXPORT PFN_xrVoidFunction displayxr_install_hooks(PFN_xrGetInstanceProcAddr next_gipa);

// --- P/Invoke exports for C# ---

DISPLAYXR_EXPORT void displayxr_set_tunables(float ipd_factor,
                                           float parallax_factor,
                                           float perspective_factor,
                                           float virtual_display_height,
                                           float inv_convergence_distance,
                                           float fov_override,
                                           float near_z,
                                           float far_z,
                                           int camera_centric,
                                           int clip_at_display_plane);

DISPLAYXR_EXPORT void displayxr_get_display_info(float *display_width_m,
                                               float *display_height_m,
                                               uint32_t *pixel_width,
                                               uint32_t *pixel_height,
                                               float *nominal_x,
                                               float *nominal_y,
                                               float *nominal_z,
                                               float *scale_x,
                                               float *scale_y,
                                               int *is_valid);

DISPLAYXR_EXPORT void displayxr_get_eye_positions(float *lx,
                                                float *ly,
                                                float *lz,
                                                float *rx,
                                                float *ry,
                                                float *rz,
                                                int *is_tracked);

DISPLAYXR_EXPORT void displayxr_set_scene_transform(float pos_x,
                                                   float pos_y,
                                                   float pos_z,
                                                   float ori_x,
                                                   float ori_y,
                                                   float ori_z,
                                                   float ori_w,
                                                   float scale_x,
                                                   float scale_y,
                                                   float scale_z,
                                                   int enabled);

DISPLAYXR_EXPORT void displayxr_set_window_handle(void *handle);

DISPLAYXR_EXPORT void displayxr_set_editor_mode(int enabled);

/// (issue runtime-pvt #191 / displayxr-unity #57) Request the runtime's
/// transparent-background mode for the next OpenXR session. Sets the
/// transparentBackgroundEnabled field on XrWin32WindowBindingCreateInfoEXT
/// (XR_EXT_win32_window_binding spec_version 4) when the next session is
/// created, opting the bound HWND's swapchain into BitBlt (D3D11) or DComp
/// (D3D12) presentation.
///
/// Must be called BEFORE xrCreateSession runs (typically from
/// [RuntimeInitializeOnLoadMethod(SubsystemRegistration)] in C#).
/// On older runtimes that don't implement spec_version 4, the field is at
/// the end of the struct and is harmlessly ignored.
DISPLAYXR_EXPORT void displayxr_set_transparent_background(int enabled);

/// (issue displayxr-unity #85) Configure Unity's main render NSWindow for
/// transparent overlay mode on macOS. Pass enabled=1 to flip
/// `setOpaque:NO` + `backgroundColor=clearColor` so per-pixel alpha from
/// the camera reaches the desktop; pass 0 to restore the saved values.
///
/// The runtime configures the CAMetalLayer it presents into (via the
/// transparentBackgroundEnabled bit on the cocoa window binding), but
/// the NSWindow itself is the app's responsibility — Unity won't flip it
/// for us. Call from DisplayXRTransparentOverlay.OnEnable / OnDisable.
///
/// No-op on non-Apple platforms.
DISPLAYXR_EXPORT void displayxr_macos_configure_unity_nswindow(int enabled);

/// Move Unity's configured NSWindow by (dx, dy) screen points. Primitive
/// for app-driven borderless-window drag (right-mouse-drag, etc.). Cocoa
/// screen coords: +x right, +y up. No-op when no window is configured or
/// on non-Apple platforms.
DISPLAYXR_EXPORT void displayxr_macos_offset_window(int dx, int dy);

/// Recommended cursor-anchored window-drag API (v1.5.5+). Begin captures
/// the cursor's offset within the window; each Update snaps the window
/// origin so the cursor stays pinned to the same spot. Avoids the
/// scale/feedback issues of computing deltas in the app from
/// Mouse.current. App calls Begin on mouse-down, Update each frame while
/// held, End on mouse-up. All no-ops on non-Apple platforms / before
/// configure_unity_nswindow.
DISPLAYXR_EXPORT void displayxr_macos_begin_window_drag(void);
DISPLAYXR_EXPORT void displayxr_macos_update_window_drag(void);
DISPLAYXR_EXPORT void displayxr_macos_end_window_drag(void);

/// Toggle Unity's configured NSWindow between borderless (no title bar /
/// close / minimize / resize chrome) and the saved original style. Apps
/// that want an avatar / floating-window look call this with enabled=1
/// after configure_unity_nswindow. Cocoa's title-bar drag is gone with
/// the title bar — use the cursor-anchored drag API to move the window.
/// Save/restore is symmetric; set(0) restores. Idempotent.
DISPLAYXR_EXPORT void displayxr_macos_set_window_borderless(int enabled);

/// Hint the typed-swapchain substitution about Unity's project color space.
/// Must be called BEFORE Unity creates its OpenXR swapchains (i.e. from the
/// OpenXR feature's OnInstanceCreate).
///   1 = Linear color space → UNORM_SRGB typed siblings.
///   0 = Gamma color space  → UNORM typed siblings (no double gamma encoding).
/// Default is 1 (sRGB) for backward compatibility.
DISPLAYXR_EXPORT void displayxr_set_use_srgb_swapchain(int enabled);

/// Check whether the plugin is running in shell/IPC mode.
/// Detected via DISPLAYXR_SHELL_SESSION=1 environment variable.
/// Callable from C# via P/Invoke.
DISPLAYXR_EXPORT int displayxr_is_shell_mode(void);

/// Get shell-mode mouse button state (from WM_INPUT usButtonFlags).
/// Returns current button mask: bit 0 = left, bit 1 = right, bit 2 = middle.
/// Also returns mouse position (client coords from shell-forwarded WM_MOUSEMOVE).
DISPLAYXR_EXPORT void displayxr_get_shell_mouse_state(int *buttons, int *mouseX, int *mouseY);

DISPLAYXR_EXPORT void displayxr_set_viewport_size(uint32_t width, uint32_t height,
                                                  int32_t screen_x, int32_t screen_y);

/// Same as displayxr_set_viewport_size but marks native (WM_SIZE) as the
/// authoritative source, causing subsequent C# displayxr_set_viewport_size
/// calls to become no-ops.  Prevents stale Screen.width/height from
/// overwriting correct values during resize/fullscreen transitions.
DISPLAYXR_EXPORT void displayxr_set_viewport_size_native(uint32_t width, uint32_t height,
                                                         int32_t screen_x, int32_t screen_y);

DISPLAYXR_EXPORT int displayxr_request_display_mode(int mode_3d);

DISPLAYXR_EXPORT void displayxr_get_stereo_matrices(float *left_view,
                                                   float *left_proj,
                                                   float *right_view,
                                                   float *right_proj,
                                                   int *valid);

DISPLAYXR_EXPORT void displayxr_get_readback(uint8_t **pixels,
                                           uint32_t *width,
                                           uint32_t *height,
                                           int *ready);

DISPLAYXR_EXPORT void *displayxr_create_shared_texture(uint32_t width, uint32_t height);

DISPLAYXR_EXPORT void displayxr_destroy_shared_texture(void);

DISPLAYXR_EXPORT void displayxr_get_shared_texture(void **native_ptr,
                                                  uint32_t *width,
                                                  uint32_t *height,
                                                  int *ready);

/// Set the canvas output rect for shared texture compositing (play mode).
/// Calls xrSetSharedTextureOutputRectEXT on the runtime session.
DISPLAYXR_EXPORT void displayxr_set_canvas_rect(
    int32_t x, int32_t y, uint32_t w, uint32_t h);

/// Kill xrPollEvent forwarding immediately. Call from C# before session/instance
/// teardown to prevent use-after-free when the runtime is unloaded.
DISPLAYXR_EXPORT void displayxr_stop_polling(void);

/// Destroy the editor preview window (if one exists).
/// Call from C# before XR teardown to prevent the compositor from blocking.
DISPLAYXR_EXPORT void displayxr_destroy_preview_window(void);

#ifdef _WIN32
/// (issue #57) Toggle transparent overlay mode on the parent (Unity top-
/// level) HWND. Strips decorations, cloaks Unity, moves it off-screen so
/// click-through routes to desktop apps, and snaps the top-level
/// NOREDIRECTIONBITMAP overlay HWND to Unity's former rect. Transparency
/// itself comes from the runtime's DComp visuals + ALPHA_BLEND swapchain;
/// no OS color key. Mutually exclusive with shell mode.
/// @param enabled  Non-zero to enable, zero to restore the original styles.
/// @param topmost  Non-zero to add WS_EX_TOPMOST while enabled.
DISPLAYXR_EXPORT void displayxr_set_transparent_overlay(int enabled,
                                                        int topmost);

/// (issue #57) Update the rectangular hit-test region used while transparent
/// overlay mode is enabled. Inside the rect, WM_NCHITTEST returns HTCLIENT;
/// outside, HTTRANSPARENT (clicks fall through to whatever's behind).
/// Coordinates are client-space pixels (top-left origin).
DISPLAYXR_EXPORT void displayxr_set_overlay_hit_rect(int x, int y, int w, int h);

/// (issue #57) Per-pixel hit-test override for transparent overlay mode.
/// AND-ed with the rect check above in WM_NCHITTEST. Set from C# each frame
/// based on a Physics.Raycast at the polled cursor — lets clicks fall
/// through inside the AABB but outside the cube silhouette.
DISPLAYXR_EXPORT void displayxr_set_overlay_hit_active(int active);

/// (issue #57) Returns 1 if the OS foreground window belongs to our process,
/// 0 otherwise. Use to gate input handlers (WASD etc.) that should be
/// inactive when the user has clicked through the overlay to another app.
/// The IAT-hooked GetForegroundWindow inside Unity always returns Unity's
/// HWND for OpenXR purposes — this function calls the real OS GetForegroundWindow
/// from the plugin DLL (whose IAT is not patched) and reflects actual OS state.
DISPLAYXR_EXPORT int displayxr_is_our_process_foreground(void);

/// (issue #57) Read the overlay's current client size in pixels. The
/// overlay's screen rect can change at runtime (e.g. an app resizing it
/// itself) but Unity's Screen.width/height stay frozen at the off-screen
/// Unity HWND's dimensions — C# raycast code must use these values for
/// cursor → NDC conversion, not Screen.*. Returns (0, 0) if no overlay HWND.
DISPLAYXR_EXPORT void displayxr_get_overlay_size(int *width, int *height);

/// (issue #57) Read cursor position (overlay-client coords, top-left origin)
/// + mouse button state. Designed for transparent overlay mode where Unity's
/// New Input System Mouse.current.position is frozen because the cloaked
/// Unity HWND isn't OS-foreground (documented Unity limitation). Returns
/// (-1, -1) for clientX/Y if no overlay HWND. Buttons: bit 0 = left, 1 =
/// right, 2 = middle.
DISPLAYXR_EXPORT void displayxr_get_overlay_pointer(int *clientX, int *clientY,
                                                    int *buttons);

/// (v1.2.2) Atomically read + zero the overlay's accumulated mouse-wheel
/// delta. Returns Win32 raw units (120 per notch; positive = wheel forward).
/// Always 0 in opaque mode (no transparent overlay). The plugin no longer
/// self-resizes the overlay on wheel events — apps consume the delta here
/// and decide what to do (e.g. drive a DisplayXRDisplay rig's
/// virtualDisplayHeight for zoom-in-window).
DISPLAYXR_EXPORT int displayxr_consume_overlay_wheel_delta(void);
#endif // _WIN32

#ifdef __cplusplus
}
#endif
