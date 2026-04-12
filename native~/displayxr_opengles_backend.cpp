// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
//
// OpenGL ES graphics backend stub (Android).
// Active when ENABLE_OPENGL is defined on Android.

#if defined(ENABLE_OPENGL) && defined(__ANDROID__)
#include "displayxr_hooks_internal.h"

class OpenGLESBackend : public GraphicsBackend {
public:
    void on_session_created(const XrSessionCreateInfo *) override {}
    void on_session_destroyed() override {}
    void on_destroy() override {}
    void inject_session_binding(XrBaseOutStructure *, DisplayXRState *) override {}
    void on_swapchain_created(XrSession, const XrSwapchainCreateInfo *, XrSwapchain) override {}
    bool handle_enumerate_swapchain_images(XrSwapchain, uint32_t, uint32_t *imageCountOutput, XrSwapchainImageBaseHeader *, XrResult *result_out) override
        { if (result_out) *result_out = XR_ERROR_FUNCTION_UNSUPPORTED; return false; }
    bool handle_acquire_swapchain_image(XrSwapchain, const XrSwapchainImageAcquireInfo *, uint32_t *, XrResult *result_out) override
        { if (result_out) *result_out = XR_ERROR_FUNCTION_UNSUPPORTED; return false; }
    bool handle_wait_swapchain_image(XrSwapchain, const XrSwapchainImageWaitInfo *, XrResult *result_out) override
        { if (result_out) *result_out = XR_ERROR_FUNCTION_UNSUPPORTED; return false; }
    bool handle_release_swapchain_image(XrSwapchain, const XrSwapchainImageReleaseInfo *, XrResult *result_out) override
        { if (result_out) *result_out = XR_ERROR_FUNCTION_UNSUPPORTED; return false; }
    void prepare_end_frame(XrSession, const XrFrameEndInfo *, void *, int *npatch_out) override
        { if (npatch_out) *npatch_out = 0; }
    void restore_end_frame(void *, int) override {}
    void *create_shared_texture(uint32_t, uint32_t) override { return nullptr; }
    void  destroy_shared_texture() override {}
    void *get_shared_texture_native_ptr() override { return nullptr; }
};

GraphicsBackend *create_opengles_backend() { return new OpenGLESBackend(); }
#endif
