// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Runtime-facing OpenXR session for the custom Unity IUnityXRDisplay provider
// (epic #166, M1). See displayxr_provider_session.h for the design rationale.
//
// Lifts the proven runtime-load / instance / session / locate-views logic from
// displayxr_standalone.cpp, but binds the session to UNITY'S OWN D3D12 device
// (zero-copy) and creates an arraySize=2 (SPI) swapchain. Windows / D3D12 only.

#include "displayxr_provider_session.h"

#include <openxr/openxr.h>
#include "../displayxr_extensions.h"
#include "../displayxr_shared_state.h" // displayxr_state_set_stereo_matrices (overlay hit-test)

#ifdef _WIN32
#include <windows.h>
#include <d3d12.h>
#include <d3d11.h>   // D3D11 zero-copy backend (#195)
#include <d3d11_4.h> // ID3D11Multithread (shared-device weave protection, #195)
#include <dxgi1_4.h>

#pragma comment(lib, "advapi32.lib") // RegGetValueA — ActiveRuntime registry fallback (#173)
#else
#include <dlfcn.h> // dlopen/dlsym runtime load (macOS mirror of LoadLibraryExA)
#include "../displayxr_metal.h" // weave-window NSView backing size (#204)
#define _strdup strdup
#include "displayxr_provider_gfx_metal.h" // Metal blit/texture glue (#204)
#endif

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

// ============================================================================
// OpenXR loader negotiation types (inlined to avoid a loader-header dependency;
// identical to displayxr_standalone.cpp).
// ============================================================================

#define XR_LOADER_INTERFACE_STRUCT_LOADER_INFO     1
#define XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST 3
#define XR_CURRENT_LOADER_RUNTIME_VERSION          1

typedef struct XrNegotiateLoaderInfo {
	uint32_t structType;
	uint32_t structVersion;
	size_t   structSize;
	uint32_t minInterfaceVersion;
	uint32_t maxInterfaceVersion;
	XrVersion minApiVersion;
	XrVersion maxApiVersion;
} XrNegotiateLoaderInfo;

typedef struct XrNegotiateRuntimeRequest {
	uint32_t structType;
	uint32_t structVersion;
	size_t   structSize;
	uint32_t runtimeInterfaceVersion;
	XrVersion runtimeApiVersion;
	PFN_xrGetInstanceProcAddr getInstanceProcAddr;
} XrNegotiateRuntimeRequest;

typedef XrResult (*PFN_xrNegotiateLoaderRuntimeInterface)(
    const XrNegotiateLoaderInfo *loaderInfo,
    XrNegotiateRuntimeRequest *runtimeRequest);

#ifdef _WIN32
// ============================================================================
// XR_KHR_D3D12_enable types (inlined — avoids requiring XR_USE_GRAPHICS_API_D3D12).
// ============================================================================

#ifndef XR_TYPE_GRAPHICS_BINDING_D3D12_KHR
#define XR_TYPE_GRAPHICS_BINDING_D3D12_KHR      ((XrStructureType)1000028000)
#endif
#ifndef XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR
#define XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR       ((XrStructureType)1000028001)
#endif
#ifndef XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR
#define XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR ((XrStructureType)1000028002)
#endif

typedef struct XrGraphicsBindingD3D12KHR {
	XrStructureType type;
	const void *next;
	ID3D12Device *device;
	ID3D12CommandQueue *queue;
} XrGraphicsBindingD3D12KHR;

typedef struct XrSwapchainImageD3D12KHR {
	XrStructureType type;
	void *next;
	ID3D12Resource *texture;
} XrSwapchainImageD3D12KHR;

typedef struct XrGraphicsRequirementsD3D12KHR {
	XrStructureType type;
	void *next;
	LUID adapterLuid;
	D3D_FEATURE_LEVEL minFeatureLevel;
} XrGraphicsRequirementsD3D12KHR;

typedef XrResult (XRAPI_PTR *PFN_xrGetD3D12GraphicsRequirementsKHR)(
    XrInstance instance, XrSystemId systemId,
    XrGraphicsRequirementsD3D12KHR *graphicsRequirements);

// ============================================================================
// XR_KHR_D3D11_enable types (inlined — same convention as the D3D12 block above;
// avoids requiring XR_USE_GRAPHICS_API_D3D11). Used by the zero-copy D3D11 backend
// (#195): the session binds on Unity's ID3D11Device and the runtime's native D3D11
// compositor creates + weaves swapchain images on that device.
// ============================================================================

#ifndef XR_TYPE_GRAPHICS_BINDING_D3D11_KHR
#define XR_TYPE_GRAPHICS_BINDING_D3D11_KHR      ((XrStructureType)1000027000)
#endif
#ifndef XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR
#define XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR       ((XrStructureType)1000027001)
#endif
#ifndef XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR
#define XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR ((XrStructureType)1000027002)
#endif

typedef struct XrGraphicsBindingD3D11KHR {
	XrStructureType type;
	const void *next;
	ID3D11Device *device; // no queue — D3D11 uses the immediate context
} XrGraphicsBindingD3D11KHR;

typedef struct XrSwapchainImageD3D11KHR {
	XrStructureType type;
	void *next;
	ID3D11Texture2D *texture;
} XrSwapchainImageD3D11KHR;

typedef struct XrGraphicsRequirementsD3D11KHR {
	XrStructureType type;
	void *next;
	LUID adapterLuid;
	D3D_FEATURE_LEVEL minFeatureLevel;
} XrGraphicsRequirementsD3D11KHR;

typedef XrResult (XRAPI_PTR *PFN_xrGetD3D11GraphicsRequirementsKHR)(
    XrInstance instance, XrSystemId systemId,
    XrGraphicsRequirementsD3D11KHR *graphicsRequirements);
#endif // _WIN32 (D3D11/D3D12 KHR types; Metal types come from displayxr_extensions.h)

// Vulkan (#247 Windows, #249 Linux): the provider's VK backend lives in
// displayxr_provider_gfx_vulkan.cpp so vulkan.h never reaches this TU. All this TU
// needs is the swapchain-image struct to enumerate into, so declare a
// layout-compatible local mirror of XrSwapchainImageVulkan2KHR. VkImage is a
// VK_DEFINE_NON_DISPATCHABLE_HANDLE, i.e. a pointer on the 64-bit targets this
// backend builds for, so void* matches exactly.
//
// NOTE this block sits OUTSIDE the _WIN32 region above on purpose — the VK backend
// is Windows AND Linux, and burying these declarations in the D3D-types block was
// the first thing that broke the Linux build (#249).
#define XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR_PS ((XrStructureType)1000025001)
typedef struct XrSwapchainImageVulkanKHR_PS {
	XrStructureType type;
	void           *next;
	void           *image; // VkImage
} XrSwapchainImageVulkanKHR_PS;

// The VK glue's C entry points, forward-declared rather than #included for the same
// reason (the header pulls vulkan.h). Kept in sync with
// displayxr_xrprovider/displayxr_provider_gfx_vulkan.h.
#if defined(ENABLE_VULKAN)
extern "C" int  dxr_pvk_create_device(XrInstance, XrSystemId, PFN_xrGetInstanceProcAddr, void *);
extern "C" const void *dxr_pvk_session_binding(const void *next);
extern "C" void dxr_pvk_set_unity_objects(void *, void *, void *, uint32_t, void *);
extern "C" void dxr_pvk_set_swapchain_images(const void *, uint32_t, uint32_t, uint32_t,
                                             uint32_t, int64_t);
extern "C" int  dxr_pvk_create_bridge(int eye, uint32_t, uint32_t, uint32_t, int64_t);
extern "C" void *dxr_pvk_unity_image_ptr(int eye);
extern "C" int  dxr_pvk_copy_to_swapchain_image(int eye, uint32_t image_index);
extern "C" void dxr_pvk_signal_unity_done(void);
extern "C" void dxr_pvk_destroy(void);
extern "C" int  dxr_pvk_device_ready(void);
#endif

// ============================================================================
// Size constants
// ============================================================================

#define PS_MAX_SWAPCHAIN_IMAGES 4
#define PS_MAX_RENDERING_MODES  16
#define PS_MAX_VIEWS            8   // runtime may report max-mode count; we submit 2
#define PS_MAX_ZONES           4   // Unity kUnityXRMaxNumRenderPasses (SPI: 1 pass/zone)

// Additional 3D zone beyond the PRIMARY zone (#166 Phase B2). The primary zone
// (index 0) uses the top-level single-zone fields (swapchain/bridge/views/zone_*),
// which stay the validated Phase-B path. Each extra zone carries its OWN zone-sized
// swapchain + cross-device bridge(s) + located views + Unity render pass, so N 3D
// zones weave into N window-pixel rects (mirrors the runtime cube_zones handle test).
typedef struct ProviderExtraZone {
	int      valid;
	uint32_t zone_id;
	int32_t  rect_x, rect_y, rect_w, rect_h;
	uint32_t rec_w, rec_h;                      // recommended view size (== swapchain size)

	XrSwapchain swapchain;
	int         swapchain_created;
	uint32_t    sc_width, sc_height, sc_image_count;
	int64_t     sc_format;
#ifdef _WIN32
	XrSwapchainImageD3D12KHR sc_images[PS_MAX_SWAPCHAIN_IMAGES];
	// D3D11 (#195): images on the runtime's device. `unity_tex` is the Unity-side
	// 2-slice texture Unity renders into (returned to C#). In PLAYER zero-copy it is a
	// plain Unity-device texture and submit same-device-copies it into the acquired
	// image. In EDITOR bridge mode it is the Unity-opened side of a shared texture whose
	// own-device side is `unity_tex_own`; submit copies own->image on the own context.
	XrSwapchainImageD3D11KHR sc_images_d3d11[PS_MAX_SWAPCHAIN_IMAGES];
	ID3D11Texture2D *unity_tex;
	ID3D11Texture2D *unity_tex_own;   // own-device side (editor bridge; copy source)
	HANDLE           unity_tex_handle;
#else
	// Metal (#206): zero-copy per-eye slice views of each arraySize=2 image (Unity
	// renders straight into them, no bridge/blit). Per-zone cousins of the primary's
	// sc_images_metal / eye_view_metal; PopulateNextFrameDesc rotates to acquired_index.
	XrSwapchainImageMetalKHR sc_images_metal[PS_MAX_SWAPCHAIN_IMAGES];
	void *eye_view_metal[PS_MAX_SWAPCHAIN_IMAGES][2];
#endif
	int         image_acquired;
	uint32_t    acquired_index;

#ifdef _WIN32
	ID3D12Resource *bridge_own, *bridge_unity; HANDLE bridge_handle;              // SPI (2-slice)
	ID3D12Resource *bridge_own_eye[2], *bridge_unity_eye[2]; HANDLE bridge_handle_eye[2]; // MultiPass
#endif

	// Live tile realloc (#172): recreate this zone's swapchain+bridge when its rect
	// (recommended view size) changes. Debounced like the primary; needs_rewrap is a
	// latch consumed by the Unity side so it drops+rewraps this zone's texture(s).
	uint32_t    realloc_pending_w, realloc_pending_h;
	int         realloc_stable;
	int         needs_rewrap;

	DxrProvView views[2];
	uint32_t    view_count;
} ProviderExtraZone;

// ============================================================================
// Session state
// ============================================================================

typedef struct ProviderSession {
	void *runtime_lib;
	PFN_xrGetInstanceProcAddr gipa;

	XrInstance instance;
	XrSystemId system_id;
	XrSession  session;
	XrSpace    local_space;

	XrSessionState session_state;
	int  running;
	int  exit_requested;     // set on XR_SESSION_STATE_EXITING/LOSS_PENDING → C# Application.Quit()
	int  session_ready;
	int  frame_begun;
	int  image_acquired;     // 1 between xrAcquire/Wait and xrRelease (guards double-acquire)
	uint32_t acquired_index;
	XrTime predicted_display_time;

	int has_view_rig;

	// XR_DXR_atlas_capture (#140): app-facing atlas screenshot (the 'I' key /
	// DisplayXRScreenshot). Detected in the probe, enabled on the instance, PFN
	// soft-resolved. Re-derived each session create — no reset preservation needed.
	int has_atlas_capture;

	// XR_DXR_display_zones + XR_DXR_local_3d_zone (#166 Phase B). Detected in
	// session_start (probe) and enabled on the instance. Caps queried lazily on the
	// first frame a zone rect is set (needs a live session). zone_caps_ok: -1 untried,
	// 0 unsupported, 1 supported (maxZones3D>=1).
	int has_display_zones;
	int has_local_3d_zone;
	int zone_caps_ok;
	uint32_t zone_max_3d;
	PFN_xrGetDisplayZoneCapabilitiesDXR       pfn_get_zone_caps;
	PFN_xrGetDisplayZoneRecommendedViewSizeDXR pfn_get_zone_view_size;
	PFN_xrGetWorkspaceTileSizeDXR             pfn_get_workspace_tile_size;

	// Live workspace-tile canvas size in px (#225), polled from
	// xrGetWorkspaceTileSizeDXR. In shell/tile mode this is what the app should
	// author its window/zone/Local2D for (it follows the shell's 3D-window
	// resize, which a minimized tile's own backbuffer can't). 0 = not a tile /
	// not yet known → window path.
	uint32_t tile_px_w, tile_px_h;

	// App-supplied single 3D-zone rect (client-window px, top-left origin). When
	// valid + caps OK, the locate hook chains XrDisplayZoneDXR before the rig
	// (zone-scoped Kooima) and submit chains the same zone on the projection layer,
	// and the swapchain is sized to the zone's recommended view size. w<=0||h<=0
	// clears (full-window framing = the Phase A path). (Single-zone first;
	// generalized to N in a follow-up.)
	int      zone_valid;
	uint32_t zone_id;
	int32_t  zone_x, zone_y, zone_w, zone_h;
	uint32_t zone_rec_w, zone_rec_h; // recommended view size for zone_[wh]

	// Additional 3D zones beyond the primary (#166 Phase B2). extra_zone_count is the
	// number of ACTIVE entries; total 3D zones = 1 (primary) + extra_zone_count.
	// Capped so total zones <= PS_MAX_ZONES (Unity render-pass limit).
	ProviderExtraZone extra_zones[PS_MAX_ZONES - 1];
	uint32_t          extra_zone_count;

	// Graphics backend for this session (#195). DXR_GFX_D3D12 = own-device + shared
	// bridge; DXR_GFX_D3D11 = zero-copy on Unity's ID3D11Device (no own device, no
	// bridge). Selected in dxr_prov_session_start; preserved across the memset there.
	DxrGfxKind          graphics_api;
	// D3D11 sub-mode (#195 editor bridge): 1 = own-device bridge (editor Play Mode),
	// 0 = zero-copy on Unity's device (built player). Selected in session_start from
	// dxr_prov_get_dedicated_window() (the editor selector). Only meaningful on D3D11.
	int                 d3d11_bridge;

	// Weave-target window handle (HWND on Windows; NSView* or NULL on macOS).
	void               *overlay_hwnd;

#ifdef __APPLE__
	// Metal (#202/#204): Unity's MTLDevice and the provider-created session
	// MTLCommandQueue on it (the runtime's in-process compositor encodes on this
	// queue — client-owned, like the D3D11 zero-copy binding). Retained/released
	// by the Metal glue TU; stored as void* so this TU stays plain C++.
	void               *metal_device; // id<MTLDevice>
	void               *metal_queue;  // id<MTLCommandQueue>
#endif

#ifdef _WIN32
	// Unity's D3D12 device — used ONLY to open the shared bridge (NOT to bind the
	// session; see own_device below). NULL on the D3D11 path.
	ID3D12Device       *unity_device;
	// Unity's D3D11 device (zero-copy #195): the session binds DIRECTLY on this and
	// the runtime allocates + weaves swapchain images on it. NULL on the D3D12 path.
	ID3D11Device        *unity_d3d11_device;
	ID3D11DeviceContext *unity_d3d11_context; // Unity's immediate context (flush before weave)

	// The session's OWN D3D12 device (matched to the runtime adapter LUID). The
	// runtime allocates its swapchain on THIS device. (Binding to Unity's device
	// crashed with device-removed — cross-device raw pointers are invalid.)
	ID3D12Device              *own_device;
	ID3D12CommandQueue        *own_queue;
	ID3D12CommandAllocator    *own_cmd_alloc;
	ID3D12GraphicsCommandList *own_cmd_list;
	ID3D12Fence               *own_fence;
	HANDLE                     own_fence_event;
	UINT64                     own_fence_value;

	// Cross-device 2-slice-array bridge: SHARED on own_device, opened on Unity's
	// device. Unity renders both eyes into it; we copy it into the acquired
	// runtime swapchain image (both slices) each frame.
	ID3D12Resource            *bridge_own;     // own_device side (copy source)
	ID3D12Resource            *bridge_unity;   // Unity-device side (Unity renders here)
	HANDLE                     bridge_handle;

	// MultiPass (BiRP) bridges. The IUnityXRDisplay contract is "one texture per
	// render pass", and textureArraySlice is "Only valid if single-pass rendering
	// mode is active" (IUnityXRDisplay.h) — so a 2-pass MultiPass eye CANNOT target
	// a slice of a shared array (that was the failed black+white-blocks attempt).
	// Instead each eye gets its OWN single-slice bridge: Unity renders eye 0/1 into
	// bridge_unity_eye[0/1]; submit copies bridge_own_eye[0/1] into swapchain slices
	// 0/1 (the runtime still gets its 2-slice array swapchain). Unused in SPI mode.
	ID3D12Resource            *bridge_own_eye[2];
	ID3D12Resource            *bridge_unity_eye[2];
	HANDLE                     bridge_handle_eye[2];
#endif // _WIN32

	// Render mode: 1 = Single-Pass-Instanced (1 pass × 2 over the 2-slice array
	// bridge); 0 = MultiPass (2 pass × 1 over the per-eye bridges). Set from C#
	// (dxr_prov_set_single_pass) BEFORE the session starts — URP+Win+D3D12 → SPI,
	// BiRP/other → MultiPass. single_pass_set distinguishes "C# chose" from the
	// zero-initialized default; readers fall back to SPI (current behavior) when
	// unset. Preserved across the session_start memset (like runtime_lib).
	int single_pass;
	int single_pass_set;

	// Transparent background (#166 Phase A): when the app requests it AND the
	// runtime advertises ALPHA_BLEND, the win32 binding sets
	// transparentBackgroundEnabled and xrEndFrame submits with ALPHA_BLEND (the
	// DComp overlay is already WS_EX_NOREDIRECTIONBITMAP). Set from C#
	// (dxr_prov_set_transparent_background) BEFORE the session starts; preserved
	// across the session_start memset like single_pass. alpha_blend_supported is
	// probed per-session in session_start (reset OK).
	int transparent_requested;
	int alpha_blend_supported;

#ifdef _WIN32
	// Unity's command queue + a SHARED cross-device fence so the own-device
	// bridge→swapchain copy waits for Unity's render into the bridge to FINISH.
	// Without it the copy races ahead of Unity's writes and reads empty texels
	// (black). Unity's queue signals shared_fence_unity after its render; the own
	// queue GPU-waits on shared_fence_own (same shared fence) before copying.
	ID3D12CommandQueue        *unity_queue;
	ID3D12Fence               *shared_fence_own;
	ID3D12Fence               *shared_fence_unity;
	HANDLE                     shared_fence_handle;
	UINT64                     sync_val;

	// --- D3D11 own-device bridge (#195 editor Play Mode; d3d11_bridge==1) ----------
	// A SEPARATE ID3D11Device on Unity's adapter (= the dGPU, matching the Leia SR
	// weaver). The session binds on it, so Unity's editor GameView present never
	// shares the weaver's device (the Optimus cross-present deadlock the zero-copy
	// editor path hit). Mirrors the D3D12 own_device + shared bridge + shared fence,
	// but with a legacy D3D11_RESOURCE_MISC_SHARED texture (NT-handle would force a
	// keyed mutex Unity never Acquire/Releases) + a shared ID3D11Fence (D3D11.4) for
	// the cross-device copy ordering (coarse EVENT-query flush fallback).
	ID3D11Device        *own_d3d11_device;
	ID3D11DeviceContext *own_d3d11_context;   // own-device immediate context (does the copy)
	ID3D11Fence         *d3d11_fence_own;      // shared fence, own side (D3D11.4)
	ID3D11Fence         *d3d11_fence_unity;    // shared fence, opened on Unity's device
	HANDLE               d3d11_fence_handle;
	UINT64               d3d11_sync_val;
	int                  d3d11_use_fence;      // 1 = ID3D11Device5 fence path, 0 = coarse flush
	// Primary 2-slice shared bridge (SPI). own creates + Unity opens.
	ID3D11Texture2D     *d3d11_bridge_own;     // own-device side (copy source)
	ID3D11Texture2D     *d3d11_bridge_unity;   // Unity-device side (C# renders both eyes here)
	HANDLE               d3d11_bridge_handle;
	// MultiPass (BiRP) per-eye targets (#195): one single-slice texture per eye (one
	// texture per render pass; textureArraySlice is SPI-only per the IUnityXRDisplay
	// contract — mirrors the D3D12 bridge_*_eye[2] arrays). d3d11_bridge_unity_eye[e]
	// is the render target Unity draws into in BOTH sub-modes (editor bridge = the
	// Unity-opened shared side; zero-copy player = a plain Unity-device texture);
	// d3d11_bridge_own_eye[e] is the editor bridge's own-device copy source (NULL in
	// zero-copy). submit copies each eye into swapchain array slice 0/1.
	ID3D11Texture2D     *d3d11_bridge_own_eye[2];
	ID3D11Texture2D     *d3d11_bridge_unity_eye[2];
	HANDLE               d3d11_bridge_handle_eye[2]; // closed at alloc; stays NULL
#endif // _WIN32

	DxrProvDisplayInfo display_info;

	// Rendering modes
	XrDisplayRenderingModeInfoDXR modes[PS_MAX_RENDERING_MODES];
	uint32_t mode_count;

	// SPI swapchain. arraySize is ALWAYS 2 (Unity's stereo topology is fixed at
	// subsystem start — rendering into fewer slices would overlap the two eyes into
	// one → ghosting). Hardware 2D is handled by SUBMITTING only sc_view_count views
	// (the first slice), not by shrinking the array (#172 P4). sc_view_count = the
	// active mode's view count (1 = 2D single tile, 2 = 3D stereo).
	XrSwapchain swapchain;
	uint32_t    sc_width, sc_height, sc_array, sc_image_count;
	uint32_t    sc_view_count;   // views to SUBMIT (1=2D, 2=3D); arraySize stays 2
	int64_t     sc_format;
#ifdef _WIN32
	XrSwapchainImageD3D12KHR sc_images[PS_MAX_SWAPCHAIN_IMAGES];
	// D3D11 zero-copy (#195): the runtime creates these swapchain images (arraySize=2)
	// on Unity's device; the provider wraps them DIRECTLY via CreateTexture (no bridge).
	// Parallel to sc_images; only one array is populated per session (by graphics_api).
	XrSwapchainImageD3D11KHR sc_images_d3d11[PS_MAX_SWAPCHAIN_IMAGES];
#endif
#ifdef __APPLE__
	// Metal (#202/#204): swapchain images on Unity's MTLDevice (the runtime's
	// in-process compositor is client-device, like the D3D11 zero-copy path).
	// Parallel to sc_images/sc_images_d3d11 — only one array populated per session.
	XrSwapchainImageMetalKHR sc_images_metal[PS_MAX_SWAPCHAIN_IMAGES];
	// ZERO-COPY MultiPass (#204): per-image per-eye single-slice VIEWS of the
	// arraySize=2 swapchain images (id<MTLTexture>, retained by the Metal glue).
	// Unity renders each eye straight into its slice — NO provider blit: a
	// provider blit CB touching session textures crashes the Unity editor's
	// Metal GfxDeviceWorker (CreateMainTextureForRS/DestroyRenderSurfaceDesc
	// NULL deref — lldb-verified, bring-up runs 7-18). Encoder-less signal/wait
	// CBs are safe (run 19) and provide the render→weave order.
	void *eye_view_metal[PS_MAX_SWAPCHAIN_IMAGES][2];
#endif
	int         swapchain_created;

	// Live tile realloc (#172): the primary swapchain+bridge are recreated to track
	// the live per-view target (window(client)×scaleXY, or the 3D-zone recommended
	// view size) when it changes — resize / split-drag / mode-switch. Debounced so an
	// interactive resize drag reallocs once on settle, not every frame.
	uint32_t    realloc_pending_w, realloc_pending_h; // last-seen target awaiting stability
	int         realloc_stable;                        // consecutive frames the target held

	// Window-space UI (HUD) overlay layer (#67/#166): own overlay swapchain +
	// cross-device bridge. C# (DisplayXRWindowSpaceUI) Graphics.CopyTexture's the
	// canvas RT into wsui_bridge_unity each frame; submit copies wsui_bridge_own ->
	// the acquired swapchain image (own device) and submits a 2nd composition layer.
	XrSwapchain wsui_swapchain;
	uint32_t    wsui_w, wsui_h, wsui_image_count;
	int64_t     wsui_format;
	int         wsui_swapchain_created;
	int         wsui_image_acquired;
	uint32_t    wsui_acquired_index;
	uint32_t        wsui_registered_w, wsui_registered_h; // bridge+swapchain sized for this
#ifdef _WIN32
	XrSwapchainImageD3D12KHR wsui_images[PS_MAX_SWAPCHAIN_IMAGES];
	ID3D12Resource *wsui_bridge_own;    // own_device side (copy source)
	ID3D12Resource *wsui_bridge_unity;  // Unity-device side (C# CopyTexture target)
	HANDLE          wsui_bridge_handle;
	// D3D11 (#195): images on the runtime's device. `wsui_unity_tex` is the Unity-side
	// texture C# CopyTexture targets. PLAYER zero-copy: a plain Unity-device texture,
	// submit same-device-copies it into the acquired image. EDITOR bridge: the
	// Unity-opened side of a shared texture whose own-device side is `wsui_unity_tex_own`;
	// submit copies own->image on the own context.
	XrSwapchainImageD3D11KHR wsui_images_d3d11[PS_MAX_SWAPCHAIN_IMAGES];
	ID3D11Texture2D *wsui_unity_tex;
	ID3D11Texture2D *wsui_unity_tex_own;   // own-device side (editor bridge)
	HANDLE           wsui_unity_tex_handle;
#else
	// Metal (#206): arraySize=1 overlay swapchain images (id<MTLTexture> in .texture).
	// No cross-device bridge — the runtime compositor is on Unity's OWN MTLDevice, so
	// submit blits the C#-registered Unity id<MTLTexture> straight into the acquired
	// image (same device, session queue). Cousin of the projection sc_images_metal.
	XrSwapchainImageMetalKHR wsui_images_metal[PS_MAX_SWAPCHAIN_IMAGES];
#endif // _WIN32

	// Local2D layer (#166 Phase B, XR_DXR_local_3d_zone) — post-weave 2D content at a
	// client-window PIXEL rect (the 2D band). Same cross-device-bridge shape as wsui,
	// but submitted as XrCompositionLayerLocal2DDXR with a pixel rect (not fractional).
	XrSwapchain l2d_swapchain;
	uint32_t    l2d_w, l2d_h, l2d_image_count;
	int64_t     l2d_format;
	int         l2d_swapchain_created;
	uint32_t        l2d_registered_w, l2d_registered_h;
#ifdef _WIN32
	XrSwapchainImageD3D12KHR l2d_images[PS_MAX_SWAPCHAIN_IMAGES];
	ID3D12Resource *l2d_bridge_own;
	ID3D12Resource *l2d_bridge_unity;
	HANDLE          l2d_bridge_handle;
	// D3D11 (#195): same shape as the wsui D3D11 fields above (zero-copy plain tex, or
	// editor-bridge shared tex with an own-device side + handle).
	XrSwapchainImageD3D11KHR l2d_images_d3d11[PS_MAX_SWAPCHAIN_IMAGES];
	ID3D11Texture2D *l2d_unity_tex;
	ID3D11Texture2D *l2d_unity_tex_own;   // own-device side (editor bridge)
	HANDLE           l2d_unity_tex_handle;
#else
	// Metal (#206): arraySize=1 overlay swapchain images (id<MTLTexture> in .texture).
	// No cross-device bridge — the compositor is on Unity's OWN MTLDevice, so submit
	// blits the C#-registered Unity id<MTLTexture> straight into the acquired image
	// (same device, session queue). Cousin of wsui_images_metal.
	XrSwapchainImageMetalKHR l2d_images_metal[PS_MAX_SWAPCHAIN_IMAGES];
#endif // _WIN32
	int32_t         l2d_rect_x, l2d_rect_y, l2d_rect_w, l2d_rect_h; // dest, client px
	int             l2d_rect_set;

	// Per-frame located views (render-ready)
	DxrProvView views[DXR_PROV_MAX_VIEWS];
	uint32_t    view_count;

	// Tunables / pose pushed from the rig
	float ipd_factor, parallax_factor, perspective_factor, virtual_display_height;
	float inv_convergence_distance, fov_override, near_z, far_z;
	int   camera_centric;
	int   tunables_set;
	XrPosef display_pose;
	int     display_pose_set;

	// Active mode + per-frame render rect (Bucket B: window×scaleXY adaptive)
	uint32_t active_mode_index;
	uint32_t render_w, render_h;

	// Event latches (Bucket C: atomic read-and-clear, pumped by poll_events)
	int      ev_mode_changed;  uint32_t ev_mode_prev, ev_mode_cur;
	int      ev_hw_changed;     int ev_hw3d;
	int      ev_track_changed;  int ev_track_is_tracking, ev_track_mode;

	// Resolved function pointers
	PFN_xrGetSystem                   pfn_get_system;
	PFN_xrGetSystemProperties         pfn_get_system_properties;
	PFN_xrCreateSession               pfn_create_session;
	PFN_xrDestroySession              pfn_destroy_session;
	PFN_xrCreateReferenceSpace        pfn_create_reference_space;
	PFN_xrEnumerateSwapchainFormats   pfn_enumerate_swapchain_formats;
	PFN_xrCreateSwapchain             pfn_create_swapchain;
	PFN_xrDestroySwapchain            pfn_destroy_swapchain;
	PFN_xrEnumerateSwapchainImages    pfn_enumerate_swapchain_images;
	PFN_xrAcquireSwapchainImage       pfn_acquire_swapchain_image;
	PFN_xrWaitSwapchainImage          pfn_wait_swapchain_image;
	PFN_xrReleaseSwapchainImage       pfn_release_swapchain_image;
	PFN_xrWaitFrame                   pfn_wait_frame;
	PFN_xrBeginFrame                  pfn_begin_frame;
	PFN_xrEndFrame                    pfn_end_frame;
	PFN_xrLocateViews                 pfn_locate_views;
	PFN_xrPollEvent                   pfn_poll_event;
	PFN_xrBeginSession                pfn_begin_session;
	PFN_xrEndSession                  pfn_end_session;
	PFN_xrDestroyInstance             pfn_destroy_instance;
	PFN_xrEnumerateEnvironmentBlendModes    pfn_enumerate_blend_modes;    // transparency
	PFN_xrEnumerateDisplayRenderingModesDXR pfn_enumerate_modes;          // optional
	PFN_xrRequestDisplayRenderingModeDXR    pfn_request_rendering_mode;   // optional
	PFN_xrRequestDisplayModeDXR             pfn_request_display_mode;     // optional
	PFN_xrRequestEyeTrackingModeDXR         pfn_request_eye_tracking_mode;// optional
	PFN_xrCaptureAtlasDXR                    pfn_capture_atlas;            // #140, optional
} ProviderSession;

static ProviderSession s_ps;
static DxrProvLogCallback s_log_cb = NULL;

void dxr_prov_set_log_callback(DxrProvLogCallback cb) { s_log_cb = cb; }

// Append a line to %TEMP%\displayxr_prov_native.log ($TMPDIR on macOS; native
// stderr also lands in Editor.log there). Unity's Player.log does not capture a
// native plugin's stderr, so route provider diagnostics to a file we can read
// after a run (M1 bring-up aid). Shared by the provider TU via dxr_prov_file_log.
extern "C" void dxr_prov_file_log(const char *s)
{
#ifdef _WIN32
	char path[MAX_PATH];
	DWORD n = GetTempPathA(MAX_PATH, path);
	if (n == 0 || n > MAX_PATH - 32) return;
	strncat(path, "displayxr_prov_native.log", MAX_PATH - strlen(path) - 1);
#else
	char path[1024];
	const char *tmp = getenv("TMPDIR");
	if (!tmp || !tmp[0]) tmp = "/tmp/";
	snprintf(path, sizeof(path), "%s%sdisplayxr_prov_native.log", tmp,
	         tmp[strlen(tmp) - 1] == '/' ? "" : "/");
#endif
	FILE *f = fopen(path, "a");
	if (!f) return;
	fputs(s, f);
	fclose(f);
}

static void ps_log(const char *fmt, ...)
{
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	fputs(buf, stderr);
#ifdef _WIN32
	OutputDebugStringA(buf);
#endif
	dxr_prov_file_log(buf);
	if (s_log_cb) s_log_cb(buf);
}

// ============================================================================
// Runtime load helpers (mirrors displayxr_standalone.cpp)
// ============================================================================

static char *ps_parse_library_path(const char *json_path)
{
	FILE *f = fopen(json_path, "r");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	long len = ftell(f);
	fseek(f, 0, SEEK_SET);
	char *buf = (char *)malloc(len + 1);
	if (!buf) { fclose(f); return NULL; }
	fread(buf, 1, len, f);
	buf[len] = '\0';
	fclose(f);

	const char *key = "\"library_path\"";
	char *pos = strstr(buf, key);
	if (!pos) { free(buf); return NULL; }
	pos += strlen(key);
	while (*pos == ' ' || *pos == '\t' || *pos == '\n' || *pos == '\r' || *pos == ':') pos++;
	if (*pos != '"') { free(buf); return NULL; }
	pos++;
	char *end = strchr(pos, '"');
	if (!end) { free(buf); return NULL; }
	size_t path_len = end - pos;
	char *result = (char *)malloc(path_len + 1);
	memcpy(result, pos, path_len);
	result[path_len] = '\0';
	free(buf);
	return result;
}

static char *ps_resolve_library_path(const char *json_path, const char *lib_path)
{
	if (lib_path[0] == '/' || (lib_path[0] && lib_path[1] == ':'))
		return _strdup(lib_path);
	const char *last_sep = strrchr(json_path, '/');
	const char *last_bsep = strrchr(json_path, '\\');
	if (last_bsep && (!last_sep || last_bsep > last_sep)) last_sep = last_bsep;
	size_t dir_len = last_sep ? (size_t)(last_sep - json_path + 1) : 0;
	size_t lib_len = strlen(lib_path);
	char *result = (char *)malloc(dir_len + lib_len + 1);
	if (dir_len > 0) memcpy(result, json_path, dir_len);
	memcpy(result + dir_len, lib_path, lib_len);
	result[dir_len + lib_len] = '\0';
	return result;
}

#ifdef _WIN32
// Read the OpenXR ActiveRuntime manifest path from a registry root (HKCU/HKLM).
// Returns a heap copy or NULL. Mirrors the OpenXR loader's registry convention.
static char *ps_read_active_runtime(HKEY root)
{
	char path[1024];
	DWORD sz = sizeof(path);
	LSTATUS rc = RegGetValueA(root, "SOFTWARE\\Khronos\\OpenXR\\1",
	                          "ActiveRuntime", RRF_RT_REG_SZ, NULL, path, &sz);
	if (rc == ERROR_SUCCESS && path[0]) return _strdup(path);
	return NULL;
}
#endif

// Resolve the active runtime manifest path: explicit arg → XR_RUNTIME_JSON →
// installed-runtime fallback (#173): the Windows registry ActiveRuntime, or on
// macOS the fixed /usr/local/share/openxr/1/active_runtime.json (the only path
// the macOS OpenXR loader convention checks). The fallback lets the provider
// find the INSTALLED runtime with no env var; a newer dev runtime still needs
// XR_RUNTIME_JSON (checked first). The loader passes NULL for the explicit
// path, so GfxStart → session_start relies on this resolver.
static char *ps_resolve_runtime_json(const char *explicit_path)
{
	if (explicit_path && explicit_path[0]) return _strdup(explicit_path);
	const char *env = getenv("XR_RUNTIME_JSON");
	if (env && env[0]) return _strdup(env);
#ifdef _WIN32
	// HKCU (per-user override) takes precedence over HKLM (machine install), same
	// order as the OpenXR loader.
	char *reg = ps_read_active_runtime(HKEY_CURRENT_USER);
	if (!reg) reg = ps_read_active_runtime(HKEY_LOCAL_MACHINE);
	if (reg) {
		ps_log("[DisplayXR-PROV] runtime JSON from registry ActiveRuntime: %s\n", reg);
		return reg;
	}
#else
	const char *fixed = "/usr/local/share/openxr/1/active_runtime.json";
	FILE *f = fopen(fixed, "r");
	if (f) {
		fclose(f);
		ps_log("[DisplayXR-PROV] runtime JSON from installed active_runtime: %s\n", fixed);
		return _strdup(fixed);
	}
#endif
	return NULL;
}

#define PS_RESOLVE(name, field, type) do { \
	PFN_xrVoidFunction _fn = NULL; \
	s_ps.gipa(s_ps.instance, name, &_fn); \
	s_ps.field = (type)_fn; \
	if (!s_ps.field) { ps_log("[DisplayXR-PROV] Failed to resolve %s\n", name); return 0; } \
} while (0)

static int ps_resolve_functions(void)
{
	PS_RESOLVE("xrGetSystem", pfn_get_system, PFN_xrGetSystem);
	PS_RESOLVE("xrGetSystemProperties", pfn_get_system_properties, PFN_xrGetSystemProperties);
	PS_RESOLVE("xrCreateSession", pfn_create_session, PFN_xrCreateSession);
	PS_RESOLVE("xrDestroySession", pfn_destroy_session, PFN_xrDestroySession);
	PS_RESOLVE("xrCreateReferenceSpace", pfn_create_reference_space, PFN_xrCreateReferenceSpace);
	PS_RESOLVE("xrEnumerateSwapchainFormats", pfn_enumerate_swapchain_formats, PFN_xrEnumerateSwapchainFormats);
	PS_RESOLVE("xrCreateSwapchain", pfn_create_swapchain, PFN_xrCreateSwapchain);
	PS_RESOLVE("xrDestroySwapchain", pfn_destroy_swapchain, PFN_xrDestroySwapchain);
	PS_RESOLVE("xrEnumerateSwapchainImages", pfn_enumerate_swapchain_images, PFN_xrEnumerateSwapchainImages);
	PS_RESOLVE("xrAcquireSwapchainImage", pfn_acquire_swapchain_image, PFN_xrAcquireSwapchainImage);
	PS_RESOLVE("xrWaitSwapchainImage", pfn_wait_swapchain_image, PFN_xrWaitSwapchainImage);
	PS_RESOLVE("xrReleaseSwapchainImage", pfn_release_swapchain_image, PFN_xrReleaseSwapchainImage);
	PS_RESOLVE("xrWaitFrame", pfn_wait_frame, PFN_xrWaitFrame);
	PS_RESOLVE("xrBeginFrame", pfn_begin_frame, PFN_xrBeginFrame);
	PS_RESOLVE("xrEndFrame", pfn_end_frame, PFN_xrEndFrame);
	PS_RESOLVE("xrLocateViews", pfn_locate_views, PFN_xrLocateViews);
	PS_RESOLVE("xrPollEvent", pfn_poll_event, PFN_xrPollEvent);
	PS_RESOLVE("xrBeginSession", pfn_begin_session, PFN_xrBeginSession);
	PS_RESOLVE("xrEndSession", pfn_end_session, PFN_xrEndSession);
	PS_RESOLVE("xrDestroyInstance", pfn_destroy_instance, PFN_xrDestroyInstance);
	PS_RESOLVE("xrEnumerateEnvironmentBlendModes", pfn_enumerate_blend_modes, PFN_xrEnumerateEnvironmentBlendModes);
	// Optional EXT (XR_DXR_display_info mode/eye-tracking control) — soft-resolve;
	// OK if absent (older runtime → the C# mode UI/events simply stay inert).
	{
		PFN_xrVoidFunction _fn = NULL;
		s_ps.gipa(s_ps.instance, "xrEnumerateDisplayRenderingModesDXR", &_fn);
		s_ps.pfn_enumerate_modes = (PFN_xrEnumerateDisplayRenderingModesDXR)_fn;
		_fn = NULL;
		s_ps.gipa(s_ps.instance, "xrRequestDisplayRenderingModeDXR", &_fn);
		s_ps.pfn_request_rendering_mode = (PFN_xrRequestDisplayRenderingModeDXR)_fn;
		_fn = NULL;
		s_ps.gipa(s_ps.instance, "xrRequestDisplayModeDXR", &_fn);
		s_ps.pfn_request_display_mode = (PFN_xrRequestDisplayModeDXR)_fn;
		_fn = NULL;
		s_ps.gipa(s_ps.instance, "xrRequestEyeTrackingModeDXR", &_fn);
		s_ps.pfn_request_eye_tracking_mode = (PFN_xrRequestEyeTrackingModeDXR)_fn;
		// Zones (#166 Phase B) — soft-resolve; inert on older runtimes.
		_fn = NULL;
		s_ps.gipa(s_ps.instance, "xrGetDisplayZoneCapabilitiesDXR", &_fn);
		s_ps.pfn_get_zone_caps = (PFN_xrGetDisplayZoneCapabilitiesDXR)_fn;
		_fn = NULL;
		s_ps.gipa(s_ps.instance, "xrGetDisplayZoneRecommendedViewSizeDXR", &_fn);
		s_ps.pfn_get_zone_view_size = (PFN_xrGetDisplayZoneRecommendedViewSizeDXR)_fn;
		// Live workspace-tile canvas size (#225, display_zones spec v2) — soft-
		// resolve; NULL on an older runtime → the window/GetClientRect path is used.
		_fn = NULL;
		s_ps.gipa(s_ps.instance, "xrGetWorkspaceTileSizeDXR", &_fn);
		s_ps.pfn_get_workspace_tile_size = (PFN_xrGetWorkspaceTileSizeDXR)_fn;
		// Atlas capture (#140) — soft-resolve; inert if the runtime lacks it.
		_fn = NULL;
		s_ps.gipa(s_ps.instance, "xrCaptureAtlasDXR", &_fn);
		s_ps.pfn_capture_atlas = (PFN_xrCaptureAtlasDXR)_fn;
	}
	return 1;
}

// ============================================================================
// Rendering-mode enumeration (lifts standalone enumerate_and_store_modes)
// ============================================================================

static void ps_enumerate_modes(void)
{
	s_ps.mode_count = 0;
	if (!s_ps.pfn_enumerate_modes) return;
	uint32_t total = 0;
	if (XR_FAILED(s_ps.pfn_enumerate_modes(s_ps.session, 0, &total, NULL)) || total == 0)
		return;
	if (total > PS_MAX_RENDERING_MODES) total = PS_MAX_RENDERING_MODES;
	for (uint32_t i = 0; i < total; i++)
		s_ps.modes[i].type = XR_TYPE_DISPLAY_RENDERING_MODE_INFO_DXR;
	if (XR_SUCCEEDED(s_ps.pfn_enumerate_modes(s_ps.session, total, &total, s_ps.modes)))
		s_ps.mode_count = total;
	for (uint32_t i = 0; i < s_ps.mode_count; i++) {
		if (s_ps.modes[i].isActive) s_ps.active_mode_index = s_ps.modes[i].modeIndex;
		ps_log("[DisplayXR-PROV] mode[%u] idx=%u '%s' views=%u tiles=%ux%u %ux%u "
		       "scale=%.3fx%.3f hw3d=%d active=%d req=%d\n",
		       i, s_ps.modes[i].modeIndex, s_ps.modes[i].modeName,
		       s_ps.modes[i].viewCount, s_ps.modes[i].tileColumns, s_ps.modes[i].tileRows,
		       s_ps.modes[i].viewWidthPixels, s_ps.modes[i].viewHeightPixels,
		       (double)s_ps.modes[i].viewScaleX, (double)s_ps.modes[i].viewScaleY,
		       (int)s_ps.modes[i].hardwareDisplay3D,
		       (int)s_ps.modes[i].isActive, (int)s_ps.modes[i].isRequestable);
	}

	// Render-path selection by view count (ADR-007). Unity's IUnityXRDisplay caps
	// at kUnityXRMaxNumRenderPasses(4) * kUnityXRMaxNumUnityXRRenderParams(2) = 8
	// views/frame, so the provider path covers eye-tracked stereo + quad. A display
	// advertising more (many-view light field, e.g. Looking Glass 45+) needs the
	// app-side N-camera -> atlas "quilt" path, which is NOT yet implemented — the
	// dormant displayxr_standalone render-to-atlas core is its seed. Until then we
	// warn once and render up to 8 views via the provider. This is the branch point
	// where the future quilt renderer slots in (keyed off max advertised view count).
	{
		const uint32_t PS_MAX_PROVIDER_VIEWS = 8; // Unity XR display cap (ADR-007)
		uint32_t max_views = 0;
		for (uint32_t i = 0; i < s_ps.mode_count; i++)
			if (s_ps.modes[i].viewCount > max_views) max_views = s_ps.modes[i].viewCount;
		static int s_warned_over_cap = 0;
		if (max_views > PS_MAX_PROVIDER_VIEWS && !s_warned_over_cap) {
			s_warned_over_cap = 1;
			ps_log("[DisplayXR-PROV] WARN: display advertises %u views; the >%u-view "
			       "quilt render path is not yet implemented (ADR-007) — provider "
			       "renders up to %u.\n",
			       max_views, PS_MAX_PROVIDER_VIEWS, PS_MAX_PROVIDER_VIEWS);
		}
	}
}

// Find an enumerated mode by modeIndex (NULL if absent).
static const XrDisplayRenderingModeInfoDXR *ps_find_mode(uint32_t mode_index)
{
	for (uint32_t i = 0; i < s_ps.mode_count; i++)
		if (s_ps.modes[i].modeIndex == mode_index) return &s_ps.modes[i];
	return NULL;
}

// The ACTIVE mode's view count: 1 for hardware 2D (one full-res tile submitted),
// 2 for stereo 3D. Caps at 2 (#166 scope). Defaults to 2 before a mode resolves so
// startup comes up in 3D. (#172 P4)
static uint32_t ps_active_view_count(void)
{
	const XrDisplayRenderingModeInfoDXR *m = ps_find_mode(s_ps.active_mode_index);
	if (m && m->viewCount >= 1) return m->viewCount >= 2 ? 2 : 1;
	return 2;
}

// The ACTIVE mode's per-view scaleXY, used to size the per-frame render rect =
// window × scaleXY. Honors the active mode directly: hardware 2D → 1.0×1.0 (one
// full-res tile), stereo 3D → e.g. 0.5×0.5. Falls back to the first stereo mode's
// scale, then display-info, then 0.5×1.0, only when NO active mode has resolved
// (startup). Previously this forced the 3D scale even in 2D, so 2D rendered at
// half-res instead of full-res (#172 P4).
static void ps_active_view_scale(float *sx, float *sy)
{
	const XrDisplayRenderingModeInfoDXR *m = ps_find_mode(s_ps.active_mode_index);
	if (!m) {
		for (uint32_t i = 0; i < s_ps.mode_count; i++)
			if (s_ps.modes[i].hardwareDisplay3D && s_ps.modes[i].viewCount == 2)
				{ m = &s_ps.modes[i]; break; }
	}
	float x = (m && m->viewScaleX > 0.0f) ? m->viewScaleX
	          : (s_ps.display_info.is_valid && s_ps.display_info.scale_x > 0.0f
	             ? s_ps.display_info.scale_x : 0.5f);
	float y = (m && m->viewScaleY > 0.0f) ? m->viewScaleY
	          : (s_ps.display_info.is_valid && s_ps.display_info.scale_y > 0.0f
	             ? s_ps.display_info.scale_y : 1.0f);
	*sx = x; *sy = y;
}

// Per-platform: displayxr_win32.c / displayxr_macos.mm / displayxr_linux.c.
// Every platform MUST define it — the shared code below calls it unconditionally,
// and a missing definition is an unresolved symbol the loader only reports when
// it binds (see the --no-undefined note in CMakeLists.txt, #249).
extern "C" int displayxr_is_shell_mode(void);

#if defined(__linux__) && !defined(__ANDROID__)
// Linux handle-app glue (#249), defined in displayxr_linux.c.
extern "C" int displayxr_linux_get_weave_window(void **out_display, unsigned long *out_window);
extern "C" void displayxr_linux_destroy_weave_window(void);
extern "C" int displayxr_linux_window_size(uint32_t *out_w, uint32_t *out_h);
#endif

// Poll the live workspace-tile canvas size (#225, display_zones spec v2). The
// compositor composites this client into a shell-driven tile that follows 3D-
// window resize; the runtime reports its current px here so a tile app tracks
// the resize (a minimized tile's OS backbuffer can't). 0x0 when not a tile /
// not yet bound / older runtime → callers keep the window path.
static void ps_query_tile_size(void)
{
	if (!s_ps.pfn_get_workspace_tile_size || s_ps.session == XR_NULL_HANDLE)
		return;
	XrExtent2Di ts = {0, 0};
	if (XR_SUCCEEDED(s_ps.pfn_get_workspace_tile_size(s_ps.session, &ts)) &&
	    ts.width > 0 && ts.height > 0) {
		if (s_ps.tile_px_w != (uint32_t)ts.width || s_ps.tile_px_h != (uint32_t)ts.height)
			ps_log("[DisplayXR-PROV] workspace tile size: %dx%d (was %ux%u)\n",
			       ts.width, ts.height, s_ps.tile_px_w, s_ps.tile_px_h);
		s_ps.tile_px_w = (uint32_t)ts.width;
		s_ps.tile_px_h = (uint32_t)ts.height;
	}
}

// Bridge for the C win32 layer's displayxr_get_overlay_size (#225): the last
// polled workspace-tile canvas px, or 0 when not a tile / not yet known.
extern "C" int dxr_prov_workspace_tile_size(uint32_t *w, uint32_t *h)
{
	if (s_ps.tile_px_w > 0 && s_ps.tile_px_h > 0) {
		if (w) *w = s_ps.tile_px_w;
		if (h) *h = s_ps.tile_px_h;
		return 1;
	}
	return 0;
}

// The size the app + provider render/author for. In workspace-tile mode (#225)
// this is the shell-driven tile canvas (so the render follows 3D-window resize);
// otherwise the bound overlay's live client size (= Unity's window client area).
// Falls back to the display pixel dims when no overlay is bound yet.
static void ps_window_size(uint32_t *w, uint32_t *h)
{
	uint32_t ww = 0, hh = 0;
#ifdef _WIN32
	// Workspace tile: author/render for the live tile canvas, not our own window.
	if (displayxr_is_shell_mode()) {
		if (s_ps.tile_px_w == 0 || s_ps.tile_px_h == 0)
			ps_query_tile_size(); // lazy first fetch until the slot binds
		if (s_ps.tile_px_w > 0 && s_ps.tile_px_h > 0) {
			*w = s_ps.tile_px_w; *h = s_ps.tile_px_h;
			return;
		}
	}
	if (s_ps.overlay_hwnd) {
		RECT rc;
		if (GetClientRect((HWND)s_ps.overlay_hwnd, &rc)) {
			ww = (uint32_t)(rc.right - rc.left);
			hh = (uint32_t)(rc.bottom - rc.top);
		}
	}
#elif defined(__APPLE__)
	// macOS (#204): the bound weave NSView's live backing size — the same
	// per-frame source the runtime's compositor derives its canvas from, so
	// the per-view sizes agree and live resize lands via the #172 reconcile.
	if (s_ps.overlay_hwnd)
		displayxr_metal_view_backing_size(s_ps.overlay_hwnd, &ww, &hh);
#else
	// Linux (#249): live geometry of the bound X11 window, same role as the
	// macOS backing size above. Returns 0 when no window is bound (runtime
	// self-hosting), which falls through to the display-info default below.
	displayxr_linux_window_size(&ww, &hh);
#endif
	if (ww == 0 || hh == 0) {
		ww = s_ps.display_info.is_valid ? s_ps.display_info.pixel_width : 1920;
		hh = s_ps.display_info.is_valid ? s_ps.display_info.pixel_height : 1080;
	}
	*w = ww; *h = hh;
}

// ============================================================================
// Zones (XR_DXR_display_zones) — #166 Phase B
// ============================================================================

// Lazy caps query: zones are usable iff the runtime advertised the extension,
// resolved the caps entry point, and reports supported && maxZones3D>=1. Mirrors
// the hook path's dxr_zones_ready.
static int ps_zones_ready(void)
{
	if (!s_ps.has_display_zones || !s_ps.has_view_rig || !s_ps.pfn_get_zone_caps)
		return 0;
	if (s_ps.zone_caps_ok < 0) {
		if (s_ps.session == XR_NULL_HANDLE) return 0;
		XrDisplayZoneCapabilitiesDXR caps = {XR_TYPE_DISPLAY_ZONE_CAPABILITIES_DXR};
		XrResult cr = s_ps.pfn_get_zone_caps(s_ps.session, &caps);
		s_ps.zone_caps_ok = (XR_SUCCEEDED(cr) && caps.supported && caps.maxZones3D >= 1) ? 1 : 0;
		s_ps.zone_max_3d = caps.maxZones3D;
		ps_log("[DisplayXR-PROV] display-zones caps: result=0x%x supported=%d maxZones3D=%u -> %s\n",
		       (unsigned)cr, (int)caps.supported, caps.maxZones3D,
		       s_ps.zone_caps_ok ? "ACTIVE" : "unsupported (full-window path)");
	}
	return s_ps.zone_caps_ok > 0;
}

// Recommended per-view render size for the current zone rect (== zone extent per
// the runtime). Falls back to the rect extent if the entry point is absent.
static void ps_query_zone_rec_size(void)
{
	s_ps.zone_rec_w = s_ps.zone_w > 0 ? (uint32_t)s_ps.zone_w : 0;
	s_ps.zone_rec_h = s_ps.zone_h > 0 ? (uint32_t)s_ps.zone_h : 0;
	if (s_ps.pfn_get_zone_view_size && s_ps.zone_valid && s_ps.session != XR_NULL_HANDLE) {
		XrRect2Di rect = {{s_ps.zone_x, s_ps.zone_y}, {s_ps.zone_w, s_ps.zone_h}};
		XrExtent2Di rec = {0, 0};
		if (XR_SUCCEEDED(s_ps.pfn_get_zone_view_size(s_ps.session, &rect, &rec)) &&
		    rec.width > 0 && rec.height > 0) {
			s_ps.zone_rec_w = (uint32_t)rec.width;
			s_ps.zone_rec_h = (uint32_t)rec.height;
		}
	}
}

// Whether the zone should drive this frame (rect set + caps OK). Zones apply in BOTH
// 2D and 3D: the zone confines WHERE the content is placed (its band), independent of
// view count. In 2D the zone tile is still rendered zone-sized and zone-scoped — just
// as a single submitted view (see sc_view_count) instead of a stereo pair. (#172 P4:
// an earlier `&& viewCount>=2` here made 2D fall back to full-window, so a zoned app's
// content escaped its band in 2D — the regression this reverts.)
static int ps_zone_active(void) { return s_ps.zone_valid && ps_zones_ready(); }

// ============================================================================
// SPI swapchain (arraySize=2). Images live on Unity's D3D12 device (zero-copy).
// ============================================================================

#ifdef _WIN32
static int ps_create_bridge(void); // defined after this function
static int ps_create_bridge_d3d11(void); // D3D11 editor bridge (#195); defined after this function
#endif
#if defined(ENABLE_VULKAN)
// Vulkan external-memory bridge (#247 Windows, #249 Linux); defined after this function.
static int ps_create_bridge_vk(void);
#endif
static void ps_publish_stereo_matrices(void); // defined after dxr_prov_begin_frame (overlay hit-test)

// --- Color-space-aware swapchain format (present-path sRGB gamma fix) ---------
// A Linear-color-space Unity project renders LINEAR values; correct display needs a final
// linear→sRGB encode. In the DOCKED texture/mirror path Unity's GameView present applies that
// encode, so the eye/woven textures stay UNORM and look right. But on the PRESENT path
// (undocked editor window + built player) the runtime presents its woven output straight to the
// window with NO such encode → the image is too dark. Fix: for a Linear project ON THE PRESENT
// PATH, request an sRGB swapchain so Unity encodes linear→sRGB on store and the runtime presents
// correctly-encoded pixels (the standard OpenXR sRGB contract). Gamma projects and the docked
// path keep UNORM. Safe fallback: if the runtime advertises no sRGB format, s_swapchain_srgb
// stays 0 and everything is byte-identical to before.
static int s_color_space_linear = 0; // Unity project color space (C# pushes pre-session)
static int s_swapchain_srgb     = 0; // primary swapchain created with an sRGB format
static int s_sc_present_path    = 1; // 1 = runtime presents (no shared texture bound); set in
                                     // session_start before ps_create_swapchain (s_probe_handle
                                     // is declared later in the file, so route it through this).
void dxr_prov_set_color_space_linear(int linear)
{
	s_color_space_linear = linear ? 1 : 0;
	ps_log("[DisplayXR-PROV] color space: %s\n", linear ? "Linear" : "Gamma");
}
int dxr_prov_get_color_space_linear(void) { return s_color_space_linear; }
int dxr_prov_swapchain_is_srgb(void) { return s_swapchain_srgb; }
#ifdef _WIN32
// Map the chosen XR swapchain format id to the DXGI format for the paired bridge/staging
// resources (Unity renders into the bridge; it must match the swapchain). Pure refactor for
// the UNORM cases (28/87); adds the sRGB variants (29/91) for the present-path Linear fix.
static DXGI_FORMAT ps_sc_dxgi_format(int64_t f)
{
	switch (f) {
	case 87: return DXGI_FORMAT_B8G8R8A8_UNORM;
	case 29: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
	case 91: return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
	default: return DXGI_FORMAT_R8G8B8A8_UNORM; // 28
	}
}
#endif

static int ps_create_swapchain(void)
{
	if (s_ps.swapchain_created) return 1;
	if (!s_ps.display_info.is_valid) return 0;
	// Zone frames size the swapchain from the zone caps + recommended-view-size
	// queries, which need a READY (begun) session. Defer the early (pre-ready)
	// attempt so we don't create it window-sized before caps resolve; the
	// session-ready path recreates it correctly.
	if (s_ps.zone_valid && s_ps.has_display_zones && !s_ps.session_ready) return 0;

	// Per-view (per-eye) render resolution = window(overlay client) × active-mode
	// scaleXY. Each SPI array slice IS one eye, sized to exactly this — so the
	// render rect == the swapchain and Unity renders the FULL slice (viewportRect
	// {0,0,1,1}, imageRect = full). This avoids a sub-rect within a larger
	// worst-case texture, whose viewport (Unity bottom-left) vs imageRect (D3D
	// top-left) Y origins don't align → the runtime would sample empty texels
	// (black). The ADR-010 "worst-case, never realloc" optimization needs that
	// Y-origin reconciled and a live bridge/swapchain realloc on resize — deferred
	// as a follow-up; for now we size to the current window (startup-adaptive).
	uint32_t w, h;
	if (ps_zone_active()) {
		// 3D-zone frame: render each eye at the zone's recommended view size (== zone
		// extent), so the weave is zone-confined by construction (like the avatar /
		// cube_zones). imageRect == swapchain == zone size, no sub-rect.
		ps_query_zone_rec_size();
		w = s_ps.zone_rec_w; h = s_ps.zone_rec_h;
		ps_log("[DisplayXR-PROV] swapchain sized to 3D-zone recommended view %ux%u (zone %dx%d)\n",
		       w, h, s_ps.zone_w, s_ps.zone_h);
	} else {
		uint32_t ww = 0, wh = 0; ps_window_size(&ww, &wh);
		float sx = 0.5f, sy = 0.5f; ps_active_view_scale(&sx, &sy);
		w = (uint32_t)(ww * sx);
		h = (uint32_t)(wh * sy);
	}
	if (w == 0 || h == 0) { w = s_ps.display_info.pixel_width / 2; h = s_ps.display_info.pixel_height; }
	if (w == 0 || h == 0) { w = 1920; h = 1080; }

	uint32_t fmt_count = 0;
	s_ps.pfn_enumerate_swapchain_formats(s_ps.session, 0, &fmt_count, NULL);
	if (fmt_count == 0) { ps_log("[DisplayXR-PROV] No swapchain formats\n"); return 0; }
	int64_t formats[32];
	if (fmt_count > 32) fmt_count = 32;
	s_ps.pfn_enumerate_swapchain_formats(s_ps.session, fmt_count, &fmt_count, formats);
	// Log the runtime's advertised formats once (diagnoses sRGB availability for the fix).
	{
		char fbuf[256]; int fn = 0;
		for (uint32_t i = 0; i < fmt_count && fn < 200; i++)
			fn += snprintf(fbuf + fn, sizeof(fbuf) - fn, "%lld ", (long long)formats[i]);
		ps_log("[DisplayXR-PROV] swapchain formats advertised: %s\n", fbuf);
	}
	// Present path (runtime presents to its window / built player) + Linear project ⇒ prefer an
	// sRGB format so Unity encodes linear→sRGB on store (else the runtime present is too dark).
	// Docked texture path (shared texture bound → s_probe_handle set) and Gamma projects keep UNORM.
	int present_path = s_sc_present_path;
	int want_srgb = s_color_space_linear && present_path;
	int64_t format = formats[0];
	// The int64 swapchain format is API-SPECIFIC: DXGI_FORMAT under D3D, VkFormat under
	// Vulkan, MTLPixelFormat on Metal. They are NOT interchangeable and the numbers
	// collide — DXGI 91 is B8G8R8A8_UNORM_SRGB but VkFormat 91 is R16G16B16A16_SFLOAT,
	// so running the DXGI table against a VK format list silently picks a half-float
	// swapchain. Hence a separate Vulkan arm rather than a shared one. (#247)
	int is_vk_fmt = (s_ps.graphics_api == DXR_GFX_VULKAN);
	for (uint32_t i = 0; i < fmt_count; i++) {
#ifdef _WIN32
		if (is_vk_fmt) {
			// RGBA is preferred over BGRA because the display-provider TU declares
			// kUnityXRRenderTextureFormatRGBA32 to Unity. A mismatch there makes Unity
			// build an image view whose format differs from the bridge image's, which
			// without MUTABLE_FORMAT is undefined behaviour — observed as a hard crash
			// in vk::Image::CreateImageViews inside the NVIDIA driver.
			if (want_srgb) {
				if (formats[i] == 43) { format = 43; break; } // VK_FORMAT_R8G8B8A8_SRGB
				if (formats[i] == 50) { format = 50; }        // VK_FORMAT_B8G8R8A8_SRGB
			} else {
				if (formats[i] == 37) { format = 37; break; } // VK_FORMAT_R8G8B8A8_UNORM
				if (formats[i] == 44) { format = 44; }        // VK_FORMAT_B8G8R8A8_UNORM
			}
		} else if (want_srgb) {
			if (formats[i] == 29) { format = 29; break; } // DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
			if (formats[i] == 91) { format = 91; }         // DXGI_FORMAT_B8G8R8A8_UNORM_SRGB
		} else {
			if (formats[i] == 28) { format = 28; break; } // DXGI_FORMAT_R8G8B8A8_UNORM
			if (formats[i] == 87) { format = 87; }         // DXGI_FORMAT_B8G8R8A8_UNORM
		}
#else
		if (want_srgb) {
			if (formats[i] == 71) { format = 71; break; } // MTLPixelFormatRGBA8Unorm_sRGB
			if (formats[i] == 81) { format = 81; }         // MTLPixelFormatBGRA8Unorm_sRGB
		} else {
			if (formats[i] == 70) { format = 70; break; } // MTLPixelFormatRGBA8Unorm
			if (formats[i] == 80) { format = 80; }         // MTLPixelFormatBGRA8Unorm
		}
#endif
	}
#ifdef _WIN32
	s_swapchain_srgb = is_vk_fmt ? (format == 43 || format == 50)
	                             : (format == 29 || format == 91);
#else
	s_swapchain_srgb = (format == 71 || format == 81);
#endif
	if (want_srgb && !s_swapchain_srgb)
		ps_log("[DisplayXR-PROV] WARN: Linear project but runtime advertised NO sRGB swapchain format — present stays too dark (needs a runtime-side present encode)\n");
	ps_log("[DisplayXR-PROV] swapchain format=%lld srgb=%d (linear=%d present_path=%d)\n",
	       (long long)format, s_swapchain_srgb, s_color_space_linear, present_path);

	XrSwapchainCreateInfo ci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
	ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
	                XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT |
	                XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
	ci.format = format;
	ci.sampleCount = 1;
	ci.width = w;
	ci.height = h;
	ci.faceCount = 1;
	ci.arraySize = 2;   // SPI: left = layer 0, right = layer 1
	ci.mipCount = 1;

	XrResult r = s_ps.pfn_create_swapchain(s_ps.session, &ci, &s_ps.swapchain);
	if (XR_FAILED(r)) { ps_log("[DisplayXR-PROV] xrCreateSwapchain failed: %d\n", r); return 0; }

	// arraySize is always 2 (Unity renders both eyes into separate slices); the
	// active view count only governs how many views we SUBMIT (#172 P4).
	s_ps.sc_width = w; s_ps.sc_height = h; s_ps.sc_array = 2;
	s_ps.sc_view_count = ps_active_view_count(); s_ps.sc_format = format;

	uint32_t count = 0;
	s_ps.pfn_enumerate_swapchain_images(s_ps.swapchain, 0, &count, NULL);
	if (count > PS_MAX_SWAPCHAIN_IMAGES) count = PS_MAX_SWAPCHAIN_IMAGES;
	s_ps.sc_image_count = count;
#if defined(ENABLE_VULKAN)
	// Vulkan arm first, and OUTSIDE the per-OS branch below: it is identical on
	// Windows (#247) and Linux (#249).
	if (s_ps.graphics_api == DXR_GFX_VULKAN) {
		// The runtime allocates these arraySize=2 images on the SESSION device (which
		// the runtime itself created via enable2), so they are not wrappable as Unity
		// textures — the eye bridge copies into them. Enumerate as
		// XrSwapchainImageVulkan2KHR and hand the VkImage array to the VK glue TU.
		XrSwapchainImageVulkanKHR_PS vk_imgs[PS_MAX_SWAPCHAIN_IMAGES] = {};
		for (uint32_t i = 0; i < count; i++) {
			vk_imgs[i].type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR_PS;
			vk_imgs[i].next = NULL;
			vk_imgs[i].image = NULL;
		}
		r = s_ps.pfn_enumerate_swapchain_images(s_ps.swapchain, count, &count,
		        (XrSwapchainImageBaseHeader *)vk_imgs);
		if (XR_SUCCEEDED(r))
			dxr_pvk_set_swapchain_images(vk_imgs, count, w, h, 2, format);
	} else
#endif
#ifdef _WIN32
	if (s_ps.graphics_api == DXR_GFX_D3D11) {
		// D3D11 zero-copy: the runtime allocates these arraySize=2 images on Unity's
		// device; the display-provider wraps them DIRECTLY via CreateTexture (no bridge).
		for (uint32_t i = 0; i < count; i++) {
			s_ps.sc_images_d3d11[i].type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR;
			s_ps.sc_images_d3d11[i].next = NULL;
			s_ps.sc_images_d3d11[i].texture = NULL;
		}
		r = s_ps.pfn_enumerate_swapchain_images(s_ps.swapchain, count, &count,
		        (XrSwapchainImageBaseHeader *)s_ps.sc_images_d3d11);
	} else {
		for (uint32_t i = 0; i < count; i++) {
			s_ps.sc_images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR;
			s_ps.sc_images[i].next = NULL;
			s_ps.sc_images[i].texture = NULL;
		}
		r = s_ps.pfn_enumerate_swapchain_images(s_ps.swapchain, count, &count,
		        (XrSwapchainImageBaseHeader *)s_ps.sc_images);
	}
#elif defined(__APPLE__)
	{
		// Metal (#204): enumerate XrSwapchainImageMetalKHR (id<MTLTexture> in .texture).
		for (uint32_t i = 0; i < count; i++) {
			s_ps.sc_images_metal[i].type = XR_TYPE_SWAPCHAIN_IMAGE_METAL_KHR;
			s_ps.sc_images_metal[i].next = NULL;
			s_ps.sc_images_metal[i].texture = NULL;
		}
		r = s_ps.pfn_enumerate_swapchain_images(s_ps.swapchain, count, &count,
		        (XrSwapchainImageBaseHeader *)s_ps.sc_images_metal);
	}
#else
	{
		// Linux (#249): Vulkan is the only backend, and the arm above consumed it.
		// Reaching here means GfxStart bound a backend this platform has no
		// enumeration path for — a programming error, not a user-facing state.
		ps_log("[DisplayXR-PROV] swapchain: no image-enumeration path for graphics api %d "
		       "on this platform\n", (int)s_ps.graphics_api);
		return 0;
	}
#endif
	if (XR_FAILED(r)) { ps_log("[DisplayXR-PROV] enumerate swapchain images failed: %d\n", r); return 0; }

	s_ps.swapchain_created = 1;
#if defined(ENABLE_VULKAN)
	// api= is the line that settles "did this run actually get Vulkan?" — Unity falls
	// back graphics APIs SILENTLY, so a healthy-looking session proves nothing until
	// this says Vulkan (#248, paid for on Windows; the same trap applies on Linux).
	if (s_ps.graphics_api == DXR_GFX_VULKAN) {
		ps_log("[DisplayXR-PROV] swapchain: %ux%u arraySize=2 submit=%u-view (%s), %u images, "
		       "fmt=%lld, api=Vulkan(enable2 bridge)\n",
		       w, h, s_ps.sc_view_count, s_ps.sc_view_count >= 2 ? "3D" : "2D", count,
		       (long long)format);
		ps_create_bridge_vk();    // external-memory bridge to Unity's VkDevice (#247/#249)
	} else
#endif
#ifdef _WIN32
	{
	ps_log("[DisplayXR-PROV] swapchain: %ux%u arraySize=2 submit=%u-view (%s), %u images, fmt=%lld, api=%s\n",
	       w, h, s_ps.sc_view_count, s_ps.sc_view_count >= 2 ? "3D" : "2D", count, format,
	       s_ps.graphics_api != DXR_GFX_D3D11 ? "D3D12"
	           : (s_ps.d3d11_bridge ? "D3D11(editor bridge)" : "D3D11(zero-copy)"));

	if (s_ps.graphics_api != DXR_GFX_D3D11)
		ps_create_bridge();       // D3D12: pair the cross-device bridge with the swapchain
	else
		ps_create_bridge_d3d11(); // D3D11: per-sub-mode targets (SPI/MultiPass, bridge/zero-copy) (#195)
	}
#elif defined(__APPLE__)
	ps_log("[DisplayXR-PROV] swapchain: %ux%u arraySize=2 submit=%u-view (%s), %u images, fmt=%lld, api=Metal\n",
	       w, h, s_ps.sc_view_count, s_ps.sc_view_count >= 2 ? "3D" : "2D", count, (long long)format);
	// ZERO-COPY (#204): per-image per-eye slice views of the swapchain images —
	// Unity renders straight into the slices; no provider blit (see the field
	// comment on eye_view_metal). Realloc-safe: the glue parks replaced views
	// in its graveyard (ADR-001 deferred destruction).
	for (uint32_t i = 0; i < count; i++) {
		for (uint32_t e = 0; e < 2; e++) {
			s_ps.eye_view_metal[i][e] =
			    dxr_prov_metal_slice_view(s_ps.sc_images_metal[i].texture, e);
			if (!s_ps.eye_view_metal[i][e]) {
				ps_log("[DisplayXR-PROV] Metal: slice view [%u][%u] failed\n", i, e);
				return 0;
			}
		}
	}
#else
	{
		// Linux (#249): Vulkan is the only backend and the arm above consumed it.
		// This closes the dangling `else` when neither D3D nor Metal is compiled in.
		ps_log("[DisplayXR-PROV] swapchain: no bridge path for graphics api %d on this "
		       "platform\n", (int)s_ps.graphics_api);
		return 0;
	}
#endif
	return 1;
}

#ifdef __APPLE__
// Internal (display-provider TU): the zero-copy per-image per-eye slice view.
extern "C" void *dxr_prov_get_metal_eye_view(uint32_t image, uint32_t eye)
{
	if (image >= s_ps.sc_image_count || eye > 1) return NULL;
	return s_ps.eye_view_metal[image][eye];
}
#endif

#if defined(ENABLE_VULKAN)
// Vulkan (#247 Windows, #249 Linux): create the external-memory render target(s) Unity
// draws into, sized to the swapchain. Structurally the D3D12 own-device bridge — there
// is no zero-copy sub-mode, because under enable2 the session device is created by the
// RUNTIME and can never be Unity's (see displayxr_provider_gfx_vulkan.h for the full
// argument).
//   - SPI:       one shared 2-layer image  -> bridge slot -1
//   - MultiPass: two shared 1-layer images -> bridge slots 0 and 1
//
// Lives OUTSIDE the Windows-only D3D helper region below — the VK bridge is
// platform-neutral and only its handle flavour differs.
static int ps_create_bridge_vk(void)
{
	uint32_t w = s_ps.sc_width, h = s_ps.sc_height;
	if (w == 0 || h == 0) return 0;
	if (!dxr_pvk_device_ready()) return 0;

	if (dxr_prov_get_single_pass())
		return dxr_pvk_create_bridge(-1, w, h, 2, s_ps.sc_format);

	for (int e = 0; e < 2; e++) {
		if (!dxr_pvk_create_bridge(e, w, h, 1, s_ps.sc_format)) {
			ps_log("[DisplayXR-PROV] Vulkan MultiPass eye %d bridge alloc failed\n", e);
			return 0;
		}
	}
	return 1;
}
#endif

#ifdef _WIN32
// LUID → human-readable adapter description ("NVIDIA GeForce RTX 3080 Laptop
// GPU") for the mismatch diagnostics below. Best-effort; "unknown adapter" on
// any failure.
static void ps_describe_adapter(LUID luid, char *out, size_t out_len)
{
	snprintf(out, out_len, "unknown adapter");
	IDXGIFactory1 *factory = NULL;
	if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&factory)) || !factory) {
		return;
	}
	for (UINT i = 0;; i++) {
		IDXGIAdapter1 *ad = NULL;
		if (factory->EnumAdapters1(i, &ad) == DXGI_ERROR_NOT_FOUND || !ad) {
			break;
		}
		DXGI_ADAPTER_DESC1 ds = {};
		if (SUCCEEDED(ad->GetDesc1(&ds)) && ds.AdapterLuid.HighPart == luid.HighPart &&
		    ds.AdapterLuid.LowPart == luid.LowPart) {
			snprintf(out, out_len, "%ls", ds.Description);
			ad->Release();
			break;
		}
		ad->Release();
	}
	factory->Release();
}

// D3D11 zero-copy (#195): call xrGetD3D11GraphicsRequirementsKHR (mandatory before
// xrCreateSession) and verify Unity's D3D11 device sits on the runtime's required
// adapter LUID. Returns 1 if OK (or the requirement PFN is absent → let the session
// create decide), 0 on an adapter mismatch (multi-GPU) so we fail gracefully
// instead of the runtime's generic GRAPHICS_DEVICE_INVALID.
//
// The mismatch arm is deliberately LOUD (#240): a quiet one-line WARN here cost
// a whole perf study a voided headline — the app looks healthy (window up,
// player rendering, 60 Hz presents) while nothing ever reaches the panel. Name
// both adapters and print the exact knobs that align them.
static int ps_d3d11_check_requirements(void)
{
	PFN_xrVoidFunction fn = NULL;
	s_ps.gipa(s_ps.instance, "xrGetD3D11GraphicsRequirementsKHR", &fn);
	if (!fn) {
		ps_log("[DisplayXR-PROV] xrGetD3D11GraphicsRequirementsKHR unresolved "
		       "(runtime lacks XR_KHR_D3D11_enable?)\n");
		return 0;
	}
	XrGraphicsRequirementsD3D11KHR req = {XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
	if (XR_FAILED(((PFN_xrGetD3D11GraphicsRequirementsKHR)fn)(s_ps.instance, s_ps.system_id, &req))) {
		ps_log("[DisplayXR-PROV] xrGetD3D11GraphicsRequirementsKHR failed\n");
		return 0;
	}
	// Compare Unity's device adapter LUID (IDXGIDevice→GetAdapter→GetDesc) to req.adapterLuid.
	LUID unity_luid = {};
	IDXGIDevice *dxgi = NULL;
	if (SUCCEEDED(s_ps.unity_d3d11_device->QueryInterface(__uuidof(IDXGIDevice), (void **)&dxgi)) && dxgi) {
		IDXGIAdapter *ad = NULL;
		if (SUCCEEDED(dxgi->GetAdapter(&ad)) && ad) {
			DXGI_ADAPTER_DESC ds = {};
			if (SUCCEEDED(ad->GetDesc(&ds))) unity_luid = ds.AdapterLuid;
			ad->Release();
		}
		dxgi->Release();
	}
	int match = (unity_luid.HighPart == req.adapterLuid.HighPart &&
	             unity_luid.LowPart == req.adapterLuid.LowPart);
	if (!match && (req.adapterLuid.HighPart != 0 || req.adapterLuid.LowPart != 0)) {
		char unity_desc[128], req_desc[128], msg[1024];
		ps_describe_adapter(unity_luid, unity_desc, sizeof(unity_desc));
		ps_describe_adapter(req.adapterLuid, req_desc, sizeof(req_desc));
		snprintf(msg, sizeof(msg),
		         "[DisplayXR-PROV] ERROR: GPU adapter mismatch - XR session NOT started, nothing "
		         "will reach the 3D display (#195/#240).\n"
		         "[DisplayXR-PROV]   Unity renders on : %s (LUID %08x-%08x)\n"
		         "[DisplayXR-PROV]   Runtime requires : %s (LUID %08x-%08x)\n"
		         "[DisplayXR-PROV]   Fix: align them. Either set this app's Windows GpuPreference "
		         "to the runtime's adapter (Settings > System > Display > Graphics), or point the "
		         "runtime at Unity's adapter with the env var DXR_D3D_FORCE_GPU=igpu|dgpu "
		         "(runtime >= v2.2.4). '-force-d3d12' on the command line is an alternative "
		         "when Unity's device filter forced an unintended adapter/API.\n",
		         unity_desc, (unsigned)unity_luid.HighPart, (unsigned)unity_luid.LowPart,
		         req_desc, (unsigned)req.adapterLuid.HighPart, (unsigned)req.adapterLuid.LowPart);
		ps_log(msg);
		return 0;
	}
	ps_log("[DisplayXR-PROV] D3D11 graphics requirements OK (zero-copy on Unity's device)\n");
	return 1;
}

// Create the session's OWN D3D12 device on the runtime's adapter LUID, plus a
// queue + command list + fence for the per-frame bridge→swapchain copy.
// (Mirrors displayxr_standalone_d3d12.cpp create_device.)
// True when the Shell launched us as a workspace tile (DISPLAYXR_WORKSPACE_SESSION=1).
// Defined in displayxr_win32.c. In this mode the runtime is in IPC/service mode.
extern "C" int displayxr_is_shell_mode(void);

// Workspace/IPC (shell) mode: bind the OpenXR D3D12 session to UNITY'S device instead of
// a separate own-device (ps_create_own_device). The own-device isolation exists ONLY to
// keep the in-process Leia SR weaver off Unity's GameView present device (the Optimus
// cross-present deadlock). Under the shell there is NO in-process weaver and NO GameView
// present of the woven output — the out-of-process compositor SERVICE composites — so that
// rationale doesn't apply. Binding Unity's device makes the whole D3D12 bridge path
// SAME-DEVICE: the per-eye bridge's own/unity views alias one resource, the render->copy
// sync is same-queue, and the copy lands Unity's pixels in the arr=2 swapchain slices
// coherently. A CROSS-device bridge left the slices BLACK under the service path (ADR-032
// §Consequences: "bind the session to the engine's own device to remove [the bridge copy]";
// issue #223 round 7). We alias own_device/own_queue = Unity's (AddRef so session_stop's
// Release stays balanced and never over-releases Unity's device) and create our own command
// list + fence on Unity's device — so every downstream helper (ps_create_bridge, submit_frame)
// is unchanged, just same-device now. Matches cube_handle_d3d12_win (engine device = session
// device), the working IPC reference.
static int ps_alias_unity_device_d3d12(void)
{
	if (!s_ps.unity_device || !s_ps.unity_queue) return 0;
	// xrGetD3D12GraphicsRequirementsKHR is MANDATORY before xrCreateSession (skipping it
	// fails the create with GRAPHICS_REQUIREMENTS_CALL_MISSING). We don't build a device
	// from the LUID — Unity already picked the display GPU — but the call must fire.
	PFN_xrVoidFunction fn = NULL;
	s_ps.gipa(s_ps.instance, "xrGetD3D12GraphicsRequirementsKHR", &fn);
	if (fn) {
		XrGraphicsRequirementsD3D12KHR req = {XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR};
		if (XR_FAILED(((PFN_xrGetD3D12GraphicsRequirementsKHR)fn)(s_ps.instance, s_ps.system_id, &req))) {
			ps_log("[DisplayXR-PROV] xrGetD3D12GraphicsRequirementsKHR failed (shell alias)\n");
			return 0;
		}
	}
	s_ps.own_device = s_ps.unity_device; s_ps.own_device->AddRef();
	s_ps.own_queue  = s_ps.unity_queue;  s_ps.own_queue->AddRef();
	s_ps.own_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
	        __uuidof(ID3D12CommandAllocator), (void **)&s_ps.own_cmd_alloc);
	s_ps.own_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
	        s_ps.own_cmd_alloc, NULL, __uuidof(ID3D12GraphicsCommandList),
	        (void **)&s_ps.own_cmd_list);
	if (s_ps.own_cmd_list) s_ps.own_cmd_list->Close();
	s_ps.own_device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
	        __uuidof(ID3D12Fence), (void **)&s_ps.own_fence);
	s_ps.own_fence_event = CreateEvent(NULL, FALSE, FALSE, NULL);
	s_ps.own_fence_value = 0;
	if (!s_ps.own_cmd_alloc || !s_ps.own_cmd_list || !s_ps.own_fence) {
		ps_log("[DisplayXR-PROV] shell alias: cmd/fence create failed\n");
		return 0;
	}
	ps_log("[DisplayXR-PROV] D3D12 session bound to UNITY's device (workspace/IPC same-device bridge, ADR-032)\n");
	return 1;
}

static int ps_create_own_device(void)
{
	PFN_xrVoidFunction fn = NULL;
	s_ps.gipa(s_ps.instance, "xrGetD3D12GraphicsRequirementsKHR", &fn);
	LUID luid = {};
	if (fn) {
		XrGraphicsRequirementsD3D12KHR req = {XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR};
		if (XR_FAILED(((PFN_xrGetD3D12GraphicsRequirementsKHR)fn)(s_ps.instance, s_ps.system_id, &req))) {
			ps_log("[DisplayXR-PROV] xrGetD3D12GraphicsRequirementsKHR failed\n");
			return 0;
		}
		luid = req.adapterLuid;
	}

	// The eye bridge copies Unity's slices into the session swapchain, and a
	// CROSS-ADAPTER bridge presents black (ADR-032 / #223; reproduced on HW in
	// #240: session fully up, mode 3D, nothing on the panel). If Unity's D3D12
	// device is not on the runtime's required adapter, refuse loudly with the
	// exact knobs instead of starting a session that can never display.
	if (s_ps.unity_device && (luid.HighPart != 0 || luid.LowPart != 0)) {
		LUID unity_luid = s_ps.unity_device->GetAdapterLuid();
		if (unity_luid.HighPart != luid.HighPart || unity_luid.LowPart != luid.LowPart) {
			char unity_desc[128], req_desc[128], msg[1024];
			ps_describe_adapter(unity_luid, unity_desc, sizeof(unity_desc));
			ps_describe_adapter(luid, req_desc, sizeof(req_desc));
			snprintf(msg, sizeof(msg),
			         "[DisplayXR-PROV] ERROR: GPU adapter mismatch - XR session NOT started, a "
			         "cross-adapter eye bridge would present black (#240).\n"
			         "[DisplayXR-PROV]   Unity renders on : %s (LUID %08x-%08x)\n"
			         "[DisplayXR-PROV]   Runtime requires : %s (LUID %08x-%08x)\n"
			         "[DisplayXR-PROV]   Fix: align them. Either set this app's Windows "
			         "GpuPreference to the runtime's adapter (Settings > System > Display > "
			         "Graphics), or point the runtime at Unity's adapter with the env var "
			         "DXR_D3D_FORCE_GPU=igpu|dgpu (runtime >= v2.2.4).\n",
			         unity_desc, (unsigned)unity_luid.HighPart, (unsigned)unity_luid.LowPart,
			         req_desc, (unsigned)luid.HighPart, (unsigned)luid.LowPart);
			ps_log(msg);
			return 0;
		}
	}

	IDXGIFactory4 *factory = NULL;
	CreateDXGIFactory2(0, __uuidof(IDXGIFactory4), (void **)&factory);
	IDXGIAdapter1 *adapter = NULL;
	if (factory && (luid.HighPart != 0 || luid.LowPart != 0))
		factory->EnumAdapterByLuid(luid, __uuidof(IDXGIAdapter1), (void **)&adapter);
	HRESULT hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0,
	                               __uuidof(ID3D12Device), (void **)&s_ps.own_device);
	if (adapter) adapter->Release();
	if (factory) factory->Release();
	if (FAILED(hr) || !s_ps.own_device) {
		ps_log("[DisplayXR-PROV] D3D12CreateDevice (own) failed: 0x%08lx\n", hr);
		return 0;
	}

	D3D12_COMMAND_QUEUE_DESC qd = {};
	qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	if (FAILED(s_ps.own_device->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue),
	        (void **)&s_ps.own_queue))) {
		ps_log("[DisplayXR-PROV] CreateCommandQueue (own) failed\n");
		return 0;
	}
	s_ps.own_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
	        __uuidof(ID3D12CommandAllocator), (void **)&s_ps.own_cmd_alloc);
	s_ps.own_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
	        s_ps.own_cmd_alloc, NULL, __uuidof(ID3D12GraphicsCommandList),
	        (void **)&s_ps.own_cmd_list);
	s_ps.own_cmd_list->Close();
	s_ps.own_device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
	        __uuidof(ID3D12Fence), (void **)&s_ps.own_fence);
	s_ps.own_fence_event = CreateEvent(NULL, FALSE, FALSE, NULL);
	s_ps.own_fence_value = 0;
	ps_log("[DisplayXR-PROV] Own D3D12 device + queue created (runtime session device)\n");
	return 1;
}

// Create the SHARED 2-slice-array bridge on own_device and open it on Unity's
// device. Unity renders both eyes into the Unity-side resource; submit copies
// the own-side resource into the runtime swapchain. (Mirrors create_atlas_bridge,
// extended arraySize=1 → 2 for SPI.)
// Allocate one SHARED render-target texture on own_device and open it on Unity's
// device. arr = 2 (SPI 2-slice array) or 1 (a single MultiPass eye).
static int ps_alloc_shared_tex(uint32_t w, uint32_t h, UINT16 arr,
                               ID3D12Resource **out_own, ID3D12Resource **out_unity,
                               HANDLE *out_handle, const char *label)
{
	// D3D11 zero-copy has no own device / shared bridge (#195). Secondary layers
	// (wsui/Local2D/extra-zones) that lean on this are a D3D11 follow-up; skip cleanly.
	if (!s_ps.own_device) {
		static int warned = 0;
		if (!warned) { warned = 1;
			ps_log("[DisplayXR-PROV] NOTE: shared-bridge layers (wsui/Local2D/extra-zones) are "
			       "not yet supported on D3D11 — skipping (#195).\n"); }
		return 0;
	}
	// Workspace/IPC (shell) same-device path: own_device == Unity's device (see
	// ps_alias_unity_device_d3d12), so there is NO cross-device boundary — a shared
	// NT-handle texture is unnecessary, and a same-device OpenSharedHandle is an unusual/
	// untested aliasing pattern that can leave the "own" view reading black even though
	// Unity rendered into the "unity" view (the arr=2 swapchain slices stayed black at the
	// service, #223 r9). Allocate a PLAIN Unity-device RT and alias own==unity to the SAME
	// resource: Unity renders into it and submit copies THAT resource into the swapchain
	// slice — guaranteed coherent (mirrors the D3D11 zero-copy secondary path,
	// ps_alloc_unity_tex). AddRef so the two cleanup Releases (bridge_own + bridge_unity)
	// stay balanced; no shared handle to close (out_handle = NULL).
	if (displayxr_is_shell_mode()) {
		D3D12_HEAP_PROPERTIES ph = {}; ph.Type = D3D12_HEAP_TYPE_DEFAULT;
		D3D12_RESOURCE_DESC pd = {};
		pd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		pd.Width = w; pd.Height = h; pd.DepthOrArraySize = arr; pd.MipLevels = 1;
		pd.Format = ps_sc_dxgi_format(s_ps.sc_format);
		pd.SampleDesc.Count = 1;
		pd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		pd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
		ID3D12Resource *ptex = NULL;
		HRESULT phr = s_ps.unity_device->CreateCommittedResource(&ph, D3D12_HEAP_FLAG_NONE, &pd,
		        D3D12_RESOURCE_STATE_COMMON, NULL, __uuidof(ID3D12Resource), (void **)&ptex);
		if (FAILED(phr) || !ptex) {
			ps_log("[DisplayXR-PROV] %s plain same-device (shell) create failed: 0x%08lx\n", label, phr);
			return 0;
		}
		*out_own = ptex; *out_unity = ptex; ptex->AddRef();
		*out_handle = NULL;
		ps_log("[DisplayXR-PROV] %s: %ux%u arr=%u PLAIN same-device (shell) tex=%p\n",
		       label, w, h, (unsigned)arr, (void *)ptex);
		return 1;
	}
	D3D12_HEAP_PROPERTIES heap = {};
	heap.Type = D3D12_HEAP_TYPE_DEFAULT;
	D3D12_RESOURCE_DESC bd = {};
	bd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	bd.Width = w;
	bd.Height = h;
	bd.DepthOrArraySize = arr;
	bd.MipLevels = 1;
	bd.Format = ps_sc_dxgi_format(s_ps.sc_format);
	bd.SampleDesc.Count = 1;
	bd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

	HRESULT hr = s_ps.own_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &bd,
	        D3D12_RESOURCE_STATE_COMMON, NULL, __uuidof(ID3D12Resource), (void **)out_own);
	if (FAILED(hr)) { ps_log("[DisplayXR-PROV] %s CreateCommittedResource failed: 0x%08lx\n", label, hr); return 0; }
	hr = s_ps.own_device->CreateSharedHandle(*out_own, NULL, GENERIC_ALL, NULL, out_handle);
	if (FAILED(hr) || !*out_handle) { ps_log("[DisplayXR-PROV] %s CreateSharedHandle failed: 0x%08lx\n", label, hr); return 0; }
	hr = s_ps.unity_device->OpenSharedHandle(*out_handle, __uuidof(ID3D12Resource), (void **)out_unity);
	if (FAILED(hr) || !*out_unity) { ps_log("[DisplayXR-PROV] %s OpenSharedHandle (Unity) failed: 0x%08lx\n", label, hr); return 0; }
	ps_log("[DisplayXR-PROV] %s: %ux%u arr=%u own=%p unity=%p\n", label, w, h, (unsigned)arr,
	       (void *)*out_own, (void *)*out_unity);
	return 1;
}

// D3D11 zero-copy (#195): secondary composition layers (wsui/Local2D/extra-zones) don't
// have the D3D12 own-device shared bridge. Instead they render into a PLAIN Unity-device
// texture and submit does a SAME-device CopyResource into the acquired runtime swapchain
// image (no own device, no shared handle, no fence — the session is already bound on
// Unity's ID3D11Device, so the runtime and Unity share it). fmt: 87=BGRA8, else RGBA8.
static ID3D11Texture2D *ps_alloc_unity_tex(uint32_t w, uint32_t h, uint32_t arr, int64_t fmt)
{
	if (!s_ps.unity_d3d11_device || w == 0 || h == 0 || arr == 0) return NULL;
	D3D11_TEXTURE2D_DESC td = {};
	td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = arr;
	td.Format = ps_sc_dxgi_format(fmt);
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
	ID3D11Texture2D *tex = NULL;
	HRESULT hr = s_ps.unity_d3d11_device->CreateTexture2D(&td, NULL, &tex);
	if (FAILED(hr) || !tex) { ps_log("[DisplayXR-PROV] ps_alloc_unity_tex %ux%u arr=%u failed: 0x%08lx\n",
	                                  w, h, (unsigned)arr, hr); return NULL; }
	return tex;
}

// ============================================================================
// D3D11 EDITOR own-device bridge (#195). In the editor, binding the runtime session
// on Unity's D3D11 device deadlocks on this Optimus laptop: Unity's GameView present
// scans out through the iGPU while the in-process Leia SR weaver runs on the dGPU,
// and the two serialize on the shared device. Give D3D11 the same isolation D3D12
// has — a SEPARATE ID3D11Device + shared-texture bridge + per-frame copy — so the
// weaver never shares Unity's present device. Gated editor-only (dxr_prov_get_
// dedicated_window); the built-player zero-copy path is untouched.
// ============================================================================

// Create a SEPARATE ID3D11Device on the runtime's required adapter (matched to the
// D3D11 graphics-requirements LUID = the display's GPU / dGPU, matching the weaver) +
// its immediate context, and a shared ID3D11Fence (D3D11.4) for the cross-device copy
// ordering. Mirrors ps_create_own_device (D3D12). NB: calling xrGetD3D11Graphics-
// RequirementsKHR is MANDATORY before xrCreateSession — skipping it (as an earlier cut
// did) makes xrCreateSession fail with GRAPHICS_REQUIREMENTS_CALL_MISSING.
static int ps_create_own_device_d3d11(void)
{
	if (!s_ps.unity_d3d11_device) return 0;

	// Mandatory pre-session call; also yields the adapter LUID the runtime requires.
	PFN_xrVoidFunction fn = NULL;
	s_ps.gipa(s_ps.instance, "xrGetD3D11GraphicsRequirementsKHR", &fn);
	LUID luid = {};
	if (fn) {
		XrGraphicsRequirementsD3D11KHR req = {XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
		if (XR_FAILED(((PFN_xrGetD3D11GraphicsRequirementsKHR)fn)(s_ps.instance, s_ps.system_id, &req))) {
			ps_log("[DisplayXR-PROV] xrGetD3D11GraphicsRequirementsKHR failed\n");
			return 0;
		}
		luid = req.adapterLuid;
	} else {
		ps_log("[DisplayXR-PROV] WARN: xrGetD3D11GraphicsRequirementsKHR unresolved (editor bridge)\n");
	}

	// Prefer the runtime's required adapter (EnumAdapterByLuid); fall back to Unity's
	// adapter if the runtime left the LUID unpinned (0). Isolation is per-DEVICE — a
	// separate device on the SAME adapter is what breaks the Optimus present/weave contention.
	IDXGIFactory4 *factory = NULL;
	CreateDXGIFactory2(0, __uuidof(IDXGIFactory4), (void **)&factory);
	IDXGIAdapter *adapter = NULL;
	if (factory && (luid.HighPart != 0 || luid.LowPart != 0))
		factory->EnumAdapterByLuid(luid, __uuidof(IDXGIAdapter), (void **)&adapter);
	if (!adapter) {
		// Runtime LUID unpinned → use Unity's adapter (= the dGPU it was forced onto).
		IDXGIDevice *dxgi_dev = NULL;
		if (SUCCEEDED(s_ps.unity_d3d11_device->QueryInterface(__uuidof(IDXGIDevice), (void **)&dxgi_dev)) && dxgi_dev) {
			dxgi_dev->GetAdapter(&adapter);
			dxgi_dev->Release();
		}
	}
	UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
	D3D_FEATURE_LEVEL fl_in[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
	D3D_FEATURE_LEVEL fl_out = (D3D_FEATURE_LEVEL)0;
	// With an explicit adapter the driver type MUST be UNKNOWN.
	HRESULT hr = D3D11CreateDevice(adapter,
	        adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, NULL, flags,
	        fl_in, 2, D3D11_SDK_VERSION, &s_ps.own_d3d11_device, &fl_out, &s_ps.own_d3d11_context);
	if (adapter) adapter->Release();
	if (factory) factory->Release();
	if (FAILED(hr) || !s_ps.own_d3d11_device || !s_ps.own_d3d11_context) {
		ps_log("[DisplayXR-PROV] D3D11CreateDevice (own) failed: 0x%08lx\n", hr);
		return 0;
	}

	// Shared fence (D3D11.4): own device creates it shared; Unity's device opens the
	// same fence. submit signals it on Unity's context (after the render) and waits on
	// it on the own context (before the copy). Optional — coarse EVENT-query flush is
	// the fallback if ID3D11Device5 is unavailable.
	s_ps.d3d11_use_fence = 0;
	ID3D11Device5 *own5 = NULL, *unity5 = NULL;
	if (SUCCEEDED(s_ps.own_d3d11_device->QueryInterface(__uuidof(ID3D11Device5), (void **)&own5)) && own5 &&
	    SUCCEEDED(s_ps.unity_d3d11_device->QueryInterface(__uuidof(ID3D11Device5), (void **)&unity5)) && unity5) {
		HRESULT hf = own5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, __uuidof(ID3D11Fence), (void **)&s_ps.d3d11_fence_own);
		if (SUCCEEDED(hf) && s_ps.d3d11_fence_own) {
			hf = s_ps.d3d11_fence_own->CreateSharedHandle(NULL, GENERIC_ALL, NULL, &s_ps.d3d11_fence_handle);
			if (SUCCEEDED(hf) && s_ps.d3d11_fence_handle)
				hf = unity5->OpenSharedFence(s_ps.d3d11_fence_handle, __uuidof(ID3D11Fence), (void **)&s_ps.d3d11_fence_unity);
			if (SUCCEEDED(hf) && s_ps.d3d11_fence_unity) s_ps.d3d11_use_fence = 1;
		}
	}
	if (own5) own5->Release();
	if (unity5) unity5->Release();
	ps_log("[DisplayXR-PROV] Own D3D11 device created (editor bridge); sync = %s\n",
	       s_ps.d3d11_use_fence ? "shared ID3D11Fence" : "coarse EVENT-query flush");
	return 1;
}

// Allocate one SHARED render-target texture on the OWN D3D11 device and open it on
// Unity's device. Uses NT-handle sharing (D3D11_RESOURCE_MISC_SHARED_NTHANDLE |
// D3D11_RESOURCE_MISC_SHARED) via IDXGIResource1::CreateSharedHandle +
// ID3D11Device1::OpenSharedResource1 — NOT keyed-mutex (Unity never Acquire/Releases →
// would deadlock), and NOT legacy MISC_SHARED (that is restricted to single 2D
// non-arrayed textures — it cannot share the 2-slice SPI array). NT-handle sharing
// supports arrays; ordering comes from the shared fence (or coarse flush). arr = 2 (SPI
// 2-slice array) or 1. fmt: 87=BGRA8, else RGBA8. The NT handle is CLOSED here once both
// sides are opened (the texture stays alive via COM refcounts), so *out_handle returns NULL
// and callers never track or close it.
static int ps_alloc_shared_tex_d3d11(uint32_t w, uint32_t h, uint32_t arr,
                                     ID3D11Texture2D **out_own, ID3D11Texture2D **out_unity,
                                     HANDLE *out_handle, int64_t fmt, const char *label)
{
	if (!s_ps.own_d3d11_device || !s_ps.unity_d3d11_device || w == 0 || h == 0 || arr == 0) return 0;
	D3D11_TEXTURE2D_DESC td = {};
	td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = arr;
	td.Format = ps_sc_dxgi_format(fmt);
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
	td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED; // no keyed mutex; array-capable
	HRESULT hr = s_ps.own_d3d11_device->CreateTexture2D(&td, NULL, out_own);
	if (FAILED(hr) || !*out_own) { ps_log("[DisplayXR-PROV] %s CreateTexture2D(own) failed: 0x%08lx\n", label, hr); return 0; }
	IDXGIResource1 *dxgi = NULL;
	hr = (*out_own)->QueryInterface(__uuidof(IDXGIResource1), (void **)&dxgi);
	if (FAILED(hr) || !dxgi) { ps_log("[DisplayXR-PROV] %s QI IDXGIResource1 failed: 0x%08lx\n", label, hr); return 0; }
	hr = dxgi->CreateSharedHandle(NULL, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, NULL, out_handle);
	dxgi->Release();
	if (FAILED(hr) || !*out_handle) { ps_log("[DisplayXR-PROV] %s CreateSharedHandle failed: 0x%08lx\n", label, hr); return 0; }
	ID3D11Device1 *unity1 = NULL;
	hr = s_ps.unity_d3d11_device->QueryInterface(__uuidof(ID3D11Device1), (void **)&unity1);
	if (FAILED(hr) || !unity1) { ps_log("[DisplayXR-PROV] %s QI ID3D11Device1(Unity) failed: 0x%08lx\n", label, hr); return 0; }
	hr = unity1->OpenSharedResource1(*out_handle, __uuidof(ID3D11Texture2D), (void **)out_unity);
	unity1->Release();
	if (FAILED(hr) || !*out_unity) { ps_log("[DisplayXR-PROV] %s OpenSharedResource1(Unity) failed: 0x%08lx\n", label, hr); return 0; }
	// Both sides now hold the resource; the NT handle is no longer needed.
	CloseHandle(*out_handle); *out_handle = NULL;
	ps_log("[DisplayXR-PROV] %s: %ux%u arr=%u own=%p unity=%p (D3D11 NT-handle shared)\n", label, w, h,
	       (unsigned)arr, (void *)*out_own, (void *)*out_unity);
	return 1;
}

// Create the D3D11 render target(s) Unity draws into, sized to the swapchain. Handles
// BOTH sub-modes (editor own-device bridge / zero-copy player) x BOTH render modes
// (SPI / MultiPass), keyed on dxr_prov_get_single_pass(). Mirrors ps_create_bridge (D3D12).
//   - EDITOR bridge: SPI = one shared 2-slice array (d3d11_bridge_own/unity); MultiPass =
//     two shared single-slice per-eye textures (d3d11_bridge_*_eye[0/1]). own creates +
//     Unity opens; submit copies the own side into the swapchain image on the own context.
//   - ZERO-COPY player: SPI = nothing (Unity renders straight into the runtime swapchain
//     image); MultiPass = two PLAIN Unity-device single-slice textures (d3d11_bridge_unity_
//     eye[0/1], own side NULL); submit same-device-copies them into the image slices.
static int ps_create_bridge_d3d11(void)
{
	if (!s_ps.unity_d3d11_device) return 0;
	uint32_t w = s_ps.sc_width, h = s_ps.sc_height;
	if (w == 0 || h == 0) return 0;
	int sp = dxr_prov_get_single_pass();

	if (s_ps.d3d11_bridge) {
		if (!s_ps.own_d3d11_device) return 0;
		if (sp) {
			if (s_ps.d3d11_bridge_own) return 1; // already created (realloc releases first)
			return ps_alloc_shared_tex_d3d11(w, h, 2, &s_ps.d3d11_bridge_own, &s_ps.d3d11_bridge_unity,
			                                 &s_ps.d3d11_bridge_handle, s_ps.sc_format,
			                                 "Bridge D3D11 (SPI 2-slice array)");
		}
		if (s_ps.d3d11_bridge_own_eye[0]) return 1; // already created
		for (int e = 0; e < 2; e++) {
			if (!ps_alloc_shared_tex_d3d11(w, h, 1, &s_ps.d3d11_bridge_own_eye[e],
			                               &s_ps.d3d11_bridge_unity_eye[e], &s_ps.d3d11_bridge_handle_eye[e],
			                               s_ps.sc_format,
			                               e == 0 ? "Bridge D3D11 (MultiPass left)" : "Bridge D3D11 (MultiPass right)"))
				return 0;
		}
		return 1;
	}

	// Zero-copy player.
	if (sp) return 1; // Unity renders straight into the runtime images; nothing to allocate.
	if (s_ps.d3d11_bridge_unity_eye[0]) return 1; // already created
	for (int e = 0; e < 2; e++) {
		s_ps.d3d11_bridge_unity_eye[e] = ps_alloc_unity_tex(w, h, 1, s_ps.sc_format);
		if (!s_ps.d3d11_bridge_unity_eye[e]) {
			ps_log("[DisplayXR-PROV] D3D11 zero-copy MultiPass eye %d target alloc failed\n", e);
			return 0;
		}
	}
	ps_log("[DisplayXR-PROV] D3D11 zero-copy MultiPass: 2 per-eye Unity targets %ux%u\n", w, h);
	return 1;
}

// Cross-device copy sync helpers (#195 editor bridge). Signal after Unity's render;
// wait before the own-device copy; drain the own context so the runtime's weave reads
// finished pixels. Fence path when available, coarse EVENT-query flush otherwise.
static void ps_d3d11_ctx_drain(ID3D11DeviceContext *ctx, ID3D11Device *dev)
{
	if (!ctx || !dev) return;
	ctx->Flush();
	D3D11_QUERY_DESC qd = {}; qd.Query = D3D11_QUERY_EVENT;
	ID3D11Query *q = NULL;
	if (SUCCEEDED(dev->CreateQuery(&qd, &q)) && q) {
		ctx->End(q);
		BOOL done = FALSE;
		while (ctx->GetData(q, &done, sizeof(done), 0) == S_FALSE) { /* spin */ }
		q->Release();
	}
}

// Order the own-device copy AFTER Unity's render into the shared bridge(s). Call once
// per frame, before any own-context CopyResource. Uses the shared fence if available.
static void ps_d3d11_bridge_sync_before_copy(void)
{
	if (!s_ps.d3d11_bridge) return;
	if (s_ps.d3d11_use_fence && s_ps.d3d11_fence_unity && s_ps.d3d11_fence_own &&
	    s_ps.unity_d3d11_context && s_ps.own_d3d11_context) {
		ID3D11DeviceContext4 *uc4 = NULL, *oc4 = NULL;
		if (SUCCEEDED(s_ps.unity_d3d11_context->QueryInterface(__uuidof(ID3D11DeviceContext4), (void **)&uc4)) && uc4 &&
		    SUCCEEDED(s_ps.own_d3d11_context->QueryInterface(__uuidof(ID3D11DeviceContext4), (void **)&oc4)) && oc4) {
			s_ps.d3d11_sync_val++;
			uc4->Signal(s_ps.d3d11_fence_unity, s_ps.d3d11_sync_val); // ordered after Unity's render
			s_ps.unity_d3d11_context->Flush();
			oc4->Wait(s_ps.d3d11_fence_own, s_ps.d3d11_sync_val);      // own GPU waits for it
		} else {
			// QI failed → coarse: fully drain Unity's writes on the CPU.
			ps_d3d11_ctx_drain(s_ps.unity_d3d11_context, s_ps.unity_d3d11_device);
		}
		if (uc4) uc4->Release();
		if (oc4) oc4->Release();
	} else {
		// Coarse fallback: CPU-drain Unity's context so the own copy reads finished pixels.
		ps_d3d11_ctx_drain(s_ps.unity_d3d11_context, s_ps.unity_d3d11_device);
	}
}

static int ps_create_bridge(void)
{
	if (!s_ps.own_device || !s_ps.unity_device) return 0;
	uint32_t w = s_ps.sc_width, h = s_ps.sc_height;
	if (w == 0 || h == 0) return 0;

	if (dxr_prov_get_single_pass()) {
		if (s_ps.bridge_own) return 1; // already created
		// SPI: one shared 2-slice array bridge (slice 0 = left, slice 1 = right).
		if (!ps_alloc_shared_tex(w, h, 2, &s_ps.bridge_own, &s_ps.bridge_unity,
		                         &s_ps.bridge_handle, "Bridge (SPI 2-slice array)"))
			return 0;
	} else {
		if (s_ps.bridge_own_eye[0]) return 1; // already created
		// MultiPass: two separate single-slice bridges, one per eye (one texture
		// per pass; textureArraySlice is SPI-only per the IUnityXRDisplay contract).
		for (int e = 0; e < 2; e++) {
			if (!ps_alloc_shared_tex(w, h, 1, &s_ps.bridge_own_eye[e], &s_ps.bridge_unity_eye[e],
			                         &s_ps.bridge_handle_eye[e],
			                         e == 0 ? "Bridge (MultiPass left)" : "Bridge (MultiPass right)"))
				return 0;
		}
	}

	// Cross-device SHARED fence: own_device creates it shared; Unity's device
	// opens the same fence. submit_frame signals it on Unity's queue (after the
	// render) and waits on it on the own queue (before the copy). Device-level and
	// size-independent, so a live-realloc (#172) keeps the existing fence and skips
	// this (recreating it would leak the old one and reset sync_val ordering).
	if (s_ps.unity_queue && !s_ps.shared_fence_own) {
		HRESULT hr2 = s_ps.own_device->CreateFence(0, D3D12_FENCE_FLAG_SHARED,
		        __uuidof(ID3D12Fence), (void **)&s_ps.shared_fence_own);
		if (SUCCEEDED(hr2)) {
			hr2 = s_ps.own_device->CreateSharedHandle(s_ps.shared_fence_own, NULL,
			        GENERIC_ALL, NULL, &s_ps.shared_fence_handle);
			if (SUCCEEDED(hr2) && s_ps.shared_fence_handle)
				s_ps.unity_device->OpenSharedHandle(s_ps.shared_fence_handle,
				        __uuidof(ID3D12Fence), (void **)&s_ps.shared_fence_unity);
		}
		ps_log("[DisplayXR-PROV] Shared sync fence: %s\n",
		       s_ps.shared_fence_unity ? "OK (cross-device render->copy sync)"
		                               : "FAILED (falling back to coarse sync)");
	} else {
		ps_log("[DisplayXR-PROV] No Unity queue → coarse sync only (possible black/tearing)\n");
	}
	return 1;
}
#endif // _WIN32 (D3D own-device / shared-bridge helpers)

// ============================================================================
// Live tile realloc (#172) — track the per-view target size on resize / split /
// mode change, like a real handle app. See dxr_prov_reconcile_size().
// ============================================================================

// Target awaits this many consecutive stable frames before we realloc. An
// interactive resize drag changes GetClientRect every frame; debouncing reallocs
// once the drag settles instead of thrashing the swapchain (the Leia weaver
// stutters during a resize drag regardless — this just avoids churn).
#define PS_REALLOC_DEBOUNCE 6

// Block the CPU until all GPU work touching the bridge/swapchain has retired, so
// they can be safely destroyed. Drains our own copy queue, then Unity's render
// queue via the shared fence (shared_fence_own mirrors unity_queue's signal).
static void ps_drain_gpu(void)
{
#if defined(__APPLE__)
	// Metal (#204): frame-level sync lands with the Metal backend; nothing to
	// drain in Phase 1 (the session never starts on macOS yet).
	return;
#elif defined(_WIN32)
	if (s_ps.own_queue && s_ps.own_fence && s_ps.own_fence_event) {
		s_ps.own_fence_value++;
		s_ps.own_queue->Signal(s_ps.own_fence, s_ps.own_fence_value);
		if (s_ps.own_fence->GetCompletedValue() < s_ps.own_fence_value) {
			s_ps.own_fence->SetEventOnCompletion(s_ps.own_fence_value, s_ps.own_fence_event);
			WaitForSingleObject(s_ps.own_fence_event, INFINITE);
		}
	}
	if (s_ps.unity_queue && s_ps.shared_fence_unity && s_ps.shared_fence_own) {
		s_ps.sync_val++;
		s_ps.unity_queue->Signal(s_ps.shared_fence_unity, s_ps.sync_val);
		if (s_ps.shared_fence_own->GetCompletedValue() < s_ps.sync_val) {
			HANDLE ev = CreateEvent(NULL, FALSE, FALSE, NULL);
			if (ev) {
				s_ps.shared_fence_own->SetEventOnCompletion(s_ps.sync_val, ev);
				WaitForSingleObject(ev, INFINITE);
				CloseHandle(ev);
			}
		}
	}
	// D3D11 editor bridge (#195): CPU-drain both contexts so the bridge textures can be
	// safely destroyed on a live realloc.
	if (s_ps.d3d11_bridge) {
		ps_d3d11_ctx_drain(s_ps.own_d3d11_context, s_ps.own_d3d11_device);
		ps_d3d11_ctx_drain(s_ps.unity_d3d11_context, s_ps.unity_d3d11_device);
	}
#else
	// Linux (#249): no cross-device drain — the Vulkan bridge orders Unity's
	// renders against the session-device copy itself (dxr_pvk_signal_unity_done).
	return;
#endif // _WIN32
}

// (#740 / editor resize crash, task 7) Deferred-release graveyard for the rewrapped
// bridge textures (ADR-001, realloc-scoped). Releasing the old bridges IMMEDIATELY
// lets the D3D12 allocator hand the replacement bridge the SAME pointer address while
// Unity's XRTextureManager is still processing the old texture's destroy
// ASYNCHRONOUSLY — its D3D12 state cache then treats the new resource as the old one
// (address-keyed), skips the initial COMMON→RENDER_TARGET transition, and clears a
// COMMON resource (debug layer id 538: "Expected RENDER_TARGET, Actual
// COMMON|PRESENT" on 'XR Texture [n]'), intermittently escalating to
// DXGI_ERROR_DEVICE_HUNG → device removed → editor fatal on interactive resizes.
// Holding the previous generation until AFTER the new bridges are allocated (and
// flushing it only then) makes address aliasing against live Unity state impossible.
// (Extra-zone tiles realloc through their own path and want the same treatment if
// zone-resize churn ever shows this signature.)
#ifdef _WIN32
#define PS_REALLOC_GRAVE_MAX 16
static IUnknown *s_realloc_grave[PS_REALLOC_GRAVE_MAX];
static int s_realloc_grave_count = 0;

static void ps_realloc_grave_flush(void)
{
	for (int i = 0; i < s_realloc_grave_count; i++)
		if (s_realloc_grave[i]) s_realloc_grave[i]->Release();
	s_realloc_grave_count = 0;
}

static void ps_realloc_grave_stash(IUnknown *p, IUnknown **stash, int *count)
{
	if (p == NULL) return;
	if (*count < PS_REALLOC_GRAVE_MAX) stash[(*count)++] = p;
	else p->Release(); // overflow safety — never expected (max 12 live bridges)
}

// (#747 bug 2 / XR_KHR_D3D12_enable swapchain-image state contract) Every copy into
// an ACQUIRED swapchain image must be bracketed with explicit transitions: the spec
// hands a color image to the app "with a resource state match with
// D3D12_RESOURCE_STATE_RENDER_TARGET" at xrWaitSwapchainImage success, and at
// xrReleaseSwapchainImage "the OpenXR runtime must interpret the image as" being back
// in RENDER_TARGET. Our copies previously relied on implicit COMMON promotion and
// released the image decayed to COMMON — the compositor's next RT-assuming barrier
// then mismatched (debug-layer id 527 on 'DXR.xr_swapchain_img[n]', runtime #747).
// Bracketing RT→COPY_DEST→RT is self-consistent from frame 2 on regardless of the
// image's creation state, and satisfies the release half the compositor depends on.
static void ps_sc_image_barrier(ID3D12GraphicsCommandList *cl, ID3D12Resource *img,
                                D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
	if (cl == NULL || img == NULL) return;
	D3D12_RESOURCE_BARRIER b = {};
	b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	b.Transition.pResource = img;
	b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	b.Transition.StateBefore = before;
	b.Transition.StateAfter = after;
	cl->ResourceBarrier(1, &b);
}
#endif // _WIN32

// Tear down the primary swapchain + its cross-device bridge(s) (NOT the shared
// fence or own device — those are size-independent), then recreate at the current
// target size via ps_create_swapchain (which recomputes size from the window/zone
// and re-pairs the bridge). Must run between frames with the GPU drained. Returns 1
// if the swapchain was recreated (caller drops+rewraps the Unity textures).
static int ps_recreate_primary_swapchain(void)
{
	ps_drain_gpu();

	// Release an acquired-but-unsubmitted image before destroying the swapchain.
	if (s_ps.image_acquired && s_ps.swapchain && s_ps.pfn_release_swapchain_image) {
		XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
		s_ps.pfn_release_swapchain_image(s_ps.swapchain, &ri);
	}
	s_ps.image_acquired = 0;

	if (s_ps.swapchain && s_ps.pfn_destroy_swapchain) s_ps.pfn_destroy_swapchain(s_ps.swapchain);
	s_ps.swapchain = XR_NULL_HANDLE;
	s_ps.swapchain_created = 0;
	s_ps.sc_image_count = 0;

#ifdef _WIN32
	// SPI bridge (2-slice) + MultiPass per-eye bridges. Keep the shared fence.
	// STASH instead of Release (see graveyard comment above): the NEW bridges must be
	// allocated while these are still alive so they can't reuse addresses Unity's
	// async texture-destroy still has D3D12 state cached for.
	IUnknown *stash[PS_REALLOC_GRAVE_MAX]; int stash_count = 0;
	ps_realloc_grave_stash(s_ps.bridge_unity, stash, &stash_count); s_ps.bridge_unity = NULL;
	if (s_ps.bridge_handle) { CloseHandle(s_ps.bridge_handle); s_ps.bridge_handle = NULL; }
	ps_realloc_grave_stash(s_ps.bridge_own, stash, &stash_count); s_ps.bridge_own = NULL;
	for (int e = 0; e < 2; e++) {
		ps_realloc_grave_stash(s_ps.bridge_unity_eye[e], stash, &stash_count); s_ps.bridge_unity_eye[e] = NULL;
		if (s_ps.bridge_handle_eye[e]) { CloseHandle(s_ps.bridge_handle_eye[e]); s_ps.bridge_handle_eye[e] = NULL; }
		ps_realloc_grave_stash(s_ps.bridge_own_eye[e], stash, &stash_count); s_ps.bridge_own_eye[e] = NULL;
	}
	// D3D11 size-dependent targets (#195): the SPI bridge (editor), the MultiPass per-eye
	// targets (editor shared + zero-copy plain) — stash + recreate. Keep own device +
	// shared fence (size-independent, like the D3D12 shared fence). The NT handles were
	// already closed inside ps_alloc_shared_tex_d3d11 (fields stay NULL).
	ps_realloc_grave_stash(s_ps.d3d11_bridge_unity, stash, &stash_count); s_ps.d3d11_bridge_unity = NULL;
	ps_realloc_grave_stash(s_ps.d3d11_bridge_own, stash, &stash_count);   s_ps.d3d11_bridge_own = NULL;
	for (int e = 0; e < 2; e++) {
		ps_realloc_grave_stash(s_ps.d3d11_bridge_unity_eye[e], stash, &stash_count); s_ps.d3d11_bridge_unity_eye[e] = NULL;
		ps_realloc_grave_stash(s_ps.d3d11_bridge_own_eye[e], stash, &stash_count);   s_ps.d3d11_bridge_own_eye[e] = NULL;
	}
#endif // _WIN32

	int ok = ps_create_swapchain(); // recomputes target size + re-pairs the bridge

#ifdef _WIN32
	// New bridges exist (their addresses are guaranteed distinct from the stash).
	// Now the generation from the PREVIOUS realloc can finally go, and this
	// generation takes its place until the next one.
	ps_realloc_grave_flush();
	for (int i = 0; i < stash_count; i++)
		s_realloc_grave[s_realloc_grave_count++] = stash[i];
#endif // _WIN32
	ps_log("[DisplayXR-PROV] realloc: primary swapchain+bridge recreated -> %ux%u (%s)\n",
	       s_ps.sc_width, s_ps.sc_height, ok ? "OK" : "FAILED");
	return ok;
}

// The primary swapchain's current target per-view size (matches ps_create_swapchain).
static void ps_primary_target_size(uint32_t *out_w, uint32_t *out_h)
{
	uint32_t w = 0, h = 0;
	if (ps_zone_active()) {
		ps_query_zone_rec_size();
		w = s_ps.zone_rec_w; h = s_ps.zone_rec_h;
	} else {
		uint32_t ww = 0, wh = 0; ps_window_size(&ww, &wh);
		float sx = 0.5f, sy = 0.5f; ps_active_view_scale(&sx, &sy);
		w = (uint32_t)(ww * sx); h = (uint32_t)(wh * sy);
	}
	*out_w = w; *out_h = h;
}

// Debounce + realloc the PRIMARY swapchain to its current target size. Returns 1 if
// it reallocated this call (caller drops+rewraps the primary Unity texture).
static int ps_reconcile_primary(void)
{
	uint32_t tw = 0, th = 0;
	ps_primary_target_size(&tw, &th);
	if (tw == 0 || th == 0) return 0;
	uint32_t tvc = ps_active_view_count();

	// A view-count change (2D↔3D mode switch) is a discrete user action → realloc
	// immediately (no debounce) so the submit count + tile size flip together (#172 P4).
	if (tvc != s_ps.sc_view_count) {
		s_ps.realloc_pending_w = 0; s_ps.realloc_pending_h = 0; s_ps.realloc_stable = 0;
		ps_log("[DisplayXR-PROV] realloc: submit views %u -> %u (mode switch, %ux%u -> %ux%u)\n",
		       s_ps.sc_view_count, tvc, s_ps.sc_width, s_ps.sc_height, tw, th);
		return ps_recreate_primary_swapchain();
	}
	if (tw == s_ps.sc_width && th == s_ps.sc_height) {
		s_ps.realloc_pending_w = 0; s_ps.realloc_pending_h = 0; s_ps.realloc_stable = 0;
		return 0; // already the right size
	}
	// Debounce: require the same new target for PS_REALLOC_DEBOUNCE frames.
	if (tw == s_ps.realloc_pending_w && th == s_ps.realloc_pending_h) {
		if (++s_ps.realloc_stable < PS_REALLOC_DEBOUNCE) return 0;
	} else {
		s_ps.realloc_pending_w = tw; s_ps.realloc_pending_h = th; s_ps.realloc_stable = 0;
		return 0;
	}
	s_ps.realloc_pending_w = 0; s_ps.realloc_pending_h = 0; s_ps.realloc_stable = 0;
	ps_log("[DisplayXR-PROV] realloc: primary target %ux%u -> %ux%u (settled)\n",
	       s_ps.sc_width, s_ps.sc_height, tw, th);
	return ps_recreate_primary_swapchain();
}

static void ps_reconcile_extra_zones(void); // defined with the zone helpers below

// Called at the top of the Unity frame (before the eye textures are wrapped).
// Reconciles the primary tile AND every extra 3D zone tile against their current
// target sizes (window×scaleXY / zone recommended view size), each debounced. Returns
// 1 if the PRIMARY reallocated (caller drops+rewraps the primary texture); extra-zone
// reallocs are signalled per-zone via dxr_prov_consume_zone_rewrap. Between frames only.
int dxr_prov_reconcile_size(void)
{
	if (!s_ps.running || !s_ps.session_ready || !s_ps.swapchain_created) return 0;
	if (s_ps.frame_begun) return 0; // never realloc between begin_frame and submit

#ifdef _WIN32
	// #225: re-poll the live tile size each reconcile so a 3D-window resize
	// flows through — ps_reconcile_primary below re-sizes the swapchain to the
	// new tile, and the app re-authors its zone from displayxr_get_overlay_size.
	if (displayxr_is_shell_mode())
		ps_query_tile_size();
#endif

	int primary_changed = ps_reconcile_primary();
	ps_reconcile_extra_zones();
	return primary_changed;
}

// ============================================================================
// Window-space UI (HUD) overlay layer (#67/#166)
// ============================================================================

// s_pending getter exported by the wsui module (displayxr_window_space_ui.cpp).
// C# DisplayXRWindowSpaceUI sets it via set_texture/set_layer regardless of which
// session is active, so the provider can drive its own window-space layer.
extern "C" int displayxr_window_space_ui_get_pending(void **out_tex, int *out_tex_w, int *out_tex_h,
                                                     float *out_x, float *out_y,
                                                     float *out_lw, float *out_lh, float *out_disp);

// s_pending getter exported by the Local2D module (displayxr_local2d.cpp). The Metal
// Local2D arm blits this Unity id<MTLTexture> straight into its overlay swapchain image
// (same device); the destination pixel rect comes from provider state (l2d_rect_*).
extern "C" int displayxr_local2d_get_pending(void **out_tex, int *out_w, int *out_h);

// Child-glue (#740, displayxr_win32.c): 1 when the dedicated weave window is a WS_CHILD
// of Unity's container; the parent-client-origin getter converts the glue's SCREEN rect
// to child coords for SetWindowPos.
extern "C" int displayxr_dedicated_is_childglue(void);
extern "C" int displayxr_dedicated_parent_client_origin(int *ox, int *oy);

// Create (or recreate) the wsui overlay swapchain + cross-device bridge sized to
// w×h. Format = B8G8R8A8_UNORM (87) to match Unity's URP wsui RT — CopyTextureRegion
// is invalid across formats and the runtime's DComp path turns a mismatch into a
// device-removal (#82 / runtime#216). Mirrors ps_create_bridge but arraySize=1.
static int ps_create_wsui(uint32_t w, uint32_t h)
{
	// Vulkan (#247) Phase 1 covers the PRIMARY stereo path only. The secondary
	// composition layers (wsui / Local2D / extra 3D zones) each need their own
	// external-memory bridge and are not wired yet — bail cleanly rather than fall
	// into the D3D12 arm below and dereference a NULL own_device.
	if (s_ps.graphics_api == DXR_GFX_VULKAN) {
		static int warned = 0;
		if (!warned) {
			warned = 1;
			ps_log("[DisplayXR-PROV] wsui: not supported on the Vulkan backend yet (#247 Phase 1 "
			       "is the primary stereo path only) — 2D UI layer inert\n");
		}
		return 0;
	}
#if defined(__APPLE__)
	// Metal (#206): an arraySize=1 overlay swapchain on Unity's device. No bridge —
	// submit blits the C#-registered Unity id<MTLTexture> straight in (same device).
	if (w == 0 || h == 0) return 0;
	if (s_ps.graphics_api != DXR_GFX_METAL || !s_ps.metal_queue) return 0;
	if (s_ps.wsui_swapchain_created && s_ps.wsui_registered_w == w && s_ps.wsui_registered_h == h)
		return 1; // already sized for this RT

	if (s_ps.wsui_swapchain && s_ps.pfn_destroy_swapchain) s_ps.pfn_destroy_swapchain(s_ps.wsui_swapchain);
	s_ps.wsui_swapchain = XR_NULL_HANDLE;
	s_ps.wsui_swapchain_created = 0;
	s_ps.wsui_image_acquired = 0;

	// Prefer BGRA8Unorm (80) to match Unity's B8G8R8A8_UNorm OverlayTexture so the
	// submit blit is same-format; fall back to the first enumerated format.
	uint32_t fmt_count = 0;
	s_ps.pfn_enumerate_swapchain_formats(s_ps.session, 0, &fmt_count, NULL);
	if (fmt_count == 0) { ps_log("[DisplayXR-PROV] wsui: no swapchain formats\n"); return 0; }
	int64_t formats[32];
	if (fmt_count > 32) fmt_count = 32;
	s_ps.pfn_enumerate_swapchain_formats(s_ps.session, fmt_count, &fmt_count, formats);
	int64_t format = formats[0];
	for (uint32_t i = 0; i < fmt_count; i++) { if (formats[i] == 80) { format = 80; break; } }

	XrSwapchainCreateInfo ci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
	ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
	                XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT |
	                XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
	ci.format = format;
	ci.sampleCount = 1;
	ci.width = w; ci.height = h;
	ci.faceCount = 1; ci.arraySize = 1; ci.mipCount = 1;
	if (XR_FAILED(s_ps.pfn_create_swapchain(s_ps.session, &ci, &s_ps.wsui_swapchain))) {
		ps_log("[DisplayXR-PROV] wsui: xrCreateSwapchain failed\n"); s_ps.wsui_swapchain = XR_NULL_HANDLE; return 0;
	}
	uint32_t count = 0;
	s_ps.pfn_enumerate_swapchain_images(s_ps.wsui_swapchain, 0, &count, NULL);
	if (count > PS_MAX_SWAPCHAIN_IMAGES) count = PS_MAX_SWAPCHAIN_IMAGES;
	for (uint32_t i = 0; i < count; i++) {
		s_ps.wsui_images_metal[i].type = XR_TYPE_SWAPCHAIN_IMAGE_METAL_KHR;
		s_ps.wsui_images_metal[i].next = NULL; s_ps.wsui_images_metal[i].texture = NULL;
	}
	if (XR_FAILED(s_ps.pfn_enumerate_swapchain_images(s_ps.wsui_swapchain, count, &count,
	        (XrSwapchainImageBaseHeader *)s_ps.wsui_images_metal))) {
		ps_log("[DisplayXR-PROV] wsui: enumerate images (Metal) failed\n"); return 0;
	}
	s_ps.wsui_w = w; s_ps.wsui_h = h; s_ps.wsui_format = format;
	s_ps.wsui_image_count = count;
	s_ps.wsui_registered_w = w; s_ps.wsui_registered_h = h;
	s_ps.wsui_swapchain_created = 1;
	ps_log("[DisplayXR-PROV] wsui: Metal swapchain %ux%u (%u imgs, fmt=%lld)\n",
	       w, h, count, (long long)format);
	return 1;
#elif defined(_WIN32)
	int is_d3d11 = (s_ps.graphics_api == DXR_GFX_D3D11);
	if (w == 0 || h == 0) return 0;
	if (is_d3d11 ? !s_ps.unity_d3d11_device : (!s_ps.own_device || !s_ps.unity_device)) return 0;
	if (s_ps.wsui_swapchain_created && s_ps.wsui_registered_w == w && s_ps.wsui_registered_h == h)
		return 1; // already sized for this RT

	// Tear down any previous (RT resized).
	if (s_ps.wsui_swapchain && s_ps.pfn_destroy_swapchain) s_ps.pfn_destroy_swapchain(s_ps.wsui_swapchain);
	if (s_ps.wsui_bridge_unity) { s_ps.wsui_bridge_unity->Release(); s_ps.wsui_bridge_unity = NULL; }
	if (s_ps.wsui_bridge_own)   { s_ps.wsui_bridge_own->Release();   s_ps.wsui_bridge_own = NULL; }
	if (s_ps.wsui_bridge_handle) { CloseHandle(s_ps.wsui_bridge_handle); s_ps.wsui_bridge_handle = NULL; }
	if (s_ps.wsui_unity_tex)    { s_ps.wsui_unity_tex->Release();    s_ps.wsui_unity_tex = NULL; }
	if (s_ps.wsui_unity_tex_own){ s_ps.wsui_unity_tex_own->Release(); s_ps.wsui_unity_tex_own = NULL; }
	s_ps.wsui_unity_tex_handle = NULL; // NT handle already closed in ps_alloc_shared_tex_d3d11
	s_ps.wsui_swapchain = XR_NULL_HANDLE;
	s_ps.wsui_swapchain_created = 0;
	s_ps.wsui_image_acquired = 0;

	// --- Overlay swapchain (arraySize=1, format 87 to match Unity URP wsui RT) ---
	uint32_t fmt_count = 0;
	s_ps.pfn_enumerate_swapchain_formats(s_ps.session, 0, &fmt_count, NULL);
	if (fmt_count == 0) { ps_log("[DisplayXR-PROV] wsui: no swapchain formats\n"); return 0; }
	int64_t formats[32];
	if (fmt_count > 32) fmt_count = 32;
	s_ps.pfn_enumerate_swapchain_formats(s_ps.session, fmt_count, &fmt_count, formats);
	int64_t format = formats[0];
	for (uint32_t i = 0; i < fmt_count; i++) { if (formats[i] == 87) { format = 87; break; } }

	XrSwapchainCreateInfo ci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
	ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
	                XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT |
	                XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
	ci.format = format;
	ci.sampleCount = 1;
	ci.width = w; ci.height = h;
	ci.faceCount = 1; ci.arraySize = 1; ci.mipCount = 1;
	if (XR_FAILED(s_ps.pfn_create_swapchain(s_ps.session, &ci, &s_ps.wsui_swapchain))) {
		ps_log("[DisplayXR-PROV] wsui: xrCreateSwapchain failed\n"); s_ps.wsui_swapchain = XR_NULL_HANDLE; return 0;
	}
	uint32_t count = 0;
	s_ps.pfn_enumerate_swapchain_images(s_ps.wsui_swapchain, 0, &count, NULL);
	if (count > PS_MAX_SWAPCHAIN_IMAGES) count = PS_MAX_SWAPCHAIN_IMAGES;
	if (is_d3d11) {
		for (uint32_t i = 0; i < count; i++) {
			s_ps.wsui_images_d3d11[i].type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR;
			s_ps.wsui_images_d3d11[i].next = NULL; s_ps.wsui_images_d3d11[i].texture = NULL;
		}
		if (XR_FAILED(s_ps.pfn_enumerate_swapchain_images(s_ps.wsui_swapchain, count, &count,
		        (XrSwapchainImageBaseHeader *)s_ps.wsui_images_d3d11))) {
			ps_log("[DisplayXR-PROV] wsui: enumerate images (D3D11) failed\n"); return 0;
		}
	} else {
		for (uint32_t i = 0; i < count; i++) {
			s_ps.wsui_images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR;
			s_ps.wsui_images[i].next = NULL; s_ps.wsui_images[i].texture = NULL;
		}
		if (XR_FAILED(s_ps.pfn_enumerate_swapchain_images(s_ps.wsui_swapchain, count, &count,
		        (XrSwapchainImageBaseHeader *)s_ps.wsui_images))) {
			ps_log("[DisplayXR-PROV] wsui: enumerate images failed\n"); return 0;
		}
	}
	s_ps.wsui_image_count = count;

	if (is_d3d11 && s_ps.d3d11_bridge) {
		// D3D11 editor bridge: an own-device shared BGRA8 texture opened on Unity's device
		// (the C# CopyTexture target); submit copies own->image on the own context.
		if (!ps_alloc_shared_tex_d3d11(w, h, 1, &s_ps.wsui_unity_tex_own, &s_ps.wsui_unity_tex,
		                               &s_ps.wsui_unity_tex_handle, 87, "wsui bridge (D3D11)")) return 0;
	} else if (is_d3d11) {
		// D3D11 zero-copy: a plain Unity-device BGRA8 texture as the C# CopyTexture target;
		// submit same-device-copies it into the acquired image (no bridge).
		s_ps.wsui_unity_tex = ps_alloc_unity_tex(w, h, 1, 87);
		if (!s_ps.wsui_unity_tex) { ps_log("[DisplayXR-PROV] wsui: Unity-device target alloc failed\n"); return 0; }
	} else {
		// --- Cross-device bridge (own_device shared BGRA8, opened on Unity's device) ---
		D3D12_HEAP_PROPERTIES heap = {}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
		D3D12_RESOURCE_DESC bd = {};
		bd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		bd.Width = w; bd.Height = h; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
		bd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		bd.SampleDesc.Count = 1;
		bd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
		HRESULT hr = s_ps.own_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &bd,
		        D3D12_RESOURCE_STATE_COMMON, NULL, __uuidof(ID3D12Resource), (void **)&s_ps.wsui_bridge_own);
		if (FAILED(hr)) { ps_log("[DisplayXR-PROV] wsui bridge create failed: 0x%08lx\n", hr); return 0; }
		hr = s_ps.own_device->CreateSharedHandle(s_ps.wsui_bridge_own, NULL, GENERIC_ALL, NULL, &s_ps.wsui_bridge_handle);
		if (FAILED(hr) || !s_ps.wsui_bridge_handle) { ps_log("[DisplayXR-PROV] wsui bridge share failed\n"); return 0; }
		hr = s_ps.unity_device->OpenSharedHandle(s_ps.wsui_bridge_handle, __uuidof(ID3D12Resource),
		        (void **)&s_ps.wsui_bridge_unity);
		if (FAILED(hr) || !s_ps.wsui_bridge_unity) { ps_log("[DisplayXR-PROV] wsui bridge open(Unity) failed\n"); return 0; }
	}

	s_ps.wsui_w = w; s_ps.wsui_h = h; s_ps.wsui_format = format;
	s_ps.wsui_registered_w = w; s_ps.wsui_registered_h = h;
	s_ps.wsui_swapchain_created = 1;
	ps_log("[DisplayXR-PROV] wsui: swapchain %ux%u (%u imgs, fmt=%lld) target=%p (%s)\n",
	       w, h, count, (long long)format,
	       is_d3d11 ? (void *)s_ps.wsui_unity_tex : (void *)s_ps.wsui_bridge_unity,
	       is_d3d11 ? "D3D11 zero-copy" : "D3D12 bridge");
	return 1;
#else
	// Linux (#249): the wsui composition layer is inert on the Vulkan backend.
	(void)w; (void)h;
	return 0;
#endif // _WIN32
}

// C# (DisplayXRWindowSpaceUI) calls this to get the Unity-device handle of the wsui
// bridge, then Graphics.CopyTexture's its canvas RT into it each frame. Lazily
// creates the swapchain+bridge sized to w×h. Returns the Unity-side ID3D12Resource*
// (or NULL). Declared DISPLAYXR_EXPORT in the header so it's exported for P/Invoke.
void dxr_prov_get_wsui_bridge(uint32_t w, uint32_t h,
                              void **out_ptr, uint32_t *out_w, uint32_t *out_h)
{
	if (out_ptr) *out_ptr = NULL;
	if (out_w) *out_w = 0;
	if (out_h) *out_h = 0;
	if (!s_ps.running || !s_ps.session_ready) return;
	if (!ps_create_wsui(w, h)) return;
#ifdef _WIN32
	if (out_ptr) *out_ptr = (s_ps.graphics_api == DXR_GFX_D3D11)
	                            ? (void *)s_ps.wsui_unity_tex : (void *)s_ps.wsui_bridge_unity;
	if (out_w) *out_w = s_ps.wsui_w;
	if (out_h) *out_h = s_ps.wsui_h;
#endif
}

// Per-frame: if a wsui texture is registered and the bridge is ready, copy
// wsui_bridge_own -> the acquired overlay swapchain image (own device) and fill
// out_layer. Returns 1 if the layer should be submitted, else 0. Called from
// dxr_prov_submit_frame AFTER the projection bridge copy (own_cmd_list is free and
// the shared-fence wait already ordered the own queue after Unity's writes).
static int ps_submit_wsui(XrCompositionLayerWindowSpaceDXR *out_layer)
{
	if (!out_layer) return 0;
	memset(out_layer, 0, sizeof(*out_layer));
#if defined(__APPLE__)
	// Metal (#206): blit the C#-registered Unity id<MTLTexture> into the acquired
	// overlay swapchain image (same device, session queue), then submit the layer.
	if (s_ps.graphics_api != DXR_GFX_METAL) return 0;
	void *tex = NULL; int tw = 0, th = 0;
	float lx = 0, ly = 0, lw = 0, lh = 0, ldisp = 0;
	if (!displayxr_window_space_ui_get_pending(&tex, &tw, &th, &lx, &ly, &lw, &lh, &ldisp))
		return 0; // no UI texture registered this frame
	if (!ps_create_wsui((uint32_t)tw, (uint32_t)th)) return 0;
	if (!s_ps.wsui_swapchain_created || s_ps.wsui_image_count == 0) return 0;

	uint32_t idx = 0;
	XrSwapchainImageAcquireInfo ai = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
	if (XR_FAILED(s_ps.pfn_acquire_swapchain_image(s_ps.wsui_swapchain, &ai, &idx)) ||
	    idx >= s_ps.wsui_image_count)
		return 0;
	XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
	wi.timeout = 1000000000;
	s_ps.pfn_wait_swapchain_image(s_ps.wsui_swapchain, &wi);

	// Same-device blit Unity UI texture -> acquired image on the session queue.
	// Ordered after Unity's OverlayCamera render by the frame's order_weave wait
	// CB (session-queue FIFO — order_weave is committed earlier in submit); the
	// compositor's weave at xrEndFrame is FIFO after this blit on the same queue.
	if (s_ps.wsui_images_metal[idx].texture)
		displayxr_metal_blit_textures(s_ps.metal_queue, tex, s_ps.wsui_images_metal[idx].texture);

	XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
	s_ps.pfn_release_swapchain_image(s_ps.wsui_swapchain, &ri);

	out_layer->type = XR_TYPE_COMPOSITION_LAYER_WINDOW_SPACE_DXR;
	out_layer->next = NULL;
	out_layer->layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
	out_layer->subImage.swapchain = s_ps.wsui_swapchain;
	out_layer->subImage.imageRect.offset = {0, 0};
	out_layer->subImage.imageRect.extent = {(int32_t)s_ps.wsui_w, (int32_t)s_ps.wsui_h};
	out_layer->subImage.imageArrayIndex = 0;
	out_layer->x = lx; out_layer->y = ly;
	out_layer->width = lw; out_layer->height = lh;
	out_layer->disparity = ldisp;
	return 1;
#elif defined(_WIN32)

	void *tex = NULL; int tw = 0, th = 0;
	float lx = 0, ly = 0, lw = 0, lh = 0, ldisp = 0;
	if (!displayxr_window_space_ui_get_pending(&tex, &tw, &th, &lx, &ly, &lw, &lh, &ldisp))
		return 0;
	int is_d3d11 = (s_ps.graphics_api == DXR_GFX_D3D11);
	if (!s_ps.wsui_swapchain_created || s_ps.wsui_image_count == 0 ||
	    (is_d3d11 ? !s_ps.wsui_unity_tex : !s_ps.wsui_bridge_own))
		return 0; // C# hasn't requested the target yet (no get_wsui_bridge call)

	// Acquire + wait an overlay swapchain image.
	uint32_t idx = 0;
	XrSwapchainImageAcquireInfo ai = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
	if (XR_FAILED(s_ps.pfn_acquire_swapchain_image(s_ps.wsui_swapchain, &ai, &idx)) ||
	    idx >= s_ps.wsui_image_count)
		return 0;
	XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
	wi.timeout = 1000000000;
	s_ps.pfn_wait_swapchain_image(s_ps.wsui_swapchain, &wi);

	if (is_d3d11 && s_ps.d3d11_bridge) {
		// D3D11 editor bridge: own-context copy own-side shared tex -> acquired image
		// (the frame's single fence Wait already ordered the own context after Unity).
		if (s_ps.wsui_images_d3d11[idx].texture && s_ps.wsui_unity_tex_own && s_ps.own_d3d11_context)
			s_ps.own_d3d11_context->CopyResource(s_ps.wsui_images_d3d11[idx].texture, s_ps.wsui_unity_tex_own);
	} else if (is_d3d11) {
		// D3D11 zero-copy: same-device copy Unity target -> acquired image (no bridge/fence).
		if (s_ps.wsui_images_d3d11[idx].texture && s_ps.wsui_unity_tex && s_ps.unity_d3d11_context)
			s_ps.unity_d3d11_context->CopyResource(s_ps.wsui_images_d3d11[idx].texture, s_ps.wsui_unity_tex);
	} else
	// own_device copy bridge -> swapchain image (single subresource).
	if (s_ps.wsui_images[idx].texture && s_ps.own_cmd_list) {
		s_ps.own_cmd_alloc->Reset();
		s_ps.own_cmd_list->Reset(s_ps.own_cmd_alloc, NULL);
		ps_sc_image_barrier(s_ps.own_cmd_list, s_ps.wsui_images[idx].texture,
		                    D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST);
		D3D12_TEXTURE_COPY_LOCATION dl = {};
		dl.pResource = s_ps.wsui_images[idx].texture;
		dl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dl.SubresourceIndex = 0;
		D3D12_TEXTURE_COPY_LOCATION sl = {};
		sl.pResource = s_ps.wsui_bridge_own;
		sl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; sl.SubresourceIndex = 0;
		s_ps.own_cmd_list->CopyTextureRegion(&dl, 0, 0, 0, &sl, NULL);
		ps_sc_image_barrier(s_ps.own_cmd_list, s_ps.wsui_images[idx].texture,
		                    D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
		s_ps.own_cmd_list->Close();
		ID3D12CommandList *lists[] = { s_ps.own_cmd_list };
		s_ps.own_queue->ExecuteCommandLists(1, lists);
		s_ps.own_fence_value++;
		s_ps.own_queue->Signal(s_ps.own_fence, s_ps.own_fence_value);
		if (s_ps.own_fence->GetCompletedValue() < s_ps.own_fence_value) {
			s_ps.own_fence->SetEventOnCompletion(s_ps.own_fence_value, s_ps.own_fence_event);
			WaitForSingleObject(s_ps.own_fence_event, INFINITE);
		}
	}

	XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
	s_ps.pfn_release_swapchain_image(s_ps.wsui_swapchain, &ri);

	out_layer->type = XR_TYPE_COMPOSITION_LAYER_WINDOW_SPACE_DXR;
	out_layer->next = NULL;
	out_layer->layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
	out_layer->subImage.swapchain = s_ps.wsui_swapchain;
	out_layer->subImage.imageRect.offset = {0, 0};
	out_layer->subImage.imageRect.extent = {(int32_t)s_ps.wsui_w, (int32_t)s_ps.wsui_h};
	out_layer->subImage.imageArrayIndex = 0;
	out_layer->x = lx; out_layer->y = ly;
	out_layer->width = lw; out_layer->height = lh;
	out_layer->disparity = ldisp;
	return 1;
#else
	// Linux (#249): wsui inert — nothing to submit.
	return 0;
#endif // _WIN32
}

// ============================================================================
// Local2D layer (#166 Phase B) — mirrors the wsui bridge, but XrCompositionLayer-
// Local2DEXT at a client-window PIXEL rect (the 2D band). BGRA8 to match Unity's URP RT.
// ============================================================================

static int ps_create_local2d(uint32_t w, uint32_t h)
{
	// See ps_create_wsui: Vulkan Phase 1 is the primary stereo path only (#247).
	if (s_ps.graphics_api == DXR_GFX_VULKAN) {
		static int warned = 0;
		if (!warned) {
			warned = 1;
			ps_log("[DisplayXR-PROV] local2d: not supported on the Vulkan backend yet "
			       "(#247 Phase 1) — 2D band layer inert\n");
		}
		return 0;
	}
#if defined(__APPLE__)
	// Metal (#206): an arraySize=1 overlay swapchain on Unity's device. No bridge —
	// submit blits the C#-registered Unity id<MTLTexture> straight in (same device).
	// Twin of the wsui Metal arm (ps_create_wsui); the only difference downstream is
	// a pixel rect at submit (XrCompositionLayerLocal2DDXR) rather than fractional.
	if (w == 0 || h == 0) return 0;
	if (s_ps.graphics_api != DXR_GFX_METAL || !s_ps.metal_queue) return 0;
	if (s_ps.l2d_swapchain_created && s_ps.l2d_registered_w == w && s_ps.l2d_registered_h == h)
		return 1; // already sized for this RT

	if (s_ps.l2d_swapchain && s_ps.pfn_destroy_swapchain) s_ps.pfn_destroy_swapchain(s_ps.l2d_swapchain);
	s_ps.l2d_swapchain = XR_NULL_HANDLE;
	s_ps.l2d_swapchain_created = 0;

	// Prefer BGRA8Unorm (80) to match Unity's B8G8R8A8_UNorm OverlayTexture so the
	// submit blit is same-format; fall back to the first enumerated format.
	uint32_t fmt_count = 0;
	s_ps.pfn_enumerate_swapchain_formats(s_ps.session, 0, &fmt_count, NULL);
	if (fmt_count == 0) { ps_log("[DisplayXR-PROV] local2d: no swapchain formats\n"); return 0; }
	int64_t formats[32];
	if (fmt_count > 32) fmt_count = 32;
	s_ps.pfn_enumerate_swapchain_formats(s_ps.session, fmt_count, &fmt_count, formats);
	int64_t format = formats[0];
	for (uint32_t i = 0; i < fmt_count; i++) { if (formats[i] == 80) { format = 80; break; } }

	XrSwapchainCreateInfo ci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
	ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
	                XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT |
	                XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
	ci.format = format;
	ci.sampleCount = 1; ci.width = w; ci.height = h;
	ci.faceCount = 1; ci.arraySize = 1; ci.mipCount = 1;
	if (XR_FAILED(s_ps.pfn_create_swapchain(s_ps.session, &ci, &s_ps.l2d_swapchain))) {
		ps_log("[DisplayXR-PROV] local2d: xrCreateSwapchain failed\n"); s_ps.l2d_swapchain = XR_NULL_HANDLE; return 0;
	}
	uint32_t count = 0;
	s_ps.pfn_enumerate_swapchain_images(s_ps.l2d_swapchain, 0, &count, NULL);
	if (count > PS_MAX_SWAPCHAIN_IMAGES) count = PS_MAX_SWAPCHAIN_IMAGES;
	for (uint32_t i = 0; i < count; i++) {
		s_ps.l2d_images_metal[i].type = XR_TYPE_SWAPCHAIN_IMAGE_METAL_KHR;
		s_ps.l2d_images_metal[i].next = NULL; s_ps.l2d_images_metal[i].texture = NULL;
	}
	if (XR_FAILED(s_ps.pfn_enumerate_swapchain_images(s_ps.l2d_swapchain, count, &count,
	        (XrSwapchainImageBaseHeader *)s_ps.l2d_images_metal))) {
		ps_log("[DisplayXR-PROV] local2d: enumerate images (Metal) failed\n"); return 0;
	}
	s_ps.l2d_w = w; s_ps.l2d_h = h; s_ps.l2d_format = format;
	s_ps.l2d_image_count = count;
	s_ps.l2d_registered_w = w; s_ps.l2d_registered_h = h;
	s_ps.l2d_swapchain_created = 1;
	ps_log("[DisplayXR-PROV] local2d: Metal swapchain %ux%u (%u imgs, fmt=%lld)\n",
	       w, h, count, (long long)format);
	return 1;
#elif defined(_WIN32)
	int is_d3d11 = (s_ps.graphics_api == DXR_GFX_D3D11);
	if (w == 0 || h == 0) return 0;
	if (is_d3d11 ? !s_ps.unity_d3d11_device : (!s_ps.own_device || !s_ps.unity_device)) return 0;
	if (s_ps.l2d_swapchain_created && s_ps.l2d_registered_w == w && s_ps.l2d_registered_h == h)
		return 1;
	if (s_ps.l2d_swapchain && s_ps.pfn_destroy_swapchain) s_ps.pfn_destroy_swapchain(s_ps.l2d_swapchain);
	if (s_ps.l2d_bridge_unity)  { s_ps.l2d_bridge_unity->Release();  s_ps.l2d_bridge_unity = NULL; }
	if (s_ps.l2d_bridge_own)    { s_ps.l2d_bridge_own->Release();    s_ps.l2d_bridge_own = NULL; }
	if (s_ps.l2d_bridge_handle) { CloseHandle(s_ps.l2d_bridge_handle); s_ps.l2d_bridge_handle = NULL; }
	if (s_ps.l2d_unity_tex)     { s_ps.l2d_unity_tex->Release();     s_ps.l2d_unity_tex = NULL; }
	if (s_ps.l2d_unity_tex_own) { s_ps.l2d_unity_tex_own->Release(); s_ps.l2d_unity_tex_own = NULL; }
	s_ps.l2d_unity_tex_handle = NULL; // NT handle already closed in ps_alloc_shared_tex_d3d11
	s_ps.l2d_swapchain = XR_NULL_HANDLE;
	s_ps.l2d_swapchain_created = 0;

	uint32_t fmt_count = 0;
	s_ps.pfn_enumerate_swapchain_formats(s_ps.session, 0, &fmt_count, NULL);
	if (fmt_count == 0) { ps_log("[DisplayXR-PROV] local2d: no swapchain formats\n"); return 0; }
	int64_t formats[32];
	if (fmt_count > 32) fmt_count = 32;
	s_ps.pfn_enumerate_swapchain_formats(s_ps.session, fmt_count, &fmt_count, formats);
	int64_t format = formats[0];
	for (uint32_t i = 0; i < fmt_count; i++) { if (formats[i] == 87) { format = 87; break; } }

	XrSwapchainCreateInfo ci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
	ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
	                XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT |
	                XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
	ci.format = format;
	ci.sampleCount = 1; ci.width = w; ci.height = h;
	ci.faceCount = 1; ci.arraySize = 1; ci.mipCount = 1;
	if (XR_FAILED(s_ps.pfn_create_swapchain(s_ps.session, &ci, &s_ps.l2d_swapchain))) {
		ps_log("[DisplayXR-PROV] local2d: xrCreateSwapchain failed\n"); s_ps.l2d_swapchain = XR_NULL_HANDLE; return 0;
	}
	uint32_t count = 0;
	s_ps.pfn_enumerate_swapchain_images(s_ps.l2d_swapchain, 0, &count, NULL);
	if (count > PS_MAX_SWAPCHAIN_IMAGES) count = PS_MAX_SWAPCHAIN_IMAGES;
	if (is_d3d11) {
		for (uint32_t i = 0; i < count; i++) {
			s_ps.l2d_images_d3d11[i].type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR;
			s_ps.l2d_images_d3d11[i].next = NULL; s_ps.l2d_images_d3d11[i].texture = NULL;
		}
		if (XR_FAILED(s_ps.pfn_enumerate_swapchain_images(s_ps.l2d_swapchain, count, &count,
		        (XrSwapchainImageBaseHeader *)s_ps.l2d_images_d3d11))) {
			ps_log("[DisplayXR-PROV] local2d: enumerate images (D3D11) failed\n"); return 0;
		}
	} else {
		for (uint32_t i = 0; i < count; i++) {
			s_ps.l2d_images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR;
			s_ps.l2d_images[i].next = NULL; s_ps.l2d_images[i].texture = NULL;
		}
		if (XR_FAILED(s_ps.pfn_enumerate_swapchain_images(s_ps.l2d_swapchain, count, &count,
		        (XrSwapchainImageBaseHeader *)s_ps.l2d_images))) {
			ps_log("[DisplayXR-PROV] local2d: enumerate images failed\n"); return 0;
		}
	}
	s_ps.l2d_image_count = count;

	if (is_d3d11 && s_ps.d3d11_bridge) {
		if (!ps_alloc_shared_tex_d3d11(w, h, 1, &s_ps.l2d_unity_tex_own, &s_ps.l2d_unity_tex,
		                               &s_ps.l2d_unity_tex_handle, 87, "local2d bridge (D3D11)")) return 0;
	} else if (is_d3d11) {
		s_ps.l2d_unity_tex = ps_alloc_unity_tex(w, h, 1, 87);
		if (!s_ps.l2d_unity_tex) { ps_log("[DisplayXR-PROV] local2d: Unity-device target alloc failed\n"); return 0; }
	} else {
		D3D12_HEAP_PROPERTIES heap = {}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
		D3D12_RESOURCE_DESC bd = {};
		bd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		bd.Width = w; bd.Height = h; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
		bd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		bd.SampleDesc.Count = 1;
		bd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
		HRESULT hr = s_ps.own_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &bd,
		        D3D12_RESOURCE_STATE_COMMON, NULL, __uuidof(ID3D12Resource), (void **)&s_ps.l2d_bridge_own);
		if (FAILED(hr)) { ps_log("[DisplayXR-PROV] local2d bridge create failed: 0x%08lx\n", hr); return 0; }
		hr = s_ps.own_device->CreateSharedHandle(s_ps.l2d_bridge_own, NULL, GENERIC_ALL, NULL, &s_ps.l2d_bridge_handle);
		if (FAILED(hr) || !s_ps.l2d_bridge_handle) { ps_log("[DisplayXR-PROV] local2d bridge share failed\n"); return 0; }
		hr = s_ps.unity_device->OpenSharedHandle(s_ps.l2d_bridge_handle, __uuidof(ID3D12Resource),
		        (void **)&s_ps.l2d_bridge_unity);
		if (FAILED(hr) || !s_ps.l2d_bridge_unity) { ps_log("[DisplayXR-PROV] local2d bridge open(Unity) failed\n"); return 0; }
	}

	s_ps.l2d_w = w; s_ps.l2d_h = h; s_ps.l2d_format = format;
	s_ps.l2d_registered_w = w; s_ps.l2d_registered_h = h;
	s_ps.l2d_swapchain_created = 1;
	ps_log("[DisplayXR-PROV] local2d: swapchain %ux%u (%u imgs) target=%p (%s)\n",
	       w, h, count,
	       is_d3d11 ? (void *)s_ps.l2d_unity_tex : (void *)s_ps.l2d_bridge_unity,
	       is_d3d11 ? "D3D11 zero-copy" : "D3D12 bridge");
	return 1;
#else
	// Linux (#249): the Local2D composition layer is inert on the Vulkan backend.
	(void)w; (void)h;
	return 0;
#endif // _WIN32
}

void dxr_prov_get_local2d_bridge(uint32_t w, uint32_t h,
                                 void **out_ptr, uint32_t *out_w, uint32_t *out_h)
{
	if (out_ptr) *out_ptr = NULL;
	if (out_w) *out_w = 0;
	if (out_h) *out_h = 0;
	if (!s_ps.running || !s_ps.session_ready) return;
	if (!ps_create_local2d(w, h)) return;
#ifdef _WIN32
	if (out_ptr) *out_ptr = (s_ps.graphics_api == DXR_GFX_D3D11)
	                            ? (void *)s_ps.l2d_unity_tex : (void *)s_ps.l2d_bridge_unity;
	if (out_w) *out_w = s_ps.l2d_w;
	if (out_h) *out_h = s_ps.l2d_h;
#endif
}

void dxr_prov_set_local2d_rect(int32_t x, int32_t y, int32_t w, int32_t h)
{
	if (w <= 0 || h <= 0) { s_ps.l2d_rect_set = 0; return; }
	s_ps.l2d_rect_x = x; s_ps.l2d_rect_y = y; s_ps.l2d_rect_w = w; s_ps.l2d_rect_h = h;
	s_ps.l2d_rect_set = 1;
}

// Per-frame: copy the Local2D bridge into its overlay swapchain image and fill the
// layer (XrCompositionLayerLocal2DDXR, dest = client-window pixel rect). Returns 1
// if the layer should be submitted. Called from submit after the projection copy.
static int ps_submit_local2d(XrCompositionLayerLocal2DDXR *out_layer)
{
	if (!out_layer) return 0;
	memset(out_layer, 0, sizeof(*out_layer));
#if defined(__APPLE__)
	// Metal (#206): blit the C#-registered Unity id<MTLTexture> into the acquired
	// Local2D swapchain image (same device, session queue), then submit the layer at
	// the client-window PIXEL rect. Twin of ps_submit_wsui; the rect comes from
	// provider state (dxr_prov_set_local2d_rect), not the pending reader.
	if (s_ps.graphics_api != DXR_GFX_METAL) return 0;
	if (!s_ps.l2d_rect_set) return 0; // no destination rect pushed this frame
	void *tex = NULL; int tw = 0, th = 0;
	if (!displayxr_local2d_get_pending(&tex, &tw, &th))
		return 0; // no Local2D texture registered
	if (!ps_create_local2d((uint32_t)tw, (uint32_t)th)) return 0;
	if (!s_ps.l2d_swapchain_created || s_ps.l2d_image_count == 0) return 0;

	uint32_t idx = 0;
	XrSwapchainImageAcquireInfo ai = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
	if (XR_FAILED(s_ps.pfn_acquire_swapchain_image(s_ps.l2d_swapchain, &ai, &idx)) ||
	    idx >= s_ps.l2d_image_count)
		return 0;
	XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
	wi.timeout = 1000000000;
	s_ps.pfn_wait_swapchain_image(s_ps.l2d_swapchain, &wi);

	// Same-device blit Unity Local2D texture -> acquired image on the session queue.
	// Ordered after Unity's overlay-camera render by the frame's order_weave wait CB
	// (session-queue FIFO); the compositor's weave at xrEndFrame is FIFO after it.
	if (s_ps.l2d_images_metal[idx].texture)
		displayxr_metal_blit_textures(s_ps.metal_queue, tex, s_ps.l2d_images_metal[idx].texture);

	XrSwapchainImageReleaseInfo ri_mtl = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
	s_ps.pfn_release_swapchain_image(s_ps.l2d_swapchain, &ri_mtl);

	out_layer->type = XR_TYPE_COMPOSITION_LAYER_LOCAL_2D_DXR;
	out_layer->next = NULL;
	// Unity Canvas is straight (unpremultiplied) alpha — flag it so the runtime
	// doesn't double-darken (matches the D3D arm below).
	out_layer->layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
	                        XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
	out_layer->subImage.swapchain = s_ps.l2d_swapchain;
	out_layer->subImage.imageRect.offset = {0, 0};
	out_layer->subImage.imageRect.extent = {(int32_t)s_ps.l2d_w, (int32_t)s_ps.l2d_h};
	out_layer->subImage.imageArrayIndex = 0;
	// Destination is a client-window PIXEL rect (XrRect2Di), NOT fractional.
	out_layer->rect.offset = {s_ps.l2d_rect_x, s_ps.l2d_rect_y};
	out_layer->rect.extent = {s_ps.l2d_rect_w, s_ps.l2d_rect_h};
	return 1;
#elif defined(_WIN32)
	int is_d3d11 = (s_ps.graphics_api == DXR_GFX_D3D11);
	if (!s_ps.l2d_rect_set || !s_ps.l2d_swapchain_created || s_ps.l2d_image_count == 0 ||
	    (is_d3d11 ? !s_ps.l2d_unity_tex : !s_ps.l2d_bridge_own))
		return 0;

	uint32_t idx = 0;
	XrSwapchainImageAcquireInfo ai = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
	if (XR_FAILED(s_ps.pfn_acquire_swapchain_image(s_ps.l2d_swapchain, &ai, &idx)) ||
	    idx >= s_ps.l2d_image_count)
		return 0;
	XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
	wi.timeout = 1000000000;
	s_ps.pfn_wait_swapchain_image(s_ps.l2d_swapchain, &wi);

	if (is_d3d11 && s_ps.d3d11_bridge) {
		// D3D11 editor bridge: own-context copy own-side shared tex -> acquired image.
		if (s_ps.l2d_images_d3d11[idx].texture && s_ps.l2d_unity_tex_own && s_ps.own_d3d11_context)
			s_ps.own_d3d11_context->CopyResource(s_ps.l2d_images_d3d11[idx].texture, s_ps.l2d_unity_tex_own);
	} else if (is_d3d11) {
		// D3D11 zero-copy: same-device copy Unity target -> acquired image (no bridge/fence).
		if (s_ps.l2d_images_d3d11[idx].texture && s_ps.l2d_unity_tex && s_ps.unity_d3d11_context)
			s_ps.unity_d3d11_context->CopyResource(s_ps.l2d_images_d3d11[idx].texture, s_ps.l2d_unity_tex);
	} else
	if (s_ps.l2d_images[idx].texture && s_ps.own_cmd_list) {
		s_ps.own_cmd_alloc->Reset();
		s_ps.own_cmd_list->Reset(s_ps.own_cmd_alloc, NULL);
		ps_sc_image_barrier(s_ps.own_cmd_list, s_ps.l2d_images[idx].texture,
		                    D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST);
		D3D12_TEXTURE_COPY_LOCATION dl = {};
		dl.pResource = s_ps.l2d_images[idx].texture;
		dl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dl.SubresourceIndex = 0;
		D3D12_TEXTURE_COPY_LOCATION sl = {};
		sl.pResource = s_ps.l2d_bridge_own;
		sl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; sl.SubresourceIndex = 0;
		s_ps.own_cmd_list->CopyTextureRegion(&dl, 0, 0, 0, &sl, NULL);
		ps_sc_image_barrier(s_ps.own_cmd_list, s_ps.l2d_images[idx].texture,
		                    D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
		s_ps.own_cmd_list->Close();
		ID3D12CommandList *lists[] = { s_ps.own_cmd_list };
		s_ps.own_queue->ExecuteCommandLists(1, lists);
		s_ps.own_fence_value++;
		s_ps.own_queue->Signal(s_ps.own_fence, s_ps.own_fence_value);
		if (s_ps.own_fence->GetCompletedValue() < s_ps.own_fence_value) {
			s_ps.own_fence->SetEventOnCompletion(s_ps.own_fence_value, s_ps.own_fence_event);
			WaitForSingleObject(s_ps.own_fence_event, INFINITE);
		}
	}

	XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
	s_ps.pfn_release_swapchain_image(s_ps.l2d_swapchain, &ri);

	out_layer->type = XR_TYPE_COMPOSITION_LAYER_LOCAL_2D_DXR;
	out_layer->next = NULL;
	// Unity Canvas is straight (unpremultiplied) alpha — flag it so the runtime
	// doesn't double-darken (matches the hook path's Local2D + avatar bubble).
	out_layer->layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
	                        XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
	out_layer->subImage.swapchain = s_ps.l2d_swapchain;
	out_layer->subImage.imageRect.offset = {0, 0};
	out_layer->subImage.imageRect.extent = {(int32_t)s_ps.l2d_w, (int32_t)s_ps.l2d_h};
	out_layer->subImage.imageArrayIndex = 0;
	out_layer->rect.offset = {s_ps.l2d_rect_x, s_ps.l2d_rect_y};
	out_layer->rect.extent = {s_ps.l2d_rect_w, s_ps.l2d_rect_h};
	return 1;
#else
	// Linux (#249): Local2D inert — nothing to submit.
	return 0;
#endif // _WIN32
}

// ============================================================================
// Extra 3D zones (#166 Phase B2) — per-zone swapchain + bridge + locate + copy,
// mirroring the primary zone path. The primary zone stays on the top-level fields.
// ============================================================================

static void ps_query_extra_zone_rec(ProviderExtraZone *z)
{
	z->rec_w = z->rect_w > 0 ? (uint32_t)z->rect_w : 0;
	z->rec_h = z->rect_h > 0 ? (uint32_t)z->rect_h : 0;
	if (s_ps.pfn_get_zone_view_size && z->valid && s_ps.session != XR_NULL_HANDLE) {
		XrRect2Di rect = {{z->rect_x, z->rect_y}, {z->rect_w, z->rect_h}};
		XrExtent2Di rec = {0, 0};
		if (XR_SUCCEEDED(s_ps.pfn_get_zone_view_size(s_ps.session, &rect, &rec)) && rec.width > 0 && rec.height > 0) {
			z->rec_w = (uint32_t)rec.width; z->rec_h = (uint32_t)rec.height;
		}
	}
}

static int ps_create_extra_zone(ProviderExtraZone *z)
{
	// See ps_create_wsui: Vulkan Phase 1 is the primary stereo path only (#247).
	// The app-authored PRIMARY 3D zone still works (it only sizes the main
	// swapchain); it is the EXTRA zones' own swapchains + bridges that are missing.
	if (s_ps.graphics_api == DXR_GFX_VULKAN) {
		static int warned = 0;
		if (!warned) {
			warned = 1;
			ps_log("[DisplayXR-PROV] extra zones: not supported on the Vulkan backend yet "
			       "(#247 Phase 1) — multi-zone layers inert\n");
		}
		return 0;
	}
#if defined(__APPLE__)
	// Metal (#206): zero-copy extra zone — an arraySize=2 swapchain whose per-eye slice
	// views Unity renders straight into (no bridge, no blit). Mirrors the primary Metal
	// swapchain arm in ps_create_swapchain. PopulateNextFrameDesc rotates to acquired_index.
	if (z->swapchain_created) return 1;
	if (!z->valid || !s_ps.session_ready) return 0;
	if (s_ps.graphics_api != DXR_GFX_METAL || !s_ps.metal_queue) return 0;
	ps_query_extra_zone_rec(z);
	uint32_t w = z->rec_w, h = z->rec_h;
	if (w == 0 || h == 0) return 0;

	uint32_t fmt_count = 0;
	s_ps.pfn_enumerate_swapchain_formats(s_ps.session, 0, &fmt_count, NULL);
	if (fmt_count == 0) return 0;
	int64_t formats[32]; if (fmt_count > 32) fmt_count = 32;
	s_ps.pfn_enumerate_swapchain_formats(s_ps.session, fmt_count, &fmt_count, formats);
	int64_t format = formats[0];
	for (uint32_t i = 0; i < fmt_count; i++) { if (formats[i] == 70) { format = 70; break; } if (formats[i] == 80) format = 80; }

	XrSwapchainCreateInfo ci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
	ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
	ci.format = format; ci.sampleCount = 1; ci.width = w; ci.height = h;
	ci.faceCount = 1; ci.arraySize = 2; ci.mipCount = 1;
	if (XR_FAILED(s_ps.pfn_create_swapchain(s_ps.session, &ci, &z->swapchain))) { z->swapchain = XR_NULL_HANDLE; return 0; }
	z->sc_width = w; z->sc_height = h; z->sc_format = format;
	uint32_t count = 0;
	s_ps.pfn_enumerate_swapchain_images(z->swapchain, 0, &count, NULL);
	if (count > PS_MAX_SWAPCHAIN_IMAGES) count = PS_MAX_SWAPCHAIN_IMAGES;
	z->sc_image_count = count;
	for (uint32_t i = 0; i < count; i++) { z->sc_images_metal[i].type = XR_TYPE_SWAPCHAIN_IMAGE_METAL_KHR; z->sc_images_metal[i].next = NULL; z->sc_images_metal[i].texture = NULL; }
	if (XR_FAILED(s_ps.pfn_enumerate_swapchain_images(z->swapchain, count, &count, (XrSwapchainImageBaseHeader *)z->sc_images_metal))) return 0;

	// Zero-copy slice views (id<MTLTexture> view per image×eye). The glue parks retired
	// generations in its graveyard (ADR-001), so recreate just re-enumerates.
	for (uint32_t i = 0; i < count; i++) {
		for (uint32_t e = 0; e < 2; e++) {
			z->eye_view_metal[i][e] = dxr_prov_metal_slice_view(z->sc_images_metal[i].texture, e);
			if (!z->eye_view_metal[i][e]) { ps_log("[DisplayXR-PROV] extra zone id=%u: Metal slice view [%u][%u] failed\n", z->zone_id, i, e); return 0; }
		}
	}
	z->swapchain_created = 1;
	ps_log("[DisplayXR-PROV] extra zone id=%u: Metal swapchain %ux%u (%u imgs, fmt=%lld, zero-copy slice views)\n",
	       z->zone_id, w, h, count, (long long)format);
	return 1;
#elif defined(_WIN32)
	if (z->swapchain_created) return 1;
	if (!z->valid || !s_ps.session_ready) return 0;
	ps_query_extra_zone_rec(z);
	uint32_t w = z->rec_w, h = z->rec_h;
	if (w == 0 || h == 0) return 0;

	uint32_t fmt_count = 0;
	s_ps.pfn_enumerate_swapchain_formats(s_ps.session, 0, &fmt_count, NULL);
	if (fmt_count == 0) return 0;
	int64_t formats[32]; if (fmt_count > 32) fmt_count = 32;
	s_ps.pfn_enumerate_swapchain_formats(s_ps.session, fmt_count, &fmt_count, formats);
	int64_t format = formats[0];
	for (uint32_t i = 0; i < fmt_count; i++) { if (formats[i] == 28) { format = 28; break; } if (formats[i] == 87) format = 87; }

	XrSwapchainCreateInfo ci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
	ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
	ci.format = format; ci.sampleCount = 1; ci.width = w; ci.height = h;
	ci.faceCount = 1; ci.arraySize = 2; ci.mipCount = 1;
	if (XR_FAILED(s_ps.pfn_create_swapchain(s_ps.session, &ci, &z->swapchain))) { z->swapchain = XR_NULL_HANDLE; return 0; }
	z->sc_width = w; z->sc_height = h; z->sc_format = format;
	int is_d3d11 = (s_ps.graphics_api == DXR_GFX_D3D11);
	uint32_t count = 0;
	s_ps.pfn_enumerate_swapchain_images(z->swapchain, 0, &count, NULL);
	if (count > PS_MAX_SWAPCHAIN_IMAGES) count = PS_MAX_SWAPCHAIN_IMAGES;
	z->sc_image_count = count;
	if (is_d3d11) {
		for (uint32_t i = 0; i < count; i++) { z->sc_images_d3d11[i].type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR; z->sc_images_d3d11[i].next = NULL; z->sc_images_d3d11[i].texture = NULL; }
		if (XR_FAILED(s_ps.pfn_enumerate_swapchain_images(z->swapchain, count, &count, (XrSwapchainImageBaseHeader *)z->sc_images_d3d11))) return 0;
	} else {
		for (uint32_t i = 0; i < count; i++) { z->sc_images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR; z->sc_images[i].next = NULL; z->sc_images[i].texture = NULL; }
		if (XR_FAILED(s_ps.pfn_enumerate_swapchain_images(z->swapchain, count, &count, (XrSwapchainImageBaseHeader *)z->sc_images))) return 0;
	}

	// Bridge(s). ps_alloc_shared_tex reads s_ps.sc_format for the D3D12 format; the
	// extra zone picks the SAME format as the primary, so this matches. D3D11 is always
	// SPI (gated in session_start) → editor bridge uses an own-device 2-slice shared
	// target + own-context copy; player zero-copy uses a plain Unity-device 2-slice target.
	int sp = dxr_prov_get_single_pass();
	if (is_d3d11 && s_ps.d3d11_bridge) {
		if (!ps_alloc_shared_tex_d3d11(w, h, 2, &z->unity_tex_own, &z->unity_tex,
		                               &z->unity_tex_handle, format, "extra-zone bridge (D3D11)")) return 0;
	} else if (is_d3d11) {
		z->unity_tex = ps_alloc_unity_tex(w, h, 2, format);
		if (!z->unity_tex) { ps_log("[DisplayXR-PROV] extra zone id=%u: Unity-device target alloc failed\n", z->zone_id); return 0; }
	} else if (sp) {
		if (!ps_alloc_shared_tex(w, h, 2, &z->bridge_own, &z->bridge_unity, &z->bridge_handle, "extra-zone bridge (SPI)")) return 0;
	} else {
		if (!ps_alloc_shared_tex(w, h, 1, &z->bridge_own_eye[0], &z->bridge_unity_eye[0], &z->bridge_handle_eye[0], "extra-zone bridge L")) return 0;
		if (!ps_alloc_shared_tex(w, h, 1, &z->bridge_own_eye[1], &z->bridge_unity_eye[1], &z->bridge_handle_eye[1], "extra-zone bridge R")) return 0;
	}
	z->swapchain_created = 1;
	ps_log("[DisplayXR-PROV] extra zone id=%u: swapchain %ux%u + %s (sp=%d)\n", z->zone_id, w, h,
	       !is_d3d11 ? "bridge(s)"
	           : (s_ps.d3d11_bridge ? "own-device shared target (D3D11 editor bridge)"
	                                : "Unity-device target (D3D11 zero-copy)"), sp);
	return 1;
#else
	// Linux (#249): extra 3D display zones are inert on the Vulkan backend.
	return 0;
#endif // _WIN32
}

// Create all pending extra-zone swapchains/bridges (called at session-ready + lazily).
static void ps_create_extra_zones(void)
{
	for (uint32_t i = 0; i < s_ps.extra_zone_count; i++)
		if (s_ps.extra_zones[i].valid && !s_ps.extra_zones[i].swapchain_created)
			ps_create_extra_zone(&s_ps.extra_zones[i]);
}

// Live tile realloc (#172) for an extra zone: drain, release+destroy the zone's
// swapchain+bridge(s), recreate at the current recommended view size, and set the
// rewrap latch so the Unity side re-wraps this zone's texture(s). Runs between frames.
static int ps_recreate_extra_zone(ProviderExtraZone *z)
{
	ps_drain_gpu();
	if (z->image_acquired && z->swapchain && s_ps.pfn_release_swapchain_image) {
		XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
		s_ps.pfn_release_swapchain_image(z->swapchain, &ri);
	}
	z->image_acquired = 0;
	if (z->swapchain && s_ps.pfn_destroy_swapchain) s_ps.pfn_destroy_swapchain(z->swapchain);
	z->swapchain = XR_NULL_HANDLE;
	z->swapchain_created = 0;
	z->sc_image_count = 0;
#ifdef _WIN32
	if (z->bridge_unity) { z->bridge_unity->Release(); z->bridge_unity = NULL; }
	if (z->bridge_handle) { CloseHandle(z->bridge_handle); z->bridge_handle = NULL; }
	if (z->bridge_own)   { z->bridge_own->Release();   z->bridge_own = NULL; }
	if (z->unity_tex)    { z->unity_tex->Release();    z->unity_tex = NULL; }
	if (z->unity_tex_own){ z->unity_tex_own->Release(); z->unity_tex_own = NULL; } // D3D11 editor bridge
	z->unity_tex_handle = NULL; // NT handle already closed in ps_alloc_shared_tex_d3d11
	for (int e = 0; e < 2; e++) {
		if (z->bridge_unity_eye[e]) { z->bridge_unity_eye[e]->Release(); z->bridge_unity_eye[e] = NULL; }
		if (z->bridge_handle_eye[e]) { CloseHandle(z->bridge_handle_eye[e]); z->bridge_handle_eye[e] = NULL; }
		if (z->bridge_own_eye[e])   { z->bridge_own_eye[e]->Release();   z->bridge_own_eye[e] = NULL; }
	}
#else
	// Metal: no bridges to release. The retired slice views are parked in the glue's
	// graveyard by the next dxr_prov_metal_slice_view call (ADR-001 deferred destruction);
	// zero them so ps_create_extra_zone repopulates cleanly.
	for (uint32_t i = 0; i < PS_MAX_SWAPCHAIN_IMAGES; i++)
		for (uint32_t e = 0; e < 2; e++) z->eye_view_metal[i][e] = NULL;
#endif
	int ok = ps_create_extra_zone(z);
	z->needs_rewrap = 1;
	ps_log("[DisplayXR-PROV] realloc: extra zone id=%u swapchain+bridge recreated -> %ux%u (%s)\n",
	       z->zone_id, z->sc_width, z->sc_height, ok ? "OK" : "FAILED");
	return ok;
}

// Reconcile every extra zone's tile against its current recommended view size,
// debounced. Runs inside dxr_prov_reconcile_size after the primary. Recreated zones
// set needs_rewrap (consumed by dxr_prov_consume_zone_rewrap).
static void ps_reconcile_extra_zones(void)
{
	for (uint32_t i = 0; i < s_ps.extra_zone_count; i++) {
		ProviderExtraZone *z = &s_ps.extra_zones[i];
		if (!z->valid || !z->swapchain_created) continue;
		ps_query_extra_zone_rec(z);
		uint32_t tw = z->rec_w, th = z->rec_h;
		if (tw == 0 || th == 0) continue;
		if (tw == z->sc_width && th == z->sc_height) {
			z->realloc_pending_w = 0; z->realloc_pending_h = 0; z->realloc_stable = 0;
			continue;
		}
		if (tw == z->realloc_pending_w && th == z->realloc_pending_h) {
			if (++z->realloc_stable < PS_REALLOC_DEBOUNCE) continue;
		} else {
			z->realloc_pending_w = tw; z->realloc_pending_h = th; z->realloc_stable = 0;
			continue;
		}
		z->realloc_pending_w = 0; z->realloc_pending_h = 0; z->realloc_stable = 0;
		ps_log("[DisplayXR-PROV] realloc: extra zone id=%u target %ux%u -> %ux%u (settled)\n",
		       z->zone_id, z->sc_width, z->sc_height, tw, th);
		ps_recreate_extra_zone(z);
	}
}

// Consume the rewrap latch for extra zone `index` (0-based). Returns 1 (and clears)
// if that zone was just reallocated and its Unity texture(s) must be re-wrapped.
int dxr_prov_consume_zone_rewrap(uint32_t index)
{
	if (index >= PS_MAX_ZONES - 1) return 0;
	ProviderExtraZone *z = &s_ps.extra_zones[index];
	if (!z->needs_rewrap) return 0;
	z->needs_rewrap = 0;
	return 1;
}

// Locate each extra zone (zone-scoped, XrDisplayZoneDXR chained) → z->views, and
// acquire its swapchain image. Mirrors the primary locate in dxr_prov_begin_frame.
static void ps_locate_extra_zones(XrTime display_time)
{
	for (uint32_t i = 0; i < s_ps.extra_zone_count; i++) {
		ProviderExtraZone *z = &s_ps.extra_zones[i];
		z->view_count = 0;
		if (!z->valid) continue;
		if (!z->swapchain_created) ps_create_extra_zone(z);
		if (s_ps.local_space == XR_NULL_HANDLE || !s_ps.pfn_locate_views) continue;

		XrViewLocateInfo li = {XR_TYPE_VIEW_LOCATE_INFO};
		li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
		li.displayTime = display_time; li.space = s_ps.local_space;
		XrView views[PS_MAX_VIEWS]; for (int k = 0; k < PS_MAX_VIEWS; k++) views[k] = {XR_TYPE_VIEW};
		XrViewState vstate = {XR_TYPE_VIEW_STATE};
		XrDisplayRigDXR display_rig = {XR_TYPE_DISPLAY_RIG_DXR};
		XrCameraRigDXR  camera_rig  = {XR_TYPE_CAMERA_RIG_DXR};
		XrDisplayZoneDXR zone = {XR_TYPE_DISPLAY_ZONE_DXR};
		XrPosef rig_pose = s_ps.display_pose_set ? s_ps.display_pose : XrPosef{{0, 0, 0, 1}, {0, 0, 0}};
		if (s_ps.has_view_rig) {
			if (s_ps.camera_centric) {
				camera_rig.pose = rig_pose;
				camera_rig.ipdFactor = s_ps.tunables_set ? s_ps.ipd_factor : 1.0f;
				camera_rig.parallaxFactor = s_ps.tunables_set ? s_ps.parallax_factor : 1.0f;
				camera_rig.convergenceDiopters = s_ps.inv_convergence_distance;
				camera_rig.verticalFov = 2.0f * atanf(s_ps.fov_override);
				camera_rig.metersToVirtual = 1.0f;
				li.next = &camera_rig;
			} else {
				float vdh = s_ps.display_info.is_valid && s_ps.display_info.height_m > 0.0f ? s_ps.display_info.height_m : 0.2f;
				display_rig.pose = rig_pose;
				display_rig.virtualDisplayHeight = (s_ps.tunables_set && s_ps.virtual_display_height > 0.0f) ? s_ps.virtual_display_height : vdh;
				display_rig.ipdFactor = s_ps.tunables_set ? s_ps.ipd_factor : 1.0f;
				display_rig.parallaxFactor = s_ps.tunables_set ? s_ps.parallax_factor : 1.0f;
				display_rig.perspectiveFactor = s_ps.tunables_set ? s_ps.perspective_factor : 1.0f;
				li.next = &display_rig;
			}
			zone.zoneId = z->zone_id;
			zone.rect.offset = {z->rect_x, z->rect_y};
			zone.rect.extent = {z->rect_w, z->rect_h};
			zone.next = li.next; li.next = &zone;
		}
		uint32_t vcount = 0;
		if (XR_SUCCEEDED(s_ps.pfn_locate_views(s_ps.session, &li, &vstate, PS_MAX_VIEWS, &vcount, views)) && vcount >= 1) {
			uint32_t n = vcount < 2 ? vcount : 2;
			for (uint32_t k = 0; k < n; k++) {
				z->views[k].position[0] = views[k].pose.position.x;
				z->views[k].position[1] = views[k].pose.position.y;
				z->views[k].position[2] = views[k].pose.position.z;
				z->views[k].orientation[0] = views[k].pose.orientation.x;
				z->views[k].orientation[1] = views[k].pose.orientation.y;
				z->views[k].orientation[2] = views[k].pose.orientation.z;
				z->views[k].orientation[3] = views[k].pose.orientation.w;
				z->views[k].fov[0] = views[k].fov.angleLeft;
				z->views[k].fov[1] = views[k].fov.angleRight;
				z->views[k].fov[2] = views[k].fov.angleUp;
				z->views[k].fov[3] = views[k].fov.angleDown;
				z->views[k].valid = s_ps.has_view_rig ? 1 : 0;
			}
			if (n == 1) { z->views[1] = z->views[0]; n = 2; }
			z->view_count = n;
		}
		if (z->swapchain_created && !z->image_acquired) {
			uint32_t idx = 0; XrSwapchainImageAcquireInfo ai = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
			if (XR_SUCCEEDED(s_ps.pfn_acquire_swapchain_image(z->swapchain, &ai, &idx))) {
				XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO}; wi.timeout = 1000000000;
				s_ps.pfn_wait_swapchain_image(z->swapchain, &wi);
				z->image_acquired = 1; z->acquired_index = idx;
			}
		}
	}
}

// Copy an extra zone's bridge → its acquired swapchain image + release (mirrors the
// primary copy). Cross-device fence-synced against Unity's render.
static void ps_copy_extra_zone(ProviderExtraZone *z)
{
	if (!z->swapchain_created || !z->image_acquired || z->acquired_index >= z->sc_image_count) return;
#if defined(__APPLE__)
	// Metal zone copy lands with Phase 4 (#206); release the acquired image so the
	// swapchain doesn't wedge if a zone was ever created.
	XrSwapchainImageReleaseInfo ri_mac = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
	s_ps.pfn_release_swapchain_image(z->swapchain, &ri_mac);
	z->image_acquired = 0;
	return;
#elif defined(_WIN32)
	if (s_ps.graphics_api == DXR_GFX_D3D11) {
		// CopyResource copies both 2-slice array layers, then release. EDITOR bridge: own
		// context copies the own-side shared target (the frame's fence Wait already ordered
		// the own context after Unity). PLAYER zero-copy: same-device copy the Unity target.
		if (s_ps.d3d11_bridge) {
			if (z->sc_images_d3d11[z->acquired_index].texture && z->unity_tex_own && s_ps.own_d3d11_context)
				s_ps.own_d3d11_context->CopyResource(z->sc_images_d3d11[z->acquired_index].texture, z->unity_tex_own);
		} else {
			if (z->sc_images_d3d11[z->acquired_index].texture && z->unity_tex && s_ps.unity_d3d11_context)
				s_ps.unity_d3d11_context->CopyResource(z->sc_images_d3d11[z->acquired_index].texture, z->unity_tex);
		}
		XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
		s_ps.pfn_release_swapchain_image(z->swapchain, &ri);
		z->image_acquired = 0;
		return;
	}
	int sp = dxr_prov_get_single_pass();
	ID3D12Resource *dst = z->sc_images[z->acquired_index].texture;
	ID3D12Resource *copy_src = sp ? z->bridge_own : z->bridge_own_eye[0];
	if (dst && copy_src && s_ps.own_cmd_list) {
		s_ps.own_cmd_alloc->Reset();
		s_ps.own_cmd_list->Reset(s_ps.own_cmd_alloc, NULL);
		ps_sc_image_barrier(s_ps.own_cmd_list, dst,
		                    D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST);
		for (UINT slice = 0; slice < 2; slice++) {
			D3D12_TEXTURE_COPY_LOCATION dl = {}; dl.pResource = dst; dl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dl.SubresourceIndex = slice;
			D3D12_TEXTURE_COPY_LOCATION sl = {}; sl.pResource = sp ? z->bridge_own : z->bridge_own_eye[slice]; sl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; sl.SubresourceIndex = sp ? slice : 0;
			s_ps.own_cmd_list->CopyTextureRegion(&dl, 0, 0, 0, &sl, NULL);
		}
		ps_sc_image_barrier(s_ps.own_cmd_list, dst,
		                    D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
		s_ps.own_cmd_list->Close();
		if (s_ps.shared_fence_unity && s_ps.shared_fence_own && s_ps.unity_queue) {
			s_ps.sync_val++;
			s_ps.unity_queue->Signal(s_ps.shared_fence_unity, s_ps.sync_val);
			s_ps.own_queue->Wait(s_ps.shared_fence_own, s_ps.sync_val);
		}
		ID3D12CommandList *lists[] = { s_ps.own_cmd_list };
		s_ps.own_queue->ExecuteCommandLists(1, lists);
		s_ps.own_fence_value++;
		s_ps.own_queue->Signal(s_ps.own_fence, s_ps.own_fence_value);
		if (s_ps.own_fence->GetCompletedValue() < s_ps.own_fence_value) {
			s_ps.own_fence->SetEventOnCompletion(s_ps.own_fence_value, s_ps.own_fence_event);
			WaitForSingleObject(s_ps.own_fence_event, INFINITE);
		}
	}
	XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
	s_ps.pfn_release_swapchain_image(z->swapchain, &ri);
	z->image_acquired = 0;
#else
	// Linux (#249): extra 3D zones inert — no acquired image to copy or release.
	return;
#endif // _WIN32
}

// Getters for the display-provider (Unity texture wrap + render passes).
void *dxr_prov_get_extra_zone_bridge(uint32_t ei, uint32_t *w, uint32_t *h)
{
	if (ei >= PS_MAX_ZONES - 1) return NULL;
	ProviderExtraZone *z = &s_ps.extra_zones[ei];
	if (w) *w = z->sc_width; if (h) *h = z->sc_height;
#ifdef _WIN32
	// D3D11 (#195): the Unity side wraps the 2-slice Unity-device target (plain in zero-copy,
	// the Unity-opened side of the shared bridge in editor bridge mode — same pointer).
	if (s_ps.graphics_api == DXR_GFX_D3D11) return (void *)z->unity_tex;
	return (void *)z->bridge_unity;
#else
	return NULL; // Metal zones land in Phase 4 (#206)
#endif
}

void *dxr_prov_get_extra_zone_bridge_eye(uint32_t ei, uint32_t eye, uint32_t *w, uint32_t *h)
{
	if (ei >= PS_MAX_ZONES - 1 || eye > 1) return NULL;
	ProviderExtraZone *z = &s_ps.extra_zones[ei];
	if (w) *w = z->sc_width; if (h) *h = z->sc_height;
#ifdef _WIN32
	return (void *)z->bridge_unity_eye[eye];
#else
	return NULL; // Metal zones land in Phase 4 (#206)
#endif
}

void dxr_prov_get_extra_zone_view(uint32_t ei, uint32_t eye, DxrProvView *out_view)
{
	if (!out_view) return;
	if (ei >= PS_MAX_ZONES - 1 || eye > 1) { memset(out_view, 0, sizeof(*out_view)); return; }
	*out_view = s_ps.extra_zones[ei].views[eye];
}

#ifdef __APPLE__
// Metal zero-copy extra-zone getters (display-provider TU). Cousins of the primary's
// dxr_prov_get_metal_eye_view — the display provider wraps each image×eye slice view and
// PopulateNextFrameDesc rotates to the zone's acquired image index each frame.
extern "C" void *dxr_prov_get_extra_zone_metal_eye_view(uint32_t ei, uint32_t img, uint32_t eye)
{
	if (ei >= PS_MAX_ZONES - 1 || img >= PS_MAX_SWAPCHAIN_IMAGES || eye > 1) return NULL;
	return s_ps.extra_zones[ei].eye_view_metal[img][eye];
}
// The whole arraySize=2 image (SPI wraps it as one 2-slice Unity texture, like the primary).
extern "C" void *dxr_prov_get_extra_zone_metal_image(uint32_t ei, uint32_t img)
{
	if (ei >= PS_MAX_ZONES - 1 || img >= PS_MAX_SWAPCHAIN_IMAGES) return NULL;
	return s_ps.extra_zones[ei].sc_images_metal[img].texture;
}
extern "C" uint32_t dxr_prov_get_extra_zone_image_count(uint32_t ei)
{
	if (ei >= PS_MAX_ZONES - 1) return 0;
	return s_ps.extra_zones[ei].sc_image_count;
}
extern "C" uint32_t dxr_prov_get_extra_zone_acquired_index(uint32_t ei)
{
	if (ei >= PS_MAX_ZONES - 1) return 0;
	return s_ps.extra_zones[ei].acquired_index;
}
#endif // __APPLE__

// ============================================================================
// Lifecycle
// ============================================================================

// ============================================================================
// Weave-to-texture PROBE (experiment/provider-weave-to-texture)
//
// Env-gated diagnostic (DISPLAYXR_PROV_TEXTURE_PROBE=1). Binds the session in
// TEXTURE MODE: passes a shared texture HANDLE on XrWin32WindowBindingCreateInfoDXR
// so the runtime weaves the FINAL output into an app-provided texture instead of
// presenting to the overlay window (windowHandle stays set for weaver position
// tracking, mirroring test_apps/texture/cube_zones_texture_*). After a warmup gate
// we read that texture back to a .bmp — proof the PLAIN (non-zones) projection
// weave-to-texture is correct before building the GameView mirror-blit on top.
//
// Fully additive/reversible: with the env var unset every path is skipped and the
// session binds exactly as before (windowHandle only). Windows/D3D-only TU.
// NOTE: in texture mode the runtime does NOT present, so the overlay/dedicated
// window stays blank while the probe runs — the .bmp is the success signal.
// ============================================================================
static int              s_probe_enabled = 0;    // texture mode active (dxr_prov_set_texture_mode / env)
static int              s_probe_readback = 0;   // .bmp readback armed — DISPLAYXR_PROV_TEXTURE_PROBE only
#ifdef _WIN32
static ID3D11Texture2D *s_probe_tex11   = NULL;  // shared texture (D3D11 bind, own device)
static ID3D12Resource  *s_probe_tex12   = NULL;  // shared texture (D3D12 bind)
static HANDLE           s_probe_handle  = NULL;  // shared HANDLE handed to the runtime
#endif
static unsigned         s_probe_frames  = 0;     // submit counter (dump gate)
static int              s_probe_dumped  = 0;     // one-shot readback done
static const unsigned   kProbeDumpFrame = 150;   // warmup gate (matches ref app)
// Initial GameView render rect, stashed from C# BEFORE session_start so the forced
// full-window zone (and thus the rendered tile size + the runtime's woven region) is
// born at the panel's native resolution. See dxr_prov_set_initial_gameview_rect docs.
static int s_init_gv_x = 0, s_init_gv_y = 0, s_init_gv_w = 0, s_init_gv_h = 0;
// GameView panel physical px, captured each frame from info.mirrorRtDesc (the
// authoritative Game-view render size — logical-vs-physical unambiguous, unlike the
// C# GetMainGameViewTargetSize path). The per-frame pump converges the forced zone to
// this so the compositor canvas == render viewport pixel-exact (Phase 1, #727 follow-up).
// Written by dxr_prov_set_panel_px / dxr_prov_set_panel_rect, read by
// dxr_prov_converge_gameview_zone. x/y = the pane's SCREEN position (zone-glue
// arrangement, #740/#742: the weave window is born at the MONITOR origin covering the
// panel and never moves; the ZONE rect carries the pane's true screen offset — the
// desktop-avatar-proven contract for placing woven content at a screen sub-rect).
// INT32_MIN = position not published (legacy window-glue arrangement, zone at 0,0).
static int s_gv_panel_w = 0, s_gv_panel_h = 0;
static int s_gv_panel_x = INT32_MIN, s_gv_panel_y = INT32_MIN;

// The APP-AUTHORED 3D zone, kept separate from the live zone (s_ps.zone_*). Recorded
// ONLY by the app-facing dxr_prov_set_3d_zone_rect(); the texture-mode forced zone and
// the GameView converge write the live zone through ps_apply_3d_zone_rect() and never
// touch this. Without the split, converge fed its own output back through the app
// entry point, so after one converge the app's original rect was gone and converge
// could no longer tell "app wants a sub-window band" from "provider default full
// window" — it paired the app's OFFSET with the pane's EXTENT and produced a zone that
// overran the window bottom (desktop-avatar: (0,284) 1728x576 -> (0,284) 1728x860 in an
// 860-tall pane). Keep this authoritative and derive the live zone from it.
static int s_app_zone_x = 0, s_app_zone_y = 0, s_app_zone_w = 0, s_app_zone_h = 0;
static int s_app_zone_valid = 0;

// Same treatment for the other APP-AUTHORED, "set once before the session starts"
// requests that used to live only in s_ps and so were lost on every session restart
// (dock<->undock): the transparent-background opt-in came back OPAQUE, and multi-zone
// apps lost every extra zone. These mirrors are file statics — they survive the memset —
// and are re-applied at session start next to the primary zone.
static int s_app_transparent_requested = 0;
static ProviderExtraZone s_app_extra_zones[PS_MAX_ZONES - 1];
static uint32_t s_app_extra_zone_count = 0;
// Per-zone cosmetic edge feather radius (unity#238, runtime#800 spec v3):
// [0] = primary zone, [1..] = extra zones. APP-side state (not in s_ps, so it
// survives the session-stop memset like s_app_extra_zones); read at submit,
// where >0 chains XrDisplayZoneFeatherDXR on the zone. 0 = hard (default).
static float s_app_zone_feather[PS_MAX_ZONES] = {};
// GameView mirror (Task (a)): the woven shared texture opened on UNITY'S device so
// the display-provider can wrap it via CreateTexture and mirror-blit it into the
// editor Game window. Opened lazily on first request (graphics thread, session ready).
#ifdef _WIN32
static ID3D11Texture2D *s_probe_tex_unity = NULL;
static ID3D12Resource  *s_probe_tex12_unity = NULL; // D3D12 Unity-device view of the woven shared tex
#endif
static uint32_t         s_probe_woven_w = 0, s_probe_woven_h = 0;

// Editor GameView weave-to-texture is the DEFAULT (v2.8.0+): C# passes the enable via
// dxr_prov_set_texture_mode BEFORE session start — 1 in editor Play Mode (unless
// DISPLAYXR_PROV_EXTERNAL_WINDOW=1 opts out), 0 in a built player. C# is the single source
// of truth for "am I the editor", so a player structurally can NEVER bind texture mode
// (it never calls the setter with 1). The DISPLAYXR_PROV_TEXTURE_PROBE env var remains a
// standalone-diagnostic fallback (forces texture mode on if the setter was never called)
// and SEPARATELY arms the .bmp readback (s_probe_readback).
static int s_texture_mode_forced = -1; // -1 unset (fall back to env), 0 off, 1 on
static int ps_texture_mode(void)
{
	if (s_texture_mode_forced >= 0) return s_texture_mode_forced;
	const char *e = getenv("DISPLAYXR_PROV_TEXTURE_PROBE");
	return (e && *e && *e != '0') ? 1 : 0;
}
void dxr_prov_set_texture_mode(int enable)
{
	s_texture_mode_forced = enable ? 1 : 0;
	ps_log("[DisplayXR-PROV] texture mode set: %s\n",
	       enable ? "ON (editor GameView weave)" : "off (external window)");
}
int dxr_prov_texture_mode_active(void) { return ps_texture_mode(); }

// Bind mode within the editor GameView feature (#740 hybrid): the feature (s_probe_enabled)
// binds in TEXTURE mode when DOCKED (weave into a shared texture → mirror-blit into the
// Game tab; the SR weaver resolves Unity's container which Unity presents → snap rides
// Unity's present; the DP phase_off cancels the container→pane offset) and in PRESENT
// mode when UNDOCKED (the Game view is its own floating top-level → runtime presents the
// woven stereo into our dedicated top-level window over it: GA_ROOT==self, correct anchor +
// snap, zero correction, no mirror seam). Set from C# via dxr_prov_set_present_mode BEFORE
// session start (dock-state-driven); env DISPLAYXR_PROV_PRESENT_MODE forces it for testing.
static int s_bind_present_override = -1; // -1 unset, 0 texture, 1 present
static int ps_bind_present(void)
{
	if (s_bind_present_override >= 0) return s_bind_present_override;
	const char *e = getenv("DISPLAYXR_PROV_PRESENT_MODE");
	return (e && *e && *e != '0') ? 1 : 0;
}
void dxr_prov_set_present_mode(int enable)
{
	s_bind_present_override = enable ? 1 : 0;
	ps_log("[DisplayXR-PROV] bind mode set: %s\n", enable ? "PRESENT (undocked)" : "TEXTURE (docked)");
}
int dxr_prov_get_present_mode(void) { return ps_bind_present(); }

// (#740 stereo unswap) The docked texture path weaves the two views in the OPPOSITE order
// vs maximized/floating — an exact ~half-lens-pitch swap that is invariant to all window
// geometry (measured: three docked in-container-X of 573/393/253 give identical interlace
// phase; only the maximized STATE differs, by ~half a period). Root cause is runtime/SDK-
// side (the maximized weave path), tracked on #740. As a STEREO-ONLY stopgap the plugin can
// submit the two views into the opposite swapchain slots, which for a 2-view interlace
// exactly cancels a view-order flip (== a half-pitch phase shift; the two are identical
// output for N=2). Does NOT generalise to N>2 (quilt) — that needs the runtime phase fix.
// Set from C# per (re)start based on dock state (docked-non-maximized => swap); env
// DISPLAYXR_PROV_VIEW_SWAP forces it for testing. Survives session stop — re-set per start.
static int s_view_swap_override = -1; // -1 unset, 0 off, 1 swap
int dxr_prov_view_swap(void)
{
	if (s_view_swap_override >= 0) return s_view_swap_override;
	const char *e = getenv("DISPLAYXR_PROV_VIEW_SWAP");
	return (e && *e && *e != '0') ? 1 : 0;
}
void dxr_prov_set_view_swap(int enable)
{
	s_view_swap_override = enable < 0 ? -1 : (enable ? 1 : 0);
	ps_log("[DisplayXR-PROV] view swap: %s\n", enable > 0 ? "ON (docked stereo unswap)" : "off");
}

// Readback path: DISPLAYXR_PROV_TEXTURE_PROBE may carry a literal path (any value
// other than "1"); otherwise default under %TEMP%.
// (macOS: the readback / shared-texture / woven-mirror helpers below are Windows/D3D-only —
// guarded out; dxr_prov_get_woven_unity_texture keeps a NULL macOS stub after #endif.)
#ifdef _WIN32
static void ps_probe_path(char *out, size_t n)
{
	const char *e = getenv("DISPLAYXR_PROV_TEXTURE_PROBE");
	if (e && *e && strcmp(e, "1") != 0) { _snprintf_s(out, n, _TRUNCATE, "%s", e); return; }
	const char *tmp = getenv("TEMP"); if (!tmp || !*tmp) tmp = ".";
	_snprintf_s(out, n, _TRUNCATE, "%s\\displayxr_prov_texture_readback.bmp", tmp);
}

// Minimal uncompressed 24-bit BMP (bottom-up, BGR). src is B8G8R8A8_UNORM.
static void ps_write_bmp_bgra(const char *path, uint32_t w, uint32_t h,
                              const uint8_t *src, size_t rowPitch)
{
	if (!path || !*path || !src || w == 0 || h == 0) return;
	const uint32_t rowBytes = w * 3;
	const uint32_t pad = (4 - (rowBytes & 3)) & 3;
	const uint32_t stride = rowBytes + pad;
	const uint32_t imgSize = stride * h;
	const uint32_t fileSize = 54 + imgSize;
	uint8_t hdr[54] = {0};
	hdr[0] = 'B'; hdr[1] = 'M';
	hdr[2] = (uint8_t)fileSize; hdr[3] = (uint8_t)(fileSize >> 8);
	hdr[4] = (uint8_t)(fileSize >> 16); hdr[5] = (uint8_t)(fileSize >> 24);
	hdr[10] = 54;                          // pixel-data offset
	hdr[14] = 40;                          // DIB header size
	hdr[18] = (uint8_t)w; hdr[19] = (uint8_t)(w >> 8);
	hdr[20] = (uint8_t)(w >> 16); hdr[21] = (uint8_t)(w >> 24);
	hdr[22] = (uint8_t)h; hdr[23] = (uint8_t)(h >> 8);
	hdr[24] = (uint8_t)(h >> 16); hdr[25] = (uint8_t)(h >> 24);
	hdr[26] = 1;                           // planes
	hdr[28] = 24;                          // bpp
	hdr[34] = (uint8_t)imgSize; hdr[35] = (uint8_t)(imgSize >> 8);
	hdr[36] = (uint8_t)(imgSize >> 16); hdr[37] = (uint8_t)(imgSize >> 24);
	FILE *f = NULL; fopen_s(&f, path, "wb");
	if (!f) { ps_log("[DisplayXR-PROV] PROBE: fopen failed for %s\n", path); return; }
	fwrite(hdr, 1, 54, f);
	uint8_t *row = (uint8_t *)malloc(stride);
	if (row) {
		memset(row, 0, stride);
		for (int32_t y = (int32_t)h - 1; y >= 0; y--) {     // bottom-up
			const uint8_t *s = src + (size_t)y * rowPitch;  // B,G,R,A
			for (uint32_t x = 0; x < w; x++) {
				row[x * 3 + 0] = s[x * 4 + 0]; // B
				row[x * 3 + 1] = s[x * 4 + 1]; // G
				row[x * 3 + 2] = s[x * 4 + 2]; // R
			}
			fwrite(row, 1, stride, f);
		}
		free(row);
	}
	fclose(f);
	ps_log("[DisplayXR-PROV] PROBE: wrote readback %ux%u -> %s\n", w, h, path);
}

// Create a shared D3D11 texture (legacy MISC_SHARED → GetSharedHandle). Returns
// the share HANDLE, or NULL. Stores the texture in s_probe_tex11.
static HANDLE ps_probe_create_tex_d3d11(ID3D11Device *dev, uint32_t w, uint32_t h)
{
	if (!dev || w == 0 || h == 0) return NULL;
	D3D11_TEXTURE2D_DESC td = {};
	td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
	td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
	td.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
	HRESULT hr = dev->CreateTexture2D(&td, NULL, &s_probe_tex11);
	if (FAILED(hr) || !s_probe_tex11) {
		ps_log("[DisplayXR-PROV] PROBE: D3D11 CreateTexture2D failed 0x%08X\n", hr);
		return NULL;
	}
	IDXGIResource *dxgi = NULL;
	hr = s_probe_tex11->QueryInterface(__uuidof(IDXGIResource), (void **)&dxgi);
	if (FAILED(hr) || !dxgi) {
		ps_log("[DisplayXR-PROV] PROBE: QI IDXGIResource failed 0x%08X\n", hr);
		return NULL;
	}
	HANDLE sh = NULL;
	hr = dxgi->GetSharedHandle(&sh);
	dxgi->Release();
	if (FAILED(hr) || !sh) {
		ps_log("[DisplayXR-PROV] PROBE: GetSharedHandle failed 0x%08X\n", hr);
		return NULL;
	}
	ps_log("[DisplayXR-PROV] PROBE: D3D11 shared texture %ux%u handle=%p\n", w, h, sh);
	return sh;
}

// Create a shared D3D12 texture (HEAP_FLAG_SHARED → CreateSharedHandle NT handle).
static HANDLE ps_probe_create_tex_d3d12(ID3D12Device *dev, uint32_t w, uint32_t h)
{
	if (!dev || w == 0 || h == 0) return NULL;
	D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
	D3D12_RESOURCE_DESC rd = {};
	rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	rd.Width = w; rd.Height = h; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
	rd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	rd.SampleDesc.Count = 1;
	rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
	HRESULT hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED, &rd,
	        D3D12_RESOURCE_STATE_COMMON, NULL, __uuidof(ID3D12Resource), (void **)&s_probe_tex12);
	if (FAILED(hr) || !s_probe_tex12) {
		ps_log("[DisplayXR-PROV] PROBE: D3D12 CreateCommittedResource failed 0x%08X\n", hr);
		return NULL;
	}
	HANDLE sh = NULL;
	hr = dev->CreateSharedHandle(s_probe_tex12, NULL, GENERIC_ALL, NULL, &sh);
	if (FAILED(hr) || !sh) {
		ps_log("[DisplayXR-PROV] PROBE: D3D12 CreateSharedHandle failed 0x%08X\n", hr);
		return NULL;
	}
	ps_log("[DisplayXR-PROV] PROBE: D3D12 shared texture %ux%u handle=%p\n", w, h, sh);
	return sh;
}

// D3D11 readback: CopyResource shared→staging, Map, dump. The runtime weaves on
// the SAME device/immediate context we use here, so a CopyResource after
// xrEndFrame is naturally ordered (coherent).
static void ps_probe_dump_d3d11(ID3D11Device *dev, ID3D11DeviceContext *ctx,
                                ID3D11Texture2D *tex, const char *path)
{
	if (!dev || !ctx || !tex) return;
	D3D11_TEXTURE2D_DESC d; tex->GetDesc(&d);
	D3D11_TEXTURE2D_DESC sd = d;
	sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0;
	sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
	ID3D11Texture2D *staging = NULL;
	if (FAILED(dev->CreateTexture2D(&sd, NULL, &staging)) || !staging) {
		ps_log("[DisplayXR-PROV] PROBE: D3D11 staging create failed\n"); return;
	}
	ctx->CopyResource(staging, tex);
	ctx->Flush();
	D3D11_MAPPED_SUBRESOURCE m = {};
	if (SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
		ps_write_bmp_bgra(path, d.Width, d.Height, (const uint8_t *)m.pData, m.RowPitch);
		ctx->Unmap(staging, 0);
	} else {
		ps_log("[DisplayXR-PROV] PROBE: D3D11 Map failed\n");
	}
	staging->Release();
}

// D3D12 readback: copy shared tex → READBACK buffer on the own queue, wait, map,
// dump. Best-effort — there is no shared fence with the runtime's compositor
// queue here, so a torn frame is possible; the static-ish scene keeps it legible.
static void ps_probe_dump_d3d12(const char *path)
{
	if (!s_probe_tex12 || !s_ps.own_device || !s_ps.own_queue ||
	    !s_ps.own_cmd_alloc || !s_ps.own_cmd_list || !s_ps.own_fence) return;
	D3D12_RESOURCE_DESC rd = s_probe_tex12->GetDesc();
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {}; UINT rows = 0; UINT64 rowBytes = 0, total = 0;
	s_ps.own_device->GetCopyableFootprints(&rd, 0, 1, 0, &fp, &rows, &rowBytes, &total);
	D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_READBACK;
	D3D12_RESOURCE_DESC bd = {};
	bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	bd.Width = total; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
	bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
	bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	ID3D12Resource *rb = NULL;
	if (FAILED(s_ps.own_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
	        D3D12_RESOURCE_STATE_COPY_DEST, NULL, __uuidof(ID3D12Resource), (void **)&rb)) || !rb) {
		ps_log("[DisplayXR-PROV] PROBE: D3D12 readback buffer create failed\n"); return;
	}
	s_ps.own_cmd_alloc->Reset();
	s_ps.own_cmd_list->Reset(s_ps.own_cmd_alloc, NULL);
	D3D12_RESOURCE_BARRIER b = {};
	b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	b.Transition.pResource = s_probe_tex12; b.Transition.Subresource = 0;
	b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
	b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
	s_ps.own_cmd_list->ResourceBarrier(1, &b);
	D3D12_TEXTURE_COPY_LOCATION dl = {};
	dl.pResource = rb; dl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dl.PlacedFootprint = fp;
	D3D12_TEXTURE_COPY_LOCATION sl = {};
	sl.pResource = s_probe_tex12; sl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; sl.SubresourceIndex = 0;
	s_ps.own_cmd_list->CopyTextureRegion(&dl, 0, 0, 0, &sl, NULL);
	D3D12_RESOURCE_BARRIER b2 = b;
	b2.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
	b2.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
	s_ps.own_cmd_list->ResourceBarrier(1, &b2);
	s_ps.own_cmd_list->Close();
	ID3D12CommandList *lists[] = { s_ps.own_cmd_list };
	s_ps.own_queue->ExecuteCommandLists(1, lists);
	s_ps.own_fence_value++;
	s_ps.own_queue->Signal(s_ps.own_fence, s_ps.own_fence_value);
	if (s_ps.own_fence->GetCompletedValue() < s_ps.own_fence_value) {
		s_ps.own_fence->SetEventOnCompletion(s_ps.own_fence_value, s_ps.own_fence_event);
		WaitForSingleObject(s_ps.own_fence_event, INFINITE);
	}
	void *p = NULL; D3D12_RANGE range = {0, (SIZE_T)total};
	if (SUCCEEDED(rb->Map(0, &range, &p)) && p) {
		ps_write_bmp_bgra(path, (uint32_t)rd.Width, (uint32_t)rd.Height,
		                  (const uint8_t *)p, fp.Footprint.RowPitch);
		rb->Unmap(0, NULL);
	} else {
		ps_log("[DisplayXR-PROV] PROBE: D3D12 readback Map failed\n");
	}
	rb->Release();
}

// GameView mirror (Task (a)): hand the display-provider the Unity-device view of the
// woven shared texture. Opened lazily here (called from the graphics thread once the
// session is up, so unity_d3d11_device is valid). The runtime writes the same
// underlying resource on the own device via sharedTextureHandle; legacy MISC_SHARED
// lets both devices open it (same adapter). NULL when the probe/texture mode is off.
void *dxr_prov_get_woven_unity_texture(uint32_t *w, uint32_t *h)
{
	if (s_probe_enabled && s_probe_handle && !s_probe_tex_unity && s_ps.unity_d3d11_device) {
		HRESULT hr = s_ps.unity_d3d11_device->OpenSharedResource(
		        s_probe_handle, __uuidof(ID3D11Texture2D), (void **)&s_probe_tex_unity);
		if (FAILED(hr) || !s_probe_tex_unity) {
			s_probe_tex_unity = NULL;
			ps_log("[DisplayXR-PROV] PROBE: OpenSharedResource(woven, Unity dev) failed 0x%08X\n", hr);
		} else {
			ps_log("[DisplayXR-PROV] PROBE: woven texture opened on Unity device for GameView mirror\n");
		}
	}
	// D3D12: open the NT-shared handle on Unity's ID3D12Device (mirrors the per-eye
	// bridge's OpenSharedHandle in ps_alloc_shared_bridge). On the D3D12 path
	// unity_d3d11_device is NULL (only unity_device is set), so the D3D11 branch above
	// is skipped. Unity accepts an ID3D12Resource* in desc.color.nativePtr on a D3D12
	// device — create_woven_mirror_texture_if_ready wraps whatever we return here.
	if (s_probe_enabled && s_probe_handle && !s_probe_tex12_unity &&
	    s_ps.graphics_api == DXR_GFX_D3D12 && s_ps.unity_device) {
		HRESULT hr = s_ps.unity_device->OpenSharedHandle(
		        s_probe_handle, __uuidof(ID3D12Resource), (void **)&s_probe_tex12_unity);
		if (FAILED(hr) || !s_probe_tex12_unity) {
			s_probe_tex12_unity = NULL;
			ps_log("[DisplayXR-PROV] PROBE: OpenSharedHandle(woven, Unity D3D12 dev) failed 0x%08X\n", hr);
		} else {
			ps_log("[DisplayXR-PROV] PROBE: woven texture opened on Unity D3D12 device for GameView mirror\n");
		}
	}
	if (w) *w = s_probe_woven_w;
	if (h) *h = s_probe_woven_h;
	return s_probe_tex_unity ? (void *)s_probe_tex_unity : (void *)s_probe_tex12_unity;
}
#else  // !_WIN32 — macOS: the weave-to-texture probe/mirror is Windows/D3D-only.
void *dxr_prov_get_woven_unity_texture(uint32_t *w, uint32_t *h)
{
	if (w) *w = 0;
	if (h) *h = 0;
	return NULL;
}
#endif // _WIN32

// The woven content occupies the canvas sub-rect of the shared texture; report it
// (+ full texture dims) so the mirror blit can normalize srcRect. The canvas tracks the
// LIVE window client (the runtime re-weaves into ps_window_size each frame via the size
// reconcile), so read it live here — NOT the frozen forced-zone dims cached in
// s_ps.zone_w/h at session start. When the window is glued to the Game view rect and
// then resized, the stale cached value would over-report the canvas → the mirror samples
// past the woven content into black (right/bottom truncation, Task (a) glue).
void dxr_prov_get_woven_canvas(int32_t *x, int32_t *y, int32_t *cw, int32_t *ch,
                               uint32_t *texw, uint32_t *texh)
{
	// The runtime composites the WHOLE PANE into the shared texture at the pane rect:
	// woven 3D inside the zone(s), plus the Local2D 2D bands outside them (the zone
	// wish mask + Local2D composite both run over the full pane region). So the mirror
	// srcRect is the PANE, not the zone.
	//
	// Using the zone here was only ever correct while the zone was forced to the full
	// pane. Once an app authors a sub-window zone (desktop-avatar: a 576-tall 3D band
	// with a 2D band above it), sampling the zone and stretching it over the Game view
	// (destRect is the whole RT) magnifies the band — which breaks the lenticular
	// interlace and hides the 2D bands entirely.
	int32_t rx = 0, ry = 0;
	int32_t rw = 0, rh = 0;
	if (s_probe_enabled && s_gv_panel_w > 0 && s_gv_panel_h > 0) {
		// Zone-glue publishes the pane's screen position; legacy window-glue doesn't
		// (the weave window origin carries it) → pane sits at the texture top-left.
		rx = (s_gv_panel_x != INT32_MIN) ? s_gv_panel_x : 0;
		ry = (s_gv_panel_y != INT32_MIN) ? s_gv_panel_y : 0;
		rw = s_gv_panel_w; rh = s_gv_panel_h;
	} else if (s_ps.zone_valid && s_ps.zone_w > 0 && s_ps.zone_h > 0) {
		rx = s_ps.zone_x; ry = s_ps.zone_y;
		rw = s_ps.zone_w; rh = s_ps.zone_h;
	}
	if (rw <= 0 || rh <= 0) {
		uint32_t ww = 0, wh = 0; ps_window_size(&ww, &wh);
		rw = ww ? (int32_t)ww : (int32_t)s_probe_woven_w;
		rh = wh ? (int32_t)wh : (int32_t)s_probe_woven_h;
	}
	if (x)  *x  = rx;
	if (y)  *y  = ry;
	if (cw) *cw = rw;
	if (ch) *ch = rh;
	if (texw) *texw = s_probe_woven_w;
	if (texh) *texh = s_probe_woven_h;
}

// Release probe resources (called from dxr_prov_session_stop and defensively at start).
#ifdef _WIN32
static void ps_probe_cleanup(void)
{
	if (s_probe_tex_unity) { s_probe_tex_unity->Release(); s_probe_tex_unity = NULL; }
	// D3D12 Unity-device view: Release only — the NT handle it was opened from
	// (s_probe_handle) is CloseHandle'd once in the s_probe_tex12 block below.
	if (s_probe_tex12_unity) { s_probe_tex12_unity->Release(); s_probe_tex12_unity = NULL; }
	s_probe_woven_w = 0; s_probe_woven_h = 0;
	if (s_probe_tex11) { s_probe_tex11->Release(); s_probe_tex11 = NULL; }
	if (s_probe_tex12) {
		s_probe_tex12->Release(); s_probe_tex12 = NULL;
		if (s_probe_handle) { CloseHandle(s_probe_handle); } // D3D12 CreateSharedHandle → NT handle
	}
	// D3D11 MISC_SHARED GetSharedHandle is owned by the resource — do NOT CloseHandle.
	s_probe_handle = NULL;
	s_probe_frames = 0;
	s_probe_dumped = 0;
}
#else
static void ps_probe_cleanup(void) {}  // macOS: the weave-to-texture probe is Windows-only
#endif // _WIN32

int dxr_prov_session_start(const char *runtime_json_path,
                           int backend_kind,
                           void *unity_device,
                           void *unity_queue,
                           void *overlay_hwnd)
{
	DxrGfxKind gfx = (DxrGfxKind)backend_kind;
	int is_d3d11 = (gfx == DXR_GFX_D3D11);
	int is_vk = (gfx == DXR_GFX_VULKAN);
	// D3D11 sub-mode (#195): the EDITOR (dedicated-window selector, set by C# on
	// Application.isEditor / the DISPLAYXR_PROV_EDITOR_WINDOW diagnostic) uses an
	// OWN-DEVICE bridge; the built PLAYER keeps the verified zero-copy path. The
	// selector reads file-statics that survive the s_ps memset below.
	int d3d11_bridge = is_d3d11 && (dxr_prov_get_dedicated_window() ||
	                                (getenv("DISPLAYXR_PROV_EDITOR_WINDOW") != NULL));
	// Preserve the C#-chosen render mode across the reset — it is pushed by the
	// loader (dxr_prov_set_single_pass) BEFORE this start runs.
	int saved_single_pass = s_ps.single_pass;
	int saved_single_pass_set = s_ps.single_pass_set;
	int saved_transparent = s_ps.transparent_requested;
	// Zone rect may be seeded before the session starts (demo SubsystemRegistration,
	// like the hook SeedLaunchZone) so the swapchain is born zone-sized. Preserve it.
	int saved_zone_valid = s_ps.zone_valid;
	uint32_t saved_zone_id = s_ps.zone_id;
	int32_t saved_zx = s_ps.zone_x, saved_zy = s_ps.zone_y, saved_zw = s_ps.zone_w, saved_zh = s_ps.zone_h;
	// Extra-zone rects are seeded before session start too; save + restore just the
	// rect fields (the swapchain/bridge handles are session-specific → reset to 0).
	uint32_t saved_extra_count = s_ps.extra_zone_count;
	ProviderExtraZone saved_extra[PS_MAX_ZONES - 1];
	for (uint32_t i = 0; i < PS_MAX_ZONES - 1; i++) saved_extra[i] = s_ps.extra_zones[i];
	memset(&s_ps, 0, sizeof(s_ps));
	s_ps.single_pass = saved_single_pass;
	s_ps.single_pass_set = saved_single_pass_set;
	s_ps.transparent_requested = saved_transparent;
	s_ps.zone_valid = saved_zone_valid;
	s_ps.zone_id = saved_zone_id;
	s_ps.zone_x = saved_zx; s_ps.zone_y = saved_zy; s_ps.zone_w = saved_zw; s_ps.zone_h = saved_zh;
	s_ps.extra_zone_count = saved_extra_count;
	for (uint32_t i = 0; i < PS_MAX_ZONES - 1; i++) {
		s_ps.extra_zones[i].valid = saved_extra[i].valid;
		s_ps.extra_zones[i].zone_id = saved_extra[i].zone_id;
		s_ps.extra_zones[i].rect_x = saved_extra[i].rect_x;
		s_ps.extra_zones[i].rect_y = saved_extra[i].rect_y;
		s_ps.extra_zones[i].rect_w = saved_extra[i].rect_w;
		s_ps.extra_zones[i].rect_h = saved_extra[i].rect_h;
	}
	s_ps.graphics_api = gfx;
#ifdef _WIN32
	s_ps.d3d11_bridge = d3d11_bridge;
	if (is_vk) {
		// Vulkan (#247): Unity's VkInstance/VkPhysicalDevice/VkDevice/queue are captured
		// by displayxr_unity_plugin.cpp from IUnityGraphicsVulkan and pushed into the VK
		// glue by the display-provider TU before this call. Nothing D3D is touched here —
		// the session device does not exist yet (the RUNTIME creates it, below).
		s_ps.unity_device = NULL;
		s_ps.unity_queue = NULL;
	} else if (is_d3d11) {
		s_ps.unity_d3d11_device = (ID3D11Device *)unity_device; // session binds directly on this
		// The runtime's native D3D11 compositor weaves on THIS device (zero-copy). It may
		// touch the immediate context from its own present/compositor thread, so mark the
		// device multithread-protected (Unity doesn't guarantee it) to avoid a shared-context
		// deadlock, and cache the immediate context so submit_frame can Flush Unity's render
		// before the runtime reads it. (#195)
		s_ps.unity_d3d11_context = NULL;
		if (s_ps.unity_d3d11_device) {
			s_ps.unity_d3d11_device->GetImmediateContext(&s_ps.unity_d3d11_context);
			// EXPERIMENT (#195): ID3D11Multithread protection is now OPT-IN via
			// DISPLAYXR_D3D11_MT_PROTECT=1 (default OFF). With it ON we saw the editor
			// render ~77 frames then hard-deadlock (both threads idle) — isolating whether
			// the shared-context lock is the culprit vs. a deeper shared-device issue.
			if (getenv("DISPLAYXR_D3D11_MT_PROTECT")) {
				ID3D11Multithread *mt = NULL;
				if (s_ps.unity_d3d11_context &&
				    SUCCEEDED(s_ps.unity_d3d11_context->QueryInterface(__uuidof(ID3D11Multithread), (void **)&mt)) && mt) {
					BOOL was = mt->SetMultithreadProtected(TRUE);
					ps_log("[DisplayXR-PROV] D3D11 immediate context multithread-protected (was=%d)\n", (int)was);
					mt->Release();
				} else {
					ps_log("[DisplayXR-PROV] WARN: could not set ID3D11Multithread protection\n");
				}
			} else {
				ps_log("[DisplayXR-PROV] D3D11 MT-protect OFF (set DISPLAYXR_D3D11_MT_PROTECT=1 to enable)\n");
			}
		}
	} else {
		s_ps.unity_device = (ID3D12Device *)unity_device;       // for opening the bridge + shared fence
		s_ps.unity_queue  = (ID3D12CommandQueue *)unity_queue;  // signals the cross-device sync fence
	}
#elif defined(__APPLE__)
	// Metal (#204): the session queue arrives via unity_queue (a provider-created
	// MTLCommandQueue on Unity's MTLDevice); stored below as metal_queue.
	(void)d3d11_bridge;
	s_ps.metal_device = unity_device;
	s_ps.metal_queue  = unity_queue;
#else
	// Linux (#249): Vulkan only. Unity's VkInstance/VkPhysicalDevice/VkDevice/queue
	// were pushed straight into the VK glue by GfxStart
	// (dxr_pvk_set_unity_objects), so nothing lands in s_ps here.
	(void)d3d11_bridge;
	(void)unity_device;
	(void)unity_queue;
#endif
	s_ps.overlay_hwnd = overlay_hwnd;
	s_ps.ipd_factor = s_ps.parallax_factor = s_ps.perspective_factor = 1.0f;
	s_ps.fov_override = tanf(0.5f * 1.0471975512f); // tan(30°) — neutral ~60° vFOV default
	s_ps.zone_caps_ok = -1; // untried (lazy caps query on first zone frame)

#ifdef _WIN32
	// Vulkan has no Unity-device precondition here: the session device is created by
	// the runtime further down (enable2), and the Unity-side objects were pushed
	// straight into the VK glue rather than through s_ps.
	if (!is_vk && (is_d3d11 ? (s_ps.unity_d3d11_device == NULL) : (s_ps.unity_device == NULL))) {
		ps_log("[DisplayXR-PROV] start: missing Unity %s device\n", is_d3d11 ? "D3D11" : "D3D12");
		return 0;
	}
#elif defined(__APPLE__)
	if (gfx != DXR_GFX_METAL || s_ps.metal_device == NULL || s_ps.metal_queue == NULL) {
		ps_log("[DisplayXR-PROV] start: Metal backend requires a device + command queue "
		       "(gfx=%d dev=%p queue=%p) — see #204\n", (int)gfx, s_ps.metal_device, s_ps.metal_queue);
		return 0;
	}
#else
	// Linux (#249): Vulkan only, and it has no Unity-device precondition here — the
	// session device is created by the runtime further down (enable2) and the
	// Unity-side objects already went to the VK glue.
	if (gfx != DXR_GFX_VULKAN) {
		ps_log("[DisplayXR-PROV] start: Linux requires the Vulkan backend (gfx=%d) — see #249\n",
		       (int)gfx);
		return 0;
	}
#endif
	// D3D11 supports BOTH render modes (#195): SPI (URP/HDRP) renders both eyes into the
	// arraySize=2 swapchain image directly (zero-copy) or via the editor bridge; MultiPass
	// (BiRP) renders each eye into its own single-slice texture (per-eye zero-copy target
	// or editor bridge) and submit copies each into swapchain slice 0/1 — the D3D11
	// analogue of the D3D12 MultiPass bridge. No render-mode gate remains.

	char *json = ps_resolve_runtime_json(runtime_json_path);
	if (!json) { ps_log("[DisplayXR-PROV] No runtime JSON (set XR_RUNTIME_JSON)\n"); return 0; }
	char *lib_rel = ps_parse_library_path(json);
	if (!lib_rel) { ps_log("[DisplayXR-PROV] library_path not found in %s\n", json); free(json); return 0; }
	char *lib_abs = ps_resolve_library_path(json, lib_rel);
	ps_log("[DisplayXR-PROV] runtime: %s\n", lib_abs);

#ifdef _WIN32
	HMODULE hmod = LoadLibraryExA(lib_abs, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
	free(json); free(lib_rel); free(lib_abs);
	if (!hmod) { ps_log("[DisplayXR-PROV] LoadLibrary failed: %lu\n", GetLastError()); return 0; }
	s_ps.runtime_lib = hmod;

	PFN_xrNegotiateLoaderRuntimeInterface negotiate =
	    (PFN_xrNegotiateLoaderRuntimeInterface)GetProcAddress(hmod, "xrNegotiateLoaderRuntimeInterface");
	if (!negotiate) { ps_log("[DisplayXR-PROV] negotiate symbol missing\n"); return 0; }
#else
	// dlopen mirror of the LoadLibraryExA path (lifted from the SA-era
	// displayxr_standalone.cpp). RTLD_LOCAL keeps the runtime's symbols out of the
	// global namespace; like Windows we never dlclose (in-process runtime threads).
	void *hmod = dlopen(lib_abs, RTLD_LOCAL | RTLD_LAZY);
	free(json); free(lib_rel);
	if (!hmod) {
		ps_log("[DisplayXR-PROV] dlopen failed: %s (%s)\n", lib_abs, dlerror());
		free(lib_abs);
		return 0;
	}
	free(lib_abs);
	s_ps.runtime_lib = hmod;

	PFN_xrNegotiateLoaderRuntimeInterface negotiate =
	    (PFN_xrNegotiateLoaderRuntimeInterface)dlsym(hmod, "xrNegotiateLoaderRuntimeInterface");
	if (!negotiate) { ps_log("[DisplayXR-PROV] negotiate symbol missing\n"); return 0; }
#endif

	XrNegotiateLoaderInfo li = {};
	li.structType = XR_LOADER_INTERFACE_STRUCT_LOADER_INFO;
	li.structVersion = 1;
	li.structSize = sizeof(li);
	li.minInterfaceVersion = 1;
	li.maxInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION;
	li.minApiVersion = XR_MAKE_VERSION(1, 0, 0);
	li.maxApiVersion = XR_MAKE_VERSION(1, 1, 0);
	XrNegotiateRuntimeRequest rr = {};
	rr.structType = XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST;
	rr.structVersion = 1;
	rr.structSize = sizeof(rr);
	if (XR_FAILED(negotiate(&li, &rr)) || !rr.getInstanceProcAddr) {
		ps_log("[DisplayXR-PROV] negotiate failed\n");
		return 0;
	}
	s_ps.gipa = rr.getInstanceProcAddr;

	// --- Probe XR_DXR_view_rig before requesting it (older runtimes reject unknown) ---
	{
		PFN_xrVoidFunction fn = NULL;
		s_ps.gipa(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", &fn);
		if (fn) {
			PFN_xrEnumerateInstanceExtensionProperties enum_ext = (PFN_xrEnumerateInstanceExtensionProperties)fn;
			uint32_t avail = 0;
			enum_ext(NULL, 0, &avail, NULL);
			if (avail > 0) {
				XrExtensionProperties *props = (XrExtensionProperties *)calloc(avail, sizeof(XrExtensionProperties));
				for (uint32_t i = 0; i < avail; i++) props[i].type = XR_TYPE_EXTENSION_PROPERTIES;
				if (XR_SUCCEEDED(enum_ext(NULL, avail, &avail, props))) {
					for (uint32_t i = 0; i < avail; i++) {
						if (strcmp(props[i].extensionName, XR_DXR_VIEW_RIG_EXTENSION_NAME) == 0)
							s_ps.has_view_rig = 1;
						else if (strcmp(props[i].extensionName, XR_DXR_DISPLAY_ZONES_EXTENSION_NAME) == 0)
							s_ps.has_display_zones = 1;
						else if (strcmp(props[i].extensionName, XR_DXR_LOCAL_3D_ZONE_EXTENSION_NAME) == 0)
							s_ps.has_local_3d_zone = 1;
						else if (strcmp(props[i].extensionName, XR_DXR_ATLAS_CAPTURE_EXTENSION_NAME) == 0)
							s_ps.has_atlas_capture = 1;
					}
				}
				free(props);
			}
		}
		ps_log("[DisplayXR-PROV] display_zones: %s; local_3d_zone: %s\n",
		       s_ps.has_display_zones ? "AVAILABLE" : "no",
		       s_ps.has_local_3d_zone ? "AVAILABLE" : "no");
		ps_log("[DisplayXR-PROV] XR_DXR_view_rig: %s\n",
		       s_ps.has_view_rig ? "AVAILABLE" : "not found (no stereo)");
		ps_log("[DisplayXR-PROV] XR_DXR_atlas_capture: %s\n",
		       s_ps.has_atlas_capture ? "AVAILABLE" : "no (screenshot inert)");
	}

	// Re-apply the APP-AUTHORED 3D zone (if any) on EVERY session start, for BOTH
	// render paths. dxr_prov_session_stop memsets s_ps and the docked<->undocked switch
	// RESTARTS the session, but the app seeds its zone once before XR init and does not
	// re-seed. The texture-mode canvas block further down runs only on the shared-texture
	// (docked) path, so without this the PRESENT (undocked) path came up with no zone at
	// all: the projection was submitted full-window, the zone wish mask never formed, and
	// the Local2D bands had nothing to composite into — desktop-avatar's speech bubble
	// vanished on undock and did not come back on re-dock.
	if (s_app_zone_valid && s_ps.has_display_zones) {
		s_ps.zone_x = s_app_zone_x; s_ps.zone_y = s_app_zone_y;
		s_ps.zone_w = s_app_zone_w; s_ps.zone_h = s_app_zone_h;
		s_ps.zone_id = 1;
		s_ps.zone_valid = 1;
		ps_log("[DisplayXR-PROV] session start: re-applied APP zone (%d,%d %dx%d)\n",
		       s_ps.zone_x, s_ps.zone_y, s_ps.zone_w, s_ps.zone_h);
	}
	// Extra (multi-)zones — same restart problem, same mirror.
	if (s_app_extra_zone_count > 0 && s_ps.has_display_zones) {
		for (uint32_t i = 0; i < s_app_extra_zone_count && i < PS_MAX_ZONES - 1; i++)
			s_ps.extra_zones[i] = s_app_extra_zones[i];
		s_ps.extra_zone_count = s_app_extra_zone_count;
		ps_log("[DisplayXR-PROV] session start: re-applied %u extra APP zone(s)\n",
		       s_app_extra_zone_count);
	}
	// Transparent-background opt-in is consumed at xrCreateSession; without this the
	// session came back OPAQUE after a restart even though the app had requested it.
	if (s_app_transparent_requested && !s_ps.transparent_requested) {
		s_ps.transparent_requested = 1;
		ps_log("[DisplayXR-PROV] session start: re-applied transparent-background request\n");
	}

	// --- Create instance ---
	const char *extensions[8];
	uint32_t ext_count = 0;
	extensions[ext_count++] = XR_DXR_DISPLAY_INFO_EXTENSION_NAME;
#ifdef _WIN32
	// Vulkan uses enable2 specifically (never enable1): under enable1 the app owns the
	// VkDevice, so the runtime gets no queue of its own and the #868 weave-rate
	// decoupling repaint silently stays off — see runtime#886.
	extensions[ext_count++] = is_vk ? "XR_KHR_vulkan_enable2"
	                                : (is_d3d11 ? "XR_KHR_D3D11_enable" : "XR_KHR_D3D12_enable");
	extensions[ext_count++] = XR_DXR_WIN32_WINDOW_BINDING_EXTENSION_NAME;
#elif defined(__linux__) && !defined(__ANDROID__)
	// Desktop Linux (#249): Vulkan only, same enable2 reasoning as above, plus the
	// xlib window binding so this is a HANDLE app like Windows and macOS — the
	// runtime weaves into the player's own window. Requested unconditionally; if we
	// cannot find the window at session-create we simply chain nothing and the
	// runtime self-hosts (enabling an extension we then don't use is harmless).
	extensions[ext_count++] = "XR_KHR_vulkan_enable2";
	extensions[ext_count++] = XR_DXR_XLIB_WINDOW_BINDING_EXTENSION_NAME;
#else
	extensions[ext_count++] = XR_KHR_METAL_ENABLE_EXTENSION_NAME;
	extensions[ext_count++] = XR_DXR_COCOA_WINDOW_BINDING_EXTENSION_NAME;
#endif
	if (s_ps.has_view_rig) extensions[ext_count++] = XR_DXR_VIEW_RIG_EXTENSION_NAME;
	// Zones (#166 Phase B): display_zones needs view_rig (composes on top of it);
	// local_3d_zone is required to submit Local2D layers for the 2D bands.
	if (s_ps.has_display_zones && s_ps.has_view_rig)
		extensions[ext_count++] = XR_DXR_DISPLAY_ZONES_EXTENSION_NAME;
	if (s_ps.has_local_3d_zone)
		extensions[ext_count++] = XR_DXR_LOCAL_3D_ZONE_EXTENSION_NAME;
	// Atlas capture (#140): app-facing screenshot ('I' key). Enable
	// unconditionally-if-advertised — independent of transparency/zones.
	if (s_ps.has_atlas_capture)
		extensions[ext_count++] = XR_DXR_ATLAS_CAPTURE_EXTENSION_NAME;

	PFN_xrVoidFunction fn_create = NULL;
	s_ps.gipa(XR_NULL_HANDLE, "xrCreateInstance", &fn_create);
	if (!fn_create) { ps_log("[DisplayXR-PROV] xrCreateInstance unresolved\n"); return 0; }
	XrInstanceCreateInfo ici = {XR_TYPE_INSTANCE_CREATE_INFO};
	strncpy(ici.applicationInfo.applicationName, "DisplayXR Unity Provider", XR_MAX_APPLICATION_NAME_SIZE - 1);
	ici.applicationInfo.applicationVersion = 1;
	strncpy(ici.applicationInfo.engineName, "Unity", XR_MAX_ENGINE_NAME_SIZE - 1);
	ici.applicationInfo.engineVersion = 1;
	ici.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
	ici.enabledExtensionCount = ext_count;
	ici.enabledExtensionNames = extensions;
	if (XR_FAILED(((PFN_xrCreateInstance)fn_create)(&ici, &s_ps.instance))) {
		ps_log("[DisplayXR-PROV] xrCreateInstance failed\n");
		return 0;
	}

	if (!ps_resolve_functions()) { dxr_prov_session_stop(); return 0; }

	// --- System ---
	XrSystemGetInfo sgi = {XR_TYPE_SYSTEM_GET_INFO};
	sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	if (XR_FAILED(s_ps.pfn_get_system(s_ps.instance, &sgi, &s_ps.system_id))) {
		ps_log("[DisplayXR-PROV] xrGetSystem failed\n"); dxr_prov_session_stop(); return 0;
	}

#if defined(ENABLE_VULKAN)
	// Vulkan (#247 Windows, #249 Linux): the RUNTIME creates the VkInstance/
	// VkPhysicalDevice/VkDevice (enable2), so it can request its own queue. The glue
	// also runs the cross-adapter guard — the VK form of #240 — and refuses on
	// mismatch. Identical on both OSes, hence outside the per-OS branch.
	if (is_vk) {
		if (!dxr_pvk_create_device(s_ps.instance, s_ps.system_id, s_ps.gipa, NULL)) {
			dxr_prov_session_stop();
			return 0;
		}
	} else
#endif
#ifdef _WIN32
	if (is_d3d11) {
		if (s_ps.d3d11_bridge) {
			// D3D11 EDITOR bridge (#195): create a SEPARATE own D3D11 device (the session
			// binds on it, not Unity's) so Unity's editor GameView present never shares the
			// weaver's device (the Optimus cross-present deadlock).
			if (!ps_create_own_device_d3d11()) { dxr_prov_session_stop(); return 0; }
		} else {
			// D3D11 zero-copy (player): no own device. The runtime REQUIRES
			// xrGetD3D11GraphicsRequirementsKHR to have been called before xrCreateSession,
			// and enforces that Unity's device sits on the returned adapter LUID; pre-check
			// it here for a provider-branded WARN.
			if (!ps_d3d11_check_requirements()) { dxr_prov_session_stop(); return 0; }
		}
	} else if (displayxr_is_shell_mode()) {
		// --- Workspace/IPC (shell) tile: bind the session to UNITY's D3D12 device (ADR-032).
		//     No in-process weaver / GameView present under the shell, so the own-device
		//     isolation isn't needed; same-device = coherent bridge into the service. ---
		if (!ps_alias_unity_device_d3d12()) { dxr_prov_session_stop(); return 0; }
	} else {
		// --- Create the session's OWN D3D12 device (matched to runtime adapter LUID).
		//     The runtime allocates its swapchain on this device; we bridge to Unity. ---
		if (!ps_create_own_device()) { dxr_prov_session_stop(); return 0; }
	}
#elif defined(__APPLE__)
	// Metal (#204): call xrGetMetalGraphicsRequirementsKHR before xrCreateSession
	// (spec-mandated) and WARN-and-continue on a device mismatch — on Apple Silicon
	// the runtime's preferred device is always the system default = Unity's.
	{
		PFN_xrVoidFunction fn = NULL;
		s_ps.gipa(s_ps.instance, "xrGetMetalGraphicsRequirementsKHR", &fn);
		if (fn) {
			XrGraphicsRequirementsMetalKHR req = {XR_TYPE_GRAPHICS_REQUIREMENTS_METAL_KHR};
			XrResult rr2 = ((PFN_xrGetMetalGraphicsRequirementsKHR)fn)(s_ps.instance, s_ps.system_id, &req);
			if (XR_FAILED(rr2))
				ps_log("[DisplayXR-PROV] WARN: xrGetMetalGraphicsRequirementsKHR failed (0x%x)\n", (unsigned)rr2);
			else if (req.metalDevice && req.metalDevice != s_ps.metal_device)
				ps_log("[DisplayXR-PROV] WARN: runtime prefers a different MTLDevice (%p vs %p)\n",
				       req.metalDevice, s_ps.metal_device);
		} else {
			ps_log("[DisplayXR-PROV] WARN: xrGetMetalGraphicsRequirementsKHR unresolved — continuing\n");
		}
	}
#else
	// Linux (#249): Vulkan-only, handled by the arm above. Closes the dangling `else`
	// when neither the D3D nor the Metal branch is compiled in.
	{
		if (!is_vk) {
			ps_log("[DisplayXR-PROV] no graphics-device path for this API on Linux — "
			       "Vulkan is required (#249)\n");
			dxr_prov_session_stop();
			return 0;
		}
	}
#endif

	// --- Display info ---
	XrDisplayInfoDXR di = {};
	di.type = XR_TYPE_DISPLAY_INFO_DXR;
	XrSystemProperties sp = {XR_TYPE_SYSTEM_PROPERTIES};
	sp.next = &di;
	if (XR_SUCCEEDED(s_ps.pfn_get_system_properties(s_ps.instance, s_ps.system_id, &sp))) {
		s_ps.display_info.width_m = di.displaySizeMeters.width;
		s_ps.display_info.height_m = di.displaySizeMeters.height;
		s_ps.display_info.pixel_width = di.displayPixelWidth;
		s_ps.display_info.pixel_height = di.displayPixelHeight;
		s_ps.display_info.nominal_x = di.nominalViewerPositionInDisplaySpace.x;
		s_ps.display_info.nominal_y = di.nominalViewerPositionInDisplaySpace.y;
		s_ps.display_info.nominal_z = di.nominalViewerPositionInDisplaySpace.z;
		s_ps.display_info.scale_x = di.recommendedViewScaleX;
		s_ps.display_info.scale_y = di.recommendedViewScaleY;
		s_ps.display_info.is_valid = 1;

		// Publish to shared state so the editor gizmos + 2D-surround sizing
		// (displayxr_get_display_info) get valid display info under the provider.
		// The provider previously kept only a local copy (s_ps.display_info),
		// leaving the shared DisplayXRDisplayInfo.is_valid = 0 — which disabled
		// the gizmo's window-relative Kooima math (#189: needs panel px + physical
		// dims to rebase eyes / size the convergence plane). Written once at
		// session start; display info is static.
		DisplayXRDisplayInfo *sdi = &displayxr_get_state()->display_info;
		sdi->display_width_meters = di.displaySizeMeters.width;
		sdi->display_height_meters = di.displaySizeMeters.height;
		sdi->display_pixel_width = di.displayPixelWidth;
		sdi->display_pixel_height = di.displayPixelHeight;
		sdi->nominal_viewer_x = di.nominalViewerPositionInDisplaySpace.x;
		sdi->nominal_viewer_y = di.nominalViewerPositionInDisplaySpace.y;
		sdi->nominal_viewer_z = di.nominalViewerPositionInDisplaySpace.z;
		sdi->recommended_view_scale_x = di.recommendedViewScaleX;
		sdi->recommended_view_scale_y = di.recommendedViewScaleY;
		sdi->is_valid = 1;

		ps_log("[DisplayXR-PROV] Display: %ux%u %.3fx%.3fm\n",
		       di.displayPixelWidth, di.displayPixelHeight,
		       di.displaySizeMeters.width, di.displaySizeMeters.height);
	} else {
		ps_log("[DisplayXR-PROV] xrGetSystemProperties failed\n");
	}

	// --- Environment blend modes (transparency, #166 Phase A) ---
	// Probe whether the runtime advertises ALPHA_BLEND for the primary-stereo view
	// config. Only then do we opt the session into a transparent background.
	s_ps.alpha_blend_supported = 0;
	if (s_ps.pfn_enumerate_blend_modes) {
		uint32_t bm_count = 0;
		s_ps.pfn_enumerate_blend_modes(s_ps.instance, s_ps.system_id,
		        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &bm_count, NULL);
		if (bm_count > 0 && bm_count <= 8) {
			XrEnvironmentBlendMode bms[8];
			if (XR_SUCCEEDED(s_ps.pfn_enumerate_blend_modes(s_ps.instance, s_ps.system_id,
			        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, bm_count, &bm_count, bms))) {
				for (uint32_t i = 0; i < bm_count; i++)
					if (bms[i] == XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND)
						{ s_ps.alpha_blend_supported = 1; break; }
			}
		}
		ps_log("[DisplayXR-PROV] ALPHA_BLEND: %s (transparent requested=%d)\n",
		       s_ps.alpha_blend_supported ? "advertised" : "not advertised",
		       s_ps.transparent_requested);
	}
	int use_transparent = s_ps.transparent_requested && s_ps.alpha_blend_supported;

#ifdef _WIN32
	// --- Session: graphics binding on UNITY'S device + window binding (overlay HWND) ---
	XrWin32WindowBindingCreateInfoDXR win_binding = {};
	win_binding.type = XR_TYPE_WIN32_WINDOW_BINDING_CREATE_INFO_DXR;
	win_binding.windowHandle = s_ps.overlay_hwnd;
	win_binding.readbackCallback = NULL;
	win_binding.readbackUserdata = NULL;
	win_binding.sharedTextureHandle = NULL;
	// Transparent background (#166 Phase A): opt the runtime's DComp swapchain into
	// per-pixel alpha over the desktop. chromaKeyColor=0 (no post-weave conversion —
	// the runtime composes-under-bg + alpha-gates, same as the hook path).
	win_binding.transparentBackgroundEnabled = use_transparent ? XR_TRUE : XR_FALSE;
	win_binding.chromaKeyColor = 0;

	// Weave-to-texture (editor GameView default, #740): bind in TEXTURE MODE so the
	// runtime weaves into our shared texture instead of presenting to the overlay window.
	// windowHandle stays set (weaver position tracking), mirroring the ref app.
	s_probe_enabled  = ps_texture_mode();
	// Vulkan (#247): weave-to-texture is a D3D-only mechanism — the shared texture handed
	// to the runtime is created by ps_probe_create_tex_d3d12 on s_ps.own_device, which does
	// not exist on this backend (the session device is a VkDevice). Leaving texture mode on
	// would hand the runtime a NULL handle and weave into nothing, i.e. a black GameView
	// that looks exactly like a rendering bug. Force PRESENT mode (the dedicated weave
	// window, #173) instead — correct, just not the docked-GameView experience. Wiring
	// weave-to-texture for VK needs a VkImage->shared-handle export path and is Phase 2.
	if (s_ps.graphics_api == DXR_GFX_VULKAN && s_probe_enabled) {
		s_probe_enabled = 0;
		ps_log("[DisplayXR-PROV] Vulkan: weave-to-texture unavailable on this backend (#247 "
		       "Phase 1) — falling back to the dedicated weave window (present mode)\n");
	}
	// The .bmp readback stays a pure diagnostic: armed ONLY when the env var is set, so it
	// never fires in the default (setter-driven) editor path.
	s_probe_readback = (getenv("DISPLAYXR_PROV_TEXTURE_PROBE") != NULL) ? 1 : 0;
	// Unconditional build marker: if this line is ABSENT from the log, the editor is
	// running a STALE DLL (restart Unity).
	int bind_present = s_probe_enabled && ps_bind_present();
	ps_log("[DisplayXR-PROV] build=weave-to-texture-2tile texture_mode=%d bind=%s\n",
	       s_probe_enabled, bind_present ? "PRESENT" : "TEXTURE");
	ps_probe_cleanup(); // clears counters + any leftover texture from a prior session
	// PRESENT mode (undocked, #740 hybrid): skip the shared-texture bind entirely — the
	// runtime presents the woven stereo into the dedicated window (windowHandle) like the
	// shipping/main path. No shared texture ⟹ no mirror-blit (s_woven_tex_id stays 0) and
	// no forced zone; the window is born VISIBLE top-level (displayxr_win32.c) and the SR
	// weaver self-anchors to it (GA_ROOT==self). Correct anchor + phase-snap, zero correction.
	if (s_probe_enabled && !bind_present) {
		DisplayXRDisplayInfo *pdi = &displayxr_get_state()->display_info;
		uint32_t pw = pdi->display_pixel_width  ? pdi->display_pixel_width  : 1920;
		uint32_t ph = pdi->display_pixel_height ? pdi->display_pixel_height : 1080;
		if (is_d3d11) {
			ID3D11Device *bindDev = s_ps.d3d11_bridge ? s_ps.own_d3d11_device
			                                          : s_ps.unity_d3d11_device;
			s_probe_handle = ps_probe_create_tex_d3d11(bindDev, pw, ph);
		} else {
			s_probe_handle = ps_probe_create_tex_d3d12(s_ps.own_device, pw, ph);
		}
		if (s_probe_handle) {
			win_binding.sharedTextureHandle = s_probe_handle;
			s_probe_woven_w = pw; s_probe_woven_h = ph; // for the GameView mirror srcRect
			// The Leia DP's texture-mode weave is CANVAS-DRIVEN: a plain projection
			// gives the DP canvas=(0,0 0x0) → it writes nothing into the shared
			// texture (confirmed: runtime log dp_d3d11_process_atlas canvas 0x0,
			// readback all-black). The cube_zones_texture_* reference apps work
			// because they submit a display ZONE, which supplies the canvas. Force a
			// full-window zone here so the DP has a canvas to weave into the shared
			// texture. (DISPLAYXR_PROV_TEXTURE_PROBE_NOZONE=1 keeps the plain path for
			// A/B.)
			if (s_ps.has_display_zones && getenv("DISPLAYXR_PROV_TEXTURE_PROBE_NOZONE") == NULL) {
				// Task (a) fill: born the forced zone at the GameView RENDER size (stashed
				// from C# before the session started, and the dedicated weave window is
				// created at that same size in displayxr_create_provider_dedicated_window)
				// so the rendered tile size + the runtime's woven region fill the panel at
				// native resolution. Without this the window is at its creation default
				// (~1248x632) and the zone freezes tiny → the mirror srcRect over-samples
				// into black. Take the zone dims from the stashed values (deterministic);
				// do NOT restyle/move the window here — the window is fragile mid-start
				// (raw-input hooks installed, device setup in flight); the per-frame glue
				// repositions it after the session is up.
				// Zone-glue arrangement (#740/#742, desktop-avatar contract): when C# has
				// published the pane's full screen rect pre-session (dxr_prov_set_panel_rect),
				// the window is born at the MONITOR origin and the ZONE carries the pane's
				// position — so seed the zone at that rect. Legacy window-glue: zone at
				// (0,0) window-sized (the window origin carries the position).
				s_ps.zone_valid = 1;
				s_ps.zone_id = 1;
				if (s_app_zone_valid) {
					// The app seeded its own zone (the documented contract) — that
					// zone IS the DP canvas. Forcing a full-window one here would
					// only be undone by the app's next push, at the cost of a
					// swapchain realloc and a transient wrong-sized canvas.
					//
					// RESTORE from the authoritative record: dxr_prov_session_stop
					// memsets s_ps, so on a session RESTART (dock<->undock switches
					// the render path and restarts the session) the live copy is
					// zeroed while s_app_zone_valid — a file static — survives.
					// Reading s_ps here would install a degenerate 0x0 zone that the
					// compositor skips, killing the zone content and the Local2D
					// bands with it. The app does not necessarily re-seed on restart.
					s_ps.zone_x = s_app_zone_x; s_ps.zone_y = s_app_zone_y;
					s_ps.zone_w = s_app_zone_w; s_ps.zone_h = s_app_zone_h;
					ps_log("[DisplayXR-PROV] texture-mode canvas: keeping APP zone (%d,%d %dx%d)\n",
					       s_ps.zone_x, s_ps.zone_y, s_ps.zone_w, s_ps.zone_h);
				} else if (s_gv_panel_x != INT32_MIN && s_gv_panel_w > 0 && s_gv_panel_h > 0) {
					s_ps.zone_x = s_gv_panel_x; s_ps.zone_y = s_gv_panel_y;
					s_ps.zone_w = s_gv_panel_w; s_ps.zone_h = s_gv_panel_h;
					ps_log("[DisplayXR-PROV] PROBE: forced ZONE-GLUE zone (%d,%d %dx%d) as texture-mode canvas\n",
					       s_ps.zone_x, s_ps.zone_y, s_ps.zone_w, s_ps.zone_h);
				} else {
					uint32_t ww = 0, wh = 0;
					if (s_init_gv_w > 0 && s_init_gv_h > 0) {
						ww = (uint32_t)s_init_gv_w; wh = (uint32_t)s_init_gv_h;
					} else {
						ps_window_size(&ww, &wh);
					}
					if (ww == 0 || wh == 0) { ww = 1280; wh = 720; }
					s_ps.zone_x = 0; s_ps.zone_y = 0;
					s_ps.zone_w = (int32_t)ww; s_ps.zone_h = (int32_t)wh;
					ps_log("[DisplayXR-PROV] PROBE: forced full-window zone %dx%d as texture-mode canvas (init_gv=%dx%d)\n",
					       s_ps.zone_w, s_ps.zone_h, s_init_gv_w, s_init_gv_h);
				}
			}
			ps_log("[DisplayXR-PROV] PROBE: TEXTURE MODE active (%ux%u) — overlay window "
			       "will NOT present; readback dumps once at frame %u.\n", pw, ph, kProbeDumpFrame);
		} else {
			ps_log("[DisplayXR-PROV] PROBE: shared-texture create FAILED — window mode.\n");
		}
	}

	// D3D11 (#195): bind DIRECTLY on Unity's ID3D11Device (no queue — the runtime's
	// native D3D11 compositor uses the immediate context). D3D12: bind on our own device.
	XrGraphicsBindingD3D11KHR d3d11 = {};
	XrGraphicsBindingD3D12KHR d3d12 = {};
	const void *gfx_binding;
	if (is_vk) {
		// XrGraphicsBindingVulkan2KHR built by the glue over the runtime-created
		// instance/physical device/device + the graphics queue family we requested.
		gfx_binding = dxr_pvk_session_binding(&win_binding);
		if (!gfx_binding) {
			ps_log("[DisplayXR-PROV] Vulkan session binding unavailable\n");
			dxr_prov_session_stop();
			return 0;
		}
	} else if (is_d3d11) {
		d3d11.type = XR_TYPE_GRAPHICS_BINDING_D3D11_KHR;
		// Editor bridge: bind on our OWN device (isolated from Unity's present device).
		// Player zero-copy: bind directly on Unity's device.
		d3d11.device = s_ps.d3d11_bridge ? s_ps.own_d3d11_device : s_ps.unity_d3d11_device;
		d3d11.next = &win_binding;
		gfx_binding = &d3d11;
	} else {
		d3d12.type = XR_TYPE_GRAPHICS_BINDING_D3D12_KHR;
		d3d12.device = s_ps.own_device;   // session runs on our own device
		d3d12.queue = s_ps.own_queue;
		d3d12.next = &win_binding;
		gfx_binding = &d3d12;
	}
#elif defined(ENABLE_VULKAN) && defined(__linux__) && !defined(__ANDROID__)
	// --- Session: Vulkan graphics binding + xlib window binding (#249) ---
	//
	// HANDLE app, same shape as Windows (HWND) and macOS (NSView): the runtime
	// weaves into UNITY'S OWN window rather than creating one of its own.
	// IUnityGraphics does not hand us the X11 window — exactly the gap macOS has
	// for its NSView — so displayxr_linux.c finds it by walking the window tree
	// for _NET_WM_PID == getpid(), the X11 analogue of walking NSApplication.
	//
	// If no window is found (no libX11, no DISPLAY, not yet mapped) we chain
	// nothing and the runtime self-hosts an XCB window instead — degraded but
	// still rendering, rather than failing the session.
	XrXlibWindowBindingCreateInfoDXR xlib_binding = {};
	const void *win_chain = NULL;
	{
		void *xdpy = NULL;
		unsigned long xwin = 0;
		if (displayxr_linux_get_weave_window(&xdpy, &xwin) && xdpy && xwin) {
			xlib_binding.type = XR_TYPE_XLIB_WINDOW_BINDING_CREATE_INFO_DXR_PS;
			xlib_binding.next = NULL;
			xlib_binding.xDisplay = xdpy;
			xlib_binding.window = xwin;
			xlib_binding.transparentBackgroundEnabled = use_transparent ? 1 : 0;
			win_chain = &xlib_binding;
			ps_log("[DisplayXR-PROV] Linux: binding the runtime's weave to the PLUGIN-OWNED "
			       "overlay window 0x%lx (transparent=%d)\n", xwin, (int)use_transparent);
		} else {
			ps_log("[DisplayXR-PROV] Linux: no overlay window available — the runtime will "
			       "self-host its weave window\n");
		}
	}
	const void *gfx_binding = dxr_pvk_session_binding(win_chain);
	if (!gfx_binding) {
		ps_log("[DisplayXR-PROV] Vulkan session binding unavailable\n");
		dxr_prov_session_stop();
		return 0;
	}
#else
	// --- Session: Metal graphics binding (client queue) + cocoa window binding ---
	// viewHandle == NULL → the runtime self-hosts its NSWindow + CAMetalLayer (the
	// SELFHOST shape); Phase 2 (#204) switches to displayxr_get_app_main_view()'s
	// NSView for in-app weave once frames are correct.
	XrCocoaWindowBindingCreateInfoDXR cocoa_binding = {};
	cocoa_binding.type = XR_TYPE_COCOA_WINDOW_BINDING_CREATE_INFO_DXR;
	cocoa_binding.viewHandle = s_ps.overlay_hwnd;
	cocoa_binding.readbackCallback = NULL;
	cocoa_binding.readbackUserdata = NULL;
	cocoa_binding.sharedIOSurface = NULL;
	cocoa_binding.transparentBackgroundEnabled = use_transparent ? XR_TRUE : XR_FALSE;

	XrGraphicsBindingMetalKHR metal = {};
	metal.type = XR_TYPE_GRAPHICS_BINDING_METAL_KHR;
	metal.commandQueue = s_ps.metal_queue; // non-NULL required by the runtime
	metal.next = &cocoa_binding;
	const void *gfx_binding = &metal;
#endif

	XrSessionCreateInfo sci = {XR_TYPE_SESSION_CREATE_INFO};
	sci.next = gfx_binding;
	sci.systemId = s_ps.system_id;
	if (XR_FAILED(s_ps.pfn_create_session(s_ps.instance, &sci, &s_ps.session))) {
		ps_log("[DisplayXR-PROV] xrCreateSession failed\n"); dxr_prov_session_stop(); return 0;
	}
#ifdef _WIN32
	ps_log("[DisplayXR-PROV] Session created (%s, overlay HWND=%p)\n",
	       is_vk ? "runtime-created Vulkan device (enable2 + external-memory bridge)"
	           : (!is_d3d11 ? (displayxr_is_shell_mode() ? "Unity's D3D12 device (workspace/IPC same-device)"
	                                                     : "own D3D12 device")
	               : (s_ps.d3d11_bridge ? "own D3D11 device (editor bridge)"
	                                    : "zero-copy on Unity's D3D11 device")), s_ps.overlay_hwnd);
#elif defined(__APPLE__)
	ps_log("[DisplayXR-PROV] Session created (Metal client queue=%p, NSView=%p%s)\n",
	       s_ps.metal_queue, s_ps.overlay_hwnd,
	       s_ps.overlay_hwnd ? "" : " — runtime self-hosted window");
#else
	ps_log("[DisplayXR-PROV] Session created (runtime-created Vulkan device, enable2 + "
	       "external-memory bridge; runtime self-hosted window)\n");
#endif

	// --- LOCAL reference space ---
	XrReferenceSpaceCreateInfo rsci = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
	rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	rsci.poseInReferenceSpace.orientation = XrQuaternionf{0, 0, 0, 1};
	rsci.poseInReferenceSpace.position = XrVector3f{0, 0, 0};
	if (XR_FAILED(s_ps.pfn_create_reference_space(s_ps.session, &rsci, &s_ps.local_space))) {
		ps_log("[DisplayXR-PROV] xrCreateReferenceSpace failed\n");
		s_ps.local_space = XR_NULL_HANDLE;
	}

	ps_enumerate_modes();

	// Present path = the runtime presents its woven output to a window (no shared texture bound):
	// undocked editor window + every built player. Drives the sRGB-swapchain gamma fix in
	// ps_create_swapchain (s_probe_handle is a file-static declared later, so latch it here).
	// macOS has no texture-mode shared texture (Editor+Windows-only), so it is always present path.
#ifdef _WIN32
	s_sc_present_path = (s_probe_handle == NULL);
#else
	s_sc_present_path = 1;
#endif

	// Swapchain is created on session-ready (deferred), but attempt early too.
	ps_create_swapchain();

	s_ps.running = 1;
	ps_log("[DisplayXR-PROV] Session start OK\n");
	return 1;
}

void dxr_prov_session_stop(void)
{
	if (s_ps.wsui_swapchain && s_ps.pfn_destroy_swapchain) s_ps.pfn_destroy_swapchain(s_ps.wsui_swapchain);
#ifdef _WIN32
	if (s_ps.wsui_bridge_unity)  s_ps.wsui_bridge_unity->Release();
	if (s_ps.wsui_bridge_handle) CloseHandle(s_ps.wsui_bridge_handle);
	if (s_ps.wsui_bridge_own)    s_ps.wsui_bridge_own->Release();
	if (s_ps.wsui_unity_tex)     s_ps.wsui_unity_tex->Release();
	if (s_ps.wsui_unity_tex_own) s_ps.wsui_unity_tex_own->Release(); // D3D11 editor bridge (NT handle closed at alloc)
#endif
	if (s_ps.l2d_swapchain && s_ps.pfn_destroy_swapchain) s_ps.pfn_destroy_swapchain(s_ps.l2d_swapchain);
#ifdef _WIN32
	if (s_ps.l2d_bridge_unity)  s_ps.l2d_bridge_unity->Release();
	if (s_ps.l2d_bridge_handle) CloseHandle(s_ps.l2d_bridge_handle);
	if (s_ps.l2d_bridge_own)    s_ps.l2d_bridge_own->Release();
	if (s_ps.l2d_unity_tex)     s_ps.l2d_unity_tex->Release();
	if (s_ps.l2d_unity_tex_own) s_ps.l2d_unity_tex_own->Release(); // D3D11 editor bridge
#endif
	for (uint32_t i = 0; i < PS_MAX_ZONES - 1; i++) {
		ProviderExtraZone *z = &s_ps.extra_zones[i];
		if (z->swapchain && s_ps.pfn_destroy_swapchain) s_ps.pfn_destroy_swapchain(z->swapchain);
#ifdef _WIN32
		if (z->bridge_unity) z->bridge_unity->Release();
		if (z->bridge_handle) CloseHandle(z->bridge_handle);
		if (z->bridge_own) z->bridge_own->Release();
		if (z->unity_tex) z->unity_tex->Release();
		if (z->unity_tex_own) z->unity_tex_own->Release(); // D3D11 editor bridge (NT handle closed at alloc)
		for (int e = 0; e < 2; e++) {
			if (z->bridge_unity_eye[e]) z->bridge_unity_eye[e]->Release();
			if (z->bridge_handle_eye[e]) CloseHandle(z->bridge_handle_eye[e]);
			if (z->bridge_own_eye[e]) z->bridge_own_eye[e]->Release();
		}
#endif
	}
	ps_probe_cleanup(); // weave-to-texture PROBE shared texture + handle (experiment)
	if (s_ps.swapchain && s_ps.pfn_destroy_swapchain) s_ps.pfn_destroy_swapchain(s_ps.swapchain);
#if defined(ENABLE_VULKAN)
	// Vulkan (#247/#249): drop the bridges BEFORE xrDestroySession — the bridge images live
	// on the session device, which the runtime tears down with the session. Unity's own
	// objects are only dereferenced, never destroyed. No-op on the D3D paths.
	if (s_ps.graphics_api == DXR_GFX_VULKAN) dxr_pvk_destroy();
#endif
	if (s_ps.session && s_ps.session_ready && s_ps.pfn_end_session) s_ps.pfn_end_session(s_ps.session);
	if (s_ps.session && s_ps.pfn_destroy_session) s_ps.pfn_destroy_session(s_ps.session);
	if (s_ps.instance && s_ps.pfn_destroy_instance) s_ps.pfn_destroy_instance(s_ps.instance);

#ifdef _WIN32
	// Bridge + cross-device fence + own-device resources.
	ps_realloc_grave_flush(); // deferred realloc generation (#740 resize crash)
	if (s_ps.shared_fence_unity)  s_ps.shared_fence_unity->Release();
	if (s_ps.shared_fence_handle) CloseHandle(s_ps.shared_fence_handle);
	if (s_ps.shared_fence_own)    s_ps.shared_fence_own->Release();
	if (s_ps.bridge_unity)    s_ps.bridge_unity->Release();
	if (s_ps.bridge_handle)   CloseHandle(s_ps.bridge_handle);
	if (s_ps.bridge_own)      s_ps.bridge_own->Release();
	for (int e = 0; e < 2; e++) {
		if (s_ps.bridge_unity_eye[e])  s_ps.bridge_unity_eye[e]->Release();
		if (s_ps.bridge_handle_eye[e]) CloseHandle(s_ps.bridge_handle_eye[e]);
		if (s_ps.bridge_own_eye[e])    s_ps.bridge_own_eye[e]->Release();
	}
	if (s_ps.own_fence_event) CloseHandle(s_ps.own_fence_event);
	if (s_ps.own_fence)       s_ps.own_fence->Release();
	if (s_ps.own_cmd_list)    s_ps.own_cmd_list->Release();
	if (s_ps.own_cmd_alloc)   s_ps.own_cmd_alloc->Release();
	if (s_ps.own_queue)       s_ps.own_queue->Release();
	if (s_ps.own_device)      s_ps.own_device->Release();

	// D3D11 (#195): primary SPI bridge + MultiPass per-eye targets + shared fence + own
	// device/context. (Texture NT handles were closed at alloc; the FENCE handle below IS
	// still open.) The zero-copy MultiPass per-eye targets live in d3d11_bridge_unity_eye[]
	// (own side NULL) and are released by the same loop.
	if (s_ps.d3d11_bridge_unity)  s_ps.d3d11_bridge_unity->Release();
	if (s_ps.d3d11_bridge_own)    s_ps.d3d11_bridge_own->Release();
	for (int e = 0; e < 2; e++) {
		if (s_ps.d3d11_bridge_unity_eye[e]) s_ps.d3d11_bridge_unity_eye[e]->Release();
		if (s_ps.d3d11_bridge_own_eye[e])   s_ps.d3d11_bridge_own_eye[e]->Release();
	}
	if (s_ps.d3d11_fence_unity)   s_ps.d3d11_fence_unity->Release();
	if (s_ps.d3d11_fence_handle)  CloseHandle(s_ps.d3d11_fence_handle); // fence handle IS an NT handle
	if (s_ps.d3d11_fence_own)     s_ps.d3d11_fence_own->Release();
	if (s_ps.own_d3d11_context)   s_ps.own_d3d11_context->Release();
	if (s_ps.own_d3d11_device)    s_ps.own_d3d11_device->Release();
	// D3D11 zero-copy (#195): we don't own Unity's device, but GetImmediateContext AddRef'd.
	if (s_ps.unity_d3d11_context) s_ps.unity_d3d11_context->Release();
#endif // _WIN32
#ifdef __APPLE__
	// Metal (#204): release everything the glue retained (session queue, shared
	// event, per-eye targets). The device is Unity's — never released.
	dxr_prov_metal_teardown();
#endif

	// Intentionally do NOT FreeLibrary(runtime_lib): background threads may still
	// reference it (matches the standalone's deliberate leak).
	void *lib = s_ps.runtime_lib;
	memset(&s_ps, 0, sizeof(s_ps));
	s_ps.runtime_lib = lib;
}

int dxr_prov_session_is_running(void) { return s_ps.running; }

// #223 follow-up: 1 once the runtime signals the app to terminate (session EXITING /
// LOSS_PENDING — e.g. the shell's workspace exit request). The C# driver polls this and
// calls Application.Quit(); a native app just exits its loop, a Unity app must be told.
int dxr_prov_exit_requested(void) { return s_ps.exit_requested; }

// Render-mode gate (#166 task #8). C# decides SPI vs MultiPass from the active
// render pipeline (URP+Win+D3D12 → SPI; BiRP/other → MultiPass — SPI renders
// opaque geometry wrong on BiRP) and pushes it before the session starts.
void dxr_prov_set_single_pass(int enable)
{
	s_ps.single_pass = enable ? 1 : 0;
	s_ps.single_pass_set = 1;
	ps_log("[DisplayXR-PROV] render mode: %s\n",
	       s_ps.single_pass ? "Single-Pass-Instanced (SPI)" : "MultiPass (2 pass x 1)");
}

// Effective mode. Default = SPI (1) when C# never set one, preserving the
// pre-gating behavior.
int dxr_prov_get_single_pass(void) { return s_ps.single_pass_set ? s_ps.single_pass : 1; }

// (#173) Dedicated-window weave target for editor Play Mode. A plain file-static
// (NOT part of s_ps) so it is untouched by the session_start memset — the loader
// sets it once before the subsystem starts and the display-provider TU reads it in
// LifecycleStart/GfxStart.
static int s_prov_dedicated_window = 0;
void dxr_prov_set_dedicated_window(int enable)
{
	s_prov_dedicated_window = enable ? 1 : 0;
	ps_log("[DisplayXR-PROV] dedicated window (editor): %d\n", s_prov_dedicated_window);
}
int dxr_prov_get_dedicated_window(void) { return s_prov_dedicated_window; }

// Glue-to-GameView (Task (a)): reposition the dedicated editor weave window so its
// client rect exactly covers the Unity Game view's on-screen region. In texture mode
// the runtime derives BOTH window-relative Kooima framing AND the weaver's lenticular
// phase from this window's screen geometry (win_binding.windowHandle) — but the woven
// output is now shown by the mirror-blit inside the Game tab, not in this window. By
// gluing the window's client rect to the Game view rect, that geometry becomes correct
// with zero runtime change, and the forced full-window zone (client-sized) makes the
// weave canvas == the Game view size so the mirror srcRect follows.
//
// The window never presents in texture mode, so we make it invisible-in-practice:
// strip the chrome (WS_POPUP → client rect == window rect == Game view rect), drop
// WS_EX_TOPMOST, and push it to the BOTTOM of the z-order so the editor window (which
// always contains the Game view region) fully occludes it. It stays a normal visible
// window to the OS, so the runtime's position tracking is unaffected.
// Called each frame from C# (editor + probe only). x,y = screen px (top-left origin),
// w,h = Game view size in px. w<=0||h<=0 is ignored.
void dxr_prov_set_initial_gameview_rect(int x, int y, int w, int h)
{
	s_init_gv_x = x; s_init_gv_y = y;
	s_init_gv_w = (w > 0) ? w : 0;
	s_init_gv_h = (h > 0) ? h : 0;
	ps_log("[DisplayXR-PROV] initial gameview rect stashed: (%d,%d %dx%d)\n", x, y, w, h);
}

int dxr_prov_get_initial_gameview_rect(int *x, int *y, int *w, int *h)
{
	if (x) *x = s_init_gv_x;
	if (y) *y = s_init_gv_y;
	if (w) *w = s_init_gv_w;
	if (h) *h = s_init_gv_h;
	return (s_init_gv_w > 0 && s_init_gv_h > 0) ? 1 : 0;
}

// The dedicated weave window is BORN WS_POPUP at the Game-view rect
// (dxr_prov_set_initial_gameview_rect, before session start). It must NOT be
// RESTYLED while the Dimenco SR weaver is live: any SetWindowPos(..., SWP_FRAMECHANGED)
// on the bound HWND fires a WM_NCCALCSIZE that trips the weaver's WndProc subclass and
// permanently collapses the weave to a single view (view0) — the "stereo for one frame
// then flat mono" bug (#727). Root-caused with a fence-synced post-weave/post-composite
// dual tap + an A/B on this push (zero window ops = stereo every frame; a single
// SWP_FRAMECHANGED push = mono from that frame on). PLAIN move/resize (no frame recalc)
// is weaver-safe — proven across dozens of scripted mid-session re-glues — so the
// follow below moves/resizes on change but never passes SWP_FRAMECHANGED.
//
// GameView follow (DEFAULT ON in texture-probe mode, #727 follow-up): when the Game
// view moves/resizes (drag, dock/undock, maximize) the weave window follows with a
// plain move+resize — SWP_FRAMECHANGED is NEVER used (a frame recalc on the SR-weaver-
// bound HWND permanently collapses the weave to mono, #727); plain move/resize is
// weaver-proven safe (slice-color captures across dozens of scripted re-glues).
//   (unset)                       → move + resize follow (default)
//   DISPLAYXR_PROV_GV_TRACK=0     → off (born-once no-op, the pre-follow behavior)
//   DISPLAYXR_PROV_GV_TRACK=move  → move only (SWP_NOSIZE) — isolation diagnostic
//
// NO #61 drag bracket here (tried 2026-07-12, REGRESSED steady-state phase — do not
// re-add): synthesizing WM_ENTERSIZEMOVE/WM_EXITSIZEMOVE around the follow pushes made
// the weaver's exit phase-snap re-anchor the bound window onto lenticular-aligned
// pixels — correct for a window that presents its own content (#61 overlay drag), but
// it UN-GLUES this hidden reference window from the Game-view rect by a position-
// dependent few px → wrong, position-dependent phase at settle in every state except
// coincidentally-aligned ones. Silent pushes settle phase-correct (HW-verified);
// mid-drag phase tracking (drag stutter) is an open follow-up needing a different
// mechanism than the bracket.
// BINDPANE experiment (#740): when set, the session binds UNITY'S OWN Game-view pane
// window (GUIView child) instead of the dedicated proxy window — the SR SDK then
// tracks the real content window natively (GA_ROOT = Unity's container, the normal
// windowed-SR-app shape) and the zone carries the render-area offset within the
// pane's client. The plugin must NEVER SetWindowPos / restyle this window (it is
// Unity's) — dxr_prov_set_gameview_rect below guards on it.
static void *s_ext_weave_hwnd = NULL;

void dxr_prov_set_gameview_rect(int x, int y, int w, int h)
{
#if defined(__APPLE__)
	// macOS: GameView weave-to-texture window tracking is a Windows-only feature
	// (SetWindowPos/child-glue on the dedicated proxy window). No-op — keep the symbol.
	(void)x; (void)y; (void)w; (void)h;
#elif defined(_WIN32)
	if (s_ext_weave_hwnd) return; // BINDPANE: the bound window is UNITY'S — never touch it
	if (w <= 0 || h <= 0 || !s_ps.overlay_hwnd) return;
	// (#740) Defensive size clamp: the render area can never exceed the virtual screen —
	// a transient container-sized rect during a layout reset must not glue the window huge.
	{
		int maxw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
		int maxh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
		if (maxw > 0 && w > maxw) w = maxw;
		if (maxh > 0 && h > maxh) h = maxh;
	}
	static int s_track = -1;      // -1 unknown, 0 off, 1 move+resize, 2 move-only
	if (s_track < 0) {
		const char *e = getenv("DISPLAYXR_PROV_GV_TRACK");
		s_track = (e && e[0]) ? (e[0] == 'm' ? 2 : (atoi(e) == 1 ? 1 : 0)) : 1;
		ps_log("[DisplayXR-PROV] gameview track mode: %s%s\n",
		       s_track == 0 ? "OFF" : s_track == 2 ? "move-only" : "move+resize",
		       (e && e[0]) ? " (env)" : " (default)");
	}
	if (s_track == 0) return; // env off-switch: born-once no-op (#727-safe baseline)

	// Seed the last-rect from the BORN rect so a docked steady state (incoming == born) never
	// fires a spurious SetWindowPos — only a genuine move/resize away from born touches the HWND.
	static int s_have_last = 0, lx = 0, ly = 0, lw = 0, lh = 0;
	if (!s_have_last) { s_have_last = 1; lx = s_init_gv_x; ly = s_init_gv_y; lw = s_init_gv_w; lh = s_init_gv_h; }
	if (x == lx && y == ly && w == lw && h == lh) return; // only on actual change from born/last
	lx = x; ly = y; lw = w; lh = h;

	// Child-glue (#740): the incoming rect is SCREEN px, but the window is a WS_CHILD —
	// SetWindowPos wants coords relative to the container's client. Convert. (When the
	// container itself moves, the pane's screen pos changes but the child coord stays put,
	// so this is a no-op; only a pane move WITHIN the container repositions the child.)
	int sx = x, sy = y;
	if (displayxr_dedicated_is_childglue()) {
		int ox = 0, oy = 0;
		if (displayxr_dedicated_parent_client_origin(&ox, &oy)) { sx = x - ox; sy = y - oy; }
	}

	UINT flags = SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER; // NO SWP_FRAMECHANGED
	if (s_track == 2) flags |= SWP_NOSIZE; // move only
	SetWindowPos((HWND)s_ps.overlay_hwnd, NULL, sx, sy, w, h, flags);
	ps_log("[DisplayXR-PROV] gameview window track (%s): screen(%d,%d) -> pos(%d,%d) %dx%d\n",
	       s_track == 2 ? "move" : "move+resize", x, y, sx, sy, w, h);
#else
	// Linux (#249): GameView weave-to-texture window tracking is Windows-only.
	(void)x; (void)y; (void)w; (void)h;
#endif // _WIN32
}

// Transparent-background request (#166 Phase A). Set from C# BEFORE the session
// starts; the actual ALPHA_BLEND opt-in also requires the runtime to advertise it
// (probed in session_start). Preserved across the session_start memset.
void dxr_prov_set_transparent_background(int enable)
{
	s_ps.transparent_requested = enable ? 1 : 0;
	s_app_transparent_requested = s_ps.transparent_requested; // survives session restart
	ps_log("[DisplayXR-PROV] transparent background requested: %d\n", s_ps.transparent_requested);
}

int dxr_prov_wants_transparent(void) { return s_ps.transparent_requested; }

// --- Zones (#166 Phase B) ---------------------------------------------------
// Define the single 3D-zone rect (client-window px, top-left origin). The locate
// chains XrDisplayZoneDXR in front of the rig and submit chains it on the
// projection layer; the swapchain is (re)sized to the zone's recommended view
// size. w<=0||h<=0 clears (full-window framing). Seed BEFORE the session starts
// (demo SubsystemRegistration) so the swapchain is born zone-sized — a later
// change only re-sizes the swapchain on the next session start (live realloc is a
// follow-up, mirroring the hook path's launch-seed model).
// Apply a rect to the LIVE zone only (no app-zone bookkeeping). Used by the
// texture-mode forced zone and the GameView converge, which derive a live rect and
// must not be mistaken for the app's authored zone.
static void ps_apply_3d_zone_rect(int32_t x, int32_t y, int32_t w, int32_t h)
{
	if (w <= 0 || h <= 0) {
		if (s_ps.zone_valid) ps_log("[DisplayXR-PROV] 3d_zone: cleared\n");
		s_ps.zone_valid = 0;
		return;
	}
	int changed = (!s_ps.zone_valid || s_ps.zone_x != x || s_ps.zone_y != y ||
	               s_ps.zone_w != w || s_ps.zone_h != h);
	s_ps.zone_x = x; s_ps.zone_y = y; s_ps.zone_w = w; s_ps.zone_h = h;
	if (s_ps.zone_id == 0) s_ps.zone_id = 1;
	s_ps.zone_valid = 1;
	if (changed) {
		ps_query_zone_rec_size();
		ps_log("[DisplayXR-PROV] 3d_zone: (%d,%d) %dx%d -> rec %ux%u\n",
		       x, y, w, h, s_ps.zone_rec_w, s_ps.zone_rec_h);
	}
}

void dxr_prov_set_3d_zone_rect(int32_t x, int32_t y, int32_t w, int32_t h)
{
	// Record the app's intent BEFORE applying — converge derives the live zone from
	// this, so it survives any number of converge passes (idempotent).
	if (w <= 0 || h <= 0) {
		s_app_zone_valid = 0;
	} else {
		s_app_zone_x = x; s_app_zone_y = y; s_app_zone_w = w; s_app_zone_h = h;
		s_app_zone_valid = 1;
	}
	ps_apply_3d_zone_rect(x, y, w, h);
}

void dxr_prov_clear_3d_zone(void)
{
	s_ps.zone_valid = 0;
	ps_log("[DisplayXR-PROV] clear_3d_zone\n");
}

// --- GameView zone convergence (Phase 1, #727 follow-up) --------------------
// C# publishes the authoritative Game-view panel PHYSICAL px here each frame
// (GetMainGameViewTargetSize x ppp). This is the ONLY reliable physical-px source:
// info.mirrorRtDesc reports LOGICAL px on a HiDPI display (879x374 at ppp=2.5), which
// wrongly shrinks the zone. The per-frame pump (dxr_prov_converge_gameview_zone)
// re-drives the forced full-window zone to it so the compositor canvas == render
// viewport pixel-exact. NO window op — the value is Scale-independent (mainSize is the
// target size, not the zoom), so magnify (absorbed by the mirror-blit downscale) drives
// no change; this converges the born size and adapts on a real tab resize.
void dxr_prov_set_panel_px(int w, int h)
{
	s_gv_panel_w = (w > 0) ? w : 0;
	s_gv_panel_h = (h > 0) ? h : 0;
}

// BINDPANE (#740): setter/getter for s_ext_weave_hwnd (declared above with the
// GV_TRACK statics — dxr_prov_set_gameview_rect guards on it). Set from C# BEFORE the
// session starts; LifecycleStart consumes it instead of creating the dedicated window.
void dxr_prov_set_external_weave_hwnd(void *hwnd)
{
	s_ext_weave_hwnd = hwnd;
	ps_log("[DisplayXR-PROV] BINDPANE: external weave HWND set: %p\n", hwnd);
}
void *dxr_prov_get_external_weave_hwnd(void) { return s_ext_weave_hwnd; }

// Zone-glue arrangement (#740/#742): publish the Game view pane's FULL screen rect.
// The weave window sits at the monitor origin (born once, never moved — no #727
// exposure, no drag re-snaps); the zone rect carries the pane position, so a Game view
// move is a pure zone x/y data update (no realloc — size unchanged) and a resize is the
// existing converge/realloc path. Called every frame from C# (editor + probe only).
void dxr_prov_set_panel_rect(int x, int y, int w, int h)
{
	s_gv_panel_x = x;
	s_gv_panel_y = y;
	s_gv_panel_w = (w > 0) ? w : 0;
	s_gv_panel_h = (h > 0) ? h : 0;
}

// Called from the per-frame pump BEFORE dxr_prov_reconcile_size. If the published panel
// px differs from the live zone, re-drive the zone (clamped to the shared woven texture
// dims) via the existing change-detected path → reconcile reallocs the swapchain/bridge,
// the submit re-chains the new zone, and the mirror srcRect remaps. Probe-mode + active
// zone only; a no-op once converged (so a steady panel triggers no realloc).
void dxr_prov_converge_gameview_zone(void)
{
	if (!s_probe_enabled || !s_ps.zone_valid) return;
	if (s_gv_panel_w <= 0 || s_gv_panel_h <= 0) return;
	// Zone-glue: the zone rect also carries the pane's screen POSITION (weave window is
	// parked at the monitor origin). Legacy window-glue publishes no position → pane
	// origin is 0,0 (the window origin carries the position).
	const int px = (s_gv_panel_x != INT32_MIN) ? s_gv_panel_x : 0;
	const int py = (s_gv_panel_y != INT32_MIN) ? s_gv_panel_y : 0;
	int x, y, w, h;
	if (s_app_zone_valid) {
		// The app authored a zone (e.g. desktop-avatar's 3D band with a 2D band above
		// it for the Local2D bubble). Converge REPOSITIONS it into the pane and never
		// redefines its extent — replacing the extent with the pane's while keeping the
		// app's offset pushes the zone past the pane bottom, which truncates the woven
		// content, shifts the lenticular phase, and swallows the 2D band.
		x = px + s_app_zone_x;
		y = py + s_app_zone_y;
		w = s_app_zone_w;
		h = s_app_zone_h;
		// Clamp to the pane — an app zone larger than the pane must not weave outside it.
		if (x < px) { w -= (px - x); x = px; }
		if (y < py) { h -= (py - y); y = py; }
		if (x + w > px + s_gv_panel_w) w = px + s_gv_panel_w - x;
		if (y + h > py + s_gv_panel_h) h = py + s_gv_panel_h - y;
	} else {
		// No app zone — the provider's own full-pane canvas (texture-mode default).
		x = px; y = py; w = s_gv_panel_w; h = s_gv_panel_h;
	}
	// Clamp to the shared woven texture (the runtime cannot weave past it).
	if (x < 0) x = 0;
	if (y < 0) y = 0;
	if (s_probe_woven_w > 0 && (uint32_t)(x + w) > s_probe_woven_w) w = (int)s_probe_woven_w - x;
	if (s_probe_woven_h > 0 && (uint32_t)(y + h) > s_probe_woven_h) h = (int)s_probe_woven_h - y;
	if (w <= 0 || h <= 0) return;
	if (x == s_ps.zone_x && y == s_ps.zone_y && w == s_ps.zone_w && h == s_ps.zone_h)
		return; // already converged — no zone churn
	ps_log("[DisplayXR-PROV] gameview zone converge: (%d,%d %dx%d) -> (%d,%d %dx%d) (panel=(%d,%d %dx%d) tex=%ux%u)\n",
	       s_ps.zone_x, s_ps.zone_y, s_ps.zone_w, s_ps.zone_h, x, y, w, h,
	       s_gv_panel_x, s_gv_panel_y, s_gv_panel_w, s_gv_panel_h,
	       s_probe_woven_w, s_probe_woven_h);
	// LIVE zone only — must NOT be recorded as the app's authored zone, or the next
	// converge would derive from its own output and the app's rect would be lost.
	ps_apply_3d_zone_rect(x, y, w, h);
}

// Multi-zone (#166 Phase B2). total_3d_zones = number of 3D zones (index 0 =
// primary, 1.. = extra). Clamped to PS_MAX_ZONES. 0/1 → no extra zones.
void dxr_prov_set_zone_count(uint32_t total_3d_zones)
{
	if (total_3d_zones > PS_MAX_ZONES) total_3d_zones = PS_MAX_ZONES;
	uint32_t extra = total_3d_zones > 1 ? total_3d_zones - 1 : 0;
	if (extra != s_ps.extra_zone_count)
		ps_log("[DisplayXR-PROV] zone count: %u 3D zone(s) (1 primary + %u extra)\n",
		       total_3d_zones ? total_3d_zones : 1, extra);
	// Deactivate any extra zones beyond the new count.
	for (uint32_t i = extra; i < PS_MAX_ZONES - 1; i++) s_ps.extra_zones[i].valid = 0;
	s_ps.extra_zone_count = extra;
	// Mirror for re-apply after a session restart (s_ps is memset at session stop).
	for (uint32_t i = extra; i < PS_MAX_ZONES - 1; i++) s_app_extra_zones[i].valid = 0;
	s_app_extra_zone_count = extra;
}

// Set 3D zone `index`'s rect (client-window px). index 0 → primary (== set_3d_zone_rect);
// index>=1 → extra zone [index-1]. Seed BEFORE session start for born-zone-sized.
void dxr_prov_set_zone(uint32_t index, uint32_t zone_id, int32_t x, int32_t y, int32_t w, int32_t h)
{
	if (index == 0) {
		dxr_prov_set_3d_zone_rect(x, y, w, h);
		if (zone_id) s_ps.zone_id = zone_id;
		return;
	}
	uint32_t ei = index - 1;
	if (ei >= PS_MAX_ZONES - 1) return;
	ProviderExtraZone *z = &s_ps.extra_zones[ei];
	if (w <= 0 || h <= 0) {
		z->valid = 0;
		s_app_extra_zones[ei].valid = 0;
		return;
	}
	z->zone_id = zone_id ? zone_id : (index + 1);
	z->rect_x = x; z->rect_y = y; z->rect_w = w; z->rect_h = h;
	z->valid = 1;
	if (ei + 1 > s_ps.extra_zone_count) s_ps.extra_zone_count = ei + 1;
	// Mirror for re-apply after a session restart (s_ps is memset at session stop).
	s_app_extra_zones[ei] = *z;
	if (ei + 1 > s_app_extra_zone_count) s_app_extra_zone_count = ei + 1;
}

void dxr_prov_set_zone_feather(uint32_t index, float feather_px)
{
	if (index >= PS_MAX_ZONES) return;
	// Sanitize here so the submit chain stays branch-simple: negative/NaN =
	// hard (matches the runtime's own contract).
	s_app_zone_feather[index] = (feather_px > 0.0f) ? feather_px : 0.0f;
}

uint32_t dxr_prov_get_extra_zone_count(void) { return s_ps.extra_zone_count; }

// ============================================================================
// Swapchain surfacing
// ============================================================================

void dxr_prov_get_swapchain_info(uint32_t *width, uint32_t *height,
                                 uint32_t *array_size, uint32_t *image_count)
{
	if (width) *width = s_ps.sc_width;
	if (height) *height = s_ps.sc_height;
	if (array_size) *array_size = s_ps.sc_array;
	if (image_count) *image_count = s_ps.sc_image_count;
}

void *dxr_prov_get_bridge_unity_texture(uint32_t *width, uint32_t *height, uint32_t *array_size)
{
	if (width) *width = s_ps.sc_width;
	if (height) *height = s_ps.sc_height;
	if (array_size) *array_size = 2;
#if defined(ENABLE_VULKAN)
	// Vulkan (#247/#249): the Unity-side alias of the shared 2-layer bridge image.
	// See dxr_pvk_unity_image_ptr for the exact pointer convention Unity's XR
	// display path expects here — it is NOT the CreateExternalTexture convention.
	if (s_ps.graphics_api == DXR_GFX_VULKAN)
		return dxr_pvk_unity_image_ptr(-1); // SPI slot; NULL in MultiPass mode
#endif
#ifdef _WIN32
	// D3D11 editor bridge (#195): the Unity-opened side of the shared 2-slice bridge
	// (the display-provider wraps it like the D3D12 SPI bridge). Else the D3D12 bridge.
	if (s_ps.graphics_api == DXR_GFX_D3D11)
		return s_ps.d3d11_bridge ? (void *)s_ps.d3d11_bridge_unity : NULL;
	return (void *)s_ps.bridge_unity; // SPI mode; NULL in MultiPass mode
#else
	// Metal has no bridge (#204 wires per-eye targets); Linux Vulkan already
	// returned above.
	return NULL;
#endif
}

void *dxr_prov_get_bridge_unity_texture_eye(uint32_t eye, uint32_t *width, uint32_t *height)
{
	if (width) *width = s_ps.sc_width;
	if (height) *height = s_ps.sc_height;
	if (eye > 1) return NULL;
#if defined(ENABLE_VULKAN)
	// Vulkan (#247/#249): the per-eye Unity-side bridge image (see above).
	if (s_ps.graphics_api == DXR_GFX_VULKAN)
		return dxr_pvk_unity_image_ptr((int)eye); // MultiPass slots; NULL in SPI mode
#endif
#ifdef _WIN32
	// D3D11 MultiPass (#195): the Unity-side per-eye render target (editor bridge = the
	// Unity-opened shared side; zero-copy = a plain Unity texture). Else the D3D12 per-eye bridge.
	if (s_ps.graphics_api == DXR_GFX_D3D11)
		return (void *)s_ps.d3d11_bridge_unity_eye[eye];
	return (void *)s_ps.bridge_unity_eye[eye]; // MultiPass mode; NULL in SPI mode
#else
	// Metal (#204): no bridge — zero-copy MultiPass wraps the swapchain slice
	// views directly (dxr_prov_get_metal_eye_view); NULL keeps callers inert.
	return NULL;
#endif
}

int dxr_prov_get_graphics_api(void)
{
	return (int)s_ps.graphics_api;
}

int dxr_prov_d3d11_bridge_active(void)
{
#ifdef _WIN32
	return s_ps.d3d11_bridge;
#else
	return 0;
#endif
}

void *dxr_prov_get_swapchain_image_texture(uint32_t index, uint32_t *out_w,
                                           uint32_t *out_h, uint32_t *out_array)
{
	if (out_w) *out_w = s_ps.sc_width;
	if (out_h) *out_h = s_ps.sc_height;
	if (out_array) *out_array = 2; // arraySize=2 (SPI: left=slice 0, right=slice 1)
#ifdef _WIN32
	// D3D11 zero-copy only: the runtime swapchain image (ID3D11Texture2D on Unity's device),
	// wrapped directly. In editor bridge mode the images live on the OWN device — the
	// display-provider wraps the Unity-side bridge instead (dxr_prov_get_bridge_unity_texture).
	if (s_ps.graphics_api != DXR_GFX_D3D11 || s_ps.d3d11_bridge || index >= s_ps.sc_image_count) return NULL;
	return (void *)s_ps.sc_images_d3d11[index].texture;
#elif defined(__APPLE__)
	// Metal zero-copy (Phase 2 #204 fast-follow): the runtime swapchain image
	// (id<MTLTexture> on Unity's device), wrapped directly by the display provider.
	if (s_ps.graphics_api != DXR_GFX_METAL || index >= s_ps.sc_image_count) return NULL;
	return s_ps.sc_images_metal[index].texture;
#else
	// Linux/Vulkan (#249): the swapchain images live on the RUNTIME's session device,
	// so there is nothing here Unity could wrap — the eye bridge is the only path
	// (dxr_prov_get_bridge_unity_texture).
	(void)index;
	return NULL;
#endif
}

// ============================================================================
// Frame loop
// ============================================================================

void dxr_prov_poll_events(void)
{
	if (!s_ps.running || !s_ps.pfn_poll_event) return;
	XrEventDataBuffer ev = {XR_TYPE_EVENT_DATA_BUFFER};
	while (s_ps.pfn_poll_event(s_ps.instance, &ev) == XR_SUCCESS) {
		if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
			XrEventDataSessionStateChanged *ssc = (XrEventDataSessionStateChanged *)&ev;
			s_ps.session_state = ssc->state;
			ps_log("[DisplayXR-PROV] session state: %d\n", (int)ssc->state);
			switch (ssc->state) {
			case XR_SESSION_STATE_READY: {
				XrSessionBeginInfo bi = {XR_TYPE_SESSION_BEGIN_INFO};
				bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
				if (XR_SUCCEEDED(s_ps.pfn_begin_session(s_ps.session, &bi))) {
					s_ps.session_ready = 1;
					if (!s_ps.swapchain_created) ps_create_swapchain();
					ps_create_extra_zones();
				}
				break;
			}
			case XR_SESSION_STATE_STOPPING:
				s_ps.session_ready = 0;
				s_ps.pfn_end_session(s_ps.session);
				break;
			case XR_SESSION_STATE_EXITING:
			case XR_SESSION_STATE_LOSS_PENDING:
				// The runtime is telling us to terminate — e.g. the shell's workspace
				// close/exit request (request_exit_by_slot) arrives as EXITING, exactly as
				// a native app (cube_handle) sees it and quits its loop. A Unity app can't
				// just exit a loop — latch this for the C# driver to call Application.Quit()
				// (#223 follow-up: the tile was ignoring the shell's close request).
				s_ps.running = 0;
				s_ps.exit_requested = 1;
				ps_log("[DisplayXR-PROV] session EXITING/LOSS_PENDING → exit_requested (Application.Quit)\n");
				break;
			default: break;
			}
		} else if (ev.type == XR_TYPE_EVENT_DATA_RENDERING_MODE_CHANGED_DXR) {
			XrEventDataRenderingModeChangedDXR *mc = (XrEventDataRenderingModeChangedDXR *)&ev;
			// Re-enumerate so active_mode_index + per-mode tiling/scale refresh;
			// the per-frame render rect (dxr_prov_get_render_rect) then adapts with
			// NO swapchain realloc (worst-case sized). Latch for C#.
			ps_enumerate_modes();
			s_ps.active_mode_index = mc->currentModeIndex;
			s_ps.ev_mode_prev = mc->previousModeIndex;
			s_ps.ev_mode_cur = mc->currentModeIndex;
			s_ps.ev_mode_changed = 1;
			ps_log("[DisplayXR-PROV] event: rendering mode %u -> %u\n",
			       mc->previousModeIndex, mc->currentModeIndex);
		} else if (ev.type == XR_TYPE_EVENT_DATA_HARDWARE_DISPLAY_STATE_CHANGED_DXR) {
			XrEventDataHardwareDisplayStateChangedDXR *hc = (XrEventDataHardwareDisplayStateChangedDXR *)&ev;
			s_ps.ev_hw3d = hc->hardwareDisplay3D ? 1 : 0;
			s_ps.ev_hw_changed = 1;
			ps_log("[DisplayXR-PROV] event: hardware 3D -> %d\n", s_ps.ev_hw3d);
			// Couple the RENDERING mode (view count / tile config) to the hardware
			// display state (#172 P4): a light-field 2D display wants ONE view submitted;
			// 3D wants two. Apps that only toggle the hardware display mode (2d-ui's V key
			// → xrRequestDisplayMode) otherwise keep the 2-view submit → the two eyes weave
			// as if 3D on the flat screen (ghosting). If the active rendering mode's
			// hardwareDisplay3D no longer matches, request the matching enumerated mode;
			// the runtime sends RENDERING_MODE_CHANGED and the provider flips submit count
			// (and reallocs the tile to full-res 2D).
			if (s_ps.pfn_request_rendering_mode) {
				const XrDisplayRenderingModeInfoDXR *cur = ps_find_mode(s_ps.active_mode_index);
				int cur_is3d = cur ? (cur->hardwareDisplay3D ? 1 : 0) : 1;
				if (cur_is3d != s_ps.ev_hw3d) {
					for (uint32_t i = 0; i < s_ps.mode_count; i++) {
						if ((s_ps.modes[i].hardwareDisplay3D ? 1 : 0) == s_ps.ev_hw3d &&
						    s_ps.modes[i].isRequestable) {
							s_ps.pfn_request_rendering_mode(s_ps.session, s_ps.modes[i].modeIndex);
							ps_log("[DisplayXR-PROV] hw display %s -> couple rendering mode idx %u (%u-view)\n",
							       s_ps.ev_hw3d ? "3D" : "2D", s_ps.modes[i].modeIndex,
							       s_ps.modes[i].viewCount);
							break;
						}
					}
				}
			}
		} else if (ev.type == XR_TYPE_EVENT_DATA_EYE_TRACKING_STATE_CHANGED_DXR) {
			XrEventDataEyeTrackingStateChangedDXR *tc = (XrEventDataEyeTrackingStateChangedDXR *)&ev;
			s_ps.ev_track_is_tracking = tc->isTracking ? 1 : 0;
			s_ps.ev_track_mode = (int)tc->activeMode;
			s_ps.ev_track_changed = 1;
			ps_log("[DisplayXR-PROV] event: eye tracking -> %d (mode %d)\n",
			       s_ps.ev_track_is_tracking, s_ps.ev_track_mode);
		}
		ev = {XR_TYPE_EVENT_DATA_BUFFER};
	}
}

#ifdef _WIN32
static void ps_diag_preclear_bridge_green(void); // defined near submit_frame (probe)
#endif

int dxr_prov_begin_frame(uint32_t *out_image_index, int *out_should_render)
{
	if (out_should_render) *out_should_render = 0;
	if (out_image_index) *out_image_index = 0;
	if (!s_ps.running || !s_ps.session_ready) return 0;

	// D3D11 zero-copy (#195): short startup trace of the begin stages (waitFrame /
	// beginFrame / acquire+wait) to confirm the frame loop came up. Bump the gate to
	// debug the editor Play-Mode deadlock (~75 frames; player is unaffected).
	static unsigned d11_bframes = 0;
	int bdiag = (s_ps.graphics_api == DXR_GFX_D3D11) && (d11_bframes < 3);
	if (bdiag) ps_log("[DisplayXR-PROV] D3D11 begin[%u]: pre-waitFrame\n", d11_bframes);

	XrFrameState fs = {XR_TYPE_FRAME_STATE};
	if (XR_FAILED(s_ps.pfn_wait_frame(s_ps.session, NULL, &fs))) return 0;
	if (bdiag) ps_log("[DisplayXR-PROV] D3D11 begin[%u]: waitFrame ok, pre-beginFrame\n", d11_bframes);
	if (XR_FAILED(s_ps.pfn_begin_frame(s_ps.session, NULL))) return 0;
	if (bdiag) ps_log("[DisplayXR-PROV] D3D11 begin[%u]: beginFrame ok\n", d11_bframes);
	s_ps.predicted_display_time = fs.predictedDisplayTime;
	s_ps.frame_begun = 1;

	// --- Per-frame render rect. In a 3D-zone frame the swapchain IS zone-sized, so
	//     render == swapchain (full slice, no sub-rect). Otherwise render =
	//     window(overlay client) × active-mode scaleXY, clamped to the swapchain. ---
	if (ps_zone_active()) {
		s_ps.render_w = s_ps.sc_width;
		s_ps.render_h = s_ps.sc_height;
	} else {
		uint32_t ww, wh; ps_window_size(&ww, &wh);
		float sx, sy; ps_active_view_scale(&sx, &sy);
		uint32_t rw = (uint32_t)(ww * sx);
		uint32_t rh = (uint32_t)(wh * sy);
		if (rw == 0) rw = 1;
		if (rh == 0) rh = 1;
		if (s_ps.sc_width  && rw > s_ps.sc_width)  rw = s_ps.sc_width;
		if (s_ps.sc_height && rh > s_ps.sc_height) rh = s_ps.sc_height;
		s_ps.render_w = rw;
		s_ps.render_h = rh;
	}

	// --- Locate views (XR_DXR_view_rig chained → render-ready pose+fov) ---
	s_ps.view_count = 0;
	if (s_ps.local_space != XR_NULL_HANDLE && s_ps.pfn_locate_views) {
		XrViewLocateInfo li = {XR_TYPE_VIEW_LOCATE_INFO};
		li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
		li.displayTime = fs.predictedDisplayTime;
		li.space = s_ps.local_space;

		XrView views[PS_MAX_VIEWS];
		for (int i = 0; i < PS_MAX_VIEWS; i++) views[i] = {XR_TYPE_VIEW};
		XrViewState vstate = {XR_TYPE_VIEW_STATE};
		uint32_t vcount = 0;

		// XR_DXR_view_rig: chain the rig descriptor matching the active rig mode
		// (mirrors displayxr_standalone.cpp). Camera-centric → XrCameraRigDXR;
		// display-centric → XrDisplayRigDXR. The runtime returns render-ready
		// XrView{pose,fov}; the raw channel recovers the raw display-space eyes.
		XrDisplayRigDXR display_rig = {XR_TYPE_DISPLAY_RIG_DXR};
		XrCameraRigDXR  camera_rig  = {XR_TYPE_CAMERA_RIG_DXR};
		XrDisplayZoneDXR locate_zone = {XR_TYPE_DISPLAY_ZONE_DXR};
		XrViewDisplayRawDXR raw = {XR_TYPE_VIEW_DISPLAY_RAW_DXR};
		XrPosef rig_pose = s_ps.display_pose_set ? s_ps.display_pose
		                                         : XrPosef{{0, 0, 0, 1}, {0, 0, 0}};
		if (s_ps.has_view_rig) {
			if (s_ps.camera_centric) {
				camera_rig.pose = rig_pose;
				camera_rig.ipdFactor = s_ps.tunables_set ? s_ps.ipd_factor : 1.0f;
				camera_rig.parallaxFactor = s_ps.tunables_set ? s_ps.parallax_factor : 1.0f;
				camera_rig.convergenceDiopters = s_ps.inv_convergence_distance;
				camera_rig.verticalFov = 2.0f * atanf(s_ps.fov_override);
				camera_rig.metersToVirtual = 1.0f; // spec v3: identity meters->world
				li.next = &camera_rig;
			} else {
				// Default virtualDisplayHeight to the physical display height (a
				// window-as-portal 1:1 framing) so a scene with no DisplayXR rig
				// pushing tunables still gets real stereo separation. Apps override
				// via dxr_prov_set_tunables (wired from DisplayXRDisplay).
				float vdh = s_ps.display_info.is_valid && s_ps.display_info.height_m > 0.0f
				            ? s_ps.display_info.height_m : 0.2f;
				display_rig.pose = rig_pose;
				// vdh <= 0 always means "use the physical display height" (matches the
				// rig's own resolution), so the C# driver can pass a raw 0 safely even
				// before its display-info query resolves.
				display_rig.virtualDisplayHeight =
				    (s_ps.tunables_set && s_ps.virtual_display_height > 0.0f)
				        ? s_ps.virtual_display_height : vdh;
				display_rig.ipdFactor = s_ps.tunables_set ? s_ps.ipd_factor : 1.0f;
				display_rig.parallaxFactor = s_ps.tunables_set ? s_ps.parallax_factor : 1.0f;
				display_rig.perspectiveFactor = s_ps.tunables_set ? s_ps.perspective_factor : 1.0f;
				li.next = &display_rig;
			}
			// Zones (#166 Phase B): chain XrDisplayZoneDXR in FRONT of the rig so the
			// runtime frames the Kooima into the zone rect (the rect IS the canvas).
			// Same instance is chained on the projection layer at submit.
			if (ps_zone_active()) {
				locate_zone.zoneId = s_ps.zone_id ? s_ps.zone_id : 1;
				locate_zone.rect.offset = {s_ps.zone_x, s_ps.zone_y};
				locate_zone.rect.extent = {s_ps.zone_w, s_ps.zone_h};
				locate_zone.next = li.next; // the rig
				li.next = &locate_zone;
			}
			vstate.next = &raw;
		}

		if (XR_SUCCEEDED(s_ps.pfn_locate_views(s_ps.session, &li, &vstate,
		                                       PS_MAX_VIEWS, &vcount, views)) && vcount >= 1) {
			uint32_t n = vcount < DXR_PROV_MAX_VIEWS ? vcount : DXR_PROV_MAX_VIEWS;
			for (uint32_t i = 0; i < n; i++) {
				s_ps.views[i].position[0] = views[i].pose.position.x;
				s_ps.views[i].position[1] = views[i].pose.position.y;
				s_ps.views[i].position[2] = views[i].pose.position.z;
				s_ps.views[i].orientation[0] = views[i].pose.orientation.x;
				s_ps.views[i].orientation[1] = views[i].pose.orientation.y;
				s_ps.views[i].orientation[2] = views[i].pose.orientation.z;
				s_ps.views[i].orientation[3] = views[i].pose.orientation.w;
				s_ps.views[i].fov[0] = views[i].fov.angleLeft;
				s_ps.views[i].fov[1] = views[i].fov.angleRight;
				s_ps.views[i].fov[2] = views[i].fov.angleUp;
				s_ps.views[i].fov[3] = views[i].fov.angleDown;
				s_ps.views[i].valid = s_ps.has_view_rig ? 1 : 0;
			}
			// Ensure 2 views for stereo (duplicate if runtime returned 1).
			if (n == 1) { s_ps.views[1] = s_ps.views[0]; n = 2; }
			s_ps.view_count = n;

			// Publish the RAW (pre-Kooima) display-space eyes to shared state so
			// the editor Scene-view gizmo (DisplayXRGizmoHelpers.TryGetLiveRawEyes)
			// tracks the head (#189). The gizmo re-applies Kooima itself, so it
			// wants the raw eyes from the XR_DXR_view_rig raw channel (chained on
			// vstate.next above), NOT the render-ready views[i].pose — matching the
			// legacy SA contract (displayxr_standalone.cpp:1603-1618). Only valid
			// when the rig is chained; otherwise `raw` is a zero-init struct and
			// is_tracked stays 0 → the gizmo falls back to nominal (correct).
			if (s_ps.has_view_rig) {
				uint32_t raw_n = raw.eyeCountOutput;
				if (raw_n == 0) raw_n = n; // tolerate a runtime that skipped the raw fill
				if (raw_n > XR_VIEW_RIG_MAX_RAW_EYES_DXR) raw_n = XR_VIEW_RIG_MAX_RAW_EYES_DXR;
				XrVector3f left  = raw.rawEyes[0];
				XrVector3f right = raw.rawEyes[raw_n >= 2 ? 1 : 0];
				displayxr_state_set_eye_positions(&left, &right, raw.isTracking ? 1 : 0);

				// Publish the Kooima canvas (window on the panel: panel-pixel rect
				// + physical size in meters) so the editor Scene-view gizmo draws
				// the window-relative eyes + the convergence-plane aspect the
				// runtime's off-axis projection actually uses (#189). The raw
				// channel fills these each frame — the rect offset tracks a window
				// move, the extent + sizeMeters track a resize.
				DisplayXRKooimaCanvas kc;
				kc.rect_x = raw.canvasRectPx.offset.x;
				kc.rect_y = raw.canvasRectPx.offset.y;
				kc.rect_w = raw.canvasRectPx.extent.width;
				kc.rect_h = raw.canvasRectPx.extent.height;
				kc.size_meters_w = raw.canvasSizeMeters.width;
				kc.size_meters_h = raw.canvasSizeMeters.height;
				kc.is_valid = (kc.rect_w > 0 && kc.rect_h > 0 &&
				               kc.size_meters_w > 0.0f && kc.size_meters_h > 0.0f) ? 1 : 0;
				displayxr_state_set_kooima_canvas(&kc);
			}
		}
	}

	// Publish stereo matrices for the transparent overlay's cyclopean hit-test
	// (LMB drag on the silhouette) — inert on the hook path, which owns this state.
	ps_publish_stereo_matrices();

	// Locate the extra 3D zones (each zone-scoped) + acquire their swapchain images.
	ps_locate_extra_zones(fs.predictedDisplayTime);

	// --- Acquire + wait the swapchain image Unity renders into next ---
	// Guard against a double-acquire: if a previous frame acquired an image but
	// never submitted (Unity may call PopulateNextFrameDesc without a matching
	// SubmitCurrentFrame on startup), reuse it rather than acquire again
	// (xrWaitSwapchainImage would return XR_ERROR_CALL_ORDER_INVALID).
	if (s_ps.swapchain_created && !s_ps.image_acquired) {
		uint32_t index = 0;
		XrSwapchainImageAcquireInfo ai = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
		if (bdiag) ps_log("[DisplayXR-PROV] D3D11 begin[%u]: pre-acquire\n", d11_bframes);
		if (XR_SUCCEEDED(s_ps.pfn_acquire_swapchain_image(s_ps.swapchain, &ai, &index))) {
			if (bdiag) ps_log("[DisplayXR-PROV] D3D11 begin[%u]: acquired idx=%u, pre-wait\n", d11_bframes, index);
			XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
			wi.timeout = 1000000000;
			s_ps.pfn_wait_swapchain_image(s_ps.swapchain, &wi);
			if (bdiag) ps_log("[DisplayXR-PROV] D3D11 begin[%u]: wait done idx=%u\n", d11_bframes, index);
			s_ps.image_acquired = 1;
			s_ps.acquired_index = index;
		}
	}
	if (out_image_index) *out_image_index = s_ps.acquired_index;

	if (out_should_render) *out_should_render = fs.shouldRender ? 1 : 0;
	if (s_ps.graphics_api == DXR_GFX_D3D11 && d11_bframes < 100000) d11_bframes++;
#ifdef _WIN32
	// PROBE: pre-clear the bridges to GREEN before Unity renders this frame, so the submit
	// readback reveals whether Unity actually draws into the XR eye textures. D3D12 only.
	if (s_ps.graphics_api != DXR_GFX_D3D11) {
		static int s_pc = -1;
		if (s_pc < 0) { const char *e = getenv("DISPLAYXR_PROV_BRIDGE_PRECLEAR");
		                s_pc = (e && e[0] && e[0] != '0') ? 1 : 0; }
		if (s_pc) ps_diag_preclear_bridge_green();
	}
#endif
	return 1;
}

void dxr_prov_get_view(uint32_t view_index, DxrProvView *out_view)
{
	if (!out_view) return;
	if (view_index >= DXR_PROV_MAX_VIEWS) { memset(out_view, 0, sizeof(*out_view)); return; }
	*out_view = s_ps.views[view_index];
}

// View matrix (column-major, OpenXR convention) from an eye pose — replicates the
// hook path's dxr_view_matrix_from_pose so the provider publishes matrices in the
// EXACT convention the transparent overlay's cyclopean hit-test expects.
static void ps_view_matrix_from_pose(const float pos[3], const float q[4], float *out)
{
	const float qx = q[0], qy = q[1], qz = q[2], qw = q[3];
	float rot[16] = {};
	rot[0] = 1.0f - 2.0f * (qy * qy + qz * qz);
	rot[1] = 2.0f * (qx * qy + qz * qw);
	rot[2] = 2.0f * (qx * qz - qy * qw);
	rot[4] = 2.0f * (qx * qy - qz * qw);
	rot[5] = 1.0f - 2.0f * (qx * qx + qz * qz);
	rot[6] = 2.0f * (qy * qz + qx * qw);
	rot[8] = 2.0f * (qx * qz + qy * qw);
	rot[9] = 2.0f * (qy * qz - qx * qw);
	rot[10] = 1.0f - 2.0f * (qx * qx + qy * qy);
	rot[15] = 1.0f;
	for (int i = 0; i < 16; i++) out[i] = 0.0f;
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++)
			out[j * 4 + i] = rot[i * 4 + j]; // R^T
	out[15] = 1.0f;
	out[12] = -(out[0] * pos[0] + out[4] * pos[1] + out[8] * pos[2]);
	out[13] = -(out[1] * pos[0] + out[5] * pos[1] + out[9] * pos[2]);
	out[14] = -(out[2] * pos[0] + out[6] * pos[1] + out[10] * pos[2]);
}

// Projection (column-major, GL clip) from an XrFovf — replicates dxr_projection_from_fov.
// fov[] = {angleLeft, angleRight, angleUp, angleDown} (the DxrProvView layout).
static void ps_projection_from_fov(const float fov[4], float nz, float fz, float *out)
{
	const float l = tanf(fov[0]) * nz;
	const float r = tanf(fov[1]) * nz;
	const float t = tanf(fov[2]) * nz;
	const float b = tanf(fov[3]) * nz;
	for (int i = 0; i < 16; i++) out[i] = 0.0f;
	out[0] = 2.0f * nz / (r - l);
	out[5] = 2.0f * nz / (t - b);
	out[8] = (r + l) / (r - l);
	out[9] = (t + b) / (t - b);
	out[10] = -(fz + nz) / (fz - nz);
	out[11] = -1.0f;
	out[14] = -2.0f * fz * nz / (fz - nz);
}

// Build a column-major GL-clip projection matrix from an XrFovf using the provider's
// current near/far. Lets the frame-desc builder hand Unity a FULL projection matrix
// (kUnityXRProjectionTypeMatrix) instead of half-angle tangents — an experiment to
// see whether URP then consumes the off-center projection correctly on its own
// (bypassing URP's buggy fov->matrix rebuild), which would let us retire the URP
// KooimaProjectionFixFeature. BiRP + HDRP already render the half-angles correctly;
// this must not regress them. near/far come from the rig (same source as the
// stereo-readback matrices) so the matrix matches Unity's camera frustum.
void dxr_prov_build_projection(const float fov[4], float *out16)
{
	float nz = s_ps.near_z > 0.0f ? s_ps.near_z : 0.01f;
	float fz = s_ps.far_z > nz ? s_ps.far_z : 1000.0f;
	ps_projection_from_fov(fov, nz, fz, out16);
}

// Publish the per-eye view+proj to the hook's shared stereo-matrices state so the
// transparent overlay's cyclopean hit-test (DisplayXRTransparentOverlay, via
// displayxr_get_stereo_matrices) works in provider mode — DisplayXRFeature.
// GetStereoMatrices is inert without Unity's OpenXR loader. Same math as the hook.
static void ps_publish_stereo_matrices(void)
{
	if (s_ps.view_count < 2) return;
	DisplayXRStereoMatrices mats = {};
	float nz = s_ps.near_z > 0.0f ? s_ps.near_z : 0.01f;
	float fz = s_ps.far_z > nz ? s_ps.far_z : 1000.0f;
	ps_view_matrix_from_pose(s_ps.views[0].position, s_ps.views[0].orientation, mats.left_view);
	ps_view_matrix_from_pose(s_ps.views[1].position, s_ps.views[1].orientation, mats.right_view);
	ps_projection_from_fov(s_ps.views[0].fov, nz, fz, mats.left_projection);
	ps_projection_from_fov(s_ps.views[1].fov, nz, fz, mats.right_projection);
	mats.valid = 1;
	displayxr_state_set_stereo_matrices(&mats);
}

// Foreground clip (#166 Phase B / #57): z of (rigPose^-1 * eyeWorld) = the eye's
// signed distance to the display plane along the rig forward. Copied verbatim from
// the hook path (dxr_rig_local_eye_z) so the provider computes the SAME per-view far.
static float ps_rig_local_eye_z(const XrPosef *rig, const float eye[3])
{
	const float dx = eye[0] - rig->position.x;
	const float dy = eye[1] - rig->position.y;
	const float dz = eye[2] - rig->position.z;
	const float qx = -rig->orientation.x, qy = -rig->orientation.y;
	const float qz = -rig->orientation.z, qw = rig->orientation.w;
	const float cx = qy * dz - qz * dy + qw * dx;
	const float cy = qz * dx - qx * dz + qw * dy;
	return dz + 2.0f * (qx * cy - qy * cx);
}

// Compute the per-view foreground clip (far + Unity-space eye pos) for one eye world
// position `p`. Eye pos in Unity coords (oxr_to_unity_pos = x, y, -z) — matches the
// deviceAnchorToEyePose the provider hands Unity, aligning with the shader's
// UNITY_MATRIX_I_V eye position (_DXREyePosL/R). Far = display-plane distance
// (display rig = |rig-local eye Z|) or convergence distance (camera rig).
static void ps_compute_eye_clip(const float *p, float *out_far,
                                float *out_ex, float *out_ey, float *out_ez)
{
	if (out_ex) *out_ex = p[0];
	if (out_ey) *out_ey = p[1];
	if (out_ez) *out_ez = -p[2];
	float far_eff = s_ps.far_z > 0.0f ? s_ps.far_z : 1000.0f;
	float near_z = s_ps.near_z > 0.0f ? s_ps.near_z : 0.01f;
	if (s_ps.camera_centric) {
		if (s_ps.inv_convergence_distance > 1e-4f) far_eff = 1.0f / s_ps.inv_convergence_distance;
	} else {
		XrPosef rig = s_ps.display_pose_set ? s_ps.display_pose
		                                    : XrPosef{{0, 0, 0, 1}, {0, 0, 0}};
		float ez = fabsf(ps_rig_local_eye_z(&rig, p));
		if (ez > near_z + 0.001f) far_eff = ez;
	}
	if (out_far) *out_far = far_eff;
}

void dxr_prov_get_eye_clip(uint32_t eye, float *out_far,
                           float *out_ex, float *out_ey, float *out_ez)
{
	if (eye >= DXR_PROV_MAX_VIEWS) eye = 0;
	ps_compute_eye_clip(s_ps.views[eye].position, out_far, out_ex, out_ey, out_ez);
}

// Per-zone per-eye foreground clip (#166). Zone 0 = primary; i>=1 = extra_zones[i-1].
// Returns 1 on success. Lets the clip publisher diagnose / drive per-zone clip data.
int dxr_prov_get_zone_eye_clip(uint32_t zone, uint32_t eye, float *out_far,
                               float *out_ex, float *out_ey, float *out_ez)
{
	if (eye >= 2) eye = 0;
	const float *p;
	if (zone == 0) {
		if (s_ps.view_count < 2) return 0;
		p = s_ps.views[eye].position;
	} else {
		uint32_t ei = zone - 1;
		if (ei >= s_ps.extra_zone_count) return 0;
		ProviderExtraZone *z = &s_ps.extra_zones[ei];
		if (!z->valid || z->view_count < 2) return 0;
		p = z->views[eye].position;
	}
	ps_compute_eye_clip(p, out_far, out_ex, out_ey, out_ez);
	return 1;
}

// ============================================================================
// Per-zone stereo matrices + screen rect (#166 — multi-zone transparent mask).
// The transparent overlay's SetWindowRgn silhouette must cover EVERY zone's
// content, not just the primary — otherwise the OS clips non-primary zones to
// see-through. Expose each zone's cyclopean view+proj (same math as
// ps_publish_stereo_matrices) and its window-pixel rect so the overlay can
// render the silhouette per-zone into that zone's sub-rect and union the result.
// Zone 0 = primary (s_ps.views / primary zone rect); zone i>=1 = extra_zones[i-1].
// ============================================================================
uint32_t dxr_prov_get_zone_count(void)
{
	uint32_t n = 1; // primary always present
	for (uint32_t i = 0; i < s_ps.extra_zone_count; i++)
		if (s_ps.extra_zones[i].valid && s_ps.extra_zones[i].view_count >= 2) n++;
	return n;
}

int dxr_prov_get_zone_stereo_matrices(uint32_t zone, float *lv, float *lp,
                                      float *rv, float *rp)
{
	float nz = s_ps.near_z > 0.0f ? s_ps.near_z : 0.01f;
	float fz = s_ps.far_z > nz ? s_ps.far_z : 1000.0f;
	const DxrProvView *v;
	if (zone == 0) {
		if (s_ps.view_count < 2) return 0;
		v = s_ps.views;
	} else {
		uint32_t ei = zone - 1;
		if (ei >= s_ps.extra_zone_count) return 0;
		ProviderExtraZone *z = &s_ps.extra_zones[ei];
		if (!z->valid || z->view_count < 2) return 0;
		v = z->views;
	}
	if (lv) ps_view_matrix_from_pose(v[0].position, v[0].orientation, lv);
	if (rv) ps_view_matrix_from_pose(v[1].position, v[1].orientation, rv);
	if (lp) ps_projection_from_fov(v[0].fov, nz, fz, lp);
	if (rp) ps_projection_from_fov(v[1].fov, nz, fz, rp);
	return 1;
}

int dxr_prov_get_zone_rect_px(uint32_t zone, int *x, int *y, int *w, int *h)
{
	if (zone == 0) {
		if (ps_zone_active()) {
			if (x) *x = s_ps.zone_x; if (y) *y = s_ps.zone_y;
			if (w) *w = s_ps.zone_w; if (h) *h = s_ps.zone_h;
		} else {
			uint32_t ww = 0, wh = 0; ps_window_size(&ww, &wh);
			if (x) *x = 0; if (y) *y = 0;
			if (w) *w = (int)ww; if (h) *h = (int)wh;
		}
		return 1;
	}
	uint32_t ei = zone - 1;
	if (ei >= s_ps.extra_zone_count) return 0;
	ProviderExtraZone *z = &s_ps.extra_zones[ei];
	if (!z->valid) return 0;
	if (x) *x = z->rect_x; if (y) *y = z->rect_y;
	if (w) *w = z->rect_w; if (h) *h = z->rect_h;
	return 1;
}

// DIAGNOSTIC (DISPLAYXR_PROV_SLICE_COLORS=1): record clears of the acquired swapchain image's
// array slice 0 = solid BLUE and slice 1 = solid RED onto the own command list (caller has
// already Reset it and will Close + execute). Used to test whether the runtime's texture-mode
// weave reads BOTH array slices (→ a blue/red interlace) or samples slice 0 for both views
// (→ a flat all-blue field, the "sliced swapchain mishandled in shared-texture case"
// hypothesis). RTV heap created lazily on the own device; never freed (diagnostic-only).
// (macOS: D3D12-only diagnostic; the sole caller is already _WIN32-guarded.)
#ifdef _WIN32
static void ps_diag_fill_slice_colors(ID3D12Resource *dst, UINT n)
{
	if (!dst || !s_ps.own_device || !s_ps.own_cmd_list) return;
	static ID3D12DescriptorHeap *rtv_heap = NULL;
	static UINT rtv_size = 0;
	if (!rtv_heap) {
		D3D12_DESCRIPTOR_HEAP_DESC hd = {};
		hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		hd.NumDescriptors = 2;
		hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
		if (FAILED(s_ps.own_device->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap),
		        (void **)&rtv_heap)) || !rtv_heap) {
			ps_log("[DisplayXR-PROV] PROBE: slice-colors RTV heap create failed\n"); return;
		}
		rtv_size = s_ps.own_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	}
	D3D12_RESOURCE_DESC rd = dst->GetDesc();
	// The runtime creates the swapchain image TYPELESS (fmt 27); an RTV needs a CONCRETE
	// format, so use the swapchain's typed format (s_ps.sc_format, e.g. 28 = R8G8B8A8_UNORM),
	// NOT rd.Format. Passing TYPELESS to CreateRenderTargetView faults the GPU (the crash).
	DXGI_FORMAT rtvFmt = (DXGI_FORMAT)s_ps.sc_format;
	if (rtvFmt == DXGI_FORMAT_UNKNOWN || rtvFmt == DXGI_FORMAT_R8G8B8A8_TYPELESS)
		rtvFmt = DXGI_FORMAT_R8G8B8A8_UNORM;
	// Safety: never RTV/clear an image that wasn't created render-target-capable (would fault).
	if (!(rd.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)) {
		static int warned = 0;
		if (!warned) { warned = 1;
			ps_log("[DisplayXR-PROV] PROBE: slice-colors SKIPPED — swapchain image not ALLOW_RENDER_TARGET (flags=0x%x)\n",
			       (unsigned)rd.Flags); }
		return;
	}
	const float colors[2][4] = { {0.0f, 0.0f, 1.0f, 1.0f},   // slice 0 = BLUE (left)
	                             {1.0f, 0.0f, 0.0f, 1.0f} };  // slice 1 = RED  (right)
	int swap = (n >= 2) && dxr_prov_view_swap(); // #740: mirror the real path's slot swap
	D3D12_CPU_DESCRIPTOR_HANDLE base = rtv_heap->GetCPUDescriptorHandleForHeapStart();
	for (UINT slice = 0; slice < n && slice < 2; slice++) {
		D3D12_RENDER_TARGET_VIEW_DESC rv = {};
		rv.Format = rtvFmt;
		rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
		rv.Texture2DArray.MipSlice = 0;
		rv.Texture2DArray.FirstArraySlice = slice;
		rv.Texture2DArray.ArraySize = 1;
		rv.Texture2DArray.PlaneSlice = 0;
		D3D12_CPU_DESCRIPTOR_HANDLE h = base; h.ptr += (SIZE_T)slice * rtv_size;
		s_ps.own_device->CreateRenderTargetView(dst, &rv, h);
		// XR_KHR_D3D12_enable contract (#747 bug 2): the acquired image is ALREADY in
		// RENDER_TARGET and must be RELEASED in RENDER_TARGET — clear directly, no
		// barriers (the old COMMON→RT→COMMON pair both mismatched the actual state and
		// released the image in the wrong state).
		UINT src_eye = swap ? (1 - slice) : slice; // #740 stereo unswap, mirror the real path
		s_ps.own_cmd_list->ClearRenderTargetView(h, colors[src_eye], 0, NULL);
	}
	static int logged = 0;
	if (!logged) { logged = 1;
		ps_log("[DisplayXR-PROV] PROBE: SLICE COLORS active — slice0=BLUE slice1=RED (n=%u fmt=%d)\n",
		       n, (int)rd.Format); }
}

// PROBE (DISPLAYXR_PROV_BRIDGE_COLORS=1): clear the copy-SOURCE bridge to solid colors
// (eye0=BLUE, eye1=RED) in our OWN command list, right before the normal bridge->swapchain
// -slice copy runs. The copy then carries whatever is in the bridge — so this splits the
// last ambiguity for the black workspace tile (#223 r10/r11): if the display shows the
// solid colors, the bridge->slice copy MECHANICS are fine and Unity's real render simply
// isn't in the bridge at copy time (ordering / wrong texture); if it's still black, the
// copy itself misses (wrong resource/slice). Unlike SLICE_COLORS (which clears the
// swapchain slice directly), this clears the SOURCE, so it exercises the exact copy path.
// The bridge is COMMON at copy time (submit's v8->RequestResourceState) — barrier
// COMMON->RENDER_TARGET to clear, then back to COMMON so the copy reads it (COPY_SOURCE
// promotion). Recorded on own_cmd_list before the copy, so it is GPU-ordered before it.
static void ps_diag_fill_bridge_colors(UINT n)
{
	if (!s_ps.own_device || !s_ps.own_cmd_list) return;
	static ID3D12DescriptorHeap *bheap = NULL;
	static UINT bsz = 0;
	if (!bheap) {
		D3D12_DESCRIPTOR_HEAP_DESC hd = {};
		hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = 2;
		if (FAILED(s_ps.own_device->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap),
		        (void **)&bheap)) || !bheap) {
			ps_log("[DisplayXR-PROV] PROBE: bridge-colors RTV heap failed\n"); return;
		}
		bsz = s_ps.own_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	}
	DXGI_FORMAT rtvFmt = (s_ps.sc_format == 87) ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
	const float colors[2][4] = { {0.0f, 0.0f, 1.0f, 1.0f},   // eye 0 = BLUE
	                             {1.0f, 0.0f, 0.0f, 1.0f} };  // eye 1 = RED
	int sp = dxr_prov_get_single_pass();
	D3D12_CPU_DESCRIPTOR_HANDLE base = bheap->GetCPUDescriptorHandleForHeapStart();
	for (UINT e = 0; e < n && e < 2; e++) {
		ID3D12Resource *br = sp ? s_ps.bridge_own : s_ps.bridge_own_eye[e];
		if (!br) continue;
		D3D12_RESOURCE_BARRIER b = {};
		b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		b.Transition.pResource = br;
		b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
		b.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
		s_ps.own_cmd_list->ResourceBarrier(1, &b);
		D3D12_RENDER_TARGET_VIEW_DESC rv = {};
		rv.Format = rtvFmt;
		rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
		rv.Texture2DArray.MipSlice = 0;
		rv.Texture2DArray.FirstArraySlice = sp ? e : 0;
		rv.Texture2DArray.ArraySize = 1;
		D3D12_CPU_DESCRIPTOR_HANDLE h = base; h.ptr += (SIZE_T)e * bsz;
		s_ps.own_device->CreateRenderTargetView(br, &rv, h);
		s_ps.own_cmd_list->ClearRenderTargetView(h, colors[e], 0, NULL);
		b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
		b.Transition.StateAfter  = D3D12_RESOURCE_STATE_COMMON;
		s_ps.own_cmd_list->ResourceBarrier(1, &b);
	}
	static int logged = 0;
	if (!logged) { logged = 1;
		ps_log("[DisplayXR-PROV] PROBE: BRIDGE COLORS active — bridge eye0=BLUE eye1=RED (copy carries to slices)\n"); }
}

// PROBE (DISPLAYXR_PROV_BRIDGE_READBACK=1): read the center pixel of the copy-SOURCE
// bridge (eye0) and log it, once, after warmup. Called at the END of submit_frame — FIFO
// after Unity's render + our copy on the (shell) shared queue — so it reads the FINISHED
// bridge. Splits the black tile (#223 r12) definitively: non-black ⇒ Unity DID render into
// the bridge (so the black slice is an ordering/sync bug in the copy); 00000000 ⇒ Unity is
// NOT rendering into the bridge we copy from (wrong texture wrap / render target).
static void ps_diag_readback_bridge_once(void)
{
	static int done = 0;
	static int frame = 0;
	if (done) return;
	if (++frame < 120) return; // warm eye tracking + real frames first
	done = 1;
	int sp = dxr_prov_get_single_pass();
	ID3D12Resource *br = sp ? s_ps.bridge_own : s_ps.bridge_own_eye[0];
	if (!br || !s_ps.own_device || !s_ps.own_cmd_list || !s_ps.own_queue) return;
	D3D12_RESOURCE_DESC rd = br->GetDesc();
	UINT rowPitch = (UINT)((rd.Width * 4 + 255) & ~255u);
	D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_READBACK;
	D3D12_RESOURCE_DESC bd = {};
	bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	bd.Width = rowPitch; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
	bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
	bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	ID3D12Resource *rb = NULL;
	if (FAILED(s_ps.own_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
	        D3D12_RESOURCE_STATE_COPY_DEST, NULL, __uuidof(ID3D12Resource), (void **)&rb)) || !rb) return;
	s_ps.own_cmd_alloc->Reset();
	s_ps.own_cmd_list->Reset(s_ps.own_cmd_alloc, NULL);
	D3D12_RESOURCE_BARRIER b = {}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	b.Transition.pResource = br; b.Transition.Subresource = 0;
	b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
	b.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
	s_ps.own_cmd_list->ResourceBarrier(1, &b);
	D3D12_TEXTURE_COPY_LOCATION dl = {}; dl.pResource = rb;
	dl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	dl.PlacedFootprint.Footprint.Format = (s_ps.sc_format == 87) ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
	dl.PlacedFootprint.Footprint.Width = (UINT)rd.Width;
	dl.PlacedFootprint.Footprint.Height = 1;
	dl.PlacedFootprint.Footprint.Depth = 1;
	dl.PlacedFootprint.Footprint.RowPitch = rowPitch;
	D3D12_TEXTURE_COPY_LOCATION slc = {}; slc.pResource = br;
	slc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; slc.SubresourceIndex = 0;
	D3D12_BOX box = {}; box.left = 0; box.right = (UINT)rd.Width;
	box.top = (UINT)rd.Height / 2; box.bottom = box.top + 1; box.front = 0; box.back = 1;
	s_ps.own_cmd_list->CopyTextureRegion(&dl, 0, 0, 0, &slc, &box);
	b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
	b.Transition.StateAfter  = D3D12_RESOURCE_STATE_COMMON;
	s_ps.own_cmd_list->ResourceBarrier(1, &b);
	s_ps.own_cmd_list->Close();
	ID3D12CommandList *lists[] = { s_ps.own_cmd_list };
	s_ps.own_queue->ExecuteCommandLists(1, lists);
	s_ps.own_fence_value++;
	s_ps.own_queue->Signal(s_ps.own_fence, s_ps.own_fence_value);
	if (s_ps.own_fence->GetCompletedValue() < s_ps.own_fence_value) {
		s_ps.own_fence->SetEventOnCompletion(s_ps.own_fence_value, s_ps.own_fence_event);
		WaitForSingleObject(s_ps.own_fence_event, INFINITE);
	}
	void *p = NULL; D3D12_RANGE mr = {0, rowPitch};
	if (SUCCEEDED(rb->Map(0, &mr, &p)) && p) {
		UINT8 *px = (UINT8 *)p + ((UINT)rd.Width / 2) * 4;
		ps_log("[DisplayXR-PROV] PROBE: BRIDGE READBACK eye0 center=(%u,%u) RGBA=%02x%02x%02x%02x "
		       "(nonzero => Unity rendered into bridge => ordering/sync bug; 00 => wrong texture)\n",
		       (UINT)rd.Width / 2, (UINT)rd.Height / 2, px[0], px[1], px[2], px[3]);
		rb->Unmap(0, NULL);
	}
	rb->Release();
}

// PROBE (DISPLAYXR_PROV_BRIDGE_PRECLEAR=1): clear BOTH per-eye bridges to GREEN and execute
// immediately — call from begin_frame, BEFORE Unity renders. Paired with the readback (end
// of submit): if the readback still reads GREEN, Unity never drew into the XR eye texture
// (stale/broken sample / camera not targeting XR); if it reads black/scene, Unity DID render.
static void ps_diag_preclear_bridge_green(void)
{
	if (!s_ps.own_device || !s_ps.own_cmd_list || !s_ps.own_queue) return;
	static ID3D12DescriptorHeap *gh = NULL; static UINT gsz = 0;
	if (!gh) {
		D3D12_DESCRIPTOR_HEAP_DESC hd = {}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = 2;
		if (FAILED(s_ps.own_device->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), (void **)&gh)) || !gh) return;
		gsz = s_ps.own_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	}
	DXGI_FORMAT rtvFmt = (s_ps.sc_format == 87) ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
	const float green[4] = {0.0f, 1.0f, 0.0f, 1.0f};
	int sp = dxr_prov_get_single_pass();
	s_ps.own_cmd_alloc->Reset();
	s_ps.own_cmd_list->Reset(s_ps.own_cmd_alloc, NULL);
	D3D12_CPU_DESCRIPTOR_HANDLE base = gh->GetCPUDescriptorHandleForHeapStart();
	auto clear_slice = [&](ID3D12Resource *br, UINT slice, UINT heapIdx) {
		if (!br) return;
		D3D12_RESOURCE_BARRIER b = {}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		b.Transition.pResource = br; b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON; b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
		s_ps.own_cmd_list->ResourceBarrier(1, &b);
		D3D12_RENDER_TARGET_VIEW_DESC rv = {}; rv.Format = rtvFmt;
		rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
		rv.Texture2DArray.FirstArraySlice = slice; rv.Texture2DArray.ArraySize = 1;
		D3D12_CPU_DESCRIPTOR_HANDLE h = base; h.ptr += (SIZE_T)heapIdx * gsz;
		s_ps.own_device->CreateRenderTargetView(br, &rv, h);
		s_ps.own_cmd_list->ClearRenderTargetView(h, green, 0, NULL);
		b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET; b.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
		s_ps.own_cmd_list->ResourceBarrier(1, &b);
	};
	if (sp) { clear_slice(s_ps.bridge_own, 0, 0); clear_slice(s_ps.bridge_own, 1, 1); } // SPI arr=2 bridge
	else    { clear_slice(s_ps.bridge_own_eye[0], 0, 0); clear_slice(s_ps.bridge_own_eye[1], 0, 1); } // MultiPass
	s_ps.own_cmd_list->Close();
	ID3D12CommandList *lists[] = { s_ps.own_cmd_list };
	s_ps.own_queue->ExecuteCommandLists(1, lists);
	s_ps.own_fence_value++;
	s_ps.own_queue->Signal(s_ps.own_fence, s_ps.own_fence_value);
	if (s_ps.own_fence->GetCompletedValue() < s_ps.own_fence_value) {
		s_ps.own_fence->SetEventOnCompletion(s_ps.own_fence_value, s_ps.own_fence_event);
		WaitForSingleObject(s_ps.own_fence_event, INFINITE);
	}
	static int logged = 0;
	if (!logged) { logged = 1; ps_log("[DisplayXR-PROV] PROBE: BRIDGE PRECLEAR green (before Unity render)\n"); }
}
#endif // _WIN32

int dxr_prov_submit_frame(uint32_t image_index)
{
	// Trace the submit path (shell/IPC bring-up: acquire spins, nothing ever released/
	// submitted). Written to %TEMP%\displayxr_prov_native.log.
	static unsigned s_subf_n = 0;
	int subf_trace = (s_subf_n < 12 || (s_subf_n % 600) == 0);
	if (subf_trace) {
		ps_log("[DisplayXR-PROV] submit_frame[%u]: frame_begun=%d swapchain_created=%d image_acquired=%d api=%d\n",
		       s_subf_n, s_ps.frame_begun, s_ps.swapchain_created, s_ps.image_acquired, (int)s_ps.graphics_api);
		// Render geometry: view count + per-eye FOV (rad) + render/swapchain dims. A degenerate
		// FOV (0 / huge) or 0-size render viewport → Unity draws nothing → black bridge (#223 r12).
		ps_log("[DisplayXR-PROV] submit_frame[%u]: view_count=%u render=%ux%u sc=%ux%u fov0=[%.3f %.3f %.3f %.3f] fov1=[%.3f %.3f %.3f %.3f]\n",
		       s_subf_n, s_ps.view_count, s_ps.render_w, s_ps.render_h, s_ps.sc_width, s_ps.sc_height,
		       s_ps.views[0].fov[0], s_ps.views[0].fov[1], s_ps.views[0].fov[2], s_ps.views[0].fov[3],
		       s_ps.views[1].fov[0], s_ps.views[1].fov[1], s_ps.views[1].fov[2], s_ps.views[1].fov[3]);
	}
	s_subf_n++;
	if (!s_ps.frame_begun || !s_ps.swapchain_created) {
		if (subf_trace) ps_log("[DisplayXR-PROV] submit_frame: BAIL (no frame/swapchain) -> end_frame_empty\n");
		dxr_prov_end_frame_empty();
		return 0;
	}
	s_ps.frame_begun = 0;

	uint32_t submit_n = s_ps.sc_view_count >= 2 ? 2 : 1;
	// D3D12: Unity rendered both eyes into the shared BRIDGE on its device (topology is
	// fixed at 2 views). Copy the SUBMITTED slices into the acquired runtime swapchain
	// image on our own device, fence-synced against Unity's render (shared fence below).
	// 3D: both slices. 2D (#172 P4): only slice 0 — submitting a single view avoids the two
	// eyes weaving as a ghost on the flat screen. SPI: 2-slice array bridge; MultiPass:
	// per-eye bridges.
	// D3D11 PLAYER (#195): ZERO-COPY — Unity rendered straight into the runtime's swapchain
	// image on its own device; there is nothing to copy and no cross-device fence. Unity and
	// the runtime share one device, so we Flush Unity's context before xrEndFrame (below) and
	// the runtime's native D3D11 compositor syncs internally via its own ID3D11Fence keyed to
	// xrReleaseSwapchainImage. Verified stable in a built player (~65fps, 2400+ frames).
	// D3D11 EDITOR bridge (#195): the session is bound on our OWN device, so Unity's render
	// lands in the shared bridge — copy it (own context) into the acquired swapchain image,
	// ordered after Unity's render by the shared fence. Mirrors the D3D12 arm below.
	// VULKAN (#247 Windows, #249 Linux): Unity rendered the eyes into the external-memory
	// bridge image(s) on ITS device; the same memory is aliased on the runtime's session
	// device. Order after Unity's render, then copy the session-side alias into the
	// acquired swapchain image. Structurally the D3D12 arm, with vkCmdCopyImage in place
	// of CopyTextureRegion. Platform-neutral, hence outside the per-OS branch.
#if defined(ENABLE_VULKAN)
	if (s_ps.graphics_api == DXR_GFX_VULKAN) {
		int sp = dxr_prov_get_single_pass();
		dxr_pvk_signal_unity_done(); // once per frame, before any copy
		if (sp) {
			dxr_pvk_copy_to_swapchain_image(-1, image_index);
		} else {
			for (uint32_t e = 0; e < submit_n; e++)
				dxr_pvk_copy_to_swapchain_image((int)e, image_index);
		}
	} else
#endif
#ifdef _WIN32
	if (s_ps.graphics_api == DXR_GFX_D3D11) {
		int sp = dxr_prov_get_single_pass();
		ID3D11Texture2D *dst = (image_index < s_ps.sc_image_count)
		                           ? s_ps.sc_images_d3d11[image_index].texture : NULL;
		if (s_ps.d3d11_bridge) {
			// EDITOR own-device bridge: own-context copy the shared own-side target(s) into the
			// acquired swapchain image, ordered after Unity's render by the shared fence.
			ps_d3d11_bridge_sync_before_copy(); // Unity signal + own wait (once/frame, before all copies)
			if (dst && s_ps.own_d3d11_context) {
				if (sp) {
					// SPI: CopyResource copies both array slices; 2D submits only slice 0 downstream.
					if (s_ps.d3d11_bridge_own)
						s_ps.own_d3d11_context->CopyResource(dst, s_ps.d3d11_bridge_own);
				} else {
					// MultiPass: copy each per-eye texture into swapchain array slice 0/1.
					for (UINT slice = 0; slice < submit_n; slice++) {
						if (s_ps.d3d11_bridge_own_eye[slice])
							s_ps.own_d3d11_context->CopySubresourceRegion(
							        dst, D3D11CalcSubresource(0, slice, 1), 0, 0, 0,
							        s_ps.d3d11_bridge_own_eye[slice], 0, NULL);
					}
				}
			}
		} else if (!sp) {
			// ZERO-COPY player MultiPass: Unity rendered each eye into its own plain Unity-device
			// texture; same-device-copy each into swapchain array slice 0/1 (no bridge/fence). SPI
			// zero-copy rendered straight into the image — nothing to copy. The Flush below orders
			// both against xrEndFrame.
			if (dst && s_ps.unity_d3d11_context) {
				for (UINT slice = 0; slice < submit_n; slice++) {
					if (s_ps.d3d11_bridge_unity_eye[slice])
						s_ps.unity_d3d11_context->CopySubresourceRegion(
						        dst, D3D11CalcSubresource(0, slice, 1), 0, 0, 0,
						        s_ps.d3d11_bridge_unity_eye[slice], 0, NULL);
				}
			}
		}
	} else if (s_ps.graphics_api != DXR_GFX_D3D11) {
		int sp = dxr_prov_get_single_pass();
		ID3D12Resource *copy_src = sp ? s_ps.bridge_own : s_ps.bridge_own_eye[0];
		if (copy_src && image_index < s_ps.sc_image_count &&
		    s_ps.sc_images[image_index].texture && s_ps.own_cmd_list) {
			ID3D12Resource *dst = s_ps.sc_images[image_index].texture;
			s_ps.own_cmd_alloc->Reset();
			s_ps.own_cmd_list->Reset(s_ps.own_cmd_alloc, NULL);
			// DIAGNOSTIC: instead of the real eye copies, paint slice 0 = BLUE, slice 1 = RED
			// so the woven output reveals whether the runtime's texture-mode weave reads both
			// array slices (blue/red interlace) or only slice 0 (flat all-blue). See helper.
			static int s_slice_colors = -1;
			if (s_slice_colors < 0) { const char *e = getenv("DISPLAYXR_PROV_SLICE_COLORS");
			                          s_slice_colors = (e && e[0] && e[0] != '0') ? 1 : 0; }
			if (s_slice_colors) {
				// Image arrives in RENDER_TARGET per the XR_KHR_D3D12_enable contract —
				// the diag clears directly and leaves it RT (no barriers, see helper).
				ps_diag_fill_slice_colors(dst, submit_n);
			} else {
				// PROBE: paint the copy-SOURCE bridge solid (BLUE/RED) before the copy, to
				// split "copy mechanics work / Unity render not in bridge" from "copy misses"
				// for the black tile (#223). Recorded before the copy on the same cmd list.
				static int s_bridge_colors = -1;
				if (s_bridge_colors < 0) { const char *e = getenv("DISPLAYXR_PROV_BRIDGE_COLORS");
				                           s_bridge_colors = (e && e[0] && e[0] != '0') ? 1 : 0; }
				if (s_bridge_colors) ps_diag_fill_bridge_colors(submit_n);
				ps_sc_image_barrier(s_ps.own_cmd_list, dst,
				                    D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST);
				int swap = (submit_n >= 2) && dxr_prov_view_swap(); // #740 stereo unswap
				for (UINT slice = 0; slice < submit_n; slice++) {
					UINT src_eye = swap ? (1 - slice) : slice; // submit views into opposite slots
					D3D12_TEXTURE_COPY_LOCATION dl = {};
					dl.pResource = dst;
					dl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
					dl.SubresourceIndex = slice;
					D3D12_TEXTURE_COPY_LOCATION sl = {};
					// SPI: slice `src_eye` of the array bridge. MultiPass: subresource 0 of the
					// per-eye bridge for `src_eye`.
					sl.pResource = sp ? s_ps.bridge_own : s_ps.bridge_own_eye[src_eye];
					sl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
					sl.SubresourceIndex = sp ? src_eye : 0;
					s_ps.own_cmd_list->CopyTextureRegion(&dl, 0, 0, 0, &sl, NULL);
				}
				ps_sc_image_barrier(s_ps.own_cmd_list, dst,
				                    D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
			}
			s_ps.own_cmd_list->Close();

			// Cross-device sync: signal a SHARED fence on Unity's queue (ordered after
			// Unity's already-submitted render into the bridge — Unity's queue is FIFO)
			// and GPU-wait on it on the own queue, so the copy reads the FINISHED render
			// (not empty texels → black).
			if (s_ps.shared_fence_unity && s_ps.shared_fence_own && s_ps.unity_queue) {
				s_ps.sync_val++;
				s_ps.unity_queue->Signal(s_ps.shared_fence_unity, s_ps.sync_val);
				s_ps.own_queue->Wait(s_ps.shared_fence_own, s_ps.sync_val);
			}

			ID3D12CommandList *lists[] = { s_ps.own_cmd_list };
			s_ps.own_queue->ExecuteCommandLists(1, lists);
			s_ps.own_fence_value++;
			s_ps.own_queue->Signal(s_ps.own_fence, s_ps.own_fence_value);
			if (s_ps.own_fence->GetCompletedValue() < s_ps.own_fence_value) {
				s_ps.own_fence->SetEventOnCompletion(s_ps.own_fence_value, s_ps.own_fence_event);
				WaitForSingleObject(s_ps.own_fence_event, INFINITE);
			}
		}
	}

	// D3D11 zero-copy (#195): Unity rendered into the swapchain image on the SAME device
	// the runtime weaves from. Flush Unity's immediate context so the runtime's weave in
	// xrEndFrame reads the FINISHED render (not mid-flight) — required for coherency on the
	// shared device. Short startup trace of the submit stages; bump the gate to debug the
	// editor Play-Mode deadlock (~75 frames).
	static unsigned d11_frames = 0;
	int d11_diag = (s_ps.graphics_api == DXR_GFX_D3D11) && (d11_frames < 3);
	if (s_ps.graphics_api == DXR_GFX_D3D11 && !s_ps.d3d11_bridge) {
		// Zero-copy only: Unity rendered into the swapchain image on the shared device.
		// (Editor bridge drains the OWN context before xrEndFrame instead — see below.)
		if (d11_diag) ps_log("[DisplayXR-PROV] D3D11 submit[%u]: pre-flush (img=%u)\n", d11_frames, image_index);
		if (s_ps.unity_d3d11_context) s_ps.unity_d3d11_context->Flush();
		if (d11_diag) ps_log("[DisplayXR-PROV] D3D11 submit[%u]: flushed, pre-release\n", d11_frames);
	}
#elif defined(__APPLE__)
	{
	// Metal ZERO-COPY (#204): Unity already rendered each eye straight into the
	// acquired image's slice views — nothing to copy. Order the compositor's
	// weave (encoded on the session queue at xrEndFrame) after Unity's renders
	// with encoder-less signal/wait CBs.
	if (s_ps.graphics_api == DXR_GFX_METAL)
		dxr_prov_metal_order_weave(s_ps.metal_queue);
	}
	static unsigned d11_frames = 0;
	int d11_diag = 0;
#else
	{
	// Linux (#249): Vulkan is the only backend and its arm above already did the
	// ordering (dxr_pvk_signal_unity_done) and the copy. Nothing further here — the
	// braces exist so the `else` above has a statement to bind to.
	}
	static unsigned d11_frames = 0;
	int d11_diag = 0;
#endif

	if (s_ps.image_acquired) {
		XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
		s_ps.pfn_release_swapchain_image(s_ps.swapchain, &ri);
		s_ps.image_acquired = 0;
		if (subf_trace) ps_log("[DisplayXR-PROV] submit_frame[%u]: RELEASED image %u\n", s_subf_n - 1, image_index);
	} else if (subf_trace) {
		ps_log("[DisplayXR-PROV] submit_frame[%u]: no image_acquired to release\n", s_subf_n - 1);
	}
	if (d11_diag) ps_log("[DisplayXR-PROV] D3D11 submit[%u]: released, building layers\n", d11_frames);

	// Build the projection layer. Submit sc_view_count views (#172 P4): 2 in stereo 3D
	// (array layers 0/1), 1 in hardware 2D (layer 0 only = a single full-res tile). The
	// runtime composites only the submitted views, so 2D shows one clean view (no weave
	// ghost) even though Unity rendered both eyes into the 2-slice bridge.
	uint32_t n = submit_n;
	if (n > s_ps.view_count) n = s_ps.view_count;
	if (n == 0) {
		if (subf_trace) ps_log("[DisplayXR-PROV] submit_frame[%u]: BAIL (n=0, submit_n=%u view_count=%u) -> end_frame_empty\n",
		                       s_subf_n - 1, submit_n, s_ps.view_count);
		dxr_prov_end_frame_empty(); return 0;
	}
	XrCompositionLayerProjectionView pv[DXR_PROV_MAX_VIEWS] = {};
	for (uint32_t eye = 0; eye < n; eye++) {
		pv[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
		pv[eye].pose.position = XrVector3f{ s_ps.views[eye].position[0],
		                                    s_ps.views[eye].position[1],
		                                    s_ps.views[eye].position[2] };
		pv[eye].pose.orientation = XrQuaternionf{ s_ps.views[eye].orientation[0],
		                                          s_ps.views[eye].orientation[1],
		                                          s_ps.views[eye].orientation[2],
		                                          s_ps.views[eye].orientation[3] };
		pv[eye].fov.angleLeft  = s_ps.views[eye].fov[0];
		pv[eye].fov.angleRight = s_ps.views[eye].fov[1];
		pv[eye].fov.angleUp    = s_ps.views[eye].fov[2];
		pv[eye].fov.angleDown  = s_ps.views[eye].fov[3];
		// SPI: each eye is array slice 0/1; imageRect is the active render
		// sub-region (window×scaleXY) within the worst-case-sized slice.
		uint32_t rw = s_ps.render_w ? s_ps.render_w : s_ps.sc_width;
		uint32_t rh = s_ps.render_h ? s_ps.render_h : s_ps.sc_height;
		pv[eye].subImage.swapchain = s_ps.swapchain;
		pv[eye].subImage.imageRect.offset = {0, 0};
		pv[eye].subImage.imageRect.extent = {(int32_t)rw, (int32_t)rh};
		pv[eye].subImage.imageArrayIndex = eye; // SPI: left=0, right=1
	}

	// Zones (#166 Phase B): bind the projection layer's views into the zone rect.
	// SAME instance/values as the locate chain; the submit values are authoritative.
	XrDisplayZoneDXR submit_zone = {XR_TYPE_DISPLAY_ZONE_DXR};
	// Per-zone opt-in edge feather (unity#238, spec v3): chained on the zone's
	// next at SUBMIT only (locate chains are ignored by the runtime). Structs
	// persist until xrEndFrame below, like the zones themselves.
	XrDisplayZoneFeatherDXR submit_zone_feather = {XR_TYPE_DISPLAY_ZONE_FEATHER_DXR};
	int zone_frame = ps_zone_active();
	if (zone_frame) {
		submit_zone.zoneId = s_ps.zone_id ? s_ps.zone_id : 1;
		submit_zone.rect.offset = {s_ps.zone_x, s_ps.zone_y};
		submit_zone.rect.extent = {s_ps.zone_w, s_ps.zone_h};
		if (s_app_zone_feather[0] > 0.0f) {
			submit_zone_feather.radiusPx = s_app_zone_feather[0];
			submit_zone.next = &submit_zone_feather;
		}
	}

	XrCompositionLayerProjection layer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
	layer.next = zone_frame ? (const void *)&submit_zone : NULL;
	// Transparent (ALPHA_BLEND) session: the runtime alpha-gates the projection
	// layer against the environment (compose-under-bg desktop) using the texture
	// alpha, so regions transparent in ALL views become pure see-through and the
	// desktop fill only shows in de-occluded regions. Without this flag the layer
	// reads as opaque-over-environment and the composed-under desktop bleeds through
	// everywhere at partial alpha. Matches Unity's own projection layer
	// (XrCompositionLayerFlags.SourceAlpha) and the runtime's cube_zones handle test.
	// Unity's eye texture is premultiplied (SourceAlpha, no UNPREMULTIPLIED bit).
	layer.layerFlags = (s_ps.transparent_requested && s_ps.alpha_blend_supported)
	        ? XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT : 0;
	layer.space = s_ps.local_space;
	layer.viewCount = n;
	layer.views = pv;

	// Extra 3D zones (#166 Phase B2): copy each bridge → its swapchain + build a
	// zone-chained projection layer. Structs persist until xrEndFrame below.
	XrCompositionLayerProjectionView extra_pv[PS_MAX_ZONES - 1][2] = {};
	XrCompositionLayerProjection     extra_layer[PS_MAX_ZONES - 1] = {};
	XrDisplayZoneDXR                 extra_zone_struct[PS_MAX_ZONES - 1] = {};
	XrDisplayZoneFeatherDXR          extra_zone_feather[PS_MAX_ZONES - 1] = {};
	int extra_has[PS_MAX_ZONES - 1] = {};
	for (uint32_t i = 0; i < s_ps.extra_zone_count; i++) {
		ProviderExtraZone *z = &s_ps.extra_zones[i];
		if (!z->valid || z->view_count < 2 || !z->swapchain_created) continue;
		ps_copy_extra_zone(z);
		for (uint32_t eye = 0; eye < 2; eye++) {
			extra_pv[i][eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
			extra_pv[i][eye].pose.position = XrVector3f{ z->views[eye].position[0], z->views[eye].position[1], z->views[eye].position[2] };
			extra_pv[i][eye].pose.orientation = XrQuaternionf{ z->views[eye].orientation[0], z->views[eye].orientation[1], z->views[eye].orientation[2], z->views[eye].orientation[3] };
			extra_pv[i][eye].fov.angleLeft  = z->views[eye].fov[0];
			extra_pv[i][eye].fov.angleRight = z->views[eye].fov[1];
			extra_pv[i][eye].fov.angleUp    = z->views[eye].fov[2];
			extra_pv[i][eye].fov.angleDown  = z->views[eye].fov[3];
			extra_pv[i][eye].subImage.swapchain = z->swapchain;
			extra_pv[i][eye].subImage.imageRect.offset = {0, 0};
			extra_pv[i][eye].subImage.imageRect.extent = {(int32_t)z->sc_width, (int32_t)z->sc_height};
			extra_pv[i][eye].subImage.imageArrayIndex = eye;
		}
		extra_zone_struct[i].type = XR_TYPE_DISPLAY_ZONE_DXR;
		extra_zone_struct[i].zoneId = z->zone_id;
		extra_zone_struct[i].rect.offset = {z->rect_x, z->rect_y};
		extra_zone_struct[i].rect.extent = {z->rect_w, z->rect_h};
		if (s_app_zone_feather[i + 1] > 0.0f) {
			extra_zone_feather[i].type = XR_TYPE_DISPLAY_ZONE_FEATHER_DXR;
			extra_zone_feather[i].radiusPx = s_app_zone_feather[i + 1];
			extra_zone_struct[i].next = &extra_zone_feather[i];
		}
		extra_layer[i].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION;
		extra_layer[i].next = &extra_zone_struct[i];
		extra_layer[i].layerFlags = (s_ps.transparent_requested && s_ps.alpha_blend_supported)
		        ? XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT : 0;
		extra_layer[i].space = s_ps.local_space;
		extra_layer[i].viewCount = 2;
		extra_layer[i].views = extra_pv[i];
		extra_has[i] = 1;
	}

	// Local2D band(s): post-weave 2D content at a client-window pixel rect,
	// composited over the woven 3D (the "glass over 3D" 2D zone). Inactive when no
	// rect/bridge is registered.
	XrCompositionLayerLocal2DDXR l2d_layer = {};
	int has_l2d = ps_submit_local2d(&l2d_layer);

	// Window-space UI (HUD). Composites over everything (fractional coords + disparity).
	XrCompositionLayerWindowSpaceDXR wsui_layer = {};
	int has_wsui = ps_submit_wsui(&wsui_layer);

	// Order: 3D projections (primary + extra zones) under, Local2D bands over the 3D,
	// HUD on top (canonical zones rule — 1 zone projection per 3D zone + 1 Local2D per
	// 2D band, all ALPHA_BLEND).
	const XrCompositionLayerBaseHeader *layers[PS_MAX_ZONES + 2];
	uint32_t lc = 0;
	layers[lc++] = (const XrCompositionLayerBaseHeader *)&layer;
	for (uint32_t i = 0; i < s_ps.extra_zone_count; i++)
		if (extra_has[i]) layers[lc++] = (const XrCompositionLayerBaseHeader *)&extra_layer[i];
	if (has_l2d)  layers[lc++] = (const XrCompositionLayerBaseHeader *)&l2d_layer;
	if (has_wsui) layers[lc++] = (const XrCompositionLayerBaseHeader *)&wsui_layer;

	// D3D11 (#195): make sure the runtime's weave in xrEndFrame reads FINISHED content.
	// EDITOR bridge: the primary + secondary copies all ran on the OWN immediate context —
	// drain it (flush + CPU wait). PLAYER zero-copy: the secondary layers same-device-copied
	// AFTER the primary flush above, so flush Unity's context once more (only if a secondary
	// layer is present).
#ifdef _WIN32
	if (s_ps.graphics_api == DXR_GFX_D3D11) {
		if (s_ps.d3d11_bridge) {
			if (d11_diag) ps_log("[DisplayXR-PROV] D3D11 submit[%u]: own-context drain (lc=%u)\n", d11_frames, lc);
			ps_d3d11_ctx_drain(s_ps.own_d3d11_context, s_ps.own_d3d11_device);
		} else if (s_ps.unity_d3d11_context && lc > 1) {
			if (d11_diag) ps_log("[DisplayXR-PROV] D3D11 submit[%u]: secondary-layer flush (lc=%u)\n", d11_frames, lc);
			s_ps.unity_d3d11_context->Flush();
		}
	}
#endif

	XrFrameEndInfo ei = {XR_TYPE_FRAME_END_INFO};
	ei.displayTime = s_ps.predicted_display_time;
	ei.environmentBlendMode = (s_ps.transparent_requested && s_ps.alpha_blend_supported)
	        ? XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND
	        : XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	ei.layerCount = lc;
	ei.layers = layers;
	if (d11_diag) ps_log("[DisplayXR-PROV] D3D11 submit[%u]: pre-xrEndFrame (layers=%u)\n", d11_frames, lc);
	if (subf_trace) ps_log("[DisplayXR-PROV] submit_frame[%u]: xrEndFrame layers=%u (views n=%u)\n", s_subf_n - 1, lc, n);
	XrResult r = s_ps.pfn_end_frame(s_ps.session, &ei);
	if (subf_trace) ps_log("[DisplayXR-PROV] submit_frame[%u]: post-xrEndFrame r=%d\n", s_subf_n - 1, r);
	if (d11_diag) ps_log("[DisplayXR-PROV] D3D11 submit[%u]: post-xrEndFrame r=%d\n", d11_frames, r);
	if (s_ps.graphics_api == DXR_GFX_D3D11 && d11_frames < 100000) d11_frames++;
	if (XR_FAILED(r)) { ps_log("[DisplayXR-PROV] xrEndFrame failed: %d\n", r); return 0; }

#ifdef _WIN32
	// PROBE (DISPLAYXR_PROV_BRIDGE_READBACK=1): once, read back the copy-source bridge to
	// tell whether Unity's render ever lands in it (ordering/sync bug) vs never (wrong
	// texture). D3D12 only.
	if (s_ps.graphics_api != DXR_GFX_D3D11) {
		static int s_brd = -1;
		if (s_brd < 0) { const char *e = getenv("DISPLAYXR_PROV_BRIDGE_READBACK");
		                 s_brd = (e && e[0] && e[0] != '0') ? 1 : 0; }
		if (s_brd) ps_diag_readback_bridge_once();
	}
#endif

#ifdef _WIN32  // weave-to-texture PROBE readback is Windows/D3D-only
	// Weave-to-texture PROBE: ON-DEMAND readback of the runtime-woven shared texture.
	// Touch %TEMP%\displayxr_woven_trigger to dump the CURRENT woven output (any frame,
	// e.g. while the Game tab is maximized) → displayxr_prov_woven_ondemand.bmp. Lets us
	// FFT the interlace phase top-vs-bottom to localize the maximize seam. Own-device,
	// coherent (same path as the one-shot below).
	if (s_probe_readback && s_probe_handle) {
		const char *tmp = getenv("TEMP"); if (!tmp || !*tmp) tmp = ".";
		char trig[512];
		_snprintf_s(trig, sizeof(trig), _TRUNCATE, "%s\\displayxr_woven_trigger", tmp);
		if (GetFileAttributesA(trig) != INVALID_FILE_ATTRIBUTES) {
			DeleteFileA(trig);
			char opath[512];
			_snprintf_s(opath, sizeof(opath), _TRUNCATE, "%s\\displayxr_prov_woven_ondemand.bmp", tmp);
			if (s_ps.graphics_api == DXR_GFX_D3D11) {
				ID3D11Device *dev = s_ps.d3d11_bridge ? s_ps.own_d3d11_device : s_ps.unity_d3d11_device;
				ID3D11DeviceContext *ctx = s_ps.d3d11_bridge ? s_ps.own_d3d11_context : s_ps.unity_d3d11_context;
				ps_probe_dump_d3d11(dev, ctx, s_probe_tex11, opath);
			} else {
				ps_probe_dump_d3d12(opath);
			}
		}
	}

	// Weave-to-texture PROBE: one-shot readback of the runtime-woven shared texture
	// after the warmup gate. The runtime wove into it during xrEndFrame above.
	if (s_probe_readback && s_probe_handle && !s_probe_dumped) {
		if (++s_probe_frames >= kProbeDumpFrame) {
			char path[512]; ps_probe_path(path, sizeof(path));
			if (s_ps.graphics_api == DXR_GFX_D3D11) {
				ID3D11Device *dev = s_ps.d3d11_bridge ? s_ps.own_d3d11_device
				                                      : s_ps.unity_d3d11_device;
				ID3D11DeviceContext *ctx = s_ps.d3d11_bridge ? s_ps.own_d3d11_context
				                                             : s_ps.unity_d3d11_context;
				ps_probe_dump_d3d11(dev, ctx, s_probe_tex11, path);
			} else {
				ps_probe_dump_d3d12(path);
			}
			// Localize the black: also dump the PRE-WEAVE eye texture (what Unity
			// rendered into the bridge). Eye has content + shared black => lost in
			// the weave/texture handoff. Eye also black => upstream (Unity render).
			if (s_ps.graphics_api == DXR_GFX_D3D11 && s_ps.d3d11_bridge && s_ps.d3d11_bridge_own_eye[0]) {
				const char *tmp = getenv("TEMP"); if (!tmp || !*tmp) tmp = ".";
				char epath[512];
				_snprintf_s(epath, sizeof(epath), _TRUNCATE, "%s\\displayxr_prov_eye0_preweave.bmp", tmp);
				ps_probe_dump_d3d11(s_ps.own_d3d11_device, s_ps.own_d3d11_context,
				                    s_ps.d3d11_bridge_own_eye[0], epath);
			}
			// Cross-device coherence check: dump the UNITY-side view of the woven
			// texture. If the own-side readback has content but this is black, the
			// GameView blit reads black because Unity's device doesn't see the
			// runtime's writes without a shared-fence sync.
			if (s_probe_tex_unity && s_ps.unity_d3d11_device) {
				ID3D11DeviceContext *uctx = NULL;
				s_ps.unity_d3d11_device->GetImmediateContext(&uctx);
				if (uctx) {
					const char *tmp = getenv("TEMP"); if (!tmp || !*tmp) tmp = ".";
					char upath[512];
					_snprintf_s(upath, sizeof(upath), _TRUNCATE, "%s\\displayxr_prov_woven_unityside.bmp", tmp);
					ps_probe_dump_d3d11(s_ps.unity_d3d11_device, uctx, s_probe_tex_unity, upath);
					uctx->Release();
				}
			}
			s_probe_dumped = 1;
		}
	}
#endif // _WIN32 — weave-to-texture PROBE readback
	return 1;
}

void dxr_prov_end_frame_empty(void)
{
	if (!s_ps.frame_begun) return;
	s_ps.frame_begun = 0;
	// Release any swapchain image acquired this frame BEFORE ending it. dxr_prov_begin_frame
	// acquires the primary image up front, but a frame that ends empty (shouldRender=false, or
	// no submittable views) skips dxr_prov_submit_frame — which is the only other place the
	// release happens. Leaving the image acquired is fatal on a 1-image swapchain (the shell/
	// IPC service compositor allocates exactly 1): the next xrAcquireSwapchainImage then fails
	// with CALL_ORDER_INVALID every frame, spinning the pump and submitting nothing (black tile).
	// Also release the extra 3D zones' images (acquired in ps_locate_extra_zones) for the same
	// reason — inert when no zones are active (the BiRP workspace tile).
	if (s_ps.image_acquired && s_ps.swapchain && s_ps.pfn_release_swapchain_image) {
		XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
		s_ps.pfn_release_swapchain_image(s_ps.swapchain, &ri);
		s_ps.image_acquired = 0;
	}
	for (uint32_t i = 0; i < s_ps.extra_zone_count && i < (PS_MAX_ZONES - 1); i++) {
		ProviderExtraZone *z = &s_ps.extra_zones[i];
		if (z->image_acquired && z->swapchain && s_ps.pfn_release_swapchain_image) {
			XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
			s_ps.pfn_release_swapchain_image(z->swapchain, &ri);
			z->image_acquired = 0;
		}
	}
	XrFrameEndInfo ei = {XR_TYPE_FRAME_END_INFO};
	ei.displayTime = s_ps.predicted_display_time;
	ei.environmentBlendMode = (s_ps.transparent_requested && s_ps.alpha_blend_supported)
	        ? XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND
	        : XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	ei.layerCount = 0;
	ei.layers = NULL;
	s_ps.pfn_end_frame(s_ps.session, &ei);
}

// ============================================================================
// Tunables / pose / queries
// ============================================================================

void dxr_prov_set_tunables(float ipd_factor, float parallax_factor,
                           float perspective_factor, float virtual_display_height,
                           float inv_convergence_distance, float fov_override,
                           float near_z, float far_z, int camera_centric)
{
	s_ps.ipd_factor = ipd_factor;
	s_ps.parallax_factor = parallax_factor;
	s_ps.perspective_factor = perspective_factor;
	s_ps.virtual_display_height = virtual_display_height;
	s_ps.inv_convergence_distance = inv_convergence_distance;
	s_ps.fov_override = fov_override;
	s_ps.near_z = near_z;            // owned by Unity's projection; stored, unused
	s_ps.far_z = far_z;
	s_ps.camera_centric = camera_centric;
	s_ps.tunables_set = 1;
}

void dxr_prov_set_display_pose(float px, float py, float pz,
                               float ox, float oy, float oz, float ow, int enabled)
{
	if (enabled) {
		// Unity (left-hand, +Z forward) → OpenXR (right-hand, -Z forward):
		// negate position Z, negate quaternion X/Y. Matches the standalone / hook.
		s_ps.display_pose.position = XrVector3f{px, py, -pz};
		s_ps.display_pose.orientation = XrQuaternionf{-ox, -oy, oz, ow};
		s_ps.display_pose_set = 1;
	} else {
		s_ps.display_pose_set = 0;
	}
}

// Expose the sent rig pose in the OpenXR frame (as stored). Returns 1 if a pose is
// set, 0 otherwise. Used by the render handoff to make deviceAnchorToEyePose
// rig-RELATIVE — the located views already bake this rig pose in, and Unity
// re-composes the same pose via the camera transform, so without subtracting it the
// rig origin is applied twice (drifts the URP foreground-clip plane; #166).
int dxr_prov_get_display_pose_oxr(float out_pos[3], float out_quat[4])
{
	if (!s_ps.display_pose_set) return 0;
	out_pos[0] = s_ps.display_pose.position.x;
	out_pos[1] = s_ps.display_pose.position.y;
	out_pos[2] = s_ps.display_pose.position.z;
	out_quat[0] = s_ps.display_pose.orientation.x;
	out_quat[1] = s_ps.display_pose.orientation.y;
	out_quat[2] = s_ps.display_pose.orientation.z;
	out_quat[3] = s_ps.display_pose.orientation.w;
	return 1;
}

void dxr_prov_get_display_info(DxrProvDisplayInfo *out_info)
{
	if (out_info) *out_info = s_ps.display_info;
}

int dxr_prov_has_view_rig(void) { return s_ps.has_view_rig; }

void dxr_prov_get_render_rect(uint32_t *out_w, uint32_t *out_h)
{
	if (out_w) *out_w = s_ps.render_w ? s_ps.render_w : s_ps.sc_width;
	if (out_h) *out_h = s_ps.render_h ? s_ps.render_h : s_ps.sc_height;
}

// ============================================================================
// Rendering modes (XR_DXR_display_info)
// ============================================================================

uint32_t dxr_prov_get_mode_count(void) { return s_ps.mode_count; }

int dxr_prov_get_mode_info(uint32_t index, DxrProvModeInfo *out_info)
{
	if (!out_info || index >= s_ps.mode_count) return 0;
	const XrDisplayRenderingModeInfoDXR *m = &s_ps.modes[index];
	out_info->mode_index = m->modeIndex;
	out_info->view_count = m->viewCount;
	out_info->tile_columns = m->tileColumns;
	out_info->tile_rows = m->tileRows;
	out_info->view_width_px = m->viewWidthPixels;
	out_info->view_height_px = m->viewHeightPixels;
	out_info->view_scale_x = m->viewScaleX;
	out_info->view_scale_y = m->viewScaleY;
	out_info->hardware_display_3d = (int)m->hardwareDisplay3D;
	out_info->is_active = (int)m->isActive;
	out_info->is_requestable = (int)m->isRequestable;
	strncpy(out_info->name, m->modeName, sizeof(out_info->name) - 1);
	out_info->name[sizeof(out_info->name) - 1] = '\0';
	return 1;
}

uint32_t dxr_prov_get_active_mode_index(void) { return s_ps.active_mode_index; }

int dxr_prov_request_rendering_mode(uint32_t mode_index)
{
	if (!s_ps.session || !s_ps.pfn_request_rendering_mode) return 0;
	XrResult r = s_ps.pfn_request_rendering_mode(s_ps.session, mode_index);
	ps_log("[DisplayXR-PROV] request rendering mode %u -> %d\n", mode_index, r);
	return XR_SUCCEEDED(r) ? 1 : 0;
}

int dxr_prov_request_display_mode(int mode3d)
{
	if (!s_ps.session || !s_ps.pfn_request_display_mode) return 0;
	XrResult r = s_ps.pfn_request_display_mode(s_ps.session,
	        mode3d ? XR_DISPLAY_MODE_3D_DXR : XR_DISPLAY_MODE_2D_DXR);
	ps_log("[DisplayXR-PROV] request display mode %s -> %d\n", mode3d ? "3D" : "2D", r);
	return XR_SUCCEEDED(r) ? 1 : 0;
}

int dxr_prov_set_eye_tracking_mode(int manual)
{
	if (!s_ps.session || !s_ps.pfn_request_eye_tracking_mode) return 0;
	XrResult r = s_ps.pfn_request_eye_tracking_mode(s_ps.session,
	        manual ? XR_EYE_TRACKING_MODE_MANUAL_DXR : XR_EYE_TRACKING_MODE_MANAGED_DXR);
	ps_log("[DisplayXR-PROV] request eye-tracking mode %s -> %d\n", manual ? "MANUAL" : "MANAGED", r);
	return XR_SUCCEEDED(r) ? 1 : 0;
}

// ---- Atlas capture (XR_DXR_atlas_capture, #140) -----------------------------

// Atlas capture: hand the runtime a path prefix + stage; it reads back its own
// compositor atlas and writes the PNG on the next composed frame. Non-blocking —
// XR_SUCCESS means accepted, not on-disk.
int dxr_prov_capture_atlas(const char *path_prefix, int stage)
{
	if (s_ps.pfn_capture_atlas == NULL || s_ps.session == XR_NULL_HANDLE) {
		ps_log("[DisplayXR-PROV] capture_atlas: unavailable (pfn=%p session=%p)\n",
		       (void *)s_ps.pfn_capture_atlas, (void *)(uintptr_t)s_ps.session);
		return 0; // Extension not resolved or no live session
	}

	XrAtlasCaptureInfoDXR info = {};
	info.type = XR_TYPE_ATLAS_CAPTURE_INFO_DXR;
	info.next = NULL;
	info.stage = (stage != 0) ? XR_ATLAS_CAPTURE_STAGE_PROJECTION_ONLY_DXR
	                          : XR_ATLAS_CAPTURE_STAGE_POST_COMPOSE_DXR;
	if (path_prefix != NULL) {
		strncpy(info.pathPrefix, path_prefix, XR_ATLAS_CAPTURE_PATH_MAX_DXR - 1);
		info.pathPrefix[XR_ATLAS_CAPTURE_PATH_MAX_DXR - 1] = '\0';
	}

	XrResult result = s_ps.pfn_capture_atlas(s_ps.session, &info, NULL);
	ps_log("[DisplayXR-PROV] capture_atlas: stage=%d prefix='%s' result=%d\n",
	       (int)info.stage, info.pathPrefix, (int)result);
	return XR_SUCCEEDED(result) ? 1 : 0;
}

// ---- Event latches (read-and-clear) -----------------------------------------

int dxr_prov_consume_mode_changed(uint32_t *prev_index, uint32_t *cur_index)
{
	if (!s_ps.ev_mode_changed) return 0;
	s_ps.ev_mode_changed = 0;
	if (prev_index) *prev_index = s_ps.ev_mode_prev;
	if (cur_index)  *cur_index = s_ps.ev_mode_cur;
	return 1;
}

int dxr_prov_consume_hw_state_changed(int *hw3d)
{
	if (!s_ps.ev_hw_changed) return 0;
	s_ps.ev_hw_changed = 0;
	if (hw3d) *hw3d = s_ps.ev_hw3d;
	return 1;
}

int dxr_prov_consume_eye_tracking_changed(int *is_tracking, int *active_mode)
{
	if (!s_ps.ev_track_changed) return 0;
	s_ps.ev_track_changed = 0;
	if (is_tracking) *is_tracking = s_ps.ev_track_is_tracking;
	if (active_mode) *active_mode = s_ps.ev_track_mode;
	return 1;
}
