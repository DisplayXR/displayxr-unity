// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Native-Wayland player: find the player's own window.
//
// Under `-force-wayland` the player has no X11 window, so displayxr_linux.c finds
// nothing to bind, and the weave must go into the player's Wayland window instead
// (displayxr_linux_wayland.c). Unity exposes no Wayland handles: its SDL2 is built
// in and exports no SDL_* symbols, and its SDL creates the window's VkSurfaceKHR
// with a vkCreateWaylandSurfaceKHR it resolves from the Vulkan loader itself, so
// IUnityGraphicsVulkan's hooks never see it (they only reach the dispatch table
// Unity built at its FIRST Vulkan init, which runs before any plug-in code).
//
// A Vulkan layer sits in the loader, under every caller. The player creates its
// real VkInstance AFTER XRSDKPreInit (a second instance, after device detection),
// so pre-init writes a layer manifest and points the loader at it with
// VK_ADD_IMPLICIT_LAYER_PATH. That instance carries the layer; the variable and
// the manifest are removed right after it is created, so no other instance (the
// runtime's) and no child process loads it.
//
// What the layer does on the player's instance, and nothing else:
//  - records the wl_display + wl_surface of every VkSurfaceKHR the player makes
//    (it makes a new one on a resolution change, and may recreate the window);
//  - records the size of the player's window swapchain (its logical size);
//  - presents that swapchain PRE_MULTIPLIED when the surface supports it, so a
//    transparent app shows the desktop through the parts of the window the
//    player leaves at alpha 0 (the weave sub-surface sits on top).
//
// The manifest points at THIS module: Unity loads the library twice (Plugins/ for
// pre-init and the provider, Plugins/x86_64/ as well), as separate modules, and
// the provider and the C# P/Invokes run in this one, so the recorded state is
// read straight from these statics.

#if defined(__linux__) && !defined(__ANDROID__) && defined(ENABLE_VULKAN)

#include "../displayxr_vk_loader.h"
#include "../displayxr_linux_wayland.h"

#include <vulkan/vk_layer.h>

#include <dlfcn.h>
#include <limits.h>
#include <mutex>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DXR_WLCAP_EXPORT extern "C" __attribute__((visibility("default")))

// VkWaylandSurfaceCreateInfoKHR, spelled out so this TU needs no Wayland headers.
typedef struct DxrVkWaylandSurfaceCreateInfo {
	VkStructureType sType;
	const void *pNext;
	VkFlags flags;
	struct wl_display *display;
	struct wl_surface *surface;
} DxrVkWaylandSurfaceCreateInfo;

typedef VkResult(VKAPI_PTR *PFN_dxrCreateWaylandSurface)(VkInstance, const DxrVkWaylandSurfaceCreateInfo *,
                                                         const VkAllocationCallbacks *, VkSurfaceKHR *);

/*
 *
 * The player's window, as recorded by the layer. Written on the player's
 * threads (surface/swapchain creation), read by the provider and C#.
 *
 */

static std::mutex s_player_mutex;
static struct wl_display *s_player_display = nullptr;
static struct wl_surface *s_player_surface = nullptr; // NULL while the player has no window surface
static uint32_t s_player_w = 0, s_player_h = 0;        // its swapchain extent, logical px
static unsigned s_player_generation = 0;               // bumps per new window VkSurfaceKHR
static VkSurfaceKHR s_player_vk[8];                    // its live window VkSurfaceKHRs

static int
player_vk_is_locked(VkSurfaceKHR sfc)
{
	for (VkSurfaceKHR p : s_player_vk)
		if (sfc != VK_NULL_HANDLE && p == sfc)
			return 1;
	return 0;
}

extern "C" int
dxr_wl_unity_surface(struct wl_display **out_display, struct wl_surface **out_surface)
{
	std::lock_guard<std::mutex> lock(s_player_mutex);
	if (out_display)
		*out_display = s_player_display;
	if (out_surface)
		*out_surface = s_player_surface;
	return s_player_display && s_player_surface ? 1 : 0;
}

