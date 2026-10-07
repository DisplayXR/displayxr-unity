// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Native-Wayland Unity player (EXPERIMENT): capture the player's own wl_display /
// wl_surface.
//
// Under `-force-wayland` the player has no X11 window, so displayxr_linux.c finds
// nothing to bind and the runtime opens a hosted window of its own: two windows.
// To weave into the player's window instead, the plugin needs the player's
// Wayland handles, and Unity exposes none (its SDL2 is built in and exports no
// SDL_* symbols).
//
// Why a Vulkan layer. The player's window surface is created by its SDL, which
// resolves vkCreateWaylandSurfaceKHR from the Vulkan loader itself, so Unity's
// own Vulkan hooks never see the call (IUnityGraphicsVulkan's InterceptVulkanAPI
// / AddInterceptInitialization only reach the dispatch table Unity built at its
// FIRST Vulkan init, which happens before any plug-in code runs). A layer sits
// in the loader, under every caller. The player creates its real VkInstance
// AFTER XRSDKPreInit (a second instance, after device detection), so pre-init
// writes a layer manifest and points the loader at it with
// VK_ADD_IMPLICIT_LAYER_PATH; that instance then carries this layer, and the
// layer records the create-info of vkCreateWaylandSurfaceKHR: exactly the
// wl_display + wl_surface pair.
//
// The layer changes nothing: every call is forwarded as is. It is only armed when
// the player was started with -force-wayland.

#if defined(__linux__) && !defined(__ANDROID__) && defined(ENABLE_VULKAN)

#include "../displayxr_vk_loader.h"
#include "../unity_pluginapi/IUnityInterface.h"

#include <vulkan/vk_layer.h>

#include <dlfcn.h>
#include <limits.h>
#include <mutex>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define DXR_WLCAP_EXPORT extern "C" __attribute__((visibility("default")))

struct wl_display;
struct wl_surface;

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

static struct wl_display *s_wl_display = nullptr;
static struct wl_surface *s_wl_surface = nullptr;
static VkSurfaceKHR s_vk_surface = VK_NULL_HANDLE; // the player's window surface
static uint32_t s_swap_w = 0, s_swap_h = 0;        // its latest swapchain extent

extern "C" int dxr_wl_test_subsurface(struct wl_display *display, struct wl_surface *parent, int width, int height);

/*
 *
 * The layer: the minimum chaining a layer must do, plus one recording hook.
 *
 */

// The loader's dispatch-table pointer is the first word of every dispatchable
// handle, and is shared by an instance's/device's children: the standard key.
static inline void *
dispatch_key(const void *handle)
{
	return *(void *const *)handle;
}

struct DxrLayerInstance {
	void *key;
	PFN_vkGetInstanceProcAddr next_gipa;
};
struct DxrLayerDevice {
	void *key;
	PFN_vkGetDeviceProcAddr next_gdpa;
};

static std::mutex s_layer_mutex;
static DxrLayerInstance s_instances[16];
static DxrLayerDevice s_devices[16];

static PFN_vkGetInstanceProcAddr
next_gipa_for(const void *dispatchable)
{
	std::lock_guard<std::mutex> lock(s_layer_mutex);
	void *key = dispatch_key(dispatchable);
	for (auto &i : s_instances)
		if (i.key == key)
			return i.next_gipa;
	return nullptr;
}

static PFN_vkGetDeviceProcAddr
next_gdpa_for(VkDevice device)
{
	std::lock_guard<std::mutex> lock(s_layer_mutex);
	void *key = dispatch_key(device);
	for (auto &d : s_devices)
		if (d.key == key)
			return d.next_gdpa;
	return nullptr;
}

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
		for (auto &i : s_instances)
			if (!i.key) {
				i.key = dispatch_key(*out);
				i.next_gipa = next_gipa;
				break;
			}
	}
	return r;
}

