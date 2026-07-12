// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Local2D overlay (#439/#491).
//
// Routes a Unity RenderTexture through to an XrCompositionLayerLocal2DDXR
// composition layer so the runtime composites it "glass over 3D" — the woven
// 3D under the layer's pixel rect goes flat 2D (implicit mask) and the 2D
// content is alpha-composited on top.
//
// C# registers the pending Unity texture + pixel rect via the C ABI setters
// below. Under the custom IUnityXRDisplay provider the layer is submitted by the
// provider itself (ps_submit_local2d in displayxr_provider_session.cpp). The
// former hooked submission path was removed in the Task-3 hook-backend cleanup
// (#166).

#pragma once

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
extern "C" {
#endif

// --- C ABI exports (Unity C# P/Invoke) ---------------------------------------

// Register the Unity RenderTexture whose contents become the Local2D layer.
// Safe to call every frame; a changed pointer/size lazily recreates the
// overlay swapchain. Passing null tex (or via _clear) disables the layer.
DISPLAYXR_EXPORT void displayxr_local2d_set_texture(void *unity_native_tex,
                                                    int width, int height);

// Set the destination rect in client-window PIXELS (post-DPI), matching the
// XrCompositionLayerLocal2DDXR::rect / mask-tier coordinate space.
DISPLAYXR_EXPORT void displayxr_local2d_set_rect(int x, int y, int width, int height);

// Disable the layer (stops submitting it this frame onward).
DISPLAYXR_EXPORT void displayxr_local2d_clear(void);

// Read the pending Unity texture (provider Metal Local2D path; mirrors
// displayxr_window_space_ui_get_pending). Returns 1 iff a non-zero-size texture is
// registered. The destination pixel rect comes from provider state, not from here.
DISPLAYXR_EXPORT int displayxr_local2d_get_pending(void **out_tex, int *out_w, int *out_h);

#ifdef __cplusplus
} // extern "C"
#endif
