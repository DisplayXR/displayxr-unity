// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
//
// Standalone OpenXR session for editor preview.
// Loads the DisplayXR runtime directly via xrNegotiateLoaderRuntimeInterface,
// bypassing Unity's OpenXR loader entirely. This gives us full control over
// the session lifecycle — no deferred destruction, no signal handlers, no
// teardown races with Unity's Game View repaint cycle.

#include "displayxr_standalone_internal.h"
#include "displayxr_window_space_ui.h"
#include <stdarg.h>

// ============================================================================
// OpenXR loader negotiation types (from openxr_loader_negotiation.h)
// Defined inline to avoid header dependency issues.
// ============================================================================

#define XR_LOADER_INTERFACE_STRUCT_LOADER_INFO   1
#define XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST 3
#define XR_CURRENT_LOADER_RUNTIME_VERSION 1

typedef struct XrNegotiateLoaderInfo {
	uint32_t structType;
	uint32_t structVersion;
	size_t structSize;
	uint32_t minInterfaceVersion;
	uint32_t maxInterfaceVersion;
	XrVersion minApiVersion;
	XrVersion maxApiVersion;
} XrNegotiateLoaderInfo;

typedef struct XrNegotiateRuntimeRequest {
	uint32_t structType;
	uint32_t structVersion;
	size_t structSize;
	uint32_t runtimeInterfaceVersion;
	XrVersion runtimeApiVersion;
	PFN_xrGetInstanceProcAddr getInstanceProcAddr;
} XrNegotiateRuntimeRequest;

typedef XrResult (*PFN_xrNegotiateLoaderRuntimeInterface)(
    const XrNegotiateLoaderInfo *loaderInfo,
    XrNegotiateRuntimeRequest *runtimeRequest);


// ============================================================================
// Standalone session state
// ============================================================================

typedef struct StandaloneState {
	void *runtime_lib;
	PFN_xrGetInstanceProcAddr gipa;
#if defined(_WIN32)
	HWND preview_hwnd;
#endif

	XrInstance instance;
	XrSystemId system_id;
	XrSession session;
	XrSpace local_space;

	XrSessionState session_state;
	volatile int running;
	int session_ready;

	DisplayXRDisplayInfo display_info;

	float left_eye[3];
	float right_eye[3];
	int is_tracked;

	// Tunables + display pose (set from C#)
	Display3DTunables tunables;
	int tunables_set;
	int camera_centric;
	float inv_convergence_distance;
	float fov_override; // half_tan_vfov for camera-centric
	float last_near_z;  // cached from last compute_views call
	float last_far_z;
	XrPosef display_pose;
	int display_pose_set;

	// Single atlas swapchain (all views tiled into one texture)
	SASwapchain atlas;
	int atlas_created;

	// Rendering mode metadata (from xrEnumerateDisplayRenderingModesEXT)
	XrDisplayRenderingModeInfoEXT rendering_modes[SA_MAX_RENDERING_MODES];
	uint32_t rendering_mode_count;
	uint32_t current_rendering_mode_index;

	// Multi-view eye positions (from xrLocateViews)
	float eye_positions[SA_MAX_VIEWS][3];
	uint32_t located_view_count;

	// Last computed Kooima views (from compute_views, used by submit_frame_atlas)
	Display3DView computed_views[SA_MAX_VIEWS];
	uint32_t computed_view_count;

	// Frame state (stored between begin_frame and submit/end)
	XrTime predicted_display_time;
	int frame_begun;

	PFN_xrDestroyInstance pfn_destroy_instance;
	PFN_xrGetSystem pfn_get_system;
	PFN_xrGetSystemProperties pfn_get_system_properties;
	PFN_xrCreateSession pfn_create_session;
	PFN_xrDestroySession pfn_destroy_session;
	PFN_xrCreateReferenceSpace pfn_create_reference_space;
	PFN_xrDestroySpace pfn_destroy_space;
	PFN_xrBeginSession pfn_begin_session;
	PFN_xrEndSession pfn_end_session;
	PFN_xrRequestExitSession pfn_request_exit_session;
	PFN_xrWaitFrame pfn_wait_frame;
	PFN_xrBeginFrame pfn_begin_frame;
	PFN_xrEndFrame pfn_end_frame;
	PFN_xrPollEvent pfn_poll_event;
	PFN_xrLocateViews pfn_locate_views;
	PFN_xrEnumerateSwapchainFormats pfn_enumerate_swapchain_formats;
	PFN_xrCreateSwapchain pfn_create_swapchain;
	PFN_xrDestroySwapchain pfn_destroy_swapchain;
	PFN_xrEnumerateSwapchainImages pfn_enumerate_swapchain_images;
	PFN_xrAcquireSwapchainImage pfn_acquire_swapchain_image;
	PFN_xrWaitSwapchainImage pfn_wait_swapchain_image;
	PFN_xrReleaseSwapchainImage pfn_release_swapchain_image;

	// Display mode extensions (optional — resolved after instance creation)
	PFN_xrRequestDisplayModeEXT pfn_request_display_mode;
	PFN_xrRequestDisplayRenderingModeEXT pfn_request_rendering_mode;
	PFN_xrEnumerateDisplayRenderingModesEXT pfn_enumerate_rendering_modes;
	PFN_xrSetSharedTextureOutputRectEXT pfn_set_output_rect;
	uint32_t canvas_width;
	uint32_t canvas_height;
	int32_t canvas_x;
	int32_t canvas_y;
	int has_display_mode_ext;
#if defined(_WIN32)
	int window_closed;  // Set to 1 when user closes the preview window
	// Mouse button state tracked by sa_wndproc for the C# input controller
	// (Unity's new Input System doesn't see our preview window's clicks).
	volatile int preview_mouse_buttons; // bit 0=L, 1=R, 2=M
	volatile int preview_wheel_accum;
	// Cursor position in client-area pixels (top-left origin), updated from
	// WM_MOUSEMOVE. -1 if cursor is outside content area or untracked.
	volatile int preview_mouse_x;
	volatile int preview_mouse_y;
	volatile int preview_mouse_in_content; // 1 = cursor inside content area
	int dragging;       // Non-zero while custom (non-modal) window move is active
	POINT drag_start_cursor;
	RECT drag_start_rect;
#endif
} StandaloneState;

static StandaloneState s_sa = {};

static StandaloneGraphicsBackend *s_sa_backend = nullptr;

// ============================================================================
// Log callback — routes native messages to Unity's Debug.Log
// ============================================================================

static DisplayXRLogCallback s_log_callback = NULL;

DISPLAYXR_EXPORT void
displayxr_standalone_set_log_callback(DisplayXRLogCallback callback)
{
	s_log_callback = callback;
}

// Helper: log to stderr AND to the Unity callback (if registered).
static void
sa_log(const char *fmt, ...)
{
	char buf[1024];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);

	fprintf(stderr, "%s", buf);
	if (s_log_callback)
		s_log_callback(buf);
}

// ============================================================================
// JSON parsing helper (minimal, no external deps)
// ============================================================================

static char *
parse_library_path(const char *json_path)
{
	FILE *f = fopen(json_path, "r");
	if (!f) return NULL;

	fseek(f, 0, SEEK_END);
	long len = ftell(f);
	fseek(f, 0, SEEK_SET);

	char *buf = (char *)malloc(len + 1);
	if (!buf) { fclose(f); return NULL; }
	fread(buf, 1, len, f);
	buf[len] = '\0';
	fclose(f);

	const char *key = "\"library_path\"";
	char *pos = strstr(buf, key);
	if (!pos) { free(buf); return NULL; }
	pos += strlen(key);

	while (*pos == ' ' || *pos == '\t' || *pos == '\n' || *pos == '\r' || *pos == ':') pos++;
	if (*pos != '"') { free(buf); return NULL; }
	pos++;

	char *end = strchr(pos, '"');
	if (!end) { free(buf); return NULL; }

	size_t path_len = end - pos;
	char *result = (char *)malloc(path_len + 1);
	memcpy(result, pos, path_len);
	result[path_len] = '\0';

	free(buf);
	return result;
}

static char *
resolve_library_path(const char *json_path, const char *lib_path)
{
	if (lib_path[0] == '/') {
		return strdup(lib_path);
	}

	const char *last_sep = strrchr(json_path, '/');
#if defined(_WIN32)
	const char *last_bsep = strrchr(json_path, '\\');
	if (last_bsep && (!last_sep || last_bsep > last_sep)) last_sep = last_bsep;
#endif

	size_t dir_len = last_sep ? (size_t)(last_sep - json_path + 1) : 0;
	size_t lib_len = strlen(lib_path);

	char *result = (char *)malloc(dir_len + lib_len + 1);
	if (dir_len > 0) memcpy(result, json_path, dir_len);
	memcpy(result + dir_len, lib_path, lib_len);
	result[dir_len + lib_len] = '\0';

	return result;
}


// ============================================================================
// Resolve OpenXR function pointers
// ============================================================================

