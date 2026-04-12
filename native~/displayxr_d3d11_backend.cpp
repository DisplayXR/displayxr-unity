// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
//
// D3D11 graphics backend for the DisplayXR hook chain.
// Extracted from displayxr_hooks.cpp — no logic changes.
//
// === SCAFFOLD STATE (Phase 1 checkpoint) ===
// Verbatim copy of a089149:native~/displayxr_d3d11_backend.cpp. NOT yet
// listed in CMake SOURCES — does not affect the current build. The big-cut
// session must:
//   1. Merge the N-view tile atlas composite from commit b7e2d22 into
//      prepare_end_frame(). The template below only handles the 2-view SBS
//      case; main's hooks.cpp has the generalized N-view atlas path that
//      this backend must absorb (Option A — atlas inlined here).
//   2. Verify the typed-swapchain substitution (2a9a933) matches main
//      verbatim. 2a9a933 was cherry-picked FROM this branch so it should
//      already match — double-check.
//   3. Port the rendering-mode tracking state from main's hooks.cpp as
//      D3D11Backend members.
//   4. Add to CMake SOURCES and remove now-duplicated code from
//      displayxr_hooks.cpp.

#if defined(_WIN32)

#include "displayxr_hooks_internal.h"

class D3D11Backend : public GraphicsBackend {
public:
	// EFPatch is defined in displayxr_hooks_internal.h (shared with hooks.cpp)

	ID3D11Device        *device = nullptr;
	ID3D11DeviceContext *context = nullptr;

	static const int kMaxScSubs = 8;
	D3D11ScSub sc_subs[kMaxScSubs] = {};
	int sc_sub_count = 0;

