// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
//
// Runtime-facing OpenXR session for the custom Unity IUnityXRDisplay provider
// (epic #166, M1). See displayxr_provider_session.h for the design rationale.
//
// Lifts the proven runtime-load / instance / session / locate-views logic from
// displayxr_standalone.cpp, but binds the session to UNITY'S OWN D3D12 device
// (zero-copy) and creates an arraySize=2 (SPI) swapchain. Windows / D3D12 only.

#include "displayxr_provider_session.h"

#include <openxr/openxr.h>
#include "../displayxr_extensions.h"
#include "../displayxr_shared_state.h" // displayxr_state_set_stereo_matrices (overlay hit-test)

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

// ============================================================================
// OpenXR loader negotiation types (inlined to avoid a loader-header dependency;
// identical to displayxr_standalone.cpp).
// ============================================================================

#define XR_LOADER_INTERFACE_STRUCT_LOADER_INFO     1
#define XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST 3
#define XR_CURRENT_LOADER_RUNTIME_VERSION          1

typedef struct XrNegotiateLoaderInfo {
	uint32_t structType;
	uint32_t structVersion;
	size_t   structSize;
	uint32_t minInterfaceVersion;
	uint32_t maxInterfaceVersion;
	XrVersion minApiVersion;
	XrVersion maxApiVersion;
} XrNegotiateLoaderInfo;

typedef struct XrNegotiateRuntimeRequest {
	uint32_t structType;
	uint32_t structVersion;
	size_t   structSize;
	uint32_t runtimeInterfaceVersion;
	XrVersion runtimeApiVersion;
	PFN_xrGetInstanceProcAddr getInstanceProcAddr;
} XrNegotiateRuntimeRequest;

typedef XrResult (*PFN_xrNegotiateLoaderRuntimeInterface)(
    const XrNegotiateLoaderInfo *loaderInfo,
    XrNegotiateRuntimeRequest *runtimeRequest);

// ============================================================================
// XR_KHR_D3D12_enable types (inlined — avoids requiring XR_USE_GRAPHICS_API_D3D12).
// ============================================================================

#ifndef XR_TYPE_GRAPHICS_BINDING_D3D12_KHR
#define XR_TYPE_GRAPHICS_BINDING_D3D12_KHR      ((XrStructureType)1000028000)
#endif
#ifndef XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR
#define XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR       ((XrStructureType)1000028001)
#endif
#ifndef XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR
#define XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR ((XrStructureType)1000028002)
#endif

typedef struct XrGraphicsBindingD3D12KHR {
	XrStructureType type;
	const void *next;
	ID3D12Device *device;
	ID3D12CommandQueue *queue;
} XrGraphicsBindingD3D12KHR;

typedef struct XrSwapchainImageD3D12KHR {
	XrStructureType type;
	void *next;
	ID3D12Resource *texture;
} XrSwapchainImageD3D12KHR;

typedef struct XrGraphicsRequirementsD3D12KHR {
	XrStructureType type;
	void *next;
	LUID adapterLuid;
	D3D_FEATURE_LEVEL minFeatureLevel;
} XrGraphicsRequirementsD3D12KHR;

typedef XrResult (XRAPI_PTR *PFN_xrGetD3D12GraphicsRequirementsKHR)(
    XrInstance instance, XrSystemId systemId,
    XrGraphicsRequirementsD3D12KHR *graphicsRequirements);

// ============================================================================
// Size constants
// ============================================================================

#define PS_MAX_SWAPCHAIN_IMAGES 4
#define PS_MAX_RENDERING_MODES  16
#define PS_MAX_VIEWS            8   // runtime may report max-mode count; we submit 2
#define PS_MAX_ZONES           4   // Unity kUnityXRMaxNumRenderPasses (SPI: 1 pass/zone)

// Additional 3D zone beyond the PRIMARY zone (#166 Phase B2). The primary zone
// (index 0) uses the top-level single-zone fields (swapchain/bridge/views/zone_*),
// which stay the validated Phase-B path. Each extra zone carries its OWN zone-sized
// swapchain + cross-device bridge(s) + located views + Unity render pass, so N 3D
// zones weave into N window-pixel rects (mirrors the runtime cube_zones handle test).
typedef struct ProviderExtraZone {
	int      valid;
	uint32_t zone_id;
	int32_t  rect_x, rect_y, rect_w, rect_h;
	uint32_t rec_w, rec_h;                      // recommended view size (== swapchain size)

	XrSwapchain swapchain;
	int         swapchain_created;
	uint32_t    sc_width, sc_height, sc_image_count;
	int64_t     sc_format;
	XrSwapchainImageD3D12KHR sc_images[PS_MAX_SWAPCHAIN_IMAGES];
	int         image_acquired;
	uint32_t    acquired_index;

	ID3D12Resource *bridge_own, *bridge_unity; HANDLE bridge_handle;              // SPI (2-slice)
	ID3D12Resource *bridge_own_eye[2], *bridge_unity_eye[2]; HANDLE bridge_handle_eye[2]; // MultiPass

	// Live tile realloc (#172): recreate this zone's swapchain+bridge when its rect
	// (recommended view size) changes. Debounced like the primary; needs_rewrap is a
	// latch consumed by the Unity side so it drops+rewraps this zone's texture(s).
	uint32_t    realloc_pending_w, realloc_pending_h;
	int         realloc_stable;
	int         needs_rewrap;

	DxrProvView views[2];
	uint32_t    view_count;
} ProviderExtraZone;

// ============================================================================
// Session state
// ============================================================================

typedef struct ProviderSession {
	void *runtime_lib;
	PFN_xrGetInstanceProcAddr gipa;

	XrInstance instance;
	XrSystemId system_id;
	XrSession  session;
	XrSpace    local_space;

	XrSessionState session_state;
	int  running;
	int  session_ready;
	int  frame_begun;
	int  image_acquired;     // 1 between xrAcquire/Wait and xrRelease (guards double-acquire)
	uint32_t acquired_index;
	XrTime predicted_display_time;

	int has_view_rig;

	// XR_EXT_display_zones + XR_EXT_local_3d_zone (#166 Phase B). Detected in
	// session_start (probe) and enabled on the instance. Caps queried lazily on the
	// first frame a zone rect is set (needs a live session). zone_caps_ok: -1 untried,
	// 0 unsupported, 1 supported (maxZones3D>=1).
	int has_display_zones;
	int has_local_3d_zone;
	int zone_caps_ok;
	uint32_t zone_max_3d;
	PFN_xrGetDisplayZoneCapabilitiesEXT       pfn_get_zone_caps;
	PFN_xrGetDisplayZoneRecommendedViewSizeEXT pfn_get_zone_view_size;

	// App-supplied single 3D-zone rect (client-window px, top-left origin). When
	// valid + caps OK, the locate hook chains XrDisplayZoneEXT before the rig
	// (zone-scoped Kooima) and submit chains the same zone on the projection layer,
	// and the swapchain is sized to the zone's recommended view size. w<=0||h<=0
	// clears (full-window framing = the Phase A path). (Single-zone first;
	// generalized to N in a follow-up.)
	int      zone_valid;
	uint32_t zone_id;
	int32_t  zone_x, zone_y, zone_w, zone_h;
	uint32_t zone_rec_w, zone_rec_h; // recommended view size for zone_[wh]

	// Additional 3D zones beyond the primary (#166 Phase B2). extra_zone_count is the
	// number of ACTIVE entries; total 3D zones = 1 (primary) + extra_zone_count.
	// Capped so total zones <= PS_MAX_ZONES (Unity render-pass limit).
	ProviderExtraZone extra_zones[PS_MAX_ZONES - 1];
	uint32_t          extra_zone_count;

	// Unity's D3D12 device — used ONLY to open the shared bridge (NOT to bind the
	// session; see own_device below).
	ID3D12Device       *unity_device;
	void               *overlay_hwnd;

	// The session's OWN D3D12 device (matched to the runtime adapter LUID). The
	// runtime allocates its swapchain on THIS device. (Binding to Unity's device
	// crashed with device-removed — cross-device raw pointers are invalid.)
	ID3D12Device              *own_device;
	ID3D12CommandQueue        *own_queue;
	ID3D12CommandAllocator    *own_cmd_alloc;
	ID3D12GraphicsCommandList *own_cmd_list;
	ID3D12Fence               *own_fence;
	HANDLE                     own_fence_event;
	UINT64                     own_fence_value;

	// Cross-device 2-slice-array bridge: SHARED on own_device, opened on Unity's
	// device. Unity renders both eyes into it; we copy it into the acquired
	// runtime swapchain image (both slices) each frame.
	ID3D12Resource            *bridge_own;     // own_device side (copy source)
	ID3D12Resource            *bridge_unity;   // Unity-device side (Unity renders here)
	HANDLE                     bridge_handle;

	// MultiPass (BiRP) bridges. The IUnityXRDisplay contract is "one texture per
	// render pass", and textureArraySlice is "Only valid if single-pass rendering
	// mode is active" (IUnityXRDisplay.h) — so a 2-pass MultiPass eye CANNOT target
	// a slice of a shared array (that was the failed black+white-blocks attempt).
	// Instead each eye gets its OWN single-slice bridge: Unity renders eye 0/1 into
	// bridge_unity_eye[0/1]; submit copies bridge_own_eye[0/1] into swapchain slices
	// 0/1 (the runtime still gets its 2-slice array swapchain). Unused in SPI mode.
	ID3D12Resource            *bridge_own_eye[2];
	ID3D12Resource            *bridge_unity_eye[2];
	HANDLE                     bridge_handle_eye[2];

	// Render mode: 1 = Single-Pass-Instanced (1 pass × 2 over the 2-slice array
	// bridge); 0 = MultiPass (2 pass × 1 over the per-eye bridges). Set from C#
	// (dxr_prov_set_single_pass) BEFORE the session starts — URP+Win+D3D12 → SPI,
	// BiRP/other → MultiPass. single_pass_set distinguishes "C# chose" from the
	// zero-initialized default; readers fall back to SPI (current behavior) when
	// unset. Preserved across the session_start memset (like runtime_lib).
	int single_pass;
	int single_pass_set;

	// Transparent background (#166 Phase A): when the app requests it AND the
	// runtime advertises ALPHA_BLEND, the win32 binding sets
	// transparentBackgroundEnabled and xrEndFrame submits with ALPHA_BLEND (the
	// DComp overlay is already WS_EX_NOREDIRECTIONBITMAP). Set from C#
	// (dxr_prov_set_transparent_background) BEFORE the session starts; preserved
	// across the session_start memset like single_pass. alpha_blend_supported is
	// probed per-session in session_start (reset OK).
	int transparent_requested;
	int alpha_blend_supported;

	// Unity's command queue + a SHARED cross-device fence so the own-device
	// bridge→swapchain copy waits for Unity's render into the bridge to FINISH.
	// Without it the copy races ahead of Unity's writes and reads empty texels
	// (black). Unity's queue signals shared_fence_unity after its render; the own
	// queue GPU-waits on shared_fence_own (same shared fence) before copying.
	ID3D12CommandQueue        *unity_queue;
	ID3D12Fence               *shared_fence_own;
	ID3D12Fence               *shared_fence_unity;
	HANDLE                     shared_fence_handle;
	UINT64                     sync_val;

	DxrProvDisplayInfo display_info;

	// Rendering modes
	XrDisplayRenderingModeInfoEXT modes[PS_MAX_RENDERING_MODES];
	uint32_t mode_count;

	// SPI swapchain (arraySize=2)
	XrSwapchain swapchain;
	uint32_t    sc_width, sc_height, sc_array, sc_image_count;
	int64_t     sc_format;
	XrSwapchainImageD3D12KHR sc_images[PS_MAX_SWAPCHAIN_IMAGES];
	int         swapchain_created;

	// Live tile realloc (#172): the primary swapchain+bridge are recreated to track
	// the live per-view target (window(client)×scaleXY, or the 3D-zone recommended
	// view size) when it changes — resize / split-drag / mode-switch. Debounced so an
	// interactive resize drag reallocs once on settle, not every frame.
	uint32_t    realloc_pending_w, realloc_pending_h; // last-seen target awaiting stability
	int         realloc_stable;                        // consecutive frames the target held

	// Window-space UI (HUD) overlay layer (#67/#166): own overlay swapchain +
	// cross-device bridge. C# (DisplayXRWindowSpaceUI) Graphics.CopyTexture's the
	// canvas RT into wsui_bridge_unity each frame; submit copies wsui_bridge_own ->
	// the acquired swapchain image (own device) and submits a 2nd composition layer.
	XrSwapchain wsui_swapchain;
	uint32_t    wsui_w, wsui_h, wsui_image_count;
	int64_t     wsui_format;
	XrSwapchainImageD3D12KHR wsui_images[PS_MAX_SWAPCHAIN_IMAGES];
	int         wsui_swapchain_created;
	int         wsui_image_acquired;
	uint32_t    wsui_acquired_index;
	ID3D12Resource *wsui_bridge_own;    // own_device side (copy source)
	ID3D12Resource *wsui_bridge_unity;  // Unity-device side (C# CopyTexture target)
	HANDLE          wsui_bridge_handle;
	uint32_t        wsui_registered_w, wsui_registered_h; // bridge+swapchain sized for this

	// Local2D layer (#166 Phase B, XR_EXT_local_3d_zone) — post-weave 2D content at a
	// client-window PIXEL rect (the 2D band). Same cross-device-bridge shape as wsui,
	// but submitted as XrCompositionLayerLocal2DEXT with a pixel rect (not fractional).
	XrSwapchain l2d_swapchain;
	uint32_t    l2d_w, l2d_h, l2d_image_count;
	int64_t     l2d_format;
	XrSwapchainImageD3D12KHR l2d_images[PS_MAX_SWAPCHAIN_IMAGES];
	int         l2d_swapchain_created;
	ID3D12Resource *l2d_bridge_own;
	ID3D12Resource *l2d_bridge_unity;
	HANDLE          l2d_bridge_handle;
	uint32_t        l2d_registered_w, l2d_registered_h;
	int32_t         l2d_rect_x, l2d_rect_y, l2d_rect_w, l2d_rect_h; // dest, client px
	int             l2d_rect_set;

	// Per-frame located views (render-ready)
	DxrProvView views[DXR_PROV_MAX_VIEWS];
	uint32_t    view_count;

	// Tunables / pose pushed from the rig
	float ipd_factor, parallax_factor, perspective_factor, virtual_display_height;
	float inv_convergence_distance, fov_override, near_z, far_z;
	int   camera_centric;
	int   tunables_set;
	XrPosef display_pose;
	int     display_pose_set;

	// Active mode + per-frame render rect (Bucket B: window×scaleXY adaptive)
	uint32_t active_mode_index;
	uint32_t render_w, render_h;

	// Event latches (Bucket C: atomic read-and-clear, pumped by poll_events)
	int      ev_mode_changed;  uint32_t ev_mode_prev, ev_mode_cur;
	int      ev_hw_changed;     int ev_hw3d;
	int      ev_track_changed;  int ev_track_is_tracking, ev_track_mode;

	// Resolved function pointers
	PFN_xrGetSystem                   pfn_get_system;
	PFN_xrGetSystemProperties         pfn_get_system_properties;
	PFN_xrCreateSession               pfn_create_session;
	PFN_xrDestroySession              pfn_destroy_session;
	PFN_xrCreateReferenceSpace        pfn_create_reference_space;
	PFN_xrEnumerateSwapchainFormats   pfn_enumerate_swapchain_formats;
	PFN_xrCreateSwapchain             pfn_create_swapchain;
	PFN_xrDestroySwapchain            pfn_destroy_swapchain;
	PFN_xrEnumerateSwapchainImages    pfn_enumerate_swapchain_images;
	PFN_xrAcquireSwapchainImage       pfn_acquire_swapchain_image;
	PFN_xrWaitSwapchainImage          pfn_wait_swapchain_image;
	PFN_xrReleaseSwapchainImage       pfn_release_swapchain_image;
	PFN_xrWaitFrame                   pfn_wait_frame;
	PFN_xrBeginFrame                  pfn_begin_frame;
	PFN_xrEndFrame                    pfn_end_frame;
	PFN_xrLocateViews                 pfn_locate_views;
	PFN_xrPollEvent                   pfn_poll_event;
	PFN_xrBeginSession                pfn_begin_session;
	PFN_xrEndSession                  pfn_end_session;
	PFN_xrDestroyInstance             pfn_destroy_instance;
	PFN_xrEnumerateEnvironmentBlendModes    pfn_enumerate_blend_modes;    // transparency
	PFN_xrEnumerateDisplayRenderingModesEXT pfn_enumerate_modes;          // optional
	PFN_xrRequestDisplayRenderingModeEXT    pfn_request_rendering_mode;   // optional
	PFN_xrRequestDisplayModeEXT             pfn_request_display_mode;     // optional
	PFN_xrRequestEyeTrackingModeEXT         pfn_request_eye_tracking_mode;// optional
} ProviderSession;

static ProviderSession s_ps;
static DxrProvLogCallback s_log_cb = NULL;

void dxr_prov_set_log_callback(DxrProvLogCallback cb) { s_log_cb = cb; }

// Append a line to %TEMP%\displayxr_prov_native.log. Unity's Player.log does not
// capture a native plugin's stderr, so route provider diagnostics to a file we
// can read after a run (M1 bring-up aid). Shared by the provider TU via
// dxr_prov_file_log.
extern "C" void dxr_prov_file_log(const char *s)
{
	char path[MAX_PATH];
	DWORD n = GetTempPathA(MAX_PATH, path);
	if (n == 0 || n > MAX_PATH - 32) return;
	strncat(path, "displayxr_prov_native.log", MAX_PATH - strlen(path) - 1);
	FILE *f = fopen(path, "a");
	if (!f) return;
	fputs(s, f);
	fclose(f);
}

static void ps_log(const char *fmt, ...)
{
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	fputs(buf, stderr);
	OutputDebugStringA(buf);
	dxr_prov_file_log(buf);
	if (s_log_cb) s_log_cb(buf);
}

// ============================================================================
// Runtime load helpers (mirrors displayxr_standalone.cpp)
// ============================================================================

static char *ps_parse_library_path(const char *json_path)
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

static char *ps_resolve_library_path(const char *json_path, const char *lib_path)
{
	if (lib_path[0] == '/' || (lib_path[0] && lib_path[1] == ':'))
		return _strdup(lib_path);
	const char *last_sep = strrchr(json_path, '/');
	const char *last_bsep = strrchr(json_path, '\\');
	if (last_bsep && (!last_sep || last_bsep > last_sep)) last_sep = last_bsep;
	size_t dir_len = last_sep ? (size_t)(last_sep - json_path + 1) : 0;
	size_t lib_len = strlen(lib_path);
	char *result = (char *)malloc(dir_len + lib_len + 1);
	if (dir_len > 0) memcpy(result, json_path, dir_len);
	memcpy(result + dir_len, lib_path, lib_len);
	result[dir_len + lib_len] = '\0';
	return result;
}

