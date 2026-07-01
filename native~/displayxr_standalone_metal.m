// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Metal helpers for the standalone preview session (macOS).
// Creates a native NSWindow for the runtime to composite into,
// and provides GPU blit for atlas → swapchain.

#import <Metal/Metal.h>
#import <AppKit/AppKit.h>
#import <Carbon/Carbon.h> // kVK_ANSI_* keycodes
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

// Don't become main window — let Unity keep main status.
// But DO allow key window so mouse events are delivered normally.
// Input suppression during title bar / resize drag is handled by
// window_is_interacting() checking frame vs content area.
- (BOOL)canBecomeMainWindow { return NO; }

// Freeze content during live resize. macOS blocks the main thread during
// live resize, so EditorApplication.update (our FrameTick) can't render
// new frames. Without this, the compositor stretches the last frame into
// the new window size → distortion. With this, the content stays at the
// last-rendered size and snaps to the correct size when resize ends.
- (BOOL)preservesContentDuringLiveResize { return YES; }

// Suppress the system beep on unhandled keyboard input. Apps polling key
// state via CGEventSourceKeyState (DisplayXRPreviewInput.IsKeyPressed)
// don't actually consume the event from the responder chain — so AppKit
// reaches the bottom and calls -noResponderFor: which plays NSBeep by
// default. Overriding to no-op silences it without affecting key-state
// polling. Real menu shortcuts (Cmd+W close, etc.) still work because
// they're handled higher up the chain.
- (void)noResponderFor:(SEL)eventSelector { }

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

	// Check if the cursor is over the window FRAME (title bar, resize edges)
	// but NOT the content area. Content-area clicks should allow camera rotation.
	NSPoint mouse = [NSEvent mouseLocation]; // screen coords
	NSRect windowFrame = [s_sa_window frame];
	if (!NSPointInRect(mouse, windowFrame))
		return 0; // Cursor not over window at all

	// Convert to window-local coords and check if inside the content rect
	NSRect contentRect = [s_sa_window contentRectForFrameRect:windowFrame];
	if (NSPointInRect(mouse, contentRect))
		return 0; // Cursor is in the content area — allow interaction

	// Cursor is over the window frame (title bar / resize edges) — suppress input
	return 1;
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

int
displayxr_sa_metal_get_preview_mouse_position(float *out_fx, float *out_fy)
{
	if (!s_sa_window || !s_sa_view) {
		if (out_fx) *out_fx = -1.0f;
		if (out_fy) *out_fy = -1.0f;
		return 0;
	}

	// Cursor in screen coords (points). +Y is up, origin at the bottom-left
	// of the primary screen — Cocoa convention.
	NSPoint cursor = [NSEvent mouseLocation];

	// Window's content rect in the same screen-coords space.
	NSRect frame = [s_sa_window frame];
	NSRect content = [s_sa_window contentRectForFrameRect:frame];

	if (!NSPointInRect(cursor, content)) {
		if (out_fx) *out_fx = -1.0f;
		if (out_fy) *out_fy = -1.0f;
		return 0;
	}

	// Fractional coords inside the content rect. Y inverts because Cocoa's
	// screen origin is bottom-left and we want top-left fractional output
	// (matches wsui layer rect convention).
	if (content.size.width <= 0 || content.size.height <= 0) {
		if (out_fx) *out_fx = -1.0f;
		if (out_fy) *out_fy = -1.0f;
		return 0;
	}
	CGFloat fx = (cursor.x - content.origin.x) / content.size.width;
	CGFloat fy = ((content.origin.y + content.size.height) - cursor.y) /
	              content.size.height;

	if (out_fx) *out_fx = (float)fx;
	if (out_fy) *out_fy = (float)fy;
	return 1;
}

int
displayxr_sa_macos_is_key_pressed(int ascii)
{
	// Translate ASCII (letters + digits) to Cocoa kVK_ANSI_* keycodes.
	// CGEventSourceKeyState polls the global hardware key state regardless
	// of which window has focus — what we need so app-side hotkeys can fire
	// while the standalone preview NSWindow owns input focus.
	int kvk = -1;
	int up = (ascii >= 'a' && ascii <= 'z') ? (ascii - 'a' + 'A') : ascii;
	switch (up) {
		case 'A': kvk = kVK_ANSI_A; break;
		case 'B': kvk = kVK_ANSI_B; break;
		case 'C': kvk = kVK_ANSI_C; break;
		case 'D': kvk = kVK_ANSI_D; break;
		case 'E': kvk = kVK_ANSI_E; break;
		case 'F': kvk = kVK_ANSI_F; break;
		case 'G': kvk = kVK_ANSI_G; break;
		case 'H': kvk = kVK_ANSI_H; break;
		case 'I': kvk = kVK_ANSI_I; break;
		case 'J': kvk = kVK_ANSI_J; break;
		case 'K': kvk = kVK_ANSI_K; break;
		case 'L': kvk = kVK_ANSI_L; break;
		case 'M': kvk = kVK_ANSI_M; break;
		case 'N': kvk = kVK_ANSI_N; break;
		case 'O': kvk = kVK_ANSI_O; break;
		case 'P': kvk = kVK_ANSI_P; break;
		case 'Q': kvk = kVK_ANSI_Q; break;
		case 'R': kvk = kVK_ANSI_R; break;
		case 'S': kvk = kVK_ANSI_S; break;
		case 'T': kvk = kVK_ANSI_T; break;
		case 'U': kvk = kVK_ANSI_U; break;
		case 'V': kvk = kVK_ANSI_V; break;
		case 'W': kvk = kVK_ANSI_W; break;
		case 'X': kvk = kVK_ANSI_X; break;
		case 'Y': kvk = kVK_ANSI_Y; break;
		case 'Z': kvk = kVK_ANSI_Z; break;
		case '0': kvk = kVK_ANSI_0; break;
		case '1': kvk = kVK_ANSI_1; break;
		case '2': kvk = kVK_ANSI_2; break;
		case '3': kvk = kVK_ANSI_3; break;
		case '4': kvk = kVK_ANSI_4; break;
		case '5': kvk = kVK_ANSI_5; break;
		case '6': kvk = kVK_ANSI_6; break;
		case '7': kvk = kVK_ANSI_7; break;
		case '8': kvk = kVK_ANSI_8; break;
		case '9': kvk = kVK_ANSI_9; break;
		default: return 0;
	}
	return CGEventSourceKeyState(kCGEventSourceStateHIDSystemState, (CGKeyCode)kvk) ? 1 : 0;
}

int
displayxr_sa_metal_get_preview_mouse_buttons(void)
{
	// `pressedMouseButtons` is a class method on NSEvent that returns the
	// global set of currently pressed mouse buttons, regardless of which
	// view has key focus. Bit layout matches Cocoa's NSEvent buttonNumber
	// (0=left, 1=right, 2=middle) — same convention as our Win32 path —
	// so callers can use the result without remapping.
	NSUInteger m = [NSEvent pressedMouseButtons];
	int b = 0;
	if (m & (1u << 0)) b |= 0x1;
	if (m & (1u << 1)) b |= 0x2;
	if (m & (1u << 2)) b |= 0x4;
	return b;
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
