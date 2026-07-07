// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
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

// Export macro for the functions P/Invoked from C# (DisplayXRProviderNative).
// Decorating the DECLARATION is what gets the symbol into the DLL export table —
// the .cpp definitions don't need to repeat it (matches displayxr_hooks.h).
#ifndef DISPLAYXR_EXPORT
# if defined(_WIN32)
#  define DISPLAYXR_EXPORT __declspec(dllexport)
# elif defined(__GNUC__)
#  define DISPLAYXR_EXPORT __attribute__((visibility("default")))
# else
#  define DISPLAYXR_EXPORT
# endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Max views M1 handles (stereo). Sized to 2; the runtime may report more
// (max-mode count) but the provider submits only the 2 eyes for SPI.
#define DXR_PROV_MAX_VIEWS 2

/// Graphics backend the provider drives the runtime with, selected at session
/// create from IUnityGraphics::GetRenderer() (#195). D3D12 is the original
/// own-device + shared-texture-bridge path; D3D11 is a zero-copy path that binds
/// the session on Unity's own ID3D11Device (the runtime's native D3D11 compositor
/// creates + weaves swapchain images on that device — no bridge/copy/fence).
/// METAL is the macOS client-queue path (#202/#204: session bound on a
/// provider-created MTLCommandQueue on Unity's MTLDevice). VULKAN is reserved.
/// Kept as an int across the C ABI; values are append-only (ABI-stable).
typedef enum DxrGfxKind {
	DXR_GFX_NONE = 0,
	DXR_GFX_D3D12,
	DXR_GFX_D3D11,
	DXR_GFX_VULKAN, // reserved (not implemented)
	DXR_GFX_METAL,  // macOS (#202/#204)
} DxrGfxKind;

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

/// One enumerated rendering mode surfaced to C# (XrDisplayRenderingModeInfoEXT).
typedef struct DxrProvModeInfo {
	uint32_t mode_index;
	uint32_t view_count;
	uint32_t tile_columns, tile_rows;
	uint32_t view_width_px, view_height_px;
	float    view_scale_x, view_scale_y;
	int      hardware_display_3d;
	int      is_active;
	int      is_requestable;
	char     name[64];
} DxrProvModeInfo;

// ---- Lifecycle --------------------------------------------------------------

/// Start the runtime-facing session on Unity's graphics device.
/// @param runtime_json_path Path to the OpenXR runtime JSON manifest (or NULL to
///        resolve from XR_RUNTIME_JSON / the active-runtime registry).
/// @param backend_kind      DxrGfxKind selected from IUnityGraphics::GetRenderer()
///        (#195): DXR_GFX_D3D12 = own-device + shared-bridge path (device+queue),
///        DXR_GFX_D3D11 = zero-copy on Unity's ID3D11Device (queue = NULL).
/// @param unity_device      Unity's device: ID3D12Device* (D3D12) or ID3D11Device*
///        (D3D11), from IUnityGraphicsD3D1*::GetDevice().
/// @param unity_queue       Unity's ID3D12CommandQueue* (D3D12 only; NULL on D3D11 —
///        D3D11's immediate context has no app-visible queue).
/// @param overlay_hwnd       Plugin-created WS_CHILD overlay HWND over Unity's
///        window (DXGI one-swapchain-per-HWND forces an overlay), or NULL to let
///        the runtime self-host a window (sim_display bring-up).
/// @return 1 on success, 0 on failure.
int  dxr_prov_session_start(const char *runtime_json_path,
                            int backend_kind,
                            void *unity_device,
                            void *unity_queue,
                            void *overlay_hwnd);

/// The graphics backend selected for the running session (DxrGfxKind as int).
/// DXR_GFX_D3D11 → zero-copy path (no shared bridge to transition/copy). Read by
/// the display-provider TU to gate the D3D12-only cross-device barrier + copy.
DISPLAYXR_EXPORT int  dxr_prov_get_graphics_api(void);