#define SA_RESOLVE_FN(xr_name, field_name, type) do { \
	PFN_xrVoidFunction fn = NULL; \
	if (XR_FAILED(s_sa.gipa(s_sa.instance, #xr_name, &fn)) || !fn) { \
		sa_log("[DisplayXR-SA] Failed to resolve %s\n", #xr_name); \
		return 0; \
	} \
	s_sa.field_name = (type)fn; \
} while(0)

static int
resolve_functions(void)
{
	SA_RESOLVE_FN(xrDestroyInstance, pfn_destroy_instance, PFN_xrDestroyInstance);
	SA_RESOLVE_FN(xrGetSystem, pfn_get_system, PFN_xrGetSystem);
	SA_RESOLVE_FN(xrGetSystemProperties, pfn_get_system_properties, PFN_xrGetSystemProperties);
	SA_RESOLVE_FN(xrCreateSession, pfn_create_session, PFN_xrCreateSession);
	SA_RESOLVE_FN(xrDestroySession, pfn_destroy_session, PFN_xrDestroySession);
	SA_RESOLVE_FN(xrCreateReferenceSpace, pfn_create_reference_space, PFN_xrCreateReferenceSpace);
	SA_RESOLVE_FN(xrDestroySpace, pfn_destroy_space, PFN_xrDestroySpace);
	SA_RESOLVE_FN(xrBeginSession, pfn_begin_session, PFN_xrBeginSession);
	SA_RESOLVE_FN(xrEndSession, pfn_end_session, PFN_xrEndSession);
	SA_RESOLVE_FN(xrRequestExitSession, pfn_request_exit_session, PFN_xrRequestExitSession);
	SA_RESOLVE_FN(xrWaitFrame, pfn_wait_frame, PFN_xrWaitFrame);
	SA_RESOLVE_FN(xrBeginFrame, pfn_begin_frame, PFN_xrBeginFrame);
	SA_RESOLVE_FN(xrEndFrame, pfn_end_frame, PFN_xrEndFrame);
	SA_RESOLVE_FN(xrPollEvent, pfn_poll_event, PFN_xrPollEvent);
	SA_RESOLVE_FN(xrLocateViews, pfn_locate_views, PFN_xrLocateViews);
	SA_RESOLVE_FN(xrEnumerateSwapchainFormats, pfn_enumerate_swapchain_formats, PFN_xrEnumerateSwapchainFormats);
	SA_RESOLVE_FN(xrCreateSwapchain, pfn_create_swapchain, PFN_xrCreateSwapchain);
	SA_RESOLVE_FN(xrDestroySwapchain, pfn_destroy_swapchain, PFN_xrDestroySwapchain);
	SA_RESOLVE_FN(xrEnumerateSwapchainImages, pfn_enumerate_swapchain_images, PFN_xrEnumerateSwapchainImages);
	SA_RESOLVE_FN(xrAcquireSwapchainImage, pfn_acquire_swapchain_image, PFN_xrAcquireSwapchainImage);
	SA_RESOLVE_FN(xrWaitSwapchainImage, pfn_wait_swapchain_image, PFN_xrWaitSwapchainImage);
	SA_RESOLVE_FN(xrReleaseSwapchainImage, pfn_release_swapchain_image, PFN_xrReleaseSwapchainImage);

	// Optional display mode extensions (don't fail if missing)
	{
		PFN_xrVoidFunction fn = NULL;
		if (XR_SUCCEEDED(s_sa.gipa(s_sa.instance, "xrRequestDisplayModeEXT", &fn)) && fn) {
			s_sa.pfn_request_display_mode = (PFN_xrRequestDisplayModeEXT)fn;
			s_sa.has_display_mode_ext = 1;
			sa_log("[DisplayXR-SA] Resolved xrRequestDisplayModeEXT\n");
		}
		fn = NULL;
		if (XR_SUCCEEDED(s_sa.gipa(s_sa.instance, "xrRequestDisplayRenderingModeEXT", &fn)) && fn) {
			s_sa.pfn_request_rendering_mode = (PFN_xrRequestDisplayRenderingModeEXT)fn;
			sa_log("[DisplayXR-SA] Resolved xrRequestDisplayRenderingModeEXT\n");
		}
		fn = NULL;
		if (XR_SUCCEEDED(s_sa.gipa(s_sa.instance, "xrEnumerateDisplayRenderingModesEXT", &fn)) && fn) {
			s_sa.pfn_enumerate_rendering_modes = (PFN_xrEnumerateDisplayRenderingModesEXT)fn;
			sa_log("[DisplayXR-SA] Resolved xrEnumerateDisplayRenderingModesEXT\n");
		}
		fn = NULL;
		if (XR_SUCCEEDED(s_sa.gipa(s_sa.instance, "xrSetSharedTextureOutputRectEXT", &fn)) && fn) {
			s_sa.pfn_set_output_rect = (PFN_xrSetSharedTextureOutputRectEXT)fn;
			sa_log("[DisplayXR-SA] Resolved xrSetSharedTextureOutputRectEXT\n");
		}
	}

	return 1;
}


// ============================================================================
// Rendering mode helpers
// ============================================================================

static void
enumerate_and_store_modes(void)
{
	s_sa.rendering_mode_count = 0;
	if (!s_sa.pfn_enumerate_rendering_modes || s_sa.session == XR_NULL_HANDLE)
		return;

	uint32_t total = 0;
	XrResult result = s_sa.pfn_enumerate_rendering_modes(s_sa.session, 0, &total, NULL);
	if (XR_FAILED(result) || total == 0) return;

	if (total > SA_MAX_RENDERING_MODES) total = SA_MAX_RENDERING_MODES;
	for (uint32_t i = 0; i < total; i++) {
		s_sa.rendering_modes[i].type = XR_TYPE_DISPLAY_RENDERING_MODE_INFO_EXT;
		s_sa.rendering_modes[i].next = NULL;
	}

	result = s_sa.pfn_enumerate_rendering_modes(s_sa.session, total, &total, s_sa.rendering_modes);
	if (XR_FAILED(result)) return;

	s_sa.rendering_mode_count = total;
	for (uint32_t i = 0; i < total; i++) {
		sa_log("[DisplayXR-SA] Mode[%u]: '%s' views=%u tiles=%ux%u viewPx=%ux%u scale=%.2fx%.2f hw3d=%d\n",
		        s_sa.rendering_modes[i].modeIndex,
		        s_sa.rendering_modes[i].modeName,
		        s_sa.rendering_modes[i].viewCount,
		        s_sa.rendering_modes[i].tileColumns,
		        s_sa.rendering_modes[i].tileRows,
		        s_sa.rendering_modes[i].viewWidthPixels,
		        s_sa.rendering_modes[i].viewHeightPixels,
		        (double)s_sa.rendering_modes[i].viewScaleX,
		        (double)s_sa.rendering_modes[i].viewScaleY,
		        (int)s_sa.rendering_modes[i].hardwareDisplay3D);
	}
}

/// Get the current rendering mode info. Returns NULL if no modes enumerated.
static const XrDisplayRenderingModeInfoEXT *
get_current_mode(void)
{
	for (uint32_t i = 0; i < s_sa.rendering_mode_count; i++) {
		if (s_sa.rendering_modes[i].modeIndex == s_sa.current_rendering_mode_index)
			return &s_sa.rendering_modes[i];
	}
	return (s_sa.rendering_mode_count > 0) ? &s_sa.rendering_modes[0] : NULL;
}

/// Get tiling parameters for a rendering mode (or defaults if NULL).
static void
get_mode_tiling(const XrDisplayRenderingModeInfoEXT *mode,
                uint32_t *view_count, uint32_t *tile_cols, uint32_t *tile_rows,
                uint32_t *view_w, uint32_t *view_h)
{
	if (mode && mode->tileColumns > 0 && mode->tileRows > 0) {
		*view_count = mode->viewCount > 0 ? mode->viewCount : 2;
		*tile_cols = mode->tileColumns;
		*tile_rows = mode->tileRows;
		*view_w = mode->viewWidthPixels > 0
			? mode->viewWidthPixels
			: (uint32_t)(mode->viewScaleX * s_sa.display_info.display_pixel_width);
		*view_h = mode->viewHeightPixels > 0
			? mode->viewHeightPixels
			: (uint32_t)(mode->viewScaleY * s_sa.display_info.display_pixel_height);
	} else {
		// Fallback: stereo SBS
		*view_count = 2;
		*tile_cols = 2;
		*tile_rows = 1;
		*view_w = s_sa.display_info.display_pixel_width;
		*view_h = s_sa.display_info.display_pixel_height;
	}
}


// Canvas-aware render tiling: computes per-view render dimensions from canvas
// dims (matching the reference app pattern). Falls back to display-based dims
// if canvas is not set. Used for actual rendering; get_mode_tiling (above)
// is used for worst-case atlas/swapchain sizing.
static void
get_render_tiling(const XrDisplayRenderingModeInfoEXT *mode,
                  uint32_t *view_count, uint32_t *tile_cols, uint32_t *tile_rows,
                  uint32_t *view_w, uint32_t *view_h)
{
	if (!mode || mode->tileColumns == 0 || mode->tileRows == 0) {
		// Fallback: use display-based tiling
		get_mode_tiling(mode, view_count, tile_cols, tile_rows, view_w, view_h);
		return;
	}

	*view_count = mode->viewCount > 0 ? mode->viewCount : 2;
	*tile_cols = mode->tileColumns;
	*tile_rows = mode->tileRows;

	if (s_sa.canvas_width > 0 && s_sa.canvas_height > 0) {
		int display3D = (int)mode->hardwareDisplay3D;
		if (!display3D) {
			// 2D mode: render at canvas size
			*view_w = s_sa.canvas_width;
			*view_h = s_sa.canvas_height;
		} else {
			// 3D mode: canvas × view scale
			float sx = mode->viewScaleX > 0 ? mode->viewScaleX : 0.5f;
			float sy = mode->viewScaleY > 0 ? mode->viewScaleY : 0.5f;
			*view_w = (uint32_t)(s_sa.canvas_width * sx);
			*view_h = (uint32_t)(s_sa.canvas_height * sy);
		}

		// Clamp to atlas tile limits
		uint32_t max_vw, max_vh, dummy_vc, dummy_tc, dummy_tr;
		get_mode_tiling(mode, &dummy_vc, &dummy_tc, &dummy_tr, &max_vw, &max_vh);
		if (*view_w > max_vw) *view_w = max_vw;
		if (*view_h > max_vh) *view_h = max_vh;
	} else {
		// No canvas set — fall back to display-based dims
		get_mode_tiling(mode, view_count, tile_cols, tile_rows, view_w, view_h);
	}
}


// ============================================================================
// Swapchain management (single atlas)
// ============================================================================

static int
create_atlas_swapchain(void)
{
	if (s_sa.atlas_created) return 1;
	if (!s_sa.display_info.is_valid) return 0;

	// Compute max atlas size across all rendering modes
	uint32_t atlas_w = 0, atlas_h = 0;

	if (s_sa.rendering_mode_count > 0) {
		for (uint32_t i = 0; i < s_sa.rendering_mode_count; i++) {
			uint32_t vc, tc, tr, vw, vh;
			get_mode_tiling(&s_sa.rendering_modes[i], &vc, &tc, &tr, &vw, &vh);
			uint32_t mw = tc * vw;
			uint32_t mh = tr * vh;
			if (mw > atlas_w) atlas_w = mw;
			if (mh > atlas_h) atlas_h = mh;
		}
	}

	// Fallback: stereo SBS at display resolution
	if (atlas_w == 0 || atlas_h == 0) {
		atlas_w = s_sa.display_info.display_pixel_width * 2;
		atlas_h = s_sa.display_info.display_pixel_height;
	}

	// Enumerate supported formats and pick one
	uint32_t fmt_count = 0;
	s_sa.pfn_enumerate_swapchain_formats(s_sa.session, 0, &fmt_count, NULL);
	if (fmt_count == 0) {
		sa_log("[DisplayXR-SA] No swapchain formats available\n");
		return 0;
	}

	int64_t formats[32];
	if (fmt_count > 32) fmt_count = 32;
	s_sa.pfn_enumerate_swapchain_formats(s_sa.session, fmt_count, &fmt_count, formats);

	// Pick a suitable format per platform
	int64_t format = formats[0];
	for (uint32_t i = 0; i < fmt_count; i++) {
		sa_log("[DisplayXR-SA] Supported format[%u]: %lld\n", i, formats[i]);
#if defined(__APPLE__)
		if (formats[i] == 80) { format = 80; break; } // MTLPixelFormatBGRA8Unorm
		if (formats[i] == 70) { format = 70; }         // MTLPixelFormatRGBA8Unorm
#elif defined(_WIN32)
		if (formats[i] == 28) { format = 28; break; }  // DXGI_FORMAT_R8G8B8A8_UNORM
		if (formats[i] == 87) { format = 87; }          // DXGI_FORMAT_B8G8R8A8_UNORM
#endif
	}
	sa_log("[DisplayXR-SA] Selected swapchain format: %lld\n", format);

	XrSwapchainCreateInfo sc_ci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
	sc_ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
	                   XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT |
	                   XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
	sc_ci.format = format;
	sc_ci.sampleCount = 1;
	sc_ci.width = atlas_w;
	sc_ci.height = atlas_h;
	sc_ci.faceCount = 1;
	sc_ci.arraySize = 1;
	sc_ci.mipCount = 1;

	XrResult result = s_sa.pfn_create_swapchain(
		s_sa.session, &sc_ci, &s_sa.atlas.handle);
	if (XR_FAILED(result)) {
		sa_log("[DisplayXR-SA] xrCreateSwapchain (atlas) failed: %d\n", result);
		return 0;
	}

	s_sa.atlas.width = atlas_w;
	s_sa.atlas.height = atlas_h;
	s_sa.atlas.format = format;

	// Enumerate swapchain images (delegated to backend)
	uint32_t count = 0;
	s_sa.pfn_enumerate_swapchain_images(s_sa.atlas.handle, 0, &count, NULL);
	if (count > SA_MAX_SWAPCHAIN_IMAGES) count = SA_MAX_SWAPCHAIN_IMAGES;
	s_sa.atlas.image_count = count;

	if (s_sa_backend) {
		if (!s_sa_backend->enumerate_atlas_images(s_sa.atlas.handle,
		                                           s_sa.pfn_enumerate_swapchain_images,
		                                           count)) {
			return 0;
		}
	}

	sa_log("[DisplayXR-SA] Atlas swapchain: %ux%u, %u images\n",
	        atlas_w, atlas_h, count);

	s_sa.atlas_created = 1;

	if (s_sa_backend) {
		s_sa_backend->create_atlas_bridge(atlas_w, atlas_h, nullptr);
	}

	return 1;
}

static void
destroy_atlas_swapchain(void)
{
	if (s_sa.atlas.handle != XR_NULL_HANDLE && s_sa.pfn_destroy_swapchain) {
		s_sa.pfn_destroy_swapchain(s_sa.atlas.handle);
		s_sa.atlas.handle = XR_NULL_HANDLE;
	}
	s_sa.atlas_created = 0;
}


// ============================================================================
// Public API: Session lifecycle
// ============================================================================


#if defined(_WIN32)
// Push the canvas rect to the runtime via xrSetSharedTextureOutputRectEXT.
// canvas_offset_x/y are WINDOW-RELATIVE (offset within the client area),
// not screen-relative — the SR weaver adds these to its own window position
// when computing phase alignment. Since our canvas IS the full window with
// no letterbox, the offset is always (0, 0) and the size is the client area.
static void
sa_push_canvas_rect_to_runtime(void)
{
	if (s_sa.pfn_set_output_rect && s_sa.session != XR_NULL_HANDLE &&
	    s_sa.canvas_width > 0 && s_sa.canvas_height > 0) {
		s_sa.pfn_set_output_rect(s_sa.session,
		                          0, 0,
		                          s_sa.canvas_width, s_sa.canvas_height);
	}
}

static void
sa_update_canvas_from_hwnd(HWND hwnd)
{
	if (!hwnd) return;
	RECT rc;
	if (GetClientRect(hwnd, &rc)) {
		POINT pt = {0, 0};
		ClientToScreen(hwnd, &pt);
		s_sa.canvas_x = pt.x;
		s_sa.canvas_y = pt.y;
		s_sa.canvas_width = (uint32_t)(rc.right - rc.left);
		s_sa.canvas_height = (uint32_t)(rc.bottom - rc.top);
		sa_push_canvas_rect_to_runtime();
	}
}

// Forward input messages from the preview window to Unity's main HWND so
// game scripts (cameras, controllers, etc.) receive mouse/keyboard input
// even when the user clicks inside the preview window. The preview window
// stays the foreground window — we just relay events into Unity's queue.
//
// Cache Unity's HWND once at session start (in displayxr_standalone_start)
// rather than re-querying lazily — find_unity_hwnd() can return our own
// preview window if it ends up matching FindWindowExW's enumeration before
// Unity's window, which would cause a feedback loop.
static HWND s_unity_hwnd = NULL;

static LRESULT CALLBACK
sa_wndproc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	// Track mouse buttons + wheel for C# input controller polling.
	// SetCapture keeps WM_*BUTTONUP flowing even if the cursor leaves the
	// window during a drag. WM_CAPTURECHANGED clears state if capture is lost
	// unexpectedly (e.g. Ctrl+Alt+Del).
	switch (msg) {
	case WM_LBUTTONDOWN: s_sa.preview_mouse_buttons |= 0x1; SetCapture(hwnd); break;
	case WM_LBUTTONUP:   s_sa.preview_mouse_buttons &= ~0x1; if (GetCapture() == hwnd) ReleaseCapture(); break;
	case WM_RBUTTONDOWN: s_sa.preview_mouse_buttons |= 0x2; SetCapture(hwnd); break;
	case WM_RBUTTONUP:   s_sa.preview_mouse_buttons &= ~0x2; if (GetCapture() == hwnd) ReleaseCapture(); break;
	case WM_MBUTTONDOWN: s_sa.preview_mouse_buttons |= 0x4; SetCapture(hwnd); break;
	case WM_MBUTTONUP:   s_sa.preview_mouse_buttons &= ~0x4; if (GetCapture() == hwnd) ReleaseCapture(); break;
	case WM_MOUSEWHEEL:
		s_sa.preview_wheel_accum += GET_WHEEL_DELTA_WPARAM(wParam);
		break;
	case WM_MOUSEMOVE: {
		// Track cursor position in client-area pixels (top-left origin), for
		// app-side input routers that map to a window-space layer.
		// Decode lParam without windowsx.h: low word = x, high word = y.
		int x = (int)(short)LOWORD(lParam);
		int y = (int)(short)HIWORD(lParam);
		RECT rc;
		if (GetClientRect(hwnd, &rc) && x >= 0 && y >= 0 &&
		    x < rc.right && y < rc.bottom) {
			s_sa.preview_mouse_x = x;
			s_sa.preview_mouse_y = y;
			s_sa.preview_mouse_in_content = 1;
		}
		break;
	}
	case WM_MOUSELEAVE:
		s_sa.preview_mouse_in_content = 0;
		s_sa.preview_mouse_x = -1;
		s_sa.preview_mouse_y = -1;
		break;
	case WM_CAPTURECHANGED:
		// Capture lost — clear button state to avoid "stuck button" bugs
		s_sa.preview_mouse_buttons = 0;
		break;
	}

	// Forward keyboard messages to Unity's main HWND. Unity normally gets
	// raw keyboard input directly (it stays foreground via WS_EX_NOACTIVATE),
	// but this is a safety net for legacy Input.GetKey paths.
	switch (msg) {
	case WM_KEYDOWN: case WM_KEYUP:
	case WM_SYSKEYDOWN: case WM_SYSKEYUP:
	case WM_CHAR: case WM_SYSCHAR: {
		if (s_unity_hwnd != NULL && s_unity_hwnd != hwnd && IsWindow(s_unity_hwnd))
			PostMessageW(s_unity_hwnd, msg, wParam, lParam);
		break;
	}
	}

	switch (msg) {
	case WM_MOUSEACTIVATE:
		// Don't activate (don't become foreground) when clicked. Combined
		// with WS_EX_NOACTIVATE on the window, this keeps Unity foreground
		// so Raw Input keeps flowing to Unity's new Input System
		// (Mouse.current.delta etc.).
		return MA_NOACTIVATE;
	case WM_CLOSE:
		s_sa.window_closed = 1;
		DestroyWindow(hwnd);
		return 0;
	case WM_DESTROY:
		s_sa.preview_hwnd = NULL;
		return 0;
	// #61: capture-based SC_MOVE intercept with synchronous
	// WM_ENTERSIZEMOVE/EXITSIZEMOVE bracketing. Bypassing DefWindowProc's
	// modal drag loop keeps Unity's main thread running so FrameTick fires
	// during the move (real-time Kooima). The bracketing messages drive the
	// SR SDK weaver's WndProc subclass into its phase-snap state so the
	// window lands on lenticular-aligned pixels. Order matters: ENTERSIZEMOVE
	// before the first SetWindowPos, EXITSIZEMOVE after the flag is cleared
	// so the recursive WM_CAPTURECHANGED from ReleaseCapture() can't re-send
	// it.
	case WM_SYSCOMMAND:
		if ((wParam & 0xFFF0) == SC_MOVE) {
			SetCapture(hwnd);
			GetCursorPos(&s_sa.drag_start_cursor);
			GetWindowRect(hwnd, &s_sa.drag_start_rect);
			s_sa.dragging = 1;
			SendMessageW(hwnd, WM_ENTERSIZEMOVE, 0, 0);
			return 0;
		}
		break;
	case WM_MOUSEMOVE:
		if (s_sa.dragging) {
			POINT cur;
			GetCursorPos(&cur);
			int dx = cur.x - s_sa.drag_start_cursor.x;
			int dy = cur.y - s_sa.drag_start_cursor.y;
			SetWindowPos(hwnd, NULL,
			             s_sa.drag_start_rect.left + dx,
			             s_sa.drag_start_rect.top + dy,
			             0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
		}
		break;
	case WM_LBUTTONUP:
		if (s_sa.dragging) {
			s_sa.dragging = 0;
			SendMessageW(hwnd, WM_EXITSIZEMOVE, 0, 0);
			ReleaseCapture();
			return 0;
		}
		break;
	case WM_CAPTURECHANGED: {
		int was_dragging = s_sa.dragging;
		s_sa.dragging = 0;
		if (was_dragging)
			SendMessageW(hwnd, WM_EXITSIZEMOVE, 0, 0);
		break;
	}
	case WM_MOVE:
	case WM_SIZE:
		// Update canvas rect from current window position and push to runtime
		// for SR weaver phase calculation. With SC_MOVE intercepted, this fires
		// continuously during the custom drag (each SetWindowPos call).
		sa_update_canvas_from_hwnd(s_sa.preview_hwnd);
		break;
	}
	return DefWindowProcW(hwnd, msg, wParam, lParam);
}
#endif

int
displayxr_standalone_start(const char *runtime_json_path)
{
	if (s_sa.running) {
		sa_log("[DisplayXR-SA] Already running\n");
		return 1;
	}

	// s_sa_backend (if created by set_unity_device) survives this memset —
	// it's a separate heap allocation, not part of s_sa.
	memset(&s_sa, 0, sizeof(s_sa));
#if defined(_WIN32)
	// Cache Unity's main HWND BEFORE we create our preview window — otherwise
	// find_unity_hwnd might enumerate our preview window first and we'd
	// forward input back to ourselves (feedback loop).
	s_unity_hwnd = (HWND)displayxr_get_unity_main_hwnd();
	if (s_unity_hwnd)
		sa_log("[DisplayXR-SA] Cached Unity main HWND: %p\n", (void *)s_unity_hwnd);
	else
		sa_log("[DisplayXR-SA] Could not find Unity main HWND for input forwarding\n");

	// Set Per-Monitor DPI Awareness V2 so the Leia SR weaver sees physical
	// pixels from GetClientRect(), matching our canvas rect dimensions.
	SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
#endif

	sa_log("[DisplayXR-SA] Starting with runtime: %s\n", runtime_json_path);

	// --- Step 1: Load the runtime library ---
	char *lib_rel = parse_library_path(runtime_json_path);
	if (!lib_rel) {
		sa_log("[DisplayXR-SA] Failed to parse library_path from %s\n", runtime_json_path);
		return 0;
	}

	char *lib_abs = resolve_library_path(runtime_json_path, lib_rel);
	free(lib_rel);

	sa_log("[DisplayXR-SA] Loading runtime library: %s\n", lib_abs);

	XrResult result;

#if defined(_WIN32)
	HMODULE hmod = LoadLibraryA(lib_abs);
	if (!hmod) {
		sa_log("[DisplayXR-SA] LoadLibrary failed: %lu\n", GetLastError());
		free(lib_abs);
		return 0;
	}
	s_sa.runtime_lib = (void *)hmod;
	free(lib_abs);

	// --- Step 2: Negotiate with the runtime ---
	PFN_xrNegotiateLoaderRuntimeInterface negotiate =
		(PFN_xrNegotiateLoaderRuntimeInterface)GetProcAddress(hmod,
		    "xrNegotiateLoaderRuntimeInterface");
	if (!negotiate) {
		sa_log("[DisplayXR-SA] Runtime doesn't export xrNegotiateLoaderRuntimeInterface\n");
		FreeLibrary(hmod);
		s_sa.runtime_lib = NULL;
		return 0;
	}

	XrNegotiateLoaderInfo loader_info = {};
	loader_info.structType = XR_LOADER_INTERFACE_STRUCT_LOADER_INFO;
	loader_info.structVersion = 1;
	loader_info.structSize = sizeof(XrNegotiateLoaderInfo);
	loader_info.minInterfaceVersion = 1;
	loader_info.maxInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION;
	loader_info.minApiVersion = XR_MAKE_VERSION(1, 0, 0);
	loader_info.maxApiVersion = XR_MAKE_VERSION(1, 1, 0);

	XrNegotiateRuntimeRequest runtime_req = {};
	runtime_req.structType = XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST;
	runtime_req.structVersion = 1;
	runtime_req.structSize = sizeof(XrNegotiateRuntimeRequest);

	result = negotiate(&loader_info, &runtime_req);
	if (XR_FAILED(result) || !runtime_req.getInstanceProcAddr) {
		sa_log("[DisplayXR-SA] Runtime negotiation failed: %d\n", result);
		FreeLibrary(hmod);
		s_sa.runtime_lib = NULL;
		return 0;
	}

	s_sa.gipa = runtime_req.getInstanceProcAddr;
	sa_log("[DisplayXR-SA] Runtime negotiation succeeded\n");
#else
	s_sa.runtime_lib = dlopen(lib_abs, RTLD_LOCAL | RTLD_LAZY);
	if (!s_sa.runtime_lib) {
		sa_log("[DisplayXR-SA] dlopen failed: %s\n", dlerror());
		free(lib_abs);
		return 0;
	}
	free(lib_abs);

	// --- Step 2: Negotiate with the runtime ---
	PFN_xrNegotiateLoaderRuntimeInterface negotiate =
		(PFN_xrNegotiateLoaderRuntimeInterface)dlsym(s_sa.runtime_lib,
		    "xrNegotiateLoaderRuntimeInterface");
	if (!negotiate) {
		sa_log("[DisplayXR-SA] Runtime doesn't export xrNegotiateLoaderRuntimeInterface\n");
		dlclose(s_sa.runtime_lib);
		s_sa.runtime_lib = NULL;
		return 0;
	}

	XrNegotiateLoaderInfo loader_info = {};
	loader_info.structType = XR_LOADER_INTERFACE_STRUCT_LOADER_INFO;
	loader_info.structVersion = 1;
	loader_info.structSize = sizeof(XrNegotiateLoaderInfo);
	loader_info.minInterfaceVersion = 1;
	loader_info.maxInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION;
	loader_info.minApiVersion = XR_MAKE_VERSION(1, 0, 0);
	loader_info.maxApiVersion = XR_MAKE_VERSION(1, 1, 0);

	XrNegotiateRuntimeRequest runtime_req = {};
	runtime_req.structType = XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST;
	runtime_req.structVersion = 1;
	runtime_req.structSize = sizeof(XrNegotiateRuntimeRequest);

	result = negotiate(&loader_info, &runtime_req);
	if (XR_FAILED(result) || !runtime_req.getInstanceProcAddr) {
		sa_log("[DisplayXR-SA] Runtime negotiation failed: %d\n", result);
		dlclose(s_sa.runtime_lib);
		s_sa.runtime_lib = NULL;
		return 0;
	}

	s_sa.gipa = runtime_req.getInstanceProcAddr;
	sa_log("[DisplayXR-SA] Runtime negotiation succeeded\n");
#endif

	// --- Step 3: Create OpenXR instance ---
	const char *extensions[] = {
		XR_EXT_DISPLAY_INFO_EXTENSION_NAME,
#if defined(__APPLE__)
		XR_KHR_METAL_ENABLE_EXTENSION_NAME,
		XR_EXT_COCOA_WINDOW_BINDING_EXTENSION_NAME,
#elif defined(_WIN32)
		"XR_KHR_D3D12_enable",
		XR_EXT_WIN32_WINDOW_BINDING_EXTENSION_NAME,
#endif
	};
	uint32_t ext_count = sizeof(extensions) / sizeof(extensions[0]);

	PFN_xrVoidFunction fn_create = NULL;
	s_sa.gipa(XR_NULL_HANDLE, "xrCreateInstance", &fn_create);
	if (!fn_create) {
		sa_log("[DisplayXR-SA] Failed to resolve xrCreateInstance\n");
		displayxr_standalone_stop();
		return 0;
	}

	XrInstanceCreateInfo instance_ci = {XR_TYPE_INSTANCE_CREATE_INFO};
	strncpy(instance_ci.applicationInfo.applicationName, "DisplayXR Preview", XR_MAX_APPLICATION_NAME_SIZE);
	instance_ci.applicationInfo.applicationVersion = 1;
	strncpy(instance_ci.applicationInfo.engineName, "Unity", XR_MAX_ENGINE_NAME_SIZE);
	instance_ci.applicationInfo.engineVersion = 1;
	instance_ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
	instance_ci.enabledExtensionCount = ext_count;
	instance_ci.enabledExtensionNames = extensions;

	result = ((PFN_xrCreateInstance)fn_create)(&instance_ci, &s_sa.instance);
	if (XR_FAILED(result)) {
		sa_log("[DisplayXR-SA] xrCreateInstance failed: %d\n", result);
		displayxr_standalone_stop();
		return 0;
	}
	sa_log("[DisplayXR-SA] Instance created\n");

	// --- Step 4: Resolve all function pointers ---
	if (!resolve_functions()) {
		displayxr_standalone_stop();
		return 0;
	}

	// --- Step 5: Get system ---
	XrSystemGetInfo sys_info = {XR_TYPE_SYSTEM_GET_INFO};
	sys_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;

	result = s_sa.pfn_get_system(s_sa.instance, &sys_info, &s_sa.system_id);
	if (XR_FAILED(result)) {
		sa_log("[DisplayXR-SA] xrGetSystem failed: %d\n", result);
		displayxr_standalone_stop();
		return 0;
	}
	sa_log("[DisplayXR-SA] System acquired\n");

	// --- Step 5b: Create graphics device via backend ---
#if defined(__APPLE__)
	if (!s_sa_backend)
		s_sa_backend = create_standalone_metal_backend();
#elif defined(_WIN32)
	if (!s_sa_backend)
		s_sa_backend = create_standalone_d3d12_backend();
#endif
	if (s_sa_backend) {
		if (!s_sa_backend->create_device(s_sa.instance, s_sa.system_id, s_sa.gipa)) {
			sa_log("[DisplayXR-SA] Backend create_device failed\n");
			displayxr_standalone_stop();
			return 0;
		}
	}

	// --- Step 6: Get system properties + display info ---
	XrDisplayInfoEXT display_info_ext = {};
	display_info_ext.type = XR_TYPE_DISPLAY_INFO_EXT;

	XrSystemProperties sys_props = {XR_TYPE_SYSTEM_PROPERTIES};
	sys_props.next = &display_info_ext;

	result = s_sa.pfn_get_system_properties(s_sa.instance, s_sa.system_id, &sys_props);
	if (XR_SUCCEEDED(result)) {
		s_sa.display_info.display_width_meters = display_info_ext.displaySizeMeters.width;
		s_sa.display_info.display_height_meters = display_info_ext.displaySizeMeters.height;
		s_sa.display_info.display_pixel_width = display_info_ext.displayPixelWidth;
		s_sa.display_info.display_pixel_height = display_info_ext.displayPixelHeight;
		s_sa.display_info.nominal_viewer_x = display_info_ext.nominalViewerPositionInDisplaySpace.x;
		s_sa.display_info.nominal_viewer_y = display_info_ext.nominalViewerPositionInDisplaySpace.y;
		s_sa.display_info.nominal_viewer_z = display_info_ext.nominalViewerPositionInDisplaySpace.z;
		s_sa.display_info.recommended_view_scale_x = display_info_ext.recommendedViewScaleX;
		s_sa.display_info.recommended_view_scale_y = display_info_ext.recommendedViewScaleY;
		s_sa.display_info.is_valid = 1;

		sa_log("[DisplayXR-SA] Display: %ux%u, %.3fx%.3fm\n",
		        display_info_ext.displayPixelWidth, display_info_ext.displayPixelHeight,
		        display_info_ext.displaySizeMeters.width, display_info_ext.displaySizeMeters.height);
	} else {
		sa_log("[DisplayXR-SA] xrGetSystemProperties failed: %d\n", result);
	}

	// --- Step 7: Create native preview window ---
#if defined(__APPLE__)
	if (s_sa.display_info.is_valid &&
	    s_sa.display_info.display_pixel_width > 0 &&
	    s_sa.display_info.display_pixel_height > 0) {
		if (!displayxr_sa_metal_create_window(s_sa.display_info.display_pixel_width,
		                                       s_sa.display_info.display_pixel_height)) {
			sa_log("[DisplayXR-SA] Preview window creation failed\n");
			displayxr_standalone_stop();
			return 0;
		}
	}
#elif defined(_WIN32)
	// Create a native preview window (same role as NSWindow on macOS)
	if (s_sa.display_info.is_valid &&
	    s_sa.display_info.display_pixel_width > 0 &&
	    s_sa.display_info.display_pixel_height > 0) {
		WNDCLASSEXW wc = {};
		wc.cbSize = sizeof(wc);
		wc.lpfnWndProc = sa_wndproc;
		wc.hInstance = GetModuleHandle(NULL);
		wc.lpszClassName = L"DisplayXRPreview";
		wc.hCursor = LoadCursor(NULL, IDC_ARROW);
		RegisterClassExW(&wc);

		// Size the window to the display dimensions (in physical pixels).
		// WS_EX_NOACTIVATE: window cannot become foreground/active. This keeps
		// Unity foreground so Raw Input keeps flowing to its new Input System.
		// WS_EX_TOPMOST: window stays above non-topmost windows. Without this,
		// clicking Unity's editor window would bring it above our preview and
		// the preview would visually disappear behind Unity.
		// Don't include WS_VISIBLE in the style — that activates the window on
		// creation. Show explicitly via SW_SHOWNOACTIVATE.
		s_sa.preview_hwnd = CreateWindowExW(
			WS_EX_NOACTIVATE | WS_EX_TOPMOST,
			L"DisplayXRPreview", L"DisplayXR Preview",
			WS_OVERLAPPEDWINDOW,
			CW_USEDEFAULT, CW_USEDEFAULT,
			(int)s_sa.display_info.display_pixel_width,
			(int)s_sa.display_info.display_pixel_height,
			NULL, NULL, wc.hInstance, NULL);

		if (s_sa.preview_hwnd) {
			// Show without stealing focus from Unity
			ShowWindow(s_sa.preview_hwnd, SW_SHOWNOACTIVATE);
			sa_log("[DisplayXR-SA] Preview HWND created: %p (%ux%u)\n",
			        (void *)s_sa.preview_hwnd,
			        s_sa.display_info.display_pixel_width,
			        s_sa.display_info.display_pixel_height);
		} else {
			sa_log("[DisplayXR-SA] CreateWindowExW failed: %lu\n", GetLastError());
		}
	} else {
		sa_log("[DisplayXR-SA] Skipping window creation: is_valid=%d, pixels=%ux%u\n",
		        s_sa.display_info.is_valid,
		        s_sa.display_info.display_pixel_width,
		        s_sa.display_info.display_pixel_height);
	}
#endif

	// --- Step 8: Create session with Metal graphics binding + window binding ---
	XrSessionCreateInfo session_ci = {XR_TYPE_SESSION_CREATE_INFO};
	session_ci.systemId = s_sa.system_id;

#if defined(__APPLE__)
	// Metal graphics binding (required by the runtime as the primary graphics API)
	XrGraphicsBindingMetalKHR metal_binding = {};
	metal_binding.type = XR_TYPE_GRAPHICS_BINDING_METAL_KHR;
	metal_binding.commandQueue = displayxr_sa_metal_get_command_queue();

	if (!metal_binding.commandQueue) {
		sa_log("[DisplayXR-SA] Failed to create MTLCommandQueue\n");
		displayxr_standalone_stop();
		return 0;
	}

	// Cocoa window binding (plugin-owned preview window)
	XrCocoaWindowBindingCreateInfoEXT mac_binding = {};
	mac_binding.type = XR_TYPE_COCOA_WINDOW_BINDING_CREATE_INFO_EXT;
	mac_binding.viewHandle = displayxr_sa_metal_get_view();
	mac_binding.readbackCallback = NULL;
	mac_binding.readbackUserdata = NULL;
	mac_binding.sharedIOSurface = NULL;

	// Chain: session_ci → metal_binding → mac_binding
	metal_binding.next = &mac_binding;
	session_ci.next = &metal_binding;
#elif defined(_WIN32)
	// D3D12 graphics binding (device + queue from backend)
	XrGraphicsBindingD3D12KHR d3d12_binding = {};
	d3d12_binding.type = XR_TYPE_GRAPHICS_BINDING_D3D12_KHR;
	d3d12_binding.device = (ID3D12Device *)s_sa_backend->get_graphics_device();
	d3d12_binding.queue = (ID3D12CommandQueue *)s_sa_backend->get_graphics_queue();

	// Win32 window binding — pass the plugin-owned preview HWND.
	// The runtime composites directly into this window.
	XrWin32WindowBindingCreateInfoEXT win_binding = {};
	win_binding.type = XR_TYPE_WIN32_WINDOW_BINDING_CREATE_INFO_EXT;
	win_binding.windowHandle = (void *)s_sa.preview_hwnd;
	win_binding.readbackCallback = NULL;
	win_binding.readbackUserdata = NULL;
	win_binding.sharedTextureHandle = NULL;

	sa_log("[DisplayXR-SA] Win32 binding: preview HWND=%p\n", (void *)s_sa.preview_hwnd);

	// Chain: session_ci → d3d12_binding → win_binding
	d3d12_binding.next = &win_binding;
	session_ci.next = &d3d12_binding;
#endif

	result = s_sa.pfn_create_session(s_sa.instance, &session_ci, &s_sa.session);
	if (XR_FAILED(result)) {
		sa_log("[DisplayXR-SA] xrCreateSession failed: %d\n", result);
		displayxr_standalone_stop();
		return 0;
	}
	sa_log("[DisplayXR-SA] Session created\n");

	// --- Step 9: Create LOCAL reference space ---
	XrReferenceSpaceCreateInfo space_ci = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
	space_ci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	space_ci.poseInReferenceSpace.orientation = XrQuaternionf{0, 0, 0, 1};
	space_ci.poseInReferenceSpace.position = XrVector3f{0, 0, 0};

	result = s_sa.pfn_create_reference_space(s_sa.session, &space_ci, &s_sa.local_space);
	if (XR_FAILED(result)) {
		sa_log("[DisplayXR-SA] xrCreateReferenceSpace failed: %d\n", result);
		s_sa.local_space = XR_NULL_HANDLE;
	}

	// --- Step 10: Enumerate rendering modes (need session) ---
	enumerate_and_store_modes();

	// Default to first 3D mode (index 1) if available
	if (s_sa.rendering_mode_count > 1)
		s_sa.current_rendering_mode_index = s_sa.rendering_modes[1].modeIndex;
	else if (s_sa.rendering_mode_count > 0)
		s_sa.current_rendering_mode_index = s_sa.rendering_modes[0].modeIndex;

	// --- Step 11: Create atlas swapchain ---
	if (!create_atlas_swapchain()) {
		sa_log("[DisplayXR-SA] Warning: atlas swapchain creation deferred to session ready\n");
	}

	s_sa.running = 1;
	sa_log("[DisplayXR-SA] Standalone session started successfully\n");
	return 1;
}


void
displayxr_standalone_stop(void)
{
	sa_log("[DisplayXR-SA] Stopping standalone session\n");
	s_sa.running = 0;
	s_sa.session_ready = 0;

	wsui_standalone_on_session_destroyed(s_sa.pfn_destroy_swapchain);
	destroy_atlas_swapchain();

	if (s_sa.local_space != XR_NULL_HANDLE && s_sa.pfn_destroy_space) {
		s_sa.pfn_destroy_space(s_sa.local_space);
		s_sa.local_space = XR_NULL_HANDLE;
	}

	if (s_sa.session != XR_NULL_HANDLE && s_sa.pfn_destroy_session) {
		if (s_sa.session_ready && s_sa.pfn_request_exit_session) {
			s_sa.pfn_request_exit_session(s_sa.session);
			// Drain events for graceful shutdown
			if (s_sa.pfn_poll_event) {
				XrEventDataBuffer event = {XR_TYPE_EVENT_DATA_BUFFER};
				for (int i = 0; i < 100; i++) {
					XrResult r = s_sa.pfn_poll_event(s_sa.instance, &event);
					if (r != XR_SUCCESS) break;
					if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
						XrEventDataSessionStateChanged *ssc =
							(XrEventDataSessionStateChanged *)&event;
						if (ssc->state == XR_SESSION_STATE_STOPPING) {
							s_sa.pfn_end_session(s_sa.session);
						}
					}
					event = {XR_TYPE_EVENT_DATA_BUFFER};
				}
			}
		}
		s_sa.pfn_destroy_session(s_sa.session);
		s_sa.session = XR_NULL_HANDLE;
		sa_log("[DisplayXR-SA] Session destroyed\n");
	}

#if defined(__APPLE__)
	displayxr_sa_metal_destroy_window();
#elif defined(_WIN32)
	if (s_sa.preview_hwnd) { DestroyWindow(s_sa.preview_hwnd); s_sa.preview_hwnd = NULL; }
#endif

	if (s_sa_backend) {
		s_sa_backend->destroy();
		delete s_sa_backend;
		s_sa_backend = nullptr;
	}

	if (s_sa.instance != XR_NULL_HANDLE && s_sa.pfn_destroy_instance) {
		s_sa.pfn_destroy_instance(s_sa.instance);
		s_sa.instance = XR_NULL_HANDLE;
		sa_log("[DisplayXR-SA] Instance destroyed\n");
	}

	// NOTE: Intentionally skip library unload on both platforms.
	// The runtime may have background threads referencing code in the
	// shared library. Unloading while those are still draining causes crashes.
	// Leaking the handle is harmless — the editor process will reclaim it on
	// exit, and we can re-load on next Start().
	if (s_sa.runtime_lib) {
		// dlclose / FreeLibrary skipped — see comment above
		s_sa.runtime_lib = NULL;
	}

	memset(&s_sa, 0, sizeof(s_sa));
	sa_log("[DisplayXR-SA] Standalone session stopped\n");
}


int
displayxr_standalone_is_running(void)
{
	return s_sa.running;
}


// ============================================================================
// Public API: Frame loop
// ============================================================================

void
displayxr_standalone_poll_events(void)
{
	if (!s_sa.running || !s_sa.pfn_poll_event) return;

#if defined(_WIN32)
	// Pump Win32 messages so our WndProc receives WM_CLOSE etc.
	MSG msg;
	while (PeekMessageW(&msg, s_sa.preview_hwnd, 0, 0, PM_REMOVE)) {
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}
#endif

	XrEventDataBuffer event = {XR_TYPE_EVENT_DATA_BUFFER};
	while (s_sa.pfn_poll_event(s_sa.instance, &event) == XR_SUCCESS) {
		if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
			XrEventDataSessionStateChanged *ssc =
				(XrEventDataSessionStateChanged *)&event;
			s_sa.session_state = ssc->state;

			sa_log("[DisplayXR-SA] Session state: %d\n", (int)ssc->state);

			switch (ssc->state) {
			case XR_SESSION_STATE_READY: {
				XrSessionBeginInfo begin_info = {XR_TYPE_SESSION_BEGIN_INFO};
				begin_info.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
				XrResult r = s_sa.pfn_begin_session(s_sa.session, &begin_info);
				if (XR_SUCCEEDED(r)) {
					s_sa.session_ready = 1;
					sa_log("[DisplayXR-SA] Session begun\n");
					// Create atlas swapchain now if deferred
					if (!s_sa.atlas_created) {
						if (s_sa.rendering_mode_count == 0)
							enumerate_and_store_modes();
						create_atlas_swapchain();
					}
				}
				break;
			}
			case XR_SESSION_STATE_STOPPING:
				s_sa.session_ready = 0;
				s_sa.pfn_end_session(s_sa.session);
				break;
			case XR_SESSION_STATE_EXITING:
			case XR_SESSION_STATE_LOSS_PENDING:
				s_sa.running = 0;
				break;
			default:
				break;
			}
		}
		event = {XR_TYPE_EVENT_DATA_BUFFER};
	}
}


