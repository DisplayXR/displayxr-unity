// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Local2D overlay (#439/#491) — implementation. See displayxr_local2d.h.
//
// Hooked path only (built apps with Unity's OpenXR loader). Models
// displayxr_window_space_ui.cpp's hooked path and reuses the GraphicsBackend
// wsui_* swapchain helpers (copy / enumerate / format) — the per-frame texture
// → swapchain mechanics are identical; only the emitted composition layer
// (XrCompositionLayerLocal2DEXT at a pixel rect) differs, and that lives in
// hooked_xrEndFrame.

#include "displayxr_local2d.h"

#include "displayxr_extensions.h"
#include "displayxr_shared_state.h"
#include "displayxr_hooks_internal.h"

#include <stdio.h>
#include <string.h>

namespace {

struct PendingState {
	void * volatile native_tex;
	volatile int width;
	volatile int height;
	volatile int rect_x;
	volatile int rect_y;
	volatile int rect_w;
	volatile int rect_h;
};

constexpr int kMaxImages = 8;

struct SessionLocal2D {
	XrSwapchain swapchain;
	uint32_t sc_width;
	uint32_t sc_height;
	void *image_native_ptrs[kMaxImages];
	uint32_t image_count;
	int create_attempted;
	int create_failed_logged;
	void *registered_tex;
	int registered_w;
	int registered_h;
};

PendingState s_pending = {};
SessionLocal2D s_hooked = {};

// Mirror wsui's format picker — match the Unity RT format (CopyTextureRegion /
// cross-device copy requires identical formats; see displayxr-unity#82).
int64_t pick_overlay_format(const int64_t *formats, uint32_t count, int64_t app_pref)
{
	if (app_pref >= 0) {
		for (uint32_t i = 0; i < count; i++)
			if (formats[i] == app_pref) return app_pref;
	}
	const int64_t prefs[] = {
#if defined(__APPLE__)
		80, 70,
#else
		28,     // DXGI_FORMAT_R8G8B8A8_UNORM
		87,     // DXGI_FORMAT_B8G8R8A8_UNORM
		37,     // VK_FORMAT_R8G8B8A8_UNORM
		0x8058, // GL_RGBA8
#endif
	};
	for (int64_t pref : prefs) {
		for (uint32_t i = 0; i < count; i++)
			if (formats[i] == pref) return pref;
	}
	return formats[0];
}

void release_session_state(SessionLocal2D *s, PFN_xrDestroySwapchain destroy_fn)
{
	if (s->swapchain != XR_NULL_HANDLE && destroy_fn != nullptr) {
		destroy_fn(s->swapchain);
	}
	memset(s, 0, sizeof(*s));
}

int64_t enumerate_and_pick_format_hooked(XrSession session, int64_t app_pref)
{
	if (s_real_enumerate_swapchain_formats == nullptr) return -1;
	uint32_t count = 0;
	if (XR_FAILED(s_real_enumerate_swapchain_formats(session, 0, &count, nullptr)) || count == 0)
		return -1;
	int64_t formats[32];
	if (count > 32) count = 32;
	if (XR_FAILED(s_real_enumerate_swapchain_formats(session, count, &count, formats)))
		return -1;
	return pick_overlay_format(formats, count, app_pref);
}

bool create_swapchain_with(XrSession session, int64_t format,
                            uint32_t width, uint32_t height, XrSwapchain *out_sc)
{
	XrSwapchainCreateInfo info = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
	info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
	                  XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
	                  XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
	info.format = format;
	info.sampleCount = 1;
	info.width = width;
	info.height = height;
	info.faceCount = 1;
	info.arraySize = 1;
	info.mipCount = 1;
	return XR_SUCCEEDED(s_real_create_swapchain(session, &info, out_sc));
}

bool tick_session(SessionLocal2D *s, GraphicsBackend *backend)
{
	void *unity_tex = s_pending.native_tex;
	if (unity_tex == nullptr || s->swapchain == XR_NULL_HANDLE ||
	    s->image_count == 0 || backend == nullptr) {
		return false;
	}

	XrSwapchainImageAcquireInfo acq = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
	uint32_t idx = 0;
	if (XR_FAILED(s_real_acquire_swapchain_image(s->swapchain, &acq, &idx)) ||
	    idx >= s->image_count) {
		return false;
	}
	XrSwapchainImageWaitInfo wai = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
	wai.timeout = XR_INFINITE_DURATION;
	if (XR_FAILED(s_real_wait_swapchain_image(s->swapchain, &wai))) {
		XrSwapchainImageReleaseInfo rel = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
		s_real_release_swapchain_image(s->swapchain, &rel);
		return false;
	}

	bool copied = backend->wsui_copy_to_swapchain_image(
		unity_tex, s->image_native_ptrs[idx], s->sc_width, s->sc_height);

	XrSwapchainImageReleaseInfo rel = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
	s_real_release_swapchain_image(s->swapchain, &rel);

	return copied;
}

} // anonymous namespace

// =============================================================================
// C ABI exports
// =============================================================================

