// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0

#include "displayxr_shared_state.h"
#include <string.h>
#include <atomic>

static DisplayXRState s_state = {};
static std::atomic<int> s_tunables_write_idx{1};
static std::atomic<int> s_eyes_write_idx{1};
static std::atomic<int> s_scene_transform_write_idx{1};
static std::atomic<int> s_stereo_matrices_write_idx{1};

DisplayXRState *
displayxr_get_state(void)
{
	return &s_state;
}

void
displayxr_state_init(void)
{
	// Preserve flags that may have been set BEFORE install_hooks runs (the
	// caller of state_init). The C# side writes transparent_background_requested
	// and transparent_chroma_key_color at SubsystemRegistration, which fires
	// before the OpenXR loader hooks us.
	uint8_t  saved_transparent = s_state.transparent_background_requested;
	uint32_t saved_chroma_key  = s_state.transparent_chroma_key_color;

	memset(&s_state, 0, sizeof(s_state));

	s_state.transparent_background_requested = saved_transparent;
	s_state.transparent_chroma_key_color     = saved_chroma_key;

	// Default tunables: all factors at 1.0
	for (int i = 0; i < 2; i++) {
		s_state.tunables[i].ipd_factor = 1.0f;
		s_state.tunables[i].parallax_factor = 1.0f;
		s_state.tunables[i].perspective_factor = 1.0f;
		s_state.tunables[i].virtual_display_height = 0.0f;
		s_state.tunables[i].inv_convergence_distance = 0.0f;
		s_state.tunables[i].fov_override = 0.0f;
		s_state.tunables[i].near_z = 0.3f;
		s_state.tunables[i].far_z = 1000.0f;
		s_state.tunables[i].camera_centric = 0;
	}

	s_state.tunables_read_idx = 0;
	s_tunables_write_idx.store(1, std::memory_order_relaxed);

	s_state.eyes_read_idx = 0;
	s_eyes_write_idx.store(1, std::memory_order_relaxed);

	// Default scene transform: identity, no zoom, disabled
	for (int i = 0; i < 2; i++) {
		s_state.scene_transform[i].position[0] = 0.0f;
		s_state.scene_transform[i].position[1] = 0.0f;
		s_state.scene_transform[i].position[2] = 0.0f;
		s_state.scene_transform[i].orientation[0] = 0.0f;
		s_state.scene_transform[i].orientation[1] = 0.0f;
		s_state.scene_transform[i].orientation[2] = 0.0f;
		s_state.scene_transform[i].orientation[3] = 1.0f; // w=1 identity
		s_state.scene_transform[i].scale[0] = 1.0f;
		s_state.scene_transform[i].scale[1] = 1.0f;
		s_state.scene_transform[i].scale[2] = 1.0f;
		s_state.scene_transform[i].enabled = 0;
	}
	s_state.scene_transform_read_idx = 0;
	s_scene_transform_write_idx.store(1, std::memory_order_relaxed);

	s_state.stereo_matrices_read_idx = 0;
	s_stereo_matrices_write_idx.store(1, std::memory_order_relaxed);

	// Default to sRGB swapchain (matches Unity Linear color space, the
	// recommended setting). C# overrides via displayxr_set_use_srgb_swapchain
	// at feature init.
	s_state.use_srgb_swapchain = 1;
}

void
displayxr_state_set_tunables(const DisplayXRTunables *t)
{
	// Write to the non-read buffer, then swap
	int write_idx = s_tunables_write_idx.load(std::memory_order_relaxed);
	s_state.tunables[write_idx] = *t;

	// Swap: make this the read buffer, old read becomes write
	int old_read = write_idx;
	int new_write = 1 - write_idx;
	s_state.tunables_read_idx = old_read;
	s_tunables_write_idx.store(new_write, std::memory_order_release);
}

DisplayXRTunables
displayxr_state_get_tunables(void)
{
	int idx = s_state.tunables_read_idx;
	return s_state.tunables[idx];
}

void
displayxr_state_set_eye_positions(const XrVector3f *left, const XrVector3f *right, uint8_t tracked)
{
	int write_idx = s_eyes_write_idx.load(std::memory_order_relaxed);
	s_state.eye_positions[write_idx].left_eye = *left;
	s_state.eye_positions[write_idx].right_eye = *right;
	s_state.eye_positions[write_idx].is_tracked = tracked;

	int old_read = write_idx;
	int new_write = 1 - write_idx;
	s_state.eyes_read_idx = old_read;
	s_eyes_write_idx.store(new_write, std::memory_order_release);
}

DisplayXREyePositions
displayxr_state_get_eye_positions(void)
{
	int idx = s_state.eyes_read_idx;
	return s_state.eye_positions[idx];
}

void
displayxr_state_set_scene_transform(const DisplayXRSceneTransform *t)
{
	int write_idx = s_scene_transform_write_idx.load(std::memory_order_relaxed);
	s_state.scene_transform[write_idx] = *t;

	int old_read = write_idx;
	int new_write = 1 - write_idx;
	s_state.scene_transform_read_idx = old_read;
	s_scene_transform_write_idx.store(new_write, std::memory_order_release);
}

DisplayXRSceneTransform
displayxr_state_get_scene_transform(void)
{
	int idx = s_state.scene_transform_read_idx;
	return s_state.scene_transform[idx];
}

void
displayxr_state_set_stereo_matrices(const DisplayXRStereoMatrices *m)
{
	int write_idx = s_stereo_matrices_write_idx.load(std::memory_order_relaxed);
	s_state.stereo_matrices[write_idx] = *m;

	int old_read = write_idx;
	int new_write = 1 - write_idx;
	s_state.stereo_matrices_read_idx = old_read;
	s_stereo_matrices_write_idx.store(new_write, std::memory_order_release);
}

DisplayXRStereoMatrices
displayxr_state_get_stereo_matrices(void)
{
	int idx = s_state.stereo_matrices_read_idx;
	return s_state.stereo_matrices[idx];
}
