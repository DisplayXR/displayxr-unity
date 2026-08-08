// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Dynamic Vulkan loader for the standalone preview backend (issue #124).
//
// The plugin must CALL Vulkan (create device/images/swapchain) for the Vulkan
// standalone session, but deliberately keeps NO hard dependency on vulkan-1.dll
// so the shipped DLL still loads on D3D11/D3D12-only systems. We therefore
// compile against the vendored Vulkan-Headers with VK_NO_PROTOTYPES (no bare
// vk* symbols are emitted, nothing to link) and resolve every entry point from
// vulkan-1.dll at runtime into the VkApi dispatch table below.
//
// Active only when ENABLE_VULKAN is defined (Windows opt-in) or on Android/Linux.

#pragma once

#if defined(ENABLE_VULKAN) || defined(__ANDROID__) || (defined(__linux__) && !defined(__ANDROID__) && !defined(__APPLE__))

#if defined(_WIN32)
#include <windows.h>
#define VK_USE_PLATFORM_WIN32_KHR
#endif

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

// Dispatch table. Filled by dxr_vk_load_* below. A single table covers one
// instance + one device; the standalone backend keeps one for its own device
// and a second for Unity's editor device (used by the cross-device bridge
// import).
struct VkApi {
	// loader-level
	PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
	PFN_vkGetDeviceProcAddr   vkGetDeviceProcAddr;

	// global (instance == NULL)
	PFN_vkCreateInstance vkCreateInstance;

	// instance-level
	PFN_vkDestroyInstance                          vkDestroyInstance;
	PFN_vkEnumeratePhysicalDevices                 vkEnumeratePhysicalDevices;
	PFN_vkGetPhysicalDeviceProperties              vkGetPhysicalDeviceProperties;
	PFN_vkGetPhysicalDeviceQueueFamilyProperties   vkGetPhysicalDeviceQueueFamilyProperties;
	PFN_vkGetPhysicalDeviceMemoryProperties        vkGetPhysicalDeviceMemoryProperties;
	PFN_vkCreateDevice                             vkCreateDevice;
	// Provider VK backend (#247): VkPhysicalDeviceIDProperties.deviceLUID drives the
	// cross-adapter guard — the VK cousin of the D3D12 LUID check (#240). Core in 1.1;
	// the KHR alias is the fallback on a 1.0 instance.
	PFN_vkGetPhysicalDeviceProperties2             vkGetPhysicalDeviceProperties2;
#if defined(_WIN32)
	PFN_vkCreateWin32SurfaceKHR                    vkCreateWin32SurfaceKHR;
#endif
	PFN_vkGetPhysicalDeviceSurfaceSupportKHR       vkGetPhysicalDeviceSurfaceSupportKHR;
	PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR  vkGetPhysicalDeviceSurfaceCapabilitiesKHR;
	PFN_vkGetPhysicalDeviceSurfaceFormatsKHR       vkGetPhysicalDeviceSurfaceFormatsKHR;
	PFN_vkGetPhysicalDeviceSurfacePresentModesKHR  vkGetPhysicalDeviceSurfacePresentModesKHR;
	PFN_vkDestroySurfaceKHR                        vkDestroySurfaceKHR;

	// device-level
	PFN_vkDestroyDevice            vkDestroyDevice;
	PFN_vkGetDeviceQueue           vkGetDeviceQueue;
	PFN_vkCreateCommandPool        vkCreateCommandPool;
	PFN_vkDestroyCommandPool       vkDestroyCommandPool;
	PFN_vkAllocateCommandBuffers   vkAllocateCommandBuffers;
	PFN_vkBeginCommandBuffer       vkBeginCommandBuffer;
	PFN_vkEndCommandBuffer         vkEndCommandBuffer;
	PFN_vkResetCommandBuffer       vkResetCommandBuffer;
	PFN_vkCmdPipelineBarrier       vkCmdPipelineBarrier;
	PFN_vkCmdCopyImage             vkCmdCopyImage;
	PFN_vkCmdBlitImage             vkCmdBlitImage;
	PFN_vkCreateFence              vkCreateFence;
	PFN_vkDestroyFence             vkDestroyFence;
	PFN_vkResetFences              vkResetFences;
	PFN_vkWaitForFences            vkWaitForFences;
	PFN_vkQueueSubmit              vkQueueSubmit;
	PFN_vkQueueWaitIdle            vkQueueWaitIdle;
	PFN_vkDeviceWaitIdle           vkDeviceWaitIdle;
	PFN_vkCreateImage              vkCreateImage;
	PFN_vkDestroyImage             vkDestroyImage;
	PFN_vkGetImageMemoryRequirements vkGetImageMemoryRequirements;
	PFN_vkAllocateMemory           vkAllocateMemory;
	PFN_vkFreeMemory               vkFreeMemory;
	PFN_vkBindImageMemory          vkBindImageMemory;
	// Provider VK backend (#247): cross-device ordering between the runtime's session
	// device and Unity's device — the VK cousin of the shared ID3D12Fence. The bridge
	// image is exported/imported as OPAQUE_WIN32 memory; the ordering primitive is an
	// OPAQUE_WIN32 semaphore signalled on one device and waited on the other.
	PFN_vkCreateSemaphore          vkCreateSemaphore;
	PFN_vkDestroySemaphore         vkDestroySemaphore;
#if defined(_WIN32)
	PFN_vkGetMemoryWin32HandleKHR  vkGetMemoryWin32HandleKHR;
	PFN_vkGetSemaphoreWin32HandleKHR    vkGetSemaphoreWin32HandleKHR;
	PFN_vkImportSemaphoreWin32HandleKHR vkImportSemaphoreWin32HandleKHR;
#endif
	PFN_vkCreateSwapchainKHR       vkCreateSwapchainKHR;
	PFN_vkDestroySwapchainKHR      vkDestroySwapchainKHR;
	PFN_vkGetSwapchainImagesKHR    vkGetSwapchainImagesKHR;
	PFN_vkAcquireNextImageKHR      vkAcquireNextImageKHR;
	PFN_vkQueuePresentKHR          vkQueuePresentKHR;
};

// Load vulkan-1.dll (once, process-wide) and resolve vkGetInstanceProcAddr +
// the global entry points (vkCreateInstance) into `api`. Returns false if
// vulkan-1.dll is absent or vkGetInstanceProcAddr can't be found — callers
// must treat that as "Vulkan unavailable" and fall back gracefully.
bool dxr_vk_load_global(VkApi *api);

// Resolve instance-level entry points for `instance` into `api` (also loads
// vkGetDeviceProcAddr). Call after vkCreateInstance.
void dxr_vk_load_instance(VkApi *api, VkInstance instance);

// Resolve device-level entry points for `device` into `api`. Requires
// api->vkGetDeviceProcAddr (set by dxr_vk_load_instance). Call after
// vkCreateDevice. Safe to call on a foreign device (e.g. Unity's) as long as
// `api` was first primed via dxr_vk_load_instance with that device's instance.
void dxr_vk_load_device(VkApi *api, VkDevice device);

#endif // ENABLE_VULKAN || Android || Linux
