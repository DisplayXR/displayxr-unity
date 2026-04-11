// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
//
// OpenXR function interception layer for the DisplayXR Unity plugin.
// Hooks into Unity's OpenXR loader chain via HookGetInstanceProcAddr.

#include "displayxr_hooks.h"
#include "displayxr_extensions.h"
#include "displayxr_kooima.h"
#include "displayxr_shared_state.h"
#include "displayxr_readback.h"
#include "camera3d_view.h"
#include <math.h>

#if defined(__APPLE__)
#include "displayxr_metal.h"
#elif defined(_WIN32)
#include "displayxr_win32.h"
#include <d3d11.h>
#endif

#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#if !defined(_WIN32)
#include <dlfcn.h>
#endif

// --- Logging helper ---
// On Windows built apps, fprintf(stderr) goes nowhere (no console).
// Write to a file next to the executable so logs are always accessible.
void displayxr_log(const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
#if defined(_WIN32)
	// Append to displayxr.log next to the executable
	static FILE *s_logfile = nullptr;
	static int s_log_init = 0;
	if (!s_log_init) {
		s_log_init = 1;
		s_logfile = fopen("displayxr.log", "w");
	}
	if (s_logfile) {
		va_list args2;
		va_copy(args2, args);
		vfprintf(s_logfile, fmt, args2);
		fflush(s_logfile);
		va_end(args2);
	}
	// Also OutputDebugString for Visual Studio / DbgView
	char buf[2048];
	vsnprintf(buf, sizeof(buf), fmt, args);
	OutputDebugStringA(buf);
#else
	vfprintf(stderr, fmt, args);
#endif
	va_end(args);
}

// --- D3D11 swapchain image struct (from openxr_platform.h) ---
// Defined inline to avoid requiring XR_USE_GRAPHICS_API_D3D11 globally.
#if defined(_WIN32)
typedef struct XrSwapchainImageD3D11KHR {
	XrStructureType type;
	void *next;
	ID3D11Texture2D *texture;
} XrSwapchainImageD3D11KHR;
#endif

// --- Stored real function pointers ---
static PFN_xrGetInstanceProcAddr s_next_gipa = nullptr;
static PFN_xrLocateViews s_real_locate_views = nullptr;
static PFN_xrGetSystemProperties s_real_get_system_properties = nullptr;
static PFN_xrCreateSession s_real_create_session = nullptr;
static PFN_xrDestroySession s_real_destroy_session = nullptr;
static PFN_xrEndFrame s_real_end_frame = nullptr;
static PFN_xrCreateReferenceSpace s_real_create_reference_space = nullptr;
static volatile PFN_xrPollEvent s_real_poll_event = nullptr;
static PFN_xrDestroyInstance s_real_destroy_instance = nullptr;

// --- Swapchain diagnostic hooks (issue #36: D3D11 black screen) ---
static PFN_xrEnumerateSwapchainFormats s_real_enumerate_swapchain_formats = nullptr;
static PFN_xrCreateSwapchain s_real_create_swapchain = nullptr;
static PFN_xrEnumerateSwapchainImages s_real_enumerate_swapchain_images = nullptr;
static PFN_xrAcquireSwapchainImage s_real_acquire_swapchain_image = nullptr;
static PFN_xrWaitSwapchainImage s_real_wait_swapchain_image = nullptr;
static PFN_xrReleaseSwapchainImage s_real_release_swapchain_image = nullptr;

// --- D3D11 GPU sync (issue #36: rendering artifacts from async GPU) ---
#if defined(_WIN32)
static ID3D11Device *s_d3d11_device = nullptr;
static ID3D11DeviceContext *s_d3d11_context = nullptr;

// --- D3D11 typed swapchain substitution (issue #91: TYPELESS compositor X-pattern) ---
// Runtime creates R8G8B8A8_TYPELESS swapchain textures (OpenXR D3D11 spec). The
// compositor cannot create valid SRVs from TYPELESS textures for lenticular weaving,
// causing the X-pattern artifact in standalone D3D11 builds.
//
// Fix: for each color swapchain Unity creates, we silently create a parallel
// R8G8B8A8_UNORM_SRGB swapchain. xrEnumerateSwapchainImages returns the typed
// swapchain's images so Unity renders into typed textures. Acquire/wait/release are
// mirrored on both swapchains. In xrEndFrame we temporarily replace the TYPELESS
// swapchain handles with the typed ones so the compositor gets typed textures.
// No CopyResource needed — Unity renders directly into the typed swapchain.

#define DISPLAYXR_MAX_SC_SUBS 8

struct D3D11ScSub {
	XrSwapchain unity_sc;  // TYPELESS swapchain created by runtime
	XrSwapchain typed_sc;  // R8G8B8A8_UNORM_SRGB swapchain we created
	uint32_t width, height; // swapchain pixel dimensions (from xrCreateSwapchain)
	bool active;
	// SBS composite support
	ID3D11Texture2D *typed_textures[8]; // textures from xrEnumerateSwapchainImages
	uint32_t typed_img_count;
	uint32_t current_idx;       // image index acquired this frame
	bool release_pending;       // deferred: composite runs before actual typed_sc release
};

static D3D11ScSub s_sc_subs[DISPLAYXR_MAX_SC_SUBS];
static int s_sc_sub_count = 0;

static PFN_xrDestroySwapchain s_real_destroy_swapchain = nullptr;

// Rendering mode tracking — enumerated at xrCreateSession, current index updated
// via hooked_xrRequestDisplayRenderingModeEXT. Used to compute the tile layout
// (tile_cols × tile_rows) and view_w × view_h for the atlas composite below.
#define DISPLAYXR_MAX_RENDERING_MODES 16
static PFN_xrEnumerateDisplayRenderingModesEXT s_real_enumerate_rendering_modes = nullptr;
static PFN_xrRequestDisplayRenderingModeEXT s_real_request_rendering_mode = nullptr;
static XrDisplayRenderingModeInfoEXT s_rendering_modes[DISPLAYXR_MAX_RENDERING_MODES];
static uint32_t s_rendering_mode_count = 0;
static uint32_t s_current_rendering_mode_index = 0;

