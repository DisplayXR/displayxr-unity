// Copyright 2025-2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Unified display-centric multiview math for 3D displays
 *
 * Canonical implementation of Kooima asymmetric frustum projection, view matrix
 * construction, and per-eye factor processing. Any project that needs
 * display-centric 3D views should use this library instead of reimplementing
 * the math. Pure C, depends only on <openxr/openxr.h> and <math.h>.
 *
 * Reference: Robert Kooima, "Generalized Perspective Projection" (2009)
 * See also: docs/architecture/stereo3d-math.md for full pipeline derivation.
 *
 * Matrix convention: all output matrices are column-major (OpenGL/Vulkan/Metal).
 * DirectX callers should transpose when loading into row-major XMMATRIX.
 */

#pragma once

#include <openxr/openxr.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- Structs ---

typedef struct Display3DTunables {
	float ipd_factor;              //!< [0, 1] — scales inter-eye distance (0=mono, 1=full)
	float parallax_factor;         //!< [0, 1] — lerps eye center toward nominal (0=no tracking, 1=full)
	float perspective_factor;      //!< [0.1, 10] — scales eye XYZ only (changes object perspective)
	float virtual_display_height;  //!< Virtual display height in app units (always required;
	                               //!< use physical display height for 1:1 meters)
	uint8_t clip_at_display_plane; //!< If non-zero, override the `far_z` arg per view with
	                               //!< |eye_scaled.z| (each view's distance to the display
	                               //!< plane, in app units after m2v scaling). Lets the
	                               //!< app render foreground-only content without losing
	                               //!< per-view geometric accuracy (each view's projection
	                               //!< clips at its own optical axis intersection with the
	                               //!< display plane). Scales to N views automatically
	                               //!< since the per-view loop in display3d_compute_views
	                               //!< already passes each view's eye position through
	                               //!< display3d_compute_view.
} Display3DTunables;

typedef struct Display3DScreen {
	float width_m;  //!< Physical screen width (meters)
	float height_m; //!< Physical screen height (meters)
} Display3DScreen;

typedef struct Display3DView {
	float view_matrix[16];       //!< Column-major 4x4 view matrix
	float projection_matrix[16]; //!< Column-major 4x4 projection matrix
	XrFovf fov;                  //!< Equivalent asymmetric FOV angles (radians)
	XrVector3f eye_display;      //!< Modified eye position in display space (after all factors)
	XrVector3f eye_world;        //!< Eye position in world space (after display pose transform)
	XrQuaternionf orientation;   //!< Display/camera orientation (same for both eyes)
} Display3DView;

// --- Functions ---

/*!
 * All-in-one: compute 3D view+projection from raw eye tracking data.
 *
 * Pipeline:
 *   1. Apply IPD factor (scale inter-view vector, keep center fixed)
 *   2. Apply parallax factor (lerp center toward nominal viewer)
 *   3. Apply perspective * m2v to eye XYZ (view matrix + Kooima eye)
 *   4. Apply m2v to screen W/H (Kooima screen dims)
 *   5. Transform display-space eye -> world-space via display_pose
 *   6. Build view matrix from world-space eye + display orientation
 *   7. Build Kooima projection matrix from display-space scaled eye + scaled screen
 *   8. Compute FOV angles from same
 *
 * @param raw_eyes       Array of N raw eye positions in DISPLAY space (from xrLocateViews)
 * @param count          Number of views (must be >= 1)
 * @param nominal_viewer Nominal viewer position in DISPLAY space (or NULL for {0,0,0.5})
 * @param screen         Physical screen dimensions
 * @param tunables       View factors (or NULL for defaults: all 1.0)
 * @param display_pose   Display pose in world space (or NULL for identity)
 * @param near_z         Near clip plane distance
 * @param far_z          Far clip plane distance
 * @param out_views      Output array of N views
 */
