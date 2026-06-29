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
	XrTime predicted_display_time;

	int has_view_rig;

	// Unity D3D12 device/queue (we bind the session to these — zero-copy).
	ID3D12Device       *unity_device;
	ID3D12CommandQueue *unity_queue;
	void               *overlay_hwnd;

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

	// Per-frame located views (render-ready)
	DxrProvView views[DXR_PROV_MAX_VIEWS];
	uint32_t    view_count;

	// Tunables / pose pushed from the rig
	float ipd_factor, parallax_factor, perspective_factor, virtual_display_height;
	int   tunables_set;
	XrPosef display_pose;
	int     display_pose_set;

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
	PFN_xrEnumerateDisplayRenderingModesEXT pfn_enumerate_modes; // optional
} ProviderSession;

static ProviderSession s_ps;
static DxrProvLogCallback s_log_cb = NULL;

void dxr_prov_set_log_callback(DxrProvLogCallback cb) { s_log_cb = cb; }

static void ps_log(const char *fmt, ...)
{
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	fputs(buf, stderr);
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
	// Optional EXT — soft-resolve (mode enumeration is M2; OK if absent in M1).
	{
		PFN_xrVoidFunction _fn = NULL;
		s_ps.gipa(s_ps.instance, "xrEnumerateDisplayRenderingModesEXT", &_fn);
		s_ps.pfn_enumerate_modes = (PFN_xrEnumerateDisplayRenderingModesEXT)_fn;
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
	for (uint32_t i = 0; i < s_ps.mode_count; i++)
		ps_log("[DisplayXR-PROV] mode[%u] '%s' views=%u %ux%u hw3d=%d\n",
		       i, s_ps.modes[i].modeName, s_ps.modes[i].viewCount,
		       s_ps.modes[i].tileColumns, s_ps.modes[i].tileRows,
		       (int)s_ps.modes[i].hardwareDisplay3D);
}

// ============================================================================
// SPI swapchain (arraySize=2). Images live on Unity's D3D12 device (zero-copy).
// ============================================================================

static int ps_create_swapchain(void)
{
	if (s_ps.swapchain_created) return 1;
	if (!s_ps.display_info.is_valid) return 0;

	uint32_t w = s_ps.display_info.pixel_width;
	uint32_t h = s_ps.display_info.pixel_height;
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
	return 1;
}

// ============================================================================
// Lifecycle
// ============================================================================

int dxr_prov_session_start(const char *runtime_json_path,
                           void *unity_d3d12_device,
                           void *unity_d3d12_queue,
                           void *overlay_hwnd)
{
	memset(&s_ps, 0, sizeof(s_ps));
	s_ps.unity_device = (ID3D12Device *)unity_d3d12_device;
	s_ps.unity_queue  = (ID3D12CommandQueue *)unity_d3d12_queue;
	s_ps.overlay_hwnd = overlay_hwnd;
	s_ps.ipd_factor = s_ps.parallax_factor = s_ps.perspective_factor = 1.0f;

	if (!s_ps.unity_device || !s_ps.unity_queue) {
		ps_log("[DisplayXR-PROV] start: missing Unity D3D12 device/queue\n");
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
					for (uint32_t i = 0; i < avail; i++)
						if (strcmp(props[i].extensionName, XR_EXT_VIEW_RIG_EXTENSION_NAME) == 0)
							{ s_ps.has_view_rig = 1; break; }
				}
				free(props);
			}
		}
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

	// --- D3D12 graphics requirements (spec requires the call before xrCreateSession).
	//     We honor Unity's device regardless of the returned LUID (validation point). ---
	{
		PFN_xrVoidFunction fn = NULL;
		s_ps.gipa(s_ps.instance, "xrGetD3D12GraphicsRequirementsKHR", &fn);
		if (fn) {
			XrGraphicsRequirementsD3D12KHR req = {XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR};
			((PFN_xrGetD3D12GraphicsRequirementsKHR)fn)(s_ps.instance, s_ps.system_id, &req);
			ps_log("[DisplayXR-PROV] D3D12 req LUID=%08lx%08lx (using Unity's device)\n",
			       (unsigned long)req.adapterLuid.HighPart, (unsigned long)req.adapterLuid.LowPart);
		}
	}

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

	// --- Session: D3D12 binding on UNITY'S device + window binding (overlay HWND) ---
	XrWin32WindowBindingCreateInfoEXT win_binding = {};
	win_binding.type = XR_TYPE_WIN32_WINDOW_BINDING_CREATE_INFO_EXT;
	win_binding.windowHandle = s_ps.overlay_hwnd;
	win_binding.readbackCallback = NULL;
	win_binding.readbackUserdata = NULL;
	win_binding.sharedTextureHandle = NULL;

	XrGraphicsBindingD3D12KHR d3d12 = {};
	d3d12.type = XR_TYPE_GRAPHICS_BINDING_D3D12_KHR;
	d3d12.device = s_ps.unity_device;
	d3d12.queue = s_ps.unity_queue;
	d3d12.next = &win_binding;

	XrSessionCreateInfo sci = {XR_TYPE_SESSION_CREATE_INFO};
	sci.next = &d3d12;
	sci.systemId = s_ps.system_id;
	if (XR_FAILED(s_ps.pfn_create_session(s_ps.instance, &sci, &s_ps.session))) {
		ps_log("[DisplayXR-PROV] xrCreateSession failed\n"); dxr_prov_session_stop(); return 0;
	}
	ps_log("[DisplayXR-PROV] Session created (Unity D3D12 device, overlay HWND=%p)\n", s_ps.overlay_hwnd);

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
	if (s_ps.swapchain && s_ps.pfn_destroy_swapchain) s_ps.pfn_destroy_swapchain(s_ps.swapchain);
	if (s_ps.session && s_ps.session_ready && s_ps.pfn_end_session) s_ps.pfn_end_session(s_ps.session);
	if (s_ps.session && s_ps.pfn_destroy_session) s_ps.pfn_destroy_session(s_ps.session);
	if (s_ps.instance && s_ps.pfn_destroy_instance) s_ps.pfn_destroy_instance(s_ps.instance);
	// Intentionally do NOT FreeLibrary(runtime_lib): background threads may still
	// reference it (matches the standalone's deliberate leak).
	XrInstance keep_lib_alive = s_ps.instance;
	(void)keep_lib_alive;
	void *lib = s_ps.runtime_lib;
	memset(&s_ps, 0, sizeof(s_ps));
	s_ps.runtime_lib = lib;
}

