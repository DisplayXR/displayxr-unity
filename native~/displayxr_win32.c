// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Win32 window management for the DisplayXR Unity plugin.
//
// Two modes:
// 1. Standalone (default): Creates a transparent child overlay HWND on top of
//    Unity's main window. The compositor renders to this overlay.
// 2. Shell mode (DISPLAYXR_SHELL_SESSION=1): Passes Unity's top-level HWND
//    directly. Installs IAT hooks and a window subclass to keep Unity "active"
//    and processing input when the shell's compositor has foreground focus.

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "displayxr_exports.h"
#include "displayxr_shared_state.h"
#include "displayxr_win32.h" // own decls — newer GCC (MinGW cross-check) errors on implicit declarations

#pragma comment(lib, "dwmapi.lib")

// DWMWA_CLOAK is defined as 13 in dwmapi.h on newer SDKs. Provide a
// fallback so we don't need a specific Windows SDK version.
#ifndef DWMWA_CLOAK
#define DWMWA_CLOAK 13
#endif

// ============================================================================
// Standalone mode: overlay child window
// ============================================================================

static HWND s_overlay_hwnd = NULL;
static WNDPROC s_original_wndproc = NULL;
// (#256) The HWND parent_subclass_proc was installed on, remembered so the teardown
// can restore s_original_wndproc onto the SAME window. find_unity_hwnd() is not a
// safe substitute at teardown time — it can return a different top-level window than
// the one that was subclassed (see the same caveat in displayxr_set_simple_window).
static HWND s_parent_subclass_hwnd = NULL;
// (#256) Posted to the overlay so its own (main) thread performs the destroy:
// DestroyWindow is only valid on the creating thread, and the session-failure path
// that triggers it runs on the render thread. Mirrors the DXR_WM_PARK_OFFSCREEN
// PostMessage pattern used by shell mode.
#define DXR_WM_DESTROY_OVERLAY (WM_APP + 0x38)
static const wchar_t OVERLAY_CLASS_NAME[] = L"DisplayXROverlay";
// (#173) Dedicated provider window class (editor Play Mode weave target). Declared
// here alongside the overlay class so find_unity_hwnd can skip it too (a visible,
// >100px window of our own process that must never be mistaken for Unity's window).
static const wchar_t DEDICATED_CLASS_NAME[] = L"DisplayXRProviderWindow";
static int s_class_registered = 0;
// True when the overlay HWND is a top-level WS_POPUP + NOREDIRECTIONBITMAP
// (issue #57 transparent path) instead of a WS_CHILD of Unity's HWND.
// Set in displayxr_get_app_main_view; consumed by parent_subclass_proc to
// reposition the overlay in screen coords vs sizing within parent client.
static int s_overlay_is_toplevel = 0;

// Accumulated raw mouse-wheel delta on the overlay (Win32 WHEEL_DELTA units;
// 120 per notch). WM_MOUSEWHEEL atomically adds; C# reads + zeros via
// displayxr_consume_overlay_wheel_delta. Used by apps to drive zoom or
// other interactions — the plugin no longer self-resizes the overlay on
// wheel events (that was experimental; removed v1.2.2).
static volatile LONG s_overlay_wheel_accum = 0;

// ============================================================================
// Transparent overlay mode (issue #57): top-level WS_EX_NOREDIRECTIONBITMAP
// overlay HWND + alpha-native DComp compositing. Unity is cloaked + moved
// off-screen so transparent-zone clicks route to whatever's behind. The
// detailed mechanism comment lives near displayxr_set_transparent_overlay
// below. Mutually exclusive with shell mode.
// ============================================================================

static DWORD s_saved_style    = 0;
static DWORD s_saved_exstyle  = 0;
static int   s_overlay_active = 0;
// (startup white window) Set when the overlay's birth cloaked Unity's main HWND
// eagerly, BEFORE ApplyWindowing's coroutine could. Tracked separately from
// s_overlay_active because every existing un-cloak is gated on THAT flag, and an
// early cloak that never got reverted would leave the app permanently invisible.
static int   s_unity_early_cloaked = 0;

// (startup curtain) The overlay is BORN CLOAKED and stays invisible until the app
// is pacing steadily, so a user never sees the warm-up. Measured on the 3DLuma
// avatar: after the first frame lands, frame+1 costs 1.5 s and frames 2-8 run ~350 ms
// each before the app reaches a steady interval ~3 s later. Showing that is what
// reads as "jerky, and the whole system goes slow" -- and it also drags DWM into
// compositing the whole desktop against a presenter running at 3 fps, because a
// transparent NOREDIRECTIONBITMAP window is never eligible for independent flip.
//
// Steadiness is judged on the RATIO between consecutive app frame intervals, never
// on an absolute target: this app legitimately runs at 20 fps under
// DXR_APP_FRAME_DIVISOR=3, and an absolute 16.7 ms gate would hold the curtain down
// forever on exactly the configuration we recommend.
static int      s_curtain_down = 0;
static uint64_t s_curtain_start_ms = 0;
static uint64_t s_curtain_last_frame_ms = 0;
static uint64_t s_curtain_prev_gap_ms = 0;
static int      s_curtain_steady_run = 0;

// How many consecutive well-paced frames raise the curtain, and the backstop that
// raises it regardless. Without the backstop an app that never reaches a steady rate
// would stay invisible forever, which presents as "the app is broken".
#define DXR_CURTAIN_STEADY_FRAMES 20
#define DXR_CURTAIN_MAX_MS_DEFAULT 20000

// Defined with the other curtain helpers further down; the overlay-birth call site
// sits above them. Without this the C compiler implicit-declares it as returning
// int and the real static void definition is then a redefinition error.
static void curtain_lower(void);
static RECT  s_hit_rect       = {0, 0, 0, 0};

// Unity's pre-transparent screen rect, captured in displayxr_set_transparent_overlay
// just before we move Unity off-screen. We restore Unity to the overlay's current
// position on disable (so the user's window appears where the avatar last was),
// but fall back to this rect if the overlay has been destroyed by then.
static RECT  s_unity_saved_rect = {0, 0, 0, 0};
static int   s_unity_offscreen  = 0;

// Off-screen position used for Unity's HWND in transparent mode. The cursor
// never lands here, so OS hit-testing routes transparent-zone clicks past
// our cloaked-but-still-in-z-order Unity HWND to whatever desktop app is at
// the actual cursor position. (-32000, -32000) is the canonical Win32
// off-screen position — same value used for minimized-window placeholder
// rects.
#define DISPLAYXR_UNITY_OFFSCREEN_X (-32000)
#define DISPLAYXR_UNITY_OFFSCREEN_Y (-32000)

// Per-pixel hit-test override. When set to 1, WM_NCHITTEST returns HTCLIENT
// inside the cursor's current frame regardless of s_hit_rect. C# updates this
// each frame from a Physics.Raycast at the current cursor position so the hit
// region matches the actual cube silhouette (not just the AABB), and clicks
// in transparent zones around the cube fall through to the desktop.
// Default 1 preserves the rect-only behavior when no clickableRenderers are
// configured.
static int   s_hit_active     = 1;

// Forward declaration — defined further down in the shell-mode section.
// displayxr_get_overlay_pointer (above the shell-mode block) reads it.
static volatile SHORT s_vkey_state[256];

// Right-click drag state (issue #57 task 2). Cursor anchor is in screen
// coords; window anchor is unity HWND's top-left at WM_RBUTTONDOWN time.
// Frame deltas are computed against these so the window tracks the cursor
// 1:1 even if Windows coalesces or drops mouse-move messages.
static int   s_drag_active        = 0;
static POINT s_drag_anchor_screen = {0, 0};
static POINT s_drag_anchor_window = {0, 0};

// Custom capture-based RESIZE of the decorated overlay. DefWindowProc's sizing-
// border modal loop silently no-ops on a WS_EX_NOREDIRECTIONBITMAP window (no
// DWM redirection surface for the live preview) even though its MOVE loop works,
// so we drive resize ourselves from the WM_NCLBUTTONDOWN hit on a resize border
// (HTLEFT/HTRIGHT/HTTOP/HTBOTTOM/corners), #61-bracketed like the move drag.
static int   s_resize_active        = 0;
static int   s_resize_edge          = 0;   // HT* code of the grabbed border
static POINT s_resize_anchor_screen = {0, 0};
static RECT  s_resize_anchor_rect   = {0, 0, 0, 0};
#define DXR_MIN_WINDOW_PX 200

// (display-zones port) Quit request raised from WM_CLOSE on the overlay HWND
// (decorated close button or Alt+F4). Default WM_CLOSE would DestroyWindow only
// the overlay, leaving cloaked Unity running headless; instead we swallow it and
// flag a quit for C# to poll (displayxr_consume_overlay_close_request) →
// Application.Quit(), which tears the whole app down cleanly.
static volatile LONG s_overlay_close_requested = 0;

static int
is_resize_ht(WPARAM ht)
{
	return ht == HTLEFT || ht == HTRIGHT || ht == HTTOP || ht == HTBOTTOM ||
	       ht == HTTOPLEFT || ht == HTTOPRIGHT ||
	       ht == HTBOTTOMLEFT || ht == HTBOTTOMRIGHT;
}

// (#131) App-managed fixed full-screen window mode. When set via
// displayxr_set_overlay_fullscreen(1), the native right-drag MOVE in
// overlay_wnd_proc is disabled — the app owns all window interaction by placing
// content in virtual rects inside the fixed full-screen overlay. The overlay
// still records button state (so displayxr_get_overlay_pointer reports the
// right button to the app for app-driven translate).
static int   s_app_managed_window = 0;

// (#131) When set before the overlay is created (displayxr_set_fullscreen_
// overlay_pref, from a fullscreen 2D-surround app's earliest init), birth the
// overlay covering NEARLY the whole monitor — the monitor rect minus 1px on the
// right and bottom — instead of Unity's (small, windowed) client rect.
//
// Two reasons for "minus 1px" rather than the exact monitor rect:
//  - A top-level window sized to the EXACT monitor rect trips Windows
//    fullscreen-optimization / independent-flip (DirectFlip), which bypasses DWM
//    alpha compositing — the same path behind the documented 5s white
//    overexposure (and the startup flashes/hang seen with an exact-fullscreen
//    overlay). One pixel short keeps the window DWM-composited. A clean-era log
//    (2026-05-11) confirmed a near-full 3517x2160 window-sized overlay was
//    perfectly smooth, so coverage was never the problem — exact-monitor was.
//  - The 1px lives at the bottom-right; the overlay still covers the taskbar
//    (tiger feet render over it) for all but that last row/column.
//
// Born at this size up front so the overlay never needs a post-creation resize
// (a resize recreates the runtime's presentation swapchain = a flash). Read at
// creation time; setting it later has no effect on birth size.
static int   s_fullscreen_overlay_pref = 0;

// (#166) Provider in-app weave: when set before displayxr_get_app_main_view(),
// the overlay is created as a TOP-LEVEL WS_POPUP + WS_EX_NOREDIRECTIONBITMAP
// (owned by Unity) instead of a WS_CHILD. A WS_CHILD doesn't composite the
// runtime's D3D12 DComp flip swapchain (the weave shows white/see-through); a
// top-level NOREDIRECTIONBITMAP popup composites it exactly like the hook path's
// transparent overlay, but WITHOUT the transparent-app extras (no Unity cloak /
// off-screen move / click-through). Opaque weave over Unity's own window = one
// window UX. Set by the custom IUnityXRDisplay provider (the default app-owned
// window path; opt out with DISPLAYXR_PROV_SELFHOST=1).
static int   s_provider_opaque_overlay = 0;

// (#131) App-requested overlay cursor shape, applied in WM_SETCURSOR. The
// overlay window class has no hCursor, so we must SetCursor explicitly (incl.
// the arrow for shape 0) or a resize cursor would stick. 0=arrow, 1=size-WE,
// 2=size-NS, 3=size-NWSE, 4=size-NESW, 5=size-all (move). Used by the region
// editor to show resize affordances on window edges/corners and region lines.
static volatile LONG s_overlay_cursor = 0;

// True once C# has pushed at least one per-pixel silhouette mask via
// displayxr_set_overlay_hit_mask. Acts as a one-way upgrade flag — the
// rect-based displayxr_set_overlay_hit_rect path STOPS calling
// SetWindowRgn after this flips so the per-pixel mask owns OS routing
// (much tighter silhouette match, opens up the gap between the tiger's
// legs and other concavities that an AABB swallows). C# is free to
// keep calling _hit_rect for opaque-mode users; the rect just becomes
// hit-test bookkeeping in transparent mode, not the routing driver.
static int   s_hit_mask_active    = 0;
// (#259) FNV-1a of the last applied hit-mask region (rects + dst size).
// SetWindowRgn was called EVERY FRAME with bRedraw=TRUE even when the region
// was identical, forcing a repaint invalidation 60x/s -- visible as flicker at
// the region boundary while the window is dragged. Identical regions are now
// skipped. 0 = no region applied / unknown (always apply next time).
static unsigned long long s_hit_mask_region_hash = 0;

// (#131) Opaque surround rect (overlay client pixels, top-left origin) for a 2D
// element drawn in the surround region — e.g. a high-res text bubble — that must
// catch clicks even though it sits outside the 3D silhouette. UNION-ed into the
// SetWindowRgn region built by displayxr_set_overlay_hit_mask each frame. Invalid
// by default (no bubble) so empty surround keeps routing clicks to the desktop.
//
// The rect is a coarse bounding box; for a non-rectangular surround element (a
// comic bubble with a triangular tail) it would catch clicks in the empty corners
// beside the shape. The surround MASK below supersedes it: a per-pixel alpha of
// the actual shape, RLE'd into the region exactly like the tiger silhouette — but
// flat 2D (no disparity / no per-view union), so the caller can rasterize it on
// the CPU. When a mask is set the caller should clear the rect (both are unioned
// in if both are valid). The rect path stays for older callers / compat.
static int   s_surround_rect_valid = 0;
static RECT  s_surround_rect       = {0, 0, 0, 0};

// (#131) Per-pixel surround shape mask (non-zero = opaque/catch). Owned copy,
// mapped over s_surround_mask_dst (overlay client px, top-left) when the region is
// rebuilt. NULL/invalid by default. Unlike the tiger hit-mask (which maps into the
// canvas sub-rect and is owned by the silhouette each frame), this maps wherever
// the caller places its 2D element in the full surround and is unioned in.
static uint8_t *s_surround_mask       = NULL;
static int      s_surround_mask_w     = 0;
static int      s_surround_mask_h     = 0;
static RECT     s_surround_mask_dst   = {0, 0, 0, 0};
static int      s_surround_mask_valid = 0;

// ============================================================================
// Simple-window mode (avatar-style): bind + style Unity's REAL main HWND
// directly. No off-screen overlay, no DWM cloak, no off-screen move — Unity is
// the on-screen render target the runtime composites into. Click-through is
// region-based (the existing hit-mask SetWindowRgn path retargeted to this
// HWND); a borderless right-drag moves the window (#61-bracketed); B toggles
// WS_POPUP <-> WS_OVERLAPPEDWINDOW. Mutually exclusive with the overlay path.
// ============================================================================
static HWND    s_simple_hwnd          = NULL;   // Unity's real main HWND
static WNDPROC s_simple_orig_wndproc  = NULL;   // saved Unity wndproc (subclass)
static int     s_simple_active        = 0;      // styling applied
static DWORD   s_simple_saved_style   = 0;
static DWORD   s_simple_saved_exstyle = 0;
static int     s_window_decorated     = 0;      // WS_OVERLAPPEDWINDOW vs WS_POPUP
static int     s_simple_topmost       = 0;
// Borderless right-drag move state (mirrors the overlay's #61-bracketed drag;
// left button stays free for scene interaction, e.g. DragRotateCube).
static int     s_simple_drag_active        = 0;
static POINT   s_simple_drag_anchor_screen = {0, 0};
static POINT   s_simple_drag_anchor_window = {0, 0};

// Forward decl — the window the avatar-style decoration toggle manages
// (overlay when active, else the dormant simple-window real HWND). Defined
// with the region helpers further down.
static HWND    managed_window_hwnd(void);

// ============================================================================
// Shell mode detection
// ============================================================================

static int s_shell_checked = 0;
static int s_shell_mode = 0;

int
displayxr_is_shell_mode(void)
{
	if (!s_shell_checked) {
		// The shell + runtime set DISPLAYXR_WORKSPACE_SESSION=1 (renamed from
		// the legacy DISPLAYXR_SHELL_SESSION). Accept the old name too so an
		// older shell still flips the plugin into shell/IPC mode.
		const char *val = getenv("DISPLAYXR_WORKSPACE_SESSION");
		if (val == NULL)
			val = getenv("DISPLAYXR_SHELL_SESSION");
		s_shell_mode = (val != NULL && val[0] == '1' && val[1] == '\0');
		s_shell_checked = 1;
	}
	return s_shell_mode;
}

// ============================================================================
// Window discovery (shared by both modes)
// ============================================================================

// Skip our OWN windows (the overlay created by displayxr_get_app_main_view, or the
// #173 dedicated provider window) when searching for Unity's main HWND. After ours
// becomes visible it can otherwise win the foreground/visible-window race in
// find_unity_hwnd and we end up styling / hooking our own window as Unity, leaving
// Unity untouched.
static int
is_displayxr_overlay_class(HWND hwnd)
{
	wchar_t cls[64] = {0};
	if (GetClassNameW(hwnd, cls, 63) == 0)
		return 0;
	return wcscmp(cls, OVERLAY_CLASS_NAME) == 0
	    || wcscmp(cls, DEDICATED_CLASS_NAME) == 0;
}

static HWND
find_unity_hwnd(void)
{
	DWORD our_pid = GetCurrentProcessId();
	HWND unity_hwnd = NULL;

	HWND fg = GetForegroundWindow();
	if (fg != NULL && !is_displayxr_overlay_class(fg)) {
		DWORD fg_pid = 0;
		GetWindowThreadProcessId(fg, &fg_pid);
		if (fg_pid == our_pid)
			unity_hwnd = fg;
	}

	if (unity_hwnd == NULL) {
		HWND hwnd = NULL;
		while ((hwnd = FindWindowExW(NULL, hwnd, NULL, NULL)) != NULL) {
			if (is_displayxr_overlay_class(hwnd))
				continue;
			DWORD pid = 0;
			GetWindowThreadProcessId(hwnd, &pid);
			if (pid == our_pid && IsWindowVisible(hwnd)) {
				RECT rc;
				if (GetClientRect(hwnd, &rc) && (rc.right - rc.left) > 100) {
					unity_hwnd = hwnd;
					break;
				}
			}
		}
	}

	return unity_hwnd;
}

// (#266) Accessor so other TUs (the provider) can reach the app's main window
// without duplicating the class-skipping enumeration. find_unity_hwnd is static
// because it is used all over this file; this is the only sanctioned way out.
void *
displayxr_find_unity_hwnd(void)
{
	return (void *)find_unity_hwnd();
}

// ============================================================================
// Standalone mode: overlay window + parent subclass
// ============================================================================

// Cross-process click-through architecture (issue #57, post-Approach-C):
//
// We toggle WS_EX_TRANSPARENT on the overlay HWND from
// displayxr_set_overlay_hit_active() based on the C# per-frame raycast
// at the current cursor position:
//
//   - cursor over a clickable renderer (cube silhouette) → s_hit_active=1
//     → WS_EX_TRANSPARENT CLEAR → overlay catches WM_LBUTTON*/WM_MOUSEMOVE
//     etc., posts them to Unity for normal handling.
//
//   - cursor over a transparent zone → s_hit_active=0 → WS_EX_TRANSPARENT
//     SET → OS skips us in WindowFromPoint hit-test and routes input
//     directly to whatever desktop app is at the cursor. Cloaked Unity
//     is moved off-screen at (-32000,-32000) in displayxr_set_transparent_
//     overlay so it is NOT in the hit-test path; clicks reach the actual
//     desktop window underneath.
//
// Native OS routing gives us cross-process click activation, real
// DefWindowProc modal SC_MOVE/SC_SIZE/SC_CLOSE loops with proper
// GetKeyState (synchronously dispatched via the target's own input
// queue), native cursor adaptation over resize edges and help buttons,
// native menu activation, native hover and TrackMouseEvent — without
// any cross-process PostMessage gymnastics on our side.
//
// Earlier attempts to manually forward WM_LBUTTON* / WM_NCLBUTTON* /
// WM_MOUSEMOVE to discovered target windows via FindWindowExW +
// PostMessage (#57 sessions 4–5, Approach C) failed because:
//   - DefWindowProc's modal loops on the target poll GetKeyState, which
//     reflects the target thread's queue-synchronous input state, not
//     the live OS state — our PostMessage'd DOWN arrives without a
//     matching key-down event in the target's queue, so the loop bails.
//     SC_MOVE drag worked only by timing luck; SC_CLOSE never worked.
//   - Foreground activation transfer across processes via
//     SetForegroundWindow was unreliable for non-classic Win32 chrome
//     (UWP/WinUI title bars, popup menus).
//   - Per-WM_MOUSEMOVE / per-WM_SETCURSOR target discovery via
//     FindWindowExW + SendMessageTimeoutW(WM_NCHITTEST) caused
//     wndproc-level lag when hovering over a foreign window.
//
// WS_EX_LAYERED is the canonical companion for WS_EX_TRANSPARENT hit-
// test pass-through (per MSDN) but conflicts with our WS_EX_NOREDIRECTION
// BITMAP per-pixel alpha. On Windows 10/11 with NOREDIRECTIONBITMAP +
// DComp compositing, the OS appears to honor WS_EX_TRANSPARENT for
// hit-test routing without requiring WS_EX_LAYERED.
// ============================================================================
// Click-through diagnostic instrumentation (issue #57)
//
// Two probes for correlating overlay input behavior with the OS hit-test:
//
//   1. WH_MOUSE_LL (global low-level mouse hook): fires before any
//      window's wndproc, so we can log WindowFromPoint(cursor) — the
//      OS's actual hit-test result — for every button event. Confirms
//      whether the OS routed the click to us or past us in real time.
//
//   2. Entry log at the top of overlay_wnd_proc's button handlers: fires
//      iff the OS dispatched the message to us (so WS_EX_TRANSPARENT
//      was OFF at the dispatch), with a live re-read of the exstyle.
//
// Both write to displayxr_log with [LLMouse] / [OvlWnd] prefixes.
// ============================================================================

static HHOOK         s_ll_mouse_hook    = NULL;
static volatile LONG s_ll_mouse_seq     = 0;
static DWORD         s_ll_install_tid   = 0;

static const char *
ll_msg_name(UINT m)
{
	switch (m) {
	case WM_LBUTTONDOWN:   return "WM_LBUTTONDOWN";
	case WM_LBUTTONUP:     return "WM_LBUTTONUP";
	case WM_LBUTTONDBLCLK: return "WM_LBUTTONDBLCLK";
	case WM_RBUTTONDOWN:   return "WM_RBUTTONDOWN";
	case WM_RBUTTONUP:     return "WM_RBUTTONUP";
	case WM_MBUTTONDOWN:   return "WM_MBUTTONDOWN";
	case WM_MBUTTONUP:     return "WM_MBUTTONUP";
	default:               return "WM_OTHER";
	}
}

static LRESULT CALLBACK
displayxr_ll_mouse_proc(int nCode, WPARAM wParam, LPARAM lParam)
{
	// Win32 contract: anything other than HC_ACTION must chain immediately.
	if (nCode != HC_ACTION)
		return CallNextHookEx(NULL, nCode, wParam, lParam);

	// Button events only. Skip WM_MOUSEMOVE / WM_MOUSEWHEEL — global LL
	// hook fires per pixel of cursor motion across every desktop process,
	// and Windows silently unhooks slow callbacks (LowLevelHooksTimeout,
	// default 300 ms; HKCU\Control Panel\Desktop). Keep the hot path tight.
	UINT msg = (UINT)wParam;
	if (msg != WM_LBUTTONDOWN && msg != WM_LBUTTONUP &&
	    msg != WM_RBUTTONDOWN && msg != WM_RBUTTONUP &&
	    msg != WM_MBUTTONDOWN && msg != WM_MBUTTONUP)
		return CallNextHookEx(NULL, nCode, wParam, lParam);

	MSLLHOOKSTRUCT *p = (MSLLHOOKSTRUCT *)lParam;
	HWND target  = WindowFromPoint(p->pt);
	HWND root    = (target != NULL) ? GetAncestor(target, GA_ROOT) : NULL;
	HWND unity   = find_unity_hwnd();
	DWORD our_pid = GetCurrentProcessId();

	wchar_t cls[64]   = {0};
	wchar_t title[65] = {0};
	DWORD target_pid = 0;
	if (target != NULL) {
		GetClassNameW(target, cls, 63);
		GetWindowThreadProcessId(target, &target_pid);
		// Use SendMessageTimeoutW(WM_GETTEXT) instead of GetWindowTextW —
		// for cross-process targets, GetWindowTextW sends a synchronous
		// WM_GETTEXT to the target's message pump and blocks us
		// indefinitely if the target is hung. SMTO_ABORTIFHUNG + 50 ms
		// keeps the LL callback well under the 300 ms LowLevelHooksTimeout
		// even when WindowFromPoint resolves to an unresponsive window.
		DWORD_PTR got = 0;
		SendMessageTimeoutW(target, WM_GETTEXT, 64, (LPARAM)title,
		                    SMTO_ABORTIFHUNG | SMTO_BLOCK, 50, &got);
	}

	int is_overlay = (target == s_overlay_hwnd || root == s_overlay_hwnd) ? 1 : 0;
	int is_unity   = (unity != NULL && (target == unity || root == unity)) ? 1 : 0;

	LONG seq = InterlockedIncrement(&s_ll_mouse_seq);
	displayxr_log("[DisplayXR][LLMouse] seq=%ld tick=%lu msg=%s(0x%04X) pt=(%d,%d) "
	              "hwnd=%p root=%p class='%ls' pid=%lu title='%ls' "
	              "is_overlay=%d is_unity=%d our_pid=%lu\n",
	              seq, (unsigned long)GetTickCount(),
	              ll_msg_name(msg), (unsigned)msg,
	              (int)p->pt.x, (int)p->pt.y,
	              (void *)target, (void *)root,
	              cls, (unsigned long)target_pid, title,
	              is_overlay, is_unity, (unsigned long)our_pid);

	// Observation only — never block delivery.
	return CallNextHookEx(NULL, nCode, wParam, lParam);
}

