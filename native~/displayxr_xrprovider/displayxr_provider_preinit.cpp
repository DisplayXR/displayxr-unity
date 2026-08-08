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
// PER-PLATFORM ADAPTER POLICY (#249) — the interesting divergence lives in
// preinit_get_adapter() below and is worth stating up front:
//
//   Windows : answer a knowingly-non-matching sentinel, ON PURPOSE, so Unity
//             falls back to D3D12 and the app gets working stereo instead of a
//             crash. Unity's matcher makes a truthful answer impossible.
//   Linux   : DECLINE the query. There is no next graphics API to fall back to
//             (this provider does not support GL), so the Windows sentinel would
//             just fail engine init. Declining is also the *correct* answer on a
//             single-GPU box, and it is what the runtime aligns to via
//             DXR_VK_FORCE_GPU / client_vk_deviceUUID.
//
// Declining is precisely the input that produced the CreateImageViews crash on
// Windows — whether that Unity defect also fires on Linux is UNVERIFIED, and
// finding out is the reason the Linux leg exists. Keep both arms reachable from
// one binary (DISPLAYXR_VK_EXPERIMENTAL) so whoever has the hardware can bisect.
//
// The export itself is compiled on all platforms so the boot.config entry never
// dangles; it is a no-op where the VK backend does not exist (macOS).

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

#if defined(ENABLE_VULKAN)

#include "../displayxr_vk_loader.h"

// Space-separated extension lists, mirroring what the session-side bridge uses
// (displayxr_provider_gfx_vulkan.cpp) — Unity's device must speak the import side
// of exactly what the session device exports. The OS-flavoured pair
// (external_memory_win32 vs external_memory_fd) must stay in lockstep with the
// PVK_EXT_EXTERNAL_* macros there, or the bridge exports a handle type Unity's
// device cannot import.
static const char *k_instance_exts =
    "VK_KHR_get_physical_device_properties2 "
    "VK_KHR_external_memory_capabilities "
    "VK_KHR_external_semaphore_capabilities";
static const char *k_device_exts =
#if defined(_WIN32)
    "VK_KHR_external_memory "
    "VK_KHR_external_memory_win32 "
    "VK_KHR_external_semaphore "
    "VK_KHR_external_semaphore_win32 "
#else
    "VK_KHR_external_memory "
    "VK_KHR_external_memory_fd "
    "VK_KHR_external_semaphore "
    "VK_KHR_external_semaphore_fd "
#endif
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
	// 0 deliberately. kUnityXRPreInitFlagsUseVulkanOffscreenSwapchain was tried
	// during the #248 bring-up and did NOT fix the CreateImageViews crash, and
	// under the deliberate-D3D12-fallback policy below Vulkan never proceeds far
	// enough for it to matter — so keep the smallest possible surface area on the
	// D3D12/D3D11 paths this provider is now registered on in every Windows build.
	*flags = 0;
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

#if !defined(_WIN32)
	// LINUX POLICY (#249): DECLINE the instance-less query.
	//
	// Do NOT copy the Windows sentinel below. Its whole value is that Unity has a
	// working D3D12 backend to fall back to; on Linux the fallback list is Vulkan
	// or GL, this provider has no GL backend, and a Vulkan-only project would
	// simply fail engine init. Declining is also the correct answer on the
	// single-GPU boxes this targets — the runtime aligns to Unity's device via
	// DXR_VK_FORCE_GPU / client_vk_deviceUUID rather than via this callback, and
	// the session-side UUID guard is the safety net if they ever diverge.
	//
	// UNVERIFIED, and the reason this leg exists: on Windows, declining leaves
	// xrDevice NULL and Unity's XR render-surface path hard-crashes in
	// vk::Image::CreateImageViews. Whether that Unity defect also fires on Linux
	// is exactly what a Linux player run has to answer. If it does, Linux Vulkan
	// is blocked upstream the same way Windows is, and this callback is where a
	// Linux-appropriate mitigation would go.
	if (instance == VK_NULL_HANDLE) {
		pre_log("[DisplayXR-PREINIT] Linux: declining the instance-less adapter query — correct "
		        "on a single-GPU box, and there is no alternative graphics API to steer to "
		        "(displayxr-unity#249). The runtime aligns to Unity's device via DXR_VK_FORCE_GPU "
		        "/ deviceUUID instead.\n");
		return false;
	}
	// instance != NULL: fall through to the enumerate-and-answer path below, which
	// is platform-neutral.
