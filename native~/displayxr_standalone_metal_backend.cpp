// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Metal graphics backend for the standalone OpenXR session.
// Extracted from displayxr_standalone.cpp — no logic changes.
//

#if defined(__APPLE__)

#include "displayxr_standalone_internal.h"

class StandaloneMetalBackend : public StandaloneGraphicsBackend {
private:
	XrSwapchainImageMetalKHR images[SA_MAX_SWAPCHAIN_IMAGES] = {};

public:
	bool create_device(XrInstance instance, XrSystemId system_id, PFN_xrGetInstanceProcAddr gipa) override
	{
		PFN_xrVoidFunction fn_req = NULL;
		gipa(instance, "xrGetMetalGraphicsRequirementsKHR", &fn_req);
		if (fn_req) {
			XrGraphicsRequirementsMetalKHR req = {};
			req.type = XR_TYPE_GRAPHICS_REQUIREMENTS_METAL_KHR;
			XrResult result = ((PFN_xrGetMetalGraphicsRequirementsKHR)fn_req)(
				instance, system_id, &req);
			if (XR_FAILED(result)) {
				fprintf(stderr, "[DisplayXR-SA] xrGetMetalGraphicsRequirementsKHR failed: %d\n", result);
				return false;
			}
			fprintf(stderr, "[DisplayXR-SA] Metal graphics requirements satisfied\n");
		} else {
			fprintf(stderr, "[DisplayXR-SA] Warning: xrGetMetalGraphicsRequirementsKHR not found\n");
		}
		return true;
	}

	bool create_shared_texture(uint32_t width, uint32_t height) override
	{
		(void)width; (void)height;
		return true;
	}

	void destroy_shared_texture() override {}

	void *get_shared_texture_native_ptr() override { return nullptr; }

	const void *build_session_binding(void *platform_window_handle, void *shared_texture_handle) override
	{
		(void)platform_window_handle;
		(void)shared_texture_handle;
		return nullptr;
	}

	bool enumerate_atlas_images(XrSwapchain swapchain, PFN_xrEnumerateSwapchainImages pfn, uint32_t count) override
	{
		for (uint32_t i = 0; i < count; i++) {
			images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_METAL_KHR;
			images[i].next = NULL;
		}
		XrResult result = pfn(swapchain, count, &count,
			(XrSwapchainImageBaseHeader *)images);
		if (XR_FAILED(result)) {
			fprintf(stderr, "[DisplayXR-SA] xrEnumerateSwapchainImages (atlas) failed: %d\n", result);
			return false;
		}
		return true;
	}

	void *get_atlas_image(uint32_t index) override
	{
		return images[index].texture;
	}

	void create_atlas_bridge(uint32_t atlas_w, uint32_t atlas_h, void *unity_device) override
	{
		(void)atlas_w; (void)atlas_h; (void)unity_device;
	}

	void destroy_atlas_bridge() override {}

	void *get_atlas_bridge_unity_ptr() override { return nullptr; }

	void blit_atlas(void *atlas_tex, uint32_t index) override
	{
		void *dst = images[index].texture;
		if (atlas_tex && dst) {
			displayxr_sa_metal_blit(atlas_tex, dst);
		}
	}

	bool fw_create_swapchain(void *hwnd, uint32_t w, uint32_t h) override
	{
		(void)hwnd; (void)w; (void)h;
		return true;
	}

	void fw_destroy_swapchain() override {}

	void fw_resize_swapchain_buffers(uint32_t w, uint32_t h) override
	{
		(void)w; (void)h;
	}

	void fw_present(uint32_t sc_w, uint32_t sc_h) override
	{
		(void)sc_w; (void)sc_h;
	}

	void destroy() override {}

	// --- Window-space UI overlay (issue #67) ---

	bool wsui_enumerate_swapchain_images(XrSwapchain sc,
	                                      PFN_xrEnumerateSwapchainImages pfn_enum,
	                                      uint32_t capacity,
	                                      uint32_t *out_count,
	                                      void *out_native_ptrs[]) override
	{
		uint32_t count = 0;
		if (XR_FAILED(pfn_enum(sc, 0, &count, NULL)) || count == 0) return false;
		if (count > capacity) count = capacity;
		XrSwapchainImageMetalKHR imgs[16] = {};
		if (count > 16) count = 16;
		for (uint32_t i = 0; i < count; i++) {
			imgs[i].type = XR_TYPE_SWAPCHAIN_IMAGE_METAL_KHR;
		}
		if (XR_FAILED(pfn_enum(sc, count, &count, (XrSwapchainImageBaseHeader *)imgs))) {
			return false;
		}
		for (uint32_t i = 0; i < count; i++) {
			out_native_ptrs[i] = imgs[i].texture;
		}
		*out_count = count;
		return true;
	}

	bool wsui_copy_to_swapchain_image(void *unity_tex, void *sc_image_native,
	                                    uint32_t /*w*/, uint32_t /*h*/) override
	{
		if (!unity_tex || !sc_image_native) return false;
		// Reuse the standalone Metal blit helper — it uses s_sa_queue, which
		// belongs to the standalone session (the right device for this path).
		return displayxr_sa_metal_blit(unity_tex, sc_image_native) != 0;
	}
};

StandaloneGraphicsBackend *create_standalone_metal_backend() { return new StandaloneMetalBackend(); }

#endif // defined(__APPLE__)
