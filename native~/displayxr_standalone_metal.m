// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
//
// Metal helpers for the standalone preview session (macOS).
// Creates a native NSWindow for the runtime to composite into,
// and provides GPU blit for atlas → swapchain.

#import <Metal/Metal.h>
#import <AppKit/AppKit.h>
#include <stdio.h>

#include "displayxr_standalone_metal.h"

static NSWindow *s_sa_window = nil;
static NSView *s_sa_view = nil;
static id<MTLDevice> s_sa_device = nil;
static id<MTLCommandQueue> s_sa_queue = nil;
static id<MTLRenderPipelineState> s_sa_blit_pipeline = nil;
static int s_sa_blit_log_once = 0;
static volatile int s_sa_window_closed = 0;

// ============================================================================
// Window delegate — detect user closing the preview window
// ============================================================================

@interface DisplayXRSAWindowDelegate : NSObject <NSWindowDelegate>
@end

@implementation DisplayXRSAWindowDelegate

- (void)windowWillClose:(NSNotification *)notification
{
	s_sa_window_closed = 1;
	s_sa_window = nil;
	s_sa_view = nil;
}


@end

static DisplayXRSAWindowDelegate *s_sa_window_delegate = nil;

// ============================================================================
// Non-key preview window — never steals keyboard/mouse focus from Unity
// ============================================================================

@interface DisplayXRPreviewNSWindow : NSWindow
@end

@implementation DisplayXRPreviewNSWindow

// Never become key window — Unity keeps all mouse/keyboard focus.
// Title bar drag (move), edge drag (resize), and close button still work
// because those are handled by the window frame, not the key window.
- (BOOL)canBecomeKeyWindow { return NO; }
- (BOOL)canBecomeMainWindow { return NO; }

// Freeze content during live resize. macOS blocks the main thread during
// live resize, so EditorApplication.update (our FrameTick) can't render
// new frames. Without this, the compositor stretches the last frame into
// the new window size → distortion. With this, the content stays at the
// last-rendered size and snaps to the correct size when resize ends.
- (BOOL)preservesContentDuringLiveResize { return YES; }

@end

// ============================================================================
// Window creation/destruction
// ============================================================================

int
displayxr_sa_metal_create_window(uint32_t width, uint32_t height)
{
	displayxr_sa_metal_destroy_window();
	s_sa_window_closed = 0;

	// Create on the main thread (AppKit requirement)
	dispatch_block_t create = ^{
		NSRect frame = NSMakeRect(100, 100, width, height);
		NSWindowStyleMask style = NSWindowStyleMaskTitled |
		                          NSWindowStyleMaskClosable |
		                          NSWindowStyleMaskResizable |
		                          NSWindowStyleMaskMiniaturizable;

		s_sa_window = [[DisplayXRPreviewNSWindow alloc] initWithContentRect:frame
		                                                          styleMask:style
		                                                            backing:NSBackingStoreBuffered
		                                                              defer:NO];
		[s_sa_window setTitle:@"DisplayXR Preview"];
		[s_sa_window setReleasedWhenClosed:NO];

		// Set delegate to detect user closing the window
		s_sa_window_delegate = [[DisplayXRSAWindowDelegate alloc] init];
		[s_sa_window setDelegate:s_sa_window_delegate];

		// Layer-backed view so the runtime can add a CAMetalLayer overlay
		s_sa_view = [[NSView alloc] initWithFrame:frame];
		[s_sa_view setWantsLayer:YES];
		[s_sa_window setContentView:s_sa_view];

		// Show without stealing keyboard focus from Unity editor
		[s_sa_window orderFront:nil];

		fprintf(stderr, "[DisplayXR-SA] Preview window created: %ux%u\n", width, height);
	};

	if ([NSThread isMainThread]) {
		create();
	} else {
		dispatch_sync(dispatch_get_main_queue(), create);
	}

	return (s_sa_window != nil) ? 1 : 0;
}

int
displayxr_sa_metal_window_was_closed(void)
{
	int closed = s_sa_window_closed;
	s_sa_window_closed = 0;
	return closed;
}

int
displayxr_sa_metal_window_is_interacting(void)
{
	if (!s_sa_window) return 0;

	// Check if the mouse cursor is anywhere over the preview window.
	NSPoint mouse = [NSEvent mouseLocation]; // screen coords
	NSRect frame = [s_sa_window frame];
	return NSPointInRect(mouse, frame) ? 1 : 0;
}

void
displayxr_sa_metal_destroy_window(void)
{
	s_sa_blit_pipeline = nil;
	s_sa_blit_log_once = 0;

	if (s_sa_window) {
		dispatch_block_t close = ^{
			[s_sa_window close];
			s_sa_window = nil;
			s_sa_view = nil;
			fprintf(stderr, "[DisplayXR-SA] Preview window destroyed\n");
		};

		if ([NSThread isMainThread]) {
			close();
		} else {
			dispatch_sync(dispatch_get_main_queue(), close);
		}
	}
}

void *
displayxr_sa_metal_get_view(void)
{
	return (__bridge void *)s_sa_view;
}

float
displayxr_sa_metal_get_backing_scale(void)
{
	if (s_sa_window) {
		return (float)[s_sa_window backingScaleFactor];
	}
	NSScreen *screen = [NSScreen mainScreen];
	return screen ? (float)[screen backingScaleFactor] : 2.0f;
}

// ============================================================================
// Window rect query
// ============================================================================

