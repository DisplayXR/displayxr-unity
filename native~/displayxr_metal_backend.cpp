// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
//
// Metal graphics backend for the DisplayXR hook chain.

#if defined(__APPLE__)

#include "displayxr_hooks_internal.h"

// XR_KHR_metal_enable types — defined inline so this TU doesn't need
// XR_USE_GRAPHICS_API_METAL globally. Mirrors the standalone copy in
// displayxr_standalone_internal.h.
#define XR_TYPE_GRAPHICS_BINDING_METAL_KHR ((XrStructureType)1000029000)
#define XR_TYPE_SWAPCHAIN_IMAGE_METAL_KHR  ((XrStructureType)1000029001)

typedef struct WsuiXrGraphicsBindingMetalKHR {
	XrStructureType type;
	const void *next;
	void *commandQueue; // id<MTLCommandQueue>
} WsuiXrGraphicsBindingMetalKHR;

typedef struct WsuiXrSwapchainImageMetalKHR {
	XrStructureType type;
	void *next;
	void *texture; // id<MTLTexture>
} WsuiXrSwapchainImageMetalKHR;

class MetalBackend : public GraphicsBackend {
public:
	void *command_queue = nullptr; // id<MTLCommandQueue> — captured from session binding chain.

	void on_session_created(const XrSessionCreateInfo *createInfo) override
	{
		// Capture MTLCommandQueue so wsui can blit Unity's RenderTexture into
		// the overlay swapchain image (issue #67).
		const XrBaseInStructure *item = (const XrBaseInStructure *)createInfo->next;
		while (item != nullptr) {
			if (item->type == XR_TYPE_GRAPHICS_BINDING_METAL_KHR) {
				const WsuiXrGraphicsBindingMetalKHR *binding =
				    (const WsuiXrGraphicsBindingMetalKHR *)item;
				command_queue = binding->commandQueue;
				displayxr_log("[DisplayXR] Metal: captured commandQueue=%p\n",
				    command_queue);
				break;
			}
			item = item->next;
		}
	}
	void on_session_destroyed() override { command_queue = nullptr; }
	void on_destroy() override { command_queue = nullptr; }
	void inject_session_binding(XrBaseOutStructure *last, DisplayXRState *state) override
	{
		static XrCocoaWindowBindingCreateInfoEXT mac_binding = {};
		mac_binding.type = XR_TYPE_COCOA_WINDOW_BINDING_CREATE_INFO_EXT;
		mac_binding.next = nullptr;
		mac_binding.viewHandle = state->window_handle;
		mac_binding.readbackCallback = displayxr_readback_callback;
		mac_binding.readbackUserdata = nullptr;
		mac_binding.sharedIOSurface = nullptr;

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
		(void)width; (void)height;
		return nullptr;
	}
	void destroy_shared_texture() override {}
	void *get_shared_texture_native_ptr() override { return nullptr; }

	// --- Window-space UI overlay (issue #67) ---

	bool wsui_enumerate_swapchain_images(XrSwapchain sc, uint32_t capacity,
	                                      uint32_t *out_count, void *out_native_ptrs[]) override
	{
		if (s_real_enumerate_swapchain_images == nullptr) return false;
		uint32_t count = 0;
		if (XR_FAILED(s_real_enumerate_swapchain_images(sc, 0, &count, nullptr)) || count == 0)
			return false;
		if (count > capacity) count = capacity;

		WsuiXrSwapchainImageMetalKHR images[16] = {};
		if (count > 16) count = 16;
		for (uint32_t i = 0; i < count; i++) {
			images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_METAL_KHR;
		}
		if (XR_FAILED(s_real_enumerate_swapchain_images(
			sc, count, &count, (XrSwapchainImageBaseHeader *)images))) {
			return false;
		}
		for (uint32_t i = 0; i < count; i++) {
			out_native_ptrs[i] = images[i].texture;
		}
		*out_count = count;
		return true;
	}

	bool wsui_copy_to_swapchain_image(void *unity_tex, void *sc_image_native,
	                                    uint32_t /*w*/, uint32_t /*h*/) override
	{
		if (command_queue == nullptr) {
			static int warned = 0;
			if (!warned) {
				warned = 1;
				displayxr_log("[DisplayXR] wsui Metal: no command queue captured "
				    "— cannot blit\n");
			}
			return false;
		}
		return displayxr_metal_blit_textures(command_queue, unity_tex, sc_image_native) != 0;
	}
};

GraphicsBackend *create_metal_backend() { return new MetalBackend(); }

#endif // defined(__APPLE__)
