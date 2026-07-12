// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Window-space UI overlay (issue #67).
//
// Routes a Unity Canvas RenderTexture through to a XrCompositionLayerWindowSpaceDXR
// composition layer so the runtime composites it on top of the eye projection.
//
// C# registers the pending Unity texture + fractional layer placement via the C
// ABI setters below; the custom IUnityXRDisplay provider reads that state via
// displayxr_window_space_ui_get_pending and drives its own overlay swapchain from
// its own session/device (ps_submit_wsui in displayxr_provider_session.cpp). The
// former hooked/standalone submission paths were removed in the Task-3
// hook-backend cleanup (#166).

#pragma once

#include <stdint.h>

// Local definition of DISPLAYXR_EXPORT (matches displayxr_exports.h). Defining
// it inline rather than including displayxr_exports.h avoids dragging that header
// into translation units that also include displayxr_win32.h, which redeclares
// some exports without __declspec(dllexport) — tripping MSVC C2375 "redefinition;
// different linkage".
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

// --- C ABI exports (called from Unity C# via P/Invoke) ------------------------

// Register the Unity-side native texture pointer that the overlay swapchain
// should mirror each frame. nativeTex is backend-typed:
//   - macOS Metal: id<MTLTexture>
//   - Windows D3D11: ID3D11Texture2D*
//   - Windows D3D12: ID3D12Resource*
// Pass NULL to deregister.
DISPLAYXR_EXPORT void displayxr_window_space_ui_set_texture(void *nativeTex, int width, int height);

// Update the layer descriptor (fractional window coords + per-eye disparity).
// May be called every frame; cheap.
DISPLAYXR_EXPORT void displayxr_window_space_ui_set_layer(float x, float y,
                                          float width, float height,
                                          float disparity);

// Clear the registered texture and mark the layer inactive.
DISPLAYXR_EXPORT void displayxr_window_space_ui_clear(void);

// (#166) Read the current pending wsui state (Unity RT native ptr + texture dims +
// fractional layer rect/disparity). Lets the custom IUnityXRDisplay provider drive
// its OWN window-space composition layer from its own session/device — the provider
// is neither the hooked nor the standalone session, but C# sets the same s_pending
// via set_texture/set_layer regardless. Returns 1 if a texture is registered (the
// layer should be shown this frame), 0 otherwise.
int displayxr_window_space_ui_get_pending(void **out_tex, int *out_tex_w, int *out_tex_h,
                                          float *out_x, float *out_y,
                                          float *out_lw, float *out_lh, float *out_disp);

#ifdef __cplusplus
}
#endif
