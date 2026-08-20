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

#include <stdlib.h> // getenv — Windows gets it via windows.h, Linux does not

#ifdef _WIN32
#include <windows.h>
#include <d3d12.h>
#include <d3d11.h> // D3D11 zero-copy backend (#195)
#include <dxgi1_4.h>
#include <d3dcompiler.h> // native mirror shader blit (urp-editor GameView, BGRA->RGBA)
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
// Extra 3D zones (#206): per-zone zero-copy slice views + acquired-image rotation.
extern "C" void *dxr_prov_get_extra_zone_metal_eye_view(uint32_t ei, uint32_t img, uint32_t eye);
extern "C" void *dxr_prov_get_extra_zone_metal_image(uint32_t ei, uint32_t img);
extern "C" uint32_t dxr_prov_get_extra_zone_image_count(uint32_t ei);
extern "C" uint32_t dxr_prov_get_extra_zone_acquired_index(uint32_t ei);
#endif
#include "../unity_pluginapi/IUnityXRDisplay.h"

#include "displayxr_provider_session.h"

// Renderer id (IUnityGraphics::GetRenderer()) — defined in displayxr_unity_plugin.cpp.
// -1 if IUnityGraphics isn't available. Used to select the graphics backend (#195).
extern "C" int displayxr_unity_get_renderer(void);

#if defined(ENABLE_VULKAN)
// Unity's Vulkan objects (displayxr_unity_plugin.cpp, via IUnityGraphicsVulkan) and the
// VK glue's entry points (displayxr_provider_gfx_vulkan.cpp). Forward-declared rather
// than #included so vulkan.h never reaches this TU.
extern "C" bool displayxr_unity_get_vulkan(void **out_instance, void **out_physical_device,
                                           void **out_device, void **out_graphics_queue,
                                           uint32_t *out_queue_family_index);
extern "C" void  dxr_pvk_set_unity_objects(void *, void *, void *, uint32_t, void *);
extern "C" void *dxr_pvk_unity_image_ptr(int eye);
extern "C" int   dxr_pvk_device_ready(void);
#endif

// #247 crash bisect — see the call site in create_textures(). Latched so the env var
// is read once rather than per texture creation.
static bool prov_vk_unity_alloc_probe(void)
{
#if defined(ENABLE_VULKAN)
	static int cached = -1;
	if (cached < 0) cached = (getenv("DISPLAYXR_VK_UNITY_ALLOC") != nullptr) ? 1 : 0;
	return cached == 1 && dxr_prov_get_graphics_api() == DXR_GFX_VULKAN;
#else
	return false;
#endif
}

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
// (workspace-ipc) True when the Shell launched us as a workspace tile — it sets
// DISPLAYXR_WORKSPACE_SESSION=1, which the runtime also reads to force IPC/service
// mode (in-process compositor disabled → client submits frames to comp_d3d11_service).
// In that mode the provider is a plain OpenXR client: no overlay, no foreground grab,
// windowHandle=NULL. Defined in displayxr_win32.c / displayxr_macos.mm.
extern "C" int displayxr_is_shell_mode(void);
#if defined(__linux__) && !defined(__ANDROID__)
// Linux handle-app glue (#249), defined in displayxr_linux.c.
extern "C" int displayxr_linux_get_weave_window(void **out_display, unsigned long *out_window);
extern "C" void displayxr_linux_destroy_weave_window(void);
extern "C" void displayxr_linux_track_window(void);
#endif
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
// (#256) Destroy the app-owned overlay + undo the Unity-window subclass / focus hook
// it installed. The overlay is created in LifecycleStart, BEFORE the session is
// attempted, so without this a session refusal leaves a TOPMOST, click-eating orphan
// for the process lifetime. Safe from any thread; see displayxr_win32.h.
extern "C" void  displayxr_destroy_app_overlay(void);
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
// GameView weave-to-texture mirror (Task (a)): the runtime-woven shared texture wrapped
// as a Unity texture, mirror-blitted into the editor Game window. Inert unless texture
// mode is active (dxr_prov_get_woven_unity_texture returns NULL otherwise).
static UnityXRRenderTextureId s_woven_tex_id = 0;
static bool                   s_woven_tex_created = false;
// Native mirror blit command objects (Unity device) — reused; released in GfxStop.
// (macOS: the native mirror blit is D3D12-only; MainBlitToMirrorViewRenderTarget is stubbed.)
#ifdef _WIN32
static ID3D12CommandAllocator   *s_mblit_alloc = nullptr;
static ID3D12GraphicsCommandList *s_mblit_list = nullptr;
#endif

// Extra 3D zones (#166 Phase B2): each extra zone gets its own Unity texture(s)
// (SPI: [i][0] = 2-slice array; MultiPass: [i][0/1] = per-eye) + render pass.
#define PROV_MAX_EXTRA_ZONES 3   // PS_MAX_ZONES - 1
UnityXRRenderTextureId  s_extra_tex_ids[PROV_MAX_EXTRA_ZONES][2] = {};
bool                    s_extra_tex_created[PROV_MAX_EXTRA_ZONES] = {};
#ifdef __APPLE__
// Metal zero-copy extra zones (#206): unlike D3D's single fixed bridge, the runtime
// rotates zone images each frame, so we wrap ALL image slice views up front (SPI: one
// 2-slice texture per image; MultiPass: image×eye) and rotate in PopulateNextFrameDesc.
// [i][img*2+eye] for MultiPass, [i][img] for SPI (4 images × 2 eyes).
UnityXRRenderTextureId  s_extra_tex_ids_metal[PROV_MAX_EXTRA_ZONES][8] = {};
uint32_t                s_extra_tex_count_metal[PROV_MAX_EXTRA_ZONES] = {};
#endif

uint32_t                s_current_image_index = 0;  // acquired this frame
bool                    s_frame_in_flight = false;

extern "C" void dxr_prov_file_log(const char *s); // defined in the session TU

#ifndef _WIN32
// macOS: MainQueryMirrorViewBlitDesc stays cross-platform (registered unconditionally) but its
// woven-mirror diagnostics use the Windows-only _snprintf_s. That body is dead on macOS (no
// woven texture is ever published there — dxr_prov_get_woven_unity_texture returns NULL), yet it
// must still COMPILE. Map _snprintf_s(buf, size, _TRUNCATE, ...) to the standard bounded snprintf;
// the count argument is consumed and dropped by the macro (snprintf truncates safely on its own).
#define _TRUNCATE ((size_t)-1)
#define _snprintf_s(buf, size, count, ...) snprintf((buf), (size), __VA_ARGS__)
#endif

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
#ifdef __APPLE__
	// Metal zero-copy (#206): wrap ALL of each zone's image slice views up front (the
	// runtime rotates zone images; PopulateNextFrameDesc picks the acquired index). SPI:
	// each image's whole arraySize=2 texture -> s_extra_tex_ids_metal[i][img] (arr=2).
	// MultiPass: each image×eye slice view -> s_extra_tex_ids_metal[i][img*2+eye] (arr=1).
	if (dxr_prov_get_graphics_api() == DXR_GFX_METAL) {
		for (uint32_t i = 0; i < ez && i < PROV_MAX_EXTRA_ZONES; i++) {
			if (s_extra_tex_created[i]) continue;
			uint32_t imgs = dxr_prov_get_extra_zone_image_count(i);
			if (imgs == 0) continue; // zone swapchain not created yet
			uint32_t w = 0, h = 0;
			dxr_prov_get_extra_zone_bridge(i, &w, &h); // NULL bridge on Metal, but fills w/h
			if (w == 0 || h == 0) continue;
			if (sp) {
				if (imgs > 8) imgs = 8;
				bool ok = true;
				for (uint32_t img = 0; img < imgs; img++) {
					void *tex = dxr_prov_get_extra_zone_metal_image(i, img);
					if (!tex) { ok = false; break; }
					UnityXRRenderTextureDesc desc; memset(&desc, 0, sizeof(desc));
					desc.colorFormat = kUnityXRRenderTextureFormatRGBA32;
					desc.color.nativePtr = tex;
					desc.depthFormat = kUnityXRDepthTextureFormat24bitOrGreater;
					desc.depth.nativePtr = (void *)(uintptr_t)kUnityXRRenderTextureIdDontCare;
					desc.width = w; desc.height = h; desc.textureArrayLength = 2;
					desc.flags = kUnityXRRenderTextureFlagsUVDirectionTopToBottom;
					UnityXRRenderTextureId id = 0;
					if (s_display->CreateTexture(s_handle, &desc, &id) != kUnitySubsystemErrorCodeSuccess) { ok = false; break; }
					s_extra_tex_ids_metal[i][img] = id;
				}
				if (ok) { s_extra_tex_count_metal[i] = imgs; s_extra_tex_created[i] = true; prov_log("[DisplayXR-PROV] extra zone: CreateTexture (Metal SPI 2-slice arrays) OK\n"); }
			} else {
				if (imgs > 4) imgs = 4; // 4 images x 2 eyes = 8 slots
				bool ok = true;
				for (uint32_t img = 0; img < imgs && ok; img++) {
					for (uint32_t eye = 0; eye < 2; eye++) {
						void *view = dxr_prov_get_extra_zone_metal_eye_view(i, img, eye);
						if (!view) { ok = false; break; }
						UnityXRRenderTextureDesc desc; memset(&desc, 0, sizeof(desc));
						desc.colorFormat = kUnityXRRenderTextureFormatRGBA32;
						desc.color.nativePtr = view;
						desc.depthFormat = kUnityXRDepthTextureFormat24bitOrGreater;
						desc.depth.nativePtr = (void *)(uintptr_t)kUnityXRRenderTextureIdDontCare;
						desc.width = w; desc.height = h; desc.textureArrayLength = 1;
						desc.flags = kUnityXRRenderTextureFlagsUVDirectionTopToBottom;
						UnityXRRenderTextureId id = 0;
						if (s_display->CreateTexture(s_handle, &desc, &id) != kUnitySubsystemErrorCodeSuccess) { ok = false; break; }
						s_extra_tex_ids_metal[i][img * 2 + eye] = id;
					}
				}
				if (ok) { s_extra_tex_count_metal[i] = imgs * 2; s_extra_tex_created[i] = true; prov_log("[DisplayXR-PROV] extra zone: CreateTexture (Metal MP slice views) OK\n"); }
			}
		}
		return;
	}