int
displayxr_standalone_begin_frame(int *should_render)
{
	*should_render = 0;
	if (!s_sa.running || !s_sa.session_ready) return 0;

	XrFrameState frame_state = {XR_TYPE_FRAME_STATE};
	XrResult result = s_sa.pfn_wait_frame(s_sa.session, NULL, &frame_state);
	if (XR_FAILED(result)) return 0;

	result = s_sa.pfn_begin_frame(s_sa.session, NULL);
	if (XR_FAILED(result)) return 0;

	s_sa.predicted_display_time = frame_state.predictedDisplayTime;
	s_sa.frame_begun = 1;

	// Update canvas rect from the preview window's current size/position
#if defined(__APPLE__)
	{
		int32_t wx, wy;
		uint32_t ww, wh;
		if (displayxr_sa_metal_get_window_rect(&wx, &wy, &ww, &wh)) {
			s_sa.canvas_x = wx;
			s_sa.canvas_y = wy;
			s_sa.canvas_width = ww;
			s_sa.canvas_height = wh;
		}
	}
#elif defined(_WIN32)
	sa_update_canvas_from_hwnd(s_sa.preview_hwnd);
#endif

	// Locate views (eye tracking — supports N views for multiview modes)
	if (s_sa.local_space != XR_NULL_HANDLE && s_sa.pfn_locate_views) {
		XrViewLocateInfo locate_info = {XR_TYPE_VIEW_LOCATE_INFO};
		locate_info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
		locate_info.displayTime = frame_state.predictedDisplayTime;
		locate_info.space = s_sa.local_space;

		XrView views[SA_MAX_VIEWS];
		for (int i = 0; i < SA_MAX_VIEWS; i++)
			views[i] = {XR_TYPE_VIEW};
		XrViewState view_state = {XR_TYPE_VIEW_STATE};
		uint32_t view_count = 0;

		result = s_sa.pfn_locate_views(s_sa.session, &locate_info,
		                               &view_state, SA_MAX_VIEWS, &view_count, views);
		if (XR_SUCCEEDED(result) && view_count >= 1) {
			if (view_count > SA_MAX_VIEWS) view_count = SA_MAX_VIEWS;
			s_sa.located_view_count = view_count;

			for (uint32_t i = 0; i < view_count; i++) {
				s_sa.eye_positions[i][0] = views[i].pose.position.x;
				s_sa.eye_positions[i][1] = views[i].pose.position.y;
				s_sa.eye_positions[i][2] = views[i].pose.position.z;
			}

			// Backward compat: populate left/right eye from first 2 views
			s_sa.left_eye[0] = views[0].pose.position.x;
			s_sa.left_eye[1] = views[0].pose.position.y;
			s_sa.left_eye[2] = views[0].pose.position.z;
			if (view_count >= 2) {
				s_sa.right_eye[0] = views[1].pose.position.x;
				s_sa.right_eye[1] = views[1].pose.position.y;
				s_sa.right_eye[2] = views[1].pose.position.z;
			} else {
				s_sa.right_eye[0] = views[0].pose.position.x;
				s_sa.right_eye[1] = views[0].pose.position.y;
				s_sa.right_eye[2] = views[0].pose.position.z;
			}

			s_sa.is_tracked = (view_state.viewStateFlags &
			                   XR_VIEW_STATE_POSITION_TRACKED_BIT) != 0;
		}
	}

	*should_render = frame_state.shouldRender ? 1 : 0;
	return 1;
}


