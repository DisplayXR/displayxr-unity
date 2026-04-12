// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
//
// D3D12 graphics backend for the standalone OpenXR session.
// Extracted from displayxr_standalone.cpp — no logic changes.
//
// === SCAFFOLD STATE (Phase 1 checkpoint) ===
// Verbatim copy of a089149:native~/displayxr_standalone_d3d12.cpp. NOT
// listed in CMake SOURCES yet. The big-cut session must:
//   1. Diff against current main's displayxr_standalone.cpp D3D12 paths.
//      The D3D12 atlas bridge (commits a7c91dc, 2a84927) is the most
//      sensitive code on main — both halves (device capture in
//      displayxr_standalone_start + cross-device blit in
//      displayxr_standalone_submit_frame_atlas) must land TOGETHER in this
//      file or the split is broken (see plan's Failure Modes section).
//   2. Merge any atlas bridge / fullscreen-window logic superset main has
//      that the template doesn't (Windows preview HWND-driven resize paths).
//   3. Add to CMake SOURCES and remove duplicated code from
//      displayxr_standalone.cpp.

#if defined(_WIN32)

#include "displayxr_standalone_internal.h"

class StandaloneD3D12Backend : public StandaloneGraphicsBackend {
public:
	// D3D12 path (when Unity uses Direct3D 12)
	ID3D12Device                 *d3d12_device         = nullptr; // Our own device (for runtime session)
	ID3D12CommandQueue           *d3d12_queue           = nullptr;
	ID3D12CommandAllocator       *d3d12_cmd_alloc       = nullptr;
	ID3D12GraphicsCommandList    *d3d12_cmd_list        = nullptr;
	ID3D12Fence                  *d3d12_fence           = nullptr;
	HANDLE                        d3d12_fence_event      = nullptr;
	UINT64                        d3d12_fence_value      = 0;
	ID3D12Resource               *d3d12_shared_texture  = nullptr; // On our device
	HANDLE                        d3d12_shared_handle    = nullptr; // DXGI shared handle (cross-device)
	ID3D12Resource               *d3d12_unity_shared     = nullptr; // Same texture opened on Unity's device
	// D3D12 atlas bridge: shared texture for cross-device atlas blit
	ID3D12Resource               *d3d12_atlas_bridge        = nullptr; // On our device, SHARED
	HANDLE                        d3d12_atlas_bridge_handle  = nullptr; // DXGI shared handle
	ID3D12Resource               *d3d12_unity_atlas_bridge   = nullptr; // Opened on Unity's device

	// Fullscreen window resources (D3D12)
	IDXGISwapChain3              *fw_swapchain   = nullptr;
	ID3D12Resource               *fw_bb[2]       = { nullptr, nullptr };
	ID3D12CommandAllocator       *fw_cmd_alloc   = nullptr;
	ID3D12GraphicsCommandList    *fw_cmd_list    = nullptr;
	ID3D12Fence                  *fw_fence       = nullptr;
	HANDLE                        fw_fence_event  = nullptr;
	uint64_t                      fw_fence_value  = 0;

	// Swapchain images
	XrSwapchainImageD3D12KHR      images[SA_MAX_SWAPCHAIN_IMAGES] = {};

	// Unity device (set before start)
	ID3D12Device                 *unity_device   = nullptr;

