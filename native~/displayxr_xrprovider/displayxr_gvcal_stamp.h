// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// (#485) The calibration marker for the mirror path that does NOT go through the native
// mirror blit: Unity's own blit of blitParams.
//
// Unity is only OFFERED the native mirror blit. The scriptable pipelines take it; the
// Built-in pipeline never calls it and blits the texture named in blitParams itself. The
// calibration marker (#459) was drawn in the native blit only, so on that path nothing could
// be measured. Here the marker is put into what Unity blits instead:
//
//   - a plug-in-owned texture, the size and declaration of the texture Unity was told about;
//   - while a marker is requested, and only then, each submitted frame draws the woven
//     canvas into it with the SAME shader as the native blit (displayxr_gvcal_shader.h),
//     through a viewport on the canvas rect, marker constants laid out in CANVAS texels;
//   - the mirror query names that texture in blitParams[].srcTexId for those frames, with
//     the srcRect / destRect it would have used for the woven texture.
//
// THE GEOMETRY CONTRACT. In the native blit the marker's texel (x, y) is pixel (x, y) of the
// Game view's mirror target. Here it is texel (cx + x, cy + y) of the source texture, the
// texel that holds woven-canvas pixel (x, y), and the marker is laid out for a cw x ch image.
// Unity's blit maps the canvas rect onto the whole mirror target, so when the canvas is the
// size of the target (condition 3 of docs~/architecture/editor-game-view-placement.md, which
// the glue drives) marker texel (x, y) reaches the same screen pixel as in the native blit,
// and the same decoder reads it. What Unity's blit does on the way (a flip, a split of a
// tall target into halves, a resample when the sizes differ) happens to the marker exactly
// as it happens to the woven image: the decoder then reports "not 1:1" or "no marker", which
// is the truth about the weave.
//
// This header is the part with no Unity and no device in it: the decision, the reasons and
// the constants. tests/gvcal_shader_test.cpp checks them and renders the stamp draw on the
// software rasteriser. The draws themselves are in displayxr_display_provider.cpp.

#pragma once

#include "displayxr_gvcal_shader.h"

// Why the stamp did or did not run for the marker that is requested now. Mirrored in
// DisplayXRGameViewCalibration.StampStatusText.
enum {
	DXR_GS_IDLE          = 0,  // no frame was submitted while a marker was requested
	DXR_GS_STAMPED       = 1,  // the marker is in the texture
	DXR_GS_DISABLED      = 2,  // DISPLAYXR_PROV_GV_CALIB_STAMP=0
	DXR_GS_NATIVE_LIVE   = 3,  // Unity calls the native mirror blit: the marker is drawn there
	DXR_GS_NO_BACKEND    = 4,  // the renderer is neither D3D11 nor D3D12
	DXR_GS_NO_INTERFACE  = 5,  // no IUnityGraphicsD3D11 / IUnityGraphicsD3D12v8, or no device
	DXR_GS_NO_WOVEN      = 6,  // no woven texture on Unity's device / none registered with Unity
	DXR_GS_CANVAS        = 7,  // the canvas is empty, too small for the marker, or not inside its texture
	DXR_GS_TEXTURE       = 8,  // the marker texture could not be created
	DXR_GS_UNITY_TEXTURE = 9,  // Unity refused it (CreateTexture failed)
	DXR_GS_PIPELINE      = 10, // shader / pipeline state / view creation failed
	DXR_GS_CMDLIST       = 11, // D3D12: no command allocator free, or the list could not be made
	DXR_GS_SETTLING      = 12, // the session has only just started: which mirror path is live is not known yet
	DXR_GS_REASONS
};

// Which draw put the marker on its way to the screen last (dxr_prov_get_gv_marker_path).
enum {
	DXR_GV_PATH_NONE         = 0,
	DXR_GV_PATH_NATIVE_D3D12 = 1, // the native mirror blit, D3D12
	DXR_GV_PATH_NATIVE_D3D11 = 2, // the native mirror blit, D3D11
	DXR_GV_PATH_STAMP        = 3, // stamped into the texture Unity's own blit samples
};