int
displayxr_standalone_submit_frame_atlas(void *atlas_tex)
{
	if (!s_sa.frame_begun || !s_sa.atlas_created) {
		displayxr_standalone_end_frame_empty();
		return 0;
	}
	s_sa.frame_begun = 0;

	// Acquire the single atlas swapchain image
	uint32_t index = 0;
	XrSwapchainImageAcquireInfo acq_info = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
	XrResult r = s_sa.pfn_acquire_swapchain_image(s_sa.atlas.handle, &acq_info, &index);
	if (XR_FAILED(r)) {
		sa_log("[DisplayXR-SA] Acquire atlas swapchain failed: %d\n", r);
		displayxr_standalone_end_frame_empty();
		return 0;
	}

	XrSwapchainImageWaitInfo wait_info = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
	wait_info.timeout = 1000000000; // 1 second
	r = s_sa.pfn_wait_swapchain_image(s_sa.atlas.handle, &wait_info);
	if (XR_FAILED(r)) {
		sa_log("[DisplayXR-SA] Wait atlas swapchain failed: %d\n", r);
	}

	// Blit Unity atlas RenderTexture → swapchain image (delegated to backend)
	if (s_sa_backend) {
		s_sa_backend->blit_atlas(atlas_tex, index);
	}

	XrSwapchainImageReleaseInfo rel_info = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
	s_sa.pfn_release_swapchain_image(s_sa.atlas.handle, &rel_info);

	// Get current mode tiling parameters (canvas-aware render dims)
	const XrDisplayRenderingModeInfoEXT *mode = get_current_mode();
	uint32_t eye_count, tile_cols, tile_rows, view_w, view_h;
	get_render_tiling(mode, &eye_count, &tile_cols, &tile_rows, &view_w, &view_h);

	// Determine whether this is a 3D mode (hw3d) or 2D/mono
	int display3D = mode ? (int)mode->hardwareDisplay3D : 1;
	uint32_t n_views = display3D ? eye_count : 1;
	if (n_views > SA_MAX_VIEWS) n_views = SA_MAX_VIEWS;


	// Build raw eye positions for submission (same duplication as compute_views)
	XrVector3f raw_eyes[SA_MAX_VIEWS];
	for (uint32_t i = 0; i < n_views; i++) {
		uint32_t src = (i < s_sa.located_view_count) ? i : 0;
		raw_eyes[i].x = s_sa.eye_positions[src][0];
		raw_eyes[i].y = s_sa.eye_positions[src][1];
		raw_eyes[i].z = s_sa.eye_positions[src][2];
	}

	// For mono mode, average all located eyes to center
	if (!display3D && s_sa.located_view_count >= 2) {
		float cx = 0, cy = 0, cz = 0;
		for (uint32_t i = 0; i < s_sa.located_view_count; i++) {
			cx += s_sa.eye_positions[i][0];
			cy += s_sa.eye_positions[i][1];
			cz += s_sa.eye_positions[i][2];
		}
		float inv = 1.0f / (float)s_sa.located_view_count;
		raw_eyes[0].x = cx * inv;
		raw_eyes[0].y = cy * inv;
		raw_eyes[0].z = cz * inv;
	}

	// Build N projection views with tiled viewports
	XrCompositionLayerProjectionView proj_views[SA_MAX_VIEWS] = {};

	for (uint32_t eye = 0; eye < n_views; eye++) {
		uint32_t tileX = display3D ? (eye % tile_cols) : 0;
		uint32_t tileY = display3D ? (eye / tile_cols) : 0;

		proj_views[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;

		// Use Kooima-computed FOV + eye_world if available (matches test app)
		if (eye < s_sa.computed_view_count) {
			proj_views[eye].fov = s_sa.computed_views[eye].fov;
			proj_views[eye].pose.position = s_sa.computed_views[eye].eye_world;
			proj_views[eye].pose.orientation = s_sa.computed_views[eye].orientation;
		} else {
			proj_views[eye].pose.position = raw_eyes[eye];
			proj_views[eye].pose.orientation = {0, 0, 0, 1};
			proj_views[eye].fov.angleLeft = -0.5f;
			proj_views[eye].fov.angleRight = 0.5f;
			proj_views[eye].fov.angleUp = 0.3f;
			proj_views[eye].fov.angleDown = -0.3f;
		}

		proj_views[eye].subImage.swapchain = s_sa.atlas.handle;
		proj_views[eye].subImage.imageRect.offset = {
			(int32_t)(tileX * view_w), (int32_t)(tileY * view_h)
		};
		proj_views[eye].subImage.imageRect.extent = {
			(int32_t)view_w, (int32_t)view_h
		};
		proj_views[eye].subImage.imageArrayIndex = 0;
	}

	XrCompositionLayerProjection proj_layer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
	proj_layer.space = s_sa.local_space;
	proj_layer.viewCount = n_views;
	proj_layer.views = proj_views;

	// Window-space UI overlay (issue #67). Lazily creates an overlay
	// swapchain on this session, copies Unity's RT into it, and fills
	// hud_layer if a Unity texture is registered.
	XrCompositionLayerWindowSpaceEXT hud_layer = {};
	WsuiStandaloneFns wsui_fns = {
		s_sa.pfn_enumerate_swapchain_formats,
		s_sa.pfn_create_swapchain,
		s_sa.pfn_destroy_swapchain,
		s_sa.pfn_enumerate_swapchain_images,
		s_sa.pfn_acquire_swapchain_image,
		s_sa.pfn_wait_swapchain_image,
		s_sa.pfn_release_swapchain_image,
	};
	int has_hud = wsui_standalone_pre_end_frame(
		s_sa.session, &wsui_fns, s_sa_backend, &hud_layer);

	const XrCompositionLayerBaseHeader *layers[2] = {
		(const XrCompositionLayerBaseHeader *)&proj_layer,
		(const XrCompositionLayerBaseHeader *)&hud_layer,
	};

	XrFrameEndInfo end_info = {XR_TYPE_FRAME_END_INFO};
	end_info.displayTime = s_sa.predicted_display_time;
	end_info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	end_info.layerCount = has_hud ? 2 : 1;
	end_info.layers = layers;

	s_sa.pfn_end_frame(s_sa.session, &end_info);
	return 1;
}


int
displayxr_standalone_submit_frame(void *left_tex, void *right_tex)
{
	// Backward compatibility wrapper — not used by atlas pipeline
	(void)left_tex;
	(void)right_tex;
	if (s_sa.frame_begun) {
		displayxr_standalone_end_frame_empty();
	}
	return 0;
}


void
displayxr_standalone_end_frame_empty(void)
{
	if (!s_sa.frame_begun) return;
	s_sa.frame_begun = 0;

	XrFrameEndInfo end_info = {XR_TYPE_FRAME_END_INFO};
	end_info.displayTime = s_sa.predicted_display_time;
	end_info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	end_info.layerCount = 0;
	end_info.layers = NULL;

	s_sa.pfn_end_frame(s_sa.session, &end_info);
}


// ============================================================================
// Public API: Multiview compute (N views)
// ============================================================================

void
displayxr_standalone_compute_views(
	uint32_t view_count,
	float near_z, float far_z,
	float *view_matrices,
	float *proj_matrices,
	int *valid)
{
	*valid = 0;
	if (!s_sa.display_info.is_valid || view_count == 0) return;

	// Cache near/far for reuse during drag repositioning
	s_sa.last_near_z = near_z;
	s_sa.last_far_z = far_z;

	// Window-relative Kooima (ADR-012): screen = actual window physical size,
	// eye positions shifted by window-center offset on monitor.
	Display3DScreen screen;
	float eyeOffsetX_mv = 0.0f, eyeOffsetY_mv = 0.0f;
	if (s_sa.canvas_width > 0 && s_sa.canvas_height > 0 &&
	    s_sa.display_info.display_pixel_width > 0 && s_sa.display_info.display_pixel_height > 0) {
		float pxSizeX = s_sa.display_info.display_width_meters / (float)s_sa.display_info.display_pixel_width;
		float pxSizeY = s_sa.display_info.display_height_meters / (float)s_sa.display_info.display_pixel_height;
		screen.width_m = (float)s_sa.canvas_width * pxSizeX;
		screen.height_m = (float)s_sa.canvas_height * pxSizeY;

		// Shift eyes from display-center to window-center coordinates
		float winCenterX = (float)s_sa.canvas_x + (float)s_sa.canvas_width * 0.5f;
		float winCenterY = (float)s_sa.canvas_y + (float)s_sa.canvas_height * 0.5f;
		float dispCenterX = (float)s_sa.display_info.display_pixel_width * 0.5f;
		float dispCenterY = (float)s_sa.display_info.display_pixel_height * 0.5f;
		eyeOffsetX_mv = (winCenterX - dispCenterX) * pxSizeX;
		eyeOffsetY_mv = (winCenterY - dispCenterY) * pxSizeY;
#ifdef _WIN32
		eyeOffsetY_mv = -eyeOffsetY_mv; // Win32 Y is top-down, eye coords are Y-up
#endif
	} else {
		screen.width_m = s_sa.display_info.display_width_meters;
		screen.height_m = s_sa.display_info.display_height_meters;
	}

	Display3DTunables tunables = s_sa.tunables_set
		? s_sa.tunables
		: display3d_default_tunables();

	XrPosef *pose_ptr = s_sa.display_pose_set ? &s_sa.display_pose : NULL;

	XrVector3f nominal = {
		s_sa.display_info.nominal_viewer_x - eyeOffsetX_mv,
		s_sa.display_info.nominal_viewer_y - eyeOffsetY_mv,
		s_sa.display_info.nominal_viewer_z
	};

	// Build raw eye positions for all requested views.
	// If mode needs more views than xrLocateViews returned (e.g. quad=4
	// but PRIMARY_STEREO=2), duplicate views[0] for extras — matches the
	// reference test app.
	XrVector3f raw_eyes[SA_MAX_VIEWS];
	uint32_t n = view_count;
	if (n > SA_MAX_VIEWS) n = SA_MAX_VIEWS;

	for (uint32_t i = 0; i < n; i++) {
		uint32_t src = (i < s_sa.located_view_count) ? i : 0;
		raw_eyes[i].x = s_sa.eye_positions[src][0] - eyeOffsetX_mv;
		raw_eyes[i].y = s_sa.eye_positions[src][1] - eyeOffsetY_mv;
		raw_eyes[i].z = s_sa.eye_positions[src][2];
	}

	if (s_sa.camera_centric) {
		// Camera-centric: use camera3d library (tangent-space Kooima)
		Camera3DTunables cam_tunables;
		cam_tunables.ipd_factor = tunables.ipd_factor;
		cam_tunables.parallax_factor = tunables.parallax_factor;
		cam_tunables.inv_convergence_distance = s_sa.inv_convergence_distance;
		cam_tunables.half_tan_vfov = s_sa.fov_override;

		Camera3DView cam_views[SA_MAX_VIEWS];
		camera3d_compute_views(
			raw_eyes, n, &nominal, &screen, &cam_tunables,
			pose_ptr, near_z, far_z, cam_views);

		for (uint32_t i = 0; i < n; i++) {
			memcpy(&view_matrices[i * 16], cam_views[i].view_matrix, 16 * sizeof(float));
			memcpy(&proj_matrices[i * 16], cam_views[i].projection_matrix, 16 * sizeof(float));
		}

		// Cache as Display3DView for submit_frame_atlas (layout-compatible fields)
		for (uint32_t i = 0; i < n; i++) {
			memcpy(s_sa.computed_views[i].view_matrix, cam_views[i].view_matrix, 16 * sizeof(float));
			memcpy(s_sa.computed_views[i].projection_matrix, cam_views[i].projection_matrix, 16 * sizeof(float));
			s_sa.computed_views[i].fov = cam_views[i].fov;
			s_sa.computed_views[i].eye_world = cam_views[i].eye_world;
			s_sa.computed_views[i].orientation = cam_views[i].orientation;
		}
		s_sa.computed_view_count = n;
	} else {
		// Display-centric: use display3d library
		Display3DView out_views[SA_MAX_VIEWS];
		display3d_compute_views(
			raw_eyes, n, &nominal, &screen, &tunables,
			pose_ptr, near_z, far_z, out_views);

		for (uint32_t i = 0; i < n; i++) {
			memcpy(&view_matrices[i * 16], out_views[i].view_matrix, 16 * sizeof(float));
			memcpy(&proj_matrices[i * 16], out_views[i].projection_matrix, 16 * sizeof(float));
		}

		// Cache computed views for submit_frame_atlas (FOV + eye_world)
		memcpy(s_sa.computed_views, out_views, n * sizeof(Display3DView));
		s_sa.computed_view_count = n;
	}

	*valid = 1;
}


// ============================================================================
// Public API: Current mode info
// ============================================================================

void
displayxr_standalone_get_current_mode_info(
	uint32_t *view_count,
	uint32_t *tile_columns, uint32_t *tile_rows,
	uint32_t *view_width_pixels, uint32_t *view_height_pixels,
	float *view_scale_x, float *view_scale_y,
	int *hardware_display_3d)
{
	const XrDisplayRenderingModeInfoEXT *mode = get_current_mode();
	uint32_t vc, tc, tr, vw, vh;
	get_render_tiling(mode, &vc, &tc, &tr, &vw, &vh);

	*view_count = vc;
	*tile_columns = tc;
	*tile_rows = tr;
	*view_width_pixels = vw;
	*view_height_pixels = vh;
	*view_scale_x = mode ? mode->viewScaleX : 1.0f;
	*view_scale_y = mode ? mode->viewScaleY : 1.0f;
	*hardware_display_3d = mode ? (int)mode->hardwareDisplay3D : 0;
}


// ============================================================================
// Public API: Tunables + display pose
// ============================================================================

void
displayxr_standalone_set_tunables(
	float ipd_factor, float parallax_factor, float perspective_factor,
	float virtual_display_height, float inv_convergence_distance, float fov_override,
	float near_z, float far_z, int camera_centric)
{
	s_sa.tunables.ipd_factor = ipd_factor;
	s_sa.tunables.parallax_factor = parallax_factor;
	s_sa.tunables.perspective_factor = perspective_factor;
	s_sa.tunables.virtual_display_height = virtual_display_height;
	s_sa.tunables_set = 1;
	s_sa.camera_centric = camera_centric;
	s_sa.inv_convergence_distance = inv_convergence_distance;
	s_sa.fov_override = fov_override;
	// near_z, far_z come from the C# compute_views call directly
	(void)near_z;
	(void)far_z;
}

void
displayxr_standalone_set_display_pose(
	float pos_x, float pos_y, float pos_z,
	float ori_x, float ori_y, float ori_z, float ori_w,
	float scale_x, float scale_y, float scale_z,
	int enabled)
{
	if (enabled) {
		// Convert Unity coords (left-hand, +Z forward) to OpenXR (right-hand, -Z forward):
		// negate position Z, negate quaternion X/Y.
		// Matches hooked_xrLocateViews convention in displayxr_hooks.cpp.
		s_sa.display_pose.position = XrVector3f{pos_x, pos_y, -pos_z};
		s_sa.display_pose.orientation = XrQuaternionf{-ori_x, -ori_y, ori_z, ori_w};
		s_sa.display_pose_set = 1;
		// scale is folded into virtual_display_height by C# side (like the hook chain)
		(void)scale_x;
		(void)scale_y;
		(void)scale_z;
	} else {
		s_sa.display_pose_set = 0;
	}
}


// ============================================================================
// Public API: Queries
// ============================================================================

// Keep old poll() as a convenience wrapper for backwards compat
void
displayxr_standalone_poll(void)
{
	displayxr_standalone_poll_events();
	int should_render = 0;
	if (displayxr_standalone_begin_frame(&should_render)) {
		displayxr_standalone_end_frame_empty();
	}
}


void
displayxr_standalone_get_display_info(float *display_width_m, float *display_height_m,
                                       uint32_t *pixel_width, uint32_t *pixel_height,
                                       float *nominal_x, float *nominal_y, float *nominal_z,
                                       float *scale_x, float *scale_y,
                                       int *is_valid)
{
	DisplayXRDisplayInfo *di = &s_sa.display_info;
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


void
displayxr_standalone_get_eye_positions(float *lx, float *ly, float *lz,
                                        float *rx, float *ry, float *rz,
                                        int *is_tracked)
{
	*lx = s_sa.left_eye[0];
	*ly = s_sa.left_eye[1];
	*lz = s_sa.left_eye[2];
	*rx = s_sa.right_eye[0];
	*ry = s_sa.right_eye[1];
	*rz = s_sa.right_eye[2];
	*is_tracked = s_sa.is_tracked;
}


void
displayxr_standalone_set_unity_device(void *unity_native_tex)
{
#if defined(_WIN32)
	if (!s_sa_backend)
		s_sa_backend = create_standalone_d3d12_backend();
	s_sa_backend->set_unity_device(unity_native_tex);
#else
	(void)unity_native_tex;
#endif
}

void
displayxr_standalone_get_atlas_bridge_texture(void **native_ptr,
                                               uint32_t *width, uint32_t *height)
{
	if (s_sa_backend) {
		*native_ptr = s_sa_backend->get_atlas_bridge_unity_ptr();
	} else {
		*native_ptr = NULL;
	}
	*width = s_sa.atlas.width;
	*height = s_sa.atlas.height;
}


#if defined(__APPLE__)
extern "C" float displayxr_sa_metal_get_backing_scale(void);
#endif

float
displayxr_get_backing_scale_factor(void)
{
#if defined(__APPLE__)
	return displayxr_sa_metal_get_backing_scale();
#elif defined(_WIN32)
	UINT dpi = GetDpiForSystem();
	static int logged = 0;
	if (!logged) {
		sa_log("[DisplayXR-SA] GetDpiForSystem=%u, backingScale=%.2f\n",
		        dpi, (float)dpi / 96.0f);
		logged = 1;
	}
	return (float)dpi / 96.0f;
#else
	return 1.0f;
#endif
}


DISPLAYXR_EXPORT void
displayxr_standalone_get_preview_mouse_state(int *buttons, int *wheel_delta)
{
#if defined(_WIN32)
	if (buttons) *buttons = s_sa.preview_mouse_buttons;
	if (wheel_delta) {
		*wheel_delta = s_sa.preview_wheel_accum;
		s_sa.preview_wheel_accum = 0; // consume
	}
#else
	if (buttons) *buttons = 0;
	if (wheel_delta) *wheel_delta = 0;
#endif
}

DISPLAYXR_EXPORT int
displayxr_standalone_get_preview_mouse_position(float *out_fx, float *out_fy)
{
#if defined(__APPLE__)
	float fx = -1.0f, fy = -1.0f;
	int ok = displayxr_sa_metal_get_preview_mouse_position(&fx, &fy);
	if (out_fx) *out_fx = fx;
	if (out_fy) *out_fy = fy;
	return ok;
#elif defined(_WIN32)
	if (s_sa.preview_mouse_in_content && s_sa.preview_hwnd != NULL) {
		// Convert tracked client-pixel position to fractional coords using
		// the current client-area size.
		RECT rc;
		if (GetClientRect(s_sa.preview_hwnd, &rc) &&
		    rc.right > 0 && rc.bottom > 0) {
			if (out_fx) *out_fx = (float)s_sa.preview_mouse_x / (float)rc.right;
			if (out_fy) *out_fy = (float)s_sa.preview_mouse_y / (float)rc.bottom;
			return 1;
		}
	}
	if (out_fx) *out_fx = -1.0f;
	if (out_fy) *out_fy = -1.0f;
	return 0;
#else
	if (out_fx) *out_fx = -1.0f;
	if (out_fy) *out_fy = -1.0f;
	return 0;
#endif
}

int
displayxr_standalone_window_is_interacting(void)
{
#if defined(__APPLE__)
	return displayxr_sa_metal_window_is_interacting();
#elif defined(_WIN32)
	if (!s_sa.preview_hwnd) return 0;
	POINT pt;
	GetCursorPos(&pt);
	// Check if cursor is over the window at all
	HWND under = WindowFromPoint(pt);
	if (under != s_sa.preview_hwnd) return 0;
	// Check if cursor is in the client (content) area — if so, allow interaction
	RECT clientRect;
	GetClientRect(s_sa.preview_hwnd, &clientRect);
	POINT clientPt = pt;
	ScreenToClient(s_sa.preview_hwnd, &clientPt);
	if (clientPt.x >= 0 && clientPt.y >= 0 &&
	    clientPt.x < clientRect.right && clientPt.y < clientRect.bottom)
		return 0; // Content area — allow camera rotation
	// Cursor is over the frame (title bar, edges) — suppress input
	return 1;
#else
	return 0;
#endif
}

int
displayxr_standalone_window_was_closed(void)
{
#if defined(__APPLE__)
	return displayxr_sa_metal_window_was_closed();
#elif defined(_WIN32)
	int closed = s_sa.window_closed;
	s_sa.window_closed = 0;
	return closed;
#else
	return 0;
#endif
}


// displayxr_standalone_set_canvas_rect has been removed — the runtime
// manages its own window and determines canvas rect from the window size.


void
displayxr_standalone_get_swapchain_size(uint32_t *width, uint32_t *height)
{
	if (s_sa.atlas_created) {
		*width = s_sa.atlas.width;
		*height = s_sa.atlas.height;
	} else if (s_sa.display_info.is_valid) {
		// Estimate atlas size from display info
		*width = s_sa.display_info.display_pixel_width * 2;
		*height = s_sa.display_info.display_pixel_height;
	} else {
		*width = 0;
		*height = 0;
	}
}


// ============================================================================
// Public API: Display mode switching
// ============================================================================

int
displayxr_standalone_request_display_mode(int mode_3d)
{
	if (!s_sa.has_display_mode_ext || !s_sa.pfn_request_display_mode ||
	    s_sa.session == XR_NULL_HANDLE)
		return 0;

	XrDisplayModeEXT mode = mode_3d ? XR_DISPLAY_MODE_3D_EXT : XR_DISPLAY_MODE_2D_EXT;
	XrResult result = s_sa.pfn_request_display_mode(s_sa.session, mode);
	sa_log("[DisplayXR-SA] RequestDisplayMode(%s) → %d\n",
		mode_3d ? "3D" : "2D", result);
	return XR_SUCCEEDED(result) ? 1 : 0;
}

int
displayxr_standalone_request_rendering_mode(uint32_t mode_index)
{
	if (!s_sa.pfn_request_rendering_mode || s_sa.session == XR_NULL_HANDLE)
		return 0;

	XrResult result = s_sa.pfn_request_rendering_mode(s_sa.session, mode_index);
	sa_log("[DisplayXR-SA] RequestRenderingMode(%u) → %d\n",
		mode_index, result);
	if (XR_SUCCEEDED(result)) {
		s_sa.current_rendering_mode_index = mode_index;
		return 1;
	}
	return 0;
}

int
displayxr_standalone_enumerate_rendering_modes(
	uint32_t capacity, uint32_t *count,
	uint32_t *mode_indices, char (*mode_names)[256],
	uint32_t *view_counts,
	uint32_t *tile_columns, uint32_t *tile_rows,
	uint32_t *view_width_pixels, uint32_t *view_height_pixels,
	float *view_scale_x, float *view_scale_y,
	int *hardware_display_3d)
{
	// Use cached modes if available, otherwise query runtime
	uint32_t total = s_sa.rendering_mode_count;
	if (total == 0) {
		if (!s_sa.pfn_enumerate_rendering_modes || s_sa.session == XR_NULL_HANDLE) {
			*count = 0;
			return 0;
		}
		// Re-enumerate
		enumerate_and_store_modes();
		total = s_sa.rendering_mode_count;
	}

	if (total == 0) {
		*count = 0;
		return 0;
	}

	*count = total;
	if (capacity == 0 || !mode_indices || !mode_names)
		return 1; // Count-only query

	uint32_t to_fetch = total < capacity ? total : capacity;
	for (uint32_t i = 0; i < to_fetch; i++) {
		mode_indices[i] = s_sa.rendering_modes[i].modeIndex;
		strncpy(mode_names[i], s_sa.rendering_modes[i].modeName, 255);
		mode_names[i][255] = '\0';

		// Extended fields (NULL-safe — callers may pass NULL for fields they don't need)
		if (view_counts) view_counts[i] = s_sa.rendering_modes[i].viewCount;
		if (tile_columns) tile_columns[i] = s_sa.rendering_modes[i].tileColumns;
		if (tile_rows) tile_rows[i] = s_sa.rendering_modes[i].tileRows;
		if (view_width_pixels) view_width_pixels[i] = s_sa.rendering_modes[i].viewWidthPixels;
		if (view_height_pixels) view_height_pixels[i] = s_sa.rendering_modes[i].viewHeightPixels;
		if (view_scale_x) view_scale_x[i] = s_sa.rendering_modes[i].viewScaleX;
		if (view_scale_y) view_scale_y[i] = s_sa.rendering_modes[i].viewScaleY;
		if (hardware_display_3d) hardware_display_3d[i] = (int)s_sa.rendering_modes[i].hardwareDisplay3D;
	}
	return 1;
}