	bool create_device(XrInstance instance, XrSystemId system_id, PFN_xrGetInstanceProcAddr gipa) override
	{
		PFN_xrVoidFunction fn_req = NULL;
		gipa(instance, "xrGetD3D12GraphicsRequirementsKHR", &fn_req);

		LUID adapter_luid = {};
		if (fn_req) {
			XrGraphicsRequirementsD3D12KHR req = {};
			req.type = XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR;
			XrResult result = ((XrResult(XRAPI_CALL *)(XrInstance, XrSystemId, void *))fn_req)(
				instance, system_id, &req);
			if (XR_FAILED(result)) {
				fprintf(stderr, "[DisplayXR-SA] xrGetD3D12GraphicsRequirementsKHR failed: %d\n", result);
				return false;
			}
			adapter_luid = req.adapterLuid;
			fprintf(stderr, "[DisplayXR-SA] D3D12 requirements: adapter LUID=%08lx-%08lx, minFeatureLevel=0x%x\n",
			        adapter_luid.HighPart, adapter_luid.LowPart, (unsigned)req.minFeatureLevel);
		}

		// Always create our own D3D12 device for the runtime session.
		// Sharing Unity's device caused device removal — the runtime's compositor
		// and Unity's renderer conflict when operating on the same device.
		// Cross-device texture sharing is done via DXGI shared handles.
		{
			IDXGIFactory4 *factory = NULL;
			CreateDXGIFactory2(0, __uuidof(IDXGIFactory4), (void **)&factory);
			IDXGIAdapter1 *adapter = NULL;
			if (factory && (adapter_luid.HighPart != 0 || adapter_luid.LowPart != 0)) {
				HRESULT hr2 = factory->EnumAdapterByLuid(adapter_luid, __uuidof(IDXGIAdapter1), (void **)&adapter);
				if (SUCCEEDED(hr2)) {
					DXGI_ADAPTER_DESC1 desc;
					adapter->GetDesc1(&desc);
					fprintf(stderr, "[DisplayXR-SA] Found matching adapter: %ls\n", desc.Description);
				}
			}
			HRESULT hr = D3D12CreateDevice(
				adapter, D3D_FEATURE_LEVEL_11_0,
				__uuidof(ID3D12Device), (void **)&d3d12_device);
			if (adapter) adapter->Release();
			if (factory) factory->Release();
			if (FAILED(hr) || !d3d12_device) {
				fprintf(stderr, "[DisplayXR-SA] D3D12CreateDevice failed: 0x%08lx\n", hr);
				return false;
			}
			fprintf(stderr, "[DisplayXR-SA] Created own D3D12 device for runtime session\n");
		}

		// Create command queue on the device
		D3D12_COMMAND_QUEUE_DESC qd = {};
		qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
		HRESULT hr = d3d12_device->CreateCommandQueue(
			&qd, __uuidof(ID3D12CommandQueue), (void **)&d3d12_queue);
		if (FAILED(hr)) {
			fprintf(stderr, "[DisplayXR-SA] CreateCommandQueue failed: 0x%08lx\n", hr);
			return false;
		}

		// Create command allocator + command list for atlas blit
		d3d12_device->CreateCommandAllocator(
			D3D12_COMMAND_LIST_TYPE_DIRECT,
			__uuidof(ID3D12CommandAllocator), (void **)&d3d12_cmd_alloc);
		d3d12_device->CreateCommandList(
			0, D3D12_COMMAND_LIST_TYPE_DIRECT, d3d12_cmd_alloc, NULL,
			__uuidof(ID3D12GraphicsCommandList), (void **)&d3d12_cmd_list);
		d3d12_cmd_list->Close(); // Start closed

		// Create fence for GPU sync
		d3d12_device->CreateFence(
			0, D3D12_FENCE_FLAG_NONE,
			__uuidof(ID3D12Fence), (void **)&d3d12_fence);
		d3d12_fence_event = CreateEvent(NULL, FALSE, FALSE, NULL);
		d3d12_fence_value = 0;

		fprintf(stderr, "[DisplayXR-SA] D3D12 command queue + blit resources created\n");
		return true;
	}

