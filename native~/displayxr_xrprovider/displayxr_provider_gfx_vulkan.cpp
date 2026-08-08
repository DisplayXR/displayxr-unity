// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Vulkan graphics glue for the IUnityXRDisplay provider (#247).
// See displayxr_provider_gfx_vulkan.h for why this is an own-device bridge.

#if defined(_WIN32) && defined(ENABLE_VULKAN)

#include "displayxr_provider_gfx_vulkan.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// The provider's shared logger (stderr + OutputDebugString + %TEMP% file).
extern "C" void dxr_prov_file_log(const char *s);

static void
pvk_log(const char *fmt, ...)
{
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	fputs(buf, stderr);
	OutputDebugStringA(buf);
	dxr_prov_file_log(buf);
}

// ---------------------------------------------------------------------------
// XR_KHR_vulkan_enable / _enable2 types, inlined.
//
// Same convention as the D3D11/D3D12 blocks in displayxr_provider_session.cpp:
// defining XR_USE_GRAPHICS_API_VULKAN would drag the platform header's Vulkan
// prototypes in, and we deliberately resolve every vk* entry point at runtime
// (VK_NO_PROTOTYPES) so the shipped DLL keeps no hard dependency on vulkan-1.dll.
// Values verified against Khronos openxr_platform.h.
// ---------------------------------------------------------------------------
#ifndef XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR
#define XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR       ((XrStructureType)1000025000)
#define XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR        ((XrStructureType)1000025001)
#define XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR  ((XrStructureType)1000025002)
#define XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR   ((XrStructureType)1000090000)
#define XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR     ((XrStructureType)1000090001)
#define XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR ((XrStructureType)1000090003)
#endif

typedef struct XrGraphicsBindingVulkanKHR {
	XrStructureType  type;
	const void      *next;
	VkInstance       instance;
	VkPhysicalDevice physicalDevice;
	VkDevice         device;
	uint32_t         queueFamilyIndex;
	uint32_t         queueIndex;
} XrGraphicsBindingVulkanKHR;

typedef struct XrSwapchainImageVulkanKHR {
	XrStructureType type;
	void           *next;
	VkImage         image;
} XrSwapchainImageVulkanKHR;

typedef struct XrGraphicsRequirementsVulkanKHR {
	XrStructureType type;
	void           *next;
	XrVersion       minApiVersionSupported;
	XrVersion       maxApiVersionSupported;
} XrGraphicsRequirementsVulkanKHR;

typedef struct XrVulkanInstanceCreateInfoKHR {
	XrStructureType              type;
	const void                  *next;
	XrSystemId                   systemId;
	XrFlags64                    createFlags;
	PFN_vkGetInstanceProcAddr    pfnGetInstanceProcAddr;
	const VkInstanceCreateInfo  *vulkanCreateInfo;
	const VkAllocationCallbacks *vulkanAllocator;
} XrVulkanInstanceCreateInfoKHR;

typedef struct XrVulkanDeviceCreateInfoKHR {
	XrStructureType              type;
	const void                  *next;
	XrSystemId                   systemId;
	XrFlags64                    createFlags;
	PFN_vkGetInstanceProcAddr    pfnGetInstanceProcAddr;
	VkPhysicalDevice             vulkanPhysicalDevice;
	const VkDeviceCreateInfo    *vulkanCreateInfo;
	const VkAllocationCallbacks *vulkanAllocator;
} XrVulkanDeviceCreateInfoKHR;

typedef struct XrVulkanGraphicsDeviceGetInfoKHR {
	XrStructureType type;
	const void     *next;
	XrSystemId      systemId;
	VkInstance      vulkanInstance;
} XrVulkanGraphicsDeviceGetInfoKHR;

typedef XrResult(XRAPI_PTR *PFN_xrGetVulkanGraphicsRequirements2KHR)(
    XrInstance, XrSystemId, XrGraphicsRequirementsVulkanKHR *);
typedef XrResult(XRAPI_PTR *PFN_xrCreateVulkanInstanceKHR)(
    XrInstance, const XrVulkanInstanceCreateInfoKHR *, VkInstance *, VkResult *);
typedef XrResult(XRAPI_PTR *PFN_xrCreateVulkanDeviceKHR)(
    XrInstance, const XrVulkanDeviceCreateInfoKHR *, VkDevice *, VkResult *);
typedef XrResult(XRAPI_PTR *PFN_xrGetVulkanGraphicsDevice2KHR)(
    XrInstance, const XrVulkanGraphicsDeviceGetInfoKHR *, VkPhysicalDevice *);

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

#define PVK_MAX_SWAPCHAIN_IMAGES 8

// One bridge slot: a shared image aliased on both devices.
//   session-side image  <- the copy source, lives on the runtime's device
//   unity-side image    <- what Unity renders into, lives on Unity's device
// Both are bound to the SAME device memory via an OPAQUE_WIN32 handle.
struct PvkBridge {
	VkImage        session_image = VK_NULL_HANDLE;
	VkDeviceMemory session_memory = VK_NULL_HANDLE;
	VkImage        unity_image = VK_NULL_HANDLE;
	VkDeviceMemory unity_memory = VK_NULL_HANDLE;
	HANDLE         shared_handle = NULL;
	uint32_t       width = 0, height = 0, layers = 0;
	VkFormat       format = VK_FORMAT_UNDEFINED;
	// GENERAL layout is applied once, then kept: the copy transitions
	// GENERAL->TRANSFER_SRC and back each frame. Mirrors the standalone
	// backend's bridge_initialized latch.
	bool           layout_initialized = false;
	bool           valid = false;
};

