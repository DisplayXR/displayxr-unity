// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Native-Wayland player: pure placement and sizing arithmetic (no Wayland, no
// D-Bus), so it can be unit-tested on its own (native~/tests/test_wayland_place.c).
//
// All rects are in mutter's STAGE coordinates (what xdg-output reports for each
// output and what the GNOME extension reports for a window's frame and monitor).

#pragma once

#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

//! A rect, stage px. w or h <= 0 = none.
typedef struct DxrWlRect4 {
	int x, y, w, h;
} DxrWlRect4;

//! An X root coordinate (what the app-facing position API speaks, and what the
//! X11 path saves) to stage px: X root = stage x n, n = the XWayland scale.
//! Rounds to nearest; n <= 0 returns v unchanged.
static inline int
dxr_wl_x11_to_stage(int v, int n)
{
	if (n <= 0)
		return v;
	return v >= 0 ? (v + n / 2) / n : -((-v + n / 2) / n);
}

//! Round to nearest, halves away from zero (no libm).
static inline int
dxr_wl_round(double v)
{
	return v >= 0 ? (int)(v + 0.5) : -(int)(-v + 0.5);
}

//! A point on the player's main surface (surface-local px, one axis) to stage px
//! and X root px: stage = origin + local x k, where origin is the surface's
//! top-left on the stage (the extension's "buffer" rect) and k the stage px per
//! surface px (1 in logical layout; the output's integer scale in physical
//! layout); X root = stage x n (n = the XWayland scale). With n <= 0 (unknown)
//! only *out_stage is set and 0 is returned.
static inline int
dxr_wl_surface_point(int origin, double local, int k, int n, int *out_stage, int *out_root)
{
	double stage = origin + local * (k > 0 ? k : 1);
	*out_stage = dxr_wl_round(stage);
	if (n <= 0)
		return 0;
	*out_root = dxr_wl_round(stage * n);
	return 1;
}

//! A window of size w x h with its top-left at (x, y) lies on `panel`: its centre
//! is inside the panel's rect (the rule mutter uses to say which monitor a window
//! is on). With no size (w or h <= 0) the top-left itself must be inside.
static inline int
dxr_wl_window_on_rect(DxrWlRect4 panel, int x, int y, int w, int h)
{
	if (panel.w <= 0 || panel.h <= 0)
		return 0;
	int cx = w > 0 ? x + w / 2 : x;
	int cy = h > 0 ? y + h / 2 : y;
	return cx >= panel.x && cx < panel.x + panel.w && cy >= panel.y && cy < panel.y + panel.h;
}

//! A saved position (X root px, as the app restores it) is a place on the 3D
//! panel today: converted back to stage px with the XWayland scale n, the window
//! (stage size w x h; 0 = unknown) would sit on `panel`. A position saved under
//! another monitor layout fails this and is stale.
static inline int
dxr_wl_saved_position_on_panel(DxrWlRect4 panel, int x11_x, int x11_y, int n, int w, int h)
{
	if (n <= 0)
		return 0;
	return dxr_wl_window_on_rect(panel, dxr_wl_x11_to_stage(x11_x, n), dxr_wl_x11_to_stage(x11_y, n), w, h);
}

//! The monitor the extension reports for our window is the 3D panel's output
//! (the same stage rect, to a pixel of rounding).
static inline int
dxr_wl_monitor_is(DxrWlRect4 monitor, DxrWlRect4 panel)
{
	if (monitor.w <= 0 || monitor.h <= 0 || panel.w <= 0 || panel.h <= 0)
		return 0;
	return abs(monitor.x - panel.x) <= 1 && abs(monitor.y - panel.y) <= 1 && abs(monitor.w - panel.w) <= 1 &&
	       abs(monitor.h - panel.h) <= 1;
}

//! A size handed to the player (sent_w x sent_h logical px), checked once it has
//! had until `until_ns` to apply it: 0 = still waiting, 1 = its window has it (or
//! nothing was sent), -1 = it does not. The player dropped the request or reverted
//! it, which it can do inside one frame (resize, then back) when the compositor
//! moves the window mid-resize, so the per-frame poll never sees the size change.
static inline int
dxr_wl_sent_size_outcome(long long now_ns, long long until_ns, int sent_w, int sent_h, int cur_w, int cur_h)
{
	if (sent_w <= 0 || sent_h <= 0)
		return 1;
	if (now_ns < until_ns)
		return 0;
	return cur_w == sent_w && cur_h == sent_h ? 1 : -1;
}

#ifdef __cplusplus
}
#endif
