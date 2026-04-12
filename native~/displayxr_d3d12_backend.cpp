// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
//
// D3D12 graphics backend for the DisplayXR hook chain.
// Extracted from displayxr_hooks.cpp — no logic changes.

#if defined(_WIN32)

#include "displayxr_hooks_internal.h"

class D3D12Backend : public GraphicsBackend {
public:
	void on_session_created(const XrSessionCreateInfo *) override {}
	void on_session_destroyed() override {}
	void on_destroy() override {}
	void inject_session_binding(XrBaseOutStructure *last, DisplayXRState *state) override
		{ win32_inject_window_binding(last, state); }
	void on_swapchain_created(XrSession, const XrSwapchainCreateInfo *, XrSwapchain) override {}
	bool handle_enumerate_swapchain_images(XrSwapchain, uint32_t, uint32_t *, XrSwapchainImageBaseHeader *, XrResult *) override { return false; }
	bool handle_acquire_swapchain_image(XrSwapchain, const XrSwapchainImageAcquireInfo *, uint32_t *, XrResult *) override { return false; }
	bool handle_wait_swapchain_image(XrSwapchain, const XrSwapchainImageWaitInfo *, XrResult *) override { return false; }
	bool handle_release_swapchain_image(XrSwapchain, const XrSwapchainImageReleaseInfo *, XrResult *) override { return false; }
	void prepare_end_frame(XrSession, const XrFrameEndInfo *, void *, int *npatch_out) override { *npatch_out = 0; }
	void restore_end_frame(void *, int) override {}
	void *create_shared_texture(uint32_t, uint32_t) override { return nullptr; }
	void  destroy_shared_texture() override {}
	void *get_shared_texture_native_ptr() override { return nullptr; }
};

GraphicsBackend *create_d3d12_backend() { return new D3D12Backend(); }

#endif // defined(_WIN32)