struct PvkState {
	// --- session device (created BY THE RUNTIME via enable2) ---
	VkApi            api = {};
	VkInstance       instance = VK_NULL_HANDLE;
	VkPhysicalDevice physical_device = VK_NULL_HANDLE;
	VkDevice         device = VK_NULL_HANDLE;
	uint32_t         queue_family = 0;
	uint32_t         queue_index = 0;
	VkQueue          queue = VK_NULL_HANDLE;
	VkCommandPool    cmd_pool = VK_NULL_HANDLE;
	VkCommandBuffer  cmd_buf = VK_NULL_HANDLE;
	VkFence          copy_fence = VK_NULL_HANDLE;
	bool             device_ready = false;

	// --- Unity's device (captured from IUnityGraphicsVulkan) ---
	VkApi            unity_api = {};
	VkInstance       unity_instance = VK_NULL_HANDLE;
	VkPhysicalDevice unity_physical_device = VK_NULL_HANDLE;
	VkDevice         unity_device = VK_NULL_HANDLE;
	uint32_t         unity_queue_family = 0;
	VkQueue          unity_queue = VK_NULL_HANDLE;

	// --- session swapchain (runtime-allocated, on the session device) ---
	VkImage  sc_images[PVK_MAX_SWAPCHAIN_IMAGES] = {};
	uint32_t sc_image_count = 0;
	uint32_t sc_width = 0, sc_height = 0, sc_array = 0;
	VkFormat sc_format = VK_FORMAT_UNDEFINED;

	// --- bridges: [0] = SPI (2-layer), [1]/[2] = MultiPass eye 0/1 ---
	PvkBridge bridge_spi;
	PvkBridge bridge_eye[2];

	// Session binding storage, handed to xrCreateSession.
	XrGraphicsBindingVulkanKHR binding = {};
};

static PvkState s_pvk;

static PvkBridge *
pvk_slot(int eye)
{
	if (eye < 0) return &s_pvk.bridge_spi;
	if (eye == 0 || eye == 1) return &s_pvk.bridge_eye[eye];
	return NULL;
}

// DXGI_FORMAT (what the provider's swapchain-format plumbing speaks) is not what
// a VK session swapchain reports — under XR_KHR_vulkan_enable* the int64 format
// IS a VkFormat. Kept as a named helper so the cast is intentional rather than
// an implicit narrowing hidden in a call site.
static VkFormat
pvk_format_from_xr(int64_t f)
{
	return (VkFormat)f;
}

// ---------------------------------------------------------------------------
// Unity object capture
// ---------------------------------------------------------------------------

void
dxr_pvk_set_unity_objects(void *instance, void *physical_device, void *device,
                          uint32_t queue_family, void *queue)
{
	s_pvk.unity_instance = (VkInstance)instance;
	s_pvk.unity_physical_device = (VkPhysicalDevice)physical_device;
	s_pvk.unity_device = (VkDevice)device;
	s_pvk.unity_queue_family = queue_family;
	s_pvk.unity_queue = (VkQueue)queue;

	// Unity's device needs its own dispatch table: entry points resolved for the
	// session device are NOT valid on a different VkDevice.
	if (!dxr_vk_load_global(&s_pvk.unity_api)) return;
	if (s_pvk.unity_instance) dxr_vk_load_instance(&s_pvk.unity_api, s_pvk.unity_instance);
	if (s_pvk.unity_device) dxr_vk_load_device(&s_pvk.unity_api, s_pvk.unity_device);

	pvk_log("[DisplayXR-PROV-VK] Unity objects: instance=%p phys=%p device=%p qf=%u queue=%p\n",
	        (void *)s_pvk.unity_instance, (void *)s_pvk.unity_physical_device,
	        (void *)s_pvk.unity_device, s_pvk.unity_queue_family, (void *)s_pvk.unity_queue);
}

// ---------------------------------------------------------------------------
// Cross-adapter guard (the VK form of #240)
// ---------------------------------------------------------------------------

// Read VkPhysicalDeviceIDProperties.deviceLUID. Returns false when the property
// is unavailable (no properties2, or deviceLUIDValid == false), which the caller
// must treat as "cannot check" rather than "mismatch".
static bool
pvk_device_luid(VkApi *api, VkPhysicalDevice phys, uint8_t out_luid[VK_LUID_SIZE])
{
	if (!api || !api->vkGetPhysicalDeviceProperties2 || phys == VK_NULL_HANDLE) return false;
	VkPhysicalDeviceIDProperties idp = {};
	idp.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
	VkPhysicalDeviceProperties2 p2 = {};
	p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
	p2.pNext = &idp;
	api->vkGetPhysicalDeviceProperties2(phys, &p2);
	if (!idp.deviceLUIDValid) return false;
	memcpy(out_luid, idp.deviceLUID, VK_LUID_SIZE);
	return true;
}

static void
pvk_describe(VkApi *api, VkPhysicalDevice phys, char *out, size_t n)
{
	snprintf(out, n, "<unknown>");
	if (!api || !api->vkGetPhysicalDeviceProperties || phys == VK_NULL_HANDLE) return;
	VkPhysicalDeviceProperties props = {};
	api->vkGetPhysicalDeviceProperties(phys, &props);
	snprintf(out, n, "%s", props.deviceName);
}