static void
displayxr_install_ll_mouse_hook(void)
{
	if (s_ll_mouse_hook != NULL)
		return;

	// WH_MOUSE_LL is global but the OS calls back on the install thread
	// — and only as long as that thread pumps messages. The transparent
	// overlay enable runs on Unity's main thread, which has a pump.
	s_ll_install_tid = GetCurrentThreadId();
	s_ll_mouse_hook  = SetWindowsHookExW(WH_MOUSE_LL,
	                                     displayxr_ll_mouse_proc,
	                                     GetModuleHandleW(NULL), 0);
	if (s_ll_mouse_hook == NULL) {
		displayxr_log("[DisplayXR][LLMouse] hook install FAILED tid=%lu err=%lu\n",
		              (unsigned long)s_ll_install_tid,
		              (unsigned long)GetLastError());
	} else {
		displayxr_log("[DisplayXR][LLMouse] hook installed hhk=%p tid=%lu result=ok\n",
		              (void *)s_ll_mouse_hook,
		              (unsigned long)s_ll_install_tid);
	}
}

static LRESULT CALLBACK
overlay_wnd_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	// Diagnostic entry log for button events (issue #57 session 5). Fires
	// iff the OS dispatched a button message to us — i.e., WS_EX_TRANSPARENT
	// did NOT skip us in WindowFromPoint. Re-reads the live exstyle so we
	// can prove whether the OS honored the bit at this exact dispatch (the
	// existing toggle log only proves what we *set*). Pair the
	// `ll_seq_at_entry` value with the [LLMouse] line of the same seq to
	// see what WindowFromPoint resolved at the OS hit-test that preceded
	// this dispatch. Not gated on WM_MOUSEMOVE — flooding risk and the
	// handoff doc explicitly excluded MOUSEMOVE from forward_click.
	if (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP ||
	    msg == WM_LBUTTONDBLCLK ||
	    msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP ||
	    msg == WM_MBUTTONDOWN || msg == WM_MBUTTONUP) {
		DWORD ex_now = (DWORD)GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
		POINT scr;
		GetCursorPos(&scr);
		displayxr_log("[DisplayXR][OvlWnd] msg=%s(0x%04X) hit_active=%d "
		              "ws_ex_transparent=%d client=(%d,%d) screen=(%d,%d) "
		              "ll_seq_at_entry=%ld\n",
		              ll_msg_name(msg), (unsigned)msg, s_hit_active,
		              (ex_now & WS_EX_TRANSPARENT) ? 1 : 0,
		              GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam),
		              (int)scr.x, (int)scr.y,
		              (long)s_ll_mouse_seq);

		// Update s_vkey_state regardless of how this event will be
		// routed below (captured / forwarded). C# polled state in
		// displayxr_get_overlay_pointer reads this array; without
		// the update, forwarded events (hit_active=0) never reach
		// Unity's wndproc subclass and the polled state stays stale.
		// Symptom: drag a press from on-target to off-target, release
		// off-target — overlay forwards the UP, Unity never sees it,
		// C# thinks left is still held, drag-rotate gets stuck and
		// keeps following the cursor after release. (#57 / drag stuck.)
		int vk = -1;
		BOOL pressed = FALSE;
		switch (msg) {
		case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: vk = VK_LBUTTON; pressed = TRUE;  break;
		case WM_LBUTTONUP:                          vk = VK_LBUTTON; pressed = FALSE; break;
		case WM_RBUTTONDOWN:                        vk = VK_RBUTTON; pressed = TRUE;  break;
		case WM_RBUTTONUP:                          vk = VK_RBUTTON; pressed = FALSE; break;
		case WM_MBUTTONDOWN:                        vk = VK_MBUTTON; pressed = TRUE;  break;
		case WM_MBUTTONUP:                          vk = VK_MBUTTON; pressed = FALSE; break;
		}
		if (vk != -1) {
			s_vkey_state[vk] = pressed ? (SHORT)(0x8000 | 0x0001) : (SHORT)0x0001;
		}
	}

	// (#166) Provider in-app weave: the overlay is a pure display surface sitting
	// over Unity's client area. Pass EVERY hit straight through to Unity beneath so
	// Unity's keyboard/mouse work normally and no overlay drag/hit/foreground logic
	// fires. (s_overlay_is_toplevel is also set — it's genuinely top-level — but the
	// transparent-app interaction model below must NOT apply to the provider.)
	if (s_provider_opaque_overlay && msg == WM_NCHITTEST)
		return HTTRANSPARENT;

	switch (msg) {
	case WM_NCHITTEST: {
		if (s_overlay_is_toplevel) {
			// Decorated (avatar B-toggle): defer to DefWindowProc so the
			// title bar + sizing border hit-test normally (HTCAPTION /
			// HTLEFT / ...) for OS move/resize.
			if (s_window_decorated)
				break;
			// Borderless transparent overlay: the SetWindowRgn silhouette
			// drives OS routing; any hit the OS delivers is inside the
			// shape, so claim it as client.
			return HTCLIENT;
		}
		// Opaque WS_CHILD path: rect-based click-through is correct
		// (the runtime composites into the child and its parent stays
		// opaque, so per-pixel click-through is the right primitive).
		POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
		ScreenToClient(hwnd, &pt);
		if (s_hit_active &&
		    pt.x >= s_hit_rect.left && pt.x < s_hit_rect.right &&
		    pt.y >= s_hit_rect.top  && pt.y < s_hit_rect.bottom)
			return HTCLIENT;
		return HTTRANSPARENT;
	}
	case WM_MOUSEACTIVATE:
		// Decorated (avatar B-toggle): allow activation so the title bar
		// drag/resize works normally. Borderless: refuse activation
		// (belt-and-braces with WS_EX_NOACTIVATE) so foreground stays on
		// Unity (cloaked but active).
		if (s_window_decorated)
			break;
		return MA_NOACTIVATE;

	case WM_SETCURSOR:
		// (#131) App-driven cursor for the region editor's resize
		// affordances. WM_NCHITTEST returns HTCLIENT for the toplevel
		// overlay, so the low word is HTCLIENT whenever the cursor is in
		// our (region-clipped) client area. The class has no hCursor, so
		// we always SetCursor explicitly — incl. the arrow for shape 0 —
		// otherwise a previously-set resize cursor would persist.
		if (s_overlay_is_toplevel && LOWORD(lParam) == HTCLIENT) {
			// IDC_* are integer resource atoms shared by the A/W APIs;
			// cast to LPCWSTR so LoadCursorW takes them without a
			// LPSTR→LPCWSTR type warning (the value is unchanged).
			LPCWSTR id = (LPCWSTR)IDC_ARROW;
			switch ((int)s_overlay_cursor) {
			case 1: id = (LPCWSTR)IDC_SIZEWE;   break;
			case 2: id = (LPCWSTR)IDC_SIZENS;   break;
			case 3: id = (LPCWSTR)IDC_SIZENWSE; break;
			case 4: id = (LPCWSTR)IDC_SIZENESW; break;
			case 5: id = (LPCWSTR)IDC_SIZEALL;  break;
			}
			SetCursor(LoadCursorW(NULL, id));
			return TRUE;
		}
		break;

	// ----- decorated frame: custom capture-based resize -----
	// Only when decorated and the OS hit-tested a sizing border. DefWindowProc's
	// own SC_SIZE loop no-ops on this NOREDIRECTIONBITMAP window, so we drive it.
	case WM_NCLBUTTONDOWN:
		displayxr_log("[DisplayXR][OvlWnd] WM_NCLBUTTONDOWN ht=0x%04X decorated=%d resize=%d\n",
		              (unsigned)wParam, s_window_decorated, is_resize_ht(wParam) ? 1 : 0);
		if (s_window_decorated && is_resize_ht(wParam)) {
			s_resize_active = 1;
			s_resize_edge = (int)wParam;
			GetCursorPos(&s_resize_anchor_screen);
			GetWindowRect(hwnd, &s_resize_anchor_rect);
			SetCapture(hwnd);
			SendMessageW(hwnd, WM_ENTERSIZEMOVE, 0, 0);
			displayxr_log("[DisplayXR][OvlWnd] resize START edge=0x%04X rect=(%ld,%ld %ldx%ld)\n",
			              (unsigned)wParam,
			              s_resize_anchor_rect.left, s_resize_anchor_rect.top,
			              s_resize_anchor_rect.right - s_resize_anchor_rect.left,
			              s_resize_anchor_rect.bottom - s_resize_anchor_rect.top);
			return 0;
		}
		break; // HTCAPTION (move), sysmenu, buttons → DefWindowProc

	case WM_NCRBUTTONDOWN:
	case WM_NCMBUTTONDOWN:
		displayxr_log("[DisplayXR][OvlWnd] WM_NC*BUTTONDOWN msg=0x%04X ht=0x%04X decorated=%d\n",
		              (unsigned)msg, (unsigned)wParam, s_window_decorated);
		break;

	case WM_SYSCOMMAND:
		// SC_SIZE/SC_MOVE/SC_CLOSE diagnostics (mask off the low 4 bits the OS
		// uses for the active-edge / accelerator).
		displayxr_log("[DisplayXR][OvlWnd] WM_SYSCOMMAND sc=0x%04X decorated=%d\n",
		              (unsigned)(wParam & 0xFFF0), s_window_decorated);
		break;

	// ----- right-button: capture-based drag of the OVERLAY window -----
	//
	// In transparent mode (Approach A from #57 session 4), Unity's HWND
	// lives off-screen at (-32000, -32000) so the OS routes transparent-
	// zone clicks to whatever desktop app is at the cursor — Unity is no
	// longer in the hit-test path. As a consequence, dragging Unity does
	// nothing visible and decouples it from the avatar; we drag the
	// OVERLAY HWND instead. parent_subclass_proc no longer follows Unity
	// when s_overlay_is_toplevel — the overlay tracks its own position
	// and pushes it to the runtime via WM_MOVE / WM_SIZE handlers below.
	//
	// Same OS-modal-drag caveat as before: SR SDK weaver phase-snap
	// requires DefWindowProc to own the drag loop, but our window-style
	// requirements (WS_EX_NOREDIRECTIONBITMAP for per-pixel alpha,
	// WS_EX_NOACTIVATE) block that. Tracked in displayxr-runtime#193.
	case WM_RBUTTONDOWN: {
		// Reaching this case means the OS routed the click to us, so
		// WS_EX_TRANSPARENT is currently OFF (s_hit_active=1 — cursor
		// over the cube). When the cursor is in a transparent zone,
		// WS_EX_TRANSPARENT is set and the OS routes past us natively
		// to the desktop window underneath.
		//
		// Cube area: claim OS foreground so subsequent keyboard goes
		// to us (Unity via INPUTSINK). WS_EX_NOACTIVATE blocks
		// click-driven activation, but programmatic SFW from "received
		// last input event" is allowed. See
		// displayxr_is_our_process_foreground.
		SetForegroundWindow(hwnd);
		// (#131) In app-managed fixed full-screen mode the app owns
		// window translation (it moves a virtual rect, not the HWND),
		// so skip the native capture-based MOVE. We still claimed
		// foreground above (keyboard focus) and recorded the right
		// button earlier, so displayxr_get_overlay_pointer reports it
		// to the app's translate logic.
		if (!s_app_managed_window) {
			GetCursorPos(&s_drag_anchor_screen);
			RECT wr;
			GetWindowRect(hwnd, &wr);
			s_drag_anchor_window.x = wr.left;
			s_drag_anchor_window.y = wr.top;
			s_drag_active = 1;
			SetCapture(hwnd);
			// #61: synchronous bracketing so the SR SDK weaver's
			// WndProc subclass sees the in-drag flag and phase-snaps
			// the window to lenticular-aligned pixels. Must precede
			// the first SetWindowPos in WM_MOUSEMOVE.
			SendMessageW(hwnd, WM_ENTERSIZEMOVE, 0, 0);
		}
		return 0;
	}
	case WM_RBUTTONUP:
		if (s_drag_active) {
			// Clear the flag before sending WM_EXITSIZEMOVE so the
			// recursive WM_CAPTURECHANGED triggered by ReleaseCapture()
			// won't re-send it.
			s_drag_active = 0;
			SendMessageW(hwnd, WM_EXITSIZEMOVE, 0, 0);
			ReleaseCapture();
			return 0;
		}
		break;
	case WM_CAPTURECHANGED: {
		int was_active = s_drag_active || s_resize_active;
		s_drag_active = 0;
		s_resize_active = 0;
		if (was_active)
			SendMessageW(hwnd, WM_EXITSIZEMOVE, 0, 0);
		break;
	}

	// ----- mouse-move: drag overlay if active, else forward to Unity -----
	// When WS_EX_TRANSPARENT is OFF (cube area), the overlay receives
	// these. When it's ON (transparent zone), the OS routes WM_MOUSEMOVE
	// to the desktop app at the cursor natively — we never see them, so
	// hover / TrackMouseEvent / tooltips / taskbar previews / cursor
	// adaptation all fire on the real recipient without our help.
	case WM_MOUSEMOVE: {
		if (s_resize_active) {
			POINT cur;
			GetCursorPos(&cur);
			int dx = cur.x - s_resize_anchor_screen.x;
			int dy = cur.y - s_resize_anchor_screen.y;
			RECT r = s_resize_anchor_rect;
			switch (s_resize_edge) {
			case HTLEFT:        r.left   += dx;               break;
			case HTRIGHT:       r.right  += dx;               break;
			case HTTOP:         r.top    += dy;               break;
			case HTBOTTOM:      r.bottom += dy;               break;
			case HTTOPLEFT:     r.left   += dx; r.top += dy;  break;
			case HTTOPRIGHT:    r.right  += dx; r.top += dy;  break;
			case HTBOTTOMLEFT:  r.left   += dx; r.bottom += dy; break;
			case HTBOTTOMRIGHT: r.right  += dx; r.bottom += dy; break;
			}
			// Clamp to a minimum size, pinning the opposite edge.
			if (r.right - r.left < DXR_MIN_WINDOW_PX) {
				if (s_resize_edge == HTLEFT || s_resize_edge == HTTOPLEFT ||
				    s_resize_edge == HTBOTTOMLEFT)
					r.left = r.right - DXR_MIN_WINDOW_PX;
				else
					r.right = r.left + DXR_MIN_WINDOW_PX;
			}
			if (r.bottom - r.top < DXR_MIN_WINDOW_PX) {
				if (s_resize_edge == HTTOP || s_resize_edge == HTTOPLEFT ||
				    s_resize_edge == HTTOPRIGHT)
					r.top = r.bottom - DXR_MIN_WINDOW_PX;
				else
					r.bottom = r.top + DXR_MIN_WINDOW_PX;
			}
			SetWindowPos(hwnd, NULL, r.left, r.top,
			             r.right - r.left, r.bottom - r.top,
			             SWP_NOZORDER | SWP_NOACTIVATE);
			return 0;
		}
		if (s_drag_active) {
			POINT cur;
			GetCursorPos(&cur);
			int nx = s_drag_anchor_window.x + (cur.x - s_drag_anchor_screen.x);
			int ny = s_drag_anchor_window.y + (cur.y - s_drag_anchor_screen.y);
			SetWindowPos(hwnd, NULL, nx, ny, 0, 0,
			             SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
			return 0;
		}
		// Cube area: forward hover to Unity for cube hover-detection /
		// cursor-state polling.
		HWND unity_hover = find_unity_hwnd();
		if (unity_hover != NULL)
			PostMessageW(unity_hover, msg, wParam, lParam);
		return 0;
	}
	case WM_LBUTTONDOWN: case WM_LBUTTONUP:
	case WM_LBUTTONDBLCLK:
	case WM_MBUTTONDOWN: case WM_MBUTTONUP: {
		// End a custom frame-resize on button-up (the WM_NCLBUTTONDOWN that
		// started it arrives as WM_LBUTTONUP once the capture is held).
		if (msg == WM_LBUTTONUP && s_resize_active) {
			s_resize_active = 0;
			SendMessageW(hwnd, WM_EXITSIZEMOVE, 0, 0);
			ReleaseCapture();
			return 0;
		}
		// Same gating as above: reaching this case means we're in the
		// cube area (WS_EX_TRANSPARENT OFF). Transparent-zone clicks
		// are routed to the desktop natively and never arrive here.
		//
		// On press, claim OS foreground so keyboard goes to our
		// process (Unity via INPUTSINK).
		if (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK ||
		    msg == WM_MBUTTONDOWN) {
			SetForegroundWindow(hwnd);
		}
		HWND unity = find_unity_hwnd();
		if (unity != NULL)
			PostMessageW(unity, msg, wParam, lParam);
		return 0;
	}

	// ----- mouse wheel: accumulate delta for the C# layer to consume -----
	// WM_MOUSEWHEEL is delivered by the OS to the FOCUSED window (not by
	// cursor position), so this only fires when our overlay is foreground.
	// After click-through to e.g. Notepad, scroll naturally goes to Notepad
	// — exactly what we want.
	//
	// v1.2.0 / v1.2.1 resized the overlay HWND here (10% per notch). That
	// was experimental and removed in v1.2.2 — apps now read the delta via
	// displayxr_consume_overlay_wheel_delta() and decide what to do (e.g.
	// drive a DisplayXRDisplay rig's virtualDisplayHeight to zoom-in-window,
	// rotate the avatar, scroll a UI, etc.). We still consume the message
	// (return 0) so it doesn't bubble to underlying apps in cases where
	// our overlay isn't the foreground window's intended target.
	case WM_MOUSEWHEEL: {
		if (!s_overlay_is_toplevel)
			break; // opaque WS_CHILD mode: let Unity's input pipeline see it

		short delta = GET_WHEEL_DELTA_WPARAM(wParam);
		if (delta != 0)
			InterlockedExchangeAdd(&s_overlay_wheel_accum, delta);
		return 0;
	}

	// ----- overlay position/size changed: push to runtime -----
	// In transparent mode the overlay owns its on-screen position; Unity
	// is off-screen and parent_subclass_proc no longer pushes viewport
	// info on Unity's WM_MOVE/WM_SIZE. Mirror those updates here based on
	// the overlay's actual screen rect so the runtime's interlacing stays
	// pixel-aligned with where the overlay is presented on the lenticular.
	case WM_MOVE: {
		if (s_overlay_is_toplevel) {
			RECT wr;
			GetWindowRect(hwnd, &wr);
			DisplayXRState *state = displayxr_get_state();
			if (state != NULL && state->viewport_width > 0) {
				displayxr_set_viewport_size_native(
					state->viewport_width, state->viewport_height,
					(int32_t)wr.left, (int32_t)wr.top);
			}
		}
		break;
	}
	case WM_SIZE: {
		if (s_overlay_is_toplevel) {
			int w = LOWORD(lParam);
			int h = HIWORD(lParam);
			if (w > 0 && h > 0) {
				RECT wr;
				GetWindowRect(hwnd, &wr);
				displayxr_set_viewport_size_native(
					(uint32_t)w, (uint32_t)h,
					(int32_t)wr.left, (int32_t)wr.top);
			}
		}
		break;
	}

	// ----- close: quit the whole app, not just the overlay -----
	// The decorated overlay's close (X) button sends SC_CLOSE → WM_CLOSE, and
	// Alt+F4 sends WM_CLOSE directly. DefWindowProc's default WM_CLOSE would
	// DestroyWindow only the overlay HWND, leaving cloaked Unity running headless
	// (no visible window, process alive). Flag a quit request for C# to poll
	// (displayxr_consume_overlay_close_request) → Application.Quit() instead, and
	// swallow the message so the overlay survives the few frames until C# acts.
	case WM_CLOSE:
		displayxr_log("[DisplayXR][OvlWnd] WM_CLOSE -> quit request\n");
		InterlockedExchange(&s_overlay_close_requested, 1);
		return 0;

	// (#256) Teardown request marshalled from another thread. We are on the
	// creating (main) thread here, so displayxr_destroy_app_overlay takes its
	// direct arm and DestroyWindow is valid.
	case DXR_WM_DESTROY_OVERLAY:
		displayxr_destroy_app_overlay();
		return 0;

	default:
		break;
	}
	return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static LRESULT CALLBACK
parent_subclass_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	if (msg == WM_NCHITTEST && s_overlay_active) {
		POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
		ScreenToClient(hwnd, &pt);
		if (pt.x >= s_hit_rect.left && pt.x < s_hit_rect.right &&
		    pt.y >= s_hit_rect.top  && pt.y < s_hit_rect.bottom) {
			return HTCLIENT;
		}
		return HTTRANSPARENT;
	}
	// (#166/#61) Provider-overlay drag phase-snap. In provider mode the runtime
	// weave target is the top-level overlay (the SR SDK weaver subclasses IT),
	// but the overlay is click-through (WS_EX_TRANSPARENT) so the user drags
	// Unity's OWN title bar — Unity's HWND (this subclass) gets the OS modal-move
	// WM_ENTERSIZEMOVE/EXITSIZEMOVE bracket, the overlay never does. Without the
	// bracket the weaver's WndProc subclass doesn't see the in-drag flag and
	// won't phase-snap to lenticular-aligned positions → 3D stutters mid-drag.
	// Forward the bracket to the overlay so the weaver phase-snaps and Kooima
	// stays live throughout (#61). Mirrors the SendMessageW bracketing in
	// overlay_wnd_proc / unity_simple_wnd_proc / sa_wndproc, which each send the
	// bracket to their own weave-bound HWND around a custom drag.
	if (s_provider_opaque_overlay &&
	    (msg == WM_ENTERSIZEMOVE || msg == WM_EXITSIZEMOVE) &&
	    s_overlay_hwnd != NULL && IsWindow(s_overlay_hwnd)) {
		SendMessageW(s_overlay_hwnd, msg, 0, 0);
	}
	if (msg == WM_SIZE && s_overlay_hwnd != NULL && IsWindow(s_overlay_hwnd)) {
		int w = LOWORD(lParam);
		int h = HIWORD(lParam);
		if (s_provider_opaque_overlay) {
			// (#166) Top-level provider overlay follows Unity's on-screen client
			// area (screen coords, unlike the WS_CHILD 0,0). Reposition + resize +
			// push the new viewport rect to the runtime.
			POINT client_origin = {0, 0};
			ClientToScreen(hwnd, &client_origin);
			SetWindowPos(s_overlay_hwnd, NULL, client_origin.x, client_origin.y,
			             w, h, SWP_NOZORDER | SWP_NOACTIVATE);
			if (w > 0 && h > 0)
				displayxr_set_viewport_size_native(
					(uint32_t)w, (uint32_t)h,
					(int32_t)client_origin.x, (int32_t)client_origin.y);
		} else if (s_overlay_is_toplevel) {
			// Transparent mode: Unity lives off-screen at (-32000,-32000)
			// for cross-process click-through (Approach A, #57 session 4).
			// Don't follow Unity's position — the overlay owns its own
			// screen rect and pushes viewport updates from overlay_wnd_proc's
			// WM_MOVE/WM_SIZE handlers. Skip the viewport push here to avoid
			// pushing Unity's off-screen client_origin to the runtime.
		} else {
			// Child overlay: resize within parent's client area.
			SetWindowPos(s_overlay_hwnd, HWND_TOP, 0, 0, w, h, SWP_NOZORDER);
			if (w > 0 && h > 0) {
				POINT client_origin = {0, 0};
				ClientToScreen(hwnd, &client_origin);
				displayxr_set_viewport_size_native(
					(uint32_t)w, (uint32_t)h,
					(int32_t)client_origin.x, (int32_t)client_origin.y);
			}
		}
	}
	if (msg == WM_MOVE) {
		// In transparent mode, Unity's position is meaningless (off-screen);
		// the overlay tracks its own position via overlay_wnd_proc WM_MOVE.
		// In opaque WS_CHILD mode the overlay follows Unity automatically (it's a
		// child); we only push the viewport client_origin. In provider top-level
		// mode (#166) the overlay is a separate window, so we must also move it to
		// Unity's new client origin.
		if (s_provider_opaque_overlay) {
			POINT client_origin = {0, 0};
			ClientToScreen(hwnd, &client_origin);
			if (s_overlay_hwnd != NULL && IsWindow(s_overlay_hwnd)) {
				RECT cr;
				GetClientRect(hwnd, &cr);
				SetWindowPos(s_overlay_hwnd, NULL, client_origin.x, client_origin.y,
				             cr.right - cr.left, cr.bottom - cr.top,
				             SWP_NOZORDER | SWP_NOACTIVATE);
			}
			DisplayXRState *state = displayxr_get_state();
			if (state->viewport_width > 0)
				displayxr_set_viewport_size_native(
					state->viewport_width, state->viewport_height,
					(int32_t)client_origin.x, (int32_t)client_origin.y);
		} else if (!s_overlay_is_toplevel) {
			POINT client_origin = {0, 0};
			ClientToScreen(hwnd, &client_origin);
			DisplayXRState *state = displayxr_get_state();
			if (state->viewport_width > 0) {
				displayxr_set_viewport_size_native(
					state->viewport_width, state->viewport_height,
					(int32_t)client_origin.x, (int32_t)client_origin.y);
			}
		}
	}
	return CallWindowProcW(s_original_wndproc, hwnd, msg, wParam, lParam);
}

