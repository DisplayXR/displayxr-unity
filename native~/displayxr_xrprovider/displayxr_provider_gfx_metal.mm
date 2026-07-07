// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Metal glue for the display provider (macOS parity epic #202, Phase 2 #204).
// See displayxr_provider_gfx_metal.h for the design notes. ARC-compiled.

#ifdef __APPLE__

#import <Metal/Metal.h>
#import <Foundation/Foundation.h>

#include <stdio.h>
#include <stdlib.h>

#include "displayxr_provider_gfx_metal.h"

#include "../unity_pluginapi/IUnityInterface.h"
#include "../unity_pluginapi/IUnityGraphicsMetal.h"

extern "C" void dxr_prov_file_log(const char *s); // session TU

static void mgl_log(const char *msg)
{
	fprintf(stderr, "%s", msg);
	dxr_prov_file_log(msg);
}

// ----------------------------------------------------------------------------
// State. The glue owns (strong refs): session queue, shared event, eye
// textures, per-frame signal command buffers' queue references. Unity owns the
// device and its command queue — we only hold weak-ish raw lookups for those
// via the interface each call.
// ----------------------------------------------------------------------------

static IUnityInterfaces *s_ifaces = nullptr;
static id<MTLCommandQueue> s_session_queue = nil;   // bound in the session
static id<MTLSharedEvent>  s_shared_event = nil;    // Unity render -> blit order
static uint64_t            s_event_value = 0;
static id<MTLTexture>      s_eye_tex[2] = {nil, nil};

// Deferred destruction (ADR-001, Metal edition): Unity destroys the
// RenderSurfaces wrapping our eye textures ASYNCHRONOUSLY on the
// GfxDeviceWorker — releasing the id<MTLTexture> at session stop leaves that
// deferred DestroyRenderSurfaceDesc dereferencing a dead object (SEGV, seen
// live during Phase 2 bring-up). Teardown parks the retired objects here;
// they're released at the NEXT session start, long after Unity's deferred
// destroys ran. Repeated Play enter/exit keeps exactly one retired generation.
static NSMutableArray *s_graveyard = nil;

void dxr_prov_metal_set_unity_ifaces(void *unity_interfaces)
{
	s_ifaces = (IUnityInterfaces *)unity_interfaces;
}

// IUnityGraphicsMetalV2 preferred (has CommandQueue()); V1 fallback (device +
// CurrentCommandBuffer only). Fetched per call — cheap, and robust across
// device events.
static IUnityGraphicsMetalV2 *get_metal_v2(void)
{
	return s_ifaces ? s_ifaces->Get<IUnityGraphicsMetalV2>() : nullptr;
}
static IUnityGraphicsMetalV1 *get_metal_v1(void)
{
	return s_ifaces ? s_ifaces->Get<IUnityGraphicsMetalV1>() : nullptr;
}

void *dxr_prov_metal_unity_device(void)
{
	if (IUnityGraphicsMetalV2 *v2 = get_metal_v2()) return (__bridge void *)v2->MetalDevice();
	if (IUnityGraphicsMetalV1 *v1 = get_metal_v1()) return (__bridge void *)v1->MetalDevice();
	mgl_log("[DisplayXR-PROV] Metal: no IUnityGraphicsMetal interface\n");
	return nullptr;
}

void *dxr_prov_metal_create_session_queue(void)
{
	if (s_session_queue) return (__bridge void *)s_session_queue;
	// New session: the previous generation's retired objects (see s_graveyard)
	// are safe to release now — Unity's deferred surface destroys are long done.
	[s_graveyard removeAllObjects];
	id<MTLDevice> dev = (__bridge id<MTLDevice>)dxr_prov_metal_unity_device();
	if (!dev) return nullptr;
	s_session_queue = [dev newCommandQueue];
	if (!s_session_queue) { mgl_log("[DisplayXR-PROV] Metal: newCommandQueue failed\n"); return nullptr; }
	s_session_queue.label = @"DisplayXR provider session queue";
	// MTLSharedEvent (not MTLEvent) from day one: same-device today, but keeps
	// the own-device + IOSurface contingency (recon doc) open without a rework.
	s_shared_event = [dev newSharedEvent];
	s_event_value = 0;
	mgl_log("[DisplayXR-PROV] Metal: session queue + shared event created on Unity's device\n");
	return (__bridge void *)s_session_queue;
}