/// (#195 editor bridge) 1 when the D3D11 backend is running in OWN-DEVICE BRIDGE
/// mode (editor Play Mode: the session binds on a separate ID3D11Device + shared
/// bridge, so Unity's editor GameView present never shares the weaver's device →
/// no Optimus cross-present deadlock). 0 = D3D11 zero-copy (built player) or D3D12.
/// The display-provider TU reads it to pick the swapchain-image wrap: zero-copy
/// wraps the runtime images directly; bridge wraps the single Unity-side bridge tex
/// (like the D3D12 SPI bridge).
DISPLAYXR_EXPORT int  dxr_prov_d3d11_bridge_active(void);

/// (D3D11 zero-copy) The runtime swapchain image `index`'s native ID3D11Texture2D*
/// (a 2-slice SPI array), which lives on Unity's device — wrapped DIRECTLY via
/// CreateTexture (no bridge). *out_array = 2. NULL for D3D12 or a bad index.
void *dxr_prov_get_swapchain_image_texture(uint32_t index, uint32_t *out_w,
                                           uint32_t *out_h, uint32_t *out_array);

/// Stop the session and release all OpenXR resources. Safe if not running.
void dxr_prov_session_stop(void);

/// Whether the session is currently running.
DISPLAYXR_EXPORT int  dxr_prov_session_is_running(void);

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

/// MultiPass mode (BiRP): the per-eye single-slice BRIDGE's Unity-side pointer.
/// In MultiPass each render pass needs its own texture (textureArraySlice is
/// SPI-only), so the provider wraps these two as separate Unity textures. Returns
/// NULL for eye>1 or in SPI mode (use dxr_prov_get_bridge_unity_texture instead).
void *dxr_prov_get_bridge_unity_texture_eye(uint32_t eye, uint32_t *width, uint32_t *height);

/// Render mode gate (#166 task #8). Set from C# BEFORE the session starts:
/// 1 = Single-Pass-Instanced (URP+Win+D3D12), 0 = MultiPass (BiRP/other — SPI
/// renders opaque geometry wrong on BiRP). Default (unset) = SPI, preserving the
/// pre-gating behavior. Preserved across the session_start reset.
DISPLAYXR_EXPORT void dxr_prov_set_single_pass(int enable);

/// Effective render mode: 1 = SPI, 0 = MultiPass. Exported: DisplayXRPostAA reads
/// it to gate the OnRenderImage FXAA pass (valid only in MultiPass, where each eye
/// is its own single-slice RT; SPI's 2-slice array breaks the Blit — #166).
DISPLAYXR_EXPORT int  dxr_prov_get_single_pass(void);

/// (#173) Dedicated-window weave target for editor Play Mode. Set from C# BEFORE
/// the subsystem starts (the loader does so when Application.isEditor): 1 = the
/// provider creates its OWN standalone, movable weave window (coexists with the
/// editor + window-relative Kooima + input, no focus-switch crash) instead of the
/// default app-owned overlay that tracks Unity's (whole-editor) window. Built
/// players keep the overlay default. Env fallback: DISPLAYXR_PROV_EDITOR_WINDOW=1.
/// Read by the display-provider TU (LifecycleStart/GfxStart); a plain file-static,
/// so it survives the session_start reset without special handling.
DISPLAYXR_EXPORT void dxr_prov_set_dedicated_window(int enable);
DISPLAYXR_EXPORT int  dxr_prov_get_dedicated_window(void);

/// Transparent-background request (#166 Phase A). Set from C# BEFORE the session
/// starts: 1 = opt the session into a transparent background (ALPHA_BLEND env
/// blend mode + transparentBackgroundEnabled on the win32 binding) so the runtime's
/// DComp overlay composites the woven 3D over the desktop with per-pixel alpha.
/// The opt-in only takes effect if the runtime advertises ALPHA_BLEND (probed in
/// session_start). Preserved across the session_start reset (like single_pass).
DISPLAYXR_EXPORT void dxr_prov_set_transparent_background(int enable);

