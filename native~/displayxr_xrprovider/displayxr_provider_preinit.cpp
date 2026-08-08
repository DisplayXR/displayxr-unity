// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// XR SDK pre-init provider (#247) — the piece that makes Vulkan viable at all.
//
// Unity's Vulkan backend has an XR-aware device-init path: during engine startup
// (BEFORE any subsystem plugin is loaded the normal way) it consults the library
// named by boot.config `xrsdk-pre-init-library` for the physical device to use and
// for extra instance/device extensions. If no pre-init provider is registered,
// `[Vulkan init] SelectPhysicalDevice` runs with xrDevice=NULL and the XR
// render-surface path later hard-crashes in vk::Image::CreateImageViews when the
// display subsystem creates its first texture — proven by bisect (the crash occurs
// even with a Unity-allocated colour target, commit f5dbf59).
//
// The engine loads THIS dll early (by the name the C# loader returns from
// IXRLoaderPreInit.GetPreInitLibraryName) and calls XRSDKPreInit. Note this is a
// SECOND, earlier load path than the subsystem load: UnityPluginLoad is NOT called
// here, and at this point RegisterLifecycleProvider is rejected (observed:
// "manifest mismatch" when probing with isPreloaded=1). Only the pre-init
// interface is serviceable this early.
//
// What we do with it, in order of importance:
//  1. GetGraphicsAdapterId: pick Unity's VkPhysicalDevice to MATCH what the
//     DisplayXR runtime will pick for the session device — honoring
//     DXR_VK_FORCE_GPU (same accepted values as the runtime: index / "igpu" /
//     "dgpu"), else discrete-first, which mirrors the runtime's own
//     device_type_priority. This is the #240 adapter alignment done at the root
//     instead of by refusal: both sides converge on the same physical GPU, so the
//     external-memory bridge is same-adapter by construction. (The session-side
//     LUID guard stays as the safety net.)
//  2. GetVulkanDeviceExtensions: merge the external-memory/semaphore extensions
//     into UNITY's VkDevice so the bridge import is legal on Unity's side.
//  3. GetVulkanInstanceExtensions: capabilities + properties2 on the instance.
//
// Windows-only in effect (the VK backend is Windows-only); the export itself is
// compiled on all platforms so the boot.config entry never dangles.

#include "../unity_pluginapi/IUnityInterface.h"
#include "../unity_pluginapi/IUnityXRPreInit.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#ifdef _WIN32
#include <windows.h> // OutputDebugStringA (the VK loader header only pulls this in under ENABLE_VULKAN)
#endif

extern "C" void dxr_prov_file_log(const char *s);

static void pre_log(const char *msg)
{
	fprintf(stderr, "%s", msg);
#ifdef _WIN32
	OutputDebugStringA(msg);
#endif
	dxr_prov_file_log(msg);
}

#if defined(_WIN32) && defined(ENABLE_VULKAN)

#include "../displayxr_vk_loader.h"

// Space-separated extension lists, mirroring what the session-side bridge uses
// (displayxr_provider_gfx_vulkan.cpp) — Unity's device must speak the import side
// of exactly what the session device exports.
static const char *k_instance_exts =
    "VK_KHR_get_physical_device_properties2 "
    "VK_KHR_external_memory_capabilities "
    "VK_KHR_external_semaphore_capabilities";
static const char *k_device_exts =
    "VK_KHR_external_memory "
    "VK_KHR_external_memory_win32 "
    "VK_KHR_external_semaphore "
    "VK_KHR_external_semaphore_win32 "
    "VK_KHR_dedicated_allocation "
    "VK_KHR_get_memory_requirements2";

// Query-size/fill convention shared by both extension callbacks.
static bool preinit_fill_names(const char *src, uint32_t cap_in,
                               uint32_t *count_out, char *names)
{
	uint32_t need = (uint32_t)strlen(src) + 1;
	if (cap_in == 0 || names == nullptr) {
		if (count_out) *count_out = need;
		return true;
	}
	uint32_t n = (cap_in < need) ? cap_in : need;
	memcpy(names, src, n - 1);
	names[n - 1] = '\0';
	if (count_out) *count_out = n;
	return true;
}

static bool UNITY_INTERFACE_API
preinit_get_flags(void *, uint64_t *flags)
{
	pre_log("[DisplayXR-PREINIT] GetPreInitFlags called\n");
	if (!flags) return false;
	// Offscreen swapchain: XR renders offscreen; without this Unity's Vulkan
	// backbuffer/XR-surface setup runs a path whose XR internals are
	// uninitialised and the first XR texture create dies in
	// vk::Image::CreateImageViews (#247 experiment — flags were 0 before).
	*flags = kUnityXRPreInitFlagsUseVulkanOffscreenSwapchain;
	return true;
}

