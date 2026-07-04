// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Window-space UI overlay (issue #67) — implementation.
//
// See displayxr_window_space_ui.h for the architecture overview. This file
// owns:
//   - The single pending Unity texture + layer descriptor (set from C#).
//   - Two SessionWsui slots: one for the hooked OpenXR session, one for the
//     standalone preview session.
//   - Lazy swapchain creation and per-frame acquire/copy/release for both.

#include "displayxr_window_space_ui.h"

#include "displayxr_extensions.h"
#include "displayxr_shared_state.h"
#include "displayxr_backend.h"
#include "displayxr_standalone_internal.h"

#include <stdio.h>
#include <string.h>

namespace {

// --- Pending state (set from Unity C# game thread) ---------------------------
struct PendingState {
	void * volatile native_tex;
	volatile int width;
	volatile int height;
	volatile float x;
	volatile float y;
	volatile float w;
	volatile float h;
	volatile float disparity;
};

constexpr int kMaxImages = 8;

// --- Per-session swapchain state ---------------------------------------------
struct SessionWsui {
	XrSwapchain swapchain;
	uint32_t sc_width;
	uint32_t sc_height;
	void *image_native_ptrs[kMaxImages];
	uint32_t image_count;
	int create_attempted;       // 1 if we tried; avoid retry spam on persistent failure
	int create_failed_logged;   // 1 if we've logged the failure once
	void *registered_tex;       // tex pointer this swapchain was created for
	int registered_w;
	int registered_h;
};

PendingState s_pending = {};
SessionWsui s_hooked = {};
SessionWsui s_standalone = {};

// --- Format selection ---
// Prefer R8G8B8A8_UNORM siblings (DXGI=28, GL=0x8058, Vulkan=37, Metal RGBA8=70).
// On Metal we'll also accept BGRA8 (=80) since CAMetalLayer drawables typically
// land on BGRA. The runtime test app picks UNORM first per
// xr_session_common.cpp:534 — match that.
// Pick an overlay swapchain format. If `app_pref` is >= 0, we try it first —
// matching the Unity-side RT's actual format is critical because D3D12
// CopyTextureRegion is invalid across formats (the runtime's compositor's
// stricter DComp/transparent path turns this into DXGI_ERROR_INVALID_CALL).
// See displayxr-unity#82 / displayxr-runtime#216.
int64_t pick_overlay_format(const int64_t *formats, uint32_t count, int64_t app_pref = -1)
{
	if (app_pref >= 0) {
		for (uint32_t i = 0; i < count; i++) {
			if (formats[i] == app_pref) return app_pref;
		}
	}
	const int64_t prefs[] = {
#if defined(__APPLE__)
		// Prefer BGRA on Apple — Unity's RenderTextureFormat.ARGB32 lands as
		// MTLPixelFormatBGRA8Unorm and we want a same-format blit.
		80,  // MTLPixelFormatBGRA8Unorm
		70,  // MTLPixelFormatRGBA8Unorm
#else
		28,  // DXGI_FORMAT_R8G8B8A8_UNORM
		87,  // DXGI_FORMAT_B8G8R8A8_UNORM
		37,  // VK_FORMAT_R8G8B8A8_UNORM
		0x8058, // GL_RGBA8
#endif
	};
	for (int64_t pref : prefs) {
		for (uint32_t i = 0; i < count; i++) {
			if (formats[i] == pref) return pref;
		}
	}
	return formats[0];
}

void release_session_state(SessionWsui *s, PFN_xrDestroySwapchain destroy_fn)
{
	if (s->swapchain != XR_NULL_HANDLE && destroy_fn != nullptr) {
		destroy_fn(s->swapchain);
	}
	memset(s, 0, sizeof(*s));
}

// --- Format enumeration helpers per path -------------------------------------
int64_t enumerate_and_pick_format_hooked(XrSession session, int64_t app_pref = -1)
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

int64_t enumerate_and_pick_format_standalone(XrSession session, const WsuiStandaloneFns *fns, int64_t app_pref = -1)
{
	uint32_t count = 0;
	if (XR_FAILED(fns->pfn_enumerate_formats(session, 0, &count, nullptr)) || count == 0)
		return -1;
	int64_t formats[32];
	if (count > 32) count = 32;
	if (XR_FAILED(fns->pfn_enumerate_formats(session, count, &count, formats)))
		return -1;
	return pick_overlay_format(formats, count, app_pref);
}

bool create_swapchain_with(XrSession session,
                            PFN_xrCreateSwapchain create_fn,
                            int64_t format,
                            uint32_t width, uint32_t height,
                            XrSwapchain *out_sc)
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
	XrResult r = create_fn(session, &info, out_sc);
	return XR_SUCCEEDED(r);
}

