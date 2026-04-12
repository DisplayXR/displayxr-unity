// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
//
// Internal header shared between displayxr_standalone.cpp and the platform
// backend translation units. Not part of the public plugin API.

#pragma once

#include "displayxr_standalone.h"
#include "displayxr_extensions.h"
#include "displayxr_shared_state.h"
#include "display3d_view.h"
#include "camera3d_view.h"

#include <openxr/openxr.h>

#if defined(__APPLE__)
#include "displayxr_standalone_metal.h"
#elif defined(_WIN32)
#include <windows.h>
#include <d3d12.h>
#include <d3d11.h>
#include <dxgi1_4.h>

// D3D12 OpenXR structs (inlined to avoid requiring XR_USE_GRAPHICS_API_D3D12).
#define XR_TYPE_GRAPHICS_BINDING_D3D12_KHR      ((XrStructureType)1000028000)
#define XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR       ((XrStructureType)1000028001)
#define XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR ((XrStructureType)1000028002)

typedef struct XrSwapchainImageD3D12KHR {
	XrStructureType type;
	void *next;
	ID3D12Resource *texture;
} XrSwapchainImageD3D12KHR;

typedef struct XrGraphicsBindingD3D12KHR {
	XrStructureType type;
	const void *next;
	ID3D12Device *device;
	ID3D12CommandQueue *queue;
} XrGraphicsBindingD3D12KHR;

typedef struct XrGraphicsRequirementsD3D12KHR {
	XrStructureType type;
	void *next;
	LUID adapterLuid;
	D3D_FEATURE_LEVEL minFeatureLevel;
} XrGraphicsRequirementsD3D12KHR;

// D3D11 OpenXR structs (inlined to avoid requiring XR_USE_GRAPHICS_API_D3D11).
#define XR_TYPE_GRAPHICS_BINDING_D3D11_KHR      ((XrStructureType)1000027000)
#define XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR       ((XrStructureType)1000027001)
#define XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR ((XrStructureType)1000027002)

typedef struct XrSwapchainImageD3D11KHR {
	XrStructureType type;
	void *next;
	ID3D11Texture2D *texture;
} XrSwapchainImageD3D11KHR;

typedef struct XrGraphicsBindingD3D11KHR {
	XrStructureType type;
	const void *next;
	ID3D11Device *device;
} XrGraphicsBindingD3D11KHR;

typedef struct XrGraphicsRequirementsD3D11KHR {
	XrStructureType type;
	void *next;
	LUID adapterLuid;
	D3D_FEATURE_LEVEL minFeatureLevel;
} XrGraphicsRequirementsD3D11KHR;
#endif

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#include <dlfcn.h>
#endif

// ============================================================================
// XR_KHR_metal_enable types (from openxr_platform.h, defined inline)
// ============================================================================

#define XR_TYPE_GRAPHICS_BINDING_METAL_KHR ((XrStructureType)1000029000)
#define XR_TYPE_SWAPCHAIN_IMAGE_METAL_KHR  ((XrStructureType)1000029001)
#define XR_TYPE_GRAPHICS_REQUIREMENTS_METAL_KHR ((XrStructureType)1000029002)
#define XR_KHR_METAL_ENABLE_EXTENSION_NAME "XR_KHR_metal_enable"

typedef struct XrGraphicsBindingMetalKHR {
	XrStructureType type;
	const void *next;
	void *commandQueue;
} XrGraphicsBindingMetalKHR;

typedef struct XrGraphicsRequirementsMetalKHR {
	XrStructureType type;
	void *next;
	void *metalDevice;
} XrGraphicsRequirementsMetalKHR;

typedef struct XrSwapchainImageMetalKHR {
	XrStructureType type;
	void *next;
	void *texture; // id<MTLTexture>
} XrSwapchainImageMetalKHR;

typedef XrResult (XRAPI_PTR *PFN_xrGetMetalGraphicsRequirementsKHR)(
    XrInstance instance, XrSystemId systemId,
    XrGraphicsRequirementsMetalKHR *graphicsRequirements);

// ============================================================================
// Size constants
// ============================================================================

