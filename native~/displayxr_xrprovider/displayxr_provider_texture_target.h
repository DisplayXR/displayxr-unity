// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// XR_DXR_win32_window_binding SPEC_VERSION 9 - texture target binding (runtime
// ADR-060, #471) - for the display provider's EDITOR texture mode.
//
// In the editor the runtime weaves into a shared texture that Unity draws inside
// the docked Game view. Until spec 9 the only way to tell the runtime WHERE that
// texture reaches the panel was an invisible anchor window parked on the image;
// the weaver phased from that window. With spec 9 the session states it directly:
// a reference window (the top-level editor window that hosts the Game view pane)
// and a target rect in that window's CLIENT area, physical device px. The runtime
// phases from the rect's live screen position and reports what it used.
//
// OPTIONAL by construction. Nothing here is chained unless, all at once:
//   - the session binds in editor texture mode (sharedTextureHandle set; a built
//     player never does), in the default window-glue arrangement;
//   - the runtime ADVERTISES XR_DXR_win32_window_binding >= 9 and its
//     XrSystemWin32TextureTargetPropertiesDXR says supportsTextureTarget;
//   - managed code has pushed a rect (dxr_prov_set_texture_target_rect) and its
//     reference window resolves to a live top-level window of this process;
//   - DISPLAYXR_PROV_TEXTURE_TARGET is not "0".
// The anchor window is kept as windowHandle either way: it is the runtime's
// fallback anchor (ANCHOR_FALLBACK), and on a session that cannot honour the
// binding at all (D3D12 until runtime #2382) the binding is ignored and the
// anchor carries the weave exactly as before.
//
// DPI. The rect crosses the C# boundary relative to the PANE window (a difference
// of two numbers read in the same space, so it is the same in every DPI awareness)
// and every Win32 read here runs in a PER_MONITOR_AWARE_V2 scope, so no
// virtual-desktop coordinate ever comes from managed code. (0,0) is a valid
// offset; absence is the DXR_TT_RECT_VALID flag being clear.
//
// Threading. The setter runs on Unity's MAIN thread; session create, xrWaitFrame,
// xrLocateViews and xrEndFrame run on the GRAPHICS thread; the state getter on the
// main thread. One mutex orders them.
//
// Lifetime. The pushed rect and the opt-out live in file statics here, NOT in the
// provider's per-session state: an editor dock <-> undock restarts the session and
// the binding is chained again from the last pushed rect (the v2.9.0 lesson: state
// pushed once must survive a restart).

#pragma once

#include <stdint.h>
#include "displayxr_provider_session.h" // DISPLAYXR_EXPORT

#include <openxr/openxr.h>
#include "../displayxr_extensions.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---- C# surface -------------------------------------------------------------

//! dxr_prov_set_texture_target_rect flags.
#define DXR_TT_RECT_VALID    0x1u //!< a rect is given; clear = "no target" (never infer it from zeros)
#define DXR_TT_RECT_MEASURED 0x2u //!< the rect was measured on screen (#459), not computed; logged, never sent

//! Where the woven texture is drawn. THE one entry point for it: whatever knows
//! where the image lands (the Game view calibration's measured origin when there
//! is one, else the computed Game view placement) feeds this.
//!
//! @p pane_hwnd  the window the offset is relative to - the Game view pane. Its
//!               top-level ancestor becomes the reference window. NULL: @p x / @p y
//!               are virtual-desktop PHYSICAL px (only pass that from a
//!               per-monitor-v2 measurement).
//! @p x, @p y    where the texture's texel (0,0) is drawn, in @p pane_hwnd's CLIENT
//!               area, physical device px, y down. (0,0) is valid.
//! @p w, @p h    the size it is drawn at, physical px (1:1 = the woven canvas size).
//! @p flags      DXR_TT_RECT_*. Without VALID (or with w/h <= 0) the target is cleared.
//!
//! Call it before the session starts (the binding needs an initial rect) and every
//! frame the placement is known. Windows-only effect; a no-op store elsewhere.
DISPLAYXR_EXPORT void dxr_prov_set_texture_target_rect(void *pane_hwnd, int32_t x, int32_t y,
                                                       int32_t w, int32_t h, uint32_t flags);

//! Why the binding was not chained on this session (DxrTextureTargetState::why_not).
#define DXR_TT_WHY_CHAINED        0 //!< it was chained
#define DXR_TT_WHY_NO_SESSION     1 //!< no session yet
#define DXR_TT_WHY_OPT_OUT        2 //!< DISPLAYXR_PROV_TEXTURE_TARGET=0
#define DXR_TT_WHY_NOT_ELIGIBLE   3 //!< not editor texture mode (player, present mode, zone-glue, ...)
#define DXR_TT_WHY_RUNTIME_SPEC   4 //!< runtime advertises XR_DXR_win32_window_binding < 9
#define DXR_TT_WHY_NOT_SUPPORTED  5 //!< system properties: supportsTextureTarget false
#define DXR_TT_WHY_NO_RECT        6 //!< no rect was pushed before the session started
#define DXR_TT_WHY_BAD_REFERENCE  7 //!< the pane has no live top-level window in this process
#define DXR_TT_WHY_CREATE_REFUSED 8 //!< xrCreateSession refused the binding; session re-created without it