/// Whether a transparent background was requested (read by the display-provider
/// TU in LifecycleStart to pick the transparent overlay path — unowned + Unity
/// cloaked/off-screen — over the provider-opaque one, which follows Unity and
/// would drag the overlay off-screen once the transparent path cloaks Unity).
int dxr_prov_wants_transparent(void);

/// Set the single 3D-zone rect (client-window px, top-left origin) the runtime
/// frames the Kooima 3D into (#166 Phase B). The locate chains XrDisplayZoneEXT in
/// front of the view-rig and submit chains the same zone on the projection layer;
/// the swapchain is sized to the zone's recommended view size. w<=0||h<=0 clears
/// (full-window framing). Requires the runtime to advertise XR_EXT_display_zones.
/// Seed BEFORE the session starts (like the hook SeedLaunchZone) for born-zone-sized.
DISPLAYXR_EXPORT void dxr_prov_set_3d_zone_rect(int32_t x, int32_t y, int32_t w, int32_t h);

/// Clear the 3D-zone rect (revert to full-window framing).
DISPLAYXR_EXPORT void dxr_prov_clear_3d_zone(void);

// ---- Multiple 3D zones (#166 Phase B2) --------------------------------------
// index 0 = the primary zone (== dxr_prov_set_3d_zone_rect); index>=1 = extra
// zones, each with its own zone-sized swapchain + bridge + render pass. Total 3D
// zones <= PS_MAX_ZONES (4, Unity's render-pass cap). Seed BEFORE session start.

/// Set the total number of 3D zones (1 primary + extras). 0/1 → no extra zones.
DISPLAYXR_EXPORT void dxr_prov_set_zone_count(uint32_t total_3d_zones);

/// Set zone `index`'s rect (client-window px). index 0 → primary; >=1 → extra.
DISPLAYXR_EXPORT void dxr_prov_set_zone(uint32_t index, uint32_t zone_id,
                                        int32_t x, int32_t y, int32_t w, int32_t h);

/// Number of active EXTRA zones (total 3D zones = 1 + this).
DISPLAYXR_EXPORT uint32_t dxr_prov_get_extra_zone_count(void);

/// Extra zone `ei`'s Unity-side SPI bridge (2-slice array) pointer, or NULL.
void *dxr_prov_get_extra_zone_bridge(uint32_t ei, uint32_t *w, uint32_t *h);

/// Extra zone `ei`'s Unity-side MultiPass per-eye bridge pointer, or NULL.
void *dxr_prov_get_extra_zone_bridge_eye(uint32_t ei, uint32_t eye, uint32_t *w, uint32_t *h);

/// Copy out extra zone `ei`'s render-ready view for `eye` (after begin_frame).
void dxr_prov_get_extra_zone_view(uint32_t ei, uint32_t eye, DxrProvView *out_view);

/// Build a column-major GL-clip projection matrix from an XrFovf (DxrProvView.fov
/// layout) using the provider's current near/far. Lets the display-provider frame
/// desc hand Unity a full matrix (kUnityXRProjectionTypeMatrix) instead of half
/// angles — URP-simplify experiment (#22). out16 = 16 floats.
void dxr_prov_build_projection(const float fov[4], float *out16);

/// Per-zone stereo matrices + screen rect (#166 — multi-zone transparent mask).
/// The transparent overlay renders its SetWindowRgn silhouette per-zone into each
/// zone's rect so ALL zones stay visible (else non-primary zones clip to see-through).
/// Zone 0 = primary; zone i>=1 = extra zone i-1. Count = 1 + active extra zones.
DISPLAYXR_EXPORT uint32_t dxr_prov_get_zone_count(void);
/// Fill 4 column-major float[16] matrices for `zone` (cyclopean L/R view+proj).
/// Returns 1 on success (zone has >=2 valid views), 0 otherwise. Any out ptr may be NULL.
DISPLAYXR_EXPORT int dxr_prov_get_zone_stereo_matrices(uint32_t zone, float *lv, float *lp,
                                                       float *rv, float *rp);
