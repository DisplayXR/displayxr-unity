// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Shared native glue re-homed out of the deleted OpenXR-hook TU
// (displayxr_hooks.cpp) so the provider-only DLL links: logger + the C#
// P/Invoke accessors (viewport/canvas-rect, stereo matrices, display info,
// eye positions, render-target size, transparent-background request).
//
// The former real-fn-pointer storage (s_real_*/s_next_gipa) and the win32
// window-binding injector were removed in the Task-3 hook-backend cleanup
// (#166): the fn-pointers were only ever populated by the deleted hook and the
// provider does its own session/window binding.

#include <cstdlib>   // calloc/free — explicit; macOS clang/libc++ no longer pulls these in transitively
#include <cstring>   // strcmp — same reason
#include "displayxr_native_shared.h"

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

// Canvas sub-rect (#34 / #131): the app sets this to the 3D-zone rect so the
// Win32 overlay's click-through silhouette mask is stamped into the SAME sub-rect
// the runtime weaves the zone into (get_canvas_rect_px, below, is read by
// displayxr_set_overlay_hit_mask + the C# GetStereoViewport). The writer
// (displayxr_set_canvas_rect) was re-homed out of the deleted hook TU — without it
// the accessor returns 0 (full-window), the mask is stamped full-window while the
// zone is woven into the sub-rect, and the mask CLIPS the woven content (#166).
// The hook-era output-rect apply (xrSetSharedTextureOutputRectDXR) is dropped: the
// provider drives the zone weave via XrDisplayZoneDXR (dxr_prov_set_3d_zone_rect),
// so this setter only needs to update the shared canvas rect the mask path reads.
static int s_canvas_rect_valid = 0;
static int32_t s_canvas_rect_x = 0, s_canvas_rect_y = 0;
static uint32_t s_canvas_rect_w = 0, s_canvas_rect_h = 0;

DISPLAYXR_EXPORT void
displayxr_set_canvas_rect(int32_t x, int32_t y, uint32_t w, uint32_t h)
{
	// w==0 || h==0 clears (full-window canvas). Otherwise cache the sub-rect
	// so displayxr_get_canvas_rect_px reports it to the overlay mask path.
	if (w == 0 || h == 0) {
		s_canvas_rect_valid = 0;
		displayxr_log("[DisplayXR] set_canvas_rect: cleared (full-window canvas)\n");
		return;
	}
	s_canvas_rect_x = x;
	s_canvas_rect_y = y;
	s_canvas_rect_w = w;
	s_canvas_rect_h = h;
	s_canvas_rect_valid = 1;
	displayxr_log("[DisplayXR] set_canvas_rect: (%d,%d) %ux%u\n", x, y, w, h);
}

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

// --- Stereo-matrix readback (C# P/Invoke) ---
// The provider publishes the per-eye view/projection into the shared state each
// frame (displayxr_state_set_stereo_matrices, from the provider session). The URP
// KooimaProjectionFixFeature reads them back through this export to re-push the
// correct off-center projection (#127). Re-homed out of the deleted hook TU; the
// state store (displayxr_shared_state.cpp) is written by the provider now.
DISPLAYXR_EXPORT void
displayxr_get_stereo_matrices(float *left_view, float *left_proj,
                              float *right_view, float *right_proj,
                              int *valid)
{
	DisplayXRStereoMatrices mats = displayxr_state_get_stereo_matrices();
	memcpy(left_view, mats.left_view, sizeof(float) * 16);
	memcpy(left_proj, mats.left_projection, sizeof(float) * 16);
	memcpy(right_view, mats.right_view, sizeof(float) * 16);
	memcpy(right_proj, mats.right_projection, sizeof(float) * 16);
	*valid = mats.valid;
}

