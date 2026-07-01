// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Unity native-plugin-interface glue (issue #124). See displayxr_unity_plugin.h.
//
// Windows-only for now: the only consumer is the standalone Vulkan preview
// backend, which is a Windows editor feature. UnityPluginLoad is invoked by
// Unity when this DLL is loaded; we grab IUnityGraphics + IUnityGraphicsVulkan
// and capture Unity's VkDevice so the SA backend can import the atlas bridge on
// Unity's own device.

#include "displayxr_unity_plugin.h"

#include <stdio.h>

// Pull in Vulkan type defs (VK_NO_PROTOTYPES — no symbols, just types) BEFORE
// the Unity Vulkan header, which does `#include <vulkan/vulkan.h>`.
#if defined(ENABLE_VULKAN) || defined(__ANDROID__) || (defined(__linux__) && !defined(__ANDROID__) && !defined(__APPLE__))
#define DXR_HAVE_UNITY_VULKAN 1
#include "displayxr_vk_loader.h"
#endif

#include "IUnityInterface.h"
#include "IUnityGraphics.h"
#if defined(DXR_HAVE_UNITY_VULKAN)
#include "IUnityGraphicsVulkan.h"
#endif

static IUnityInterfaces *s_unity_ifaces = nullptr;
static IUnityGraphics   *s_unity_gfx    = nullptr;
#if defined(DXR_HAVE_UNITY_VULKAN)
static IUnityGraphicsVulkan *s_unity_vk = nullptr;   // V1 view (works for both)
static UnityVulkanInstance   s_vk_inst  = {};
static bool                  s_vk_captured = false;

// IUnityGraphicsVulkan::Instance() is stable between device init and shutdown,
// so we can capture it lazily on first request as well as on the init event.
static void capture_vulkan_instance(void)
{
	if (s_vk_captured || !s_unity_vk) return;
	if (!s_unity_gfx || s_unity_gfx->GetRenderer() != kUnityGfxRendererVulkan) return;
	s_vk_inst = s_unity_vk->Instance();
	if (s_vk_inst.device != VK_NULL_HANDLE) {
		s_vk_captured = true;
		fprintf(stderr, "[DisplayXR] Unity Vulkan device captured: instance=%p physicalDevice=%p device=%p queue=%p qf=%u\n",
		        (void *)s_vk_inst.instance, (void *)s_vk_inst.physicalDevice,
		        (void *)s_vk_inst.device, (void *)s_vk_inst.graphicsQueue,
		        s_vk_inst.queueFamilyIndex);
	}
}
#endif

static void UNITY_INTERFACE_API
on_graphics_device_event(UnityGfxDeviceEventType eventType)
{
#if defined(DXR_HAVE_UNITY_VULKAN)
	if (eventType == kUnityGfxDeviceEventInitialize) {
		capture_vulkan_instance();
	} else if (eventType == kUnityGfxDeviceEventShutdown) {
		s_vk_captured = false;
		s_vk_inst = {};
	}
#else
	(void)eventType;
#endif
}

extern "C" void UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API
UnityPluginLoad(IUnityInterfaces *unityInterfaces)
{
	s_unity_ifaces = unityInterfaces;
	if (!s_unity_ifaces) return;

	s_unity_gfx = s_unity_ifaces->Get<IUnityGraphics>();
#if defined(DXR_HAVE_UNITY_VULKAN)
	// Instance() is at the same vtable slot in V1 and V2 (after
	// InterceptInitialization / InterceptVulkanAPI / ConfigureEvent), so a V2
	// pointer can be used through the V1 type for our needs. Prefer V2 (Unity 6
	// editors may register only it), fall back to V1.
	s_unity_vk = reinterpret_cast<IUnityGraphicsVulkan *>(
	    s_unity_ifaces->Get<IUnityGraphicsVulkanV2>());
	if (!s_unity_vk)
		s_unity_vk = s_unity_ifaces->Get<IUnityGraphicsVulkan>();
#endif

	if (s_unity_gfx) {
		s_unity_gfx->RegisterDeviceEventCallback(on_graphics_device_event);
		// The plugin may load AFTER the device was created (editor), in which
		// case kUnityGfxDeviceEventInitialize already fired — capture now too.
		on_graphics_device_event(kUnityGfxDeviceEventInitialize);
	}
}

extern "C" void UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API
UnityPluginUnload(void)
{
	if (s_unity_gfx)
		s_unity_gfx->UnregisterDeviceEventCallback(on_graphics_device_event);
	s_unity_gfx    = nullptr;
	s_unity_ifaces = nullptr;
#if defined(DXR_HAVE_UNITY_VULKAN)
	s_unity_vk     = nullptr;
	s_vk_captured  = false;
	s_vk_inst      = {};
#endif
}

// ---- C ABI exposed to the rest of the plugin --------------------------------

extern "C" int
displayxr_unity_get_renderer(void)
{
	if (!s_unity_gfx) return -1;
	return (int)s_unity_gfx->GetRenderer();
}

extern "C" bool
displayxr_unity_get_vulkan(void **out_instance, void **out_physical_device,
                           void **out_device, void **out_graphics_queue,
                           uint32_t *out_queue_family_index)
{
#if defined(DXR_HAVE_UNITY_VULKAN)
	capture_vulkan_instance(); // lazy capture if the init event was missed
	if (!s_vk_captured || s_vk_inst.device == VK_NULL_HANDLE) return false;
	if (out_instance)            *out_instance            = (void *)s_vk_inst.instance;
	if (out_physical_device)     *out_physical_device     = (void *)s_vk_inst.physicalDevice;
	if (out_device)              *out_device              = (void *)s_vk_inst.device;
	if (out_graphics_queue)      *out_graphics_queue      = (void *)s_vk_inst.graphicsQueue;
	if (out_queue_family_index)  *out_queue_family_index  = s_vk_inst.queueFamilyIndex;
	return true;
#else
	(void)out_instance; (void)out_physical_device; (void)out_device;
	(void)out_graphics_queue; (void)out_queue_family_index;
	return false;
#endif
}
