// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Native-Wayland player: pure placement arithmetic (no Wayland, no D-Bus), so it
// can be unit-tested on its own (native~/tests/test_wayland_place.c).
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

#ifdef __cplusplus
}
#endif
