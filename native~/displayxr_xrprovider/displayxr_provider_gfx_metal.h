// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Metal glue for the display provider (macOS parity epic #202, Phase 2 #204).
//
// All ObjC lives in displayxr_provider_gfx_metal.mm (ARC); this header is a
// narrow void*-only C ABI so displayxr_provider_session.cpp and
// displayxr_display_provider.cpp stay plain C++. Object lifetimes: the .mm
// retains everything it hands out (device excepted — Unity owns it) and
// releases it all in dxr_prov_metal_teardown().
//
// Design (docs~/experiments/macos-metal-recon.md): the session binds a
// provider-created MTLCommandQueue on UNITY'S MTLDevice (client-queue model —
// the runtime's in-process compositor encodes on it). Zero-copy MultiPass:
// Unity renders each eye straight into a single-slice VIEW of the acquired
// arraySize=2 swapchain image; dxr_prov_metal_order_weave orders the
// compositor's weave after Unity's renders with an MTLSharedEvent signalled
// on Unity's own queue (IUnityGraphicsMetalV2::CommandQueue()).

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Stash IUnityInterfaces* (from UnityPluginLoad) so the glue can fetch
/// IUnityGraphicsMetal lazily on the graphics thread.
void dxr_prov_metal_set_unity_ifaces(void *unity_interfaces);

/// Unity's MTLDevice (IUnityGraphicsMetalV1/V2::MetalDevice()), or NULL.
void *dxr_prov_metal_unity_device(void);

/// Create (once) and return the session's own MTLCommandQueue on Unity's
/// device — the queue passed in XrGraphicsBindingMetalKHR. NULL on failure.
void *dxr_prov_metal_create_session_queue(void);

/// Zero-copy (the shipping path): a single-slice MTLTexture VIEW of one slice
/// of the runtime's arraySize=2 swapchain image. Unity renders straight into
/// the slice — no provider blit (a provider blit CB touching session textures
/// crashes the Unity editor's Metal device worker; bring-up runs 7-18).
/// Retained by the glue until teardown/realloc. Returns id<MTLTexture> or NULL.
void *dxr_prov_metal_slice_view(void *array_tex, uint32_t slice);

/// Cross-queue frame order for zero-copy: signal the shared event on UNITY'S
/// queue (FIFO after this frame's eye renders) and GPU-wait it on the session
/// queue (FIFO before the compositor's weave at xrEndFrame). Both command
/// buffers are encoder-less (safe — bring-up run 19). Default ON;
/// DISPLAYXR_METAL_SYNC=0 disables (triage escape hatch).
void dxr_prov_metal_order_weave(void *session_queue);

/// Release every object the glue retained (session queue, shared event, slice
/// views). Safe to call repeatedly; called from dxr_prov_session_stop.
void dxr_prov_metal_teardown(void);

#ifdef __cplusplus
}
#endif
