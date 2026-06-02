// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
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
#include "displayxr_hooks.h"
#include "displayxr_shared_state.h"

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
static const wchar_t OVERLAY_CLASS_NAME[] = L"DisplayXROverlay";
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
// Shell mode detection
// ============================================================================

static int s_shell_checked = 0;
static int s_shell_mode = 0;

int
displayxr_is_shell_mode(void)
{
	if (!s_shell_checked) {
		const char *val = getenv("DISPLAYXR_SHELL_SESSION");
		s_shell_mode = (val != NULL && val[0] == '1' && val[1] == '\0');
		s_shell_checked = 1;
	}
	return s_shell_mode;
}

// ============================================================================
// Window discovery (shared by both modes)
// ============================================================================

// Skip our own overlay window (created by displayxr_get_app_main_view) when
// searching for Unity's main HWND. After the overlay becomes visible it can
// otherwise win the foreground/visible-window race in find_unity_hwnd and
// we end up styling our own overlay as Unity, leaving Unity untouched.
static int
is_displayxr_overlay_class(HWND hwnd)
{
	wchar_t cls[64] = {0};
	if (GetClassNameW(hwnd, cls, 63) == 0)
		return 0;
	return wcscmp(cls, OVERLAY_CLASS_NAME) == 0;
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

	switch (msg) {
	case WM_NCHITTEST: {
		if (s_overlay_is_toplevel) {
			// Transparent overlay: WS_EX_TRANSPARENT toggling in
			// displayxr_set_overlay_hit_active drives the OS hit-
			// test routing. When WS_EX_TRANSPARENT is ON, the OS
			// skips us in WindowFromPoint and never calls our
			// WM_NCHITTEST. When it's OFF (cursor over the cube),
			// the OS calls us — return HTCLIENT to claim the click.
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
		// Belt-and-braces with WS_EX_NOACTIVATE: refuse activation on
		// click so foreground stays on Unity (cloaked but active).
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
		int was_active = s_drag_active;
		s_drag_active = 0;
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
	if (msg == WM_SIZE && s_overlay_hwnd != NULL && IsWindow(s_overlay_hwnd)) {
		int w = LOWORD(lParam);
		int h = HIWORD(lParam);
		if (s_overlay_is_toplevel) {
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
		// In opaque (WS_CHILD) mode, the overlay follows Unity 1:1 and we
		// also push viewport_size_native from Unity's client_origin.
		if (!s_overlay_is_toplevel) {
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

	POINT client_origin = {0, 0};
	ClientToScreen(unity_hwnd, &client_origin);

	DWORD style    = transparent_mode
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
	DWORD ex_style = transparent_mode
	    ? (DWORD)(WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)
	    : (DWORD)(WS_EX_TRANSPARENT);
	int x = transparent_mode ? client_origin.x : 0;
	int y = transparent_mode ? client_origin.y : 0;

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

	s_overlay_is_toplevel = transparent_mode;

	s_original_wndproc = (WNDPROC)SetWindowLongPtrW(
	    unity_hwnd, GWLP_WNDPROC, (LONG_PTR)parent_subclass_proc);

	displayxr_log("[DisplayXR] Created overlay HWND (%dx%d at %d,%d) on Unity window %p — %s\n",
	              w, h, x, y, (void *)unity_hwnd,
	              transparent_mode ? "TOP-LEVEL WS_POPUP + NOREDIRECTIONBITMAP (transparent)" : "WS_CHILD (opaque)");

	displayxr_set_viewport_size_native(
		(uint32_t)w, (uint32_t)h,
		(int32_t)client_origin.x, (int32_t)client_origin.y);

	return (void *)s_overlay_hwnd;
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

void
displayxr_set_overlay_hit_rect(int x, int y, int w, int h)
{
	s_hit_rect.left   = x;
	s_hit_rect.top    = y;
	s_hit_rect.right  = x + w;
	s_hit_rect.bottom = y + h;

	if (!s_overlay_is_toplevel)
		return; // opaque WS_CHILD: WM_NCHITTEST consumes s_hit_rect, done.
	if (s_overlay_hwnd == NULL || !IsWindow(s_overlay_hwnd))
		return;
	// Per-pixel silhouette mask path is active: stop driving the
	// region from the AABB. Mask owns SetWindowRgn from here on.
	if (s_hit_mask_active)
		return;

	// Cache of last applied region rect (post-padding). Sentinel value
	// {INT_MIN,...} so the first call always goes through. Also reset
	// to sentinel when we clear the region (w<=0 path below).
	static RECT s_last_rgn_rect = { INT_MIN, INT_MIN, INT_MIN, INT_MIN };

	if (w <= 0 || h <= 0) {
		SetWindowRgn(s_overlay_hwnd, NULL, TRUE);
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
	if (!SetWindowRgn(s_overlay_hwnd, rgn, TRUE)) {
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
		displayxr_log("[DisplayXR] hit_region: raw=(%d,%d %dx%d) padded=(%ld,%ld %ldx%ld) on overlay=%p\n",
		              x, y, w, h,
		              padded.left, padded.top,
		              padded.right - padded.left, padded.bottom - padded.top,
		              (void *)s_overlay_hwnd);
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
void
displayxr_set_overlay_hit_mask(const uint8_t *mask, int mask_w, int mask_h,
                               int dst_w, int dst_h)
{
	if (!s_overlay_is_toplevel)
		return;
	if (s_overlay_hwnd == NULL || !IsWindow(s_overlay_hwnd))
		return;

	if (mask == NULL || mask_w <= 0 || mask_h <= 0 ||
	    dst_w <= 0 || dst_h <= 0) {
		// NULL/empty: clear and revert to AABB-region path.
		SetWindowRgn(s_overlay_hwnd, NULL, TRUE);
		s_hit_mask_active = 0;
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

	// (#131) When a canvas sub-rect is active, the runtime shrinks the 3D weave
	// into that sub-rect, so the displayed silhouette lives there too. Map the
	// full-window mask into the sub-rect (same scale+offset the weave applies)
	// instead of the full overlay client, so the click-through region matches the
	// shrunk tiger. No sub-rect → leaves origin (0,0)/extent (dst_w,dst_h), i.e.
	// the legacy whole-client mapping.
	int32_t  cvx = 0, cvy = 0;
	uint32_t cvw = (uint32_t)dst_w, cvh = (uint32_t)dst_h;
	displayxr_get_canvas_rect_px(&cvx, &cvy, &cvw, &cvh);

	// Round each rect's edges OUTWARD when mapping mask cells → target pixels:
	// leading edges (left/top) floor, trailing edges (right/bottom) ceil. A mask
	// cell covers cvw/mask_w (cvh/mask_h) target px; with the canvas sub-rect this
	// can be several px per cell, and plain truncation on the trailing edges would
	// shrink the catch-region inside the silhouette and clip the tiger's edges.
	// Outward rounding dilates by <1 cell so the region fully covers the silhouette.
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

	if (!SetWindowRgn(s_overlay_hwnd, rgn, TRUE)) {
		DeleteObject(rgn);
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
		displayxr_log("[DisplayXR] hit_mask: mask=%dx%d rects=%d dst=%dx%d\n",
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

void
displayxr_get_overlay_size(int *width, int *height)
{
	if (s_overlay_hwnd != NULL && IsWindow(s_overlay_hwnd)) {
		RECT rc;
		GetClientRect(s_overlay_hwnd, &rc);
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
		// s_vkey_state is updated by shell_subclass_proc on Unity's HWND
		// (installed via displayxr_install_focus_hook from the transparent
		// path). PostMessage'd left/right/middle clicks from overlay_wnd_proc
		// flow through Unity's wndproc → subclass → here.
		int b = 0;
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
	static int s_focus_hook_installed = 0;
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
