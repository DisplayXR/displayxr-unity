// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Shared native glue re-homed out of the deleted OpenXR-hook TU
// (displayxr_hooks.cpp) so the provider-only DLL links: logger,
// real-fn-pointer storage (inert in provider mode), win32 window-binding
// injector, viewport/canvas-rect accessors.

#include <cstdlib>   // calloc/free — explicit; macOS clang/libc++ no longer pulls these in transitively
#include <cstring>   // strcmp — same reason
#include "displayxr_backend.h"

// --- Logging helper ---
// On Windows built apps, fprintf(stderr) goes nowhere (no console).
// Write to a file at an absolute path so logs are always findable.
//
// Path resolution order on Windows:
//   1. <ExeDir>\displayxr.log — preferred (alongside the .exe)
//   2. %TEMP%\displayxr.log   — fallback if the .exe dir is not writable
//   3. .\displayxr.log        — last resort (CWD); historical behavior
//
// The chosen path is reported once via OutputDebugStringA (DbgView) and
// also written as the first line of the log so it's discoverable post-hoc.
void displayxr_log(const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
#if defined(_WIN32)
	static FILE *s_logfile = nullptr;
	static int s_log_init = 0;
	if (!s_log_init) {
		s_log_init = 1;
		char log_path[MAX_PATH * 2] = {};

		// (1) <ExeDir>\displayxr.log. GetModuleFileNameA(NULL, ...) returns
		// the .exe path even when called from a plugin DLL.
		char exe_path[MAX_PATH] = {};
		DWORD got = GetModuleFileNameA(NULL, exe_path, MAX_PATH);
		if (got > 0 && got < MAX_PATH) {
			for (DWORD i = got; i > 0; i--) {
				if (exe_path[i - 1] == '\\' || exe_path[i - 1] == '/') {
					exe_path[i] = 0;
					break;
				}
			}
			snprintf(log_path, sizeof(log_path), "%sdisplayxr.log", exe_path);
			s_logfile = fopen(log_path, "w");
		}

		// (2) %TEMP%\displayxr.log
		if (!s_logfile) {
			char tmp_path[MAX_PATH] = {};
			DWORD t = GetTempPathA(MAX_PATH, tmp_path);
			if (t > 0 && t < MAX_PATH - 16) {
				snprintf(log_path, sizeof(log_path), "%sdisplayxr.log", tmp_path);
				s_logfile = fopen(log_path, "w");
			}
		}

		// (3) CWD fallback (Unity's CWD is not guaranteed to be writable
		// or to match the .exe directory; kept for parity with old behavior).
		if (!s_logfile) {
			snprintf(log_path, sizeof(log_path), "displayxr.log");
			s_logfile = fopen(log_path, "w");
		}

		// Announce the chosen path so users can find the file. Always emit
		// to OutputDebugString (visible in DbgView even if file open failed)
		// and to the log itself when open.
		char marker[MAX_PATH + 64];
		snprintf(marker, sizeof(marker),
		         "[DisplayXR] log_path=%s (opened=%d)\n",
		         log_path, s_logfile != nullptr ? 1 : 0);
		OutputDebugStringA(marker);
		if (s_logfile) {
			fputs(marker, s_logfile);
			fflush(s_logfile);
		}
	}
	// (#131) Coarse relative timestamp on every line so startup hitches can be
	// localized in the log (e.g. the gap between "xrCreateSession succeeded"
	// and the first "xrEndFrame" quantifies any first-frame weaver warmup).
	// Captured once on the first call; GetTickCount64 (~15ms granularity) is
	// plenty for spotting multi-frame freezes.
	static unsigned long long s_log_t0 = 0;
	if (s_log_t0 == 0)
		s_log_t0 = GetTickCount64();
	char ts[24];
	snprintf(ts, sizeof(ts), "[+%llums] ",
	         (unsigned long long)(GetTickCount64() - s_log_t0));

	if (s_logfile) {
		fputs(ts, s_logfile);
		va_list args2;
		va_copy(args2, args);
		vfprintf(s_logfile, fmt, args2);
		fflush(s_logfile);
		va_end(args2);
	}
	// Also OutputDebugString for Visual Studio / DbgView
	char buf[2048];
	int tn = snprintf(buf, sizeof(buf), "%s", ts);
	if (tn < 0) tn = 0;
	vsnprintf(buf + tn, sizeof(buf) - (size_t)tn, fmt, args);
	OutputDebugStringA(buf);
#else
	vfprintf(stderr, fmt, args);
#endif
	va_end(args);
}

// --- Stored real function pointers (non-static — backends access via extern) ---
// Populated only by the deleted OpenXR-hook path, so in provider mode they stay
// nullptr — that is intended (the hooked-swapchain code paths in backends/wsui/
// local2d become inert in provider mode).
PFN_xrGetInstanceProcAddr s_next_gipa = nullptr;
PFN_xrLocateViews s_real_locate_views = nullptr;
PFN_xrGetSystemProperties s_real_get_system_properties = nullptr;
PFN_xrCreateSession s_real_create_session = nullptr;
PFN_xrDestroySession s_real_destroy_session = nullptr;
PFN_xrEndFrame s_real_end_frame = nullptr;
PFN_xrCreateReferenceSpace s_real_create_reference_space = nullptr;
PFN_xrLocateSpace s_real_locate_space = nullptr; // URP head-pose comp (#115)
volatile PFN_xrPollEvent s_real_poll_event = nullptr;
PFN_xrDestroyInstance s_real_destroy_instance = nullptr;

