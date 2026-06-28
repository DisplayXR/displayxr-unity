// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
//
// OpenXR function interception layer for the DisplayXR Unity plugin.
// Hooks into Unity's OpenXR loader chain via HookGetInstanceProcAddr.

#include <cstdlib>   // calloc/free — explicit; macOS clang/libc++ no longer pulls these in transitively
#include <cstring>   // strcmp — same reason
#include "displayxr_hooks_internal.h"
#include "displayxr_window_space_ui.h"
#include "displayxr_local2d.h"

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

// --- Active graphics backend (selected at xrCreateSession time) ---
static GraphicsBackend *s_backend = nullptr;

GraphicsBackend *displayxr_get_hooked_backend() { return s_backend; }
// displayxr_get_hooked_session lives further down (after s_session is declared).

// Thin wrappers so callers in other TUs can read rendering-mode state on
// the active GraphicsBackend without needing the full class definition (which
// drags in Win32 headers via displayxr_hooks_internal.h → displayxr_win32.h).
uint32_t displayxr_hooked_get_rendering_mode_count(GraphicsBackend *b)
{
	return b ? b->rendering_mode_count : 0;
}
const XrDisplayRenderingModeInfoEXT *displayxr_hooked_get_rendering_modes(GraphicsBackend *b)
{
	return b ? b->rendering_modes : nullptr;
}
XrResult displayxr_hooked_request_rendering_mode(GraphicsBackend *b, XrSession s, uint32_t mode_index)
{
	return b ? b->request_rendering_mode(s, mode_index) : XR_ERROR_FUNCTION_UNSUPPORTED;
}
enum BackendType { kBackendNone, kBackendD3D11, kBackendD3D12, kBackendMetal, kBackendVulkan };
static BackendType s_backend_type = kBackendNone;

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

static XrInstance s_instance = XR_NULL_HANDLE;
static XrSession s_session = XR_NULL_HANDLE;
XrSession displayxr_get_hooked_session() { return s_session; }
static XrSpace s_local_space = XR_NULL_HANDLE;
// Private VIEW reference space used solely for Unity URP head-pose compensation
// in hooked_xrLocateViews. The runtime returns xrLocateSpace(s_view_space,
// s_local_space) = the current head pose in LOCAL coords, which we add to
// views[i].pose.position so Unity's URP "eye_world = pose - head_pose" math
// resolves back to Kooima eye_world. See issue #115.
static XrSpace s_view_space = XR_NULL_HANDLE;
static volatile int s_session_alive = 0; // Guard for teardown
static volatile int s_instance_alive = 0; // Guard for post-destroy polling

// --- Deferred destruction ---
// Unity's OpenXR loader calls xrPollEvent AFTER xrDestroyInstance returns,
// through JIT-generated dispatch trampolines that reference runtime memory.
// If we actually destroy the instance, those trampolines read freed pages → SIGSEGV.
// Fix: defer the real destroy calls until the next instance lifecycle begins.
static XrSession s_deferred_destroy_session = XR_NULL_HANDLE;
static PFN_xrDestroySession s_deferred_destroy_session_fn = nullptr;
static XrInstance s_deferred_destroy_instance = XR_NULL_HANDLE;
static PFN_xrDestroyInstance s_deferred_destroy_instance_fn = nullptr;
static int s_runtime_pinned = 0; // Whether we've pinned the runtime via RTLD_NODELETE
static volatile int s_stop_polling = 0; // Stop forwarding xrPollEvent after EXITING event
static PFN_xrSetSharedTextureOutputRectEXT s_pfn_set_output_rect = nullptr;
static int s_native_viewport_active = 0; // WM_SIZE (native) is authoritative source

// Canvas sub-rect (#34 / #131): when an app opts in (via displayxr_set_canvas_rect),
// the 3D content weaves into this HWND-client sub-rect instead of the full window,
// leaving the rest as the 2D surround region. Re-applied each xrEndFrame (matches
// the cube_texture reference). Default invalid → full-window canvas (no change).
static int s_canvas_rect_valid = 0;
static int32_t s_canvas_rect_x = 0, s_canvas_rect_y = 0;
static uint32_t s_canvas_rect_w = 0, s_canvas_rect_h = 0;

// 2D surround (#131): the runtime fills the non-canvas region post-weave from an
// app-supplied full-window 2D texture (xrSetSharedTextureSurround2DFenceEXT, v7).
// C# registers a Unity RenderTexture pointer; hooked_xrEndFrame copies it into a
// SHARED surround texture (via the backend), signals a SHARED fence, and registers
// the handles + value each frame. Default null → no surround (no change).
static PFN_xrSetSharedTextureSurround2DFenceEXT s_pfn_set_surround_fence = nullptr;
static void * volatile s_surround_unity_tex = nullptr;
static volatile uint32_t s_surround_w = 0, s_surround_h = 0;

// XR_EXT_atlas_capture (#140 / #396 W6): runtime-owned atlas screenshot. Resolved
// alongside the other extension entry points in hooked_xrGetSystemProperties; the
// 'I'-key screenshot (DisplayXRScreenshot.cs → DisplayXRFeature.CaptureAtlas →
// displayxr_capture_atlas) routes through this instead of an app-side GPU readback.
static PFN_xrCaptureAtlasEXT s_pfn_capture_atlas = nullptr;

// Max view count handled by hooked_xrLocateViews. The DisplayXR runtime
// advertises max-view-count across all render modes for the active display
// (e.g. sim_display = 4 because of its quad mode; lenticular displays could
// be 8+). Bump if a higher-N display ships. Matches DisplayXRGizmoHelpers
// .MAX_VIEWS on the C# side so gizmo + native agree on the cap.
#define DISPLAYXR_HOOKS_MAX_VIEWS 16

// XR_EXT_view_rig (#396): set in the xrGetSystemProperties hook when the runtime
// advertises the extension. When set, hooked_xrLocateViews chains a rig descriptor
// and consumes the runtime's render-ready XrView{pose,fov} instead of computing
// Kooima locally (the legacy display3d/camera3d path is the fallback).
static int s_has_view_rig = 0;

// XR_EXT_display_zones: 3D content framed to a window-pixel zone rect instead of
// the full window + surround crop. Detected (extension present) in the
// xrGetSystemProperties hook alongside view_rig, with the two entry points
// resolved there too. Caps are queried lazily on the first frame a zone rect is
// set (the query needs a live session). When a zone rect is set
// (displayxr_set_3d_zone_rect) AND caps are OK, hooked_xrLocateViews chains an
// XrDisplayZoneEXT in front of the rig (zone-scoped Kooima) and hooked_xrEndFrame
// chains the same zone on each projection layer (binding its views into the
// rect). Requires view_rig — zones compose on top of it.
static int s_has_display_zones = 0;
static PFN_xrGetDisplayZoneCapabilitiesEXT s_pfn_get_zone_caps = nullptr;
static PFN_xrGetDisplayZoneRecommendedViewSizeEXT s_pfn_get_zone_view_size = nullptr;
static int s_zone_caps_ok = -1; // -1 untried, 0 unsupported, 1 supported (maxZones3D>=1)

// App-supplied 3D-zone rect (client-window pixels, top-left origin), pushed via
// displayxr_set_3d_zone_rect / cleared via displayxr_clear_3d_zone. When valid
// (and zones are supported) the locate + endframe hooks scope the 3D to it.
static volatile int s_zone_valid = 0;
static volatile int32_t s_zone_x = 0, s_zone_y = 0;
static volatile int32_t s_zone_w = 0, s_zone_h = 0;

// Lazy caps query: zones are supported iff the runtime advertised the extension,
// resolved the caps entry point, and reports supported && maxZones3D>=1. Returns
// nonzero when the zone path may run this frame (ext + caps + view_rig).
static int dxr_zones_ready(XrSession session) {
	if (!s_has_display_zones || !s_has_view_rig || !s_pfn_get_zone_caps)
		return 0;
	if (s_zone_caps_ok < 0) {
		if (session == XR_NULL_HANDLE) return 0;
		XrDisplayZoneCapabilitiesEXT caps = {XR_TYPE_DISPLAY_ZONE_CAPABILITIES_EXT};
		XrResult cr = s_pfn_get_zone_caps(session, &caps);
		s_zone_caps_ok = (XR_SUCCEEDED(cr) && caps.supported && caps.maxZones3D >= 1) ? 1 : 0;
		displayxr_log("[DisplayXR] display-zones caps: result=0x%x supported=%d maxZones3D=%u -> %s\n",
		              (unsigned)cr, (int)caps.supported, caps.maxZones3D,
		              s_zone_caps_ok ? "ACTIVE" : "unsupported (legacy single-canvas path)");
	}
	return s_zone_caps_ok > 0;
}

// XR_EXT_view_rig: render-ready XrView{pose,fov} -> renderer matrices. Same
// convention the displayxr::math rigs emit (OpenXR view matrix = R^T*translate(-p);
// GL [-1,1] off-axis projection), so the BiRP C# shim's FlipViewZ stays correct.
static void dxr_view_matrix_from_pose(const XrPosef *pose, float *out) {
	const float qx = pose->orientation.x, qy = pose->orientation.y;
	const float qz = pose->orientation.z, qw = pose->orientation.w;
	float rot[16] = {};
	rot[0] = 1.0f - 2.0f * (qy * qy + qz * qz);
	rot[1] = 2.0f * (qx * qy + qz * qw);
	rot[2] = 2.0f * (qx * qz - qy * qw);
	rot[4] = 2.0f * (qx * qy - qz * qw);
	rot[5] = 1.0f - 2.0f * (qx * qx + qz * qz);
	rot[6] = 2.0f * (qy * qz + qx * qw);
	rot[8] = 2.0f * (qx * qz + qy * qw);
	rot[9] = 2.0f * (qy * qz - qx * qw);
	rot[10] = 1.0f - 2.0f * (qx * qx + qy * qy);
	rot[15] = 1.0f;
	for (int i = 0; i < 16; i++) out[i] = 0.0f;
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++)
			out[j * 4 + i] = rot[i * 4 + j]; // R^T
	out[15] = 1.0f;
	out[12] = -(out[0] * pose->position.x + out[4] * pose->position.y + out[8] * pose->position.z);
	out[13] = -(out[1] * pose->position.x + out[5] * pose->position.y + out[9] * pose->position.z);
	out[14] = -(out[2] * pose->position.x + out[6] * pose->position.y + out[10] * pose->position.z);
}

static void dxr_projection_from_fov(const XrFovf *fov, float near_z, float far_z, float *out) {
	const float l = tanf(fov->angleLeft) * near_z;
	const float r = tanf(fov->angleRight) * near_z;
	const float b = tanf(fov->angleDown) * near_z;
	const float t = tanf(fov->angleUp) * near_z;
	for (int i = 0; i < 16; i++) out[i] = 0.0f;
	out[0] = 2.0f * near_z / (r - l);
	out[5] = 2.0f * near_z / (t - b);
	out[8] = (r + l) / (r - l);
	out[9] = (t + b) / (t - b);
	out[10] = -(far_z + near_z) / (far_z - near_z);
	out[11] = -1.0f;
	out[14] = -2.0f * far_z * near_z / (far_z - near_z);
}

// Foreground-only clip (#57 family): the eye's distance to the display plane =
// z of (rigPose^-1 * eyeWorld). Used as the per-view far so geometry behind the
// display plane is clipped. Degenerates to eyeWorld.z at an identity rig pose.
static float dxr_rig_local_eye_z(const XrPosef *rig, const XrVector3f *eye_world) {
	const float dx = eye_world->x - rig->position.x;
	const float dy = eye_world->y - rig->position.y;
	const float dz = eye_world->z - rig->position.z;
	const float qx = -rig->orientation.x, qy = -rig->orientation.y;
	const float qz = -rig->orientation.z, qw = rig->orientation.w;
	const float cx = qy * dz - qz * dy + qw * dx;
	const float cy = qz * dx - qx * dz + qw * dy;
	return dz + 2.0f * (qx * cy - qy * cx);
}

// ============================================================================
// Intercepted OpenXR functions
// ============================================================================

