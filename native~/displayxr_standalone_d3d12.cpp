// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
//
// D3D12 graphics backend for the standalone OpenXR session.
// Extracted from displayxr_standalone.cpp.
//
// The SA session always runs on its own D3D12 device (not Unity's — sharing
// caused device removal). Atlas blit and shared-texture output cross the
// device boundary via DXGI shared handles. Unity may be running on either
// D3D12 or D3D11 — the bridge opens on whichever API Unity is using so that
// C# can Graphics.CopyTexture into it from the atlas RenderTexture.

#if defined(_WIN32)

#include "displayxr_standalone_internal.h"
#include <d3d11_1.h>  // ID3D11Device1::OpenSharedResource1

class StandaloneD3D12Backend : public StandaloneGraphicsBackend {
public:
	// D3D12 path (SA's own device for runtime session)
	ID3D12Device                 *d3d12_device         = nullptr;
	ID3D12CommandQueue           *d3d12_queue           = nullptr;
	ID3D12CommandAllocator       *d3d12_cmd_alloc       = nullptr;
	ID3D12GraphicsCommandList    *d3d12_cmd_list        = nullptr;
	ID3D12Fence                  *d3d12_fence           = nullptr;
	HANDLE                        d3d12_fence_event      = nullptr;
	UINT64                        d3d12_fence_value      = 0;
	ID3D12Resource               *d3d12_shared_texture  = nullptr;
	HANDLE                        d3d12_shared_handle    = nullptr;
	ID3D12Resource               *d3d12_unity_shared     = nullptr;

	// Atlas bridge: SHARED on SA device, opened on Unity's device.
	ID3D12Resource               *d3d12_atlas_bridge        = nullptr;
	HANDLE                        d3d12_atlas_bridge_handle  = nullptr;
	ID3D12Resource               *d3d12_unity_atlas_bridge   = nullptr;  // if Unity = D3D12
	ID3D11Texture2D              *d3d11_unity_atlas_bridge   = nullptr;  // if Unity = D3D11

	// WSUI bridge: SHARED on SA device, opened on Unity's device.
	// Mirrors the atlas bridge but sized to the wsui RT (1024x1024 BGRA8 typical).
	ID3D12Resource               *d3d12_wsui_bridge         = nullptr;
	HANDLE                        d3d12_wsui_bridge_handle  = nullptr;
	ID3D12Resource               *d3d12_unity_wsui_bridge   = nullptr;
	ID3D11Texture2D              *d3d11_unity_wsui_bridge   = nullptr;
	uint32_t                      wsui_bridge_w             = 0;
	uint32_t                      wsui_bridge_h             = 0;

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

	// Unity devices (set by set_unity_device before start)
	ID3D12Device                 *unity_device       = nullptr;  // non-null if Unity = D3D12
	ID3D11Device                 *unity_d3d11_device  = nullptr;  // non-null if Unity = D3D11

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

		D3D12_COMMAND_QUEUE_DESC qd = {};
		qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
		HRESULT hr = d3d12_device->CreateCommandQueue(
			&qd, __uuidof(ID3D12CommandQueue), (void **)&d3d12_queue);
		if (FAILED(hr)) {
			fprintf(stderr, "[DisplayXR-SA] CreateCommandQueue failed: 0x%08lx\n", hr);
			return false;
		}

		d3d12_device->CreateCommandAllocator(
			D3D12_COMMAND_LIST_TYPE_DIRECT,
			__uuidof(ID3D12CommandAllocator), (void **)&d3d12_cmd_alloc);
		d3d12_device->CreateCommandList(
			0, D3D12_COMMAND_LIST_TYPE_DIRECT, d3d12_cmd_alloc, NULL,
			__uuidof(ID3D12GraphicsCommandList), (void **)&d3d12_cmd_list);
		d3d12_cmd_list->Close();

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
		// unity_dev is unused here — we use the devices already captured by
		// set_unity_device(). The bridge resource is always D3D12 on the SA
		// device; we open it on whichever API Unity is running.
		(void)unity_dev;
		if ((!unity_device && !unity_d3d11_device) || !d3d12_device) return;