void *dxr_prov_metal_alloc_eye_texture(uint32_t width, uint32_t height,
                                       int64_t mtl_pixel_format, uint32_t eye)
{
	if (eye > 1 || width == 0 || height == 0) return nullptr;
	id<MTLDevice> dev = (__bridge id<MTLDevice>)dxr_prov_metal_unity_device();
	if (!dev) return nullptr;

	MTLPixelFormat fmt = (MTLPixelFormat)mtl_pixel_format;
	if (fmt == MTLPixelFormatInvalid) fmt = MTLPixelFormatRGBA8Unorm;

	MTLTextureDescriptor *td =
	    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt
	                                                       width:width
	                                                      height:height
	                                                   mipmapped:NO];
	// Unity renders into it (render target) and the blit reads it. Private
	// storage: GPU-only, no CPU access needed.
	td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
	td.storageMode = MTLStorageModePrivate;

	id<MTLTexture> tex = [dev newTextureWithDescriptor:td];
	if (!tex) {
		mgl_log("[DisplayXR-PROV] Metal: eye texture alloc failed\n");
		return nullptr;
	}
	tex.label = eye == 0 ? @"DisplayXR eye L" : @"DisplayXR eye R";
	// Realloc (#172): park the PREVIOUS texture in the graveyard instead of
	// letting ARC drop it — Unity's DestroyTexture of the old wrap is deferred
	// to the GfxDeviceWorker (ADR-001) and must not race a dead MTLTexture.
	if (s_eye_tex[eye]) {
		if (!s_graveyard) s_graveyard = [NSMutableArray array];
		[s_graveyard addObject:s_eye_tex[eye]];
	}
	s_eye_tex[eye] = tex;
	char buf[160];
	snprintf(buf, sizeof(buf), "[DisplayXR-PROV] Metal: eye %u target %ux%u fmt=%lld\n",
	         eye, width, height, (long long)mtl_pixel_format);
	mgl_log(buf);
	return (__bridge void *)tex;
}

void *dxr_prov_metal_slice_view(void *array_tex, uint32_t slice)
{
	id<MTLTexture> arr = (__bridge id<MTLTexture>)array_tex;
	if (!arr || slice >= arr.arrayLength) return nullptr;
	id<MTLTexture> view =
	    [arr newTextureViewWithPixelFormat:arr.pixelFormat
	                           textureType:MTLTextureType2D
	                                levels:NSMakeRange(0, 1)
	                                slices:NSMakeRange(slice, 1)];
	if (!view) {
		mgl_log("[DisplayXR-PROV] Metal: slice view creation failed\n");
		return nullptr;
	}
	view.label = slice == 0 ? @"DisplayXR swapchain slice L" : @"DisplayXR swapchain slice R";
	// Retain until teardown/realloc (Unity's wrap of it is destroyed deferred —
	// ADR-001; parked in the graveyard like the eye textures).
	if (!s_graveyard) s_graveyard = [NSMutableArray array];
	static NSMutableArray *s_live_views = nil;
	if (!s_live_views) s_live_views = [NSMutableArray array];
	[s_live_views addObject:view];
	// Cap growth across reallocs: move all but the newest generation (up to
	// 8 images x 2 eyes) to the graveyard.
	while (s_live_views.count > 16) {
		[s_graveyard addObject:s_live_views[0]];
		[s_live_views removeObjectAtIndex:0];
	}
	return (__bridge void *)view;
}

void dxr_prov_metal_order_weave(void *session_queue)
{
	// Default OFF while the editor crash is bisected: DISPLAYXR_METAL_SYNC=1
	// enables the cross-queue event ordering (Unity-queue signal CB suspect).
	static int sync_on = -1;
	if (sync_on < 0) sync_on = getenv("DISPLAYXR_METAL_SYNC") ? 1 : 0;
	if (!sync_on) return;
	id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)session_queue;
	if (!queue || !s_shared_event) return;
	IUnityGraphicsMetalV2 *v2 = get_metal_v2();
	id<MTLCommandQueue> unity_q = v2 ? v2->CommandQueue() : nil;
	if (!unity_q) return;
	uint64_t v = ++s_event_value;
	id<MTLCommandBuffer> sig = [unity_q commandBuffer];
	[sig encodeSignalEvent:s_shared_event value:v];
	[sig commit];
	id<MTLCommandBuffer> wait_cb = [queue commandBuffer];
	[wait_cb encodeWaitForEvent:s_shared_event value:v];
	[wait_cb commit];
}

