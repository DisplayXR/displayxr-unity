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

// Hook-path Win32 helper (displayxr_win32.c): finds Unity's main HWND and
// creates a WS_CHILD overlay sized to its client area, subclassing Unity's
// wndproc so the overlay tracks Unity's window on resize. In provider mode the
// OpenXR hook is NOT installed, so the provider is the sole caller — the runtime
// weaves into this overlay (in-app weave) and per-view resolution tracks the
// overlay's live client size (displayxr_get_overlay_size).
extern "C" void *displayxr_get_app_main_view(void);
// (#166) Make the overlay a top-level WS_POPUP+NOREDIRECTIONBITMAP (composites the
// runtime DComp weave) instead of a WS_CHILD. Call before displayxr_get_app_main_view().
extern "C" void displayxr_set_provider_opaque_overlay(int enabled);

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

// WS_CHILD overlay HWND over Unity's window, created on the MAIN thread in
// LifecycleStart (NOT in GfxStart). Creating a WS_CHILD of Unity's main-thread
// window from the render thread attaches the two threads' input queues and
// deadlocks (Unity's main thread is blocked waiting for GfxStart to return).
// The hook path avoids this because it creates the overlay during xrCreateSession
// on the main thread. NULL → runtime self-hosts a window (fallback).
void                   *s_overlay_hwnd = nullptr;

// Unity textures wrapping each runtime swapchain image (zero-copy). Indexed by
// swapchain image index; PopulateNextFrameDesc rotates to the acquired index.
UnityXRRenderTextureId  s_tex_ids[8] = {0};
uint32_t                s_tex_count = 0;
bool                    s_textures_created = false;

uint32_t                s_current_image_index = 0;  // acquired this frame
bool                    s_frame_in_flight = false;

extern "C" void dxr_prov_file_log(const char *s); // defined in the session TU

