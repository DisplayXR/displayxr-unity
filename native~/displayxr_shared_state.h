// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
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

// --- Window-space layer descriptor (set from game thread, read from render thread) ---

#define DISPLAYXR_MAX_WINDOW_LAYERS 4

typedef struct DisplayXRWindowLayer {
    XrSwapchain swapchain;      // Overlay swapchain handle
    uint32_t swapchain_width;
    uint32_t swapchain_height;
    float x, y, width, height;  // Fractional window coordinates [0..1]
    float disparity;
    uint8_t active;
} DisplayXRWindowLayer;

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

    // Stereo matrices from Kooima (updated each frame from render thread)
    DisplayXRStereoMatrices stereo_matrices[2];
    volatile int stereo_matrices_read_idx;

    // Window handle for session creation
    void *window_handle; // HWND on Win32, NSView* on macOS

    // Window-space overlay layers
    DisplayXRWindowLayer window_layers[DISPLAYXR_MAX_WINDOW_LAYERS];
    int window_layer_count;

    // Readback state
    uint8_t *readback_pixels;
    uint32_t readback_width;
    uint32_t readback_height;
    volatile int readback_ready;

    // Editor mode flag: create own preview window instead of auto-detecting app window
    uint8_t editor_mode;

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
    PFN_xrRequestDisplayModeEXT pfn_request_display_mode;
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
