// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Unity native-plugin-interface glue (issue #124). Implements UnityPluginLoad
// to capture Unity's editor graphics device. Currently used to expose Unity's
// Vulkan instance/device/queue to the standalone Vulkan backend so it can
// import the cross-device atlas-bridge image on Unity's own device.
//
// Handles are exposed as opaque void* so consumers don't need the Vulkan or
// Unity PluginAPI headers.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Unity graphics renderer id, mirrors UnityGfxRenderer (kUnityGfxRendererVulkan
// == 21, D3D12 == 18, D3D11 == 2). Returns -1 if not yet known.
int displayxr_unity_get_renderer(void);

// If Unity's editor is running Vulkan and the device has been captured, fills
// the out params (each a VkInstance/VkPhysicalDevice/VkDevice/VkQueue cast to
// void*) and returns true. Any out pointer may be NULL. Returns false when
// Unity isn't on Vulkan or the device isn't available yet.
bool displayxr_unity_get_vulkan(void **out_instance,
                                void **out_physical_device,
                                void **out_device,
                                void **out_graphics_queue,
                                uint32_t *out_queue_family_index);

#ifdef __cplusplus
}
#endif