// Tile-layout atlas output swapchain: a single swapchain sized to the current
// rendering mode's atlas dimensions (tile_cols × view_w by tile_rows × view_h),
// into which we composite N typed shadow textures (one per view) at their tile
// positions. Submitting this with per-tile imageRects satisfies u_tiling_can_zero_copy
// and routes the runtime to its zero-copy fast path regardless of the number of
// views. Recreated when the rendering mode changes (we clear s_atlas_sc to
// XR_NULL_HANDLE on mode change and the next xrEndFrame recreates it).
static XrSwapchain s_atlas_sc = XR_NULL_HANDLE;
static ID3D11Texture2D *s_atlas_textures[8] = {};
static uint32_t s_atlas_img_count = 0;
static uint32_t s_atlas_width = 0;
static uint32_t s_atlas_height = 0;
static uint32_t s_atlas_mode_index = 0xFFFFFFFFu; // the mode index the current atlas was sized for

static const XrDisplayRenderingModeInfoEXT *d3d11_get_current_rendering_mode()
{
	for (uint32_t i = 0; i < s_rendering_mode_count; i++) {
		if (s_rendering_modes[i].modeIndex == s_current_rendering_mode_index)
			return &s_rendering_modes[i];
	}
	return (s_rendering_mode_count > 0) ? &s_rendering_modes[0] : nullptr;
}

static void d3d11_destroy_atlas_swapchain()
{
	if (s_atlas_sc != XR_NULL_HANDLE && s_real_destroy_swapchain) {
		s_real_destroy_swapchain(s_atlas_sc);
	}
	s_atlas_sc = XR_NULL_HANDLE;
	for (uint32_t i = 0; i < 8; i++) s_atlas_textures[i] = nullptr;
	s_atlas_img_count = 0;
	s_atlas_width = 0;
	s_atlas_height = 0;
	s_atlas_mode_index = 0xFFFFFFFFu;
}

static void d3d11_sub_cleanup_all()
{
	for (int i = 0; i < s_sc_sub_count; i++) {
		if (s_sc_subs[i].active && s_sc_subs[i].typed_sc != XR_NULL_HANDLE) {
			if (s_real_destroy_swapchain)
				s_real_destroy_swapchain(s_sc_subs[i].typed_sc);
			s_sc_subs[i].typed_sc = XR_NULL_HANDLE;
		}
		s_sc_subs[i].active = false;
	}
	s_sc_sub_count = 0;
	d3d11_destroy_atlas_swapchain();
	s_rendering_mode_count = 0;
	s_current_rendering_mode_index = 0;
}

static D3D11ScSub *d3d11_sub_find(XrSwapchain unity_sc)
{
	for (int i = 0; i < s_sc_sub_count; i++) {
		if (s_sc_subs[i].active && s_sc_subs[i].unity_sc == unity_sc)
			return &s_sc_subs[i];
	}
	return nullptr;
}
#endif