// Subclass installed on Unity's REAL main HWND in simple-window mode. Handles
// the borderless right-drag window move (#61-bracketed so the SR weaver phase-
// snaps) and claims WM_NCHITTEST while borderless (the SetWindowRgn silhouette
// already governs OS routing). Everything else falls through to Unity's
// original wndproc so Unity's input system behaves normally — Unity is the
// real on-screen foreground window here, unlike the cloaked-overlay path.
static LRESULT CALLBACK
unity_simple_wnd_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg) {
	case WM_NCHITTEST:
		// Borderless: any hit the OS delivers is inside the shaped region, so
		// claim it as client. Decorated: defer to Unity/DefWindowProc for the
		// standard title-bar drag + sizing border (matches the avatar).
		if (!s_window_decorated)
			return HTCLIENT;
		break;

	// ----- right-button: capture-based borderless move of Unity's HWND -----
	// Mirrors overlay_wnd_proc's right-drag. Disabled when decorated (the OS
	// title bar handles move/resize). Left button stays free for the scene.
	case WM_RBUTTONDOWN:
		if (!s_window_decorated) {
			GetCursorPos(&s_simple_drag_anchor_screen);
			RECT wr;
			GetWindowRect(hwnd, &wr);
			s_simple_drag_anchor_window.x = wr.left;
			s_simple_drag_anchor_window.y = wr.top;
			s_simple_drag_active = 1;
			SetCapture(hwnd);
			// #61: synchronous bracketing so the SR SDK weaver's WndProc
			// subclass sees the in-drag flag and phase-snaps. Must precede
			// the first SetWindowPos in WM_MOUSEMOVE.
			SendMessageW(hwnd, WM_ENTERSIZEMOVE, 0, 0);
			return 0;
		}
		break;
	case WM_RBUTTONUP:
		if (s_simple_drag_active) {
			// Clear the flag before WM_EXITSIZEMOVE so the recursive
			// WM_CAPTURECHANGED from ReleaseCapture() won't re-send it.
			s_simple_drag_active = 0;
			SendMessageW(hwnd, WM_EXITSIZEMOVE, 0, 0);
			ReleaseCapture();
			return 0;
		}
		break;
	case WM_CAPTURECHANGED: {
		int was_active = s_simple_drag_active;
		s_simple_drag_active = 0;
		if (was_active)
			SendMessageW(hwnd, WM_EXITSIZEMOVE, 0, 0);
		break;
	}
	case WM_MOUSEMOVE:
		if (s_simple_drag_active) {
			POINT cur;
			GetCursorPos(&cur);
			int nx = s_simple_drag_anchor_window.x + (cur.x - s_simple_drag_anchor_screen.x);
			int ny = s_simple_drag_anchor_window.y + (cur.y - s_simple_drag_anchor_screen.y);
			SetWindowPos(hwnd, NULL, nx, ny, 0, 0,
			             SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
			return 0;
		}
		break;

	default:
		break;
	}
	return CallWindowProcW(s_simple_orig_wndproc, hwnd, msg, wParam, lParam);
}

static int
register_overlay_class(void)
{
	if (s_class_registered)
		return 1;

	WNDCLASSEXW wc = {0};
	wc.cbSize = sizeof(wc);
	wc.lpfnWndProc = overlay_wnd_proc;
	wc.hInstance = GetModuleHandleW(NULL);
	wc.lpszClassName = OVERLAY_CLASS_NAME;
	wc.style = CS_OWNDC;

	if (RegisterClassExW(&wc) == 0) {
		fprintf(stderr, "[DisplayXR] Failed to register overlay window class: %lu\n",
		        GetLastError());
		return 0;
	}

	s_class_registered = 1;
	return 1;
}

void *
displayxr_get_app_main_view(void)
{
	if (s_overlay_hwnd != NULL && IsWindow(s_overlay_hwnd))
		return (void *)s_overlay_hwnd;

	HWND unity_hwnd = find_unity_hwnd();
	if (unity_hwnd == NULL) {
		fprintf(stderr, "[DisplayXR] No Unity main window found\n");
		return NULL;
	}

	if (!register_overlay_class())
		return NULL;

	RECT client_rc;
	GetClientRect(unity_hwnd, &client_rc);
	int w = client_rc.right - client_rc.left;
	int h = client_rc.bottom - client_rc.top;

	// Transparent overlay mode (issue #57 / runtime-pvt #191): create the
	// overlay as a top-level WS_POPUP with WS_EX_NOREDIRECTIONBITMAP.
	// Without NOREDIRECTIONBITMAP the HWND has an opaque DWM redirection
	// surface (default-cleared to black), and the runtime's DComp visuals
	// just paint on top of that — desktop never shows through transparent
	// pixels. NOREDIRECTIONBITMAP tells DWM "compositing comes purely from
	// DComp visuals attached to this HWND."
	//
	// Top-level (not WS_CHILD of Unity's HWND) avoids Unity's parent-window
	// swapchain interfering with our compositing. Unity owns the parent for
	// input/lifecycle; the overlay is a separate top-level window owned by
	// (not parented to) Unity.
	DisplayXRState *state = displayxr_get_state();
	int transparent_mode = (state != NULL && state->transparent_background_requested);

	// (#166) Provider in-app weave wants a TOP-LEVEL popup (composites the runtime
	// DComp weave) but opaque — no transparent-app extras. transparent_mode keeps
	// precedence if both are somehow set.
	int provider_opaque = s_provider_opaque_overlay && !transparent_mode;
	int toplevel = transparent_mode || provider_opaque;

	POINT client_origin = {0, 0};
	ClientToScreen(unity_hwnd, &client_origin);

	DWORD style    = toplevel
	    ? (DWORD)(WS_POPUP | WS_VISIBLE)
	    : (DWORD)(WS_CHILD | WS_VISIBLE);
	// Transparent path: WS_EX_TRANSPARENT is NOT in the creation exstyle —
	// it's toggled per-frame by displayxr_set_overlay_hit_active() driven
	// by the C# raycast at the current cursor position. When the cursor
	// is over a clickable renderer (cube silhouette), WS_EX_TRANSPARENT
	// is cleared so the overlay catches clicks for Unity. When the cursor
	// is in a transparent zone, WS_EX_TRANSPARENT is set and the OS
	// routes hit-tests natively past us to whatever desktop window is
	// underneath — full cross-process click/hover/cursor fidelity without
	// any in-plugin forwarder gymnastics.
	// WS_EX_NOACTIVATE keeps activation/foreground on Unity's (cloaked) HWND
	// when the user clicks the cube — otherwise Application.isFocused
	// flips false and Unity stops processing input.
	// WS_EX_TOPMOST keeps the avatar visually above the underlying app.
	// Provider-opaque: NOREDIRECTIONBITMAP so DWM composites purely from the
	// runtime's DComp visuals (same as transparent); NOACTIVATE so the overlay
	// never takes foreground (focus/keyboard stay on Unity); and TRANSPARENT so the
	// overlay is invisible to OS hit-testing — mouse clicks pass straight through to
	// Unity's window directly beneath (the overlay is a pure display surface over
	// Unity's client area). Without TRANSPARENT the overlay swallows mouse input and
	// the #57 RMB-drag wndproc branches (enabled by s_overlay_is_toplevel) fire
	// (#166). No TOPMOST/TOOLWINDOW — the popup is owned by Unity (below), so it
	// tracks Unity's z-order/visibility without being a global top-most tool window.
	DWORD ex_style = transparent_mode
	    ? (DWORD)(WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)
	    : provider_opaque
	        ? (DWORD)(WS_EX_NOREDIRECTIONBITMAP | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT)
	        : (DWORD)(WS_EX_TRANSPARENT);
	int x = toplevel ? client_origin.x : 0;
	int y = toplevel ? client_origin.y : 0;

	// (#131) Fullscreen-overlay preference: when the app opted in BEFORE the
	// overlay was created (displayxr_set_fullscreen_overlay_pref, from a
	// fullscreen 2D-surround app's earliest init), BIRTH the overlay covering
	// nearly the whole monitor — at the monitor origin, sized to the monitor
	// MINUS 1px on the right and bottom. The "minus 1px" keeps the window from
	// being the exact monitor rect, which would trip Windows fullscreen-
	// optimization / independent-flip (DWM-alpha bypass = the white flash); 1px
	// short stays DWM-composited. The overlay still covers the taskbar (tiger
	// feet render over it) save the last row/column. Born at this size so it is
	// never resized later (a resize recreates the swapchain = a flash). If the
	// pref lands too late, the overlay is born at Unity's window size and
	// set_overlay_fullscreen sizes it (one flash) as a graceful fallback.
	if (transparent_mode && s_fullscreen_overlay_pref) {
		HMONITOR mon = MonitorFromWindow(unity_hwnd, MONITOR_DEFAULTTONEAREST);
		MONITORINFO mi;
		mi.cbSize = sizeof(mi);
		if (GetMonitorInfo(mon, &mi)) {
			x = mi.rcMonitor.left;
			y = mi.rcMonitor.top;
			w = (mi.rcMonitor.right - mi.rcMonitor.left) - 1;
			h = (mi.rcMonitor.bottom - mi.rcMonitor.top) - 1;
			displayxr_log("[DisplayXR] overlay born near-fullscreen: "
			              "(%d,%d) %dx%d (monitor %dx%d, minus 1px to stay "
			              "DWM-composited)\n", x, y, w, h,
			              (int)(mi.rcMonitor.right - mi.rcMonitor.left),
			              (int)(mi.rcMonitor.bottom - mi.rcMonitor.top));
		}
	}

	// Owner field:
	// - WS_CHILD path: parent is unity_hwnd. The overlay is automatically
	//   clipped, z-ordered, and destroyed with Unity. Standard.
	// - WS_POPUP transparent path: NO owner (NULL). An owned popup follows
	//   its owner's visibility — when we cloak Unity to hide its grey
	//   backbuffer, an owned overlay would be cloaked too. Unowned makes
	//   the overlay genuinely independent of Unity's compositing state.
	HWND owner = transparent_mode ? NULL : unity_hwnd;

	s_overlay_hwnd = CreateWindowExW(
	    ex_style,
	    OVERLAY_CLASS_NAME,
	    L"DisplayXR Overlay",
	    style,
	    x, y, w, h,
	    owner,
	    NULL,
	    GetModuleHandleW(NULL),
	    NULL);

	if (s_overlay_hwnd == NULL) {
		fprintf(stderr, "[DisplayXR] Failed to create overlay window: %lu\n",
		        GetLastError());
		return NULL;
	}

	s_overlay_is_toplevel = toplevel;

	s_original_wndproc = (WNDPROC)SetWindowLongPtrW(
	    unity_hwnd, GWLP_WNDPROC, (LONG_PTR)parent_subclass_proc);
	s_parent_subclass_hwnd = unity_hwnd;

	// Cloak Unity's main window NOW rather than waiting for ApplyWindowing.
	//
	// The cloak that hides Unity's empty backbuffer already exists, but its only
	// caller is DisplayXRTransparentOverlay.ApplyWindowing -- a Unity COROUTINE,
	// so it cannot run while the main thread is busy loading the scene. Measured
	// on the 3DLuma avatar: Unity shows its main window at ~1.7 s, the main thread
	// then does not yield until ~10 s, and the cloak lands there. For those ~8 s a
	// full-screen empty (white) window sits on the panel -- which Windows also
	// re-titles "(Not responding)", the pump being blocked -- and it
	// is the first thing a user sees on every launch.
	//
	// This site runs at session init, BEFORE Unity's own ShowWindow, and
	// DWMWA_CLOAK is a pure DWM-side attribute: it needs no message pump, it
	// survives the later ShowWindow, and it leaves input, focus and rendering
	// untouched. ApplyWindowing still does the style strip and the off-screen park
	// once the main thread frees up; re-cloaking there is idempotent.
	if (transparent_mode) {
		BOOL cloak = TRUE;
		HRESULT hr = DwmSetWindowAttribute(unity_hwnd, DWMWA_CLOAK,
		                                   &cloak, sizeof(cloak));
		if (SUCCEEDED(hr))
			s_unity_early_cloaked = 1;
		displayxr_log("[DisplayXR] Early-cloaked Unity main window at overlay "
		              "birth (before ApplyWindowing): hr=0x%08X\n", (unsigned)hr);
	}

	// Hold the curtain: the overlay exists but stays invisible until the app is
	// pacing steadily. Suki's requirement was explicit -- a slower startup is fine,
	// the desktop going jerky is not.
	curtain_lower();

	displayxr_log("[DisplayXR] Created overlay HWND (%dx%d at %d,%d) on Unity window %p — %s\n",
	              w, h, x, y, (void *)unity_hwnd,
	              transparent_mode ? "TOP-LEVEL WS_POPUP + NOREDIRECTIONBITMAP (transparent)"
	              : provider_opaque ? "TOP-LEVEL WS_POPUP + NOREDIRECTIONBITMAP (provider opaque)"
	              : "WS_CHILD (opaque)");

	displayxr_set_viewport_size_native(
		(uint32_t)w, (uint32_t)h,
		(int32_t)client_origin.x, (int32_t)client_origin.y);

	return (void *)s_overlay_hwnd;
}

// ============================================================================
// (#173) Dedicated provider window — editor Play Mode weave target.
//
// The app-owned overlay (displayxr_get_app_main_view) binds a real HWND but
// TRACKS Unity's window: in the editor "Unity's window" is the whole editor, so
// the overlay covers it. Self-host coexists but binds windowHandle=NULL (no real
// geometry → window-relative Kooima dead, and the runtime's own activatable
// window steals foreground → keyboard dead + an SR-weaver DPI crash on focus
// switch). This third mode gives BOTH: a standalone, movable/resizable weave
// window that does NOT track Unity, bound as win_binding.windowHandle.
//
// Recipe mirrors the proven standalone preview window (displayxr_standalone.cpp
// sa_wndproc): WS_OVERLAPPEDWINDOW + WS_EX_NOACTIVATE (never steals OS foreground,
// so the editor keeps focus and Unity's Raw Input keeps flowing — Mouse.current
// .delta for camera drag etc.) + WS_EX_TOPMOST (visible on the Leia panel) +
// Per-Monitor-Aware-V2 DPI (the SR weaver crashed on a DPI/activation message when
// self-host handed focus to the per-monitor-aware editor). CRITICAL: the wndproc
// INTERCEPTS the OS SC_MOVE/SC_SIZE modal loops with a custom capture-based drag —
// letting DefWindowProc run its modal move/resize loop BLOCKS Unity's main thread
// (the message pump never returns to the PlayerLoop), so FrameTick stops and the
// weave/Kooima freezes for the whole drag = the "no window-relative Kooima on drag"
// symptom. The custom drag keeps the main thread pumping (real-time Kooima) and
// #61-brackets the SR weaver so it phase-snaps to lenticular-aligned pixels.
// ============================================================================

static HWND s_dedicated_hwnd = NULL; // DEDICATED_CLASS_NAME declared at top (find_unity_hwnd skip)
static int s_dedicated_class_registered = 0;
// Custom SC_MOVE/SC_SIZE drag state (mirrors s_sa.dragging / sizing_edge).
static int   s_ded_dragging     = 0;
static int   s_ded_sizing_edge  = 0;   // WMSZ_* (1..8) while resizing, else 0
static int   s_ded_clickthrough = 0;   // glued probe window: pass ALL mouse to the editor below
static int   s_childglue        = 0;   // dedicated window born WS_CHILD of the container (#740)

// (#740) 1 when the dedicated weave window is a WS_CHILD of Unity's container
// (child-glue). The provider's per-frame glue reads this to convert the incoming
// SCREEN rect to container-client child coords before SetWindowPos.
int displayxr_dedicated_is_childglue(void) { return s_childglue; }

// (#740 auto-switch) Programmatic child-glue selection. The C# dock-state detector
// drives this before every session (re)start: docked → (1, matched PANE hwnd),
// undocked → (0, NULL). -1 leaves the DISPLAYXR_PROV_GV_CHILDGLUE env in charge
// (test launchers; C# skips this setter when that env is set). The PANE handle is
// stashed and its GA_ROOT container is resolved AT WINDOW-CREATION time — the
// attempt-#1 lesson: a container captured in C# at loader.Start() can be destroyed
// as Play settles (WS_CHILD dies with its parent → mono). Like the present-mode
// override this survives session stop — C# must re-set it per (re)start.
static int  s_childglue_override = -1;
static HWND s_childglue_pane     = NULL;

void displayxr_set_child_glue(int enable, void *pane_hwnd)
{
	s_childglue_override = enable < 0 ? -1 : (enable ? 1 : 0);
	s_childglue_pane     = (HWND)pane_hwnd;
}

// (#740 auto-switch) Alive-check for the C# recovery watcher: a child-glue weave
// window dies WITH its parent container (Unity can rebuild containers as Play
// settles or on layout churn) — the session then weaves against a dead HWND
// (atlas canvas (-1,-1) → mono). The watcher polls this and restarts the
// subsystem, which recreates the window under the surviving container.
int displayxr_dedicated_window_alive(void)
{
	return s_dedicated_hwnd != NULL && IsWindow(s_dedicated_hwnd) ? 1 : 0;
}

// (#740) Pane-follow during OS modal drags. When the user drags a UNITY window (editor
// container or the undocked floating Game view), Windows enters a modal move loop that
// blocks Unity's PlayerLoop — so the C# glue (LateUpdate → dxr_prov_set_gameview_rect)
// freezes and the weave window stops following until mouse-up (the woven output lags the
// window). A WM_TIMER on our window keeps firing during that modal loop (same thread), so
// the timer re-derives the pane's LIVE screen rect (GetWindowRect on the pane HWND updates
// as its host window moves) and repositions our window to stay glued. C# publishes the
// pane HWND + the render-area offset within the pane window + size via displayxr_set_pane_follow.
static HWND s_pane_follow_hwnd = NULL;
static int  s_pane_follow_ox = 0, s_pane_follow_oy = 0; // render origin − pane window origin
static int  s_pane_follow_w = 0, s_pane_follow_h = 0;
static int  s_ded_present = 0;                          // present mode: SR resolves OUR window
// Follow-burst / #61 phase-snap bracket state (present mode only). We track the PANE's
// last-followed screen position (not our window's) so the weaver's exit phase-snap — which
// nudges our window a few px to lenticular-aligned pixels — is NOT itself treated as a
// pane move and re-followed (that would fight the snap). Bracket rides the follow so the
// weaver phase-snaps on settle, exactly like main's dedicated-window drag.
static int       s_pane_follow_last_valid = 0;
static LONG      s_pane_follow_last_px = 0, s_pane_follow_last_py = 0;
static int       s_pane_follow_in_bracket = 0;
static ULONGLONG s_pane_follow_last_move_tick = 0;
#define DXR_PANE_FOLLOW_TIMER 0xD7A0
#define DXR_PANE_FOLLOW_SETTLE_MS 180

// (#740) LAG-FREE follow: subclass the pane's HOST window (GA_ROOT of the pane — the
// top-level Unity window the user actually drags). During its OS modal move loop the host
// gets WM_WINDOWPOSCHANGED/WM_MOVE synchronously per move, so we reposition our window in
// LOCKSTEP (no polling lag, unlike the WM_TIMER fallback), and forward the host's REAL
// WM_ENTERSIZEMOVE/EXITSIZEMOVE to our window so the SR weaver phase-snaps (present mode).
// Mirrors the overlay path's parent_subclass_proc. Re-targets on dock/undock (host change).
static HWND    s_follow_host_hwnd = NULL;
static WNDPROC s_follow_host_orig_proc = NULL;
// (#740) Custom host MOVE drag: DefWindowProc's modal move loop blocks Unity's PlayerLoop,
// so the per-frame pump (window-relative Kooima / POV) freezes until mouse-up. Instead of
// handing the OS the loop, run the #61 capture-based custom drag (dedicated_wnd_proc's
// SC_MOVE recipe) ON THE HOST: each WM_MOUSEMOVE does a plain SetWindowPos and returns, so
// the main thread keeps returning to Unity's message loop → PlayerLoop ticks → POV live
// while the 3D keeps lockstep-following. "pending" = caption pressed, not yet past the OS
// drag threshold (a plain title-bar click must not move the window or open a bracket).
static int   s_follow_drag_pending = 0;
static int   s_follow_dragging     = 0;
static POINT s_follow_drag_cursor  = {0, 0};
static RECT  s_follow_drag_rect    = {0, 0, 0, 0};

// Trade-off while a session is live: Win11 Snap Layouts / drag-to-edge / drag-to-top for
// the Unity window don't engage during the custom drag (they are modal-loop features;
// Win+Arrow snapping still works). DISPLAYXR_PROV_HOST_DRAG=0 restores the OS drag
// (and with it the frozen-POV-during-drag behavior).
static int follow_host_drag_enabled(void)
{
	static int s_checked = 0, s_enabled = 1;
	if (!s_checked) {
		const char *v = getenv("DISPLAYXR_PROV_HOST_DRAG");
		s_enabled = !(v != NULL && v[0] == '0' && v[1] == '\0');
		s_checked = 1;
	}
	return s_enabled;
}

// End the custom host drag. Clears the flags BEFORE the bracket close / ReleaseCapture so
// the recursive WM_CAPTURECHANGED sees the drag inactive (dedicated_wnd_proc's ordering).
static void follow_host_drag_end(HWND host, int release_capture)
{
	int was_dragging = s_follow_dragging;
	s_follow_drag_pending = 0;
	s_follow_dragging = 0;
	if (was_dragging) {
		SendMessageW(host, WM_EXITSIZEMOVE, 0, 0);
		displayxr_log("[DisplayXR][HostDrag] end\n");
	}
	if (release_capture && GetCapture() == host)
		ReleaseCapture();
}

// (#740 f-up) 1 while the custom host MOVE drag is in progress. The C# glue pauses
// its per-frame GameView pushes during the drag: Unity's maximized-view layout
// readings FLAP by the toolbar height every frame while the container moves
// (mainSize 1596↔1536 observed) → per-frame zone re-drive → swapchain realloc
// storm → visible shimmer/loss of 3D. During the drag the native lockstep follow
// owns the window position (the pre-drag-fix behavior, which was smooth); the C#
// glue resumes at mouse-up.
int displayxr_host_drag_active(void)
{
	return (s_follow_drag_pending || s_follow_dragging) ? 1 : 0;
}

static void follow_reposition_to_pane(void)
{
	if (!s_dedicated_hwnd || !s_pane_follow_hwnd || !IsWindow(s_pane_follow_hwnd)) return;
	RECT pr;
	if (!GetWindowRect(s_pane_follow_hwnd, &pr)) return;
	int tx = pr.left + s_pane_follow_ox, ty = pr.top + s_pane_follow_oy;
	// Child-glue (#740 auto-switch): SetWindowPos on a WS_CHILD wants coords relative
	// to the parent container's client, not screen. (When the container moves, the
	// child rides along and this becomes a no-op — only a pane move WITHIN the
	// container repositions it.)
	if (s_childglue) {
		HWND parent = GetAncestor(s_dedicated_hwnd, GA_PARENT);
		POINT o = {0, 0};
		if (parent != NULL && ClientToScreen(parent, &o)) { tx -= o.x; ty -= o.y; }
	}
	SetWindowPos(s_dedicated_hwnd, NULL, tx, ty,
	             s_pane_follow_w, s_pane_follow_h,
	             SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER); // no FRAMECHANGED (#727)
}

