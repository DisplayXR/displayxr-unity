// Copyright 2025-2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Camera-centric multiview math for 3D displays
 *
 * Companion to display3d_view.h. Instead of scaling a physical display into
 * virtual app-space, the app defines a virtual camera (position, orientation,
 * vFOV) and eye tracking data produces per-eye asymmetric frustum views.
 *
 * Both abstractions share the same double-duty pattern: a single post-IPD/parallax
 * eye displacement feeds both the view matrix and the projection.
 *
 * See also: docs/architecture/stereo3d-math.md for full pipeline derivation.
 *
 * The tunables struct takes half_tan_vfov = tan(vFOV/2) rather than the angle:
 *   - Zoom is trivial: half_tan_vfov / zoom_factor (no trig round-trip)
 *   - The math operates natively in tangent-space
 *   - Composable with any tangent-space operation
 *
 * Matrix convention: all output matrices are column-major (OpenGL/Vulkan/Metal).
 * DirectX callers should transpose when loading into row-major XMMATRIX.
 */

#pragma once

#include <stdint.h>

#include "display3d_view.h" // reuse Display3DScreen, display3d_apply_eye_factors

#ifdef __cplusplus
extern "C" {
#endif

// --- Structs ---

typedef struct Camera3DTunables {
	float ipd_factor;                //!< [0, 1] — scales inter-eye distance (0=mono, 1=full)
	float parallax_factor;           //!< [0, 1] — lerps eye center toward nominal (0=no tracking, 1=full)
	float inv_convergence_distance;  //!< 1/convergence_dist (1/meters)
	float half_tan_vfov;             //!< tan(vFOV/2) — divide by zoom at call site
	uint8_t clip_at_display_plane;   //!< If non-zero, override per-view far_z with the
	                                 //!< convergence distance (1/inv_convergence_distance).
	                                 //!< Foreground-only render: geometry past where the
	                                 //!< asymmetric frustum axes converge is clipped. In
	                                 //!< camera-centric mode the convergence point is the
	                                 //!< analogue of the "display plane" in display-centric
	                                 //!< mode. When inv_convergence_distance == 0 (parallel
	                                 //!< projection) the flag is a no-op.
} Camera3DTunables;

typedef struct Camera3DView {
	float view_matrix[16];       //!< Column-major 4x4 (per-eye: eye displaced in world space)
	float projection_matrix[16]; //!< Column-major 4x4 asymmetric frustum (per-eye)
	XrFovf fov;                  //!< Asymmetric FOV angles in radians (per-eye)
	XrVector3f eye_world;        //!< Eye position in world space
	XrQuaternionf orientation;   //!< Camera orientation (same for both eyes)
} Camera3DView;

// --- Functions ---

/*!
 * All-in-one: compute camera-centric 3D view+projection from raw eye tracking data.
 *
 * Pipeline:
 *   1. Apply IPD factor + parallax factor (reuses display3d_apply_eye_factors)
 *   2. Compute eye_local = processed - (0, 0, nominal_z)  (displacement from screen plane)
 *   3. Transform eye_local to world space via camera_pose
 *   4. Build view matrix from world-space eye + camera orientation
 *   5. Scale eye_local by inv_convergence_distance for projection shifts
 *   6. Compute asymmetric tangent half-angles from half_tan_vfov + aspect ratio
 *   7. Build projection matrix from tangent half-angles + near/far
 *   8. Convert tangents to XrFovf angles
 *
 * @param raw_eyes       Array of N raw eye positions in DISPLAY space (from xrLocateViews)
 * @param count          Number of views (must be >= 1)
 * @param nominal_viewer Nominal viewer position in DISPLAY space (or NULL for {0,0,0.5})
 * @param screen         Physical screen dimensions (used for aspect ratio)
 * @param tunables       Camera tunables (or NULL for defaults)
 * @param camera_pose    Camera pose in world space (or NULL for identity)
 * @param near_z         Near clip plane distance
 * @param far_z          Far clip plane distance
 * @param out_views      Output array of N views
 */
void
camera3d_compute_views(const XrVector3f *raw_eyes,
                              uint32_t count,
                              const XrVector3f *nominal_viewer,
                              const Display3DScreen *screen,
                              const Camera3DTunables *tunables,
                              const XrPosef *camera_pose,
                              float near_z,
                              float far_z,
                              Camera3DView *out_views);

/*!
 * Compute camera-centric view+projection for a single eye.
 *
 * Takes a pre-processed eye position (after display3d_apply_eye_factors) and
 * computes view matrix, projection matrix, FOV, and world-space eye position.
 *
 * @param processed_eye  Processed eye position (after apply_eye_factors)
 * @param nominal_z      Nominal viewer Z distance (e.g. 0.5)
 * @param screen         Physical screen dimensions (for aspect ratio)
 * @param tunables       Camera tunables (or NULL for defaults)
 * @param camera_pose    Camera pose in world space (or NULL for identity)
 * @param near_z         Near clip plane distance
 * @param far_z          Far clip plane distance
 * @param out            Output view for this eye
 */
void
camera3d_compute_view(const XrVector3f *processed_eye,
                      float nominal_z,
                      const Display3DScreen *screen,
                      const Camera3DTunables *tunables,
                      const XrPosef *camera_pose,
                      float near_z,
                      float far_z,
                      Camera3DView *out);

/*!
 * Default camera tunables: ipd=1, parallax=1, invd=1, half_tan=tan(18deg) (~36deg vFOV).
 */
Camera3DTunables
camera3d_default_tunables(void);

#ifdef __cplusplus
}
#endif
