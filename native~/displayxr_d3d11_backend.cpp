// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
//
// D3D11 graphics backend for the DisplayXR hook chain.
// Extracted from displayxr_hooks.cpp — no logic changes.
//
// Contains:
//   - D3D11 typed-swapchain substitution (issue #91, cherry-picked to main as
//     2a9a933): pair each Unity color swapchain with an R8G8B8A8_UNORM_SRGB
//     sibling so the compositor gets typed textures.
//   - N-view tile atlas composite (main commit b7e2d22): per-frame, copy each
//     typed shadow texture into a tile of a single atlas swapchain sized to
//     the current rendering mode's (tile_cols × view_w, tile_rows × view_h),
//     and patch all projection views to reference the atlas with tile rects.
//     This routes the runtime D3D11 compositor through its working zero-copy
//     fast path for any view count (stereo, quad, light field), side-stepping
//     the latent-broken per-view renderer fallback (runtime issue #143).

#if defined(_WIN32)

#include "displayxr_hooks_internal.h"

#define DISPLAYXR_MAX_RENDERING_MODES 16

class D3D11Backend : public GraphicsBackend {
public:
	// --- D3D11 device for GPU sync (captured from session binding chain) ---
	ID3D11Device        *device  = nullptr;
	ID3D11DeviceContext *context = nullptr;

	// --- Typed swapchain substitution pool (2a9a933) ---
	static const int kMaxScSubs = 8;
	D3D11ScSub sc_subs[kMaxScSubs] = {};
	int sc_sub_count = 0;

	// --- Rendering mode tracking (b7e2d22) ---
	// Populated by on_session_ready() (called by the hooks.cpp dispatcher after
	// xrCreateSession succeeds). set_rendering_mode_fns() must be called first
	// to wire up the extension function pointers.
	PFN_xrEnumerateDisplayRenderingModesEXT real_enumerate_rendering_modes = nullptr;
	PFN_xrRequestDisplayRenderingModeEXT    real_request_rendering_mode    = nullptr;
	XrDisplayRenderingModeInfoEXT rendering_modes[DISPLAYXR_MAX_RENDERING_MODES] = {};
	uint32_t rendering_mode_count         = 0;
	uint32_t current_rendering_mode_index = 0;

	// --- Tile-atlas output swapchain (b7e2d22) ---
	// Sized to (tile_cols × view_w) × (tile_rows × view_h) for the current
	// rendering mode; recreated when the mode (or its dims) changes.
	XrSwapchain      atlas_sc         = XR_NULL_HANDLE;
	ID3D11Texture2D *atlas_textures[8] = {};
	uint32_t         atlas_img_count  = 0;
	uint32_t         atlas_width      = 0;
	uint32_t         atlas_height     = 0;
	uint32_t         atlas_mode_index = 0xFFFFFFFFu;

private:
	const XrDisplayRenderingModeInfoEXT *get_current_rendering_mode()
	{
		for (uint32_t i = 0; i < rendering_mode_count; i++) {
			if (rendering_modes[i].modeIndex == current_rendering_mode_index)
				return &rendering_modes[i];
		}
		return (rendering_mode_count > 0) ? &rendering_modes[0] : nullptr;
	}

	void destroy_atlas_swapchain()
	{
		if (atlas_sc != XR_NULL_HANDLE && s_real_destroy_swapchain) {
			s_real_destroy_swapchain(atlas_sc);
		}
		atlas_sc = XR_NULL_HANDLE;
		for (uint32_t i = 0; i < 8; i++) atlas_textures[i] = nullptr;
		atlas_img_count  = 0;
		atlas_width      = 0;
		atlas_height     = 0;
		atlas_mode_index = 0xFFFFFFFFu;
	}

	void sub_cleanup_all()
	{
		for (int i = 0; i < sc_sub_count; i++) {
			if (sc_subs[i].active && sc_subs[i].typed_sc != XR_NULL_HANDLE) {
				if (s_real_destroy_swapchain)
					s_real_destroy_swapchain(sc_subs[i].typed_sc);
				sc_subs[i].typed_sc = XR_NULL_HANDLE;
			}
			sc_subs[i].active = false;
		}
		sc_sub_count = 0;
		destroy_atlas_swapchain();
		rendering_mode_count         = 0;
		current_rendering_mode_index = 0;
	}

