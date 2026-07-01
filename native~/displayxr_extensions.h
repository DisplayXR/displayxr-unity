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

// --- Eye-tracking mode control (XR_EXT_display_info v6) ---
// Verbatim from the runtime's openxr/XR_EXT_display_info.h. MANAGED (0) is the
// default (vendor SDK owns transitions); MANUAL (1) gives unfiltered positions +
// an explicit isTracking flag the app drives its own 2D/3D ramp from.
typedef enum XrEyeTrackingModeEXT {
    XR_EYE_TRACKING_MODE_MANAGED_EXT  = 0,
    XR_EYE_TRACKING_MODE_MANUAL_EXT   = 1,
    XR_EYE_TRACKING_MODE_MAX_ENUM_EXT = 0x7FFFFFFF
} XrEyeTrackingModeEXT;

typedef XrResult(XRAPI_PTR *PFN_xrRequestEyeTrackingModeEXT)(
    XrSession session, XrEyeTrackingModeEXT mode);

// --- Unified display-mode events (XR_EXT_display_info v10/v14) ---
// Verbatim struct layouts + type values from the runtime header. The provider's
// xrPollEvent loop consumes these to reconfigure tiling/resolution live and to
// notify C#. (Not used by the hook/standalone path; provider-only for #166 M2.)
#define XR_TYPE_EVENT_DATA_RENDERING_MODE_CHANGED_EXT         ((XrStructureType)1000999010)
#define XR_TYPE_EVENT_DATA_HARDWARE_DISPLAY_STATE_CHANGED_EXT ((XrStructureType)1000999011)
#define XR_TYPE_EVENT_DATA_EYE_TRACKING_STATE_CHANGED_EXT     ((XrStructureType)1000999013)

typedef struct XrEventDataRenderingModeChangedEXT {
    XrStructureType type; // Must be XR_TYPE_EVENT_DATA_RENDERING_MODE_CHANGED_EXT
    const void *next;
    XrSession session;
    uint32_t previousModeIndex;
    uint32_t currentModeIndex;
} XrEventDataRenderingModeChangedEXT;

typedef struct XrEventDataHardwareDisplayStateChangedEXT {
    XrStructureType type; // Must be XR_TYPE_EVENT_DATA_HARDWARE_DISPLAY_STATE_CHANGED_EXT
    const void *next;
    XrSession session;
    XrBool32 hardwareDisplay3D;
} XrEventDataHardwareDisplayStateChangedEXT;

typedef struct XrEventDataEyeTrackingStateChangedEXT {
    XrStructureType type; // Must be XR_TYPE_EVENT_DATA_EYE_TRACKING_STATE_CHANGED_EXT
    const void *next;
    XrSession session;
    XrBool32 isTracking;          // New derived state
    XrEyeTrackingModeEXT activeMode; // Session's MANAGED/MANUAL preference
} XrEventDataEyeTrackingStateChangedEXT;

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

// --- XR_EXT_local_3d_zone (#439/#491) ---
// Post-weave 2D content placed at a client-window PIXEL rect, composited over
// the woven 3D with an implicit mask (the union of Local2D layer rects implies
// M=0 inside / M=1 elsewhere — the region under the rect goes flat 2D, "glass
// over 3D"). Unlike XrCompositionLayerWindowSpaceEXT (fractional, disparity),
// the dest rect here is in post-DPI client-window pixels.
#define XR_EXT_LOCAL_3D_ZONE_EXTENSION_NAME "XR_EXT_local_3d_zone"
#define XR_EXT_local_3d_zone_SPEC_VERSION 1

#define XR_TYPE_COMPOSITION_LAYER_LOCAL_2D_EXT ((XrStructureType)1000999165)

typedef struct XrCompositionLayerLocal2DEXT {
    XrStructureType type;
    const void *next;
    XrCompositionLayerFlags layerFlags; // alpha bits honored
    XrSwapchainSubImage subImage;       // source texture + sub-rect
    XrRect2Di rect;                     // dest, client-window pixels
} XrCompositionLayerLocal2DEXT;

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