// The eye bridge shares memory between the runtime's device and Unity's. If they
// are different physical devices the OPAQUE_WIN32 import either fails outright or
// (worse, and what #240 actually looked like on D3D12) succeeds and presents
// black with a fully healthy-looking session. Refuse, and name both adapters plus
// the knobs, rather than start a session that can never display.
static bool
pvk_check_same_adapter(void)
{
	if (s_pvk.unity_physical_device == VK_NULL_HANDLE) {
		pvk_log("[DisplayXR-PROV-VK] WARN: Unity's VkPhysicalDevice unknown — skipping the "
		        "cross-adapter guard. A mismatch here presents black (#240).\n");
		return true;
	}
	// Same handle from the same VkInstance is already conclusive.
	if (s_pvk.unity_instance == s_pvk.instance &&
	    s_pvk.unity_physical_device == s_pvk.physical_device)
		return true;

	uint8_t luid_rt[VK_LUID_SIZE] = {}, luid_unity[VK_LUID_SIZE] = {};
	bool ok_rt = pvk_device_luid(&s_pvk.api, s_pvk.physical_device, luid_rt);
	bool ok_unity = pvk_device_luid(&s_pvk.unity_api, s_pvk.unity_physical_device, luid_unity);
	if (!ok_rt || !ok_unity) {
		pvk_log("[DisplayXR-PROV-VK] WARN: device LUID unavailable (runtime=%d unity=%d) — "
		        "cross-adapter guard skipped.\n", (int)ok_rt, (int)ok_unity);
		return true;
	}
	if (memcmp(luid_rt, luid_unity, VK_LUID_SIZE) == 0) return true;

	char d_rt[256], d_unity[256];
	pvk_describe(&s_pvk.api, s_pvk.physical_device, d_rt, sizeof(d_rt));
	pvk_describe(&s_pvk.unity_api, s_pvk.unity_physical_device, d_unity, sizeof(d_unity));
	pvk_log("[DisplayXR-PROV-VK] ERROR: GPU adapter mismatch - XR session NOT started, a "
	        "cross-adapter eye bridge would present black (#240, Vulkan form).\n"
	        "[DisplayXR-PROV-VK]   Unity renders on : %s\n"
	        "[DisplayXR-PROV-VK]   Runtime selected : %s\n"
	        "[DisplayXR-PROV-VK]   Fix: align them. Set this app's Windows GpuPreference to the "
	        "runtime's adapter (Settings > System > Display > Graphics), or point the runtime at "
	        "Unity's adapter. NOTE the DXGI-based DisplayXRGpuPreference lever (#242) steers the "
	        "D3D paths only - under Vulkan the runtime picks via xrGetVulkanGraphicsDevice2KHR.\n",
	        d_unity, d_rt);
	return false;
}

// ---------------------------------------------------------------------------
// enable2 device creation
// ---------------------------------------------------------------------------

