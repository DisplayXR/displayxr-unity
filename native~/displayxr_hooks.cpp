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

// --- D3D11 TYPELESS workaround attempts (issue #36) ---
// Runtime creates TYPELESS textures per OpenXR D3D11 spec. Unity has geometry
// corruption with TYPELESS but crashes with ANY texture pointer substitution
// (proxy textures crash, COM wrappers crash). Both approaches cause GPU TDR
// because D3D11 runtime rejects non-original objects in rendering calls.
// Leaving textures as TYPELESS (corruption but no crash) and exploring
// synchronization-based fixes instead.
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
	}

	return result;
}

static XrResult XRAPI_CALL
hooked_xrDestroySession(XrSession session)
{
	displayxr_log( "[DisplayXR] xrDestroySession BEGIN session=%p (DEFERRED)\n", (void *)(uintptr_t)session);
	s_session_alive = 0;
	s_local_space = XR_NULL_HANDLE;

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

	// Count active window-space layers
	int active_layers = 0;
	for (int i = 0; i < DISPLAYXR_MAX_WINDOW_LAYERS; i++) {
		if (state->window_layers[i].active && state->window_layers[i].swapchain != XR_NULL_HANDLE) {
			active_layers++;
		}
	}

	if (active_layers == 0) {
		// No overlay layers — pass through
		return s_real_end_frame(session, frameEndInfo);
	}

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

	XrResult result = s_real_end_frame(session, &modified);
	delete[] layers;
	return result;
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
	XrResult result = s_real_enumerate_swapchain_images(swapchain, imageCapacityInput,
	                                                    imageCountOutput, images);
	if (XR_SUCCEEDED(result) && images != nullptr && imageCountOutput != nullptr) {
		displayxr_log( "[DisplayXR] xrEnumerateSwapchainImages: sc=%p count=%u type=%u\n",
		        (void *)(uintptr_t)swapchain, *imageCountOutput, (unsigned)images->type);

#if defined(_WIN32)
		// D3D11: stride is sizeof(XrSwapchainImageD3D11KHR), query texture descriptors
		if (images->type == XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR) {
			XrSwapchainImageD3D11KHR *d3d_images = (XrSwapchainImageD3D11KHR *)images;
			for (uint32_t i = 0; i < *imageCountOutput; i++) {
				ID3D11Texture2D *tex = d3d_images[i].texture;
				displayxr_log( "  image[%u] texture=%p", i, (void *)tex);
				if (tex != nullptr) {
					D3D11_TEXTURE2D_DESC desc = {};
					tex->GetDesc(&desc);
					displayxr_log( "\n    D3D11: %ux%u fmt=%u bindFlags=0x%x "
					        "usage=%u miscFlags=0x%x cpuAccess=0x%x "
					        "mips=%u arrays=%u samples=%u",
					        desc.Width, desc.Height,
					        (unsigned)desc.Format,
					        (unsigned)desc.BindFlags,
					        (unsigned)desc.Usage,
					        (unsigned)desc.MiscFlags,
					        (unsigned)desc.CPUAccessFlags,
					        desc.MipLevels, desc.ArraySize,
					        desc.SampleDesc.Count);
					// Annotate bind flags
					if (desc.BindFlags & D3D11_BIND_RENDER_TARGET)
						displayxr_log( " [RT]");
					if (desc.BindFlags & D3D11_BIND_SHADER_RESOURCE)
						displayxr_log( " [SRV]");
					if (desc.BindFlags & D3D11_BIND_UNORDERED_ACCESS)
						displayxr_log( " [UAV]");
					// Annotate misc flags
					if (desc.MiscFlags & D3D11_RESOURCE_MISC_SHARED)
						displayxr_log( " [SHARED]");
					if (desc.MiscFlags & D3D11_RESOURCE_MISC_SHARED_NTHANDLE)
						displayxr_log( " [SHARED_NTHANDLE]");
					if (desc.MiscFlags & D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX)
						displayxr_log( " [KEYED_MUTEX]");
				}
				displayxr_log( "\n");
			}
		} else
#endif
		{
			// Generic fallback: just log type for each image
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
	// D3D11 GPU sync: flush all pending commands before the compositor reads.
	// Unity's render thread may not have finished submitting draw commands when
	// the main thread calls xrReleaseSwapchainImage. Without this, the compositor
	// reads a partially-rendered texture → missing faces, distorted geometry.
	// RenderDoc serializes execution and masks this issue.
	if (s_d3d11_context != nullptr) {
		s_d3d11_context->Flush();
	}
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
	} else if (strcmp(name, "xrCreateSwapchain") == 0) {
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
