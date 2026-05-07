// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
//
// Metal helpers for the standalone preview session (macOS).
// Window creation and GPU blit for atlas → swapchain.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Create a native NSWindow+NSView for the standalone preview session.
/// The runtime will create a CAMetalLayer overlay on the view.
/// @param width  Initial window width in pixels.
/// @param height Initial window height in pixels.
/// @return 1 on success, 0 on failure.
int displayxr_sa_metal_create_window(uint32_t width, uint32_t height);

/// Destroy the preview window.
void displayxr_sa_metal_destroy_window(void);

/// Get the NSView* for the standalone session window binding.
void *displayxr_sa_metal_get_view(void);

/// Get (or lazily create) an MTLCommandQueue for the standalone session.
/// Required for XrGraphicsBindingMetalKHR.
void *displayxr_sa_metal_get_command_queue(void);

/// Blit from a source MTLTexture to a destination MTLTexture using Metal.
/// Handles format conversion and size mismatch via blit encoder.
/// @param src_tex Source id<MTLTexture> (from Unity RenderTexture).
/// @param dst_tex Destination id<MTLTexture> (swapchain image).
/// @return 1 on success, 0 on failure.
int displayxr_sa_metal_blit(void *src_tex, void *dst_tex);

/// Get the display backing scale factor (Retina).
/// Returns 2.0 on macOS Retina, 1.0 on non-Retina.
float displayxr_sa_metal_get_backing_scale(void);

/// Query the preview window's current rect (backing pixels + screen position).
/// Used to update canvas state for render tiling and window-relative Kooima.
/// @param out_x    Screen x of window's content area (backing pixels).
/// @param out_y    Screen y of window's content area (backing pixels).
/// @param out_w    Width of window's content area (backing pixels).
/// @param out_h    Height of window's content area (backing pixels).
/// @return 1 if window exists, 0 otherwise.
int displayxr_sa_metal_get_window_rect(int32_t *out_x, int32_t *out_y,
                                        uint32_t *out_w, uint32_t *out_h);

/// Returns 1 if the user closed the preview window, 0 otherwise.
/// Resets the flag after reading.
int displayxr_sa_metal_window_was_closed(void);

/// Returns 1 if the preview window is currently being moved or resized.
int displayxr_sa_metal_window_is_interacting(void);

/// Cursor position in the preview window's content area as FRACTIONAL
/// coordinates (0..1, top-left origin). Returns 1 if the cursor is inside
/// the content area (out_fx/out_fy filled), 0 otherwise (out_fx and out_fy
/// set to -1.0). The fractional convention is what app-side input routers
/// already work with — wsui layer rects use the same fractional space — so
/// callers can hit-test directly without needing the window's pixel size.
int displayxr_sa_metal_get_preview_mouse_position(float *out_fx, float *out_fy);

/// Bitmask of currently pressed mouse buttons on the preview window
/// (bit 0 = left, 1 = right, 2 = middle). Polled live via
/// `[NSEvent pressedMouseButtons]` — works regardless of whether the
/// window has key focus, as long as the app is foreground.
int displayxr_sa_metal_get_preview_mouse_buttons(void);

#ifdef __cplusplus
}
#endif
