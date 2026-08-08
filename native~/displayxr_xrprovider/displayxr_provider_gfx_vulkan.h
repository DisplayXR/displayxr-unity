// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Vulkan graphics glue for the IUnityXRDisplay provider (#247).
//
// WHY THIS IS AN OWN-DEVICE BRIDGE AND NOT ZERO-COPY
// --------------------------------------------------
// The tempting shape is the D3D11 one: bind the OpenXR session directly to
// Unity's own VkDevice, so the runtime's swapchain images ARE Unity textures and
// nothing is copied. Two facts kill it:
//
//  1. The runtime needs XR_KHR_vulkan_enable2. Under enable1 the app owns the
//     VkDevice, so the runtime can never request a queue of its own and the #868
//     weave-rate-decoupling repaint silently stays off (the runtime WARNs "#886:"
//     about exactly this at session create). enable2 means the runtime creates
//     the VkInstance/VkDevice via xrCreateVulkanInstanceKHR /
//     xrCreateVulkanDeviceKHR — it does not accept a device we made earlier.
//  2. Unity's VkDevice could in principle be routed through those calls via
//     IUnityGraphicsVulkan::InterceptInitialization, but that hook must run
//     before kUnityGfxDeviceEventInitialize. In editor Play Mode our DLL is
//     loaded from the subsystem manifest LONG after Unity's graphics device
//     exists (Editor.log: "Forcing GfxDevice: Vulkan" at line 123 vs "Loading
//     plugin displayxr_unity" at line 826). Play Mode *is* the shipping preview
//     workflow here, so an editor-only hole is not acceptable.
//
// So the session runs on a runtime-created VkDevice and we bridge to Unity's
// separate VkDevice with VK_KHR_external_memory_win32 (OPAQUE_WIN32) images plus
// an OPAQUE_WIN32 semaphore for cross-device ordering. That is the DXR_GFX_D3D12
// own-device-bridge shape expressed in Vulkan: own device + shared 2-slice array
// + per-frame copy, with the semaphore playing the role of the shared
// ID3D12Fence. Intercept-based zero-copy stays available as a player-only
// optimisation later.
//
// Everything here is Windows-only for now (OPAQUE_WIN32 handles). Vulkan on
// macOS remains out of scope — Metal is the macOS backend (#202/#204).

#pragma once

#include <stdint.h>

#if defined(_WIN32) && defined(ENABLE_VULKAN)

#include "../displayxr_vk_loader.h"

#define XR_USE_GRAPHICS_API_VULKAN 0 // types are inlined in the .cpp, like D3D11/D3D12
#include <openxr/openxr.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Create the session's Vulkan device through XR_KHR_vulkan_enable2.
///
/// Runs the full enable2 sequence: xrGetVulkanGraphicsRequirements2KHR ->
/// xrCreateVulkanInstanceKHR -> xrGetVulkanGraphicsDevice2KHR ->
/// xrCreateVulkanDeviceKHR, then grabs the graphics queue and creates the
/// command pool/buffer/fence used by the per-frame bridge copy. Because the
/// RUNTIME performs the vkCreateDevice, it can inject its own queue request —
/// which is the entire point (#886 / #868).
///
/// Also performs the cross-adapter guard: if the physical device the runtime
/// selected is not the one Unity is rendering on, the bridge would be
/// cross-adapter and present black with a fully healthy-looking session (the VK
/// form of #240). We refuse loudly instead, naming both adapters.
///
/// @param instance   The XrInstance (must have been created with
///                   XR_KHR_vulkan_enable2 enabled).
/// @param system_id  The XrSystemId from xrGetSystem.
/// @param gipa       xrGetInstanceProcAddr for `instance`.
/// @param unity_physical_device  Unity's VkPhysicalDevice, for the LUID guard.
///                   May be VK_NULL_HANDLE, which downgrades the guard to a WARN.
/// @return 1 on success, 0 on failure (session must not be started).
int dxr_pvk_create_device(XrInstance instance, XrSystemId system_id,
                          PFN_xrGetInstanceProcAddr gipa,
                          void *unity_physical_device);

/// Fill an XrGraphicsBindingVulkan2KHR for xrCreateSession, chaining `next`
/// (the win32 window binding). Returns a pointer to storage owned by this TU,
/// valid until dxr_pvk_destroy(). NULL if the device was never created.
const void *dxr_pvk_session_binding(const void *next);

/// Adopt Unity's Vulkan objects, captured from IUnityGraphicsVulkan by
/// displayxr_unity_plugin.cpp. Must be called BEFORE dxr_pvk_create_device so
/// the LUID guard has something to compare against, and before any bridge call
/// (the Unity-side import happens on this device).
void dxr_pvk_set_unity_objects(void *instance, void *physical_device,
                               void *device, uint32_t queue_family, void *queue);

/// Record the session swapchain's VkImages (from xrEnumerateSwapchainImages with
/// XrSwapchainImageVulkan2KHR). `images` is an array of `count` VkImage handles
/// living on the SESSION device. Also records the format/extent needed by the
/// per-frame copy.
void dxr_pvk_set_swapchain_images(const void *images, uint32_t count,
                                  uint32_t width, uint32_t height,
                                  uint32_t array_size, int64_t format);

/// Create the eye bridge: a `array_size`-layer VkImage on the SESSION device
/// exported as OPAQUE_WIN32, imported as a matching VkImage on UNITY's device,
/// plus the OPAQUE_WIN32 ordering semaphore. Unity renders into the Unity-side
/// image; dxr_pvk_copy_to_swapchain_image() copies the session-side alias into
/// the acquired swapchain image.
///
/// `eye` selects which bridge slot: -1 = the single SPI bridge (array_size == 2),
/// 0/1 = the per-eye MultiPass bridges (array_size == 1 each). This mirrors the
/// D3D12 bridge's SPI/MultiPass split.
int dxr_pvk_create_bridge(int eye, uint32_t width, uint32_t height,
                          uint32_t array_size, int64_t format);

/// POINTER to the Unity-side VkImage handle for bridge slot `eye`, suitable for
/// UnityXRRenderTextureDesc / Texture2D.CreateExternalTexture.
///
/// TRAP (cost us a session's worth of garbage-VkImage debugging in the
/// standalone backend): on Vulkan, Unity's CreateExternalTexture /
/// RegisterNativeTextureWithParams expects a POINTER TO the VkImage handle, not
/// the handle value the way every D3D path passes an ID3D1xTexture2D*. Passing
/// the value makes Unity dereference the handle as an address and produce a
/// garbage VkImage. Hence &image, and hence this returns void* to storage that
/// must outlive the texture.
void *dxr_pvk_unity_image_ptr(int eye);

/// Per-frame: copy bridge slot `eye` into swapchain image `image_index`, with
/// the layout barriers and cross-device semaphore wait. `eye` == -1 copies the
/// whole 2-layer SPI bridge; 0/1 copy that eye into the matching array slice.
/// Returns 1 on success.
int dxr_pvk_copy_to_swapchain_image(int eye, uint32_t image_index);

/// Signal the ordering semaphore from UNITY's queue, so the session-device copy
/// above waits for Unity's renders to land. Called from the render thread after
/// Unity has submitted the eye work.
void dxr_pvk_signal_unity_done(void);

/// Tear down every Vulkan object this TU owns (bridges, semaphores, command
/// pool, device, instance). Safe to call when nothing was created.
void dxr_pvk_destroy(void);

/// 1 once dxr_pvk_create_device() has succeeded.
int dxr_pvk_device_ready(void);

#ifdef __cplusplus
}
#endif

#endif // _WIN32 && ENABLE_VULKAN