static XrInstance s_instance = XR_NULL_HANDLE;
static XrSession s_session = XR_NULL_HANDLE;
static XrSpace s_local_space = XR_NULL_HANDLE;
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
#endif

	// Use our LOCAL space if available, otherwise pass through the original space.
	// LOCAL space gives us raw eye positions relative to the display.
	XrViewLocateInfo modified_info = *viewLocateInfo;
	if (s_local_space != XR_NULL_HANDLE) {
		modified_info.space = s_local_space;
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

	// Cache raw eye positions for C# access (before any transforms)
	uint8_t tracked = (viewState->viewStateFlags & XR_VIEW_STATE_POSITION_TRACKED_BIT) != 0;
	displayxr_state_set_eye_positions(&views[0].pose.position, &views[1].pose.position, tracked);


	// Get current tunables, scene transform, and display info
	DisplayXRTunables tunables = displayxr_state_get_tunables();
	DisplayXRSceneTransform scene_xform = displayxr_state_get_scene_transform();
	DisplayXRState *state = displayxr_get_state();
	DisplayXRDisplayInfo *di = &state->display_info;

	if (!di->is_valid) {
		static int s_no_di_count = 0;
		if (s_no_di_count++ % 60 == 0) {
			displayxr_log( "[DisplayXR] xrLocateViews: display_info NOT valid, passing through raw views "
			        "(raw_L=(%.3f,%.3f,%.3f) raw_R=(%.3f,%.3f,%.3f))\n",
			        views[0].pose.position.x, views[0].pose.position.y, views[0].pose.position.z,
			        views[1].pose.position.x, views[1].pose.position.y, views[1].pose.position.z);
		}
		return result; // No display info — pass through unmodified
	}

	// Delegate to canonical view libraries for IPD/parallax/Kooima.
	// Raw eye positions and scene transform (camera/display pose) are passed
	// to the libraries, matching the native test app's pipeline exactly.
	XrVector3f raw_left = views[0].pose.position;
	XrVector3f raw_right = views[1].pose.position;
	XrVector3f nominal = {di->nominal_viewer_x, di->nominal_viewer_y, di->nominal_viewer_z};

	// Window-relative Kooima (ADR-012): screen = actual window physical size,
	// eye positions shifted by window-center offset on monitor.
	Display3DScreen screen = {di->display_width_meters, di->display_height_meters};
	float eyeOffX_h = 0, eyeOffY_h = 0;
	if (state->viewport_width > 0 && state->viewport_height > 0 &&
	    di->display_pixel_width > 0 && di->display_pixel_height > 0) {
		float px_size_x = di->display_width_meters / (float)di->display_pixel_width;
		float px_size_y = di->display_height_meters / (float)di->display_pixel_height;
		screen.width_m = (float)state->viewport_width * px_size_x;
		screen.height_m = (float)state->viewport_height * px_size_y;

		// Shift eyes from display-center to window-center coordinates
		float winCenterX = (float)state->viewport_x + (float)state->viewport_width * 0.5f;
		float winCenterY = (float)state->viewport_y + (float)state->viewport_height * 0.5f;
		float dispCenterX = (float)di->display_pixel_width * 0.5f;
		float dispCenterY = (float)di->display_pixel_height * 0.5f;
		eyeOffX_h = (winCenterX - dispCenterX) * px_size_x;
		eyeOffY_h = (winCenterY - dispCenterY) * px_size_y;
#ifdef _WIN32
		eyeOffY_h = -eyeOffY_h; // Win32 Y is top-down, eye coords are Y-up
#endif
		raw_left.x -= eyeOffX_h;
		raw_left.y -= eyeOffY_h;
		raw_right.x -= eyeOffX_h;
		raw_right.y -= eyeOffY_h;
		nominal.x -= eyeOffX_h;
		nominal.y -= eyeOffY_h;
	}

	// Log Kooima params on viewport resize/move
	{
		static uint32_t s_prev_vp_w = 0, s_prev_vp_h = 0;
		static int32_t s_prev_vp_x = 0, s_prev_vp_y = 0;
		if (state->viewport_width != s_prev_vp_w || state->viewport_height != s_prev_vp_h ||
		    state->viewport_x != s_prev_vp_x || state->viewport_y != s_prev_vp_y) {
			s_prev_vp_w = state->viewport_width;
			s_prev_vp_h = state->viewport_height;
			s_prev_vp_x = state->viewport_x;
			s_prev_vp_y = state->viewport_y;
			displayxr_log("[DisplayXR] Kooima hooks: vp=%ux%u@(%d,%d) disp=%ux%u "
			              "screen=%.4fx%.4fm eyeOff=(%.4f,%.4f) "
			              "nom=(%.4f,%.4f,%.4f)\n",
			              state->viewport_width, state->viewport_height,
			              state->viewport_x, state->viewport_y,
			              di->display_pixel_width, di->display_pixel_height,
			              screen.width_m, screen.height_m,
			              eyeOffX_h, eyeOffY_h,
			              nominal.x, nominal.y, nominal.z);
		}
	}

	// Build pose from scene transform (Unity camera/display world pose).
	// Convert Unity coords (left-hand, +Z forward) to OpenXR (right-hand, -Z forward):
	// position Z negated, quaternion (x,y) negated + (z,w) kept.
	XrPosef scene_pose = {};
	if (scene_xform.enabled) {
		scene_pose.position = XrVector3f{
			scene_xform.position[0],
			scene_xform.position[1],
			-scene_xform.position[2]};
		scene_pose.orientation = XrQuaternionf{
			-scene_xform.orientation[0],
			-scene_xform.orientation[1],
			scene_xform.orientation[2],
			scene_xform.orientation[3]};
	} else {
		scene_pose.orientation = XrQuaternionf{0, 0, 0, 1};
		scene_pose.position = XrVector3f{0, 0, 0};
	}

	if (tunables.camera_centric) {
		// Camera-centric: tangent-based Kooima (camera3d_view library)
		// scene_pose = Unity camera world pose converted to OpenXR coords.
		static int s_cam_log = 0;
		if (s_cam_log++ % 60 == 0) {
			displayxr_log( "[DisplayXR] CAM-CENTRIC: scene_pose=(%.3f,%.3f,%.3f) "
			        "raw_L=(%.3f,%.3f,%.3f) raw_R=(%.3f,%.3f,%.3f) "
			        "nominal=(%.3f,%.3f,%.3f) invd=%.4f half_tan_vfov=%.4f "
			        "scale=(%.3f,%.3f,%.3f)\n",
			        scene_pose.position.x, scene_pose.position.y, scene_pose.position.z,
			        raw_left.x, raw_left.y, raw_left.z,
			        raw_right.x, raw_right.y, raw_right.z,
			        nominal.x, nominal.y, nominal.z,
			        tunables.inv_convergence_distance,
			        tunables.fov_override,
			        scene_xform.scale[0], scene_xform.scale[1], scene_xform.scale[2]);
		}
		Camera3DTunables cam_tunables;
		cam_tunables.ipd_factor = tunables.ipd_factor;
		cam_tunables.parallax_factor = tunables.parallax_factor;
		cam_tunables.half_tan_vfov = tunables.fov_override;

		// Parent camera scale: multiply eye positions and nominal viewer,
		// divide inv_convergence_distance by sz.
		float sx = (scene_xform.scale[0] > 0.001f) ? scene_xform.scale[0] : 1.0f;
		float sy = (scene_xform.scale[1] > 0.001f) ? scene_xform.scale[1] : 1.0f;
		float sz = (scene_xform.scale[2] > 0.001f) ? scene_xform.scale[2] : 1.0f;

		cam_tunables.inv_convergence_distance = tunables.inv_convergence_distance / sz;

		XrVector3f raw_eyes[2] = {raw_left, raw_right};
		for (int i = 0; i < 2; i++) {
			raw_eyes[i].x *= sx;
			raw_eyes[i].y *= sy;
			raw_eyes[i].z *= sz;
		}

		nominal.x *= sx;
		nominal.y *= sy;
		nominal.z *= sz;

		Camera3DView cam_views[2];
		camera3d_compute_views(
			raw_eyes, 2,
			&nominal, &screen, &cam_tunables,
			&scene_pose,
			tunables.near_z, tunables.far_z,
			cam_views);

		views[0].fov = cam_views[0].fov;
		views[1].fov = cam_views[1].fov;

		views[0].pose.position = cam_views[0].eye_world;
		views[1].pose.position = cam_views[1].eye_world;
		views[0].pose.orientation = scene_pose.orientation;
		views[1].pose.orientation = scene_pose.orientation;

		// Store Kooima matrices for C# stereo override
		DisplayXRStereoMatrices mats = {};
		memcpy(mats.left_view, cam_views[0].view_matrix, sizeof(float) * 16);
		memcpy(mats.left_projection, cam_views[0].projection_matrix, sizeof(float) * 16);
		memcpy(mats.right_view, cam_views[1].view_matrix, sizeof(float) * 16);
		memcpy(mats.right_projection, cam_views[1].projection_matrix, sizeof(float) * 16);
		mats.valid = 1;
		displayxr_state_set_stereo_matrices(&mats);

		if (s_cam_log % 60 == 1) {
			displayxr_log( "[DisplayXR] OUTPUT L: eye_world=(%.3f,%.3f,%.3f) "
			        "fov=(L=%.1f R=%.1f U=%.1f D=%.1f)\n",
			        cam_views[0].eye_world.x, cam_views[0].eye_world.y, cam_views[0].eye_world.z,
			        cam_views[0].fov.angleLeft * 57.2958f, cam_views[0].fov.angleRight * 57.2958f,
			        cam_views[0].fov.angleUp * 57.2958f, cam_views[0].fov.angleDown * 57.2958f);
		}
	} else {
		// Display-centric: atan-based Kooima (display3d_view library)
		static int s_disp_log = 0;
		if (s_disp_log++ % 60 == 0) {
			displayxr_log("[DisplayXR] DISP-CENTRIC: scene_pose=(%.3f,%.3f,%.3f) "
				"scale=(%.3f,%.3f,%.3f) vdh=%.3f cam_centric=%d "
				"raw_L=(%.3f,%.3f,%.3f) raw_R=(%.3f,%.3f,%.3f)\n",
				scene_pose.position.x, scene_pose.position.y, scene_pose.position.z,
				scene_xform.scale[0], scene_xform.scale[1], scene_xform.scale[2],
				tunables.virtual_display_height, tunables.camera_centric,
				raw_left.x, raw_left.y, raw_left.z,
				raw_right.x, raw_right.y, raw_right.z);
		}

		float sx = (scene_xform.scale[0] > 0.001f) ? scene_xform.scale[0] : 1.0f;
		float sy = (scene_xform.scale[1] > 0.001f) ? scene_xform.scale[1] : 1.0f;
		float sz = (scene_xform.scale[2] > 0.001f) ? scene_xform.scale[2] : 1.0f;

		// Primary zoom via virtual_display_height (reference app approach)
		float vdh = tunables.virtual_display_height / sy;

		// Anisotropic corrections: m2v gives uniform 1/sy; these extra
		// factors achieve per-axis 1/sx, 1/sz on top.
		float ax = sy / sx;   // 1.0 for uniform scale
		float az = sy / sz;   // 1.0 for uniform scale

		// Adjust screen width for X-axis aspect ratio
		screen.width_m *= ax;

		Display3DTunables disp_tunables;
		disp_tunables.ipd_factor = tunables.ipd_factor;
		disp_tunables.parallax_factor = tunables.parallax_factor;
		disp_tunables.perspective_factor = tunables.perspective_factor;
		disp_tunables.virtual_display_height = vdh;

		XrVector3f raw_eyes[2] = {raw_left, raw_right};

		// Anisotropic eye position corrections
		for (int i = 0; i < 2; i++) {
			raw_eyes[i].x *= ax;
			raw_eyes[i].z *= az;
		}

		// Scale nominal viewer depth for consistency
		nominal.z *= az;
		Display3DView disp_views[2];
		display3d_compute_views(
			raw_eyes, 2,
			&nominal, &screen, &disp_tunables,
			scene_xform.enabled ? &scene_pose : NULL,
			tunables.near_z, tunables.far_z,
			disp_views);

		views[0].fov = disp_views[0].fov;
		views[1].fov = disp_views[1].fov;
		views[0].pose.position = disp_views[0].eye_world;
		views[1].pose.position = disp_views[1].eye_world;
		views[0].pose.orientation = scene_pose.orientation;
		views[1].pose.orientation = scene_pose.orientation;

		// Store Kooima matrices for C# stereo override
		DisplayXRStereoMatrices mats = {};
		memcpy(mats.left_view, disp_views[0].view_matrix, sizeof(float) * 16);
		memcpy(mats.left_projection, disp_views[0].projection_matrix, sizeof(float) * 16);
		memcpy(mats.right_view, disp_views[1].view_matrix, sizeof(float) * 16);
		memcpy(mats.right_projection, disp_views[1].projection_matrix, sizeof(float) * 16);
		mats.valid = 1;
		displayxr_state_set_stereo_matrices(&mats);
	}

	// Debug: log every 60 frames (AFTER writeback so we see final values)
	static int s_frame_count = 0;
	if (s_frame_count++ % 60 == 0) {
		float l_hfov = (views[0].fov.angleRight - views[0].fov.angleLeft) * 57.2958f;
		float l_vfov = (views[0].fov.angleUp - views[0].fov.angleDown) * 57.2958f;
		displayxr_log(
		        "[DisplayXR] FINALv2: cam_centric=%d "
		        "pos_L=(%.4f,%.4f,%.4f) pos_R=(%.4f,%.4f,%.4f) "
		        "fov_L=(%.2f,%.2f,%.2f,%.2f)deg hfov=%.1f vfov=%.1f "
		        "ori_L=(%.3f,%.3f,%.3f,%.3f)\n",
		        tunables.camera_centric,
		        views[0].pose.position.x, views[0].pose.position.y, views[0].pose.position.z,
		        views[1].pose.position.x, views[1].pose.position.y, views[1].pose.position.z,
		        views[0].fov.angleLeft * 57.2958f, views[0].fov.angleRight * 57.2958f,
		        views[0].fov.angleUp * 57.2958f, views[0].fov.angleDown * 57.2958f,
		        l_hfov, l_vfov,
		        views[0].pose.orientation.x, views[0].pose.orientation.y,
		        views[0].pose.orientation.z, views[0].pose.orientation.w);
	}

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
				displayxr_log( "[DisplayXR] Graphics binding: VULKAN (via MoltenVK on macOS)\n");
			} else if (item->type == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR) {
				displayxr_log( "[DisplayXR] Graphics binding: D3D11\n");
#if defined(_WIN32)
				// Extract D3D11 device for GPU sync in xrReleaseSwapchainImage
				typedef struct { XrStructureType type; const void *next; ID3D11Device *device; } XrGfxBindingD3D11;
				const XrGfxBindingD3D11 *binding = (const XrGfxBindingD3D11 *)item;
				s_d3d11_device = binding->device;
				if (s_d3d11_device) {
					s_d3d11_device->GetImmediateContext(&s_d3d11_context);
					displayxr_log("[DisplayXR] Captured D3D11 device=%p context=%p for GPU sync\n",
					              (void *)s_d3d11_device, (void *)s_d3d11_context);
				}
#endif
			} else if (item->type == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR) {
				displayxr_log( "[DisplayXR] Graphics binding: D3D12\n");
			} else {
				displayxr_log( "[DisplayXR] Session chain struct type=%u\n", (unsigned)item->type);
			}
			item = item->next;
		}
	}

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

			displayxr_log( "[DisplayXR] Injecting win32 window binding: windowHandle=%p, sharedTextureHandle=%p\n",
			        win_binding.windowHandle, win_binding.sharedTextureHandle);

			((XrBaseOutStructure *)last_in_chain)->next = (XrBaseOutStructure *)&win_binding;