int dxr_prov_session_is_running(void) { return s_ps.running; }

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

void *dxr_prov_get_swapchain_image(uint32_t index)
{
	if (index >= s_ps.sc_image_count) return NULL;
	return s_ps.sc_images[index].texture;
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

		XrDisplayRigEXT rig = {XR_TYPE_DISPLAY_RIG_EXT};
		XrViewDisplayRawEXT raw = {XR_TYPE_VIEW_DISPLAY_RAW_EXT};
		XrPosef rig_pose = s_ps.display_pose_set ? s_ps.display_pose
		                                         : XrPosef{{0, 0, 0, 1}, {0, 0, 0}};
		if (s_ps.has_view_rig) {
			rig.pose = rig_pose;
			rig.virtualDisplayHeight = s_ps.tunables_set ? s_ps.virtual_display_height : 0.0f;
			rig.ipdFactor = s_ps.tunables_set ? s_ps.ipd_factor : 1.0f;
			rig.parallaxFactor = s_ps.tunables_set ? s_ps.parallax_factor : 1.0f;
			rig.perspectiveFactor = s_ps.tunables_set ? s_ps.perspective_factor : 1.0f;
			li.next = &rig;
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

	// --- Acquire + wait the swapchain image Unity renders into next ---
	uint32_t index = 0;
	XrSwapchainImageAcquireInfo ai = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
	if (s_ps.swapchain_created &&
	    XR_SUCCEEDED(s_ps.pfn_acquire_swapchain_image(s_ps.swapchain, &ai, &index))) {
		XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
		wi.timeout = 1000000000;
		s_ps.pfn_wait_swapchain_image(s_ps.swapchain, &wi);
		if (out_image_index) *out_image_index = index;
	}

	if (out_should_render) *out_should_render = fs.shouldRender ? 1 : 0;
	return 1;
}

void dxr_prov_get_view(uint32_t view_index, DxrProvView *out_view)
{
	if (!out_view) return;
	if (view_index >= DXR_PROV_MAX_VIEWS) { memset(out_view, 0, sizeof(*out_view)); return; }
	*out_view = s_ps.views[view_index];
}

int dxr_prov_submit_frame(uint32_t image_index)
{
	if (!s_ps.frame_begun || !s_ps.swapchain_created) {
		dxr_prov_end_frame_empty();
		return 0;
	}
	s_ps.frame_begun = 0;
	(void)image_index;

	// Unity already rendered both eyes into the acquired image's two array slices.
	XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
	s_ps.pfn_release_swapchain_image(s_ps.swapchain, &ri);

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
		pv[eye].subImage.swapchain = s_ps.swapchain;
		pv[eye].subImage.imageRect.offset = {0, 0};
		pv[eye].subImage.imageRect.extent = {(int32_t)s_ps.sc_width, (int32_t)s_ps.sc_height};
		pv[eye].subImage.imageArrayIndex = eye; // SPI: left=0, right=1
	}

	XrCompositionLayerProjection layer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
	layer.space = s_ps.local_space;
	layer.viewCount = n;
	layer.views = pv;
	const XrCompositionLayerBaseHeader *layers[1] = {
		(const XrCompositionLayerBaseHeader *)&layer
	};

	XrFrameEndInfo ei = {XR_TYPE_FRAME_END_INFO};
	ei.displayTime = s_ps.predicted_display_time;
	ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	ei.layerCount = 1;
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
	ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	ei.layerCount = 0;
	ei.layers = NULL;
	s_ps.pfn_end_frame(s_ps.session, &ei);
}

// ============================================================================
// Tunables / pose / queries
// ============================================================================

void dxr_prov_set_tunables(float ipd_factor, float parallax_factor,
                           float perspective_factor, float virtual_display_height)
{
	s_ps.ipd_factor = ipd_factor;
	s_ps.parallax_factor = parallax_factor;
	s_ps.perspective_factor = perspective_factor;
	s_ps.virtual_display_height = virtual_display_height;
	s_ps.tunables_set = 1;
}

void dxr_prov_set_display_pose(float px, float py, float pz,
                               float ox, float oy, float oz, float ow, int enabled)
{
	s_ps.display_pose.position = XrVector3f{px, py, pz};
	s_ps.display_pose.orientation = XrQuaternionf{ox, oy, oz, ow};
	s_ps.display_pose_set = enabled ? 1 : 0;
}

void dxr_prov_get_display_info(DxrProvDisplayInfo *out_info)
{
	if (out_info) *out_info = s_ps.display_info;
}

int dxr_prov_has_view_rig(void) { return s_ps.has_view_rig; }
