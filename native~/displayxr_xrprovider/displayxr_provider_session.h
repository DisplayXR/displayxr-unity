// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
//
// Runtime-facing OpenXR session for the custom Unity IUnityXRDisplay Display
// Provider (epic #166, milestone M1 — GO/NO-GO spike).
//
// This is the provider's analog of displayxr_standalone.cpp: it loads the
// DisplayXR runtime directly via xrNegotiateLoaderRuntimeInterface, creates the
// instance/system/session, enables the EXT extensions, enumerates rendering
// modes, creates the (SPI, arraySize=2) swapchain, and consumes XR_EXT_view_rig
// render-ready views — but it differs from the standalone in two ways:
//
//   1. The graphics binding is created on UNITY'S OWN D3D12 device + queue
//      (zero-copy, the runtime's documented Unity-D3D12 contract), so the
//      runtime's swapchain ID3D12Resource* images can be surfaced straight to
//      Unity via IUnityXRDisplayInterface::CreateTexture — no cross-device
//      bridge/copy. (The standalone's separate-device + atlas-bridge path is the
//      D3D11/editor fallback; not used here.)
//   2. The swapchain is arraySize=2 (Single-Pass-Instanced): the two eyes are
//      array layers 0/1 and the projection layer submits per-view
//      subImage.imageArrayIndex 0/1 (the shipped runtime SPI contract).
//
// M1 scope: Windows / D3D12 only. macOS/Vulkan/D3D11 out of scope.
//
// NOTE (spike): this module lifts the proven runtime-load / instance / session /
// locate-views logic from displayxr_standalone.cpp. It is intentionally kept
// separate so the hook path AND the editor standalone path stay untouched.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Max views M1 handles (stereo). Sized to 2; the runtime may report more
// (max-mode count) but the provider submits only the 2 eyes for SPI.
#define DXR_PROV_MAX_VIEWS 2

/// One render-ready view consumed from xrLocateViews (XR_EXT_view_rig).
typedef struct DxrProvView {
	float position[3];    ///< Eye position, OpenXR LOCAL (display) space.
	float orientation[4]; ///< Eye orientation quaternion (x,y,z,w).
	float fov[4];         ///< angleLeft, angleRight, angleUp, angleDown (radians).
	int   valid;          ///< 1 if this view is render-ready.
} DxrProvView;

/// Display geometry surfaced from XR_EXT_display_info (for C# / logging).
typedef struct DxrProvDisplayInfo {
	float    width_m, height_m;          ///< Physical display size (meters).
	uint32_t pixel_width, pixel_height;  ///< Display pixel dimensions.
	float    nominal_x, nominal_y, nominal_z; ///< Nominal viewer position.
	float    scale_x, scale_y;           ///< Recommended per-view scale.
	int      is_valid;
} DxrProvDisplayInfo;

// ---- Lifecycle --------------------------------------------------------------

/// Start the runtime-facing session on Unity's D3D12 device.
/// @param runtime_json_path Path to the OpenXR runtime JSON manifest (or NULL to
///        resolve from XR_RUNTIME_JSON / the active-runtime registry).
/// @param unity_d3d12_device Unity's ID3D12Device* (from IUnityGraphicsD3D12v*::GetDevice()).
/// @param unity_d3d12_queue  Unity's ID3D12CommandQueue* (GetCommandQueue()).
/// @param overlay_hwnd       Plugin-created WS_CHILD overlay HWND over Unity's
///        window (DXGI one-swapchain-per-HWND forces an overlay), or NULL to let
///        the runtime self-host a window (sim_display bring-up).
/// @return 1 on success, 0 on failure.
int  dxr_prov_session_start(const char *runtime_json_path,
                            void *unity_d3d12_device,
                            void *unity_d3d12_queue,
                            void *overlay_hwnd);

/// Stop the session and release all OpenXR resources. Safe if not running.
void dxr_prov_session_stop(void);

/// Whether the session is currently running.
int  dxr_prov_session_is_running(void);

// ---- Bridge surfacing (for CreateTexture) -----------------------------------

/// Swapchain shape, valid after a successful start.
void dxr_prov_get_swapchain_info(uint32_t *width, uint32_t *height,
                                 uint32_t *array_size, uint32_t *image_count);

/// The cross-device BRIDGE texture's Unity-side native pointer (ID3D12Resource*
/// on Unity's device, a 2-slice array). The provider wraps this ONCE via
/// CreateTexture; Unity renders both eyes into it (slices 0/1); the provider
/// then copies it into the runtime swapchain on its own device each frame
/// (dxr_prov_submit_frame). This replaces the zero-copy handoff, which crashed
/// with D3D12 device-removed because the runtime allocates its swapchain on its
/// own ID3D12Device instance (cross-device raw pointers are invalid). Returns
/// NULL if the bridge isn't created yet.
void *dxr_prov_get_bridge_unity_texture(uint32_t *width, uint32_t *height,
                                        uint32_t *array_size);

// ---- Frame loop -------------------------------------------------------------

/// Pump OpenXR session-state events (xrPollEvent). Drives the session to READY.
void dxr_prov_poll_events(void);

/// Begin a frame: xrWaitFrame + xrBeginFrame + xrLocateViews (view-rig chained)
/// + acquire/wait the swapchain image Unity should render into next.
/// @param out_image_index Receives the acquired swapchain image index.
/// @param out_should_render Set to 1 if the app should render this frame.
/// @return 1 on success, 0 on failure or session not ready.
int  dxr_prov_begin_frame(uint32_t *out_image_index, int *out_should_render);

/// Copy out a render-ready view (after dxr_prov_begin_frame).
void dxr_prov_get_view(uint32_t view_index, DxrProvView *out_view);

/// Submit the rendered frame: release the swapchain image + xrEndFrame a 2-view
/// projection layer (per-eye subImage.imageArrayIndex 0/1).
/// @param image_index The index returned by dxr_prov_begin_frame.
/// @return 1 on success, 0 on failure.
int  dxr_prov_submit_frame(uint32_t image_index);

/// End the current frame with no layers (keeps the session alive when not rendering).
void dxr_prov_end_frame_empty(void);

// ---- Tunables / pose (pushed from the rig via C#, mirrors the hook path) -----

/// Set the display-rig tunables (display-centric). Camera-centric variant TBD in M2.
void dxr_prov_set_tunables(float ipd_factor, float parallax_factor,
                           float perspective_factor, float virtual_display_height);

/// Set the display pose (parent transform), OpenXR LOCAL space.
void dxr_prov_set_display_pose(float px, float py, float pz,
                               float ox, float oy, float oz, float ow,
                               int enabled);

// ---- Queries ----------------------------------------------------------------

/// Display info surfaced from XR_EXT_display_info.
void dxr_prov_get_display_info(DxrProvDisplayInfo *out_info);

/// Whether the runtime advertised XR_EXT_view_rig (else no stereo — WARN+passthrough).
int  dxr_prov_has_view_rig(void);

/// Optional log sink (routes [DisplayXR-PROV] messages to Unity's Debug.Log).
typedef void (*DxrProvLogCallback)(const char *message);
void dxr_prov_set_log_callback(DxrProvLogCallback cb);

#ifdef __cplusplus
}
#endif