PFN_xrEnumerateViewConfigurationViews s_real_enumerate_view_configuration_views = nullptr;
PFN_xrEnumerateSwapchainFormats s_real_enumerate_swapchain_formats = nullptr;
PFN_xrCreateSwapchain s_real_create_swapchain = nullptr;
PFN_xrEnumerateSwapchainImages s_real_enumerate_swapchain_images = nullptr;
PFN_xrAcquireSwapchainImage s_real_acquire_swapchain_image = nullptr;
PFN_xrWaitSwapchainImage s_real_wait_swapchain_image = nullptr;
PFN_xrReleaseSwapchainImage s_real_release_swapchain_image = nullptr;
#if defined(_WIN32)
PFN_xrDestroySwapchain s_real_destroy_swapchain = nullptr;
#endif

// Win32 window binding helper (shared with D3D11Backend / D3D12Backend)
#if defined(_WIN32)
void win32_inject_window_binding(XrBaseOutStructure *last, DisplayXRState *state)
{
	static XrWin32WindowBindingCreateInfoEXT win_binding = {};
	win_binding.type = XR_TYPE_WIN32_WINDOW_BINDING_CREATE_INFO_EXT;
	win_binding.next = nullptr;
	win_binding.windowHandle = state->window_handle;
	win_binding.readbackCallback = displayxr_readback_callback;
	win_binding.readbackUserdata = nullptr;
	win_binding.sharedTextureHandle = nullptr;
	// runtime-pvt #191 / displayxr-unity#57: opt-in BitBlt (D3D11) / DComp
	// (D3D12) swapchain. Only meaningful with a real HWND and outside shell
	// mode.
	win_binding.transparentBackgroundEnabled =
	    (state->transparent_background_requested
	     && state->window_handle != nullptr
	     && !displayxr_is_shell_mode())
	    ? XR_TRUE : XR_FALSE;
	// Spec v5 chromaKeyColor: post-weave chroma-key conversion is disabled
	// (runtime uses the compose-under-bg + alpha-gate DP path instead;
	// Unity emits per-pixel alpha via ALPHA_BLEND environment blend mode).
	win_binding.chromaKeyColor = 0;
	displayxr_log("[DisplayXR] Injecting win32 window binding: windowHandle=%p, sharedTextureHandle=%p, transparentBackgroundEnabled=%d, chromaKeyColor=0 (alpha-native)\n",
	              win_binding.windowHandle, win_binding.sharedTextureHandle,
	              (int)win_binding.transparentBackgroundEnabled);
	last->next = (XrBaseOutStructure *)&win_binding;
}
#endif

// --- Viewport size (native WM_SIZE authority) ---
// s_native_viewport_active was written by the C# push path in the deleted
// hook TU (displayxr_set_viewport_size) and set here by the native WM_SIZE
// path. In provider mode only the native writer survives; win32.c calls this.
static int s_native_viewport_active = 0; // WM_SIZE (native) is authoritative source

void
displayxr_set_viewport_size_native(uint32_t width, uint32_t height,
                                   int32_t screen_x, int32_t screen_y)
{
	s_native_viewport_active = 1;
	DisplayXRState *state = displayxr_get_state();
	state->viewport_width = width;
	state->viewport_height = height;
	state->viewport_x = screen_x;
	state->viewport_y = screen_y;
}

// Canvas sub-rect (#34 / #131): the only WRITER (displayxr_set_canvas_rect)
// died with the hook TU, so in provider mode these stay zero → the accessor
// returns 0 ("full-window canvas"), which is the correct provider-mode default
// (DisplayXRTransparentOverlay/win32.c treat 0 as full window).
static int s_canvas_rect_valid = 0;
static int32_t s_canvas_rect_x = 0, s_canvas_rect_y = 0;
static uint32_t s_canvas_rect_w = 0, s_canvas_rect_h = 0;

DISPLAYXR_EXPORT int
displayxr_get_canvas_rect_px(int32_t *x, int32_t *y, uint32_t *w, uint32_t *h)
{
	// Report the active canvas sub-rect (#34/#131) in HWND-client pixels so the
	// Win32 overlay can scale+offset the click-through silhouette into the same
	// sub-rect the runtime weaves the 3D into. Returns 0 when no sub-rect is set
	// (full-window canvas → overlay maps the silhouette to the whole client).
	if (!s_canvas_rect_valid)
		return 0;
	if (x) *x = s_canvas_rect_x;
	if (y) *y = s_canvas_rect_y;
	if (w) *w = s_canvas_rect_w;
	if (h) *h = s_canvas_rect_h;
	return 1;
}
