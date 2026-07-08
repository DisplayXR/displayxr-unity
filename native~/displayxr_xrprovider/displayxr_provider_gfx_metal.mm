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
// State. The glue owns (strong refs): session queue, shared event, and the
// retained swapchain slice views. Unity owns the device and its command
// queue — we only hold weak-ish raw lookups for those via the interface
// each call.
// ----------------------------------------------------------------------------

static IUnityInterfaces *s_ifaces = nullptr;
static id<MTLCommandQueue> s_session_queue = nil;   // bound in the session
static id<MTLSharedEvent>  s_shared_event = nil;    // Unity render -> weave order
static uint64_t            s_event_value = 0;

// Deferred destruction (ADR-001, Metal edition): Unity destroys the
// RenderSurfaces wrapping our slice views ASYNCHRONOUSLY on the
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
	// ADR-001; retired generations park in the graveyard).
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
	// Default ON (#204): orders the compositor's weave (session queue, encoded
	// at xrEndFrame) after Unity's eye renders (Unity queue FIFO) with
	// encoder-less signal/wait CBs. DISPLAYXR_METAL_SYNC=0 is the triage
	// escape hatch (frames then rely on Unity's own queue FIFO only).
	static int sync_on = -1;
	if (sync_on < 0) {
		const char *env = getenv("DISPLAYXR_METAL_SYNC");
		sync_on = (env && env[0] == '0') ? 0 : 1;
	}
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

void dxr_prov_metal_teardown(void)
{
	// Park (don't release) — see the s_graveyard comment above. The next
	// session start empties the graveyard.
	if (!s_graveyard) s_graveyard = [NSMutableArray array];
	if (s_shared_event)  { [s_graveyard addObject:s_shared_event];  s_shared_event = nil; }
	if (s_session_queue) { [s_graveyard addObject:s_session_queue]; s_session_queue = nil; }
	s_event_value = 0;
}

#endif // __APPLE__
