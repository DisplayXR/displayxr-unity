// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
//
// Custom Unity IUnityXRDisplay Display Provider (epic #166, M1 — GO/NO-GO spike).
//
// This is the Unity-facing half: it registers a display subsystem lifecycle
// provider (matching Runtime/UnitySubsystemsManifest.json), wires the main-thread
// + graphics-thread provider callbacks, surfaces the DisplayXR runtime's SPI
// swapchain images to Unity via CreateTexture (zero-copy on Unity's D3D12
// device), and per-frame drives the runtime session through
// displayxr_provider_session.{h,cpp}.
//
// Lifecycle (main thread):  Initialize -> Start -> Stop -> Shutdown
// Graphics thread:          Start -> PopulateNextFrameDesc / SubmitCurrentFrame -> Stop
//
// Registered from displayxr_unity_plugin.cpp's UnityPluginLoad. Windows / D3D12,
// M1 only. Does NOT touch the OpenXR hook path or the editor standalone path.

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include "../unity_pluginapi/IUnityInterface.h"
#include "../unity_pluginapi/IUnityGraphics.h"
#include "../unity_pluginapi/IUnityGraphicsD3D12.h"
#include "../unity_pluginapi/IUnityXRDisplay.h"

#include "displayxr_provider_session.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

// Must match Runtime/UnitySubsystemsManifest.json ("name" + display "id").
static const char *k_plugin_name = "DisplayXR";
static const char *k_display_id  = "DisplayXR Display";

// ============================================================================
// Provider state
// ============================================================================