#endif
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

// sRGB-aware flags for the PRIMARY eye render textures Unity draws into. When the primary
// swapchain was created sRGB (a Linear project on the present path — see
// dxr_prov_set_color_space_linear), tell Unity to render into an sRGB view so it encodes
// linear→sRGB on store; the runtime then presents correctly-encoded pixels. Otherwise (docked
// texture path, Gamma projects, or a runtime with no sRGB format) this is just the UV flag —
// byte-identical to before. NOT applied to the woven-mirror texture or the UI/zone layers.
static uint32_t prov_eye_tex_flags(void)
{
	uint32_t f = kUnityXRRenderTextureFlagsUVDirectionTopToBottom;
	if (dxr_prov_swapchain_is_srgb()) f |= kUnityXRRenderTextureFlagsSRGB;
	return f;
}

// GameView mirror (Task (a)): wrap the runtime-woven shared texture as a Unity texture
// so MainQueryMirrorViewBlitDesc can blit it into the editor Game window. Independent of
// the per-eye render textures; runs only when texture mode published a woven texture.
static void create_woven_mirror_texture_if_ready()
{
	if (s_woven_tex_created || !s_display || !s_handle) return;
	uint32_t ww = 0, wh = 0;
	void *wov = dxr_prov_get_woven_unity_texture(&ww, &wh);
	if (!wov || ww == 0 || wh == 0) return;
	UnityXRRenderTextureDesc desc; memset(&desc, 0, sizeof(desc));
	desc.colorFormat = kUnityXRRenderTextureFormatBGRA32; // woven tex is B8G8R8A8_UNORM (fmt 87)
	desc.color.nativePtr = wov;
	desc.depthFormat = kUnityXRDepthTextureFormatNone;    // blit source; no depth
	desc.depth.nativePtr = (void *)(uintptr_t)kUnityXRRenderTextureIdDontCare;
	desc.width = ww;
	desc.height = wh;
	desc.textureArrayLength = 1;
	desc.flags = kUnityXRRenderTextureFlagsUVDirectionTopToBottom; // D3D top-left origin
	UnityXRRenderTextureId id = 0;
	if (s_display->CreateTexture(s_handle, &desc, &id) == kUnitySubsystemErrorCodeSuccess) {
		s_woven_tex_id = id;
		s_woven_tex_created = true;
		prov_log("[DisplayXR-PROV] CreateTexture: wrapped WOVEN shared texture -> GameView mirror\n");
	} else {
		prov_log("[DisplayXR-PROV] CreateTexture (woven GameView mirror) failed\n");
	}
}