#define SA_MAX_SWAPCHAIN_IMAGES 4
#define SA_MAX_RENDERING_MODES 16
#define SA_MAX_VIEWS 32

// ============================================================================
// SASwapchain — swapchain handle with metadata
// ============================================================================

typedef struct SASwapchain {
	XrSwapchain handle;
#if defined(__APPLE__)
	XrSwapchainImageMetalKHR images[SA_MAX_SWAPCHAIN_IMAGES];
#elif defined(_WIN32)
	XrSwapchainImageD3D12KHR images[SA_MAX_SWAPCHAIN_IMAGES];
	XrSwapchainImageD3D11KHR images_d3d11[SA_MAX_SWAPCHAIN_IMAGES];
#endif
	uint32_t image_count;
	uint32_t width;
	uint32_t height;
	int64_t format; // selected swapchain format (for typed RTV creation)
} SASwapchain;

// ============================================================================
// StandaloneGraphicsBackend abstract base class
// ============================================================================

class StandaloneGraphicsBackend {
public:
	virtual ~StandaloneGraphicsBackend() = default;
	virtual bool create_device(XrInstance instance, XrSystemId system_id, PFN_xrGetInstanceProcAddr gipa) = 0;
	virtual bool create_shared_texture(uint32_t width, uint32_t height) = 0;
	virtual void destroy_shared_texture() = 0;
	virtual void *get_shared_texture_native_ptr() = 0;
	virtual const void *build_session_binding(void *platform_window_handle, void *shared_texture_handle) = 0;
	virtual bool enumerate_atlas_images(XrSwapchain swapchain, PFN_xrEnumerateSwapchainImages pfn, uint32_t count) = 0;
	virtual void *get_atlas_image(uint32_t index) = 0;
	virtual void create_atlas_bridge(uint32_t atlas_w, uint32_t atlas_h, void *unity_device) = 0;
	virtual void destroy_atlas_bridge() = 0;
	virtual void *get_atlas_bridge_unity_ptr() = 0;
	virtual void blit_atlas(void *atlas_tex, uint32_t index) = 0;
	virtual bool fw_create_swapchain(void *hwnd, uint32_t w, uint32_t h) = 0;
	virtual void fw_destroy_swapchain() = 0;
	virtual void fw_resize_swapchain_buffers(uint32_t w, uint32_t h) = 0;
	virtual void fw_present(uint32_t sc_w, uint32_t sc_h) = 0;
	virtual void destroy() = 0;

	// Accessor methods used during session binding assembly in standalone_start.
	// These allow standalone.cpp to retrieve graphics objects without knowing
	// the concrete type (avoids the need for static_cast to backends).
	virtual void set_unity_device(void *dev) { (void)dev; }
	virtual void *get_graphics_device() { return nullptr; }   // D3D device (ID3D11Device* or ID3D12Device*)
	virtual void *get_graphics_queue() { return nullptr; }    // D3D12 command queue (nullptr for D3D11)
	virtual void *get_shared_handle() { return nullptr; }     // DXGI shared handle
};

// Factory functions (defined in the backend .cpp files)
#if defined(_WIN32)
StandaloneGraphicsBackend *create_standalone_d3d11_backend();
StandaloneGraphicsBackend *create_standalone_d3d12_backend();
#elif defined(__APPLE__)
StandaloneGraphicsBackend *create_standalone_metal_backend();
#endif

// Vulkan (Win32 opt-in, Android/Linux default)
#if defined(ENABLE_VULKAN) || defined(__ANDROID__) || (defined(__linux__) && !defined(__ANDROID__) && !defined(__APPLE__))
StandaloneGraphicsBackend *create_standalone_vulkan_backend();
#endif

// OpenGL desktop (non-Android, ENABLE_OPENGL)
#if defined(ENABLE_OPENGL) && !defined(__ANDROID__)
StandaloneGraphicsBackend *create_standalone_opengl_backend();
#endif

// OpenGL ES (Android, ENABLE_OPENGL)
#if defined(ENABLE_OPENGL) && defined(__ANDROID__)
StandaloneGraphicsBackend *create_standalone_opengles_backend();
#endif