// Resolve the active runtime manifest path: explicit arg → XR_RUNTIME_JSON.
// (Registry ActiveRuntime fallback is a future addition; XR_RUNTIME_JSON covers
// the dev / sim_display bring-up path.)
static char *ps_resolve_runtime_json(const char *explicit_path)
{
	if (explicit_path && explicit_path[0]) return _strdup(explicit_path);
	const char *env = getenv("XR_RUNTIME_JSON");
	if (env && env[0]) return _strdup(env);
	return NULL;
}

#define PS_RESOLVE(name, field, type) do { \
	PFN_xrVoidFunction _fn = NULL; \
	s_ps.gipa(s_ps.instance, name, &_fn); \
	s_ps.field = (type)_fn; \
	if (!s_ps.field) { ps_log("[DisplayXR-PROV] Failed to resolve %s\n", name); return 0; } \
} while (0)

static int ps_resolve_functions(void)
{
	PS_RESOLVE("xrGetSystem", pfn_get_system, PFN_xrGetSystem);
	PS_RESOLVE("xrGetSystemProperties", pfn_get_system_properties, PFN_xrGetSystemProperties);
	PS_RESOLVE("xrCreateSession", pfn_create_session, PFN_xrCreateSession);
	PS_RESOLVE("xrDestroySession", pfn_destroy_session, PFN_xrDestroySession);
	PS_RESOLVE("xrCreateReferenceSpace", pfn_create_reference_space, PFN_xrCreateReferenceSpace);
	PS_RESOLVE("xrEnumerateSwapchainFormats", pfn_enumerate_swapchain_formats, PFN_xrEnumerateSwapchainFormats);
	PS_RESOLVE("xrCreateSwapchain", pfn_create_swapchain, PFN_xrCreateSwapchain);
	PS_RESOLVE("xrDestroySwapchain", pfn_destroy_swapchain, PFN_xrDestroySwapchain);
	PS_RESOLVE("xrEnumerateSwapchainImages", pfn_enumerate_swapchain_images, PFN_xrEnumerateSwapchainImages);
	PS_RESOLVE("xrAcquireSwapchainImage", pfn_acquire_swapchain_image, PFN_xrAcquireSwapchainImage);
	PS_RESOLVE("xrWaitSwapchainImage", pfn_wait_swapchain_image, PFN_xrWaitSwapchainImage);
	PS_RESOLVE("xrReleaseSwapchainImage", pfn_release_swapchain_image, PFN_xrReleaseSwapchainImage);
	PS_RESOLVE("xrWaitFrame", pfn_wait_frame, PFN_xrWaitFrame);
	PS_RESOLVE("xrBeginFrame", pfn_begin_frame, PFN_xrBeginFrame);
	PS_RESOLVE("xrEndFrame", pfn_end_frame, PFN_xrEndFrame);
	PS_RESOLVE("xrLocateViews", pfn_locate_views, PFN_xrLocateViews);
	PS_RESOLVE("xrPollEvent", pfn_poll_event, PFN_xrPollEvent);
	PS_RESOLVE("xrBeginSession", pfn_begin_session, PFN_xrBeginSession);
	PS_RESOLVE("xrEndSession", pfn_end_session, PFN_xrEndSession);
	PS_RESOLVE("xrDestroyInstance", pfn_destroy_instance, PFN_xrDestroyInstance);
	PS_RESOLVE("xrEnumerateEnvironmentBlendModes", pfn_enumerate_blend_modes, PFN_xrEnumerateEnvironmentBlendModes);
	// Optional EXT (XR_EXT_display_info mode/eye-tracking control) — soft-resolve;
	// OK if absent (older runtime → the C# mode UI/events simply stay inert).
	{
		PFN_xrVoidFunction _fn = NULL;
		s_ps.gipa(s_ps.instance, "xrEnumerateDisplayRenderingModesEXT", &_fn);
		s_ps.pfn_enumerate_modes = (PFN_xrEnumerateDisplayRenderingModesEXT)_fn;
		_fn = NULL;
		s_ps.gipa(s_ps.instance, "xrRequestDisplayRenderingModeEXT", &_fn);
		s_ps.pfn_request_rendering_mode = (PFN_xrRequestDisplayRenderingModeEXT)_fn;
		_fn = NULL;
		s_ps.gipa(s_ps.instance, "xrRequestDisplayModeEXT", &_fn);
		s_ps.pfn_request_display_mode = (PFN_xrRequestDisplayModeEXT)_fn;
		_fn = NULL;
		s_ps.gipa(s_ps.instance, "xrRequestEyeTrackingModeEXT", &_fn);
		s_ps.pfn_request_eye_tracking_mode = (PFN_xrRequestEyeTrackingModeEXT)_fn;
		// Zones (#166 Phase B) — soft-resolve; inert on older runtimes.
		_fn = NULL;
		s_ps.gipa(s_ps.instance, "xrGetDisplayZoneCapabilitiesEXT", &_fn);
		s_ps.pfn_get_zone_caps = (PFN_xrGetDisplayZoneCapabilitiesEXT)_fn;
		_fn = NULL;
		s_ps.gipa(s_ps.instance, "xrGetDisplayZoneRecommendedViewSizeEXT", &_fn);
		s_ps.pfn_get_zone_view_size = (PFN_xrGetDisplayZoneRecommendedViewSizeEXT)_fn;
	}
	return 1;
}

// ============================================================================
// Rendering-mode enumeration (lifts standalone enumerate_and_store_modes)
// ============================================================================

static void ps_enumerate_modes(void)
{
	s_ps.mode_count = 0;
	if (!s_ps.pfn_enumerate_modes) return;
	uint32_t total = 0;
	if (XR_FAILED(s_ps.pfn_enumerate_modes(s_ps.session, 0, &total, NULL)) || total == 0)
		return;
	if (total > PS_MAX_RENDERING_MODES) total = PS_MAX_RENDERING_MODES;
	for (uint32_t i = 0; i < total; i++)
		s_ps.modes[i].type = XR_TYPE_DISPLAY_RENDERING_MODE_INFO_EXT;
	if (XR_SUCCEEDED(s_ps.pfn_enumerate_modes(s_ps.session, total, &total, s_ps.modes)))
		s_ps.mode_count = total;
	for (uint32_t i = 0; i < s_ps.mode_count; i++) {
		if (s_ps.modes[i].isActive) s_ps.active_mode_index = s_ps.modes[i].modeIndex;
		ps_log("[DisplayXR-PROV] mode[%u] idx=%u '%s' views=%u tiles=%ux%u %ux%u "
		       "scale=%.3fx%.3f hw3d=%d active=%d req=%d\n",
		       i, s_ps.modes[i].modeIndex, s_ps.modes[i].modeName,
		       s_ps.modes[i].viewCount, s_ps.modes[i].tileColumns, s_ps.modes[i].tileRows,
		       s_ps.modes[i].viewWidthPixels, s_ps.modes[i].viewHeightPixels,
		       (double)s_ps.modes[i].viewScaleX, (double)s_ps.modes[i].viewScaleY,
		       (int)s_ps.modes[i].hardwareDisplay3D,
		       (int)s_ps.modes[i].isActive, (int)s_ps.modes[i].isRequestable);
	}
}

// Find an enumerated mode by modeIndex (NULL if absent).
static const XrDisplayRenderingModeInfoEXT *ps_find_mode(uint32_t mode_index)
{
	for (uint32_t i = 0; i < s_ps.mode_count; i++)
		if (s_ps.modes[i].modeIndex == mode_index) return &s_ps.modes[i];
	return NULL;
}

// The active 3D (hardwareDisplay3D, viewCount==2) mode's per-view scaleXY, used
// to size the per-frame render rect = window × scaleXY. Falls back to the
// XR_EXT_display_info recommended scale, then to 0.5×1.0 (half-width SBS).
static void ps_active_view_scale(float *sx, float *sy)
{
	const XrDisplayRenderingModeInfoEXT *m = ps_find_mode(s_ps.active_mode_index);
	if ((!m || !m->hardwareDisplay3D) ) {
		// No active 3D mode resolved — prefer the first 3D stereo mode.
		for (uint32_t i = 0; i < s_ps.mode_count; i++)
			if (s_ps.modes[i].hardwareDisplay3D && s_ps.modes[i].viewCount == 2)
				{ m = &s_ps.modes[i]; break; }
	}
	float x = (m && m->viewScaleX > 0.0f) ? m->viewScaleX
	          : (s_ps.display_info.is_valid && s_ps.display_info.scale_x > 0.0f
	             ? s_ps.display_info.scale_x : 0.5f);
	float y = (m && m->viewScaleY > 0.0f) ? m->viewScaleY
	          : (s_ps.display_info.is_valid && s_ps.display_info.scale_y > 0.0f
	             ? s_ps.display_info.scale_y : 1.0f);
	*sx = x; *sy = y;
}

// The bound overlay's live client size (= Unity's window client area). The
// runtime weaves into this WS_CHILD overlay; per-view render res tracks it.
// Falls back to the display pixel dims when no overlay is bound yet.
static void ps_window_size(uint32_t *w, uint32_t *h)
{
	uint32_t ww = 0, hh = 0;
	if (s_ps.overlay_hwnd) {
		RECT rc;
		if (GetClientRect((HWND)s_ps.overlay_hwnd, &rc)) {
			ww = (uint32_t)(rc.right - rc.left);
			hh = (uint32_t)(rc.bottom - rc.top);
		}
	}
	if (ww == 0 || hh == 0) {
		ww = s_ps.display_info.is_valid ? s_ps.display_info.pixel_width : 1920;
		hh = s_ps.display_info.is_valid ? s_ps.display_info.pixel_height : 1080;
	}
	*w = ww; *h = hh;
}

// ============================================================================
// Zones (XR_EXT_display_zones) — #166 Phase B
// ============================================================================

// Lazy caps query: zones are usable iff the runtime advertised the extension,
// resolved the caps entry point, and reports supported && maxZones3D>=1. Mirrors
// the hook path's dxr_zones_ready.
static int ps_zones_ready(void)
{
	if (!s_ps.has_display_zones || !s_ps.has_view_rig || !s_ps.pfn_get_zone_caps)
		return 0;
	if (s_ps.zone_caps_ok < 0) {
		if (s_ps.session == XR_NULL_HANDLE) return 0;
		XrDisplayZoneCapabilitiesEXT caps = {XR_TYPE_DISPLAY_ZONE_CAPABILITIES_EXT};
		XrResult cr = s_ps.pfn_get_zone_caps(s_ps.session, &caps);
		s_ps.zone_caps_ok = (XR_SUCCEEDED(cr) && caps.supported && caps.maxZones3D >= 1) ? 1 : 0;
		s_ps.zone_max_3d = caps.maxZones3D;
		ps_log("[DisplayXR-PROV] display-zones caps: result=0x%x supported=%d maxZones3D=%u -> %s\n",
		       (unsigned)cr, (int)caps.supported, caps.maxZones3D,
		       s_ps.zone_caps_ok ? "ACTIVE" : "unsupported (full-window path)");
	}
	return s_ps.zone_caps_ok > 0;
}

// Recommended per-view render size for the current zone rect (== zone extent per
// the runtime). Falls back to the rect extent if the entry point is absent.
static void ps_query_zone_rec_size(void)
{
	s_ps.zone_rec_w = s_ps.zone_w > 0 ? (uint32_t)s_ps.zone_w : 0;
	s_ps.zone_rec_h = s_ps.zone_h > 0 ? (uint32_t)s_ps.zone_h : 0;
	if (s_ps.pfn_get_zone_view_size && s_ps.zone_valid && s_ps.session != XR_NULL_HANDLE) {
		XrRect2Di rect = {{s_ps.zone_x, s_ps.zone_y}, {s_ps.zone_w, s_ps.zone_h}};
		XrExtent2Di rec = {0, 0};
		if (XR_SUCCEEDED(s_ps.pfn_get_zone_view_size(s_ps.session, &rect, &rec)) &&
		    rec.width > 0 && rec.height > 0) {
			s_ps.zone_rec_w = (uint32_t)rec.width;
			s_ps.zone_rec_h = (uint32_t)rec.height;
		}
	}
}

// Whether the 3D zone should drive this frame (rect set + caps OK).
static int ps_zone_active(void) { return s_ps.zone_valid && ps_zones_ready(); }

// ============================================================================
// SPI swapchain (arraySize=2). Images live on Unity's D3D12 device (zero-copy).
// ============================================================================

static int ps_create_bridge(void); // defined after this function
static void ps_publish_stereo_matrices(void); // defined after dxr_prov_begin_frame (overlay hit-test)

static int ps_create_swapchain(void)
{
	if (s_ps.swapchain_created) return 1;
	if (!s_ps.display_info.is_valid) return 0;
	// Zone frames size the swapchain from the zone caps + recommended-view-size
	// queries, which need a READY (begun) session. Defer the early (pre-ready)
	// attempt so we don't create it window-sized before caps resolve; the
	// session-ready path recreates it correctly.
	if (s_ps.zone_valid && s_ps.has_display_zones && !s_ps.session_ready) return 0;

	// Per-view (per-eye) render resolution = window(overlay client) × active-mode
	// scaleXY. Each SPI array slice IS one eye, sized to exactly this — so the
	// render rect == the swapchain and Unity renders the FULL slice (viewportRect
	// {0,0,1,1}, imageRect = full). This avoids a sub-rect within a larger
	// worst-case texture, whose viewport (Unity bottom-left) vs imageRect (D3D
	// top-left) Y origins don't align → the runtime would sample empty texels
	// (black). The ADR-010 "worst-case, never realloc" optimization needs that
	// Y-origin reconciled and a live bridge/swapchain realloc on resize — deferred
	// as a follow-up; for now we size to the current window (startup-adaptive).
	uint32_t w, h;
	if (ps_zone_active()) {
		// 3D-zone frame: render each eye at the zone's recommended view size (== zone
		// extent), so the weave is zone-confined by construction (like the avatar /
		// cube_zones). imageRect == swapchain == zone size, no sub-rect.
		ps_query_zone_rec_size();
		w = s_ps.zone_rec_w; h = s_ps.zone_rec_h;
		ps_log("[DisplayXR-PROV] swapchain sized to 3D-zone recommended view %ux%u (zone %dx%d)\n",
		       w, h, s_ps.zone_w, s_ps.zone_h);
	} else {
		uint32_t ww = 0, wh = 0; ps_window_size(&ww, &wh);
		float sx = 0.5f, sy = 0.5f; ps_active_view_scale(&sx, &sy);
		w = (uint32_t)(ww * sx);
		h = (uint32_t)(wh * sy);
	}
	if (w == 0 || h == 0) { w = s_ps.display_info.pixel_width / 2; h = s_ps.display_info.pixel_height; }
	if (w == 0 || h == 0) { w = 1920; h = 1080; }

	uint32_t fmt_count = 0;
	s_ps.pfn_enumerate_swapchain_formats(s_ps.session, 0, &fmt_count, NULL);
	if (fmt_count == 0) { ps_log("[DisplayXR-PROV] No swapchain formats\n"); return 0; }
	int64_t formats[32];
	if (fmt_count > 32) fmt_count = 32;
	s_ps.pfn_enumerate_swapchain_formats(s_ps.session, fmt_count, &fmt_count, formats);
	int64_t format = formats[0];
	for (uint32_t i = 0; i < fmt_count; i++) {
		if (formats[i] == 28) { format = 28; break; } // DXGI_FORMAT_R8G8B8A8_UNORM
		if (formats[i] == 87) { format = 87; }         // DXGI_FORMAT_B8G8R8A8_UNORM
	}

	XrSwapchainCreateInfo ci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
	ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
	                XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT |
	                XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
	ci.format = format;
	ci.sampleCount = 1;
	ci.width = w;
	ci.height = h;
	ci.faceCount = 1;
	ci.arraySize = 2;   // SPI: left = layer 0, right = layer 1
	ci.mipCount = 1;

	XrResult r = s_ps.pfn_create_swapchain(s_ps.session, &ci, &s_ps.swapchain);
	if (XR_FAILED(r)) { ps_log("[DisplayXR-PROV] xrCreateSwapchain failed: %d\n", r); return 0; }

	s_ps.sc_width = w; s_ps.sc_height = h; s_ps.sc_array = 2; s_ps.sc_format = format;

	uint32_t count = 0;
	s_ps.pfn_enumerate_swapchain_images(s_ps.swapchain, 0, &count, NULL);
	if (count > PS_MAX_SWAPCHAIN_IMAGES) count = PS_MAX_SWAPCHAIN_IMAGES;
	s_ps.sc_image_count = count;
	for (uint32_t i = 0; i < count; i++) {
		s_ps.sc_images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR;
		s_ps.sc_images[i].next = NULL;
		s_ps.sc_images[i].texture = NULL;
	}
	r = s_ps.pfn_enumerate_swapchain_images(s_ps.swapchain, count, &count,
	        (XrSwapchainImageBaseHeader *)s_ps.sc_images);
	if (XR_FAILED(r)) { ps_log("[DisplayXR-PROV] enumerate swapchain images failed: %d\n", r); return 0; }

	s_ps.swapchain_created = 1;
	ps_log("[DisplayXR-PROV] SPI swapchain: %ux%u arraySize=2, %u images, fmt=%lld\n",
	       w, h, count, format);

	ps_create_bridge(); // pair the cross-device bridge with the swapchain
	return 1;
}

// Create the session's OWN D3D12 device on the runtime's adapter LUID, plus a
// queue + command list + fence for the per-frame bridge→swapchain copy.
// (Mirrors displayxr_standalone_d3d12.cpp create_device.)
static int ps_create_own_device(void)
{
	PFN_xrVoidFunction fn = NULL;
	s_ps.gipa(s_ps.instance, "xrGetD3D12GraphicsRequirementsKHR", &fn);
	LUID luid = {};
	if (fn) {
		XrGraphicsRequirementsD3D12KHR req = {XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR};
		if (XR_FAILED(((PFN_xrGetD3D12GraphicsRequirementsKHR)fn)(s_ps.instance, s_ps.system_id, &req))) {
			ps_log("[DisplayXR-PROV] xrGetD3D12GraphicsRequirementsKHR failed\n");
			return 0;
		}
		luid = req.adapterLuid;
	}

	IDXGIFactory4 *factory = NULL;
	CreateDXGIFactory2(0, __uuidof(IDXGIFactory4), (void **)&factory);
	IDXGIAdapter1 *adapter = NULL;
	if (factory && (luid.HighPart != 0 || luid.LowPart != 0))
		factory->EnumAdapterByLuid(luid, __uuidof(IDXGIAdapter1), (void **)&adapter);
	HRESULT hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0,
	                               __uuidof(ID3D12Device), (void **)&s_ps.own_device);
	if (adapter) adapter->Release();
	if (factory) factory->Release();
	if (FAILED(hr) || !s_ps.own_device) {
		ps_log("[DisplayXR-PROV] D3D12CreateDevice (own) failed: 0x%08lx\n", hr);
		return 0;
	}

	D3D12_COMMAND_QUEUE_DESC qd = {};
	qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	if (FAILED(s_ps.own_device->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue),
	        (void **)&s_ps.own_queue))) {
		ps_log("[DisplayXR-PROV] CreateCommandQueue (own) failed\n");
		return 0;
	}
	s_ps.own_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
	        __uuidof(ID3D12CommandAllocator), (void **)&s_ps.own_cmd_alloc);
	s_ps.own_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
	        s_ps.own_cmd_alloc, NULL, __uuidof(ID3D12GraphicsCommandList),
	        (void **)&s_ps.own_cmd_list);
	s_ps.own_cmd_list->Close();
	s_ps.own_device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
	        __uuidof(ID3D12Fence), (void **)&s_ps.own_fence);
	s_ps.own_fence_event = CreateEvent(NULL, FALSE, FALSE, NULL);
	s_ps.own_fence_value = 0;
	ps_log("[DisplayXR-PROV] Own D3D12 device + queue created (runtime session device)\n");
	return 1;
}