// --- Display-info / eye-position / render-target readback (C# P/Invoke) ---
// Thin readers of the shared DisplayXRState / win32 window. Re-homed out of the
// deleted hook TU so editor gizmos (DisplayXRGizmoHelpers) and 2D-surround sizing
// don't hit a missing entry point. They read whatever the shared state holds
// (defaults when no session has populated it), so they degrade gracefully.
DISPLAYXR_EXPORT void
displayxr_get_display_info(float *display_width_m, float *display_height_m,
                           uint32_t *pixel_width, uint32_t *pixel_height,
                           float *nominal_x, float *nominal_y, float *nominal_z,
                           float *scale_x, float *scale_y, int *is_valid)
{
	DisplayXRState *state = displayxr_get_state();
	DisplayXRDisplayInfo *di = &state->display_info;
	*display_width_m = di->display_width_meters;
	*display_height_m = di->display_height_meters;
	*pixel_width = di->display_pixel_width;
	*pixel_height = di->display_pixel_height;
	*nominal_x = di->nominal_viewer_x;
	*nominal_y = di->nominal_viewer_y;
	*nominal_z = di->nominal_viewer_z;
	*scale_x = di->recommended_view_scale_x;
	*scale_y = di->recommended_view_scale_y;
	*is_valid = di->is_valid;
}

DISPLAYXR_EXPORT void
displayxr_get_eye_positions(float *lx, float *ly, float *lz,
                            float *rx, float *ry, float *rz, int *is_tracked)
{
	DisplayXREyePositions eyes = displayxr_state_get_eye_positions();
	*lx = eyes.left_eye.x;  *ly = eyes.left_eye.y;  *lz = eyes.left_eye.z;
	*rx = eyes.right_eye.x; *ry = eyes.right_eye.y; *rz = eyes.right_eye.z;
	*is_tracked = eyes.is_tracked;
}

DISPLAYXR_EXPORT int
displayxr_get_kooima_canvas(int *rect_x, int *rect_y, int *rect_w, int *rect_h,
                            float *size_w_m, float *size_h_m)
{
	DisplayXRKooimaCanvas c = displayxr_state_get_kooima_canvas();
	if (rect_x)   *rect_x   = c.rect_x;
	if (rect_y)   *rect_y   = c.rect_y;
	if (rect_w)   *rect_w   = c.rect_w;
	if (rect_h)   *rect_h   = c.rect_h;
	if (size_w_m) *size_w_m = c.size_meters_w;
	if (size_h_m) *size_h_m = c.size_meters_h;
	return c.is_valid;
}

DISPLAYXR_EXPORT void
displayxr_get_render_target_size(uint32_t *out_w, uint32_t *out_h)
{
	uint32_t w = 0, h = 0;
#if defined(_WIN32)
	DisplayXRState *state = displayxr_get_state();
	if (state && state->window_handle) {
		RECT rc = {0, 0, 0, 0};
		if (GetClientRect((HWND)state->window_handle, &rc)) {
			w = (uint32_t)(rc.right - rc.left);
			h = (uint32_t)(rc.bottom - rc.top);
		}
	}
#endif
	if (out_w) *out_w = w;
	if (out_h) *out_h = h;
}

// --- Transparent-background request (C# P/Invoke) ---
// Sets the shared-state flag the win32 overlay reads (get_app_main_view →
// transparent_mode) to build the transparent NOREDIRECTIONBITMAP overlay and
// keep it on-screen while Unity's real HWND is cloaked/off-screen. This is
// SEPARATE from the provider's own transparent flag (dxr_prov_set_transparent_
// background, which drives ALPHA_BLEND on the provider session) — a transparent
// app sets both. Re-homed out of the deleted hook TU; without it the overlay
// builds opaque/off-screen and the woven content isn't visible (#166).
DISPLAYXR_EXPORT void
displayxr_set_transparent_background(int enabled)
{
	DisplayXRState *state = displayxr_get_state();
	state->transparent_background_requested = (uint8_t)(enabled != 0);
	displayxr_log("[DisplayXR] set_transparent_background: requested=%d\n",
	              (int)state->transparent_background_requested);
}