extern "C" int
dxr_wl_unity_swapchain_size(int *out_w, int *out_h)
{
	std::lock_guard<std::mutex> lock(s_player_mutex);
	if (out_w)
		*out_w = (int)s_player_w;
	if (out_h)
		*out_h = (int)s_player_h;
	return s_player_w && s_player_h ? 1 : 0;
}

extern "C" unsigned
dxr_wl_player_surface_generation(void)
{
	std::lock_guard<std::mutex> lock(s_player_mutex);
	return s_player_generation;
}

/*
 *
 * The layer: the chaining every layer must do, plus three hooks.
 *
 */

// The loader's dispatch-table pointer is the first word of every dispatchable
// handle, shared by an instance and its physical devices, and by a device and its
// queues: the standard key.
static inline void *
dispatch_key(const void *handle)
{
	return *(void *const *)handle;
}

struct DxrLayerInstance {
	void *key;
	VkInstance instance;
	PFN_vkGetInstanceProcAddr next_gipa;
};
struct DxrLayerDevice {
	void *key;
	VkInstance instance;
	VkPhysicalDevice physical_device;
	PFN_vkGetInstanceProcAddr next_gipa;
	PFN_vkGetDeviceProcAddr next_gdpa;
};

static std::mutex s_layer_mutex;
static DxrLayerInstance s_instances[16];
static DxrLayerDevice s_devices[16];

static const DxrLayerInstance *
instance_for_locked(const void *dispatchable)
{
	void *key = dispatch_key(dispatchable);
	for (auto &i : s_instances)
		if (i.key == key)
			return &i;
	return nullptr;
}

static PFN_vkGetInstanceProcAddr
next_gipa_for(const void *dispatchable)
{
	std::lock_guard<std::mutex> lock(s_layer_mutex);
	const DxrLayerInstance *i = instance_for_locked(dispatchable);
	return i ? i->next_gipa : nullptr;
}

static int
device_for(VkDevice device, DxrLayerDevice *out)
{
	std::lock_guard<std::mutex> lock(s_layer_mutex);
	void *key = dispatch_key(device);
	for (auto &d : s_devices)
		if (d.key == key) {
			*out = d;
			return 1;
		}
	return 0;
}

static void layer_disarm(void); // below: the env var and the manifest go

static VKAPI_ATTR VkResult VKAPI_CALL
layer_CreateInstance(const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *alloc, VkInstance *out)
{
	VkLayerInstanceCreateInfo *chain = (VkLayerInstanceCreateInfo *)ci->pNext;
	while (chain && !(chain->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO &&
	                  chain->function == VK_LAYER_LINK_INFO))
		chain = (VkLayerInstanceCreateInfo *)chain->pNext;
	if (!chain)
		return VK_ERROR_INITIALIZATION_FAILED;
	PFN_vkGetInstanceProcAddr next_gipa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
	chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;
	PFN_vkCreateInstance next_create = (PFN_vkCreateInstance)next_gipa(VK_NULL_HANDLE, "vkCreateInstance");
	VkResult r = next_create(ci, alloc, out);
	if (r == VK_SUCCESS) {
		std::lock_guard<std::mutex> lock(s_layer_mutex);
		bool stored = false;
		for (auto &i : s_instances)
			if (!i.key) {
				i = DxrLayerInstance{dispatch_key(*out), *out, next_gipa};
				stored = true;
				break;
			}
		if (!stored)
			fprintf(stderr, "[DisplayXR-WL] layer: instance table full; this instance is not tracked\n");
	}
	// This is the player's real instance: nothing else needs the layer.
	layer_disarm();
	return r;
}

