// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
//
// IOSurface helper for zero-copy GPU texture sharing on macOS.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Create a native preview window for editor Play Mode.
/// The runtime composites directly into this window.
/// @return NSView* cast to void*, or NULL on failure.
void *displayxr_metal_create_preview_window(uint32_t width, uint32_t height);

/// Destroy the editor Play Mode preview window.
void displayxr_metal_destroy_preview_window(void);

/// Get the app's main window NSView (for passing to the cocoa window binding).
/// Tries mainWindow, keyWindow, then first visible window.
/// @return NSView* cast to void*, or NULL if no window found.
void *displayxr_get_app_main_view(void);

#ifdef __cplusplus
}
#endif