	bool create_shared_texture(uint32_t width, uint32_t height) override
	{
		D3D12_HEAP_PROPERTIES heap = {};
		heap.Type = D3D12_HEAP_TYPE_DEFAULT;

		D3D12_RESOURCE_DESC td = {};
		td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		td.Width  = width;
		td.Height = height;
		td.DepthOrArraySize = 1;
		td.MipLevels = 1;
		td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		td.SampleDesc.Count = 1;
		td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

		HRESULT hr = d3d12_device->CreateCommittedResource(
			&heap, D3D12_HEAP_FLAG_SHARED, &td,
			D3D12_RESOURCE_STATE_COMMON, NULL,
			__uuidof(ID3D12Resource), (void **)&d3d12_shared_texture);
		if (FAILED(hr)) {
			fprintf(stderr, "[DisplayXR-SA] CreateCommittedResource (shared) failed: 0x%08lx\n", hr);
			return false;
		}

		hr = d3d12_device->CreateSharedHandle(
			d3d12_shared_texture, NULL, GENERIC_ALL, NULL,
			&d3d12_shared_handle);
		if (FAILED(hr)) {
			fprintf(stderr, "[DisplayXR-SA] CreateSharedHandle failed: 0x%08lx\n", hr);
			return false;
		}

		fprintf(stderr, "[DisplayXR-SA] D3D12 shared texture: %ux%u, handle=%p\n",
		        (unsigned)td.Width, td.Height, d3d12_shared_handle);

		// Open the shared texture on Unity's device so C# can wrap it
		// with CreateExternalTexture (needs an ID3D12Resource* on Unity's device).
		if (unity_device && d3d12_shared_handle) {
			hr = unity_device->OpenSharedHandle(
				d3d12_shared_handle,
				__uuidof(ID3D12Resource), (void **)&d3d12_unity_shared);
			if (SUCCEEDED(hr) && d3d12_unity_shared) {
				fprintf(stderr, "[DisplayXR-SA] Opened shared texture on Unity device: %p\n",
				        (void *)d3d12_unity_shared);
			} else {
				fprintf(stderr, "[DisplayXR-SA] OpenSharedHandle on Unity device failed: 0x%08lx\n", hr);
			}
		}
		return true;
	}

	void destroy_shared_texture() override
	{
		if (d3d12_unity_shared)   { d3d12_unity_shared->Release();              d3d12_unity_shared   = nullptr; }
		if (d3d12_fence_event)    { CloseHandle(d3d12_fence_event);              d3d12_fence_event    = nullptr; }
		if (d3d12_fence)          { d3d12_fence->Release();                      d3d12_fence          = nullptr; }
		if (d3d12_cmd_list)       { d3d12_cmd_list->Release();                   d3d12_cmd_list       = nullptr; }
		if (d3d12_cmd_alloc)      { d3d12_cmd_alloc->Release();                  d3d12_cmd_alloc      = nullptr; }
		if (d3d12_shared_texture) { d3d12_shared_texture->Release();             d3d12_shared_texture = nullptr; }
		if (d3d12_shared_handle)  { CloseHandle(d3d12_shared_handle);            d3d12_shared_handle  = nullptr; }
		if (d3d12_queue)          { d3d12_queue->Release();                      d3d12_queue          = nullptr; }
		if (d3d12_device)         { d3d12_device->Release();                     d3d12_device         = nullptr; }
	}

	void *get_shared_texture_native_ptr() override
	{
		if (d3d12_unity_shared) return (void *)d3d12_unity_shared;
		return (void *)d3d12_shared_texture;
	}

	const void *build_session_binding(void *platform_window_handle, void *shared_texture_handle) override
	{
		// Not used directly — D3D12 session binding is assembled in standalone_start
		(void)platform_window_handle;
		(void)shared_texture_handle;
		return nullptr;
	}