// Pick the VkPhysicalDevice for Unity — same policy the runtime applies to its own
// session device (DXR_VK_FORCE_GPU override, else discrete-first), so the two
// sides land on one adapter. See vk_bundle_init.c env_forced_gpu_index (runtime,
// supported contract #845) for the value grammar this mirrors.
static bool UNITY_INTERFACE_API
preinit_get_adapter(void *, UnityXRPreInitRenderer renderer,
                    uint64_t rendererData, uint64_t *adapterId)
{
	{
		char m[160];
		snprintf(m, sizeof(m),
		         "[DisplayXR-PREINIT] GetGraphicsAdapterId called: renderer=%d rendererData=%p\n",
		         (int)renderer, (void *)(uintptr_t)rendererData);
		pre_log(m);
	}
	if (renderer != kUnityXRPreInitRendererVulkan || !adapterId) return false;
	VkInstance instance = (VkInstance)rendererData;

	// Unity 6000.4 asks ONCE, at boot, with rendererData==NULL, and matches the
	// answer against its later instance's devices in a way that accepts NEITHER a
	// foreign VkPhysicalDevice handle NOR a packed LUID (both observed to produce
	// "Could not select a physical device" → silent D3D12 fallback). Until the
	// accepted encoding is known, answering at all is strictly worse than not:
	// declining keeps Unity on Vulkan with its own (discrete-first) pick, and the
	// runtime is aligned to THAT via DXR_VK_FORCE_GPU (DisplayXRGpuPreference sets
	// it from Unity's adapter class before xrCreateInstance), with the session-side
	// LUID guard as the backstop. DISPLAYXR_VK_PREINIT_ADAPTER=1 re-enables the
	// answer for continued experimentation.
	if (instance == VK_NULL_HANDLE && getenv("DISPLAYXR_VK_PREINIT_ADAPTER") == NULL) {
		pre_log("[DisplayXR-PREINIT] GetGraphicsAdapterId: declining instance-less query "
		        "(Unity keeps its own pick; runtime follows via DXR_VK_FORCE_GPU)\n");
		return false;
	}

	VkApi api = {};
	if (!dxr_vk_load_global(&api)) return false;

	// Unity 6 calls this ONCE, BEFORE creating any VkInstance (rendererData==NULL,
	// observed), caches the answer, and never asks again. Its matcher then compares
	// the returned id against ITS OWN instance's devices — a VkPhysicalDevice
	// handle from any instance we create here can never match ("Selected physical
	// device 0 / Could not select a physical device" → silent D3D12 fallback,
	// observed). The only instance-independent adapter identity is the LUID, which
	// is also exactly what this API's D3D arm returns — so for the instance-less
	// call we answer with the 8-byte deviceLUID packed into the uint64. We still
	// need SOME instance to enumerate from; it stays private and is destroyed on
	// exit paths' natural process teardown.
	bool instanceless = (instance == VK_NULL_HANDLE);
	static VkInstance s_own_instance = VK_NULL_HANDLE;
	if (instanceless) {
		if (s_own_instance == VK_NULL_HANDLE) {
			const char *enum_exts[] = { "VK_KHR_get_physical_device_properties2" };
			VkApplicationInfo app = {};
			app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
			app.pApplicationName = "DisplayXR PreInit";
			app.apiVersion = VK_API_VERSION_1_1;
			VkInstanceCreateInfo ici = {};
			ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
			ici.pApplicationInfo = &app;
			ici.enabledExtensionCount = 1;
			ici.ppEnabledExtensionNames = enum_exts;
			if (api.vkCreateInstance(&ici, NULL, &s_own_instance) != VK_SUCCESS) {
				pre_log("[DisplayXR-PREINIT] vkCreateInstance (enumeration instance) failed\n");
				return false;
			}
		}
		instance = s_own_instance;
	}
	dxr_vk_load_instance(&api, instance);
	if (!api.vkEnumeratePhysicalDevices || !api.vkGetPhysicalDeviceProperties) return false;

	uint32_t count = 0;
	api.vkEnumeratePhysicalDevices(instance, &count, NULL);
	if (count == 0) return false;
	if (count > 16) count = 16;
	VkPhysicalDevice devs[16] = {};
	api.vkEnumeratePhysicalDevices(instance, &count, devs);

	int pick = -1;
	const char *force = getenv("DXR_VK_FORCE_GPU");
	if (force && force[0]) {
		if (force[0] >= '0' && force[0] <= '9') {
			int idx = atoi(force);
			if ((uint32_t)idx < count) pick = idx;
		} else {
			VkPhysicalDeviceType want =
			    (!strcmp(force, "igpu") || !strcmp(force, "integrated"))
			        ? VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU
			        : (!strcmp(force, "dgpu") || !strcmp(force, "discrete"))
			              ? VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU
			              : VK_PHYSICAL_DEVICE_TYPE_OTHER;
			if (want != VK_PHYSICAL_DEVICE_TYPE_OTHER) {
				for (uint32_t i = 0; i < count; i++) {
					VkPhysicalDeviceProperties p = {};
					api.vkGetPhysicalDeviceProperties(devs[i], &p);
					if (p.deviceType == want) { pick = (int)i; break; }
				}
			}
		}
	}
	if (pick < 0) {
		// Discrete-first, mirroring the runtime's device_type_priority default.
		for (uint32_t i = 0; i < count; i++) {
			VkPhysicalDeviceProperties p = {};
			api.vkGetPhysicalDeviceProperties(devs[i], &p);
			if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) { pick = (int)i; break; }
		}
		if (pick < 0) pick = 0;
	}

	VkPhysicalDeviceProperties props = {};
	api.vkGetPhysicalDeviceProperties(devs[pick], &props);

	uint64_t id = 0;
	const char *id_kind;
	if (instanceless) {
		// Instance-less query → LUID (instance-independent; Unity matches its own
		// enumeration against it). Requires properties2 on the enumeration instance.
		if (!api.vkGetPhysicalDeviceProperties2) {
			pre_log("[DisplayXR-PREINIT] properties2 unavailable — cannot produce a LUID\n");
			return false;
		}
		VkPhysicalDeviceIDProperties idp = {};
		idp.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
		VkPhysicalDeviceProperties2 p2 = {};
		p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
		p2.pNext = &idp;
		api.vkGetPhysicalDeviceProperties2(devs[pick], &p2);
		if (!idp.deviceLUIDValid) {
			pre_log("[DisplayXR-PREINIT] deviceLUID invalid on the picked adapter\n");
			return false;
		}
		memcpy(&id, idp.deviceLUID, sizeof(id));
		id_kind = "LUID";
	} else {
		// Instance provided (the classic contract) → the handle from THAT instance.
		id = (uint64_t)devs[pick];
		id_kind = "VkPhysicalDevice";
	}

	char msg[352];
	snprintf(msg, sizeof(msg),
	         "[DisplayXR-PREINIT] Vulkan adapter for Unity: #%d %s %s=%llx (%s%s)\n",
	         pick, props.deviceName, id_kind, (unsigned long long)id,
	         force && force[0] ? "DXR_VK_FORCE_GPU=" : "discrete-first default",
	         force && force[0] ? force : "");
	pre_log(msg);

	*adapterId = id;
	return true;
}

