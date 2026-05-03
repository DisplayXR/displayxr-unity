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

// ============================================================================
// Transparent overlay mode (issue #57): chroma-key + WS_EX_LAYERED on the
// parent (Unity top-level) HWND. Mutually exclusive with shell mode.
// ============================================================================

static DWORD s_saved_style    = 0;
static DWORD s_saved_exstyle  = 0;
static int   s_overlay_active = 0;
static RECT  s_hit_rect       = {0, 0, 0, 0};

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

static LRESULT CALLBACK
overlay_wnd_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg) {
	case WM_NCHITTEST: {
		// Per-pixel click-through: HTCLIENT inside the C#-supplied hit
		// rect (cube AABB, coarse) AND s_hit_active is set (fine, from
		// per-frame Physics.Raycast at the cursor in C#). Both gates
		// must pass.
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

	// ----- right-button: capture-based drag of the Unity window (task 2) -----
	//
	// We've tried twice to inherit opaque-mode-grade no-stutter drag UX via
	// the OS modal drag loop:
	//   1. SendMessage(unity, WM_NCLBUTTONDOWN, HTCAPTION, 0) — silently
	//      ignored. Cloaked Unity HWND or its WS_POPUP-no-WS_CAPTION style
	//      blocks DefWindowProc from starting the drag.
	//   2. SendMessage(overlay, WM_SYSCOMMAND, SC_MOVE | HTCAPTION, 0) —
	//      also silently ignored. Suspect WS_EX_NOREDIRECTIONBITMAP +
	//      WS_EX_NOACTIVATE on the overlay are incompatible with
	//      DefWindowProc's modal drag (no DWM redirection surface for the
	//      drag preview). NOREDIRECTIONBITMAP can't be dropped — it's what
	//      gives us per-pixel transparency.
	//
	// So the SR SDK weaver phase-snap stays unreachable from this side.
	// The capture-based drag below trades 3D stutter (no phase-snap) for
	// real-time Kooima updates (cube keeps animating during drag). Proper
	// fix needs the runtime "external drag" API outlined in CLAUDE.md
	// "Known Issues" — same blocker as the standalone preview window.
	case WM_RBUTTONDOWN: {
		HWND unity = find_unity_hwnd();
		if (unity != NULL) {
			GetCursorPos(&s_drag_anchor_screen);
			RECT wr;
			GetWindowRect(unity, &wr);
			s_drag_anchor_window.x = wr.left;
			s_drag_anchor_window.y = wr.top;
			s_drag_active = 1;
			SetCapture(hwnd);
		}
		return 0;
	}
	case WM_RBUTTONUP:
		if (s_drag_active) {
			ReleaseCapture();
			s_drag_active = 0;
			return 0;
		}
		break;
	case WM_CAPTURECHANGED:
		s_drag_active = 0;
		break;

	// ----- left/middle-button + mouse-move: forward to Unity (task 1) -----
	case WM_MOUSEMOVE:
		if (s_drag_active) {
			POINT cur;
			GetCursorPos(&cur);
			HWND unity = find_unity_hwnd();
			if (unity != NULL) {
				int nx = s_drag_anchor_window.x + (cur.x - s_drag_anchor_screen.x);
				int ny = s_drag_anchor_window.y + (cur.y - s_drag_anchor_screen.y);
				SetWindowPos(unity, NULL, nx, ny, 0, 0,
				             SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
			}
			return 0;
		}
		// fall through to forward
	case WM_LBUTTONDOWN: case WM_LBUTTONUP:
	case WM_LBUTTONDBLCLK:
	case WM_MBUTTONDOWN: case WM_MBUTTONUP: {
		HWND unity = find_unity_hwnd();
		if (unity != NULL)
			PostMessageW(unity, msg, wParam, lParam);
		return 0;
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
			// Top-level overlay: position in screen coords matching parent's
			// client area. Keep z-order topmost.
			POINT client_origin = {0, 0};
			ClientToScreen(hwnd, &client_origin);
			SetWindowPos(s_overlay_hwnd, HWND_TOPMOST,
			             client_origin.x, client_origin.y, w, h,
			             SWP_NOACTIVATE);
		} else {
			// Child overlay: resize within parent's client area.
			SetWindowPos(s_overlay_hwnd, HWND_TOP, 0, 0, w, h, SWP_NOZORDER);
		}
		if (w > 0 && h > 0) {
			POINT client_origin = {0, 0};
			ClientToScreen(hwnd, &client_origin);
			displayxr_set_viewport_size_native(
				(uint32_t)w, (uint32_t)h,
				(int32_t)client_origin.x, (int32_t)client_origin.y);
		}
	}
	if (msg == WM_MOVE) {
		POINT client_origin = {0, 0};
		ClientToScreen(hwnd, &client_origin);
		// Top-level overlay follows Unity's screen position.
		if (s_overlay_is_toplevel
		    && s_overlay_hwnd != NULL && IsWindow(s_overlay_hwnd)) {
			SetWindowPos(s_overlay_hwnd, HWND_TOPMOST,
			             client_origin.x, client_origin.y, 0, 0,
			             SWP_NOSIZE | SWP_NOACTIVATE);
		}
		DisplayXRState *state = displayxr_get_state();
		if (state->viewport_width > 0) {
			displayxr_set_viewport_size_native(
				state->viewport_width, state->viewport_height,
				(int32_t)client_origin.x, (int32_t)client_origin.y);
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
	// Transparent path: NO WS_EX_TRANSPARENT — that bypasses WM_NCHITTEST so
	// click-through becomes all-or-nothing. We instead gate per-pixel
	// click-through via overlay_wnd_proc's WM_NCHITTEST + s_hit_rect.
	// WS_EX_NOACTIVATE keeps activation/foreground on Unity's (cloaked) HWND
	// when the user clicks the overlay — otherwise Application.isFocused
	// flips false and Unity stops processing input.
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
// Chroma-key transparent overlay for desktop avatar use cases. The Leia weaver
// drops alpha and the D3D11 compositor uses DXGI_ALPHA_MODE_IGNORE, so true
// per-pixel alpha doesn't survive end-to-end. Workaround: app renders a magic
// color (default magenta) in transparent regions of both eye views; we flip
// the parent HWND to WS_POPUP | WS_EX_LAYERED with LWA_COLORKEY so DWM punches
// those pixels through to the desktop. Click-through outside a rectangular hit
// region is handled by returning HTTRANSPARENT from WM_NCHITTEST.
//
// In standalone mode the DisplayXR runtime presents into a child overlay HWND
// (created in displayxr_get_app_main_view) — not into Unity's top-level
// window. DXGI flip-model swap chains in a child window are composed by DWM
// independently, so a colorkey on the parent doesn't punch through the
// child's pixels. We mirror WS_EX_LAYERED + LWA_COLORKEY onto the child too;
// since Win 8, layered child windows are supported and DWM applies the
// colorkey to the child's composed swapchain content directly.
//
// Mutually exclusive with shell mode (early-out below).
// ============================================================================

void
displayxr_set_transparent_overlay(int enabled, uint32_t color_key, int topmost)
{
	displayxr_log("[DisplayXR] transparent_overlay: called enabled=%d key=0x%08X topmost=%d shell=%d\n",
	              enabled, (unsigned)color_key, topmost, displayxr_is_shell_mode());

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
		// We tried adding WS_EX_TRANSPARENT here for desktop click-through
		// (clicks falling through both overlay AND Unity to the actual
		// app behind), but it broke RawInput delivery to Unity — Unity
		// stopped seeing button events from PostMessage'd clicks too. Per
		// the log, btn=0x00 stayed throughout test even though clicks were
		// happening. Cross-process click-through stays unsolved for now;
		// transparent zone clicks still die on the cloaked Unity HWND.
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
		// NOT preserve OS-foreground status though, which is why we need
		// the focus hook below.
		{
			BOOL cloak = TRUE;
			HRESULT hr = DwmSetWindowAttribute(hwnd, DWMWA_CLOAK,
			                                   &cloak, sizeof(cloak));
			displayxr_log("[DisplayXR] Cloaked Unity main window via DWMWA_CLOAK: hr=0x%08X\n",
			              (unsigned)hr);
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

		// color_key parameter is informational only — runtime owns the
		// chroma key via the binding struct's chromaKeyColor field.
		(void)color_key;

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
	} else if (!enabled && s_overlay_active) {
		// Uncloak Unity's main window before restoring styles, so the
		// frame-changed repaint can run with a visible window.
		{
			BOOL cloak = FALSE;
			DwmSetWindowAttribute(hwnd, DWMWA_CLOAK, &cloak, sizeof(cloak));
		}
		SetWindowLongPtrW(hwnd, GWL_STYLE, (LONG_PTR)s_saved_style);
		SetWindowLongPtrW(hwnd, GWL_EXSTYLE, (LONG_PTR)s_saved_exstyle);
		SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
		             SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED);

		// Drop WS_EX_LAYERED on the child too. We didn't save its prior
		// exstyle separately because the child is created with a fixed style
		// (WS_EX_TRANSPARENT) that we know.
		if (s_overlay_hwnd != NULL && IsWindow(s_overlay_hwnd)) {
			DWORD cex = (DWORD)GetWindowLongPtrW(s_overlay_hwnd, GWL_EXSTYLE);
			SetWindowLongPtrW(s_overlay_hwnd, GWL_EXSTYLE,
			                  (LONG_PTR)(cex & ~WS_EX_LAYERED));
		}

		s_overlay_active = 0;
		displayxr_log("[DisplayXR] transparent_overlay: disabled\n");
	}
}

void
displayxr_set_overlay_hit_rect(int x, int y, int w, int h)
{
	s_hit_rect.left   = x;
	s_hit_rect.top    = y;
	s_hit_rect.right  = x + w;
	s_hit_rect.bottom = y + h;
}

void
displayxr_set_overlay_hit_active(int active)
{
	int new_active = active ? 1 : 0;
	if (new_active == s_hit_active)
		return;
	s_hit_active = new_active;

	// Toggle WS_EX_TRANSPARENT on the overlay HWND to match. With the flag
	// cleared (cursor off cube) the OS routes clicks past the overlay to
	// the next window in DWM z-order, which gives us click-through to apps
	// behind the cube area. (Note: clicks still die on the cloaked Unity
	// HWND beneath us in the current build — true cross-process click-
	// through is still unsolved; tracked for the next iteration.)
	if (s_overlay_is_toplevel && s_overlay_hwnd != NULL && IsWindow(s_overlay_hwnd)) {
		LONG_PTR ex = GetWindowLongPtrW(s_overlay_hwnd, GWL_EXSTYLE);
		LONG_PTR new_ex = s_hit_active
		    ? (ex & ~WS_EX_TRANSPARENT)   // overlay catches clicks
		    : (ex |  WS_EX_TRANSPARENT);  // OS routes around overlay
		if (new_ex != ex)
			SetWindowLongPtrW(s_overlay_hwnd, GWL_EXSTYLE, new_ex);
	}
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
	if (msg == WM_SIZE) {
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
	if (msg == WM_MOVE) {
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