/// Fill `zone`'s window-client pixel rect (top-left origin). Returns 1 on success.
DISPLAYXR_EXPORT int dxr_prov_get_zone_rect_px(uint32_t zone, int *x, int *y, int *w, int *h);

/// Local2D layer (#166 Phase B): lazily create the provider's Local2D overlay
/// swapchain + cross-device bridge sized to w×h and return the Unity-device handle
/// of the bridge. C# (DisplayXRLocal2D) Graphics.CopyTexture's its canvas RT into it
/// each frame; the provider submits an XrCompositionLayerLocal2DEXT at the pixel
/// rect. Mirrors dxr_prov_get_wsui_bridge. *out_ptr = NULL when no session running.
DISPLAYXR_EXPORT void dxr_prov_get_local2d_bridge(uint32_t w, uint32_t h,
                                                  void **out_ptr, uint32_t *out_w, uint32_t *out_h);

/// Set the Local2D dest rect in client-window PIXELS (post-DPI). w<=0||h<=0 clears
/// (layer inactive). Cheap; safe to call every frame.
DISPLAYXR_EXPORT void dxr_prov_set_local2d_rect(int32_t x, int32_t y, int32_t w, int32_t h);

// ---- Frame loop -------------------------------------------------------------

/// Pump OpenXR session-state events (xrPollEvent). Drives the session to READY.
/// Exported for the macOS main-thread pump (#204): the runtime's poll drains
/// NSApp events + flushes CATransaction (main-thread-only), so on macOS the C#
/// driver calls this every LateUpdate instead of the graphics-thread pump.
DISPLAYXR_EXPORT void dxr_prov_poll_events(void);

/// Live tile realloc (#172): if the primary per-view target size (window×scaleXY,
/// or the 3D-zone recommended view size) has changed and held stable, recreate the
/// swapchain+bridge to match. Call at the top of the Unity frame BEFORE wrapping the
/// eye textures. Returns 1 if it reallocated (caller must drop + rewrap the Unity
/// textures). Runs only between frames (no-op mid-frame).
int  dxr_prov_reconcile_size(void);

/// Consume the per-extra-zone realloc latch (0-based index). Returns 1 (and clears)
/// if that extra zone was just reallocated by dxr_prov_reconcile_size and its Unity
/// texture(s) must be dropped + re-wrapped. Call for each extra zone each frame.
int  dxr_prov_consume_zone_rewrap(uint32_t index);

/// Begin a frame: xrWaitFrame + xrBeginFrame + xrLocateViews (view-rig chained)
/// + acquire/wait the swapchain image Unity should render into next.
/// @param out_image_index Receives the acquired swapchain image index.
/// @param out_should_render Set to 1 if the app should render this frame.
/// @return 1 on success, 0 on failure or session not ready.
int  dxr_prov_begin_frame(uint32_t *out_image_index, int *out_should_render);

/// Copy out a render-ready view (after dxr_prov_begin_frame).
void dxr_prov_get_view(uint32_t view_index, DxrProvView *out_view);

/// Per-eye foreground-clip data (#166 Phase B): the eye's foreground far (view-space
/// display-plane distance in world units — display rig = |rig-local eye Z|, camera
/// rig = convergence distance) + the eye WORLD position (Unity coords). C#
/// (DisplayXRDisplay) publishes these to the URP DisplayXR/ForegroundClipURP globals
/// (_DXRForegroundFar / _DXREyePosL/R), since DisplayXRFeature.GetStereoMatrices —
/// the hook-path source — is inert in provider mode.
DISPLAYXR_EXPORT void dxr_prov_get_eye_clip(uint32_t eye, float *out_far,
                                            float *out_ex, float *out_ey, float *out_ez);