int
dxr_pvk_create_device(XrInstance instance, XrSystemId system_id,
                      PFN_xrGetInstanceProcAddr gipa, void *unity_physical_device)
{
	if (unity_physical_device && s_pvk.unity_physical_device == VK_NULL_HANDLE)
		s_pvk.unity_physical_device = (VkPhysicalDevice)unity_physical_device;

	if (!dxr_vk_load_global(&s_pvk.api)) {
		pvk_log("[DisplayXR-PROV-VK] vulkan-1.dll unavailable — cannot start a Vulkan session\n");
		return 0;
	}

	PFN_xrVoidFunction fn = NULL;
	PFN_xrGetVulkanGraphicsRequirements2KHR get_req = NULL;
	PFN_xrCreateVulkanInstanceKHR create_inst = NULL;
	PFN_xrGetVulkanGraphicsDevice2KHR get_dev = NULL;
	PFN_xrCreateVulkanDeviceKHR create_dev = NULL;

	gipa(instance, "xrGetVulkanGraphicsRequirements2KHR", &fn);
	get_req = (PFN_xrGetVulkanGraphicsRequirements2KHR)fn; fn = NULL;
	gipa(instance, "xrCreateVulkanInstanceKHR", &fn);
	create_inst = (PFN_xrCreateVulkanInstanceKHR)fn; fn = NULL;
	gipa(instance, "xrGetVulkanGraphicsDevice2KHR", &fn);
	get_dev = (PFN_xrGetVulkanGraphicsDevice2KHR)fn; fn = NULL;
	gipa(instance, "xrCreateVulkanDeviceKHR", &fn);
	create_dev = (PFN_xrCreateVulkanDeviceKHR)fn; fn = NULL;

	if (!create_inst || !get_dev || !create_dev) {
		// enable1 would technically let us make our own device, but then the runtime
		// has no queue of its own and the #868 repaint silently stays off — the exact
		// failure runtime#886 exists to stop. Refuse rather than start a degraded session.
		pvk_log("[DisplayXR-PROV-VK] ERROR: XR_KHR_vulkan_enable2 entry points unresolved "
		        "(xrCreateVulkanInstanceKHR=%p xrGetVulkanGraphicsDevice2KHR=%p "
		        "xrCreateVulkanDeviceKHR=%p). This runtime does not implement enable2; the "
		        "provider requires it so the runtime owns a queue (#886/#868).\n",
		        (void *)create_inst, (void *)get_dev, (void *)create_dev);
		return 0;
	}

	// Graphics requirements must be queried before the create calls (spec-mandated,
	// and the runtime enforces it the same way the D3D paths do).
	if (get_req) {
		XrGraphicsRequirementsVulkanKHR req = {XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
		XrResult r = get_req(instance, system_id, &req);
		if (XR_FAILED(r))
			pvk_log("[DisplayXR-PROV-VK] WARN: xrGetVulkanGraphicsRequirements2KHR failed (0x%x)\n",
			        (unsigned)r);
	} else {
		pvk_log("[DisplayXR-PROV-VK] WARN: xrGetVulkanGraphicsRequirements2KHR unresolved\n");
	}

	// --- VkInstance, created BY THE RUNTIME (it merges its own required extensions) ---
	// We ask only for what the bridge needs; the v1 string-query helpers are gone
	// (they are not dispatchable under enable2-only).
	const char *inst_exts[] = {
		VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,
		VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME,
		VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
	};
	VkApplicationInfo app = {};
	app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	app.pApplicationName = "DisplayXR Unity Provider";
	app.applicationVersion = 1;
	app.pEngineName = "Unity";
	app.engineVersion = 1;
	app.apiVersion = VK_API_VERSION_1_1; // 1.1 gives properties2/external-memory in core

	VkInstanceCreateInfo ici = {};
	ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	ici.pApplicationInfo = &app;
	ici.enabledExtensionCount = (uint32_t)(sizeof(inst_exts) / sizeof(inst_exts[0]));
	ici.ppEnabledExtensionNames = inst_exts;

	XrVulkanInstanceCreateInfoKHR xici = {};
	xici.type = XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR;
	xici.systemId = system_id;
	xici.pfnGetInstanceProcAddr = s_pvk.api.vkGetInstanceProcAddr;
	xici.vulkanCreateInfo = &ici;

	VkResult vk_res = VK_SUCCESS;
	XrResult r = create_inst(instance, &xici, &s_pvk.instance, &vk_res);
	if (XR_FAILED(r) || vk_res != VK_SUCCESS || s_pvk.instance == VK_NULL_HANDLE) {
		pvk_log("[DisplayXR-PROV-VK] xrCreateVulkanInstanceKHR failed (xr=0x%x vk=%d)\n",
		        (unsigned)r, (int)vk_res);
		return 0;
	}
	dxr_vk_load_instance(&s_pvk.api, s_pvk.instance);

	// --- VkPhysicalDevice, chosen by the runtime ---
	XrVulkanGraphicsDeviceGetInfoKHR gdi = {};
	gdi.type = XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR;
	gdi.systemId = system_id;
	gdi.vulkanInstance = s_pvk.instance;
	r = get_dev(instance, &gdi, &s_pvk.physical_device);
	if (XR_FAILED(r) || s_pvk.physical_device == VK_NULL_HANDLE) {
		pvk_log("[DisplayXR-PROV-VK] xrGetVulkanGraphicsDevice2KHR failed (0x%x)\n", (unsigned)r);
		return 0;
	}

	// Guard BEFORE creating the device: a mismatch is terminal, and failing here
	// leaves less to unwind.
	if (!pvk_check_same_adapter()) return 0;

	// --- Pick a graphics queue family ---
	uint32_t qf_count = 0;
	s_pvk.api.vkGetPhysicalDeviceQueueFamilyProperties(s_pvk.physical_device, &qf_count, NULL);
	if (qf_count == 0) {
		pvk_log("[DisplayXR-PROV-VK] no queue families on the selected physical device\n");
		return 0;
	}
	if (qf_count > 32) qf_count = 32;
	VkQueueFamilyProperties qfp[32] = {};
	s_pvk.api.vkGetPhysicalDeviceQueueFamilyProperties(s_pvk.physical_device, &qf_count, qfp);
	uint32_t gfx_family = UINT32_MAX;
	for (uint32_t i = 0; i < qf_count; i++) {
		if (qfp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { gfx_family = i; break; }
	}
	if (gfx_family == UINT32_MAX) {
		pvk_log("[DisplayXR-PROV-VK] no graphics-capable queue family\n");
		return 0;
	}
	s_pvk.queue_family = gfx_family;
	s_pvk.queue_index = 0;

	// --- VkDevice, created BY THE RUNTIME ---
	// This is the whole point of enable2: because the runtime performs the
	// vkCreateDevice it can append its OWN queue request to pQueueCreateInfos, which
	// is what lights up the #868 weave-rate-decoupling repaint. We ask for one
	// graphics queue; the runtime adds a second on the same family if it can (look
	// for "requesting a runtime-owned queue (family N index M)" in the runtime log).
	float prio = 1.0f;
	VkDeviceQueueCreateInfo qci = {};
	qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	qci.queueFamilyIndex = gfx_family;
	qci.queueCount = 1;
	qci.pQueuePriorities = &prio;

	const char *dev_exts[] = {
		VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
		VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
		VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME,
		VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME,
		VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME,
		VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,
	};
	VkDeviceCreateInfo dci = {};
	dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	dci.queueCreateInfoCount = 1;
	dci.pQueueCreateInfos = &qci;
	dci.enabledExtensionCount = (uint32_t)(sizeof(dev_exts) / sizeof(dev_exts[0]));
	dci.ppEnabledExtensionNames = dev_exts;

	XrVulkanDeviceCreateInfoKHR xdci = {};
	xdci.type = XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR;
	xdci.systemId = system_id;
	xdci.pfnGetInstanceProcAddr = s_pvk.api.vkGetInstanceProcAddr;
	xdci.vulkanPhysicalDevice = s_pvk.physical_device;
	xdci.vulkanCreateInfo = &dci;

	vk_res = VK_SUCCESS;
	r = create_dev(instance, &xdci, &s_pvk.device, &vk_res);
	if (XR_FAILED(r) || vk_res != VK_SUCCESS || s_pvk.device == VK_NULL_HANDLE) {
		pvk_log("[DisplayXR-PROV-VK] xrCreateVulkanDeviceKHR failed (xr=0x%x vk=%d)\n",
		        (unsigned)r, (int)vk_res);
		return 0;
	}
	dxr_vk_load_device(&s_pvk.api, s_pvk.device);
	s_pvk.api.vkGetDeviceQueue(s_pvk.device, s_pvk.queue_family, s_pvk.queue_index, &s_pvk.queue);

	// --- Command pool / buffer / fence for the per-frame bridge copy ---
	VkCommandPoolCreateInfo cpci = {};
	cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	cpci.queueFamilyIndex = s_pvk.queue_family;
	if (s_pvk.api.vkCreateCommandPool(s_pvk.device, &cpci, NULL, &s_pvk.cmd_pool) != VK_SUCCESS) {
		pvk_log("[DisplayXR-PROV-VK] vkCreateCommandPool failed\n");
		return 0;
	}
	VkCommandBufferAllocateInfo cbai = {};
	cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	cbai.commandPool = s_pvk.cmd_pool;
	cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	cbai.commandBufferCount = 1;
	if (s_pvk.api.vkAllocateCommandBuffers(s_pvk.device, &cbai, &s_pvk.cmd_buf) != VK_SUCCESS) {
		pvk_log("[DisplayXR-PROV-VK] vkAllocateCommandBuffers failed\n");
		return 0;
	}
	VkFenceCreateInfo fci = {};
	fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	if (s_pvk.api.vkCreateFence(s_pvk.device, &fci, NULL, &s_pvk.copy_fence) != VK_SUCCESS) {
		pvk_log("[DisplayXR-PROV-VK] vkCreateFence failed\n");
		return 0;
	}

	char desc[256];
	pvk_describe(&s_pvk.api, s_pvk.physical_device, desc, sizeof(desc));
	pvk_log("[DisplayXR-PROV-VK] Own Vulkan device created via enable2 (runtime session device) "
	        "on %s: instance=%p phys=%p device=%p qf=%u\n",
	        desc, (void *)s_pvk.instance, (void *)s_pvk.physical_device,
	        (void *)s_pvk.device, s_pvk.queue_family);
	s_pvk.device_ready = true;
	return 1;
}

int
dxr_pvk_device_ready(void)
{
	return s_pvk.device_ready ? 1 : 0;
}

const void *
dxr_pvk_session_binding(const void *next)
{
	if (!s_pvk.device_ready) return NULL;
	s_pvk.binding.type = XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR;
	s_pvk.binding.next = next;
	s_pvk.binding.instance = s_pvk.instance;
	s_pvk.binding.physicalDevice = s_pvk.physical_device;
	s_pvk.binding.device = s_pvk.device;
	s_pvk.binding.queueFamilyIndex = s_pvk.queue_family;
	s_pvk.binding.queueIndex = s_pvk.queue_index;
	return &s_pvk.binding;
}

void
dxr_pvk_set_swapchain_images(const void *images, uint32_t count,
                             uint32_t width, uint32_t height,
                             uint32_t array_size, int64_t format)
{
	const XrSwapchainImageVulkanKHR *imgs = (const XrSwapchainImageVulkanKHR *)images;
	if (count > PVK_MAX_SWAPCHAIN_IMAGES) count = PVK_MAX_SWAPCHAIN_IMAGES;
	for (uint32_t i = 0; i < count; i++) s_pvk.sc_images[i] = imgs[i].image;
	s_pvk.sc_image_count = count;
	s_pvk.sc_width = width;
	s_pvk.sc_height = height;
	s_pvk.sc_array = array_size;
	s_pvk.sc_format = pvk_format_from_xr(format);
	pvk_log("[DisplayXR-PROV-VK] swapchain images: %u x (%ux%u arr=%u fmt=%d)\n",
	        count, width, height, array_size, (int)s_pvk.sc_format);
}

// ---------------------------------------------------------------------------
// The eye bridge
// ---------------------------------------------------------------------------

static uint32_t
pvk_find_memory_type(VkApi *api, VkPhysicalDevice phys, uint32_t type_bits,
                     VkMemoryPropertyFlags props)
{
	VkPhysicalDeviceMemoryProperties mp = {};
	api->vkGetPhysicalDeviceMemoryProperties(phys, &mp);
	for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
		if ((type_bits & (1u << i)) &&
		    (mp.memoryTypes[i].propertyFlags & props) == props)
			return i;
	}
	return UINT32_MAX;
}

// Build the image create-info shared by BOTH sides of the bridge. The two images
// alias the same memory, so their create-infos must match exactly — building them
// from one function is the cheapest way to keep that true.
static VkImageCreateInfo
pvk_bridge_image_ci(uint32_t w, uint32_t h, uint32_t layers, VkFormat fmt,
                    VkExternalMemoryImageCreateInfo *ext)
{
	ext->sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
	ext->pNext = NULL;
	ext->handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;

	VkImageCreateInfo ici = {};
	ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	ici.pNext = ext;
	ici.imageType = VK_IMAGE_TYPE_2D;
	ici.format = fmt;
	ici.extent.width = w;
	ici.extent.height = h;
	ici.extent.depth = 1;
	ici.mipLevels = 1;
	ici.arrayLayers = layers;
	ici.samples = VK_SAMPLE_COUNT_1_BIT;
	ici.tiling = VK_IMAGE_TILING_OPTIMAL;
	// COLOR_ATTACHMENT: Unity renders the eyes into it.
	// TRANSFER_SRC:     the session device copies it into the swapchain image.
	// SAMPLED:          Unity may sample it (mirror blit / GameView).
	ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
	            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	return ici;
}

int
dxr_pvk_create_bridge(int eye, uint32_t width, uint32_t height,
                      uint32_t array_size, int64_t format)
{
	PvkBridge *b = pvk_slot(eye);
	if (!b || !s_pvk.device_ready) return 0;
	if (s_pvk.unity_device == VK_NULL_HANDLE) {
		pvk_log("[DisplayXR-PROV-VK] bridge: Unity VkDevice not captured yet\n");
		return 0;
	}
	if (b->valid && b->width == width && b->height == height && b->layers == array_size)
		return 1; // already sized

	VkFormat fmt = pvk_format_from_xr(format);

	// --- session side: create + export ---
	VkExternalMemoryImageCreateInfo ext_ci = {};
	VkImageCreateInfo ici = pvk_bridge_image_ci(width, height, array_size, fmt, &ext_ci);
	if (s_pvk.api.vkCreateImage(s_pvk.device, &ici, NULL, &b->session_image) != VK_SUCCESS) {
		pvk_log("[DisplayXR-PROV-VK] bridge: vkCreateImage (session) failed\n");
		return 0;
	}
	VkMemoryRequirements mr = {};
	s_pvk.api.vkGetImageMemoryRequirements(s_pvk.device, b->session_image, &mr);

	// Dedicated allocation: required in practice for external images on Windows
	// drivers, and it keeps the two sides' allocations trivially identical.
	VkMemoryDedicatedAllocateInfo ded = {};
	ded.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
	ded.image = b->session_image;

	VkExportMemoryWin32HandleInfoKHR exp_win32 = {};
	exp_win32.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_WIN32_HANDLE_INFO_KHR;
	exp_win32.pNext = &ded;
	exp_win32.dwAccess = GENERIC_ALL;

	VkExportMemoryAllocateInfo exp = {};
	exp.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
	exp.pNext = &exp_win32;
	exp.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;

	VkMemoryAllocateInfo mai = {};
	mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	mai.pNext = &exp;
	mai.allocationSize = mr.size;
	mai.memoryTypeIndex = pvk_find_memory_type(&s_pvk.api, s_pvk.physical_device,
	                                           mr.memoryTypeBits,
	                                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (mai.memoryTypeIndex == UINT32_MAX) {
		pvk_log("[DisplayXR-PROV-VK] bridge: no device-local memory type\n");
		return 0;
	}
	if (s_pvk.api.vkAllocateMemory(s_pvk.device, &mai, NULL, &b->session_memory) != VK_SUCCESS) {
		pvk_log("[DisplayXR-PROV-VK] bridge: vkAllocateMemory (session) failed\n");
		return 0;
	}
	if (s_pvk.api.vkBindImageMemory(s_pvk.device, b->session_image, b->session_memory, 0) != VK_SUCCESS) {
		pvk_log("[DisplayXR-PROV-VK] bridge: vkBindImageMemory (session) failed\n");
		return 0;
	}

	if (!s_pvk.api.vkGetMemoryWin32HandleKHR) {
		pvk_log("[DisplayXR-PROV-VK] bridge: vkGetMemoryWin32HandleKHR unresolved — "
		        "VK_KHR_external_memory_win32 missing on the session device\n");
		return 0;
	}
	VkMemoryGetWin32HandleInfoKHR ghi = {};
	ghi.sType = VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR;
	ghi.memory = b->session_memory;
	ghi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
	if (s_pvk.api.vkGetMemoryWin32HandleKHR(s_pvk.device, &ghi, &b->shared_handle) != VK_SUCCESS ||
	    !b->shared_handle) {
		pvk_log("[DisplayXR-PROV-VK] bridge: vkGetMemoryWin32HandleKHR failed\n");
		return 0;
	}

	// --- Unity side: create an identical image and IMPORT the same memory ---
	VkExternalMemoryImageCreateInfo u_ext_ci = {};
	VkImageCreateInfo u_ici = pvk_bridge_image_ci(width, height, array_size, fmt, &u_ext_ci);
	if (s_pvk.unity_api.vkCreateImage(s_pvk.unity_device, &u_ici, NULL, &b->unity_image) != VK_SUCCESS) {
		pvk_log("[DisplayXR-PROV-VK] bridge: vkCreateImage (unity) failed\n");
		return 0;
	}
	VkMemoryRequirements u_mr = {};
	s_pvk.unity_api.vkGetImageMemoryRequirements(s_pvk.unity_device, b->unity_image, &u_mr);

	VkMemoryDedicatedAllocateInfo u_ded = {};
	u_ded.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
	u_ded.image = b->unity_image;

	VkImportMemoryWin32HandleInfoKHR imp = {};
	imp.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR;
	imp.pNext = &u_ded;
	imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
	imp.handle = b->shared_handle;

	VkMemoryAllocateInfo u_mai = {};
	u_mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	u_mai.pNext = &imp;
	u_mai.allocationSize = u_mr.size;
	u_mai.memoryTypeIndex = pvk_find_memory_type(&s_pvk.unity_api, s_pvk.unity_physical_device,
	                                             u_mr.memoryTypeBits,
	                                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (u_mai.memoryTypeIndex == UINT32_MAX) {
		pvk_log("[DisplayXR-PROV-VK] bridge: no device-local memory type on Unity's device\n");
		return 0;
	}
	if (s_pvk.unity_api.vkAllocateMemory(s_pvk.unity_device, &u_mai, NULL, &b->unity_memory) != VK_SUCCESS) {
		pvk_log("[DisplayXR-PROV-VK] bridge: vkAllocateMemory (unity import) failed\n");
		return 0;
	}
	if (s_pvk.unity_api.vkBindImageMemory(s_pvk.unity_device, b->unity_image, b->unity_memory, 0) != VK_SUCCESS) {
		pvk_log("[DisplayXR-PROV-VK] bridge: vkBindImageMemory (unity) failed\n");
		return 0;
	}

	b->width = width; b->height = height; b->layers = array_size;
	b->format = fmt;
	b->layout_initialized = false;
	b->valid = true;
	pvk_log("[DisplayXR-PROV-VK] bridge[%d]: %ux%u layers=%u fmt=%d shared (session=%p unity=%p handle=%p)\n",
	        eye, width, height, array_size, (int)fmt,
	        (void *)b->session_image, (void *)b->unity_image, (void *)b->shared_handle);
	return 1;
}

void *
dxr_pvk_unity_image_ptr(int eye)
{
	PvkBridge *b = pvk_slot(eye);
	if (!b || !b->valid) return NULL;
	// See the header: Unity wants a POINTER TO the VkImage handle on Vulkan, not
	// the handle value. Returning &b->unity_image (storage that outlives the
	// texture) rather than (void*)b->unity_image is deliberate.
	return (void *)&b->unity_image;
}

// ---------------------------------------------------------------------------
// Per-frame copy
// ---------------------------------------------------------------------------

static VkImageMemoryBarrier
pvk_barrier(VkImage img, VkImageLayout old_layout, VkImageLayout new_layout,
            VkAccessFlags src_access, VkAccessFlags dst_access,
            uint32_t base_layer, uint32_t layer_count)
{
	VkImageMemoryBarrier b = {};
	b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	b.oldLayout = old_layout;
	b.newLayout = new_layout;
	b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.image = img;
	b.srcAccessMask = src_access;
	b.dstAccessMask = dst_access;
	b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	b.subresourceRange.baseMipLevel = 0;
	b.subresourceRange.levelCount = 1;
	b.subresourceRange.baseArrayLayer = base_layer;
	b.subresourceRange.layerCount = layer_count;
	return b;
}

void
dxr_pvk_signal_unity_done(void)
{
	// ORDERING, PHASE 1: drain Unity's queue so its eye renders are complete
	// before the session device reads the shared image.
	//
	// This is the Vulkan analogue of ps_d3d11_ctx_drain() on the D3D11 editor
	// bridge — correct, and deliberately the blunt instrument. The cheap version
	// is an OPAQUE_WIN32 semaphore signalled from Unity's queue and waited by our
	// copy submit (the loader already resolves vkGet/ImportSemaphoreWin32HandleKHR
	// for it), but submitting to Unity's queue safely requires plugin-event queue
	// access (kUnityVulkanGraphicsQueueAccess_Allow) that the IUnityXRDisplay
	// callbacks do not carry today. Landing the drain first keeps Phase 1
	// verifiable; the semaphore is a measured optimisation, not a correctness fix.
	if (s_pvk.unity_api.vkQueueWaitIdle && s_pvk.unity_queue != VK_NULL_HANDLE)
		s_pvk.unity_api.vkQueueWaitIdle(s_pvk.unity_queue);
	else if (s_pvk.unity_api.vkDeviceWaitIdle && s_pvk.unity_device != VK_NULL_HANDLE)
		s_pvk.unity_api.vkDeviceWaitIdle(s_pvk.unity_device);
}

int
dxr_pvk_copy_to_swapchain_image(int eye, uint32_t image_index)
{
	PvkBridge *b = pvk_slot(eye);
	if (!b || !b->valid || !s_pvk.device_ready) return 0;
	if (image_index >= s_pvk.sc_image_count) return 0;
	VkImage dst = s_pvk.sc_images[image_index];
	if (dst == VK_NULL_HANDLE) return 0;

	// MultiPass writes eye e into swapchain array slice e; SPI copies both layers.
	uint32_t dst_base_layer = (eye < 0) ? 0 : (uint32_t)eye;
	uint32_t layer_count = (eye < 0) ? b->layers : 1;

	VkCommandBufferBeginInfo bi = {};
	bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	s_pvk.api.vkResetCommandBuffer(s_pvk.cmd_buf, 0);
	if (s_pvk.api.vkBeginCommandBuffer(s_pvk.cmd_buf, &bi) != VK_SUCCESS) return 0;

	// The bridge sits in GENERAL between frames (first frame comes from UNDEFINED).
	VkImageLayout src_old = b->layout_initialized ? VK_IMAGE_LAYOUT_GENERAL
	                                              : VK_IMAGE_LAYOUT_UNDEFINED;
	VkImageMemoryBarrier to_src = pvk_barrier(b->session_image, src_old,
	                                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	                                          0, VK_ACCESS_TRANSFER_READ_BIT,
	                                          0, b->layers);
	VkImageMemoryBarrier to_dst = pvk_barrier(dst, VK_IMAGE_LAYOUT_UNDEFINED,
	                                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	                                          0, VK_ACCESS_TRANSFER_WRITE_BIT,
	                                          dst_base_layer, layer_count);
	VkImageMemoryBarrier pre[2] = {to_src, to_dst};
	s_pvk.api.vkCmdPipelineBarrier(s_pvk.cmd_buf,
	                               VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
	                               VK_PIPELINE_STAGE_TRANSFER_BIT,
	                               0, 0, NULL, 0, NULL, 2, pre);

	VkImageCopy region = {};
	region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	region.srcSubresource.mipLevel = 0;
	region.srcSubresource.baseArrayLayer = 0;
	region.srcSubresource.layerCount = layer_count;
	region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	region.dstSubresource.mipLevel = 0;
	region.dstSubresource.baseArrayLayer = dst_base_layer;
	region.dstSubresource.layerCount = layer_count;
	region.extent.width = b->width;
	region.extent.height = b->height;
	region.extent.depth = 1;
	s_pvk.api.vkCmdCopyImage(s_pvk.cmd_buf,
	                         b->session_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	                         dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	                         1, &region);

	// Swapchain images must be back in COLOR_ATTACHMENT_OPTIMAL for the runtime's
	// compositor; the bridge returns to GENERAL for Unity's next render.
	VkImageMemoryBarrier back_src = pvk_barrier(b->session_image,
	                                            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	                                            VK_IMAGE_LAYOUT_GENERAL,
	                                            VK_ACCESS_TRANSFER_READ_BIT, 0,
	                                            0, b->layers);
	VkImageMemoryBarrier back_dst = pvk_barrier(dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	                                            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	                                            VK_ACCESS_TRANSFER_WRITE_BIT,
	                                            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
	                                            dst_base_layer, layer_count);
	VkImageMemoryBarrier post[2] = {back_src, back_dst};
	s_pvk.api.vkCmdPipelineBarrier(s_pvk.cmd_buf,
	                               VK_PIPELINE_STAGE_TRANSFER_BIT,
	                               VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
	                               0, 0, NULL, 0, NULL, 2, post);

	if (s_pvk.api.vkEndCommandBuffer(s_pvk.cmd_buf) != VK_SUCCESS) return 0;

	VkSubmitInfo si = {};
	si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	si.commandBufferCount = 1;
	si.pCommandBuffers = &s_pvk.cmd_buf;
	s_pvk.api.vkResetFences(s_pvk.device, 1, &s_pvk.copy_fence);
	if (s_pvk.api.vkQueueSubmit(s_pvk.queue, 1, &si, s_pvk.copy_fence) != VK_SUCCESS) {
		pvk_log("[DisplayXR-PROV-VK] copy: vkQueueSubmit failed\n");
		return 0;
	}
	// xrReleaseSwapchainImage must not run before the copy lands.
	s_pvk.api.vkWaitForFences(s_pvk.device, 1, &s_pvk.copy_fence, VK_TRUE, UINT64_MAX);

	b->layout_initialized = true;
	return 1;
}

// ---------------------------------------------------------------------------
// Teardown
// ---------------------------------------------------------------------------

static void
pvk_destroy_bridge(PvkBridge *b)
{
	if (!b) return;
	if (b->unity_image && s_pvk.unity_api.vkDestroyImage)
		s_pvk.unity_api.vkDestroyImage(s_pvk.unity_device, b->unity_image, NULL);
	if (b->unity_memory && s_pvk.unity_api.vkFreeMemory)
		s_pvk.unity_api.vkFreeMemory(s_pvk.unity_device, b->unity_memory, NULL);
	if (b->session_image && s_pvk.api.vkDestroyImage)
		s_pvk.api.vkDestroyImage(s_pvk.device, b->session_image, NULL);
	if (b->session_memory && s_pvk.api.vkFreeMemory)
		s_pvk.api.vkFreeMemory(s_pvk.device, b->session_memory, NULL);
	// The imported allocation owns a reference to the handle, so it is closed only
	// after BOTH sides are gone (Vulkan takes a reference at import, but the
	// exported handle itself is ours to close).
	if (b->shared_handle) CloseHandle(b->shared_handle);
	*b = PvkBridge{};
}

void
dxr_pvk_destroy(void)
{
	if (s_pvk.device && s_pvk.api.vkDeviceWaitIdle) s_pvk.api.vkDeviceWaitIdle(s_pvk.device);

	pvk_destroy_bridge(&s_pvk.bridge_spi);
	pvk_destroy_bridge(&s_pvk.bridge_eye[0]);
	pvk_destroy_bridge(&s_pvk.bridge_eye[1]);

	if (s_pvk.copy_fence && s_pvk.api.vkDestroyFence)
		s_pvk.api.vkDestroyFence(s_pvk.device, s_pvk.copy_fence, NULL);
	if (s_pvk.cmd_pool && s_pvk.api.vkDestroyCommandPool)
		s_pvk.api.vkDestroyCommandPool(s_pvk.device, s_pvk.cmd_pool, NULL);
	if (s_pvk.device && s_pvk.api.vkDestroyDevice)
		s_pvk.api.vkDestroyDevice(s_pvk.device, NULL);
	if (s_pvk.instance && s_pvk.api.vkDestroyInstance)
		s_pvk.api.vkDestroyInstance(s_pvk.instance, NULL);

	// Unity's objects are NOT ours to destroy — drop the references only.
	VkApi unity_api = s_pvk.unity_api;
	VkInstance ui = s_pvk.unity_instance;
	VkPhysicalDevice up = s_pvk.unity_physical_device;
	VkDevice ud = s_pvk.unity_device;
	uint32_t uqf = s_pvk.unity_queue_family;
	VkQueue uq = s_pvk.unity_queue;

	s_pvk = PvkState{};

	s_pvk.unity_api = unity_api;
	s_pvk.unity_instance = ui;
	s_pvk.unity_physical_device = up;
	s_pvk.unity_device = ud;
	s_pvk.unity_queue_family = uqf;
	s_pvk.unity_queue = uq;

	pvk_log("[DisplayXR-PROV-VK] destroyed\n");
}

#endif // _WIN32 && ENABLE_VULKAN