void create_textures_if_ready()
{
	create_woven_mirror_texture_if_ready();
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
		// SPI experiment (#205): wrap each WHOLE arraySize=2 swapchain image as
		// one 2-slice Unity texture (the Metal cousin of the D3D11 zero-copy SPI
		// arm below); PopulateNextFrameDesc rotates to the acquired image.
		if (dxr_prov_get_single_pass()) {
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
				desc.flags = prov_eye_tex_flags();
				UnityXRRenderTextureId id = 0;
				if (s_display->CreateTexture(s_handle, &desc, &id) != kUnitySubsystemErrorCodeSuccess) {
					prov_log("[DisplayXR-PROV] CreateTexture (Metal SPI array) failed\n");
					return;
				}
				s_tex_ids[i] = id;
			}
			s_tex_count = simgs;
			s_textures_created = true;
			prov_log("[DisplayXR-PROV] CreateTexture: wrapped Metal swapchain arrays (zero-copy SPI, #205)\n");
			return;
		}
		if (simgs > 4) simgs = 4;          // s_tex_ids capacity: 4 images x 2 eyes
		for (uint32_t i = 0; i < simgs; i++) {
			for (uint32_t e = 0; e < 2; e++) {
				void *view = dxr_prov_get_metal_eye_view(i, e);
				if (!view) return; // views not ready yet
				UnityXRRenderTextureDesc desc;
				memset(&desc, 0, sizeof(desc));
				desc.colorFormat = kUnityXRRenderTextureFormatRGBA32;
				desc.color.nativePtr = view;
				desc.depthFormat = kUnityXRDepthTextureFormat24bitOrGreater;
				desc.depth.nativePtr = (void *)(uintptr_t)kUnityXRRenderTextureIdDontCare;
				desc.width = sw;
				desc.height = sh;
				desc.textureArrayLength = 1; // single-slice view per eye
				desc.flags = prov_eye_tex_flags();
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
			desc.flags = prov_eye_tex_flags();
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
		// DIAGNOSTIC (#247, DISPLAYXR_VK_UNITY_ALLOC=1): let Unity allocate the colour
		// target instead of importing ours. This is a BISECT, not a feature — with a
		// Unity-allocated colour surface the eyes never reach the runtime, so nothing
		// displays. Its only job is to separate two hypotheses for the crash inside
		// vk::Image::CreateImageViews: if Unity-allocated survives, the fault is in the
		// EXTERNAL image we import; if it crashes too, the fault is elsewhere in this
		// descriptor (flags/size/depth pairing) and the bridge is exonerated.
		if (prov_vk_unity_alloc_probe()) {
			desc.color.nativePtr = (void *)(uintptr_t)kUnityXRRenderTextureIdDontCare;
			prov_log("[DisplayXR-PROV] VK PROBE: DISPLAYXR_VK_UNITY_ALLOC=1 — Unity allocates the "
			         "eye colour target (bridge NOT imported). Nothing will display; this is a "
			         "crash bisect only.\n");
		}
		desc.depthFormat = kUnityXRDepthTextureFormat24bitOrGreater;
		desc.depth.nativePtr = (void *)(uintptr_t)kUnityXRRenderTextureIdDontCare; // Unity allocates depth
		// Unity allocates a matched 2-slice depth array (verified via QueryTextureDesc).
		desc.width = w;
		desc.height = h;
		desc.textureArrayLength = arr;  // 2 -> SPI texture array
		desc.flags = prov_eye_tex_flags(); // D3D top-left origin

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
			desc.flags = prov_eye_tex_flags();

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
	if (s_woven_tex_id) { s_display->DestroyTexture(s_handle, s_woven_tex_id); s_woven_tex_id = 0; }
	s_woven_tex_created = false;
}

// Drop one extra zone's Unity texture(s) after a live realloc (#172) so
// create_extra_zone_textures re-wraps the zone's fresh bridge next frame.
static void destroy_extra_zone_texture(uint32_t i)
{
	if (!s_display || !s_handle || i >= PROV_MAX_EXTRA_ZONES) return;
	for (int e = 0; e < 2; e++)
		if (s_extra_tex_ids[i][e]) { s_display->DestroyTexture(s_handle, s_extra_tex_ids[i][e]); s_extra_tex_ids[i][e] = 0; }
#ifdef __APPLE__
	for (uint32_t k = 0; k < s_extra_tex_count_metal[i]; k++)
		if (s_extra_tex_ids_metal[i][k]) { s_display->DestroyTexture(s_handle, s_extra_tex_ids_metal[i][k]); s_extra_tex_ids_metal[i][k] = 0; }
	s_extra_tex_count_metal[i] = 0;
#endif
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
	// Workspace tile (shell/IPC): never own an app window — the runtime is in service
	// mode and the compositor service draws our submitted frames as a tile. The mode
	// decision below checks displayxr_is_shell_mode() first; this guard keeps any other
	// caller coherent too.
	if (displayxr_is_shell_mode()) return 0;
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

#if defined(ENABLE_VULKAN)
// Capture Unity's Vulkan objects for the eye bridge. Shared by the Windows (#247)
// and Linux (#249) arms of the backend switch — the sequence is identical because
// the only per-OS part of the bridge is the external-memory handle flavour, which
// lives inside displayxr_provider_gfx_vulkan.cpp.
//
// Unlike D3D, the SESSION device is not Unity's and does not exist yet: the runtime
// creates it inside dxr_prov_session_start via enable2. All we do here is hand the
// glue Unity's objects, which the eye bridge imports into.
static bool
prov_bind_vulkan_backend(void)
{
	void *inst = nullptr, *phys = nullptr, *dev = nullptr, *queue = nullptr;
	uint32_t qf = 0;
	if (!displayxr_unity_get_vulkan(&inst, &phys, &dev, &queue, &qf) || !dev) {
		prov_log("[DisplayXR-PROV] Vulkan: Unity's VkDevice not captured - session not "
		         "started (#247). IUnityGraphicsVulkan was unavailable or the device was "
		         "not yet created when the plugin loaded.\n");
		return false;
	}
	dxr_pvk_set_unity_objects(inst, phys, dev, (uint32_t)qf, queue);
	prov_log("[DisplayXR-PROV] Renderer is Vulkan - using the enable2 own-device bridge "
	         "backend (#247/#249). Phase 1 covers the primary stereo path; wsui / Local2D / "
	         "extra 3D zones are inert on this backend.\n");
	return true;
}
#endif

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
#elif defined(__linux__) && !defined(__ANDROID__)
		// Linux/Vulkan (#249). The provider records and submits its own Vulkan
		// command buffers, so Unity's render state must be re-validated around each
		// callback — same as the Windows own-device bridge.
		caps->invalidateRenderStateAfterEachCallback = true;
		// Unity keeps presenting to ITS OWN window — same as Windows, and for the
		// same reason: the runtime weaves into a SEPARATE plugin-owned overlay
		// window, so the two never contend for one surface.
		//
		// Measured on the Odyssey, both wrong ways round: binding Unity's own window
		// gave a healthy session (weaver holding the XID, 1800+ frames) with a BLACK
		// panel when Unity also presented, and an EMPTY window when it did not —
		// because XR_DXR_xlib_window_binding hands presentation of that window to the
		// runtime, and Unity already owns a swapchain on its main window.
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
		// Common on integrated Intel: Unity's built-in D3D12 device filter denies
		// the iGPU and silently falls back to D3D11 even when the project targets
		// D3D12 (#240). That's fine — the D3D11 zero-copy backend is fully
		// supported — but say so, so a D3D11 log line on a D3D12 project reads as
		// expected behavior rather than a mystery. '-force-d3d12' bypasses the
		// filter if the D3D12 backend is specifically wanted.
		prov_log("[DisplayXR-PROV] Renderer is Direct3D11 - using the D3D11 zero-copy backend. "
		         "(If this project targets D3D12, Unity's device filter likely denied it on this "
		         "GPU - integrated Intel is the usual case; launch with -force-d3d12 to override.)\n");
#if defined(ENABLE_VULKAN)
	} else if (renderer == kUnityGfxRendererVulkan) {
		if (!prov_bind_vulkan_backend()) return kUnitySubsystemErrorCodeFailure;
		backend_kind = DXR_GFX_VULKAN; dev_ptr = nullptr; queue_ptr = nullptr;
#endif
	} else
#elif defined(__linux__) && !defined(__ANDROID__)
#if defined(ENABLE_VULKAN)
	// Desktop Linux (#249) is Vulkan-only for this provider: the runtime's Linux
	// presentation path is the native Vulkan/XCB compositor, and there is no GL
	// backend here. Same enable2 own-device bridge as Windows — see
	// prov_bind_vulkan_backend().
	if (renderer == kUnityGfxRendererVulkan) {
		if (!prov_bind_vulkan_backend()) return kUnitySubsystemErrorCodeFailure;
		backend_kind = DXR_GFX_VULKAN; dev_ptr = nullptr; queue_ptr = nullptr;
	} else
#endif
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
#elif defined(__linux__) && !defined(__ANDROID__)
		snprintf(msg, sizeof(msg),
		         "[DisplayXR-PROV] WARN: unsupported graphics API (renderer=%d). The DisplayXR "
		         "provider requires Vulkan on Linux — session not started (#249). Set Vulkan "
		         "first in Project Settings > Player > Graphics APIs.\n", renderer);
#else
		snprintf(msg, sizeof(msg),
		         "[DisplayXR-PROV] WARN: unsupported graphics API (renderer=%d). The DisplayXR "
		         "provider requires Metal on macOS — session not started (#202/#204).\n", renderer);
#endif
		prov_log(msg);
		return kUnitySubsystemErrorCodeFailure;
	}

	// Weave target. Four modes (window, if any, created on the MAIN thread in
	// LifecycleStart, which sets s_overlay_hwnd for the bound-HWND modes):
	//   - Workspace tile (shell/IPC, DISPLAYXR_WORKSPACE_SESSION=1): windowHandle=NULL,
	//     no overlay. The runtime is in service mode (in-process compositor disabled) →
	//     the client_d3d12_compositor submits our frames to comp_d3d11_service, which
	//     draws the tile. We present nothing. (LifecycleStart leaves s_overlay_hwnd NULL,
	//     or Unity's own hidden HWND under the DISPLAYXR_WORKSPACE_BIND_UNITY_HWND A/B.)
	//   - Dedicated window (#173, editor Play Mode): a standalone movable window that
	//     does NOT track Unity → bind s_overlay_hwnd.
	//   - App-owned overlay (default, built players): a top-level WS_POPUP overlay
	//     tracking Unity's window → bind s_overlay_hwnd.
	//   - Self-host (DISPLAYXR_PROV_SELFHOST=1, bring-up diagnostic): runtime hosts its
	//     own window → windowHandle=NULL.
	// If a bound-HWND mode's window failed to create, self-host is the safe fallback
	// (s_overlay_hwnd stays NULL).
	bool shell = displayxr_is_shell_mode();
	void *overlay_hwnd;
	if (shell) {
		// s_overlay_hwnd is NULL by default; non-NULL only under the A/B env fallback.
		overlay_hwnd = s_overlay_hwnd;
		prov_log(overlay_hwnd
		             ? "[DisplayXR-PROV] weave target: workspace TILE (shell/IPC; bound Unity's hidden HWND — A/B)\n"
		             : "[DisplayXR-PROV] weave target: workspace TILE (shell/IPC; windowHandle=NULL, service-composited)\n");
	} else {
		bool want_bound = prov_want_dedicated_window() || prov_want_app_window();
		overlay_hwnd = want_bound ? s_overlay_hwnd : nullptr;
		prov_log(overlay_hwnd
		             ? (prov_want_dedicated_window()
		                    ? "[DisplayXR-PROV] weave target: dedicated provider window (editor, #173)\n"
		                    : "[DisplayXR-PROV] weave target: app-owned top-level overlay (in-app)\n")
#if defined(__linux__) && !defined(__ANDROID__)
		             // Linux carries its window through the xlib binding, not through
		             // s_overlay_hwnd (the binding needs a Display*+Window PAIR), so a
		             // NULL handle here does NOT mean self-hosting — saying "SELFHOST"
		             // read as a contradiction against the xlib-binding line that
		             // follows it. The session logs the real target.
		             : "[DisplayXR-PROV] weave target: X11 window binding (decided at session create)\n");
#else
		             : "[DisplayXR-PROV] weave target: runtime self-hosted window (SELFHOST diagnostic)\n");