static VKAPI_ATTR void VKAPI_CALL
layer_DestroyInstance(VkInstance instance, const VkAllocationCallbacks *alloc)
{
	void *key = dispatch_key(instance); // before the instance is freed
	PFN_vkGetInstanceProcAddr gipa = next_gipa_for(instance);
	if (gipa)
		((PFN_vkDestroyInstance)gipa(instance, "vkDestroyInstance"))(instance, alloc);
	std::lock_guard<std::mutex> lock(s_layer_mutex);
	for (auto &i : s_instances)
		if (i.key == key)
			i = DxrLayerInstance{};
}

static VKAPI_ATTR VkResult VKAPI_CALL
layer_CreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo *ci, const VkAllocationCallbacks *alloc,
                   VkDevice *out)
{
	VkLayerDeviceCreateInfo *chain = (VkLayerDeviceCreateInfo *)ci->pNext;
	while (chain && !(chain->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO &&
	                  chain->function == VK_LAYER_LINK_INFO))
		chain = (VkLayerDeviceCreateInfo *)chain->pNext;
	if (!chain)
		return VK_ERROR_INITIALIZATION_FAILED;
	PFN_vkGetInstanceProcAddr next_gipa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
	PFN_vkGetDeviceProcAddr next_gdpa = chain->u.pLayerInfo->pfnNextGetDeviceProcAddr;
	chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;
	VkInstance instance = VK_NULL_HANDLE;
	{
		// A physical device shares its instance's dispatch key.
		std::lock_guard<std::mutex> lock(s_layer_mutex);
		const DxrLayerInstance *i = instance_for_locked(pd);
		if (i)
			instance = i->instance;
	}
	PFN_vkCreateDevice next_create = (PFN_vkCreateDevice)next_gipa(instance, "vkCreateDevice");
	if (!next_create)
		return VK_ERROR_INITIALIZATION_FAILED;
	VkResult r = next_create(pd, ci, alloc, out);
	if (r == VK_SUCCESS) {
		std::lock_guard<std::mutex> lock(s_layer_mutex);
		bool stored = false;
		for (auto &d : s_devices)
			if (!d.key) {
				d = DxrLayerDevice{dispatch_key(*out), instance, pd, next_gipa, next_gdpa};
				stored = true;
				break;
			}
		if (!stored)
			fprintf(stderr, "[DisplayXR-WL] layer: device table full; this device is not tracked\n");
	}
	return r;
}

static VKAPI_ATTR void VKAPI_CALL
layer_DestroyDevice(VkDevice device, const VkAllocationCallbacks *alloc)
{
	void *key = dispatch_key(device); // before the device is freed
	DxrLayerDevice d = {};
	if (device_for(device, &d))
		((PFN_vkDestroyDevice)d.next_gdpa(device, "vkDestroyDevice"))(device, alloc);
	std::lock_guard<std::mutex> lock(s_layer_mutex);
	for (auto &e : s_devices)
		if (e.key == key)
			e = DxrLayerDevice{};
}

static VKAPI_ATTR VkResult VKAPI_CALL
layer_CreateWaylandSurfaceKHR(VkInstance instance, const DxrVkWaylandSurfaceCreateInfo *ci,
                              const VkAllocationCallbacks *alloc, VkSurfaceKHR *out)
{
	PFN_vkGetInstanceProcAddr gipa = next_gipa_for(instance);
	PFN_dxrCreateWaylandSurface next =
	    gipa ? (PFN_dxrCreateWaylandSurface)gipa(instance, "vkCreateWaylandSurfaceKHR") : nullptr;
	if (!next)
		return VK_ERROR_EXTENSION_NOT_PRESENT;
	VkResult r = next(instance, ci, alloc, out);
	if (r != VK_SUCCESS || !ci)
		return r;
	// Only the player's instance carries this layer, and the only surface it
	// presents to is its window.
	unsigned generation;
	{
		std::lock_guard<std::mutex> lock(s_player_mutex);
		s_player_display = ci->display;
		s_player_surface = ci->surface;
		generation = ++s_player_generation;
		for (VkSurfaceKHR &p : s_player_vk)
			if (p == VK_NULL_HANDLE) {
				p = *out;
				break;
			}
	}
	fprintf(stderr, "[DisplayXR-WL] the player's window surface #%u: wl_surface=%p\n", generation,
	        (void *)ci->surface);
	return r;
}