void prov_log(const char *msg)
{
	fprintf(stderr, "%s", msg);
	OutputDebugStringA(msg);
	dxr_prov_file_log(msg);
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

	uint32_t w = 0, h = 0, arr = 0;
	void *bridge = dxr_prov_get_bridge_unity_texture(&w, &h, &arr);
	if (!bridge || w == 0) return; // bridge not created yet (await session-ready)

	// One Unity texture wrapping the shared 2-slice-array BRIDGE. Unity renders
	// both eyes into it; the provider copies it into the runtime swapchain each
	// frame (dxr_prov_submit_frame). Cross-device-safe (NT-handle shared resource).
	UnityXRRenderTextureDesc desc;
	memset(&desc, 0, sizeof(desc));
	desc.colorFormat = kUnityXRRenderTextureFormatRGBA32; // bridge is R8G8B8A8_UNORM (fmt 28)
	desc.color.nativePtr = bridge;
	desc.depthFormat = kUnityXRDepthTextureFormat24bitOrGreater;
	desc.depth.nativePtr = (void *)(uintptr_t)kUnityXRRenderTextureIdDontCare; // Unity allocates depth
	// Unity allocates a matched 2-slice depth array (verified via QueryTextureDesc).
	desc.width = w;
	desc.height = h;
	desc.textureArrayLength = arr;  // 2 -> SPI texture array
	desc.flags = kUnityXRRenderTextureFlagsUVDirectionTopToBottom; // D3D top-left origin

	UnityXRRenderTextureId id = 0;
	UnitySubsystemErrorCode rc = s_display->CreateTexture(s_handle, &desc, &id);
	if (rc != kUnitySubsystemErrorCodeSuccess) {
		prov_log("[DisplayXR-PROV] CreateTexture (bridge) failed\n");
		return;
	}
	s_tex_ids[0] = id;
	s_tex_count = 1;
	s_textures_created = true;
	prov_log("[DisplayXR-PROV] CreateTexture: wrapped shared bridge (2-slice array)\n");
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

	// Weave target. Default = runtime self-hosts its own window (the M1b known-good
	// baseline, validated on the panel). Set DISPLAYXR_PROV_OVERLAY=1 to instead
	// bind to the WS_CHILD overlay over Unity's window (in-app weave) created on
	// the main thread in LifecycleStart — that path's D3D12 swapchain present on a
	// WS_CHILD is still being brought up (a child window doesn't composite a D3D12
	// flip swapchain the way the hook path's top-level WS_POPUP does), so it is
	// opt-in until that window-side issue is resolved.
	const char *use_overlay = getenv("DISPLAYXR_PROV_OVERLAY");
	void *overlay_hwnd = (use_overlay && use_overlay[0] == '1') ? s_overlay_hwnd : nullptr;
	prov_log(overlay_hwnd
	             ? "[DisplayXR-PROV] weave target: top-level WS_POPUP overlay (in-app)\n"
	             : "[DisplayXR-PROV] weave target: runtime self-hosted window\n");

	// runtime_json = NULL -> resolve from XR_RUNTIME_JSON (sim_display bring-up).
	if (!dxr_prov_session_start(nullptr, dev, q, overlay_hwnd)) {
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

	// Single-Pass-Instanced: 1 render pass, 2 render params over the 2-slice bridge
	// array (slice 0 = left, slice 1 = right). (img drives the swapchain copy
	// destination in SubmitCurrentFrame.) NOTE: SPI renders opaque geometry wrong on
	// BiRP (skybox survives, geometry doesn't — Unity's per-eye projection there is
	// MultiPass-only); SPI is correct on URP+D3D12. BiRP MultiPass-into-array was
	// tried and rendered worse — proper BiRP support is a follow-up (#166).
	UnityXRNextFrameDesc::UnityXRRenderPass &pass = next->renderPasses[0];
	pass.textureId = s_tex_ids[0];
	pass.cullingPassIndex = 0;
	pass.renderParamsCount = 2;

	uint32_t scW = 0, scH = 0, scArr = 0, scImgs = 0;
	dxr_prov_get_swapchain_info(&scW, &scH, &scArr, &scImgs);
	uint32_t rW = 0, rH = 0;
	dxr_prov_get_render_rect(&rW, &rH);
	float vpW = (scW > 0 && rW > 0) ? (float)rW / (float)scW : 1.0f;
	float vpH = (scH > 0 && rH > 0) ? (float)rH / (float)scH : 1.0f;
	if (vpW > 1.0f) vpW = 1.0f;
	if (vpH > 1.0f) vpH = 1.0f;

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
		rp.viewportRect = UnityXRRectf{0.0f, 0.0f, vpW, vpH};
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

	// Cross-device handoff: Unity just rendered both eyes into the shared bridge
	// (leaving it in RENDER_TARGET on Unity's device). Ask Unity to transition it
	// to COMMON in its active command list so our SEPARATE device can read it
	// coherently across the device boundary (a D3D12 shared resource must be in
	// COMMON at a cross-device sync point). Without this the own-device copy reads
	// incoherent/empty memory → black. Pairs with the shared sync fence.
	uint32_t bw = 0, bh = 0, ba = 0;
	void *bridge = dxr_prov_get_bridge_unity_texture(&bw, &bh, &ba);
	if (bridge) {
		if (IUnityGraphicsD3D12v8 *v8 = s_ifaces->Get<IUnityGraphicsD3D12v8>())
			v8->RequestResourceState((ID3D12Resource *)bridge, D3D12_RESOURCE_STATE_COMMON);
	}

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
	// In-app weave (DISPLAYXR_PROV_OVERLAY=1): create a TOP-LEVEL WS_POPUP overlay
	// over Unity's window HERE — this is the MAIN thread. (Creating it in GfxStart,
	// the render thread, deadlocks: attaching to Unity's main-thread window joins
	// the input queues while the main thread waits on GfxStart.) GfxStart binds the
	// runtime to s_overlay_hwnd. Default (env unset) = runtime self-hosts its own
	// window (the M1b baseline). The top-level popup composites the runtime's DComp
	// weave; a WS_CHILD does not (#166).
	const char *use_overlay = getenv("DISPLAYXR_PROV_OVERLAY");
	if (use_overlay && use_overlay[0] == '1') {
		displayxr_set_provider_opaque_overlay(1);
		s_overlay_hwnd = displayxr_get_app_main_view();
		prov_log(s_overlay_hwnd
		             ? "[DisplayXR-PROV] Lifecycle Start (top-level overlay created on main thread)\n"
		             : "[DisplayXR-PROV] Lifecycle Start (overlay create FAILED; runtime self-hosts)\n");
	} else {
		s_overlay_hwnd = nullptr;
		prov_log("[DisplayXR-PROV] Lifecycle Start (no overlay; runtime self-hosts)\n");
	}
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