	bool enumerate_atlas_images(XrSwapchain swapchain, PFN_xrEnumerateSwapchainImages pfn, uint32_t count) override
	{
		for (uint32_t i = 0; i < count; i++) {
			images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR;
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
		return (void *)images[index].texture;
	}

	void create_atlas_bridge(uint32_t atlas_w, uint32_t atlas_h, void *unity_dev) override
	{
		ID3D12Device *unity_d3d12 = (ID3D12Device *)unity_dev;
		if (!unity_d3d12 || !d3d12_device) return;

		D3D12_HEAP_PROPERTIES heap = {};
		heap.Type = D3D12_HEAP_TYPE_DEFAULT;

		D3D12_RESOURCE_DESC bd = {};
		bd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		bd.Width = atlas_w;
		bd.Height = atlas_h;
		bd.DepthOrArraySize = 1;
		bd.MipLevels = 1;
		bd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		bd.SampleDesc.Count = 1;
		bd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

		HRESULT hr = d3d12_device->CreateCommittedResource(
			&heap, D3D12_HEAP_FLAG_SHARED, &bd,
			D3D12_RESOURCE_STATE_COMMON, NULL,
			__uuidof(ID3D12Resource), (void **)&d3d12_atlas_bridge);
		if (FAILED(hr)) {
			fprintf(stderr, "[DisplayXR-SA] Atlas bridge CreateCommittedResource failed: 0x%08lx\n", hr);
		} else {
			hr = d3d12_device->CreateSharedHandle(
				d3d12_atlas_bridge, NULL, GENERIC_ALL, NULL,
				&d3d12_atlas_bridge_handle);
			if (SUCCEEDED(hr) && d3d12_atlas_bridge_handle) {
				hr = unity_d3d12->OpenSharedHandle(
					d3d12_atlas_bridge_handle,
					__uuidof(ID3D12Resource), (void **)&d3d12_unity_atlas_bridge);
				if (SUCCEEDED(hr) && d3d12_unity_atlas_bridge) {
					fprintf(stderr, "[DisplayXR-SA] Atlas bridge: %ux%u, handle=%p, unity_res=%p\n",
					        atlas_w, atlas_h, d3d12_atlas_bridge_handle,
					        (void *)d3d12_unity_atlas_bridge);
				} else {
					fprintf(stderr, "[DisplayXR-SA] Atlas bridge OpenSharedHandle failed: 0x%08lx\n", hr);
				}
			}
		}
	}

	void destroy_atlas_bridge() override
	{
		if (d3d12_unity_atlas_bridge)  { d3d12_unity_atlas_bridge->Release();              d3d12_unity_atlas_bridge  = nullptr; }
		if (d3d12_atlas_bridge_handle) { CloseHandle(d3d12_atlas_bridge_handle);           d3d12_atlas_bridge_handle = nullptr; }
		if (d3d12_atlas_bridge)        { d3d12_atlas_bridge->Release();                    d3d12_atlas_bridge        = nullptr; }
	}

	void *get_atlas_bridge_unity_ptr() override
	{
		return (void *)d3d12_unity_atlas_bridge;
	}

	void blit_atlas(void *atlas_tex, uint32_t index) override
	{
		// D3D12 cross-device atlas blit via shared bridge texture.
		// C# copies Unity's atlas RT → bridge (Unity device, via Graphics.CopyTexture).
		// We copy bridge → swapchain image (our device, same device).
		// Note: content is Y-flipped (Unity D3D12 convention) — the C# display
		// flips the final shared texture output via UV coords.
		ID3D12Resource *bridge = d3d12_atlas_bridge;
		ID3D12Resource *dst    = images[index].texture;
		if (bridge && dst && d3d12_cmd_list) {
			d3d12_cmd_alloc->Reset();
			d3d12_cmd_list->Reset(d3d12_cmd_alloc, NULL);

			D3D12_TEXTURE_COPY_LOCATION dst_loc = {};
			dst_loc.pResource = dst;
			dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			dst_loc.SubresourceIndex = 0;

			D3D12_TEXTURE_COPY_LOCATION src_loc = {};
			src_loc.pResource = bridge;
			src_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			src_loc.SubresourceIndex = 0;

			d3d12_cmd_list->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, NULL);
			d3d12_cmd_list->Close();

			ID3D12CommandList *lists[] = { d3d12_cmd_list };
			d3d12_queue->ExecuteCommandLists(1, lists);

			d3d12_fence_value++;
			d3d12_queue->Signal(d3d12_fence, d3d12_fence_value);
			if (d3d12_fence->GetCompletedValue() < d3d12_fence_value) {
				d3d12_fence->SetEventOnCompletion(d3d12_fence_value, d3d12_fence_event);
				WaitForSingleObject(d3d12_fence_event, INFINITE);
			}
		}
		(void)atlas_tex; // Unused — C# copies to bridge via Graphics.CopyTexture
	}

