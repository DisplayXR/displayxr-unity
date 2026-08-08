// Unity XR SDK pre-init interface (vendored, like the other IUnity* headers here).
//
// Mechanism (#247): when boot.config carries `xrsdk-pre-init-library=<libname>`
// (written at build time by XR Management's XRGeneralBuildProcessor for the active
// loader's IXRLoaderPreInit.GetPreInitLibraryName), the engine loads that library
// BEFORE creating the graphics device and calls its exported
//
//     extern "C" void UNITY_INTERFACE_API XRSDKPreInit(IUnityInterfaces*);
//
// which registers a UnityXRPreInitProvider. The engine then consults the provider
// during graphics-device creation — on Vulkan this is what makes
// `[Vulkan init] SelectPhysicalDevice ... xrDevice=` non-NULL, lets the provider
// pick the VkPhysicalDevice (GetGraphicsAdapterId, rendererData = VkInstance),
// and merges extra instance/device extensions into Unity's own VkInstance/VkDevice
// creation. Without a registered pre-init provider, Unity's XR-aware Vulkan
// render-surface path runs with xrDevice=NULL and hard-crashes in
// vk::Image::CreateImageViews as soon as an XR texture is created — the #247
// blocker this header exists to fix.
//
// Definitions match the public Unity XR SDK ProviderInterface header
// (IUnityXRPreInit.h as shipped in e.g. ValveSoftware/unity-xr-plugin).

#pragma once

#include "IUnityInterface.h"
#include <stdint.h>
#include <stdbool.h>

typedef enum UnityXRPreInitRenderer
{
    kUnityXRPreInitRendererNull,
    kUnityXRPreInitRendererD3D11,
    kUnityXRPreInitRendererD3D12,
    kUnityXRPreInitRendererGLES20,
    kUnityXRPreInitRendererGLES3x,
    kUnityXRPreInitRendererMetal,
    kUnityXRPreInitRendererGLCore,
    kUnityXRPreInitRendererVulkan
} UnityXRPreInitRenderer;

typedef enum UnityXRPreInitFlags
{
    kUnityXRPreInitFlagsEGLUsePBuffer = (1 << 0),
    kUnityXRPreInitFlagsEGLUseNoErrorContext = (1 << 1),
    kUnityXRPreInitFlagsDisableMainDisplayFramebuffer = (1 << 2),
    kUnityXRPreInitFlagsRequestAdditionalVulkanGraphicsQueue = (1 << 3),
    kUnityXRPreInitFlagsUseVulkanOffscreenSwapchain = (1 << 4)
} UnityXRPreInitFlags;

typedef struct UnityXRPreInitProvider
{
    void* userData;

    /// OR of UnityXRPreInitFlags. Return true if flags were written.
    bool(UNITY_INTERFACE_API * GetPreInitFlags)(void* userData, uint64_t* flags);

    /// The adapter Unity should create its graphics device on. For Vulkan,
    /// rendererData is the VkInstance and *adapterId receives the
    /// VkPhysicalDevice; for D3D it is the adapter LUID; for Metal the
    /// id<MTLDevice>. Return false to let Unity choose.
    bool(UNITY_INTERFACE_API * GetGraphicsAdapterId)(void* userData,
        UnityXRPreInitRenderer renderer, uint64_t rendererData, uint64_t* adapterId);

    /// Space-separated Vulkan instance extensions to merge into Unity's
    /// VkInstance creation. Query-size convention: with namesCapacityIn==0 write
    /// the required capacity (incl. NUL) to *namesCountOut and return true;
    /// otherwise fill namesString (capped at namesCapacityIn) and set
    /// *namesCountOut to the written length.
    bool(UNITY_INTERFACE_API * GetVulkanInstanceExtensions)(void* userData,
        uint32_t namesCapacityIn, uint32_t* namesCountOut, char* namesString);

    /// Same convention, for VkDevice extensions.
    bool(UNITY_INTERFACE_API * GetVulkanDeviceExtensions)(void* userData,
        uint32_t namesCapacityIn, uint32_t* namesCountOut, char* namesString);
} UnityXRPreInitProvider;

UNITY_DECLARE_INTERFACE(IUnityXRPreInit)
{
    void(UNITY_INTERFACE_API * RegisterPreInitProvider)(UnityXRPreInitProvider * provider);
};
UNITY_REGISTER_INTERFACE_GUID(0x4E5EB567159F4848ULL, 0x9969601F505A455EULL, IUnityXRPreInit);