#elif defined(__APPLE__)
			static XrCocoaWindowBindingCreateInfoEXT mac_binding = {};
			mac_binding.type = XR_TYPE_COCOA_WINDOW_BINDING_CREATE_INFO_EXT;
			mac_binding.next = nullptr;
			mac_binding.viewHandle = state->window_handle;
			mac_binding.readbackCallback = displayxr_readback_callback;
			mac_binding.readbackUserdata = nullptr;
			mac_binding.sharedIOSurface = nullptr;

			displayxr_log( "[DisplayXR] Injecting cocoa window binding: viewHandle=%p, sharedIOSurface=%p\n",
			        mac_binding.viewHandle, mac_binding.sharedIOSurface);

			((XrBaseOutStructure *)last_in_chain)->next = (XrBaseOutStructure *)&mac_binding;
#endif
		}
	}

	XrResult result = s_real_create_session(instance, createInfo, session);
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
		}

#if defined(_WIN32)
		// Enumerate display rendering modes for the D3D11 atlas composite path.
		// Stores mode tile layouts so hooked_xrEndFrame can size and write into
		// a tiled atlas swapchain regardless of how many views the active mode
		// has (stereo, quad, light field, etc.). Resolved here rather than in
		// xrGetInstanceProcAddr dispatch because these are extension functions
		// that Unity doesn't call itself — we need to resolve them directly.
		if (s_next_gipa != nullptr && s_instance != XR_NULL_HANDLE) {
			if (s_real_enumerate_rendering_modes == nullptr) {
				PFN_xrVoidFunction fn = nullptr;
				if (XR_SUCCEEDED(s_next_gipa(s_instance,
				        "xrEnumerateDisplayRenderingModesEXT", &fn)) && fn) {
					s_real_enumerate_rendering_modes =
					    (PFN_xrEnumerateDisplayRenderingModesEXT)fn;
				}
			}
			if (s_real_request_rendering_mode == nullptr) {
				PFN_xrVoidFunction fn = nullptr;
				if (XR_SUCCEEDED(s_next_gipa(s_instance,
				        "xrRequestDisplayRenderingModeEXT", &fn)) && fn) {
					s_real_request_rendering_mode =
					    (PFN_xrRequestDisplayRenderingModeEXT)fn;
				}
			}
		}

		s_rendering_mode_count = 0;
		if (s_real_enumerate_rendering_modes != nullptr) {
			uint32_t total = 0;
			XrResult er = s_real_enumerate_rendering_modes(
			    s_session, 0, &total, nullptr);
			if (XR_SUCCEEDED(er) && total > 0) {
				if (total > DISPLAYXR_MAX_RENDERING_MODES)
					total = DISPLAYXR_MAX_RENDERING_MODES;
				for (uint32_t i = 0; i < total; i++) {
					s_rendering_modes[i] = {};
					s_rendering_modes[i].type =
					    XR_TYPE_DISPLAY_RENDERING_MODE_INFO_EXT;
				}
				er = s_real_enumerate_rendering_modes(
				    s_session, total, &total, s_rendering_modes);
				if (XR_SUCCEEDED(er)) {
					s_rendering_mode_count = total;
					// Default to first hardware_display_3d mode so we end up
					// targeting a proper stereo/multi-view layout instead of
					// the 2D fallback if one is enumerated first.
					bool picked = false;
					for (uint32_t i = 0; i < total; i++) {
						const XrDisplayRenderingModeInfoEXT *m =
						    &s_rendering_modes[i];
						displayxr_log(
						    "[DisplayXR] Mode[%u]: '%s' views=%u tiles=%ux%u "
						    "viewPx=%ux%u hw3d=%d\n",
						    m->modeIndex, m->modeName, m->viewCount,
						    m->tileColumns, m->tileRows,
						    m->viewWidthPixels, m->viewHeightPixels,
						    (int)m->hardwareDisplay3D);
						if (!picked && m->hardwareDisplay3D) {
							s_current_rendering_mode_index = m->modeIndex;
							picked = true;
						}
					}
					if (!picked && total > 0)
						s_current_rendering_mode_index =
						    s_rendering_modes[0].modeIndex;
					displayxr_log(
					    "[DisplayXR] Rendering mode: %u modes enumerated, "
					    "current=%u\n", total, s_current_rendering_mode_index);
				} else {
					displayxr_log(
					    "[DisplayXR] xrEnumerateDisplayRenderingModesEXT "
					    "(populate) failed: %d\n", er);
				}
			}
		}