// --- Per-frame copy + descriptor update --------------------------------------

// Returns true if the layer is active and the descriptor is filled into
// state->window_layers[slot] for hooked-path consumption.
template <typename Backend, typename AcquireFn, typename WaitFn, typename ReleaseFn>
bool tick_session(SessionWsui *s,
                   Backend *backend,
                   AcquireFn acquire_fn,
                   WaitFn wait_fn,
                   ReleaseFn release_fn)
{
	void *unity_tex = s_pending.native_tex;
	if (unity_tex == nullptr || s->swapchain == XR_NULL_HANDLE ||
	    s->image_count == 0 || backend == nullptr) {
		return false;
	}

	XrSwapchainImageAcquireInfo acq = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
	uint32_t idx = 0;
	if (XR_FAILED(acquire_fn(s->swapchain, &acq, &idx)) || idx >= s->image_count) {
		return false;
	}
	XrSwapchainImageWaitInfo wai = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
	wai.timeout = XR_INFINITE_DURATION;
	if (XR_FAILED(wait_fn(s->swapchain, &wai))) {
		XrSwapchainImageReleaseInfo rel = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
		release_fn(s->swapchain, &rel);
		return false;
	}

	bool copied = backend->wsui_copy_to_swapchain_image(
		unity_tex, s->image_native_ptrs[idx], s->sc_width, s->sc_height);

	XrSwapchainImageReleaseInfo rel = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
	release_fn(s->swapchain, &rel);

	return copied;
}

} // anonymous namespace

// =============================================================================
// C ABI exports — called from Unity C# via P/Invoke
// =============================================================================

extern "C" void
displayxr_window_space_ui_set_texture(void *nativeTex, int width, int height)
{
	s_pending.native_tex = nativeTex;
	s_pending.width = width;
	s_pending.height = height;
	displayxr_log("[DisplayXR] wsui_set_texture: tex=%p %dx%d\n",
	    nativeTex, width, height);
}

extern "C" void
displayxr_window_space_ui_set_layer(float x, float y,
                                     float width, float height,
                                     float disparity)
{
	s_pending.x = x;
	s_pending.y = y;
	s_pending.w = width;
	s_pending.h = height;
	s_pending.disparity = disparity;
}

extern "C" void
displayxr_window_space_ui_clear(void)
{
	s_pending.native_tex = nullptr;
	displayxr_log("[DisplayXR] wsui_clear\n");
}

extern "C" int
displayxr_window_space_ui_get_pending(void **out_tex, int *out_tex_w, int *out_tex_h,
                                      float *out_x, float *out_y,
                                      float *out_lw, float *out_lh, float *out_disp)
{
	void *tex = s_pending.native_tex;
	if (out_tex)   *out_tex   = tex;
	if (out_tex_w) *out_tex_w = s_pending.width;
	if (out_tex_h) *out_tex_h = s_pending.height;
	if (out_x)     *out_x     = s_pending.x;
	if (out_y)     *out_y     = s_pending.y;
	if (out_lw)    *out_lw    = s_pending.w;
	if (out_lh)    *out_lh    = s_pending.h;
	if (out_disp)  *out_disp  = s_pending.disparity;
	return (tex != nullptr && s_pending.width > 0 && s_pending.height > 0) ? 1 : 0;
}

// =============================================================================
// Hooked path (Unity OpenXR loader)
// =============================================================================