static LRESULT CALLBACK follow_host_subclass_proc(HWND host, UINT msg, WPARAM wParam, LPARAM lParam)
{
	WNDPROC orig = s_follow_host_orig_proc;
	switch (msg) {
	case WM_ENTERSIZEMOVE:
		// Real drag start on the host → hand the weaver the same bracket so it phase-snaps.
		if (s_ded_present && s_dedicated_hwnd) SendMessageW(s_dedicated_hwnd, WM_ENTERSIZEMOVE, 0, 0);
		break;
	case WM_EXITSIZEMOVE:
		if (s_ded_present && s_dedicated_hwnd) SendMessageW(s_dedicated_hwnd, WM_EXITSIZEMOVE, 0, 0);
		break;
	case WM_MOVE:
	case WM_WINDOWPOSCHANGED:
		// Host moved/resized (fires synchronously during its modal loop) → lockstep-follow.
		follow_reposition_to_pane();
		break;

	// ----- (#740) #61 custom MOVE drag of the HOST (bypass the Unity-blocking modal loop) -----
	case WM_SYSCOMMAND:
		// Only a mouse caption drag (SC_MOVE|HTCAPTION = 0xF012, button down): keyboard
		// move (Alt+Space,M → 0xF010) and a maximized-window drag (OS restore-and-drag)
		// keep the OS modal loop. SC_SIZE is untouched by design (resize stays modal).
		if (host == s_follow_host_hwnd && s_dedicated_hwnd
		    && (wParam & 0xFFF0) == SC_MOVE && (wParam & 0x000F) == HTCAPTION
		    && GetKeyState(VK_LBUTTON) < 0 && !IsZoomed(host)
		    && follow_host_drag_enabled()) {
			SetCapture(host);
			GetCursorPos(&s_follow_drag_cursor);
			GetWindowRect(host, &s_follow_drag_rect);
			s_follow_drag_pending = 1; // promoted to a drag past the OS threshold below
			return 0; // no DefWindowProc → no modal loop
		}
		break;

	case WM_MOUSEMOVE:
		if (s_follow_drag_pending || s_follow_dragging) {
			if (GetKeyState(VK_LBUTTON) >= 0) { // missed the up somehow — end cleanly
				follow_host_drag_end(host, 1);
				return 0;
			}
			POINT cur; GetCursorPos(&cur);
			int dx = cur.x - s_follow_drag_cursor.x;
			int dy = cur.y - s_follow_drag_cursor.y;
			if (!s_follow_dragging) {
				if (abs(dx) < GetSystemMetrics(SM_CXDRAG) && abs(dy) < GetSystemMetrics(SM_CYDRAG))
					return 0; // still a click, not a drag
				s_follow_dragging = 1;
				// Deliver the bracket a real OS drag would: to the HOST. Our own
				// ENTERSIZEMOVE case forwards it to the weave window in present mode
				// only (b01c850 guardrail), and the SR weaver's docked-container
				// subclass sees it for phase-snap, exactly like a real drag.
				SendMessageW(host, WM_ENTERSIZEMOVE, 0, 0);
				displayxr_log("[DisplayXR][HostDrag] begin\n");
			}
			SetWindowPos(host, NULL, s_follow_drag_rect.left + dx, s_follow_drag_rect.top + dy,
			             0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
			return 0; // swallow, as the OS modal loop does
		}
		break;

	case WM_LBUTTONUP:
		if (s_follow_drag_pending || s_follow_dragging) {
			follow_host_drag_end(host, 1);
			return 0;
		}
		break;

	case WM_CAPTURECHANGED:
		if (s_follow_drag_pending || s_follow_dragging)
			follow_host_drag_end(host, 0); // capture already gone — just close the bracket
		break;

	case WM_NCDESTROY:
		if (host == s_follow_host_hwnd) {
			follow_host_drag_end(host, 1);
			if (orig) SetWindowLongPtrW(host, GWLP_WNDPROC, (LONG_PTR)orig);
			s_follow_host_hwnd = NULL; s_follow_host_orig_proc = NULL;
		}
		break;
	}
	return orig ? CallWindowProcW(orig, host, msg, wParam, lParam)
	            : DefWindowProcW(host, msg, wParam, lParam);
}

static void follow_unsubclass_host(void)
{
	// A retarget/teardown can land mid-drag: end it first (flags cleared before the
	// recursive WM_CAPTURECHANGED) so no capture is left on a window whose proc we
	// are about to stop handling.
	if (s_follow_host_hwnd && IsWindow(s_follow_host_hwnd))
		follow_host_drag_end(s_follow_host_hwnd, 1);
	else
		{ s_follow_drag_pending = 0; s_follow_dragging = 0; }
	if (s_follow_host_hwnd && s_follow_host_orig_proc && IsWindow(s_follow_host_hwnd))
		SetWindowLongPtrW(s_follow_host_hwnd, GWLP_WNDPROC, (LONG_PTR)s_follow_host_orig_proc);
	s_follow_host_hwnd = NULL; s_follow_host_orig_proc = NULL;
}

void displayxr_set_pane_follow(void *pane_hwnd, int off_x, int off_y, int w, int h)
{
	// (#740) Defensive size clamp at the storage point (both follow paths — host
	// subclass and WM_TIMER — reposition from these): the render area can never
	// exceed the virtual screen, so no publish may ever stick the weave window at
	// an insane size (a transient container-sized rect during a layout reset once
	// glued it huge and it stuck when the pane died).
	int maxw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
	int maxh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
	if (maxw > 0 && w > maxw) w = maxw;
	if (maxh > 0 && h > maxh) h = maxh;

	int pane_changed = (s_pane_follow_hwnd != (HWND)pane_hwnd);
	int geom_changed = (s_pane_follow_ox != off_x || s_pane_follow_oy != off_y ||
	                    s_pane_follow_w != w || s_pane_follow_h != h);
	s_pane_follow_hwnd = (HWND)pane_hwnd;
	s_pane_follow_ox = off_x; s_pane_follow_oy = off_y;
	s_pane_follow_w = w; s_pane_follow_h = h;

	// Keep the host subclass targeted at the pane's current top-level host (re-targets on
	// dock/undock). The subclass gives lockstep follow + real phase-snap brackets; the
	// WM_TIMER on our window is only the fallback for when no host is subclassed.
	HWND host = pane_hwnd ? GetAncestor((HWND)pane_hwnd, GA_ROOT) : NULL;
	if (host != s_follow_host_hwnd) {
		follow_unsubclass_host();
		if (host && IsWindow(host)) {
			s_follow_host_orig_proc =
			    (WNDPROC)SetWindowLongPtrW(host, GWLP_WNDPROC, (LONG_PTR)follow_host_subclass_proc);
			s_follow_host_hwnd = host;
		}
	}

	// (#740) Re-glue immediately when the pane target OR the published offsets change
	// (layout reset replaces the pane; the phase-nudge calibration knob shifts off_x):
	// with a static host no host message fires and the timer is gated off while a host
	// is subclassed, so nothing else would reposition until the next host move.
	if (pane_hwnd && (pane_changed || geom_changed)) {
		if (pane_changed)
			displayxr_log("[DisplayXR][PaneFollow] retarget pane=%p host=%p\n", pane_hwnd, (void *)host);
		follow_reposition_to_pane();
	}
}

// (#740) The dedicated window's container parent's client origin in screen px (child-glue
// coord base). Returns 1 + fills *ox,*oy when child-glue is active and the parent resolves.
int displayxr_dedicated_parent_client_origin(int *ox, int *oy)
{
	if (!s_childglue || s_dedicated_hwnd == NULL) return 0;
	HWND parent = GetAncestor(s_dedicated_hwnd, GA_PARENT);
	if (parent == NULL) return 0;
	POINT o = {0, 0};
	if (!ClientToScreen(parent, &o)) return 0;
	if (ox) *ox = o.x;
	if (oy) *oy = o.y;
	return 1;
}
static POINT s_ded_drag_cursor  = {0, 0};
static RECT  s_ded_drag_rect    = {0, 0, 0, 0};

static LRESULT CALLBACK
dedicated_wnd_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	// Glued probe window (Task (a)): the window is parked exactly over the editor Game
	// view as an invisible geometry proxy for the weaver/Kooima. Report EVERY point as
	// transparent so ALL mouse (buttons AND motion) falls through to Unity's Game view
	// underneath — otherwise this window swallows the button (Mouse.current.leftButton
	// stays false) and the in-game drag controller never activates (motion via raw-input
	// delta still works, which is why it looked like "moves then resets" while the window
	// jittered on/off the panel). More reliable than WS_EX_TRANSPARENT alone.
	if (s_ded_clickthrough && msg == WM_NCHITTEST)
		return HTTRANSPARENT;

	// Track mouse-button state in the shared s_vkey_state table so the app can read
	// it via displayxr_get_overlay_pointer. The woven output is a SEPARATE window
	// from Unity's Game View, so Unity's Input System doesn't see clicks here
	// (Mouse.current.leftButton stays false) — same reason the SA preview window
	// tracks buttons natively. Motion still comes from Mouse.current.delta (Raw
	// Input reaches the foreground editor because the window is WS_EX_NOACTIVATE).
	switch (msg) {
	case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK:
		s_vkey_state[VK_LBUTTON] = (SHORT)(0x8000 | 0x0001);
		SetCapture(hwnd); // keep the matching UP even if the drag leaves the window
		break;
	case WM_LBUTTONUP:
		s_vkey_state[VK_LBUTTON] = 0x0001;
		// Release capture only if we're not in a custom window move/resize (those
		// own the capture + release it in their own branches below).
		if (!s_ded_dragging && !s_ded_sizing_edge && GetCapture() == hwnd)
			ReleaseCapture();
		break;
	case WM_RBUTTONDOWN: s_vkey_state[VK_RBUTTON] = (SHORT)(0x8000 | 0x0001); break;
	case WM_RBUTTONUP:   s_vkey_state[VK_RBUTTON] = 0x0001; break;
	case WM_MBUTTONDOWN: s_vkey_state[VK_MBUTTON] = (SHORT)(0x8000 | 0x0001); break;
	case WM_MBUTTONUP:   s_vkey_state[VK_MBUTTON] = 0x0001; break;
	}

	switch (msg) {
	case WM_MOUSEACTIVATE:
		// Don't become foreground on click (belt-and-braces with WS_EX_NOACTIVATE):
		// keeps the editor foreground so Unity's Input System keeps getting Raw Input
		// (mouse delta/buttons for camera drag). Without this, clicking the weave
		// window to drag-rotate can steal focus and freeze Unity's mouse.
		return MA_NOACTIVATE;

	case WM_TIMER:
		// (#740) Pane-follow during OS modal drags: fires even while a Unity window's
		// modal move loop has frozen the PlayerLoop (same-thread timer). Re-derive the
		// pane's LIVE screen rect and keep our window glued. Plain move/resize (no
		// SWP_FRAMECHANGED) — weaver-safe (#727). Skips itself during our OWN custom drag.
		// In PRESENT mode the SR weaver resolves OUR window, so bracket the follow burst
		// with #61 WM_ENTERSIZEMOVE/EXITSIZEMOVE — the weaver phase-snaps on settle (main's
		// dedicated-window recipe). NOT bracketed in texture mode (SR resolves Unity's
		// container, which gets its own OS brackets; a synthetic bracket there re-anchors
		// the hidden proxy off the pane — the reverted b01c850 regression).
		if (wParam == DXR_PANE_FOLLOW_TIMER && s_pane_follow_hwnd && IsWindow(s_pane_follow_hwnd)
		    && !s_ded_dragging && !s_ded_sizing_edge && !s_follow_host_hwnd) {
			RECT pr;
			if (GetWindowRect(s_pane_follow_hwnd, &pr)) {
				// Track the PANE's position (not our window's — the exit snap moves ours).
				int pane_moved = !s_pane_follow_last_valid
				                 || pr.left != s_pane_follow_last_px
				                 || pr.top  != s_pane_follow_last_py;
				if (pane_moved) {
					s_pane_follow_last_valid = 1;
					s_pane_follow_last_px = pr.left; s_pane_follow_last_py = pr.top;
					if (s_ded_present && !s_pane_follow_in_bracket) {
						s_pane_follow_in_bracket = 1;
						SendMessageW(hwnd, WM_ENTERSIZEMOVE, 0, 0); // #61 phase-snap bracket
					}
					s_pane_follow_last_move_tick = GetTickCount64();
					follow_reposition_to_pane(); // child-glue-aware coords (#740)
				} else if (s_pane_follow_in_bracket &&
				           GetTickCount64() - s_pane_follow_last_move_tick >= DXR_PANE_FOLLOW_SETTLE_MS) {
					// Pane settled: close the bracket → the weaver snaps to lenticular pixels.
					s_pane_follow_in_bracket = 0;
					SendMessageW(hwnd, WM_EXITSIZEMOVE, 0, 0);
				}
			}
			return 0;
		}
		break;

	case WM_CLOSE:
		// Quit the whole app on close (X / Alt+F4), mirroring the overlay path:
		// swallow WM_CLOSE (don't DestroyWindow the live weave target) and raise the
		// close-request flag for C# to poll (Application.Quit → Play stop → the
		// subsystem's LifecycleStop destroys the window; see teardown below).
		displayxr_log("[DisplayXR][DedWnd] WM_CLOSE -> quit request\n");
		InterlockedExchange(&s_overlay_close_requested, 1);
		return 0;

	case WM_DESTROY:
		// The window is going away (our own DestroyWindow on teardown, or an OS
		// destroy). Restore the host subclass first, then clear the handle so a re-Play
		// recreates it and the app-facing overlay helpers stop targeting a dead HWND.
		follow_unsubclass_host(); // (#740)
		if (s_overlay_hwnd == hwnd) { s_overlay_hwnd = NULL; s_overlay_is_toplevel = 0; }
		s_dedicated_hwnd = NULL;
		return 0;

	// ----- #61 custom move/resize (bypass DefWindowProc's Unity-blocking modal loop) -----
	case WM_SYSCOMMAND:
		if ((wParam & 0xFFF0) == SC_MOVE) {
			SetCapture(hwnd);
			GetCursorPos(&s_ded_drag_cursor);
			GetWindowRect(hwnd, &s_ded_drag_rect);
			s_ded_dragging = 1;
			SendMessageW(hwnd, WM_ENTERSIZEMOVE, 0, 0); // weaver phase-snap bracket
			return 0;
		}
		if ((wParam & 0xFFF0) == SC_SIZE) {
			UINT edge = (UINT)(wParam & 0xF); // WMSZ_* (1..8)
			if (edge >= WMSZ_LEFT && edge <= WMSZ_BOTTOMRIGHT) {
				SetCapture(hwnd);
				GetCursorPos(&s_ded_drag_cursor);
				GetWindowRect(hwnd, &s_ded_drag_rect);
				s_ded_sizing_edge = (int)edge;
				SendMessageW(hwnd, WM_ENTERSIZEMOVE, 0, 0);
				return 0;
			}
		}
		break;

	case WM_SETCURSOR:
		// Force the sizing cursor during a custom resize (we hold capture, so the OS
		// would otherwise revert to the arrow).
		if (s_ded_sizing_edge) {
			LPCWSTR shape = (LPCWSTR)IDC_ARROW;
			switch (s_ded_sizing_edge) {
			case WMSZ_LEFT: case WMSZ_RIGHT:          shape = (LPCWSTR)IDC_SIZEWE;   break;
			case WMSZ_TOP: case WMSZ_BOTTOM:          shape = (LPCWSTR)IDC_SIZENS;   break;
			case WMSZ_TOPLEFT: case WMSZ_BOTTOMRIGHT: shape = (LPCWSTR)IDC_SIZENWSE; break;
			case WMSZ_TOPRIGHT: case WMSZ_BOTTOMLEFT: shape = (LPCWSTR)IDC_SIZENESW; break;
			}
			SetCursor(LoadCursorW(NULL, shape));
			return TRUE;
		}
		break;

	case WM_MOUSEMOVE:
		if (s_ded_dragging) {
			POINT cur; GetCursorPos(&cur);
			int dx = cur.x - s_ded_drag_cursor.x;
			int dy = cur.y - s_ded_drag_cursor.y;
			SetWindowPos(hwnd, NULL,
			             s_ded_drag_rect.left + dx, s_ded_drag_rect.top + dy,
			             0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
			return 0;
		}
		if (s_ded_sizing_edge) {
			POINT cur; GetCursorPos(&cur);
			int dx = cur.x - s_ded_drag_cursor.x;
			int dy = cur.y - s_ded_drag_cursor.y;
			RECT r = s_ded_drag_rect;
			switch (s_ded_sizing_edge) {
			case WMSZ_LEFT:        r.left   += dx; break;
			case WMSZ_RIGHT:       r.right  += dx; break;
			case WMSZ_TOP:         r.top    += dy; break;
			case WMSZ_TOPLEFT:     r.top    += dy; r.left  += dx; break;
			case WMSZ_TOPRIGHT:    r.top    += dy; r.right += dx; break;
			case WMSZ_BOTTOM:      r.bottom += dy; break;
			case WMSZ_BOTTOMLEFT:  r.bottom += dy; r.left  += dx; break;
			case WMSZ_BOTTOMRIGHT: r.bottom += dy; r.right += dx; break;
			}
			int minW = GetSystemMetrics(SM_CXMINTRACK);
			int minH = GetSystemMetrics(SM_CYMINTRACK);
			if (r.right - r.left < minW) {
				if (s_ded_sizing_edge == WMSZ_LEFT || s_ded_sizing_edge == WMSZ_TOPLEFT ||
				    s_ded_sizing_edge == WMSZ_BOTTOMLEFT) r.left = r.right - minW;
				else r.right = r.left + minW;
			}
			if (r.bottom - r.top < minH) {
				if (s_ded_sizing_edge == WMSZ_TOP || s_ded_sizing_edge == WMSZ_TOPLEFT ||
				    s_ded_sizing_edge == WMSZ_TOPRIGHT) r.top = r.bottom - minH;
				else r.bottom = r.top + minH;
			}
			SetWindowPos(hwnd, NULL, r.left, r.top,
			             r.right - r.left, r.bottom - r.top,
			             SWP_NOZORDER | SWP_NOACTIVATE);
			return 0;
		}
		break;

	case WM_LBUTTONUP:
		if (s_ded_dragging) {
			s_ded_dragging = 0;
			SendMessageW(hwnd, WM_EXITSIZEMOVE, 0, 0);
			ReleaseCapture();
			return 0;
		}
		if (s_ded_sizing_edge) {
			s_ded_sizing_edge = 0;
			SendMessageW(hwnd, WM_EXITSIZEMOVE, 0, 0);
			ReleaseCapture();
			return 0;
		}
		break;

	case WM_CAPTURECHANGED: {
		int was_active = s_ded_dragging || s_ded_sizing_edge;
		s_ded_dragging = 0;
		s_ded_sizing_edge = 0;
		if (was_active)
			SendMessageW(hwnd, WM_EXITSIZEMOVE, 0, 0);
		break;
	}

	default:
		break;
	}
	// WM_DPICHANGED (Per-Monitor-V2) and any non-intercepted message fall through to
	// DefWindowProc. Window-relative Kooima + #172 realloc read the live HWND geometry
	// directly (runtime tracks the bound HWND; provider polls GetClientRect); the
	// custom drag above keeps that geometry updating in real time during the drag.
	return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void *
displayxr_create_provider_dedicated_window(void)
{
	if (s_dedicated_hwnd != NULL && IsWindow(s_dedicated_hwnd))
		return (void *)s_dedicated_hwnd;

	if (!s_dedicated_class_registered) {
		WNDCLASSEXW wc = {0};
		wc.cbSize = sizeof(wc);
		wc.lpfnWndProc = dedicated_wnd_proc;
		wc.hInstance = GetModuleHandleW(NULL);
		wc.lpszClassName = DEDICATED_CLASS_NAME;
		wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
		wc.style = CS_OWNDC;
		if (RegisterClassExW(&wc) == 0) {
			fprintf(stderr, "[DisplayXR] Failed to register dedicated provider window class: %lu\n",
			        GetLastError());
			return NULL;
		}
		s_dedicated_class_registered = 1;
	}

	// Per-Monitor DPI Awareness V2 so the SR weaver's window subclass reads a
	// consistent physical-pixel geometry from GetClientRect() and DefWindowProc
	// handles WM_DPICHANGED — WITHOUT this the weaver crashed on a DPI/activation
	// message when self-host handed focus to the per-monitor-aware editor (#173).
	// Matches displayxr_standalone.cpp's preview-window setup.
	SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

	// Default windowed size + OS-picked position (CW_USEDEFAULT) so it coexists
	// with the editor; the user drags it onto the Leia panel and resizes to taste
	// (#172 live realloc tracks the new client size). Intentionally NOT sized to /
	// positioned over Unity's window — this is a standalone window that does NOT
	// track the editor.
	const int def_w = 1280, def_h = 720;

	// Task (a) GameView weave-to-texture fill: if C# stashed the Game view render rect
	// before the session started, born the window as a WS_POPUP (client == window) at
	// that rect so its client is EXACTLY the panel size when session_start captures the
	// forced full-window zone → the runtime renders tiles + weaves at the panel's native
	// resolution (no tiny-zone freeze / mirror over-sample). The per-frame glue keeps it
	// tracking afterwards. Shipping (non-probe) path keeps the movable default window.
	extern int dxr_prov_get_initial_gameview_rect(int *x, int *y, int *w, int *h);
	int igx = 0, igy = 0, igw = 0, igh = 0;
	int have_init = dxr_prov_get_initial_gameview_rect(&igx, &igy, &igw, &igh);

	// In the glued probe path the window is parked exactly over the editor Game view (it's
	// only a geometry proxy for the weaver/Kooima — texture mode never presents to it).
	// WS_EX_LAYERED (+ alpha 0 below) keeps it invisible; the WM_NCHITTEST → HTTRANSPARENT
	// handler (gated on s_ded_clickthrough) passes ALL mouse through to the Game view
	// underneath so the in-game mouse controller keeps working.
	//
	// CHILD-GLUE (#740): born as a WS_CHILD of Unity's container window instead of a
	// parentless top-level. GA_ROOT(child) == the container = the window the SR SDK
	// resolves as its phase anchor → the runtime's weave-phase fix computes the correct
	// pane-vs-container offset. Unlike BINDPANE (which binds Unity's OWN pane GUIView and
	// dies when Unity re-hosts it on dock/undock/maximize/domain-reload), this window is
	// OURS: the container is stable across tab re-hosts, so the child survives them; we
	// recreate it each session. Child coords are relative to the container's client, so it
	// follows the container automatically and only needs repositioning when the pane moves
	// WITHIN the container (plain move, weaver-safe #727). Gated: DISPLAYXR_PROV_GV_CHILDGLUE.
	// PRESENT mode (#740 hybrid, undocked): the runtime presents the woven stereo INTO
	// this window, so it must be a VISIBLE top-level window over the floating pane (not the
	// invisible child-glue proxy used for the docked texture path). SR self-anchors to it
	// (GA_ROOT==self). Forces top-level + visible, overriding child-glue.
	extern int dxr_prov_get_present_mode(void);
	int present_mode = have_init && dxr_prov_get_present_mode();
	s_ded_present = present_mode;                 // wndproc reads this for the #61 snap bracket
	s_pane_follow_last_valid = 0; s_pane_follow_in_bracket = 0; // reset per session

	HWND child_parent = NULL;
	int cx = igx, cy = igy;
	// Auto-switch (#740): the C# override (dock-state detector) wins over the env gate.
	int childglue_want = (s_childglue_override >= 0)
	    ? s_childglue_override
	    : (getenv("DISPLAYXR_PROV_GV_CHILDGLUE") != NULL ? 1 : 0);
	s_childglue = (!present_mode && have_init && childglue_want) ? 1 : 0;
	if (s_childglue) {
		// Resolve the container NOW (window-creation time), from the C#-matched PANE's
		// live GA_ROOT — deterministic, and as late as possible (attempt #1 pre-captured
		// the container in C# at loader.Start() and Unity destroyed it as Play settled).
		// find_unity_hwnd() remains the fallback for env-driven runs (it prefers the
		// foreground window, which can be wrong when a floating Unity window is focused).
		child_parent = (s_childglue_pane != NULL && IsWindow(s_childglue_pane))
		    ? GetAncestor(s_childglue_pane, GA_ROOT) : NULL;
		if (child_parent == NULL)
			child_parent = find_unity_hwnd();
		if (child_parent != NULL) {
			POINT o = {0, 0};
			ClientToScreen(child_parent, &o);
			cx = igx - o.x; cy = igy - o.y; // screen → container-client child coords
		} else {
			s_childglue = 0; // no container found → fall back to top-level
		}
	}

	// PRESENT mode z-order (#740 f-up): the woven window must float over the FLOATING
	// Game view but respect global z-order (focusing another app used to leave the
	// TOPMOST weave on top of everything). Make it an OWNED window of the pane's
	// top-level container: it z-rides above its owner, drops with it when another app
	// is focused, and minimizes with it. OWNERSHIP ≠ parentship — GA_ROOT stays self,
	// so the SR weaver's DXGI/phase self-anchor is untouched. The pane handle comes
	// from the dock-state detector (displayxr_set_child_glue stashes it in both
	// modes); env-driven runs without a pane keep the legacy TOPMOST behavior.
	HWND present_owner = NULL;
	if (present_mode && s_childglue_pane != NULL && IsWindow(s_childglue_pane))
		present_owner = GetAncestor(s_childglue_pane, GA_ROOT);

	// (#740 styles A/B) The docked child is normally WS_EX_LAYERED with alpha 0 +
	// WS_EX_TRANSPARENT. Per MSDN, "hit testing of a layered window is based on the shape
	// and transparency of the window ... areas whose alpha value is zero will let the
	// mouse messages through" — i.e. an alpha-0 layered window is INVISIBLE to
	// WindowFromPoint and to any point-based window resolution. If the SR SDK resolves
	// its weaving window that way, our child is skipped and the phase anchors to whatever
	// lies underneath (Unity's pane/container) — the leading #740 hypothesis, and the one
	// delta between our child and the runtime agent's plain harness child (which weaves
	// correctly at our exact geometry).
	// DISPLAYXR_PROV_CHILD_ALPHA=<1..255> borns the child at that alpha (1/255 is visually
	// imperceptible) and WITHOUT WS_EX_TRANSPARENT, so it IS hit-test-visible; input still
	// falls through via our WM_NCHITTEST → HTTRANSPARENT (s_ded_clickthrough). If docked
	// phase snaps correct under this variant, the mechanism is confirmed app-side.
	int child_alpha = 0;
	{
		const char *e = getenv("DISPLAYXR_PROV_CHILD_ALPHA");
		if (e && e[0]) {
			child_alpha = atoi(e);
			if (child_alpha < 0) child_alpha = 0;
			if (child_alpha > 255) child_alpha = 255;
		}
	}

	// Children can't be WS_EX_TOPMOST; use TRANSPARENT for click-through (no top-level
	// HTTRANSPARENT z-fight). Top-level path keeps NOACTIVATE|TOPMOST as before.
	// PRESENT mode: VISIBLE top-level (no WS_EX_LAYERED alpha-0 — the runtime presents the
	// woven stereo into it and the user must SEE it), NOACTIVATE (never steals editor
	// focus); TOPMOST only when no owner resolved. Keep clickthrough so the input path +
	// #61 drag brackets still work.
	DWORD ex_style = WS_EX_NOACTIVATE;
	if (!s_childglue && present_owner == NULL) ex_style |= WS_EX_TOPMOST;
	if (have_init && !present_mode) {
		ex_style |= WS_EX_LAYERED;
		if (s_childglue && child_alpha == 0) ex_style |= WS_EX_TRANSPARENT;
		s_ded_clickthrough = 1;
	} else if (present_mode) {
		s_ded_clickthrough = 1; // input via GetAsyncKeyState + drag brackets; window is visible
	} else s_ded_clickthrough = 0;

	DWORD style = s_childglue ? (WS_CHILD | WS_VISIBLE)
	                          : (have_init ? WS_POPUP : WS_OVERLAPPEDWINDOW);

	s_dedicated_hwnd = CreateWindowExW(
		ex_style,
		DEDICATED_CLASS_NAME,
		L"DisplayXR (Provider)",
		style,
		s_childglue ? cx : (have_init ? igx : CW_USEDEFAULT),
		s_childglue ? cy : (have_init ? igy : CW_USEDEFAULT),
		have_init ? igw : def_w,          have_init ? igh : def_h,
		s_childglue ? child_parent : present_owner, NULL, GetModuleHandleW(NULL), NULL);

	if (s_dedicated_hwnd == NULL) {
		fprintf(stderr, "[DisplayXR] Failed to create dedicated provider window: %lu\n",
		        GetLastError());
		return NULL;
	}
	if (s_childglue)
		displayxr_log("[DisplayXR] CHILD-GLUE (#740): dedicated window born WS_CHILD of "
		              "container %p at child (%d,%d) [screen (%d,%d)] %dx%d\n",
		              (void *)child_parent, cx, cy, igx, igy, igw, igh);

	// Glued texture-probe path: the window is WS_EX_LAYERED — make it fully transparent
	// (alpha 0) so the invisible geometry proxy never paints a black rect over the Game
	// view. PRESENT mode is NOT layered (visible — it presents the woven stereo).
	// (#740 styles A/B) DISPLAYXR_PROV_CHILD_ALPHA overrides the alpha — see above.
	if (have_init && !present_mode) {
		SetLayeredWindowAttributes(s_dedicated_hwnd, 0, (BYTE)child_alpha, LWA_ALPHA);
		if (child_alpha != 0)
			displayxr_log("[DisplayXR] #740 styles A/B: child born LAYERED alpha=%d, "
			              "WS_EX_TRANSPARENT OFF — hit-test VISIBLE\n", child_alpha);
	}
	if (present_mode)
		displayxr_log("[DisplayXR] PRESENT mode (#740): dedicated window born VISIBLE top-level "
		              "WS_POPUP at screen (%d,%d) %dx%d owner=%p (%s) — runtime presents woven "
		              "stereo into it\n",
		              igx, igy, igw, igh, (void *)present_owner,
		              present_owner ? "z-rides the floating Game view" : "TOPMOST legacy");

	// Show WITHOUT activating so the editor keeps OS foreground (input stays live).
	ShowWindow(s_dedicated_hwnd, SW_SHOWNOACTIVATE);

	// (#740) Pane-follow timer: keeps the window glued during OS modal drags of a Unity
	// window (which freeze the PlayerLoop + the C# glue). Only needed for the glued paths
	// (have_init); harmless if C# never publishes a pane (the WM_TIMER no-ops). Raise the
	// system timer resolution to ~1ms (default ~15.6ms starves a fast drag → the woven
	// window visibly lags the cursor) and run at ~8ms so the follow keeps up. winmm loaded
	// dynamically (not linked); handle intentionally leaked (editor-lifetime).
	if (have_init) {
		static int s_tbp_done = 0;
		if (!s_tbp_done) {
			s_tbp_done = 1;
			HMODULE winmm = LoadLibraryW(L"winmm.dll");
			if (winmm) {
				typedef UINT (WINAPI *pfn_tbp)(UINT);
				pfn_tbp tbp = (pfn_tbp)GetProcAddress(winmm, "timeBeginPeriod");
				if (tbp) tbp(1);
			}
		}
		SetTimer(s_dedicated_hwnd, DXR_PANE_FOLLOW_TIMER, 8, NULL);
	}

	// Publish as the managed top-level overlay HWND so the app-facing window
	// helpers (displayxr_resize_overlay / _get/_set_overlay_position /
	// _consume_overlay_close_request) target the dedicated window too — the app's
	// keyboard windowing (e.g. Ctrl+arrows, #172 testing) then works in editor Play
	// Mode. The overlay_wnd_proc branches gated on these only run for OVERLAY_CLASS
	// windows (the dedicated window uses its own class + wndproc), so this is inert
	// for message dispatch; it only redirects the app-facing helpers.
	s_overlay_hwnd = s_dedicated_hwnd;
	s_overlay_is_toplevel = 1;

	displayxr_log("[DisplayXR] Created dedicated provider window %p (%dx%d) — "
	              "WS_OVERLAPPEDWINDOW + NOACTIVATE/TOPMOST, Per-Monitor-V2 DPI (#173)\n",
	              (void *)s_dedicated_hwnd, def_w, def_h);
	return (void *)s_dedicated_hwnd;
}

// (#173) Destroy the dedicated provider window on subsystem teardown (editor Play
// stop). Called from the provider's LifecycleStop — which runs on the MAIN thread
// (where the window was created, so DestroyWindow is valid) and AFTER GfxStop's
// dxr_prov_session_stop (xrDestroySession unhooks the SR weaver's window subclass),
// so the destroy is clean. Without this the window lingered visible + TOPMOST
// showing a frozen last frame with a stale weaver subclass = the "stopping Play
// freezes the window" report. Idempotent; a re-Play recreates it in LifecycleStart.
void
displayxr_destroy_provider_dedicated_window(void)
{
	follow_unsubclass_host(); // (#740) restore the host window's original WndProc first
	s_pane_follow_hwnd = NULL; s_pane_follow_last_valid = 0; s_pane_follow_in_bracket = 0;
	HWND h = s_dedicated_hwnd;
	s_dedicated_hwnd = NULL;
	if (s_overlay_hwnd == h) { s_overlay_hwnd = NULL; s_overlay_is_toplevel = 0; }
	if (h != NULL && IsWindow(h)) {
		displayxr_log("[DisplayXR] Destroying dedicated provider window %p (#173 teardown)\n", (void *)h);
		DestroyWindow(h);
	}
}

// (#256) Destroy the app-owned overlay created by displayxr_get_app_main_view and
// undo everything that call installed. Two callers, one shape:
//
//   - GfxStart's session-start failure. The overlay is created in LifecycleStart,
//     BEFORE the session is attempted, so a refusal (no runtime, unsupported GPU,
//     cross-adapter guard) used to leak a TOPMOST window for the process lifetime —
//     and in transparent mode it is created WITHOUT WS_EX_TRANSPARENT, so the orphan
//     also swallowed every click over the app.
//   - LifecycleStop, so the overlay does not outlive the subsystem on Windows either
//     (previously only the editor's dedicated window was destroyed here).
//
// TEARDOWN ORDER matters and is the reverse of installation:
//   1. Un-cloak / un-park Unity's window (no-op unless the transparent path applied
//      it). Must precede the destroy: the restore rect is read off the overlay.
//   2. Remove the focus hook's subclass, which was chained ON TOP of
//      parent_subclass_proc — a WndProc chain must be unwound from the outside in.
//   3. Restore parent_subclass_proc's saved original, verifying we are still the
//      current proc so a foreign subclass installed since is never clobbered.
//   4. Clear the overlay statics, THEN DestroyWindow — no surviving handler may see
//      a dangling s_overlay_hwnd.
//
// Thread affinity: DestroyWindow is only valid on the creating (main) thread, so a
// call from anywhere else is marshalled via PostMessage and returns immediately.
// Idempotent. Never touches the #173 dedicated window (that has its own destroy).
void
displayxr_destroy_app_overlay(void)
{
	HWND h = s_overlay_hwnd;
	if (h == NULL || h == s_dedicated_hwnd || !IsWindow(h))
		return;

	if (GetWindowThreadProcessId(h, NULL) != GetCurrentThreadId()) {
		PostMessageW(h, DXR_WM_DESTROY_OVERLAY, 0, 0);
		return;
	}

	displayxr_log("[DisplayXR] Destroying app overlay %p (#256 teardown)\n", (void *)h);

	// 1. Restore Unity's own window if the transparent path hid it. Inert when it
	//    didn't (s_overlay_active / s_simple_active are 0) — including the ordinary
	//    case where the component's OnDisable already reverted it.
	displayxr_set_transparent_overlay(0, 0);
	displayxr_set_simple_window(0, 0);

	// 2 + 3. Unwind the WndProc chain on Unity's window, outermost first.
	displayxr_uninstall_focus_hook();
	if (s_original_wndproc != NULL && s_parent_subclass_hwnd != NULL
	    && IsWindow(s_parent_subclass_hwnd)) {
		WNDPROC cur = (WNDPROC)GetWindowLongPtrW(s_parent_subclass_hwnd, GWLP_WNDPROC);
		if (cur == parent_subclass_proc)
			SetWindowLongPtrW(s_parent_subclass_hwnd, GWLP_WNDPROC,
			                  (LONG_PTR)s_original_wndproc);
		else
			displayxr_log("[DisplayXR] app overlay teardown: Unity WndProc is not ours "
			              "(%p) — leaving the chain alone\n", (void *)cur);
	}
	s_original_wndproc = NULL;
	s_parent_subclass_hwnd = NULL;

	// 4. Drop the overlay.
	s_overlay_hwnd = NULL;
	s_overlay_is_toplevel = 0;
	DestroyWindow(h);
}

void *
displayxr_get_unity_main_hwnd(void)
{
	HWND hwnd = find_unity_hwnd();
	if (hwnd == NULL)
		fprintf(stderr, "[DisplayXR] No Unity main window found (shell mode)\n");
	return (void *)hwnd;
}

// ============================================================================
// Transparent overlay mode (issue #57)
//
// Per-pixel-alpha transparent overlay for desktop avatar use cases. End-to-end:
//
//   1) The OpenXR session is opted into XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND
//      (see DisplayXRFeature.OnInstanceCreate), so Unity emits real per-pixel
//      alpha into the swapchain — including alpha=0 for transparent camera
//      clears. The plugin sets chromaKeyColor=0 in the binding extension
//      (see displayxr_hooks.cpp), telling the runtime to skip the legacy
//      post-weave chroma-key conversion: it isn't needed.
//
//   2) The runtime DP uses the compose-under-bg + alpha-gate path: it captures
//      the desktop background under each tile pre-weave, blends with the
//      atlas RGBA, then alpha-gates post-weave so silhouettes carry true
//      anti-aliased alpha. Fully replaces the older chroma-color sentinel.
//
//   3) The overlay HWND is top-level WS_POPUP with WS_EX_NOREDIRECTIONBITMAP
//      (created in displayxr_get_app_main_view), so DWM has no opaque
//      redirection surface and composites the HWND purely from the DComp
//      visuals the runtime attached. Real per-pixel alpha shows the desktop
//      through alpha=0 regions natively.
//
// IMPORTANT: we do NOT use WS_EX_LAYERED / LWA_COLORKEY. The set_transparent_
// overlay function below explicitly STRIPS WS_EX_LAYERED off Unity's HWND if
// it was there. Earlier versions of this plugin DID paint a magenta/gray
// chroma color in the camera clear and rely on the runtime DP's post-weave
// chroma→alpha conversion — that workaround is gone now that the runtime
// advertises ALPHA_BLEND and Unity emits true alpha.
//
// Click-through is independent of all of the above: the overlay's
// WS_EX_TRANSPARENT bit is toggled per-frame by
// displayxr_set_overlay_hit_active() based on the C# raycast at the
// current cursor position. When set (transparent zone), the OS hit-test
// routes natively past us to the desktop app underneath. When clear
// (cube silhouette), the overlay catches clicks for Unity.
//
// Mutually exclusive with shell mode (early-out below).
// ============================================================================

// (startup white window) Unity's main HWND, found even while it is still HIDDEN.
//
// find_unity_hwnd() cannot serve the pre-cloak: it requires IsWindowVisible() and a
// client width > 100, which is exactly what Unity's window is NOT yet at the moment
// we need to cloak it. Measured: the window exists (hidden, 183x137) from ~0.3 s and
// Unity shows it at ~1.65 s, so the only useful window of opportunity is while it is
// still hidden. Match on the class name instead -- that is stable and unambiguous.
//
// EDITOR GUARD: the Unity editor's own main window carries the same class, and
// cloaking the editor would hide the user's IDE. Only ever run this in a player.
static HWND
find_unity_main_hwnd_any(void)
{
	wchar_t exe[MAX_PATH];
	if (GetModuleFileNameW(NULL, exe, MAX_PATH) > 0) {
		const wchar_t *base = wcsrchr(exe, L'\\');
		base = base ? base + 1 : exe;
		if (_wcsicmp(base, L"Unity.exe") == 0)
			return NULL; // editor, not a player
	}

	DWORD our_pid = GetCurrentProcessId();
	HWND hwnd = NULL;
	while ((hwnd = FindWindowExW(NULL, hwnd, NULL, NULL)) != NULL) {
		DWORD pid = 0;
		GetWindowThreadProcessId(hwnd, &pid);
		if (pid != our_pid || is_displayxr_overlay_class(hwnd))
			continue;
		wchar_t cls[64];
		if (GetClassNameW(hwnd, cls, 64) > 0
		    && _wcsicmp(cls, L"UnityWndClass") == 0)
			return hwnd;
	}
	return NULL;
}

// Cloak Unity's main window at the EARLIEST native touchpoint we have.
//
// Cloaking at overlay birth still left a visible flash: the overlay is born at
// ~1.87 s but Unity shows its window at ~1.65 s, so ~220 ms of empty white window
// reached the panel (confirmed by DWMWA_CLOAKED going True only after the show, and
// by eye). displayxr_set_transparent_background() is the app's first native call --
// ~110 ms before the show -- so cloaking there closes the gap.
//
// Idempotent, and safe to fail: if Unity's window does not exist yet the overlay-birth
// cloak still runs as the backstop.
void
displayxr_precloak_unity_main_window(void)
{
	if (s_unity_early_cloaked)
		return;
	if (displayxr_is_shell_mode())
		return;

	HWND hwnd = find_unity_main_hwnd_any();
	if (hwnd == NULL) {
		displayxr_log("[DisplayXR] pre-cloak: no Unity main window yet "
		              "(overlay birth will cloak instead)\n");
		return;
	}

	BOOL cloak = TRUE;
	HRESULT hr = DwmSetWindowAttribute(hwnd, DWMWA_CLOAK, &cloak, sizeof(cloak));
	if (SUCCEEDED(hr))
		s_unity_early_cloaked = 1;
	displayxr_log("[DisplayXR] Pre-cloaked Unity main window %p BEFORE its "
		      "first ShowWindow: hr=0x%08X\n", (void *)hwnd, (unsigned)hr);
}

static uint64_t
curtain_now_ms(void)
{
	return (uint64_t)GetTickCount64();
}

static int
curtain_enabled(void)
{
	const char *e = getenv("DXR_AVATAR_CURTAIN");
	return !(e != NULL && e[0] == '0');
}

static uint64_t
curtain_max_ms(void)
{
	const char *e = getenv("DXR_AVATAR_CURTAIN_MS");
	if (e != NULL && e[0] != '\0') {
		long v = strtol(e, NULL, 10);
		if (v > 0)
			return (uint64_t)v;
	}
	return (uint64_t)DXR_CURTAIN_MAX_MS_DEFAULT;
}

// Uncloak the overlay and stop tracking. Idempotent.
static void
curtain_raise(const char *why)
{
	if (!s_curtain_down)
		return;
	s_curtain_down = 0;
	if (s_overlay_hwnd != NULL && IsWindow(s_overlay_hwnd)) {
		BOOL cloak = FALSE;
		DwmSetWindowAttribute(s_overlay_hwnd, DWMWA_CLOAK, &cloak, sizeof(cloak));
	}
	displayxr_log("[DisplayXR] startup curtain UP after %llu ms (%s) "
		      "-- the overlay is now visible\n",
		      (unsigned long long)(curtain_now_ms() - s_curtain_start_ms), why);
}

// Called at overlay birth. Cloaks the overlay so nothing of the warm-up is seen.
static void
curtain_lower(void)
{
	if (!curtain_enabled()) {
		displayxr_log("[DisplayXR] startup curtain disabled "
		              "(DXR_AVATAR_CURTAIN=0)\n");
		return;
	}
	if (s_overlay_hwnd == NULL)
		return;
	BOOL cloak = TRUE;
	HRESULT hr = DwmSetWindowAttribute(s_overlay_hwnd, DWMWA_CLOAK, &cloak, sizeof(cloak));
	if (FAILED(hr)) {
		displayxr_log("[DisplayXR] startup curtain: cloak failed hr=0x%08X "
		              "-- showing the warm-up\n", (unsigned)hr);
		return;
	}
	s_curtain_down = 1;
	s_curtain_start_ms = curtain_now_ms();
	s_curtain_last_frame_ms = 0;
	s_curtain_prev_gap_ms = 0;
	s_curtain_steady_run = 0;
	displayxr_log("[DisplayXR] startup curtain DOWN -- overlay cloaked until the app "
		      "paces %d consecutive frames steadily (or %llu ms elapse)\n",
		      DXR_CURTAIN_STEADY_FRAMES, (unsigned long long)curtain_max_ms());
}

void
displayxr_curtain_note_frame(void)
{
	if (!s_curtain_down)
		return;

	const uint64_t now = curtain_now_ms();
	if (now - s_curtain_start_ms >= curtain_max_ms()) {
		curtain_raise("timeout");
		return;
	}

	if (s_curtain_last_frame_ms == 0) {
		s_curtain_last_frame_ms = now;
		return;
	}
	const uint64_t gap = now - s_curtain_last_frame_ms;
	s_curtain_last_frame_ms = now;
	if (s_curtain_prev_gap_ms == 0) {
		s_curtain_prev_gap_ms = gap;
		return;
	}

	// Ratio test, so 60 fps and the divisor's 20 fps both qualify. A frame that
	// took more than 1.5x its predecessor (or less than 2/3) restarts the run.
	const uint64_t lo = (gap < s_curtain_prev_gap_ms) ? gap : s_curtain_prev_gap_ms;
	const uint64_t hi = (gap < s_curtain_prev_gap_ms) ? s_curtain_prev_gap_ms : gap;
	s_curtain_prev_gap_ms = gap;
	if (lo > 0 && hi * 2 <= lo * 3) {
		s_curtain_steady_run++;
		if (s_curtain_steady_run >= DXR_CURTAIN_STEADY_FRAMES)
			curtain_raise("steady");
	} else {
		s_curtain_steady_run = 0;
	}
}

void
displayxr_set_transparent_overlay(int enabled, int topmost)
{
	displayxr_log("[DisplayXR] transparent_overlay: called enabled=%d topmost=%d shell=%d\n",
	              enabled, topmost, displayxr_is_shell_mode());

	if (displayxr_is_shell_mode())
		return;

	HWND hwnd = find_unity_hwnd();
	if (hwnd == NULL) {
		displayxr_log("[DisplayXR] transparent_overlay: no Unity HWND\n");
		return;
	}

	// The early cloak has no matching un-cloak below: every branch there is gated
	// on s_overlay_active, which is still 0 when the main thread never freed up to
	// run ApplyWindowing. Without this, a session-loss revert (or a teardown on
	// that path) would leave Unity's window cloaked and the app invisible.
	if (!enabled && s_unity_early_cloaked && !s_overlay_active) {
		BOOL cloak = FALSE;
		DwmSetWindowAttribute(hwnd, DWMWA_CLOAK, &cloak, sizeof(cloak));
		s_unity_early_cloaked = 0;
		displayxr_log("[DisplayXR] transparent_overlay: reverted the EARLY "
		              "cloak (ApplyWindowing never ran)\n");
	}

	if (enabled && !s_overlay_active) {
		s_saved_style   = (DWORD)GetWindowLongPtrW(hwnd, GWL_STYLE);
		s_saved_exstyle = (DWORD)GetWindowLongPtrW(hwnd, GWL_EXSTYLE);

		// Strip decorations on the parent: WS_POPUP gives us a borderless
		// window. We do NOT add WS_EX_LAYERED — Unity's main HWND has its
		// own opaque flip-model swapchain that bypasses LWA_COLORKEY anyway.
		// Transparency comes from the OVERLAY HWND being top-level with
		// WS_EX_NOREDIRECTIONBITMAP (created in displayxr_get_app_main_view
		// when transparent_background_requested is set), so the overlay
		// composites independently of Unity's swapchain.
		//
		// We do NOT add WS_EX_TRANSPARENT to Unity's HWND for cross-process
		// click-through — that broke RawInput delivery to Unity. Instead,
		// we move Unity off-screen (further down in this function) so the
		// cursor never lands on its HWND in the first place. RawInput
		// delivery is by HWND, not by cursor position, so it keeps flowing.
		SetWindowLongPtrW(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
		DWORD ex = s_saved_exstyle & ~WS_EX_LAYERED;
		if (topmost)
			ex |= WS_EX_TOPMOST;
		SetWindowLongPtrW(hwnd, GWL_EXSTYLE, (LONG_PTR)ex);

		SetWindowPos(hwnd, topmost ? HWND_TOPMOST : HWND_TOP, 0, 0, 0, 0,
		             SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED | SWP_SHOWWINDOW);

		// Cloak Unity's main window via DWMWA_CLOAK — DWM stops compositing
		// it (so we don't see Unity's grey/black backbuffer behind the
		// transparent overlay), but the HWND keeps receiving input and
		// Unity keeps rendering normally (eye textures still flow to the
		// runtime via OpenXR). Cloaking is a pure DWM-side hide; it does
		// NOT remove the HWND from OS hit-testing — that's why we ALSO
		// move Unity off-screen below (cross-process click-through, #57).
		{
			BOOL cloak = TRUE;
			HRESULT hr = DwmSetWindowAttribute(hwnd, DWMWA_CLOAK,
			                                   &cloak, sizeof(cloak));
			displayxr_log("[DisplayXR] Cloaked Unity main window via DWMWA_CLOAK: hr=0x%08X\n",
			              (unsigned)hr);
		}

		// Move Unity's HWND off-screen at (-32000, -32000) — Approach A
		// from #57 session 4 for cross-process click-through.
		//
		// Why: cloaking hides Unity from DWM compositing but leaves the
		// HWND in WindowFromPoint's hit-test path. With WS_EX_TRANSPARENT
		// toggled on the overlay (in s_hit_active=0 zones), the OS routes
		// transparent-zone clicks past the overlay to the next window in
		// z-order — that was cloaked Unity, where the click died on
		// Unity's wndproc and never reached desktop apps behind. Moving
		// Unity off-screen takes its HWND out of the hit-test path
		// entirely (cursor never lands on it). The OS then routes
		// transparent-zone clicks to whatever desktop app is at the
		// actual cursor position.
		//
		// Unity continues to receive input via:
		//   - WM_INPUT (RIDEV_INPUTSINK + hwndTarget=unity_hwnd) —
		//     RawInput is delivered by HWND, not by cursor position.
		//   - PostMessage'd left/middle/mouse-move from overlay_wnd_proc.
		//   - IAT-hooked GetForegroundWindow → s_shell_unity_hwnd keeps
		//     Application.isFocused true.
		// Unity's main-window swapchain is never read because
		// XRSettings.gameViewRenderMode = None (set in DisplayXRFeature).
		{
			GetWindowRect(hwnd, &s_unity_saved_rect);
			int w = s_unity_saved_rect.right - s_unity_saved_rect.left;
			int h = s_unity_saved_rect.bottom - s_unity_saved_rect.top;

			// Snap the overlay to Unity's current window rect before we
			// disconnect them. The overlay was created at Unity's old
			// client_origin (still WS_OVERLAPPED with title bar at the
			// time), so it's slightly inset from Unity's window rect.
			// After the WS_POPUP style strip Unity's client area is the
			// full window, and that's what the user perceives as Unity's
			// boundaries — line the overlay up there. After this snap,
			// parent_subclass_proc no longer follows Unity (Unity will
			// be off-screen) and the overlay owns its on-screen position.
			//
			// (#131) EXCEPT in near-fullscreen mode: the overlay was
			// already born covering nearly the whole monitor (monitor
			// minus 1px), and snapping it down to Unity's small WINDOWED
			// rect here would shrink it — then set_overlay_fullscreen would
			// grow it back, two resizes each recreating the presentation
			// swapchain (= flash). The fullscreen overlay is meant to cover
			// the monitor, not Unity's window, and all downstream C# works
			// in full-monitor coords, so leave it where it was born.
			if (s_overlay_hwnd != NULL && IsWindow(s_overlay_hwnd)
			    && !s_fullscreen_overlay_pref) {
				SetWindowPos(s_overlay_hwnd, HWND_TOPMOST,
				             s_unity_saved_rect.left,
				             s_unity_saved_rect.top, w, h,
				             SWP_NOACTIVATE);
			}

			SetWindowPos(hwnd, NULL,
			             DISPLAYXR_UNITY_OFFSCREEN_X,
			             DISPLAYXR_UNITY_OFFSCREEN_Y,
			             w, h,
			             SWP_NOZORDER | SWP_NOACTIVATE);
			s_unity_offscreen = 1;

			// Verify off-screen move took effect — if Unity engine
			// (or some hook) re-positions the window, we'd see a
			// readback rect that's not at -32000,-32000.
			RECT vrc;
			GetWindowRect(hwnd, &vrc);
			displayxr_log("[DisplayXR] Moved Unity main window off-screen: requested (%d,%d %dx%d) readback (%d,%d %dx%d) — was at (%d,%d)\n",
			              DISPLAYXR_UNITY_OFFSCREEN_X,
			              DISPLAYXR_UNITY_OFFSCREEN_Y, w, h,
			              (int)vrc.left, (int)vrc.top,
			              (int)(vrc.right - vrc.left),
			              (int)(vrc.bottom - vrc.top),
			              (int)s_unity_saved_rect.left,
			              (int)s_unity_saved_rect.top);

			// Also log overlay's actual position for sanity check —
			// after the snap above, overlay should be at the saved
			// Unity rect. If overlay is elsewhere, the C# raycast
			// math will be off.
			if (s_overlay_hwnd != NULL && IsWindow(s_overlay_hwnd)) {
				RECT orc;
				GetWindowRect(s_overlay_hwnd, &orc);
				DWORD oex = (DWORD)GetWindowLongPtrW(s_overlay_hwnd, GWL_EXSTYLE);
				displayxr_log("[DisplayXR] Overlay rect after snap: (%d,%d %dx%d) exstyle=0x%08X (TRANSPARENT=%s)\n",
				              (int)orc.left, (int)orc.top,
				              (int)(orc.right - orc.left),
				              (int)(orc.bottom - orc.top),
				              (unsigned)oex,
				              (oex & WS_EX_TRANSPARENT) ? "ON" : "OFF");
			}
		}

		// Install the same focus / raw-input hooks shell mode uses. Cloaked
		// Unity doesn't naturally receive raw input (RAWINPUT defaults to
		// foreground HWND only), so Mouse.current.position freezes and
		// button events don't fire. The focus hook adds RIDEV_INPUTSINK to
		// raw input devices, IAT-hooks GetForegroundWindow / GetFocus to
		// return Unity's HWND, and subclasses Unity's wndproc to suppress
		// deactivation messages — all of which let Unity's input system
		// behave as if Unity were the foreground window. Confirmed via
		// Player.log showing Mouse.current.position frozen across an
		// entire session before this hook was added on the transparent
		// path. Idempotent — safe to call here even though the hook is
		// also installed in the shell-mode branch of xrCreateSession.
		displayxr_install_focus_hook(hwnd);

		s_overlay_active = 1;
		displayxr_log("[DisplayXR] transparent_overlay: enabled (topmost=%d, child=%p) — overlay is top-level + NOREDIRECTIONBITMAP; Unity parent cloaked; transparency owned by runtime DComp\n",
		              topmost, (void *)s_overlay_hwnd);

		// Diagnostic dump — exstyle + layered attrs on parent and overlay
		// HWND. Note: in transparent mode the overlay is now top-level
		// (s_overlay_is_toplevel=1), so it won't appear in the descendant
		// walk below — it's an OWNED top-level, not a child.
		{
			HWND ov = s_overlay_hwnd;
			DWORD pex = (DWORD)GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
			COLORREF k = 0; BYTE a = 0; DWORD f = 0;
			BOOL ok = GetLayeredWindowAttributes(hwnd, &k, &a, &f);
			RECT pcr; GetClientRect(hwnd, &pcr);
			displayxr_log("[DisplayXR] dx parent=%p exstyle=0x%08X layered=%d key=0x%08X flags=0x%X client=%dx%d\n",
			              (void *)hwnd, (unsigned)pex, ok ? 1 : 0,
			              (unsigned)k, (unsigned)f,
			              (int)(pcr.right - pcr.left), (int)(pcr.bottom - pcr.top));

			if (ov != NULL && IsWindow(ov)) {
				DWORD cex2 = (DWORD)GetWindowLongPtrW(ov, GWL_EXSTYLE);
				DWORD cst  = (DWORD)GetWindowLongPtrW(ov, GWL_STYLE);
				COLORREF ck = 0; BYTE ca = 0; DWORD cf = 0;
				BOOL cok = GetLayeredWindowAttributes(ov, &ck, &ca, &cf);
				RECT ocr; GetClientRect(ov, &ocr);
				RECT owr; GetWindowRect(ov, &owr);
				displayxr_log("[DisplayXR] dx overlay=%p style=0x%08X exstyle=0x%08X layered=%d key=0x%08X flags=0x%X client=%dx%d screen=(%d,%d %dx%d) toplevel=%d\n",
				              (void *)ov, (unsigned)cst, (unsigned)cex2,
				              cok ? 1 : 0, (unsigned)ck, (unsigned)cf,
				              (int)(ocr.right - ocr.left), (int)(ocr.bottom - ocr.top),
				              (int)owr.left, (int)owr.top,
				              (int)(owr.right - owr.left), (int)(owr.bottom - owr.top),
				              s_overlay_is_toplevel);
			} else {
				displayxr_log("[DisplayXR] dx overlay=NULL — no overlay HWND set; runtime may present elsewhere\n");
			}

			// Walk descendant windows of parent owned by our process — so we
			// can see if the runtime made its own HWND beneath the overlay.
			HWND walk = NULL;
			DWORD our_pid = GetCurrentProcessId();
			while ((walk = FindWindowExW(hwnd, walk, NULL, NULL)) != NULL) {
				DWORD wpid = 0; GetWindowThreadProcessId(walk, &wpid);
				if (wpid != our_pid) continue;
				char cls[64] = {0};
				GetClassNameA(walk, cls, 63);
				DWORD wex = (DWORD)GetWindowLongPtrW(walk, GWL_EXSTYLE);
				DWORD wst = (DWORD)GetWindowLongPtrW(walk, GWL_STYLE);
				RECT wr; GetClientRect(walk, &wr);
				displayxr_log("[DisplayXR] dx descendant=%p class=%s style=0x%08X exstyle=0x%08X size=%dx%d\n",
				              (void *)walk, cls, (unsigned)wst, (unsigned)wex,
				              (int)(wr.right - wr.left), (int)(wr.bottom - wr.top));
			}
		}

		// Install global low-level mouse hook for click-routing
		// diagnostics. Idempotent; never uninstalled. See the
		// instrumentation block above displayxr_set_overlay_hit_active.
		displayxr_install_ll_mouse_hook();
	} else if (!enabled && s_overlay_active) {
		// Pick the rect Unity should restore to before uncloaking. If the
		// overlay still exists, use its current screen rect — the user
		// has been dragging the avatar around in transparent mode, and
		// they expect the windowed app to appear where the avatar last
		// was. Otherwise fall back to the rect we captured pre-cloak.
		RECT restore_rc = s_unity_saved_rect;
		if (s_overlay_hwnd != NULL && IsWindow(s_overlay_hwnd)) {
			GetWindowRect(s_overlay_hwnd, &restore_rc);
		}

		// Uncloak Unity's main window before restoring styles, so the
		// frame-changed repaint can run with a visible window.
		{
			BOOL cloak = FALSE;
			DwmSetWindowAttribute(hwnd, DWMWA_CLOAK, &cloak, sizeof(cloak));
			s_unity_early_cloaked = 0;
		}
		SetWindowLongPtrW(hwnd, GWL_STYLE, (LONG_PTR)s_saved_style);
		SetWindowLongPtrW(hwnd, GWL_EXSTYLE, (LONG_PTR)s_saved_exstyle);

		// Move Unity back on-screen at the restore rect, then frame-change.
		if (s_unity_offscreen) {
			int w = restore_rc.right - restore_rc.left;
			int h = restore_rc.bottom - restore_rc.top;
			SetWindowPos(hwnd, HWND_NOTOPMOST,
			             restore_rc.left, restore_rc.top, w, h,
			             SWP_FRAMECHANGED);
			s_unity_offscreen = 0;
			displayxr_log("[DisplayXR] Restored Unity main window to (%d,%d) size=%dx%d\n",
			              (int)restore_rc.left, (int)restore_rc.top, w, h);
		} else {
			SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
			             SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED);
		}

		// Drop WS_EX_LAYERED on the overlay too. We didn't save its prior
		// exstyle separately because the overlay is created with a fixed
		// style that we know.
		if (s_overlay_hwnd != NULL && IsWindow(s_overlay_hwnd)) {
			DWORD cex = (DWORD)GetWindowLongPtrW(s_overlay_hwnd, GWL_EXSTYLE);
			SetWindowLongPtrW(s_overlay_hwnd, GWL_EXSTYLE,
			                  (LONG_PTR)(cex & ~WS_EX_LAYERED));
		}

		s_overlay_active = 0;
		displayxr_log("[DisplayXR] transparent_overlay: disabled\n");
	}
}

// ============================================================================
// Simple-window mode (avatar-style) public API
// ============================================================================

void
displayxr_set_simple_window(int enabled, int topmost)
{
	displayxr_log("[DisplayXR] set_simple_window: called enabled=%d topmost=%d shell=%d\n",
	              enabled, topmost, displayxr_is_shell_mode());

	if (displayxr_is_shell_mode())
		return;

	// Style the SAME HWND the runtime was bound to at xrCreateSession
	// (state->window_handle), not a fresh find_unity_hwnd() — the latter
	// can race the foreground/splash window and return a different HWND than
	// the one the runtime composites into (observed: bound CC0F88 vs a later
	// find returning 5D00DA), leaving the real window decorated + unshaped.
	HWND hwnd = NULL;
	DisplayXRState *bound_state = displayxr_get_state();
	if (bound_state != NULL && bound_state->window_handle != NULL)
		hwnd = (HWND)bound_state->window_handle;
	if (hwnd == NULL || !IsWindow(hwnd))
		hwnd = find_unity_hwnd();
	if (hwnd == NULL) {
		displayxr_log("[DisplayXR] set_simple_window: no Unity HWND\n");
		return;
	}

	if (enabled && !s_simple_active) {
		s_simple_hwnd          = hwnd;
		s_simple_topmost       = topmost ? 1 : 0;
		s_simple_saved_style   = (DWORD)GetWindowLongPtrW(hwnd, GWL_STYLE);
		s_simple_saved_exstyle = (DWORD)GetWindowLongPtrW(hwnd, GWL_EXSTYLE);

		// Borderless by default (avatar model): strip Unity's decorations to
		// WS_POPUP. No DWM cloak and no off-screen move — Unity IS the on-
		// screen window the runtime composites into. Decoration toggles back
		// on demand via displayxr_toggle_window_decoration (the B key).
		s_window_decorated = 0;
		SetWindowLongPtrW(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
		DWORD ex = s_simple_saved_exstyle & ~WS_EX_LAYERED;
		if (topmost)
			ex |= WS_EX_TOPMOST;
		SetWindowLongPtrW(hwnd, GWL_EXSTYLE, (LONG_PTR)ex);
		SetWindowPos(hwnd, topmost ? HWND_TOPMOST : HWND_TOP, 0, 0, 0, 0,
		             SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED | SWP_SHOWWINDOW);

		// Subclass Unity's wndproc for the borderless right-drag (#61-
		// bracketed) and the WM_NCHITTEST claim.
		s_simple_orig_wndproc = (WNDPROC)SetWindowLongPtrW(
			hwnd, GWLP_WNDPROC, (LONG_PTR)unity_simple_wnd_proc);

		s_simple_active = 1;
		displayxr_log("[DisplayXR] set_simple_window: enabled on Unity HWND %p (borderless, topmost=%d)\n",
		              (void *)hwnd, topmost);
	} else if (!enabled && s_simple_active) {
		// Restore Unity's original wndproc + styles, drop any region.
		if (s_simple_orig_wndproc != NULL) {
			SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)s_simple_orig_wndproc);
			s_simple_orig_wndproc = NULL;
		}
		SetWindowRgn(hwnd, NULL, TRUE);
		s_hit_mask_region_hash = 0;
		SetWindowLongPtrW(hwnd, GWL_STYLE, (LONG_PTR)s_simple_saved_style);
		SetWindowLongPtrW(hwnd, GWL_EXSTYLE, (LONG_PTR)s_simple_saved_exstyle);
		SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
		             SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED);
		s_simple_active    = 0;
		s_simple_hwnd      = NULL;
		s_window_decorated = 0;
		displayxr_log("[DisplayXR] set_simple_window: disabled\n");
	}
}