extern "C" void
displayxr_local2d_set_texture(void *unity_native_tex, int width, int height)
{
	s_pending.native_tex = unity_native_tex;
	s_pending.width = width;
	s_pending.height = height;
	displayxr_log("[DisplayXR] local2d_set_texture: tex=%p %dx%d\n",
	    unity_native_tex, width, height);
}

extern "C" void
displayxr_local2d_set_rect(int x, int y, int width, int height)
{
	s_pending.rect_x = x;
	s_pending.rect_y = y;
	s_pending.rect_w = width;
	s_pending.rect_h = height;
}

extern "C" void
displayxr_local2d_clear(void)
{
	s_pending.native_tex = nullptr;
	displayxr_log("[DisplayXR] local2d_clear\n");
}

// =============================================================================
// Hooked path
// =============================================================================

void
local2d_hooked_pre_end_frame(XrSession session, GraphicsBackend *backend)
{
	DisplayXRState *state = displayxr_get_state();

	// Default: layer inactive. Re-enabled below if everything succeeds.
	state->local2d_layer.active = 0;

	void *unity_tex = s_pending.native_tex;
	int tex_w = s_pending.width;
	int tex_h = s_pending.height;

	if (unity_tex == nullptr || tex_w <= 0 || tex_h <= 0 ||
	    backend == nullptr || session == XR_NULL_HANDLE) {
		return;
	}

	// Recreate the overlay swapchain if the Unity RT was reallocated (dims
	// changed). This is exactly the robustness the legacy surround path
	// lacked — a recreated RT there left the runtime copying a dead resource.
	if (s_hooked.swapchain != XR_NULL_HANDLE &&
	    (s_hooked.registered_w != tex_w || s_hooked.registered_h != tex_h)) {
#if defined(_WIN32)
		release_session_state(&s_hooked, s_real_destroy_swapchain);
#else
		release_session_state(&s_hooked, nullptr);
#endif
	}

	if (s_hooked.swapchain == XR_NULL_HANDLE) {
		if (s_hooked.create_attempted && s_hooked.create_failed_logged) {
			return;
		}
		s_hooked.create_attempted = 1;

		if (s_real_create_swapchain == nullptr ||
		    s_real_enumerate_swapchain_formats == nullptr) {
			displayxr_log("[DisplayXR] local2d_hooked: missing swapchain fn pointers\n");
			s_hooked.create_failed_logged = 1;
			return;
		}

		int64_t app_pref = backend->wsui_get_native_texture_format(unity_tex);
		int64_t format = enumerate_and_pick_format_hooked(session, app_pref);
		if (format < 0) {
			displayxr_log("[DisplayXR] local2d_hooked: format enumeration failed\n");
			s_hooked.create_failed_logged = 1;
			return;
		}

		XrSwapchain sc = XR_NULL_HANDLE;
		if (!create_swapchain_with(session, format,
		                           (uint32_t)tex_w, (uint32_t)tex_h, &sc)) {
			displayxr_log("[DisplayXR] local2d_hooked: xrCreateSwapchain failed (fmt=%lld)\n",
			    (long long)format);
			s_hooked.create_failed_logged = 1;
			return;
		}

		uint32_t img_count = 0;
		if (!backend->wsui_enumerate_swapchain_images(
			sc, kMaxImages, &img_count, s_hooked.image_native_ptrs)) {
			displayxr_log("[DisplayXR] local2d_hooked: backend enumerate failed\n");
#if defined(_WIN32)
			if (s_real_destroy_swapchain) s_real_destroy_swapchain(sc);
#endif
			s_hooked.create_failed_logged = 1;
			return;
		}

		s_hooked.swapchain = sc;
		s_hooked.sc_width = (uint32_t)tex_w;
		s_hooked.sc_height = (uint32_t)tex_h;
		s_hooked.image_count = img_count;
		s_hooked.registered_tex = unity_tex;
		s_hooked.registered_w = tex_w;
		s_hooked.registered_h = tex_h;
		displayxr_log("[DisplayXR] local2d_hooked: swapchain created %ux%u (%u images, fmt=%lld)\n",
		    s_hooked.sc_width, s_hooked.sc_height, img_count, (long long)format);
	}

	if (!tick_session(&s_hooked, backend)) return;

	DisplayXRLocal2DLayer *l2 = &state->local2d_layer;
	l2->swapchain = s_hooked.swapchain;
	l2->swapchain_width = s_hooked.sc_width;
	l2->swapchain_height = s_hooked.sc_height;
	l2->rect_x = s_pending.rect_x;
	l2->rect_y = s_pending.rect_y;
	l2->rect_w = s_pending.rect_w;
	l2->rect_h = s_pending.rect_h;
	l2->active = 1;
}

void
local2d_hooked_on_session_destroyed(void)
{
	memset(&s_hooked, 0, sizeof(s_hooked));
	DisplayXRState *state = displayxr_get_state();
	state->local2d_layer.active = 0;
	state->local2d_layer.swapchain = XR_NULL_HANDLE;
}
