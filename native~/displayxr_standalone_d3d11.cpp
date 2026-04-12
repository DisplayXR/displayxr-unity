// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
//
// D3D11 graphics backend for the standalone OpenXR session.
// Extracted from displayxr_standalone.cpp — no logic changes.
//
// === SCAFFOLD STATE (Phase 1 checkpoint) ===
// Verbatim copy of a089149:native~/displayxr_standalone_d3d11.cpp. NOT
// listed in CMake SOURCES yet. The big-cut session must:
//   1. Diff against current main's displayxr_standalone.cpp D3D11 paths
//      (from 2a84927 Windows SA preview atlas bridge work) and merge any
//      superset into this file.
//   2. Align the atlas bridge path here with main's post-a7c91dc shape.
//   3. Add to CMake SOURCES and remove duplicated code from
//      displayxr_standalone.cpp.

#if defined(_WIN32)

#include "displayxr_standalone_internal.h"

class StandaloneD3D11Backend : public StandaloneGraphicsBackend {
public:
	// D3D11 path (when Unity uses Direct3D 11)
	ID3D11Device             *d3d11_device         = nullptr; // Our own D3D11 device (for runtime session)
	ID3D11DeviceContext      *d3d11_context         = nullptr; // Immediate context for d3d11_device
	ID3D11Texture2D          *d3d11_shared_texture  = nullptr; // Weaved output on our device, SHARED
	HANDLE                    d3d11_shared_handle    = nullptr; // DXGI shared handle for output texture
	ID3D11Texture2D          *d3d11_unity_shared     = nullptr; // Output texture opened on Unity's device
	// D3D11 atlas bridge: shared texture for cross-device atlas blit
	ID3D11Texture2D          *d3d11_atlas_bridge        = nullptr; // On our device, SHARED
	HANDLE                    d3d11_atlas_bridge_handle  = nullptr; // DXGI shared handle
	ID3D11Texture2D          *d3d11_unity_atlas_bridge   = nullptr; // Opened on Unity's device

	// Fullscreen window resources (D3D11)
	IDXGISwapChain3          *fw_swapchain   = nullptr;
	ID3D11Texture2D          *fw_bb[2]       = { nullptr, nullptr };

	// Swapchain images
	XrSwapchainImageD3D11KHR  images[SA_MAX_SWAPCHAIN_IMAGES] = {};

	// Unity device (set before start)
	ID3D11Device             *unity_device   = nullptr;

