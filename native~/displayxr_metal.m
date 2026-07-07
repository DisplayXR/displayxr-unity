// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// macOS Metal helpers for the hook chain path.
// Preview window creation (editor Play Mode) and overlay view (built apps).

#import <Metal/Metal.h>
#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>

#include "displayxr_metal.h"
#include "displayxr_shared_state.h"

// ============================================================================
// Editor Play Mode: plugin-created preview window
// ============================================================================

static NSWindow *s_preview_window = nil;
static NSView *s_preview_view = nil;

void *
displayxr_metal_create_preview_window(uint32_t width, uint32_t height)
{
	displayxr_metal_destroy_preview_window();

	dispatch_block_t create = ^{
		NSRect frame = NSMakeRect(100, 100, width, height);
		NSWindowStyleMask style = NSWindowStyleMaskTitled |
		                          NSWindowStyleMaskClosable |
		                          NSWindowStyleMaskResizable |
		                          NSWindowStyleMaskMiniaturizable;

		s_preview_window = [[NSWindow alloc] initWithContentRect:frame
		                                              styleMask:style
		                                                backing:NSBackingStoreBuffered
		                                                  defer:NO];
		[s_preview_window setTitle:@"DisplayXR Preview"];
		[s_preview_window setReleasedWhenClosed:NO];

		s_preview_view = [[NSView alloc] initWithFrame:frame];
		// Layer-HOSTING view with a CAMetalLayer (set layer BEFORE wantsLayer).
		// The runtime's setup_external_window() takes the CAMetalLayer branch
		// directly; the plain-backing-layer branch would dispatch_sync to the
		// main queue — a deadlock when the session is created from Unity's
		// render thread while the main thread blocks in GfxStart (#204).
		[s_preview_view setLayer:[CAMetalLayer layer]];
		[s_preview_view setWantsLayer:YES];
		[s_preview_window setContentView:s_preview_view];

		// Show without stealing keyboard focus from Unity
		[s_preview_window orderFront:nil];

		fprintf(stderr, "[DisplayXR] Preview window created: %ux%u\n", width, height);
	};

	if ([NSThread isMainThread]) {
		create();
	} else {
		dispatch_sync(dispatch_get_main_queue(), create);
	}

	return (__bridge void *)s_preview_view;
}

void
displayxr_metal_destroy_preview_window(void)
{
	if (s_preview_window) {
		dispatch_block_t close = ^{
			[s_preview_window close];
			s_preview_window = nil;
			s_preview_view = nil;
			fprintf(stderr, "[DisplayXR] Preview window destroyed\n");
		};

		if ([NSThread isMainThread]) {
			close();
		} else {
			dispatch_sync(dispatch_get_main_queue(), close);
		}
	}
}

// ============================================================================
// Window-space UI overlay (issue #67) — same-format MTLBlitCommandEncoder copy.
// ============================================================================

int
displayxr_metal_blit_textures(void *queue_ptr, void *src_ptr, void *dst_ptr)
{
	if (!queue_ptr || !src_ptr || !dst_ptr) return 0;

	id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)queue_ptr;
	id<MTLTexture> src = (__bridge id<MTLTexture>)src_ptr;
	id<MTLTexture> dst = (__bridge id<MTLTexture>)dst_ptr;

	if (src.pixelFormat != dst.pixelFormat) {
		// Same-format-only path: log once if mismatched. The format-conversion
		// blit (render-pass shader) is intentionally not implemented here to
		// keep the hooked path lean — picking BGRA on Apple in the wsui
		// format selection should match Unity's RenderTextureFormat.ARGB32.
		static int warned = 0;
		if (!warned) {
			warned = 1;
			fprintf(stderr,
			    "[DisplayXR] wsui blit format mismatch: src=%lu dst=%lu — "
			    "expected match (Unity ARGB32 → BGRA8Unorm)\n",
			    (unsigned long)src.pixelFormat, (unsigned long)dst.pixelFormat);
		}
	}

	id<MTLCommandBuffer> cmd = [queue commandBuffer];
	if (!cmd) return 0;

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

	[cmd commit];
	[cmd waitUntilCompleted];
	return 1;
}

// ============================================================================
// Built apps: overlay NSView on top of Unity's window
// ============================================================================

// Sits on top of Unity's contentView with its own CAMetalLayer.
// Unity renders underneath; compositor renders on top via this layer.
// hitTest: returns nil so all input passes through to Unity.

@interface DisplayXROverlayView : NSView
@end

@implementation DisplayXROverlayView

- (NSView *)hitTest:(NSPoint)point
{
	return nil; // Pass all input through to Unity's contentView
}

- (BOOL)wantsUpdateLayer
{
	return YES;
}

@end

static NSView *s_overlay_view = nil;

void *
displayxr_get_app_main_view(void)
{
	@try {
		// Return existing overlay if already created
		if (s_overlay_view != nil && [s_overlay_view window] != nil)
			return (__bridge void *)s_overlay_view;

		NSWindow *window = [[NSApplication sharedApplication] mainWindow];
		if (window == nil)
			window = [[NSApplication sharedApplication] keyWindow];
		if (window == nil) {
			// Fallback: grab the first visible window from the app's window list
			for (NSWindow *w in [[NSApplication sharedApplication] windows]) {
				if ([w isVisible] && [w contentView] != nil) {
					window = w;
					break;
				}
			}
		}
		if (window == nil)
			return NULL;

		NSView *contentView = [window contentView];

		// Create overlay view with its own CAMetalLayer
		DisplayXROverlayView *overlay = [[DisplayXROverlayView alloc]
		    initWithFrame:[contentView bounds]];
		overlay.wantsLayer = YES;
		overlay.layer = [CAMetalLayer layer];
		overlay.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;

		[contentView addSubview:overlay];
		s_overlay_view = overlay;

		fprintf(stderr, "[DisplayXR] Created overlay NSView (%dx%d) on window '%s'\n",
		        (int)[contentView bounds].size.width,
		        (int)[contentView bounds].size.height,
		        [[window title] UTF8String]);

		return (__bridge void *)overlay;
	} @catch (NSException *e) {
		fprintf(stderr, "[DisplayXR] Exception getting main view: %s\n",
		        [[e reason] UTF8String]);
		return NULL;
	}
}

int
displayxr_metal_get_app_window_rect(int32_t *out_x, int32_t *out_y,
                                     uint32_t *out_w, uint32_t *out_h)
{
	if (s_overlay_view == nil) {
		if (out_x) *out_x = 0;
		if (out_y) *out_y = 0;
		if (out_w) *out_w = 0;
		if (out_h) *out_h = 0;
		return 0;
	}
	NSWindow *window = [s_overlay_view window];
	if (window == nil) {
		if (out_x) *out_x = 0;
		if (out_y) *out_y = 0;
		if (out_w) *out_w = 0;
		if (out_h) *out_h = 0;
		return 0;
	}
	NSRect frame = [window frame];
	NSRect content = [window contentRectForFrameRect:frame];
	CGFloat scale = [window backingScaleFactor];
	if (out_x) *out_x = (int32_t)(content.origin.x * scale);
	if (out_y) *out_y = (int32_t)(content.origin.y * scale);
	if (out_w) *out_w = (uint32_t)(content.size.width * scale);
	if (out_h) *out_h = (uint32_t)(content.size.height * scale);
	return 1;
}
