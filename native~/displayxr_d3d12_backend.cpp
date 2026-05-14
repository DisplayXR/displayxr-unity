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

	int64_t wsui_get_native_texture_format(void *unity_tex) override
	{
		ID3D12Resource *res = (ID3D12Resource *)unity_tex;
		if (res == nullptr) return -1;
		return (int64_t)res->GetDesc().Format;
	}

	bool wsui_copy_to_swapchain_image(void *unity_tex, void *sc_image_native,
	                                    uint32_t w, uint32_t h) override
	{
		ID3D12Resource *src = (ID3D12Resource *)unity_tex;
		ID3D12Resource *dst = (ID3D12Resource *)sc_image_native;
		if (!src || !dst || !queue || !ensure_copy_resources()) return false;

		// Explicit resource state transitions around CopyTextureRegion.
		//
		// Previously this relied on D3D12 common-state implicit promotion
		// (COMMON -> COPY_SOURCE / COPY_DEST during the copy, decay back
		// to COMMON when the command list drains). That contract holds
		// for opaque sessions, where the runtime composites the wsui
		// swapchain image as PIXEL_SHADER_RESOURCE and lets it decay to
		// COMMON before the next acquire.
		//
		// In transparent sessions (DComp swapchain on D3D12, alpha-native
		// compose-under-bg + alpha-gate DP path), the runtime's compositor
		// can leave the swapchain image in a non-COMMON state between
		// frames, breaking implicit promotion. The result was a SEGV
		// inside CopyTextureRegion on the second xrEndFrame after the
		// wsui swapchain was created — see issue #82.
		//
		// Explicit transitions make the copy state-independent: the
		// resource is forced into the copy state regardless of what the
		// runtime left it in, then put back into COMMON before we
		// release the swapchain image. Same convention as the standalone
		// D3D12 backend's blit_atlas (displayxr_standalone_d3d12.cpp).
		cmd_alloc->Reset();
		cmd_list->Reset(cmd_alloc, nullptr);

		// dst (runtime's wsui swapchain image): created by the runtime in
		// D3D12_RESOURCE_STATE_RENDER_TARGET (see displayxr-runtime's
		// comp_d3d12_swapchain.cpp:284) and expected back in RENDER_TARGET
		// for the next render_window_space_layer barrier in the runtime's
		// renderer. Match that contract exactly.
		// src (Unity's RT): Unity manages, expected to be in COMMON between
		// frames per OpenXR spec convention.
		D3D12_RESOURCE_BARRIER pre_barriers[2] = {};
		pre_barriers[0].Type  = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		pre_barriers[0].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
		pre_barriers[0].Transition.pResource   = dst;
		pre_barriers[0].Transition.Subresource = 0;
		pre_barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
		pre_barriers[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
		pre_barriers[1].Type  = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		pre_barriers[1].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
		pre_barriers[1].Transition.pResource   = src;
		pre_barriers[1].Transition.Subresource = 0;
		pre_barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
		pre_barriers[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
		cmd_list->ResourceBarrier(2, pre_barriers);

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

		D3D12_RESOURCE_BARRIER post_barriers[2] = {};
		post_barriers[0].Type  = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		post_barriers[0].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
		post_barriers[0].Transition.pResource   = dst;
		post_barriers[0].Transition.Subresource = 0;
		post_barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
		post_barriers[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
		post_barriers[1].Type  = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		post_barriers[1].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
		post_barriers[1].Transition.pResource   = src;
		post_barriers[1].Transition.Subresource = 0;
		post_barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
		post_barriers[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_COMMON;
		cmd_list->ResourceBarrier(2, post_barriers);

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