namespace {

IUnityInterfaces       *s_ifaces = nullptr;
IUnityXRDisplayInterface *s_display = nullptr;

UnitySubsystemHandle    s_handle = nullptr;   // from lifecycle callbacks
bool                    s_session_active = false;

// Unity textures wrapping each runtime swapchain image (zero-copy). Indexed by
// swapchain image index; PopulateNextFrameDesc rotates to the acquired index.
UnityXRRenderTextureId  s_tex_ids[8] = {0};
uint32_t                s_tex_count = 0;
bool                    s_textures_created = false;

uint32_t                s_current_image_index = 0;  // acquired this frame
bool                    s_frame_in_flight = false;

void prov_log(const char *msg)
{
	fprintf(stderr, "%s", msg);
}

// OpenXR (right-handed, -Z forward) -> Unity (left-handed, +Z forward).
// Standard conversion: negate Z on position; flip X/Y on the quaternion.
// (Validation point at Editor bring-up — flagged in the design doc.)
inline UnityXRVector3 oxr_to_unity_pos(const float p[3])
{
	return UnityXRVector3{ p[0], p[1], -p[2] };
}
inline UnityXRVector4 oxr_to_unity_quat(const float q[4])
{
	// q = (x,y,z,w) in OpenXR; mirror about the XY plane -> (-x,-y,z,w).
	return UnityXRVector4{ -q[0], -q[1], q[2], q[3] };
}

// ============================================================================
// Acquire Unity's D3D12 device + command queue (graphics thread)
// ============================================================================

bool get_unity_d3d12(ID3D12Device **out_device, ID3D12CommandQueue **out_queue)
{
	*out_device = nullptr;
	*out_queue = nullptr;
	if (!s_ifaces) return false;

	ID3D12Device *dev = nullptr;
	ID3D12CommandQueue *q = nullptr;

	// Prefer the newest interface version Unity 6 registers; fall back down.
	if (IUnityGraphicsD3D12v8 *v8 = s_ifaces->Get<IUnityGraphicsD3D12v8>()) {
		dev = v8->GetDevice();
		q = v8->GetCommandQueue();
	} else if (IUnityGraphicsD3D12v7 *v7 = s_ifaces->Get<IUnityGraphicsD3D12v7>()) {
		dev = v7->GetDevice();
		q = v7->GetCommandQueue();
	} else if (IUnityGraphicsD3D12v6 *v6 = s_ifaces->Get<IUnityGraphicsD3D12v6>()) {
		dev = v6->GetDevice();
		q = v6->GetCommandQueue();
	} else if (IUnityGraphicsD3D12v5 *v5 = s_ifaces->Get<IUnityGraphicsD3D12v5>()) {
		dev = v5->GetDevice();
		q = v5->GetCommandQueue();
	}

	if (!dev) {
		prov_log("[DisplayXR-PROV] No IUnityGraphicsD3D12 device (editor not on D3D12?)\n");
		return false;
	}
	if (!q) {
		// GetCommandQueue can be gated by ConfigureEvent(graphicsQueueAccess).
		// Flagged as a validation point; the runtime needs a queue for D3D12.
		prov_log("[DisplayXR-PROV] WARNING: Unity command queue null (ConfigureEvent gating?)\n");
	}
	*out_device = dev;
	*out_queue = q;
	return true;
}

// ============================================================================
// CreateTexture: wrap each runtime swapchain image as a Unity render texture.
// ============================================================================

void create_textures_if_ready()
{
	if (s_textures_created || !s_display || !s_handle) return;

	uint32_t w = 0, h = 0, arr = 0, imgs = 0;
	dxr_prov_get_swapchain_info(&w, &h, &arr, &imgs);
	if (imgs == 0 || w == 0) return; // swapchain not created yet (await session-ready)

	for (uint32_t i = 0; i < imgs && i < 8; i++) {
		void *native = dxr_prov_get_swapchain_image(i);
		if (!native) { prov_log("[DisplayXR-PROV] swapchain image native ptr null\n"); continue; }

		UnityXRRenderTextureDesc desc;
		memset(&desc, 0, sizeof(desc));
		desc.colorFormat = kUnityXRRenderTextureFormatBGRA32; // matches DXGI 87; 28=RGBA also fine
		desc.color.nativePtr = native;                        // zero-copy: Unity renders into this
		desc.depthFormat = kUnityXRDepthTextureFormat24bitOrGreater;
		desc.depth.nativePtr = (void *)(uintptr_t)kUnityXRRenderTextureIdDontCare; // Unity allocates depth
		desc.width = w;
		desc.height = h;
		desc.textureArrayLength = arr;  // 2 -> SPI texture array
		desc.flags = kUnityXRRenderTextureFlagsUVDirectionTopToBottom; // D3D top-left origin

		UnityXRRenderTextureId id = 0;
		UnitySubsystemErrorCode rc = s_display->CreateTexture(s_handle, &desc, &id);
		if (rc != kUnitySubsystemErrorCodeSuccess) {
			prov_log("[DisplayXR-PROV] CreateTexture failed\n");
			return;
		}
		s_tex_ids[i] = id;
	}
	s_tex_count = imgs;
	s_textures_created = true;
	prov_log("[DisplayXR-PROV] CreateTexture: wrapped runtime swapchain images (zero-copy)\n");
}

void destroy_textures()
{
	if (!s_display || !s_handle) return;
	for (uint32_t i = 0; i < s_tex_count; i++)
		if (s_tex_ids[i]) s_display->DestroyTexture(s_handle, s_tex_ids[i]);
	memset(s_tex_ids, 0, sizeof(s_tex_ids));
	s_tex_count = 0;
	s_textures_created = false;
}

// ============================================================================
// Graphics-thread provider callbacks
// ============================================================================

UnitySubsystemErrorCode UNITY_INTERFACE_API
GfxStart(UnitySubsystemHandle handle, void *userData, UnityXRRenderingCapabilities *caps)
{
	(void)userData;
	s_handle = handle;
	if (caps) {
		caps->noSinglePassRenderingSupport = false;          // SPI supported
		caps->invalidateRenderStateAfterEachCallback = true; // we touch D3D state
		caps->skipPresentToMainScreen = false;
	}

	ID3D12Device *dev = nullptr;
	ID3D12CommandQueue *q = nullptr;
	if (!get_unity_d3d12(&dev, &q)) return kUnitySubsystemErrorCodeFailure;

	// runtime_json = NULL -> resolve from XR_RUNTIME_JSON (sim_display bring-up).
	// overlay_hwnd = NULL -> runtime self-hosts a window (hosted class). The
	// WS_CHILD overlay over Unity's window is an M2 refinement.
	if (!dxr_prov_session_start(nullptr, dev, q, nullptr)) {
		prov_log("[DisplayXR-PROV] dxr_prov_session_start failed\n");
		return kUnitySubsystemErrorCodeFailure;
	}
	s_session_active = true;
	prov_log("[DisplayXR-PROV] GfxStart OK\n");
	return kUnitySubsystemErrorCodeSuccess;
}

UnitySubsystemErrorCode UNITY_INTERFACE_API
GfxPopulateNextFrameDesc(UnitySubsystemHandle handle, void *userData,
                         const UnityXRFrameSetupHints *hints, UnityXRNextFrameDesc *next)
{
	(void)userData; (void)hints;
	s_handle = handle;
	memset(next, 0, sizeof(*next));
	next->renderPassesCount = 0;
	next->mirrorBlitMode = kUnityXRMirrorBlitNone;

	if (!s_session_active) return kUnitySubsystemErrorCodeSuccess;

	dxr_prov_poll_events();
	create_textures_if_ready();
	if (!s_textures_created) return kUnitySubsystemErrorCodeSuccess; // not session-ready yet

	uint32_t img = 0;
	int should_render = 0;
	if (!dxr_prov_begin_frame(&img, &should_render) || !should_render)
		return kUnitySubsystemErrorCodeSuccess;

	s_current_image_index = img;
	s_frame_in_flight = true;

	// Single-Pass-Instanced: 1 render pass, 2 render params over a 2-slice array.
	UnityXRNextFrameDesc::UnityXRRenderPass &pass = next->renderPasses[0];
	pass.textureId = s_tex_ids[img < s_tex_count ? img : 0];
	pass.cullingPassIndex = 0;
	pass.renderParamsCount = 2;

	for (uint32_t eye = 0; eye < 2; eye++) {
		DxrProvView v;
		dxr_prov_get_view(eye, &v);
		UnityXRNextFrameDesc::UnityXRRenderPass::UnityXRRenderParams &rp = pass.renderParams[eye];
		rp.deviceAnchorToEyePose.position = oxr_to_unity_pos(v.position);
		rp.deviceAnchorToEyePose.rotation = oxr_to_unity_quat(v.orientation);
		rp.projection.type = kUnityXRProjectionTypeHalfAngles;
		rp.projection.data.halfAngles.left   = tanf(v.fov[0]);
		rp.projection.data.halfAngles.right  = tanf(v.fov[1]);
		rp.projection.data.halfAngles.top    = tanf(v.fov[2]);
		rp.projection.data.halfAngles.bottom = tanf(v.fov[3]);
		rp.occlusionMeshId = 0;
		rp.textureArraySlice = (int32_t)eye;          // SPI slice
		rp.viewportRect = UnityXRRectf{0.0f, 0.0f, 1.0f, 1.0f};
	}

	// One combined culling pass (use the left eye as the culling viewpoint).
	UnityXRNextFrameDesc::UnityXRCullingPass &cull = next->cullingPasses[0];
	cull.deviceAnchorToCullingPose = pass.renderParams[0].deviceAnchorToEyePose;
	cull.projection = pass.renderParams[0].projection;
	cull.separation = 0.0f;

	next->renderPassesCount = 1;
	return kUnitySubsystemErrorCodeSuccess;
}

UnitySubsystemErrorCode UNITY_INTERFACE_API
GfxSubmitCurrentFrame(UnitySubsystemHandle handle, void *userData)
{
	(void)handle; (void)userData;
	if (!s_session_active || !s_frame_in_flight) return kUnitySubsystemErrorCodeSuccess;
	s_frame_in_flight = false;
	dxr_prov_submit_frame(s_current_image_index);
	return kUnitySubsystemErrorCodeSuccess;
}

UnitySubsystemErrorCode UNITY_INTERFACE_API
GfxStop(UnitySubsystemHandle handle, void *userData)
{
	(void)handle; (void)userData;
	destroy_textures();
	if (s_session_active) {
		dxr_prov_session_stop();
		s_session_active = false;
	}
	prov_log("[DisplayXR-PROV] GfxStop\n");
	return kUnitySubsystemErrorCodeSuccess;
}

// ============================================================================
// Main-thread provider callbacks
// ============================================================================

UnitySubsystemErrorCode UNITY_INTERFACE_API
MainUpdateDisplayState(UnitySubsystemHandle handle, void *userData, UnityXRDisplayState *state)
{
	(void)handle; (void)userData;
	if (state) {
		state->focusLost = false;
		state->displayIsTransparent = false;
		state->contentProtectionEnabled = false;
		state->reprojectionMode = kUnityXRReprojectionModeNone;
		state->nativePtr = nullptr;
	}
	return kUnitySubsystemErrorCodeSuccess;
}

UnitySubsystemErrorCode UNITY_INTERFACE_API
MainQueryMirrorViewBlitDesc(UnitySubsystemHandle handle, void *userData,
                            const UnityXRMirrorViewBlitInfo info, UnityXRMirrorViewBlitDesc *desc)
{
	(void)handle; (void)userData; (void)info;
	if (desc) {
		desc->nativeBlitAvailable = false;
		desc->nativeBlitInvalidStates = false;
		desc->blitParamsCount = 0;   // no mirror blit; the runtime weaves to its window
	}
	return kUnitySubsystemErrorCodeSuccess;
}

// ============================================================================
// Lifecycle (main thread)
// ============================================================================

UnitySubsystemErrorCode UNITY_INTERFACE_API
LifecycleInitialize(UnitySubsystemHandle handle, void *userData)
{
	(void)userData;
	s_handle = handle;
	if (!s_display) return kUnitySubsystemErrorCodeFailure;

	UnityXRDisplayGraphicsThreadProvider gfx;
	memset(&gfx, 0, sizeof(gfx));
	gfx.userData = nullptr;
	gfx.Start = GfxStart;
	gfx.PopulateNextFrameDesc = GfxPopulateNextFrameDesc;
	gfx.SubmitCurrentFrame = GfxSubmitCurrentFrame;
	gfx.Stop = GfxStop;
	gfx.BlitToMirrorViewRenderTarget = nullptr;
	if (s_display->RegisterProviderForGraphicsThread(handle, &gfx) != kUnitySubsystemErrorCodeSuccess)
		return kUnitySubsystemErrorCodeFailure;

	UnityXRDisplayProvider main;
	memset(&main, 0, sizeof(main));
	main.userData = nullptr;
	main.UpdateDisplayState = MainUpdateDisplayState;
	main.QueryMirrorViewBlitDesc = MainQueryMirrorViewBlitDesc;
	if (s_display->RegisterProvider(handle, &main) != kUnitySubsystemErrorCodeSuccess)
		return kUnitySubsystemErrorCodeFailure;

	prov_log("[DisplayXR-PROV] Lifecycle Initialize: providers registered\n");
	return kUnitySubsystemErrorCodeSuccess;
}

UnitySubsystemErrorCode UNITY_INTERFACE_API
LifecycleStart(UnitySubsystemHandle handle, void *userData)
{
	(void)handle; (void)userData;
	prov_log("[DisplayXR-PROV] Lifecycle Start\n");
	return kUnitySubsystemErrorCodeSuccess;
}

void UNITY_INTERFACE_API
LifecycleStop(UnitySubsystemHandle handle, void *userData)
{
	(void)handle; (void)userData;
	prov_log("[DisplayXR-PROV] Lifecycle Stop\n");
}

void UNITY_INTERFACE_API
LifecycleShutdown(UnitySubsystemHandle handle, void *userData)
{
	(void)handle; (void)userData;
	prov_log("[DisplayXR-PROV] Lifecycle Shutdown\n");
}

} // namespace