	bool fw_create_swapchain(void *hwnd, uint32_t w, uint32_t h) override
	{
		HWND s_fw_hwnd = (HWND)hwnd;
		if (!s_fw_hwnd) return false;

		IDXGIFactory4 *factory = NULL;
		if (FAILED(CreateDXGIFactory2(0, __uuidof(IDXGIFactory4), (void **)&factory))) {
			fprintf(stderr, "[DisplayXR-FW] CreateDXGIFactory2 failed\n");
			return false;
		}

		DXGI_SWAP_CHAIN_DESC1 sd = {};
		sd.BufferCount  = 2;
		sd.Width        = w;
		sd.Height       = h;
		sd.Format       = DXGI_FORMAT_R8G8B8A8_UNORM;
		sd.BufferUsage  = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		sd.SampleDesc.Count = 1;
		sd.SwapEffect   = DXGI_SWAP_EFFECT_FLIP_DISCARD;

		if (!d3d12_device || !d3d12_queue) { factory->Release(); return false; }
		IDXGISwapChain1 *sc1 = NULL;
		HRESULT hr = factory->CreateSwapChainForHwnd(
		    d3d12_queue, s_fw_hwnd, &sd, NULL, NULL, &sc1);
		factory->MakeWindowAssociation(s_fw_hwnd, DXGI_MWA_NO_ALT_ENTER);
		factory->Release();
		if (FAILED(hr)) {
			fprintf(stderr, "[DisplayXR-FW] CreateSwapChainForHwnd failed: 0x%08lx\n", hr);
			return false;
		}
		sc1->QueryInterface(__uuidof(IDXGISwapChain3), (void **)&fw_swapchain);
		sc1->Release();

		for (UINT i = 0; i < 2; i++)
			fw_swapchain->GetBuffer(i, __uuidof(ID3D12Resource), (void **)&fw_bb[i]);

		d3d12_device->CreateCommandAllocator(
		    D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator),
		    (void **)&fw_cmd_alloc);
		d3d12_device->CreateCommandList(
		    0, D3D12_COMMAND_LIST_TYPE_DIRECT, fw_cmd_alloc, NULL,
		    __uuidof(ID3D12GraphicsCommandList), (void **)&fw_cmd_list);
		fw_cmd_list->Close();