static bool UNITY_INTERFACE_API
preinit_get_instance_exts(void *, uint32_t cap, uint32_t *count, char *names)
{
	char m[96];
	snprintf(m, sizeof(m), "[DisplayXR-PREINIT] GetVulkanInstanceExtensions called cap=%u\n", cap);
	pre_log(m);
	return preinit_fill_names(k_instance_exts, cap, count, names);
}

static bool UNITY_INTERFACE_API
preinit_get_device_exts(void *, uint32_t cap, uint32_t *count, char *names)
{
	char m[96];
	snprintf(m, sizeof(m), "[DisplayXR-PREINIT] GetVulkanDeviceExtensions called cap=%u\n", cap);
	pre_log(m);
	return preinit_fill_names(k_device_exts, cap, count, names);
}

static UnityXRPreInitProvider s_preinit_provider = {
    nullptr,
    preinit_get_flags,
    preinit_get_adapter,
    preinit_get_instance_exts,
    preinit_get_device_exts,
};

#endif // _WIN32 && ENABLE_VULKAN

// The engine-called entry point (boot.config xrsdk-pre-init-library). Present on
// every platform so the boot.config entry the C# loader emits never dangles; a
// no-op where the VK backend doesn't exist.
extern "C" UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API
XRSDKPreInit(IUnityInterfaces *interfaces)
{
#if defined(_WIN32) && defined(ENABLE_VULKAN)
	if (!interfaces) return;
	IUnityXRPreInit *preinit = UNITY_GET_INTERFACE(interfaces, IUnityXRPreInit);
	if (!preinit) {
		pre_log("[DisplayXR-PREINIT] IUnityXRPreInit unavailable — engine too old?\n");
		return;
	}
	preinit->RegisterPreInitProvider(&s_preinit_provider);
	pre_log("[DisplayXR-PREINIT] XRSDKPreInit: provider registered (pre-graphics-init)\n");
#else
	(void)interfaces;
	pre_log("[DisplayXR-PREINIT] XRSDKPreInit: no-op on this platform\n");
#endif
}