// --- XR_EXT_atlas_capture ---
// Vendor-neutral "snapshot the runtime's composed multi-view atlas to a PNG"
// entry point. Replaces the app-side GPU readback (AsyncGPUReadback + hidden
// camera re-render) the plugin used to do for the screenshot ('I') key — the
// runtime now does the readback with the compositor's own atlas image at a
// caller-selected stage and writes the PNG (appending
// "_atlas_<viewCount>_<cols>x<rows>.png" to the supplied path prefix, opaque
// alpha — DisplayXR/displayxr-runtime#425). Source of truth:
// displayxr-runtime/src/external/openxr_includes/openxr/XR_EXT_atlas_capture.h
#define XR_EXT_ATLAS_CAPTURE_EXTENSION_NAME "XR_EXT_atlas_capture"
#define XR_EXT_ATLAS_CAPTURE_SPEC_VERSION 2

#define XR_TYPE_ATLAS_CAPTURE_INFO_EXT ((XrStructureType)1000999120)
#define XR_TYPE_ATLAS_CAPTURE_RESULT_EXT ((XrStructureType)1000999121)

#define XR_ATLAS_CAPTURE_PATH_MAX_EXT 256

// Values match enum mcp_capture_mode in the runtime (no translation layer):
//   POST_COMPOSE    — atlas handed to the display processor (projection +
//                     window-space + quad layers), end of frame.
//   PROJECTION_ONLY — atlas with only projection-class layers, captured at the
//                     projection-done boundary.
typedef enum XrAtlasCaptureStageEXT {
    XR_ATLAS_CAPTURE_STAGE_POST_COMPOSE_EXT = 0,
    XR_ATLAS_CAPTURE_STAGE_PROJECTION_ONLY_EXT = 1,
    XR_ATLAS_CAPTURE_STAGE_MAX_ENUM_EXT = 0x7FFFFFFF
} XrAtlasCaptureStageEXT;

typedef struct XrAtlasCaptureInfoEXT {
    XrStructureType type; // Must be XR_TYPE_ATLAS_CAPTURE_INFO_EXT
    const void *next;
    XrAtlasCaptureStageEXT stage; // Capture stage (post-compose / projection-only)
    char pathPrefix[XR_ATLAS_CAPTURE_PATH_MAX_EXT];
} XrAtlasCaptureInfoEXT;

typedef struct XrAtlasCaptureResultEXT {
    XrStructureType type; // Must be XR_TYPE_ATLAS_CAPTURE_RESULT_EXT
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
} XrAtlasCaptureResultEXT;

typedef XrResult(XRAPI_PTR *PFN_xrCaptureAtlasEXT)(
    XrSession session,
    const XrAtlasCaptureInfoEXT *info,
    XrAtlasCaptureResultEXT *result);

// --- XR_EXT_view_rig ---
// App-facing control of the runtime's view-rig (Kooima) math: chain ONE rig
// descriptor on XrViewLocateInfo::next and consume render-ready XrView{pose,fov}
// instead of computing the projection from raw eyes ourselves. Chain
// XrViewDisplayRawEXT on XrViewState::next to recover the raw display-space eyes
// (for the gizmo / eye-position cache). Descriptors carry NO clip params (near/far
// stay app-side) and NO placement params (the runtime owns the window/canvas).
// Per-locate: chain every frame you want it to drive; a locate chaining nothing
// keeps the default raw-eye transport. Out-of-range values clamp (one-shot runtime
// WARN), never reject; if both rigs are chained the camera rig wins. Source of
// truth: displayxr-runtime/src/external/openxr_includes/openxr/XR_EXT_view_rig.h
// (SPEC_VERSION 3 — reconcile with the Khronos registry before spec freeze).
#define XR_EXT_VIEW_RIG_EXTENSION_NAME "XR_EXT_view_rig"
#define XR_EXT_view_rig_SPEC_VERSION 3

#define XR_TYPE_DISPLAY_RIG_EXT ((XrStructureType)1000999140)
#define XR_TYPE_CAMERA_RIG_EXT ((XrStructureType)1000999141)
#define XR_TYPE_VIEW_DISPLAY_RAW_EXT ((XrStructureType)1000999142)

