// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Internal header shared between displayxr_hooks.cpp and the platform backend
// translation units. Not part of the public plugin API.

#pragma once

#include "displayxr_hooks.h"
#include "displayxr_extensions.h"
#include "displayxr_shared_state.h"
#include "displayxr_readback.h"
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

// --- Logging helper (non-static so backends in separate TUs can call it) ---
void displayxr_log(const char *fmt, ...);

// --- Graphics binding type constants (inlined to avoid platform-header requirements) ---
#if defined(_WIN32)
#ifndef XR_TYPE_GRAPHICS_BINDING_D3D11_KHR
#define XR_TYPE_GRAPHICS_BINDING_D3D11_KHR      ((XrStructureType)1000027000)
#endif
#ifndef XR_TYPE_GRAPHICS_BINDING_D3D12_KHR
#define XR_TYPE_GRAPHICS_BINDING_D3D12_KHR      ((XrStructureType)1000028000)
#endif
#endif
#ifndef XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR
#define XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR     ((XrStructureType)1000025000)
#endif

// --- D3D11 swapchain image struct (from openxr_platform.h) ---
// Defined inline to avoid requiring XR_USE_GRAPHICS_API_D3D11 globally.
// Guarded so it doesn't collide with the matching definition in
// displayxr_standalone_internal.h when wsui_window_space_ui.cpp includes both.
#if defined(_WIN32) && !defined(DISPLAYXR_HAS_D3D11_SWAPCHAIN_IMAGE)
#define DISPLAYXR_HAS_D3D11_SWAPCHAIN_IMAGE 1
typedef struct XrSwapchainImageD3D11KHR {
	XrStructureType type;
	void *next;
	ID3D11Texture2D *texture;
} XrSwapchainImageD3D11KHR;
#endif

// --- Stored real function pointers (extern — defined in displayxr_hooks.cpp) ---
extern PFN_xrGetInstanceProcAddr s_next_gipa;
extern PFN_xrLocateViews s_real_locate_views;
extern PFN_xrGetSystemProperties s_real_get_system_properties;
extern PFN_xrCreateSession s_real_create_session;
extern PFN_xrDestroySession s_real_destroy_session;
extern PFN_xrEndFrame s_real_end_frame;
extern PFN_xrCreateReferenceSpace s_real_create_reference_space;
extern volatile PFN_xrPollEvent s_real_poll_event;
extern PFN_xrDestroyInstance s_real_destroy_instance;
extern PFN_xrEnumerateSwapchainFormats s_real_enumerate_swapchain_formats;
extern PFN_xrCreateSwapchain s_real_create_swapchain;
extern PFN_xrEnumerateSwapchainImages s_real_enumerate_swapchain_images;
extern PFN_xrAcquireSwapchainImage s_real_acquire_swapchain_image;
extern PFN_xrWaitSwapchainImage s_real_wait_swapchain_image;
extern PFN_xrReleaseSwapchainImage s_real_release_swapchain_image;
#if defined(_WIN32)
extern PFN_xrDestroySwapchain s_real_destroy_swapchain;
#endif

// --- D3D11 typed swapchain substitution struct (Win32 only) ---
#if defined(_WIN32)
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
#endif

// ============================================================================
// GraphicsBackend abstract base class
// ============================================================================

#ifndef DISPLAYXR_MAX_RENDERING_MODES
#define DISPLAYXR_MAX_RENDERING_MODES 16
#endif

class GraphicsBackend {
public:
	// Rendering-mode extension state — generic XR data, shared across backends.
	// Populated by on_session_ready() (called from the hooks dispatcher after
	// xrCreateSession succeeds). set_rendering_mode_fns() must be called first
	// to wire up the extension function pointers.
	PFN_xrEnumerateDisplayRenderingModesEXT real_enumerate_rendering_modes = nullptr;
	PFN_xrRequestDisplayRenderingModeEXT    real_request_rendering_mode    = nullptr;
	XrDisplayRenderingModeInfoEXT rendering_modes[DISPLAYXR_MAX_RENDERING_MODES] = {};
	uint32_t rendering_mode_count         = 0;
	uint32_t current_rendering_mode_index = 0;