static VKAPI_ATTR void VKAPI_CALL
layer_DestroyInstance(VkInstance instance, const VkAllocationCallbacks *alloc)
{
	PFN_vkGetInstanceProcAddr gipa = next_gipa_for(instance);
	if (gipa)
		((PFN_vkDestroyInstance)gipa(instance, "vkDestroyInstance"))(instance, alloc);
	std::lock_guard<std::mutex> lock(s_layer_mutex);
	void *key = dispatch_key(instance);
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
	PFN_vkCreateDevice next_create = (PFN_vkCreateDevice)next_gipa(VK_NULL_HANDLE, "vkCreateDevice");
	VkResult r = next_create(pd, ci, alloc, out);
	if (r == VK_SUCCESS) {
		std::lock_guard<std::mutex> lock(s_layer_mutex);
		for (auto &d : s_devices)
			if (!d.key) {
				d.key = dispatch_key(*out);
				d.next_gdpa = next_gdpa;
				break;
			}
	}
	return r;
}

static VKAPI_ATTR void VKAPI_CALL
layer_DestroyDevice(VkDevice device, const VkAllocationCallbacks *alloc)
{
	PFN_vkGetDeviceProcAddr gdpa = next_gdpa_for(device);
	if (gdpa)
		((PFN_vkDestroyDevice)gdpa(device, "vkDestroyDevice"))(device, alloc);
	std::lock_guard<std::mutex> lock(s_layer_mutex);
	void *key = dispatch_key(device);
	for (auto &d : s_devices)
		if (d.key == key)
			d = DxrLayerDevice{};
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
	if (r == VK_SUCCESS && ci && !s_wl_surface) {
		// The first Wayland surface in the process is the player's window: the
		// runtime's own surfaces are only made later, for a session.
		s_wl_display = ci->display;
		s_wl_surface = ci->surface;
		s_vk_surface = *out;
		// Unity loads this library twice (Plugins/ for pre-init, Plugins/x86_64/ as
		// the XR plug-in): separate modules, separate globals. Publish the handles in
		// the process environment too, so whichever copy runs the provider sees them.
		char v[64];
		snprintf(v, sizeof(v), "%p %p", (void *)ci->display, (void *)ci->surface);
		setenv("DXR_WL_PLAYER_SURFACE", v, 1);
		fprintf(stderr, "[DisplayXR-WL] the player's window surface: wl_display=%p wl_surface=%p "
		        "(VkSurfaceKHR 0x%llx)\n", (void *)ci->display, (void *)ci->surface,
		        (unsigned long long)*out);
		fflush(stderr);
	}
	return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL
layer_CreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR *ci, const VkAllocationCallbacks *alloc,
                         VkSwapchainKHR *out)
{
	PFN_vkGetDeviceProcAddr gdpa = next_gdpa_for(device);
	PFN_vkCreateSwapchainKHR next = gdpa ? (PFN_vkCreateSwapchainKHR)gdpa(device, "vkCreateSwapchainKHR") : nullptr;
	if (!next)
		return VK_ERROR_INITIALIZATION_FAILED;
	VkResult r = next(device, ci, alloc, out);
	if (r == VK_SUCCESS && ci && s_vk_surface != VK_NULL_HANDLE && ci->surface == s_vk_surface) {
		s_swap_w = ci->imageExtent.width;
		s_swap_h = ci->imageExtent.height;
		char v[32];
		snprintf(v, sizeof(v), "%u %u", s_swap_w, s_swap_h);
		setenv("DXR_WL_PLAYER_SIZE", v, 1);
		fprintf(stderr, "[DisplayXR-WL] the player's window swapchain: %ux%u\n", s_swap_w, s_swap_h);
		fflush(stderr);
		const char *test = getenv("DXR_WL_TEST_SUBSURFACE");
		if (test && *test == '1')
			dxr_wl_test_subsurface(s_wl_display, s_wl_surface, (int)s_swap_w, (int)s_swap_h);
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
	PFN_vkGetDeviceProcAddr gdpa = next_gdpa_for(device);
	return gdpa ? gdpa(device, name) : nullptr;
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
 * Arming it from XRSDKPreInit.
 *
 */

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

//! The copy of this library the player loads as its XR plug-in: Unity loads the
//! pre-init library from Plugins/ and the plug-in from Plugins/x86_64/, two
//! separate modules, so the layer must live in the one the provider will ask.
static bool
plugin_module_path(char *out, size_t cap)
{
	Dl_info info;
	if (!dladdr((void *)&plugin_module_path, &info) || !info.dli_fname)
		return false;
	char dir[PATH_MAX];
	snprintf(dir, sizeof(dir), "%s", info.dli_fname);
	char *slash = strrchr(dir, '/');
	if (!slash)
		return false;
	*slash = 0;
	const char *base = slash + 1;
	char arch[PATH_MAX];
	snprintf(arch, sizeof(arch), "%s/x86_64/%s", dir, base);
	struct stat st;
	snprintf(out, cap, "%s", stat(arch, &st) == 0 ? arch : info.dli_fname);
	return true;
}

extern "C" void
dxr_wl_capture_install(IUnityInterfaces *ifaces, const char *when)
{
	(void)ifaces;
	if (!started_with_force_wayland())
		return;
	char lib[PATH_MAX];
	if (!plugin_module_path(lib, sizeof(lib))) {
		fprintf(stderr, "[DisplayXR-WL] %s: cannot locate the plug-in library; capture off\n", when);
		return;
	}
	const char *rt = getenv("XDG_RUNTIME_DIR");
	char dir[PATH_MAX];
	snprintf(dir, sizeof(dir), "%s/displayxr-wlcap-%d", rt && *rt ? rt : "/tmp", (int)getpid());
	mkdir(dir, 0700);
	char manifest[PATH_MAX + 32];
	snprintf(manifest, sizeof(manifest), "%s/displayxr_wlcap.json", dir);
	FILE *f = fopen(manifest, "w");
	if (!f) {
		fprintf(stderr, "[DisplayXR-WL] %s: cannot write %s; capture off\n", when, manifest);
		return;
	}
	fprintf(f,
	        "{\n"
	        "  \"file_format_version\": \"1.1.2\",\n"
	        "  \"layer\": {\n"
	        "    \"name\": \"VK_LAYER_DXR_unity_wayland_capture\",\n"
	        "    \"type\": \"GLOBAL\",\n"
	        "    \"library_path\": \"%s\",\n"
	        "    \"api_version\": \"1.3.0\",\n"
	        "    \"implementation_version\": \"1\",\n"
	        "    \"description\": \"DisplayXR: records the Unity player's Wayland window surface\",\n"
	        "    \"functions\": {\n"
	        "      \"vkNegotiateLoaderLayerInterfaceVersion\": \"dxr_wlcap_NegotiateLoaderLayerInterfaceVersion\"\n"
	        "    },\n"
	        "    \"disable_environment\": { \"DXR_WL_CAPTURE_DISABLE\": \"1\" }\n"
	        "  }\n"
	        "}\n",
	        lib);
	fclose(f);
	const char *prev = getenv("VK_ADD_IMPLICIT_LAYER_PATH");
	char paths[2 * PATH_MAX];
	if (prev && *prev)
		snprintf(paths, sizeof(paths), "%s:%s", dir, prev);
	else
		snprintf(paths, sizeof(paths), "%s", dir);
	setenv("VK_ADD_IMPLICIT_LAYER_PATH", paths, 1);
	fprintf(stderr, "[DisplayXR-WL] %s: Wayland surface capture layer armed (%s -> %s)\n", when, manifest, lib);
	fflush(stderr);
}

//! The player's window size in logical px (its swapchain extent; the player
//! draws at buffer scale 1). Returns 1 once known.
extern "C" int
dxr_wl_unity_swapchain_size(int *out_w, int *out_h)
{
	unsigned w = s_swap_w, h = s_swap_h;
	const char *v = getenv("DXR_WL_PLAYER_SIZE"); // set by whichever copy saw it (see above)
	if (v)
		sscanf(v, "%u %u", &w, &h);
	if (out_w)
		*out_w = (int)w;
	if (out_h)
		*out_h = (int)h;
	return w && h ? 1 : 0;
}

//! The player's own Wayland handles, once its window surface was created.
//! Returns 1 when both are known.
extern "C" int
dxr_wl_unity_surface(struct wl_display **out_display, struct wl_surface **out_surface)
{
	void *d = s_wl_display, *s = s_wl_surface;
	const char *v = getenv("DXR_WL_PLAYER_SURFACE"); // set by whichever copy saw it (see above)
	if (!s && v)
		sscanf(v, "%p %p", &d, &s);
	if (out_display)
		*out_display = (struct wl_display *)d;
	if (out_surface)
		*out_surface = (struct wl_surface *)s;
	return d && s ? 1 : 0;
}

#endif