// Capacity of XrViewDisplayRawEXT::rawEyes (matches the runtime's max views).
#define XR_VIEW_RIG_MAX_RAW_EYES_EXT 8

// Display-centric rig (window/canvas as a portal). Chain on XrViewLocateInfo::next.
typedef struct XrDisplayRigEXT {
    XrStructureType type; // Must be XR_TYPE_DISPLAY_RIG_EXT
    const void *next;
    XrPosef pose;               // virtual display plane pose in the locate space
    float virtualDisplayHeight; // app units; m2v = this / physical canvas height
    float ipdFactor;            // [0,1] multiplies view-pose spread about center
    float parallaxFactor;       // [0,1] lerps eye centroid toward nominal viewer
    float perspectiveFactor;    // [0.1,10] scales eye XYZ (object perspective)
} XrDisplayRigEXT;

// Camera-centric rig (app camera perturbed by eye tracking). Chain on next.
typedef struct XrCameraRigEXT {
    XrStructureType type; // Must be XR_TYPE_CAMERA_RIG_EXT
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
} XrCameraRigEXT;

// Raw rig inputs for the locate, filled by the runtime. Chain on XrViewState::next.
// rawEyes are verbatim physical-display-space eye positions (meters, display-center
// origin, +X right +Y up +Z toward viewer), one per active view; NOT canvas-rebased.
typedef struct XrViewDisplayRawEXT {
    XrStructureType type; // Must be XR_TYPE_VIEW_DISPLAY_RAW_EXT
    void *next;
    XrVector3f rawEyes[XR_VIEW_RIG_MAX_RAW_EYES_EXT];
    uint32_t eyeCountOutput;      // eyes written = the DP's per-view count
    XrPosef displayPlanePose;     // physical display plane in the locate space
    XrRect2Di canvasRectPx;       // effective canvas on the panel (client/sub-rect)
    XrExtent2Df canvasSizeMeters; // physical size of that canvas
    int64_t sampleTimeNs;         // when the eyes were sampled (monotonic)
    XrBool32 isTracking;          // physical tracker lock (vs nominal fallback)
} XrViewDisplayRawEXT;

// Workspace-controller-only entry point — the plugin never calls this (Unity is
// never the workspace controller); typedef kept for header completeness.
typedef XrResult(XRAPI_PTR *PFN_xrSetWorkspaceViewRigEXT)(XrSession session, const void *rig);

// --- XR_EXT_display_zones ---
// Compose N 3D zones (each a window-pixel rect with its own view-rig framing and
// its own projection layer) within the window. Built BY COMPOSITION on top of
// XR_EXT_view_rig (the rig descriptors are chained per-locate as usual) and
// XR_EXT_local_3d_zone (the 2D zones are XrCompositionLayerLocal2DEXT). A frame
// is a ZONES FRAME iff >= 1 projection layer carries an XrDisplayZoneEXT chain;
// then the canvas output rect (xrSetSharedTextureOutputRectEXT) and the surround
// path are inert. Chain the SAME XrDisplayZoneEXT instance at the locate
// (XrViewLocateInfo::next, scoping the Kooima framing to the rect — the rect IS
// the canvas) and at the submit (XrCompositionLayerProjection::next, binding the
// layer's views into that rect). Source of truth: displayxr-runtime (and the
// avatar's openxr_includes/openxr/XR_EXT_display_zones.h), SPEC_VERSION 1.
#define XR_EXT_DISPLAY_ZONES_EXTENSION_NAME "XR_EXT_display_zones"
#define XR_EXT_display_zones_SPEC_VERSION 1

#define XR_TYPE_DISPLAY_ZONE_CAPABILITIES_EXT               ((XrStructureType)1000999150)
#define XR_TYPE_DISPLAY_ZONE_EXT                            ((XrStructureType)1000999151)
#define XR_TYPE_DISPLAY_ZONES_FRAME_END_INFO_EXT            ((XrStructureType)1000999152)
#define XR_TYPE_EVENT_DATA_DISPLAY_ZONE_METRICS_CHANGED_EXT ((XrStructureType)1000999153)

