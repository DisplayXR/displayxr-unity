// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
//
// Vulkan standalone graphics backend for the DisplayXR editor preview (#124).
//
// The standalone session renders on its OWN Vulkan device (matching the Unity
// editor's Vulkan API) and the runtime composites the woven 3D into the native
// preview HWND via XR_EXT_win32_window_binding. The Unity->SA atlas upload uses
// a cross-device external-memory bridge:
//
//   eye cameras -> s_AtlasRT (Unity VkDevice)
//   C# Graphics.CopyTexture(s_AtlasRT -> bridge texture)  [Unity VkDevice]
//   blit_atlas: bridge VkImage -> atlas swapchain VkImage  [SA VkDevice]
//   runtime weaves -> native HWND
//
// The bridge is an exportable VkImage created on the SA device; the SAME memory
// is imported into Unity's VkDevice (obtained via IUnityGraphicsVulkan, see
// displayxr_unity_plugin.cpp) and returned to C# as a VkImage for
// Texture2D.CreateExternalTexture. Mirrors the D3D12 backend's OpenSharedHandle
// atlas bridge, Vulkan<->Vulkan.
//
// All Vulkan entry points are resolved at runtime from vulkan-1.dll via the
// dispatch table in displayxr_vk_loader.h (compiled with VK_NO_PROTOTYPES) — the
// shipped DLL keeps NO hard dependency on vulkan-1.dll.

#if defined(ENABLE_VULKAN) || defined(__ANDROID__) || (defined(__linux__) && !defined(__ANDROID__) && !defined(__APPLE__))

#include "displayxr_standalone_internal.h"
#include "displayxr_vk_loader.h"
#include "displayxr_unity_plugin.h"

#include <string.h>
#include <vector>
#include <string>

// ---------------------------------------------------------------------------
// OpenXR Vulkan types + function pointer typedefs (inline — avoids requiring
// XR_USE_GRAPHICS_API_VULKAN; mirrors the D3D12 inline structs in the header).
// ---------------------------------------------------------------------------
// XR_KHR_vulkan_enable (extension #25) structure types. NOTE: the 98a6403
// scaffold used 1000090xxx here, which is wrong — that range is unrelated, and
// it's why the scaffold never passed runtime validation. The correct values
// match the working in-session backend (displayxr_hooks_internal.h:44).
#ifndef XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR
#define XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR      ((XrStructureType)1000025000)
#endif
#ifndef XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR
#define XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR        ((XrStructureType)1000025001)
#endif
#ifndef XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR
#define XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR  ((XrStructureType)1000025002)
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
	uint64_t        minApiVersionSupported;
	uint64_t        maxApiVersionSupported;
} XrGraphicsRequirementsVulkanKHR;

typedef XrResult(XRAPI_PTR *PFN_xrGetVulkanGraphicsRequirementsKHR)(
    XrInstance, XrSystemId, XrGraphicsRequirementsVulkanKHR *);
typedef XrResult(XRAPI_PTR *PFN_xrGetVulkanGraphicsDeviceKHR)(
    XrInstance, XrSystemId, VkInstance, VkPhysicalDevice *);

#if defined(_WIN32)

// Split a runtime-supplied, space-separated, null-terminated extension string
// into `out`, then append any of `extra` not already present.
static void
merge_extensions(PFN_xrVoidFunction fn, XrInstance instance, XrSystemId system,
                 const char *const *extra, uint32_t extra_count,
                 std::vector<std::string> &out)
{
	if (fn) {
		auto pfn = (XrResult(XRAPI_CALL *)(XrInstance, XrSystemId, uint32_t, uint32_t *, char *))fn;
		uint32_t cap = 0;
		if (XR_SUCCEEDED(pfn(instance, system, 0, &cap, nullptr)) && cap > 0) {
			std::vector<char> buf(cap);
			if (XR_SUCCEEDED(pfn(instance, system, cap, &cap, buf.data()))) {
				char *ctx = nullptr;
				char *tok = strtok_s(buf.data(), " ", &ctx);
				while (tok) { out.emplace_back(tok); tok = strtok_s(nullptr, " ", &ctx); }
			}
		}
	}
	for (uint32_t i = 0; i < extra_count; i++) {
		bool present = false;
		for (auto &s : out) if (s == extra[i]) { present = true; break; }
		if (!present) out.emplace_back(extra[i]);
	}
}