	D3D11ScSub *sub_find(XrSwapchain unity_sc)
	{
		for (int i = 0; i < sc_sub_count; i++) {
			if (sc_subs[i].active && sc_subs[i].unity_sc == unity_sc)
				return &sc_subs[i];
		}
		return nullptr;
	}

public:
	// --- Rendering mode wiring (called by dispatcher in hooks.cpp) ---
	// These are not virtual methods on GraphicsBackend because the rendering-mode
	// extension is currently D3D11-only. Once GL/Vulkan backends need equivalent
	// tile-layout info, promote to a virtual method. Until then, the dispatcher
	// casts GraphicsBackend* → D3D11Backend* when the active backend is D3D11.

	void set_rendering_mode_fns(PFN_xrEnumerateDisplayRenderingModesEXT enum_fn,
	                            PFN_xrRequestDisplayRenderingModeEXT    req_fn)
	{
		real_enumerate_rendering_modes = enum_fn;
		real_request_rendering_mode    = req_fn;
	}

	// Enumerate rendering modes after the session handle is valid. The
	// extension function pointers must have been wired via set_rendering_mode_fns
	// before this is called.
	void on_session_ready(XrSession session)
	{
		rendering_mode_count = 0;
		if (real_enumerate_rendering_modes == nullptr) return;

		uint32_t total = 0;
		XrResult er = real_enumerate_rendering_modes(session, 0, &total, nullptr);
		if (XR_FAILED(er) || total == 0) return;
		if (total > DISPLAYXR_MAX_RENDERING_MODES) total = DISPLAYXR_MAX_RENDERING_MODES;

		for (uint32_t i = 0; i < total; i++) {
			rendering_modes[i] = {};
			rendering_modes[i].type = XR_TYPE_DISPLAY_RENDERING_MODE_INFO_EXT;
		}
		er = real_enumerate_rendering_modes(session, total, &total, rendering_modes);
		if (XR_FAILED(er)) {
			displayxr_log(
			    "[DisplayXR] xrEnumerateDisplayRenderingModesEXT (populate) failed: %d\n", er);
			return;
		}
		rendering_mode_count = total;

		// Default to first hardware_display_3d mode so we end up targeting a
		// proper stereo/multi-view layout instead of the 2D fallback if one is
		// enumerated first.
		bool picked = false;
		for (uint32_t i = 0; i < total; i++) {
			const XrDisplayRenderingModeInfoEXT *m = &rendering_modes[i];
			displayxr_log(
			    "[DisplayXR] Mode[%u]: '%s' views=%u tiles=%ux%u "
			    "viewPx=%ux%u hw3d=%d\n",
			    m->modeIndex, m->modeName, m->viewCount,
			    m->tileColumns, m->tileRows,
			    m->viewWidthPixels, m->viewHeightPixels,
			    (int)m->hardwareDisplay3D);
			if (!picked && m->hardwareDisplay3D) {
				current_rendering_mode_index = m->modeIndex;
				picked = true;
			}
		}
		if (!picked && total > 0)
			current_rendering_mode_index = rendering_modes[0].modeIndex;

		displayxr_log(
		    "[DisplayXR] Rendering mode: %u modes enumerated, current=%u\n",
		    total, current_rendering_mode_index);
	}

	// Called by the dispatcher for hooked_xrRequestDisplayRenderingModeEXT.
	XrResult request_rendering_mode(XrSession session, uint32_t modeIndex)
	{
		if (real_request_rendering_mode == nullptr)
			return XR_ERROR_FUNCTION_UNSUPPORTED;
		XrResult r = real_request_rendering_mode(session, modeIndex);
		if (XR_SUCCEEDED(r)) {
			if (modeIndex != current_rendering_mode_index) {
				displayxr_log(
				    "[DisplayXR] Rendering mode changed: %u -> %u "
				    "(forcing atlas swapchain recreation)\n",
				    current_rendering_mode_index, modeIndex);
				current_rendering_mode_index = modeIndex;
				// Next prepare_end_frame will notice size mismatch and recreate.
			}
		}
		return r;
	}

