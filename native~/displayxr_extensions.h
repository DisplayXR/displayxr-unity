// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Custom OpenXR extension constants and structs for DisplayXR support.
// These mirror the definitions in the CNSDK-OpenXR extension headers.

#pragma once

#include <openxr/openxr.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- XR_DXR_display_info ---
#define XR_DXR_DISPLAY_INFO_EXTENSION_NAME "XR_DXR_display_info"
#define XR_DXR_DISPLAY_INFO_SPEC_VERSION 12

#define XR_TYPE_DISPLAY_INFO_DXR ((XrStructureType)1004999003)

typedef struct XrDisplayInfoDXR {
    XrStructureType type;
    void *next;
    XrExtent2Df displaySizeMeters;
    XrVector3f nominalViewerPositionInDisplaySpace;
    float recommendedViewScaleX;
    float recommendedViewScaleY;
    uint32_t displayPixelWidth;
    uint32_t displayPixelHeight;
} XrDisplayInfoDXR;

typedef enum XrDisplayModeDXR {
    XR_DISPLAY_MODE_2D_DXR = 0,
    XR_DISPLAY_MODE_3D_DXR = 1,
    XR_DISPLAY_MODE_MAX_ENUM_DXR = 0x7FFFFFFF
} XrDisplayModeDXR;

typedef XrResult(XRAPI_PTR *PFN_xrRequestDisplayModeDXR)(XrSession session, XrDisplayModeDXR displayMode);

// --- Display rendering mode (vendor-specific: SBS, anaglyph, lenticular, etc.) ---
typedef XrResult(XRAPI_PTR *PFN_xrRequestDisplayRenderingModeDXR)(XrSession session, uint32_t modeIndex);

#define XR_TYPE_DISPLAY_RENDERING_MODE_INFO_DXR ((XrStructureType)1004999008)

typedef struct XrDisplayRenderingModeInfoDXR {
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
    // clients (runtime drops xrRequestDisplayRenderingModeDXR). Plugin
    // doesn't currently consume them, but the struct sizeof must match
    // the runtime's so xrEnumerateDisplayRenderingModesDXR writes modes[i]
    // at the right stride. See DisplayXR/displayxr-runtime#234.
    XrBool32 isActive;
    XrBool32 isRequestable;
} XrDisplayRenderingModeInfoDXR;

typedef XrResult(XRAPI_PTR *PFN_xrEnumerateDisplayRenderingModesDXR)(
    XrSession session,
    uint32_t modeCapacityInput,
    uint32_t *modeCountOutput,
    XrDisplayRenderingModeInfoDXR *modes);

// --- Eye-tracking mode control (XR_DXR_display_info v6) ---
// Verbatim from the runtime's openxr/XR_DXR_display_info.h. MANAGED (0) is the
// default (vendor SDK owns transitions); MANUAL (1) gives unfiltered positions +
// an explicit isTracking flag the app drives its own 2D/3D ramp from.
typedef enum XrEyeTrackingModeDXR {
    XR_EYE_TRACKING_MODE_MANAGED_DXR  = 0,
    XR_EYE_TRACKING_MODE_MANUAL_DXR   = 1,
    XR_EYE_TRACKING_MODE_MAX_ENUM_DXR = 0x7FFFFFFF
} XrEyeTrackingModeDXR;

typedef XrResult(XRAPI_PTR *PFN_xrRequestEyeTrackingModeDXR)(
    XrSession session, XrEyeTrackingModeDXR mode);

// --- Unified display-mode events (XR_DXR_display_info v10/v14) ---
// Verbatim struct layouts + type values from the runtime header. The provider's
// xrPollEvent loop consumes these to reconfigure tiling/resolution live and to
// notify C#. (Not used by the hook/standalone path; provider-only for #166 M2.)
#define XR_TYPE_EVENT_DATA_RENDERING_MODE_CHANGED_DXR         ((XrStructureType)1004999010)
#define XR_TYPE_EVENT_DATA_HARDWARE_DISPLAY_STATE_CHANGED_DXR ((XrStructureType)1004999011)
#define XR_TYPE_EVENT_DATA_EYE_TRACKING_STATE_CHANGED_DXR     ((XrStructureType)1004999013)

