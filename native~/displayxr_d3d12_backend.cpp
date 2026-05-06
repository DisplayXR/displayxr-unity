// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
//
// D3D12 graphics backend for the DisplayXR hook chain.

#if defined(_WIN32)

#include "displayxr_hooks_internal.h"
#include <d3d12.h>

// XR_KHR_D3D12_enable types — defined inline so this TU doesn't need
// XR_USE_GRAPHICS_API_D3D12 globally. Mirrors displayxr_standalone_internal.h.
#ifndef XR_TYPE_GRAPHICS_BINDING_D3D12_KHR
#define XR_TYPE_GRAPHICS_BINDING_D3D12_KHR ((XrStructureType)1000028000)
#endif
#ifndef XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR
#define XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR  ((XrStructureType)1000028001)
#endif

typedef struct WsuiXrGraphicsBindingD3D12KHR {
	XrStructureType type;
	const void *next;
	ID3D12Device *device;
	ID3D12CommandQueue *queue;
} WsuiXrGraphicsBindingD3D12KHR;

typedef struct WsuiXrSwapchainImageD3D12KHR {
	XrStructureType type;
	void *next;
	ID3D12Resource *texture;
} WsuiXrSwapchainImageD3D12KHR;

class D3D12Backend : public GraphicsBackend {
public:
	// Captured from session binding chain.
	ID3D12Device       *device       = nullptr;
	ID3D12CommandQueue *queue        = nullptr;

	// Lazy one-shot copy resources (issue #67).
	ID3D12CommandAllocator    *cmd_alloc  = nullptr;
	ID3D12GraphicsCommandList *cmd_list   = nullptr;
	ID3D12Fence               *fence      = nullptr;
	HANDLE                     fence_event = nullptr;
	UINT64                     fence_value = 0;

	bool ensure_copy_resources()
	{
		if (cmd_list) return true;
		if (!device) return false;
		if (FAILED(device->CreateCommandAllocator(
			D3D12_COMMAND_LIST_TYPE_DIRECT,
			__uuidof(ID3D12CommandAllocator), (void **)&cmd_alloc))) {
			displayxr_log("[DisplayXR] wsui D3D12: CreateCommandAllocator failed\n");
			return false;
		}
		if (FAILED(device->CreateCommandList(
			0, D3D12_COMMAND_LIST_TYPE_DIRECT, cmd_alloc, nullptr,
			__uuidof(ID3D12GraphicsCommandList), (void **)&cmd_list))) {
			displayxr_log("[DisplayXR] wsui D3D12: CreateCommandList failed\n");
			return false;
		}
		cmd_list->Close();
		if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
			__uuidof(ID3D12Fence), (void **)&fence))) {
			displayxr_log("[DisplayXR] wsui D3D12: CreateFence failed\n");
			return false;
		}
		fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		fence_value = 0;
		return true;
	}

	void release_copy_resources()
	{
		if (cmd_list)    { cmd_list->Release();    cmd_list  = nullptr; }
		if (cmd_alloc)   { cmd_alloc->Release();   cmd_alloc = nullptr; }
		if (fence)       { fence->Release();       fence     = nullptr; }
		if (fence_event) { CloseHandle(fence_event); fence_event = nullptr; }
		fence_value = 0;
	}

	void on_session_created(const XrSessionCreateInfo *createInfo) override
	{
		const XrBaseInStructure *item = (const XrBaseInStructure *)createInfo->next;
		while (item != nullptr) {
			if (item->type == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR) {
				const WsuiXrGraphicsBindingD3D12KHR *binding =
				    (const WsuiXrGraphicsBindingD3D12KHR *)item;
				device = binding->device;
				queue  = binding->queue;
				if (device) device->AddRef();
				if (queue)  queue->AddRef();
				displayxr_log("[DisplayXR] D3D12: captured device=%p queue=%p\n",
				    (void *)device, (void *)queue);
				break;
			}
			item = item->next;
		}
	}

	void on_session_destroyed() override
	{
		release_copy_resources();
		if (queue)  { queue->Release();  queue  = nullptr; }
		if (device) { device->Release(); device = nullptr; }
	}

	void on_destroy() override { on_session_destroyed(); }

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

	// --- Window-space UI overlay (issue #67) ---

	bool wsui_enumerate_swapchain_images(XrSwapchain sc, uint32_t capacity,
	                                      uint32_t *out_count, void *out_native_ptrs[]) override
	{
		if (s_real_enumerate_swapchain_images == nullptr) return false;
		uint32_t count = 0;
		if (XR_FAILED(s_real_enumerate_swapchain_images(sc, 0, &count, nullptr)) || count == 0)
			return false;
		if (count > capacity) count = capacity;

		WsuiXrSwapchainImageD3D12KHR images[16] = {};
		if (count > 16) count = 16;
		for (uint32_t i = 0; i < count; i++) {
			images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR;
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
	                                    uint32_t w, uint32_t h) override
	{
		ID3D12Resource *src = (ID3D12Resource *)unity_tex;
		ID3D12Resource *dst = (ID3D12Resource *)sc_image_native;
		if (!src || !dst || !queue || !ensure_copy_resources()) return false;

		// Rely on D3D12 common-state implicit promotion: any resource in
		// COMMON state implicitly promotes to COPY_SOURCE / COPY_DEST during
		// CopyTextureRegion and decays back to COMMON when the command list
		// drains. Unity's RT and the runtime's swapchain image are both
		// expected to land in COMMON between frames; the standalone D3D12
		// backend's blit_atlas (displayxr_standalone_d3d12.cpp:317) follows
		// the same convention.
		cmd_alloc->Reset();
		cmd_list->Reset(cmd_alloc, nullptr);

		D3D12_TEXTURE_COPY_LOCATION dst_loc = {};
		dst_loc.pResource = dst;
		dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		dst_loc.SubresourceIndex = 0;

		D3D12_TEXTURE_COPY_LOCATION src_loc = {};
		src_loc.pResource = src;
		src_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		src_loc.SubresourceIndex = 0;

		// Clamp the box to actual dimensions.
		D3D12_RESOURCE_DESC src_desc = src->GetDesc();
		D3D12_RESOURCE_DESC dst_desc = dst->GetDesc();
		uint32_t copy_w = w;
		if ((uint32_t)src_desc.Width  < copy_w) copy_w = (uint32_t)src_desc.Width;
		if ((uint32_t)dst_desc.Width  < copy_w) copy_w = (uint32_t)dst_desc.Width;
		uint32_t copy_h = h;
		if (src_desc.Height < copy_h) copy_h = src_desc.Height;
		if (dst_desc.Height < copy_h) copy_h = dst_desc.Height;
		D3D12_BOX box = { 0, 0, 0, copy_w, copy_h, 1 };

		cmd_list->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, &box);
		cmd_list->Close();

		ID3D12CommandList *lists[] = { cmd_list };
		queue->ExecuteCommandLists(1, lists);

		fence_value++;
		queue->Signal(fence, fence_value);
		if (fence->GetCompletedValue() < fence_value) {
			fence->SetEventOnCompletion(fence_value, fence_event);
			WaitForSingleObject(fence_event, INFINITE);
		}
		return true;
	}
};

GraphicsBackend *create_d3d12_backend() { return new D3D12Backend(); }

#endif // defined(_WIN32)