	// ========================================================================
	// GraphicsBackend interface
	// ========================================================================

	void on_session_created(const XrSessionCreateInfo *createInfo) override
	{
		// Extract D3D11 device from the session binding chain for GPU sync
		// in xrReleaseSwapchainImage and the atlas composite.
		const XrBaseInStructure *item = (const XrBaseInStructure *)createInfo->next;
		while (item != nullptr) {
			if (item->type == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR) {
				typedef struct { XrStructureType type; const void *next; ID3D11Device *device; } XrGfxBindingD3D11;
				const XrGfxBindingD3D11 *binding = (const XrGfxBindingD3D11 *)item;
				device = binding->device;
				if (device) {
					device->GetImmediateContext(&context);
					displayxr_log(
					    "[DisplayXR] Captured D3D11 device=%p context=%p for GPU sync\n",
					    (void *)device, (void *)context);
				}
				break;
			}
			item = item->next;
		}
	}

	void on_session_destroyed() override
	{
		sub_cleanup_all();
	}

	void on_destroy() override
	{
		sub_cleanup_all();
		if (context) { context->Release(); context = nullptr; }
		device = nullptr; // Not owned by us — don't Release.
	}

	void inject_session_binding(XrBaseOutStructure *last, DisplayXRState *state) override
	{
		win32_inject_window_binding(last, state);
	}

	void on_swapchain_created(XrSession session, const XrSwapchainCreateInfo *createInfo, XrSwapchain unity_sc) override
	{
		// Pair a R8G8B8A8_UNORM_SRGB typed sibling to the Unity-facing TYPELESS
		// color swapchain so Unity's RTV creation works and the compositor gets
		// typed textures. Non-color swapchains (depth, etc.) are left alone.
		if (device != nullptr &&
		    (createInfo->usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) &&
		    sc_sub_count < kMaxScSubs) {
			XrSwapchainCreateInfo typed_info = *createInfo;
			typed_info.format = 29; // DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
			XrSwapchain typed_sc = XR_NULL_HANDLE;
			XrResult tr = s_real_create_swapchain(session, &typed_info, &typed_sc);
			if (XR_SUCCEEDED(tr)) {
				D3D11ScSub &sub = sc_subs[sc_sub_count++];
				sub.unity_sc = unity_sc;
				sub.typed_sc = typed_sc;
				sub.width    = createInfo->width;
				sub.height   = createInfo->height;
				sub.active   = true;
				displayxr_log(
				    "[DisplayXR] Typed swapchain paired: unity=%p typed=%p\n",
				    (void *)(uintptr_t)unity_sc, (void *)(uintptr_t)typed_sc);
			} else {
				displayxr_log("[DisplayXR] Typed swapchain create FAILED: result=%d\n", tr);
			}
		}
	}

	bool handle_enumerate_swapchain_images(XrSwapchain swapchain, uint32_t imageCapacityInput, uint32_t *imageCountOutput, XrSwapchainImageBaseHeader *images, XrResult *result_out) override
	{
		// Route to the typed sibling so Unity receives the images it can create
		// valid RTVs from. Cache the texture handles per substitution record so
		// the atlas composite can find them by current_idx later.
		D3D11ScSub *sub = sub_find(swapchain);
		if (sub != nullptr) {
			XrResult result = s_real_enumerate_swapchain_images(
			    sub->typed_sc, imageCapacityInput, imageCountOutput, images);
			if (XR_SUCCEEDED(result) && images != nullptr && imageCountOutput != nullptr) {
				displayxr_log(
				    "[DisplayXR] xrEnumerateSwapchainImages: unity_sc=%p → typed_sc=%p count=%u\n",
				    (void *)(uintptr_t)swapchain,
				    (void *)(uintptr_t)sub->typed_sc,
				    *imageCountOutput);
				if (images->type == XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR) {
					XrSwapchainImageD3D11KHR *d3d = (XrSwapchainImageD3D11KHR *)images;
					uint32_t n = *imageCountOutput < 8 ? *imageCountOutput : 8;
					sub->typed_img_count = n;
					for (uint32_t i = 0; i < n; i++) {
						sub->typed_textures[i] = d3d[i].texture;
						if (d3d[i].texture) {
							D3D11_TEXTURE2D_DESC desc = {};
							d3d[i].texture->GetDesc(&desc);
							displayxr_log("  typed[%u] tex=%p fmt=%u\n", i, (void *)d3d[i].texture, desc.Format);
						}
					}
				}
			}
			*result_out = result;
			return true;
		}
		return false;
	}