// ============================================================================
// Registration entry — called from UnityPluginLoad / UnityPluginUnload.
// ============================================================================

extern "C" void displayxr_register_xr_display_provider(IUnityInterfaces *ifaces)
{
	s_ifaces = ifaces;
	if (!s_ifaces) return;
	s_display = s_ifaces->Get<IUnityXRDisplayInterface>();
	if (!s_display) {
		prov_log("[DisplayXR-PROV] IUnityXRDisplayInterface unavailable\n");
		return;
	}

	UnityLifecycleProvider lifecycle;
	memset(&lifecycle, 0, sizeof(lifecycle));
	lifecycle.userData = nullptr;
	lifecycle.Initialize = LifecycleInitialize;
	lifecycle.Start = LifecycleStart;
	lifecycle.Stop = LifecycleStop;
	lifecycle.Shutdown = LifecycleShutdown;

	UnitySubsystemErrorCode rc =
	    s_display->RegisterLifecycleProvider(k_plugin_name, k_display_id, &lifecycle);
	prov_log(rc == kUnitySubsystemErrorCodeSuccess
	             ? "[DisplayXR-PROV] RegisterLifecycleProvider OK\n"
	             : "[DisplayXR-PROV] RegisterLifecycleProvider FAILED (manifest mismatch?)\n");
}

extern "C" void displayxr_unregister_xr_display_provider(void)
{
	s_display = nullptr;
	s_ifaces = nullptr;
}