// Saved borderless ex-style of the managed window, captured the first time
// decoration is toggled so we can restore it when going borderless again.
static DWORD s_deco_saved_exstyle = 0;
static int   s_deco_exstyle_saved = 0;

// Toggle window decoration (WS_POPUP borderless <-> WS_OVERLAPPEDWINDOW) on the
// managed window — the transparent overlay (default) or the dormant simple-
// window real HWND. Preserves the client rect across the style change so the
// rendered content doesn't jump/rescale (port of the avatar's ToggleDecoration).
static void
apply_window_decoration(int decorated)
{
	HWND hwnd = managed_window_hwnd();
	if (hwnd == NULL)
		return;
	int is_overlay = (hwnd == s_overlay_hwnd);

	// Capture the current client rect in screen space — the invariant we keep
	// fixed so the rendered content doesn't jump or rescale.
	RECT client = {0};
	GetClientRect(hwnd, &client);
	POINT tl = { client.left, client.top };
	ClientToScreen(hwnd, &tl);
	int cw = client.right - client.left;
	int ch = client.bottom - client.top;

	// Remember the borderless ex-style once, to restore it on the way back.
	if (!s_deco_exstyle_saved) {
		s_deco_saved_exstyle = (DWORD)GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
		s_deco_exstyle_saved = 1;
	}

	s_window_decorated = decorated ? 1 : 0;
	DWORD style = (DWORD)((s_window_decorated ? WS_OVERLAPPEDWINDOW : WS_POPUP) | WS_VISIBLE);
	SetWindowLongPtrW(hwnd, GWL_STYLE, style);

	if (is_overlay) {
		// The overlay is born WS_EX_NOREDIRECTIONBITMAP | TOPMOST | TOOLWINDOW |
		// NOACTIVATE. When decorated, drop TOOLWINDOW + NOACTIVATE so a normal
		// title bar shows and the window can be activated for OS drag/resize;
		// keep NOREDIRECTIONBITMAP (transparency) + TOPMOST. Restore the saved
		// borderless ex-style when going back.
		DWORD ex = s_window_decorated
		    ? (s_deco_saved_exstyle & ~(DWORD)(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE))
		    : s_deco_saved_exstyle;
		SetWindowLongPtrW(hwnd, GWL_EXSTYLE, (LONG_PTR)ex);
	}

	// Inflate the desired client rect by the NEW frame so the client area
	// stays put (WS_POPUP has no frame, so this is a no-op going borderless).
	RECT want = { tl.x, tl.y, tl.x + cw, tl.y + ch };
	AdjustWindowRect(&want, style, FALSE);
	SetWindowPos(hwnd, HWND_TOPMOST,
	             want.left, want.top,
	             want.right - want.left, want.bottom - want.top,
	             SWP_FRAMECHANGED);

	// Decorated: clear the silhouette region so the whole framed window is
	// grabbable/visible. Borderless: the next hit-mask push re-shapes it (the
	// region path skips updates while decorated — see region_target_ready).
	if (s_window_decorated) {
		SetWindowRgn(hwnd, NULL, TRUE);
		s_hit_mask_region_hash = 0;
	}

	displayxr_log("[DisplayXR] window decoration: %s (hwnd=%p overlay=%d)\n",
	              s_window_decorated ? "ON (move/resize)" : "OFF (borderless)",
	              (void *)hwnd, is_overlay);
}

