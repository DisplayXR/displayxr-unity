// Copyright 2025-2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Camera-centric multiview math for 3D displays
 *
 * Canonical implementation — see camera3d_view.h for API docs.
 */

#include "camera3d_view.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Internal helpers (duplicated from display3d_view.c as static to avoid
// exposing internal symbols — ~60 lines total)
// ============================================================================

// Quaternion-rotate a vector: v' = q * v * q^-1
static XrVector3f
cam3d_quat_rotate(XrQuaternionf q, XrVector3f v)
{
	float tx = 2.0f * (q.y * v.z - q.z * v.y);
	float ty = 2.0f * (q.z * v.x - q.x * v.z);
	float tz = 2.0f * (q.x * v.y - q.y * v.x);

	XrVector3f out;
	out.x = v.x + q.w * tx + (q.y * tz - q.z * ty);
	out.y = v.y + q.w * ty + (q.z * tx - q.x * tz);
	out.z = v.z + q.w * tz + (q.x * ty - q.y * tx);
	return out;
}

// Set a column-major 4x4 matrix to identity.
static void
cam3d_mat4_identity(float *m)
{
	memset(m, 0, 16 * sizeof(float));
	m[0] = m[5] = m[10] = m[15] = 1.0f;
}

// Build rotation matrix (column-major) from quaternion.
static void
cam3d_mat4_from_quat(float *m, XrQuaternionf q)
{
	float qx = q.x, qy = q.y, qz = q.z, qw = q.w;

	cam3d_mat4_identity(m);
	m[0] = 1.0f - 2.0f * (qy * qy + qz * qz);
	m[1] = 2.0f * (qx * qy + qz * qw);
	m[2] = 2.0f * (qx * qz - qy * qw);
	m[4] = 2.0f * (qx * qy - qz * qw);
	m[5] = 1.0f - 2.0f * (qx * qx + qz * qz);
	m[6] = 2.0f * (qy * qz + qx * qw);
	m[8] = 2.0f * (qx * qz + qy * qw);
	m[9] = 2.0f * (qy * qz - qx * qw);
	m[10] = 1.0f - 2.0f * (qx * qx + qy * qy);
}

// Build view matrix from camera pose and world-space eye position.
// viewMatrix = transpose(R) * translate(-eye_world)
static void
cam3d_build_view_matrix(float *out, XrQuaternionf orientation, XrVector3f eye_world)
{
	float rot[16];
	cam3d_mat4_from_quat(rot, orientation);

	// Transpose rotation (inverse of orthonormal matrix)
	float inv_rot[16];
	cam3d_mat4_identity(inv_rot);
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++)
			inv_rot[j * 4 + i] = rot[i * 4 + j];

	// Translation: -eye_world
	float inv_trans[16];
	cam3d_mat4_identity(inv_trans);
	inv_trans[12] = -eye_world.x;
	inv_trans[13] = -eye_world.y;
	inv_trans[14] = -eye_world.z;

	// viewMatrix = inv_rot * inv_trans (column-major multiplication)
	float tmp[16];
	for (int col = 0; col < 4; col++) {
		for (int row = 0; row < 4; row++) {
			float sum = 0.0f;
			for (int k = 0; k < 4; k++) {
				sum += inv_rot[k * 4 + row] * inv_trans[col * 4 + k];
			}
			tmp[col * 4 + row] = sum;
		}
	}
	memcpy(out, tmp, sizeof(tmp));
}

// Build asymmetric frustum projection (column-major) from tangent half-angles.
static void
cam3d_build_projection_from_tangents(float tan_left,
                                     float tan_right,
                                     float tan_down,
                                     float tan_up,
                                     float near_z,
                                     float far_z,
                                     float *out)
{
	float left = -tan_left * near_z;
	float right = tan_right * near_z;
	float bottom = -tan_down * near_z;
	float top = tan_up * near_z;

	float w = right - left;
	float h = top - bottom;

	memset(out, 0, 16 * sizeof(float));
	out[0] = 2.0f * near_z / w;
	out[5] = 2.0f * near_z / h;
	out[8] = (right + left) / w;
	out[9] = (top + bottom) / h;
	out[10] = -(far_z + near_z) / (far_z - near_z);
	out[11] = -1.0f;
	out[14] = -(2.0f * far_z * near_z) / (far_z - near_z);
}

// ============================================================================
// Public API
// ============================================================================

