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
#include <stdio.h>
#include <stdlib.h>
#include "displayxr_hooks.h"
#include "displayxr_shared_state.h"

// ============================================================================
// Standalone mode: overlay child window
// ============================================================================

static HWND s_overlay_hwnd = NULL;
static WNDPROC s_original_wndproc = NULL;
static const wchar_t OVERLAY_CLASS_NAME[] = L"DisplayXROverlay";
static int s_class_registered = 0;

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

static HWND
find_unity_hwnd(void)
{
	DWORD our_pid = GetCurrentProcessId();
	HWND unity_hwnd = NULL;

	HWND fg = GetForegroundWindow();
	if (fg != NULL) {
		DWORD fg_pid = 0;
		GetWindowThreadProcessId(fg, &fg_pid);
		if (fg_pid == our_pid)
			unity_hwnd = fg;
	}

	if (unity_hwnd == NULL) {
		HWND hwnd = NULL;
		while ((hwnd = FindWindowExW(NULL, hwnd, NULL, NULL)) != NULL) {
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
	case WM_NCHITTEST:
		return HTTRANSPARENT;
	default:
		return DefWindowProcW(hwnd, msg, wParam, lParam);
	}
}

static LRESULT CALLBACK
parent_subclass_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	if (msg == WM_SIZE && s_overlay_hwnd != NULL && IsWindow(s_overlay_hwnd)) {
		int w = LOWORD(lParam);
		int h = HIWORD(lParam);
		SetWindowPos(s_overlay_hwnd, HWND_TOP, 0, 0, w, h, SWP_NOZORDER);
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

	s_overlay_hwnd = CreateWindowExW(
	    WS_EX_TRANSPARENT,
	    OVERLAY_CLASS_NAME,
	    L"DisplayXR Overlay",
	    WS_CHILD | WS_VISIBLE,
	    0, 0, w, h,
	    unity_hwnd,
	    NULL,
	    GetModuleHandleW(NULL),
	    NULL);

	if (s_overlay_hwnd == NULL) {
		fprintf(stderr, "[DisplayXR] Failed to create overlay window: %lu\n",
		        GetLastError());
		return NULL;
	}

	s_original_wndproc = (WNDPROC)SetWindowLongPtrW(
	    unity_hwnd, GWLP_WNDPROC, (LONG_PTR)parent_subclass_proc);

	fprintf(stderr, "[DisplayXR] Created overlay HWND (%dx%d) on Unity window %p\n",
	        w, h, (void *)unity_hwnd);

	{
		POINT client_origin = {0, 0};
		ClientToScreen(unity_hwnd, &client_origin);
		displayxr_set_viewport_size_native(
			(uint32_t)w, (uint32_t)h,
			(int32_t)client_origin.x, (int32_t)client_origin.y);
	}

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
