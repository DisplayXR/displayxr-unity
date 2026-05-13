// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
//
// macOS-specific helpers for DisplayXRTransparentOverlay (issue #85).
// Flips Unity's NSWindow + contentView layer so per-pixel alpha from the
// camera reaches the desktop via Cocoa per-pixel transparency.
//
// The view hierarchy in built Unity apps is:
//
//   NSWindow (PlayerWindow)
//     └── contentView (Unity's main render view — OPAQUE by default)
//         └── DisplayXROverlayView (created in displayxr_metal.m — has the
//             CAMetalLayer that the runtime renders into; opaque=NO)
//
// For per-pixel transparency to reach the desktop, EVERY layer above the
// pixel must be non-opaque. The runtime sets the CAMetalLayer.opaque on
// the overlay (good); this helper sets NSWindow.opaque + clear background
// AND flips the contentView's layer to non-opaque + clear background so
// alpha=0 in the overlay passes through the contentView to the desktop
// (#86 — the contentView's default-opaque setting was blocking alpha).

#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>

#include "displayxr_hooks.h"

static NSWindow *s_configured_window     = nil;
static BOOL      s_saved_window_opaque   = YES;
static NSColor  *s_saved_window_bg       = nil;
static BOOL      s_saved_content_wantsLayer = NO;
static BOOL      s_saved_content_layer_opaque = YES;
static CGColorRef s_saved_content_layer_bg = NULL;


static BOOL layer_tree_has_metal(CALayer *layer) {
	if (layer == nil) return NO;
	if ([layer isKindOfClass:[CAMetalLayer class]]) return YES;
	for (CALayer *child in layer.sublayers) {
		if (layer_tree_has_metal(child)) return YES;
	}
	return NO;
}

static BOOL is_displayxr_window(NSWindow *w) {
	return [NSStringFromClass([w class]) hasPrefix:@"DisplayXR"];
}

static NSWindow *find_unity_render_window(void) {
	NSWindow *key = [NSApp keyWindow];
	if (key != nil && !is_displayxr_window(key) && layer_tree_has_metal(key.contentView.layer)) {
		return key;
	}
	for (NSWindow *w in [NSApp windows]) {
		if (![w isVisible]) continue;
		if (is_displayxr_window(w)) continue;
		if (layer_tree_has_metal(w.contentView.layer)) {
			return w;
		}
	}
	return nil;
}

extern "C" DISPLAYXR_EXPORT void
displayxr_macos_configure_unity_nswindow(int enabled) {
	dispatch_block_t block = ^{
		if (enabled) {
			if (s_configured_window != nil) {
				return;
			}
			NSWindow *w = find_unity_render_window();
			if (w == nil) {
				displayxr_log("[DisplayXR] configure_unity_nswindow(1): no Unity render NSWindow found\n");
				return;
			}
			NSView *contentView = w.contentView;

			// Save current state
			s_saved_window_opaque       = [w isOpaque];
			s_saved_window_bg           = [w backgroundColor];
			s_saved_content_wantsLayer  = contentView.wantsLayer;
			s_saved_content_layer_opaque = contentView.layer ? contentView.layer.opaque : YES;
			if (contentView.layer && contentView.layer.backgroundColor != NULL) {
				s_saved_content_layer_bg = CGColorRetain(contentView.layer.backgroundColor);
			} else {
				s_saved_content_layer_bg = NULL;
			}

			// Flip NSWindow
			[w setOpaque:NO];
			[w setBackgroundColor:[NSColor clearColor]];

			// Flip contentView's layer — the layer beneath the overlay
			// (issue #86). Without this, alpha=0 in the overlay shows the
			// contentView's opaque background instead of the desktop.
			contentView.wantsLayer = YES;
			contentView.layer.opaque = NO;
			contentView.layer.backgroundColor = CGColorGetConstantColor(kCGColorClear);
			// Unity renders an opaque texture into contentView.layer (a
			// CAMetalLayer); CALayer draws .contents ON TOP of
			// .backgroundColor, so opaque=NO + clearColor alone isn't
			// enough — we must also wipe the existing contents so the
			// overlay's alpha=0 regions reveal the window background
			// (clearColor → desktop). Unity's main-window render path is
			// gated by XRSettings.gameViewRenderMode=None in the test
			// repo, so no new opaque frames should land here.
			contentView.layer.contents = nil;

			s_configured_window = w;
			displayxr_log("[DisplayXR] configure_unity_nswindow(1): NSWindow %p class=%s setOpaque:NO + contentView (%s) layer.opaque=NO + contents=nil\n",
			              (__bridge void *)w,
			              [NSStringFromClass([w class]) UTF8String],
			              [NSStringFromClass([contentView class]) UTF8String]);
		} else {
			if (s_configured_window == nil) {
				return;
			}
			NSWindow *w = s_configured_window;
			NSView *contentView = w.contentView;

			// Restore NSWindow
			[w setOpaque:s_saved_window_opaque];
			[w setBackgroundColor:(s_saved_window_bg ?: [NSColor windowBackgroundColor])];

			// Restore contentView's layer
			if (contentView.layer) {
				contentView.layer.opaque = s_saved_content_layer_opaque;
				contentView.layer.backgroundColor = s_saved_content_layer_bg;
			}
			contentView.wantsLayer = s_saved_content_wantsLayer;
			if (s_saved_content_layer_bg != NULL) {
				CGColorRelease(s_saved_content_layer_bg);
				s_saved_content_layer_bg = NULL;
			}

			displayxr_log("[DisplayXR] configure_unity_nswindow(0): restored NSWindow %p\n",
			              (__bridge void *)w);
			s_configured_window = nil;
			s_saved_window_bg = nil;
		}
	};

	if ([NSThread isMainThread]) {
		block();
	} else {
		dispatch_async(dispatch_get_main_queue(), block);
	}
}