// Create the SHARED 2-slice-array bridge on own_device and open it on Unity's
// device. Unity renders both eyes into the Unity-side resource; submit copies
// the own-side resource into the runtime swapchain. (Mirrors create_atlas_bridge,
// extended arraySize=1 → 2 for SPI.)
// Allocate one SHARED render-target texture on own_device and open it on Unity's
// device. arr = 2 (SPI 2-slice array) or 1 (a single MultiPass eye).
static int ps_alloc_shared_tex(uint32_t w, uint32_t h, UINT16 arr,
                               ID3D12Resource **out_own, ID3D12Resource **out_unity,
                               HANDLE *out_handle, const char *label)
{
	D3D12_HEAP_PROPERTIES heap = {};
	heap.Type = D3D12_HEAP_TYPE_DEFAULT;
	D3D12_RESOURCE_DESC bd = {};
	bd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	bd.Width = w;
	bd.Height = h;
	bd.DepthOrArraySize = arr;
	bd.MipLevels = 1;
	bd.Format = (s_ps.sc_format == 87) ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
	bd.SampleDesc.Count = 1;
	bd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

	HRESULT hr = s_ps.own_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &bd,
	        D3D12_RESOURCE_STATE_COMMON, NULL, __uuidof(ID3D12Resource), (void **)out_own);
	if (FAILED(hr)) { ps_log("[DisplayXR-PROV] %s CreateCommittedResource failed: 0x%08lx\n", label, hr); return 0; }
	hr = s_ps.own_device->CreateSharedHandle(*out_own, NULL, GENERIC_ALL, NULL, out_handle);
	if (FAILED(hr) || !*out_handle) { ps_log("[DisplayXR-PROV] %s CreateSharedHandle failed: 0x%08lx\n", label, hr); return 0; }
	hr = s_ps.unity_device->OpenSharedHandle(*out_handle, __uuidof(ID3D12Resource), (void **)out_unity);
	if (FAILED(hr) || !*out_unity) { ps_log("[DisplayXR-PROV] %s OpenSharedHandle (Unity) failed: 0x%08lx\n", label, hr); return 0; }
	ps_log("[DisplayXR-PROV] %s: %ux%u arr=%u own=%p unity=%p\n", label, w, h, (unsigned)arr,
	       (void *)*out_own, (void *)*out_unity);
	return 1;
}

static int ps_create_bridge(void)
{
	if (!s_ps.own_device || !s_ps.unity_device) return 0;
	uint32_t w = s_ps.sc_width, h = s_ps.sc_height;
	if (w == 0 || h == 0) return 0;

	if (dxr_prov_get_single_pass()) {
		if (s_ps.bridge_own) return 1; // already created
		// SPI: one shared 2-slice array bridge (slice 0 = left, slice 1 = right).
		if (!ps_alloc_shared_tex(w, h, 2, &s_ps.bridge_own, &s_ps.bridge_unity,
		                         &s_ps.bridge_handle, "Bridge (SPI 2-slice array)"))
			return 0;
	} else {
		if (s_ps.bridge_own_eye[0]) return 1; // already created
		// MultiPass: two separate single-slice bridges, one per eye (one texture
		// per pass; textureArraySlice is SPI-only per the IUnityXRDisplay contract).
		for (int e = 0; e < 2; e++) {
			if (!ps_alloc_shared_tex(w, h, 1, &s_ps.bridge_own_eye[e], &s_ps.bridge_unity_eye[e],
			                         &s_ps.bridge_handle_eye[e],
			                         e == 0 ? "Bridge (MultiPass left)" : "Bridge (MultiPass right)"))
				return 0;
		}
	}

	// Cross-device SHARED fence: own_device creates it shared; Unity's device
	// opens the same fence. submit_frame signals it on Unity's queue (after the
	// render) and waits on it on the own queue (before the copy). Device-level and
	// size-independent, so a live-realloc (#172) keeps the existing fence and skips
	// this (recreating it would leak the old one and reset sync_val ordering).
	if (s_ps.unity_queue && !s_ps.shared_fence_own) {
		HRESULT hr2 = s_ps.own_device->CreateFence(0, D3D12_FENCE_FLAG_SHARED,
		        __uuidof(ID3D12Fence), (void **)&s_ps.shared_fence_own);
		if (SUCCEEDED(hr2)) {
			hr2 = s_ps.own_device->CreateSharedHandle(s_ps.shared_fence_own, NULL,
			        GENERIC_ALL, NULL, &s_ps.shared_fence_handle);
			if (SUCCEEDED(hr2) && s_ps.shared_fence_handle)
				s_ps.unity_device->OpenSharedHandle(s_ps.shared_fence_handle,
				        __uuidof(ID3D12Fence), (void **)&s_ps.shared_fence_unity);
		}
		ps_log("[DisplayXR-PROV] Shared sync fence: %s\n",
		       s_ps.shared_fence_unity ? "OK (cross-device render->copy sync)"
		                               : "FAILED (falling back to coarse sync)");
	} else {
		ps_log("[DisplayXR-PROV] No Unity queue → coarse sync only (possible black/tearing)\n");
	}
	return 1;
}

// ============================================================================
// Live tile realloc (#172) — track the per-view target size on resize / split /
// mode change, like a real handle app. See dxr_prov_reconcile_size().
// ============================================================================

// Target awaits this many consecutive stable frames before we realloc. An
// interactive resize drag changes GetClientRect every frame; debouncing reallocs
// once the drag settles instead of thrashing the swapchain (the Leia weaver
// stutters during a resize drag regardless — this just avoids churn).
#define PS_REALLOC_DEBOUNCE 6

// Block the CPU until all GPU work touching the bridge/swapchain has retired, so
// they can be safely destroyed. Drains our own copy queue, then Unity's render
// queue via the shared fence (shared_fence_own mirrors unity_queue's signal).
static void ps_drain_gpu(void)
{
	if (s_ps.own_queue && s_ps.own_fence && s_ps.own_fence_event) {
		s_ps.own_fence_value++;
		s_ps.own_queue->Signal(s_ps.own_fence, s_ps.own_fence_value);
		if (s_ps.own_fence->GetCompletedValue() < s_ps.own_fence_value) {
			s_ps.own_fence->SetEventOnCompletion(s_ps.own_fence_value, s_ps.own_fence_event);
			WaitForSingleObject(s_ps.own_fence_event, INFINITE);
		}
	}
	if (s_ps.unity_queue && s_ps.shared_fence_unity && s_ps.shared_fence_own) {
		s_ps.sync_val++;
		s_ps.unity_queue->Signal(s_ps.shared_fence_unity, s_ps.sync_val);
		if (s_ps.shared_fence_own->GetCompletedValue() < s_ps.sync_val) {
			HANDLE ev = CreateEvent(NULL, FALSE, FALSE, NULL);
			if (ev) {
				s_ps.shared_fence_own->SetEventOnCompletion(s_ps.sync_val, ev);
				WaitForSingleObject(ev, INFINITE);
				CloseHandle(ev);
			}
		}
	}
}

// Tear down the primary swapchain + its cross-device bridge(s) (NOT the shared
// fence or own device — those are size-independent), then recreate at the current
// target size via ps_create_swapchain (which recomputes size from the window/zone
// and re-pairs the bridge). Must run between frames with the GPU drained. Returns 1
// if the swapchain was recreated (caller drops+rewraps the Unity textures).
static int ps_recreate_primary_swapchain(void)
{
	ps_drain_gpu();

	// Release an acquired-but-unsubmitted image before destroying the swapchain.
	if (s_ps.image_acquired && s_ps.swapchain && s_ps.pfn_release_swapchain_image) {
		XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
		s_ps.pfn_release_swapchain_image(s_ps.swapchain, &ri);
	}
	s_ps.image_acquired = 0;

	if (s_ps.swapchain && s_ps.pfn_destroy_swapchain) s_ps.pfn_destroy_swapchain(s_ps.swapchain);
	s_ps.swapchain = XR_NULL_HANDLE;
	s_ps.swapchain_created = 0;
	s_ps.sc_image_count = 0;

	// SPI bridge (2-slice) + MultiPass per-eye bridges. Keep the shared fence.
	if (s_ps.bridge_unity) { s_ps.bridge_unity->Release(); s_ps.bridge_unity = NULL; }
	if (s_ps.bridge_handle) { CloseHandle(s_ps.bridge_handle); s_ps.bridge_handle = NULL; }
	if (s_ps.bridge_own)   { s_ps.bridge_own->Release();   s_ps.bridge_own = NULL; }
	for (int e = 0; e < 2; e++) {
		if (s_ps.bridge_unity_eye[e]) { s_ps.bridge_unity_eye[e]->Release(); s_ps.bridge_unity_eye[e] = NULL; }
		if (s_ps.bridge_handle_eye[e]) { CloseHandle(s_ps.bridge_handle_eye[e]); s_ps.bridge_handle_eye[e] = NULL; }
		if (s_ps.bridge_own_eye[e])   { s_ps.bridge_own_eye[e]->Release();   s_ps.bridge_own_eye[e] = NULL; }
	}

	int ok = ps_create_swapchain(); // recomputes target size + re-pairs the bridge
	ps_log("[DisplayXR-PROV] realloc: primary swapchain+bridge recreated -> %ux%u (%s)\n",
	       s_ps.sc_width, s_ps.sc_height, ok ? "OK" : "FAILED");
	return ok;
}

// The primary swapchain's current target per-view size (matches ps_create_swapchain).
static void ps_primary_target_size(uint32_t *out_w, uint32_t *out_h)
{
	uint32_t w = 0, h = 0;
	if (ps_zone_active()) {
		ps_query_zone_rec_size();
		w = s_ps.zone_rec_w; h = s_ps.zone_rec_h;
	} else {
		uint32_t ww = 0, wh = 0; ps_window_size(&ww, &wh);
		float sx = 0.5f, sy = 0.5f; ps_active_view_scale(&sx, &sy);
		w = (uint32_t)(ww * sx); h = (uint32_t)(wh * sy);
	}
	*out_w = w; *out_h = h;
}

// Debounce + realloc the PRIMARY swapchain to its current target size. Returns 1 if
// it reallocated this call (caller drops+rewraps the primary Unity texture).
static int ps_reconcile_primary(void)
{
	uint32_t tw = 0, th = 0;
	ps_primary_target_size(&tw, &th);
	if (tw == 0 || th == 0) return 0;

	if (tw == s_ps.sc_width && th == s_ps.sc_height) {
		s_ps.realloc_pending_w = 0; s_ps.realloc_pending_h = 0; s_ps.realloc_stable = 0;
		return 0; // already the right size
	}
	// Debounce: require the same new target for PS_REALLOC_DEBOUNCE frames.
	if (tw == s_ps.realloc_pending_w && th == s_ps.realloc_pending_h) {
		if (++s_ps.realloc_stable < PS_REALLOC_DEBOUNCE) return 0;
	} else {
		s_ps.realloc_pending_w = tw; s_ps.realloc_pending_h = th; s_ps.realloc_stable = 0;
		return 0;
	}
	s_ps.realloc_pending_w = 0; s_ps.realloc_pending_h = 0; s_ps.realloc_stable = 0;
	ps_log("[DisplayXR-PROV] realloc: primary target %ux%u -> %ux%u (settled)\n",
	       s_ps.sc_width, s_ps.sc_height, tw, th);
	return ps_recreate_primary_swapchain();
}

static void ps_reconcile_extra_zones(void); // defined with the zone helpers below

// Called at the top of the Unity frame (before the eye textures are wrapped).
// Reconciles the primary tile AND every extra 3D zone tile against their current
// target sizes (window×scaleXY / zone recommended view size), each debounced. Returns
// 1 if the PRIMARY reallocated (caller drops+rewraps the primary texture); extra-zone
// reallocs are signalled per-zone via dxr_prov_consume_zone_rewrap. Between frames only.
int dxr_prov_reconcile_size(void)
{
	if (!s_ps.running || !s_ps.session_ready || !s_ps.swapchain_created) return 0;
	if (s_ps.frame_begun) return 0; // never realloc between begin_frame and submit

	int primary_changed = ps_reconcile_primary();
	ps_reconcile_extra_zones();
	return primary_changed;
}

// ============================================================================
// Window-space UI (HUD) overlay layer (#67/#166)
// ============================================================================

// s_pending getter exported by the wsui module (displayxr_window_space_ui.cpp).
// C# DisplayXRWindowSpaceUI sets it via set_texture/set_layer regardless of which
// session is active, so the provider can drive its own window-space layer.
extern "C" int displayxr_window_space_ui_get_pending(void **out_tex, int *out_tex_w, int *out_tex_h,
                                                     float *out_x, float *out_y,
                                                     float *out_lw, float *out_lh, float *out_disp);

// Create (or recreate) the wsui overlay swapchain + cross-device bridge sized to
// w×h. Format = B8G8R8A8_UNORM (87) to match Unity's URP wsui RT — CopyTextureRegion
// is invalid across formats and the runtime's DComp path turns a mismatch into a
// device-removal (#82 / runtime#216). Mirrors ps_create_bridge but arraySize=1.
static int ps_create_wsui(uint32_t w, uint32_t h)
{
	if (w == 0 || h == 0 || !s_ps.own_device || !s_ps.unity_device) return 0;
	if (s_ps.wsui_swapchain_created && s_ps.wsui_registered_w == w && s_ps.wsui_registered_h == h)
		return 1; // already sized for this RT

	// Tear down any previous (RT resized).
	if (s_ps.wsui_swapchain && s_ps.pfn_destroy_swapchain) s_ps.pfn_destroy_swapchain(s_ps.wsui_swapchain);
	if (s_ps.wsui_bridge_unity) { s_ps.wsui_bridge_unity->Release(); s_ps.wsui_bridge_unity = NULL; }
	if (s_ps.wsui_bridge_own)   { s_ps.wsui_bridge_own->Release();   s_ps.wsui_bridge_own = NULL; }
	if (s_ps.wsui_bridge_handle) { CloseHandle(s_ps.wsui_bridge_handle); s_ps.wsui_bridge_handle = NULL; }
	s_ps.wsui_swapchain = XR_NULL_HANDLE;
	s_ps.wsui_swapchain_created = 0;
	s_ps.wsui_image_acquired = 0;

	// --- Overlay swapchain (arraySize=1, format 87 to match Unity URP wsui RT) ---
	uint32_t fmt_count = 0;
	s_ps.pfn_enumerate_swapchain_formats(s_ps.session, 0, &fmt_count, NULL);
	if (fmt_count == 0) { ps_log("[DisplayXR-PROV] wsui: no swapchain formats\n"); return 0; }
	int64_t formats[32];
	if (fmt_count > 32) fmt_count = 32;
	s_ps.pfn_enumerate_swapchain_formats(s_ps.session, fmt_count, &fmt_count, formats);
	int64_t format = formats[0];
	for (uint32_t i = 0; i < fmt_count; i++) { if (formats[i] == 87) { format = 87; break; } }

	XrSwapchainCreateInfo ci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
	ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
	                XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT |
	                XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
	ci.format = format;
	ci.sampleCount = 1;
	ci.width = w; ci.height = h;
	ci.faceCount = 1; ci.arraySize = 1; ci.mipCount = 1;
	if (XR_FAILED(s_ps.pfn_create_swapchain(s_ps.session, &ci, &s_ps.wsui_swapchain))) {
		ps_log("[DisplayXR-PROV] wsui: xrCreateSwapchain failed\n"); s_ps.wsui_swapchain = XR_NULL_HANDLE; return 0;
	}
	uint32_t count = 0;
	s_ps.pfn_enumerate_swapchain_images(s_ps.wsui_swapchain, 0, &count, NULL);
	if (count > PS_MAX_SWAPCHAIN_IMAGES) count = PS_MAX_SWAPCHAIN_IMAGES;
	for (uint32_t i = 0; i < count; i++) {
		s_ps.wsui_images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR;
		s_ps.wsui_images[i].next = NULL; s_ps.wsui_images[i].texture = NULL;
	}
	if (XR_FAILED(s_ps.pfn_enumerate_swapchain_images(s_ps.wsui_swapchain, count, &count,
	        (XrSwapchainImageBaseHeader *)s_ps.wsui_images))) {
		ps_log("[DisplayXR-PROV] wsui: enumerate images failed\n"); return 0;
	}
	s_ps.wsui_image_count = count;

	// --- Cross-device bridge (own_device shared BGRA8, opened on Unity's device) ---
	D3D12_HEAP_PROPERTIES heap = {}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
	D3D12_RESOURCE_DESC bd = {};
	bd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	bd.Width = w; bd.Height = h; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
	bd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	bd.SampleDesc.Count = 1;
	bd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
	HRESULT hr = s_ps.own_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &bd,
	        D3D12_RESOURCE_STATE_COMMON, NULL, __uuidof(ID3D12Resource), (void **)&s_ps.wsui_bridge_own);
	if (FAILED(hr)) { ps_log("[DisplayXR-PROV] wsui bridge create failed: 0x%08lx\n", hr); return 0; }
	hr = s_ps.own_device->CreateSharedHandle(s_ps.wsui_bridge_own, NULL, GENERIC_ALL, NULL, &s_ps.wsui_bridge_handle);
	if (FAILED(hr) || !s_ps.wsui_bridge_handle) { ps_log("[DisplayXR-PROV] wsui bridge share failed\n"); return 0; }
	hr = s_ps.unity_device->OpenSharedHandle(s_ps.wsui_bridge_handle, __uuidof(ID3D12Resource),
	        (void **)&s_ps.wsui_bridge_unity);
	if (FAILED(hr) || !s_ps.wsui_bridge_unity) { ps_log("[DisplayXR-PROV] wsui bridge open(Unity) failed\n"); return 0; }

	s_ps.wsui_w = w; s_ps.wsui_h = h; s_ps.wsui_format = format;
	s_ps.wsui_registered_w = w; s_ps.wsui_registered_h = h;
	s_ps.wsui_swapchain_created = 1;
	ps_log("[DisplayXR-PROV] wsui: swapchain %ux%u (%u imgs, fmt=%lld) + bridge own=%p unity=%p\n",
	       w, h, count, (long long)format, (void *)s_ps.wsui_bridge_own, (void *)s_ps.wsui_bridge_unity);
	return 1;
}