#else
	// DELIBERATE D3D12 FALLBACK (the shipping policy on Unity 6000.4, Windows only).
	//
	// Background, all hardware-verified (#248): Unity asks for the adapter ONCE, at
	// boot, with rendererData==NULL, and matches the cached answer against its
	// later instance's devices by RAW VkPhysicalDevice handle equality
	// (UnityPlayer.dll rva 0x10c47c0) — an encoding no provider can produce before
	// that instance exists. The two possible truthful answers are both bad:
	//   - decline (return false) → Unity stays on Vulkan with xrDevice=0 → its
	//     XR render-surface path hard-CRASHES in vk::Image::CreateImageViews on
	//     the first XR texture, with every provider-controllable input bisected
	//     away;
	//   - answer anything → no match → "Could not select a physical device" →
	//     Unity falls back to the next API in the project's list.
	// The second failure is the useful one: a Vulkan-configured project lands on
	// the fully-working D3D12 backend and gets STEREO, instead of a crash. So we
	// answer a guaranteed-non-matching sentinel ON PURPOSE, and say so loudly.
	//
	// Consequences to know:
	//   - A project whose graphics-API list is Vulkan-ONLY has no API to fall back
	//     to and fails engine init. List D3D12 after Vulkan (the sample projects
	//     do). Still better than the alternative, which is a mid-run crash.
	//   - DISPLAYXR_VK_EXPERIMENTAL=1 restores the decline path (Unity proceeds on
	//     Vulkan and reaches the crash) for anyone verifying a future Unity fix.
	if (instance == VK_NULL_HANDLE && getenv("DISPLAYXR_VK_EXPERIMENTAL") == NULL) {
		pre_log("[DisplayXR-PREINIT] Vulkan XR is broken in Unity 6000.4 (adapter query "
		        "unanswerable + CreateImageViews crash, displayxr-unity#248) — deliberately "
		        "steering the engine to the next graphics API in the project list (expect "
		        "D3D12). Stereo will run on that backend. Set DISPLAYXR_VK_EXPERIMENTAL=1 "
		        "to attempt Vulkan anyway.\n");
		*adapterId = 0x1; // non-NULL, can never equal a real VkPhysicalDevice handle
		return true;
	}
	if (instance == VK_NULL_HANDLE) {
		pre_log("[DisplayXR-PREINIT] DISPLAYXR_VK_EXPERIMENTAL=1: declining the adapter "
		        "query — Unity will proceed on Vulkan (crash expected until Unity fixes "
		        "#248's defects)\n");
		return false;
	}
#endif // !_WIN32 / _WIN32 adapter policy

	// From here on instance != NULL: the classic contract, only reachable if a
	// (future/fixed) Unity passes its real VkInstance again. Enumerate from THAT
	// instance and answer with one of its handles — the only encoding the matcher
	// accepts. (The instance-less LUID/own-instance experiments were removed after
	// the matcher disassembly proved no instance-less answer can ever match.)
	VkApi api = {};
	if (!dxr_vk_load_global(&api)) return false;
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

	// Handle from THE PROVIDED instance — the only encoding the matcher accepts.
	uint64_t id = (uint64_t)devs[pick];

	char msg[352];
	snprintf(msg, sizeof(msg),
	         "[DisplayXR-PREINIT] Vulkan adapter for Unity: #%d %s VkPhysicalDevice=%llx (%s%s)\n",
	         pick, props.deviceName, (unsigned long long)id,
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

#endif // ENABLE_VULKAN

// The engine-called entry point (boot.config xrsdk-pre-init-library). Present on
// every platform so the boot.config entry the C# loader emits never dangles; a
// no-op where the VK backend doesn't exist.
extern "C" UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API
XRSDKPreInit(IUnityInterfaces *interfaces)
{
#if defined(ENABLE_VULKAN)
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