// Move Unity's configured render NSWindow by (dx, dy) screen points. App
// code calls this from its own input handler (e.g. right-mouse-drag) to
// implement borderless-window drag — the policy choice (which button,
// where on the window, when) lives in the app; the plugin just exposes
// the mechanism. Win32 has an equivalent baked into the overlay HWND's
// WndProc (#57), but on Mac there is no separate overlay HWND, so the
// app drives it directly.
//
// Coordinates: screen-space delta in points (not pixels). Positive dx
// moves the window right; positive dy moves the window UP (Cocoa
// screen-coords are bottom-left origin). If the app's input source is
// in top-left pixel space (e.g. Unity's Y-flipped PointerPosition),
// negate dy before calling.
//
// No-op if no window has been configured yet (configure_unity_nswindow
// hasn't been called or already torn down). Marshalled to the AppKit
// main thread.
extern "C" DISPLAYXR_EXPORT void
displayxr_macos_offset_window(int dx, int dy) {
	dispatch_block_t block = ^{
		NSWindow *w = s_configured_window;
		if (w == nil) return;
		NSRect frame = w.frame;
		frame.origin.x += (CGFloat)dx;
		frame.origin.y += (CGFloat)dy;
		[w setFrameOrigin:frame.origin];
	};
	if ([NSThread isMainThread]) {
		block();
	} else {
		dispatch_async(dispatch_get_main_queue(), block);
	}
}

// Mac stub for the Windows-only shell-mode predicate. On macOS there is no
// shell/IPC session path yet, so this always returns 0. Lets the shared
// xrCreateSession hook reference the symbol unconditionally.
extern "C" DISPLAYXR_EXPORT int
displayxr_is_shell_mode(void) {
	return 0;
}

// macOS implementation of "is our process the foreground app?". Win32 uses
// it to gate keyboard polling (RIDEV_INPUTSINK delivers keystrokes from
// every window system-wide; without the gate the HUD would toggle on
// Shift+Tab typed in some other app). On macOS, Cocoa only delivers
// keyboard events to the active app — if Unity is seeing a keystroke at
// all, we are (or have just become) foreground. So returning 1
// unconditionally is correct AND avoids the [NSApp isActive] transient
// false-negative window during app-activation handoff that was making
// Shift+Tab unreliable in transparent-overlay builds. The Win32 reason
// for gating doesn't apply on Mac.
extern "C" DISPLAYXR_EXPORT int
displayxr_is_our_process_foreground(void) {
	return 1;
}