typedef struct XrEventDataRenderingModeChangedDXR {
    XrStructureType type; // Must be XR_TYPE_EVENT_DATA_RENDERING_MODE_CHANGED_DXR
    const void *next;
    XrSession session;
    uint32_t previousModeIndex;
    uint32_t currentModeIndex;
} XrEventDataRenderingModeChangedDXR;

typedef struct XrEventDataHardwareDisplayStateChangedDXR {
    XrStructureType type; // Must be XR_TYPE_EVENT_DATA_HARDWARE_DISPLAY_STATE_CHANGED_DXR
    const void *next;
    XrSession session;
    XrBool32 hardwareDisplay3D;
} XrEventDataHardwareDisplayStateChangedDXR;

typedef struct XrEventDataEyeTrackingStateChangedDXR {
    XrStructureType type; // Must be XR_TYPE_EVENT_DATA_EYE_TRACKING_STATE_CHANGED_DXR
    const void *next;
    XrSession session;
    XrBool32 isTracking;          // New derived state
    XrEyeTrackingModeDXR activeMode; // Session's MANAGED/MANUAL preference
} XrEventDataEyeTrackingStateChangedDXR;

// --- Shared texture output rect (canvas positioning for weaver alignment) ---
typedef XrResult (XRAPI_PTR *PFN_xrSetSharedTextureOutputRectDXR)(
    XrSession session, int32_t x, int32_t y, uint32_t width, uint32_t height);

// --- 2D surround texture (post-weave fill of the non-canvas region) ---
// XR_DXR_win32_window_binding spec v6 (D3D11 keyed-mutex) / v7 (D3D12 fence).
// The runtime blits the non-canvas pixels of the output (HWND back buffer or
// app shared texture) from this app-supplied full-window 2D texture each frame,
// AFTER the weave — so the surround is always at full native panel resolution.
// Works in handle mode (runtime presents) as well as shared-texture mode;
// requires xrSetSharedTextureOutputRectDXR to define the canvas sub-rect.
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

// --- XR_DXR_win32_window_binding ---
#define XR_DXR_WIN32_WINDOW_BINDING_EXTENSION_NAME "XR_DXR_win32_window_binding"
// SPEC_VERSION 7 adds xrSetSharedTextureSurround2DFenceEXT (D3D12 fence-synced
// 2D surround); v6 added xrSetSharedTextureSurround2DEXT (D3D11 keyed-mutex).
// SPEC_VERSION 5 adds chromaKeyColor — runtime-side post-weave chroma-key
// conversion that writes alpha=0 for matching pixels before the DComp
// (D3D12) or BitBlt (D3D11) present (runtime-pvt #191).
// SPEC_VERSION 4 added transparentBackgroundEnabled.
// SPEC_VERSION 3 added sharedTextureHandle. The CreateInfo struct is unchanged
// since v5 (surround is a separate entry-point family, not new struct fields);
// older runtimes ignore trailing fields (struct grows at the end, ABI-safe).
#define XR_DXR_WIN32_WINDOW_BINDING_SPEC_VERSION 7

#define XR_TYPE_WIN32_WINDOW_BINDING_CREATE_INFO_DXR ((XrStructureType)1004999001)
#define XR_TYPE_COMPOSITION_LAYER_WINDOW_SPACE_DXR ((XrStructureType)1004999002)

typedef struct XrWin32WindowBindingCreateInfoDXR {
    XrStructureType type;
    const void *next;
    void *windowHandle; // HWND
    PFN_xrReadbackCallback readbackCallback;
    void *readbackUserdata;
    void *sharedTextureHandle; // D3D11 shared HANDLE
    XrBool32 transparentBackgroundEnabled; // SPEC_VERSION 4: opt-in BitBlt/DComp swapchain
    uint32_t chromaKeyColor; // SPEC_VERSION 5: Win32 COLORREF (0x00BBGGRR); 0 = no post-weave conversion
} XrWin32WindowBindingCreateInfoDXR;

