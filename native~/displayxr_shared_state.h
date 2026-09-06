// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Thread-safe shared state between Unity's game thread (C# tunables updates)
// and the render thread (OpenXR hook execution).

#pragma once

#include <openxr/openxr.h>
#include <stdint.h>
#include "displayxr_extensions.h"

#ifdef __cplusplus
extern "C" {
#endif

// --- Tunables (set from game thread, read from render thread) ---

// Scene transform applied to raw eye positions before Kooima computation.
// Set from C# (game thread) based on camera/display scene hierarchy.
// Maps raw LOCAL-space eye positions into the app's desired coordinate frame.
typedef struct DisplayXRSceneTransform {
    float position[3];    // Translation offset (meters)
    float orientation[4]; // Rotation quaternion (x, y, z, w)
    float scale[3];       // Transform scale (x,y,z): spatial coords divided by this
    uint8_t enabled;      // Whether to apply this transform
} DisplayXRSceneTransform;

typedef struct DisplayXRTunables {
    float ipd_factor;           // Scales inter-eye distance
    float parallax_factor;      // Scales eye X/Y offset from center
    float perspective_factor;   // Scales eye Z only
    float virtual_display_height; // Virtual display height in meters (0 = physical)
    float inv_convergence_distance; // 1/convergence (0 = infinity)
    float fov_override;         // Override FOV in radians (0 = compute)
    float near_z;               // Near clip plane (meters), from camera
    float far_z;                // Far clip plane (meters), from camera
    uint8_t camera_centric;     // Use camera-centric parameters
    uint8_t clip_at_display_plane; // VESTIGIAL. Per-view far override: clip each view at
                                // its own |eye.z|*m2v (display-centric) or 1/invd
                                // (camera-centric). It was the hook path's channel and has
                                // had no reader since #166 removed that path: under the
                                // provider the foreground clip is a screen-space discard
                                // driven by dxr_prov_get_eye_clip (whose far now also
                                // carries the rear depth budget, #318), and the app's
                                // opt-in lives in the rig's foregroundOnlyClip, not here.
} DisplayXRTunables;

// --- Display info (set from render thread, read from game thread) ---

typedef struct DisplayXRDisplayInfo {
    float display_width_meters;
    float display_height_meters;
    uint32_t display_pixel_width;
    uint32_t display_pixel_height;
    float nominal_viewer_x;
    float nominal_viewer_y;
    float nominal_viewer_z;
    float recommended_view_scale_x;
    float recommended_view_scale_y;
    uint8_t is_valid;
} DisplayXRDisplayInfo;

// --- Eye positions (set from render thread, read from game thread) ---

typedef struct DisplayXREyePositions {
    XrVector3f left_eye;  // Raw left eye position in LOCAL space
    XrVector3f right_eye; // Raw right eye position in LOCAL space
    uint8_t is_tracked;   // Whether eye tracking is active
} DisplayXREyePositions;

// --- Kooima canvas (#189) — the window as the runtime frames Kooima into it ---
// The XR_DXR_view_rig raw channel (XrViewDisplayRawDXR) reports the effective
// canvas the runtime uses for the window-relative off-axis projection each
// frame: its rect ON THE PANEL (panel pixels, top-left origin) and its physical
// size (meters). The provider publishes it so the editor Scene-view gizmo can
// draw the window-relative eyes + the convergence-plane aspect Kooima actually
// consumes (instead of the physical-panel dims). Set from render thread, read
// from game thread.
typedef struct DisplayXRKooimaCanvas {
    int32_t rect_x, rect_y;   // Canvas offset on the panel (panel pixels)
    int32_t rect_w, rect_h;   // Canvas size (pixels)
    float   size_meters_w;    // Physical canvas width  (meters)
    float   size_meters_h;    // Physical canvas height (meters)
    uint8_t is_valid;         // Set once the runtime reports a canvas
} DisplayXRKooimaCanvas;

// --- Stereo matrices (set from render thread, read from game thread) ---
// The Kooima library produces matched view+projection matrix pairs.
// These are stored here so C# can apply them directly, bypassing Unity's
// matrix reconstruction from (fov, position, orientation).

typedef struct DisplayXRStereoMatrices {
    float left_view[16];        // Column-major 4x4, OpenXR convention
    float left_projection[16];  // Column-major 4x4, OpenGL clip space
    float right_view[16];
    float right_projection[16];
    uint8_t valid;              // Set when matrices have been computed
} DisplayXRStereoMatrices;

// --- Window-space layer descriptor ---
// Vestigial: formerly filled by the OpenXR-hook path's wsui pre-end-frame and
// consumed in hooked_xrEndFrame. That path was removed in the Task-3 hook-backend
// cleanup (#166); the provider builds its own XrCompositionLayerWindowSpaceDXR in
// displayxr_provider_session.cpp (ps_submit_wsui). Retained for a possible future
// follow-up removal (see the epic plan) — no live reader/writer today.

#define DISPLAYXR_MAX_WINDOW_LAYERS 4

typedef struct DisplayXRWindowLayer {
    XrSwapchain swapchain;      // Overlay swapchain handle
    uint32_t swapchain_width;
    uint32_t swapchain_height;
    float x, y, width, height;  // Fractional window coordinates [0..1]
    float disparity;
    uint8_t active;
} DisplayXRWindowLayer;

// --- Local2D layer descriptor (#439/#491). Post-weave 2D content at a
// client-window PIXEL rect, composited "glass over 3D" via the runtime's
// implicit mask. Vestigial like DisplayXRWindowLayer above: formerly filled by
// the removed hook path; the provider now submits its own
// XrCompositionLayerLocal2DDXR (ps_submit_local2d). Retained pending a future
// follow-up removal.
typedef struct DisplayXRLocal2DLayer {
    XrSwapchain swapchain;      // Overlay swapchain handle
    uint32_t swapchain_width;
    uint32_t swapchain_height;
    int32_t rect_x, rect_y;     // Dest, client-window pixels (post-DPI)
    int32_t rect_w, rect_h;
    uint8_t active;
} DisplayXRLocal2DLayer;

// --- Global shared state ---

typedef struct DisplayXRState {
    // Double-buffered tunables: write index toggled by game thread
    DisplayXRTunables tunables[2];
    volatile int tunables_read_idx;

    // Double-buffered scene transform: game thread writes, render thread reads
    DisplayXRSceneTransform scene_transform[2];
    volatile int scene_transform_read_idx;

    // Display info (written once during init)
    DisplayXRDisplayInfo display_info;

    // Eye positions (updated each frame from render thread)
    DisplayXREyePositions eye_positions[2];
    volatile int eyes_read_idx;

    // Kooima canvas (window on the panel) — updated each frame from render thread (#189)
    DisplayXRKooimaCanvas kooima_canvas[2];
    volatile int kooima_canvas_read_idx;

    // Stereo matrices from Kooima (updated each frame from render thread)
    DisplayXRStereoMatrices stereo_matrices[2];
    volatile int stereo_matrices_read_idx;

    // Window handle for session creation
    void *window_handle; // HWND on Win32, NSView* on macOS

    // Window-space overlay layers
    DisplayXRWindowLayer window_layers[DISPLAYXR_MAX_WINDOW_LAYERS];
    int window_layer_count;

    // Local2D overlay layer (#439/#491) — single slot (one speech bubble etc.)
    DisplayXRLocal2DLayer local2d_layer;

    // Editor mode flag: create own preview window instead of auto-detecting app window
    uint8_t editor_mode;

    // Transparent background opt-in (issue runtime-pvt #191, displayxr-unity#57).
    // Set from C# at SubsystemRegistration before xrCreateSession; consumed
    // when constructing XrWin32WindowBindingCreateInfoDXR to request the
    // runtime's BitBlt (D3D11) or DComp (D3D12) swapchain path.
    uint8_t transparent_background_requested;

    // Simple-window mode opt-in (displayxr-unity, avatar-style windowing).
    // Set from C# at SubsystemRegistration before xrCreateSession. When set
    // (and transparent, non-shell, non-editor), the plugin binds Unity's REAL
    // main HWND directly — no off-screen overlay, no DWM cloak, no off-screen
    // move. Window decoration toggles via displayxr_toggle_window_decoration
    // and click-through is region-based (SetWindowRgn on Unity's HWND).
    uint8_t simple_window_requested;

    // Color-space hint for typed-swapchain substitution (D3D11).
    // 1 = use DXGI_FORMAT_R8G8B8A8_UNORM_SRGB (matches Unity Linear color space).
    // 0 = use DXGI_FORMAT_R8G8B8A8_UNORM (matches Unity Gamma color space; also
    //     the runtime's preferred format on the test displays).
    // Defaults to 1 for backward compatibility; C# overrides at OnInstanceCreate.
    uint8_t use_srgb_swapchain;

    // Viewport (window) size and screen position for window-relative Kooima
    uint32_t viewport_width;
    uint32_t viewport_height;
    int32_t viewport_x;
    int32_t viewport_y;

    // Extension support flags
    uint8_t has_display_info_ext;
    uint8_t has_win32_window_ext;
    uint8_t has_cocoa_window_ext;
    uint8_t has_display_mode_ext;

    // Function pointers for display mode switching
    PFN_xrRequestDisplayModeDXR pfn_request_display_mode;
} DisplayXRState;

// Get the global shared state singleton.
DisplayXRState *displayxr_get_state(void);

// Initialize shared state to defaults.
void displayxr_state_init(void);

// Set tunables from game thread (double-buffer swap).
void displayxr_state_set_tunables(const DisplayXRTunables *t);

// Read current tunables from render thread.
DisplayXRTunables displayxr_state_get_tunables(void);

// Update eye positions from render thread.
void displayxr_state_set_eye_positions(const XrVector3f *left, const XrVector3f *right, uint8_t tracked);

// Read eye positions from game thread.
DisplayXREyePositions displayxr_state_get_eye_positions(void);

// Update the Kooima canvas from render thread (#189).
void displayxr_state_set_kooima_canvas(const DisplayXRKooimaCanvas *c);

// Read the Kooima canvas from game thread (#189).
DisplayXRKooimaCanvas displayxr_state_get_kooima_canvas(void);

// Set scene transform from game thread (double-buffer swap).
void displayxr_state_set_scene_transform(const DisplayXRSceneTransform *t);

// Read scene transform from render thread.
DisplayXRSceneTransform displayxr_state_get_scene_transform(void);

// Update stereo matrices from render thread (after Kooima computation).
void displayxr_state_set_stereo_matrices(const DisplayXRStereoMatrices *m);

// Read stereo matrices from game thread.
DisplayXRStereoMatrices displayxr_state_get_stereo_matrices(void);

#ifdef __cplusplus
}
#endif