static XrResult XRAPI_CALL
hooked_xrLocateViews(XrSession session,
                     const XrViewLocateInfo *viewLocateInfo,
                     XrViewState *viewState,
                     uint32_t viewCapacityInput,
                     uint32_t *viewCountOutput,
                     XrView *views)
{
	// Guard: skip if session is being torn down
	if (!s_session_alive) {
		return s_real_locate_views(session, viewLocateInfo, viewState, viewCapacityInput, viewCountOutput,
		                           views);
	}

#if defined(_WIN32)
	// Shell mode: per-frame maintenance for viewport and input focus.
	if (displayxr_is_shell_mode()) {
		DisplayXRState *poll_state = displayxr_get_state();
		if (poll_state->window_handle != nullptr) {
			HWND hwnd = (HWND)poll_state->window_handle;

			// Keep Unity rendering under the shell (#unity-black).
			//
			// Unlike a native Win32 render loop (e.g. cube_handle_d3d12, which
			// renders regardless of window state), Unity's player does NOT run
			// the camera/scene render while its main window lacks WS_VISIBLE —
			// it still ticks scripts (Application.runInBackground) and submits
			// the XR frame, but the eye textures are never drawn, so the
			// swapchain the shell composites stays black.
			//
			// In workspace/shell mode the app's HWND gets hidden because the
			// shell shows the composited content via the IPC swapchain, not the
			// app's own window. It is hidden in (at least) TWO places — the
			// runtime at session create (`ShowWindow SW_HIDE` in
			// oxr_session.c) AND the shell's own window management at workspace
			// takeover (verified: fixing only the runtime site is undone by the
			// shell, which re-hides + repositions the window). That hide is
			// exactly what stops Unity from rendering.
			//
			// Resolve it from the app process — the one place that sees every
			// hide regardless of who issued it — by keeping the window VISIBLE
			// but parked far OFF-SCREEN: Unity resumes rendering (verified:
			// off-screen + visible draws the full scene into the swapchain) and
			// the user never sees the bare window.
			//
			// Two failure modes are corrected here, every frame, until both are
			// satisfied (then this is a no-op):
			//   * HIDDEN      → the engine stops rendering → black.
			//   * ON-SCREEN   → the bare app window would be visible to the user
			//                   (the shell repositions it back on-screen after
			//                   hiding it, so a visibility-only gate isn't
			//                   enough).
			// "On-screen" uses a generous off-screen threshold (-8000): the
			// shell/runtime sometimes parks the window at its own off-screen
			// spot (e.g. -12800, the modal-owner convention). Treating that as
			// already-parked avoids a per-frame tug-of-war between our -32000
			// and the shell's -12800 (both invisible, so the fight is harmless,
			// but pointless). Only a window actually near/on a monitor re-parks.
			//
			// CRITICAL: this runs on Unity's RENDER thread (xrLocateViews), but
			// the HWND is owned by the main thread. A synchronous SetWindowPos /
			// ShowWindow here SendMessages cross-thread and DEADLOCKs (verified:
			// app stalled at frame 1); an *async* SetWindowPos from the render
			// thread did not reliably move the window (only the show took, so it
			// stayed on-screen). So delegate to the main thread:
			// displayxr_shell_park_offscreen() PostMessages a request that the
			// window subclass handles synchronously on the main thread, where
			// the move actually sticks. GetWindowRect / IsWindowVisible are
			// read-only and safe to call from here.
			RECT park_rc;
			bool on_screen = GetWindowRect(hwnd, &park_rc) && park_rc.left > -8000;
			if (!IsWindowVisible(hwnd) || on_screen) {
				displayxr_shell_park_offscreen(hwnd);
				static int s_parked_logged = 0;
				if (!s_parked_logged) {
					s_parked_logged = 1;
					displayxr_log("[DisplayXR] Shell mode: requesting off-screen + visible park "
					              "of app HWND %p so Unity keeps rendering (hidden/repositioned by "
					              "the runtime or shell)\n",
					              hwnd);
				}
			}

			// Poll window size/position since we have no parent_subclass_proc.
			// Keeps Kooima projection correct when the shell resizes the app.
			RECT rc;
			if (GetClientRect(hwnd, &rc)) {
				uint32_t w = (uint32_t)(rc.right - rc.left);
				uint32_t h = (uint32_t)(rc.bottom - rc.top);
				if (w > 0 && h > 0 &&
				    (w != poll_state->viewport_width || h != poll_state->viewport_height ||
				     !s_native_viewport_active)) {
					POINT origin = {0, 0};
					ClientToScreen(hwnd, &origin);
					displayxr_set_viewport_size_native(w, h,
						(int32_t)origin.x, (int32_t)origin.y);
				}
			}
		}
	}
#elif defined(__APPLE__)
	// macOS built apps: poll the overlay window's screen rect each frame so
	// the window-relative Kooima math below sees the actual screen position.
	// C# DisplayXRDisplay/DisplayXRCamera only know Screen.width/height and
	// hard-code (0, 0) for the screen position; native is the only place
	// that has the NSWindow handle. Mirror the Windows shell-mode pattern.
	{
		int32_t wx, wy;
		uint32_t ww, wh;
		if (displayxr_metal_get_app_window_rect(&wx, &wy, &ww, &wh) &&
		    ww > 0 && wh > 0) {
			displayxr_set_viewport_size_native(ww, wh, wx, wy);
		}
	}
#endif

	// Use our LOCAL space if available, otherwise pass through the original space.
	// LOCAL space gives us raw eye positions relative to the display.
	XrViewLocateInfo modified_info = *viewLocateInfo;
	if (s_local_space != XR_NULL_HANDLE) {
		modified_info.space = s_local_space;
	}

	// XR_EXT_view_rig (#396): when the runtime owns the Kooima math, chain a rig
	// descriptor so the real locate returns render-ready XrView{pose,fov}, plus the
	// raw-eyes channel for the gizmo/eye cache. Built from the same tunables + scene
	// pose the legacy compute uses (below). Anisotropic scene scale (non-uniform
	// lossyScale) is NOT expressible via the rig API — uniform scale is exact; this
	// is a known #396 limitation. The post-process + early-return is after the locate.
	XrDisplayRigEXT rig_display = {XR_TYPE_DISPLAY_RIG_EXT};
	XrCameraRigEXT rig_camera = {XR_TYPE_CAMERA_RIG_EXT};
	XrViewDisplayRawEXT rig_raw = {XR_TYPE_VIEW_DISPLAY_RAW_EXT};
	XrDisplayZoneEXT rig_zone = {XR_TYPE_DISPLAY_ZONE_EXT}; // XR_EXT_display_zones (#zones)
	XrPosef rig_pose_world = {{0, 0, 0, 1}, {0, 0, 0}}; // display-plane / camera pose (for foreground clip)
	bool rig_chained = false;
	if (s_has_view_rig) {
		DisplayXRTunables rt = displayxr_state_get_tunables();
		DisplayXRSceneTransform rx = displayxr_state_get_scene_transform();
		DisplayXRState *rs = displayxr_get_state();
		if (rs->display_info.is_valid) {
			if (rx.enabled) {
				rig_pose_world.position = XrVector3f{rx.position[0], rx.position[1], -rx.position[2]};
				rig_pose_world.orientation = XrQuaternionf{-rx.orientation[0], -rx.orientation[1],
				                               rx.orientation[2], rx.orientation[3]};
			}
			float ssy = (rx.enabled && rx.scale[1] > 0.001f) ? rx.scale[1] : 1.0f;
			float ssz = (rx.enabled && rx.scale[2] > 0.001f) ? rx.scale[2] : 1.0f;
			if (rt.camera_centric) {
				rig_camera.pose = rig_pose_world;
				rig_camera.ipdFactor = rt.ipd_factor;
				rig_camera.parallaxFactor = rt.parallax_factor;
				rig_camera.convergenceDiopters = rt.inv_convergence_distance / ssz;
				rig_camera.verticalFov = 2.0f * atanf(rt.fov_override);
				// Spec v3: scene scale is already folded into convergenceDiopters
				// (/ssz), so pass identity meters->world to preserve pre-v3 behavior.
				rig_camera.metersToVirtual = 1.0f;
				modified_info.next = &rig_camera;
			} else {
				rig_display.pose = rig_pose_world;
				rig_display.virtualDisplayHeight = rt.virtual_display_height / ssy;
				rig_display.ipdFactor = rt.ipd_factor;
				rig_display.parallaxFactor = rt.parallax_factor;
				rig_display.perspectiveFactor = rt.perspective_factor;
				modified_info.next = &rig_display;
			}

			// XR_EXT_display_zones: scope the rig's Kooima framing to the app's
			// zone rect (the rect IS the canvas) by chaining the zone in FRONT of
			// the rig. The runtime returns render-ready XrView{pose,fov} framed to
			// the rect, and reports the resolved canvas via rig_raw.canvasRectPx.
			// Gated on caps (queried lazily here — session is live). Mutually
			// exclusive with the canvas-rect/surround path (inert in a zones frame).
			if (s_zone_valid && dxr_zones_ready(session)) {
				rig_zone.zoneId = 1;
				rig_zone.rect.offset = {(int32_t)s_zone_x, (int32_t)s_zone_y};
				rig_zone.rect.extent = {(int32_t)s_zone_w, (int32_t)s_zone_h};
				rig_zone.next = modified_info.next; // the rig
				modified_info.next = &rig_zone;
			}

			rig_raw.next = (void *)viewState->next;
			viewState->next = &rig_raw;
			rig_chained = true;
		}
	}

	// Call the real xrLocateViews
	XrResult result = s_real_locate_views(session, &modified_info, viewState, viewCapacityInput, viewCountOutput,
	                                      views);
	if (XR_FAILED(result) || viewCapacityInput < 2 || views == nullptr) {
		return result;
	}

	uint32_t count = *viewCountOutput;
	if (count < 2) {
		return result;
	}

	// SPI diagnostic (experiment/spi-single-pass): log the RAW per-view poses + fovs
	// the runtime just returned, BEFORE any of our processing. This localizes the
	// single-pass disparity collapse: run the SAME native build in SPI vs MultiPass
	// (C# render mode) and compare here. If v0 != v1 in BOTH modes, the runtime
	// returns distinct eyes and the collapse is Unity-OpenXR-side (it loses the
	// separation before C# sees it under single-pass). If v0 == v1 under SPI but
	// distinct under MultiPass, the runtime itself returns identical views for the
	// single SPI locate (runtime-side). Throttled to the first few frames.
	{
		static int s_locate_diag = 0;
		if (s_locate_diag < 6) {
			s_locate_diag++;
			displayxr_log("[DisplayXR][locate-diag] capIn=%u countOut=%u "
			              "v0.pos=(%.4f,%.4f,%.4f) v1.pos=(%.4f,%.4f,%.4f) dPosX=%.4f "
			              "v0.fov(L,R)=(%.4f,%.4f) v1.fov(L,R)=(%.4f,%.4f) dFovL=%.4f\n",
			              viewCapacityInput, count,
			              views[0].pose.position.x, views[0].pose.position.y, views[0].pose.position.z,
			              views[1].pose.position.x, views[1].pose.position.y, views[1].pose.position.z,
			              views[1].pose.position.x - views[0].pose.position.x,
			              views[0].fov.angleLeft, views[0].fov.angleRight,
			              views[1].fov.angleLeft, views[1].fov.angleRight,
			              views[1].fov.angleLeft - views[0].fov.angleLeft);
		}
	}

	// Sim_display advertises max-view-count over all render modes (quad => 4),
	// even in 2D / anaglyph where it replicates view 0 into the extras. We
	// compute Kooima for all `count` views: replica raw eyes propagate to
	// replica projections through the math automatically. URP reads each
	// XRPass.GetProjMatrix from views[i].fov, so all of them must be set.
	if (count > DISPLAYXR_HOOKS_MAX_VIEWS) {
		static int s_clamp_warned = 0;
		if (!s_clamp_warned) {
			s_clamp_warned = 1;
			displayxr_log("[DisplayXR] xrLocateViews: viewCount=%u exceeds "
			              "DISPLAYXR_HOOKS_MAX_VIEWS=%d, clamping (extra views "
			              "will keep runtime defaults)\n",
			              count, DISPLAYXR_HOOKS_MAX_VIEWS);
		}
		count = DISPLAYXR_HOOKS_MAX_VIEWS;
	}

	// XR_EXT_view_rig (#396): the runtime already returned render-ready views[i]
	// (Kooima + window/canvas resolve done runtime-side). Cache raw eyes from the
	// raw channel for the gizmo, build the BiRP view-shim matrices (view from the
	// render-ready pose; proj from fov — BiRP itself reads views[i].fov directly,
	// Probe A), apply the #115 URP head-pose comp, and pass the views through. The
	// legacy display3d/camera3d compute below is skipped (it's the no-EXT fallback).
	if (rig_chained) {
		DisplayXRTunables rt = displayxr_state_get_tunables();
		uint8_t rtracked = rig_raw.isTracking ? 1 : 0;
		if (rig_raw.eyeCountOutput >= 2)
			displayxr_state_set_eye_positions(&rig_raw.rawEyes[0], &rig_raw.rawEyes[1], rtracked);
		else
			displayxr_state_set_eye_positions(&views[0].pose.position, &views[1].pose.position, rtracked);

		// XR_EXT_display_zones: log the canvas the runtime resolved for the zone
		// (verification — should match the requested rect on the panel). Throttled.
		if (s_zone_valid && dxr_zones_ready(session)) {
			static int s_zone_loc_count = 0;
			if (s_zone_loc_count % 120 == 0) {
				displayxr_log("[DisplayXR] zone locate: req=(%d,%d %dx%d) -> "
				              "canvasRectPx=(%d,%d %dx%d)\n",
				              (int)s_zone_x, (int)s_zone_y, (int)s_zone_w, (int)s_zone_h,
				              rig_raw.canvasRectPx.offset.x, rig_raw.canvasRectPx.offset.y,
				              rig_raw.canvasRectPx.extent.width, rig_raw.canvasRectPx.extent.height);
			}
			s_zone_loc_count++;
		}

		// BiRP view-matrix shim (handedness): built from the render-ready pose
		// BEFORE the #115 comp below, so BiRP gets the eye_world view and URP gets
		// the comp'd pose. Projection: the rig fov is clip-independent, so near/far
		// stay app-side. For foreground-only clip (#57 family) rebuild the per-view
		// far at the eye's display-plane distance (display rig = |rig-local eye Z|;
		// camera rig = the convergence distance) so geometry behind the plane is
		// clipped — matches the runtime test apps. The C# shim applies this
		// projection ONLY when foregroundOnlyClip is set; otherwise BiRP reads
		// views[i].fov directly (Probe A) and this projection is unused.
		DisplayXRStereoMatrices mats = {};
		dxr_view_matrix_from_pose(&views[0].pose, mats.left_view);
		dxr_view_matrix_from_pose(&views[1].pose, mats.right_view);
		for (int e = 0; e < 2; e++) {
			float far_eff = rt.far_z;
			if (rt.clip_at_display_plane) {
				if (rt.camera_centric) {
					if (rt.inv_convergence_distance > 1e-4f)
						far_eff = 1.0f / rt.inv_convergence_distance;
				} else {
					float ez = fabsf(dxr_rig_local_eye_z(&rig_pose_world, &views[e].pose.position));
					if (ez > rt.near_z + 0.001f) far_eff = ez;
				}
			}
			dxr_projection_from_fov(&views[e].fov, rt.near_z, far_eff,
			                        e == 0 ? mats.left_projection : mats.right_projection);
		}
		mats.valid = 1;
		displayxr_state_set_stereo_matrices(&mats);

		// #115 URP head-pose comp: add the runtime head pose so URP's
		// (pose - head_pose) subtraction lands back on the render-ready eye.
		if (s_view_space != XR_NULL_HANDLE && s_local_space != XR_NULL_HANDLE) {
			XrSpaceLocation hl = {XR_TYPE_SPACE_LOCATION};
			if (XR_SUCCEEDED(s_real_locate_space(s_view_space, s_local_space,
			                                     viewLocateInfo->displayTime, &hl)) &&
			    (hl.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) {
				for (uint32_t i = 0; i < count; i++) {
					views[i].pose.position.x += hl.pose.position.x;
					views[i].pose.position.y += hl.pose.position.y;
					views[i].pose.position.z += hl.pose.position.z;
				}
			}
		}
		return result;
	}

	// XR_EXT_view_rig (#396 W7): this build delegates the Kooima math to the
	// runtime. Without the extension there is NO local fallback — pass the raw
	// views through (loud one-shot WARN) and cache raw eyes for the gizmo. Update
	// the DisplayXR runtime to one that advertises XR_EXT_view_rig.
	static int s_no_rig_warned = 0;
	if (!s_no_rig_warned) {
		s_no_rig_warned = 1;
		displayxr_log("[DisplayXR] xrLocateViews: runtime lacks XR_EXT_view_rig — "
		              "stereo projection disabled (raw passthrough).\n");
	}
	uint8_t tracked = (viewState->viewStateFlags & XR_VIEW_STATE_POSITION_TRACKED_BIT) != 0;
	displayxr_state_set_eye_positions(&views[0].pose.position, &views[1].pose.position, tracked);
	return result;
}

static XrResult XRAPI_CALL
hooked_xrGetSystemProperties(XrInstance instance, XrSystemId systemId, XrSystemProperties *properties)
{
	// Inject XrDisplayInfoEXT into the next chain so the runtime fills it in.
	// Unity's OpenXR loader doesn't know about this extension, so we chain it ourselves.
	static XrDisplayInfoEXT display_info_ext = {};
	display_info_ext.type = XR_TYPE_DISPLAY_INFO_EXT;
	display_info_ext.next = (XrBaseOutStructure *)properties->next;
	properties->next = &display_info_ext;

	// Call real function first
	XrResult result = s_real_get_system_properties(instance, systemId, properties);
	if (XR_FAILED(result)) {
		return result;
	}

	// Walk the next chain looking for XrDisplayInfoEXT
	void *next = properties->next;
	while (next != nullptr) {
		XrBaseOutStructure *base = (XrBaseOutStructure *)next;
		if (base->type == XR_TYPE_DISPLAY_INFO_EXT) {
			XrDisplayInfoEXT *di = (XrDisplayInfoEXT *)base;
			DisplayXRState *state = displayxr_get_state();

			state->display_info.display_width_meters = di->displaySizeMeters.width;
			state->display_info.display_height_meters = di->displaySizeMeters.height;
			state->display_info.display_pixel_width = di->displayPixelWidth;
			state->display_info.display_pixel_height = di->displayPixelHeight;
			state->display_info.nominal_viewer_x = di->nominalViewerPositionInDisplaySpace.x;
			state->display_info.nominal_viewer_y = di->nominalViewerPositionInDisplaySpace.y;
			state->display_info.nominal_viewer_z = di->nominalViewerPositionInDisplaySpace.z;
			state->display_info.recommended_view_scale_x = di->recommendedViewScaleX;
			state->display_info.recommended_view_scale_y = di->recommendedViewScaleY;
			state->display_info.is_valid = 1;

			displayxr_log( "[DisplayXR] xrGetSystemProperties: display=%ux%u, %.3fx%.3fm\n",
			        di->displayPixelWidth, di->displayPixelHeight,
			        di->displaySizeMeters.width, di->displaySizeMeters.height);

			// Editor mode: create a native preview window for the runtime.
			// Built apps auto-detect the app's window in xrCreateSession.
#if defined(__APPLE__)
			if (state->editor_mode &&
			    state->window_handle == nullptr &&
			    di->displayPixelWidth > 0 && di->displayPixelHeight > 0) {
				void *view = displayxr_metal_create_preview_window(
				    di->displayPixelWidth, di->displayPixelHeight);
				if (view != nullptr) {
					state->window_handle = view;
					displayxr_log("[DisplayXR] Preview window created: %ux%u\n",
					        di->displayPixelWidth, di->displayPixelHeight);
				}
			}
#endif

			// Look up display mode function (always try — deprecated but still supported)
			if (s_next_gipa && s_instance) {
				PFN_xrVoidFunction fn = nullptr;
				if (XR_SUCCEEDED(s_next_gipa(s_instance, "xrRequestDisplayModeEXT", &fn)) && fn) {
					state->pfn_request_display_mode = (PFN_xrRequestDisplayModeEXT)fn;
					state->has_display_mode_ext = 1;
				}
				fn = nullptr;
				if (XR_SUCCEEDED(s_next_gipa(s_instance, "xrSetSharedTextureOutputRectEXT", &fn)) && fn) {
					s_pfn_set_output_rect = (PFN_xrSetSharedTextureOutputRectEXT)fn;
					displayxr_log( "[DisplayXR] Resolved xrSetSharedTextureOutputRectEXT\n");
				}
				fn = nullptr;
				if (XR_SUCCEEDED(s_next_gipa(s_instance, "xrSetSharedTextureSurround2DFenceEXT", &fn)) && fn) {
					s_pfn_set_surround_fence = (PFN_xrSetSharedTextureSurround2DFenceEXT)fn;
					displayxr_log( "[DisplayXR] Resolved xrSetSharedTextureSurround2DFenceEXT\n");
				}
				fn = nullptr;
				if (XR_SUCCEEDED(s_next_gipa(s_instance, "xrCaptureAtlasEXT", &fn)) && fn) {
					s_pfn_capture_atlas = (PFN_xrCaptureAtlasEXT)fn;
					displayxr_log( "[DisplayXR] Resolved xrCaptureAtlasEXT (#140)\n");
				}
				fn = nullptr;
				if (XR_SUCCEEDED(s_next_gipa(s_instance, "xrGetDisplayZoneCapabilitiesEXT", &fn)) && fn) {
					s_pfn_get_zone_caps = (PFN_xrGetDisplayZoneCapabilitiesEXT)fn;
					displayxr_log( "[DisplayXR] Resolved xrGetDisplayZoneCapabilitiesEXT (display_zones)\n");
				}
				fn = nullptr;
				if (XR_SUCCEEDED(s_next_gipa(s_instance, "xrGetDisplayZoneRecommendedViewSizeEXT", &fn)) && fn) {
					s_pfn_get_zone_view_size = (PFN_xrGetDisplayZoneRecommendedViewSizeEXT)fn;
					displayxr_log( "[DisplayXR] Resolved xrGetDisplayZoneRecommendedViewSizeEXT (display_zones)\n");
				}

				// XR_EXT_view_rig (#396): no entry point to resolve (rig is chained
				// on xrLocateViews), so detect by enumerating available instance
				// extensions. We requested it in OpenxrExtensionStrings, so available
				// == enabled here. When present, the locate hook delegates Kooima to
				// the runtime; otherwise it falls back to the local display3d/camera3d.
				fn = nullptr;
				if (XR_SUCCEEDED(s_next_gipa(s_instance, "xrEnumerateInstanceExtensionProperties", &fn)) && fn) {
					PFN_xrEnumerateInstanceExtensionProperties enum_ext =
						(PFN_xrEnumerateInstanceExtensionProperties)fn;
					uint32_t avail = 0;
					enum_ext(nullptr, 0, &avail, nullptr);
					if (avail > 0) {
						XrExtensionProperties *props = (XrExtensionProperties *)
							calloc(avail, sizeof(XrExtensionProperties));
						for (uint32_t i = 0; i < avail; i++)
							props[i].type = XR_TYPE_EXTENSION_PROPERTIES;
						if (XR_SUCCEEDED(enum_ext(nullptr, avail, &avail, props))) {
							for (uint32_t i = 0; i < avail; i++) {
								if (strcmp(props[i].extensionName, XR_EXT_VIEW_RIG_EXTENSION_NAME) == 0)
									s_has_view_rig = 1;
								else if (strcmp(props[i].extensionName, XR_EXT_DISPLAY_ZONES_EXTENSION_NAME) == 0)
									s_has_display_zones = 1;
							}
						}
						free(props);
					}
					displayxr_log("[DisplayXR] XR_EXT_display_zones: %s\n",
					              s_has_display_zones ? "AVAILABLE (zone-framed 3D)"
					                                  : "not found (full-window + surround path)");
					displayxr_log("[DisplayXR] XR_EXT_view_rig: %s\n",
					              s_has_view_rig ? "AVAILABLE (runtime owns Kooima)"
					                             : "not found (legacy local Kooima path)");
				}
			}
			break;
		}
		next = (void *)base->next;
	}

	return result;
}

static XrResult XRAPI_CALL
hooked_xrCreateSession(XrInstance instance, const XrSessionCreateInfo *createInfo, XrSession *session)
{
	DisplayXRState *state = displayxr_get_state();

	// Unity may call xrGetSystemProperties AFTER xrCreateSession, so we
	// force-call it here to ensure display info (and the IOSurface) are
	// populated before we inject the binding struct.
	if (!state->display_info.is_valid && s_real_get_system_properties != nullptr) {
		XrSystemProperties sys_props = {XR_TYPE_SYSTEM_PROPERTIES};
		hooked_xrGetSystemProperties(instance, createInfo->systemId, &sys_props);
		displayxr_log( "[DisplayXR] Force-called xrGetSystemProperties: is_valid=%d\n",
		        state->display_info.is_valid);
	}

	// Log the graphics binding type Unity is using
	{
		const XrBaseInStructure *item = (const XrBaseInStructure *)createInfo->next;
		while (item != nullptr) {
			if (item->type == XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR) {
#if defined(__APPLE__)
				displayxr_log( "[DisplayXR] Graphics binding: VULKAN (via MoltenVK on macOS)\n");
#else
				displayxr_log( "[DisplayXR] Graphics binding: VULKAN\n");
#endif
			} else if (item->type == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR) {
				displayxr_log( "[DisplayXR] Graphics binding: D3D11\n");
			} else if (item->type == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR) {
				displayxr_log( "[DisplayXR] Graphics binding: D3D12\n");
			} else {
				displayxr_log( "[DisplayXR] Session chain struct type=%u\n", (unsigned)item->type);
			}
			item = item->next;
		}
	}

	// Select the graphics backend based on the binding type.
	if (s_backend) { s_backend->on_destroy(); delete s_backend; s_backend = nullptr; }
	s_backend_type = kBackendNone;
	{
		const XrBaseInStructure *item = (const XrBaseInStructure *)createInfo->next;
		while (item != nullptr) {
#if defined(_WIN32)
			if (item->type == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR) {
				s_backend = create_d3d12_backend(); s_backend_type = kBackendD3D12; break;
			} else if (item->type == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR) {
				s_backend = create_d3d11_backend(); s_backend_type = kBackendD3D11; break;
			}
#if defined(ENABLE_VULKAN)
			else if (item->type == XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR) {
				s_backend = create_vulkan_backend(); s_backend_type = kBackendVulkan; break;
			}
#endif
#elif defined(__APPLE__)
			if (item->type == (XrStructureType)1000029000) { // XR_TYPE_GRAPHICS_BINDING_METAL_KHR
				s_backend = create_metal_backend(); s_backend_type = kBackendMetal; break;
			}
#endif
			item = item->next;
		}
	}
	if (s_backend) s_backend->on_session_created(createInfo);

	// Inject window binding into the next chain.
	{
#if defined(__APPLE__)
		// Editor mode: create preview window if not already done (fallback).
		if (state->editor_mode &&
		    state->window_handle == nullptr && state->display_info.is_valid &&
		    state->display_info.display_pixel_width > 0 &&
		    state->display_info.display_pixel_height > 0) {
			void *view = displayxr_metal_create_preview_window(
			    state->display_info.display_pixel_width,
			    state->display_info.display_pixel_height);
			if (view != nullptr) {
				state->window_handle = view;
				displayxr_log("[DisplayXR] Preview window created in xrCreateSession: %ux%u\n",
				        state->display_info.display_pixel_width,
				        state->display_info.display_pixel_height);
			}
		}

		// Built apps: auto-detect the app's main window and create an overlay view.
		if (state->window_handle == nullptr && !state->editor_mode) {
			void *view = displayxr_get_app_main_view();
			if (view != nullptr) {
				state->window_handle = view;
				displayxr_log("[DisplayXR] Auto-detected main window (overlay): %p\n", view);
			} else {
				displayxr_log("[DisplayXR] No main window found — offscreen mode\n");
			}
		}
#elif defined(_WIN32)
		// Auto-detect Unity's main HWND.
		// Shell mode: pass top-level HWND directly (no overlay).
		// Standalone mode: create overlay child window for compositor output.
		if (state->window_handle == nullptr && !state->editor_mode) {
			if (displayxr_is_shell_mode()) {
				void *hwnd = displayxr_get_unity_main_hwnd();
				if (hwnd != nullptr) {
					state->window_handle = hwnd;
					displayxr_log("[DisplayXR] Shell mode: using top-level HWND %p (no overlay)\n", hwnd);
					// Hook GetForegroundWindow so Unity thinks it has focus
					// and processes the PostMessage'd input from the shell
					displayxr_install_focus_hook(hwnd);
				} else {
					displayxr_log("[DisplayXR] Shell mode: no main HWND found\n");
				}
			} else if (state->simple_window_requested) {
				// Avatar-style simple-window mode: bind Unity's REAL main HWND
				// directly (no overlay, no cloak, no off-screen move). Unity
				// stays the on-screen render target; transparentBackgroundEnabled
				// (below) still applies, and click-through / decoration are
				// driven on this same HWND via displayxr_set_simple_window.
				void *hwnd = displayxr_get_unity_main_hwnd();
				if (hwnd != nullptr) {
					state->window_handle = hwnd;
					displayxr_log("[DisplayXR] Simple-window mode: using real top-level HWND %p (no overlay)\n", hwnd);
					displayxr_install_focus_hook(hwnd);
				} else {
					displayxr_log("[DisplayXR] Simple-window mode: no main HWND found\n");
				}
			} else {
				void *hwnd = displayxr_get_app_main_view();
				if (hwnd != nullptr) {
					state->window_handle = hwnd;
					displayxr_log("[DisplayXR] Auto-detected main window HWND (overlay): %p\n", hwnd);
				} else {
					displayxr_log("[DisplayXR] No main window HWND found — compositor will create own window\n");
				}
			}
		}
#endif

		// Walk the chain to find the last item before NULL
		const XrBaseInStructure *chain = (const XrBaseInStructure *)createInfo->next;
		const XrBaseInStructure *last_in_chain = nullptr;

		while (chain != nullptr) {
			last_in_chain = chain;
			chain = chain->next;
		}

		if (last_in_chain != nullptr) {
#if defined(_WIN32)
			static XrWin32WindowBindingCreateInfoEXT win_binding = {};
			win_binding.type = XR_TYPE_WIN32_WINDOW_BINDING_CREATE_INFO_EXT;
			win_binding.next = nullptr;
			win_binding.windowHandle = state->window_handle;
			if (displayxr_is_shell_mode()) {
				// Shell/IPC: function pointers and process-local handles
				// cannot cross process boundaries
				win_binding.readbackCallback = nullptr;
				win_binding.readbackUserdata = nullptr;
				win_binding.sharedTextureHandle = nullptr;
			} else {
				win_binding.readbackCallback = displayxr_readback_callback;
				win_binding.readbackUserdata = nullptr;
				win_binding.sharedTextureHandle = nullptr;
			}
			// runtime-pvt #191 / displayxr-unity#57: opt-in BitBlt (D3D11) /
			// DComp (D3D12) swapchain. Only meaningful with a real HWND and
			// outside shell mode.
			win_binding.transparentBackgroundEnabled =
			    (state->transparent_background_requested
			     && state->window_handle != nullptr
			     && !displayxr_is_shell_mode())
			    ? XR_TRUE : XR_FALSE;
			// Spec v5 chromaKeyColor: post-weave chroma-key conversion is
			// disabled (runtime uses the compose-under-bg + alpha-gate DP
			// path instead; Unity emits per-pixel alpha via ALPHA_BLEND).
			win_binding.chromaKeyColor = 0;

			displayxr_log( "[DisplayXR] Injecting win32 window binding: windowHandle=%p, sharedTextureHandle=%p, transparentBackgroundEnabled=%d, chromaKeyColor=0 (alpha-native)\n",
			        win_binding.windowHandle, win_binding.sharedTextureHandle,
			        (int)win_binding.transparentBackgroundEnabled);

			((XrBaseOutStructure *)last_in_chain)->next = (XrBaseOutStructure *)&win_binding;
#elif defined(__APPLE__)
			static XrCocoaWindowBindingCreateInfoEXT mac_binding = {};
			mac_binding.type = XR_TYPE_COCOA_WINDOW_BINDING_CREATE_INFO_EXT;
			mac_binding.next = nullptr;
			mac_binding.viewHandle = state->window_handle;
			mac_binding.readbackCallback = displayxr_readback_callback;
			mac_binding.readbackUserdata = nullptr;
			mac_binding.sharedIOSurface = nullptr;
			// Spec v5: opt-in transparent background. sim_display_processor_metal
			// is alpha-native, so the runtime preserves per-pixel alpha end-to-end
			// and flips its NSWindow + CAMetalLayer isOpaque=NO. The plugin still
			// needs to flip Unity's own NSWindow (see displayxr_macos.mm).
			mac_binding.transparentBackgroundEnabled =
			    (state->transparent_background_requested
			     && !displayxr_is_shell_mode())
			    ? XR_TRUE : XR_FALSE;

			displayxr_log( "[DisplayXR] Injecting cocoa window binding: viewHandle=%p, sharedIOSurface=%p, transparentBackgroundEnabled=%d\n",
			        mac_binding.viewHandle, mac_binding.sharedIOSurface,
			        (int)mac_binding.transparentBackgroundEnabled);

			((XrBaseOutStructure *)last_in_chain)->next = (XrBaseOutStructure *)&mac_binding;
#endif
		}
	}

	XrResult result = s_real_create_session(instance, createInfo, session);
	if (XR_FAILED(result)) {
		displayxr_log("[DisplayXR] xrCreateSession FAILED, result=%d (runtime may not support the supplied graphics binding)\n", (int)result);
	}
	if (XR_SUCCEEDED(result)) {
		s_session = *session;
		s_session_alive = 1;
		displayxr_log( "[DisplayXR] xrCreateSession succeeded, session=%p\n", (void *)(uintptr_t)s_session);

		// Create LOCAL reference space for xrLocateViews.
		// LOCAL space gives raw eye positions relative to the display origin.
		if (s_real_create_reference_space != nullptr) {
			XrReferenceSpaceCreateInfo space_info = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
			space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
			space_info.poseInReferenceSpace.orientation = {0, 0, 0, 1};
			space_info.poseInReferenceSpace.position = {0, 0, 0};

			XrResult space_result = s_real_create_reference_space(s_session, &space_info, &s_local_space);
			if (XR_FAILED(space_result)) {
				s_local_space = XR_NULL_HANDLE;
				displayxr_log( "[DisplayXR] LOCAL reference space FAILED (result=%d) — "
				        "will use app's reference space\n", space_result);
			} else {
				displayxr_log( "[DisplayXR] LOCAL reference space created successfully\n");
			}

			// Also create our own VIEW space so hooked_xrLocateViews can query
			// the current head pose without depending on the app having created
			// one. Used for Unity URP head-pose compensation (issue #115) —
			// xrLocateSpace(s_view_space, s_local_space) returns the current
			// head pose in LOCAL coords, which we add to views[i].pose.position.
			XrReferenceSpaceCreateInfo view_info = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
			view_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
			view_info.poseInReferenceSpace.orientation = {0, 0, 0, 1};
			view_info.poseInReferenceSpace.position = {0, 0, 0};
			XrResult view_result = s_real_create_reference_space(s_session, &view_info, &s_view_space);
			if (XR_FAILED(view_result)) {
				s_view_space = XR_NULL_HANDLE;
				displayxr_log("[DisplayXR] VIEW reference space FAILED (result=%d) — "
				              "Unity URP head-pose compensation will be skipped\n",
				              view_result);
			} else {
				displayxr_log("[DisplayXR] VIEW reference space created successfully\n");
			}
		}

		// Wire the rendering-mode extension function pointers and enumerate
		// modes from the runtime. Backend default implementation lives on
		// GraphicsBackend base — all platforms inherit it, so built apps on
		// any graphics API get the mode list (and can switch via
		// xrRequestDisplayRenderingModeEXT) without a separate standalone
		// session. The C ABI displayxr_standalone_*_rendering_mode* shims
		// fall back to s_backend->rendering_modes when s_sa.session is null.
		if (s_backend && s_next_gipa && s_instance) {
			PFN_xrVoidFunction fn_enum = nullptr, fn_req = nullptr;
			s_next_gipa(s_instance, "xrEnumerateDisplayRenderingModesEXT", &fn_enum);
			s_next_gipa(s_instance, "xrRequestDisplayRenderingModeEXT", &fn_req);
			s_backend->set_rendering_mode_fns(
			    (PFN_xrEnumerateDisplayRenderingModesEXT)fn_enum,
			    (PFN_xrRequestDisplayRenderingModeEXT)fn_req);
			s_backend->on_session_ready(s_session);
		}
	}

	return result;
}

#if defined(ENABLE_VULKAN)
// --- xrCreateVulkanDeviceKHR interception (displayxr-runtime#314) ---
//
// Unity (XR_KHR_vulkan_enable2) routes its logical-device creation through the
// runtime's xrCreateVulkanDeviceKHR. Unity's own VkDeviceCreateInfo enables
// VK_KHR_external_memory / _semaphore but NOT the _win32 variants. The SR SDK
// Vulkan weaver (CreateVulkanWeaver) builds a D3D-interop compose-under bridge,
// which is the kind of work that needs the _win32 external-memory/semaphore
// extensions. We intercept device creation and append them so the device Unity
// hands the runtime is interop-capable — the correct contract for an
// interop-fed device regardless of who consumes it.
//
// NOTE: injecting these alone does NOT resolve the LeiaSR weaver AV during
// xrCreateSession on Vulkan (displayxr-runtime#314). Verified: the bound device
// is this exact device handle and vkCreateDevice accepted the extensions, yet
// the weaver still faults — so the root cause is elsewhere in the weaver's
// handling of Unity's device. This hook is kept as a correct prerequisite, not
// as the fix.
//
// Minimal stable-ABI struct definitions (avoids a Vulkan SDK dependency — same
// opaque-handle philosophy as XrGraphicsBindingVulkanKHR in
// displayxr_vulkan_backend.cpp). Layout matches Vulkan 1.x / OpenXR 1.x on x64.
typedef void  *DxrVkPhysicalDevice;
typedef void  *DxrVkDevice;
typedef void (*DxrPFN_vkVoidFunction)(void);
typedef DxrPFN_vkVoidFunction (XRAPI_PTR *DxrPFN_vkGetInstanceProcAddr)(void *instance, const char *name);

typedef struct DxrVkDeviceCreateInfo {
	int32_t            sType;
	const void        *pNext;
	uint32_t           flags;
	uint32_t           queueCreateInfoCount;
	const void        *pQueueCreateInfos;
	uint32_t           enabledLayerCount;
	const char *const *ppEnabledLayerNames;
	uint32_t           enabledExtensionCount;
	const char *const *ppEnabledExtensionNames;
	const void        *pEnabledFeatures;
} DxrVkDeviceCreateInfo;

typedef struct DxrXrVulkanDeviceCreateInfoKHR {
	XrStructureType               type;
	const void                   *next;
	XrSystemId                    systemId;
	uint64_t                      createFlags;
	DxrPFN_vkGetInstanceProcAddr  pfnGetInstanceProcAddr;
	DxrVkPhysicalDevice           vulkanPhysicalDevice;
	const DxrVkDeviceCreateInfo  *vulkanCreateInfo;
	const void                   *vulkanAllocator;
} DxrXrVulkanDeviceCreateInfoKHR;

typedef XrResult (XRAPI_PTR *DxrPFN_xrCreateVulkanDeviceKHR)(
    XrInstance instance, const DxrXrVulkanDeviceCreateInfoKHR *createInfo,
    DxrVkDevice *vulkanDevice, int32_t *vulkanResult);

static DxrPFN_xrCreateVulkanDeviceKHR s_real_create_vulkan_device = nullptr;

static XrResult XRAPI_CALL
hooked_xrCreateVulkanDeviceKHR(XrInstance instance,
                               const DxrXrVulkanDeviceCreateInfoKHR *createInfo,
                               DxrVkDevice *vulkanDevice, int32_t *vulkanResult)
{
	static const char *kRequired[] = {
		"VK_KHR_external_memory",
		"VK_KHR_external_memory_win32",
		"VK_KHR_external_semaphore",
		"VK_KHR_external_semaphore_win32",
	};
	const uint32_t kRequiredCount = (uint32_t)(sizeof(kRequired) / sizeof(kRequired[0]));

	if (s_real_create_vulkan_device == nullptr) return XR_ERROR_FUNCTION_UNSUPPORTED;
	if (createInfo == nullptr || createInfo->vulkanCreateInfo == nullptr) {
		return s_real_create_vulkan_device(instance, createInfo, vulkanDevice, vulkanResult);
	}

	const DxrVkDeviceCreateInfo *src = createInfo->vulkanCreateInfo;
	const uint32_t orig = src->enabledExtensionCount;

	const char *merged[256];
	if (orig + kRequiredCount > 256) {
		// Implausibly long list — pass through rather than risk a stack overrun.
		displayxr_log("[DisplayXR] Vulkan: device extension list too long (%u), not injecting\n", orig);
		return s_real_create_vulkan_device(instance, createInfo, vulkanDevice, vulkanResult);
	}

	uint32_t n = 0;
	for (uint32_t i = 0; i < orig; i++) merged[n++] = src->ppEnabledExtensionNames[i];
	for (uint32_t r = 0; r < kRequiredCount; r++) {
		bool present = false;
		for (uint32_t i = 0; i < orig; i++) {
			if (src->ppEnabledExtensionNames[i] &&
			    strcmp(src->ppEnabledExtensionNames[i], kRequired[r]) == 0) { present = true; break; }
		}
		if (!present) merged[n++] = kRequired[r];
	}

	DxrVkDeviceCreateInfo dci = *src;
	dci.enabledExtensionCount   = n;
	dci.ppEnabledExtensionNames = merged;

	DxrXrVulkanDeviceCreateInfoKHR ci = *createInfo;
	ci.vulkanCreateInfo = &dci;

	displayxr_log("[DisplayXR] xrCreateVulkanDeviceKHR: enabled %u device extensions (%u app + win32 external-memory/semaphore interop for SR weaver)\n", n, orig);
	return s_real_create_vulkan_device(instance, &ci, vulkanDevice, vulkanResult);
}
#endif // ENABLE_VULKAN

static XrResult XRAPI_CALL
hooked_xrRequestDisplayRenderingModeEXT(XrSession session, uint32_t modeIndex)
{
	if (s_backend) return s_backend->request_rendering_mode(session, modeIndex);
	return XR_ERROR_FUNCTION_UNSUPPORTED;
}

static XrResult XRAPI_CALL
hooked_xrDestroySession(XrSession session)
{
	displayxr_log( "[DisplayXR] xrDestroySession BEGIN session=%p (DEFERRED)\n", (void *)(uintptr_t)session);
	s_session_alive = 0;
	s_local_space = XR_NULL_HANDLE;
	s_view_space = XR_NULL_HANDLE;
	wsui_hooked_on_session_destroyed();
	local2d_hooked_on_session_destroyed();
	if (s_backend) { s_backend->on_session_destroyed(); }

	// Destroy the editor preview window before deferring the session destroy.
	// The compositor is being torn down so the window is no longer needed.
#if defined(__APPLE__)
	{
		DisplayXRState *state = displayxr_get_state();
		if (state->editor_mode && state->window_handle != nullptr) {
			displayxr_metal_destroy_preview_window();
			state->window_handle = nullptr;
		}
	}
#endif

	// Defer the real destroy — Unity calls xrPollEvent after xrDestroyInstance,
	// and its dispatch trampolines reference runtime session/compositor objects.
	// Keep everything alive until the next instance lifecycle.
	s_deferred_destroy_session = session;
	s_deferred_destroy_session_fn = s_real_destroy_session;

	displayxr_log( "[DisplayXR] xrDestroySession END (deferred, returning XR_SUCCESS)\n");
	s_session = XR_NULL_HANDLE;
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL
hooked_xrEndFrame(XrSession session, const XrFrameEndInfo *frameEndInfo)
{
	// Guard: skip if session is being torn down
	if (!s_session_alive) {
		displayxr_log( "[DisplayXR] xrEndFrame: session not alive, passing through\n");
		return s_real_end_frame(session, frameEndInfo);
	}

	// XR_EXT_display_zones: this is a ZONES FRAME when the app set a zone rect and
	// the runtime supports it. We then chain the zone on each projection layer
	// below (binding its views into the rect — the runtime weaves Unity's
	// full-swapchain eye tiles into the zone), and the legacy canvas output rect
	// becomes INERT (skip it). Computed up front so the canvas block can gate on it.
	bool zones_frame = s_zone_valid && frameEndInfo != nullptr && dxr_zones_ready(session);

	// Canvas sub-rect (#34/#131): re-apply each frame so the runtime weaves the
	// 3D content into the app-chosen sub-rect (the rest becomes the 2D surround
	// region). No-op unless an app called displayxr_set_canvas_rect. Per-frame
	// matches the cube_texture reference and survives runtime state resets. Inert
	// in a zones frame (the zone rect places the 3D instead).
	if (s_canvas_rect_valid && s_pfn_set_output_rect && !zones_frame) {
		s_pfn_set_output_rect(session, s_canvas_rect_x, s_canvas_rect_y,
		                      s_canvas_rect_w, s_canvas_rect_h);
	}

	// XR_EXT_display_zones: chain the zone on each projection layer Unity submitted
	// (XrCompositionLayerProjection::next), making it a zones frame so the runtime
	// frames the views into the rect. Unity owns the layer structs (const) so we
	// patch ::next in place and restore after s_real_end_frame — mirrors the
	// D3D11 ef_patches view-rect restore below. Unity submits a single projection
	// layer (MultiPass, N views), so one zoneId=1 suffices; if it ever submits
	// more, all get the same zone (the runtime auto-derives the wish from the
	// union of zone rects regardless).
	XrDisplayZoneEXT end_zone = {XR_TYPE_DISPLAY_ZONE_EXT};
	const void *zone_saved_next[16];
	XrCompositionLayerProjection *zone_patched[16];
	int zone_npatch = 0;
	if (zones_frame) {
		end_zone.zoneId = 1;
		end_zone.rect.offset = {(int32_t)s_zone_x, (int32_t)s_zone_y};
		end_zone.rect.extent = {(int32_t)s_zone_w, (int32_t)s_zone_h};
		end_zone.next = nullptr;
		for (uint32_t i = 0; i < frameEndInfo->layerCount && zone_npatch < 16; i++) {
			const XrCompositionLayerBaseHeader *hdr = frameEndInfo->layers[i];
			if (hdr && hdr->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
				XrCompositionLayerProjection *proj = (XrCompositionLayerProjection *)hdr;
				zone_saved_next[zone_npatch] = proj->next;
				zone_patched[zone_npatch] = proj;
				proj->next = &end_zone;
				zone_npatch++;
			}
		}
		static int s_zone_ef_count = 0;
		if (s_zone_ef_count % 120 == 0) {
			displayxr_log("[DisplayXR] zones frame: rect=(%d,%d %dx%d), chained on %d "
			              "projection layer(s)\n", (int)s_zone_x, (int)s_zone_y,
			              (int)s_zone_w, (int)s_zone_h, zone_npatch);
		}
		s_zone_ef_count++;
	}

	// 2D surround (#131): copy the registered Unity RT into the SHARED surround
	// texture, signal the SHARED fence, and (re)register with the runtime so it
	// blits the non-canvas region post-weave. Runs here on the render thread
	// (same context as wsui below) so queue submission is safe. No-op unless
	// C# registered a texture AND the backend supports it (D3D12). The register
	// happens BEFORE s_real_end_frame so the runtime sees the await value for
	// this frame's strip blit.
	if (s_surround_unity_tex && s_backend && s_pfn_set_surround_fence) {
		void *tex_h = nullptr, *fence_h = nullptr;
		uint64_t await_val = 0;
		if (s_backend->surround_update(s_surround_unity_tex,
		        s_surround_w, s_surround_h, &tex_h, &fence_h, &await_val) &&
		    tex_h && fence_h) {
			s_pfn_set_surround_fence(session, tex_h, s_surround_w,
			                         s_surround_h, fence_h, await_val);
		}
	}

	// Diagnostic: log what Unity submits (every 120 frames, skip first 2)
	static int s_ef_count = 0;
	if (s_ef_count >= 2 && s_ef_count % 120 == 0 &&
	    frameEndInfo != nullptr && frameEndInfo->layerCount > 0 &&
	    frameEndInfo->layers != nullptr) {
		displayxr_log( "[DisplayXR] xrEndFrame: %u layers\n", frameEndInfo->layerCount);
		for (uint32_t i = 0; i < frameEndInfo->layerCount; i++) {
			const XrCompositionLayerBaseHeader *hdr = frameEndInfo->layers[i];
			if (hdr == nullptr) continue;
			if (hdr->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
				const XrCompositionLayerProjection *proj =
				    (const XrCompositionLayerProjection *)hdr;
				if (proj->views == nullptr) continue;
				displayxr_log( "  layer[%u] PROJECTION: viewCount=%u\n",
				        i, proj->viewCount);
				for (uint32_t v = 0; v < proj->viewCount; v++) {
					const XrCompositionLayerProjectionView *pv = &proj->views[v];
					float hfov = (pv->fov.angleRight - pv->fov.angleLeft) * 57.2958f;
					displayxr_log(
					        "    view[%u]: pos=(%.4f,%.4f,%.4f) hfov=%.1f "
					        "arrayIdx=%u rect=(%d,%d %dx%d)\n",
					        v,
					        pv->pose.position.x, pv->pose.position.y,
					        pv->pose.position.z,
					        hfov,
					        pv->subImage.imageArrayIndex,
					        pv->subImage.imageRect.offset.x,
					        pv->subImage.imageRect.offset.y,
					        pv->subImage.imageRect.extent.width,
					        pv->subImage.imageRect.extent.height);
				}
			} else {
				displayxr_log( "  layer[%u] type=%u\n", i, (unsigned)hdr->type);
			}
		}
	}
	s_ef_count++;

	DisplayXRState *state = displayxr_get_state();

#if defined(_WIN32)
	// Backend-specific end-frame processing (D3D11 atlas composite, etc.).
	EFPatch ef_patches[16]; int ef_npatch = 0;
	if (s_backend) {
		s_backend->prepare_end_frame(session, frameEndInfo, ef_patches, &ef_npatch);
	}
#endif

	// Window-space UI overlay (issue #67): copy Unity texture into our overlay
	// swapchain and populate window_layers[0]. The loop below picks it up.
	wsui_hooked_pre_end_frame(session, s_backend);

	// Local2D overlay (#439/#491): copy Unity texture into our overlay
	// swapchain and populate local2d_layer. Appended below as a
	// XrCompositionLayerLocal2DEXT ("glass over 3D").
	local2d_hooked_pre_end_frame(session, s_backend);

	// Count active window-space layers
	int active_layers = 0;
	for (int i = 0; i < DISPLAYXR_MAX_WINDOW_LAYERS; i++) {
		if (state->window_layers[i].active && state->window_layers[i].swapchain != XR_NULL_HANDLE) {
			active_layers++;
		}
	}
	int local2d_active = (state->local2d_layer.active &&
	                      state->local2d_layer.swapchain != XR_NULL_HANDLE) ? 1 : 0;

	XrResult ef_result;
	if (active_layers == 0 && local2d_active == 0) {
		// No overlay layers — pass through
		ef_result = s_real_end_frame(session, frameEndInfo);
	} else {

	// Build extended layer array: original layers + window-space + local2d
	uint32_t total = frameEndInfo->layerCount + (uint32_t)active_layers + (uint32_t)local2d_active;
	const XrCompositionLayerBaseHeader **layers = new const XrCompositionLayerBaseHeader *[total];

	// Copy original layers
	for (uint32_t i = 0; i < frameEndInfo->layerCount; i++) {
		layers[i] = frameEndInfo->layers[i];
	}

	// Append window-space layers
	static XrCompositionLayerWindowSpaceEXT ws_layers[DISPLAYXR_MAX_WINDOW_LAYERS] = {};
	uint32_t idx = frameEndInfo->layerCount;
	for (int i = 0; i < DISPLAYXR_MAX_WINDOW_LAYERS; i++) {
		DisplayXRWindowLayer *wl = &state->window_layers[i];
		if (!wl->active || wl->swapchain == XR_NULL_HANDLE) {
			continue;
		}

		ws_layers[i].type = XR_TYPE_COMPOSITION_LAYER_WINDOW_SPACE_EXT;
		ws_layers[i].next = nullptr;
		// BLEND_TEXTURE_SOURCE_ALPHA_BIT: blend this layer over what's
		// below using the texture's source alpha.
		// UNPREMULTIPLIED_ALPHA_BIT: Unity Canvas (which renders into
		// the window-space layer's swapchain via DisplayXRWindowSpaceUI)
		// outputs non-premultiplied alpha — (color, alpha) where color
		// is the un-scaled value. Without this flag the runtime treats
		// the texture as premultiplied and the color math goes wrong
		// (panel too bright) AND alpha math can end up < 1 even where
		// the HUD panel covers the opaque tiger, letting captured
		// desktop bleed through the semi-transparent panel area.
		ws_layers[i].layerFlags =
			XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
			XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
		ws_layers[i].subImage.swapchain = wl->swapchain;
		ws_layers[i].subImage.imageRect.offset = {0, 0};
		ws_layers[i].subImage.imageRect.extent = {(int32_t)wl->swapchain_width,
		                                           (int32_t)wl->swapchain_height};
		ws_layers[i].subImage.imageArrayIndex = 0;
		ws_layers[i].x = wl->x;
		ws_layers[i].y = wl->y;
		ws_layers[i].width = wl->width;
		ws_layers[i].height = wl->height;
		ws_layers[i].disparity = wl->disparity;

		layers[idx++] = (const XrCompositionLayerBaseHeader *)&ws_layers[i];
	}

	// Append the Local2D layer (#439/#491). The runtime composites it post-weave
	// at the pixel rect with an implicit mask (region goes flat 2D, "glass over
	// 3D"). Unity Canvas content is straight (unpremultiplied) alpha — same as
	// the window-space path — so flag it UNPREMULTIPLIED so the runtime's
	// alpha-over math is correct.
	static XrCompositionLayerLocal2DEXT l2d_layer = {};
	if (local2d_active) {
		DisplayXRLocal2DLayer *l2 = &state->local2d_layer;
		l2d_layer.type = XR_TYPE_COMPOSITION_LAYER_LOCAL_2D_EXT;
		l2d_layer.next = nullptr;
		l2d_layer.layerFlags =
			XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
			XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
		l2d_layer.subImage.swapchain = l2->swapchain;
		l2d_layer.subImage.imageRect.offset = {0, 0};
		l2d_layer.subImage.imageRect.extent = {(int32_t)l2->swapchain_width,
		                                       (int32_t)l2->swapchain_height};
		l2d_layer.subImage.imageArrayIndex = 0;
		l2d_layer.rect.offset = {l2->rect_x, l2->rect_y};
		l2d_layer.rect.extent = {l2->rect_w, l2->rect_h};
		layers[idx++] = (const XrCompositionLayerBaseHeader *)&l2d_layer;
	}

	// Submit with extended layers
	XrFrameEndInfo modified = *frameEndInfo;
	modified.layerCount = total;
	modified.layers = layers;

	ef_result = s_real_end_frame(session, &modified);
	delete[] layers;
	}

#if defined(_WIN32)
	// Restore original swapchain handles and rects in projection views.
	for (int i = 0; i < ef_npatch; i++) {
		ef_patches[i].view->subImage.swapchain = ef_patches[i].orig_sc;
		ef_patches[i].view->subImage.imageRect = ef_patches[i].orig_rect;
	}
#endif
	// XR_EXT_display_zones: restore the projection layers' original ::next.
	for (int i = 0; i < zone_npatch; i++)
		zone_patched[i]->next = zone_saved_next[i];
	return ef_result;
}



// ============================================================================
// Swapchain diagnostic hooks (issue #36: D3D11 black screen)
// Pure passthrough + logging — no behavioral changes.
// ============================================================================

static XrResult XRAPI_CALL
hooked_xrEnumerateSwapchainFormats(XrSession session,
                                   uint32_t formatCapacityInput,
                                   uint32_t *formatCountOutput,
                                   int64_t *formats)
{
	XrResult result = s_real_enumerate_swapchain_formats(session, formatCapacityInput,
	                                                     formatCountOutput, formats);
	if (XR_SUCCEEDED(result) && formats != nullptr && formatCountOutput != nullptr) {
		displayxr_log( "[DisplayXR] xrEnumerateSwapchainFormats: %u formats\n", *formatCountOutput);
		for (uint32_t i = 0; i < *formatCountOutput; i++) {
			displayxr_log( "  format[%u] = %lld", i, (long long)formats[i]);
#if defined(_WIN32)
			// Annotate well-known DXGI formats
			switch (formats[i]) {
			case 28: displayxr_log( " (DXGI_FORMAT_R8G8B8A8_UNORM)"); break;
			case 29: displayxr_log( " (DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)"); break;
			case 87: displayxr_log( " (DXGI_FORMAT_B8G8R8A8_UNORM)"); break;
			case 91: displayxr_log( " (DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)"); break;
			case 10: displayxr_log( " (DXGI_FORMAT_R16G16B16A16_FLOAT)"); break;
			case 24: displayxr_log( " (DXGI_FORMAT_R10G10B10A2_UNORM)"); break;
			default: break;
			}
#endif
			displayxr_log( "\n");
		}
	}
	return result;
}

// ============================================================================
// xrEnumerateViewConfigurationViews — zone-sized eye render (avatar-faithful)
// ============================================================================
// When the app has published a 3D-zone rect *before* XR init (the early seed
// from C# via displayxr_set_3d_zone_rect), size Unity's per-eye swapchain to the
// zone extent instead of the full window. Then Unity renders the zone-scoped
// frustum into a zone-aspect eye RT and submits subImage.imageRect == that RT ==
// the zone extent. By the runtime contract recommendedViewSize == zone rect
// extent, so the compositor blit is a clean 1:1-aspect fit into the zone rect —
// zone-confined by construction, exactly like displayxr-demo-avatar (which
// renders each eye at xrGetDisplayZoneRecommendedViewSizeEXT and submits a
// zone-sized imageRect). Because recommendedViewSize == zone extent we can use
// s_zone_w/h directly and need no live session / s_pfn_get_zone_view_size here
// (a session does not yet exist at enumerate time).
//
// Gated on the explicit app seed (s_zone_valid) rather than s_has_display_zones:
// the extension-presence flag is set in hooked_xrGetSystemProperties, whose order
// relative to this call is not guaranteed, so depending on it could silently
// disable the override (the plan's primary risk). When unseeded we pass through
// unchanged (today's full-window behavior).
static XrResult XRAPI_CALL
hooked_xrEnumerateViewConfigurationViews(XrInstance instance,
                                         XrSystemId systemId,
                                         XrViewConfigurationType viewConfigurationType,
                                         uint32_t viewCapacityInput,
                                         uint32_t *viewCountOutput,
                                         XrViewConfigurationView *views)
{
	XrResult result = s_real_enumerate_view_configuration_views(
	    instance, systemId, viewConfigurationType,
	    viewCapacityInput, viewCountOutput, views);

	if (XR_SUCCEEDED(result) && views != nullptr && viewCapacityInput > 0) {
		uint32_t count = viewCapacityInput;
		if (viewCountOutput != nullptr && *viewCountOutput < count)
			count = *viewCountOutput;

		if (s_zone_valid && s_zone_w > 0 && s_zone_h > 0) {
			uint32_t zw = (uint32_t)s_zone_w;
			uint32_t zh = (uint32_t)s_zone_h;
			for (uint32_t i = 0; i < count; i++) {
				uint32_t orig_w = views[i].recommendedImageRectWidth;
				uint32_t orig_h = views[i].recommendedImageRectHeight;
				if (views[i].maxImageRectWidth  < zw) views[i].maxImageRectWidth  = zw;
				if (views[i].maxImageRectHeight < zh) views[i].maxImageRectHeight = zh;
				views[i].recommendedImageRectWidth  = zw;
				views[i].recommendedImageRectHeight = zh;
				displayxr_log("[DisplayXR] xrEnumerateViewConfigurationViews: view[%u] zone-override recommended %ux%u -> %ux%u (max now %ux%u, has_zones=%d)\n",
				              i, orig_w, orig_h, zw, zh,
				              views[i].maxImageRectWidth, views[i].maxImageRectHeight,
				              s_has_display_zones);
			}
		} else {
			displayxr_log("[DisplayXR] xrEnumerateViewConfigurationViews: %u view(s), no zone override (zone_valid=%d zone=%dx%d has_zones=%d) — full-window eye RT\n",
			              count, (int)s_zone_valid, (int)s_zone_w, (int)s_zone_h,
			              s_has_display_zones);
		}
	}
	return result;
}

static XrResult XRAPI_CALL
hooked_xrCreateSwapchain(XrSession session,
                          const XrSwapchainCreateInfo *createInfo,
                          XrSwapchain *swapchain)
{
	// In a Unity Gamma-color-space project, Unity's shader output is already
	// gamma-encoded. Writing those values to an sRGB-typed swapchain causes
	// the GPU to apply sRGB encoding a second time (double-gamma → on-display
	// content is too dark). Downgrade sRGB color formats to their UNORM
	// equivalents so the values land unchanged. Linear projects keep sRGB.
	// The format numbering is graphics-API-specific: D3D uses DXGI_FORMAT,
	// Vulkan uses VkFormat, so the sRGB→UNORM map must match the active
	// backend (displayxr-unity#122 — Vulkan was previously missed, leaving
	// Gamma projects dark on Vulkan).
	XrSwapchainCreateInfo info = *createInfo;
	DisplayXRState *st = displayxr_get_state();
	if (!st->use_srgb_swapchain &&
	    (info.usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT)) {
		int64_t orig = info.format;
#if defined(ENABLE_VULKAN)
		if (s_backend_type == kBackendVulkan) {
			// VkFormat sRGB → UNORM. Unity-on-Vulkan requests an sRGB
			// swapchain (e.g. VK_FORMAT_B8G8R8A8_SRGB=50) even in Gamma mode.
			if      (info.format == 43) info.format = 37; // R8G8B8A8_SRGB → R8G8B8A8_UNORM
			else if (info.format == 50) info.format = 44; // B8G8R8A8_SRGB → B8G8R8A8_UNORM
			else if (info.format == 58) info.format = 51; // A8B8G8R8_SRGB_PACK32 → _UNORM_PACK32
		} else
#endif
		if (s_backend_type == kBackendMetal) {
			// MTLPixelFormat sRGB → UNORM. Unity (macOS) requests an sRGB eye
			// swapchain (RGBA8Unorm_sRGB=71 / BGRA8Unorm_sRGB=81) even in a Gamma
			// project; writing already-gamma-encoded shader output to it makes the
			// GPU sRGB-encode a second time (double-gamma → washed out / over-
			// exposed on display). Metal was previously missed here (only DXGI +
			// Vulkan were mapped), so Gamma projects looked overexposed on macOS.
			if      (info.format == 71) info.format = 70; // RGBA8Unorm_sRGB → RGBA8Unorm
			else if (info.format == 81) info.format = 80; // BGRA8Unorm_sRGB → BGRA8Unorm
		} else {
			if      (info.format == 29) info.format = 28; // DXGI RGBA8 SRGB → UNORM
			else if (info.format == 91) info.format = 87; // DXGI BGRA8 SRGB → UNORM
		}
		if (info.format != orig) {
			displayxr_log("[DisplayXR] xrCreateSwapchain: Gamma-space override %lld → %lld\n",
			    (long long)orig, (long long)info.format);
		}
	}

	displayxr_log( "[DisplayXR] xrCreateSwapchain: format=%lld size=%ux%u "
	        "samples=%u faces=%u arrays=%u mips=%u "
	        "createFlags=0x%llx usageFlags=0x%llx\n",
	        (long long)info.format,
	        info.width, info.height,
	        info.sampleCount, info.faceCount,
	        info.arraySize, info.mipCount,
	        (unsigned long long)info.createFlags,
	        (unsigned long long)info.usageFlags);
	const XrSwapchainCreateInfo *createInfoEffective = &info;

#if defined(_WIN32)
	// Annotate usage flags
	if (createInfo->usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT)
		displayxr_log( "  usage: COLOR_ATTACHMENT\n");
	if (createInfo->usageFlags & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)
		displayxr_log( "  usage: DEPTH_STENCIL_ATTACHMENT\n");
	if (createInfo->usageFlags & XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT)
		displayxr_log( "  usage: UNORDERED_ACCESS\n");
	if (createInfo->usageFlags & XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT)
		displayxr_log( "  usage: TRANSFER_SRC\n");
	if (createInfo->usageFlags & XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT)
		displayxr_log( "  usage: TRANSFER_DST\n");
	if (createInfo->usageFlags & XR_SWAPCHAIN_USAGE_SAMPLED_BIT)
		displayxr_log( "  usage: SAMPLED\n");
	if (createInfo->usageFlags & XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT)
		displayxr_log( "  usage: MUTABLE_FORMAT\n");
#endif

	XrResult result = s_real_create_swapchain(session, createInfoEffective, swapchain);
	if (XR_SUCCEEDED(result)) {
		displayxr_log( "[DisplayXR] xrCreateSwapchain: OK swapchain=%p\n",
		        (void *)(uintptr_t)*swapchain);
		if (s_backend) s_backend->on_swapchain_created(session, createInfoEffective, *swapchain);
	} else {
		displayxr_log( "[DisplayXR] xrCreateSwapchain: FAILED result=%d\n", result);
	}
	return result;
}

static XrResult XRAPI_CALL
hooked_xrEnumerateSwapchainImages(XrSwapchain swapchain,
                                   uint32_t imageCapacityInput,
                                   uint32_t *imageCountOutput,
                                   XrSwapchainImageBaseHeader *images)
{
	// Backend may intercept (e.g. D3D11 typed-swapchain substitution).
	if (s_backend) {
		XrResult bk_result;
		if (s_backend->handle_enumerate_swapchain_images(swapchain, imageCapacityInput, imageCountOutput, images, &bk_result))
			return bk_result;
	}

	XrResult result = s_real_enumerate_swapchain_images(swapchain, imageCapacityInput,
	                                                    imageCountOutput, images);
	if (XR_SUCCEEDED(result) && images != nullptr && imageCountOutput != nullptr) {
		displayxr_log( "[DisplayXR] xrEnumerateSwapchainImages: sc=%p count=%u type=%u\n",
		        (void *)(uintptr_t)swapchain, *imageCountOutput, (unsigned)images->type);

#if defined(_WIN32)
		if (images->type == XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR) {
			XrSwapchainImageD3D11KHR *d3d_images = (XrSwapchainImageD3D11KHR *)images;
			for (uint32_t i = 0; i < *imageCountOutput; i++) {
				ID3D11Texture2D *tex = d3d_images[i].texture;
				displayxr_log( "  image[%u] texture=%p", i, (void *)tex);
				if (tex != nullptr) {
					D3D11_TEXTURE2D_DESC desc = {};
					tex->GetDesc(&desc);
					displayxr_log( "\n    D3D11: %ux%u fmt=%u bindFlags=0x%x "
					        "miscFlags=0x%x",
					        desc.Width, desc.Height,
					        (unsigned)desc.Format,
					        (unsigned)desc.BindFlags,
					        (unsigned)desc.MiscFlags);
					if (desc.BindFlags & D3D11_BIND_RENDER_TARGET)  displayxr_log( " [RT]");
					if (desc.BindFlags & D3D11_BIND_SHADER_RESOURCE) displayxr_log( " [SRV]");
				}
				displayxr_log( "\n");
			}
		} else
#endif
		{
			for (uint32_t i = 0; i < *imageCountOutput; i++) {
				displayxr_log( "  image[%u] type=%u\n", i, (unsigned)images[i].type);
			}
		}
	}
	return result;
}

static XrResult XRAPI_CALL
hooked_xrAcquireSwapchainImage(XrSwapchain swapchain,
                                const XrSwapchainImageAcquireInfo *acquireInfo,
                                uint32_t *index)
{
	if (s_backend) {
		XrResult bk_result;
		if (s_backend->handle_acquire_swapchain_image(swapchain, acquireInfo, index, &bk_result))
			return bk_result;
	}
	XrResult result = s_real_acquire_swapchain_image(swapchain, acquireInfo, index);
	// Log first few acquires per swapchain, then every 120th frame
	static int s_acq_count = 0;
	if (s_acq_count < 6 || s_acq_count % 120 == 0) {
		displayxr_log( "[DisplayXR] xrAcquireSwapchainImage: sc=%p idx=%u result=%d\n",
		        (void *)(uintptr_t)swapchain,
		        (index != nullptr) ? *index : 0xFFFFFFFF,
		        result);
	}
	s_acq_count++;
	return result;
}

static XrResult XRAPI_CALL
hooked_xrWaitSwapchainImage(XrSwapchain swapchain,
                             const XrSwapchainImageWaitInfo *waitInfo)
{
	if (s_backend) {
		XrResult bk_result;
		if (s_backend->handle_wait_swapchain_image(swapchain, waitInfo, &bk_result))
			return bk_result;
	}
	XrResult result = s_real_wait_swapchain_image(swapchain, waitInfo);
	static int s_wait_count = 0;
	if (s_wait_count < 6 || s_wait_count % 120 == 0) {
		displayxr_log( "[DisplayXR] xrWaitSwapchainImage: sc=%p timeout=%llu result=%d\n",
		        (void *)(uintptr_t)swapchain,
		        (unsigned long long)(waitInfo ? waitInfo->timeout : 0),
		        result);
	}
	s_wait_count++;
	return result;
}

static XrResult XRAPI_CALL
hooked_xrReleaseSwapchainImage(XrSwapchain swapchain,
                                const XrSwapchainImageReleaseInfo *releaseInfo)
{
	if (s_backend) {
		XrResult bk_result;
		if (s_backend->handle_release_swapchain_image(swapchain, releaseInfo, &bk_result))
			return bk_result;
	}

	static int s_rel_count = 0;
	if (s_rel_count < 6 || s_rel_count % 120 == 0) {
		displayxr_log( "[DisplayXR] xrReleaseSwapchainImage: sc=%p\n",
		        (void *)(uintptr_t)swapchain);
	}
	s_rel_count++;
	return s_real_release_swapchain_image(swapchain, releaseInfo);
}


static XrResult XRAPI_CALL
hooked_xrDestroyInstance(XrInstance instance)
{
	displayxr_log( "[DisplayXR] xrDestroyInstance BEGIN (DEFERRED)\n");
	s_instance_alive = 0;

	// Defer the real destroy — Unity's OpenXR loader calls xrPollEvent AFTER
	// xrDestroyInstance returns, through JIT-generated dispatch trampolines that
	// reference runtime memory (code pages, dispatch tables, session/compositor
	// objects). If we destroy now, those trampolines read freed pages → SIGSEGV.
	//
	// Instead, mark everything as dead (guards will reject API calls) but keep
	// the runtime instance alive. The actual destroy happens at the start of
	// the next instance lifecycle in displayxr_install_hooks().
	s_deferred_destroy_instance = instance;
	s_deferred_destroy_instance_fn = s_real_destroy_instance;

	// Null out function pointers so our guards reject post-destroy calls,
	// but the runtime's actual objects stay allocated and mapped.
	s_real_locate_views = nullptr;
	s_real_get_system_properties = nullptr;
	s_real_create_session = nullptr;
	s_real_destroy_session = nullptr;
	s_real_end_frame = nullptr;
	s_real_create_reference_space = nullptr;
	s_real_locate_space = nullptr;
	s_real_poll_event = nullptr;
	s_real_destroy_instance = nullptr;
	s_real_enumerate_swapchain_formats = nullptr;
	s_real_create_swapchain = nullptr;
	s_real_enumerate_swapchain_images = nullptr;
	s_real_acquire_swapchain_image = nullptr;
	s_real_wait_swapchain_image = nullptr;
	s_real_release_swapchain_image = nullptr;
#if defined(_WIN32)
	s_real_destroy_swapchain = nullptr;
#endif

	if (s_backend) { s_backend->on_destroy(); delete s_backend; s_backend = nullptr; }
	s_backend_type = kBackendNone;

	displayxr_log( "[DisplayXR] xrDestroyInstance END (deferred, returning XR_SUCCESS)\n");
	s_instance = XR_NULL_HANDLE;
	s_session = XR_NULL_HANDLE;
	s_local_space = XR_NULL_HANDLE;
	s_view_space = XR_NULL_HANDLE;
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL
hooked_xrPollEvent(XrInstance instance, XrEventDataBuffer *eventData)
{
	// Load function pointer into local BEFORE any guards, so the compiler
	// can't reorder the load past the null check.
	PFN_xrPollEvent poll_fn = s_real_poll_event;

	// Guard 1: instance dead or function pointer nulled
	if (!s_instance_alive || poll_fn == nullptr) {
		return XR_EVENT_UNAVAILABLE;
	}

	// Guard 2: stop polling after EXITING event
	if (s_stop_polling) {
		return XR_EVENT_UNAVAILABLE;
	}

	XrResult result = poll_fn(instance, eventData);

	// After EXITING, null out the function pointer to prevent ALL future calls.
	// This is the nuclear guard: even if s_stop_polling is somehow reset,
	// the nullptr check in guard 1 will catch it.
	if (result == XR_SUCCESS && eventData != nullptr &&
	    eventData->type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
		const XrEventDataSessionStateChanged *ssc =
			(const XrEventDataSessionStateChanged *)eventData;
		if (ssc->state == XR_SESSION_STATE_EXITING ||
		    ssc->state == XR_SESSION_STATE_LOSS_PENDING) {
			displayxr_log( "[DisplayXR] xrPollEvent: EXITING detected, nulling poll function\n");
			s_stop_polling = 1;
			s_real_poll_event = nullptr; // Nuclear: guard 1 catches all future calls
			s_instance_alive = 0;        // Belt and suspenders
		}
	}

	return result;
}


// ============================================================================
// Hook installation — called by Unity's OpenXR Feature
// ============================================================================

XrResult XRAPI_CALL
displayxr_hook_xrGetInstanceProcAddr(XrInstance instance, const char *name, PFN_xrVoidFunction *function)
{
	// First get the real function from the next in chain
	XrResult result = s_next_gipa(instance, name, function);
	if (XR_FAILED(result)) {
		return result;
	}

	// Cache instance handle
	if (instance != XR_NULL_HANDLE) {
		s_instance = instance;
		s_instance_alive = 1;
	}

	// Pin the runtime library in memory so Unity's dlclose doesn't unmap
	// its code pages. We defer xrDestroySession/xrDestroyInstance to keep
	// runtime objects alive, but Unity calls Internal_UnloadOpenXRLibrary()
	// which dlcloses the runtime. RTLD_NODELETE prevents the unmap, so
	// post-destroy xrPollEvent calls (from editor repaint paths) can still
	// safely reach the runtime's dispatch stubs.
#if !defined(_WIN32)
	if (!s_runtime_pinned && *function != nullptr) {
		Dl_info dl_info;
		if (dladdr((void *)*function, &dl_info) && dl_info.dli_fname) {
			void *handle = dlopen(dl_info.dli_fname, RTLD_LAZY | RTLD_NODELETE);
			if (handle) {
				displayxr_log( "[DisplayXR] Pinned runtime library: %s\n", dl_info.dli_fname);
				s_runtime_pinned = 1;
				dlclose(handle); // Decrement refcount but RTLD_NODELETE keeps it mapped
			}
		}
	}
#endif

	// Log function resolution for debugging second-instance issues
	displayxr_log( "[DisplayXR] xrGetInstanceProcAddr: resolving '%s'\n", name);

	// Intercept specific functions
	if (strcmp(name, "xrLocateViews") == 0) {
		s_real_locate_views = (PFN_xrLocateViews)*function;
		*function = (PFN_xrVoidFunction)hooked_xrLocateViews;
	} else if (strcmp(name, "xrGetSystemProperties") == 0) {
		s_real_get_system_properties = (PFN_xrGetSystemProperties)*function;
		*function = (PFN_xrVoidFunction)hooked_xrGetSystemProperties;
	} else if (strcmp(name, "xrCreateSession") == 0) {
		s_real_create_session = (PFN_xrCreateSession)*function;
		*function = (PFN_xrVoidFunction)hooked_xrCreateSession;
	} else if (strcmp(name, "xrDestroySession") == 0) {
		s_real_destroy_session = (PFN_xrDestroySession)*function;
		*function = (PFN_xrVoidFunction)hooked_xrDestroySession;
	} else if (strcmp(name, "xrEndFrame") == 0) {
		s_real_end_frame = (PFN_xrEndFrame)*function;
		*function = (PFN_xrVoidFunction)hooked_xrEndFrame;
	} else if (strcmp(name, "xrCreateReferenceSpace") == 0) {
		s_real_create_reference_space = (PFN_xrCreateReferenceSpace)*function;
		// Cache only — we call this ourselves to create our LOCAL + VIEW
		// reference spaces (s_local_space, s_view_space) in hooked_xrCreateSession.
	} else if (strcmp(name, "xrLocateSpace") == 0) {
		s_real_locate_space = (PFN_xrLocateSpace)*function;
		// Cache only — we call this ourselves for Unity URP head-pose
		// compensation in hooked_xrLocateViews (issue #115).
	} else if (strcmp(name, "xrPollEvent") == 0) {
		s_real_poll_event = (PFN_xrPollEvent)*function;
		*function = (PFN_xrVoidFunction)hooked_xrPollEvent;
	} else if (strcmp(name, "xrDestroyInstance") == 0) {
		s_real_destroy_instance = (PFN_xrDestroyInstance)*function;
		*function = (PFN_xrVoidFunction)hooked_xrDestroyInstance;
	}
	// --- Zone-sized eye render (avatar-faithful, XR_EXT_display_zones) ---
	else if (strcmp(name, "xrEnumerateViewConfigurationViews") == 0) {
		s_real_enumerate_view_configuration_views = (PFN_xrEnumerateViewConfigurationViews)*function;
		*function = (PFN_xrVoidFunction)hooked_xrEnumerateViewConfigurationViews;
	}
	// --- Swapchain diagnostic hooks (issue #36) ---
	else if (strcmp(name, "xrEnumerateSwapchainFormats") == 0) {
		s_real_enumerate_swapchain_formats = (PFN_xrEnumerateSwapchainFormats)*function;
		*function = (PFN_xrVoidFunction)hooked_xrEnumerateSwapchainFormats;
	}
#if defined(_WIN32)
	else if (strcmp(name, "xrDestroySwapchain") == 0) {
		s_real_destroy_swapchain = (PFN_xrDestroySwapchain)*function;
		// No hook needed — just capture the pointer for typed swapchain cleanup.
	}
#endif
	else if (strcmp(name, "xrRequestDisplayRenderingModeEXT") == 0) {
		*function = (PFN_xrVoidFunction)hooked_xrRequestDisplayRenderingModeEXT;
	}
#if defined(ENABLE_VULKAN)
	else if (strcmp(name, "xrCreateVulkanDeviceKHR") == 0) {
		s_real_create_vulkan_device = (DxrPFN_xrCreateVulkanDeviceKHR)*function;
		*function = (PFN_xrVoidFunction)hooked_xrCreateVulkanDeviceKHR;
	}
#endif
	else if (strcmp(name, "xrCreateSwapchain") == 0) {
		s_real_create_swapchain = (PFN_xrCreateSwapchain)*function;
		*function = (PFN_xrVoidFunction)hooked_xrCreateSwapchain;
	} else if (strcmp(name, "xrEnumerateSwapchainImages") == 0) {
		s_real_enumerate_swapchain_images = (PFN_xrEnumerateSwapchainImages)*function;
		*function = (PFN_xrVoidFunction)hooked_xrEnumerateSwapchainImages;
	} else if (strcmp(name, "xrAcquireSwapchainImage") == 0) {
		s_real_acquire_swapchain_image = (PFN_xrAcquireSwapchainImage)*function;
		*function = (PFN_xrVoidFunction)hooked_xrAcquireSwapchainImage;
	} else if (strcmp(name, "xrWaitSwapchainImage") == 0) {
		s_real_wait_swapchain_image = (PFN_xrWaitSwapchainImage)*function;
		*function = (PFN_xrVoidFunction)hooked_xrWaitSwapchainImage;
	} else if (strcmp(name, "xrReleaseSwapchainImage") == 0) {
		s_real_release_swapchain_image = (PFN_xrReleaseSwapchainImage)*function;
		*function = (PFN_xrVoidFunction)hooked_xrReleaseSwapchainImage;
	}

	return result;
}

PFN_xrVoidFunction
displayxr_install_hooks(PFN_xrGetInstanceProcAddr next_gipa)
{
	displayxr_log( "[DisplayXR] install_hooks called (new instance lifecycle)\n");

	// Execute deferred session/instance destruction from the previous lifecycle.
	// These were deferred because Unity's loader calls xrPollEvent after
	// xrDestroyInstance through dispatch trampolines that reference runtime memory.
	// The runtime is pinned via RTLD_NODELETE, so the function pointers are still
	// valid and we MUST call them to clean up the runtime's internal state.
	if (s_deferred_destroy_session != XR_NULL_HANDLE && s_deferred_destroy_session_fn != nullptr) {
		displayxr_log("[DisplayXR] Executing deferred xrDestroySession\n");
		s_deferred_destroy_session_fn(s_deferred_destroy_session);
		s_deferred_destroy_session = XR_NULL_HANDLE;
		s_deferred_destroy_session_fn = nullptr;
	}
	if (s_deferred_destroy_instance != XR_NULL_HANDLE && s_deferred_destroy_instance_fn != nullptr) {
		displayxr_log("[DisplayXR] Executing deferred xrDestroyInstance\n");
		s_deferred_destroy_instance_fn(s_deferred_destroy_instance);
		s_deferred_destroy_instance = XR_NULL_HANDLE;
		s_deferred_destroy_instance_fn = nullptr;
	}

	displayxr_state_init();
	s_next_gipa = next_gipa;

	// Reset all state for the new instance lifecycle.
	// Previous function pointers may be stale if the old instance was destroyed.
	s_real_locate_views = nullptr;
	s_real_get_system_properties = nullptr;
	s_real_create_session = nullptr;
	s_real_destroy_session = nullptr;
	s_real_end_frame = nullptr;
	s_real_create_reference_space = nullptr;
	s_real_locate_space = nullptr;
	s_real_poll_event = nullptr;
	s_real_destroy_instance = nullptr;
	s_instance = XR_NULL_HANDLE;
	s_session = XR_NULL_HANDLE;
	s_local_space = XR_NULL_HANDLE;
	s_view_space = XR_NULL_HANDLE;
	s_session_alive = 0;
	s_instance_alive = 0;
	s_stop_polling = 0;

	return (PFN_xrVoidFunction)displayxr_hook_xrGetInstanceProcAddr;
}


// ============================================================================
// P/Invoke exports
// ============================================================================

void
displayxr_stop_polling(void)
{
	displayxr_log( "[DisplayXR] displayxr_stop_polling: killing poll forwarding\n");
	s_stop_polling = 1;
	s_real_poll_event = nullptr;
	s_instance_alive = 0;
	s_session_alive = 0;
}

void
displayxr_destroy_preview_window(void)
{
	DisplayXRState *state = displayxr_get_state();
	if (state->window_handle != nullptr) {
		displayxr_log("[DisplayXR] displayxr_destroy_preview_window\n");
#if defined(__APPLE__)
		displayxr_metal_destroy_preview_window();
#endif
		state->window_handle = nullptr;
	}
}

void
displayxr_set_tunables(float ipd_factor,
                      float parallax_factor,
                      float perspective_factor,
                      float virtual_display_height,
                      float inv_convergence_distance,
                      float fov_override,
                      float near_z,
                      float far_z,
                      int camera_centric,
                      int clip_at_display_plane)
{
	DisplayXRTunables t;
	t.ipd_factor = ipd_factor;
	t.parallax_factor = parallax_factor;
	t.perspective_factor = perspective_factor;
	t.virtual_display_height = virtual_display_height;
	t.inv_convergence_distance = inv_convergence_distance;
	t.fov_override = fov_override;
	t.near_z = near_z > 0.0001f ? near_z : 0.01f;
	t.far_z = far_z > t.near_z ? far_z : 1000.0f;
	t.camera_centric = camera_centric ? 1 : 0;
	t.clip_at_display_plane = clip_at_display_plane ? 1 : 0;
	displayxr_state_set_tunables(&t);
}

void
displayxr_get_display_info(float *display_width_m,
                          float *display_height_m,
                          uint32_t *pixel_width,
                          uint32_t *pixel_height,
                          float *nominal_x,
                          float *nominal_y,
                          float *nominal_z,
                          float *scale_x,
                          float *scale_y,
                          int *is_valid)
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

void
displayxr_get_eye_positions(float *lx, float *ly, float *lz, float *rx, float *ry, float *rz, int *is_tracked)
{
	DisplayXREyePositions eyes = displayxr_state_get_eye_positions();
	*lx = eyes.left_eye.x;
	*ly = eyes.left_eye.y;
	*lz = eyes.left_eye.z;
	*rx = eyes.right_eye.x;
	*ry = eyes.right_eye.y;
	*rz = eyes.right_eye.z;
	*is_tracked = eyes.is_tracked;
}

void
displayxr_set_window_handle(void *handle)
{
	DisplayXRState *state = displayxr_get_state();
	state->window_handle = handle;
}

void
displayxr_set_editor_mode(int enabled)
{
	DisplayXRState *state = displayxr_get_state();
	state->editor_mode = (uint8_t)(enabled != 0);
}

DISPLAYXR_EXPORT void
displayxr_set_transparent_background(int enabled)
{
	DisplayXRState *state = displayxr_get_state();
	state->transparent_background_requested = (uint8_t)(enabled != 0);
	displayxr_log("[DisplayXR] set_transparent_background: requested=%d\n",
	              (int)state->transparent_background_requested);
}

DISPLAYXR_EXPORT void
displayxr_request_simple_window(int enabled)
{
	DisplayXRState *state = displayxr_get_state();
	state->simple_window_requested = (uint8_t)(enabled != 0);
	displayxr_log("[DisplayXR] request_simple_window: requested=%d\n",
	              (int)state->simple_window_requested);
}

DISPLAYXR_EXPORT void
displayxr_surround_set_texture(void *unity_native_tex, uint32_t width, uint32_t height)
{
	// Register a Unity RenderTexture (R8G8B8A8_UNORM) as the 2D surround source
	// (#131). hooked_xrEndFrame copies it into the SHARED surround texture each
	// frame, signals the SHARED fence, and registers via
	// xrSetSharedTextureSurround2DFenceEXT so the runtime fills the non-canvas
	// region post-weave (always full native panel resolution). Pair with
	// displayxr_set_canvas_rect to define where the 3D lives. Pass NULL to clear.
	s_surround_w = width;
	s_surround_h = height;
	s_surround_unity_tex = unity_native_tex;
	displayxr_log("[DisplayXR] surround_set_texture: tex=%p %ux%u\n",
	              unity_native_tex, width, height);
}

DISPLAYXR_EXPORT void
displayxr_surround_clear(void)
{
	s_surround_unity_tex = nullptr;
	if (s_pfn_set_surround_fence && s_session != XR_NULL_HANDLE)
		s_pfn_set_surround_fence(s_session, nullptr, 0, 0, nullptr, 0);
	if (s_backend) s_backend->surround_release();
	displayxr_log("[DisplayXR] surround_clear\n");
}

void
displayxr_set_use_srgb_swapchain(int enabled)
{
	// Must be called BEFORE the OpenXR session creates its swapchains
	// (i.e. from the OpenXR feature's OnInstanceCreate). Reading is wired
	// in displayxr_d3d11_backend.cpp on_swapchain_created and the atlas
	// swapchain creation. 1 = UNORM_SRGB (Unity Linear), 0 = UNORM (Unity Gamma).
	DisplayXRState *state = displayxr_get_state();
	state->use_srgb_swapchain = (uint8_t)(enabled != 0);
	displayxr_log("[DisplayXR] use_srgb_swapchain set to %d (typed sibling will be format %d)\n",
	    state->use_srgb_swapchain, state->use_srgb_swapchain ? 29 : 28);
}

void
displayxr_set_viewport_size(uint32_t width, uint32_t height,
                            int32_t screen_x, int32_t screen_y)
{
	if (s_native_viewport_active)
		return; // WM_SIZE is driving viewport — ignore C# push
	DisplayXRState *state = displayxr_get_state();
	state->viewport_width = width;
	state->viewport_height = height;
	state->viewport_x = screen_x;
	state->viewport_y = screen_y;
}

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

int
displayxr_request_display_mode(int mode_3d)
{
	DisplayXRState *state = displayxr_get_state();
	if (!state->has_display_mode_ext || state->pfn_request_display_mode == nullptr || s_session == XR_NULL_HANDLE) {
		return 0; // Not supported
	}

	XrDisplayModeEXT mode = mode_3d ? XR_DISPLAY_MODE_3D_EXT : XR_DISPLAY_MODE_2D_EXT;
	XrResult result = state->pfn_request_display_mode(s_session, mode);
	return XR_SUCCEEDED(result) ? 1 : 0;
}

DISPLAYXR_EXPORT int
displayxr_capture_atlas(const char *path_prefix, int stage)
{
	// XR_EXT_atlas_capture (#140 / #396 W6): hand the runtime a path prefix and a
	// capture stage; the runtime reads back its own compositor atlas and writes
	// "<prefix>_atlas.png". Non-blocking — the PNG lands on the next composed
	// frame, so XR_SUCCESS means accepted, not on-disk (matches the native apps).
	if (s_pfn_capture_atlas == nullptr || s_session == XR_NULL_HANDLE) {
		displayxr_log("[DisplayXR] capture_atlas: unavailable (pfn=%p session=%p)\n",
		              (void *)s_pfn_capture_atlas, (void *)(uintptr_t)s_session);
		return 0; // Extension not resolved or no live session
	}

	XrAtlasCaptureInfoEXT info = {};
	info.type = XR_TYPE_ATLAS_CAPTURE_INFO_EXT;
	info.next = nullptr;
	info.stage = (stage != 0) ? XR_ATLAS_CAPTURE_STAGE_PROJECTION_ONLY_EXT
	                          : XR_ATLAS_CAPTURE_STAGE_POST_COMPOSE_EXT;
	if (path_prefix != nullptr) {
		strncpy(info.pathPrefix, path_prefix, XR_ATLAS_CAPTURE_PATH_MAX_EXT - 1);
		info.pathPrefix[XR_ATLAS_CAPTURE_PATH_MAX_EXT - 1] = '\0';
	}

	XrResult result = s_pfn_capture_atlas(s_session, &info, nullptr);
	displayxr_log("[DisplayXR] capture_atlas: stage=%d prefix='%s' result=%d\n",
	              (int)info.stage, info.pathPrefix, (int)result);
	return XR_SUCCEEDED(result) ? 1 : 0;
}

void
displayxr_set_scene_transform(float pos_x,
                             float pos_y,
                             float pos_z,
                             float ori_x,
                             float ori_y,
                             float ori_z,
                             float ori_w,
                             float scale_x,
                             float scale_y,
                             float scale_z,
                             int enabled)
{
	DisplayXRSceneTransform t;
	t.position[0] = pos_x;
	t.position[1] = pos_y;
	t.position[2] = pos_z;
	t.orientation[0] = ori_x;
	t.orientation[1] = ori_y;
	t.orientation[2] = ori_z;
	t.orientation[3] = ori_w;
	t.scale[0] = scale_x;
	t.scale[1] = scale_y;
	t.scale[2] = scale_z;
	t.enabled = enabled ? 1 : 0;
	displayxr_state_set_scene_transform(&t);
}

void
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

void
displayxr_get_readback(uint8_t **pixels, uint32_t *width, uint32_t *height, int *ready)
{
	DisplayXRState *state = displayxr_get_state();
	*pixels = state->readback_pixels;
	*width = state->readback_width;
	*height = state->readback_height;
	*ready = state->readback_ready;
}

// Deprecated: shared texture functions are no longer used.
// The plugin now creates its own native window and passes it to the runtime.
void *
displayxr_create_shared_texture(uint32_t width, uint32_t height)
{
	(void)width; (void)height;
	return nullptr;
}

void
displayxr_destroy_shared_texture(void)
{
}

void
displayxr_get_shared_texture(void **native_ptr, uint32_t *width, uint32_t *height, int *ready)
{
	*native_ptr = nullptr;
	*width = 0;
	*height = 0;
	*ready = 0;
}

void
displayxr_set_canvas_rect(int32_t x, int32_t y, uint32_t w, uint32_t h)
{
	// Cache the rect so hooked_xrEndFrame re-applies it each frame (#34/#131):
	// the 3D weaves into this sub-rect, the rest becomes the 2D surround region.
	// w==0 || h==0 clears (full-window canvas). Also apply immediately if live.
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
	if (s_pfn_set_output_rect && s_session != XR_NULL_HANDLE)
		s_pfn_set_output_rect(s_session, x, y, w, h);
	displayxr_log("[DisplayXR] set_canvas_rect: (%d,%d) %ux%u\n", x, y, w, h);
}

void
displayxr_set_3d_zone_rect(int32_t x, int32_t y, int32_t w, int32_t h)
{
	// XR_EXT_display_zones: define the 3D-zone rect (client-window pixels,
	// top-left origin) the runtime frames the Kooima 3D into. hooked_xrLocateViews
	// chains it in front of the rig (zone-scoped projection) and hooked_xrEndFrame
	// chains it on the projection layer (binding the views into the rect). w<=0 ||
	// h<=0 clears. Inert until the runtime advertises XR_EXT_display_zones and
	// reports caps.supported (the locate/endframe hooks gate on dxr_zones_ready).
	if (w <= 0 || h <= 0) {
		s_zone_valid = 0;
		displayxr_log("[DisplayXR] set_3d_zone_rect: cleared\n");
		return;
	}
	s_zone_x = x;
	s_zone_y = y;
	s_zone_w = w;
	s_zone_h = h;
	s_zone_valid = 1;
	displayxr_log("[DisplayXR] set_3d_zone_rect: (%d,%d) %dx%d\n", x, y, w, h);
}

void
displayxr_clear_3d_zone(void)
{
	s_zone_valid = 0;
	displayxr_log("[DisplayXR] clear_3d_zone\n");
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

DISPLAYXR_EXPORT void
displayxr_get_render_target_size(uint32_t *out_w, uint32_t *out_h)
{
	// The runtime weaves into the bound HWND's client area, which on Leia SR is
	// NOT the display panel size (the SR weaver oversizes/crops the window for
	// lenticular phase alignment). 2D surround + canvas sub-rect must be sized in
	// THESE pixels, not the display panel dims (#131). Returns 0x0 if no window.
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
