# Unity bug report draft — Vulkan + third-party XR display provider (6000.4)

> Submit via **Help > Report a Bug** from Unity 6000.4.0f1 (attach a minimal
> project: any URP project + an XR Management loader whose `IXRLoaderPreInit`
> names a native plugin exporting `XRSDKPreInit`). Text below is paste-ready.
> Two defects, one report — they compound.

---

**Title:** Vulkan: XR pre-init `GetGraphicsAdapterId` cannot be answered by any
provider (called pre-instance, matched by raw handle), and XR texture creation
crashes in `vk::Image::CreateImageViews` when `xrDevice` is unset

**Unity version:** 6000.4.0f1 (8cf496087c8f), Windows 11, NVIDIA RTX 3080 Laptop
(driver 32.0.15.8157) + Intel UHD hybrid.

**Summary**

A third-party XR display provider (custom `IUnityXRDisplay` subsystem +
`UnityXRPreInitProvider` registered from the boot.config
`xrsdk-pre-init-library` mechanism) cannot run under Vulkan in 6000.4:

**Defect 1 — the adapter query is unsatisfiable.**
`UnityXRPreInitProvider::GetGraphicsAdapterId` is invoked exactly once, at engine
boot, with `renderer=kUnityXRPreInitRendererVulkan` and `rendererData == NULL`
(no `VkInstance` exists yet), and the answer is cached. During device init,
`SelectPhysicalDevice` matches the cached value against the enumerated
`VkPhysicalDevice` handles of the engine's own (later-created) instance by raw
handle equality (a `std::find` over the handle array — UnityPlayer.dll
6000.4.0f1 x64, function containing the "Could not select a physical device"
log, rva 0x10c47c0). Since Vulkan physical-device handles are per-instance
loader objects, **no value a provider can produce at pre-instance time can ever
match**:
- returning a `VkPhysicalDevice` from a provider-created instance → no match;
- returning the 8-byte `deviceLUID` packed in the uint64 → no match;
- any non-match → `[Vulkan init] Selected physical device 0000000000000000` /
  `Could not select a physical device` → **the engine silently falls back to
  D3D12** (no warning that the requested API was abandoned).

Expected: either call `GetGraphicsAdapterId` (again) once the real `VkInstance`
exists (passing it as `rendererData`, as the header documents), or match an
instance-independent identity (LUID/UUID), or at minimum log the API fallback.

**Defect 2 — XR texture creation crashes when `xrDevice` was never set.**
If the provider declines `GetGraphicsAdapterId` (the only non-destructive
option, see above), the engine stays on Vulkan with `xrDevice=0`. The XR session
runs, but the first `IUnityXRDisplayInterface::CreateTexture` from the provider
crashes the process inside:

    vk::Image::CreateImageViews
    vk::ImageManager::CreateImageFromExternalNativeImage
    vk::Texture::CreateFromExternalNativeImage
    vk::RenderSurface::CreateColorSurfaceImpl
    GfxDeviceVK::CreateColorRenderSurfacePlatform

The crash is **invariant across every input the provider controls** (all
bisected on hardware): an imported external `VkImage` (by value), a
`UnityVulkanImage*` descriptor, and — decisively — a **Unity-allocated** colour
target (`nativePtr = kUnityXRRenderTextureIdDontCare`), with the provider's
extra instance/device extensions merged and with
`kUnityXRPreInitFlagsUseVulkanOffscreenSwapchain` set. The same provider code
path works correctly on D3D11, D3D12, and Metal.

**Repro steps** (project attached; uses the open-source DisplayXR provider,
which now works around this bug by default — the env var below disables the
workaround to expose the defect)
1. Install the DisplayXR OpenXR runtime (github.com/DisplayXR/displayxr-runtime
   releases; with no 3D panel present it falls back to a simulated display
   automatically — no hardware needed).
2. Open the attached project. Build a Windows Standalone player (Graphics APIs
   list is already Vulkan-first with D3D12 second).
3. Run the player with environment variable `DISPLAYXR_VK_EXPERIMENTAL=1`
   (without it, the plugin demonstrates defect 1 instead: it deliberately
   answers the adapter query with a non-matching sentinel, and Player.log shows
   "Could not select a physical device" followed by a SILENT fall-back to
   D3D12 — no warning that the configured API was abandoned).
4. With the env var set, the provider declines the adapter query, Unity proceeds
   on Vulkan, the XR session starts, and the first XR texture create crashes
   (defect 2) — stack in the evidence logs.

**Impact:** third-party XR display providers cannot target Vulkan on Windows
Standalone at all in Unity 6. D3D11/D3D12/Metal identical code paths work.

---

### Evidence appendix (attach logs)
- Player.log showing: `xrsdk-pre-init-library` honored, extension-merge calls,
  `SelectPhysicalDevice ... xrDevice=...`, `Could not select a physical device`,
  silent `Direct3D: Version: Direct3D 12` fallback, and the crash stack.
- Provider native log showing the bisect matrix (external image / bare handle /
  descriptor / Unity-allocated → identical crash).