typedef struct XrCompositionLayerWindowSpaceDXR {
    XrStructureType type;
    const void *next;
    XrCompositionLayerFlags layerFlags;
    XrSwapchainSubImage subImage;
    float x;         // Left edge, fraction of window [0..1]
    float y;         // Top edge, fraction of window [0..1]
    float width;     // Fraction of window [0..1]
    float height;    // Fraction of window [0..1]
    float disparity; // Horizontal shift, fraction of window
} XrCompositionLayerWindowSpaceDXR;

// --- XR_DXR_local_3d_zone (#439/#491) ---
// Post-weave 2D content placed at a client-window PIXEL rect, composited over
// the woven 3D with an implicit mask (the union of Local2D layer rects implies
// M=0 inside / M=1 elsewhere — the region under the rect goes flat 2D, "glass
// over 3D"). Unlike XrCompositionLayerWindowSpaceDXR (fractional, disparity),
// the dest rect here is in post-DPI client-window pixels.
#define XR_DXR_LOCAL_3D_ZONE_EXTENSION_NAME "XR_DXR_local_3d_zone"
#define XR_DXR_local_3d_zone_SPEC_VERSION 1

#define XR_TYPE_COMPOSITION_LAYER_LOCAL_2D_DXR ((XrStructureType)1004999165)

typedef struct XrCompositionLayerLocal2DDXR {
    XrStructureType type;
    const void *next;
    XrCompositionLayerFlags layerFlags; // alpha bits honored
    XrSwapchainSubImage subImage;       // source texture + sub-rect
    XrRect2Di rect;                     // dest, client-window pixels
} XrCompositionLayerLocal2DDXR;

// --- XR_DXR_cocoa_window_binding ---
// SPEC_VERSION 5 adds transparentBackgroundEnabled — runtime configures
// the runtime-owned NSWindow + CAMetalLayer with isOpaque=NO so per-pixel
// alpha from the app reaches the desktop via Cocoa per-pixel transparency.
// sim_display_processor_metal is alpha-native — no chroma-key trick needed.
// (Sibling of XrWin32WindowBindingCreateInfoDXR.transparentBackgroundEnabled.)
#define XR_DXR_COCOA_WINDOW_BINDING_EXTENSION_NAME "XR_DXR_cocoa_window_binding"
#define XR_DXR_COCOA_WINDOW_BINDING_SPEC_VERSION 5

#define XR_TYPE_COCOA_WINDOW_BINDING_CREATE_INFO_DXR ((XrStructureType)1004999004)

typedef struct XrCocoaWindowBindingCreateInfoDXR {
    XrStructureType type;
    const void *next;
    void *viewHandle;                    // NSView* or NULL for offscreen
    PFN_xrReadbackCallback readbackCallback;
    void *readbackUserdata;
    void *sharedIOSurface;               // IOSurfaceRef for zero-copy GPU sharing
    XrBool32 transparentBackgroundEnabled; // SPEC_VERSION 5
} XrCocoaWindowBindingCreateInfoDXR;

// --- XR_KHR_metal_enable ---
// Hand-defined: the fetched OpenXR-SDK release-1.0.34 headers predate the
// Metal enable extension (it landed in the 1.1.x line), so openxr_platform.h
// has no XrGraphicsBindingMetalKHR even with XR_USE_GRAPHICS_API_METAL set.
// Struct-type values are the registry-assigned 1000029000..2. Canonical copy —
// the dormant SA-era duplicate in displayxr_standalone_internal.h is not
// included by any provider TU. The runtime also accepts the provisional alias
// "XR_KHRX2_metal_enable"; the provider requests the plain name only.
#ifndef XR_TYPE_GRAPHICS_BINDING_METAL_KHR
#define XR_TYPE_GRAPHICS_BINDING_METAL_KHR ((XrStructureType)1000029000)
#define XR_TYPE_SWAPCHAIN_IMAGE_METAL_KHR ((XrStructureType)1000029001)
#define XR_TYPE_GRAPHICS_REQUIREMENTS_METAL_KHR ((XrStructureType)1000029002)
#define XR_KHR_METAL_ENABLE_EXTENSION_NAME "XR_KHR_metal_enable"