// Submitted frames before "Unity has not called the native blit" is taken to mean "this
// pipeline does not call it". A scriptable pipeline calls it every frame from the first.
#define DXR_GS_SETTLE_SUBMITS 8u
// The native blit counts as the live mirror path while it ran within this many submits
// (the #480 mirror copy uses the same window).
#define DXR_GS_NATIVE_LIVE_WINDOW 4u

static inline int
dxr_gvstamp_native_live(int native_seen, unsigned submits, unsigned native_last_submit)
{
	return (native_seen && (submits - native_last_submit) < DXR_GS_NATIVE_LIVE_WINDOW) ? 1 : 0;
}

// Is the canvas something the marker can be laid out in, inside a tw x th texture?
static inline int
dxr_gvstamp_canvas_ok(int cx, int cy, int cw, int ch, unsigned tw, unsigned th)
{
	if (cw < GVCAL_MIN_SIDE || ch < GVCAL_MIN_SIDE) return 0;
	if (cx < 0 || cy < 0) return 0;
	if ((long long)cx + cw > (long long)tw || (long long)cy + ch > (long long)th) return 0;
	return 1;
}

// May the marker be stamped this frame? DXR_GS_STAMPED = yes; anything else is the reason.
// The order is the order of the log lines a hardware run is read by: the opt-out first, then
// what makes the stamp unnecessary, then what makes it impossible.
static inline int
dxr_gvstamp_decide(int env_enabled, int native_live, unsigned submits, int backend_ok, int woven_registered,
                   int cx, int cy, int cw, int ch, unsigned tw, unsigned th)
{
	if (!env_enabled) return DXR_GS_DISABLED;
	if (native_live) return DXR_GS_NATIVE_LIVE;
	if (!backend_ok) return DXR_GS_NO_BACKEND;
	if (!woven_registered) return DXR_GS_NO_WOVEN;
	if (submits < DXR_GS_SETTLE_SUBMITS) return DXR_GS_SETTLING;
	if (!dxr_gvstamp_canvas_ok(cx, cy, cw, ch, tw, th)) return DXR_GS_CANVAS;
	return DXR_GS_STAMPED;
}

// Does the mirror query name the marker texture instead of the woven one? Only for the very
// request the texture was stamped for (a texture stamped for an earlier request holds the
// earlier marker), and only while the canvas the query is about to describe is the canvas
// that was stamped: the srcRect is computed from the query's canvas.
static inline int
dxr_gvstamp_hand_out(int requested, unsigned request_gen, unsigned stamped_gen, int has_texture,
                     int q_cx, int q_cy, int q_cw, int q_ch, int s_cx, int s_cy, int s_cw, int s_ch)
{
	if (!requested || !has_texture) return 0;
	if (request_gen == 0 || stamped_gen != request_gen) return 0;
	return (q_cx == s_cx && q_cy == s_cy && q_cw == s_cw && q_ch == s_ch) ? 1 : 0;
}

// The shader's 16 constants for the stamp draw of canvas (cx, cy, cw, ch) of a tw x th
// texture into a tw x th target: the draw's viewport is the canvas rect and each pixel LOADS
// the source texel at its own position (fl.w: an exact copy; the srcRect is filled in for the
// record and is what a filtered sample would use), no decode (the bytes are copied raw:
// source and target are one format family), the marker laid out for a cw x ch image whose
// texel (0,0) is target pixel (cx, cy). strip as gvcal_fill_constants.
static inline void
dxr_gvstamp_fill_constants(int cx, int cy, int cw, int ch, unsigned tw, unsigned th, int strip, int half,
                           float src_rect[4], unsigned mk[4], unsigned st[4], unsigned fl[4])
{
	src_rect[0] = (float)cx / (float)tw; src_rect[1] = (float)cy / (float)th;
	src_rect[2] = (float)cw / (float)tw; src_rect[3] = (float)ch / (float)th;
	gvcal_fill_constants(cw, ch, strip, half, mk, st);
	fl[0] = 0u; fl[1] = (unsigned)cx; fl[2] = (unsigned)cy; fl[3] = 1u;
}