#endif
	}

	// runtime_json = NULL -> resolve from XR_RUNTIME_JSON (sim_display bring-up).
	if (!dxr_prov_session_start(nullptr, backend_kind, dev_ptr, queue_ptr, overlay_hwnd)) {
		prov_log("[DisplayXR-PROV] dxr_prov_session_start failed\n");
#ifdef _WIN32
		// (#256) No session ⇒ nothing will ever draw into the overlay LifecycleStart
		// created, and Unity is about to go on rendering into its own window. Take the
		// overlay (and the Unity-window subclass/focus hook installed alongside it) back
		// down so the app degrades to a normal, visible 2D window instead of sitting
		// behind a TOPMOST orphan that eats clicks. We are on the RENDER thread here —
		// the destroy marshals itself to the window's creating thread. Scoped to the
		// app-window mode: the editor's dedicated window (#173) has its own teardown in
		// LifecycleStop and must survive a failed start so Play-stop can log/see it.
		if (!shell && !prov_want_dedicated_window() && prov_want_app_window()) {
			displayxr_destroy_app_overlay();
			s_overlay_hwnd = nullptr;
		}
#endif
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

#if defined(_WIN32) || (defined(__linux__) && !defined(__ANDROID__))
	// Windows and Linux pump xrPollEvent HERE, on the graphics thread.
	//
	// This is load-bearing and easy to miss: without it the READY event is never
	// consumed, so xrBeginSession is never called, the session sits at state 2
	// (READY) forever, shouldRender stays false, and NOTHING renders. On the Odyssey
	// that presented as a perfectly healthy-looking run — session created, weaver
	// holding our window, ~1800 pump ticks — with a blank panel and zero bridge
	// copies. Linux originally inherited the macOS arm below and hit exactly that
	// (#249); it has no AppKit constraint, so it belongs on the Windows side.
	dxr_prov_poll_events();
#endif
#if defined(__linux__) && !defined(__ANDROID__)
	// Keep the plugin-owned weave overlay parked over — and ABOVE — Unity's window.
	// The Windows overlay does the same tracking via SetWindowPos in its WndProc.
	displayxr_linux_track_window();
#endif
#ifdef _WIN32
	// GameView zone convergence (Phase 1, #727 follow-up): re-drive the forced full-window
	// zone to the authoritative panel px the mirror callback published, so the compositor
	// canvas == Game-view render viewport pixel-exact. A no-op once converged; when it does
	// change the zone, the reconcile below reallocs the swapchain/bridge to match.
	dxr_prov_converge_gameview_zone();
#elif defined(__APPLE__)
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

	// GameView weave-to-texture mirror (Task (a)): the editor XR game view ONLY displays the
	// XR mirror-blit — normal Unity rendering (IMGUI, overlay UI) does NOT composite into it
	// (verified: an IMGUI overlay's sentinels never appeared though OnGUI fired). So the
	// mirror-blit is the sole path. Request a RESERVED mode (LeftEye, in the manifest) once
	// the woven texture is up so QueryMirrorViewBlitDesc fires; it returns the woven canvas
	// with a compose-fill destRect (see MainQueryMirrorViewBlitDesc). Shipping window path is
	// untouched: with no woven texture this stays kUnityXRMirrorBlitNone.
	if (s_woven_tex_id != 0)
		next->mirrorBlitMode = kUnityXRMirrorBlitLeftEye;

	// Pump the OpenXR frame loop EVERY tick once the session is running — mirroring a
	// compliant native client (runtime test_apps/handle/cube_handle_d3d12_win). Its
	// render thread calls xrWaitFrame → xrBeginFrame → xrEndFrame every iteration while
	// sessionRunning, UNCONDITIONALLY, and gates only the actual rendering on
	// shouldRender (it still submits a 0-layer xrEndFrame when it doesn't render). That
	// continuous frame submission is what drives the runtime's session lifecycle
	// SYNCHRONIZED → VISIBLE → FOCUSED — and shouldRender only latches true once VISIBLE.
	//
	// The provider used to skip the ENTIRE pump whenever it couldn't produce a real
	// frame — a bare `return` when textures weren't wrapped yet, and (pre-#163df07)
	// again when shouldRender was false. Under the shell/IPC service the session starts
	// SYNCHRONIZED (shouldRender=false) and needs frames to climb to VISIBLE, so those
	// skips stalled the loop: the session never left SYNCHRONIZED and the tile stayed
	// black (HW round 2: xrEndFrame=1, no submits). On the app-owned overlay path this
	// was masked — the focused overlay pins the session at FOCUSED so shouldRender is
	// always true and textures are always ready. Fix: begin AND end a frame here every
	// tick regardless; only take the render path when we actually have textures AND the
	// runtime asks us to render.
	uint32_t img = 0;
	int should_render = 0;
	if (!dxr_prov_begin_frame(&img, &should_render))
		return kUnitySubsystemErrorCodeSuccess; // session not ready — no frame begun, nothing to end

	// Trace the pump's view of the loop (first frames + periodically) so a shell run
	// shows whether shouldRender ever latches true (session reached VISIBLE) or stays
	// false forever — the single fact that pins a black-tile regression here.
	{
		static unsigned s_pump_n = 0;
		int renderable = (s_textures_created && should_render);
		if (s_pump_n < 12 || (s_pump_n % 600) == 0) {
			char m[176];
			snprintf(m, sizeof(m),
			         "[DisplayXR-PROV] pump[%u]: should_render=%d textures=%d -> %s\n",
			         s_pump_n, should_render, s_textures_created ? 1 : 0,
			         renderable ? "render" : "empty-frame");
			prov_log(m);
		}
		s_pump_n++;
	}

	// Not renderable this tick (textures not wrapped yet, or shouldRender=false — e.g.
	// the session is still SYNCHRONIZED / the tile is occluded under the shell service):
	// submit a 0-layer xrEndFrame. This pairs every xrBeginFrame, releases the acquired
	// image (fatal to leak on the 1-image IPC swapchain), keeps xrWaitFrame pacing, and
	// — the point of pumping unconditionally — lets the runtime advance toward VISIBLE.
	if (!s_textures_created || !should_render) {
		dxr_prov_end_frame_empty();
		return kUnitySubsystemErrorCodeSuccess;
	}

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
		// zero-copy (player) and Metal zero-copy SPI (#205): Unity renders directly
		// into the acquired runtime swapchain image, so pick the texture wrapping
		// THIS frame's acquired image.
		bool rotate = (dxr_prov_get_graphics_api() == DXR_GFX_D3D11 && !dxr_prov_d3d11_bridge_active())
		              || dxr_prov_get_graphics_api() == DXR_GFX_METAL;
		UnityXRNextFrameDesc::UnityXRRenderPass &pass = next->renderPasses[0];
		pass.textureId = rotate ? s_tex_ids[s_current_image_index] : s_tex_ids[0];
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
			pass.textureId = (dxr_prov_get_graphics_api() == DXR_GFX_METAL)
			                     ? s_tex_ids[s_current_image_index * 2 + eye]
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
#ifdef __APPLE__
				// Metal zero-copy: pick the texture wrapping THIS zone's acquired image.
				if (dxr_prov_get_graphics_api() == DXR_GFX_METAL)
					pass.textureId = s_extra_tex_ids_metal[i][dxr_prov_get_extra_zone_acquired_index(i)];
#endif
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
#ifdef __APPLE__
					// Metal zero-copy: this zone's acquired image, per-eye slice view.
					if (dxr_prov_get_graphics_api() == DXR_GFX_METAL)
						pass.textureId = s_extra_tex_ids_metal[i][dxr_prov_get_extra_zone_acquired_index(i) * 2 + eye];
#endif
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
	// Trace whether Unity drives the submit callback at all, and whether a frame was in
	// flight to submit — the missing half of the acquire→render→release→endFrame cycle
	// (shell/IPC bring-up: acquire spins but nothing is ever released/submitted). Written
	// to %TEMP%\displayxr_prov_native.log (the provider's own log; NOT Player.log).
	{
		static unsigned s_sub_n = 0;
		if (s_sub_n < 12 || (s_sub_n % 600) == 0) {
			char m[144];
			snprintf(m, sizeof(m),
			         "[DisplayXR-PROV] submit[%u]: session_active=%d frame_in_flight=%d\n",
			         s_sub_n, s_session_active ? 1 : 0, s_frame_in_flight ? 1 : 0);
			prov_log(m);
		}
		s_sub_n++;
	}
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
#ifdef _WIN32
	if (s_mblit_list)  { s_mblit_list->Release();  s_mblit_list  = nullptr; }
	if (s_mblit_alloc) { s_mblit_alloc->Release(); s_mblit_alloc = nullptr; }
#endif
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

// Native mirror blit (maximize seam fix, gated by DISPLAYXR_PROV_NATIVE_MIRROR=1). Unity's
// default mirror blit tiles a large RT into top/bottom halves, which breaks the woven
// lenticular interlace phase at the RT midline (a seam only visible for tall maximized RTs;
// proven: the woven shared texture is phase-continuous, the on-screen present is not). Doing
// the blit ourselves — one CopyTextureRegion of the woven canvas into the whole RT — bypasses
// Unity's tiling. Falls back to blitParams if the env is unset, the interface/handles are
// missing, or the formats aren't copy-compatible (logged; would need a shader blit).
static int s_native_mirror_env = -1;
static bool native_mirror_enabled()
{
	// DEFAULT ON: the native shader-blit is the GameView mirror path (only engages when a woven
	// texture exists, i.e. editor docked/texture mode). Unity's own blitParams mirror produces a
	// BLACK docked Game view for URP (its blit is discarded inside URP's RenderGraph GameView
	// present) — the native draw writes straight to the presented mirror RT. Opt out with
	// DISPLAYXR_PROV_NATIVE_MIRROR=0 (restores Unity's blitParams path).
	if (s_native_mirror_env < 0) {
		const char *e = getenv("DISPLAYXR_PROV_NATIVE_MIRROR");
		s_native_mirror_env = (e && e[0] == '0') ? 0 : 1; // default ON
	}
	return s_native_mirror_env == 1;
}

#ifdef _WIN32  // native mirror blit is D3D12-only (macOS stub below)
// Native shader blit (urp-editor-black-screen fix): sample the woven texture (BGRA, fmt 87)
// and DRAW it into the GameView mirror RT (RGBA, fmt 28) — a format-converting blit that
// CopyTextureRegion cannot do (RGBA and BGRA are different DXGI copy groups). Unity's own
// blitParams mirror path produces black for URP (its blit is issued inside URP's RenderGraph
// GameView present and gets discarded), whereas this native callback writes straight to the
// presented mirror RT — proven by the red-clear test. Pipeline is built once per RT format.
static ID3D12RootSignature  *s_blit_rs       = nullptr;
static ID3D12PipelineState  *s_blit_pso      = nullptr;
static ID3D12DescriptorHeap *s_blit_srvheap  = nullptr; // shader-visible, 1 SRV
static ID3D12DescriptorHeap *s_blit_rtvheap  = nullptr; // 1 RTV
static DXGI_FORMAT           s_blit_pso_fmt   = DXGI_FORMAT_UNKNOWN;

static bool prov_ensure_blit_pipeline(ID3D12Device *dev, DXGI_FORMAT rtFmt)
{
	if (s_blit_pso && s_blit_pso_fmt == rtFmt) return true;
	if (s_blit_pso) { s_blit_pso->Release(); s_blit_pso = nullptr; }
	if (!s_blit_srvheap) {
		D3D12_DESCRIPTOR_HEAP_DESC hd = {};
		hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = 1;
		hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&s_blit_srvheap)))) return false;
	}
	if (!s_blit_rtvheap) {
		D3D12_DESCRIPTOR_HEAP_DESC hd = {};
		hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = 1;
		if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&s_blit_rtvheap)))) return false;
	}
	if (!s_blit_rs) {
		D3D12_DESCRIPTOR_RANGE range = {};
		range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; range.NumDescriptors = 1;
		range.BaseShaderRegister = 0; range.OffsetInDescriptorsFromTableStart = 0;
		D3D12_ROOT_PARAMETER params[2] = {};
		params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
		params[0].Constants.ShaderRegister = 0; params[0].Constants.Num32BitValues = 4;
		params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		params[1].DescriptorTable.NumDescriptorRanges = 1;
		params[1].DescriptorTable.pDescriptorRanges = &range;
		params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
		D3D12_STATIC_SAMPLER_DESC samp = {};
		samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
		samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		samp.ShaderRegister = 0; samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
		D3D12_ROOT_SIGNATURE_DESC rsd = {};
		rsd.NumParameters = 2; rsd.pParameters = params;
		rsd.NumStaticSamplers = 1; rsd.pStaticSamplers = &samp;
		rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
		ID3DBlob *sig = nullptr, *err = nullptr;
		if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err))) {
			if (err) err->Release(); return false; }
		HRESULT hr = dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
		                                      IID_PPV_ARGS(&s_blit_rs));
		sig->Release(); if (err) err->Release();
		if (FAILED(hr)) return false;
	}
	static const char *kHLSL =
		"cbuffer C : register(b0){ float4 srcRect; }\n"
		"Texture2D tex : register(t0); SamplerState smp : register(s0);\n"
		"struct V{ float4 pos:SV_Position; float2 uv:TEXCOORD0; };\n"
		"V VSMain(uint id:SV_VertexID){ V o; float2 t=float2((id<<1)&2, id&2);\n"
		"  o.uv=t; o.pos=float4(t*float2(2,-2)+float2(-1,1),0,1); return o; }\n"
		"float4 PSMain(V i):SV_Target{ float2 uv=srcRect.xy + i.uv*srcRect.zw; return tex.Sample(smp, uv); }\n";
	ID3DBlob *vs = nullptr, *ps = nullptr, *e1 = nullptr, *e2 = nullptr;
	if (FAILED(D3DCompile(kHLSL, strlen(kHLSL), nullptr, nullptr, nullptr, "VSMain", "vs_5_0", 0, 0, &vs, &e1)) ||
	    FAILED(D3DCompile(kHLSL, strlen(kHLSL), nullptr, nullptr, nullptr, "PSMain", "ps_5_0", 0, 0, &ps, &e2))) {
		if (vs) vs->Release(); if (ps) ps->Release(); if (e1) e1->Release(); if (e2) e2->Release();
		prov_log("[DisplayXR-PROV] native mirror: shader compile failed\n"); return false;
	}
	D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
	pd.pRootSignature = s_blit_rs;
	pd.VS.pShaderBytecode = vs->GetBufferPointer(); pd.VS.BytecodeLength = vs->GetBufferSize();
	pd.PS.pShaderBytecode = ps->GetBufferPointer(); pd.PS.BytecodeLength = ps->GetBufferSize();
	pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
	pd.SampleMask = 0xFFFFFFFFu;
	pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	pd.NumRenderTargets = 1; pd.RTVFormats[0] = rtFmt;
	pd.SampleDesc.Count = 1;
	HRESULT hr = dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&s_blit_pso));
	vs->Release(); ps->Release();
	if (FAILED(hr)) { prov_log("[DisplayXR-PROV] native mirror: PSO create failed\n"); return false; }
	s_blit_pso_fmt = rtFmt;
	prov_log("[DisplayXR-PROV] native mirror: shader-blit pipeline ready\n");
	return true;
}