		fprintf(stderr, "[DisplayXR-SA] Atlas bridge: unity_d3d12=%p, unity_d3d11=%p, sa_device=%p\n",
		        (void *)unity_device, (void *)unity_d3d11_device, (void *)d3d12_device);

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
			return;
		}

		hr = d3d12_device->CreateSharedHandle(
			d3d12_atlas_bridge, NULL, GENERIC_ALL, NULL,
			&d3d12_atlas_bridge_handle);
		if (FAILED(hr) || !d3d12_atlas_bridge_handle) {
			fprintf(stderr, "[DisplayXR-SA] Atlas bridge CreateSharedHandle failed: 0x%08lx\n", hr);
			return;
		}

		if (unity_device) {
			// D3D12 Unity: open the NT handle directly on the Unity D3D12 device.
			hr = unity_device->OpenSharedHandle(
				d3d12_atlas_bridge_handle,
				__uuidof(ID3D12Resource), (void **)&d3d12_unity_atlas_bridge);
			if (SUCCEEDED(hr) && d3d12_unity_atlas_bridge) {
				fprintf(stderr, "[DisplayXR-SA] Atlas bridge (D3D12): %ux%u, handle=%p, unity_res=%p\n",
				        atlas_w, atlas_h, d3d12_atlas_bridge_handle,
				        (void *)d3d12_unity_atlas_bridge);
			} else {
				fprintf(stderr, "[DisplayXR-SA] Atlas bridge OpenSharedHandle (D3D12) failed: 0x%08lx\n", hr);
			}
		} else if (unity_d3d11_device) {
			// D3D11 Unity: open the NT handle on the D3D11 device via
			// ID3D11Device1::OpenSharedResource1, which accepts NT-shared
			// handles created by D3D12's CreateSharedHandle.
			ID3D11Device1 *dev1 = NULL;
			hr = unity_d3d11_device->QueryInterface(
				__uuidof(ID3D11Device1), (void **)&dev1);
			if (SUCCEEDED(hr) && dev1) {
				hr = dev1->OpenSharedResource1(
					d3d12_atlas_bridge_handle,
					__uuidof(ID3D11Texture2D),
					(void **)&d3d11_unity_atlas_bridge);
				dev1->Release();
				if (SUCCEEDED(hr) && d3d11_unity_atlas_bridge) {
					fprintf(stderr, "[DisplayXR-SA] Atlas bridge (D3D11): %ux%u, handle=%p, tex=%p\n",
					        atlas_w, atlas_h, d3d12_atlas_bridge_handle,
					        (void *)d3d11_unity_atlas_bridge);
				} else {
					fprintf(stderr, "[DisplayXR-SA] Atlas bridge OpenSharedResource1 (D3D11) failed: 0x%08lx\n", hr);
				}
			}
		}
	}

	void destroy_atlas_bridge() override
	{
		if (d3d11_unity_atlas_bridge) { d3d11_unity_atlas_bridge->Release(); d3d11_unity_atlas_bridge = nullptr; }
		if (d3d12_unity_atlas_bridge) { d3d12_unity_atlas_bridge->Release(); d3d12_unity_atlas_bridge = nullptr; }
		if (d3d12_atlas_bridge_handle) { CloseHandle(d3d12_atlas_bridge_handle); d3d12_atlas_bridge_handle = nullptr; }
		if (d3d12_atlas_bridge)       { d3d12_atlas_bridge->Release();          d3d12_atlas_bridge        = nullptr; }
	}

	void *get_atlas_bridge_unity_ptr() override
	{
		// Return whichever Unity-side texture we opened — C# passes this to
		// Texture2D.CreateExternalTexture, which accepts ID3D11Texture2D* on
		// D3D11 and ID3D12Resource* on D3D12 based on the editor's graphics API.
		if (d3d11_unity_atlas_bridge) return (void *)d3d11_unity_atlas_bridge;
		if (d3d12_unity_atlas_bridge) return (void *)d3d12_unity_atlas_bridge;
		return nullptr;
	}

	void blit_atlas(void *atlas_tex, uint32_t index) override
	{
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
		(void)atlas_tex;
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
		wsui_destroy_bridge();
		if (unity_d3d11_device)  { unity_d3d11_device->Release();  unity_d3d11_device = nullptr; }
		if (unity_device)        { unity_device->Release();        unity_device       = nullptr; }
		destroy_shared_texture();
	}

	void set_unity_device(void *native_tex_ptr) override
	{
		// Detect Unity's graphics API by QueryInterface on a native texture ptr.
		// C# passes Texture2D.GetNativeTexturePtr() — ID3D12Resource* on D3D12,
		// ID3D11Texture2D* on D3D11.
		if (!native_tex_ptr) return;
		IUnknown *unk = (IUnknown *)native_tex_ptr;

		// Try D3D12 first.
		{
			ID3D12Resource *res = NULL;
			HRESULT hr = unk->QueryInterface(__uuidof(ID3D12Resource), (void **)&res);
			if (SUCCEEDED(hr) && res) {
				ID3D12Device *dev = NULL;
				hr = res->GetDevice(__uuidof(ID3D12Device), (void **)&dev);
				res->Release();
				if (SUCCEEDED(hr) && dev) {
					unity_device = dev;
					fprintf(stderr, "[DisplayXR-SA] Unity graphics: D3D12 device=%p\n", (void *)dev);
					return;
				}
			}
		}

		// Fall back to D3D11.
		{
			ID3D11Resource *res = NULL;
			HRESULT hr = unk->QueryInterface(__uuidof(ID3D11Resource), (void **)&res);
			if (SUCCEEDED(hr) && res) {
				ID3D11Device *dev = NULL;
				res->GetDevice(&dev);
				res->Release();
				if (dev) {
					unity_d3d11_device = dev;
					fprintf(stderr, "[DisplayXR-SA] Unity graphics: D3D11 device=%p\n", (void *)dev);
					return;
				}
			}
		}

		fprintf(stderr, "[DisplayXR-SA] Texture is neither ID3D12Resource nor ID3D11Resource — atlas bridge disabled\n");
	}

	void *get_graphics_device() override { return (void *)d3d12_device; }
	void *get_graphics_queue() override { return (void *)d3d12_queue; }
	void *get_shared_handle() override { return (void *)d3d12_shared_handle; }

	// --- Window-space UI overlay (issue #67) ---
	// Same cross-device limitation as the D3D11 standalone backend (see the
	// matching comment there). Built apps go through the hooked path which
	// IS supported. Editor Preview Window UI overlay on Windows D3D12 is a
	// follow-up.

	bool wsui_enumerate_swapchain_images(XrSwapchain sc,
	                                      PFN_xrEnumerateSwapchainImages pfn_enum,
	                                      uint32_t capacity,
	                                      uint32_t *out_count,
	                                      void *out_native_ptrs[]) override
	{
		uint32_t count = 0;
		if (XR_FAILED(pfn_enum(sc, 0, &count, NULL)) || count == 0) return false;
		if (count > capacity) count = capacity;
		XrSwapchainImageD3D12KHR imgs[16] = {};
		if (count > 16) count = 16;
		for (uint32_t i = 0; i < count; i++) {
			imgs[i].type = XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR;
		}
		if (XR_FAILED(pfn_enum(sc, count, &count, (XrSwapchainImageBaseHeader *)imgs))) {
			return false;
		}
		for (uint32_t i = 0; i < count; i++) {
			out_native_ptrs[i] = (void *)imgs[i].texture;
		}
		*out_count = count;
		return true;
	}

	// Cross-device copy via the wsui bridge. Caller is expected to have called
	// wsui_create_bridge() once (done lazily by wsui_standalone_pre_end_frame)
	// and Unity-side C# is expected to have copied its RT into the bridge
	// (Texture2D wrapping d3d12_unity_wsui_bridge / d3d11_unity_wsui_bridge)
	// before this fires. We just copy SA-bridge → swapchain image on the SA
	// device queue. unity_tex is unused — the bridge is the data path.
	bool wsui_copy_to_swapchain_image(void * /*unity_tex*/, void *sc_image,
	                                    uint32_t w, uint32_t h) override
	{
		ID3D12Resource *bridge = d3d12_wsui_bridge;
		ID3D12Resource *dst    = (ID3D12Resource *)sc_image;
		if (!bridge || !dst || !d3d12_queue || !d3d12_cmd_list || !d3d12_cmd_alloc)
			return false;
		// Bridge must be sized to the wsui RT.
		if (wsui_bridge_w != w || wsui_bridge_h != h) {
			static int warned_dim = 0;
			if (!warned_dim) {
				warned_dim = 1;
				fprintf(stderr, "[DisplayXR-SA] wsui D3D12: bridge size mismatch "
				    "(bridge=%ux%u, requested=%ux%u)\n",
				    wsui_bridge_w, wsui_bridge_h, w, h);
			}
			return false;
		}

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
		return true;
	}

	void wsui_create_bridge(uint32_t w, uint32_t h) override
	{
		if (d3d12_wsui_bridge != nullptr) {
			// Already created. Recreate only if dimensions changed.
			if (wsui_bridge_w == w && wsui_bridge_h == h) return;
			wsui_destroy_bridge();
		}
		if ((!unity_device && !unity_d3d11_device) || !d3d12_device) return;
		if (w == 0 || h == 0) return;

		D3D12_HEAP_PROPERTIES heap = {};
		heap.Type = D3D12_HEAP_TYPE_DEFAULT;

		D3D12_RESOURCE_DESC bd = {};
		bd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		bd.Width = w;
		bd.Height = h;
		bd.DepthOrArraySize = 1;
		bd.MipLevels = 1;
		// Unity C# WSUI creates its RT in B8G8R8A8_UNorm (URP RenderGraph
		// requirement). Match here so Graphics.CopyTexture on Unity side
		// and CopyTextureRegion on SA side are identical-format copies.
		bd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		bd.SampleDesc.Count = 1;
		bd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

		HRESULT hr = d3d12_device->CreateCommittedResource(
			&heap, D3D12_HEAP_FLAG_SHARED, &bd,
			D3D12_RESOURCE_STATE_COMMON, NULL,
			__uuidof(ID3D12Resource), (void **)&d3d12_wsui_bridge);
		if (FAILED(hr)) {
			fprintf(stderr, "[DisplayXR-SA] wsui bridge CreateCommittedResource failed: 0x%08lx\n", hr);
			return;
		}

		hr = d3d12_device->CreateSharedHandle(
			d3d12_wsui_bridge, NULL, GENERIC_ALL, NULL, &d3d12_wsui_bridge_handle);
		if (FAILED(hr) || !d3d12_wsui_bridge_handle) {
			fprintf(stderr, "[DisplayXR-SA] wsui bridge CreateSharedHandle failed: 0x%08lx\n", hr);
			wsui_destroy_bridge();
			return;
		}

		if (unity_device) {
			hr = unity_device->OpenSharedHandle(
				d3d12_wsui_bridge_handle,
				__uuidof(ID3D12Resource), (void **)&d3d12_unity_wsui_bridge);
			if (SUCCEEDED(hr) && d3d12_unity_wsui_bridge) {
				fprintf(stderr, "[DisplayXR-SA] wsui bridge (D3D12): %ux%u, handle=%p, unity_res=%p\n",
				        w, h, d3d12_wsui_bridge_handle, (void *)d3d12_unity_wsui_bridge);
			} else {
				fprintf(stderr, "[DisplayXR-SA] wsui bridge OpenSharedHandle (D3D12) failed: 0x%08lx\n", hr);
				wsui_destroy_bridge();
				return;
			}
		} else if (unity_d3d11_device) {
			ID3D11Device1 *dev1 = NULL;
			hr = unity_d3d11_device->QueryInterface(
				__uuidof(ID3D11Device1), (void **)&dev1);
			if (SUCCEEDED(hr) && dev1) {
				hr = dev1->OpenSharedResource1(
					d3d12_wsui_bridge_handle,
					__uuidof(ID3D11Texture2D),
					(void **)&d3d11_unity_wsui_bridge);
				dev1->Release();
				if (SUCCEEDED(hr) && d3d11_unity_wsui_bridge) {
					fprintf(stderr, "[DisplayXR-SA] wsui bridge (D3D11): %ux%u, handle=%p, tex=%p\n",
					        w, h, d3d12_wsui_bridge_handle, (void *)d3d11_unity_wsui_bridge);
				} else {
					fprintf(stderr, "[DisplayXR-SA] wsui bridge OpenSharedResource1 (D3D11) failed: 0x%08lx\n", hr);
					wsui_destroy_bridge();
					return;
				}
			}
		}

		wsui_bridge_w = w;
		wsui_bridge_h = h;
	}

	void wsui_destroy_bridge() override
	{
		if (d3d11_unity_wsui_bridge) { d3d11_unity_wsui_bridge->Release(); d3d11_unity_wsui_bridge = nullptr; }
		if (d3d12_unity_wsui_bridge) { d3d12_unity_wsui_bridge->Release(); d3d12_unity_wsui_bridge = nullptr; }
		if (d3d12_wsui_bridge_handle) { CloseHandle(d3d12_wsui_bridge_handle); d3d12_wsui_bridge_handle = nullptr; }
		if (d3d12_wsui_bridge)       { d3d12_wsui_bridge->Release();          d3d12_wsui_bridge        = nullptr; }
		wsui_bridge_w = 0;
		wsui_bridge_h = 0;
	}

	void *wsui_get_bridge_unity_ptr() override
	{
		if (d3d11_unity_wsui_bridge) return (void *)d3d11_unity_wsui_bridge;
		if (d3d12_unity_wsui_bridge) return (void *)d3d12_unity_wsui_bridge;
		return nullptr;
	}
};

StandaloneGraphicsBackend *create_standalone_d3d12_backend() { return new StandaloneD3D12Backend(); }

#endif // defined(_WIN32)
