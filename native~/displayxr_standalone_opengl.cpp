// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
//
// OpenGL standalone graphics backend stub (desktop GL for Windows/macOS/Linux).
// Active when ENABLE_OPENGL is defined on non-Android platforms.
// Full implementation pending; currently provides no-op session binding and swapchain handling.

#if defined(ENABLE_OPENGL) && !defined(__ANDROID__)
#include "displayxr_standalone_internal.h"

class StandaloneOpenGLBackend : public StandaloneGraphicsBackend {
public:
    bool create_device(XrInstance, XrSystemId, PFN_xrGetInstanceProcAddr) override { return false; }
    bool create_shared_texture(uint32_t, uint32_t) override { return false; }
    void destroy_shared_texture() override {}
    void *get_shared_texture_native_ptr() override { return nullptr; }
    const void *build_session_binding(void *, void *) override { return nullptr; }
    bool enumerate_atlas_images(XrSwapchain, PFN_xrEnumerateSwapchainImages, uint32_t) override { return false; }
    void *get_atlas_image(uint32_t) override { return nullptr; }
    void create_atlas_bridge(uint32_t, uint32_t, void *) override {}
    void destroy_atlas_bridge() override {}
    void *get_atlas_bridge_unity_ptr() override { return nullptr; }
    void blit_atlas(void *, uint32_t) override {}
    bool fw_create_swapchain(void *, uint32_t, uint32_t) override { return false; }
    void fw_destroy_swapchain() override {}
    void fw_resize_swapchain_buffers(uint32_t, uint32_t) override {}
    void fw_present(uint32_t, uint32_t) override {}
    void destroy() override {}
};

StandaloneGraphicsBackend *create_standalone_opengl_backend() { return new StandaloneOpenGLBackend(); }
#endif