//! DxrTextureTargetState::mismatch bits.
#define DXR_TT_MISMATCH_ORIGIN  0x1 //!< woven screen origin != the origin requested for that frame
#define DXR_TT_MISMATCH_RECT    0x2 //!< woven rect != the rect sent for that frame
#define DXR_TT_MISMATCH_CLAMPED 0x4 //!< the runtime clamped the rect to the shared texture

//! Blittable; mirrored field for field in C# (DisplayXRProviderNative.DxrTextureTargetState).
typedef struct DxrTextureTargetState {
	int32_t  chained;      //!< 1 = this session chained the binding
	int32_t  why_not;      //!< DXR_TT_WHY_*
	int32_t  reported;     //!< 1 once the runtime reported a woven frame
	uint32_t flags;        //!< XrWin32TextureTargetStateFlagsDXR (low 32 bits); 0 = runtime did not bind
	int32_t  woven_x, woven_y;         //!< wovenScreenOrigin, desktop physical px
	int32_t  requested_x, requested_y; //!< the origin requested for the same frame (valid if matched)
	int32_t  matched;      //!< 1 = the woven frame was found in the sent history
	int32_t  mismatch;     //!< DXR_TT_MISMATCH_*
	int32_t  rect_x, rect_y, rect_w, rect_h; //!< wovenTargetRect
	int32_t  windowless_predicted;           //!< system properties' windowlessPhase
} DxrTextureTargetState;

//! Snapshot for diagnostics (the editor's `GameView 1:1:` line). Returns 1 when
//! the binding is chained on the live session, else 0 (out is still filled).
DISPLAYXR_EXPORT int dxr_prov_get_texture_target_state(DxrTextureTargetState *out);

// ---- Provider-internal hooks (called from displayxr_provider_session.cpp) ----

typedef void (*DxrTtLogFn)(const char *fmt, ...);

//! What a pushed rect resolves to right now.
typedef struct DxrTtResolved {
	void   *reference_window;   //!< top-level window (NULL = desktop px)
	int32_t x, y;               //!< rect origin in reference_window's client area, physical px
	int32_t screen_x, screen_y; //!< the same point on the virtual desktop, physical px
} DxrTtResolved;

//! (pane, x, y in the pane's client px) -> reference window + rect origin. Returns
//! 1 on success, 0 when the pane has no live top-level window in this process.
typedef int (*DxrTtResolveFn)(void *pane_hwnd, int32_t x, int32_t y, DxrTtResolved *out);

//! Tests only: replace the Win32 resolver (NULL restores the platform default,
//! which always fails off Windows).
void dxr_tt_set_resolver(DxrTtResolveFn fn);

//! Start of every session start: forget the previous session's decision and
//! state (the pushed rect survives), take the log sink, read the opt-out.
void dxr_tt_on_session_starting(DxrTtLogFn log);

//! After enumerating instance extensions: the XR_DXR_win32_window_binding
//! extensionVersion the runtime ADVERTISED (0 = absent).
void dxr_tt_on_extension_version(uint32_t advertised_spec_version);

//! Before xrGetSystemProperties: returns the new head of the chain (@p next when
//! the runtime does not advertise spec 9). Call dxr_tt_on_system_properties after.
void *dxr_tt_chain_system_properties(void *next);
void dxr_tt_on_system_properties(int succeeded);

//! Before xrCreateSession, once @p wb is final. @p eligible: the session binds in
//! editor texture mode, window-glue arrangement. Chains the binding on @p wb when
//! every gate passes and returns 1; else leaves @p wb untouched and returns 0.
int dxr_tt_bind_session(XrWin32WindowBindingCreateInfoDXR *wb, int eligible);

//! xrCreateSession failed with the binding chained: take it off @p wb again so the
//! caller can retry without it. Returns 1 if it had been chained.
int dxr_tt_unbind_after_create_failure(XrWin32WindowBindingCreateInfoDXR *wb);

//! Before xrLocateViews / xrEndFrame: returns the new head (@p next when nothing
//! is to be chained). @p storage must outlive the call it is chained on. The
//! rect is resolved once per @p display_time, so a frame's locates and its
//! xrEndFrame carry identical values. @p at_frame_end: this is the xrEndFrame
//! (the authoritative one; it is what the reported state is compared against).
const void *dxr_tt_chain_frame(XrWin32TextureTargetFrameDXR *storage, const void *next,
                               XrTime display_time, int at_frame_end);

//! After xrEndFrame, with its result (a refused frame is logged once).
void dxr_tt_on_frame_end_result(XrResult result);

//! Before xrWaitFrame: returns the new head (@p next when the session did not
//! chain the binding). Call dxr_tt_on_frame_state after a successful wait.
void *dxr_tt_chain_frame_state(XrWin32TextureTargetStateDXR *storage, void *next);
void dxr_tt_on_frame_state(const XrWin32TextureTargetStateDXR *state);

//! Session stopped (or failed to start): no more chaining until the next start.
void dxr_tt_on_session_stopped(void);

#ifdef __cplusplus
}
#endif
