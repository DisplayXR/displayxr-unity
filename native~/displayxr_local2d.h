// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
//
// Local2D overlay (#439/#491).
//
// Routes a Unity RenderTexture through to an XrCompositionLayerLocal2DEXT
// composition layer so the runtime composites it "glass over 3D" — the woven
// 3D under the layer's pixel rect goes flat 2D (implicit mask) and the 2D
// content is alpha-composited on top. This is the modern, mask-based path the
// native VK avatar uses for its speech bubble; it replaces the legacy 2D
// surround handoff (xrSetSharedTextureSurround2DFenceEXT) for in-canvas 2D.
//
// Mirrors displayxr_window_space_ui's hooked path: own overlay swapchain, lazy
// create, per-frame acquire/copy/release reusing the GraphicsBackend's wsui_*
// methods. The difference is the emitted layer type + a client-window PIXEL
// dest rect (vs the window-space layer's fractional coords + disparity).

#pragma once

#include <openxr/openxr.h>
#include "displayxr_extensions.h"
#include <stdint.h>

#ifndef DISPLAYXR_EXPORT
# if defined(_WIN32)
#  define DISPLAYXR_EXPORT __declspec(dllexport)
# elif defined(__GNUC__)
#  define DISPLAYXR_EXPORT __attribute__((visibility("default")))
# else
#  define DISPLAYXR_EXPORT
# endif
#endif

#ifdef __cplusplus

// Forward declaration of the backend abstract base (same as wsui).
class GraphicsBackend;

// Called from hooked_xrEndFrame just before the window-space pre-end-frame.
// Lazily creates the overlay swapchain (recreated when the Unity RT dims
// change), then acquires + copies + releases one image and populates
// state->local2d_layer so the hooked_xrEndFrame loop appends the layer.
void local2d_hooked_pre_end_frame(XrSession session, GraphicsBackend *backend);

// Called from hooked_xrDestroySession. Drops our cached swapchain handle.
void local2d_hooked_on_session_destroyed(void);

extern "C" {
#endif

// --- C ABI exports (Unity C# P/Invoke) ---------------------------------------

// Register the Unity RenderTexture whose contents become the Local2D layer.
// Safe to call every frame; a changed pointer/size lazily recreates the
// overlay swapchain. Passing null tex (or via _clear) disables the layer.
DISPLAYXR_EXPORT void displayxr_local2d_set_texture(void *unity_native_tex,
                                                    int width, int height);

// Set the destination rect in client-window PIXELS (post-DPI), matching the
// XrCompositionLayerLocal2DEXT::rect / mask-tier coordinate space.
DISPLAYXR_EXPORT void displayxr_local2d_set_rect(int x, int y, int width, int height);

// Disable the layer (stops submitting it this frame onward).
DISPLAYXR_EXPORT void displayxr_local2d_clear(void);

#ifdef __cplusplus
} // extern "C"
#endif