int
displayxr_sa_metal_get_window_rect(int32_t *out_x, int32_t *out_y,
                                    uint32_t *out_w, uint32_t *out_h)
{
	if (!s_sa_window || !s_sa_view) {
		*out_x = *out_y = 0;
		*out_w = *out_h = 0;
		return 0;
	}

	// Content rect in screen coordinates (points)
	NSRect frame = [s_sa_window frame];
	NSRect content = [s_sa_window contentRectForFrameRect:frame];
	CGFloat scale = [s_sa_window backingScaleFactor];

	// Convert to backing pixels
	*out_x = (int32_t)(content.origin.x * scale);
	*out_y = (int32_t)(content.origin.y * scale);
	*out_w = (uint32_t)(content.size.width * scale);
	*out_h = (uint32_t)(content.size.height * scale);

	return 1;
}

// ============================================================================
// Metal command queue + blit
// ============================================================================

void *
displayxr_sa_metal_get_command_queue(void)
{
	if (!s_sa_device) {
		s_sa_device = MTLCreateSystemDefaultDevice();
	}
	if (!s_sa_queue && s_sa_device) {
		s_sa_queue = [s_sa_device newCommandQueue];
	}
	return (__bridge void *)s_sa_queue;
}

// Render-pass blit pipeline for format conversion (RGBA↔BGRA)
static id<MTLRenderPipelineState>
create_blit_pipeline(id<MTLDevice> device, MTLPixelFormat dstFormat)
{
	NSError *error = nil;
	NSString *shaderSrc = @
		"#include <metal_stdlib>\n"
		"using namespace metal;\n"
		"struct V2F { float4 pos [[position]]; float2 uv; };\n"
		"vertex V2F blit_vs(uint vid [[vertex_id]]) {\n"
		"    float2 p = float2((vid & 1) * 2.0 - 1.0, (vid & 2) - 1.0);\n"
		"    V2F o; o.pos = float4(p, 0, 1); o.uv = float2((p.x+1)*0.5, (1-p.y)*0.5);\n"
		"    return o;\n"
		"}\n"
		"fragment float4 blit_fs(V2F in [[stage_in]], texture2d<float> tex [[texture(0)]]) {\n"
		"    constexpr sampler s(filter::nearest);\n"
		"    return tex.sample(s, in.uv);\n"
		"}\n";

	id<MTLLibrary> lib = [device newLibraryWithSource:shaderSrc options:nil error:&error];
	if (!lib) {
		fprintf(stderr, "[DisplayXR-SA] Blit shader compile failed: %s\n",
		        [[error localizedDescription] UTF8String]);
		return nil;
	}

	MTLRenderPipelineDescriptor *desc = [[MTLRenderPipelineDescriptor alloc] init];
	desc.vertexFunction = [lib newFunctionWithName:@"blit_vs"];
	desc.fragmentFunction = [lib newFunctionWithName:@"blit_fs"];
	desc.colorAttachments[0].pixelFormat = dstFormat;

	id<MTLRenderPipelineState> pso = [device newRenderPipelineStateWithDescriptor:desc error:&error];
	if (!pso) {
		fprintf(stderr, "[DisplayXR-SA] Blit pipeline creation failed: %s\n",
		        [[error localizedDescription] UTF8String]);
	}
	return pso;
}

int
displayxr_sa_metal_blit(void *src_ptr, void *dst_ptr)
{
	if (!src_ptr || !dst_ptr || !s_sa_queue) return 0;

	id<MTLTexture> src = (__bridge id<MTLTexture>)src_ptr;
	id<MTLTexture> dst = (__bridge id<MTLTexture>)dst_ptr;

	if (!s_sa_blit_log_once) {
		s_sa_blit_log_once = 1;
		fprintf(stderr, "[DisplayXR-SA] Blit: src format=%lu dst format=%lu (%s)\n",
		        (unsigned long)src.pixelFormat, (unsigned long)dst.pixelFormat,
		        (src.pixelFormat == dst.pixelFormat) ? "match" : "MISMATCH — using render blit");
	}

	id<MTLCommandBuffer> cmd = [s_sa_queue commandBuffer];
	if (!cmd) return 0;

	if (src.pixelFormat == dst.pixelFormat) {
		// Same format: raw byte copy is safe
		id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];
		if (!blit) return 0;

		NSUInteger w = MIN(src.width, dst.width);
		NSUInteger h = MIN(src.height, dst.height);

		[blit copyFromTexture:src
		          sourceSlice:0
		          sourceLevel:0
		         sourceOrigin:MTLOriginMake(0, 0, 0)
		           sourceSize:MTLSizeMake(w, h, 1)
		            toTexture:dst
		     destinationSlice:0
		     destinationLevel:0
		    destinationOrigin:MTLOriginMake(0, 0, 0)];

		[blit endEncoding];
	} else {
		// Format mismatch: use render pass for proper conversion
		if (!s_sa_blit_pipeline) {
			s_sa_blit_pipeline = create_blit_pipeline(s_sa_device, dst.pixelFormat);
			if (!s_sa_blit_pipeline) return 0;
		}

		MTLRenderPassDescriptor *rpd = [MTLRenderPassDescriptor renderPassDescriptor];
		rpd.colorAttachments[0].texture = dst;
		rpd.colorAttachments[0].loadAction = MTLLoadActionDontCare;
		rpd.colorAttachments[0].storeAction = MTLStoreActionStore;

		id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:rpd];
		if (!enc) return 0;

		[enc setRenderPipelineState:s_sa_blit_pipeline];
		[enc setFragmentTexture:src atIndex:0];
		[enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
		[enc endEncoding];
	}

	[cmd commit];
	[cmd waitUntilCompleted];

	return 1;
}
