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
#include "displayxr_window_space_ui.h" // DXR_WSUI_MAX_SLOTS, get_pending_slot
#if defined(__linux__) && !defined(__ANDROID__)
#include "displayxr_linux_wayland.h"
#endif

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

// Custom IUnityXRDisplay Display Provider registration (epic #166, M1).
// Implemented in displayxr_xrprovider/displayxr_display_provider.cpp.
extern "C" void displayxr_register_xr_display_provider(IUnityInterfaces *ifaces);
extern "C" void displayxr_unregister_xr_display_provider(void);

#ifdef __APPLE__
// Metal glue (#204): stash IUnityInterfaces so the provider can fetch
// IUnityGraphicsMetal (device/queue) on the graphics thread.
#include "displayxr_xrprovider/displayxr_provider_gfx_metal.h"
#endif

static IUnityInterfaces *s_unity_ifaces = nullptr;
static IUnityGraphics   *s_unity_gfx    = nullptr;
#if defined(DXR_HAVE_UNITY_VULKAN)
static IUnityGraphicsVulkan *s_unity_vk = nullptr;   // V1 view (works for both)
static UnityVulkanInstance   s_vk_inst  = {};
static bool                  s_vk_captured = false;

// IUnityGraphicsVulkan::Instance() is stable between device init and shutdown,
// so we can capture it lazily on first request as well as on the init event.
#if defined(ENABLE_VULKAN)
// ---------------------------------------------------------------------------
// 2D overlay copy event (#336)
// ---------------------------------------------------------------------------
//
// On Vulkan the Local2D/wsui canvas RT can't be Graphics.CopyTexture'd into a
// wrapped bridge the way the D3D path does (see displayxr_provider_gfx_vulkan.h).
// C# issues this event instead (GL.IssuePluginEvent); on Unity's render thread we
// let Unity transition the RT to TRANSFER_SRC (AccessTexture) and record the copy
// into the overlay bridge on Unity's own command buffer.
//
// Keep the ids in sync with DisplayXRProviderNative.kVkOverlayCopy*Event.
#define DXR_EVENT_VK_OVERLAY_COPY_LOCAL2D 0x44585201
// wsui slot N is DXR_EVENT_VK_OVERLAY_COPY_WSUI0 + N (N < DXR_WSUI_MAX_SLOTS).
#define DXR_EVENT_VK_OVERLAY_COPY_WSUI0   0x44585210

extern "C" int displayxr_local2d_get_pending(void **out_tex, int *out_w, int *out_h);
extern "C" int dxr_pvk_overlay_ready(int kind);
extern "C" int dxr_pvk_overlay_record_unity_copy(int kind, void *cmd_buf, void *src_image,
                                                 int64_t src_format, uint32_t src_w, uint32_t src_h,
                                                 void *src_id);

static void configure_vulkan_events(void)
{
	// Outside a render pass (a transfer can't be recorded inside one), no queue
	// access (we only record into Unity's command buffer), and no command-buffer
	// state is changed, so ModifiesCommandBuffersState is left clear.
	UnityVulkanPluginEventConfig cfg = {};
	cfg.renderPassPrecondition = kUnityVulkanRenderPass_EnsureOutside;
	cfg.graphicsQueueAccess = kUnityVulkanGraphicsQueueAccess_DontCare;
	cfg.flags = kUnityVulkanEventConfigFlag_EnsurePreviousFrameSubmission;
	s_unity_vk->ConfigureEvent(DXR_EVENT_VK_OVERLAY_COPY_LOCAL2D, &cfg);
	for (int i = 0; i < DXR_WSUI_MAX_SLOTS; i++)
		s_unity_vk->ConfigureEvent(DXR_EVENT_VK_OVERLAY_COPY_WSUI0 + i, &cfg);
}