void
displayxr_set_window_decorated(int decorated)
{
	if (managed_window_hwnd() == NULL)
		return;
	apply_window_decoration(decorated);
}

void
displayxr_toggle_window_decoration(void)
{
	if (managed_window_hwnd() == NULL)
		return;
	apply_window_decoration(!s_window_decorated);
}

// Set the rectangular hit-test region of the overlay.
//
// In opaque WS_CHILD mode (legacy / Game View overlay): updates
// s_hit_rect, which WM_NCHITTEST in overlay_wnd_proc / parent_subclass_
// proc uses as a fast HTCLIENT-vs-HTTRANSPARENT discriminator.
//
// In transparent WS_POPUP + NOREDIRECTIONBITMAP mode (issue #57): also
// drives SetWindowRgn on the overlay HWND. Outside the region, the OS
// treats our window as if it didn't exist — both rendering and hit-
// testing — so input is routed natively to whatever desktop window is
// at the cursor (full cross-process fidelity: real DefWindowProc modal
// SC_MOVE/SC_SIZE/SC_CLOSE loops with proper GetKeyState, native
// cursor adaptation, native menu activation, native hover and
// TrackMouseEvent — without any plugin forwarder gymnastics).
//
// Push the screen-space bounding rect of the clickable renderers
// (cube/avatar silhouette) each frame, converted to overlay client
// coords (top-left origin). C# already computes this rect via
// TryGetUnionScreenRect for opaque mode; the same rect drives both
// paths in transparent mode.
//
// Region stability — padding + hysteresis. Skinned-mesh idle
// animation jitters the screen-space AABB by a few pixels per frame
// even when the user isn't moving the cube. Without stabilization the
// region's edge oscillates across a stationary cursor and the OS flips
// routing every frame (visible as: hover state on the desktop app
// underneath blinks on/off; cursor adaptation flickers). We address
// this with two tweaks:
//   - PADDING_PX (+16 on each side): the region is larger than the
//     raw silhouette AABB. The cursor must be at least PADDING_PX
//     outside the silhouette before falling into the OS-routed zone.
//     Idle jitter can't cross a 16-pixel buffer.
//   - HYSTERESIS_PX (4): cache the last-applied region rect; skip
//     SetWindowRgn if the new (padded) rect is within HYSTERESIS_PX
//     of the last on all four edges. Idle jitter never re-pushes;
//     only real motion (cube drag, animation re-pose, zoom) does.
//
// SetWindowRgn takes ownership of the HRGN, so we must NOT
// DeleteObject afterward on success. The previous region (if any) is
// freed by the OS as part of SetWindowRgn.
//
// w<=0 / h<=0 → clear the region (overlay catches everywhere — used
// as the init default before C# pushes per-frame).
#define DXR_HIT_RECT_PADDING_PX     16
#define DXR_HIT_RECT_HYSTERESIS_PX  4

// The HWND that owns the click-through region: Unity's REAL HWND in simple-
// window mode, the top-level overlay HWND otherwise. Both use the identical
// SetWindowRgn region machinery below.
static HWND
region_target_hwnd(void)
{
	if (s_simple_active)
		return s_simple_hwnd;
	return s_overlay_hwnd;
}

// True when a region target exists and is accepting region updates. A decorated
// window shows its full frame (no silhouette clip), so region updates are
// suppressed until it goes borderless again — for both the overlay and the
// (dormant) simple-window real HWND.
static int
region_target_ready(void)
{
	if (s_window_decorated)
		return 0;
	if (s_simple_active)
		return s_simple_hwnd != NULL && IsWindow(s_simple_hwnd);
	return s_overlay_is_toplevel && s_overlay_hwnd != NULL && IsWindow(s_overlay_hwnd);
}

// The window the avatar-style decoration toggle (B key) manages: the transparent
// overlay when active, else the (dormant) simple-window real HWND.
static HWND
managed_window_hwnd(void)
{
	if (s_overlay_active && s_overlay_hwnd != NULL && IsWindow(s_overlay_hwnd))
		return s_overlay_hwnd;
	if (s_simple_active && s_simple_hwnd != NULL && IsWindow(s_simple_hwnd))
		return s_simple_hwnd;
	return NULL;
}

void
displayxr_set_overlay_hit_rect(int x, int y, int w, int h)
{
	s_hit_rect.left   = x;
	s_hit_rect.top    = y;
	s_hit_rect.right  = x + w;
	s_hit_rect.bottom = y + h;

	// Opaque WS_CHILD overlay: WM_NCHITTEST consumes s_hit_rect, no region.
	if (!s_simple_active && !s_overlay_is_toplevel)
		return;
	if (!region_target_ready())
		return;
	HWND target = region_target_hwnd();
	// Per-pixel silhouette mask path is active: stop driving the
	// region from the AABB. Mask owns SetWindowRgn from here on.
	if (s_hit_mask_active)
		return;

	// Cache of last applied region rect (post-padding). Sentinel value
	// {INT_MIN,...} so the first call always goes through. Also reset
	// to sentinel when we clear the region (w<=0 path below).
	static RECT s_last_rgn_rect = { INT_MIN, INT_MIN, INT_MIN, INT_MIN };

	if (w <= 0 || h <= 0) {
		SetWindowRgn(target, NULL, TRUE);
		s_last_rgn_rect.left = INT_MIN;
		displayxr_log("[DisplayXR] hit_region: cleared (overlay catches everywhere)\n");
		return;
	}

	// Pad — gives the cursor a hysteresis margin outside the silhouette.
	RECT padded = {
		.left   = x - DXR_HIT_RECT_PADDING_PX,
		.top    = y - DXR_HIT_RECT_PADDING_PX,
		.right  = x + w + DXR_HIT_RECT_PADDING_PX,
		.bottom = y + h + DXR_HIT_RECT_PADDING_PX,
	};

	// Hysteresis — skip the update if all four edges are within the
	// threshold of the last applied rect. Compares against the PADDED
	// rect so the comparison is in the same coordinate space as what
	// we'd push.
	if (s_last_rgn_rect.left != INT_MIN) {
		LONG dl = padded.left   - s_last_rgn_rect.left;
		LONG dt = padded.top    - s_last_rgn_rect.top;
		LONG dr = padded.right  - s_last_rgn_rect.right;
		LONG db = padded.bottom - s_last_rgn_rect.bottom;
		if (dl < 0) dl = -dl;
		if (dt < 0) dt = -dt;
		if (dr < 0) dr = -dr;
		if (db < 0) db = -db;
		if (dl <= DXR_HIT_RECT_HYSTERESIS_PX &&
		    dt <= DXR_HIT_RECT_HYSTERESIS_PX &&
		    dr <= DXR_HIT_RECT_HYSTERESIS_PX &&
		    db <= DXR_HIT_RECT_HYSTERESIS_PX) {
			return;
		}
	}

	HRGN rgn = CreateRectRgn(padded.left, padded.top, padded.right, padded.bottom);
	if (rgn == NULL) {
		displayxr_log("[DisplayXR] hit_region: CreateRectRgn failed (%ld,%ld %ldx%ld) err=%lu\n",
		              padded.left, padded.top,
		              padded.right - padded.left, padded.bottom - padded.top,
		              (unsigned long)GetLastError());
		return;
	}
	// SetWindowRgn takes ownership of rgn on success.
	if (!SetWindowRgn(target, rgn, TRUE)) {
		DeleteObject(rgn);
		displayxr_log("[DisplayXR] hit_region: SetWindowRgn failed (%ld,%ld %ldx%ld) err=%lu\n",
		              padded.left, padded.top,
		              padded.right - padded.left, padded.bottom - padded.top,
		              (unsigned long)GetLastError());
		return;
	}
	s_last_rgn_rect = padded;

	// Throttle the log to ~1 Hz — even with hysteresis a moving cube
	// re-pushes regularly.
	static DWORD s_last_log_tick = 0;
	DWORD now = GetTickCount();
	if (now - s_last_log_tick >= 1000) {
		s_last_log_tick = now;
		displayxr_log("[DisplayXR] hit_region: raw=(%d,%d %dx%d) padded=(%ld,%ld %ldx%ld) on hwnd=%p\n",
		              x, y, w, h,
		              padded.left, padded.top,
		              padded.right - padded.left, padded.bottom - padded.top,
		              (void *)target);
	}
}