	virtual ~GraphicsBackend() = default;
	virtual void on_session_created(const XrSessionCreateInfo *createInfo) = 0;
	virtual void on_session_destroyed() = 0;
	virtual void on_destroy() = 0;
	virtual void inject_session_binding(XrBaseOutStructure *last, DisplayXRState *state) = 0;
	virtual void on_swapchain_created(XrSession session, const XrSwapchainCreateInfo *createInfo, XrSwapchain unity_sc) = 0;
	virtual bool handle_enumerate_swapchain_images(XrSwapchain swapchain, uint32_t imageCapacityInput, uint32_t *imageCountOutput, XrSwapchainImageBaseHeader *images, XrResult *result_out) = 0;
	virtual bool handle_acquire_swapchain_image(XrSwapchain swapchain, const XrSwapchainImageAcquireInfo *acquireInfo, uint32_t *index, XrResult *result_out) = 0;
	virtual bool handle_wait_swapchain_image(XrSwapchain swapchain, const XrSwapchainImageWaitInfo *waitInfo, XrResult *result_out) = 0;
	virtual bool handle_release_swapchain_image(XrSwapchain swapchain, const XrSwapchainImageReleaseInfo *releaseInfo, XrResult *result_out) = 0;
	virtual void prepare_end_frame(XrSession session, const XrFrameEndInfo *frameEndInfo, void *patches_out, int *npatch_out) = 0;
	virtual void restore_end_frame(void *patches, int npatch) = 0;
	virtual void *create_shared_texture(uint32_t width, uint32_t height) = 0;
	virtual void  destroy_shared_texture() = 0;
	virtual void *get_shared_texture_native_ptr() = 0;

	// Rendering-mode extension wiring. Default implementation lives on the
	// base class so all backends inherit it; D3D11 overrides on_session_ready
	// to additionally trigger atlas swapchain setup that depends on the mode.
	virtual void set_rendering_mode_fns(PFN_xrEnumerateDisplayRenderingModesEXT enum_fn,
	                                     PFN_xrRequestDisplayRenderingModeEXT    req_fn)
	{
		real_enumerate_rendering_modes = enum_fn;
		real_request_rendering_mode    = req_fn;
	}
	virtual void on_session_ready(XrSession session)
	{
		rendering_mode_count = 0;
		if (real_enumerate_rendering_modes == nullptr) return;
		uint32_t total = 0;
		XrResult er = real_enumerate_rendering_modes(session, 0, &total, nullptr);
		if (XR_FAILED(er) || total == 0) return;
		if (total > DISPLAYXR_MAX_RENDERING_MODES) total = DISPLAYXR_MAX_RENDERING_MODES;
		for (uint32_t i = 0; i < total; i++) {
			rendering_modes[i] = {};
			rendering_modes[i].type = XR_TYPE_DISPLAY_RENDERING_MODE_INFO_EXT;
		}
		er = real_enumerate_rendering_modes(session, total, &total, rendering_modes);
		if (XR_FAILED(er)) return;
		rendering_mode_count = total;

		// Find currently-active mode (v13 `isActive` field) and the first
		// hardware_display_3d candidate.
		uint32_t active_idx = 0;
		bool active_is_3d = false;
		uint32_t first_3d_idx = 0;
		bool found_3d = false;
		for (uint32_t i = 0; i < total; i++) {
			if (rendering_modes[i].isActive) {
				active_idx = rendering_modes[i].modeIndex;
				active_is_3d = rendering_modes[i].hardwareDisplay3D;
			}
			if (!found_3d && rendering_modes[i].hardwareDisplay3D &&
			    rendering_modes[i].isRequestable) {
				first_3d_idx = rendering_modes[i].modeIndex;
				found_3d = true;
			}
		}

		// If the runtime started us in 2D (mode 0 is the default since
		// DisplayXR/displayxr-runtime#112), switch to the first 3D mode so
		// the DP weaves instead of force-passing-through. Matches what the
		// standalone preview does when a rig camera is selected. Respect
		// any 3D mode the user pre-selected (e.g. SIM_DISPLAY_OUTPUT env
		// var, workspace override) — only intervene when active is 2D.
		if (!active_is_3d && found_3d && real_request_rendering_mode != nullptr) {
			XrResult r = real_request_rendering_mode(session, first_3d_idx);
			if (XR_SUCCEEDED(r)) {
				current_rendering_mode_index = first_3d_idx;
			} else {
				current_rendering_mode_index = active_idx;
			}
		} else {
			current_rendering_mode_index = active_idx;
		}
	}
	virtual XrResult request_rendering_mode(XrSession session, uint32_t modeIndex)
	{
		if (real_request_rendering_mode == nullptr) return XR_ERROR_FUNCTION_UNSUPPORTED;
		XrResult r = real_request_rendering_mode(session, modeIndex);
		if (XR_SUCCEEDED(r)) current_rendering_mode_index = modeIndex;
		return r;
	}