#endif
	}

	return result;
}

#if defined(_WIN32)
static XrResult XRAPI_CALL
hooked_xrRequestDisplayRenderingModeEXT(XrSession session, uint32_t modeIndex)
{
	if (s_real_request_rendering_mode == nullptr)
		return XR_ERROR_FUNCTION_UNSUPPORTED;
	XrResult r = s_real_request_rendering_mode(session, modeIndex);
	if (XR_SUCCEEDED(r)) {
		if (modeIndex != s_current_rendering_mode_index) {
			displayxr_log("[DisplayXR] Rendering mode changed: %u -> %u "
			              "(forcing atlas swapchain recreation)\n",
			              s_current_rendering_mode_index, modeIndex);
			s_current_rendering_mode_index = modeIndex;
			// Next xrEndFrame will notice size mismatch and recreate.
		}
	}
	return r;
}
#endif

static XrResult XRAPI_CALL
hooked_xrDestroySession(XrSession session)
{
	displayxr_log( "[DisplayXR] xrDestroySession BEGIN session=%p (DEFERRED)\n", (void *)(uintptr_t)session);
	s_session_alive = 0;
	s_local_space = XR_NULL_HANDLE;
#if defined(_WIN32)
	d3d11_sub_cleanup_all();
#endif

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
	// D3D11 tile-atlas composite (issue #91 — generalized N-view path).
	//
	// Background:
	// - Unity renders into plugin-provided "typed shadow" swapchains
	//   (hooked_xrCreateSwapchain creates a concrete-format sibling per
	//   Unity-facing swapchain, returned from hooked_xrEnumerateSwapchainImages)
	//   so Unity's RTV creation works even though the runtime promotes color
	//   textures to TYPELESS internally.
	// - The runtime D3D11 compositor has a working zero-copy fast path when the
	//   submitted projection layer references a single atlas swapchain with
	//   per-view imageRects matching the current rendering mode's tile layout.
	//   The per-view renderer fallback is latent-broken for multi-swapchain
	//   submissions (tracked as runtime issue #143 — parity with D3D12 renderer).
	// - Unity submits one swapchain per eye, which would otherwise fall into the
	//   broken fallback. To avoid that and to support any tile layout (stereo,
	//   quad, light-field), we composite the typed shadow textures into a single
	//   atlas swapchain sized to the current mode's (tile_cols * view_w,
	//   tile_rows * view_h) and patch all projection views to reference it with
	//   tile rects.
	//
	// Per-frame flow:
	// 1. Look up the current rendering mode's tile layout.
	// 2. Lazily create (or recreate on mode change) s_atlas_sc.
	// 3. Acquire an atlas image, CopySubresourceRegion each view's typed shadow
	//    texture into tile (v % tile_cols, v / tile_cols) of the atlas.
	// 4. Release the deferred typed shadow images + the atlas image.
	// 5. Patch views to reference s_atlas_sc with per-tile imageRects.
	// 6. After s_real_end_frame, restore the original view fields.
	struct EFPatch {
		XrCompositionLayerProjectionView *view;
		XrSwapchain orig_sc;
		XrRect2Di orig_rect;
	};
	// 16 view tiles covers up to a 4×4 tile layout (light-field modes). Bump
	// if a future mode enumerates more tiles than this.
	EFPatch ef_patches[16]; int ef_npatch = 0;
	const int kMaxEfPatches = (int)(sizeof(ef_patches) / sizeof(ef_patches[0]));
	if (s_sc_sub_count > 0 && frameEndInfo != nullptr && frameEndInfo->layers != nullptr) {
		const XrDisplayRenderingModeInfoEXT *mode = d3d11_get_current_rendering_mode();

		// Derive tile layout. Prefer the enumerated mode; fall back to 2×1 SBS
		// with per-view dimensions inferred from the first shadow swapchain.
		uint32_t tile_cols = 0, tile_rows = 0, view_w = 0, view_h = 0;
		if (mode != nullptr && mode->tileColumns > 0 && mode->tileRows > 0 &&
		    mode->viewWidthPixels > 0 && mode->viewHeightPixels > 0) {
			tile_cols = mode->tileColumns;
			tile_rows = mode->tileRows;
			view_w    = mode->viewWidthPixels;
			view_h    = mode->viewHeightPixels;
		} else {
			// Fallback: 2-view SBS inferred from the first typed shadow swapchain.
			// Matches the pre-N-view behavior so stereo apps keep working if the
			// rendering-mode extension isn't exposed.
			for (int si = 0; si < s_sc_sub_count; si++) {
				if (s_sc_subs[si].active) {
					view_w = s_sc_subs[si].width;
					view_h = s_sc_subs[si].height;
					break;
				}
			}
			tile_cols = 2;
			tile_rows = 1;
		}
		uint32_t atlas_w = tile_cols * view_w;
		uint32_t atlas_h = tile_rows * view_h;

		// Recreate the atlas swapchain if the target mode (or its dims) changed.
		if (s_atlas_sc != XR_NULL_HANDLE &&
		    (s_atlas_width != atlas_w || s_atlas_height != atlas_h ||
		     s_atlas_mode_index != s_current_rendering_mode_index)) {
			d3d11_destroy_atlas_swapchain();
		}

		// Lazily create the atlas output swapchain.
		if (s_atlas_sc == XR_NULL_HANDLE && s_real_create_swapchain &&
		    atlas_w > 0 && atlas_h > 0) {
			XrSwapchainCreateInfo atlas_info = {};
			atlas_info.type            = XR_TYPE_SWAPCHAIN_CREATE_INFO;
			atlas_info.format          = 29; // DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
			atlas_info.width           = atlas_w;
			atlas_info.height          = atlas_h;
			atlas_info.sampleCount     = 1;
			atlas_info.faceCount       = 1;
			atlas_info.arraySize       = 1;
			atlas_info.mipCount        = 1;
			atlas_info.usageFlags      = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT
			                           | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
			XrResult sr = s_real_create_swapchain(session, &atlas_info, &s_atlas_sc);
			if (XR_SUCCEEDED(sr) && s_real_enumerate_swapchain_images) {
				uint32_t cnt = 0;
				s_real_enumerate_swapchain_images(s_atlas_sc, 0, &cnt, nullptr);
				if (cnt > 0 && cnt <= 8) {
					XrSwapchainImageD3D11KHR imgs[8] = {};
					for (uint32_t k = 0; k < cnt; k++)
						imgs[k].type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR;
					s_real_enumerate_swapchain_images(
					    s_atlas_sc, cnt, &cnt,
					    (XrSwapchainImageBaseHeader *)imgs);
					s_atlas_img_count = cnt;
					for (uint32_t k = 0; k < cnt; k++)
						s_atlas_textures[k] = imgs[k].texture;
				}
				s_atlas_width = atlas_w;
				s_atlas_height = atlas_h;
				s_atlas_mode_index = s_current_rendering_mode_index;
				displayxr_log(
				    "[DisplayXR] Atlas swapchain created: sc=%p %ux%u imgs=%u "
				    "tiles=%ux%u viewPx=%ux%u mode=%u\n",
				    (void *)(uintptr_t)s_atlas_sc, atlas_w, atlas_h,
				    s_atlas_img_count, tile_cols, tile_rows, view_w, view_h,
				    s_current_rendering_mode_index);
			} else {
				displayxr_log(
				    "[DisplayXR] Atlas swapchain creation FAILED: %d\n", (int)sr);
			}
		}

		if (s_atlas_sc != XR_NULL_HANDLE) {
			// Acquire + wait one atlas image for this frame.
			uint32_t atlas_idx = 0;
			XrSwapchainImageAcquireInfo acq = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
			XrSwapchainImageWaitInfo    wai = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
			wai.timeout = XR_INFINITE_DURATION;
			s_real_acquire_swapchain_image(s_atlas_sc, &acq, &atlas_idx);
			s_real_wait_swapchain_image(s_atlas_sc, &wai);
			ID3D11Texture2D *atlas_tex = (atlas_idx < s_atlas_img_count)
			    ? s_atlas_textures[atlas_idx] : nullptr;

			uint32_t tile_cap = tile_cols * tile_rows;

			// Walk projection layers, tile each view's shadow texture into the
			// atlas, and patch the view to reference the atlas tile.
			for (uint32_t i = 0; i < frameEndInfo->layerCount; i++) {
				const XrCompositionLayerBaseHeader *hdr = frameEndInfo->layers[i];
				if (!hdr || hdr->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION) continue;
				const XrCompositionLayerProjection *proj =
				    (const XrCompositionLayerProjection*)hdr;
				if (!proj->views || proj->viewCount == 0) continue;
				XrCompositionLayerProjectionView *views =
				    (XrCompositionLayerProjectionView*)proj->views;

				uint32_t n = proj->viewCount;
				if (n > tile_cap) n = tile_cap;

				for (uint32_t v = 0; v < n && ef_npatch < kMaxEfPatches; v++) {
					D3D11ScSub *sub = d3d11_sub_find(views[v].subImage.swapchain);
					if (!sub) continue;

					uint32_t col = v % tile_cols;
					uint32_t row = v / tile_cols;
					uint32_t dst_x = col * view_w;
					uint32_t dst_y = row * view_h;

					// Copy the shadow texture into its tile. Use min(source,
					// tile) dims so oversized shadows don't overrun neighbors.
					if (atlas_tex && s_d3d11_context) {
						ID3D11Texture2D *src_tex =
						    (sub->current_idx < sub->typed_img_count)
						        ? sub->typed_textures[sub->current_idx]
						        : nullptr;
						if (src_tex) {
							uint32_t copy_w = (sub->width  < view_w) ? sub->width  : view_w;
							uint32_t copy_h = (sub->height < view_h) ? sub->height : view_h;
							D3D11_BOX box = { 0, 0, 0, copy_w, copy_h, 1 };
							s_d3d11_context->CopySubresourceRegion(
							    atlas_tex, 0, dst_x, dst_y, 0,
							    src_tex, 0, &box);
						}
					}

					EFPatch &p = ef_patches[ef_npatch++];
					p.view     = &views[v];
					p.orig_sc  = views[v].subImage.swapchain;
					p.orig_rect = views[v].subImage.imageRect;
					views[v].subImage.swapchain               = s_atlas_sc;
					views[v].subImage.imageRect.offset.x      = (int32_t)dst_x;
					views[v].subImage.imageRect.offset.y      = (int32_t)dst_y;
					views[v].subImage.imageRect.extent.width  = (int32_t)view_w;
					views[v].subImage.imageRect.extent.height = (int32_t)view_h;
				}
			}

			if (s_d3d11_context) s_d3d11_context->Flush();

			// Release the deferred typed shadow images, then the atlas image.
			XrSwapchainImageReleaseInfo rel = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
			for (int si = 0; si < s_sc_sub_count; si++) {
				if (s_sc_subs[si].active && s_sc_subs[si].release_pending) {
					s_real_release_swapchain_image(s_sc_subs[si].typed_sc, &rel);
					s_sc_subs[si].release_pending = false;
				}
			}
			s_real_release_swapchain_image(s_atlas_sc, &rel);

			static int s_atlas_log = 0;
			if (ef_npatch > 0 && s_atlas_log++ < 4) {
				displayxr_log(
				    "[DisplayXR] xrEndFrame: atlas composite OK "
				    "(n=%d tiles=%ux%u atlas=%ux%u viewPx=%ux%u)\n",
				    ef_npatch, tile_cols, tile_rows, atlas_w, atlas_h,
				    view_w, view_h);
			}
		} else {
			// Atlas creation failed — release any deferred typed shadows so we
			// don't leak acquired images, and fall through to end_frame with the
			// original (broken) view swapchains. The frame will render wrong
			// but we keep the session alive.
			XrSwapchainImageReleaseInfo rel = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
			for (int si = 0; si < s_sc_sub_count; si++) {
				if (s_sc_subs[si].active && s_sc_subs[si].release_pending) {
					s_real_release_swapchain_image(s_sc_subs[si].typed_sc, &rel);
					s_sc_subs[si].release_pending = false;
				}
			}
		}
	}
#endif

	// Count active window-space layers
	int active_layers = 0;
	for (int i = 0; i < DISPLAYXR_MAX_WINDOW_LAYERS; i++) {
		if (state->window_layers[i].active && state->window_layers[i].swapchain != XR_NULL_HANDLE) {
			active_layers++;
		}
	}

	XrResult ef_result;
	if (active_layers == 0) {
		// No overlay layers — pass through
		ef_result = s_real_end_frame(session, frameEndInfo);
	} else {

	// Build extended layer array: original layers + window-space layers
	uint32_t total = frameEndInfo->layerCount + (uint32_t)active_layers;
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
		ws_layers[i].layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
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

static XrResult XRAPI_CALL
hooked_xrCreateSwapchain(XrSession session,
                          const XrSwapchainCreateInfo *createInfo,
                          XrSwapchain *swapchain)
{
	displayxr_log( "[DisplayXR] xrCreateSwapchain: format=%lld size=%ux%u "
	        "samples=%u faces=%u arrays=%u mips=%u "
	        "createFlags=0x%llx usageFlags=0x%llx\n",
	        (long long)createInfo->format,
	        createInfo->width, createInfo->height,
	        createInfo->sampleCount, createInfo->faceCount,
	        createInfo->arraySize, createInfo->mipCount,
	        (unsigned long long)createInfo->createFlags,
	        (unsigned long long)createInfo->usageFlags);

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

	XrResult result = s_real_create_swapchain(session, createInfo, swapchain);
	if (XR_SUCCEEDED(result)) {
		displayxr_log( "[DisplayXR] xrCreateSwapchain: OK swapchain=%p\n",
		        (void *)(uintptr_t)*swapchain);
#if defined(_WIN32)
		// D3D11: create a parallel R8G8B8A8_UNORM_SRGB swapchain so the compositor
		// receives typed textures in xrEndFrame (TYPELESS → compositor X-pattern).
		if (s_d3d11_device != nullptr &&
		    (createInfo->usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) &&
		    s_sc_sub_count < DISPLAYXR_MAX_SC_SUBS) {
			XrSwapchainCreateInfo typed_info = *createInfo;
			typed_info.format = 29; // DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
			XrSwapchain typed_sc = XR_NULL_HANDLE;
			XrResult tr = s_real_create_swapchain(session, &typed_info, &typed_sc);
			if (XR_SUCCEEDED(tr)) {
				D3D11ScSub &sub = s_sc_subs[s_sc_sub_count++];
				sub.unity_sc = *swapchain;
				sub.typed_sc = typed_sc;
				sub.width    = createInfo->width;
				sub.height   = createInfo->height;
				sub.active   = true;
				displayxr_log("[DisplayXR] Typed swapchain paired: unity=%p typed=%p\n",
				              (void *)(uintptr_t)*swapchain, (void *)(uintptr_t)typed_sc);
			} else {
				displayxr_log("[DisplayXR] Typed swapchain create FAILED: result=%d\n", tr);
			}
		}
#endif
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
#if defined(_WIN32)
	// D3D11 typed swapchain substitution: route to typed_sc so Unity gets
	// R8G8B8A8_UNORM_SRGB textures it can create valid RTVs from.
	D3D11ScSub *sub = d3d11_sub_find(swapchain);
	if (sub != nullptr) {
		XrResult result = s_real_enumerate_swapchain_images(
		    sub->typed_sc, imageCapacityInput, imageCountOutput, images);
		if (XR_SUCCEEDED(result) && images != nullptr && imageCountOutput != nullptr) {
			displayxr_log("[DisplayXR] xrEnumerateSwapchainImages: unity_sc=%p → typed_sc=%p count=%u\n",
			              (void *)(uintptr_t)swapchain,
			              (void *)(uintptr_t)sub->typed_sc,
			              *imageCountOutput);
			if (images->type == XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR) {
				XrSwapchainImageD3D11KHR *d3d = (XrSwapchainImageD3D11KHR *)images;
				uint32_t n = *imageCountOutput < 8 ? *imageCountOutput : 8;
				sub->typed_img_count = n;
				for (uint32_t i = 0; i < n; i++) {
					sub->typed_textures[i] = d3d[i].texture;
					if (d3d[i].texture) {
						D3D11_TEXTURE2D_DESC desc = {};
						d3d[i].texture->GetDesc(&desc);
						displayxr_log("  typed[%u] tex=%p fmt=%u\n", i, (void *)d3d[i].texture, desc.Format);
					}
				}
			}
		}
		return result;
	}
#endif

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
#if defined(_WIN32)
	// D3D11: acquire from typed_sc (Unity renders into it) and unity_sc (keep state sane).
	D3D11ScSub *sub = d3d11_sub_find(swapchain);
	if (sub != nullptr) {
		XrResult result = s_real_acquire_swapchain_image(sub->typed_sc, acquireInfo, index);
		if (XR_SUCCEEDED(result) && index)
			sub->current_idx = *index;
		uint32_t dummy = 0;
		s_real_acquire_swapchain_image(swapchain, acquireInfo, &dummy);
		static int s_acq_count = 0;
		if (s_acq_count < 6 || s_acq_count % 120 == 0)
			displayxr_log("[DisplayXR] xrAcquireSwapchainImage: unity=%p typed=%p typed_idx=%u\n",
			              (void *)(uintptr_t)swapchain, (void *)(uintptr_t)sub->typed_sc,
			              index ? *index : 0xFFFFFFFF);
		s_acq_count++;
		return result;
	}
#endif
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
#if defined(_WIN32)
	D3D11ScSub *sub = d3d11_sub_find(swapchain);
	if (sub != nullptr) {
		s_real_wait_swapchain_image(sub->typed_sc, waitInfo);
		return s_real_wait_swapchain_image(swapchain, waitInfo);
	}
#endif
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
#if defined(_WIN32)
	D3D11ScSub *sub = d3d11_sub_find(swapchain);
	if (sub != nullptr) {
		// Flush so Unity's render commands reach the GPU before we composite.
		if (s_d3d11_context != nullptr)
			s_d3d11_context->Flush();
		// Defer typed_sc release: we still need to write into it (SBS composite)
		// inside hooked_xrEndFrame before handing it to the compositor.
		sub->release_pending = true;
		// Release unity_sc now (we never rendered into it, just keeping state sane).
		return s_real_release_swapchain_image(swapchain, releaseInfo);
	}
	// Non-substituted swapchain: original flush-and-release behavior.
	if (s_d3d11_context != nullptr)
		s_d3d11_context->Flush();
#endif

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
	d3d11_sub_cleanup_all();
	if (s_d3d11_context) { s_d3d11_context->Release(); s_d3d11_context = nullptr; }
	s_d3d11_device = nullptr; // Not owned by us — don't Release
#endif

	displayxr_log( "[DisplayXR] xrDestroyInstance END (deferred, returning XR_SUCCESS)\n");
	s_instance = XR_NULL_HANDLE;
	s_session = XR_NULL_HANDLE;
	s_local_space = XR_NULL_HANDLE;
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
		// Don't intercept — just cache the function pointer
	} else if (strcmp(name, "xrPollEvent") == 0) {
		s_real_poll_event = (PFN_xrPollEvent)*function;
		*function = (PFN_xrVoidFunction)hooked_xrPollEvent;
	} else if (strcmp(name, "xrDestroyInstance") == 0) {
		s_real_destroy_instance = (PFN_xrDestroyInstance)*function;
		*function = (PFN_xrVoidFunction)hooked_xrDestroyInstance;
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
	else if (strcmp(name, "xrRequestDisplayRenderingModeEXT") == 0) {
		s_real_request_rendering_mode = (PFN_xrRequestDisplayRenderingModeEXT)*function;
		*function = (PFN_xrVoidFunction)hooked_xrRequestDisplayRenderingModeEXT;
	}
	else if (strcmp(name, "xrEnumerateDisplayRenderingModesEXT") == 0) {
		s_real_enumerate_rendering_modes = (PFN_xrEnumerateDisplayRenderingModesEXT)*function;
		// No hook — capture pointer so xrCreateSession's own enumeration call can
		// reuse it even when the app (e.g. Unity built player) doesn't query it.
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
	s_real_poll_event = nullptr;
	s_real_destroy_instance = nullptr;
	s_instance = XR_NULL_HANDLE;
	s_session = XR_NULL_HANDLE;
	s_local_space = XR_NULL_HANDLE;
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
                      int camera_centric)
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
	if (s_pfn_set_output_rect && s_session != XR_NULL_HANDLE)
		s_pfn_set_output_rect(s_session, x, y, w, h);
}
