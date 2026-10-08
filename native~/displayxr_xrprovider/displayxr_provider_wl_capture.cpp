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
// so pre-init writes a layer manifest and enables it as an EXPLICIT layer through
// the environment (VK_LAYER_PATH + VK_INSTANCE_LAYERS: every Vulkan loader has
// them; VK_ADD_IMPLICIT_LAYER_PATH only exists from loader 1.3.296, newer than
// Ubuntu 22.04's and 24.04's). That instance carries the layer. The variables and
// the manifest are restored/removed once Unity's graphics device exists (and at
// the latest when the XR session starts), so the runtime's instance never loads
// the layer; and its hooks only act in the process that armed it, so a child
// process that inherited the environment in that window gets a pass-through.
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

#include <atomic>
#include <dlfcn.h>
#include <limits.h>
#include <mutex>
#include <string>
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

// The next functions down the chain for what we hook, resolved ONCE, at
// vkCreateInstance / vkCreateDevice, as the layer interface intends. Never at
// call time: an older loader (Ubuntu 22.04's 1.3.204) hands the last layer a
// next-gipa that answers from the instance's dispatch table, which after creation
// holds the TOP of the chain - this layer - so asking it then for a function we
// hook returns our own hook, and the call recurses until the stack overflows.
struct DxrLayerInstance {
	void *key;
	VkInstance instance;
	PFN_vkGetInstanceProcAddr next_gipa;
	PFN_vkDestroyInstance destroy_instance;
	PFN_dxrCreateWaylandSurface create_wayland_surface;
	PFN_vkDestroySurfaceKHR destroy_surface;
	PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR surface_caps;
};
struct DxrLayerDevice {
	void *key;
	VkPhysicalDevice physical_device;
	PFN_vkGetDeviceProcAddr next_gdpa;
	PFN_vkDestroyDevice destroy_device;
	PFN_vkCreateSwapchainKHR create_swapchain;
	PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR surface_caps;
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

//! The instance an instance or physical device belongs to (a copy). 1 if known.
static int
instance_for(const void *dispatchable, DxrLayerInstance *out)
{
	std::lock_guard<std::mutex> lock(s_layer_mutex);
	const DxrLayerInstance *i = instance_for_locked(dispatchable);
	if (i)
		*out = *i;
	return i != nullptr;
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

static std::atomic<int> s_installed{0}; // armed in THIS process (below): the hooks act only then

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
		DxrLayerInstance e = {};
		e.key = dispatch_key(*out);
		e.instance = *out;
		e.next_gipa = next_gipa;
		e.destroy_instance = (PFN_vkDestroyInstance)next_gipa(*out, "vkDestroyInstance");
		e.create_wayland_surface = (PFN_dxrCreateWaylandSurface)next_gipa(*out, "vkCreateWaylandSurfaceKHR");
		e.destroy_surface = (PFN_vkDestroySurfaceKHR)next_gipa(*out, "vkDestroySurfaceKHR");
		e.surface_caps = (PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)next_gipa(
		    *out, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
		std::lock_guard<std::mutex> lock(s_layer_mutex);
		bool stored = false;
		for (auto &i : s_instances)
			if (!i.key) {
				i = e;
				stored = true;
				break;
			}
		if (!stored)
			fprintf(stderr, "[DisplayXR-WL] layer: instance table full; this instance is not tracked\n");
		fprintf(stderr, "[DisplayXR-WL] capture layer is in VkInstance %p%s\n", (void *)*out,
		        s_installed ? "" : " (not armed in this process: pass-through)");
	}
	return r;
}

static VKAPI_ATTR void VKAPI_CALL
layer_DestroyInstance(VkInstance instance, const VkAllocationCallbacks *alloc)
{
	void *key = dispatch_key(instance); // before the instance is freed
	DxrLayerInstance e = {};
	if (instance_for(instance, &e) && e.destroy_instance)
		e.destroy_instance(instance, alloc);
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
	DxrLayerInstance inst = {}; // a physical device shares its instance's dispatch key
	instance_for(pd, &inst);
	PFN_vkCreateDevice next_create = (PFN_vkCreateDevice)next_gipa(inst.instance, "vkCreateDevice");
	if (!next_create)
		return VK_ERROR_INITIALIZATION_FAILED;
	VkResult r = next_create(pd, ci, alloc, out);
	if (r == VK_SUCCESS) {
		DxrLayerDevice e = {};
		e.key = dispatch_key(*out);
		e.physical_device = pd;
		e.next_gdpa = next_gdpa;
		e.destroy_device = (PFN_vkDestroyDevice)next_gdpa(*out, "vkDestroyDevice");
		e.create_swapchain = (PFN_vkCreateSwapchainKHR)next_gdpa(*out, "vkCreateSwapchainKHR");
		e.surface_caps = inst.surface_caps;
		std::lock_guard<std::mutex> lock(s_layer_mutex);
		bool stored = false;
		for (auto &d : s_devices)
			if (!d.key) {
				d = e;
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
	if (device_for(device, &d) && d.destroy_device)
		d.destroy_device(device, alloc);
	std::lock_guard<std::mutex> lock(s_layer_mutex);
	for (auto &e : s_devices)
		if (e.key == key)
			e = DxrLayerDevice{};
}

static VKAPI_ATTR VkResult VKAPI_CALL
layer_CreateWaylandSurfaceKHR(VkInstance instance, const DxrVkWaylandSurfaceCreateInfo *ci,
                              const VkAllocationCallbacks *alloc, VkSurfaceKHR *out)
{
	DxrLayerInstance e = {};
	if (!instance_for(instance, &e) || !e.create_wayland_surface)
		return VK_ERROR_EXTENSION_NOT_PRESENT;
	VkResult r = e.create_wayland_surface(instance, ci, alloc, out);
	if (r != VK_SUCCESS || !ci || !s_installed)
		return r; // not ours to record (e.g. a child process that inherited the layer)
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
	DxrLayerInstance e = {};
	if (instance_for(instance, &e) && e.destroy_surface)
		e.destroy_surface(instance, surface, alloc);
}

static VKAPI_ATTR VkResult VKAPI_CALL
layer_CreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR *ci, const VkAllocationCallbacks *alloc,
                         VkSwapchainKHR *out)
{
	DxrLayerDevice d = {};
	if (!device_for(device, &d) || !d.create_swapchain)
		return VK_ERROR_INITIALIZATION_FAILED;
	PFN_vkCreateSwapchainKHR next = d.create_swapchain;
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
	if (ci->compositeAlpha != VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR && d.surface_caps) {
		VkSurfaceCapabilitiesKHR caps = {};
		if (d.surface_caps(d.physical_device, ci->surface, &caps) == VK_SUCCESS &&
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
	// Everything else passes straight through to the next gipa, as layers do. (The
	// call-time recursion above only bites for a function we hook ourselves.)
	DxrLayerInstance e = {};
	return instance_for(instance, &e) ? e.next_gipa(instance, name) : nullptr;
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
 * Arming from XRSDKPreInit, disarming once Unity's graphics device exists.
 *
 */

#define DXR_WLCAP_LAYER_NAME "VK_LAYER_DXR_unity_wayland_capture"

static std::mutex s_arm_mutex;
static char s_manifest_dir[PATH_MAX];
static char s_manifest_path[PATH_MAX + 32];
// The variables as they were before we armed (malloc'd; NULL = unset).
static char *s_prev_layer_path;
static char *s_prev_instance_layers;
static int s_armed; // the environment points the loader at the layer right now

static bool
started_with_force_wayland(void)
{
	static int cached = -1;
	if (cached >= 0)
		return cached != 0;
	int found = 0;
	FILE *f = fopen("/proc/self/cmdline", "rb");
	if (f) {
		char buf[8192];
		size_t n = fread(buf, 1, sizeof(buf) - 1, f);
		fclose(f);
		buf[n] = 0;
		for (size_t i = 0; i < n && !found; i += strlen(buf + i) + 1)
			found = !strcmp(buf + i, "-force-wayland");
	}
	cached = found;
	return found != 0;
}

extern "C" int
dxr_wl_capture_wanted(void)
{
	const char *off = getenv("DXR_WL_CAPTURE_DISABLE");
	return started_with_force_wayland() && !(off && !strcmp(off, "1"));
}

//! The loader's default explicit-layer folders: VK_LAYER_PATH REPLACES them, so
//! they go after ours, or an explicit layer Unity asks for (validation) would
//! not be found while we are armed.
static std::string
default_explicit_layer_dirs(void)
{
	const char *home = getenv("HOME");
	auto dir_or = [&](const char *var, const char *home_rel) -> std::string {
		const char *v = getenv(var);
		if (v && *v)
			return v;
		return home && *home ? std::string(home) + home_rel : std::string();
	};
	auto list_or = [](const char *var, const char *dflt) -> std::string {
		const char *v = getenv(var);
		return v && *v ? v : dflt;
	};
	std::string out;
	auto add = [&](const std::string &base) {
		if (base.empty())
			return;
		if (!out.empty())
			out += ':';
		out += base + "/vulkan/explicit_layer.d";
	};
	auto add_list = [&](const std::string &list) {
		size_t start = 0;
		while (start <= list.size()) {
			size_t end = list.find(':', start);
			if (end == std::string::npos)
				end = list.size();
			add(list.substr(start, end - start));
			start = end + 1;
		}
	};
	add(dir_or("XDG_CONFIG_HOME", "/.config"));
	add_list(list_or("XDG_CONFIG_DIRS", "/etc/xdg"));
	add("/etc");
	add(dir_or("XDG_DATA_HOME", "/.local/share"));
	add_list(list_or("XDG_DATA_DIRS", "/usr/local/share:/usr/share"));
	return out;
}

static void
restore_env(const char *name, char **prev)
{
	if (*prev) {
		setenv(name, *prev, 1);
		free(*prev);
		*prev = nullptr;
	} else {
		unsetenv(name);
	}
}

extern "C" void
dxr_wl_capture_disarm(const char *why)
{
	std::lock_guard<std::mutex> lock(s_arm_mutex);
	if (!s_armed)
		return;
	s_armed = 0;
	restore_env("VK_LAYER_PATH", &s_prev_layer_path);
	restore_env("VK_INSTANCE_LAYERS", &s_prev_instance_layers);
	unlink(s_manifest_path);
	rmdir(s_manifest_dir);
	fprintf(stderr, "[DisplayXR-WL] window capture layer disarmed (%s)\n", why ? why : "?");
}

extern "C" void
dxr_wl_capture_install(void)
{
	std::lock_guard<std::mutex> lock(s_arm_mutex);
	if (s_installed || !dxr_wl_capture_wanted())
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
	        "    \"name\": \"" DXR_WLCAP_LAYER_NAME "\",\n"
	        "    \"type\": \"GLOBAL\",\n"
	        "    \"library_path\": \"%s\",\n"
	        "    \"api_version\": \"1.3.0\",\n"
	        "    \"implementation_version\": \"1\",\n"
	        "    \"description\": \"DisplayXR: finds the Unity player's Wayland window\",\n"
	        "    \"functions\": {\n"
	        "      \"vkNegotiateLoaderLayerInterfaceVersion\": \"dxr_wlcap_NegotiateLoaderLayerInterfaceVersion\"\n"
	        "    }\n"
	        "  }\n"
	        "}\n",
	        lib);
	fclose(f);

	const char *prev_path = getenv("VK_LAYER_PATH");
	const char *prev_layers = getenv("VK_INSTANCE_LAYERS");
	s_prev_layer_path = prev_path ? strdup(prev_path) : nullptr;
	s_prev_instance_layers = prev_layers ? strdup(prev_layers) : nullptr;
	std::string path = std::string(s_manifest_dir) + ":" +
	                   (prev_path && *prev_path ? std::string(prev_path) : default_explicit_layer_dirs());
	std::string layers = std::string(DXR_WLCAP_LAYER_NAME) +
	                     (prev_layers && *prev_layers ? ":" + std::string(prev_layers) : std::string());
	setenv("VK_LAYER_PATH", path.c_str(), 1);
	setenv("VK_INSTANCE_LAYERS", layers.c_str(), 1);
	s_armed = 1;
	s_installed = 1;
	fprintf(stderr, "[DisplayXR-WL] native-Wayland player: window capture layer armed (%s)\n", self.dli_fname);
}

extern "C" void
dxr_wl_capture_note_caller(const char *what)
{
	// Unity loads this library twice (Plugins/ and Plugins/x86_64/), as separate
	// modules. Pre-init, the provider and the C# P/Invokes have always resolved to
	// the same one; if a P/Invoke ever lands in the other copy, it sees no Wayland
	// window (ui_scale 1, no resizes) without any error. Say so, once.
	static std::atomic<bool> s_checked{false};
	if (s_checked.exchange(true) || s_installed || !dxr_wl_capture_wanted())
		return;
	Dl_info self;
	fprintf(stderr,
	        "[DisplayXR-WL] WARN: %s resolved to a copy of the plugin that did not arm the window capture (%s): "
	        "Unity loaded the plugin twice and this copy sees no Wayland window\n",
	        what ? what : "a call", dladdr((void *)&dxr_wl_capture_note_caller, &self) && self.dli_fname ? self.dli_fname : "?");
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

extern "C" void
dxr_wl_capture_disarm(const char *why)
{
	(void)why;
}

extern "C" int
dxr_wl_capture_wanted(void)
{
	return 0;
}

extern "C" void
dxr_wl_capture_note_caller(const char *what)
{
	(void)what;
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