/// Per-zone per-eye foreground clip (#166 multi-zone). Zone 0 = primary; i>=1 =
/// extra zone i-1. Returns 1 on success. Same data as dxr_prov_get_eye_clip but for
/// a specific zone's views, so the clip publisher can diagnose / apply per-zone clip.
DISPLAYXR_EXPORT int dxr_prov_get_zone_eye_clip(uint32_t zone, uint32_t eye, float *out_far,
                                                float *out_ex, float *out_ey, float *out_ez);

/// Submit the rendered frame: release the swapchain image + xrEndFrame a 2-view
/// projection layer (per-eye subImage.imageArrayIndex 0/1).
/// @param image_index The index returned by dxr_prov_begin_frame.
/// @return 1 on success, 0 on failure.
int  dxr_prov_submit_frame(uint32_t image_index);

/// End the current frame with no layers (keeps the session alive when not rendering).
void dxr_prov_end_frame_empty(void);

// ---- Tunables / pose (pushed from the rig via C#, mirrors the hook path) -----

/// Set the stereo rig tunables. Full signature mirrors
/// displayxr_standalone_set_tunables so DisplayXRDisplay (display-centric) and
/// DisplayXRCamera (camera-centric) both drive the provider:
///   - display-centric (camera_centric=0): uses virtual_display_height, ipd,
///     parallax, perspective → chained as XrDisplayRigEXT.
///   - camera-centric  (camera_centric=1): uses inv_convergence_distance,
///     fov_override (= tan(vFov/2)), ipd, parallax → chained as XrCameraRigEXT.
/// near_z/far_z are owned by Unity's projection (not the rig descriptor); stored
/// but unused, matching the standalone. Scale-as-zoom is folded into
/// virtual_display_height / inv_convergence_distance on the C# side (the rig's
/// lossyScale), again matching the standalone — the native side ignores scale.
DISPLAYXR_EXPORT void dxr_prov_set_tunables(float ipd_factor, float parallax_factor,
                           float perspective_factor, float virtual_display_height,
                           float inv_convergence_distance, float fov_override,
                           float near_z, float far_z, int camera_centric);

/// Set the rig pose (parent/scene transform). Coordinates are Unity world
/// (left-hand, +Z forward); the native side converts to OpenXR (right-hand,
/// -Z forward) — negate position Z, negate quaternion X/Y — matching the
/// standalone / hook convention. enabled=0 reverts to the identity rig pose.
DISPLAYXR_EXPORT void dxr_prov_set_display_pose(float px, float py, float pz,
                               float ox, float oy, float oz, float ow,
                               int enabled);

/// Get the sent rig pose in the OpenXR frame (as stored). Returns 1 if a pose is
/// set, else 0. The render handoff uses this to make deviceAnchorToEyePose
/// rig-RELATIVE so the rig origin isn't applied twice (#166).
DISPLAYXR_EXPORT int dxr_prov_get_display_pose_oxr(float out_pos[3], float out_quat[4]);

// ---- Queries ----------------------------------------------------------------

/// Display info surfaced from XR_EXT_display_info.
DISPLAYXR_EXPORT void dxr_prov_get_display_info(DxrProvDisplayInfo *out_info);

/// Whether the runtime advertised XR_EXT_view_rig (else no stereo — WARN+passthrough).
int  dxr_prov_has_view_rig(void);

/// The active per-view RENDER rect this frame = window(overlay client) ×
/// active-mode scaleXY, clamped to the worst-case swapchain (ADR-010). The
/// provider sets renderParams[eye].viewportRect to the normalized sub-rect
/// (w/sc_width, h/sc_height) so Unity renders only this sub-region, and submits
/// each view's imageRect.extent = {w, h}. Re-derived every frame (live resize).
DISPLAYXR_EXPORT void dxr_prov_get_render_rect(uint32_t *out_w, uint32_t *out_h);

