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
// the runtime's in-process compositor encodes on it). MultiPass first light:
// Unity renders each eye into its own single-slice MTLTexture; submit blits
// them into slices 0/1 of the acquired arraySize=2 swapchain image on the
// session queue, ordered after Unity's render by an MTLSharedEvent signalled
// on Unity's own queue (IUnityGraphicsMetalV2::CommandQueue()).
// DISPLAYXR_METAL_COARSE_SYNC=1 falls back to waitUntilCompleted bring-up sync.

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

/// Allocate a render-target MTLTexture on Unity's device (single slice,
/// mtl_pixel_format = the runtime-enumerated swapchain format). Retained by
/// the glue until teardown. Returns id<MTLTexture> as void*, or NULL.
void *dxr_prov_metal_alloc_eye_texture(uint32_t width, uint32_t height,
                                       int64_t mtl_pixel_format, uint32_t eye);

/// Zero-copy (the shipping path): a single-slice MTLTexture VIEW of one slice
/// of the runtime's arraySize=2 swapchain image. Unity renders straight into
/// the slice — no provider blit (a provider blit CB touching session textures
/// crashes the Unity editor's Metal device worker; bring-up runs 7-18).
/// Retained by the glue until teardown/realloc. Returns id<MTLTexture> or NULL.
void *dxr_prov_metal_slice_view(void *array_tex, uint32_t slice);

/// Cross-queue frame order for zero-copy: signal the shared event on UNITY'S
/// queue (FIFO after this frame's eye renders) and GPU-wait it on the session
/// queue (FIFO before the compositor's weave at xrEndFrame). Both command
/// buffers are encoder-less (safe — bring-up run 19). No-op without the event.
void dxr_prov_metal_order_weave(void *session_queue);

/// Per-frame MultiPass hand-off: order the session queue after Unity's render
/// (MTLSharedEvent signalled on Unity's queue; coarse waitUntilCompleted if
/// DISPLAYXR_METAL_COARSE_SYNC=1), then blit eye_tex[0..n_eyes-1] into slices
/// 0..n_eyes-1 of dst_swapchain_tex (id<MTLTexture>, arraySize=2) on the
/// session queue. Returns 1 on success.
int dxr_prov_metal_submit(void *session_queue,
                          void *eye_tex_l, void *eye_tex_r,
                          void *dst_swapchain_tex, uint32_t n_eyes);

/// Release every object the glue retained (session queue, shared event, eye
/// textures). Safe to call repeatedly; called from dxr_prov_session_stop.
void dxr_prov_metal_teardown(void);

#ifdef __cplusplus
}
#endif