// C# (DisplayXRWindowSpaceUI) calls this to get the Unity-device handle of the wsui
// bridge, then Graphics.CopyTexture's its canvas RT into it each frame. Lazily
// creates the swapchain+bridge sized to w×h. Returns the Unity-side ID3D12Resource*
// (or NULL). Declared DISPLAYXR_EXPORT in the header so it's exported for P/Invoke.
void dxr_prov_get_wsui_bridge(uint32_t w, uint32_t h,
                              void **out_ptr, uint32_t *out_w, uint32_t *out_h)
{
	if (out_ptr) *out_ptr = NULL;
	if (out_w) *out_w = 0;
	if (out_h) *out_h = 0;
	if (!s_ps.running || !s_ps.session_ready) return;
	if (!ps_create_wsui(w, h)) return;
	if (out_ptr) *out_ptr = s_ps.wsui_bridge_unity;
	if (out_w) *out_w = s_ps.wsui_w;
	if (out_h) *out_h = s_ps.wsui_h;
}

// Per-frame: if a wsui texture is registered and the bridge is ready, copy
// wsui_bridge_own -> the acquired overlay swapchain image (own device) and fill
// out_layer. Returns 1 if the layer should be submitted, else 0. Called from
// dxr_prov_submit_frame AFTER the projection bridge copy (own_cmd_list is free and
// the shared-fence wait already ordered the own queue after Unity's writes).
static int ps_submit_wsui(XrCompositionLayerWindowSpaceEXT *out_layer)
{
	if (!out_layer) return 0;
	memset(out_layer, 0, sizeof(*out_layer));

	void *tex = NULL; int tw = 0, th = 0;
	float lx = 0, ly = 0, lw = 0, lh = 0, ldisp = 0;
	if (!displayxr_window_space_ui_get_pending(&tex, &tw, &th, &lx, &ly, &lw, &lh, &ldisp))
		return 0;
	if (!s_ps.wsui_swapchain_created || !s_ps.wsui_bridge_own || s_ps.wsui_image_count == 0)
		return 0; // C# hasn't requested the bridge yet (no get_wsui_bridge call)

	// Acquire + wait an overlay swapchain image.
	uint32_t idx = 0;
	XrSwapchainImageAcquireInfo ai = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
	if (XR_FAILED(s_ps.pfn_acquire_swapchain_image(s_ps.wsui_swapchain, &ai, &idx)) ||
	    idx >= s_ps.wsui_image_count)
		return 0;
	XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
	wi.timeout = 1000000000;
	s_ps.pfn_wait_swapchain_image(s_ps.wsui_swapchain, &wi);

	// own_device copy bridge -> swapchain image (single subresource).
	if (s_ps.wsui_images[idx].texture && s_ps.own_cmd_list) {
		s_ps.own_cmd_alloc->Reset();
		s_ps.own_cmd_list->Reset(s_ps.own_cmd_alloc, NULL);
		D3D12_TEXTURE_COPY_LOCATION dl = {};
		dl.pResource = s_ps.wsui_images[idx].texture;
		dl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dl.SubresourceIndex = 0;
		D3D12_TEXTURE_COPY_LOCATION sl = {};
		sl.pResource = s_ps.wsui_bridge_own;
		sl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; sl.SubresourceIndex = 0;
		s_ps.own_cmd_list->CopyTextureRegion(&dl, 0, 0, 0, &sl, NULL);
		s_ps.own_cmd_list->Close();
		ID3D12CommandList *lists[] = { s_ps.own_cmd_list };
		s_ps.own_queue->ExecuteCommandLists(1, lists);
		s_ps.own_fence_value++;
		s_ps.own_queue->Signal(s_ps.own_fence, s_ps.own_fence_value);
		if (s_ps.own_fence->GetCompletedValue() < s_ps.own_fence_value) {
			s_ps.own_fence->SetEventOnCompletion(s_ps.own_fence_value, s_ps.own_fence_event);
			WaitForSingleObject(s_ps.own_fence_event, INFINITE);
		}
	}

	XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
	s_ps.pfn_release_swapchain_image(s_ps.wsui_swapchain, &ri);

	out_layer->type = XR_TYPE_COMPOSITION_LAYER_WINDOW_SPACE_EXT;
	out_layer->next = NULL;
	out_layer->layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
	out_layer->subImage.swapchain = s_ps.wsui_swapchain;
	out_layer->subImage.imageRect.offset = {0, 0};
	out_layer->subImage.imageRect.extent = {(int32_t)s_ps.wsui_w, (int32_t)s_ps.wsui_h};
	out_layer->subImage.imageArrayIndex = 0;
	out_layer->x = lx; out_layer->y = ly;
	out_layer->width = lw; out_layer->height = lh;
	out_layer->disparity = ldisp;
	return 1;
}

// ============================================================================
// Local2D layer (#166 Phase B) — mirrors the wsui bridge, but XrCompositionLayer-
// Local2DEXT at a client-window PIXEL rect (the 2D band). BGRA8 to match Unity's URP RT.
// ============================================================================

static int ps_create_local2d(uint32_t w, uint32_t h)
{
	if (w == 0 || h == 0 || !s_ps.own_device || !s_ps.unity_device) return 0;
	if (s_ps.l2d_swapchain_created && s_ps.l2d_registered_w == w && s_ps.l2d_registered_h == h)
		return 1;
	if (s_ps.l2d_swapchain && s_ps.pfn_destroy_swapchain) s_ps.pfn_destroy_swapchain(s_ps.l2d_swapchain);
	if (s_ps.l2d_bridge_unity)  { s_ps.l2d_bridge_unity->Release();  s_ps.l2d_bridge_unity = NULL; }
	if (s_ps.l2d_bridge_own)    { s_ps.l2d_bridge_own->Release();    s_ps.l2d_bridge_own = NULL; }
	if (s_ps.l2d_bridge_handle) { CloseHandle(s_ps.l2d_bridge_handle); s_ps.l2d_bridge_handle = NULL; }
	s_ps.l2d_swapchain = XR_NULL_HANDLE;
	s_ps.l2d_swapchain_created = 0;

	uint32_t fmt_count = 0;
	s_ps.pfn_enumerate_swapchain_formats(s_ps.session, 0, &fmt_count, NULL);
	if (fmt_count == 0) { ps_log("[DisplayXR-PROV] local2d: no swapchain formats\n"); return 0; }
	int64_t formats[32];
	if (fmt_count > 32) fmt_count = 32;
	s_ps.pfn_enumerate_swapchain_formats(s_ps.session, fmt_count, &fmt_count, formats);
	int64_t format = formats[0];
	for (uint32_t i = 0; i < fmt_count; i++) { if (formats[i] == 87) { format = 87; break; } }

	XrSwapchainCreateInfo ci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
	ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
	                XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT |
	                XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
	ci.format = format;
	ci.sampleCount = 1; ci.width = w; ci.height = h;
	ci.faceCount = 1; ci.arraySize = 1; ci.mipCount = 1;
	if (XR_FAILED(s_ps.pfn_create_swapchain(s_ps.session, &ci, &s_ps.l2d_swapchain))) {
		ps_log("[DisplayXR-PROV] local2d: xrCreateSwapchain failed\n"); s_ps.l2d_swapchain = XR_NULL_HANDLE; return 0;
	}
	uint32_t count = 0;
	s_ps.pfn_enumerate_swapchain_images(s_ps.l2d_swapchain, 0, &count, NULL);
	if (count > PS_MAX_SWAPCHAIN_IMAGES) count = PS_MAX_SWAPCHAIN_IMAGES;
	for (uint32_t i = 0; i < count; i++) {
		s_ps.l2d_images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR;
		s_ps.l2d_images[i].next = NULL; s_ps.l2d_images[i].texture = NULL;
	}
	if (XR_FAILED(s_ps.pfn_enumerate_swapchain_images(s_ps.l2d_swapchain, count, &count,
	        (XrSwapchainImageBaseHeader *)s_ps.l2d_images))) {
		ps_log("[DisplayXR-PROV] local2d: enumerate images failed\n"); return 0;
	}
	s_ps.l2d_image_count = count;

	D3D12_HEAP_PROPERTIES heap = {}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
	D3D12_RESOURCE_DESC bd = {};
	bd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	bd.Width = w; bd.Height = h; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
	bd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	bd.SampleDesc.Count = 1;
	bd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
	HRESULT hr = s_ps.own_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &bd,
	        D3D12_RESOURCE_STATE_COMMON, NULL, __uuidof(ID3D12Resource), (void **)&s_ps.l2d_bridge_own);
	if (FAILED(hr)) { ps_log("[DisplayXR-PROV] local2d bridge create failed: 0x%08lx\n", hr); return 0; }
	hr = s_ps.own_device->CreateSharedHandle(s_ps.l2d_bridge_own, NULL, GENERIC_ALL, NULL, &s_ps.l2d_bridge_handle);
	if (FAILED(hr) || !s_ps.l2d_bridge_handle) { ps_log("[DisplayXR-PROV] local2d bridge share failed\n"); return 0; }
	hr = s_ps.unity_device->OpenSharedHandle(s_ps.l2d_bridge_handle, __uuidof(ID3D12Resource),
	        (void **)&s_ps.l2d_bridge_unity);
	if (FAILED(hr) || !s_ps.l2d_bridge_unity) { ps_log("[DisplayXR-PROV] local2d bridge open(Unity) failed\n"); return 0; }

	s_ps.l2d_w = w; s_ps.l2d_h = h; s_ps.l2d_format = format;
	s_ps.l2d_registered_w = w; s_ps.l2d_registered_h = h;
	s_ps.l2d_swapchain_created = 1;
	ps_log("[DisplayXR-PROV] local2d: swapchain %ux%u (%u imgs) + bridge own=%p unity=%p\n",
	       w, h, count, (void *)s_ps.l2d_bridge_own, (void *)s_ps.l2d_bridge_unity);
	return 1;
}

void dxr_prov_get_local2d_bridge(uint32_t w, uint32_t h,
                                 void **out_ptr, uint32_t *out_w, uint32_t *out_h)
{
	if (out_ptr) *out_ptr = NULL;
	if (out_w) *out_w = 0;
	if (out_h) *out_h = 0;
	if (!s_ps.running || !s_ps.session_ready) return;
	if (!ps_create_local2d(w, h)) return;
	if (out_ptr) *out_ptr = s_ps.l2d_bridge_unity;
	if (out_w) *out_w = s_ps.l2d_w;
	if (out_h) *out_h = s_ps.l2d_h;
}

void dxr_prov_set_local2d_rect(int32_t x, int32_t y, int32_t w, int32_t h)
{
	if (w <= 0 || h <= 0) { s_ps.l2d_rect_set = 0; return; }
	s_ps.l2d_rect_x = x; s_ps.l2d_rect_y = y; s_ps.l2d_rect_w = w; s_ps.l2d_rect_h = h;
	s_ps.l2d_rect_set = 1;
}

// Per-frame: copy the Local2D bridge into its overlay swapchain image and fill the
// layer (XrCompositionLayerLocal2DEXT, dest = client-window pixel rect). Returns 1
// if the layer should be submitted. Called from submit after the projection copy.
static int ps_submit_local2d(XrCompositionLayerLocal2DEXT *out_layer)
{
	if (!out_layer) return 0;
	memset(out_layer, 0, sizeof(*out_layer));
	if (!s_ps.l2d_rect_set || !s_ps.l2d_swapchain_created || !s_ps.l2d_bridge_own || s_ps.l2d_image_count == 0)
		return 0;

	uint32_t idx = 0;
	XrSwapchainImageAcquireInfo ai = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
	if (XR_FAILED(s_ps.pfn_acquire_swapchain_image(s_ps.l2d_swapchain, &ai, &idx)) ||
	    idx >= s_ps.l2d_image_count)
		return 0;
	XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
	wi.timeout = 1000000000;
	s_ps.pfn_wait_swapchain_image(s_ps.l2d_swapchain, &wi);

	if (s_ps.l2d_images[idx].texture && s_ps.own_cmd_list) {
		s_ps.own_cmd_alloc->Reset();
		s_ps.own_cmd_list->Reset(s_ps.own_cmd_alloc, NULL);
		D3D12_TEXTURE_COPY_LOCATION dl = {};
		dl.pResource = s_ps.l2d_images[idx].texture;
		dl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dl.SubresourceIndex = 0;
		D3D12_TEXTURE_COPY_LOCATION sl = {};
		sl.pResource = s_ps.l2d_bridge_own;
		sl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; sl.SubresourceIndex = 0;
		s_ps.own_cmd_list->CopyTextureRegion(&dl, 0, 0, 0, &sl, NULL);
		s_ps.own_cmd_list->Close();
		ID3D12CommandList *lists[] = { s_ps.own_cmd_list };
		s_ps.own_queue->ExecuteCommandLists(1, lists);
		s_ps.own_fence_value++;
		s_ps.own_queue->Signal(s_ps.own_fence, s_ps.own_fence_value);
		if (s_ps.own_fence->GetCompletedValue() < s_ps.own_fence_value) {
			s_ps.own_fence->SetEventOnCompletion(s_ps.own_fence_value, s_ps.own_fence_event);
			WaitForSingleObject(s_ps.own_fence_event, INFINITE);
		}
	}

	XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
	s_ps.pfn_release_swapchain_image(s_ps.l2d_swapchain, &ri);

	out_layer->type = XR_TYPE_COMPOSITION_LAYER_LOCAL_2D_EXT;
	out_layer->next = NULL;
	// Unity Canvas is straight (unpremultiplied) alpha — flag it so the runtime
	// doesn't double-darken (matches the hook path's Local2D + avatar bubble).
	out_layer->layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
	                        XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
	out_layer->subImage.swapchain = s_ps.l2d_swapchain;
	out_layer->subImage.imageRect.offset = {0, 0};
	out_layer->subImage.imageRect.extent = {(int32_t)s_ps.l2d_w, (int32_t)s_ps.l2d_h};
	out_layer->subImage.imageArrayIndex = 0;
	out_layer->rect.offset = {s_ps.l2d_rect_x, s_ps.l2d_rect_y};
	out_layer->rect.extent = {s_ps.l2d_rect_w, s_ps.l2d_rect_h};
	return 1;
}

// ============================================================================
// Extra 3D zones (#166 Phase B2) — per-zone swapchain + bridge + locate + copy,
// mirroring the primary zone path. The primary zone stays on the top-level fields.
// ============================================================================

static void ps_query_extra_zone_rec(ProviderExtraZone *z)
{
	z->rec_w = z->rect_w > 0 ? (uint32_t)z->rect_w : 0;
	z->rec_h = z->rect_h > 0 ? (uint32_t)z->rect_h : 0;
	if (s_ps.pfn_get_zone_view_size && z->valid && s_ps.session != XR_NULL_HANDLE) {
		XrRect2Di rect = {{z->rect_x, z->rect_y}, {z->rect_w, z->rect_h}};
		XrExtent2Di rec = {0, 0};
		if (XR_SUCCEEDED(s_ps.pfn_get_zone_view_size(s_ps.session, &rect, &rec)) && rec.width > 0 && rec.height > 0) {
			z->rec_w = (uint32_t)rec.width; z->rec_h = (uint32_t)rec.height;
		}
	}
}

static int ps_create_extra_zone(ProviderExtraZone *z)
{
	if (z->swapchain_created) return 1;
	if (!z->valid || !s_ps.session_ready) return 0;
	ps_query_extra_zone_rec(z);
	uint32_t w = z->rec_w, h = z->rec_h;
	if (w == 0 || h == 0) return 0;

	uint32_t fmt_count = 0;
	s_ps.pfn_enumerate_swapchain_formats(s_ps.session, 0, &fmt_count, NULL);
	if (fmt_count == 0) return 0;
	int64_t formats[32]; if (fmt_count > 32) fmt_count = 32;
	s_ps.pfn_enumerate_swapchain_formats(s_ps.session, fmt_count, &fmt_count, formats);
	int64_t format = formats[0];
	for (uint32_t i = 0; i < fmt_count; i++) { if (formats[i] == 28) { format = 28; break; } if (formats[i] == 87) format = 87; }

	XrSwapchainCreateInfo ci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
	ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
	ci.format = format; ci.sampleCount = 1; ci.width = w; ci.height = h;
	ci.faceCount = 1; ci.arraySize = 2; ci.mipCount = 1;
	if (XR_FAILED(s_ps.pfn_create_swapchain(s_ps.session, &ci, &z->swapchain))) { z->swapchain = XR_NULL_HANDLE; return 0; }
	z->sc_width = w; z->sc_height = h; z->sc_format = format;
	uint32_t count = 0;
	s_ps.pfn_enumerate_swapchain_images(z->swapchain, 0, &count, NULL);
	if (count > PS_MAX_SWAPCHAIN_IMAGES) count = PS_MAX_SWAPCHAIN_IMAGES;
	z->sc_image_count = count;
	for (uint32_t i = 0; i < count; i++) { z->sc_images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR; z->sc_images[i].next = NULL; z->sc_images[i].texture = NULL; }
	if (XR_FAILED(s_ps.pfn_enumerate_swapchain_images(z->swapchain, count, &count, (XrSwapchainImageBaseHeader *)z->sc_images))) return 0;

	// Bridge(s). ps_alloc_shared_tex reads s_ps.sc_format for the D3D12 format; the
	// extra zone picks the SAME format as the primary, so this matches.
	int sp = dxr_prov_get_single_pass();
	if (sp) {
		if (!ps_alloc_shared_tex(w, h, 2, &z->bridge_own, &z->bridge_unity, &z->bridge_handle, "extra-zone bridge (SPI)")) return 0;
	} else {
		if (!ps_alloc_shared_tex(w, h, 1, &z->bridge_own_eye[0], &z->bridge_unity_eye[0], &z->bridge_handle_eye[0], "extra-zone bridge L")) return 0;
		if (!ps_alloc_shared_tex(w, h, 1, &z->bridge_own_eye[1], &z->bridge_unity_eye[1], &z->bridge_handle_eye[1], "extra-zone bridge R")) return 0;
	}
	z->swapchain_created = 1;
	ps_log("[DisplayXR-PROV] extra zone id=%u: swapchain %ux%u + bridge(s) (sp=%d)\n", z->zone_id, w, h, sp);
	return 1;
}

// Create all pending extra-zone swapchains/bridges (called at session-ready + lazily).
static void ps_create_extra_zones(void)
{
	for (uint32_t i = 0; i < s_ps.extra_zone_count; i++)
		if (s_ps.extra_zones[i].valid && !s_ps.extra_zones[i].swapchain_created)
			ps_create_extra_zone(&s_ps.extra_zones[i]);
}

// Live tile realloc (#172) for an extra zone: drain, release+destroy the zone's
// swapchain+bridge(s), recreate at the current recommended view size, and set the
// rewrap latch so the Unity side re-wraps this zone's texture(s). Runs between frames.
static int ps_recreate_extra_zone(ProviderExtraZone *z)
{
	ps_drain_gpu();
	if (z->image_acquired && z->swapchain && s_ps.pfn_release_swapchain_image) {
		XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
		s_ps.pfn_release_swapchain_image(z->swapchain, &ri);
	}
	z->image_acquired = 0;
	if (z->swapchain && s_ps.pfn_destroy_swapchain) s_ps.pfn_destroy_swapchain(z->swapchain);
	z->swapchain = XR_NULL_HANDLE;
	z->swapchain_created = 0;
	z->sc_image_count = 0;
	if (z->bridge_unity) { z->bridge_unity->Release(); z->bridge_unity = NULL; }
	if (z->bridge_handle) { CloseHandle(z->bridge_handle); z->bridge_handle = NULL; }
	if (z->bridge_own)   { z->bridge_own->Release();   z->bridge_own = NULL; }
	for (int e = 0; e < 2; e++) {
		if (z->bridge_unity_eye[e]) { z->bridge_unity_eye[e]->Release(); z->bridge_unity_eye[e] = NULL; }
		if (z->bridge_handle_eye[e]) { CloseHandle(z->bridge_handle_eye[e]); z->bridge_handle_eye[e] = NULL; }
		if (z->bridge_own_eye[e])   { z->bridge_own_eye[e]->Release();   z->bridge_own_eye[e] = NULL; }
	}
	int ok = ps_create_extra_zone(z);
	z->needs_rewrap = 1;
	ps_log("[DisplayXR-PROV] realloc: extra zone id=%u swapchain+bridge recreated -> %ux%u (%s)\n",
	       z->zone_id, z->sc_width, z->sc_height, ok ? "OK" : "FAILED");
	return ok;
}

