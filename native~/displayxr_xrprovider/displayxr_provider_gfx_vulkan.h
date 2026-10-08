// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Vulkan graphics glue for the IUnityXRDisplay provider (#247).
//
// WHY THIS IS AN OWN-DEVICE BRIDGE AND NOT ZERO-COPY
// --------------------------------------------------
// The tempting shape is the D3D11 one: bind the OpenXR session directly to
// Unity's own VkDevice, so the runtime's swapchain images ARE Unity textures and
// nothing is copied. Two facts kill it:
//
//  1. The runtime needs XR_KHR_vulkan_enable2. Under enable1 the app owns the
//     VkDevice, so the runtime can never request a queue of its own and the #868
//     weave-rate-decoupling repaint silently stays off (the runtime WARNs "#886:"
//     about exactly this at session create). enable2 means the runtime creates
//     the VkInstance/VkDevice via xrCreateVulkanInstanceKHR /
//     xrCreateVulkanDeviceKHR — it does not accept a device we made earlier.
//  2. Unity's VkDevice could in principle be routed through those calls via
//     IUnityGraphicsVulkan::InterceptInitialization, but that hook must run
//     before kUnityGfxDeviceEventInitialize. In editor Play Mode our DLL is
//     loaded from the subsystem manifest LONG after Unity's graphics device
//     exists (Editor.log: "Forcing GfxDevice: Vulkan" at line 123 vs "Loading
//     plugin displayxr_unity" at line 826). Play Mode *is* the shipping preview
//     workflow here, so an editor-only hole is not acceptable.
//
// So the session runs on a runtime-created VkDevice and we bridge to Unity's
// separate VkDevice with external-memory images plus an external semaphore for
// cross-device ordering. That is the DXR_GFX_D3D12 own-device-bridge shape
// expressed in Vulkan: own device + shared 2-slice array + per-frame copy, with
// the semaphore playing the role of the shared ID3D12Fence. Intercept-based
// zero-copy stays available as a player-only optimisation later.
//
// PLATFORMS: Windows (#247) and desktop Linux (#249). The two differ only in the
// external-memory handle flavour — OPAQUE_WIN32 `HANDLE`s vs OPAQUE_FD file
// descriptors — which the .cpp isolates behind a handful of PVK_* macros. Vulkan
// on macOS is out of scope (Metal is the macOS backend, #202/#204), and Android
// is a separate leg: its runtime is out-of-process, so this in-process bridge is
// the wrong shape for it.

#pragma once

#include <stdint.h>

#if defined(ENABLE_VULKAN)

#include "../displayxr_vk_loader.h"
#include "../displayxr_window_space_ui.h" // DXR_WSUI_MAX_SLOTS

#define XR_USE_GRAPHICS_API_VULKAN 0 // types are inlined in the .cpp, like D3D11/D3D12
#include <openxr/openxr.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Create the session's Vulkan device through XR_KHR_vulkan_enable2.
///
/// Runs the full enable2 sequence: xrGetVulkanGraphicsRequirements2KHR ->
/// xrCreateVulkanInstanceKHR -> xrGetVulkanGraphicsDevice2KHR ->
/// xrCreateVulkanDeviceKHR, then grabs the graphics queue and creates the
/// command pool/buffer/fence used by the per-frame bridge copy. Because the
/// RUNTIME performs the vkCreateDevice, it can inject its own queue request —
/// which is the entire point (#886 / #868).
///
/// Also performs the cross-adapter guard: if the physical device the runtime
/// selected is not the one Unity is rendering on, the bridge would be
/// cross-adapter and present black with a fully healthy-looking session (the VK
/// form of #240). We refuse loudly instead, naming both adapters.
///
/// @param instance   The XrInstance (must have been created with
///                   XR_KHR_vulkan_enable2 enabled).
/// @param system_id  The XrSystemId from xrGetSystem.
/// @param gipa       xrGetInstanceProcAddr for `instance`.
/// @param unity_physical_device  Unity's VkPhysicalDevice, for the LUID guard.
///                   May be VK_NULL_HANDLE, which downgrades the guard to a WARN.
/// @return 1 on success, 0 on failure (session must not be started).
int dxr_pvk_create_device(XrInstance instance, XrSystemId system_id,
                          PFN_xrGetInstanceProcAddr gipa,
                          void *unity_physical_device);

