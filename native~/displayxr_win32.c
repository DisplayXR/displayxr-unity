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
#include <stdio.h>
#include <stdlib.h>
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

// Left-click capture state. When WM_NCHITTEST returns HTCLIENT (cursor
// over tiger silhouette per s_hit_active) and the user presses LMB, we
// SetCapture(overlay) so subsequent WM_MOUSEMOVE / WM_LBUTTONUP keep
// flowing to us even if the cursor leaves the silhouette mid-drag.
// Without capture, the per-pixel WM_NCHITTEST routing would silently
// reroute mid-gesture to whatever desktop window is at the new cursor
// position, breaking the drag. ReleaseCapture happens on WM_LBUTTONUP
// (or WM_CAPTURECHANGED for safety).
static int   s_left_capture_active = 0;

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
			// Transparent overlay: per-pixel native routing via
			// WM_NCHITTEST.
			//
			//   s_hit_active=1 (cursor on tiger silhouette, per C#
			//   per-pixel raycast each LateUpdate) → HTCLIENT.
			//   Overlay catches WM_LBUTTON*/WM_MOUSEMOVE/WM_MOUSEWHEEL
			//   and posts to Unity.
			//
			//   s_hit_active=0 (transparent zone) → HTTRANSPARENT.
			//   OS recurses to find the next window beneath us at
			//   this exact pixel and dispatches input there with
			//   full native fidelity: real DefWindowProc modal
			//   SC_MOVE/SC_SIZE/SC_CLOSE loops (proper GetKeyState
			//   semantics), native click-to-activate, native cursor
			//   adaptation over resize edges, native menu activation,
			//   native hover and TrackMouseEvent, native taskbar
			//   thumbnail previews and tooltips. Unity is moved off-
			//   screen at (-32000,-32000) in displayxr_set_transparent_
			//   overlay so it is NOT in the z-order beneath us;
			//   input reaches the actual desktop window underneath.
			//
			// Per-pixel HTTRANSPARENT does NOT affect rendering — it
			// only routes hit-testing. The cube continues to render
			// via DComp + NOREDIRECTIONBITMAP per-pixel alpha. Unlike
			// the whole-window WS_EX_TRANSPARENT flag (which requires
			// WS_EX_LAYERED and conflicts with NOREDIRECTIONBITMAP),
			// WM_NCHITTEST per-pixel routing is mechanism-independent
			// of the window's compositing config.
			//
			// During an active left-drag (SetCapture by overlay on
			// WM_LBUTTONDOWN), WM_NCHITTEST is NOT called by the OS —
			// captured input bypasses hit-testing. So drag continues
			// reliably even when cursor leaves the tiger silhouette.
			return s_hit_active ? HTCLIENT : HTTRANSPARENT;
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
		GetCursorPos(&s_drag_anchor_screen);
		RECT wr;
		GetWindowRect(hwnd, &wr);
		s_drag_anchor_window.x = wr.left;
		s_drag_anchor_window.y = wr.top;
		s_drag_active = 1;
		SetCapture(hwnd);
		// #61: synchronous bracketing so the SR SDK weaver's WndProc
		// subclass sees the in-drag flag and phase-snaps the window
		// to lenticular-aligned pixels. Must precede the first
		// SetWindowPos in WM_MOUSEMOVE.
		SendMessageW(hwnd, WM_ENTERSIZEMOVE, 0, 0);
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
		// Also clear left-click capture if some other window stole it,
		// so the next WM_LBUTTONDOWN re-captures cleanly.
		s_left_capture_active = 0;
		break;
	}

	// ----- mouse-move: drag overlay if right-drag, else forward to Unity -----
	// In transparent mode (s_overlay_is_toplevel), we only receive
	// WM_MOUSEMOVE when (a) WM_NCHITTEST returned HTCLIENT — cursor over
	// tiger silhouette — OR (b) we have SetCapture (left-drag in
	// progress; OS bypasses hit-testing for captured input). Hover in
	// transparent zones (HTTRANSPARENT) is routed natively by the OS to
	// the desktop app underneath, so we never see those — desktop
	// hover effects / tooltips / cursor adaptation / taskbar previews
	// all fire on the real recipient without plugin involvement.
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
		// Forward hover to Unity for cube hover-detection / cursor-state
		// polling. During an active left-drag (s_left_capture_active),
		// this also delivers the drag motion to Unity even when the
		// cursor leaves the tiger silhouette — SetCapture keeps events
		// flowing to us regardless of HTCLIENT/HTTRANSPARENT result.
		HWND unity_hover = find_unity_hwnd();
		if (unity_hover != NULL)
			PostMessageW(unity_hover, msg, wParam, lParam);
		return 0;
	}
	case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: {
		// We received this because WM_NCHITTEST returned HTCLIENT —
		// cursor was over tiger silhouette at click time. Take capture
		// so subsequent mouse events (move, up) keep flowing to us
		// even if the cursor leaves the silhouette mid-drag. Without
		// capture, WM_NCHITTEST would re-route past us as soon as the
		// cursor crosses out of the per-pixel hit zone, and the drag
		// would be lost. ReleaseCapture happens on WM_LBUTTONUP /
		// WM_CAPTURECHANGED.
		if (!s_drag_active && !s_left_capture_active) {
			s_left_capture_active = 1;
			SetCapture(hwnd);
		}
		// Claim OS foreground so keyboard goes to our process (Unity
		// via INPUTSINK).
		SetForegroundWindow(hwnd);
		HWND unity = find_unity_hwnd();
		if (unity != NULL)
			PostMessageW(unity, msg, wParam, lParam);
		return 0;
	}
	case WM_LBUTTONUP: {
		if (s_left_capture_active) {
			s_left_capture_active = 0;
			ReleaseCapture();
		}
		HWND unity = find_unity_hwnd();
		if (unity != NULL)
			PostMessageW(unity, msg, wParam, lParam);
		return 0;
	}
	case WM_MBUTTONDOWN: case WM_MBUTTONUP: {
		// Middle button: forward to Unity, no capture (no drag use case yet).
		if (msg == WM_MBUTTONDOWN)
			SetForegroundWindow(hwnd);
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
			if (s_overlay_hwnd != NULL && IsWindow(s_overlay_hwnd)) {
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

// Set the rectangular hit-test region of the overlay (opaque WS_CHILD
// mode only). WM_NCHITTEST in overlay_wnd_proc / parent_subclass_proc
// uses s_hit_rect as a fast HTCLIENT-vs-HTTRANSPARENT discriminator.
//
// In transparent WS_POPUP + NOREDIRECTIONBITMAP mode (#57), routing is
// owned entirely by WM_NCHITTEST per-pixel: HTCLIENT when s_hit_active
// (cursor on tiger silhouette, per the C# per-pixel raycast each
// LateUpdate), HTTRANSPARENT elsewhere. The OS handles native click-
// through with full fidelity in HTTRANSPARENT zones (real
// DefWindowProc modal loops, native cursor adaptation, native menu
// activation, native hover, taskbar previews, tooltips). No
// SetWindowRgn needed — s_hit_rect is unused in transparent mode.
void
displayxr_set_overlay_hit_rect(int x, int y, int w, int h)
{
	s_hit_rect.left   = x;
	s_hit_rect.top    = y;
	s_hit_rect.right  = x + w;
	s_hit_rect.bottom = y + h;
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