Camera3DTunables
camera3d_default_tunables(void)
{
	Camera3DTunables t;
	t.ipd_factor = 1.0f;
	t.parallax_factor = 1.0f;
	t.inv_convergence_distance = 1.0f;
	t.half_tan_vfov = 0.32491969623f; // tan(18 deg) → 36° vFOV
	t.clip_at_display_plane = 0;
	return t;
}

void
camera3d_compute_view(const XrVector3f *processed_eye,
                      float nominal_z,
                      const Display3DScreen *screen,
                      const Camera3DTunables *tunables,
                      const XrPosef *camera_pose,
                      float near_z,
                      float far_z,
                      Camera3DView *out)
{
	Camera3DTunables t = tunables ? *tunables : camera3d_default_tunables();

	XrQuaternionf cam_ori = {0, 0, 0, 1};
	XrVector3f cam_pos = {0, 0, 0};
	if (camera_pose) {
		cam_ori = camera_pose->orientation;
		cam_pos = camera_pose->position;
	}

	float aspect = screen->width_m / screen->height_m;
	float ro = t.half_tan_vfov * aspect;
	float uo = t.half_tan_vfov;
	float invd = t.inv_convergence_distance;

	// eye_local = displacement from nominal screen plane
	XrVector3f eye_local;
	eye_local.x = processed_eye->x;
	eye_local.y = processed_eye->y;
	eye_local.z = processed_eye->z - nominal_z;

	// Transform to world space
	XrVector3f eye_world = cam3d_quat_rotate(cam_ori, eye_local);
	eye_world.x += cam_pos.x;
	eye_world.y += cam_pos.y;
	eye_world.z += cam_pos.z;
	out->eye_world = eye_world;

	// Build view matrix
	cam3d_build_view_matrix(out->view_matrix, cam_ori, eye_world);
	out->orientation = cam_ori;

	// Scale by inv_convergence_distance for projection shifts
	float dx = eye_local.x * invd;
	float dy = eye_local.y * invd;
	float dz = eye_local.z * invd;

	// Asymmetric frustum tangent half-angles
	float denom = 1.0f + dz;
	float tan_right = (ro - dx) / denom;
	float tan_left = (ro + dx) / denom;
	float tan_up = (uo - dy) / denom;
	float tan_down = (uo + dy) / denom;

	// Foreground-only mode (camera-centric): clip at the convergence distance.
	// invd == 0 means parallel projection (convergence at infinity) — flag is
	// a no-op in that case. Otherwise convergence_dist = 1/invd is the analogue
	// of the display plane and we set far to that.
	float effective_far = far_z;
	if (t.clip_at_display_plane && invd > 0.0001f) {
		float conv = 1.0f / invd;
		float min_far = near_z + 0.001f;
		effective_far = (conv > min_far) ? conv : min_far;
	}

	// Build projection matrix
	cam3d_build_projection_from_tangents(tan_left, tan_right, tan_down, tan_up,
	                                     near_z, effective_far, out->projection_matrix);

	// Convert tangents to XrFovf
	out->fov.angleLeft = -atanf(tan_left);
	out->fov.angleRight = atanf(tan_right);
	out->fov.angleUp = atanf(tan_up);
	out->fov.angleDown = -atanf(tan_down);
}

void
camera3d_compute_views(const XrVector3f *raw_eyes,
                              uint32_t count,
                              const XrVector3f *nominal_viewer,
                              const Display3DScreen *screen,
                              const Camera3DTunables *tunables,
                              const XrPosef *camera_pose,
                              float near_z,
                              float far_z,
                              Camera3DView *out_views)
{
	Camera3DTunables t = tunables ? *tunables : camera3d_default_tunables();

	float nom_z = 0.5f;
	if (nominal_viewer) {
		nom_z = nominal_viewer->z;
	}

	// Apply IPD and parallax factors (N-view path)
	XrVector3f stack_processed[8];
	XrVector3f *processed = (count <= 8) ? stack_processed : (XrVector3f *)malloc(count * sizeof(XrVector3f));
	display3d_apply_eye_factors_n(raw_eyes, count, nominal_viewer,
	                              t.ipd_factor, t.parallax_factor,
	                              processed);

	// Compute each view via single-eye primitive
	for (uint32_t i = 0; i < count; i++) {
		camera3d_compute_view(&processed[i], nom_z, screen, &t,
		                      camera_pose, near_z, far_z, &out_views[i]);
	}

	if (count > 8)
		free(processed);
}
