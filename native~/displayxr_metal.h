// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// IOSurface helper for zero-copy GPU texture sharing on macOS.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Create a native preview window for editor Play Mode.
/// The runtime composites directly into this window.
/// Pass width/height == 0 to auto-size: the largest content rect with the
/// main display's aspect that fits the screen's usable area, centered.
/// @return NSView* cast to void*, or NULL on failure.
void *displayxr_metal_create_preview_window(uint32_t width, uint32_t height);

/// Destroy the editor Play Mode preview window.
void displayxr_metal_destroy_preview_window(void);

/// Remove the built-player in-app weave overlay from Unity's window (teardown).
void displayxr_metal_destroy_app_overlay(void);

/// Live backing-pixel size of an NSView (bounds converted to backing pixels).
/// Safe to poll from the render thread; matches the runtime's per-frame
/// canvas source so provider and compositor agree on the per-view size.
/// @return 1 if a non-zero size was written, 0 otherwise.
int displayxr_metal_view_backing_size(void *view, uint32_t *out_w, uint32_t *out_h);

/// Get the app's main window NSView (for passing to the cocoa window binding).
/// Tries mainWindow, keyWindow, then first visible window.
/// @return NSView* cast to void*, or NULL if no window found.
void *displayxr_get_app_main_view(void);

/// Get the app's main window content rect in screen-coord backing pixels.
/// Uses the same NSWindow as displayxr_get_app_main_view. Cocoa convention:
/// origin is bottom-left of the primary screen, Y-up. Output is suitable for
/// feeding into displayxr_set_viewport_size_native for window-relative Kooima.
/// @return 1 if the rect is valid, 0 if no window is available.
int displayxr_metal_get_app_window_rect(int32_t *out_x, int32_t *out_y,
                                         uint32_t *out_w, uint32_t *out_h);

/// Hooked-path Metal blit helper (issue #67).
/// Same-format blit src → dst using a caller-provided MTLCommandQueue.
/// Returns 1 on success. Differs from displayxr_sa_metal_blit by accepting
/// an explicit queue (the hooked path can't use the standalone module's
/// static queue — different session, possibly different MTLDevice).
int displayxr_metal_blit_textures(void *queue, void *src_tex, void *dst_tex);

#ifdef __cplusplus
}
#endif
