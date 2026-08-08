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

**Repro steps** (minimal project attached)
1. Windows Standalone player, Graphics APIs = Vulkan first.
2. XR Management loader implements `IXRLoaderPreInit` → boot.config gets
   `xrsdk-pre-init-library=<plugin>`; plugin exports `XRSDKPreInit` and registers
   a `UnityXRPreInitProvider`.
3. Observe `GetGraphicsAdapterId(renderer=Vulkan, rendererData=NULL)` called once
   at boot (defect 1). Return any value → D3D12 silent fallback; return false →
   continue.
4. Provider's display subsystem starts; first CreateTexture → crash (defect 2).

**Impact:** third-party XR display providers cannot target Vulkan on Windows
Standalone at all in Unity 6. D3D11/D3D12/Metal identical code paths work.

---

### Evidence appendix (attach logs)
- Player.log showing: `xrsdk-pre-init-library` honored, extension-merge calls,
  `SelectPhysicalDevice ... xrDevice=...`, `Could not select a physical device`,
  silent `Direct3D: Version: Direct3D 12` fallback, and the crash stack.
- Provider native log showing the bisect matrix (external image / bare handle /
  descriptor / Unity-allocated → identical crash).