typedef struct XrGraphicsBindingMetalKHR {
    XrStructureType type;
    const void *next;
    void *commandQueue; // id<MTLCommandQueue>, non-NULL required by the runtime
} XrGraphicsBindingMetalKHR;

typedef struct XrGraphicsRequirementsMetalKHR {
    XrStructureType type;
    void *next;
    void *metalDevice; // id<MTLDevice> the runtime prefers
} XrGraphicsRequirementsMetalKHR;

typedef struct XrSwapchainImageMetalKHR {
    XrStructureType type;
    void *next;
    void *texture; // id<MTLTexture>
} XrSwapchainImageMetalKHR;

typedef XrResult(XRAPI_PTR *PFN_xrGetMetalGraphicsRequirementsKHR)(
    XrInstance instance, XrSystemId systemId,
    XrGraphicsRequirementsMetalKHR *graphicsRequirements);
#endif // XR_TYPE_GRAPHICS_BINDING_METAL_KHR

// --- XR_DXR_atlas_capture ---
// Vendor-neutral "snapshot the runtime's composed multi-view atlas to a PNG"
// entry point. Replaces the app-side GPU readback (AsyncGPUReadback + hidden
// camera re-render) the plugin used to do for the screenshot ('I') key — the
// runtime now does the readback with the compositor's own atlas image at a
// caller-selected stage and writes the PNG (appending
// "_atlas_<viewCount>_<cols>x<rows>.png" to the supplied path prefix, opaque
// alpha — DisplayXR/displayxr-runtime#425). Source of truth:
// displayxr-runtime/src/external/openxr_includes/openxr/XR_DXR_atlas_capture.h
#define XR_DXR_ATLAS_CAPTURE_EXTENSION_NAME "XR_DXR_atlas_capture"
// SPEC_VERSION 3: XrStructureType values relocated 1004999120..121 ->
// 1004999170..171 (the old block collided with XR_DXR_workspace_file_dialog,
// which reserved it first). No struct/field/entry-point changes — header re-sync
// only. Sending the stale type made the runtime reject with VALIDATION_FAILURE.
#define XR_DXR_ATLAS_CAPTURE_SPEC_VERSION 3

#define XR_TYPE_ATLAS_CAPTURE_INFO_DXR ((XrStructureType)1004999170)
#define XR_TYPE_ATLAS_CAPTURE_RESULT_DXR ((XrStructureType)1004999171)

#define XR_ATLAS_CAPTURE_PATH_MAX_DXR 256

// Values match enum mcp_capture_mode in the runtime (no translation layer):
//   POST_COMPOSE    — atlas handed to the display processor (projection +
//                     window-space + quad layers), end of frame.
//   PROJECTION_ONLY — atlas with only projection-class layers, captured at the
//                     projection-done boundary.
typedef enum XrAtlasCaptureStageDXR {
    XR_ATLAS_CAPTURE_STAGE_POST_COMPOSE_DXR = 0,
    XR_ATLAS_CAPTURE_STAGE_PROJECTION_ONLY_DXR = 1,
    XR_ATLAS_CAPTURE_STAGE_MAX_ENUM_DXR = 0x7FFFFFFF
} XrAtlasCaptureStageDXR;

typedef struct XrAtlasCaptureInfoDXR {
    XrStructureType type; // Must be XR_TYPE_ATLAS_CAPTURE_INFO_DXR
    const void *next;
    XrAtlasCaptureStageDXR stage; // Capture stage (post-compose / projection-only)
    char pathPrefix[XR_ATLAS_CAPTURE_PATH_MAX_DXR];
} XrAtlasCaptureInfoDXR;

typedef struct XrAtlasCaptureResultDXR {
    XrStructureType type; // Must be XR_TYPE_ATLAS_CAPTURE_RESULT_DXR
    void *next;
    uint64_t timestampNs;
    uint32_t atlasWidth;
    uint32_t atlasHeight;
    uint32_t eyeWidth;
    uint32_t eyeHeight;
    uint32_t tileColumns;
    uint32_t tileRows;
    float displayWidthM;
    float displayHeightM;
    float eyeLeftM[3];
    float eyeRightM[3];
} XrAtlasCaptureResultDXR;