/// Window-space UI (HUD) layer (#67/#166): lazily create the provider's wsui
/// overlay swapchain + cross-device bridge sized to w×h and return the Unity-device
/// handle of the bridge. C# (DisplayXRWindowSpaceUI) Graphics.CopyTexture's its
/// canvas RT into it each frame; the provider submits the wsui composition layer.
/// Returns *out_ptr = NULL when no session is running.
DISPLAYXR_EXPORT void dxr_prov_get_wsui_bridge(uint32_t w, uint32_t h,
                                               void **out_ptr, uint32_t *out_w, uint32_t *out_h);

// ---- Rendering modes (XR_EXT_display_info) ----------------------------------

/// Number of enumerated rendering modes.
DISPLAYXR_EXPORT uint32_t dxr_prov_get_mode_count(void);

/// Copy out one enumerated mode (0..count-1). Returns 1 on success.
DISPLAYXR_EXPORT int  dxr_prov_get_mode_info(uint32_t index, DxrProvModeInfo *out_info);

/// modeIndex of the currently active mode (isActive), or 0 if unknown.
DISPLAYXR_EXPORT uint32_t dxr_prov_get_active_mode_index(void);

/// Request a vendor rendering mode by modeIndex (xrRequestDisplayRenderingModeEXT).
/// Returns 1 on XR_SUCCEEDED.
DISPLAYXR_EXPORT int  dxr_prov_request_rendering_mode(uint32_t mode_index);

/// Request the hardware 2D/3D display state (xrRequestDisplayModeEXT). mode3d=1
/// → XR_DISPLAY_MODE_3D_EXT, 0 → 2D. Returns 1 on XR_SUCCEEDED.
DISPLAYXR_EXPORT int  dxr_prov_request_display_mode(int mode3d);

/// Request the eye-tracking mode (xrRequestEyeTrackingModeEXT). manual=1 →
/// MANUAL, 0 → MANAGED. Returns 1 on XR_SUCCEEDED (0 if unsupported/unresolved).
DISPLAYXR_EXPORT int  dxr_prov_set_eye_tracking_mode(int manual);

// ---- Atlas capture (XR_EXT_atlas_capture, #140) -----------------------------

/// App-facing atlas screenshot ('I' key / DisplayXRScreenshot): hand the runtime
/// a path prefix + capture stage; the runtime reads back its own compositor atlas
/// and writes "<prefix>_atlas_<viewCount>_<cols>x<rows>.png". Non-blocking (the PNG
/// lands on the next composed frame). stage!=0 → PROJECTION_ONLY, 0 → POST_COMPOSE.
/// Returns 1 on XR_SUCCEEDED, 0 if the extension is unresolved or no live session.
/// Provider-mode analog of displayxr_capture_atlas on the hook path.
DISPLAYXR_EXPORT int  dxr_prov_capture_atlas(const char *path_prefix, int stage);

// ---- Event consumption (atomic read-and-clear; pumped by dxr_prov_poll_events) --

/// Returns 1 once after a XrEventDataRenderingModeChangedEXT, filling the
/// previous/current modeIndex; 0 otherwise. The native side has already
/// re-enumerated modes + re-derived tiling/resolution by the time this returns 1.
DISPLAYXR_EXPORT int  dxr_prov_consume_mode_changed(uint32_t *prev_index, uint32_t *cur_index);

/// Returns 1 once after a XrEventDataHardwareDisplayStateChangedEXT, filling the
/// new hardwareDisplay3D state; 0 otherwise.
DISPLAYXR_EXPORT int  dxr_prov_consume_hw_state_changed(int *hw3d);

/// Returns 1 once after a XrEventDataEyeTrackingStateChangedEXT, filling the new
/// isTracking state and activeMode (0=MANAGED, 1=MANUAL); 0 otherwise.
DISPLAYXR_EXPORT int  dxr_prov_consume_eye_tracking_changed(int *is_tracking, int *active_mode);

/// Optional log sink (routes [DisplayXR-PROV] messages to Unity's Debug.Log).
typedef void (*DxrProvLogCallback)(const char *message);
void dxr_prov_set_log_callback(DxrProvLogCallback cb);

#ifdef __cplusplus
}
#endif