static VKAPI_ATTR void VKAPI_CALL
layer_DestroySurfaceKHR(VkInstance instance, VkSurfaceKHR surface, const VkAllocationCallbacks *alloc)
{
	{
		// Forget the window BEFORE it goes, so nobody sends requests to a
		// wl_surface the player is about to destroy; a new one is recorded when
		// the player makes its next surface.
		std::lock_guard<std::mutex> lock(s_player_mutex);
		int live = 0;
		for (VkSurfaceKHR &p : s_player_vk) {
			if (p == surface && surface != VK_NULL_HANDLE)
				p = VK_NULL_HANDLE;
			live += p != VK_NULL_HANDLE;
		}
		if (!live)
			s_player_surface = nullptr;
	}
	PFN_vkGetInstanceProcAddr gipa = next_gipa_for(instance);
	PFN_vkDestroySurfaceKHR next = gipa ? (PFN_vkDestroySurfaceKHR)gipa(instance, "vkDestroySurfaceKHR") : nullptr;
	if (next)
		next(instance, surface, alloc);
}

static VKAPI_ATTR VkResult VKAPI_CALL
layer_CreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR *ci, const VkAllocationCallbacks *alloc,
                         VkSwapchainKHR *out)
{
	DxrLayerDevice d = {};
	if (!device_for(device, &d))
		return VK_ERROR_INITIALIZATION_FAILED;
	PFN_vkCreateSwapchainKHR next = (PFN_vkCreateSwapchainKHR)d.next_gdpa(device, "vkCreateSwapchainKHR");
	if (!next)
		return VK_ERROR_INITIALIZATION_FAILED;
	int player;
	{
		std::lock_guard<std::mutex> lock(s_player_mutex);
		player = ci && player_vk_is_locked(ci->surface);
	}
	if (!player)
		return next(device, ci, alloc, out);

	// Transparent avatar: let the compositor honour the window's alpha (the player
	// asks for OPAQUE), so what the player leaves at alpha 0 shows the desktop.
	// Only where the surface supports it.
	VkSwapchainCreateInfoKHR alpha_ci = *ci;
	if (ci->compositeAlpha != VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR && d.instance) {
		PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR caps_fn =
		    (PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)d.next_gipa(
		        d.instance, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
		VkSurfaceCapabilitiesKHR caps = {};
		if (caps_fn && caps_fn(d.physical_device, ci->surface, &caps) == VK_SUCCESS &&
		    (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR))
			alpha_ci.compositeAlpha = VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;
	}
	VkResult r = next(device, &alpha_ci, alloc, out);
	if (r == VK_SUCCESS) {
		bool changed;
		{
			std::lock_guard<std::mutex> lock(s_player_mutex);
			changed = s_player_w != ci->imageExtent.width || s_player_h != ci->imageExtent.height;
			s_player_w = ci->imageExtent.width;
			s_player_h = ci->imageExtent.height;
		}
		if (changed)
			fprintf(stderr, "[DisplayXR-WL] the player's window: %ux%u logical, %s\n", ci->imageExtent.width,
			        ci->imageExtent.height,
			        alpha_ci.compositeAlpha == VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR ? "alpha PRE_MULTIPLIED"
			                                                                             : "alpha as requested");
	}
	return r;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL layer_GetInstanceProcAddr(VkInstance instance, const char *name);

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
layer_GetDeviceProcAddr(VkDevice device, const char *name)
{
	if (!strcmp(name, "vkGetDeviceProcAddr"))
		return (PFN_vkVoidFunction)layer_GetDeviceProcAddr;
	if (!strcmp(name, "vkDestroyDevice"))
		return (PFN_vkVoidFunction)layer_DestroyDevice;
	if (!strcmp(name, "vkCreateSwapchainKHR"))
		return (PFN_vkVoidFunction)layer_CreateSwapchainKHR;
	DxrLayerDevice d = {};
	return device_for(device, &d) ? d.next_gdpa(device, name) : nullptr;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
layer_GetInstanceProcAddr(VkInstance instance, const char *name)
{
	if (!strcmp(name, "vkGetInstanceProcAddr"))
		return (PFN_vkVoidFunction)layer_GetInstanceProcAddr;
	if (!strcmp(name, "vkCreateInstance"))
		return (PFN_vkVoidFunction)layer_CreateInstance;
	if (!strcmp(name, "vkDestroyInstance"))
		return (PFN_vkVoidFunction)layer_DestroyInstance;
	if (!strcmp(name, "vkCreateDevice"))
		return (PFN_vkVoidFunction)layer_CreateDevice;
	if (!strcmp(name, "vkGetDeviceProcAddr"))
		return (PFN_vkVoidFunction)layer_GetDeviceProcAddr;
	if (!strcmp(name, "vkDestroyDevice"))
		return (PFN_vkVoidFunction)layer_DestroyDevice;
	if (!strcmp(name, "vkCreateWaylandSurfaceKHR"))
		return (PFN_vkVoidFunction)layer_CreateWaylandSurfaceKHR;
	if (!strcmp(name, "vkDestroySurfaceKHR"))
		return (PFN_vkVoidFunction)layer_DestroySurfaceKHR;
	if (instance == VK_NULL_HANDLE)
		return nullptr;
	PFN_vkGetInstanceProcAddr gipa = next_gipa_for(instance);
	return gipa ? gipa(instance, name) : nullptr;
}

DXR_WLCAP_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
dxr_wlcap_NegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *v)
{
	if (!v || v->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT)
		return VK_ERROR_INITIALIZATION_FAILED;
	if (v->loaderLayerInterfaceVersion > 2)
		v->loaderLayerInterfaceVersion = 2;
	v->pfnGetInstanceProcAddr = layer_GetInstanceProcAddr;
	v->pfnGetDeviceProcAddr = layer_GetDeviceProcAddr;
	v->pfnGetPhysicalDeviceProcAddr = nullptr;
	return VK_SUCCESS;
}

/*
 *
 * Arming from XRSDKPreInit, and disarming once the player's instance exists.
 *
 */

static char s_manifest_dir[PATH_MAX];
static char s_manifest_path[PATH_MAX + 32];
static char *s_prev_layer_path; // VK_ADD_IMPLICIT_LAYER_PATH before we armed (malloc'd)
static int s_armed;

static bool
started_with_force_wayland(void)
{
	FILE *f = fopen("/proc/self/cmdline", "rb");
	if (!f)
		return false;
	char buf[8192];
	size_t n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[n] = 0;
	for (size_t i = 0; i < n; i += strlen(buf + i) + 1)
		if (!strcmp(buf + i, "-force-wayland"))
			return true;
	return false;
}

static void
layer_disarm(void)
{
	if (!s_armed)
		return;
	s_armed = 0;
	if (s_prev_layer_path) {
		setenv("VK_ADD_IMPLICIT_LAYER_PATH", s_prev_layer_path, 1);
		free(s_prev_layer_path);
		s_prev_layer_path = nullptr;
	} else {
		unsetenv("VK_ADD_IMPLICIT_LAYER_PATH");
	}
	unlink(s_manifest_path);
	rmdir(s_manifest_dir);
}

extern "C" void
dxr_wl_capture_install(void)
{
	if (s_armed || !started_with_force_wayland())
		return;
	Dl_info self;
	if (!dladdr((void *)&dxr_wl_capture_install, &self) || !self.dli_fname) {
		fprintf(stderr, "[DisplayXR-WL] cannot locate this library; Wayland window capture off\n");
		return;
	}
	const char *rt = getenv("XDG_RUNTIME_DIR");
	snprintf(s_manifest_dir, sizeof(s_manifest_dir), "%s/displayxr-wlcap-XXXXXX", rt && *rt ? rt : "/tmp");
	if (!mkdtemp(s_manifest_dir)) {
		fprintf(stderr, "[DisplayXR-WL] cannot create the layer manifest dir; Wayland window capture off\n");
		return;
	}
	snprintf(s_manifest_path, sizeof(s_manifest_path), "%s/displayxr_wlcap.json", s_manifest_dir);
	FILE *f = fopen(s_manifest_path, "wx");
	if (!f) {
		rmdir(s_manifest_dir);
		fprintf(stderr, "[DisplayXR-WL] cannot write the layer manifest; Wayland window capture off\n");
		return;
	}
	// The library path goes into JSON: escape what JSON requires.
	char lib[2 * PATH_MAX];
	size_t o = 0;
	for (const char *c = self.dli_fname; *c && o + 2 < sizeof(lib); c++) {
		if (*c == '"' || *c == '\\')
			lib[o++] = '\\';
		lib[o++] = *c;
	}
	lib[o] = 0;
	fprintf(f,
	        "{\n"
	        "  \"file_format_version\": \"1.1.2\",\n"
	        "  \"layer\": {\n"
	        "    \"name\": \"VK_LAYER_DXR_unity_wayland_capture\",\n"
	        "    \"type\": \"GLOBAL\",\n"
	        "    \"library_path\": \"%s\",\n"
	        "    \"api_version\": \"1.3.0\",\n"
	        "    \"implementation_version\": \"1\",\n"
	        "    \"description\": \"DisplayXR: finds the Unity player's Wayland window\",\n"
	        "    \"functions\": {\n"
	        "      \"vkNegotiateLoaderLayerInterfaceVersion\": \"dxr_wlcap_NegotiateLoaderLayerInterfaceVersion\"\n"
	        "    },\n"
	        "    \"disable_environment\": { \"DXR_WL_CAPTURE_DISABLE\": \"1\" }\n"
	        "  }\n"
	        "}\n",
	        lib);
	fclose(f);
	const char *prev = getenv("VK_ADD_IMPLICIT_LAYER_PATH");
	s_prev_layer_path = prev ? strdup(prev) : nullptr;
	char paths[2 * PATH_MAX];
	if (prev && *prev)
		snprintf(paths, sizeof(paths), "%s:%s", s_manifest_dir, prev);
	else
		snprintf(paths, sizeof(paths), "%s", s_manifest_dir);
	setenv("VK_ADD_IMPLICIT_LAYER_PATH", paths, 1);
	s_armed = 1;
	fprintf(stderr, "[DisplayXR-WL] native-Wayland player: window capture layer armed\n");
}

#elif defined(__linux__) && !defined(__ANDROID__)

// Built without the Vulkan backend: no capture layer, so never a native-Wayland
// player window. The accessors still exist for the shim and the provider.

#include "../displayxr_linux_wayland.h"

#include <stddef.h>

extern "C" void
dxr_wl_capture_install(void)
{
}

extern "C" int
dxr_wl_unity_surface(struct wl_display **out_display, struct wl_surface **out_surface)
{
	if (out_display)
		*out_display = NULL;
	if (out_surface)
		*out_surface = NULL;
	return 0;
}

extern "C" int
dxr_wl_unity_swapchain_size(int *out_w, int *out_h)
{
	if (out_w)
		*out_w = 0;
	if (out_h)
		*out_h = 0;
	return 0;
}

extern "C" unsigned
dxr_wl_player_surface_generation(void)
{
	return 0;
}

#endif