typedef XrFlags64 XrDisplayZonesFrameEndFlagsEXT;
// Cross-check zone/locate/mask consistency this frame (one-shot WARN per
// violation class, never a per-frame error). Bring-up diagnostic only.
#define XR_DISPLAY_ZONES_FRAME_END_VALIDATE_BIT_EXT ((XrDisplayZonesFrameEndFlagsEXT)0x00000001)

// Capabilities of the display-zones path for a session. supported==XR_FALSE =>
// only the legacy single-canvas path. maxZones3D = max zone-chained projection
// layers per frame.
typedef struct XrDisplayZoneCapabilitiesEXT {
	XrStructureType type; // Must be XR_TYPE_DISPLAY_ZONE_CAPABILITIES_EXT
	void *next;
	XrBool32 supported;
	uint32_t maxZones3D;
} XrDisplayZoneCapabilitiesEXT;

// A 3D display zone: identity + placement. Valid at TWO chain points —
// XrViewLocateInfo::next (zone-scoped locate, rect IS the canvas) and
// XrCompositionLayerProjection::next (binds the layer's views to the zone at
// xrEndFrame). Chain the SAME instance at both points within a frame; the
// xrEndFrame values are authoritative. rect is in client-window pixels (same
// space as XrCompositionLayerLocal2DEXT::rect). zoneId is app-chosen, unique
// among the frame's 3D zones; there is NO zone handle (zones are stateless
// per-frame data, like layers).
typedef struct XrDisplayZoneEXT {
	XrStructureType type; // Must be XR_TYPE_DISPLAY_ZONE_EXT
	const void *next;
	uint32_t zoneId;
	XrRect2Di rect; // client-window pixels
} XrDisplayZoneEXT;

// The mask handle from XR_EXT_local_3d_zone. The plugin never authors a wish
// mask (it always auto-derives), so we forward-declare the handle here rather
// than pull in the full mask-authoring API. (XR_DEFINE_HANDLE-equivalent.)
#ifndef XR_DISPLAYXR_LOCAL3D_ZONE_MASK_DEFINED
#define XR_DISPLAYXR_LOCAL3D_ZONE_MASK_DEFINED
XR_DEFINE_HANDLE(XrLocal3DZoneMaskEXT)
#endif

// Per-frame wish reference. Optional, chained on XrFrameEndInfo::next in a zones
// frame. Absent or wishMask==XR_NULL_HANDLE: the wish auto-derives as the union
// of the frame's 3D-zone rects (feathered). The plugin chains this only when the
// VALIDATE bit is requested for bring-up; otherwise nothing is chained.
typedef struct XrDisplayZonesFrameEndInfoEXT {
	XrStructureType type; // Must be XR_TYPE_DISPLAY_ZONES_FRAME_END_INFO_EXT
	const void *next;
	XrDisplayZonesFrameEndFlagsEXT flags;
	XrLocal3DZoneMaskEXT wishMask; // XR_NULL_HANDLE = auto-derive
} XrDisplayZonesFrameEndInfoEXT;

// Advisory: per-zone recommended view sizes may have changed (display-mode /
// tile-count switch, DPI change). Re-query each zone via
// xrGetDisplayZoneRecommendedViewSizeEXT; stale sizes stay correct, just soft.
typedef struct XrEventDataDisplayZoneMetricsChangedEXT {
	XrStructureType type; // Must be XR_TYPE_EVENT_DATA_DISPLAY_ZONE_METRICS_CHANGED_EXT
	const void *next;
	XrSession session;
} XrEventDataDisplayZoneMetricsChangedEXT;

typedef XrResult(XRAPI_PTR *PFN_xrGetDisplayZoneCapabilitiesEXT)(
    XrSession session, XrDisplayZoneCapabilitiesEXT *capabilities);

typedef XrResult(XRAPI_PTR *PFN_xrGetDisplayZoneRecommendedViewSizeEXT)(
    XrSession session, const XrRect2Di *zoneRect, XrExtent2Di *recommendedViewSize);

#ifdef __cplusplus
}
#endif
