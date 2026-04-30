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
                                           int camera_centric);

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

#ifdef __cplusplus
}
#endif