// Per-pixel silhouette hit-test region (issue #57 Approach B+).
//
// C# renders each clickable Renderer to a small R8 RenderTexture with
// the Hidden/DisplayXR/Silhouette shader (red=1 where any geometry
// rasterizes), AsyncGPUReadbacks the result, and hands the bytes to
// us each frame. We walk the mask row by row, RLE-encoding horizontal
// runs of opaque pixels into RECTs scaled to overlay client coords,
// then ExtCreateRegion + SetWindowRgn. Outside the silhouette the OS
// treats our window as if it didn't exist — clicks/hover route
// natively to whatever desktop window is at the cursor, including in
// the gaps that an AABB can't express (between the tiger's legs,
// above the hat tip, around the curling tail). Per-pixel accurate
// cross-process click-through with no PostMessage gymnastics.
//
// Inputs:
//   mask:   mask_w * mask_h bytes; non-zero = opaque (catch), zero =
//           transparent (route past). Typically 256x144 — high enough
//           to capture detail like leg gaps, low enough that the
//           readback + RLE per frame is well under the budget.
//   dst_w, dst_h: overlay client size; each mask pixel covers
//           dst_w/mask_w by dst_h/mask_h overlay pixels (integer
//           division — exact for power-of-two ratios, off-by-one at
//           worst otherwise, which is invisible at 8x upsampling).
//
// First successful call sets s_hit_mask_active=1 — the AABB-region
// path in displayxr_set_overlay_hit_rect stops calling SetWindowRgn
// from that point on so the per-pixel region owns routing. This is a
// one-way flag: once a mask has been applied, callers are committed
// to keeping it fresh each frame (or the OS sees the last mask, which
// may not match where the cube is now). NULL mask resets the flag
// and restores the AABB-region path.
//
// HRGN ownership: SetWindowRgn takes ownership on success. We must
// NOT DeleteObject the region we just handed it. The previous region
// (if any) is freed by the OS as part of the call.
//
// Capacity: the worst case for a 256x144 mask is ~36k single-pixel
// rects (alternating opaque/transparent every pixel of every row),
// which would be ~580 KB of RECT data — fine but wasteful. Real
// silhouettes RLE down to a few hundred to a few thousand rects.
//
// (#166) Multi-zone: when the provider advertises >1 3D zone, the full-window mask
// is stamped into EACH zone's rect (union), not just the primary canvas rect — all
// zones are portals of the same content, so the same silhouette belongs in every
// zone. On the hook path / single zone the provider reports 1 zone and this falls
// back to the canvas-rect mapping below. Provider getters (extern "C", defined in
// displayxr_provider_session.cpp) are safe to call when the provider is inactive
// (they read zero-initialized state → count 1).
extern uint32_t dxr_prov_get_zone_count(void);
extern int      dxr_prov_get_zone_rect_px(uint32_t zone, int *x, int *y, int *w, int *h);

void
displayxr_set_overlay_hit_mask(const uint8_t *mask, int mask_w, int mask_h,
                               int dst_w, int dst_h)
{
	if (!region_target_ready())
		return;
	HWND target = region_target_hwnd();

	if (mask == NULL || mask_w <= 0 || mask_h <= 0 ||
	    dst_w <= 0 || dst_h <= 0) {
		// NULL/empty: clear and revert to AABB-region path.
		SetWindowRgn(target, NULL, TRUE);
		s_hit_mask_active = 0;
		s_hit_mask_region_hash = 0;
		displayxr_log("[DisplayXR] hit_mask: cleared (reverting to AABB region)\n");
		return;
	}

	// Build RECTs into a growable buffer. Initial capacity 1024 is
	// usually enough for a single contiguous silhouette; we realloc if
	// fragmentation pushes higher.
	int  cap = 1024;
	int  n   = 0;
	RECT *rects = (RECT *)malloc((size_t)cap * sizeof(RECT));
	if (rects == NULL)
		return;

	// Target rect(s) the full-window mask is mapped into. (#131) A single canvas
	// sub-rect: the runtime shrinks the 3D weave into it, so the silhouette lives
	// there too — map the mask into it (same scale+offset the weave applies), not the
	// full overlay. (#166) Multi-zone: the runtime weaves each zone into its own rect,
	// so stamp the mask into EACH zone rect and union. dxr_prov_get_zone_count()>1
	// selects multi-zone; otherwise fall back to the canvas rect (== full client when
	// no sub-rect is set) — the legacy single-zone/hook mapping, unchanged.
	int      zone_count = 1;
	int      use_zones  = 0;
	uint32_t pzc = dxr_prov_get_zone_count();
	if (pzc > 1) { zone_count = (int)pzc; use_zones = 1; }

	for (int zi = 0; zi < zone_count; zi++) {
		int32_t  cvx = 0, cvy = 0;
		uint32_t cvw = (uint32_t)dst_w, cvh = (uint32_t)dst_h;
		if (use_zones) {
			int rx = 0, ry = 0, rw = 0, rh = 0;
			if (!dxr_prov_get_zone_rect_px((uint32_t)zi, &rx, &ry, &rw, &rh)
			    || rw <= 0 || rh <= 0)
				continue;
			cvx = rx; cvy = ry; cvw = (uint32_t)rw; cvh = (uint32_t)rh;
		} else {
			displayxr_get_canvas_rect_px(&cvx, &cvy, &cvw, &cvh);
		}

		// Round each rect's edges OUTWARD when mapping mask cells → target pixels:
		// leading edges (left/top) floor, trailing edges (right/bottom) ceil. A mask
		// cell covers cvw/mask_w (cvh/mask_h) target px; with a sub-rect this can be
		// several px per cell, and plain truncation on the trailing edges would shrink
		// the catch-region inside the silhouette and clip the tiger's edges. Outward
		// rounding dilates by <1 cell so the region fully covers the silhouette.
		for (int my = 0; my < mask_h; my++) {
			const uint8_t *row = mask + (size_t)my * (size_t)mask_w;
			int top    = (int)(cvy + (LONGLONG)my * cvh / mask_h);
			int bottom = (int)(cvy + ((LONGLONG)(my + 1) * cvh + mask_h - 1) / mask_h);
			int mx = 0;
			while (mx < mask_w) {
				while (mx < mask_w && row[mx] == 0) mx++;
				if (mx >= mask_w) break;
				int x0 = mx;
				while (mx < mask_w && row[mx] != 0) mx++;
				int x1 = mx;
				if (n >= cap) {
					int new_cap = cap * 2;
					RECT *nr = (RECT *)realloc(rects,
					                           (size_t)new_cap * sizeof(RECT));
					if (nr == NULL) { free(rects); return; }
					rects = nr;
					cap = new_cap;
				}
				rects[n].left   = (LONG)(cvx + (LONGLONG)x0 * cvw / mask_w);
				rects[n].top    = (LONG)top;
				rects[n].right  = (LONG)(cvx + ((LONGLONG)x1 * cvw + mask_w - 1) / mask_w);
				rects[n].bottom = (LONG)bottom;
				n++;
			}
		}
	}

	// (#131) Union the opaque surround rect (e.g. a high-res text bubble drawn in
	// the surround region) so it catches clicks even though it's outside the 3D
	// silhouette. Appended as one more RECT before ExtCreateRegion; the empty
	// surround around it stays click-through.
	if (s_surround_rect_valid) {
		if (n >= cap) {
			int new_cap = cap + 1;
			RECT *nr = (RECT *)realloc(rects,
			                           (size_t)new_cap * sizeof(RECT));
			if (nr == NULL) { free(rects); return; }
			rects = nr;
			cap = new_cap;
		}
		rects[n++] = s_surround_rect;
	}

	// (#131) Union the per-pixel surround mask (e.g. the exact rounded-bubble +
	// triangular-tail shape), RLE'd into rects over its dst rect with the same
	// outward edge rounding as the tiger silhouette above. This is what makes the
	// empty area BESIDE a non-rectangular tail route clicks through — a single
	// bounding rect can't express that. Flat 2D: the caller hands us the shape
	// directly (no view/disparity math, since the surround is post-weave).
	if (s_surround_mask_valid && s_surround_mask != NULL) {
		LONG sdx = s_surround_mask_dst.left;
		LONG sdy = s_surround_mask_dst.top;
		LONG sdw = s_surround_mask_dst.right  - s_surround_mask_dst.left;
		LONG sdh = s_surround_mask_dst.bottom - s_surround_mask_dst.top;
		int  smw = s_surround_mask_w, smh = s_surround_mask_h;
		for (int my = 0; my < smh; my++) {
			const uint8_t *row = s_surround_mask + (size_t)my * (size_t)smw;
			int top    = (int)(sdy + (LONGLONG)my * sdh / smh);
			int bottom = (int)(sdy + ((LONGLONG)(my + 1) * sdh + smh - 1) / smh);
			int mx = 0;
			while (mx < smw) {
				while (mx < smw && row[mx] == 0) mx++;
				if (mx >= smw) break;
				int x0 = mx;
				while (mx < smw && row[mx] != 0) mx++;
				int x1 = mx;
				if (n >= cap) {
					int new_cap = cap * 2;
					RECT *nr = (RECT *)realloc(rects,
					                           (size_t)new_cap * sizeof(RECT));
					if (nr == NULL) { free(rects); return; }
					rects = nr;
					cap = new_cap;
				}
				rects[n].left   = (LONG)(sdx + (LONGLONG)x0 * sdw / smw);
				rects[n].top    = (LONG)top;
				rects[n].right  = (LONG)(sdx + ((LONGLONG)x1 * sdw + smw - 1) / smw);
				rects[n].bottom = (LONG)bottom;
				n++;
			}
		}
	}

	// (#259) Skip identical regions: hash the final rect list + dst size and
	// compare against the last applied region. SetWindowRgn(bRedraw=TRUE)
	// every frame forces a repaint invalidation even when nothing changed --
	// with a static avatar that is 60 wasted invalidations per second and a
	// visible flicker at the region edge during window drags.
	{
		unsigned long long hh = 1469598103934665603ULL; /* FNV-1a 64 */
		const unsigned char *hb = (const unsigned char *)rects;
		size_t hn = (size_t)n * sizeof(RECT);
		for (size_t hi = 0; hi < hn; hi++) {
			hh ^= hb[hi];
			hh *= 1099511628211ULL;
		}
		hh ^= (unsigned long long)(unsigned)n;      hh *= 1099511628211ULL;
		hh ^= (unsigned long long)(unsigned)dst_w;  hh *= 1099511628211ULL;
		hh ^= (unsigned long long)(unsigned)dst_h;  hh *= 1099511628211ULL;
		if (hh == 0) hh = 1; /* 0 is the "unknown" sentinel */
		if (s_hit_mask_active && hh == s_hit_mask_region_hash) {
			free(rects);
			return;
		}
		s_hit_mask_region_hash = hh;
	}

	HRGN rgn = NULL;
	if (n == 0) {
		// Empty silhouette: 0x0 rect = nothing-catches region.
		rgn = CreateRectRgn(0, 0, 0, 0);
	} else {
		DWORD buf_size = sizeof(RGNDATAHEADER) + (DWORD)n * sizeof(RECT);
		RGNDATA *data = (RGNDATA *)malloc(buf_size);
		if (data == NULL) { free(rects); return; }

		data->rdh.dwSize   = sizeof(RGNDATAHEADER);
		data->rdh.iType    = RDH_RECTANGLES;
		data->rdh.nCount   = (DWORD)n;
		data->rdh.nRgnSize = (DWORD)n * sizeof(RECT);

		LONG bl = rects[0].left,  bt = rects[0].top;
		LONG br = rects[0].right, bb = rects[0].bottom;
		for (int i = 1; i < n; i++) {
			if (rects[i].left   < bl) bl = rects[i].left;
			if (rects[i].top    < bt) bt = rects[i].top;
			if (rects[i].right  > br) br = rects[i].right;
			if (rects[i].bottom > bb) bb = rects[i].bottom;
		}
		data->rdh.rcBound.left   = bl;
		data->rdh.rcBound.top    = bt;
		data->rdh.rcBound.right  = br;
		data->rdh.rcBound.bottom = bb;
		memcpy(data->Buffer, rects, (size_t)n * sizeof(RECT));

		rgn = ExtCreateRegion(NULL, buf_size, data);
		free(data);

		if (rgn == NULL) {
			displayxr_log("[DisplayXR] hit_mask: ExtCreateRegion failed (n=%d) err=%lu\n",
			              n, (unsigned long)GetLastError());
			free(rects);
			return;
		}
	}
	free(rects);

	if (!SetWindowRgn(target, rgn, TRUE)) {
		DeleteObject(rgn);
		s_hit_mask_region_hash = 0;
		displayxr_log("[DisplayXR] hit_mask: SetWindowRgn failed err=%lu\n",
		              (unsigned long)GetLastError());
		return;
	}
	s_hit_mask_active = 1;

	// Throttle the log to ~1 Hz — runs every frame.
	static DWORD s_last_log_tick = 0;
	DWORD now = GetTickCount();
	if (now - s_last_log_tick >= 1000) {
		s_last_log_tick = now;
		displayxr_log( "[DisplayXR] hit_mask: mask=%dx%d rects=%d dst=%dx%d\n",
		              mask_w, mask_h, n, dst_w, dst_h);
	}
}

void
displayxr_set_overlay_surround_rect(int x, int y, int w, int h)
{
	// (#131) Store the opaque surround rect (overlay client pixels, top-left
	// origin); displayxr_set_overlay_hit_mask unions it into the window region on
	// the next frame so the bubble catches clicks. w<=0 || h<=0 clears it.
	if (w <= 0 || h <= 0) {
		if (s_surround_rect_valid)
			displayxr_log("[DisplayXR] surround_rect: cleared\n");
		s_surround_rect_valid = 0;
		return;
	}
	s_surround_rect.left   = x;
	s_surround_rect.top    = y;
	s_surround_rect.right  = x + w;
	s_surround_rect.bottom = y + h;
	s_surround_rect_valid  = 1;
	displayxr_log("[DisplayXR] surround_rect: (%d,%d) %dx%d\n", x, y, w, h);
}

void
displayxr_set_overlay_surround_mask(const uint8_t *mask, int mask_w, int mask_h,
                                    int dst_x, int dst_y, int dst_w, int dst_h)
{
	// (#131) Store a per-pixel surround shape mask (non-zero = opaque/catch),
	// mapped over [dst_x,dst_y,dst_w,dst_h] in overlay client px when the window
	// region is rebuilt by displayxr_set_overlay_hit_mask. Owned copy so the
	// caller's buffer need not outlive the call. NULL/empty clears it.
	if (mask == NULL || mask_w <= 0 || mask_h <= 0 || dst_w <= 0 || dst_h <= 0) {
		if (s_surround_mask_valid)
			displayxr_log("[DisplayXR] surround_mask: cleared\n");
		free(s_surround_mask);
		s_surround_mask       = NULL;
		s_surround_mask_w     = 0;
		s_surround_mask_h     = 0;
		s_surround_mask_valid = 0;
		return;
	}

	size_t bytes = (size_t)mask_w * (size_t)mask_h;
	uint8_t *copy = (uint8_t *)malloc(bytes);
	if (copy == NULL) {
		displayxr_log("[DisplayXR] surround_mask: alloc failed (%dx%d)\n",
		              mask_w, mask_h);
		return;
	}
	memcpy(copy, mask, bytes);

	free(s_surround_mask);
	s_surround_mask        = copy;
	s_surround_mask_w      = mask_w;
	s_surround_mask_h      = mask_h;
	s_surround_mask_dst.left   = dst_x;
	s_surround_mask_dst.top    = dst_y;
	s_surround_mask_dst.right  = dst_x + dst_w;
	s_surround_mask_dst.bottom = dst_y + dst_h;
	s_surround_mask_valid  = 1;
	displayxr_log("[DisplayXR] surround_mask: %dx%d -> dst (%d,%d) %dx%d\n",
	              mask_w, mask_h, dst_x, dst_y, dst_w, dst_h);
}

void
displayxr_set_fullscreen_overlay_pref(int enabled)
{
	// (#131) Opt in to a born-fullscreen overlay. Call as EARLY as possible
	// (e.g. C# RuntimeInitializeOnLoadMethod(BeforeSplashScreen)) so it is set
	// before the runtime creates the overlay in displayxr_get_app_main_view;
	// then the overlay is born covering the monitor and the later
	// displayxr_set_overlay_fullscreen() does not resize → no startup flash.
	s_fullscreen_overlay_pref = enabled ? 1 : 0;
	displayxr_log("[DisplayXR] fullscreen_overlay_pref = %d\n",
	              s_fullscreen_overlay_pref);
}

void
displayxr_set_provider_opaque_overlay(int enabled)
{
	// (#166) Call BEFORE displayxr_get_app_main_view() so the overlay is born as a
	// top-level WS_POPUP + NOREDIRECTIONBITMAP (composites the runtime's DComp
	// weave) instead of a WS_CHILD (which doesn't). Used by the provider's in-app
	// weave path. No effect if transparent-overlay mode is also requested.
	s_provider_opaque_overlay = enabled ? 1 : 0;
	displayxr_log("[DisplayXR] provider_opaque_overlay = %d\n",
	              s_provider_opaque_overlay);
}

void
displayxr_set_overlay_fullscreen(int enabled)
{
	// (#131) Fixed full-screen, app-managed window mode. Resizing the OS HWND
	// per interaction caused the three region-editor quirks (outline only in the
	// 2D area, shift-then-snap on resize, content vanishing mid-resize) plus the
	// #61 phase-snap. Instead we pin the overlay to its monitor ONCE at the
	// aligned origin and let the app place its 3D canvas sub-rect + 2D surround
	// in virtual rects inside it — pure rectangle math, no OS move/resize, no
	// snap. The app drives translate/resize via displayxr_set_canvas_rect and
	// the surround mask; this fn only disables the native right-drag MOVE and
	// sizes the surface.
	s_app_managed_window = enabled ? 1 : 0;

	if (s_overlay_hwnd == NULL) {
		displayxr_log("[DisplayXR] fullscreen: %s (no overlay HWND yet)\n",
		              enabled ? "enabled" : "disabled");
		return;
	}

	if (enabled) {
		HMONITOR mon = MonitorFromWindow(s_overlay_hwnd,
		                                 MONITOR_DEFAULTTONEAREST);
		MONITORINFO mi;
		mi.cbSize = sizeof(mi);
		if (GetMonitorInfo(mon, &mi)) {
			// Near-fullscreen target = monitor origin, sized to monitor MINUS
			// 1px (right + bottom). Exactly the birth geometry in
			// displayxr_get_app_main_view, so for a born-near-fullscreen overlay
			// this is already satisfied and we skip the SetWindowPos. The 1px
			// short keeps the window DWM-composited (an exact-monitor window
			// trips fullscreen-optimization / independent-flip = the white
			// flash). HWND_TOPMOST keeps it above the taskbar.
			int x = mi.rcMonitor.left;
			int y = mi.rcMonitor.top;
			int w = (mi.rcMonitor.right - mi.rcMonitor.left) - 1;
			int h = (mi.rcMonitor.bottom - mi.rcMonitor.top) - 1;

			// If already at the target rect (born near-fullscreen), skip the
			// SetWindowPos entirely — a same-size SetWindowPos still fires
			// WM_WINDOWPOSCHANGED / a z-order change that DWM can hitch on, and
			// any genuine resize recreates the presentation swapchain (flash).
			// The resize path here is only a fallback for when the pref landed
			// too late and the overlay was born at Unity's window size.
			RECT cur;
			GetWindowRect(s_overlay_hwnd, &cur);
			if (cur.left == x && cur.top == y &&
			    cur.right == x + w && cur.bottom == y + h) {
				displayxr_log("[DisplayXR] fullscreen: enabled -> "
				              "(%d,%d) %dx%d already near-fullscreen, "
				              "no resize (app-managed)\n", x, y, w, h);
				return;
			}

			SetWindowPos(s_overlay_hwnd, HWND_TOPMOST, x, y, w, h,
			             SWP_NOACTIVATE);
			displayxr_log("[DisplayXR] fullscreen: enabled -> "
			              "(%d,%d) %dx%d topmost (resized)\n", x, y, w, h);
		} else {
			displayxr_log("[DisplayXR] fullscreen: GetMonitorInfo "
			              "failed\n");
		}
	} else {
		displayxr_log("[DisplayXR] fullscreen: disabled\n");
	}
}

void
displayxr_set_overlay_cursor(int shape)
{
	// (#131) Store the desired overlay cursor; WM_SETCURSOR applies it on every
	// mouse move within the client area. Only an int write here (no SetCursor)
	// so it is safe to call from C# on Unity's main thread regardless of which
	// thread pumps the overlay's messages. Out-of-range falls back to arrow.
	if (shape < 0 || shape > 5)
		shape = 0;
	InterlockedExchange(&s_overlay_cursor, (LONG)shape);
}

// Provided by the provider session (#225): the live workspace-tile canvas px.
extern int dxr_prov_workspace_tile_size(uint32_t *w, uint32_t *h);

void
displayxr_get_overlay_size(int *width, int *height)
{
	// Workspace tile (#225): report the shell-driven tile canvas so the app
	// authors its window/zone/Local2D for the tile and re-fits on resize (its
	// own minimized OS window/backbuffer can't track the 3D-window resize).
	if (displayxr_is_shell_mode()) {
		uint32_t tw = 0, th = 0;
		if (dxr_prov_workspace_tile_size(&tw, &th)) {
			if (width)  *width  = (int)tw;
			if (height) *height = (int)th;
			return;
		}
	}

	// Simple-window mode has no overlay — report Unity's REAL HWND client
	// size so C# silhouette/dst math uses the live window dimensions.
	HWND h = (s_simple_active && s_simple_hwnd != NULL) ? s_simple_hwnd
	                                                    : s_overlay_hwnd;
	if (h != NULL && IsWindow(h)) {
		RECT rc;
		GetClientRect(h, &rc);
		if (width)  *width  = (int)(rc.right - rc.left);
		if (height) *height = (int)(rc.bottom - rc.top);
	} else {
		if (width)  *width  = 0;
		if (height) *height = 0;
	}
}

int
displayxr_is_our_process_foreground(void)
{
	// Shell/workspace mode: the workspace shell is the OS-foreground process,
	// never us — but it forwards input ONLY to the shell-focused app. So any
	// input that reaches us is, by construction, meant for us; always report
	// "foreground" so the app's input controller (which gates WASD/mouse on
	// this) doesn't drop shell-forwarded input. Without this, Unity apps under
	// the shell silently ignore all keyboard/mouse (the real check below
	// returns the shell's PID). This bug was masked until window-gated engines
	// actually rendered under the shell. Mirrors the macOS stub, which returns
	// 1 unconditionally for the same "if we're seeing input, it's ours" reason.
	if (displayxr_is_shell_mode())
		return 1;

	// GetForegroundWindow is IAT-hooked in Unity's exe + UnityPlayer.dll
	// (see displayxr_install_focus_hook) so Unity always sees itself as
	// foreground. The plugin DLL's IAT is NOT hooked, so this call returns
	// the actual OS foreground.
	HWND fg = GetForegroundWindow();
	if (fg == NULL)
		return 0;
	DWORD pid = 0;
	GetWindowThreadProcessId(fg, &pid);
	return (pid == GetCurrentProcessId()) ? 1 : 0;
}