typedef XrResult(XRAPI_PTR *PFN_xrCaptureAtlasDXR)(
    XrSession session,
    const XrAtlasCaptureInfoDXR *info,
    XrAtlasCaptureResultDXR *result);

// --- XR_DXR_view_rig ---
// App-facing control of the runtime's view-rig (Kooima) math: chain ONE rig
// descriptor on XrViewLocateInfo::next and consume render-ready XrView{pose,fov}
// instead of computing the projection from raw eyes ourselves. Chain
// XrViewDisplayRawDXR on XrViewState::next to recover the raw display-space eyes
// (for the gizmo / eye-position cache). Descriptors carry NO clip params (near/far
// stay app-side) and NO placement params (the runtime owns the window/canvas).
// Per-locate: chain every frame you want it to drive; a locate chaining nothing
// keeps the default raw-eye transport. Out-of-range values clamp (one-shot runtime
// WARN), never reject; if both rigs are chained the camera rig wins. Source of
// truth: displayxr-runtime/src/external/openxr_includes/openxr/XR_DXR_view_rig.h
// (SPEC_VERSION 3 — reconcile with the Khronos registry before spec freeze).
#define XR_DXR_VIEW_RIG_EXTENSION_NAME "XR_DXR_view_rig"
#define XR_DXR_view_rig_SPEC_VERSION 3

#define XR_TYPE_DISPLAY_RIG_DXR ((XrStructureType)1004999140)
#define XR_TYPE_CAMERA_RIG_DXR ((XrStructureType)1004999141)
#define XR_TYPE_VIEW_DISPLAY_RAW_DXR ((XrStructureType)1004999142)

// Capacity of XrViewDisplayRawDXR::rawEyes (matches the runtime's max views).
#define XR_VIEW_RIG_MAX_RAW_EYES_DXR 8

// Display-centric rig (window/canvas as a portal). Chain on XrViewLocateInfo::next.
typedef struct XrDisplayRigDXR {
    XrStructureType type; // Must be XR_TYPE_DISPLAY_RIG_DXR
    const void *next;
    XrPosef pose;               // virtual display plane pose in the locate space
    float virtualDisplayHeight; // app units; m2v = this / physical canvas height
    float ipdFactor;            // [0,1] multiplies view-pose spread about center
    float parallaxFactor;       // [0,1] lerps eye centroid toward nominal viewer
    float perspectiveFactor;    // [0.1,10] scales eye XYZ (object perspective)
} XrDisplayRigDXR;

// Camera-centric rig (app camera perturbed by eye tracking). Chain on next.
typedef struct XrCameraRigDXR {
    XrStructureType type; // Must be XR_TYPE_CAMERA_RIG_DXR
    const void *next;
    XrPosef pose;             // camera pose in the locate space
    float ipdFactor;          // [0,1] multiplies view-pose spread about center
    float parallaxFactor;     // [0,1] lerps eye centroid toward nominal viewer
    float convergenceDiopters; // 1/m to the convergence plane; 0 = infinity
    float verticalFov;        // radians, full vertical angle
    float metersToVirtual;    // meters->world scale on the eye; 0/unset = 1.0 (spec v3).
                              // The plugin already folds scene scale into
                              // convergenceDiopters (/ssz), so it passes 1.0 here to
                              // keep the runtime's pre-v3 (1 unit = 1 m) behavior.
} XrCameraRigDXR;

// Raw rig inputs for the locate, filled by the runtime. Chain on XrViewState::next.
// rawEyes are verbatim physical-display-space eye positions (meters, display-center
// origin, +X right +Y up +Z toward viewer), one per active view; NOT canvas-rebased.
typedef struct XrViewDisplayRawDXR {
    XrStructureType type; // Must be XR_TYPE_VIEW_DISPLAY_RAW_DXR
    void *next;
    XrVector3f rawEyes[XR_VIEW_RIG_MAX_RAW_EYES_DXR];
    uint32_t eyeCountOutput;      // eyes written = the DP's per-view count
    XrPosef displayPlanePose;     // physical display plane in the locate space
    XrRect2Di canvasRectPx;       // effective canvas on the panel (client/sub-rect)
    XrExtent2Df canvasSizeMeters; // physical size of that canvas
    int64_t sampleTimeNs;         // when the eyes were sampled (monotonic)
    XrBool32 isTracking;          // physical tracker lock (vs nominal fallback)
} XrViewDisplayRawDXR;