void
display3d_compute_views(const XrVector3f *raw_eyes,
                               uint32_t count,
                               const XrVector3f *nominal_viewer,
                               const Display3DScreen *screen,
                               const Display3DTunables *tunables,
                               const XrPosef *display_pose,
                               float near_z,
                               float far_z,
                               Display3DView *out_views);

/*!
 * Compute Kooima FOV angles only (no matrices). Useful for runtime-side
 * computation where the app builds its own projection.
 *
 * @param eye_pos          Eye position in display space (after all factors)
 * @param screen_width_m   Screen width in meters
 * @param screen_height_m  Screen height in meters
 * @return XrFovf with asymmetric frustum angles (radians)
 */
XrFovf
display3d_compute_fov(XrVector3f eye_pos, float screen_width_m, float screen_height_m);

/*!
 * Compute Kooima projection matrix (column-major float[16]).
 *
 * @param eye_pos          Eye position in display space (after all factors)
 * @param screen_width_m   Screen width in meters
 * @param screen_height_m  Screen height in meters
 * @param near_z           Near clip plane distance
 * @param far_z            Far clip plane distance
 * @param out_matrix       Output float[16], column-major
 */
void
display3d_compute_projection(XrVector3f eye_pos,
                             float screen_width_m,
                             float screen_height_m,
                             float near_z,
                             float far_z,
                             float *out_matrix);

/*!
 * Apply eye factors only (IPD + parallax). Useful when caller builds
 * its own view matrix but wants consistent eye processing.
 *
 * @param raw_left        Raw left eye position
 * @param raw_right       Raw right eye position
 * @param nominal_viewer  Nominal viewer position (or NULL for {0,0,0.5})
 * @param ipd_factor      IPD scaling factor [0, 1]
 * @param parallax_factor Parallax lerp factor [0, 1]
 * @param out_left        Output processed left eye position
 * @param out_right       Output processed right eye position
 */
void
display3d_apply_eye_factors(const XrVector3f *raw_left,
                            const XrVector3f *raw_right,
                            const XrVector3f *nominal_viewer,
                            float ipd_factor,
                            float parallax_factor,
                            XrVector3f *out_left,
                            XrVector3f *out_right);

/*!
 * Apply eye factors to N eyes. N-eye generalization of display3d_apply_eye_factors.
 * Center is the centroid of all N raw positions.
 *
 * @param raw_eyes       Array of N raw eye positions
 * @param count          Number of eyes (must be >= 1)
 * @param nominal_viewer Nominal viewer position (or NULL for {0,0,0.5})
 * @param ipd_factor     IPD scaling factor [0, 1]
 * @param parallax_factor Parallax lerp factor [0, 1]
 * @param out_eyes       Output array of N processed eye positions
 */
void
display3d_apply_eye_factors_n(const XrVector3f *raw_eyes,
                              uint32_t count,
                              const XrVector3f *nominal_viewer,
                              float ipd_factor,
                              float parallax_factor,
                              XrVector3f *out_eyes);

/*!
 * Compute display-centric view+projection for a single eye.
 *
 * Takes a pre-processed eye position (after apply_eye_factors) and computes
 * view matrix, projection matrix, FOV, and world-space eye position.
 *
 * @param processed_eye  Processed eye position (after apply_eye_factors)
 * @param screen         Physical screen dimensions
 * @param tunables       View factors (perspective_factor, virtual_display_height)
 * @param display_pose   Display pose in world space (or NULL for identity)
 * @param near_z         Near clip plane distance
 * @param far_z          Far clip plane distance
 * @param out            Output view for this eye
 */
void
display3d_compute_view(const XrVector3f *processed_eye,
                       const Display3DScreen *screen,
                       const Display3DTunables *tunables,
                       const XrPosef *display_pose,
                       float near_z,
                       float far_z,
                       Display3DView *out);

/*!
 * Default tunables (all factors = 1.0).
 */
Display3DTunables
display3d_default_tunables(void);

#ifdef __cplusplus
}
#endif
