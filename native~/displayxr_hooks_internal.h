// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
//
// Internal header shared between displayxr_hooks.cpp and the platform backend
// translation units. Not part of the public plugin API.

#pragma once

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

// --- D3D11 swapchain image struct (from openxr_platform.h) ---
// Defined inline to avoid requiring XR_USE_GRAPHICS_API_D3D11 globally.
#if defined(_WIN32)
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

class GraphicsBackend {
public:
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
};

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