	bool handle_acquire_swapchain_image(XrSwapchain swapchain, const XrSwapchainImageAcquireInfo *acquireInfo, uint32_t *index, XrResult *result_out) override
	{
		// Acquire from the typed sibling (Unity renders into it) and also the
		// unity_sc (keep runtime state sane / avoid leaked handles).
		D3D11ScSub *sub = sub_find(swapchain);
		if (sub != nullptr) {
			XrResult result = s_real_acquire_swapchain_image(sub->typed_sc, acquireInfo, index);
			if (XR_SUCCEEDED(result) && index)
				sub->current_idx = *index;
			uint32_t dummy = 0;
			s_real_acquire_swapchain_image(swapchain, acquireInfo, &dummy);
			static int s_acq_count = 0;
			if (s_acq_count < 6 || s_acq_count % 120 == 0)
				displayxr_log(
				    "[DisplayXR] xrAcquireSwapchainImage: unity=%p typed=%p typed_idx=%u\n",
				    (void *)(uintptr_t)swapchain, (void *)(uintptr_t)sub->typed_sc,
				    index ? *index : 0xFFFFFFFF);
			s_acq_count++;
			*result_out = result;
			return true;
		}
		return false;
	}

	bool handle_wait_swapchain_image(XrSwapchain swapchain, const XrSwapchainImageWaitInfo *waitInfo, XrResult *result_out) override
	{
		D3D11ScSub *sub = sub_find(swapchain);
		if (sub != nullptr) {
			s_real_wait_swapchain_image(sub->typed_sc, waitInfo);
			*result_out = s_real_wait_swapchain_image(swapchain, waitInfo);
			return true;
		}
		return false;
	}

	bool handle_release_swapchain_image(XrSwapchain swapchain, const XrSwapchainImageReleaseInfo *releaseInfo, XrResult *result_out) override
	{
		D3D11ScSub *sub = sub_find(swapchain);
		if (sub != nullptr) {
			// Flush so Unity's render commands reach the GPU before we composite.
			if (context != nullptr)
				context->Flush();
			// Defer typed_sc release: we still need to write into it (atlas
			// composite) inside prepare_end_frame before handing it to the
			// compositor.
			sub->release_pending = true;
			// Release unity_sc now (we never rendered into it, just keeping state sane).
			*result_out = s_real_release_swapchain_image(swapchain, releaseInfo);
			return true;
		}
		// Non-substituted swapchain: flush only.
		if (context != nullptr)
			context->Flush();
		return false;
	}

