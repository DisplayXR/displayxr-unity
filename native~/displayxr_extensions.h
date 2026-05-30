// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
//
// Custom OpenXR extension constants and structs for DisplayXR support.
// These mirror the definitions in the CNSDK-OpenXR extension headers.

#pragma once

#include <openxr/openxr.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- XR_EXT_display_info ---
#define XR_EXT_DISPLAY_INFO_EXTENSION_NAME "XR_EXT_display_info"
#define XR_EXT_DISPLAY_INFO_SPEC_VERSION 12

#define XR_TYPE_DISPLAY_INFO_EXT ((XrStructureType)1000999003)

typedef struct XrDisplayInfoEXT {
    XrStructureType type;
    void *next;
    XrExtent2Df displaySizeMeters;
    XrVector3f nominalViewerPositionInDisplaySpace;
    float recommendedViewScaleX;
    float recommendedViewScaleY;
    uint32_t displayPixelWidth;
    uint32_t displayPixelHeight;
} XrDisplayInfoEXT;

typedef enum XrDisplayModeEXT {
    XR_DISPLAY_MODE_2D_EXT = 0,
    XR_DISPLAY_MODE_3D_EXT = 1,
    XR_DISPLAY_MODE_MAX_ENUM_EXT = 0x7FFFFFFF
} XrDisplayModeEXT;

typedef XrResult(XRAPI_PTR *PFN_xrRequestDisplayModeEXT)(XrSession session, XrDisplayModeEXT displayMode);

// --- Display rendering mode (vendor-specific: SBS, anaglyph, lenticular, etc.) ---
typedef XrResult(XRAPI_PTR *PFN_xrRequestDisplayRenderingModeEXT)(XrSession session, uint32_t modeIndex);

#define XR_TYPE_DISPLAY_RENDERING_MODE_INFO_EXT ((XrStructureType)1000999008)

typedef struct XrDisplayRenderingModeInfoEXT {
    XrStructureType type;
    void *next;
    uint32_t modeIndex;
    char modeName[XR_MAX_SYSTEM_NAME_SIZE];
    uint32_t viewCount;
    float viewScaleX;
    float viewScaleY;
    XrBool32 hardwareDisplay3D;
    uint32_t tileColumns;
    uint32_t tileRows;
    uint32_t viewWidthPixels;
    uint32_t viewHeightPixels;
    // v13 (DisplayXR runtime v1.4.0+): isActive marks the current mode for
    // this session; isRequestable is false for non-controller workspace
    // clients (runtime drops xrRequestDisplayRenderingModeEXT). Plugin
    // doesn't currently consume them, but the struct sizeof must match
    // the runtime's so xrEnumerateDisplayRenderingModesEXT writes modes[i]
    // at the right stride. See DisplayXR/displayxr-runtime#234.
    XrBool32 isActive;
    XrBool32 isRequestable;
} XrDisplayRenderingModeInfoEXT;

typedef XrResult(XRAPI_PTR *PFN_xrEnumerateDisplayRenderingModesEXT)(
    XrSession session,
    uint32_t modeCapacityInput,
    uint32_t *modeCountOutput,
    XrDisplayRenderingModeInfoEXT *modes);

// --- Shared texture output rect (canvas positioning for weaver alignment) ---
typedef XrResult (XRAPI_PTR *PFN_xrSetSharedTextureOutputRectEXT)(
    XrSession session, int32_t x, int32_t y, uint32_t width, uint32_t height);

// --- 2D surround texture (post-weave fill of the non-canvas region) ---
// XR_EXT_win32_window_binding spec v6 (D3D11 keyed-mutex) / v7 (D3D12 fence).
// The runtime blits the non-canvas pixels of the output (HWND back buffer or
// app shared texture) from this app-supplied full-window 2D texture each frame,
// AFTER the weave — so the surround is always at full native panel resolution.
// Works in handle mode (runtime presents) as well as shared-texture mode;
// requires xrSetSharedTextureOutputRectEXT to define the canvas sub-rect.
// v6: D3D11 path, IDXGIKeyedMutex sync (key 0). Pass NULL handle to clear.
typedef XrResult (XRAPI_PTR *PFN_xrSetSharedTextureSurround2DEXT)(
    XrSession session, void *sharedTextureHandle, uint32_t width, uint32_t height);