static void vk_overlay_copy(int kind, void *tex, int w, int h)
{
	// Only once the provider has created the bridge (render thread, at submit).
	if (!dxr_pvk_overlay_ready(kind)) return;
	UnityVulkanImage img = {};
	if (!s_unity_vk->AccessTexture(tex, UnityVulkanWholeImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	                               VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT,
	                               kUnityVulkanResourceAccess_PipelineBarrier, &img)) {
		static bool warned = false;
		if (!warned) {
			warned = true;
			fprintf(stderr, "[DisplayXR] Vulkan overlay copy: AccessTexture failed (tex=%p)\n", tex);
		}
		return;
	}
	// After AccessTexture: resource access may record commands and invalidates any
	// recording state fetched earlier.
	UnityVulkanRecordingState st = {};
	if (!s_unity_vk->CommandRecordingState(&st, kUnityVulkanGraphicsQueueAccess_DontCare) ||
	    st.commandBuffer == VK_NULL_HANDLE)
		return;
	uint32_t sw = img.extent.width ? img.extent.width : (uint32_t)w;
	uint32_t sh = img.extent.height ? img.extent.height : (uint32_t)h;
	dxr_pvk_overlay_record_unity_copy(kind, (void *)st.commandBuffer, (void *)img.image,
	                                  (int64_t)img.format, sw, sh, tex);
}
#endif

static void UNITY_INTERFACE_API on_render_event(int event_id)
{
#if defined(ENABLE_VULKAN)
	if (!s_unity_vk || !s_vk_captured) return;
	if (event_id == DXR_EVENT_VK_OVERLAY_COPY_LOCAL2D) {
		void *tex = NULL; int w = 0, h = 0;
		if (displayxr_local2d_get_pending(&tex, &w, &h)) vk_overlay_copy(0 /* LOCAL2D */, tex, w, h);
	} else if (event_id >= DXR_EVENT_VK_OVERLAY_COPY_WSUI0 &&
	           event_id < DXR_EVENT_VK_OVERLAY_COPY_WSUI0 + DXR_WSUI_MAX_SLOTS) {
		int slot = event_id - DXR_EVENT_VK_OVERLAY_COPY_WSUI0;
		void *tex = NULL; int w = 0, h = 0;
		float x, y, lw, lh, disp;
		if (displayxr_window_space_ui_get_pending_slot(slot, &tex, &w, &h, &x, &y, &lw, &lh, &disp))
			vk_overlay_copy(1 + slot /* DXR_PVK_OVERLAY_WSUI0 + slot */, tex, w, h);
	}
#else
	(void)event_id;
#endif
}

static void capture_vulkan_instance(void)
{
	if (s_vk_captured || !s_unity_vk) return;
	if (!s_unity_gfx || s_unity_gfx->GetRenderer() != kUnityGfxRendererVulkan) return;
	s_vk_inst = s_unity_vk->Instance();
	if (s_vk_inst.device != VK_NULL_HANDLE) {
		s_vk_captured = true;
#if defined(ENABLE_VULKAN)
		configure_vulkan_events();
#endif
#if defined(__linux__) && !defined(__ANDROID__)
		// Unity's real VkInstance exists, with the native-Wayland capture layer in
		// its chain if it was armed: take the layer out of the environment now.
		dxr_wl_capture_disarm("Unity's graphics device is up");
#endif
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

#ifdef __APPLE__
	dxr_prov_metal_set_unity_ifaces(s_unity_ifaces);
#endif

	// Register the custom IUnityXRDisplay Display Provider (epic #166, M1).
	// This is what makes Unity drive the DisplayXR runtime as a first-class
	// display subsystem (instead of the OpenXR hook). Safe even when the
	// DisplayXR display subsystem isn't selected in XR Plug-in Management —
	// RegisterLifecycleProvider just sits idle until Unity starts it.
	displayxr_register_xr_display_provider(s_unity_ifaces);
}

extern "C" void UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API
UnityPluginUnload(void)
{
	displayxr_unregister_xr_display_provider();
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

// Render-event entry point for GL.IssuePluginEvent (#336). Valid on every backend;
// the callback only does work on Vulkan.
extern "C" UNITY_INTERFACE_EXPORT UnityRenderingEvent UNITY_INTERFACE_API
dxr_prov_get_render_event_func(void)
{
#if defined(DXR_HAVE_UNITY_VULKAN)
	return on_render_event;
#else
	return nullptr;
#endif
}

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