	void prepare_end_frame(XrSession session, const XrFrameEndInfo *frameEndInfo, void *patches_out, int *npatch_out) override
	{
		// N-view tile atlas composite (main commit b7e2d22).
		//
		// Caller contract: patches_out points at an EFPatch buffer with capacity
		// ≥ kMaxEfPatches. The hooks.cpp dispatcher must allocate at least 16
		// entries to cover up to a 4×4 tile layout (light-field modes).
		const int kMaxEfPatches = 16;

		EFPatch *ef_patches = (EFPatch *)patches_out;
		*npatch_out = 0;

		if (sc_sub_count > 0 && frameEndInfo != nullptr && frameEndInfo->layers != nullptr) {
			const XrDisplayRenderingModeInfoEXT *mode = get_current_rendering_mode();

			// Derive tile layout. Prefer the enumerated mode; fall back to 2×1
			// SBS with per-view dimensions inferred from the first shadow
			// swapchain so stereo apps keep working if the rendering-mode
			// extension isn't exposed.
			uint32_t tile_cols = 0, tile_rows = 0, view_w = 0, view_h = 0;
			if (mode != nullptr && mode->tileColumns > 0 && mode->tileRows > 0 &&
			    mode->viewWidthPixels > 0 && mode->viewHeightPixels > 0) {
				tile_cols = mode->tileColumns;
				tile_rows = mode->tileRows;
				view_w    = mode->viewWidthPixels;
				view_h    = mode->viewHeightPixels;
			} else {
				for (int si = 0; si < sc_sub_count; si++) {
					if (sc_subs[si].active) {
						view_w = sc_subs[si].width;
						view_h = sc_subs[si].height;
						break;
					}
				}
				tile_cols = 2;
				tile_rows = 1;
			}
			uint32_t atlas_w = tile_cols * view_w;
			uint32_t atlas_h = tile_rows * view_h;

			// Recreate the atlas swapchain if the target mode (or its dims) changed.
			if (atlas_sc != XR_NULL_HANDLE &&
			    (atlas_width != atlas_w || atlas_height != atlas_h ||
			     atlas_mode_index != current_rendering_mode_index)) {
				destroy_atlas_swapchain();
			}

			// Lazily create the atlas output swapchain.
			if (atlas_sc == XR_NULL_HANDLE && s_real_create_swapchain &&
			    atlas_w > 0 && atlas_h > 0) {
				XrSwapchainCreateInfo atlas_info = {};
				atlas_info.type            = XR_TYPE_SWAPCHAIN_CREATE_INFO;
				atlas_info.format          = 29; // DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
				atlas_info.width           = atlas_w;
				atlas_info.height          = atlas_h;
				atlas_info.sampleCount     = 1;
				atlas_info.faceCount       = 1;
				atlas_info.arraySize       = 1;
				atlas_info.mipCount        = 1;
				atlas_info.usageFlags      = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT
				                           | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
				XrResult sr = s_real_create_swapchain(session, &atlas_info, &atlas_sc);
				if (XR_SUCCEEDED(sr) && s_real_enumerate_swapchain_images) {
					uint32_t cnt = 0;
					s_real_enumerate_swapchain_images(atlas_sc, 0, &cnt, nullptr);
					if (cnt > 0 && cnt <= 8) {
						XrSwapchainImageD3D11KHR imgs[8] = {};
						for (uint32_t k = 0; k < cnt; k++)
							imgs[k].type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR;
						s_real_enumerate_swapchain_images(
						    atlas_sc, cnt, &cnt,
						    (XrSwapchainImageBaseHeader *)imgs);
						atlas_img_count = cnt;
						for (uint32_t k = 0; k < cnt; k++)
							atlas_textures[k] = imgs[k].texture;
					}
					atlas_width      = atlas_w;
					atlas_height     = atlas_h;
					atlas_mode_index = current_rendering_mode_index;
					displayxr_log(
					    "[DisplayXR] Atlas swapchain created: sc=%p %ux%u imgs=%u "
					    "tiles=%ux%u viewPx=%ux%u mode=%u\n",
					    (void *)(uintptr_t)atlas_sc, atlas_w, atlas_h,
					    atlas_img_count, tile_cols, tile_rows, view_w, view_h,
					    current_rendering_mode_index);
				} else {
					displayxr_log(
					    "[DisplayXR] Atlas swapchain creation FAILED: %d\n", (int)sr);
				}
			}

			if (atlas_sc != XR_NULL_HANDLE) {
				// Acquire + wait one atlas image for this frame.
				uint32_t atlas_idx = 0;
				XrSwapchainImageAcquireInfo acq = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
				XrSwapchainImageWaitInfo    wai = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
				wai.timeout = XR_INFINITE_DURATION;
				s_real_acquire_swapchain_image(atlas_sc, &acq, &atlas_idx);
				s_real_wait_swapchain_image(atlas_sc, &wai);
				ID3D11Texture2D *atlas_tex = (atlas_idx < atlas_img_count)
				    ? atlas_textures[atlas_idx] : nullptr;

				uint32_t tile_cap = tile_cols * tile_rows;

				// Walk projection layers, tile each view's shadow texture into
				// the atlas, and patch the view to reference the atlas tile.
				for (uint32_t i = 0; i < frameEndInfo->layerCount; i++) {
					const XrCompositionLayerBaseHeader *hdr = frameEndInfo->layers[i];
					if (!hdr || hdr->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION) continue;
					const XrCompositionLayerProjection *proj =
					    (const XrCompositionLayerProjection*)hdr;
					if (!proj->views || proj->viewCount == 0) continue;
					XrCompositionLayerProjectionView *views =
					    (XrCompositionLayerProjectionView*)proj->views;

					uint32_t n = proj->viewCount;
					if (n > tile_cap) n = tile_cap;

					for (uint32_t v = 0; v < n && *npatch_out < kMaxEfPatches; v++) {
						D3D11ScSub *sub = sub_find(views[v].subImage.swapchain);
						if (!sub) continue;

						uint32_t col = v % tile_cols;
						uint32_t row = v / tile_cols;
						uint32_t dst_x = col * view_w;
						uint32_t dst_y = row * view_h;

						// Copy the shadow texture into its tile. Use
						// min(source, tile) dims so oversized shadows don't
						// overrun neighbors.
						if (atlas_tex && context) {
							ID3D11Texture2D *src_tex =
							    (sub->current_idx < sub->typed_img_count)
							        ? sub->typed_textures[sub->current_idx]
							        : nullptr;
							if (src_tex) {
								uint32_t copy_w = (sub->width  < view_w) ? sub->width  : view_w;
								uint32_t copy_h = (sub->height < view_h) ? sub->height : view_h;
								D3D11_BOX box = { 0, 0, 0, copy_w, copy_h, 1 };
								context->CopySubresourceRegion(
								    atlas_tex, 0, dst_x, dst_y, 0,
								    src_tex, 0, &box);
							}
						}

						EFPatch &p = ef_patches[(*npatch_out)++];
						p.view      = &views[v];
						p.orig_sc   = views[v].subImage.swapchain;
						p.orig_rect = views[v].subImage.imageRect;
						views[v].subImage.swapchain               = atlas_sc;
						views[v].subImage.imageRect.offset.x      = (int32_t)dst_x;
						views[v].subImage.imageRect.offset.y      = (int32_t)dst_y;
						views[v].subImage.imageRect.extent.width  = (int32_t)view_w;
						views[v].subImage.imageRect.extent.height = (int32_t)view_h;
					}
				}

				if (context) context->Flush();

				// Release the deferred typed shadow images, then the atlas image.
				XrSwapchainImageReleaseInfo rel = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
				for (int si = 0; si < sc_sub_count; si++) {
					if (sc_subs[si].active && sc_subs[si].release_pending) {
						s_real_release_swapchain_image(sc_subs[si].typed_sc, &rel);
						sc_subs[si].release_pending = false;
					}
				}
				s_real_release_swapchain_image(atlas_sc, &rel);

				static int s_atlas_log = 0;
				if (*npatch_out > 0 && s_atlas_log++ < 4) {
					displayxr_log(
					    "[DisplayXR] xrEndFrame: atlas composite OK "
					    "(n=%d tiles=%ux%u atlas=%ux%u viewPx=%ux%u)\n",
					    *npatch_out, tile_cols, tile_rows, atlas_w, atlas_h,
					    view_w, view_h);
				}
			} else {
				// Atlas creation failed — release any deferred typed shadows
				// so we don't leak acquired images, and fall through to
				// end_frame with the original (broken) view swapchains. The
				// frame will render wrong but we keep the session alive.
				XrSwapchainImageReleaseInfo rel = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
				for (int si = 0; si < sc_sub_count; si++) {
					if (sc_subs[si].active && sc_subs[si].release_pending) {
						s_real_release_swapchain_image(sc_subs[si].typed_sc, &rel);
						sc_subs[si].release_pending = false;
					}
				}
			}
		}
	}

	void restore_end_frame(void *patches, int npatch) override
	{
		EFPatch *ef_patches = (EFPatch *)patches;
		for (int i = 0; i < npatch; i++) {
			ef_patches[i].view->subImage.swapchain = ef_patches[i].orig_sc;
			ef_patches[i].view->subImage.imageRect = ef_patches[i].orig_rect;
		}
	}

	void *create_shared_texture(uint32_t /*width*/, uint32_t /*height*/) override
	{
		return nullptr;
	}

	void destroy_shared_texture() override {}

	void *get_shared_texture_native_ptr() override
	{
		return nullptr;
	}
};

GraphicsBackend *create_d3d11_backend() { return new D3D11Backend(); }

#endif // defined(_WIN32)