// Draw the woven canvas sub-rect (src pixels [cx,cy cw x ch] of a tw x th texture) into the
// full GameView mirror RT. Returns true if it recorded+executed the draw.
static bool prov_shader_blit_woven(ID3D12Device *dev, IUnityGraphicsD3D12v8 *v8,
                                   ID3D12Resource *rt, ID3D12Resource *src,
                                   int32_t cx, int32_t cy, int32_t cw, int32_t ch,
                                   uint32_t tw, uint32_t th)
{
	if (!dev || !v8 || !rt || !src || tw == 0 || th == 0) return false;
	D3D12_RESOURCE_DESC rd = rt->GetDesc();
	if (!prov_ensure_blit_pipeline(dev, rd.Format)) return false;
	if (!s_mblit_alloc)
		dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&s_mblit_alloc));
	if (!s_mblit_list && s_mblit_alloc) {
		dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s_mblit_alloc, nullptr, IID_PPV_ARGS(&s_mblit_list));
		if (s_mblit_list) s_mblit_list->Close();
	}
	if (!s_mblit_alloc || !s_mblit_list) return false;

	// SRV for the woven source (BGRA).
	D3D12_CPU_DESCRIPTOR_HANDLE srvCpu = s_blit_srvheap->GetCPUDescriptorHandleForHeapStart();
	D3D12_GPU_DESCRIPTOR_HANDLE srvGpu = s_blit_srvheap->GetGPUDescriptorHandleForHeapStart();
	D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
	sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	sv.Format = src->GetDesc().Format; sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	sv.Texture2D.MipLevels = 1;
	dev->CreateShaderResourceView(src, &sv, srvCpu);
	// RTV for the mirror RT.
	D3D12_CPU_DESCRIPTOR_HANDLE rtvCpu = s_blit_rtvheap->GetCPUDescriptorHandleForHeapStart();
	D3D12_RENDER_TARGET_VIEW_DESC rv = {}; rv.Format = rd.Format; rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
	dev->CreateRenderTargetView(rt, &rv, rtvCpu);

	s_mblit_alloc->Reset();
	s_mblit_list->Reset(s_mblit_alloc, s_blit_pso);
	D3D12_RESOURCE_BARRIER b = {}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	b.Transition.pResource = rt; b.Transition.Subresource = 0;
	b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
	b.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
	s_mblit_list->ResourceBarrier(1, &b);
	s_mblit_list->OMSetRenderTargets(1, &rtvCpu, FALSE, nullptr);
	D3D12_VIEWPORT vp = { 0.0f, 0.0f, (float)rd.Width, (float)rd.Height, 0.0f, 1.0f };
	D3D12_RECT sc = { 0, 0, (LONG)rd.Width, (LONG)rd.Height };
	s_mblit_list->RSSetViewports(1, &vp);
	s_mblit_list->RSSetScissorRects(1, &sc);
	s_mblit_list->SetGraphicsRootSignature(s_blit_rs);
	ID3D12DescriptorHeap *heaps[] = { s_blit_srvheap };
	s_mblit_list->SetDescriptorHeaps(1, heaps);
	float srcRect[4] = { (float)cx / tw, (float)cy / th, (float)cw / tw, (float)ch / th };
	s_mblit_list->SetGraphicsRoot32BitConstants(0, 4, srcRect, 0);
	s_mblit_list->SetGraphicsRootDescriptorTable(1, srvGpu);
	s_mblit_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	s_mblit_list->DrawInstanced(3, 1, 0, 0);
	b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
	b.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
	s_mblit_list->ResourceBarrier(1, &b);
	s_mblit_list->Close();
	UnityGraphicsD3D12ResourceState st = {};
	st.resource = rt; st.expected = D3D12_RESOURCE_STATE_COPY_DEST; st.current = D3D12_RESOURCE_STATE_COPY_DEST;
	v8->ExecuteCommandList(s_mblit_list, 1, &st);
	static bool logged = false;
	if (!logged) { logged = true;
		prov_log("[DisplayXR-PROV] native mirror: shader-blit ACTIVE (woven BGRA -> GameView RT RGBA)\n"); }
	return true;
}

