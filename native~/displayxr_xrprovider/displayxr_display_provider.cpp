// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
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

#ifdef _WIN32
#include <windows.h>
#include <d3d12.h>
#include <d3d11.h> // D3D11 zero-copy backend (#195)
#include <dxgi1_4.h>
#endif

#include "../unity_pluginapi/IUnityInterface.h"
#include "../unity_pluginapi/IUnityGraphics.h"
#ifdef _WIN32
#include "../unity_pluginapi/IUnityGraphicsD3D12.h"
#include "../unity_pluginapi/IUnityGraphicsD3D11.h" // #195
#endif
#ifdef __APPLE__
#include "displayxr_provider_gfx_metal.h" // Metal device/queue glue (#204)
#include "../displayxr_metal.h"           // provider weave window (SA-era preview window)
// Zero-copy per-image per-eye slice view (defined in the session TU, #204).
extern "C" void *dxr_prov_get_metal_eye_view(uint32_t image, uint32_t eye);
#endif
#include "../unity_pluginapi/IUnityXRDisplay.h"

#include "displayxr_provider_session.h"

// Renderer id (IUnityGraphics::GetRenderer()) — defined in displayxr_unity_plugin.cpp.
// -1 if IUnityGraphics isn't available. Used to select the graphics backend (#195).
extern "C" int displayxr_unity_get_renderer(void);

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Cross-platform weave-target helper: on Windows (displayxr_win32.c) it creates
// the top-level overlay HWND tracking Unity's window; on macOS (displayxr_metal.m)
// it returns a passthrough CAMetalLayer NSView added to Unity's window. In provider
// mode the OpenXR hook is NOT installed, so the provider is the sole caller — the
// runtime weaves into this surface (in-app weave).
extern "C" void *displayxr_get_app_main_view(void);
#ifdef _WIN32
// (#166) Make the overlay a top-level WS_POPUP+NOREDIRECTIONBITMAP (composites the
// runtime DComp weave) instead of a WS_CHILD. Call before displayxr_get_app_main_view().
extern "C" void displayxr_set_provider_opaque_overlay(int enabled);
// (#166) Keyboard input: the focus / raw-input hooks the transparent & shell paths
// use so Unity receives input while not the OS-foreground window — IAT-hooks
// GetForegroundWindow/GetFocus → Unity's HWND (Application.isFocused stays true),
// adds RIDEV_INPUTSINK so raw input (WM_INPUT, the Input System's keyboard/mouse
// source) flows to Unity in the background, and subclasses Unity's wndproc to
// suppress deactivation. Idempotent.
extern "C" int   displayxr_install_focus_hook(void *unity_hwnd);
extern "C" void *displayxr_get_unity_main_hwnd(void);
// (#173) Create a DEDICATED standalone weave window (editor Play Mode) — a movable
// top-level window that does NOT track Unity's (whole-editor) window, bound as the
// runtime's weave target. See displayxr_win32.c.
extern "C" void *displayxr_create_provider_dedicated_window(void);
// (#173) Destroy that window on teardown so it doesn't linger frozen after Play stop.
extern "C" void  displayxr_destroy_provider_dedicated_window(void);
#endif

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

// Extra 3D zones (#166 Phase B2): each extra zone gets its own Unity texture(s)
// (SPI: [i][0] = 2-slice array; MultiPass: [i][0/1] = per-eye) + render pass.
#define PROV_MAX_EXTRA_ZONES 3   // PS_MAX_ZONES - 1
UnityXRRenderTextureId  s_extra_tex_ids[PROV_MAX_EXTRA_ZONES][2] = {};
bool                    s_extra_tex_created[PROV_MAX_EXTRA_ZONES] = {};

uint32_t                s_current_image_index = 0;  // acquired this frame
bool                    s_frame_in_flight = false;

extern "C" void dxr_prov_file_log(const char *s); // defined in the session TU

