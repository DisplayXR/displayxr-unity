// Copyright 2025-2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Unified display-centric multiview math for 3D displays
 *
 * Canonical implementation — see display3d_view.h for API docs.
 */

#include "display3d_view.h"
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Internal helpers
// ============================================================================

// Quaternion-rotate a vector: v' = q * v * q^-1
// Uses the efficient cross-product form (no matrix conversion).
static XrVector3f
quat_rotate(XrQuaternionf q, XrVector3f v)
{
	// t = 2 * cross(q.xyz, v)
	float tx = 2.0f * (q.y * v.z - q.z * v.y);
	float ty = 2.0f * (q.z * v.x - q.x * v.z);
	float tz = 2.0f * (q.x * v.y - q.y * v.x);

	// v' = v + w*t + cross(q.xyz, t)
	XrVector3f out;
	out.x = v.x + q.w * tx + (q.y * tz - q.z * ty);
	out.y = v.y + q.w * ty + (q.z * tx - q.x * tz);
	out.z = v.z + q.w * tz + (q.x * ty - q.y * tx);
	return out;
}

// Set a column-major 4x4 matrix to identity.
static void
mat4_identity(float *m)
{
	memset(m, 0, 16 * sizeof(float));
	m[0] = m[5] = m[10] = m[15] = 1.0f;
}