/// Fill an XrGraphicsBindingVulkan2KHR for xrCreateSession, chaining `next`
/// (the win32 window binding). Returns a pointer to storage owned by this TU,
/// valid until dxr_pvk_destroy_device(). NULL if the device was never created.
const void *dxr_pvk_session_binding(const void *next);

/// Adopt Unity's Vulkan objects, captured from IUnityGraphicsVulkan by
/// displayxr_unity_plugin.cpp. Must be called BEFORE dxr_pvk_create_device so
/// the LUID guard has something to compare against, and before any bridge call
/// (the Unity-side import happens on this device).
void dxr_pvk_set_unity_objects(void *instance, void *physical_device,
                               void *device, uint32_t queue_family, void *queue);

/// Record the session swapchain's VkImages (from xrEnumerateSwapchainImages with
/// XrSwapchainImageVulkan2KHR). `images` is an array of `count` VkImage handles
/// living on the SESSION device. Also records the format/extent needed by the
/// per-frame copy.
void dxr_pvk_set_swapchain_images(const void *images, uint32_t count,
                                  uint32_t width, uint32_t height,
                                  uint32_t array_size, int64_t format);

/// Create the eye bridge: a `array_size`-layer VkImage on the SESSION device
/// exported as external memory (OPAQUE_WIN32 / OPAQUE_FD), imported as a matching
/// VkImage on UNITY's device, plus the ordering semaphore. Unity renders into the Unity-side
/// image; dxr_pvk_copy_to_swapchain_image() copies the session-side alias into
/// the acquired swapchain image.
///
/// `eye` selects which bridge slot: -1 = the single SPI bridge (array_size == 2),
/// 0/1 = the per-eye MultiPass bridges (array_size == 1 each). This mirrors the
/// D3D12 bridge's SPI/MultiPass split.
int dxr_pvk_create_bridge(int eye, uint32_t width, uint32_t height,
                          uint32_t array_size, int64_t format);

/// The value to put in UnityXRRenderTextureDesc::color.nativePtr for bridge slot
/// `eye`: a pointer to a populated UnityVulkanImage. Storage is owned here and
/// outlives the texture.
///
/// TRAP, and note it is NOT the same trap as the standalone backend's: Unity has
/// two different Vulkan external-texture entry points with different contracts.
///   - Texture2D.CreateExternalTexture (C#) -> RegisterNativeTextureWithParams
///     wants a POINTER TO a bare VkImage handle. That is what the old standalone
///     backend documented.
///   - The XR display provider's CreateTexture path goes through
///     vk::Texture::CreateFromExternalNativeImage ->
///     vk::ImageManager::CreateImageFromExternalNativeImage, which reads a whole
///     UnityVulkanImage. It needs format/aspect/extent/layers/mipCount to build
///     image views, and NONE of that is queryable from a VkImage handle.
/// Passing a bare &VkImage here makes Unity read ~100 bytes of struct out of an
/// 8-byte handle and build views from garbage — observed as a hard crash inside
/// the NVIDIA driver under vk::Image::CreateImageViews.
void *dxr_pvk_unity_image_ptr(int eye);

/// Per-frame: copy bridge slot `eye` into swapchain image `image_index`, with
/// the layout barriers and cross-device semaphore wait. `eye` == -1 copies the
/// whole 2-layer SPI bridge; 0/1 copy that eye into the matching array slice.
/// Returns 1 on success.
int dxr_pvk_copy_to_swapchain_image(int eye, uint32_t image_index);

/// Signal the ordering semaphore from UNITY's queue, so the session-device copy
/// above waits for Unity's renders to land. Called from the render thread after
/// Unity has submitted the eye work.
void dxr_pvk_signal_unity_done(void);

// ---------------------------------------------------------------------------
// 2D overlay layers (Local2D, window-space UI) — #336
// ---------------------------------------------------------------------------
//
// Same bridge shape as the eye bridge (an external-memory image aliased on both
// devices, parked in GENERAL), one per layer, but the UNITY side is filled
// differently. The eyes are an XR render target Unity renders into directly; an
// overlay's source is an ordinary Unity RenderTexture. Its layout is tracked by
// Unity and only reachable through IUnityGraphicsVulkan::AccessTexture inside a
// plugin event, so the Unity-side copy is recorded into Unity's own command
// buffer from that event (dxr_pvk_overlay_record_unity_copy) rather than done
// with Graphics.CopyTexture into a wrapped bridge. The session side then copies
// the bridge into the layer's swapchain image at submit, exactly like the eyes.