void prov_log(const char *msg)
{
	fprintf(stderr, "%s", msg);
#ifdef _WIN32
	OutputDebugStringA(msg);
#endif
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

// Fill deviceAnchorToEyePose (Unity frame) with the located view expressed RELATIVE
// to the sent rig pose: rigPose^-1 * eyePose, computed in the OpenXR frame, then
// converted. The located views already bake the rig pose in (the runtime owns the
// Kooima math and we chain rig_pose onto xrLocateViews), and Unity re-composes the
// SAME rig pose via the camera transform (deviceAnchor == cam.transform == the pose
// we sent). Without subtracting it here the rig origin is applied TWICE — the render
// eye drifts by the rig displacement, and the URP foreground-clip plane (built from
// the single-application cam.transform) no longer matches, so the clip stops tracking
// the moving display plane (#166). If no rig pose is set, pass the view through.
inline void set_eye_pose_rig_relative(
	UnityXRNextFrameDesc::UnityXRRenderPass::UnityXRRenderParams &rp, const DxrProvView &v)
{
	float rp_pos[3], rp_quat[4];
	if (!dxr_prov_get_display_pose_oxr(rp_pos, rp_quat)) {
		rp.deviceAnchorToEyePose.position = oxr_to_unity_pos(v.position);
		rp.deviceAnchorToEyePose.rotation = oxr_to_unity_quat(v.orientation);
		return;
	}
	// invRot = conjugate(rigRot)
	const float qx = -rp_quat[0], qy = -rp_quat[1], qz = -rp_quat[2], qw = rp_quat[3];
	// d = eyePos - rigPos
	const float dx = v.position[0] - rp_pos[0];
	const float dy = v.position[1] - rp_pos[1];
	const float dz = v.position[2] - rp_pos[2];
	// relPos = invRot * d  (rotate vector d by quaternion (qx,qy,qz,qw))
	const float tx = 2.0f * (qy * dz - qz * dy);
	const float ty = 2.0f * (qz * dx - qx * dz);
	const float tz = 2.0f * (qx * dy - qy * dx);
	float rel_pos[3] = {
		dx + qw * tx + (qy * tz - qz * ty),
		dy + qw * ty + (qz * tx - qx * tz),
		dz + qw * tz + (qx * ty - qy * tx),
	};
	// relRot = invRot * eyeRot  (quaternion product)
	const float ex = v.orientation[0], ey = v.orientation[1], ez = v.orientation[2], ew = v.orientation[3];
	float rel_quat[4] = {
		qw * ex + qx * ew + qy * ez - qz * ey, // x
		qw * ey - qx * ez + qy * ew + qz * ex, // y
		qw * ez + qx * ey - qy * ex + qz * ew, // z
		qw * ew - qx * ex - qy * ey - qz * ez, // w
	};
	rp.deviceAnchorToEyePose.position = oxr_to_unity_pos(rel_pos);
	rp.deviceAnchorToEyePose.rotation = oxr_to_unity_quat(rel_quat);
}

// ============================================================================
// Acquire Unity's D3D12 device + command queue (graphics thread)
// ============================================================================

#ifdef _WIN32
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

// Acquire Unity's ID3D11Device for the zero-copy D3D11 backend (#195). There is no
// versioned interface set and no command queue for D3D11 (immediate-context model) —
// a single IUnityGraphicsD3D11::GetDevice() is all the runtime needs.
bool get_unity_d3d11(ID3D11Device **out_device)
{
	*out_device = nullptr;
	if (!s_ifaces) return false;
	IUnityGraphicsD3D11 *d11 = s_ifaces->Get<IUnityGraphicsD3D11>();
	ID3D11Device *dev = d11 ? d11->GetDevice() : nullptr;
	if (!dev) {
		prov_log("[DisplayXR-PROV] No IUnityGraphicsD3D11 device\n");
		return false;
	}
	*out_device = dev;
	return true;
}
#endif // _WIN32

// ============================================================================
// CreateTexture: wrap each runtime swapchain image as a Unity render texture.
// ============================================================================

// Wrap the extra 3D zones' bridges as Unity textures (once each). #166 Phase B2.
static void create_extra_zone_textures()
{
	uint32_t ez = dxr_prov_get_extra_zone_count();
	bool sp = dxr_prov_get_single_pass();
	for (uint32_t i = 0; i < ez && i < PROV_MAX_EXTRA_ZONES; i++) {
		if (s_extra_tex_created[i]) continue;
		if (sp) {
			uint32_t w = 0, h = 0;
			void *b = dxr_prov_get_extra_zone_bridge(i, &w, &h);
			if (!b || w == 0) continue;
			UnityXRRenderTextureDesc desc; memset(&desc, 0, sizeof(desc));
			desc.colorFormat = kUnityXRRenderTextureFormatRGBA32;
			desc.color.nativePtr = b;
			desc.depthFormat = kUnityXRDepthTextureFormat24bitOrGreater;
			desc.depth.nativePtr = (void *)(uintptr_t)kUnityXRRenderTextureIdDontCare;
			desc.width = w; desc.height = h; desc.textureArrayLength = 2;
			desc.flags = kUnityXRRenderTextureFlagsUVDirectionTopToBottom;
			UnityXRRenderTextureId id = 0;
			if (s_display->CreateTexture(s_handle, &desc, &id) == kUnitySubsystemErrorCodeSuccess) {
				s_extra_tex_ids[i][0] = id; s_extra_tex_created[i] = true;
				prov_log("[DisplayXR-PROV] extra zone: CreateTexture (SPI 2-slice) OK\n");
			}
		} else {
			uint32_t w = 0, h = 0;
			void *bl = dxr_prov_get_extra_zone_bridge_eye(i, 0, &w, &h);
			void *br = dxr_prov_get_extra_zone_bridge_eye(i, 1, &w, &h);
			if (!bl || !br || w == 0) continue;
			bool ok = true;
			for (uint32_t eye = 0; eye < 2; eye++) {
				UnityXRRenderTextureDesc desc; memset(&desc, 0, sizeof(desc));
				desc.colorFormat = kUnityXRRenderTextureFormatRGBA32;
				desc.color.nativePtr = (eye == 0) ? bl : br;
				desc.depthFormat = kUnityXRDepthTextureFormat24bitOrGreater;
				desc.depth.nativePtr = (void *)(uintptr_t)kUnityXRRenderTextureIdDontCare;
				desc.width = w; desc.height = h; desc.textureArrayLength = 1;
				desc.flags = kUnityXRRenderTextureFlagsUVDirectionTopToBottom;
				UnityXRRenderTextureId id = 0;
				if (s_display->CreateTexture(s_handle, &desc, &id) != kUnitySubsystemErrorCodeSuccess) { ok = false; break; }
				s_extra_tex_ids[i][eye] = id;
			}
			if (ok) { s_extra_tex_created[i] = true; prov_log("[DisplayXR-PROV] extra zone: CreateTexture (MP 2-tex) OK\n"); }
		}
	}
}

void create_textures_if_ready()
{
	if (!s_display || !s_handle) return;
	// Extra zones can come up alongside / after the primary; keep trying until wrapped.
	if (s_textures_created) { create_extra_zone_textures(); return; }

#ifdef __APPLE__
	// Metal ZERO-COPY MultiPass (#204): wrap each swapchain image's per-eye
	// slice views directly (image i, eye e → s_tex_ids[i*2+e]); Unity renders
	// each eye straight into the acquired image's slice. No bridge, no blit —
	// PopulateNextFrameDesc rotates to the acquired image index each frame,
	// the Metal cousin of the D3D11 zero-copy image rotation below.
	if (dxr_prov_get_graphics_api() == DXR_GFX_METAL) {
		uint32_t sw = 0, sh = 0, sarr = 0, simgs = 0;
		dxr_prov_get_swapchain_info(&sw, &sh, &sarr, &simgs);
		if (simgs == 0 || sw == 0) return; // swapchain not created yet
		if (simgs > 4) simgs = 4;          // s_tex_ids capacity: 4 images x 2 eyes
		for (uint32_t i = 0; i < simgs; i++) {
			for (uint32_t e = 0; e < 2; e++) {
				void *view = dxr_prov_get_metal_eye_view(i, e);
				if (!view) return; // views not ready yet
				UnityXRRenderTextureDesc desc;
				memset(&desc, 0, sizeof(desc));
				desc.colorFormat = kUnityXRRenderTextureFormatRGBA32;
				desc.color.nativePtr = view;
				// DISPLAYXR_METAL_NO_DEPTH=1: crash-triage lever — skip the paired
				// Unity depth allocation for the wrapped slice views (#204).
				desc.depthFormat = getenv("DISPLAYXR_METAL_NO_DEPTH")
				                       ? kUnityXRDepthTextureFormatNone
				                       : kUnityXRDepthTextureFormat24bitOrGreater;
				desc.depth.nativePtr = (void *)(uintptr_t)kUnityXRRenderTextureIdDontCare;
				desc.width = sw;
				desc.height = sh;
				desc.textureArrayLength = 1; // single-slice view per eye
				desc.flags = kUnityXRRenderTextureFlagsUVDirectionTopToBottom;
				UnityXRRenderTextureId id = 0;
				if (s_display->CreateTexture(s_handle, &desc, &id) != kUnitySubsystemErrorCodeSuccess) {
					prov_log("[DisplayXR-PROV] CreateTexture (Metal slice view) failed\n");
					return;
				}
				s_tex_ids[i * 2 + e] = id;
			}
		}
		s_tex_count = simgs * 2;
		s_textures_created = true;
		prov_log("[DisplayXR-PROV] CreateTexture: wrapped swapchain slice views (Metal zero-copy MultiPass)\n");
		return;
	}
#endif

	// D3D11 zero-copy SPI (#195): wrap the runtime's swapchain images DIRECTLY (no bridge).
	// Each image is a 2-slice SPI array on Unity's device; wrap ALL of them and
	// PopulateNextFrameDesc selects s_tex_ids[acquired image index] each frame (the
	// runtime rotates images, unlike the D3D12 path's single fixed bridge).
	// EDITOR bridge (#195): the runtime images live on the OWN device — fall through to
	// the SPI bridge arm below (wraps the single Unity-side bridge tex, like D3D12 SPI).
	// D3D11 MultiPass (BiRP, #195): falls through to the MultiPass arm below, which wraps the
	// two per-eye targets via dxr_prov_get_bridge_unity_texture_eye (D3D11-aware) — both
	// sub-modes (zero-copy plain Unity textures / editor shared bridge).
	if (dxr_prov_get_graphics_api() == DXR_GFX_D3D11 && !dxr_prov_d3d11_bridge_active()
	    && dxr_prov_get_single_pass()) {
		uint32_t sw = 0, sh = 0, sarr = 0, simgs = 0;
		dxr_prov_get_swapchain_info(&sw, &sh, &sarr, &simgs);
		if (simgs == 0 || sw == 0) return; // swapchain not created yet (await session-ready)
		if (simgs > 8) simgs = 8;
		for (uint32_t i = 0; i < simgs; i++) {
			uint32_t w = 0, h = 0, arr = 0;
			void *tex = dxr_prov_get_swapchain_image_texture(i, &w, &h, &arr);
			if (!tex || w == 0) return; // image not ready
			UnityXRRenderTextureDesc desc;
			memset(&desc, 0, sizeof(desc));
			desc.colorFormat = kUnityXRRenderTextureFormatRGBA32;
			desc.color.nativePtr = tex;
			desc.depthFormat = kUnityXRDepthTextureFormat24bitOrGreater;
			desc.depth.nativePtr = (void *)(uintptr_t)kUnityXRRenderTextureIdDontCare;
			desc.width = w;
			desc.height = h;
			desc.textureArrayLength = arr; // 2 -> SPI texture array
			desc.flags = kUnityXRRenderTextureFlagsUVDirectionTopToBottom;
			UnityXRRenderTextureId id = 0;
			if (s_display->CreateTexture(s_handle, &desc, &id) != kUnitySubsystemErrorCodeSuccess) {
				prov_log("[DisplayXR-PROV] CreateTexture (D3D11 zero-copy swapchain image) failed\n");
				return;
			}
			s_tex_ids[i] = id;
		}
		s_tex_count = simgs;
		s_textures_created = true;
		prov_log("[DisplayXR-PROV] CreateTexture: wrapped D3D11 swapchain images directly (zero-copy SPI)\n");
		return;
	}

	if (dxr_prov_get_single_pass()) {
		// SPI: ONE Unity texture wrapping the shared 2-slice-array BRIDGE. Unity
		// renders both eyes into it (slices 0/1); the provider copies it into the
		// runtime swapchain each frame. Cross-device-safe (NT-handle shared resource).
		uint32_t w = 0, h = 0, arr = 0;
		void *bridge = dxr_prov_get_bridge_unity_texture(&w, &h, &arr);
		if (!bridge || w == 0) return; // bridge not created yet (await session-ready)

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
		prov_log("[DisplayXR-PROV] CreateTexture: wrapped shared bridge (SPI 2-slice array)\n");
	} else {
		// MultiPass: TWO Unity textures, one per eye, each wrapping a separate
		// single-slice BRIDGE (textureArrayLength=1). One texture per render pass —
		// the spec-correct MultiPass topology (textureArraySlice is SPI-only).
		uint32_t w0 = 0, h0 = 0;
		void *bl = dxr_prov_get_bridge_unity_texture_eye(0, &w0, &h0);
		void *br = dxr_prov_get_bridge_unity_texture_eye(1, &w0, &h0);
		if (!bl || !br || w0 == 0) return; // bridges not created yet (await session-ready)

		for (uint32_t eye = 0; eye < 2; eye++) {
			UnityXRRenderTextureDesc desc;
			memset(&desc, 0, sizeof(desc));
			desc.colorFormat = kUnityXRRenderTextureFormatRGBA32;
			desc.color.nativePtr = (eye == 0) ? bl : br;
			desc.depthFormat = kUnityXRDepthTextureFormat24bitOrGreater;
			desc.depth.nativePtr = (void *)(uintptr_t)kUnityXRRenderTextureIdDontCare;
			desc.width = w0;
			desc.height = h0;
			desc.textureArrayLength = 1;  // single-slice per eye
			desc.flags = kUnityXRRenderTextureFlagsUVDirectionTopToBottom;

			UnityXRRenderTextureId id = 0;
			if (s_display->CreateTexture(s_handle, &desc, &id) != kUnitySubsystemErrorCodeSuccess) {
				prov_log("[DisplayXR-PROV] CreateTexture (MultiPass eye) failed\n");
				return;
			}
			s_tex_ids[eye] = id;
		}
		s_tex_count = 2;
		s_textures_created = true;
		prov_log("[DisplayXR-PROV] CreateTexture: wrapped per-eye bridges (MultiPass, 2 textures)\n");
	}
}

void destroy_textures()
{
	if (!s_display || !s_handle) return;
	prov_log("[DisplayXR-PROV] destroy_textures: dropping wrapped Unity textures (rewrap)\n");
	for (uint32_t i = 0; i < s_tex_count; i++)
		if (s_tex_ids[i]) s_display->DestroyTexture(s_handle, s_tex_ids[i]);
	memset(s_tex_ids, 0, sizeof(s_tex_ids));
	s_tex_count = 0;
	s_textures_created = false;
}

// Drop one extra zone's Unity texture(s) after a live realloc (#172) so
// create_extra_zone_textures re-wraps the zone's fresh bridge next frame.
static void destroy_extra_zone_texture(uint32_t i)
{
	if (!s_display || !s_handle || i >= PROV_MAX_EXTRA_ZONES) return;
	for (int e = 0; e < 2; e++)
		if (s_extra_tex_ids[i][e]) { s_display->DestroyTexture(s_handle, s_extra_tex_ids[i][e]); s_extra_tex_ids[i][e] = 0; }
	s_extra_tex_created[i] = false;
}

// Weave-target policy (#166). A Unity DisplayXR app owns its window like a native
// handle app: the runtime weaves into a top-level overlay that tracks the app's
// window (in-app, single-window UX, keyboard/mouse route to Unity via the focus
// hook). This is the DEFAULT and the only supported shipping model.
//
// Self-host (runtime hosts its OWN window, windowHandle=NULL) is a bring-up /
// diagnostic fallback only — opt in with DISPLAYXR_PROV_SELFHOST=1. Self-host has
// no real window geometry (tracking origin floats to standing height) and leaves
// Unity's window non-foreground (keyboard doesn't reach the Input System), so it
// is NOT an app deployment mode. Returns 1 for the app-owned window, 0 for self-host.
static int
prov_want_app_window(void)
{
	const char *self_host = getenv("DISPLAYXR_PROV_SELFHOST");
	return (self_host && self_host[0] == '1') ? 0 : 1;
}

// Dedicated-window weave target (#173) — editor Play Mode. Prefer the C#-driven
// flag (the loader sets dxr_prov_set_dedicated_window(1) when Application.isEditor,
// before the subsystem starts); env fallback DISPLAYXR_PROV_EDITOR_WINDOW=1. When
// set, the provider creates its OWN standalone movable weave window (coexists with
// the editor) instead of the app-owned overlay that tracks Unity's window. Built
// players never set it → they keep the overlay default. Takes precedence over
// self-host: if both are somehow set, the dedicated window wins.
static int
prov_want_dedicated_window(void)
{
	if (dxr_prov_get_dedicated_window()) return 1;
	const char *env = getenv("DISPLAYXR_PROV_EDITOR_WINDOW");
	return (env && env[0] == '1') ? 1 : 0;
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
		// Match the gated render mode (set from C# before the subsystem starts):
		// SPI on URP+Win+D3D12, MultiPass on BiRP/other (SPI renders opaque geometry
		// wrong on BiRP). noSinglePassRenderingSupport=true tells Unity to use
		// MultiPass; false allows the 1-pass-x-2 instanced path.
		caps->noSinglePassRenderingSupport = dxr_prov_get_single_pass() ? false : true;
#ifdef _WIN32
		caps->invalidateRenderStateAfterEachCallback = true; // we touch D3D state
		caps->skipPresentToMainScreen = false;
#else
		// macOS zero-copy touches NO Unity render state in the callbacks (no
		// provider command buffers at all), and the invalidation path is what
		// churns Metal render surfaces around each callback (#204 crash triage).
		caps->invalidateRenderStateAfterEachCallback = false;
		// macOS (#204): skip Unity's present-to-main-screen. With it on, Unity's
		// Metal GameView drawable surfaces churn against the in-process weaver's
		// presents and Unity SEGVs destroying a drawable-backed RenderSurface
		// whose texture is nil (DestroyRenderSurfaceDesc, NULL vtable load —
		// lldb-verified during Phase 2 bring-up; the Metal cousin of the D3D11
		// GameView-present/weaver contention that forced the own-device bridge).
		// The weave window is the preview surface; the GameView shows nothing
		// XR-driven on macOS today (mirror blit is a Phase 4 topic, #206).
		caps->skipPresentToMainScreen = true;
#endif
	}
	prov_log(dxr_prov_get_single_pass()
	             ? "[DisplayXR-PROV] GfxStart: render mode = Single-Pass-Instanced\n"
	             : "[DisplayXR-PROV] GfxStart: render mode = MultiPass (2 pass x 1)\n");

	// Select the graphics backend from Unity's renderer (#195): D3D12 (own-device +
	// shared bridge) or D3D11 (zero-copy on Unity's device) on Windows. Anything else →
	// one clear WARN + graceful no-start (instead of the confusing "ID3D12Device is not
	// created yet"). macOS/Metal lands with the Metal backend (#204); until then Metal
	// hits the WARN arm below with a pointer at the tracking issue.
	int renderer = displayxr_unity_get_renderer();
	int backend_kind = DXR_GFX_NONE;
	void *dev_ptr = nullptr;
	void *queue_ptr = nullptr;
#ifdef _WIN32
	if (renderer == kUnityGfxRendererD3D12) {
		ID3D12Device *dev = nullptr; ID3D12CommandQueue *q = nullptr;
		if (!get_unity_d3d12(&dev, &q)) return kUnitySubsystemErrorCodeFailure;
		backend_kind = DXR_GFX_D3D12; dev_ptr = dev; queue_ptr = q;
	} else if (renderer == kUnityGfxRendererD3D11) {
		ID3D11Device *dev = nullptr;
		if (!get_unity_d3d11(&dev)) return kUnitySubsystemErrorCodeFailure;
		backend_kind = DXR_GFX_D3D11; dev_ptr = dev; queue_ptr = nullptr;
	} else
#elif defined(__APPLE__)
	if (renderer == kUnityGfxRendererMetal) {
		// Metal (#204): the session binds a provider-created MTLCommandQueue on
		// UNITY'S MTLDevice (client-queue model — the runtime's in-process Metal
		// compositor encodes on it, like the D3D11 zero-copy binding on Windows).
		void *dev = dxr_prov_metal_unity_device();
		void *q = dev ? dxr_prov_metal_create_session_queue() : nullptr;
		if (!dev || !q) {
			prov_log("[DisplayXR-PROV] Metal: no Unity MTLDevice/queue — session not started (#204)\n");
			return kUnitySubsystemErrorCodeFailure;
		}
		backend_kind = DXR_GFX_METAL; dev_ptr = dev; queue_ptr = q;
	} else
#endif
	{
		char msg[256];
#ifdef _WIN32
		snprintf(msg, sizeof(msg),
		         "[DisplayXR-PROV] WARN: unsupported graphics API (renderer=%d). The DisplayXR "
		         "provider requires Direct3D11 or Direct3D12 — session not started (#195).\n", renderer);
#else
		snprintf(msg, sizeof(msg),
		         "[DisplayXR-PROV] WARN: unsupported graphics API (renderer=%d). The DisplayXR "
		         "provider requires Metal on macOS — session not started (#202/#204).\n", renderer);
#endif
		prov_log(msg);
		return kUnitySubsystemErrorCodeFailure;
	}

	// Weave target. Three modes (window created on the MAIN thread in LifecycleStart,
	// which sets s_overlay_hwnd for the bound-HWND modes):
	//   - Dedicated window (#173, editor Play Mode): a standalone movable window that
	//     does NOT track Unity → bind s_overlay_hwnd.
	//   - App-owned overlay (default, built players): a top-level WS_POPUP overlay
	//     tracking Unity's window → bind s_overlay_hwnd.
	//   - Self-host (DISPLAYXR_PROV_SELFHOST=1, bring-up diagnostic): runtime hosts its
	//     own window → windowHandle=NULL.
	// If a bound-HWND mode's window failed to create, self-host is the safe fallback
	// (s_overlay_hwnd stays NULL).
	bool want_bound = prov_want_dedicated_window() || prov_want_app_window();
	void *overlay_hwnd = want_bound ? s_overlay_hwnd : nullptr;
	prov_log(overlay_hwnd
	             ? (prov_want_dedicated_window()
	                    ? "[DisplayXR-PROV] weave target: dedicated provider window (editor, #173)\n"
	                    : "[DisplayXR-PROV] weave target: app-owned top-level overlay (in-app)\n")
	             : "[DisplayXR-PROV] weave target: runtime self-hosted window (SELFHOST diagnostic)\n");

	// runtime_json = NULL -> resolve from XR_RUNTIME_JSON (sim_display bring-up).
	if (!dxr_prov_session_start(nullptr, backend_kind, dev_ptr, queue_ptr, overlay_hwnd)) {
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

#ifdef _WIN32
	dxr_prov_poll_events();
#else
	// macOS (#204): xrPollEvent is pumped from the MAIN thread instead — the
	// runtime's oxr_macos_pump_events drains NSApp events + flushes CATransaction,
	// both main-thread-only (AppKit throws off-main). DisplayXRProviderDriver
	// calls dxr_prov_poll_events() every LateUpdate on macOS.
#endif
	// Live tile realloc (#172): if the window/zone target size changed, the session
	// recreates the swapchain+bridge here (between frames). Drop the stale Unity
	// textures wrapping the old bridge so create_textures_if_ready rewraps the new one.
	// The primary is signalled by the return; each reallocated extra zone via its latch.
	if (dxr_prov_reconcile_size()) destroy_textures();
	for (uint32_t z = 0; z < PROV_MAX_EXTRA_ZONES; z++)
		if (dxr_prov_consume_zone_rewrap(z)) destroy_extra_zone_texture(z);
	create_textures_if_ready();
	if (!s_textures_created) return kUnitySubsystemErrorCodeSuccess; // not session-ready yet

	uint32_t img = 0;
	int should_render = 0;
	if (!dxr_prov_begin_frame(&img, &should_render) || !should_render)
		return kUnitySubsystemErrorCodeSuccess;

	s_current_image_index = img;
	s_frame_in_flight = true;

	uint32_t scW = 0, scH = 0, scArr = 0, scImgs = 0;
	dxr_prov_get_swapchain_info(&scW, &scH, &scArr, &scImgs);
	uint32_t rW = 0, rH = 0;
	dxr_prov_get_render_rect(&rW, &rH);
	float vpW = (scW > 0 && rW > 0) ? (float)rW / (float)scW : 1.0f;
	float vpH = (scH > 0 && rH > 0) ? (float)rH / (float)scH : 1.0f;
	if (vpW > 1.0f) vpW = 1.0f;
	if (vpH > 1.0f) vpH = 1.0f;

	if (dxr_prov_get_single_pass()) {
		// Single-Pass-Instanced: 1 render pass, 2 render params over the 2-slice
		// array (slice 0 = left, slice 1 = right). Correct on URP/HDRP.
		// D3D12 + D3D11 editor bridge: the single fixed bridge is s_tex_ids[0]. D3D11
		// zero-copy (player): Unity renders directly into the acquired runtime swapchain
		// image, so pick the texture wrapping THIS frame's acquired image.
		UnityXRNextFrameDesc::UnityXRRenderPass &pass = next->renderPasses[0];
		pass.textureId = (dxr_prov_get_graphics_api() == DXR_GFX_D3D11 && !dxr_prov_d3d11_bridge_active())
		                     ? s_tex_ids[s_current_image_index]
		                     : s_tex_ids[0];
		pass.cullingPassIndex = 0;
		pass.renderParamsCount = 2;

		for (uint32_t eye = 0; eye < 2; eye++) {
			DxrProvView v;
			dxr_prov_get_view(eye, &v);
			UnityXRNextFrameDesc::UnityXRRenderPass::UnityXRRenderParams &rp = pass.renderParams[eye];
			set_eye_pose_rig_relative(rp, v);
			rp.projection.type = kUnityXRProjectionTypeMatrix;
			dxr_prov_build_projection(v.fov, (float *)&rp.projection.data.matrix);
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
	} else {
		// MultiPass (BiRP): 2 render passes × 1 param, each rendering one eye into
		// its OWN single-slice texture (s_tex_ids[0]=left, [1]=right). This is the
		// spec-correct MultiPass topology — textureArraySlice is SPI-only, so the
		// earlier "MultiPass into a 2-slice array" attempt produced garbage. Renders
		// opaque geometry correctly where SPI doesn't (BiRP off-center projection).
		for (uint32_t eye = 0; eye < 2; eye++) {
			DxrProvView v;
			dxr_prov_get_view(eye, &v);
			UnityXRNextFrameDesc::UnityXRRenderPass &pass = next->renderPasses[eye];
#ifdef __APPLE__
			// Metal zero-copy: rotate to THIS frame's acquired image (i*2+eye);
			// Unity renders each eye straight into the acquired slice view.
			// DISPLAYXR_METAL_NO_ROTATE=1: bisection lever — always image 0.
			uint32_t mimg = getenv("DISPLAYXR_METAL_NO_ROTATE") ? 0 : s_current_image_index;
			pass.textureId = (dxr_prov_get_graphics_api() == DXR_GFX_METAL)
			                     ? s_tex_ids[mimg * 2 + eye]
			                     : s_tex_ids[eye];
#else
			pass.textureId = s_tex_ids[eye];
#endif
			pass.cullingPassIndex = eye;
			pass.renderParamsCount = 1;
			UnityXRNextFrameDesc::UnityXRRenderPass::UnityXRRenderParams &rp = pass.renderParams[0];
			set_eye_pose_rig_relative(rp, v);
			rp.projection.type = kUnityXRProjectionTypeMatrix;
			dxr_prov_build_projection(v.fov, (float *)&rp.projection.data.matrix);
			rp.occlusionMeshId = 0;
			rp.textureArraySlice = 0;                     // single-slice per eye
			rp.viewportRect = UnityXRRectf{0.0f, 0.0f, vpW, vpH};

			// Per-eye culling pass (mirrors the eye's projection).
			UnityXRNextFrameDesc::UnityXRCullingPass &cull = next->cullingPasses[eye];
			cull.deviceAnchorToCullingPose = rp.deviceAnchorToEyePose;
			cull.projection = rp.projection;
			cull.separation = 0.0f;
		}

		next->renderPassesCount = 2;
	}

	// Extra 3D zones (#166 Phase B2): append one render-pass group per zone (SPI: 1
	// pass × 2 params; MultiPass: 2 passes × 1), each rendering the full scene from
	// the zone's Kooima framing into the zone's own zone-sized texture (viewport full,
	// no sub-rect). Capped at Unity's render-pass limit.
	{
		uint32_t ez = dxr_prov_get_extra_zone_count();
		bool sp = dxr_prov_get_single_pass();
		uint32_t pi = next->renderPassesCount;
		for (uint32_t i = 0; i < ez && i < PROV_MAX_EXTRA_ZONES; i++) {
			if (!s_extra_tex_created[i]) continue;
			if (sp) {
				if (pi >= (uint32_t)kUnityXRMaxNumRenderPasses) { prov_log("[DisplayXR-PROV] zone pass cap hit (SPI)\n"); break; }
				UnityXRNextFrameDesc::UnityXRRenderPass &pass = next->renderPasses[pi];
				pass.textureId = s_extra_tex_ids[i][0];
				pass.cullingPassIndex = pi;
				pass.renderParamsCount = 2;
				for (uint32_t eye = 0; eye < 2; eye++) {
					DxrProvView v; dxr_prov_get_extra_zone_view(i, eye, &v);
					UnityXRNextFrameDesc::UnityXRRenderPass::UnityXRRenderParams &rp = pass.renderParams[eye];
					set_eye_pose_rig_relative(rp, v);
					rp.projection.type = kUnityXRProjectionTypeMatrix;
					dxr_prov_build_projection(v.fov, (float *)&rp.projection.data.matrix);
					rp.occlusionMeshId = 0;
					rp.textureArraySlice = (int32_t)eye;
					rp.viewportRect = UnityXRRectf{0.0f, 0.0f, 1.0f, 1.0f};
				}
				UnityXRNextFrameDesc::UnityXRCullingPass &cull = next->cullingPasses[pi];
				cull.deviceAnchorToCullingPose = pass.renderParams[0].deviceAnchorToEyePose;
				cull.projection = pass.renderParams[0].projection;
				cull.separation = 0.0f;
				pi++;
			} else {
				if (pi + 1 >= (uint32_t)kUnityXRMaxNumRenderPasses) { prov_log("[DisplayXR-PROV] zone pass cap hit (MP)\n"); break; }
				for (uint32_t eye = 0; eye < 2; eye++) {
					DxrProvView v; dxr_prov_get_extra_zone_view(i, eye, &v);
					UnityXRNextFrameDesc::UnityXRRenderPass &pass = next->renderPasses[pi];
					pass.textureId = s_extra_tex_ids[i][eye];
					pass.cullingPassIndex = pi;
					pass.renderParamsCount = 1;
					UnityXRNextFrameDesc::UnityXRRenderPass::UnityXRRenderParams &rp = pass.renderParams[0];
					set_eye_pose_rig_relative(rp, v);
					rp.projection.type = kUnityXRProjectionTypeMatrix;
					dxr_prov_build_projection(v.fov, (float *)&rp.projection.data.matrix);
					rp.occlusionMeshId = 0;
					rp.textureArraySlice = 0;
					rp.viewportRect = UnityXRRectf{0.0f, 0.0f, 1.0f, 1.0f};
					UnityXRNextFrameDesc::UnityXRCullingPass &cull = next->cullingPasses[pi];
					cull.deviceAnchorToCullingPose = rp.deviceAnchorToEyePose;
					cull.projection = rp.projection;
					cull.separation = 0.0f;
					pi++;
				}
			}
		}
		next->renderPassesCount = pi;
	}
	return kUnitySubsystemErrorCodeSuccess;
}

UnitySubsystemErrorCode UNITY_INTERFACE_API
GfxSubmitCurrentFrame(UnitySubsystemHandle handle, void *userData)
{
	(void)handle; (void)userData;
	if (!s_session_active || !s_frame_in_flight) return kUnitySubsystemErrorCodeSuccess;
	s_frame_in_flight = false;

	// D3D11 zero-copy (#195): Unity rendered straight into the runtime swapchain image
	// on its own device — no shared bridge, no cross-device barrier. The runtime syncs
	// internally on xrReleaseSwapchainImage. Skip the D3D12-only transition entirely.
	if (dxr_prov_get_graphics_api() == DXR_GFX_D3D11) {
		dxr_prov_submit_frame(s_current_image_index);
		return kUnitySubsystemErrorCodeSuccess;
	}

#ifdef _WIN32
	// Cross-device handoff: Unity just rendered both eyes into the shared bridge
	// (leaving it in RENDER_TARGET on Unity's device). Ask Unity to transition it
	// to COMMON in its active command list so our SEPARATE device can read it
	// coherently across the device boundary (a D3D12 shared resource must be in
	// COMMON at a cross-device sync point). Without this the own-device copy reads
	// incoherent/empty memory → black. Pairs with the shared sync fence.
	IUnityGraphicsD3D12v8 *v8 = s_ifaces->Get<IUnityGraphicsD3D12v8>();
	if (v8) {
		uint32_t bw = 0, bh = 0, ba = 0;
		if (dxr_prov_get_single_pass()) {
			void *bridge = dxr_prov_get_bridge_unity_texture(&bw, &bh, &ba);
			if (bridge)
				v8->RequestResourceState((ID3D12Resource *)bridge, D3D12_RESOURCE_STATE_COMMON);
		} else {
			// MultiPass: both per-eye bridges were rendered into this frame.
			for (uint32_t eye = 0; eye < 2; eye++) {
				void *be = dxr_prov_get_bridge_unity_texture_eye(eye, &bw, &bh);
				if (be)
					v8->RequestResourceState((ID3D12Resource *)be, D3D12_RESOURCE_STATE_COMMON);
			}
		}
		// Extra 3D zones: transition their bridges too (#166 Phase B2).
		uint32_t ez = dxr_prov_get_extra_zone_count();
		for (uint32_t i = 0; i < ez && i < PROV_MAX_EXTRA_ZONES; i++) {
			if (dxr_prov_get_single_pass()) {
				void *b = dxr_prov_get_extra_zone_bridge(i, &bw, &bh);
				if (b) v8->RequestResourceState((ID3D12Resource *)b, D3D12_RESOURCE_STATE_COMMON);
			} else {
				for (uint32_t eye = 0; eye < 2; eye++) {
					void *b = dxr_prov_get_extra_zone_bridge_eye(i, eye, &bw, &bh);
					if (b) v8->RequestResourceState((ID3D12Resource *)b, D3D12_RESOURCE_STATE_COMMON);
				}
			}
		}
	}
#endif // _WIN32

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
	// Window creation happens HERE on the MAIN thread. (Creating a window that
	// attaches to / tracks Unity's main-thread window from GfxStart, the render
	// thread, deadlocks: the input queues join while the main thread waits on
	// GfxStart.) GfxStart binds the runtime to s_overlay_hwnd.
#ifndef _WIN32
	// macOS (#204): create the provider's weave window HERE on the MAIN thread
	// and bind its NSView via XrCocoaWindowBindingCreateInfoEXT. AppKit windows
	// cannot be created off the main thread — the runtime's self-host NSWindow
	// create inside xrCreateSession (GfxStart = render thread, while Unity's
	// main thread is blocked waiting on GfxStart) deadlocks. The macOS analog
	// of the Windows #173 lesson. Reuses the proven SA-era preview window
	// (displayxr_metal.m); auto-sized to the display aspect within the
	// screen's usable area — ps_window_size tracks the live NSView backing
	// size, and resize lands with the #172 reconcile.
	s_overlay_hwnd = displayxr_metal_create_preview_window(0, 0);
	prov_log(s_overlay_hwnd
	             ? "[DisplayXR-PROV] Lifecycle Start (macOS: provider weave window created on main thread)\n"
	             : "[DisplayXR-PROV] Lifecycle Start (macOS: weave window FAILED; runtime offscreen)\n");
#else
	if (prov_want_dedicated_window()) {
		// (#173) Editor Play Mode: a STANDALONE movable weave window that does NOT
		// track Unity's (whole-editor) window — coexists with the editor while still
		// binding a real HWND (window-relative Kooima + #172 realloc). See
		// displayxr_win32.c for the WS_OVERLAPPEDWINDOW + NOACTIVATE/TOPMOST + DPI
		// recipe (mirrors the proven standalone preview window).
		s_overlay_hwnd = displayxr_create_provider_dedicated_window();
		prov_log(s_overlay_hwnd
		             ? "[DisplayXR-PROV] Lifecycle Start (dedicated provider window created, #173)\n"
		             : "[DisplayXR-PROV] Lifecycle Start (dedicated window FAILED; runtime self-hosts)\n");

		// Keyboard/mouse input (#166 task #9): install the same focus / raw-input
		// hooks the app-owned overlay uses, so Unity's Input System keeps receiving
		// input (RIDEV_INPUTSINK + GetForegroundWindow/GetFocus → Unity) even if the
		// separate weave window is ever the OS-foreground window. The window is
		// WS_EX_NOACTIVATE so it should not steal foreground from the editor in the
		// first place (that alone keeps input alive, like the standalone preview) —
		// the hook is belt-and-braces / parity with the overlay path.
		void *unity_hwnd = displayxr_get_unity_main_hwnd();
		if (unity_hwnd != nullptr) {
			displayxr_install_focus_hook(unity_hwnd);
			prov_log("[DisplayXR-PROV] Lifecycle Start: installed keyboard focus/raw-input hooks (dedicated window)\n");
		}
	} else if (prov_want_app_window()) {
		// App-owned window (default, built players): create a TOP-LEVEL WS_POPUP
		// overlay over Unity's window so the runtime weaves into the app's own window
		// like a native handle app. DISPLAYXR_PROV_SELFHOST=1 opts out to the runtime
		// self-hosting its own window (bring-up diagnostic). The top-level popup
		// composites the runtime's DComp weave; a WS_CHILD does not (#166).
		// Transparent apps take the UNOWNED transparent overlay path (created by
		// displayxr_get_app_main_view because transparent_background_requested is set)
		// + Unity cloak/off-screen (DisplayXRTransparentOverlay). Do NOT set
		// provider-opaque there: it makes parent_subclass_proc FOLLOW Unity, which the
		// transparent path moves off-screen — dragging the overlay off-screen with it
		// (the "[DisplayXR.Inject] clientX=-413" fight). Opaque apps keep provider-opaque
		// (top-level popup that tracks Unity's on-screen window).
		if (!dxr_prov_wants_transparent())
			displayxr_set_provider_opaque_overlay(1);
		s_overlay_hwnd = displayxr_get_app_main_view();
		prov_log(s_overlay_hwnd
		             ? "[DisplayXR-PROV] Lifecycle Start (top-level overlay created on main thread)\n"
		             : "[DisplayXR-PROV] Lifecycle Start (overlay create FAILED; runtime self-hosts)\n");

		// Keyboard input (#166 task #9). The overlay is a NOACTIVATE, click-through
		// (WS_EX_TRANSPARENT) display surface sitting on top of Unity, so Unity never
		// takes OS foreground on its own — and Windows delivers raw input (WM_INPUT,
		// the source the Input System reads keyboard/mouse from) only to the
		// FOREGROUND HWND. Mouse messages still reach Unity (they route to the window
		// under the cursor), which is why dragging worked but keys didn't. Install the
		// same focus / raw-input hooks the transparent path uses so Unity's input
		// system behaves as if it were foreground (RIDEV_INPUTSINK + GetForegroundWindow
		// /GetFocus → Unity + deactivation suppression). Also explicitly hand Unity real
		// foreground + focus once at startup so the legacy Input Manager — which needs
		// actual WM_KEYDOWN delivery to the focused window — works too; the overlay is
		// NOACTIVATE so it won't fight Unity for it. Runs on the main (UI) thread, where
		// SetFocus is valid. (The focus hook chains cleanly after parent_subclass_proc,
		// already installed by displayxr_get_app_main_view above.)
		void *unity_hwnd = displayxr_get_unity_main_hwnd();
		if (unity_hwnd != nullptr) {
			displayxr_install_focus_hook(unity_hwnd);
			SetForegroundWindow((HWND)unity_hwnd);
			SetFocus((HWND)unity_hwnd);
			prov_log("[DisplayXR-PROV] Lifecycle Start: installed keyboard focus/raw-input hooks + handed Unity foreground\n");
		}
	} else {
		s_overlay_hwnd = nullptr;
		prov_log("[DisplayXR-PROV] Lifecycle Start (DISPLAYXR_PROV_SELFHOST: runtime self-hosts its own window)\n");
	}
#endif // _WIN32
	return kUnitySubsystemErrorCodeSuccess;
}

void UNITY_INTERFACE_API
LifecycleStop(UnitySubsystemHandle handle, void *userData)
{
	(void)handle; (void)userData;
	// (#173) Editor Play Mode: destroy the dedicated weave window so it doesn't
	// linger frozen after Play stops. Runs on the MAIN thread, after GfxStop already
	// ran dxr_prov_session_stop (xrDestroySession unhooked the SR weaver subclass) —
	// so the destroy is clean. Built-player overlay/self-host paths don't create it
	// (the call is a no-op there). A re-Play recreates it in LifecycleStart.
#ifdef _WIN32
	if (prov_want_dedicated_window()) {
		displayxr_destroy_provider_dedicated_window();
		s_overlay_hwnd = nullptr;
	}
#else
	// macOS (#204): destroy the provider weave window on the MAIN thread, after
	// GfxStop already ran dxr_prov_session_stop (xrDestroySession released the
	// runtime's references into the view) — mirrors the #173 teardown ordering.
	displayxr_metal_destroy_preview_window();
	s_overlay_hwnd = nullptr;
#endif
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