	bool create_device(XrInstance instance, XrSystemId system_id, PFN_xrGetInstanceProcAddr gipa) override
	{
		PFN_xrVoidFunction fn_req = NULL;
		gipa(instance, "xrGetD3D11GraphicsRequirementsKHR", &fn_req);

		LUID adapter_luid = {};
		if (fn_req) {
			XrGraphicsRequirementsD3D11KHR req = {};
			req.type = XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR;
			XrResult result = ((XrResult(XRAPI_CALL *)(XrInstance, XrSystemId, void *))fn_req)(
				instance, system_id, &req);
			if (XR_FAILED(result)) {
				fprintf(stderr, "[DisplayXR-SA] xrGetD3D11GraphicsRequirementsKHR failed: %d\n", result);
				return false;
			}
			adapter_luid = req.adapterLuid;
			fprintf(stderr, "[DisplayXR-SA] D3D11 requirements: adapter LUID=%08lx-%08lx, minFeatureLevel=0x%x\n",
			        adapter_luid.HighPart, adapter_luid.LowPart, (unsigned)req.minFeatureLevel);
		}

		// Create our own D3D11 device for the runtime session (same reason as D3D12 —
		// sharing Unity's device causes conflicts with the compositor).
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
			if (factory) factory->Release();

			UINT flags = 0;
			D3D_FEATURE_LEVEL feat_levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
			D3D_FEATURE_LEVEL chosen_level = D3D_FEATURE_LEVEL_11_0;
			HRESULT hr = D3D11CreateDevice(
				adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
				NULL, flags,
				feat_levels, 2,
				D3D11_SDK_VERSION,
				&d3d11_device, &chosen_level, &d3d11_context);
			if (adapter) adapter->Release();
			if (FAILED(hr) || !d3d11_device) {
				fprintf(stderr, "[DisplayXR-SA] D3D11CreateDevice failed: 0x%08lx\n", hr);
				return false;
			}
			fprintf(stderr, "[DisplayXR-SA] Created own D3D11 device for runtime session (featureLevel=0x%x)\n",
			        (unsigned)chosen_level);
		}
		return true;
	}

	bool create_shared_texture(uint32_t width, uint32_t height) override
	{
		D3D11_TEXTURE2D_DESC td = {};
		td.Width  = width;
		td.Height = height;
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_DEFAULT;
		td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
		td.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

		HRESULT hr = d3d11_device->CreateTexture2D(&td, NULL, &d3d11_shared_texture);
		if (FAILED(hr)) {
			fprintf(stderr, "[DisplayXR-SA] D3D11 CreateTexture2D (shared) failed: 0x%08lx\n", hr);
			return false;
		}

		IDXGIResource *dxgi_res = NULL;
		hr = d3d11_shared_texture->QueryInterface(__uuidof(IDXGIResource), (void **)&dxgi_res);
		if (SUCCEEDED(hr) && dxgi_res) {
			dxgi_res->GetSharedHandle(&d3d11_shared_handle);
			dxgi_res->Release();
		}
		if (!d3d11_shared_handle) {
			fprintf(stderr, "[DisplayXR-SA] D3D11 GetSharedHandle failed\n");
			return false;
		}

		fprintf(stderr, "[DisplayXR-SA] D3D11 shared texture: %ux%u, handle=%p\n",
		        width, height, d3d11_shared_handle);

		// Open on Unity's device for C# CreateExternalTexture
		if (unity_device) {
			hr = unity_device->OpenSharedResource(
				d3d11_shared_handle,
				__uuidof(ID3D11Texture2D),
				(void **)&d3d11_unity_shared);
			if (SUCCEEDED(hr) && d3d11_unity_shared) {
				fprintf(stderr, "[DisplayXR-SA] D3D11 shared texture opened on Unity device: %p\n",
				        (void *)d3d11_unity_shared);
			} else {
				fprintf(stderr, "[DisplayXR-SA] D3D11 OpenSharedResource on Unity device failed: 0x%08lx\n", hr);
			}
		}
		return true;
	}

	void destroy_shared_texture() override
	{
		if (d3d11_unity_shared)    { d3d11_unity_shared->Release();    d3d11_unity_shared    = nullptr; }
		if (d3d11_shared_handle)   { CloseHandle(d3d11_shared_handle); d3d11_shared_handle   = nullptr; }
		if (d3d11_shared_texture)  { d3d11_shared_texture->Release();  d3d11_shared_texture  = nullptr; }
	}

	void *get_shared_texture_native_ptr() override
	{
		if (d3d11_unity_shared) return (void *)d3d11_unity_shared;
		return (void *)d3d11_shared_texture;
	}

	const void *build_session_binding(void *platform_window_handle, void *shared_texture_handle) override
	{
		// Not used directly — D3D11 session binding is assembled in standalone_start
		// This method is unused for D3D11 (binding is assembled inline like before)
		(void)platform_window_handle;
		(void)shared_texture_handle;
		return nullptr;
	}

	bool enumerate_atlas_images(XrSwapchain swapchain, PFN_xrEnumerateSwapchainImages pfn, uint32_t count) override
	{
		for (uint32_t i = 0; i < count; i++) {
			images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR;
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
		ID3D11Device *unity_d3d11 = (ID3D11Device *)unity_dev;
		if (!unity_d3d11 || !d3d11_device) return;

		D3D11_TEXTURE2D_DESC bd = {};
		bd.Width  = atlas_w;
		bd.Height = atlas_h;
		bd.MipLevels = 1;
		bd.ArraySize = 1;
		bd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		bd.SampleDesc.Count = 1;
		bd.Usage = D3D11_USAGE_DEFAULT;
		bd.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
		bd.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

		HRESULT hr = d3d11_device->CreateTexture2D(&bd, NULL, &d3d11_atlas_bridge);
		if (FAILED(hr)) {
			fprintf(stderr, "[DisplayXR-SA] D3D11 atlas bridge CreateTexture2D failed: 0x%08lx\n", hr);
		} else {
			IDXGIResource *dxgi_res = NULL;
			hr = d3d11_atlas_bridge->QueryInterface(__uuidof(IDXGIResource), (void **)&dxgi_res);
			if (SUCCEEDED(hr) && dxgi_res) {
				dxgi_res->GetSharedHandle(&d3d11_atlas_bridge_handle);
				dxgi_res->Release();
			}
			if (d3d11_atlas_bridge_handle) {
				hr = unity_d3d11->OpenSharedResource(
					d3d11_atlas_bridge_handle,
					__uuidof(ID3D11Texture2D),
					(void **)&d3d11_unity_atlas_bridge);
				if (SUCCEEDED(hr) && d3d11_unity_atlas_bridge) {
					fprintf(stderr, "[DisplayXR-SA] D3D11 atlas bridge: %ux%u, handle=%p, unity_res=%p\n",
					        atlas_w, atlas_h, d3d11_atlas_bridge_handle,
					        (void *)d3d11_unity_atlas_bridge);
				} else {
					fprintf(stderr, "[DisplayXR-SA] D3D11 atlas bridge OpenSharedResource failed: 0x%08lx\n", hr);
				}
			}
		}
	}

	void destroy_atlas_bridge() override
	{
		if (d3d11_unity_atlas_bridge) { d3d11_unity_atlas_bridge->Release(); d3d11_unity_atlas_bridge = nullptr; }
		if (d3d11_atlas_bridge_handle) { CloseHandle(d3d11_atlas_bridge_handle); d3d11_atlas_bridge_handle = nullptr; }
		if (d3d11_atlas_bridge) { d3d11_atlas_bridge->Release(); d3d11_atlas_bridge = nullptr; }
	}

	void *get_atlas_bridge_unity_ptr() override
	{
		return (void *)d3d11_unity_atlas_bridge;
	}

	void blit_atlas(void *atlas_tex, uint32_t index) override
	{
		// D3D11 cross-device atlas blit via shared bridge texture.
		// C# copies Unity's atlas RT → d3d11_unity_atlas_bridge (Unity device).
		// We copy d3d11_atlas_bridge → swapchain image (our device, same device).
		ID3D11Texture2D *bridge = d3d11_atlas_bridge;
		ID3D11Texture2D *dst    = images[index].texture;
		if (bridge && dst && d3d11_context) {
			d3d11_context->CopyResource(dst, bridge);
			d3d11_context->Flush();
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

		if (!d3d11_device) { factory->Release(); return false; }
		IDXGISwapChain1 *sc1 = NULL;
		HRESULT hr = factory->CreateSwapChainForHwnd(
		    d3d11_device, s_fw_hwnd, &sd, NULL, NULL, &sc1);
		factory->MakeWindowAssociation(s_fw_hwnd, DXGI_MWA_NO_ALT_ENTER);
		factory->Release();
		if (FAILED(hr)) {
			fprintf(stderr, "[DisplayXR-FW] CreateSwapChainForHwnd failed: 0x%08lx\n", hr);
			return false;
		}
		sc1->QueryInterface(__uuidof(IDXGISwapChain3), (void **)&fw_swapchain);
		sc1->Release();

		for (UINT i = 0; i < 2; i++)
			fw_swapchain->GetBuffer(i, __uuidof(ID3D11Texture2D), (void **)&fw_bb[i]);
		// No command list/fence needed for D3D11 immediate context
		fprintf(stderr, "[DisplayXR-FW] D3D11 swap chain created: %ux%u\n", w, h);
		return true;
	}

	void fw_destroy_swapchain() override
	{
		if (fw_bb[0])     { fw_bb[0]->Release();     fw_bb[0]     = nullptr; }
		if (fw_bb[1])     { fw_bb[1]->Release();     fw_bb[1]     = nullptr; }
		if (fw_swapchain) { fw_swapchain->Release(); fw_swapchain = nullptr; }
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
			fw_swapchain->GetBuffer(i, __uuidof(ID3D11Texture2D), (void **)&fw_bb[i]);
	}

	void fw_present(uint32_t sc_w, uint32_t sc_h) override
	{
		if (!fw_swapchain) return;
		UINT bb_idx = fw_swapchain->GetCurrentBackBufferIndex();

		// D3D11 blit: CopySubresourceRegion canvas region → back buffer
		ID3D11Texture2D *src = d3d11_shared_texture;
		ID3D11Texture2D *bb  = fw_bb[bb_idx];
		if (!src || !bb || !d3d11_context) return;

		D3D11_BOX box = { 0, 0, 0, sc_w, sc_h, 1 };
		d3d11_context->CopySubresourceRegion(bb, 0, 0, 0, 0, src, 0, &box);
		d3d11_context->Flush();

		fw_swapchain->Present(0, 0);
	}

	void destroy() override
	{
		destroy_atlas_bridge();
		destroy_shared_texture();
		if (d3d11_context) { d3d11_context->Release(); d3d11_context = nullptr; }
		if (d3d11_device)  { d3d11_device->Release();  d3d11_device  = nullptr; }
	}

	void set_unity_device(void *dev) override { unity_device = (ID3D11Device *)dev; }
	void *get_graphics_device() override { return (void *)d3d11_device; }
	void *get_graphics_queue() override { return nullptr; } // D3D11 has no explicit queue
	void *get_shared_handle() override { return (void *)d3d11_shared_handle; }
};

StandaloneGraphicsBackend *create_standalone_d3d11_backend() { return new StandaloneD3D11Backend(); }

#endif // defined(_WIN32)