static uint32_t
find_memory_type(VkApi *api, VkPhysicalDevice phys, uint32_t type_bits, VkMemoryPropertyFlags props)
{
	VkPhysicalDeviceMemoryProperties mp;
	api->vkGetPhysicalDeviceMemoryProperties(phys, &mp);
	for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
		if ((type_bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props)
			return i;
	}
	return 0;
}

#endif // _WIN32

class StandaloneVulkanBackend : public StandaloneGraphicsBackend {
public:
	VkApi vk = {};               // SA instance + device dispatch

	VkInstance       vk_instance        = VK_NULL_HANDLE;
	VkPhysicalDevice vk_physical_device = VK_NULL_HANDLE;
	VkDevice         vk_device          = VK_NULL_HANDLE;
	uint32_t         vk_queue_family    = 0;
	VkQueue          vk_queue           = VK_NULL_HANDLE;
	VkCommandPool    vk_cmd_pool        = VK_NULL_HANDLE;
	VkCommandBuffer  vk_blit_cmd        = VK_NULL_HANDLE;
	VkFence          vk_fence           = VK_NULL_HANDLE;

	XrSwapchainImageVulkanKHR atlas_images[SA_MAX_SWAPCHAIN_IMAGES] = {};
	uint32_t atlas_image_count = 0;

	XrGraphicsBindingVulkanKHR session_binding = {};

#if defined(_WIN32)
	// Atlas bridge — exportable on the SA device, imported on Unity's device.
	uint32_t       atlas_w = 0, atlas_h = 0;
	VkImage        bridge_image  = VK_NULL_HANDLE;
	VkDeviceMemory bridge_memory = VK_NULL_HANDLE;
	HANDLE         bridge_handle = nullptr;
	bool           bridge_initialized = false; // GENERAL layout applied once

	// Unity-side imported view of the bridge memory.
	VkApi          unity_vk = {};
	VkInstance     unity_instance = VK_NULL_HANDLE;
	VkDevice       unity_device   = VK_NULL_HANDLE;
	VkImage        unity_bridge_image  = VK_NULL_HANDLE;
	VkDeviceMemory unity_bridge_memory = VK_NULL_HANDLE;
#endif