// Workspace-controller-only entry point — the plugin never calls this (Unity is
// never the workspace controller); typedef kept for header completeness.
typedef XrResult(XRAPI_PTR *PFN_xrSetWorkspaceViewRigDXR)(XrSession session, const void *rig);

// --- XR_DXR_display_zones ---
// Compose N 3D zones (each a window-pixel rect with its own view-rig framing and
// its own projection layer) within the window. Built BY COMPOSITION on top of
// XR_DXR_view_rig (the rig descriptors are chained per-locate as usual) and
// XR_DXR_local_3d_zone (the 2D zones are XrCompositionLayerLocal2DDXR). A frame
// is a ZONES FRAME iff >= 1 projection layer carries an XrDisplayZoneDXR chain;
// then the canvas output rect (xrSetSharedTextureOutputRectDXR) and the surround
// path are inert. Chain the SAME XrDisplayZoneDXR instance at the locate
// (XrViewLocateInfo::next, scoping the Kooima framing to the rect — the rect IS
// the canvas) and at the submit (XrCompositionLayerProjection::next, binding the
// layer's views into that rect). Source of truth: displayxr-runtime (and the
// avatar's openxr_includes/openxr/XR_DXR_display_zones.h), SPEC_VERSION 1.
#define XR_DXR_DISPLAY_ZONES_EXTENSION_NAME "XR_DXR_display_zones"
// SPEC_VERSION 2 (#225): + xrGetWorkspaceTileSizeDXR (live tile canvas px).
// SPEC_VERSION 3 (runtime#800): + XrDisplayZoneFeatherDXR — per-zone cosmetic
// edge feather, OPT-IN. Zone edges are HARD by default (and the published
// hardware wish is always binary/un-feathered regardless of this struct);
// chain a feather on the zone at xrEndFrame to soften the COMPOSITE only.
#define XR_DXR_display_zones_SPEC_VERSION 3

#define XR_TYPE_DISPLAY_ZONE_CAPABILITIES_DXR               ((XrStructureType)1004999150)
#define XR_TYPE_DISPLAY_ZONE_DXR                            ((XrStructureType)1004999151)
#define XR_TYPE_DISPLAY_ZONES_FRAME_END_INFO_DXR            ((XrStructureType)1004999152)
#define XR_TYPE_EVENT_DATA_DISPLAY_ZONE_METRICS_CHANGED_DXR ((XrStructureType)1004999153)
#define XR_TYPE_DISPLAY_ZONE_FEATHER_DXR                    ((XrStructureType)1004999154)

typedef XrFlags64 XrDisplayZonesFrameEndFlagsDXR;
// Cross-check zone/locate/mask consistency this frame (one-shot WARN per
// violation class, never a per-frame error). Bring-up diagnostic only.
#define XR_DISPLAY_ZONES_FRAME_END_VALIDATE_BIT_DXR ((XrDisplayZonesFrameEndFlagsDXR)0x00000001)

// Capabilities of the display-zones path for a session. supported==XR_FALSE =>
// only the legacy single-canvas path. maxZones3D = max zone-chained projection
// layers per frame.
typedef struct XrDisplayZoneCapabilitiesDXR {
	XrStructureType type; // Must be XR_TYPE_DISPLAY_ZONE_CAPABILITIES_DXR
	void *next;
	XrBool32 supported;
	uint32_t maxZones3D;
} XrDisplayZoneCapabilitiesDXR;