// Build rotation matrix (column-major) from quaternion.
static void
mat4_from_quat(float *m, XrQuaternionf q)
{
	float qx = q.x, qy = q.y, qz = q.z, qw = q.w;

	mat4_identity(m);
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

// Build view matrix from display pose and world-space eye position.
// viewMatrix = transpose(R) * translate(-eye_world)
// where R is the rotation matrix from display_pose.orientation.
static void
build_view_matrix(float *out, XrQuaternionf orientation, XrVector3f eye_world)
{
	// Rotation matrix from quaternion
	float rot[16];
	mat4_from_quat(rot, orientation);

	// Transpose rotation (inverse of orthonormal matrix)
	float inv_rot[16];
	mat4_identity(inv_rot);
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++)
			inv_rot[j * 4 + i] = rot[i * 4 + j];

	// Translation: -eye_world
	float inv_trans[16];
	mat4_identity(inv_trans);
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

// ============================================================================
// Public API
// ============================================================================

Display3DTunables
display3d_default_tunables(void)
{
	Display3DTunables t;
	t.ipd_factor = 1.0f;
	t.parallax_factor = 1.0f;
	t.perspective_factor = 1.0f;
	t.virtual_display_height = 0.0f;
	t.clip_at_display_plane = 0;
	return t;
}

void
display3d_apply_eye_factors(const XrVector3f *raw_left,
                            const XrVector3f *raw_right,
                            const XrVector3f *nominal_viewer,
                            float ipd_factor,
                            float parallax_factor,
                            XrVector3f *out_left,
                            XrVector3f *out_right)
{
	// Default nominal viewer if NULL (only z is used — x/y lerp toward origin)
	float nom_z = 0.5f;
	if (nominal_viewer) {
		nom_z = nominal_viewer->z;
	}

	// Step 1: IPD factor — scale inter-eye vector, keep center fixed
	float cx = (raw_left->x + raw_right->x) * 0.5f;
	float cy = (raw_left->y + raw_right->y) * 0.5f;
	float cz = (raw_left->z + raw_right->z) * 0.5f;

	float lvx = (raw_left->x - cx) * ipd_factor;
	float lvy = (raw_left->y - cy) * ipd_factor;
	float lvz = (raw_left->z - cz) * ipd_factor;

	float rvx = (raw_right->x - cx) * ipd_factor;
	float rvy = (raw_right->y - cy) * ipd_factor;
	float rvz = (raw_right->z - cz) * ipd_factor;

	// Step 2: Parallax factor — lerp center toward (0, 0, nom_z).
	// We use origin for x/y so that reducing parallax drives the viewpoint
	// to the display-center axis rather than to an arbitrary nominal offset.
	float cx2 = parallax_factor * cx;
	float cy2 = parallax_factor * cy;
	float cz2 = nom_z + parallax_factor * (cz - nom_z);

	out_left->x = cx2 + lvx;
	out_left->y = cy2 + lvy;
	out_left->z = cz2 + lvz;

	out_right->x = cx2 + rvx;
	out_right->y = cy2 + rvy;
	out_right->z = cz2 + rvz;
}

XrFovf
display3d_compute_fov(XrVector3f eye_pos, float screen_width_m, float screen_height_m)
{
	float ez = eye_pos.z;
	if (ez <= 0.001f)
		ez = 0.65f; // Fallback: ~arm's length

	float half_w = screen_width_m / 2.0f;
	float half_h = screen_height_m / 2.0f;
	float ex = eye_pos.x;
	float ey = eye_pos.y;

	XrFovf fov;
	fov.angleLeft = atanf((-half_w - ex) / ez);
	fov.angleRight = atanf((half_w - ex) / ez);
	fov.angleUp = atanf((half_h - ey) / ez);
	fov.angleDown = atanf((-half_h - ey) / ez);

	return fov;
}

void
display3d_compute_projection(XrVector3f eye_pos,
                             float screen_width_m,
                             float screen_height_m,
                             float near_z,
                             float far_z,
                             float *out_matrix)
{
	float ez = eye_pos.z;
	if (ez <= 0.001f)
		ez = 0.65f;

	float half_w = screen_width_m / 2.0f;
	float half_h = screen_height_m / 2.0f;
	float ex = eye_pos.x;
	float ey = eye_pos.y;

	// Near-plane edge distances (similar triangles: project screen edges through eye)
	float left = near_z * (-half_w - ex) / ez;
	float right = near_z * (half_w - ex) / ez;
	float bottom = near_z * (-half_h - ey) / ez;
	float top = near_z * (half_h - ey) / ez;

	float w = right - left;
	float h = top - bottom;

	// Column-major asymmetric frustum projection matrix
	memset(out_matrix, 0, 16 * sizeof(float));
	out_matrix[0] = 2.0f * near_z / w;
	out_matrix[5] = 2.0f * near_z / h;
	out_matrix[8] = (right + left) / w;
	out_matrix[9] = (top + bottom) / h;
	out_matrix[10] = -(far_z + near_z) / (far_z - near_z);
	out_matrix[11] = -1.0f;
	out_matrix[14] = -(2.0f * far_z * near_z) / (far_z - near_z);
}

void
display3d_apply_eye_factors_n(const XrVector3f *raw_eyes,
                              uint32_t count,
                              const XrVector3f *nominal_viewer,
                              float ipd_factor,
                              float parallax_factor,
                              XrVector3f *out_eyes)
{
	if (count == 0) {
		return;
	}

	float nom_z = 0.5f;
	if (nominal_viewer) {
		nom_z = nominal_viewer->z;
	}

	// Compute centroid of all eyes
	float cx = 0.0f, cy = 0.0f, cz = 0.0f;
	for (uint32_t i = 0; i < count; i++) {
		cx += raw_eyes[i].x;
		cy += raw_eyes[i].y;
		cz += raw_eyes[i].z;
	}
	float inv_n = 1.0f / (float)count;
	cx *= inv_n;
	cy *= inv_n;
	cz *= inv_n;

	// Parallax factor: lerp center toward (0, 0, nom_z)
	float cx2 = parallax_factor * cx;
	float cy2 = parallax_factor * cy;
	float cz2 = nom_z + parallax_factor * (cz - nom_z);

	// IPD factor: scale each eye's offset from center
	for (uint32_t i = 0; i < count; i++) {
		out_eyes[i].x = cx2 + (raw_eyes[i].x - cx) * ipd_factor;
		out_eyes[i].y = cy2 + (raw_eyes[i].y - cy) * ipd_factor;
		out_eyes[i].z = cz2 + (raw_eyes[i].z - cz) * ipd_factor;
	}
}

void
display3d_compute_view(const XrVector3f *processed_eye,
                       const Display3DScreen *screen,
                       const Display3DTunables *tunables,
                       const XrPosef *display_pose,
                       float near_z,
                       float far_z,
                       Display3DView *out)
{
	Display3DTunables t = tunables ? *tunables : display3d_default_tunables();

	XrQuaternionf disp_ori = {0, 0, 0, 1};
	XrVector3f disp_pos = {0, 0, 0};
	if (display_pose) {
		disp_ori = display_pose->orientation;
		disp_pos = display_pose->position;
	}

	float m2v = t.virtual_display_height / screen->height_m;

	// Apply perspective * m2v to eye XYZ
	float es = t.perspective_factor * m2v;
	XrVector3f eye_scaled;
	eye_scaled.x = processed_eye->x * es;
	eye_scaled.y = processed_eye->y * es;
	eye_scaled.z = processed_eye->z * es;
	out->eye_display = eye_scaled;

	// Apply m2v to screen dimensions
	float kScreenW = screen->width_m * m2v;
	float kScreenH = screen->height_m * m2v;

	// Transform display-space eye -> world-space via display_pose
	XrVector3f eye_world = quat_rotate(disp_ori, eye_scaled);
	eye_world.x += disp_pos.x;
	eye_world.y += disp_pos.y;
	eye_world.z += disp_pos.z;
	out->eye_world = eye_world;

	// Build view matrix + projection + FOV
	build_view_matrix(out->view_matrix, disp_ori, eye_world);
	out->orientation = disp_ori;

	// Foreground-only mode: override far_z per view with this view's distance
	// to the display plane. eye_scaled is in app units (already m2v-scaled),
	// so |eye_scaled.z| is the eye-to-display distance in projection-matrix
	// units. We clamp to a sane minimum above near_z to avoid degenerate
	// matrices (e.g. far == near → division by zero in m22/m23).
	float effective_far = far_z;
	if (t.clip_at_display_plane) {
		float ez_abs = fabsf(eye_scaled.z);
		float min_far = near_z + 0.001f;
		effective_far = (ez_abs > min_far) ? ez_abs : min_far;
	}
	display3d_compute_projection(eye_scaled, kScreenW, kScreenH,
	                             near_z, effective_far, out->projection_matrix);
	out->fov = display3d_compute_fov(eye_scaled, kScreenW, kScreenH);
}

void
display3d_compute_views(const XrVector3f *raw_eyes,
                               uint32_t count,
                               const XrVector3f *nominal_viewer,
                               const Display3DScreen *screen,
                               const Display3DTunables *tunables,
                               const XrPosef *display_pose,
                               float near_z,
                               float far_z,
                               Display3DView *out_views)
{
	Display3DTunables t = tunables ? *tunables : display3d_default_tunables();

	// Apply IPD and parallax factors (N-view path)
	XrVector3f stack_processed[8];
	XrVector3f *processed = (count <= 8) ? stack_processed : (XrVector3f *)malloc(count * sizeof(XrVector3f));
	display3d_apply_eye_factors_n(raw_eyes, count, nominal_viewer,
	                              t.ipd_factor, t.parallax_factor,
	                              processed);

	// Compute each view via single-eye primitive
	for (uint32_t i = 0; i < count; i++) {
		display3d_compute_view(&processed[i], screen, &t,
		                       display_pose, near_z, far_z,
		                       &out_views[i]);
	}

	if (count > 8)
		free(processed);
}