	bool create_device(XrInstance instance, XrSystemId system_id, PFN_xrGetInstanceProcAddr gipa) override
	{
#if defined(_WIN32)
		if (!dxr_vk_load_global(&vk)) return false; // vulkan-1.dll absent

		// --- Graphics requirements (api version) ---
		uint32_t api_version = VK_API_VERSION_1_1;
		{
			PFN_xrVoidFunction fn = nullptr;
			gipa(instance, "xrGetVulkanGraphicsRequirementsKHR", &fn);
			if (fn) {
				XrGraphicsRequirementsVulkanKHR req = {};
				req.type = XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR;
				((PFN_xrGetVulkanGraphicsRequirementsKHR)fn)(instance, system_id, &req);
			}
		}

		// --- Instance extensions (runtime-required + our interop needs) ---
		const char *inst_extra[] = {
			VK_KHR_SURFACE_EXTENSION_NAME,
			VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
			VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,
			VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
		};
		std::vector<std::string> inst_exts;
		{
			PFN_xrVoidFunction fn = nullptr;
			gipa(instance, "xrGetVulkanInstanceExtensionsKHR", &fn);
			merge_extensions(fn, instance, system_id, inst_extra,
			                 (uint32_t)(sizeof(inst_extra) / sizeof(inst_extra[0])), inst_exts);
		}
		std::vector<const char *> inst_ptrs;
		for (auto &s : inst_exts) inst_ptrs.push_back(s.c_str());

		VkApplicationInfo app = {};
		app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
		app.pApplicationName = "DisplayXR-Standalone";
		app.pEngineName = "DisplayXR";
		app.apiVersion = api_version;

		VkInstanceCreateInfo ici = {};
		ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
		ici.pApplicationInfo = &app;
		ici.enabledExtensionCount = (uint32_t)inst_ptrs.size();
		ici.ppEnabledExtensionNames = inst_ptrs.data();

		if (vk.vkCreateInstance(&ici, nullptr, &vk_instance) != VK_SUCCESS) {
			fprintf(stderr, "[DisplayXR-SA-VK] vkCreateInstance failed\n");
			return false;
		}
		dxr_vk_load_instance(&vk, vk_instance);

		// --- Physical device: MUST be the one the runtime suggests (the
		// runtime validates the binding's physicalDevice against this). ---
		{
			PFN_xrVoidFunction fn = nullptr;
			gipa(instance, "xrGetVulkanGraphicsDeviceKHR", &fn);
			if (fn) {
				((PFN_xrGetVulkanGraphicsDeviceKHR)fn)(instance, system_id, vk_instance, &vk_physical_device);
			}
		}
		if (vk_physical_device == VK_NULL_HANDLE) {
			// Fallback: first device. (Single-GPU machines only.)
			uint32_t n = 0;
			vk.vkEnumeratePhysicalDevices(vk_instance, &n, nullptr);
			if (n == 0) { fprintf(stderr, "[DisplayXR-SA-VK] no physical devices\n"); return false; }
			std::vector<VkPhysicalDevice> devs(n);
			vk.vkEnumeratePhysicalDevices(vk_instance, &n, devs.data());
			vk_physical_device = devs[0];
		}
		{
			VkPhysicalDeviceProperties p;
			vk.vkGetPhysicalDeviceProperties(vk_physical_device, &p);
			fprintf(stderr, "[DisplayXR-SA-VK] physical device: %s\n", p.deviceName);
		}

		// --- Graphics queue family ---
		{
			uint32_t qn = 0;
			vk.vkGetPhysicalDeviceQueueFamilyProperties(vk_physical_device, &qn, nullptr);
			std::vector<VkQueueFamilyProperties> qfp(qn);
			vk.vkGetPhysicalDeviceQueueFamilyProperties(vk_physical_device, &qn, qfp.data());
			vk_queue_family = 0;
			for (uint32_t q = 0; q < qn; q++)
				if (qfp[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) { vk_queue_family = q; break; }
		}

		// --- Device extensions (runtime-required + interop) ---
		const char *dev_extra[] = {
			VK_KHR_SWAPCHAIN_EXTENSION_NAME,
			VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
			VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
			VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME,
			VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,
		};
		std::vector<std::string> dev_exts;
		{
			PFN_xrVoidFunction fn = nullptr;
			gipa(instance, "xrGetVulkanDeviceExtensionsKHR", &fn);
			merge_extensions(fn, instance, system_id, dev_extra,
			                 (uint32_t)(sizeof(dev_extra) / sizeof(dev_extra[0])), dev_exts);
		}
		std::vector<const char *> dev_ptrs;
		for (auto &s : dev_exts) dev_ptrs.push_back(s.c_str());

		float prio = 1.0f;
		VkDeviceQueueCreateInfo qci = {};
		qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
		qci.queueFamilyIndex = vk_queue_family;
		qci.queueCount = 1;
		qci.pQueuePriorities = &prio;

		VkDeviceCreateInfo dci = {};
		dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
		dci.queueCreateInfoCount = 1;
		dci.pQueueCreateInfos = &qci;
		dci.enabledExtensionCount = (uint32_t)dev_ptrs.size();
		dci.ppEnabledExtensionNames = dev_ptrs.data();

		if (vk.vkCreateDevice(vk_physical_device, &dci, nullptr, &vk_device) != VK_SUCCESS) {
			fprintf(stderr, "[DisplayXR-SA-VK] vkCreateDevice failed\n");
			return false;
		}
		dxr_vk_load_device(&vk, vk_device);
		vk.vkGetDeviceQueue(vk_device, vk_queue_family, 0, &vk_queue);

		// --- Command pool + blit command buffer + fence ---
		VkCommandPoolCreateInfo pci = {};
		pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
		pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		pci.queueFamilyIndex = vk_queue_family;
		vk.vkCreateCommandPool(vk_device, &pci, nullptr, &vk_cmd_pool);

		VkCommandBufferAllocateInfo cbai = {};
		cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		cbai.commandPool = vk_cmd_pool;
		cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		cbai.commandBufferCount = 1;
		vk.vkAllocateCommandBuffers(vk_device, &cbai, &vk_blit_cmd);

		VkFenceCreateInfo fci = {};
		fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
		vk.vkCreateFence(vk_device, &fci, nullptr, &vk_fence);

		fprintf(stderr, "[DisplayXR-SA-VK] Device created: instance=%p device=%p qf=%u\n",
		        (void *)vk_instance, (void *)vk_device, vk_queue_family);
		return true;
#else
		(void)instance; (void)system_id; (void)gipa;
		fprintf(stderr, "[DisplayXR-SA-VK] Vulkan standalone preview is Windows-only\n");
		return false;
#endif
	}

	const void *build_session_binding(void *, void *) override
	{
		if (vk_device == VK_NULL_HANDLE) return nullptr;
		session_binding.type = XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR;
		session_binding.next = nullptr;
		session_binding.instance = vk_instance;
		session_binding.physicalDevice = vk_physical_device;
		session_binding.device = vk_device;
		session_binding.queueFamilyIndex = vk_queue_family;
		session_binding.queueIndex = 0;
		return &session_binding;
	}

	bool enumerate_atlas_images(XrSwapchain swapchain, PFN_xrEnumerateSwapchainImages pfn, uint32_t count) override
	{
		if (count > SA_MAX_SWAPCHAIN_IMAGES) count = SA_MAX_SWAPCHAIN_IMAGES;
		for (uint32_t i = 0; i < count; i++) {
			atlas_images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR;
			atlas_images[i].next = nullptr;
			atlas_images[i].image = VK_NULL_HANDLE;
		}
		XrResult r = pfn(swapchain, count, &count, (XrSwapchainImageBaseHeader *)atlas_images);
		if (XR_FAILED(r)) {
			fprintf(stderr, "[DisplayXR-SA-VK] xrEnumerateSwapchainImages failed: %d\n", r);
			return false;
		}
		atlas_image_count = count;
		return true;
	}

	void *get_atlas_image(uint32_t index) override
	{
		return (index < atlas_image_count) ? (void *)atlas_images[index].image : nullptr;
	}

	void create_atlas_bridge(uint32_t atlas_width, uint32_t atlas_height, void *) override
	{
#if defined(_WIN32)
		if (vk_device == VK_NULL_HANDLE || !vk.vkGetMemoryWin32HandleKHR) return;
		atlas_w = atlas_width;
		atlas_h = atlas_height;

		// 1) Exportable bridge image on the SA device.
		VkExternalMemoryImageCreateInfo ext = {};
		ext.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
		ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;

		VkImageCreateInfo ici = {};
		ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		ici.pNext = &ext;
		ici.imageType = VK_IMAGE_TYPE_2D;
		ici.format = VK_FORMAT_R8G8B8A8_UNORM; // matches atlas swapchain + C# RGBA32
		ici.extent = { atlas_w, atlas_h, 1 };
		ici.mipLevels = 1;
		ici.arrayLayers = 1;
		ici.samples = VK_SAMPLE_COUNT_1_BIT;
		ici.tiling = VK_IMAGE_TILING_OPTIMAL;
		ici.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
		ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

		if (vk.vkCreateImage(vk_device, &ici, nullptr, &bridge_image) != VK_SUCCESS) {
			fprintf(stderr, "[DisplayXR-SA-VK] bridge vkCreateImage failed\n");
			return;
		}

		VkMemoryRequirements mr = {};
		vk.vkGetImageMemoryRequirements(vk_device, bridge_image, &mr);

		VkMemoryDedicatedAllocateInfo ded = {};
		ded.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
		ded.image = bridge_image;
		VkExportMemoryAllocateInfo exp = {};
		exp.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
		exp.pNext = &ded;
		exp.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;

		VkMemoryAllocateInfo mai = {};
		mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		mai.pNext = &exp;
		mai.allocationSize = mr.size;
		mai.memoryTypeIndex = find_memory_type(&vk, vk_physical_device, mr.memoryTypeBits,
		                                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
		if (vk.vkAllocateMemory(vk_device, &mai, nullptr, &bridge_memory) != VK_SUCCESS) {
			fprintf(stderr, "[DisplayXR-SA-VK] bridge vkAllocateMemory failed\n");
			destroy_atlas_bridge();
			return;
		}
		vk.vkBindImageMemory(vk_device, bridge_image, bridge_memory, 0);

		VkMemoryGetWin32HandleInfoKHR gh = {};
		gh.sType = VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR;
		gh.memory = bridge_memory;
		gh.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
		if (vk.vkGetMemoryWin32HandleKHR(vk_device, &gh, &bridge_handle) != VK_SUCCESS || !bridge_handle) {
			fprintf(stderr, "[DisplayXR-SA-VK] bridge vkGetMemoryWin32HandleKHR failed\n");
			destroy_atlas_bridge();
			return;
		}

		// 2) Import the same memory on Unity's editor VkDevice.
		void *u_inst = nullptr, *u_phys = nullptr, *u_dev = nullptr, *u_queue = nullptr;
		uint32_t u_qf = 0;
		if (!displayxr_unity_get_vulkan(&u_inst, &u_phys, &u_dev, &u_queue, &u_qf)) {
			fprintf(stderr, "[DisplayXR-SA-VK] Unity Vulkan device unavailable — atlas bridge import skipped (preview will be blank)\n");
			return;
		}
		unity_instance = (VkInstance)u_inst;
		unity_device   = (VkDevice)u_dev;
		VkPhysicalDevice u_physical = (VkPhysicalDevice)u_phys;

		// Prime a dispatch table for Unity's instance/device.
		unity_vk.vkGetInstanceProcAddr = vk.vkGetInstanceProcAddr;
		dxr_vk_load_instance(&unity_vk, unity_instance);
		dxr_vk_load_device(&unity_vk, unity_device);

		VkImageCreateInfo u_ici = ici; // identical create info (external OPAQUE_WIN32)
		if (unity_vk.vkCreateImage(unity_device, &u_ici, nullptr, &unity_bridge_image) != VK_SUCCESS) {
			fprintf(stderr, "[DisplayXR-SA-VK] Unity-side bridge vkCreateImage failed\n");
			return;
		}
		VkMemoryRequirements u_mr = {};
		unity_vk.vkGetImageMemoryRequirements(unity_device, unity_bridge_image, &u_mr);

		VkMemoryDedicatedAllocateInfo u_ded = {};
		u_ded.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
		u_ded.image = unity_bridge_image;
		VkImportMemoryWin32HandleInfoKHR imp = {};
		imp.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR;
		imp.pNext = &u_ded;
		imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
		imp.handle = bridge_handle; // OPAQUE_WIN32: ownership NOT transferred

		VkMemoryAllocateInfo u_mai = {};
		u_mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		u_mai.pNext = &imp;
		u_mai.allocationSize = u_mr.size;
		u_mai.memoryTypeIndex = find_memory_type(&unity_vk, u_physical, u_mr.memoryTypeBits,
		                                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
		if (unity_vk.vkAllocateMemory(unity_device, &u_mai, nullptr, &unity_bridge_memory) != VK_SUCCESS) {
			fprintf(stderr, "[DisplayXR-SA-VK] Unity-side bridge import vkAllocateMemory failed "
			    "(likely Unity is on a different GPU than the runtime — multi-GPU not yet supported)\n");
			unity_vk.vkDestroyImage(unity_device, unity_bridge_image, nullptr);
			unity_bridge_image = VK_NULL_HANDLE;
			return;
		}
		unity_vk.vkBindImageMemory(unity_device, unity_bridge_image, unity_bridge_memory, 0);

		fprintf(stderr, "[DisplayXR-SA-VK] Atlas bridge: %ux%u, SA image=%p, Unity image=%p\n",
		        atlas_w, atlas_h, (void *)bridge_image, (void *)unity_bridge_image);
#else
		(void)atlas_width; (void)atlas_height;
#endif
	}

	void destroy_atlas_bridge() override
	{
#if defined(_WIN32)
		if (unity_device != VK_NULL_HANDLE) {
			if (unity_bridge_image)  { unity_vk.vkDestroyImage(unity_device, unity_bridge_image, nullptr);  unity_bridge_image = VK_NULL_HANDLE; }
			if (unity_bridge_memory) { unity_vk.vkFreeMemory(unity_device, unity_bridge_memory, nullptr);   unity_bridge_memory = VK_NULL_HANDLE; }
		}
		if (bridge_handle) { CloseHandle(bridge_handle); bridge_handle = nullptr; }
		if (vk_device != VK_NULL_HANDLE) {
			if (bridge_image)  { vk.vkDestroyImage(vk_device, bridge_image, nullptr);  bridge_image = VK_NULL_HANDLE; }
			if (bridge_memory) { vk.vkFreeMemory(vk_device, bridge_memory, nullptr);   bridge_memory = VK_NULL_HANDLE; }
		}
		bridge_initialized = false;
#endif
	}

	void *get_atlas_bridge_unity_ptr() override
	{
#if defined(_WIN32)
		// Unity's Texture2D.CreateExternalTexture on Vulkan expects a POINTER to
		// the VkImage handle — NOT the handle value (the way D3D passes the
		// resource pointer). Passing the value makes Unity dereference the handle
		// as an address inside RegisterNativeTextureWithParams → garbage VkImage →
		// vkCreateImageView crashes in the driver. Return the address of the
		// stable member (valid for the backend's lifetime).
		return (void *)&unity_bridge_image;
#else
		return nullptr;
#endif
	}

	void blit_atlas(void * /*atlas_tex*/, uint32_t index) override
	{
#if defined(_WIN32)
		if (bridge_image == VK_NULL_HANDLE || index >= atlas_image_count) return;
		VkImage dst = atlas_images[index].image;
		if (dst == VK_NULL_HANDLE || vk_blit_cmd == VK_NULL_HANDLE) return;

		// Coarse cross-device sync: a full FrameTick separates Unity's
		// CopyTexture into the bridge from this read, so a 1-frame-stale atlas
		// is the worst case (invisible in a preview). If tearing/corruption
		// appears on hardware, the fix is a shared timeline semaphore between
		// Unity's queue and ours (tracked as a follow-up). See plan §risks.
		vk.vkQueueWaitIdle(vk_queue);
		vk.vkResetCommandBuffer(vk_blit_cmd, 0);

		VkCommandBufferBeginInfo bi = {};
		bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		vk.vkBeginCommandBuffer(vk_blit_cmd, &bi);

		auto barrier = [](VkImage img, VkImageLayout oldL, VkImageLayout newL,
		                  VkAccessFlags srcA, VkAccessFlags dstA) {
			VkImageMemoryBarrier b = {};
			b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
			b.oldLayout = oldL; b.newLayout = newL;
			b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			b.image = img;
			b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
			b.srcAccessMask = srcA; b.dstAccessMask = dstA;
			return b;
		};

		// Bridge (src): aliases Unity-written memory. Use GENERAL as the steady
		// layout and ALL_COMMANDS scope to conservatively cover Unity's writes.
		VkImageLayout bridge_old = bridge_initialized ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
		VkImageMemoryBarrier to_src = barrier(bridge_image, bridge_old,
		    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
		vk.vkCmdPipelineBarrier(vk_blit_cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
		    0, 0, nullptr, 0, nullptr, 1, &to_src);

		// Swapchain image (dst): UNDEFINED old layout — we overwrite the whole
		// image so its prior contents need not be preserved.
		VkImageMemoryBarrier to_dst = barrier(dst, VK_IMAGE_LAYOUT_UNDEFINED,
		    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
		vk.vkCmdPipelineBarrier(vk_blit_cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
		    0, 0, nullptr, 0, nullptr, 1, &to_dst);

		VkImageCopy region = {};
		region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
		region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
		region.extent = { atlas_w, atlas_h, 1 };
		vk.vkCmdCopyImage(vk_blit_cmd,
		    bridge_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		    dst,          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		    1, &region);

		// Restore: bridge -> GENERAL; swapchain -> COLOR_ATTACHMENT_OPTIMAL
		// (the layout the runtime expects at release).
		VkImageMemoryBarrier back_src = barrier(bridge_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		    VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_MEMORY_WRITE_BIT);
		vk.vkCmdPipelineBarrier(vk_blit_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
		    0, 0, nullptr, 0, nullptr, 1, &back_src);

		VkImageMemoryBarrier back_dst = barrier(dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
		vk.vkCmdPipelineBarrier(vk_blit_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		    0, 0, nullptr, 0, nullptr, 1, &back_dst);

		vk.vkEndCommandBuffer(vk_blit_cmd);

		vk.vkResetFences(vk_device, 1, &vk_fence);
		VkSubmitInfo si = {};
		si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		si.commandBufferCount = 1;
		si.pCommandBuffers = &vk_blit_cmd;
		vk.vkQueueSubmit(vk_queue, 1, &si, vk_fence);
		vk.vkWaitForFences(vk_device, 1, &vk_fence, VK_TRUE, UINT64_MAX);
		bridge_initialized = true;
#else
		(void)index;
#endif
	}

	void destroy() override
	{
		destroy_atlas_bridge();
		destroy_shared_texture();
#if defined(_WIN32)
		if (vk_device != VK_NULL_HANDLE) {
			vk.vkDeviceWaitIdle(vk_device);
			if (vk_fence)    { vk.vkDestroyFence(vk_device, vk_fence, nullptr);          vk_fence = VK_NULL_HANDLE; }
			if (vk_cmd_pool) { vk.vkDestroyCommandPool(vk_device, vk_cmd_pool, nullptr); vk_cmd_pool = VK_NULL_HANDLE; }
			vk_blit_cmd = VK_NULL_HANDLE;
			vk.vkDestroyDevice(vk_device, nullptr); vk_device = VK_NULL_HANDLE;
		}
		if (vk_instance != VK_NULL_HANDLE) { vk.vkDestroyInstance(vk_instance, nullptr); vk_instance = VK_NULL_HANDLE; }
		vk_physical_device = VK_NULL_HANDLE;
		vk_queue = VK_NULL_HANDLE;
#endif
	}

	// --- Methods not on the editor-preview critical path (faithful stubs) ---
	// create_shared_texture / get_shared_texture_native_ptr back the
	// shared-texture-output (GameView/readback) path, and fw_* back a fullscreen
	// present path; neither is exercised by the windowed editor preview that
	// #124 targets. Implemented as no-ops; the runtime composites into the
	// window-bound HWND directly.
	bool create_shared_texture(uint32_t, uint32_t) override { return false; }
	void destroy_shared_texture() override {}
	void *get_shared_texture_native_ptr() override { return nullptr; }
	bool fw_create_swapchain(void *, uint32_t, uint32_t) override { return false; }
	void fw_destroy_swapchain() override {}
	void fw_resize_swapchain_buffers(uint32_t, uint32_t) override {}
	void fw_present(uint32_t, uint32_t) override {}

	// Vulkan device comes from IUnityGraphicsVulkan, not a native texture ptr.
	void set_unity_device(void *) override {}
	void *get_graphics_device() override { return (void *)vk_device; }
	void *get_graphics_queue() override { return (void *)vk_queue; }
};

StandaloneGraphicsBackend *create_standalone_vulkan_backend() { return new StandaloneVulkanBackend(); }

#endif // ENABLE_VULKAN || Android || Linux
