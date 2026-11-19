// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Render scale (#475): the per-eye render textures are allocated at
//     base x scale
// where `base` is what the provider has always used (window px x the active mode's
// view scale, or a 3D zone's recommended view size) and `scale` is one number.
// Below 1 trades resolution for frame rate; above 1 supersamples (the runtime
// scales each view into its atlas, so the woven output keeps its size).
//
// WHAT IS SCALED: the primary eye swapchain + its bridge(s), and every extra 3D
// zone's swapchain + bridge(s). NOTHING ELSE. Zone rects, the window, the editor
// texture-mode weave target, window-space UI and Local2D layers are in window /
// panel pixels and never see this number.
//
// WHERE THE NUMBER COMES FROM, highest priority first:
//   1. DISPLAYXR_RENDER_SCALE (environment; read at every session start) - a test
//      harness / support override.
//   2. The app: dxr_prov_set_render_scale (DisplayXRProvider.RenderScale, or the
//      settings asset). 0 clears it.
//   3. Unity's own hint, UnityXRFrameSetupHints::appSetup.textureResolutionScale -
//      what XRSettings.eyeTextureResolutionScale and the URP asset's Render Scale
//      feed. This is what makes the stock Unity knobs work with no plug-in code.
//   4. 1.0.
//
// LIMITS. The request is clamped to [DXR_RS_MIN, DXR_RS_MAX] and quantised to
// 1/100. The resulting size is then clamped - uniformly, so the aspect is kept -
// to the runtime's maximum image size (the view configuration's maxImageRect and
// the system's maxSwapchainImage). A scale of exactly 1.0 is the identity: the
// size is the base size, bit for bit, with no limit applied - so a project that
// never touches the scale allocates exactly what it did before this existed.
//
// NO THRASH. A change of request becomes the TARGET only after it has held still
// for DXR_RS_SETTLE_NS (an animated scale therefore reallocates once, when it
// stops), except for the first hint of a session, which is taken at once. The
// session's existing size debounce (PS_REALLOC_DEBOUNCE stable frames, #172) then
// applies on top, exactly as it does for a window resize: a scale change IS a
// target-size change and rides the same live reallocation path.
//
// LIFETIME. The app's number, Unity's last hint and the environment override live
// in file statics here, NOT in the provider's per-session state, so an editor
// dock <-> undock (a session restart) allocates at the right size straight away.
//
// THREADING. The app setter runs on Unity's main thread; the hint, the tick and
// the size queries on the graphics thread (session-ready on the main thread on
// macOS). One mutex orders them.

#pragma once

#include <stdint.h>
#include "displayxr_provider_session.h" // DISPLAYXR_EXPORT

#include <openxr/openxr.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DXR_RS_MIN 0.25f
#define DXR_RS_MAX 2.0f
//! A changed request must hold still this long before it drives a reallocation.
#define DXR_RS_SETTLE_NS 250000000ull

//! Who the current request came from (DxrRenderScaleState::source).
#define DXR_RS_SOURCE_DEFAULT 0 //!< nobody asked: 1.0
#define DXR_RS_SOURCE_UNITY   1 //!< Unity's textureResolutionScale hint
#define DXR_RS_SOURCE_APP     2 //!< dxr_prov_set_render_scale
#define DXR_RS_SOURCE_ENV     3 //!< DISPLAYXR_RENDER_SCALE

//! DxrRenderScaleState::flags.
#define DXR_RS_FLAG_RANGE_CLAMPED 0x1u //!< the request was outside [DXR_RS_MIN, DXR_RS_MAX]
#define DXR_RS_FLAG_LIMIT_CLAMPED 0x2u //!< the allocated size was reduced to the runtime's maximum

// ---- C# surface -------------------------------------------------------------

//! Mirrored byte-for-byte in C# (DisplayXRProviderNative.DxrRenderScaleState).
typedef struct DxrRenderScaleState
{
	float requested;      //!< the request after precedence, range clamp and quantisation
	float target;         //!< the settled request the eye textures are (being) sized to
	float applied;        //!< allocated / base of the primary eye texture; 0 before the first allocation
	int32_t source;       //!< DXR_RS_SOURCE_*
	uint32_t base_width;  //!< unscaled per-eye size of the primary at its last allocation
	uint32_t base_height;
	uint32_t width;       //!< allocated per-eye size of the primary
	uint32_t height;
	uint32_t max_width;   //!< the runtime's maximum image size; 0 = not known
	uint32_t max_height;
	uint32_t alloc_count; //!< primary allocations this session (1 = created, never reallocated)
	uint32_t flags;       //!< DXR_RS_FLAG_*
} DxrRenderScaleState;

//! The app's render scale. <= 0 (or NaN) clears it, so Unity's hint decides again.
//! Kept across session restarts. Safe to call before the session exists.
DISPLAYXR_EXPORT void dxr_prov_set_render_scale(float scale);

//! What the app set (0 when it set nothing) - not the effective request.
DISPLAYXR_EXPORT float dxr_prov_get_render_scale_override(void);

//! The effective state. Returns 1 (it is always available, session or not).
DISPLAYXR_EXPORT int dxr_prov_get_render_scale_state(DxrRenderScaleState *out);

// ---- Provider-internal ------------------------------------------------------

typedef void (*DxrRsLogFn)(const char *fmt, ...);

//! A new session is being created: reset the per-session part (limits, settle,
//! allocation record), re-read the environment, keep the app's number and the hint.
void dxr_rs_on_session_starting(DxrRsLogFn log);

//! The session is gone.
void dxr_rs_on_session_stopped(void);

//! The runtime's size limits. @p system_max_* are XrSystemGraphicsProperties'
//! maxSwapchainImageWidth/Height (0 = unknown). The view configuration's
//! maxImageRect is queried here through @p gipa (xrEnumerateViewConfigurationViews,
//! PRIMARY_STEREO); a runtime that cannot answer leaves only the system limit.
void dxr_rs_on_system(XrInstance instance, XrSystemId system_id, PFN_xrGetInstanceProcAddr gipa,
                      uint32_t system_max_width, uint32_t system_max_height);

//! Unity's hint, every frame, from PopulateNextFrameDesc. Non-finite or <= 0 is
//! read as 1.0.
void dxr_rs_set_unity_hint(float scale);

//! Once per frame, before the session reconciles sizes: settles a changed request
//! into the target. @p now_ns is a monotonic clock.
void dxr_rs_tick(uint64_t now_ns);

//! The scale eye textures should be allocated at right now.
float dxr_rs_target(void);

//! base -> allocated size for @p scale. Pure given the session's limits: identity
//! at exactly 1.0; otherwise round(base * scale), at least 1 px, reduced uniformly
//! to the runtime's maximum. A zero base gives zero. Returns 1 when the limit bit.
int dxr_rs_scale_extent(float scale, uint32_t base_width, uint32_t base_height,
                        uint32_t *out_width, uint32_t *out_height);

//! The primary eye swapchain was (re)created at @p width x @p height from
//! @p base_width x @p base_height with @p scale. Logs the one line that says what
//! was requested and what was allocated.
void dxr_rs_note_primary_allocated(float scale, uint32_t base_width, uint32_t base_height,
                                   uint32_t width, uint32_t height);

//! Sanitise a raw request: NaN / inf / <= 0 -> 0 ("not set"); otherwise clamped to
//! [DXR_RS_MIN, DXR_RS_MAX] and quantised to 1/100. @p out_clamped (optional) is set
//! when the range clamp changed the value.
float dxr_rs_sanitize(float scale, int *out_clamped);

#ifdef __cplusplus
}
#endif
