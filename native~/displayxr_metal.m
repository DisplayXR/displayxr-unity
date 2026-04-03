// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
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
