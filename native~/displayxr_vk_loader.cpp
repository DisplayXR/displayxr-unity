// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Dynamic Vulkan loader implementation. See displayxr_vk_loader.h.

#if defined(ENABLE_VULKAN) || defined(__ANDROID__) || (defined(__linux__) && !defined(__ANDROID__) && !defined(__APPLE__))

#include "displayxr_vk_loader.h"
#include <stdio.h>

#if defined(_WIN32)
#define DXR_VK_LIBNAME L"vulkan-1.dll"
#elif defined(__APPLE__)
#define DXR_VK_LIBNAME "libvulkan.dylib"
#else
#define DXR_VK_LIBNAME "libvulkan.so.1"
#endif

#if defined(_WIN32)
static HMODULE s_vk_lib = nullptr;
static void *dxr_dlsym(const char *name) { return s_vk_lib ? (void *)GetProcAddress(s_vk_lib, name) : nullptr; }
#else
#include <dlfcn.h>
static void *s_vk_lib = nullptr;
static void *dxr_dlsym(const char *name) { return s_vk_lib ? dlsym(s_vk_lib, name) : nullptr; }
#endif

// Resolve `name` for `instance` and store into `*slot` (cast through void* to
// silence the function-pointer cast). Logs once on failure.
#define DXR_VK_INST(api, instance, name) \
	*(PFN_vkVoidFunction *)&(api)->name = (api)->vkGetInstanceProcAddr((instance), #name)

#define DXR_VK_DEV(api, device, name) \
	*(PFN_vkVoidFunction *)&(api)->name = (api)->vkGetDeviceProcAddr((device), #name)

bool
dxr_vk_load_global(VkApi *api)
{
	if (!api) return false;
	if (api->vkGetInstanceProcAddr && api->vkCreateInstance) return true; // already loaded

	if (!s_vk_lib) {
#if defined(_WIN32)
		s_vk_lib = LoadLibraryW(DXR_VK_LIBNAME);
#else
		s_vk_lib = dlopen(DXR_VK_LIBNAME, RTLD_NOW | RTLD_LOCAL);
#endif
	}
	if (!s_vk_lib) {
		fprintf(stderr, "[DisplayXR-SA-VK] vulkan-1 loader not present — Vulkan standalone unavailable\n");
		return false;
	}

	api->vkGetInstanceProcAddr =
	    (PFN_vkGetInstanceProcAddr)dxr_dlsym("vkGetInstanceProcAddr");
	if (!api->vkGetInstanceProcAddr) {
		fprintf(stderr, "[DisplayXR-SA-VK] vkGetInstanceProcAddr not found in loader\n");
		return false;
	}

	// Global commands are resolved with a NULL instance.
	*(PFN_vkVoidFunction *)&api->vkCreateInstance =
	    api->vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance");
	if (!api->vkCreateInstance) {
		fprintf(stderr, "[DisplayXR-SA-VK] vkCreateInstance not resolvable\n");
		return false;
	}
	return true;
}

void
dxr_vk_load_instance(VkApi *api, VkInstance instance)
{
	if (!api || !api->vkGetInstanceProcAddr || instance == VK_NULL_HANDLE) return;

	*(PFN_vkVoidFunction *)&api->vkGetDeviceProcAddr =
	    api->vkGetInstanceProcAddr(instance, "vkGetDeviceProcAddr");

	DXR_VK_INST(api, instance, vkDestroyInstance);
	DXR_VK_INST(api, instance, vkEnumeratePhysicalDevices);
	DXR_VK_INST(api, instance, vkGetPhysicalDeviceProperties);
	DXR_VK_INST(api, instance, vkGetPhysicalDeviceQueueFamilyProperties);
	DXR_VK_INST(api, instance, vkGetPhysicalDeviceMemoryProperties);
	DXR_VK_INST(api, instance, vkCreateDevice);
#if defined(_WIN32)
	DXR_VK_INST(api, instance, vkCreateWin32SurfaceKHR);
#endif
	DXR_VK_INST(api, instance, vkGetPhysicalDeviceSurfaceSupportKHR);
	DXR_VK_INST(api, instance, vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
	DXR_VK_INST(api, instance, vkGetPhysicalDeviceSurfaceFormatsKHR);
	DXR_VK_INST(api, instance, vkGetPhysicalDeviceSurfacePresentModesKHR);
	DXR_VK_INST(api, instance, vkDestroySurfaceKHR);
}

void
dxr_vk_load_device(VkApi *api, VkDevice device)
{
	if (!api || !api->vkGetDeviceProcAddr || device == VK_NULL_HANDLE) return;

	DXR_VK_DEV(api, device, vkDestroyDevice);
	DXR_VK_DEV(api, device, vkGetDeviceQueue);
	DXR_VK_DEV(api, device, vkCreateCommandPool);
	DXR_VK_DEV(api, device, vkDestroyCommandPool);
	DXR_VK_DEV(api, device, vkAllocateCommandBuffers);
	DXR_VK_DEV(api, device, vkBeginCommandBuffer);
	DXR_VK_DEV(api, device, vkEndCommandBuffer);
	DXR_VK_DEV(api, device, vkResetCommandBuffer);
	DXR_VK_DEV(api, device, vkCmdPipelineBarrier);
	DXR_VK_DEV(api, device, vkCmdCopyImage);
	DXR_VK_DEV(api, device, vkCmdBlitImage);
	DXR_VK_DEV(api, device, vkCreateFence);
	DXR_VK_DEV(api, device, vkDestroyFence);
	DXR_VK_DEV(api, device, vkResetFences);
	DXR_VK_DEV(api, device, vkWaitForFences);
	DXR_VK_DEV(api, device, vkQueueSubmit);
	DXR_VK_DEV(api, device, vkQueueWaitIdle);
	DXR_VK_DEV(api, device, vkDeviceWaitIdle);
	DXR_VK_DEV(api, device, vkCreateImage);
	DXR_VK_DEV(api, device, vkDestroyImage);
	DXR_VK_DEV(api, device, vkGetImageMemoryRequirements);
	DXR_VK_DEV(api, device, vkAllocateMemory);
	DXR_VK_DEV(api, device, vkFreeMemory);
	DXR_VK_DEV(api, device, vkBindImageMemory);
#if defined(_WIN32)
	DXR_VK_DEV(api, device, vkGetMemoryWin32HandleKHR);
#endif
	DXR_VK_DEV(api, device, vkCreateSwapchainKHR);
	DXR_VK_DEV(api, device, vkDestroySwapchainKHR);
	DXR_VK_DEV(api, device, vkGetSwapchainImagesKHR);
	DXR_VK_DEV(api, device, vkAcquireNextImageKHR);
	DXR_VK_DEV(api, device, vkQueuePresentKHR);
}

#endif // ENABLE_VULKAN || Android || Linux