// v7: D3D12 path. D3D12-native shared resources don't expose IDXGIKeyedMutex,
// so sync is via a shared ID3D12Fence. App signals (fence, awaitFenceValue) on
// its queue after recording surround content; the runtime waits on that value
// before the strip blit. awaitFenceValue must increase monotonically. Pass
// NULL sharedTextureHandle to clear (fence handle ignored in that case).
typedef XrResult (XRAPI_PTR *PFN_xrSetSharedTextureSurround2DFenceEXT)(
    XrSession session, void *sharedTextureHandle, uint32_t width, uint32_t height,
    void *sharedFenceHandle, uint64_t awaitFenceValue);

// --- Readback callback (shared by macOS and Win32 bindings) ---
typedef void (*PFN_xrReadbackCallback)(const uint8_t *pixels, uint32_t width, uint32_t height, void *userdata);

// --- XR_EXT_win32_window_binding ---
#define XR_EXT_WIN32_WINDOW_BINDING_EXTENSION_NAME "XR_EXT_win32_window_binding"
// SPEC_VERSION 7 adds xrSetSharedTextureSurround2DFenceEXT (D3D12 fence-synced
// 2D surround); v6 added xrSetSharedTextureSurround2DEXT (D3D11 keyed-mutex).
// SPEC_VERSION 5 adds chromaKeyColor — runtime-side post-weave chroma-key
// conversion that writes alpha=0 for matching pixels before the DComp
// (D3D12) or BitBlt (D3D11) present (runtime-pvt #191).
// SPEC_VERSION 4 added transparentBackgroundEnabled.
// SPEC_VERSION 3 added sharedTextureHandle. The CreateInfo struct is unchanged
// since v5 (surround is a separate entry-point family, not new struct fields);
// older runtimes ignore trailing fields (struct grows at the end, ABI-safe).
#define XR_EXT_WIN32_WINDOW_BINDING_SPEC_VERSION 7

#define XR_TYPE_WIN32_WINDOW_BINDING_CREATE_INFO_EXT ((XrStructureType)1000999001)
#define XR_TYPE_COMPOSITION_LAYER_WINDOW_SPACE_EXT ((XrStructureType)1000999002)

typedef struct XrWin32WindowBindingCreateInfoEXT {
    XrStructureType type;
    const void *next;
    void *windowHandle; // HWND
    PFN_xrReadbackCallback readbackCallback;
    void *readbackUserdata;
    void *sharedTextureHandle; // D3D11 shared HANDLE
    XrBool32 transparentBackgroundEnabled; // SPEC_VERSION 4: opt-in BitBlt/DComp swapchain
    uint32_t chromaKeyColor; // SPEC_VERSION 5: Win32 COLORREF (0x00BBGGRR); 0 = no post-weave conversion
} XrWin32WindowBindingCreateInfoEXT;

typedef struct XrCompositionLayerWindowSpaceEXT {
    XrStructureType type;
    const void *next;
    XrCompositionLayerFlags layerFlags;
    XrSwapchainSubImage subImage;
    float x;         // Left edge, fraction of window [0..1]
    float y;         // Top edge, fraction of window [0..1]
    float width;     // Fraction of window [0..1]
    float height;    // Fraction of window [0..1]
    float disparity; // Horizontal shift, fraction of window
} XrCompositionLayerWindowSpaceEXT;

// --- XR_EXT_cocoa_window_binding ---
// SPEC_VERSION 5 adds transparentBackgroundEnabled — runtime configures
// the runtime-owned NSWindow + CAMetalLayer with isOpaque=NO so per-pixel
// alpha from the app reaches the desktop via Cocoa per-pixel transparency.
// sim_display_processor_metal is alpha-native — no chroma-key trick needed.
// (Sibling of XrWin32WindowBindingCreateInfoEXT.transparentBackgroundEnabled.)
#define XR_EXT_COCOA_WINDOW_BINDING_EXTENSION_NAME "XR_EXT_cocoa_window_binding"
#define XR_EXT_COCOA_WINDOW_BINDING_SPEC_VERSION 5

#define XR_TYPE_COCOA_WINDOW_BINDING_CREATE_INFO_EXT ((XrStructureType)1000999004)

typedef struct XrCocoaWindowBindingCreateInfoEXT {
    XrStructureType type;
    const void *next;
    void *viewHandle;                    // NSView* or NULL for offscreen
    PFN_xrReadbackCallback readbackCallback;
    void *readbackUserdata;
    void *sharedIOSurface;               // IOSurfaceRef for zero-copy GPU sharing
    XrBool32 transparentBackgroundEnabled; // SPEC_VERSION 5
} XrCocoaWindowBindingCreateInfoEXT;

#ifdef __cplusplus
}
#endif