	XrSwapchain sbs_sc = XR_NULL_HANDLE;
	ID3D11Texture2D *sbs_textures[8] = {};
	uint32_t sbs_img_count = 0;

private:
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
		if (sbs_sc != XR_NULL_HANDLE && s_real_destroy_swapchain) {
			s_real_destroy_swapchain(sbs_sc);
			sbs_sc = XR_NULL_HANDLE;
		}
		sbs_img_count = 0;
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
	void on_session_created(const XrSessionCreateInfo *createInfo) override
	{
		const XrBaseInStructure *item = (const XrBaseInStructure *)createInfo->next;
		while (item != nullptr) {
			if (item->type == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR) {
				// Extract D3D11 device for GPU sync in xrReleaseSwapchainImage
				typedef struct { XrStructureType type; const void *next; ID3D11Device *device; } XrGfxBindingD3D11;
				const XrGfxBindingD3D11 *binding = (const XrGfxBindingD3D11 *)item;
				device = binding->device;
				if (device) {
					device->GetImmediateContext(&context);
					displayxr_log("[DisplayXR] Captured D3D11 device=%p context=%p for GPU sync\n",
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
		device = nullptr; // Not owned by us — don't Release
	}

	void inject_session_binding(XrBaseOutStructure *last, DisplayXRState *state) override
	{
		win32_inject_window_binding(last, state);
	}

	void on_swapchain_created(XrSession session, const XrSwapchainCreateInfo *createInfo, XrSwapchain unity_sc) override
	{
		// D3D11: create a parallel R8G8B8A8_UNORM_SRGB swapchain so the compositor
		// receives typed textures in xrEndFrame (TYPELESS → compositor X-pattern).
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
				displayxr_log("[DisplayXR] Typed swapchain paired: unity=%p typed=%p\n",
				              (void *)(uintptr_t)unity_sc, (void *)(uintptr_t)typed_sc);
			} else {
				displayxr_log("[DisplayXR] Typed swapchain create FAILED: result=%d\n", tr);
			}
		}
	}

	bool handle_enumerate_swapchain_images(XrSwapchain swapchain, uint32_t imageCapacityInput, uint32_t *imageCountOutput, XrSwapchainImageBaseHeader *images, XrResult *result_out) override
	{
		// D3D11 typed swapchain substitution: route to typed_sc so Unity gets
		// R8G8B8A8_UNORM_SRGB textures it can create valid RTVs from.
		D3D11ScSub *sub = sub_find(swapchain);
		if (sub != nullptr) {
			XrResult result = s_real_enumerate_swapchain_images(
			    sub->typed_sc, imageCapacityInput, imageCountOutput, images);
			if (XR_SUCCEEDED(result) && images != nullptr && imageCountOutput != nullptr) {
				displayxr_log("[DisplayXR] xrEnumerateSwapchainImages: unity_sc=%p → typed_sc=%p count=%u\n",
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
		// D3D11: acquire from typed_sc (Unity renders into it) and unity_sc (keep state sane).
		D3D11ScSub *sub = sub_find(swapchain);
		if (sub != nullptr) {
			XrResult result = s_real_acquire_swapchain_image(sub->typed_sc, acquireInfo, index);
			if (XR_SUCCEEDED(result) && index)
				sub->current_idx = *index;
			uint32_t dummy = 0;
			s_real_acquire_swapchain_image(swapchain, acquireInfo, &dummy);
			static int s_acq_count = 0;
			if (s_acq_count < 6 || s_acq_count % 120 == 0)
				displayxr_log("[DisplayXR] xrAcquireSwapchainImage: unity=%p typed=%p typed_idx=%u\n",
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
			// Defer typed_sc release: we still need to write into it (SBS composite)
			// inside hooked_xrEndFrame before handing it to the compositor.
			sub->release_pending = true;
			// Release unity_sc now (we never rendered into it, just keeping state sane).
			*result_out = s_real_release_swapchain_image(swapchain, releaseInfo);
			return true;
		}
		// Non-substituted swapchain: original flush-and-release behavior.
		if (context != nullptr)
			context->Flush();
		return false;
	}

	void prepare_end_frame(XrSession session, const XrFrameEndInfo *frameEndInfo, void *patches_out, int *npatch_out) override
	{
		EFPatch *ef_patches = (EFPatch *)patches_out;
		*npatch_out = 0;

		if (sc_sub_count > 0 && frameEndInfo != nullptr && frameEndInfo->layers != nullptr) {
			for (uint32_t i = 0; i < frameEndInfo->layerCount && *npatch_out < 8; i++) {
				const XrCompositionLayerBaseHeader *hdr = frameEndInfo->layers[i];
				if (!hdr || hdr->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION) continue;
				const XrCompositionLayerProjection *proj = (const XrCompositionLayerProjection*)hdr;
				if (!proj->views || proj->viewCount != 2) continue;
				XrCompositionLayerProjectionView *views = (XrCompositionLayerProjectionView*)proj->views;

				D3D11ScSub *sub1 = sub_find(views[0].subImage.swapchain); // left eye
				D3D11ScSub *sub2 = sub_find(views[1].subImage.swapchain); // right eye
				if (!sub1 || !sub2 || sub1 == sub2) continue;

				uint32_t eye_w = sub1->width;
				uint32_t eye_h = sub1->height;
				uint32_t sbs_w = eye_w * 2; // 3840 when eye_w=1920

				// Lazily create the SBS output swapchain on first xrEndFrame.
				if (sbs_sc == XR_NULL_HANDLE && s_real_create_swapchain) {
					XrSwapchainCreateInfo sbs_info = {};
					sbs_info.type            = XR_TYPE_SWAPCHAIN_CREATE_INFO;
					sbs_info.format          = 29; // DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
					sbs_info.width           = sbs_w;
					sbs_info.height          = eye_h;
					sbs_info.sampleCount     = 1;
					sbs_info.faceCount       = 1;
					sbs_info.arraySize       = 1;
					sbs_info.mipCount        = 1;
					sbs_info.usageFlags      = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT
					                         | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
					XrResult sr = s_real_create_swapchain(session, &sbs_info, &sbs_sc);
					if (XR_SUCCEEDED(sr) && s_real_enumerate_swapchain_images) {
						uint32_t cnt = 0;
						s_real_enumerate_swapchain_images(sbs_sc, 0, &cnt, nullptr);
						if (cnt > 0 && cnt <= 8) {
							XrSwapchainImageD3D11KHR imgs[8] = {};
							for (uint32_t k = 0; k < cnt; k++)
								imgs[k].type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR;
							s_real_enumerate_swapchain_images(
							    sbs_sc, cnt, &cnt,
							    (XrSwapchainImageBaseHeader *)imgs);
							sbs_img_count = cnt;
							for (uint32_t k = 0; k < cnt; k++)
								sbs_textures[k] = imgs[k].texture;
						}
						displayxr_log("[DisplayXR] SBS swapchain created: sc=%p %ux%u imgs=%u\n",
						              (void *)(uintptr_t)sbs_sc, sbs_w, eye_h, sbs_img_count);
					} else {
						displayxr_log("[DisplayXR] SBS swapchain creation FAILED: %d\n", (int)sr);
					}
				}
				if (sbs_sc == XR_NULL_HANDLE) continue;

				// Acquire + wait SBS image.
				uint32_t sbs_idx = 0;
				XrSwapchainImageAcquireInfo acq = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
				XrSwapchainImageWaitInfo    wai = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
				wai.timeout = XR_INFINITE_DURATION;
				s_real_acquire_swapchain_image(sbs_sc, &acq, &sbs_idx);
				s_real_wait_swapchain_image(sbs_sc, &wai);

				// Composite: copy full left eye → SBS left half, right eye → SBS right half.
				ID3D11Texture2D *sbs_tex  = (sbs_idx < sbs_img_count) ? sbs_textures[sbs_idx] : nullptr;
				ID3D11Texture2D *left_tex = (sub1->current_idx < sub1->typed_img_count)
				                          ? sub1->typed_textures[sub1->current_idx] : nullptr;
				ID3D11Texture2D *right_tex = (sub2->current_idx < sub2->typed_img_count)
				                          ? sub2->typed_textures[sub2->current_idx] : nullptr;
				if (sbs_tex && left_tex && right_tex && context) {
					D3D11_BOX eye_box = { 0, 0, 0, eye_w, eye_h, 1 };
					context->CopySubresourceRegion(sbs_tex, 0, 0,     0, 0, left_tex,  0, &eye_box);
					context->CopySubresourceRegion(sbs_tex, 0, eye_w, 0, 0, right_tex, 0, &eye_box);
					context->Flush();
				}

				// Release deferred typed swapchains.
				XrSwapchainImageReleaseInfo rel = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
				for (int si = 0; si < sc_sub_count; si++) {
					if (sc_subs[si].active && sc_subs[si].release_pending) {
						s_real_release_swapchain_image(sc_subs[si].typed_sc, &rel);
						sc_subs[si].release_pending = false;
					}
				}
				// Release SBS image.
				s_real_release_swapchain_image(sbs_sc, &rel);

				// Patch both views to reference sbs_sc with correct half-width rects.
				for (uint32_t v = 0; v < 2 && *npatch_out < 8; v++) {
					EFPatch &p  = ef_patches[(*npatch_out)++];
					p.view      = &views[v];
					p.orig_sc   = views[v].subImage.swapchain;
					p.orig_rect = views[v].subImage.imageRect;
					views[v].subImage.swapchain              = sbs_sc;
					views[v].subImage.imageRect.offset.x     = (int32_t)(v * eye_w);
					views[v].subImage.imageRect.offset.y     = 0;
					views[v].subImage.imageRect.extent.width  = (int32_t)eye_w;
					views[v].subImage.imageRect.extent.height = (int32_t)eye_h;
				}

				static int s_sub_log = 0;
				if (s_sub_log++ < 4) {
					displayxr_log("[DisplayXR] xrEndFrame: SBS composite OK"
					              " (eye=%ux%u sbs=%ux%u sbs_tex=%p L=%p R=%p)\n",
					              eye_w, eye_h, sbs_w, eye_h,
					              (void *)sbs_tex, (void *)left_tex, (void *)right_tex);
				}
			}

			// Release any deferred typed swapchains not handled above.
			XrSwapchainImageReleaseInfo rel = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
			for (int si = 0; si < sc_sub_count; si++) {
				if (sc_subs[si].active && sc_subs[si].release_pending) {
					s_real_release_swapchain_image(sc_subs[si].typed_sc, &rel);
					sc_subs[si].release_pending = false;
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
		// shared texture is handled on Metal side
		return nullptr;
	}

	void destroy_shared_texture() override
	{
		// no-op on D3D11
	}

	void *get_shared_texture_native_ptr() override
	{
		return nullptr;
	}
};

GraphicsBackend *create_d3d11_backend() { return new D3D11Backend(); }

#endif // defined(_WIN32)