void
displayxr_set_overlay_hit_active(int active)
{
	// Approach B (region-based click-through): s_hit_active still tracks
	// the C# per-pixel raycast result for callers that want to know "is
	// the cursor over a clickable renderer?", but it no longer drives the
	// OS hit-test routing. Routing is owned by SetWindowRgn via
	// displayxr_set_overlay_hit_region — outside the region the OS treats
	// our window as if it doesn't exist (native click-through); inside,
	// the overlay catches the click and posts it to Unity, which runs its
	// own per-pixel raycast to decide whether to act on it.
	int new_active = active ? 1 : 0;
	if (new_active == s_hit_active)
		return;
	s_hit_active = new_active;
	// (No log line per frame — would flood at 60 Hz. The region-update
	// path logs from displayxr_set_overlay_hit_region instead.)
}

void
displayxr_get_overlay_pointer(int *clientX, int *clientY, int *buttons)
{
	if (clientX != NULL) *clientX = -1;
	if (clientY != NULL) *clientY = -1;

	POINT pt;
	if (GetCursorPos(&pt) && s_overlay_hwnd != NULL && IsWindow(s_overlay_hwnd)) {
		ScreenToClient(s_overlay_hwnd, &pt);
		if (clientX != NULL) *clientX = pt.x;
		if (clientY != NULL) *clientY = pt.y;
	}

	if (buttons != NULL) {
		int b = 0;
		if (s_ded_clickthrough) {
			// GameView weave-to-texture probe (Task (a)): the weave window is
			// click-through (WM_NCHITTEST → HTTRANSPARENT) so it never receives
			// WM_LBUTTONDOWN, and the focus-hook subclass is skipped in this path —
			// so s_vkey_state is never populated. The editor Game view is the
			// clickable surface here, so read the physical button state globally
			// (GetAsyncKeyState). Motion comes from Mouse.current.delta (raw input to
			// the foreground editor). This lets the sample controller's provider-mode
			// button path (displayxr_get_overlay_pointer) drive left-drag rotate.
			//
			// (#740 f-up) Origin-gate that global state: report a button only while
			// a press that STARTED inside the weave window rect (== the Game view
			// surface) is held. GetAsyncKeyState is global, so without this a
			// title-bar drag of the editor (which now keeps the PlayerLoop running —
			// the custom host drag) or a click on any other editor panel would drive
			// the sample controller's left-drag rotate while the user is just moving
			// the window. Latch at the up→down transition; a drag that leaves the
			// rect keeps its latch until release (normal drag semantics).
			static int s_btn_prev[3]   = {0, 0, 0};
			static int s_btn_inside[3] = {0, 0, 0};
			static const int s_btn_vk[3] = {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON};
			RECT wr;
			int have_rect = s_overlay_hwnd != NULL && IsWindow(s_overlay_hwnd) &&
			                GetWindowRect(s_overlay_hwnd, &wr);
			for (int i = 0; i < 3; i++) {
				int down = (GetAsyncKeyState(s_btn_vk[i]) & 0x8000) != 0;
				if (down && !s_btn_prev[i]) {
					POINT cp;
					s_btn_inside[i] = have_rect && GetCursorPos(&cp) && PtInRect(&wr, cp);
				} else if (!down) {
					s_btn_inside[i] = 0;
				}
				s_btn_prev[i] = down;
				if (down && s_btn_inside[i]) b |= (1 << i);
			}
			*buttons = b;
			return;
		}
		// s_vkey_state is updated by shell_subclass_proc on Unity's HWND
		// (installed via displayxr_install_focus_hook from the transparent
		// path). PostMessage'd left/right/middle clicks from overlay_wnd_proc
		// flow through Unity's wndproc → subclass → here.
		if (s_vkey_state[VK_LBUTTON] & 0x8000) b |= 1;
		if (s_vkey_state[VK_RBUTTON] & 0x8000) b |= 2;
		if (s_vkey_state[VK_MBUTTON] & 0x8000) b |= 4;
		*buttons = b;
	}
}

int
displayxr_consume_overlay_wheel_delta(void)
{
	// Atomic read-and-zero. Win32 raw delta units (120 per notch).
	return (int)InterlockedExchange(&s_overlay_wheel_accum, 0);
}

int
displayxr_consume_overlay_close_request(void)
{
	// Atomic read-and-zero. Returns 1 once after the overlay's close button
	// (or Alt+F4) was pressed; C# calls Application.Quit() on a 1.
	return (int)InterlockedExchange(&s_overlay_close_requested, 0);
}

void
displayxr_resize_overlay(int width, int height)
{
	// Keyboard-/app-driven resize of the managed window (overlay HWND, or the
	// dormant simple-window HWND). This is the RELIABLE resize path on the SR
	// display: the Leia weaver subclass on the overlay claims edge button-downs
	// when the overlay is a non-activating satellite of cloaked Unity (hardware-
	// traced), so OS sizing-border / mouse-edge resize cannot start. SetWindowPos
	// is not a mouse interaction and is never intercepted (it is exactly what the
	// working right-drag MOVE uses), so it resizes cleanly. #61-bracketed so the
	// weaver phase-snaps to lenticular-aligned pixels across the size change.
	HWND hwnd = managed_window_hwnd();
	if (hwnd == NULL)
		return;
	if (width  < DXR_MIN_WINDOW_PX) width  = DXR_MIN_WINDOW_PX;
	if (height < DXR_MIN_WINDOW_PX) height = DXR_MIN_WINDOW_PX;
	SendMessageW(hwnd, WM_ENTERSIZEMOVE, 0, 0);
	SetWindowPos(hwnd, NULL, 0, 0, width, height,
	             SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
	SendMessageW(hwnd, WM_EXITSIZEMOVE, 0, 0);
	displayxr_log("[DisplayXR] resize_overlay -> %dx%d\n", width, height);
}

void
displayxr_get_overlay_position(int *x, int *y)
{
	// Screen-space top-left of the managed window. Pairs with
	// displayxr_set_overlay_position for app-side window-position persistence.
	if (x) *x = 0;
	if (y) *y = 0;
	HWND hwnd = managed_window_hwnd();
	if (hwnd == NULL)
		return;
	RECT r;
	if (GetWindowRect(hwnd, &r)) {
		if (x) *x = (int)r.left;
		if (y) *y = (int)r.top;
	}
}

void
displayxr_set_overlay_position(int x, int y)
{
	// Move the managed window to a screen-space top-left (size unchanged).
	// #61-bracketed like resize/drag so the weaver phase-snaps. App drives this
	// to restore a remembered window position at launch.
	HWND hwnd = managed_window_hwnd();
	if (hwnd == NULL)
		return;
	SendMessageW(hwnd, WM_ENTERSIZEMOVE, 0, 0);
	SetWindowPos(hwnd, NULL, x, y, 0, 0,
	             SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
	SendMessageW(hwnd, WM_EXITSIZEMOVE, 0, 0);
	displayxr_log("[DisplayXR] set_overlay_position -> (%d,%d)\n", x, y);
}

// ============================================================================
// Shell mode: IAT hooks + window subclass for background input
//
// When the shell's compositor window has OS foreground, Unity stops processing
// input. We fix this with:
//
// 1. IAT hooks on GetForegroundWindow/GetFocus → return Unity's HWND so
//    Application.isFocused stays true.
//
// 2. IAT hook on RegisterRawInputDevices → add RIDEV_INPUTSINK so WM_INPUT
//    (keyboard + mouse delta) keeps flowing in background.
//
// 3. Window subclass → suppress WM_ACTIVATE/WM_ACTIVATEAPP/WM_KILLFOCUS
//    deactivation messages, reclaim focus via SetFocus.
//
// 4. Virtual key state table → tracks WM_KEYDOWN/WM_KEYUP and WM_*BUTTON*
//    from shell-forwarded PostMessage. Exposed to C# via
//    displayxr_get_shell_mouse_state() for mouse button input (Unity reads
//    mouse buttons from legacy messages, not WM_INPUT usButtonFlags).
//
// Mouse position: Mouse.current.delta works via RIDEV_INPUTSINK (Raw Input
// deltas). Mouse.current.position is frozen in background (Unity limitation).
// C# uses delta directly for drag rotation in shell mode.
// ============================================================================

static HWND s_shell_unity_hwnd = NULL;
static WNDPROC s_shell_original_wndproc = NULL;
// File scope (not function-local) so displayxr_uninstall_focus_hook can clear it —
// a later re-install must be able to re-patch. See both functions below.
static int s_focus_hook_installed = 0;

// Custom message: park the window off-screen + visible (handled synchronously
// on the main UI thread by shell_subclass_proc). Posted from the render thread
// via displayxr_shell_park_offscreen — see that function and the header doc.
#define DXR_WM_PARK_OFFSCREEN (WM_APP + 0x37)
// Far-off-screen parking origin (matches the runtime's off-screen owner
// convention); negative enough to clear any monitor arrangement.
#define DXR_PARK_X (-32000)
#define DXR_PARK_Y (-32000)

// Real function pointers (saved before IAT patching)
static HWND (WINAPI *s_real_GetForegroundWindow)(void) = NULL;
static HWND (WINAPI *s_real_GetFocus)(void) = NULL;
static BOOL (WINAPI *s_real_RegisterRawInputDevices)(PCRAWINPUTDEVICE, UINT, UINT) = NULL;

// Virtual key state table — updated by WM_KEYDOWN/WM_KEYUP/WM_*BUTTON*.
// Bit 0x8000 = currently pressed, bit 0x0001 = toggled since last query.
static volatile SHORT s_vkey_state[256] = {0};

// --- IAT hook functions ---

static HWND WINAPI
hooked_GetForegroundWindow(void)
{
	if (s_shell_unity_hwnd != NULL && IsWindow(s_shell_unity_hwnd))
		return s_shell_unity_hwnd;
	return s_real_GetForegroundWindow();
}

static HWND WINAPI
hooked_GetFocus(void)
{
	if (s_shell_unity_hwnd != NULL && IsWindow(s_shell_unity_hwnd))
		return s_shell_unity_hwnd;
	return s_real_GetFocus();
}

static BOOL WINAPI
hooked_RegisterRawInputDevices(PCRAWINPUTDEVICE pRawInputDevices, UINT uiNumDevices, UINT cbSize)
{
	RAWINPUTDEVICE *devices = (RAWINPUTDEVICE *)_alloca(uiNumDevices * cbSize);
	memcpy(devices, pRawInputDevices, uiNumDevices * cbSize);

	for (UINT i = 0; i < uiNumDevices; i++) {
		if (!(devices[i].dwFlags & RIDEV_REMOVE)) {
			devices[i].dwFlags |= RIDEV_INPUTSINK;
			if (devices[i].hwndTarget == NULL)
				devices[i].hwndTarget = s_shell_unity_hwnd;
		}
	}

	return s_real_RegisterRawInputDevices(devices, uiNumDevices, cbSize);
}

// --- Window subclass ---

static LRESULT CALLBACK
shell_subclass_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	// Park off-screen + visible (render-thread request). Done here on the main
	// UI thread so the SetWindowPos/ShowWindow are synchronous and actually
	// stick — an async SetWindowPos from the render thread did not reliably
	// move the window. Keeps Unity rendering (it skips the scene render for a
	// hidden window) while the bare window stays out of sight.
	if (msg == DXR_WM_PARK_OFFSCREEN) {
		SetWindowPos(hwnd, NULL, DXR_PARK_X, DXR_PARK_Y, 0, 0,
		             SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
		ShowWindow(hwnd, SW_SHOWNA);
		return 0;
	}

	// Track key/button state from shell-forwarded PostMessage input.
	switch (msg) {
	case WM_KEYDOWN: case WM_SYSKEYDOWN:
		if (wParam < 256)
			s_vkey_state[wParam] = (SHORT)(0x8000 | 0x0001);
		break;
	case WM_KEYUP: case WM_SYSKEYUP:
		if (wParam < 256)
			s_vkey_state[wParam] = 0x0001;
		break;
	case WM_LBUTTONDOWN:
		s_vkey_state[VK_LBUTTON] = (SHORT)(0x8000 | 0x0001);
		break;
	case WM_LBUTTONUP:
		s_vkey_state[VK_LBUTTON] = 0x0001;
		break;
	case WM_RBUTTONDOWN:
		s_vkey_state[VK_RBUTTON] = (SHORT)(0x8000 | 0x0001);
		break;
	case WM_RBUTTONUP:
		s_vkey_state[VK_RBUTTON] = 0x0001;
		break;
	case WM_MBUTTONDOWN:
		s_vkey_state[VK_MBUTTON] = (SHORT)(0x8000 | 0x0001);
		break;
	case WM_MBUTTONUP:
		s_vkey_state[VK_MBUTTON] = 0x0001;
		break;
	}

	// Suppress deactivation messages so Unity stays "active".
	switch (msg) {
	case WM_ACTIVATEAPP:
		if (!wParam)
			return 0;
		break;
	case WM_ACTIVATE:
		if (LOWORD(wParam) == WA_INACTIVE) {
			return CallWindowProcW(s_shell_original_wndproc, hwnd, msg,
				MAKEWPARAM(WA_ACTIVE, HIWORD(wParam)), lParam);
		}
		break;
	case WM_KILLFOCUS: {
		// Reclaim focus on the window's thread (SetFocus is thread-local).
		static int s_reclaiming = 0;
		if (!s_reclaiming) {
			s_reclaiming = 1;
			SetFocus(hwnd);
			s_reclaiming = 0;
		}
		return 0;
	}
	case WM_NCACTIVATE:
		if (!wParam)
			return CallWindowProcW(s_shell_original_wndproc, hwnd, msg, TRUE, lParam);
		break;
	}

	// Viewport update on resize/move — mirrors parent_subclass_proc logic
	// but without overlay HWND (shell mode has no overlay).
	//
	// Skip in transparent overlay mode (#57): Unity is moved off-screen so
	// its client_origin would be (-32000,-32000); the overlay's own
	// WM_MOVE/WM_SIZE in overlay_wnd_proc pushes viewport coords instead.
	if (msg == WM_SIZE && !s_overlay_is_toplevel) {
		int w = LOWORD(lParam);
		int h = HIWORD(lParam);
		if (w > 0 && h > 0) {
			POINT client_origin = {0, 0};
			ClientToScreen(hwnd, &client_origin);
			displayxr_set_viewport_size_native(
				(uint32_t)w, (uint32_t)h,
				(int32_t)client_origin.x, (int32_t)client_origin.y);
		}
	}
	if (msg == WM_MOVE && !s_overlay_is_toplevel) {
		POINT client_origin = {0, 0};
		ClientToScreen(hwnd, &client_origin);
		DisplayXRState *state = displayxr_get_state();
		if (state->viewport_width > 0) {
			displayxr_set_viewport_size_native(
				state->viewport_width, state->viewport_height,
				(int32_t)client_origin.x, (int32_t)client_origin.y);
		}
	}

	return CallWindowProcW(s_shell_original_wndproc, hwnd, msg, wParam, lParam);
}

// Render-thread-safe request to park the window off-screen + visible.
// PostMessage just queues to the window's (main) thread, so this never blocks
// or deadlocks the caller. The actual move happens synchronously in
// shell_subclass_proc (DXR_WM_PARK_OFFSCREEN) on the main thread.
void
displayxr_shell_park_offscreen(void *unity_hwnd)
{
	HWND hwnd = (HWND)unity_hwnd;
	if (hwnd != NULL && IsWindow(hwnd))
		PostMessageW(hwnd, DXR_WM_PARK_OFFSCREEN, 0, 0);
}

// --- IAT patching infrastructure ---

static int
iat_hook(HMODULE module, const char *target_dll, void *real_func, void *hook_func, void **out_original)
{
	if (module == NULL)
		return 0;

	IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)module;
	if (dos->e_magic != IMAGE_DOS_SIGNATURE)
		return 0;

	IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((BYTE *)module + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE)
		return 0;

	DWORD import_rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
	if (import_rva == 0)
		return 0;

	IMAGE_IMPORT_DESCRIPTOR *import_desc = (IMAGE_IMPORT_DESCRIPTOR *)((BYTE *)module + import_rva);
	int patched = 0;

	for (; import_desc->Name != 0; import_desc++) {
		const char *dll_name = (const char *)((BYTE *)module + import_desc->Name);
		if (_stricmp(dll_name, target_dll) != 0)
			continue;

		IMAGE_THUNK_DATA *thunk = (IMAGE_THUNK_DATA *)((BYTE *)module + import_desc->FirstThunk);
		for (; thunk->u1.Function != 0; thunk++) {
			if ((void *)(uintptr_t)thunk->u1.Function == real_func) {
				DWORD old_protect;
				if (VirtualProtect(&thunk->u1.Function, sizeof(void *), PAGE_READWRITE, &old_protect)) {
					if (out_original)
						*out_original = (void *)(uintptr_t)thunk->u1.Function;
					thunk->u1.Function = (ULONGLONG)(uintptr_t)hook_func;
					VirtualProtect(&thunk->u1.Function, sizeof(void *), old_protect, &old_protect);
					patched++;
				}
			}
		}
	}

	return patched;
}

// --- Hook installation ---

int
displayxr_install_focus_hook(void *unity_hwnd)
{
	if (unity_hwnd == NULL)
		return 0;

	// Idempotent — second call would re-subclass the wndproc and recurse
	// infinitely (s_shell_original_wndproc would point at our own subclass
	// proc). Both shell mode and transparent overlay (#57) install this for
	// the same reason: Unity needs to receive input while not OS-foreground.
	if (s_focus_hook_installed) {
		s_shell_unity_hwnd = (HWND)unity_hwnd;
		return 1;
	}
	s_focus_hook_installed = 1;

	s_shell_unity_hwnd = (HWND)unity_hwnd;

	HMODULE user32 = GetModuleHandleA("user32.dll");
	if (user32 == NULL)
		return 0;

	HMODULE exe = GetModuleHandleW(NULL);
	HMODULE unity_player = GetModuleHandleA("UnityPlayer.dll");
	int count = 0;

	// Hook GetForegroundWindow — Application.isFocused returns true
	void *real_fg = (void *)GetProcAddress(user32, "GetForegroundWindow");
	if (real_fg != NULL) {
		s_real_GetForegroundWindow = (HWND (WINAPI *)(void))real_fg;
		count += iat_hook(exe, "user32.dll", real_fg, (void *)hooked_GetForegroundWindow, NULL);
		if (unity_player != NULL)
			count += iat_hook(unity_player, "user32.dll", real_fg, (void *)hooked_GetForegroundWindow, NULL);
	}

	// Hook GetFocus — keyboard focus queries return Unity's HWND
	void *real_focus = (void *)GetProcAddress(user32, "GetFocus");
	if (real_focus != NULL) {
		s_real_GetFocus = (HWND (WINAPI *)(void))real_focus;
		count += iat_hook(exe, "user32.dll", real_focus, (void *)hooked_GetFocus, NULL);
		if (unity_player != NULL)
			count += iat_hook(unity_player, "user32.dll", real_focus, (void *)hooked_GetFocus, NULL);
	}

	// Hook RegisterRawInputDevices — add RIDEV_INPUTSINK for background WM_INPUT
	void *real_rawinput = (void *)GetProcAddress(user32, "RegisterRawInputDevices");
	if (real_rawinput != NULL) {
		s_real_RegisterRawInputDevices = (BOOL (WINAPI *)(PCRAWINPUTDEVICE, UINT, UINT))real_rawinput;
		count += iat_hook(exe, "user32.dll", real_rawinput, (void *)hooked_RegisterRawInputDevices, NULL);
		if (unity_player != NULL)
			count += iat_hook(unity_player, "user32.dll", real_rawinput, (void *)hooked_RegisterRawInputDevices, NULL);
	}

	// Re-register existing raw input devices with RIDEV_INPUTSINK.
	// Unity may have already called RegisterRawInputDevices before our hook.
	{
		UINT num_devices = 0;
		GetRegisteredRawInputDevices(NULL, &num_devices, sizeof(RAWINPUTDEVICE));
		if (num_devices > 0) {
			RAWINPUTDEVICE *devices = (RAWINPUTDEVICE *)_alloca(num_devices * sizeof(RAWINPUTDEVICE));
			if (GetRegisteredRawInputDevices(devices, &num_devices, sizeof(RAWINPUTDEVICE))) {
				for (UINT i = 0; i < num_devices; i++) {
					if (!(devices[i].dwFlags & RIDEV_REMOVE)) {
						devices[i].dwFlags |= RIDEV_INPUTSINK;
						if (devices[i].hwndTarget == NULL)
							devices[i].hwndTarget = s_shell_unity_hwnd;
					}
				}
				s_real_RegisterRawInputDevices(devices, num_devices, sizeof(RAWINPUTDEVICE));
				displayxr_log("[DisplayXR] Shell mode: re-registered %u raw input devices with RIDEV_INPUTSINK\n", num_devices);
			}
		}
	}

	// Subclass Unity's window to suppress deactivation + track key/button state
	s_shell_original_wndproc = (WNDPROC)SetWindowLongPtrW(
		(HWND)unity_hwnd, GWLP_WNDPROC, (LONG_PTR)shell_subclass_proc);

	displayxr_log("[DisplayXR] Shell mode: installed hooks (patched %d IAT entries, subclassed HWND %p)\n",
	        count, unity_hwnd);

	return count > 0;
}

// (#256) Reverse displayxr_install_focus_hook. Needed on the app-overlay teardown
// path: with no session there is no overlay for Unity to be "behind", so the lies
// this hook tells (GetForegroundWindow/GetFocus always answer Unity, WM_ACTIVATE
// deactivation suppressed, WM_KILLFOCUS reclaimed) become active harm — an ordinary
// 2D window that can never lose focus.
//
// iat_hook matches on the value currently in the thunk, so passing (hook, real)
// swaps the patch back. The wndproc restore is guarded on shell_subclass_proc still
// being the outermost proc: anything else means a subclass was installed after ours
// and unwinding here would break its chain.
void
displayxr_uninstall_focus_hook(void)
{
	if (!s_focus_hook_installed)
		return;
	s_focus_hook_installed = 0;

	HWND hwnd = s_shell_unity_hwnd;
	if (s_shell_original_wndproc != NULL && hwnd != NULL && IsWindow(hwnd)) {
		WNDPROC cur = (WNDPROC)GetWindowLongPtrW(hwnd, GWLP_WNDPROC);
		if (cur == shell_subclass_proc)
			SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)s_shell_original_wndproc);
		else
			displayxr_log("[DisplayXR] focus hook: Unity WndProc is not ours (%p) — "
			              "leaving the chain alone\n", (void *)cur);
	}
	s_shell_original_wndproc = NULL;

	HMODULE exe = GetModuleHandleW(NULL);
	HMODULE unity_player = GetModuleHandleA("UnityPlayer.dll");
	int count = 0;
	if (s_real_GetForegroundWindow != NULL) {
		count += iat_hook(exe, "user32.dll", (void *)hooked_GetForegroundWindow,
		                  (void *)s_real_GetForegroundWindow, NULL);
		count += iat_hook(unity_player, "user32.dll", (void *)hooked_GetForegroundWindow,
		                  (void *)s_real_GetForegroundWindow, NULL);
	}
	if (s_real_GetFocus != NULL) {
		count += iat_hook(exe, "user32.dll", (void *)hooked_GetFocus,
		                  (void *)s_real_GetFocus, NULL);
		count += iat_hook(unity_player, "user32.dll", (void *)hooked_GetFocus,
		                  (void *)s_real_GetFocus, NULL);
	}
	if (s_real_RegisterRawInputDevices != NULL) {
		count += iat_hook(exe, "user32.dll", (void *)hooked_RegisterRawInputDevices,
		                  (void *)s_real_RegisterRawInputDevices, NULL);
		count += iat_hook(unity_player, "user32.dll", (void *)hooked_RegisterRawInputDevices,
		                  (void *)s_real_RegisterRawInputDevices, NULL);
	}

	// Keep the real pointers: the hooked_* thunks may still be executing on
	// another thread as we unpatch, and they dereference these.
	s_shell_unity_hwnd = NULL;
	displayxr_log("[DisplayXR] Uninstalled focus/raw-input hooks (restored %d IAT entries)\n",
	              count);
}

// --- Shell mouse state for C# ---

void
displayxr_get_shell_mouse_state(int *buttons, int *mouseX, int *mouseY)
{
	if (buttons) {
		int b = 0;
		if (s_vkey_state[VK_LBUTTON] & 0x8000) b |= 1;
		if (s_vkey_state[VK_RBUTTON] & 0x8000) b |= 2;
		if (s_vkey_state[VK_MBUTTON] & 0x8000) b |= 4;
		*buttons = b;
	}
	if (mouseX) *mouseX = 0;
	if (mouseY) *mouseY = 0;
}

#endif // _WIN32