static UnitySubsystemErrorCode UNITY_INTERFACE_API
MainBlitToMirrorViewRenderTarget(UnitySubsystemHandle handle, void *userData,
                                 const UnityXRMirrorViewBlitInfo info)
{
	(void)handle; (void)userData;
	if (!native_mirror_enabled() || s_woven_tex_id == 0) return kUnitySubsystemErrorCodeSuccess;
	if (!info.mirrorRtDesc || !info.mirrorRtDesc->rtNative || !s_ifaces) return kUnitySubsystemErrorCodeSuccess;
	IUnityGraphicsD3D12v8 *v8 = s_ifaces->Get<IUnityGraphicsD3D12v8>();
	if (!v8) return kUnitySubsystemErrorCodeSuccess;
	ID3D12Device *dev = v8->GetDevice();
	ID3D12Resource *rt = v8->TextureFromRenderBuffer(info.mirrorRtDesc->rtNative);

	uint32_t ww = 0, wh = 0;
	ID3D12Resource *src = (ID3D12Resource *)dxr_prov_get_woven_unity_texture(&ww, &wh);
	if (!dev || !rt || !src) return kUnitySubsystemErrorCodeSuccess;

	int32_t cx = 0, cy = 0, cw = 0, ch = 0; uint32_t tw = 0, th = 0;
	dxr_prov_get_woven_canvas(&cx, &cy, &cw, &ch, &tw, &th);
	if (cw <= 0 || ch <= 0 || tw == 0 || th == 0) return kUnitySubsystemErrorCodeSuccess;
	// Format-converting SHADER blit: sample the woven canvas sub-rect (BGRA) and draw it,
	// stretched to fill the GameView mirror RT (RGBA). CopyTextureRegion can't cross the
	// RGBA/BGRA copy groups, and Unity's blitParams mirror produces black for URP (discarded
	// inside URP's RenderGraph present) — this native draw writes straight to the presented RT.
	prov_shader_blit_woven(dev, v8, rt, src, cx, cy, cw, ch, tw, th);
	return kUnitySubsystemErrorCodeSuccess;
}
#else  // !_WIN32 — macOS: the native mirror blit is D3D12-only; woven mirror not used.
static UnitySubsystemErrorCode UNITY_INTERFACE_API
MainBlitToMirrorViewRenderTarget(UnitySubsystemHandle handle, void *userData,
                                 const UnityXRMirrorViewBlitInfo info)
{
	(void)handle; (void)userData; (void)info;
	return kUnitySubsystemErrorCodeSuccess;
}
#endif // _WIN32