// Reconcile every extra zone's tile against its current recommended view size,
// debounced. Runs inside dxr_prov_reconcile_size after the primary. Recreated zones
// set needs_rewrap (consumed by dxr_prov_consume_zone_rewrap).
static void ps_reconcile_extra_zones(void)
{
	for (uint32_t i = 0; i < s_ps.extra_zone_count; i++) {
		ProviderExtraZone *z = &s_ps.extra_zones[i];
		if (!z->valid || !z->swapchain_created) continue;
		ps_query_extra_zone_rec(z);
		uint32_t tw = z->rec_w, th = z->rec_h;
		if (tw == 0 || th == 0) continue;
		if (tw == z->sc_width && th == z->sc_height) {
			z->realloc_pending_w = 0; z->realloc_pending_h = 0; z->realloc_stable = 0;
			continue;
		}
		if (tw == z->realloc_pending_w && th == z->realloc_pending_h) {
			if (++z->realloc_stable < PS_REALLOC_DEBOUNCE) continue;
		} else {
			z->realloc_pending_w = tw; z->realloc_pending_h = th; z->realloc_stable = 0;
			continue;
		}
		z->realloc_pending_w = 0; z->realloc_pending_h = 0; z->realloc_stable = 0;
		ps_log("[DisplayXR-PROV] realloc: extra zone id=%u target %ux%u -> %ux%u (settled)\n",
		       z->zone_id, z->sc_width, z->sc_height, tw, th);
		ps_recreate_extra_zone(z);
	}
}

// Consume the rewrap latch for extra zone `index` (0-based). Returns 1 (and clears)
// if that zone was just reallocated and its Unity texture(s) must be re-wrapped.
int dxr_prov_consume_zone_rewrap(uint32_t index)
{
	if (index >= PS_MAX_ZONES - 1) return 0;
	ProviderExtraZone *z = &s_ps.extra_zones[index];
	if (!z->needs_rewrap) return 0;
	z->needs_rewrap = 0;
	return 1;
}

// Locate each extra zone (zone-scoped, XrDisplayZoneEXT chained) → z->views, and
// acquire its swapchain image. Mirrors the primary locate in dxr_prov_begin_frame.
static void ps_locate_extra_zones(XrTime display_time)
{
	for (uint32_t i = 0; i < s_ps.extra_zone_count; i++) {
		ProviderExtraZone *z = &s_ps.extra_zones[i];
		z->view_count = 0;
		if (!z->valid) continue;
		if (!z->swapchain_created) ps_create_extra_zone(z);
		if (s_ps.local_space == XR_NULL_HANDLE || !s_ps.pfn_locate_views) continue;

		XrViewLocateInfo li = {XR_TYPE_VIEW_LOCATE_INFO};
		li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
		li.displayTime = display_time; li.space = s_ps.local_space;
		XrView views[PS_MAX_VIEWS]; for (int k = 0; k < PS_MAX_VIEWS; k++) views[k] = {XR_TYPE_VIEW};
		XrViewState vstate = {XR_TYPE_VIEW_STATE};
		XrDisplayRigEXT display_rig = {XR_TYPE_DISPLAY_RIG_EXT};
		XrCameraRigEXT  camera_rig  = {XR_TYPE_CAMERA_RIG_EXT};
		XrDisplayZoneEXT zone = {XR_TYPE_DISPLAY_ZONE_EXT};
		XrPosef rig_pose = s_ps.display_pose_set ? s_ps.display_pose : XrPosef{{0, 0, 0, 1}, {0, 0, 0}};
		if (s_ps.has_view_rig) {
			if (s_ps.camera_centric) {
				camera_rig.pose = rig_pose;
				camera_rig.ipdFactor = s_ps.tunables_set ? s_ps.ipd_factor : 1.0f;
				camera_rig.parallaxFactor = s_ps.tunables_set ? s_ps.parallax_factor : 1.0f;
				camera_rig.convergenceDiopters = s_ps.inv_convergence_distance;
				camera_rig.verticalFov = 2.0f * atanf(s_ps.fov_override);
				camera_rig.metersToVirtual = 1.0f;
				li.next = &camera_rig;
			} else {
				float vdh = s_ps.display_info.is_valid && s_ps.display_info.height_m > 0.0f ? s_ps.display_info.height_m : 0.2f;
				display_rig.pose = rig_pose;
				display_rig.virtualDisplayHeight = (s_ps.tunables_set && s_ps.virtual_display_height > 0.0f) ? s_ps.virtual_display_height : vdh;
				display_rig.ipdFactor = s_ps.tunables_set ? s_ps.ipd_factor : 1.0f;
				display_rig.parallaxFactor = s_ps.tunables_set ? s_ps.parallax_factor : 1.0f;
				display_rig.perspectiveFactor = s_ps.tunables_set ? s_ps.perspective_factor : 1.0f;
				li.next = &display_rig;
			}
			zone.zoneId = z->zone_id;
			zone.rect.offset = {z->rect_x, z->rect_y};
			zone.rect.extent = {z->rect_w, z->rect_h};
			zone.next = li.next; li.next = &zone;
		}
		uint32_t vcount = 0;
		if (XR_SUCCEEDED(s_ps.pfn_locate_views(s_ps.session, &li, &vstate, PS_MAX_VIEWS, &vcount, views)) && vcount >= 1) {
			uint32_t n = vcount < 2 ? vcount : 2;
			for (uint32_t k = 0; k < n; k++) {
				z->views[k].position[0] = views[k].pose.position.x;
				z->views[k].position[1] = views[k].pose.position.y;
				z->views[k].position[2] = views[k].pose.position.z;
				z->views[k].orientation[0] = views[k].pose.orientation.x;
				z->views[k].orientation[1] = views[k].pose.orientation.y;
				z->views[k].orientation[2] = views[k].pose.orientation.z;
				z->views[k].orientation[3] = views[k].pose.orientation.w;
				z->views[k].fov[0] = views[k].fov.angleLeft;
				z->views[k].fov[1] = views[k].fov.angleRight;
				z->views[k].fov[2] = views[k].fov.angleUp;
				z->views[k].fov[3] = views[k].fov.angleDown;
				z->views[k].valid = s_ps.has_view_rig ? 1 : 0;
			}
			if (n == 1) { z->views[1] = z->views[0]; n = 2; }
			z->view_count = n;
		}
		if (z->swapchain_created && !z->image_acquired) {
			uint32_t idx = 0; XrSwapchainImageAcquireInfo ai = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
			if (XR_SUCCEEDED(s_ps.pfn_acquire_swapchain_image(z->swapchain, &ai, &idx))) {
				XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO}; wi.timeout = 1000000000;
				s_ps.pfn_wait_swapchain_image(z->swapchain, &wi);
				z->image_acquired = 1; z->acquired_index = idx;
			}
		}
	}
}

// Copy an extra zone's bridge → its acquired swapchain image + release (mirrors the
// primary copy). Cross-device fence-synced against Unity's render.
static void ps_copy_extra_zone(ProviderExtraZone *z)
{
	if (!z->swapchain_created || !z->image_acquired || z->acquired_index >= z->sc_image_count) return;
	int sp = dxr_prov_get_single_pass();
	ID3D12Resource *dst = z->sc_images[z->acquired_index].texture;
	ID3D12Resource *copy_src = sp ? z->bridge_own : z->bridge_own_eye[0];
	if (dst && copy_src && s_ps.own_cmd_list) {
		s_ps.own_cmd_alloc->Reset();
		s_ps.own_cmd_list->Reset(s_ps.own_cmd_alloc, NULL);
		for (UINT slice = 0; slice < 2; slice++) {
			D3D12_TEXTURE_COPY_LOCATION dl = {}; dl.pResource = dst; dl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dl.SubresourceIndex = slice;
			D3D12_TEXTURE_COPY_LOCATION sl = {}; sl.pResource = sp ? z->bridge_own : z->bridge_own_eye[slice]; sl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; sl.SubresourceIndex = sp ? slice : 0;
			s_ps.own_cmd_list->CopyTextureRegion(&dl, 0, 0, 0, &sl, NULL);
		}
		s_ps.own_cmd_list->Close();
		if (s_ps.shared_fence_unity && s_ps.shared_fence_own && s_ps.unity_queue) {
			s_ps.sync_val++;
			s_ps.unity_queue->Signal(s_ps.shared_fence_unity, s_ps.sync_val);
			s_ps.own_queue->Wait(s_ps.shared_fence_own, s_ps.sync_val);
		}
		ID3D12CommandList *lists[] = { s_ps.own_cmd_list };
		s_ps.own_queue->ExecuteCommandLists(1, lists);
		s_ps.own_fence_value++;
		s_ps.own_queue->Signal(s_ps.own_fence, s_ps.own_fence_value);
		if (s_ps.own_fence->GetCompletedValue() < s_ps.own_fence_value) {
			s_ps.own_fence->SetEventOnCompletion(s_ps.own_fence_value, s_ps.own_fence_event);
			WaitForSingleObject(s_ps.own_fence_event, INFINITE);
		}
	}
	XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
	s_ps.pfn_release_swapchain_image(z->swapchain, &ri);
	z->image_acquired = 0;
}

// Getters for the display-provider (Unity texture wrap + render passes).
void *dxr_prov_get_extra_zone_bridge(uint32_t ei, uint32_t *w, uint32_t *h)
{
	if (ei >= PS_MAX_ZONES - 1) return NULL;
	ProviderExtraZone *z = &s_ps.extra_zones[ei];
	if (w) *w = z->sc_width; if (h) *h = z->sc_height;
	return (void *)z->bridge_unity;
}

void *dxr_prov_get_extra_zone_bridge_eye(uint32_t ei, uint32_t eye, uint32_t *w, uint32_t *h)
{
	if (ei >= PS_MAX_ZONES - 1 || eye > 1) return NULL;
	ProviderExtraZone *z = &s_ps.extra_zones[ei];
	if (w) *w = z->sc_width; if (h) *h = z->sc_height;
	return (void *)z->bridge_unity_eye[eye];
}

void dxr_prov_get_extra_zone_view(uint32_t ei, uint32_t eye, DxrProvView *out_view)
{
	if (!out_view) return;
	if (ei >= PS_MAX_ZONES - 1 || eye > 1) { memset(out_view, 0, sizeof(*out_view)); return; }
	*out_view = s_ps.extra_zones[ei].views[eye];
}

// ============================================================================
// Lifecycle
// ============================================================================

