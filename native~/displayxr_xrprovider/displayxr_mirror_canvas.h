// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Which part of the runtime-woven shared texture the editor Game view mirror shows
// (dxr_prov_get_woven_canvas). Pure arithmetic, in its own header so that it is
// unit-tested (native~/tests/render_scale_test.cpp) and so that its INPUTS are
// visible in one place:
//
//   the Game view pane rect, the 3D zone rect, the window size, the woven texture size.
//
// All of them are window / panel pixels. The eye swapchain size and the render scale
// (#475) are deliberately not among them: the mirror draws the woven picture texel
// for pixel (docs~/architecture/editor-game-view-placement.md), and how many pixels
// each eye was RENDERED at must never move or resize what is drawn. Adding an
// eye-size input here is the change that would break that - do not.

#pragma once

#include <stdint.h>

//! Everything the mirror canvas depends on.
typedef struct DxrMirrorCanvasIn
{
	int probe_enabled;          //!< editor texture mode is active
	int32_t pane_x, pane_y;     //!< pane position in the woven texture; INT32_MIN = not published (0)
	int32_t pane_w, pane_h;     //!< Game view pane px; <= 0 = not published
	int zone_valid;             //!< a 3D zone rect is set
	int32_t zone_x, zone_y, zone_w, zone_h;
} DxrMirrorCanvasIn;

//! The canvas from the pane (texture mode) or, without one, the zone. Returns 1 when
//! one of them gave a non-empty rect, else 0 with a zero rect (the caller then uses
//! dxr_mirror_canvas_fallback).
static inline int
dxr_mirror_canvas(const DxrMirrorCanvasIn *in, int32_t *x, int32_t *y, int32_t *w, int32_t *h)
{
	int32_t rx = 0, ry = 0, rw = 0, rh = 0;
	if (in->probe_enabled && in->pane_w > 0 && in->pane_h > 0) {
		// Zone-glue publishes the pane's screen position; legacy window-glue doesn't
		// (the weave window origin carries it) -> pane sits at the texture top-left.
		rx = (in->pane_x != INT32_MIN) ? in->pane_x : 0;
		ry = (in->pane_y != INT32_MIN) ? in->pane_y : 0;
		rw = in->pane_w;
		rh = in->pane_h;
	} else if (in->zone_valid && in->zone_w > 0 && in->zone_h > 0) {
		rx = in->zone_x;
		ry = in->zone_y;
		rw = in->zone_w;
		rh = in->zone_h;
	}
	*x = rx; *y = ry; *w = rw; *h = rh;
	return (rw > 0 && rh > 0) ? 1 : 0;
}

//! No pane and no zone: the window's size, else the whole woven texture.
static inline void
dxr_mirror_canvas_fallback(uint32_t window_w, uint32_t window_h, uint32_t woven_w, uint32_t woven_h,
                           int32_t *w, int32_t *h)
{
	*w = window_w ? (int32_t)window_w : (int32_t)woven_w;
	*h = window_h ? (int32_t)window_h : (int32_t)woven_h;
}