// A 3D display zone: identity + placement. Valid at TWO chain points —
// XrViewLocateInfo::next (zone-scoped locate, rect IS the canvas) and
// XrCompositionLayerProjection::next (binds the layer's views to the zone at
// xrEndFrame). Chain the SAME instance at both points within a frame; the
// xrEndFrame values are authoritative. rect is in client-window pixels (same
// space as XrCompositionLayerLocal2DDXR::rect). zoneId is app-chosen, unique
// among the frame's 3D zones; there is NO zone handle (zones are stateless
// per-frame data, like layers).
typedef struct XrDisplayZoneDXR {
	XrStructureType type; // Must be XR_TYPE_DISPLAY_ZONE_DXR
	const void *next;
	uint32_t zoneId;
	XrRect2Di rect; // client-window pixels
} XrDisplayZoneDXR;

// Per-zone cosmetic edge feather (spec v3, runtime#800/#803), OPT-IN: chain on
// XrDisplayZoneDXR::next at the SUBMIT chain point (locate-instance chains are
// ignored). 0/negative/NaN = hard (the default); the runtime clamps the radius
// to half the zone's shorter side. Softens the runtime COMPOSITE only — the
// published hardware wish stays binary regardless. Runtimes predating spec v3
// ignore the chained struct (hard edges, no error).
typedef struct XrDisplayZoneFeatherDXR {
	XrStructureType type; // Must be XR_TYPE_DISPLAY_ZONE_FEATHER_DXR
	const void *next;
	float radiusPx; // inward ramp width, client-window px; 0 = hard
} XrDisplayZoneFeatherDXR;

// The mask handle from XR_DXR_local_3d_zone. The plugin never authors a wish
// mask (it always auto-derives), so we forward-declare the handle here rather
// than pull in the full mask-authoring API. (XR_DEFINE_HANDLE-equivalent.)
#ifndef XR_DISPLAYXR_LOCAL3D_ZONE_MASK_DEFINED
#define XR_DISPLAYXR_LOCAL3D_ZONE_MASK_DEFINED
XR_DEFINE_HANDLE(XrLocal3DZoneMaskDXR)
#endif

// Per-frame wish reference. Optional, chained on XrFrameEndInfo::next in a zones
// frame. Absent or wishMask==XR_NULL_HANDLE: the wish auto-derives as the union
// of the frame's 3D-zone rects (feathered). The plugin chains this only when the
// VALIDATE bit is requested for bring-up; otherwise nothing is chained.
typedef struct XrDisplayZonesFrameEndInfoDXR {
	XrStructureType type; // Must be XR_TYPE_DISPLAY_ZONES_FRAME_END_INFO_DXR
	const void *next;
	XrDisplayZonesFrameEndFlagsDXR flags;
	XrLocal3DZoneMaskDXR wishMask; // XR_NULL_HANDLE = auto-derive
} XrDisplayZonesFrameEndInfoDXR;

// Advisory: per-zone recommended view sizes may have changed (display-mode /
// tile-count switch, DPI change). Re-query each zone via
// xrGetDisplayZoneRecommendedViewSizeDXR; stale sizes stay correct, just soft.
typedef struct XrEventDataDisplayZoneMetricsChangedDXR {
	XrStructureType type; // Must be XR_TYPE_EVENT_DATA_DISPLAY_ZONE_METRICS_CHANGED_DXR
	const void *next;
	XrSession session;
} XrEventDataDisplayZoneMetricsChangedDXR;

typedef XrResult(XRAPI_PTR *PFN_xrGetDisplayZoneCapabilitiesDXR)(
    XrSession session, XrDisplayZoneCapabilitiesDXR *capabilities);

typedef XrResult(XRAPI_PTR *PFN_xrGetDisplayZoneRecommendedViewSizeDXR)(
    XrSession session, const XrRect2Di *zoneRect, XrExtent2Di *recommendedViewSize);

// (spec v2, #225) Live workspace-tile canvas px — follows the shell's 3D-window
// resize; a minimized tile queries it to re-author its zone/Local2D each frame.
typedef XrResult(XRAPI_PTR *PFN_xrGetWorkspaceTileSizeDXR)(
    XrSession session, XrExtent2Di *tileSize);

#ifdef __cplusplus
}
#endif
