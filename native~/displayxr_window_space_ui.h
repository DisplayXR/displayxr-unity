// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
//
// Window-space UI overlay (issue #67).
//
// Routes a Unity Canvas RenderTexture through to a XrCompositionLayerWindowSpaceEXT
// composition layer so the runtime composites it on top of the eye projection.
//
// Two xrEndFrame call sites need wiring:
//   - hooked path (built apps with Unity's OpenXR loader)
//   - standalone path (editor preview window + Play Mode with PlayModeIntegration)
//
// Each path owns its own overlay swapchain (different OpenXR sessions), but the
// pending Unity texture pointer + layer placement is shared.

#pragma once

#include <openxr/openxr.h>
#include "displayxr_extensions.h"
#include <stdint.h>

#ifdef __cplusplus

// Forward declarations of backend abstract bases.
class GraphicsBackend;
class StandaloneGraphicsBackend;

// --- Hooked path (Unity OpenXR loader) ----------------------------------------

// Called from hooked_xrEndFrame just before the existing window_layers iteration.
// Lazily creates the overlay swapchain (one-shot, on first call after session is
// alive), then acquires + copies + releases one image and populates
// state->window_layers[0] so the existing 836-880 loop submits the layer.
void wsui_hooked_pre_end_frame(XrSession session, GraphicsBackend *backend);

// Called from hooked_xrDestroySession. Drops our cached swapchain handle (the
// runtime tears it down as part of session destruction).
void wsui_hooked_on_session_destroyed(void);

// --- Standalone path (preview / Play Mode integration) ------------------------

// Function-pointer bag the standalone path passes in. Avoids dragging
// StandaloneState into this header.
struct WsuiStandaloneFns {
	PFN_xrEnumerateSwapchainFormats pfn_enumerate_formats;
	PFN_xrCreateSwapchain pfn_create_swapchain;
	PFN_xrDestroySwapchain pfn_destroy_swapchain;
	PFN_xrEnumerateSwapchainImages pfn_enumerate_images;
	PFN_xrAcquireSwapchainImage pfn_acquire;
	PFN_xrWaitSwapchainImage pfn_wait;
	PFN_xrReleaseSwapchainImage pfn_release;
};

// Returns 1 if a window-space layer is active and was filled into out_layer.
// Caller appends out_layer to its xrEndFrame layer array.
int wsui_standalone_pre_end_frame(XrSession session,
                                   const WsuiStandaloneFns *fns,
                                   StandaloneGraphicsBackend *backend,
                                   XrCompositionLayerWindowSpaceEXT *out_layer);

// Called when the standalone session is being torn down. If
// `pfn_destroy_swapchain` is non-null we explicitly destroy our cached
// swapchain (matches how destroy_atlas_swapchain is called before
// pfn_destroy_session in displayxr_standalone.cpp).
void wsui_standalone_on_session_destroyed(PFN_xrDestroySwapchain destroy_fn);

#endif // __cplusplus

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
void displayxr_window_space_ui_set_texture(void *nativeTex, int width, int height);

// Update the layer descriptor (fractional window coords + per-eye disparity).
// May be called every frame; cheap.
void displayxr_window_space_ui_set_layer(float x, float y,
                                          float width, float height,
                                          float disparity);

// Clear the registered texture and mark the layer inactive.
void displayxr_window_space_ui_clear(void);

#ifdef __cplusplus
}
#endif