int dxr_prov_metal_submit(void *session_queue,
                          void *eye_tex_l, void *eye_tex_r,
                          void *dst_swapchain_tex, uint32_t n_eyes)
{
	id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)session_queue;
	id<MTLTexture> dst = (__bridge id<MTLTexture>)dst_swapchain_tex;
	if (!queue || !dst || n_eyes == 0) return 0;

	// Bring-up bisection lever: skip the blit + sync entirely (frame still ends).
	if (getenv("DISPLAYXR_METAL_SKIP_BLIT")) return 1;

	// Bisection lever 4: commit an EMPTY command buffer on the session queue —
	// no encoder, no textures. Isolates "any provider CB" vs "the blit encoder".
	if (getenv("DISPLAYXR_METAL_EMPTY_CB")) {
		id<MTLCommandBuffer> ecb = [queue commandBuffer];
		[ecb commit];
		return 1;
	}

	// Bisection lever 2: blit into a same-shape DUMMY texture instead of the
	// runtime swapchain image (isolates src-read vs dst-write as the trigger).
	if (getenv("DISPLAYXR_METAL_DUMMY_DST")) {
		static id<MTLTexture> dummy = nil;
		if (!dummy || dummy.width != dst.width || dummy.height != dst.height) {
			MTLTextureDescriptor *dd =
			    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:dst.pixelFormat
			                                                       width:dst.width
			                                                      height:dst.height
			                                                   mipmapped:NO];
			dd.textureType = MTLTextureType2DArray;
			dd.arrayLength = 2;
			dd.usage = MTLTextureUsageShaderRead;
			dd.storageMode = MTLStorageModePrivate;
			dummy = [((__bridge id<MTLCommandQueue>)session_queue).device newTextureWithDescriptor:dd];
		}
		if (dummy) dst = dummy;
	}

	static int coarse = -1;
	if (coarse < 0) coarse = getenv("DISPLAYXR_METAL_COARSE_SYNC") ? 1 : 0;

	// Encode the blit on UNITY'S OWN queue (IUnityGraphicsMetalV2::CommandQueue):
	// queue FIFO orders it after the eye renders Unity already committed this
	// frame — no event needed for the source side, and no foreign command buffer
	// reads Unity's live render targets from another queue (a cross-queue blit
	// on the session queue reading the eye textures crashed Unity's Metal
	// surface bookkeeping — DestroyRenderSurfaceDesc SEGV, bring-up run 7-11).
	// The session queue then gets a tiny wait-CB so the runtime's compositor
	// (which encodes the weave on the session queue at xrEndFrame) is ordered
	// after the blit via the MTLSharedEvent.
	IUnityGraphicsMetalV2 *v2 = get_metal_v2();
	id<MTLCommandQueue> unity_q = v2 ? v2->CommandQueue() : nil;
	id<MTLCommandQueue> blit_q = unity_q ? unity_q : queue;

	id<MTLCommandBuffer> cb = [blit_q commandBuffer];
	if (!cb) return 0;

	id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
	id<MTLTexture> eyes[2] = {(__bridge id<MTLTexture>)eye_tex_l,
	                          (__bridge id<MTLTexture>)eye_tex_r};

	// Bisection lever 3: read from a DUMMY source instead of Unity's eye targets.
	if (getenv("DISPLAYXR_METAL_DUMMY_SRC")) {
		static id<MTLTexture> dsrc = nil;
		if (!dsrc && eyes[0]) {
			MTLTextureDescriptor *sd =
			    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:eyes[0].pixelFormat
			                                                       width:eyes[0].width
			                                                      height:eyes[0].height
			                                                   mipmapped:NO];
			sd.usage = MTLTextureUsageShaderRead;
			sd.storageMode = MTLStorageModePrivate;
			dsrc = [((__bridge id<MTLCommandQueue>)session_queue).device newTextureWithDescriptor:sd];
		}
		if (dsrc) { eyes[0] = dsrc; eyes[1] = dsrc; }
	}
	for (uint32_t e = 0; e < n_eyes && e < 2; e++) {
		id<MTLTexture> src = eyes[e];
		if (!src) continue;
		NSUInteger w = MIN(src.width, dst.width);
		NSUInteger h = MIN(src.height, dst.height);
		[blit copyFromTexture:src
		          sourceSlice:0
		          sourceLevel:0
		         sourceOrigin:MTLOriginMake(0, 0, 0)
		           sourceSize:MTLSizeMake(w, h, 1)
		            toTexture:dst
		     destinationSlice:e
		     destinationLevel:0
		    destinationOrigin:MTLOriginMake(0, 0, 0)];
	}
	[blit endEncoding];

	if (!coarse && s_shared_event && unity_q) {
		// blit(Unity q) --signal--> wait(session q) --FIFO--> compositor weave.
		uint64_t v = ++s_event_value;
		[cb encodeSignalEvent:s_shared_event value:v];
		[cb commit];
		id<MTLCommandBuffer> wait_cb = [queue commandBuffer];
		[wait_cb encodeWaitForEvent:s_shared_event value:v];
		[wait_cb commit];
	} else {
		[cb commit];
		[cb waitUntilCompleted]; // coarse bring-up sync: correctness over latency
	}
	return 1;
}

void dxr_prov_metal_teardown(void)
{
	// Park (don't release) — see the s_graveyard comment above. The next
	// session start empties the graveyard.
	if (!s_graveyard) s_graveyard = [NSMutableArray array];
	for (int e = 0; e < 2; e++) {
		if (s_eye_tex[e]) [s_graveyard addObject:s_eye_tex[e]];
		s_eye_tex[e] = nil;
	}
	if (s_shared_event)  { [s_graveyard addObject:s_shared_event];  s_shared_event = nil; }
	if (s_session_queue) { [s_graveyard addObject:s_session_queue]; s_session_queue = nil; }
	s_event_value = 0;
}

#endif // __APPLE__