enum {
	DXR_PVK_OVERLAY_LOCAL2D = 0,
	DXR_PVK_OVERLAY_WSUI0 = 1,   // wsui slot N is DXR_PVK_OVERLAY_WSUI0 + N
	DXR_PVK_OVERLAY_COUNT = 1 + DXR_WSUI_MAX_SLOTS, // Local2D + one per wsui slot
};

/// Record the overlay layer's swapchain VkImages (session device) and format.
void dxr_pvk_overlay_set_swapchain_images(int kind, const void *images, uint32_t count,
                                          int64_t format);

/// Create (or keep, if already that size) the overlay bridge. Must run on the
/// render thread: it shares the session command buffer with the per-frame copy.
int dxr_pvk_overlay_create_bridge(int kind, uint32_t width, uint32_t height, int64_t format);

/// 1 while the overlay bridge exists. Safe from any thread.
int dxr_pvk_overlay_ready(int kind);

/// 1 once the bridge exists and Unity has copied into it at least once. Until
/// then its memory is uninitialised, so the layer must not be submitted.
/// Render thread only.
int dxr_pvk_overlay_has_content(int kind);

/// 1 while the bridge exists and wants a Unity copy: after it was created, or
/// after dxr_pvk_overlay_request_copy, until a copy is actually RECORDED (an event
/// that had to skip leaves it set, so the copy is retried). Safe from any thread.
int dxr_pvk_overlay_needs_copy(int kind);

/// Ask for a Unity copy (the canvas re-rendered). Safe from any thread.
void dxr_pvk_overlay_request_copy(int kind);

/// Plugin-event side (Unity's render thread): record a copy of `src_image` (a
/// Unity RenderTexture already transitioned to TRANSFER_SRC_OPTIMAL by
/// AccessTexture) into the overlay bridge's Unity-side alias, on Unity's
/// `cmd_buf`. `src_format` is the source VkFormat; an RGBA source is blitted so
/// the channels land right in a BGRA bridge. `src_id` is the texture's registered
/// native pointer, remembered as the bridge content's source (see
/// dxr_pvk_overlay_content_source). Returns 1 if a copy was recorded.
int dxr_pvk_overlay_record_unity_copy(int kind, void *cmd_buf, void *src_image,
                                      int64_t src_format, uint32_t src_w, uint32_t src_h,
                                      void *src_id);

/// The registered texture the bridge's current content was copied from. Submit
/// compares it with the texture registered NOW, so a slot taken over by another
/// component never shows the previous owner's last image. Render thread only.
void *dxr_pvk_overlay_content_source(int kind);

/// Per-frame session side: copy the overlay bridge into swapchain image `image_index`.
int dxr_pvk_overlay_copy_to_swapchain_image(int kind, uint32_t image_index);

/// Drop the overlay's bridge and swapchain images (layer resize / teardown).
void dxr_pvk_overlay_destroy(int kind);

/// Tear down the per-session objects (bridges, overlays, fence, command pool).
/// Call BEFORE xrEndSession. Leaves the device and instance alive: the session
/// still runs on them (DisplayXR/displayxr-runtime#1779). Safe to call when
/// nothing was created.
void dxr_pvk_destroy(void);

/// Destroy the VkDevice + VkInstance made through XR_KHR_vulkan_enable2. Call
/// only AFTER xrDestroySession: the runtime's compositor (its repaint thread
/// included) uses them until then. Safe to call when nothing was created.
void dxr_pvk_destroy_device(void);

/// Keep this session's VkInstance instead of destroying it in
/// dxr_pvk_destroy_device() (the device still goes); kept instances are held in a
/// process static and counted in the log. For the native-Wayland player only: the
/// runtime made its weave surface with that instance on the PLAYER's wl_display,
/// and with at least one current desktop driver destroying it tears down WSI state
/// the driver shares per wl_display, so the player's own swapchain destroy then
/// crashes (PC 0, inside the driver) at exit. Measured: 8/8 runs crash when it is
/// destroyed, whether the weave's wl_surface goes before or after it; 0 when kept.
/// The cost is one instance per session restart. Reset by dxr_pvk_destroy_device().
void dxr_pvk_keep_instance_alive(void);

/// 1 once dxr_pvk_create_device() has succeeded.
int dxr_pvk_device_ready(void);

#ifdef __cplusplus
}
#endif

#endif // ENABLE_VULKAN