void
wsui_hooked_pre_end_frame(XrSession session, GraphicsBackend *backend)
{
	DisplayXRState *state = displayxr_get_state();

	// Default: layer inactive. Re-enabled below if everything succeeds.
	state->window_layers[0].active = 0;

	void *unity_tex = s_pending.native_tex;
	int tex_w = s_pending.width;
	int tex_h = s_pending.height;

	if (unity_tex == nullptr || tex_w <= 0 || tex_h <= 0 ||
	    backend == nullptr || session == XR_NULL_HANDLE) {
		return;
	}

	// Recreate swapchain if dimensions or texture changed (e.g. RT recreated).
	if (s_hooked.swapchain != XR_NULL_HANDLE &&
	    (s_hooked.registered_w != tex_w || s_hooked.registered_h != tex_h)) {
#if defined(_WIN32)
		release_session_state(&s_hooked, s_real_destroy_swapchain);
#else
		// Non-Windows: no destroy fn cached. Drop reference; runtime tears
		// down on session destroy. Mild leak across resolution changes
		// within a single session.
		release_session_state(&s_hooked, nullptr);
#endif
	}

	// Lazy create.
	if (s_hooked.swapchain == XR_NULL_HANDLE) {
		if (s_hooked.create_attempted && s_hooked.create_failed_logged) {
			// Already failed once this session; don't spam.
			return;
		}
		s_hooked.create_attempted = 1;

		if (s_real_create_swapchain == nullptr ||
		    s_real_enumerate_swapchain_formats == nullptr ||
		    s_real_enumerate_swapchain_images == nullptr) {
			displayxr_log("[DisplayXR] wsui_hooked: missing swapchain fn pointers\n");
			s_hooked.create_failed_logged = 1;
			return;
		}

		// Match the swapchain format to Unity's RT format. CopyTextureRegion
		// requires identical formats; a mismatch (e.g. R8G8B8A8 dst with
		// B8G8R8A8 src) makes the GPU work invalid and removes the device
		// under DComp-backed transparent swapchains. See #82 / runtime#216.
		int64_t app_pref = backend->wsui_get_native_texture_format(unity_tex);
		int64_t format = enumerate_and_pick_format_hooked(session, app_pref);
		if (format < 0) {
			displayxr_log("[DisplayXR] wsui_hooked: format enumeration failed\n");
			s_hooked.create_failed_logged = 1;
			return;
		}

		XrSwapchain sc = XR_NULL_HANDLE;
		bool sc_ok = create_swapchain_with(session, s_real_create_swapchain, format,
		                            (uint32_t)tex_w, (uint32_t)tex_h, &sc);
		if (!sc_ok) {
			displayxr_log("[DisplayXR] wsui_hooked: xrCreateSwapchain failed (fmt=%lld)\n",
			    (long long)format);
			s_hooked.create_failed_logged = 1;
			return;
		}

		uint32_t img_count = 0;
		bool enum_ok = backend->wsui_enumerate_swapchain_images(
			sc, kMaxImages, &img_count, s_hooked.image_native_ptrs);
		if (!enum_ok) {
			displayxr_log("[DisplayXR] wsui_hooked: backend enumerate failed\n");
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
		displayxr_log("[DisplayXR] wsui_hooked: swapchain created %ux%u (%u images, fmt=%lld)\n",
		    s_hooked.sc_width, s_hooked.sc_height, img_count, (long long)format);
	}

	bool ok = tick_session(&s_hooked, backend,
		s_real_acquire_swapchain_image,
		s_real_wait_swapchain_image,
		s_real_release_swapchain_image);

	if (!ok) return;

	DisplayXRWindowLayer *wl = &state->window_layers[0];
	wl->swapchain = s_hooked.swapchain;
	wl->swapchain_width = s_hooked.sc_width;
	wl->swapchain_height = s_hooked.sc_height;
	wl->x = s_pending.x;
	wl->y = s_pending.y;
	wl->width = s_pending.w;
	wl->height = s_pending.h;
	wl->disparity = s_pending.disparity;
	wl->active = 1;
}

void
wsui_hooked_on_session_destroyed(void)
{
	// The runtime tears down our swapchain as part of session destroy;
	// we just drop the handle. (s_real_destroy_swapchain is platform-gated
	// and may be nullptr by this point — the deferred-destroy mechanism
	// in displayxr_hooks.cpp handles cleanup.)
	memset(&s_hooked, 0, sizeof(s_hooked));
	DisplayXRState *state = displayxr_get_state();
	state->window_layers[0].active = 0;
	state->window_layers[0].swapchain = XR_NULL_HANDLE;
}

// =============================================================================
// Standalone path (preview window / Play Mode integration)
// =============================================================================

int
wsui_standalone_pre_end_frame(XrSession session,
                               const WsuiStandaloneFns *fns,
                               StandaloneGraphicsBackend *backend,
                               XrCompositionLayerWindowSpaceEXT *out_layer)
{
	if (out_layer == nullptr) return 0;
	memset(out_layer, 0, sizeof(*out_layer));

	void *unity_tex = s_pending.native_tex;
	int tex_w = s_pending.width;
	int tex_h = s_pending.height;
	if (unity_tex == nullptr || tex_w <= 0 || tex_h <= 0 ||
	    fns == nullptr || backend == nullptr || session == XR_NULL_HANDLE) {
		return 0;
	}

	if (s_standalone.swapchain != XR_NULL_HANDLE &&
	    (s_standalone.registered_w != tex_w || s_standalone.registered_h != tex_h)) {
		release_session_state(&s_standalone, fns->pfn_destroy_swapchain);
	}

	if (s_standalone.swapchain == XR_NULL_HANDLE) {
		if (s_standalone.create_attempted && s_standalone.create_failed_logged) {
			return 0;
		}
		s_standalone.create_attempted = 1;

		// Force the wsui swapchain format to match the cross-device bridge
		// texture format on Windows (DXGI_FORMAT_B8G8R8A8_UNORM = 87).
		// Without this, the picker defaults to RGBA8 on non-Apple, the
		// bridge stays BGRA8, the SA-device CopyTextureRegion(bridge -> sc)
		// is cross-format, D3D12 strict validation rejects it at GPU exec
		// time, the runtime device gets removed, and the next atlas
		// xrEndFrame crashes in the GPU driver. The bridge format is
		// chosen to match Unity's wsui RT, which the C# code creates as
		// B8G8R8A8_UNorm (URP RenderGraph requirement).
#if defined(_WIN32)
		const int64_t bridge_format_pref = 87;
#else
		const int64_t bridge_format_pref = -1;
#endif
		int64_t format = enumerate_and_pick_format_standalone(session, fns, bridge_format_pref);
		if (format < 0) {
			displayxr_log("[DisplayXR] wsui_sa: format enumeration failed\n");
			s_standalone.create_failed_logged = 1;
			return 0;
		}

		XrSwapchain sc = XR_NULL_HANDLE;
		if (!create_swapchain_with(session, fns->pfn_create_swapchain, format,
		                            (uint32_t)tex_w, (uint32_t)tex_h, &sc)) {
			displayxr_log("[DisplayXR] wsui_sa: xrCreateSwapchain failed\n");
			s_standalone.create_failed_logged = 1;
			return 0;
		}

		uint32_t img_count = 0;
		if (!backend->wsui_enumerate_swapchain_images(
			sc, fns->pfn_enumerate_images, kMaxImages,
			&img_count, s_standalone.image_native_ptrs)) {
			displayxr_log("[DisplayXR] wsui_sa: backend enumerate failed\n");
			fns->pfn_destroy_swapchain(sc);
			s_standalone.create_failed_logged = 1;
			return 0;
		}

		s_standalone.swapchain = sc;
		s_standalone.sc_width = (uint32_t)tex_w;
		s_standalone.sc_height = (uint32_t)tex_h;
		s_standalone.image_count = img_count;
		s_standalone.registered_tex = unity_tex;
		s_standalone.registered_w = tex_w;
		s_standalone.registered_h = tex_h;
		displayxr_log("[DisplayXR] wsui_sa: swapchain created %ux%u (%u images, fmt=%lld)\n",
		    s_standalone.sc_width, s_standalone.sc_height, img_count, (long long)format);

		// Cross-device bridge for Windows: SA device != Unity device, so a
		// direct Unity-RT → swapchain copy is invalid. Create a SHARED
		// texture on the SA device, opened on Unity's device. C# copies its
		// wsui RT into the Unity-side bridge each frame (Graphics.CopyTexture),
		// then tick_session below copies SA-side bridge → swapchain image.
		// No-op on platforms where the backend default-implements the bridge
		// methods (Metal: unified device, doesn't need a bridge).
		backend->wsui_create_bridge((uint32_t)tex_w, (uint32_t)tex_h);
	}

	bool ok = tick_session(&s_standalone, backend,
		fns->pfn_acquire,
		fns->pfn_wait,
		fns->pfn_release);

	if (!ok) return 0;

	out_layer->type = XR_TYPE_COMPOSITION_LAYER_WINDOW_SPACE_EXT;
	out_layer->next = nullptr;
	out_layer->layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
	out_layer->subImage.swapchain = s_standalone.swapchain;
	out_layer->subImage.imageRect.offset = {0, 0};
	out_layer->subImage.imageRect.extent = {
		(int32_t)s_standalone.sc_width, (int32_t)s_standalone.sc_height};
	out_layer->subImage.imageArrayIndex = 0;
	out_layer->x = s_pending.x;
	out_layer->y = s_pending.y;
	out_layer->width = s_pending.w;
	out_layer->height = s_pending.h;
	out_layer->disparity = s_pending.disparity;
	return 1;
}

void
wsui_standalone_on_session_destroyed(PFN_xrDestroySwapchain destroy_fn)
{
	if (s_standalone.swapchain != XR_NULL_HANDLE && destroy_fn != nullptr) {
		destroy_fn(s_standalone.swapchain);
	}
	memset(&s_standalone, 0, sizeof(s_standalone));
}