UnitySubsystemErrorCode UNITY_INTERFACE_API
MainQueryMirrorViewBlitDesc(UnitySubsystemHandle handle, void *userData,
                            const UnityXRMirrorViewBlitInfo info, UnityXRMirrorViewBlitDesc *desc)
{
	(void)handle; (void)userData;
	{
		static bool q_logged = false;
		if (!q_logged) { q_logged = true;
			char buf[128];
			_snprintf_s(buf, sizeof(buf), _TRUNCATE,
			            "[DisplayXR-PROV] MainQueryMirrorViewBlitDesc CALLED (mode=%d)\n",
			            info.mirrorBlitMode);
			prov_log(buf);
		}
	}
	if (!desc) return kUnitySubsystemErrorCodeSuccess;
	// When the native mirror blit is enabled and a woven texture exists, tell Unity we'll do
	// the blit ourselves (MainBlitToMirrorViewRenderTarget) — avoids Unity's tiling seam. The
	// blitParams below are still filled as a fallback (Unity may skip the native callback).
	desc->nativeBlitAvailable = (native_mirror_enabled() && s_woven_tex_id != 0);
	desc->nativeBlitInvalidStates = false;
	// GameView weave-to-texture mirror (Task (a)): if texture mode published a woven
	// texture, blit its canvas sub-rect into the Game window. Otherwise no mirror blit
	// (the runtime weaves to its own window — the shipping default).
	if (s_woven_tex_id != 0) {
		int32_t cx = 0, cy = 0, cw = 0, ch = 0; uint32_t tw = 0, th = 0;
		dxr_prov_get_woven_canvas(&cx, &cy, &cw, &ch, &tw, &th);
		if (tw > 0 && th > 0 && cw > 0 && ch > 0) {
			// --- Calibration mode (Task (a) fill, env DISPLAYXR_PROV_MIRROR_CALIB) -------
			// Instead of the compose-fill math, emit FIXED, KNOWN blitParams so a single
			// screenshot pins Unity's mirror mapping empirically. The woven texture is itself
			// a known asymmetric pattern: the canvas is a filled rect in the TOP-LEFT of an
			// otherwise-black tw×th field. CALIB=1: whole tex → whole RT (reveals RT→panel
			// present + UV origin/Y-flip). CALIB=2: shrunk copies of the whole tex at known
			// destRects incl. one out-of-range (reveals the destRect coordinate system +
			// clip-vs-clamp past [0,1]).
			static int s_calib = -1;
			if (s_calib < 0) {
				const char *e = getenv("DISPLAYXR_PROV_MIRROR_CALIB");
				s_calib = (e && e[0]) ? atoi(e) : 0;
			}
			// NOTE: info.mirrorRtDesc reports the Game-view render RT in LOGICAL px on a
			// HiDPI display (e.g. 879x374 at ppp=2.5), NOT the physical panel px — so it is
			// UNUSABLE as the zone/canvas size (proven on the 3840x2160@250% dev display: it
			// forces a 2.5x min Game-view Scale, and both rtScaled and rtOriginal come back
			// 879x374 = the logical size). The authoritative physical panel px is supplied by
			// C# (mainSize x ppp) via dxr_prov_set_panel_px instead. Kept here for diagnostics.
			int rtSW0 = 0, rtSH0 = 0, rtOW0 = 0, rtOH0 = 0, rtArr0 = 0, rtSamp0 = 0, rtMip0 = 0;
			if (info.mirrorRtDesc) {
				rtSW0 = info.mirrorRtDesc->rtScaledWidth;
				rtSH0 = info.mirrorRtDesc->rtScaledHeight;
				rtOW0 = info.mirrorRtDesc->rtOriginalWidth;
				rtOH0 = info.mirrorRtDesc->rtOriginalHeight;
				rtArr0 = info.mirrorRtDesc->rtDepthOrArraySize;
				rtSamp0 = info.mirrorRtDesc->rtSamples;
				rtMip0 = info.mirrorRtDesc->rtMipCount;
			}
			if (s_calib != 0) {
				UnityXRRectf full = { 0.0f, 0.0f, 1.0f, 1.0f };
				if (s_calib == 2) {
					// Corner + center + out-of-range marker tiles, whole tex each.
					const UnityXRRectf dests[5] = {
						{ 0.00f, 0.00f, 0.25f, 0.25f }, // top-left in dest-space
						{ 0.75f, 0.00f, 0.25f, 0.25f }, // top-right
						{ 0.00f, 0.75f, 0.25f, 0.25f }, // bottom-left
						{ 0.375f, 0.375f, 0.25f, 0.25f }, // center
						{ 0.90f, 0.90f, 0.40f, 0.40f }, // deliberately past [0,1] (clip vs clamp)
					};
					desc->blitParamsCount = 5;
					for (int i = 0; i < 5; i++) {
						desc->blitParams[i].srcTexId = s_woven_tex_id;
						desc->blitParams[i].srcTexArraySlice = 0;
						desc->blitParams[i].srcRect = full;
						desc->blitParams[i].destRect = dests[i];
					}
				} else {
					// CALIB=1 (and any other nonzero): whole tex → whole RT, 1:1.
					desc->blitParamsCount = 1;
					desc->blitParams[0].srcTexId = s_woven_tex_id;
					desc->blitParams[0].srcTexArraySlice = 0;
					desc->blitParams[0].srcRect = full;
					desc->blitParams[0].destRect = full;
				}
				static int c_count = 0;
				if ((c_count++ % 120) == 0) {
					char buf[256];
					_snprintf_s(buf, sizeof(buf), _TRUNCATE,
					            "[DisplayXR-PROV] mirror CALIB=%d: params=%d canvas=(%dx%d) tex=%ux%u "
					            "mirrorRT scaled=%dx%d original=%dx%d\n",
					            s_calib, desc->blitParamsCount, cw, ch, tw, th,
					            rtSW0, rtSH0, rtOW0, rtOH0);
					prov_log(buf);
				}
				return kUnitySubsystemErrorCodeSuccess;
			}
			// --------------------------------------------------------------------------
			// Two-tile mirror (DISPLAYXR_PROV_MIRROR_2TILE=1): the phase seam was localized to
			// the RT vertical MIDLINE (RT-geometric, not content) — Unity splits our single
			// full-RT blit into a top/bottom half with a ~few-px offset. Hand it the two halves
			// pre-split (contiguous source regions → contiguous dest halves) so Unity blits each
			// as-is with no internal midline split. If the seam persists it's downstream of the
			// blit (RT->panel present), which we can't reach.
			static int s_2tile = -1;
			if (s_2tile < 0) { const char *e = getenv("DISPLAYXR_PROV_MIRROR_2TILE"); s_2tile = (e && e[0] == '1') ? 1 : 0; }
			if (s_2tile) {
				float sx = (float)cx / tw, sw = (float)cw / tw;
				float shTop = (float)ch * 0.5f / th;      // half the content height, normalized
				float syTop = (float)cy / th;
				float syBot = ((float)cy + (float)ch * 0.5f) / th;
				desc->blitParamsCount = 2;
				desc->blitParams[0].srcTexId = s_woven_tex_id;
				desc->blitParams[0].srcTexArraySlice = 0;
				desc->blitParams[0].srcRect = { sx, syTop, sw, shTop };
				desc->blitParams[0].destRect = { 0.0f, 0.0f, 1.0f, 0.5f };
				desc->blitParams[1].srcTexId = s_woven_tex_id;
				desc->blitParams[1].srcTexArraySlice = 0;
				desc->blitParams[1].srcRect = { sx, syBot, sw, shTop };
				desc->blitParams[1].destRect = { 0.0f, 0.5f, 1.0f, 0.5f };
				static bool t_logged = false;
				if (!t_logged) { t_logged = true; char b[192];
					_snprintf_s(b, sizeof(b), _TRUNCATE,
					            "[DisplayXR-PROV] mirror 2-tile: top src=(%.3f,%.3f %.3f,%.3f) bot src y=%.3f -> dest halves\n",
					            sx, syTop, sw, shTop, syBot);
					prov_log(b); }
				return kUnitySubsystemErrorCodeSuccess;
			}
			desc->blitParamsCount = 1;
			desc->blitParams[0].srcTexId = s_woven_tex_id;
			desc->blitParams[0].srcTexArraySlice = 0;
			desc->blitParams[0].srcRect = { (float)cx / tw, (float)cy / th,
			                                (float)cw / tw, (float)ch / th };
			// Unity's game-view mirror blit is a STANDARD stretch: it maps the srcRect
			// region of the source texture onto the destRect region of the mirror RT
			// (verified by CALIB=1 — srcRect=(0,0,1,1)/destRect=(0,0,1,1) maps the whole
			// texture 1:1 to the RT — and by the over-zoom observed when destRect was set
			// to 1/srcRect, which scaled the content by exactly that factor). So to fill
			// the RT with the woven region, the destRect is simply the full RT. The zone is
			// born at the panel's render size (== the mirror RT), so this stretch is ~1:1
			// (no resample of the woven lenticular pixels).
			desc->blitParams[0].destRect = { 0.0f, 0.0f, 1.0f, 1.0f };
			{
				// Throttled diagnostic (~every 120 calls): the live canvas + tex dims +
				// normalized srcRect, so a truncation (canvas/srcRect desync) is visible.
				static int q_count = 0;
				if ((q_count++ % 120) == 0) {
					char buf[288];
					_snprintf_s(buf, sizeof(buf), _TRUNCATE,
					            "[DisplayXR-PROV] mirror blit: canvas=(%d,%d %dx%d) tex=%ux%u "
					            "srcRect=(%.3f,%.3f %.3f,%.3f) destRect=(%.3f,%.3f %.3f,%.3f) "
					            "mirrorRT scaled=%dx%d original=%dx%d arraySize=%d samples=%d mips=%d\n",
					            cx, cy, cw, ch, tw, th,
					            desc->blitParams[0].srcRect.x, desc->blitParams[0].srcRect.y,
					            desc->blitParams[0].srcRect.width, desc->blitParams[0].srcRect.height,
					            desc->blitParams[0].destRect.x, desc->blitParams[0].destRect.y,
					            desc->blitParams[0].destRect.width, desc->blitParams[0].destRect.height,
					            rtSW0, rtSH0, rtOW0, rtOH0, rtArr0, rtSamp0, rtMip0);
					prov_log(buf);
				}
			}
			return kUnitySubsystemErrorCodeSuccess;
		}
	}
	desc->blitParamsCount = 0;
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
	gfx.BlitToMirrorViewRenderTarget = MainBlitToMirrorViewRenderTarget; // gated by nativeBlitAvailable
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
#if defined(__APPLE__)
	// macOS: bind the runtime's weave to an NSView (via XrCocoaWindowBindingCreateInfoDXR).
	// Two shapes, both on the MAIN thread (AppKit windows can't be created off-main; and
	// the runtime's own self-host NSWindow-create inside xrCreateSession — GfxStart =
	// render thread while the main thread blocks in GfxStart — deadlocks; the macOS
	// analog of the Windows #173 lesson):
	//   - Built player (#205): weave into UNITY'S OWN game window via an input-transparent
	//     overlay NSView (displayxr_get_app_main_view; hitTest→nil passes mouse/keyboard
	//     through to Unity). One window, native input — mirrors the Windows app-owned
	//     overlay. The window is grown to the display aspect.
	//   - Editor Play Mode (#204, opt-in): a SEPARATE preview window that coexists with
	//     the editor (binding Unity's editor window would track the whole editor).
	if (prov_want_dedicated_window()) {
		s_overlay_hwnd = displayxr_metal_create_preview_window(0, 0);
		prov_log(s_overlay_hwnd
		             ? "[DisplayXR-PROV] Lifecycle Start (macOS editor: dedicated preview window)\n"
		             : "[DisplayXR-PROV] Lifecycle Start (macOS editor: preview window FAILED)\n");
	} else {
		s_overlay_hwnd = displayxr_get_app_main_view();
		if (!s_overlay_hwnd) {
			// Unity's window not ready yet — fall back to a standalone weave window.
			s_overlay_hwnd = displayxr_metal_create_preview_window(0, 0);
			prov_log("[DisplayXR-PROV] Lifecycle Start (macOS player: no app window yet; standalone weave window)\n");
		} else {
			prov_log("[DisplayXR-PROV] Lifecycle Start (macOS player: in-app weave overlay on Unity's window)\n");
		}
	}
#elif defined(__linux__) && !defined(__ANDROID__)
	// Linux (#249): HANDLE app — resolve the player's own top-level X11 window here,
	// on the main thread, before GfxStart binds the session (same ordering as the
	// Windows overlay HWND and the macOS NSView). displayxr_linux.c caches the
	// result; dxr_prov_session_start chains it as XR_DXR_xlib_window_binding.
	//
	// s_overlay_hwnd stays NULL: on Windows/macOS it carries the native window the
	// session binds, but the xlib binding needs a (Display*, Window) PAIR that does
	// not fit one pointer — so the session pulls both from displayxr_linux.c rather
	// than through this field.
	{
		void *xdpy = nullptr;
		unsigned long xwin = 0;
		bool have = displayxr_linux_get_weave_window(&xdpy, &xwin) != 0;
		s_overlay_hwnd = nullptr;
		prov_log(have
		             ? "[DisplayXR-PROV] Lifecycle Start (Linux: plugin-owned overlay weave window)\n"
		             : "[DisplayXR-PROV] Lifecycle Start (Linux: no overlay window — runtime self-hosted weave window)\n");
	}
#else
	if (displayxr_is_shell_mode()) {
		// Workspace tile (shell/IPC). The Shell launched us with
		// DISPLAYXR_WORKSPACE_SESSION=1 (window pre-hidden SW_HIDE, unelevated), and the
		// runtime reads the same var → forces IPC/service mode → the in-process compositor
		// is disabled and the client_d3d12_compositor submits our OpenXR frames to
		// comp_d3d11_service, which composites the tile. So we own NO window and present
		// nothing:
		//   - No overlay (a self-weaving in-process overlay never connects to the service).
		//   - No SetForegroundWindow — the Shell is the foreground app and forwards input.
		//   - No focus/raw-input hook — input routes to Unity's OWN window naturally
		//     (displayxr_is_our_process_foreground() already returns 1 in shell mode so the
		//     app's input controller doesn't drop shell-forwarded input).
		// Window binding: default windowHandle=NULL (session_start passes s_overlay_hwnd).
		// In service mode the runtime's in-process compositor is off, so NULL will NOT
		// spawn a stray self-host window — it means "client submits to the service". This
		// is the same binding shape as SELFHOST, which is a proven session-create path.
		// EMPIRICAL A/B (runtime-side hardware bring-up): if the client compositor turns
		// out to require a non-NULL window handle, DISPLAYXR_WORKSPACE_BIND_UNITY_HWND=1
		// binds Unity's OWN (shell-hidden) main HWND instead — mirroring the native
		// cube_handle_* sample under the shell. No overlay is created either way.
		if (getenv("DISPLAYXR_WORKSPACE_BIND_UNITY_HWND") != nullptr) {
			s_overlay_hwnd = displayxr_get_unity_main_hwnd();
			prov_log(s_overlay_hwnd
			             ? "[DisplayXR-PROV] Lifecycle Start (workspace tile: bound Unity's own hidden main HWND — A/B fallback)\n"
			             : "[DisplayXR-PROV] Lifecycle Start (workspace tile: Unity HWND not found; windowHandle=NULL)\n");
		} else {
			s_overlay_hwnd = nullptr;
			prov_log("[DisplayXR-PROV] Lifecycle Start (workspace tile: shell/IPC — windowHandle=NULL, no overlay/foreground)\n");
		}
		// KEEP UNITY RENDERING. The Shell launches us window-hidden (SW_HIDE) and never gives
		// us OS foreground — so Unity's player sees itself as background/unfocused and STOPS
		// rendering its cameras into the XR eye textures (the bridge reads back all-zero even
		// though the OpenXR session is FOCUSED and shouldRender=1). Install the focus hook: it
		// IAT-hooks GetForegroundWindow/GetFocus in Unity's process to return Unity's own HWND
		// (so Application.isFocused stays true → the player keeps rendering) + suppresses
		// deactivation. Unlike the app-overlay path we do NOT SetForegroundWindow — that would
		// fight the Shell for real OS foreground; the hook is a per-process lie that never
		// touches the Shell. (#223 r12: black tile == Unity not drawing into the bridge.)
		void *unity_hwnd = displayxr_get_unity_main_hwnd();
		if (unity_hwnd != nullptr) {
			displayxr_install_focus_hook(unity_hwnd);
			prov_log("[DisplayXR-PROV] Lifecycle Start (workspace tile: installed focus hook)\n");
		} else {
			prov_log("[DisplayXR-PROV] Lifecycle Start (workspace tile: WARN Unity HWND not found — focus hook NOT installed)\n");
		}
	} else if (prov_want_dedicated_window()) {
		// BINDPANE experiment (#740): C# supplied Unity's own Game-view pane HWND —
		// bind THAT (the SR SDK tracks the real content window natively; the zone
		// carries the render-area offset within its client). No dedicated window.
		// (dxr_prov_get_external_weave_hwnd is declared in displayxr_provider_session.h.)
		void *ext = dxr_prov_get_external_weave_hwnd();
		if (ext) {
			s_overlay_hwnd = ext;
			prov_log("[DisplayXR-PROV] Lifecycle Start (BINDPANE: bound Unity pane HWND — no dedicated window)\n");
		} else {
		// (#173) Editor Play Mode: a STANDALONE movable weave window that does NOT
		// track Unity's (whole-editor) window — coexists with the editor while still
		// binding a real HWND (window-relative Kooima + #172 realloc). See
		// displayxr_win32.c for the WS_OVERLAPPEDWINDOW + NOACTIVATE/TOPMOST + DPI
		// recipe (mirrors the proven standalone preview window).
		s_overlay_hwnd = displayxr_create_provider_dedicated_window();
		prov_log(s_overlay_hwnd
		             ? "[DisplayXR-PROV] Lifecycle Start (dedicated provider window created, #173)\n"
		             : "[DisplayXR-PROV] Lifecycle Start (dedicated window FAILED; runtime self-hosts)\n");
		}

		// Keyboard/mouse input (#166 task #9): install the same focus / raw-input
		// hooks the app-owned overlay uses, so Unity's Input System keeps receiving
		// input (RIDEV_INPUTSINK + GetForegroundWindow/GetFocus → Unity) even if the
		// separate weave window is ever the OS-foreground window. The window is
		// WS_EX_NOACTIVATE so it should not steal foreground from the editor in the
		// first place (that alone keeps input alive, like the standalone preview) —
		// the hook is belt-and-braces / parity with the overlay path.
		//
		// EXCEPTION — GameView weave-to-texture probe (Task (a)): the weave window is
		// glued OVER the editor Game view and the editor IS the foreground app, so the
		// hook is not needed here. Worse, its main-window subclass reclaims focus on
		// WM_KILLFOCUS (SetFocus) and suppresses WM_ACTIVATE — meant for a cloaked/
		// off-screen Unity — which FIGHTS the Game view taking focus for a mouse drag
		// (keyboard still works via the raw-input sink, so it looked like "mouse dead,
		// keyboard fine"). Skip it in the probe path.
		void *unity_hwnd = displayxr_get_unity_main_hwnd();
		bool probe_texture = (dxr_prov_texture_mode_active() != 0);
		if (unity_hwnd != nullptr && !probe_texture) {
			displayxr_install_focus_hook(unity_hwnd);
			prov_log("[DisplayXR-PROV] Lifecycle Start: installed keyboard focus/raw-input hooks (dedicated window)\n");
		} else if (probe_texture) {
			prov_log("[DisplayXR-PROV] Lifecycle Start: SKIPPED focus hook (texture probe — editor is foreground; hook fights GameView mouse focus)\n");
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
	} else {
		// (#256) Built players: drop the app-owned overlay too. It used to outlive the
		// subsystem — only the editor's dedicated window was destroyed here — so a
		// subsystem that stopped without the process exiting left a TOPMOST window with
		// a frozen last frame in front of a still-running app. Same ordering guarantee
		// as the #173 branch: MAIN thread, after GfxStop ran dxr_prov_session_stop, so
		// xrDestroySession has already released the runtime's references into it. No-op
		// in shell/self-host mode (no overlay was created).
		displayxr_destroy_app_overlay();
		s_overlay_hwnd = nullptr;
	}
#elif defined(__APPLE__)
	// macOS (#204/#205): tear down the weave target on the MAIN thread, after
	// GfxStop already ran dxr_prov_session_stop (xrDestroySession released the
	// runtime's references into the view) — mirrors the #173 teardown ordering.
	// Both are safe no-ops if the other shape was used this session.
	displayxr_metal_destroy_app_overlay();     // built-player in-app overlay
	displayxr_metal_destroy_preview_window();  // editor / fallback window
	s_overlay_hwnd = nullptr;
#else
	// Linux (#249): drop the plugin-owned overlay on the MAIN thread, after GfxStop
	// already ran dxr_prov_session_stop (xrDestroySession released the runtime's
	// surface on it) — same ordering rule as the #173 Windows / #204 macOS teardown.
	displayxr_linux_destroy_weave_window();
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

#ifdef _WIN32
// ============================================================================
// (#242) Unity's adapter LUID, for displayxr_gpu_preference.cpp.
//
// The device getters above live in this file's anonymous namespace (internal
// linkage), so the GPU-preference TU can't call them directly — this is the
// narrow bridge. It only exposes WHICH adapter Unity landed on; classifying
// that adapter as integrated/discrete stays with the rest of the GPU-preference
// logic, mirroring the runtime's VRAM rule in one place.
//
// Valid from plugin load onward (Unity's graphics device exists before the XR
// loader's Initialize()). Returns 1 on success, 0 if no device is reachable.
// ============================================================================

extern "C" int dxr_prov_unity_adapter_luid(int32_t *out_high, uint32_t *out_low)
{
	if (!out_high || !out_low) return 0;
	*out_high = 0;
	*out_low = 0;

	ID3D12Device *d12 = nullptr;
	ID3D12CommandQueue *q = nullptr;
	if (get_unity_d3d12(&d12, &q) && d12) {
		LUID luid = d12->GetAdapterLuid();
		*out_high = luid.HighPart;
		*out_low = luid.LowPart;
		return 1;
	}

	// D3D11 — commonly Unity's device filter denying D3D12 on integrated Intel (#240).
	ID3D11Device *d11 = nullptr;
	if (get_unity_d3d11(&d11) && d11) {
		IDXGIDevice *dxgi = nullptr;
		if (SUCCEEDED(d11->QueryInterface(__uuidof(IDXGIDevice), (void **)&dxgi)) && dxgi) {
			IDXGIAdapter *ad = nullptr;
			if (SUCCEEDED(dxgi->GetAdapter(&ad)) && ad) {
				DXGI_ADAPTER_DESC desc = {};
				if (SUCCEEDED(ad->GetDesc(&desc))) {
					*out_high = desc.AdapterLuid.HighPart;
					*out_low = desc.AdapterLuid.LowPart;
					ad->Release();
					dxgi->Release();
					return 1;
				}
				ad->Release();
			}
			dxgi->Release();
		}
	}
	return 0;
}
#endif // _WIN32

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