		fw_fence_value = 0;
		d3d12_device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
		    __uuidof(ID3D12Fence), (void **)&fw_fence);
		fw_fence_event = CreateEventW(NULL, FALSE, FALSE, NULL);

		fprintf(stderr, "[DisplayXR-FW] D3D12 swap chain created: %ux%u\n", w, h);
		return true;
	}

	void fw_destroy_swapchain() override
	{
		if (fw_cmd_list)  { fw_cmd_list->Release();             fw_cmd_list  = nullptr; }
		if (fw_cmd_alloc) { fw_cmd_alloc->Release();            fw_cmd_alloc = nullptr; }
		if (fw_bb[0])     { fw_bb[0]->Release();                fw_bb[0]     = nullptr; }
		if (fw_bb[1])     { fw_bb[1]->Release();                fw_bb[1]     = nullptr; }
		if (fw_swapchain) { fw_swapchain->Release();            fw_swapchain = nullptr; }
		if (fw_fence)     { fw_fence->Release();                fw_fence     = nullptr; }
		if (fw_fence_event) { CloseHandle(fw_fence_event);      fw_fence_event = nullptr; }
		fw_fence_value = 0;
	}

	void fw_resize_swapchain_buffers(uint32_t w, uint32_t h) override
	{
		if (!fw_swapchain) return;
		if (fw_bb[0]) { fw_bb[0]->Release(); fw_bb[0] = nullptr; }
		if (fw_bb[1]) { fw_bb[1]->Release(); fw_bb[1] = nullptr; }

		HRESULT hr = fw_swapchain->ResizeBuffers(2, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, 0);
		if (FAILED(hr)) {
			fprintf(stderr, "[DisplayXR-FW] ResizeBuffers failed: 0x%08lx\n", hr);
			return;
		}
		for (UINT i = 0; i < 2; i++)
			fw_swapchain->GetBuffer(i, __uuidof(ID3D12Resource), (void **)&fw_bb[i]);
	}

	void fw_present(uint32_t sc_w, uint32_t sc_h) override
	{
		if (!fw_swapchain) return;
		UINT bb_idx = fw_swapchain->GetCurrentBackBufferIndex();

		// D3D12 blit via command list
		if (!d3d12_shared_texture) return;
		ID3D12Resource *bb = fw_bb[bb_idx];

		fw_cmd_alloc->Reset();
		fw_cmd_list->Reset(fw_cmd_alloc, NULL);

		D3D12_RESOURCE_BARRIER barriers[2] = {};
		barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barriers[0].Transition.pResource  = d3d12_shared_texture;
		barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
		barriers[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
		barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

		barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barriers[1].Transition.pResource  = bb;
		barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
		barriers[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
		barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

		fw_cmd_list->ResourceBarrier(2, barriers);

		// Copy only the canvas-sized content region from the shared texture.
		// The shared texture is always disp_w×disp_h but the runtime only writes
		// in the top-left sc_w×sc_h area (= current canvas).
		{
			D3D12_TEXTURE_COPY_LOCATION src = {};
			src.pResource        = d3d12_shared_texture;
			src.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			src.SubresourceIndex = 0;

			D3D12_TEXTURE_COPY_LOCATION dst = {};
			dst.pResource        = bb;
			dst.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			dst.SubresourceIndex = 0;

			D3D12_BOX box = { 0, 0, 0, sc_w, sc_h, 1 };
			fw_cmd_list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
		}

		barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
		barriers[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_COMMON;
		barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
		barriers[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
		fw_cmd_list->ResourceBarrier(2, barriers);

		fw_cmd_list->Close();
		ID3D12CommandList *lists[] = { fw_cmd_list };
		d3d12_queue->ExecuteCommandLists(1, lists);

		// Wait for GPU completion before Present (fence on same queue as runtime)
		fw_fence_value++;
		d3d12_queue->Signal(fw_fence, fw_fence_value);
		if (fw_fence->GetCompletedValue() < fw_fence_value) {
			fw_fence->SetEventOnCompletion(fw_fence_value, fw_fence_event);
			WaitForSingleObject(fw_fence_event, INFINITE);
		}

		fw_swapchain->Present(0, 0);
	}

	void destroy() override
	{
		destroy_atlas_bridge();
		// Release fence/cmd resources (shared texture destroy also releases these,
		// but destroy_shared_texture is called separately)
		if (d3d12_unity_atlas_bridge)  { d3d12_unity_atlas_bridge->Release();  d3d12_unity_atlas_bridge  = nullptr; }
		if (d3d12_atlas_bridge_handle) { CloseHandle(d3d12_atlas_bridge_handle); d3d12_atlas_bridge_handle = nullptr; }
		if (d3d12_atlas_bridge)        { d3d12_atlas_bridge->Release();          d3d12_atlas_bridge        = nullptr; }
		destroy_shared_texture();
	}

	void set_unity_device(void *dev) override { unity_device = (ID3D12Device *)dev; }
	void *get_graphics_device() override { return (void *)d3d12_device; }
	void *get_graphics_queue() override { return (void *)d3d12_queue; }
	void *get_shared_handle() override { return (void *)d3d12_shared_handle; }
};

StandaloneGraphicsBackend *create_standalone_d3d12_backend() { return new StandaloneD3D12Backend(); }

#endif // defined(_WIN32)
