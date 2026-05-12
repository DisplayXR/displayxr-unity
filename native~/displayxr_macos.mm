// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
//
// macOS-specific helpers for DisplayXRTransparentOverlay (issue #85).
// Flips Unity's NSWindow `isOpaque` so per-pixel alpha from the camera
// reaches the desktop via Cocoa per-pixel transparency. The runtime
// configures its own CAMetalLayer; the NSWindow is the app's job.

#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>

#include "displayxr_hooks.h"

// Mac stub for the Windows-only shell-mode predicate. On macOS there is no
// shell/IPC session path yet, so this always returns 0. Lets the shared
// xrCreateSession hook reference the symbol unconditionally.
extern "C" DISPLAYXR_EXPORT int
displayxr_is_shell_mode(void) {
    return 0;
}

static NSWindow *s_configured_window = nil;
static BOOL      s_saved_opaque      = YES;
static NSColor  *s_saved_bg          = nil;

// Heuristic for Unity's main render window:
//  - contentView's layer (or a sublayer) is a CAMetalLayer
//  - NOT a DisplayXR preview window (runtime-owned; class prefixed "DisplayXR")
//  - prefer keyWindow when it qualifies
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
            s_saved_opaque      = [w isOpaque];
            s_saved_bg          = [w backgroundColor];
            s_configured_window = w;
            [w setOpaque:NO];
            [w setBackgroundColor:[NSColor clearColor]];
            displayxr_log("[DisplayXR] configure_unity_nswindow(1): NSWindow %p class=%s setOpaque:NO\n",
                          (__bridge void *)w,
                          [NSStringFromClass([w class]) UTF8String]);
        } else {
            if (s_configured_window == nil) {
                return;
            }
            NSWindow *w = s_configured_window;
            [w setOpaque:s_saved_opaque];
            [w setBackgroundColor:(s_saved_bg ?: [NSColor windowBackgroundColor])];
            displayxr_log("[DisplayXR] configure_unity_nswindow(0): restored NSWindow %p opaque=%d\n",
                          (__bridge void *)w, (int)s_saved_opaque);
            s_configured_window = nil;
            s_saved_bg          = nil;
        }
    };

    if ([NSThread isMainThread]) {
        block();
    } else {
        dispatch_async(dispatch_get_main_queue(), block);
    }
}
