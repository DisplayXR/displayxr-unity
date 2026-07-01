// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Vulkan graphics backend.
// Active when ENABLE_VULKAN is defined (Windows opt-in via CMakeLists) or on
// Android/Linux (default). The plugin doesn't link or call into Vulkan — it
// captures the binding handles supplied by Unity's OpenXR loader and forwards
// them unchanged via the next->xrCreateSession chain. No swapchain typing or
// atlas work is required (Vulkan doesn't suffer from D3D11's TYPELESS
// swapchain substitution problem). Window binding injection on Windows uses
// the same graphics-API-agnostic helper as D3D11/D3D12.

#if defined(ENABLE_VULKAN) || defined(__ANDROID__) || (defined(__linux__) && !defined(__ANDROID__) && !defined(__APPLE__))
#include "displayxr_hooks_internal.h"

// Inline definition of XrGraphicsBindingVulkanKHR — uses opaque void* for the
// Vulkan handles so this TU doesn't need <vulkan/vulkan.h> or
// XR_USE_GRAPHICS_API_VULKAN. Same pattern as WsuiXrGraphicsBindingD3D12KHR
// in displayxr_d3d12_backend.cpp.
typedef struct DxrXrGraphicsBindingVulkanKHR {
    XrStructureType type;
    const void *next;
    void   *instance;          // VkInstance (opaque)
    void   *physicalDevice;    // VkPhysicalDevice (opaque)
    void   *device;            // VkDevice (opaque)
    uint32_t queueFamilyIndex;
    uint32_t queueIndex;
} DxrXrGraphicsBindingVulkanKHR;

class VulkanBackend : public GraphicsBackend {
public:
    // Captured from session binding chain. Opaque handles — Unity owns the
    // Vulkan device's lifetime; we never call into these.
    void   *instance         = nullptr;
    void   *physical_device  = nullptr;
    void   *device           = nullptr;
    uint32_t queue_family    = 0;
    uint32_t queue_index     = 0;

    void on_session_created(const XrSessionCreateInfo *createInfo) override
    {
        const XrBaseInStructure *item = (const XrBaseInStructure *)createInfo->next;
        while (item != nullptr) {
            if (item->type == XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR) {
                const DxrXrGraphicsBindingVulkanKHR *binding =
                    (const DxrXrGraphicsBindingVulkanKHR *)item;
                instance        = binding->instance;
                physical_device = binding->physicalDevice;
                device          = binding->device;
                queue_family    = binding->queueFamilyIndex;
                queue_index     = binding->queueIndex;
                displayxr_log("[DisplayXR] Vulkan: captured instance=%p physicalDevice=%p device=%p queueFamily=%u queueIndex=%u\n",
                    instance, physical_device, device,
                    (unsigned)queue_family, (unsigned)queue_index);
                break;
            }
            item = item->next;
        }
    }

    void on_session_destroyed() override
    {
        instance = physical_device = device = nullptr;
        queue_family = queue_index = 0;
    }

    void on_destroy() override { on_session_destroyed(); }

    void inject_session_binding(XrBaseOutStructure *last, DisplayXRState *state) override
    {
#if defined(_WIN32)
        win32_inject_window_binding(last, state);
#else
        (void)last; (void)state;
#endif
    }

    void on_swapchain_created(XrSession, const XrSwapchainCreateInfo *, XrSwapchain) override {}
    bool handle_enumerate_swapchain_images(XrSwapchain, uint32_t, uint32_t *, XrSwapchainImageBaseHeader *, XrResult *) override { return false; }
    bool handle_acquire_swapchain_image(XrSwapchain, const XrSwapchainImageAcquireInfo *, uint32_t *, XrResult *) override { return false; }
    bool handle_wait_swapchain_image(XrSwapchain, const XrSwapchainImageWaitInfo *, XrResult *) override { return false; }
    bool handle_release_swapchain_image(XrSwapchain, const XrSwapchainImageReleaseInfo *, XrResult *) override { return false; }
    void prepare_end_frame(XrSession, const XrFrameEndInfo *, void *, int *npatch_out) override
        { if (npatch_out) *npatch_out = 0; }
    void restore_end_frame(void *, int) override {}
    void *create_shared_texture(uint32_t, uint32_t) override { return nullptr; }
    void  destroy_shared_texture() override {}
    void *get_shared_texture_native_ptr() override { return nullptr; }
};

GraphicsBackend *create_vulkan_backend() { return new VulkanBackend(); }
#endif
