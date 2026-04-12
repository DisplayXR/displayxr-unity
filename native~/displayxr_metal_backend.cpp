// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
//
// Metal graphics backend for the DisplayXR hook chain.
// Extracted from displayxr_hooks.cpp — no logic changes.

#if defined(__APPLE__)

#include "displayxr_hooks_internal.h"

class MetalBackend : public GraphicsBackend {
public:
	void on_session_created(const XrSessionCreateInfo *) override {}
	void on_session_destroyed() override {}
	void on_destroy() override {}
	void inject_session_binding(XrBaseOutStructure *last, DisplayXRState *state) override
	{
		static XrCocoaWindowBindingCreateInfoEXT mac_binding = {};
		mac_binding.type = XR_TYPE_COCOA_WINDOW_BINDING_CREATE_INFO_EXT;
		mac_binding.next = nullptr;
		mac_binding.viewHandle = state->window_handle;
		mac_binding.readbackCallback = displayxr_readback_callback;
		mac_binding.readbackUserdata = nullptr;
		mac_binding.sharedIOSurface = state->shared_iosurface;

		displayxr_log( "[DisplayXR] Injecting cocoa window binding: viewHandle=%p, sharedIOSurface=%p\n",
		        mac_binding.viewHandle, mac_binding.sharedIOSurface);

		last->next = (XrBaseOutStructure *)&mac_binding;
	}
	void on_swapchain_created(XrSession, const XrSwapchainCreateInfo *, XrSwapchain) override {}
	bool handle_enumerate_swapchain_images(XrSwapchain, uint32_t, uint32_t *, XrSwapchainImageBaseHeader *, XrResult *) override { return false; }
	bool handle_acquire_swapchain_image(XrSwapchain, const XrSwapchainImageAcquireInfo *, uint32_t *, XrResult *) override { return false; }
	bool handle_wait_swapchain_image(XrSwapchain, const XrSwapchainImageWaitInfo *, XrResult *) override { return false; }
	bool handle_release_swapchain_image(XrSwapchain, const XrSwapchainImageReleaseInfo *, XrResult *) override { return false; }
	void prepare_end_frame(XrSession, const XrFrameEndInfo *, void *, int *npatch_out) override { *npatch_out = 0; }
	void restore_end_frame(void *, int) override {}
	void *create_shared_texture(uint32_t width, uint32_t height) override
	{
		if (displayxr_metal_create_shared_surface(width, height)) {
			return displayxr_metal_get_texture();
		}
		return nullptr;
	}
	void destroy_shared_texture() override
	{
		displayxr_metal_destroy_shared_surface();
		DisplayXRState *state = displayxr_get_state();
		state->shared_iosurface = nullptr;
	}
	void *get_shared_texture_native_ptr() override
	{
		return displayxr_metal_get_texture();
	}
};

GraphicsBackend *create_metal_backend() { return new MetalBackend(); }

#endif // defined(__APPLE__)