int dxr_prov_session_start(const char *runtime_json_path,
                           void *unity_d3d12_device,
                           void *unity_d3d12_queue,
                           void *overlay_hwnd)
{
	// Preserve the C#-chosen render mode across the reset — it is pushed by the
	// loader (dxr_prov_set_single_pass) BEFORE this start runs.
	int saved_single_pass = s_ps.single_pass;
	int saved_single_pass_set = s_ps.single_pass_set;
	int saved_transparent = s_ps.transparent_requested;
	// Zone rect may be seeded before the session starts (demo SubsystemRegistration,
	// like the hook SeedLaunchZone) so the swapchain is born zone-sized. Preserve it.
	int saved_zone_valid = s_ps.zone_valid;
	uint32_t saved_zone_id = s_ps.zone_id;
	int32_t saved_zx = s_ps.zone_x, saved_zy = s_ps.zone_y, saved_zw = s_ps.zone_w, saved_zh = s_ps.zone_h;
	// Extra-zone rects are seeded before session start too; save + restore just the
	// rect fields (the swapchain/bridge handles are session-specific → reset to 0).
	uint32_t saved_extra_count = s_ps.extra_zone_count;
	ProviderExtraZone saved_extra[PS_MAX_ZONES - 1];
	for (uint32_t i = 0; i < PS_MAX_ZONES - 1; i++) saved_extra[i] = s_ps.extra_zones[i];
	memset(&s_ps, 0, sizeof(s_ps));
	s_ps.single_pass = saved_single_pass;
	s_ps.single_pass_set = saved_single_pass_set;
	s_ps.transparent_requested = saved_transparent;
	s_ps.zone_valid = saved_zone_valid;
	s_ps.zone_id = saved_zone_id;
	s_ps.zone_x = saved_zx; s_ps.zone_y = saved_zy; s_ps.zone_w = saved_zw; s_ps.zone_h = saved_zh;
	s_ps.extra_zone_count = saved_extra_count;
	for (uint32_t i = 0; i < PS_MAX_ZONES - 1; i++) {
		s_ps.extra_zones[i].valid = saved_extra[i].valid;
		s_ps.extra_zones[i].zone_id = saved_extra[i].zone_id;
		s_ps.extra_zones[i].rect_x = saved_extra[i].rect_x;
		s_ps.extra_zones[i].rect_y = saved_extra[i].rect_y;
		s_ps.extra_zones[i].rect_w = saved_extra[i].rect_w;
		s_ps.extra_zones[i].rect_h = saved_extra[i].rect_h;
	}
	s_ps.unity_device = (ID3D12Device *)unity_d3d12_device; // for opening the bridge + shared fence
	s_ps.unity_queue  = (ID3D12CommandQueue *)unity_d3d12_queue; // signals the cross-device sync fence
	s_ps.overlay_hwnd = overlay_hwnd;
	s_ps.ipd_factor = s_ps.parallax_factor = s_ps.perspective_factor = 1.0f;
	s_ps.fov_override = tanf(0.5f * 1.0471975512f); // tan(30°) — neutral ~60° vFOV default
	s_ps.zone_caps_ok = -1; // untried (lazy caps query on first zone frame)

	if (!s_ps.unity_device) {
		ps_log("[DisplayXR-PROV] start: missing Unity D3D12 device (needed for bridge)\n");
		return 0;
	}

	char *json = ps_resolve_runtime_json(runtime_json_path);
	if (!json) { ps_log("[DisplayXR-PROV] No runtime JSON (set XR_RUNTIME_JSON)\n"); return 0; }
	char *lib_rel = ps_parse_library_path(json);
	if (!lib_rel) { ps_log("[DisplayXR-PROV] library_path not found in %s\n", json); free(json); return 0; }
	char *lib_abs = ps_resolve_library_path(json, lib_rel);
	ps_log("[DisplayXR-PROV] runtime: %s\n", lib_abs);

	HMODULE hmod = LoadLibraryExA(lib_abs, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
	free(json); free(lib_rel); free(lib_abs);
	if (!hmod) { ps_log("[DisplayXR-PROV] LoadLibrary failed: %lu\n", GetLastError()); return 0; }
	s_ps.runtime_lib = hmod;

	PFN_xrNegotiateLoaderRuntimeInterface negotiate =
	    (PFN_xrNegotiateLoaderRuntimeInterface)GetProcAddress(hmod, "xrNegotiateLoaderRuntimeInterface");
	if (!negotiate) { ps_log("[DisplayXR-PROV] negotiate symbol missing\n"); return 0; }

	XrNegotiateLoaderInfo li = {};
	li.structType = XR_LOADER_INTERFACE_STRUCT_LOADER_INFO;
	li.structVersion = 1;
	li.structSize = sizeof(li);
	li.minInterfaceVersion = 1;
	li.maxInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION;
	li.minApiVersion = XR_MAKE_VERSION(1, 0, 0);
	li.maxApiVersion = XR_MAKE_VERSION(1, 1, 0);
	XrNegotiateRuntimeRequest rr = {};
	rr.structType = XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST;
	rr.structVersion = 1;
	rr.structSize = sizeof(rr);
	if (XR_FAILED(negotiate(&li, &rr)) || !rr.getInstanceProcAddr) {
		ps_log("[DisplayXR-PROV] negotiate failed\n");
		return 0;
	}
	s_ps.gipa = rr.getInstanceProcAddr;

	// --- Probe XR_EXT_view_rig before requesting it (older runtimes reject unknown) ---
	{
		PFN_xrVoidFunction fn = NULL;
		s_ps.gipa(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", &fn);
		if (fn) {
			PFN_xrEnumerateInstanceExtensionProperties enum_ext = (PFN_xrEnumerateInstanceExtensionProperties)fn;
			uint32_t avail = 0;
			enum_ext(NULL, 0, &avail, NULL);
			if (avail > 0) {
				XrExtensionProperties *props = (XrExtensionProperties *)calloc(avail, sizeof(XrExtensionProperties));
				for (uint32_t i = 0; i < avail; i++) props[i].type = XR_TYPE_EXTENSION_PROPERTIES;
				if (XR_SUCCEEDED(enum_ext(NULL, avail, &avail, props))) {
					for (uint32_t i = 0; i < avail; i++) {
						if (strcmp(props[i].extensionName, XR_EXT_VIEW_RIG_EXTENSION_NAME) == 0)
							s_ps.has_view_rig = 1;
						else if (strcmp(props[i].extensionName, XR_EXT_DISPLAY_ZONES_EXTENSION_NAME) == 0)
							s_ps.has_display_zones = 1;
						else if (strcmp(props[i].extensionName, XR_EXT_LOCAL_3D_ZONE_EXTENSION_NAME) == 0)
							s_ps.has_local_3d_zone = 1;
					}
				}
				free(props);
			}
		}
		ps_log("[DisplayXR-PROV] display_zones: %s; local_3d_zone: %s\n",
		       s_ps.has_display_zones ? "AVAILABLE" : "no",
		       s_ps.has_local_3d_zone ? "AVAILABLE" : "no");
		ps_log("[DisplayXR-PROV] XR_EXT_view_rig: %s\n",
		       s_ps.has_view_rig ? "AVAILABLE" : "not found (no stereo)");
	}

	// --- Create instance ---
	const char *extensions[8];
	uint32_t ext_count = 0;
	extensions[ext_count++] = XR_EXT_DISPLAY_INFO_EXTENSION_NAME;
	extensions[ext_count++] = "XR_KHR_D3D12_enable";
	extensions[ext_count++] = XR_EXT_WIN32_WINDOW_BINDING_EXTENSION_NAME;
	if (s_ps.has_view_rig) extensions[ext_count++] = XR_EXT_VIEW_RIG_EXTENSION_NAME;
	// Zones (#166 Phase B): display_zones needs view_rig (composes on top of it);
	// local_3d_zone is required to submit Local2D layers for the 2D bands.
	if (s_ps.has_display_zones && s_ps.has_view_rig)
		extensions[ext_count++] = XR_EXT_DISPLAY_ZONES_EXTENSION_NAME;
	if (s_ps.has_local_3d_zone)
		extensions[ext_count++] = XR_EXT_LOCAL_3D_ZONE_EXTENSION_NAME;

	PFN_xrVoidFunction fn_create = NULL;
	s_ps.gipa(XR_NULL_HANDLE, "xrCreateInstance", &fn_create);
	if (!fn_create) { ps_log("[DisplayXR-PROV] xrCreateInstance unresolved\n"); return 0; }
	XrInstanceCreateInfo ici = {XR_TYPE_INSTANCE_CREATE_INFO};
	strncpy(ici.applicationInfo.applicationName, "DisplayXR Unity Provider", XR_MAX_APPLICATION_NAME_SIZE - 1);
	ici.applicationInfo.applicationVersion = 1;
	strncpy(ici.applicationInfo.engineName, "Unity", XR_MAX_ENGINE_NAME_SIZE - 1);
	ici.applicationInfo.engineVersion = 1;
	ici.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
	ici.enabledExtensionCount = ext_count;
	ici.enabledExtensionNames = extensions;
	if (XR_FAILED(((PFN_xrCreateInstance)fn_create)(&ici, &s_ps.instance))) {
		ps_log("[DisplayXR-PROV] xrCreateInstance failed\n");
		return 0;
	}

	if (!ps_resolve_functions()) { dxr_prov_session_stop(); return 0; }

	// --- System ---
	XrSystemGetInfo sgi = {XR_TYPE_SYSTEM_GET_INFO};
	sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	if (XR_FAILED(s_ps.pfn_get_system(s_ps.instance, &sgi, &s_ps.system_id))) {
		ps_log("[DisplayXR-PROV] xrGetSystem failed\n"); dxr_prov_session_stop(); return 0;
	}

	// --- Create the session's OWN D3D12 device (matched to runtime adapter LUID).
	//     The runtime allocates its swapchain on this device; we bridge to Unity. ---
	if (!ps_create_own_device()) { dxr_prov_session_stop(); return 0; }

	// --- Display info ---
	XrDisplayInfoEXT di = {};
	di.type = XR_TYPE_DISPLAY_INFO_EXT;
	XrSystemProperties sp = {XR_TYPE_SYSTEM_PROPERTIES};
	sp.next = &di;
	if (XR_SUCCEEDED(s_ps.pfn_get_system_properties(s_ps.instance, s_ps.system_id, &sp))) {
		s_ps.display_info.width_m = di.displaySizeMeters.width;
		s_ps.display_info.height_m = di.displaySizeMeters.height;
		s_ps.display_info.pixel_width = di.displayPixelWidth;
		s_ps.display_info.pixel_height = di.displayPixelHeight;
		s_ps.display_info.nominal_x = di.nominalViewerPositionInDisplaySpace.x;
		s_ps.display_info.nominal_y = di.nominalViewerPositionInDisplaySpace.y;
		s_ps.display_info.nominal_z = di.nominalViewerPositionInDisplaySpace.z;
		s_ps.display_info.scale_x = di.recommendedViewScaleX;
		s_ps.display_info.scale_y = di.recommendedViewScaleY;
		s_ps.display_info.is_valid = 1;
		ps_log("[DisplayXR-PROV] Display: %ux%u %.3fx%.3fm\n",
		       di.displayPixelWidth, di.displayPixelHeight,
		       di.displaySizeMeters.width, di.displaySizeMeters.height);
	} else {
		ps_log("[DisplayXR-PROV] xrGetSystemProperties failed\n");
	}

	// --- Environment blend modes (transparency, #166 Phase A) ---
	// Probe whether the runtime advertises ALPHA_BLEND for the primary-stereo view
	// config. Only then do we opt the session into a transparent background.
	s_ps.alpha_blend_supported = 0;
	if (s_ps.pfn_enumerate_blend_modes) {
		uint32_t bm_count = 0;
		s_ps.pfn_enumerate_blend_modes(s_ps.instance, s_ps.system_id,
		        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &bm_count, NULL);
		if (bm_count > 0 && bm_count <= 8) {
			XrEnvironmentBlendMode bms[8];
			if (XR_SUCCEEDED(s_ps.pfn_enumerate_blend_modes(s_ps.instance, s_ps.system_id,
			        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, bm_count, &bm_count, bms))) {
				for (uint32_t i = 0; i < bm_count; i++)
					if (bms[i] == XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND)
						{ s_ps.alpha_blend_supported = 1; break; }
			}
		}
		ps_log("[DisplayXR-PROV] ALPHA_BLEND: %s (transparent requested=%d)\n",
		       s_ps.alpha_blend_supported ? "advertised" : "not advertised",
		       s_ps.transparent_requested);
	}
	int use_transparent = s_ps.transparent_requested && s_ps.alpha_blend_supported;

	// --- Session: D3D12 binding on UNITY'S device + window binding (overlay HWND) ---
	XrWin32WindowBindingCreateInfoEXT win_binding = {};
	win_binding.type = XR_TYPE_WIN32_WINDOW_BINDING_CREATE_INFO_EXT;
	win_binding.windowHandle = s_ps.overlay_hwnd;
	win_binding.readbackCallback = NULL;
	win_binding.readbackUserdata = NULL;
	win_binding.sharedTextureHandle = NULL;
	// Transparent background (#166 Phase A): opt the runtime's DComp swapchain into
	// per-pixel alpha over the desktop. chromaKeyColor=0 (no post-weave conversion —
	// the runtime composes-under-bg + alpha-gates, same as the hook path).
	win_binding.transparentBackgroundEnabled = use_transparent ? XR_TRUE : XR_FALSE;
	win_binding.chromaKeyColor = 0;

	XrGraphicsBindingD3D12KHR d3d12 = {};
	d3d12.type = XR_TYPE_GRAPHICS_BINDING_D3D12_KHR;
	d3d12.device = s_ps.own_device;   // session runs on our own device
	d3d12.queue = s_ps.own_queue;
	d3d12.next = &win_binding;

	XrSessionCreateInfo sci = {XR_TYPE_SESSION_CREATE_INFO};
	sci.next = &d3d12;
	sci.systemId = s_ps.system_id;
	if (XR_FAILED(s_ps.pfn_create_session(s_ps.instance, &sci, &s_ps.session))) {
		ps_log("[DisplayXR-PROV] xrCreateSession failed\n"); dxr_prov_session_stop(); return 0;
	}
	ps_log("[DisplayXR-PROV] Session created (own D3D12 device, overlay HWND=%p)\n", s_ps.overlay_hwnd);

	// --- LOCAL reference space ---
	XrReferenceSpaceCreateInfo rsci = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
	rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	rsci.poseInReferenceSpace.orientation = XrQuaternionf{0, 0, 0, 1};
	rsci.poseInReferenceSpace.position = XrVector3f{0, 0, 0};
	if (XR_FAILED(s_ps.pfn_create_reference_space(s_ps.session, &rsci, &s_ps.local_space))) {
		ps_log("[DisplayXR-PROV] xrCreateReferenceSpace failed\n");
		s_ps.local_space = XR_NULL_HANDLE;
	}

	ps_enumerate_modes();

	// Swapchain is created on session-ready (deferred), but attempt early too.
	ps_create_swapchain();

	s_ps.running = 1;
	ps_log("[DisplayXR-PROV] Session start OK\n");
	return 1;
}

void dxr_prov_session_stop(void)
{
	if (s_ps.wsui_swapchain && s_ps.pfn_destroy_swapchain) s_ps.pfn_destroy_swapchain(s_ps.wsui_swapchain);
	if (s_ps.wsui_bridge_unity)  s_ps.wsui_bridge_unity->Release();
	if (s_ps.wsui_bridge_handle) CloseHandle(s_ps.wsui_bridge_handle);
	if (s_ps.wsui_bridge_own)    s_ps.wsui_bridge_own->Release();
	if (s_ps.l2d_swapchain && s_ps.pfn_destroy_swapchain) s_ps.pfn_destroy_swapchain(s_ps.l2d_swapchain);
	if (s_ps.l2d_bridge_unity)  s_ps.l2d_bridge_unity->Release();
	if (s_ps.l2d_bridge_handle) CloseHandle(s_ps.l2d_bridge_handle);
	if (s_ps.l2d_bridge_own)    s_ps.l2d_bridge_own->Release();
	for (uint32_t i = 0; i < PS_MAX_ZONES - 1; i++) {
		ProviderExtraZone *z = &s_ps.extra_zones[i];
		if (z->swapchain && s_ps.pfn_destroy_swapchain) s_ps.pfn_destroy_swapchain(z->swapchain);
		if (z->bridge_unity) z->bridge_unity->Release();
		if (z->bridge_handle) CloseHandle(z->bridge_handle);
		if (z->bridge_own) z->bridge_own->Release();
		for (int e = 0; e < 2; e++) {
			if (z->bridge_unity_eye[e]) z->bridge_unity_eye[e]->Release();
			if (z->bridge_handle_eye[e]) CloseHandle(z->bridge_handle_eye[e]);
			if (z->bridge_own_eye[e]) z->bridge_own_eye[e]->Release();
		}
	}
	if (s_ps.swapchain && s_ps.pfn_destroy_swapchain) s_ps.pfn_destroy_swapchain(s_ps.swapchain);
	if (s_ps.session && s_ps.session_ready && s_ps.pfn_end_session) s_ps.pfn_end_session(s_ps.session);
	if (s_ps.session && s_ps.pfn_destroy_session) s_ps.pfn_destroy_session(s_ps.session);
	if (s_ps.instance && s_ps.pfn_destroy_instance) s_ps.pfn_destroy_instance(s_ps.instance);

	// Bridge + cross-device fence + own-device resources.
	if (s_ps.shared_fence_unity)  s_ps.shared_fence_unity->Release();
	if (s_ps.shared_fence_handle) CloseHandle(s_ps.shared_fence_handle);
	if (s_ps.shared_fence_own)    s_ps.shared_fence_own->Release();
	if (s_ps.bridge_unity)    s_ps.bridge_unity->Release();
	if (s_ps.bridge_handle)   CloseHandle(s_ps.bridge_handle);
	if (s_ps.bridge_own)      s_ps.bridge_own->Release();
	for (int e = 0; e < 2; e++) {
		if (s_ps.bridge_unity_eye[e])  s_ps.bridge_unity_eye[e]->Release();
		if (s_ps.bridge_handle_eye[e]) CloseHandle(s_ps.bridge_handle_eye[e]);
		if (s_ps.bridge_own_eye[e])    s_ps.bridge_own_eye[e]->Release();
	}
	if (s_ps.own_fence_event) CloseHandle(s_ps.own_fence_event);
	if (s_ps.own_fence)       s_ps.own_fence->Release();
	if (s_ps.own_cmd_list)    s_ps.own_cmd_list->Release();
	if (s_ps.own_cmd_alloc)   s_ps.own_cmd_alloc->Release();
	if (s_ps.own_queue)       s_ps.own_queue->Release();
	if (s_ps.own_device)      s_ps.own_device->Release();

	// Intentionally do NOT FreeLibrary(runtime_lib): background threads may still
	// reference it (matches the standalone's deliberate leak).
	void *lib = s_ps.runtime_lib;
	memset(&s_ps, 0, sizeof(s_ps));
	s_ps.runtime_lib = lib;
}

int dxr_prov_session_is_running(void) { return s_ps.running; }

// Render-mode gate (#166 task #8). C# decides SPI vs MultiPass from the active
// render pipeline (URP+Win+D3D12 → SPI; BiRP/other → MultiPass — SPI renders
// opaque geometry wrong on BiRP) and pushes it before the session starts.
void dxr_prov_set_single_pass(int enable)
{
	s_ps.single_pass = enable ? 1 : 0;
	s_ps.single_pass_set = 1;
	ps_log("[DisplayXR-PROV] render mode: %s\n",
	       s_ps.single_pass ? "Single-Pass-Instanced (SPI)" : "MultiPass (2 pass x 1)");
}

// Effective mode. Default = SPI (1) when C# never set one, preserving the
// pre-gating behavior.
int dxr_prov_get_single_pass(void) { return s_ps.single_pass_set ? s_ps.single_pass : 1; }

// Transparent-background request (#166 Phase A). Set from C# BEFORE the session
// starts; the actual ALPHA_BLEND opt-in also requires the runtime to advertise it
// (probed in session_start). Preserved across the session_start memset.
void dxr_prov_set_transparent_background(int enable)
{
	s_ps.transparent_requested = enable ? 1 : 0;
	ps_log("[DisplayXR-PROV] transparent background requested: %d\n", s_ps.transparent_requested);
}

int dxr_prov_wants_transparent(void) { return s_ps.transparent_requested; }

// --- Zones (#166 Phase B) ---------------------------------------------------
// Define the single 3D-zone rect (client-window px, top-left origin). The locate
// chains XrDisplayZoneEXT in front of the rig and submit chains it on the
// projection layer; the swapchain is (re)sized to the zone's recommended view
// size. w<=0||h<=0 clears (full-window framing). Seed BEFORE the session starts
// (demo SubsystemRegistration) so the swapchain is born zone-sized — a later
// change only re-sizes the swapchain on the next session start (live realloc is a
// follow-up, mirroring the hook path's launch-seed model).
void dxr_prov_set_3d_zone_rect(int32_t x, int32_t y, int32_t w, int32_t h)
{
	if (w <= 0 || h <= 0) {
		if (s_ps.zone_valid) ps_log("[DisplayXR-PROV] 3d_zone: cleared\n");
		s_ps.zone_valid = 0;
		return;
	}
	int changed = (!s_ps.zone_valid || s_ps.zone_x != x || s_ps.zone_y != y ||
	               s_ps.zone_w != w || s_ps.zone_h != h);
	s_ps.zone_x = x; s_ps.zone_y = y; s_ps.zone_w = w; s_ps.zone_h = h;
	if (s_ps.zone_id == 0) s_ps.zone_id = 1;
	s_ps.zone_valid = 1;
	if (changed) {
		ps_query_zone_rec_size();
		ps_log("[DisplayXR-PROV] 3d_zone: (%d,%d) %dx%d -> rec %ux%u\n",
		       x, y, w, h, s_ps.zone_rec_w, s_ps.zone_rec_h);
	}
}

void dxr_prov_clear_3d_zone(void)
{
	s_ps.zone_valid = 0;
	ps_log("[DisplayXR-PROV] clear_3d_zone\n");
}

// Multi-zone (#166 Phase B2). total_3d_zones = number of 3D zones (index 0 =
// primary, 1.. = extra). Clamped to PS_MAX_ZONES. 0/1 → no extra zones.
void dxr_prov_set_zone_count(uint32_t total_3d_zones)
{
	if (total_3d_zones > PS_MAX_ZONES) total_3d_zones = PS_MAX_ZONES;
	uint32_t extra = total_3d_zones > 1 ? total_3d_zones - 1 : 0;
	if (extra != s_ps.extra_zone_count)
		ps_log("[DisplayXR-PROV] zone count: %u 3D zone(s) (1 primary + %u extra)\n",
		       total_3d_zones ? total_3d_zones : 1, extra);
	// Deactivate any extra zones beyond the new count.
	for (uint32_t i = extra; i < PS_MAX_ZONES - 1; i++) s_ps.extra_zones[i].valid = 0;
	s_ps.extra_zone_count = extra;
}

// Set 3D zone `index`'s rect (client-window px). index 0 → primary (== set_3d_zone_rect);
// index>=1 → extra zone [index-1]. Seed BEFORE session start for born-zone-sized.
void dxr_prov_set_zone(uint32_t index, uint32_t zone_id, int32_t x, int32_t y, int32_t w, int32_t h)
{
	if (index == 0) {
		dxr_prov_set_3d_zone_rect(x, y, w, h);
		if (zone_id) s_ps.zone_id = zone_id;
		return;
	}
	uint32_t ei = index - 1;
	if (ei >= PS_MAX_ZONES - 1) return;
	ProviderExtraZone *z = &s_ps.extra_zones[ei];
	if (w <= 0 || h <= 0) { z->valid = 0; return; }
	z->zone_id = zone_id ? zone_id : (index + 1);
	z->rect_x = x; z->rect_y = y; z->rect_w = w; z->rect_h = h;
	z->valid = 1;
	if (ei + 1 > s_ps.extra_zone_count) s_ps.extra_zone_count = ei + 1;
}

uint32_t dxr_prov_get_extra_zone_count(void) { return s_ps.extra_zone_count; }

// ============================================================================
// Swapchain surfacing
// ============================================================================

void dxr_prov_get_swapchain_info(uint32_t *width, uint32_t *height,
                                 uint32_t *array_size, uint32_t *image_count)
{
	if (width) *width = s_ps.sc_width;
	if (height) *height = s_ps.sc_height;
	if (array_size) *array_size = s_ps.sc_array;
	if (image_count) *image_count = s_ps.sc_image_count;
}

void *dxr_prov_get_bridge_unity_texture(uint32_t *width, uint32_t *height, uint32_t *array_size)
{
	if (width) *width = s_ps.sc_width;
	if (height) *height = s_ps.sc_height;
	if (array_size) *array_size = 2;
	return (void *)s_ps.bridge_unity; // SPI mode; NULL in MultiPass mode
}

void *dxr_prov_get_bridge_unity_texture_eye(uint32_t eye, uint32_t *width, uint32_t *height)
{
	if (width) *width = s_ps.sc_width;
	if (height) *height = s_ps.sc_height;
	if (eye > 1) return NULL;
	return (void *)s_ps.bridge_unity_eye[eye]; // MultiPass mode; NULL in SPI mode
}

// ============================================================================
// Frame loop
// ============================================================================

void dxr_prov_poll_events(void)
{
	if (!s_ps.running || !s_ps.pfn_poll_event) return;
	XrEventDataBuffer ev = {XR_TYPE_EVENT_DATA_BUFFER};
	while (s_ps.pfn_poll_event(s_ps.instance, &ev) == XR_SUCCESS) {
		if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
			XrEventDataSessionStateChanged *ssc = (XrEventDataSessionStateChanged *)&ev;
			s_ps.session_state = ssc->state;
			ps_log("[DisplayXR-PROV] session state: %d\n", (int)ssc->state);
			switch (ssc->state) {
			case XR_SESSION_STATE_READY: {
				XrSessionBeginInfo bi = {XR_TYPE_SESSION_BEGIN_INFO};
				bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
				if (XR_SUCCEEDED(s_ps.pfn_begin_session(s_ps.session, &bi))) {
					s_ps.session_ready = 1;
					if (!s_ps.swapchain_created) ps_create_swapchain();
					ps_create_extra_zones();
				}
				break;
			}
			case XR_SESSION_STATE_STOPPING:
				s_ps.session_ready = 0;
				s_ps.pfn_end_session(s_ps.session);
				break;
			case XR_SESSION_STATE_EXITING:
			case XR_SESSION_STATE_LOSS_PENDING:
				s_ps.running = 0;
				break;
			default: break;
			}
		} else if (ev.type == XR_TYPE_EVENT_DATA_RENDERING_MODE_CHANGED_EXT) {
			XrEventDataRenderingModeChangedEXT *mc = (XrEventDataRenderingModeChangedEXT *)&ev;
			// Re-enumerate so active_mode_index + per-mode tiling/scale refresh;
			// the per-frame render rect (dxr_prov_get_render_rect) then adapts with
			// NO swapchain realloc (worst-case sized). Latch for C#.
			ps_enumerate_modes();
			s_ps.active_mode_index = mc->currentModeIndex;
			s_ps.ev_mode_prev = mc->previousModeIndex;
			s_ps.ev_mode_cur = mc->currentModeIndex;
			s_ps.ev_mode_changed = 1;
			ps_log("[DisplayXR-PROV] event: rendering mode %u -> %u\n",
			       mc->previousModeIndex, mc->currentModeIndex);
		} else if (ev.type == XR_TYPE_EVENT_DATA_HARDWARE_DISPLAY_STATE_CHANGED_EXT) {
			XrEventDataHardwareDisplayStateChangedEXT *hc = (XrEventDataHardwareDisplayStateChangedEXT *)&ev;
			s_ps.ev_hw3d = hc->hardwareDisplay3D ? 1 : 0;
			s_ps.ev_hw_changed = 1;
			ps_log("[DisplayXR-PROV] event: hardware 3D -> %d\n", s_ps.ev_hw3d);
		} else if (ev.type == XR_TYPE_EVENT_DATA_EYE_TRACKING_STATE_CHANGED_EXT) {
			XrEventDataEyeTrackingStateChangedEXT *tc = (XrEventDataEyeTrackingStateChangedEXT *)&ev;
			s_ps.ev_track_is_tracking = tc->isTracking ? 1 : 0;
			s_ps.ev_track_mode = (int)tc->activeMode;
			s_ps.ev_track_changed = 1;
			ps_log("[DisplayXR-PROV] event: eye tracking -> %d (mode %d)\n",
			       s_ps.ev_track_is_tracking, s_ps.ev_track_mode);
		}
		ev = {XR_TYPE_EVENT_DATA_BUFFER};
	}
}

int dxr_prov_begin_frame(uint32_t *out_image_index, int *out_should_render)
{
	if (out_should_render) *out_should_render = 0;
	if (out_image_index) *out_image_index = 0;
	if (!s_ps.running || !s_ps.session_ready) return 0;

	XrFrameState fs = {XR_TYPE_FRAME_STATE};
	if (XR_FAILED(s_ps.pfn_wait_frame(s_ps.session, NULL, &fs))) return 0;
	if (XR_FAILED(s_ps.pfn_begin_frame(s_ps.session, NULL))) return 0;
	s_ps.predicted_display_time = fs.predictedDisplayTime;
	s_ps.frame_begun = 1;

	// --- Per-frame render rect. In a 3D-zone frame the swapchain IS zone-sized, so
	//     render == swapchain (full slice, no sub-rect). Otherwise render =
	//     window(overlay client) × active-mode scaleXY, clamped to the swapchain. ---
	if (ps_zone_active()) {
		s_ps.render_w = s_ps.sc_width;
		s_ps.render_h = s_ps.sc_height;
	} else {
		uint32_t ww, wh; ps_window_size(&ww, &wh);
		float sx, sy; ps_active_view_scale(&sx, &sy);
		uint32_t rw = (uint32_t)(ww * sx);
		uint32_t rh = (uint32_t)(wh * sy);
		if (rw == 0) rw = 1;
		if (rh == 0) rh = 1;
		if (s_ps.sc_width  && rw > s_ps.sc_width)  rw = s_ps.sc_width;
		if (s_ps.sc_height && rh > s_ps.sc_height) rh = s_ps.sc_height;
		s_ps.render_w = rw;
		s_ps.render_h = rh;
	}

	// --- Locate views (XR_EXT_view_rig chained → render-ready pose+fov) ---
	s_ps.view_count = 0;
	if (s_ps.local_space != XR_NULL_HANDLE && s_ps.pfn_locate_views) {
		XrViewLocateInfo li = {XR_TYPE_VIEW_LOCATE_INFO};
		li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
		li.displayTime = fs.predictedDisplayTime;
		li.space = s_ps.local_space;

		XrView views[PS_MAX_VIEWS];
		for (int i = 0; i < PS_MAX_VIEWS; i++) views[i] = {XR_TYPE_VIEW};
		XrViewState vstate = {XR_TYPE_VIEW_STATE};
		uint32_t vcount = 0;

		// XR_EXT_view_rig: chain the rig descriptor matching the active rig mode
		// (mirrors displayxr_standalone.cpp). Camera-centric → XrCameraRigEXT;
		// display-centric → XrDisplayRigEXT. The runtime returns render-ready
		// XrView{pose,fov}; the raw channel recovers the raw display-space eyes.
		XrDisplayRigEXT display_rig = {XR_TYPE_DISPLAY_RIG_EXT};
		XrCameraRigEXT  camera_rig  = {XR_TYPE_CAMERA_RIG_EXT};
		XrDisplayZoneEXT locate_zone = {XR_TYPE_DISPLAY_ZONE_EXT};
		XrViewDisplayRawEXT raw = {XR_TYPE_VIEW_DISPLAY_RAW_EXT};
		XrPosef rig_pose = s_ps.display_pose_set ? s_ps.display_pose
		                                         : XrPosef{{0, 0, 0, 1}, {0, 0, 0}};
		if (s_ps.has_view_rig) {
			if (s_ps.camera_centric) {
				camera_rig.pose = rig_pose;
				camera_rig.ipdFactor = s_ps.tunables_set ? s_ps.ipd_factor : 1.0f;
				camera_rig.parallaxFactor = s_ps.tunables_set ? s_ps.parallax_factor : 1.0f;
				camera_rig.convergenceDiopters = s_ps.inv_convergence_distance;
				camera_rig.verticalFov = 2.0f * atanf(s_ps.fov_override);
				camera_rig.metersToVirtual = 1.0f; // spec v3: identity meters->world
				li.next = &camera_rig;
			} else {
				// Default virtualDisplayHeight to the physical display height (a
				// window-as-portal 1:1 framing) so a scene with no DisplayXR rig
				// pushing tunables still gets real stereo separation. Apps override
				// via dxr_prov_set_tunables (wired from DisplayXRDisplay).
				float vdh = s_ps.display_info.is_valid && s_ps.display_info.height_m > 0.0f
				            ? s_ps.display_info.height_m : 0.2f;
				display_rig.pose = rig_pose;
				// vdh <= 0 always means "use the physical display height" (matches the
				// rig's own resolution), so the C# driver can pass a raw 0 safely even
				// before its display-info query resolves.
				display_rig.virtualDisplayHeight =
				    (s_ps.tunables_set && s_ps.virtual_display_height > 0.0f)
				        ? s_ps.virtual_display_height : vdh;
				display_rig.ipdFactor = s_ps.tunables_set ? s_ps.ipd_factor : 1.0f;
				display_rig.parallaxFactor = s_ps.tunables_set ? s_ps.parallax_factor : 1.0f;
				display_rig.perspectiveFactor = s_ps.tunables_set ? s_ps.perspective_factor : 1.0f;
				li.next = &display_rig;
			}
			// Zones (#166 Phase B): chain XrDisplayZoneEXT in FRONT of the rig so the
			// runtime frames the Kooima into the zone rect (the rect IS the canvas).
			// Same instance is chained on the projection layer at submit.
			if (ps_zone_active()) {
				locate_zone.zoneId = s_ps.zone_id ? s_ps.zone_id : 1;
				locate_zone.rect.offset = {s_ps.zone_x, s_ps.zone_y};
				locate_zone.rect.extent = {s_ps.zone_w, s_ps.zone_h};
				locate_zone.next = li.next; // the rig
				li.next = &locate_zone;
			}
			vstate.next = &raw;
		}

		if (XR_SUCCEEDED(s_ps.pfn_locate_views(s_ps.session, &li, &vstate,
		                                       PS_MAX_VIEWS, &vcount, views)) && vcount >= 1) {
			uint32_t n = vcount < DXR_PROV_MAX_VIEWS ? vcount : DXR_PROV_MAX_VIEWS;
			for (uint32_t i = 0; i < n; i++) {
				s_ps.views[i].position[0] = views[i].pose.position.x;
				s_ps.views[i].position[1] = views[i].pose.position.y;
				s_ps.views[i].position[2] = views[i].pose.position.z;
				s_ps.views[i].orientation[0] = views[i].pose.orientation.x;
				s_ps.views[i].orientation[1] = views[i].pose.orientation.y;
				s_ps.views[i].orientation[2] = views[i].pose.orientation.z;
				s_ps.views[i].orientation[3] = views[i].pose.orientation.w;
				s_ps.views[i].fov[0] = views[i].fov.angleLeft;
				s_ps.views[i].fov[1] = views[i].fov.angleRight;
				s_ps.views[i].fov[2] = views[i].fov.angleUp;
				s_ps.views[i].fov[3] = views[i].fov.angleDown;
				s_ps.views[i].valid = s_ps.has_view_rig ? 1 : 0;
			}
			// Ensure 2 views for stereo (duplicate if runtime returned 1).
			if (n == 1) { s_ps.views[1] = s_ps.views[0]; n = 2; }
			s_ps.view_count = n;
		}
	}

	// Publish stereo matrices for the transparent overlay's cyclopean hit-test
	// (LMB drag on the silhouette) — inert on the hook path, which owns this state.
	ps_publish_stereo_matrices();

	// Locate the extra 3D zones (each zone-scoped) + acquire their swapchain images.
	ps_locate_extra_zones(fs.predictedDisplayTime);

	// --- Acquire + wait the swapchain image Unity renders into next ---
	// Guard against a double-acquire: if a previous frame acquired an image but
	// never submitted (Unity may call PopulateNextFrameDesc without a matching
	// SubmitCurrentFrame on startup), reuse it rather than acquire again
	// (xrWaitSwapchainImage would return XR_ERROR_CALL_ORDER_INVALID).
	if (s_ps.swapchain_created && !s_ps.image_acquired) {
		uint32_t index = 0;
		XrSwapchainImageAcquireInfo ai = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
		if (XR_SUCCEEDED(s_ps.pfn_acquire_swapchain_image(s_ps.swapchain, &ai, &index))) {
			XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
			wi.timeout = 1000000000;
			s_ps.pfn_wait_swapchain_image(s_ps.swapchain, &wi);
			s_ps.image_acquired = 1;
			s_ps.acquired_index = index;
		}
	}
	if (out_image_index) *out_image_index = s_ps.acquired_index;

	if (out_should_render) *out_should_render = fs.shouldRender ? 1 : 0;
	return 1;
}

void dxr_prov_get_view(uint32_t view_index, DxrProvView *out_view)
{
	if (!out_view) return;
	if (view_index >= DXR_PROV_MAX_VIEWS) { memset(out_view, 0, sizeof(*out_view)); return; }
	*out_view = s_ps.views[view_index];
}

// View matrix (column-major, OpenXR convention) from an eye pose — replicates the
// hook path's dxr_view_matrix_from_pose so the provider publishes matrices in the
// EXACT convention the transparent overlay's cyclopean hit-test expects.
static void ps_view_matrix_from_pose(const float pos[3], const float q[4], float *out)
{
	const float qx = q[0], qy = q[1], qz = q[2], qw = q[3];
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
	out[12] = -(out[0] * pos[0] + out[4] * pos[1] + out[8] * pos[2]);
	out[13] = -(out[1] * pos[0] + out[5] * pos[1] + out[9] * pos[2]);
	out[14] = -(out[2] * pos[0] + out[6] * pos[1] + out[10] * pos[2]);
}

// Projection (column-major, GL clip) from an XrFovf — replicates dxr_projection_from_fov.
// fov[] = {angleLeft, angleRight, angleUp, angleDown} (the DxrProvView layout).
static void ps_projection_from_fov(const float fov[4], float nz, float fz, float *out)
{
	const float l = tanf(fov[0]) * nz;
	const float r = tanf(fov[1]) * nz;
	const float t = tanf(fov[2]) * nz;
	const float b = tanf(fov[3]) * nz;
	for (int i = 0; i < 16; i++) out[i] = 0.0f;
	out[0] = 2.0f * nz / (r - l);
	out[5] = 2.0f * nz / (t - b);
	out[8] = (r + l) / (r - l);
	out[9] = (t + b) / (t - b);
	out[10] = -(fz + nz) / (fz - nz);
	out[11] = -1.0f;
	out[14] = -2.0f * fz * nz / (fz - nz);
}

// Publish the per-eye view+proj to the hook's shared stereo-matrices state so the
// transparent overlay's cyclopean hit-test (DisplayXRTransparentOverlay, via
// displayxr_get_stereo_matrices) works in provider mode — DisplayXRFeature.
// GetStereoMatrices is inert without Unity's OpenXR loader. Same math as the hook.
static void ps_publish_stereo_matrices(void)
{
	if (s_ps.view_count < 2) return;
	DisplayXRStereoMatrices mats = {};
	float nz = s_ps.near_z > 0.0f ? s_ps.near_z : 0.01f;
	float fz = s_ps.far_z > nz ? s_ps.far_z : 1000.0f;
	ps_view_matrix_from_pose(s_ps.views[0].position, s_ps.views[0].orientation, mats.left_view);
	ps_view_matrix_from_pose(s_ps.views[1].position, s_ps.views[1].orientation, mats.right_view);
	ps_projection_from_fov(s_ps.views[0].fov, nz, fz, mats.left_projection);
	ps_projection_from_fov(s_ps.views[1].fov, nz, fz, mats.right_projection);
	mats.valid = 1;
	displayxr_state_set_stereo_matrices(&mats);
}

// Foreground clip (#166 Phase B / #57): z of (rigPose^-1 * eyeWorld) = the eye's
// signed distance to the display plane along the rig forward. Copied verbatim from
// the hook path (dxr_rig_local_eye_z) so the provider computes the SAME per-view far.
static float ps_rig_local_eye_z(const XrPosef *rig, const float eye[3])
{
	const float dx = eye[0] - rig->position.x;
	const float dy = eye[1] - rig->position.y;
	const float dz = eye[2] - rig->position.z;
	const float qx = -rig->orientation.x, qy = -rig->orientation.y;
	const float qz = -rig->orientation.z, qw = rig->orientation.w;
	const float cx = qy * dz - qz * dy + qw * dx;
	const float cy = qz * dx - qx * dz + qw * dy;
	return dz + 2.0f * (qx * cy - qy * cx);
}

// Compute the per-view foreground clip (far + Unity-space eye pos) for one eye world
// position `p`. Eye pos in Unity coords (oxr_to_unity_pos = x, y, -z) — matches the
// deviceAnchorToEyePose the provider hands Unity, aligning with the shader's
// UNITY_MATRIX_I_V eye position (_DXREyePosL/R). Far = display-plane distance
// (display rig = |rig-local eye Z|) or convergence distance (camera rig).
static void ps_compute_eye_clip(const float *p, float *out_far,
                                float *out_ex, float *out_ey, float *out_ez)
{
	if (out_ex) *out_ex = p[0];
	if (out_ey) *out_ey = p[1];
	if (out_ez) *out_ez = -p[2];
	float far_eff = s_ps.far_z > 0.0f ? s_ps.far_z : 1000.0f;
	float near_z = s_ps.near_z > 0.0f ? s_ps.near_z : 0.01f;
	if (s_ps.camera_centric) {
		if (s_ps.inv_convergence_distance > 1e-4f) far_eff = 1.0f / s_ps.inv_convergence_distance;
	} else {
		XrPosef rig = s_ps.display_pose_set ? s_ps.display_pose
		                                    : XrPosef{{0, 0, 0, 1}, {0, 0, 0}};
		float ez = fabsf(ps_rig_local_eye_z(&rig, p));
		if (ez > near_z + 0.001f) far_eff = ez;
	}
	if (out_far) *out_far = far_eff;
}

void dxr_prov_get_eye_clip(uint32_t eye, float *out_far,
                           float *out_ex, float *out_ey, float *out_ez)
{
	if (eye >= DXR_PROV_MAX_VIEWS) eye = 0;
	ps_compute_eye_clip(s_ps.views[eye].position, out_far, out_ex, out_ey, out_ez);
}

// Per-zone per-eye foreground clip (#166). Zone 0 = primary; i>=1 = extra_zones[i-1].
// Returns 1 on success. Lets the clip publisher diagnose / drive per-zone clip data.
int dxr_prov_get_zone_eye_clip(uint32_t zone, uint32_t eye, float *out_far,
                               float *out_ex, float *out_ey, float *out_ez)
{
	if (eye >= 2) eye = 0;
	const float *p;
	if (zone == 0) {
		if (s_ps.view_count < 2) return 0;
		p = s_ps.views[eye].position;
	} else {
		uint32_t ei = zone - 1;
		if (ei >= s_ps.extra_zone_count) return 0;
		ProviderExtraZone *z = &s_ps.extra_zones[ei];
		if (!z->valid || z->view_count < 2) return 0;
		p = z->views[eye].position;
	}
	ps_compute_eye_clip(p, out_far, out_ex, out_ey, out_ez);
	return 1;
}

// ============================================================================
// Per-zone stereo matrices + screen rect (#166 — multi-zone transparent mask).
// The transparent overlay's SetWindowRgn silhouette must cover EVERY zone's
// content, not just the primary — otherwise the OS clips non-primary zones to
// see-through. Expose each zone's cyclopean view+proj (same math as
// ps_publish_stereo_matrices) and its window-pixel rect so the overlay can
// render the silhouette per-zone into that zone's sub-rect and union the result.
// Zone 0 = primary (s_ps.views / primary zone rect); zone i>=1 = extra_zones[i-1].
// ============================================================================
uint32_t dxr_prov_get_zone_count(void)
{
	uint32_t n = 1; // primary always present
	for (uint32_t i = 0; i < s_ps.extra_zone_count; i++)
		if (s_ps.extra_zones[i].valid && s_ps.extra_zones[i].view_count >= 2) n++;
	return n;
}

int dxr_prov_get_zone_stereo_matrices(uint32_t zone, float *lv, float *lp,
                                      float *rv, float *rp)
{
	float nz = s_ps.near_z > 0.0f ? s_ps.near_z : 0.01f;
	float fz = s_ps.far_z > nz ? s_ps.far_z : 1000.0f;
	const DxrProvView *v;
	if (zone == 0) {
		if (s_ps.view_count < 2) return 0;
		v = s_ps.views;
	} else {
		uint32_t ei = zone - 1;
		if (ei >= s_ps.extra_zone_count) return 0;
		ProviderExtraZone *z = &s_ps.extra_zones[ei];
		if (!z->valid || z->view_count < 2) return 0;
		v = z->views;
	}
	if (lv) ps_view_matrix_from_pose(v[0].position, v[0].orientation, lv);
	if (rv) ps_view_matrix_from_pose(v[1].position, v[1].orientation, rv);
	if (lp) ps_projection_from_fov(v[0].fov, nz, fz, lp);
	if (rp) ps_projection_from_fov(v[1].fov, nz, fz, rp);
	return 1;
}

int dxr_prov_get_zone_rect_px(uint32_t zone, int *x, int *y, int *w, int *h)
{
	if (zone == 0) {
		if (ps_zone_active()) {
			if (x) *x = s_ps.zone_x; if (y) *y = s_ps.zone_y;
			if (w) *w = s_ps.zone_w; if (h) *h = s_ps.zone_h;
		} else {
			uint32_t ww = 0, wh = 0; ps_window_size(&ww, &wh);
			if (x) *x = 0; if (y) *y = 0;
			if (w) *w = (int)ww; if (h) *h = (int)wh;
		}
		return 1;
	}
	uint32_t ei = zone - 1;
	if (ei >= s_ps.extra_zone_count) return 0;
	ProviderExtraZone *z = &s_ps.extra_zones[ei];
	if (!z->valid) return 0;
	if (x) *x = z->rect_x; if (y) *y = z->rect_y;
	if (w) *w = z->rect_w; if (h) *h = z->rect_h;
	return 1;
}

int dxr_prov_submit_frame(uint32_t image_index)
{
	if (!s_ps.frame_begun || !s_ps.swapchain_created) {
		dxr_prov_end_frame_empty();
		return 0;
	}
	s_ps.frame_begun = 0;

	// Unity rendered the eyes into the shared BRIDGE on its device. Copy the bridge
	// into the acquired runtime swapchain image's two array slices on our own device,
	// fence-synced against Unity's render (shared fence below). SPI: one 2-slice array
	// bridge → both slices. MultiPass: two single-slice eye bridges → slices 0/1.
	int sp = dxr_prov_get_single_pass();
	ID3D12Resource *copy_src = sp ? s_ps.bridge_own : s_ps.bridge_own_eye[0];
	if (copy_src && image_index < s_ps.sc_image_count &&
	    s_ps.sc_images[image_index].texture && s_ps.own_cmd_list) {
		ID3D12Resource *dst = s_ps.sc_images[image_index].texture;
		s_ps.own_cmd_alloc->Reset();
		s_ps.own_cmd_list->Reset(s_ps.own_cmd_alloc, NULL);
		for (UINT slice = 0; slice < 2; slice++) {
			D3D12_TEXTURE_COPY_LOCATION dl = {};
			dl.pResource = dst;
			dl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			dl.SubresourceIndex = slice;
			D3D12_TEXTURE_COPY_LOCATION sl = {};
			// SPI: slice `slice` of the array bridge. MultiPass: subresource 0 of the
			// per-eye bridge for this eye.
			sl.pResource = sp ? s_ps.bridge_own : s_ps.bridge_own_eye[slice];
			sl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			sl.SubresourceIndex = sp ? slice : 0;
			s_ps.own_cmd_list->CopyTextureRegion(&dl, 0, 0, 0, &sl, NULL);
		}
		s_ps.own_cmd_list->Close();

		// Cross-device sync: signal a SHARED fence on Unity's queue (ordered after
		// Unity's already-submitted render into the bridge — Unity's queue is FIFO)
		// and GPU-wait on it on the own queue, so the copy reads the FINISHED render
		// (not empty texels → black).
		if (s_ps.shared_fence_unity && s_ps.shared_fence_own && s_ps.unity_queue) {
			s_ps.sync_val++;
			s_ps.unity_queue->Signal(s_ps.shared_fence_unity, s_ps.sync_val);
			s_ps.own_queue->Wait(s_ps.shared_fence_own, s_ps.sync_val);
		}

		ID3D12CommandList *lists[] = { s_ps.own_cmd_list };
		s_ps.own_queue->ExecuteCommandLists(1, lists);
		s_ps.own_fence_value++;
		s_ps.own_queue->Signal(s_ps.own_fence, s_ps.own_fence_value);
		if (s_ps.own_fence->GetCompletedValue() < s_ps.own_fence_value) {
			s_ps.own_fence->SetEventOnCompletion(s_ps.own_fence_value, s_ps.own_fence_event);
			WaitForSingleObject(s_ps.own_fence_event, INFINITE);
		}
	}

	if (s_ps.image_acquired) {
		XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
		s_ps.pfn_release_swapchain_image(s_ps.swapchain, &ri);
		s_ps.image_acquired = 0;
	}

	// Build a 2-view projection layer; eyes are array layers 0/1 (SPI).
	uint32_t n = s_ps.view_count >= 2 ? 2 : s_ps.view_count;
	if (n == 0) { dxr_prov_end_frame_empty(); return 0; }
	XrCompositionLayerProjectionView pv[DXR_PROV_MAX_VIEWS] = {};
	for (uint32_t eye = 0; eye < n; eye++) {
		pv[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
		pv[eye].pose.position = XrVector3f{ s_ps.views[eye].position[0],
		                                    s_ps.views[eye].position[1],
		                                    s_ps.views[eye].position[2] };
		pv[eye].pose.orientation = XrQuaternionf{ s_ps.views[eye].orientation[0],
		                                          s_ps.views[eye].orientation[1],
		                                          s_ps.views[eye].orientation[2],
		                                          s_ps.views[eye].orientation[3] };
		pv[eye].fov.angleLeft  = s_ps.views[eye].fov[0];
		pv[eye].fov.angleRight = s_ps.views[eye].fov[1];
		pv[eye].fov.angleUp    = s_ps.views[eye].fov[2];
		pv[eye].fov.angleDown  = s_ps.views[eye].fov[3];
		// SPI: each eye is array slice 0/1; imageRect is the active render
		// sub-region (window×scaleXY) within the worst-case-sized slice.
		uint32_t rw = s_ps.render_w ? s_ps.render_w : s_ps.sc_width;
		uint32_t rh = s_ps.render_h ? s_ps.render_h : s_ps.sc_height;
		pv[eye].subImage.swapchain = s_ps.swapchain;
		pv[eye].subImage.imageRect.offset = {0, 0};
		pv[eye].subImage.imageRect.extent = {(int32_t)rw, (int32_t)rh};
		pv[eye].subImage.imageArrayIndex = eye; // SPI: left=0, right=1
	}

	// Zones (#166 Phase B): bind the projection layer's views into the zone rect.
	// SAME instance/values as the locate chain; the submit values are authoritative.
	XrDisplayZoneEXT submit_zone = {XR_TYPE_DISPLAY_ZONE_EXT};
	int zone_frame = ps_zone_active();
	if (zone_frame) {
		submit_zone.zoneId = s_ps.zone_id ? s_ps.zone_id : 1;
		submit_zone.rect.offset = {s_ps.zone_x, s_ps.zone_y};
		submit_zone.rect.extent = {s_ps.zone_w, s_ps.zone_h};
	}

	XrCompositionLayerProjection layer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
	layer.next = zone_frame ? (const void *)&submit_zone : NULL;
	// Transparent (ALPHA_BLEND) session: the runtime alpha-gates the projection
	// layer against the environment (compose-under-bg desktop) using the texture
	// alpha, so regions transparent in ALL views become pure see-through and the
	// desktop fill only shows in de-occluded regions. Without this flag the layer
	// reads as opaque-over-environment and the composed-under desktop bleeds through
	// everywhere at partial alpha. Matches Unity's own projection layer
	// (XrCompositionLayerFlags.SourceAlpha) and the runtime's cube_zones handle test.
	// Unity's eye texture is premultiplied (SourceAlpha, no UNPREMULTIPLIED bit).
	layer.layerFlags = (s_ps.transparent_requested && s_ps.alpha_blend_supported)
	        ? XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT : 0;
	layer.space = s_ps.local_space;
	layer.viewCount = n;
	layer.views = pv;

	// Extra 3D zones (#166 Phase B2): copy each bridge → its swapchain + build a
	// zone-chained projection layer. Structs persist until xrEndFrame below.
	XrCompositionLayerProjectionView extra_pv[PS_MAX_ZONES - 1][2] = {};
	XrCompositionLayerProjection     extra_layer[PS_MAX_ZONES - 1] = {};
	XrDisplayZoneEXT                 extra_zone_struct[PS_MAX_ZONES - 1] = {};
	int extra_has[PS_MAX_ZONES - 1] = {};
	for (uint32_t i = 0; i < s_ps.extra_zone_count; i++) {
		ProviderExtraZone *z = &s_ps.extra_zones[i];
		if (!z->valid || z->view_count < 2 || !z->swapchain_created) continue;
		ps_copy_extra_zone(z);
		for (uint32_t eye = 0; eye < 2; eye++) {
			extra_pv[i][eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
			extra_pv[i][eye].pose.position = XrVector3f{ z->views[eye].position[0], z->views[eye].position[1], z->views[eye].position[2] };
			extra_pv[i][eye].pose.orientation = XrQuaternionf{ z->views[eye].orientation[0], z->views[eye].orientation[1], z->views[eye].orientation[2], z->views[eye].orientation[3] };
			extra_pv[i][eye].fov.angleLeft  = z->views[eye].fov[0];
			extra_pv[i][eye].fov.angleRight = z->views[eye].fov[1];
			extra_pv[i][eye].fov.angleUp    = z->views[eye].fov[2];
			extra_pv[i][eye].fov.angleDown  = z->views[eye].fov[3];
			extra_pv[i][eye].subImage.swapchain = z->swapchain;
			extra_pv[i][eye].subImage.imageRect.offset = {0, 0};
			extra_pv[i][eye].subImage.imageRect.extent = {(int32_t)z->sc_width, (int32_t)z->sc_height};
			extra_pv[i][eye].subImage.imageArrayIndex = eye;
		}
		extra_zone_struct[i].type = XR_TYPE_DISPLAY_ZONE_EXT;
		extra_zone_struct[i].zoneId = z->zone_id;
		extra_zone_struct[i].rect.offset = {z->rect_x, z->rect_y};
		extra_zone_struct[i].rect.extent = {z->rect_w, z->rect_h};
		extra_layer[i].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION;
		extra_layer[i].next = &extra_zone_struct[i];
		extra_layer[i].layerFlags = (s_ps.transparent_requested && s_ps.alpha_blend_supported)
		        ? XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT : 0;
		extra_layer[i].space = s_ps.local_space;
		extra_layer[i].viewCount = 2;
		extra_layer[i].views = extra_pv[i];
		extra_has[i] = 1;
	}

	// Local2D band(s): post-weave 2D content at a client-window pixel rect,
	// composited over the woven 3D (the "glass over 3D" 2D zone). Inactive when no
	// rect/bridge is registered.
	XrCompositionLayerLocal2DEXT l2d_layer = {};
	int has_l2d = ps_submit_local2d(&l2d_layer);

	// Window-space UI (HUD). Composites over everything (fractional coords + disparity).
	XrCompositionLayerWindowSpaceEXT wsui_layer = {};
	int has_wsui = ps_submit_wsui(&wsui_layer);

	// Order: 3D projections (primary + extra zones) under, Local2D bands over the 3D,
	// HUD on top (canonical zones rule — 1 zone projection per 3D zone + 1 Local2D per
	// 2D band, all ALPHA_BLEND).
	const XrCompositionLayerBaseHeader *layers[PS_MAX_ZONES + 2];
	uint32_t lc = 0;
	layers[lc++] = (const XrCompositionLayerBaseHeader *)&layer;
	for (uint32_t i = 0; i < s_ps.extra_zone_count; i++)
		if (extra_has[i]) layers[lc++] = (const XrCompositionLayerBaseHeader *)&extra_layer[i];
	if (has_l2d)  layers[lc++] = (const XrCompositionLayerBaseHeader *)&l2d_layer;
	if (has_wsui) layers[lc++] = (const XrCompositionLayerBaseHeader *)&wsui_layer;

	XrFrameEndInfo ei = {XR_TYPE_FRAME_END_INFO};
	ei.displayTime = s_ps.predicted_display_time;
	ei.environmentBlendMode = (s_ps.transparent_requested && s_ps.alpha_blend_supported)
	        ? XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND
	        : XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	ei.layerCount = lc;
	ei.layers = layers;
	XrResult r = s_ps.pfn_end_frame(s_ps.session, &ei);
	if (XR_FAILED(r)) { ps_log("[DisplayXR-PROV] xrEndFrame failed: %d\n", r); return 0; }
	return 1;
}

void dxr_prov_end_frame_empty(void)
{
	if (!s_ps.frame_begun) return;
	s_ps.frame_begun = 0;
	XrFrameEndInfo ei = {XR_TYPE_FRAME_END_INFO};
	ei.displayTime = s_ps.predicted_display_time;
	ei.environmentBlendMode = (s_ps.transparent_requested && s_ps.alpha_blend_supported)
	        ? XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND
	        : XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	ei.layerCount = 0;
	ei.layers = NULL;
	s_ps.pfn_end_frame(s_ps.session, &ei);
}

// ============================================================================
// Tunables / pose / queries
// ============================================================================

void dxr_prov_set_tunables(float ipd_factor, float parallax_factor,
                           float perspective_factor, float virtual_display_height,
                           float inv_convergence_distance, float fov_override,
                           float near_z, float far_z, int camera_centric)
{
	s_ps.ipd_factor = ipd_factor;
	s_ps.parallax_factor = parallax_factor;
	s_ps.perspective_factor = perspective_factor;
	s_ps.virtual_display_height = virtual_display_height;
	s_ps.inv_convergence_distance = inv_convergence_distance;
	s_ps.fov_override = fov_override;
	s_ps.near_z = near_z;            // owned by Unity's projection; stored, unused
	s_ps.far_z = far_z;
	s_ps.camera_centric = camera_centric;
	s_ps.tunables_set = 1;
}

void dxr_prov_set_display_pose(float px, float py, float pz,
                               float ox, float oy, float oz, float ow, int enabled)
{
	if (enabled) {
		// Unity (left-hand, +Z forward) → OpenXR (right-hand, -Z forward):
		// negate position Z, negate quaternion X/Y. Matches the standalone / hook.
		s_ps.display_pose.position = XrVector3f{px, py, -pz};
		s_ps.display_pose.orientation = XrQuaternionf{-ox, -oy, oz, ow};
		s_ps.display_pose_set = 1;
	} else {
		s_ps.display_pose_set = 0;
	}
}

// Expose the sent rig pose in the OpenXR frame (as stored). Returns 1 if a pose is
// set, 0 otherwise. Used by the render handoff to make deviceAnchorToEyePose
// rig-RELATIVE — the located views already bake this rig pose in, and Unity
// re-composes the same pose via the camera transform, so without subtracting it the
// rig origin is applied twice (drifts the URP foreground-clip plane; #166).
int dxr_prov_get_display_pose_oxr(float out_pos[3], float out_quat[4])
{
	if (!s_ps.display_pose_set) return 0;
	out_pos[0] = s_ps.display_pose.position.x;
	out_pos[1] = s_ps.display_pose.position.y;
	out_pos[2] = s_ps.display_pose.position.z;
	out_quat[0] = s_ps.display_pose.orientation.x;
	out_quat[1] = s_ps.display_pose.orientation.y;
	out_quat[2] = s_ps.display_pose.orientation.z;
	out_quat[3] = s_ps.display_pose.orientation.w;
	return 1;
}

void dxr_prov_get_display_info(DxrProvDisplayInfo *out_info)
{
	if (out_info) *out_info = s_ps.display_info;
}

int dxr_prov_has_view_rig(void) { return s_ps.has_view_rig; }

void dxr_prov_get_render_rect(uint32_t *out_w, uint32_t *out_h)
{
	if (out_w) *out_w = s_ps.render_w ? s_ps.render_w : s_ps.sc_width;
	if (out_h) *out_h = s_ps.render_h ? s_ps.render_h : s_ps.sc_height;
}

// ============================================================================
// Rendering modes (XR_EXT_display_info)
// ============================================================================

uint32_t dxr_prov_get_mode_count(void) { return s_ps.mode_count; }

int dxr_prov_get_mode_info(uint32_t index, DxrProvModeInfo *out_info)
{
	if (!out_info || index >= s_ps.mode_count) return 0;
	const XrDisplayRenderingModeInfoEXT *m = &s_ps.modes[index];
	out_info->mode_index = m->modeIndex;
	out_info->view_count = m->viewCount;
	out_info->tile_columns = m->tileColumns;
	out_info->tile_rows = m->tileRows;
	out_info->view_width_px = m->viewWidthPixels;
	out_info->view_height_px = m->viewHeightPixels;
	out_info->view_scale_x = m->viewScaleX;
	out_info->view_scale_y = m->viewScaleY;
	out_info->hardware_display_3d = (int)m->hardwareDisplay3D;
	out_info->is_active = (int)m->isActive;
	out_info->is_requestable = (int)m->isRequestable;
	strncpy(out_info->name, m->modeName, sizeof(out_info->name) - 1);
	out_info->name[sizeof(out_info->name) - 1] = '\0';
	return 1;
}

uint32_t dxr_prov_get_active_mode_index(void) { return s_ps.active_mode_index; }

int dxr_prov_request_rendering_mode(uint32_t mode_index)
{
	if (!s_ps.session || !s_ps.pfn_request_rendering_mode) return 0;
	XrResult r = s_ps.pfn_request_rendering_mode(s_ps.session, mode_index);
	ps_log("[DisplayXR-PROV] request rendering mode %u -> %d\n", mode_index, r);
	return XR_SUCCEEDED(r) ? 1 : 0;
}

int dxr_prov_request_display_mode(int mode3d)
{
	if (!s_ps.session || !s_ps.pfn_request_display_mode) return 0;
	XrResult r = s_ps.pfn_request_display_mode(s_ps.session,
	        mode3d ? XR_DISPLAY_MODE_3D_EXT : XR_DISPLAY_MODE_2D_EXT);
	ps_log("[DisplayXR-PROV] request display mode %s -> %d\n", mode3d ? "3D" : "2D", r);
	return XR_SUCCEEDED(r) ? 1 : 0;
}

int dxr_prov_set_eye_tracking_mode(int manual)
{
	if (!s_ps.session || !s_ps.pfn_request_eye_tracking_mode) return 0;
	XrResult r = s_ps.pfn_request_eye_tracking_mode(s_ps.session,
	        manual ? XR_EYE_TRACKING_MODE_MANUAL_EXT : XR_EYE_TRACKING_MODE_MANAGED_EXT);
	ps_log("[DisplayXR-PROV] request eye-tracking mode %s -> %d\n", manual ? "MANUAL" : "MANAGED", r);
	return XR_SUCCEEDED(r) ? 1 : 0;
}

// ---- Event latches (read-and-clear) -----------------------------------------

int dxr_prov_consume_mode_changed(uint32_t *prev_index, uint32_t *cur_index)
{
	if (!s_ps.ev_mode_changed) return 0;
	s_ps.ev_mode_changed = 0;
	if (prev_index) *prev_index = s_ps.ev_mode_prev;
	if (cur_index)  *cur_index = s_ps.ev_mode_cur;
	return 1;
}

int dxr_prov_consume_hw_state_changed(int *hw3d)
{
	if (!s_ps.ev_hw_changed) return 0;
	s_ps.ev_hw_changed = 0;
	if (hw3d) *hw3d = s_ps.ev_hw3d;
	return 1;
}

int dxr_prov_consume_eye_tracking_changed(int *is_tracking, int *active_mode)
{
	if (!s_ps.ev_track_changed) return 0;
	s_ps.ev_track_changed = 0;
	if (is_tracking) *is_tracking = s_ps.ev_track_is_tracking;
	if (active_mode) *active_mode = s_ps.ev_track_mode;
	return 1;
}