	// Window-space UI overlay (issue #67).
	// Enumerate the named swapchain's images using the platform-appropriate
	// XrSwapchainImage*KHR struct, extract the native texture pointer
	// (id<MTLTexture>, ID3D11Texture2D*, ID3D12Resource*) for each image,
	// and store up to `capacity` of them in `out_native_ptrs`. Writes the
	// actual count to *out_count. Returns true on success.
	virtual bool wsui_enumerate_swapchain_images(XrSwapchain, uint32_t /*capacity*/,
	    uint32_t * /*out_count*/, void * /*out_native_ptrs*/[]) { return false; }

	// Copy the Unity-side native texture (backend-typed) into the named
	// swapchain image (also backend-typed). w/h are the source/dest extent.
	// Returns true on success.
	virtual bool wsui_copy_to_swapchain_image(void * /*unity_tex*/,
	    void * /*sc_image_native*/, uint32_t /*w*/, uint32_t /*h*/) { return false; }

	// Query the backend-native format of a texture (Unity's wsui canvas RT).
	// Returns the OpenXR-spec format code (DXGI_FORMAT on D3D, VkFormat on
	// Vulkan, MTLPixelFormat on Metal, GLenum on GL), or -1 if unknown /
	// unsupported. Used to align the wsui swapchain format with Unity's
	// actual RT format — a mismatch makes D3D12 CopyTextureRegion invalid,
	// which a DComp swapchain detects and kills the device over (whereas
	// opaque flip-model swapchains silently tolerate the byte permutation).
	// See displayxr-unity#82 / displayxr-runtime#216.
	virtual int64_t wsui_get_native_texture_format(void * /*unity_tex*/) { return -1; }

	// --- 2D surround (issue #131, hooked path) ---
	// Update the app-supplied 2D surround texture from a Unity RenderTexture and
	// prepare it for registration via xrSetSharedTextureSurround2DFenceEXT.
	// On first call (or when w/h change) allocates a SHARED RGBA8 surround
	// texture + a SHARED ID3D12Fence on the runtime/Unity device; each call
	// copies unity_rt into the surround texture on the runtime queue and signals
	// the fence (monotonic). Returns the NT handles + the just-signaled fence
	// value via out_* so the caller can register. Runs on the xrEndFrame render
	// thread (same context as wsui_copy_to_swapchain_image). Returns true if
	// out_* are filled. Default: unsupported (non-D3D12 backends).
	virtual bool surround_update(void * /*unity_rt*/, uint32_t /*w*/, uint32_t /*h*/,
	    void ** /*out_tex_handle*/, void ** /*out_fence_handle*/,
	    uint64_t * /*out_value*/) { return false; }
	// Release surround texture/fence/handles. Safe to call repeatedly.
	virtual void surround_release() {}
};

// Internal accessor for the currently-active hooked backend. Returns nullptr
// if no hooked session is running (e.g. inside the standalone preview path,
// or before xrCreateSession). Used by the standalone C ABI's rendering-mode
// shims so they can fall back to the hooked backend's enumerated modes when
// no standalone session exists (built apps).
GraphicsBackend *displayxr_get_hooked_backend();

// Currently-active hooked session handle, or XR_NULL_HANDLE. Pairs with
// displayxr_get_hooked_backend() for the fallback path.
XrSession displayxr_get_hooked_session();

// --- Factory functions for concrete backend classes ---
// Implementations are in displayxr_d3d11_backend.cpp, displayxr_d3d12_backend.cpp,
// and displayxr_metal_backend.cpp respectively.
#if defined(_WIN32)
GraphicsBackend *create_d3d11_backend();
GraphicsBackend *create_d3d12_backend();
#elif defined(__APPLE__)
GraphicsBackend *create_metal_backend();
#endif

// Vulkan (Win32 opt-in, Android/Linux default)
#if defined(ENABLE_VULKAN) || defined(__ANDROID__) || (defined(__linux__) && !defined(__ANDROID__) && !defined(__APPLE__))
GraphicsBackend *create_vulkan_backend();
#endif

// OpenGL desktop (non-Android, ENABLE_OPENGL)
#if defined(ENABLE_OPENGL) && !defined(__ANDROID__)
GraphicsBackend *create_opengl_backend();
#endif

// OpenGL ES (Android, ENABLE_OPENGL)
#if defined(ENABLE_OPENGL) && defined(__ANDROID__)
GraphicsBackend *create_opengles_backend();
#endif

// --- Win32 helper used by both D3D11Backend and D3D12Backend ---
#if defined(_WIN32)
void win32_inject_window_binding(XrBaseOutStructure *last, DisplayXRState *state);

// Patch record used by D3D11Backend::prepare_end_frame / restore_end_frame
// and by hooked_xrEndFrame in displayxr_hooks.cpp.
struct EFPatch {
	XrCompositionLayerProjectionView *view;
	XrSwapchain orig_sc;
	XrRect2Di orig_rect;
};
#endif
