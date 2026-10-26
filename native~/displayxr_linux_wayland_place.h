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

//! The output (an index into @p outs, @p n of them; a rect with no size = no
//! output) a window with its top-left at (x, y) is sized for once there. @p ws /
//! @p hs [i]: the size it will have on outs[i] once there (dxr_wl_size_after_move,
//! the size the saved-position check judges a position with too; 0 = unknown);
//! @p scale120 [i]: that output's scale x 120. An output qualifies when the window's
//! centre, at the size it has there, is on it (dxr_wl_window_on_rect; for a window
//! across a monitor edge the output under its top-left can be another one, with
//! another scale). Of those, the one with the highest scale: a window across a
//! monitor edge gets the highest scale of the monitors it is on (measured: mutter
//! 50, physical layout), so one that would be on the panel at the panel's size and on
//! the laptop at the laptop's gets the panel's. When none qualifies (partly off the
//! desktop), the one under (x, y). -1: neither.
static inline int
dxr_wl_output_for_window(const DxrWlRect4 *outs, const int *ws, const int *hs, const unsigned *scale120, int n, int x,
                         int y)
{
	int best = -1;
	for (int i = 0; i < n; i++)
		if (ws[i] > 0 && hs[i] > 0 && dxr_wl_window_on_rect(outs[i], x, y, ws[i], hs[i]) &&
		    (best < 0 || scale120[i] > scale120[best]))
			best = i;
	if (best >= 0)
		return best;
	for (int i = 0; i < n; i++)
		if (dxr_wl_window_on_rect(outs[i], x, y, 0, 0))
			return i;
	return -1;
}

//! The highest scale (x 120) of the outputs (@p outs, their scales @p scale120, @p n
//! of them; a rect with no size = no output) a window frame (x, y, w, h) is on, even
//! partly: the scale mutter gives a window across monitors (measured: mutter 50,
//! physical layout). 0: it is on none.
static inline unsigned
dxr_wl_highest_scale_under(const DxrWlRect4 *outs, const unsigned *scale120, int n, int x, int y, int w, int h)
{
	unsigned best = 0;
	for (int i = 0; i < n; i++) {
		const DxrWlRect4 o = outs[i];
		if (o.w > 0 && o.h > 0 && w > 0 && h > 0 && x < o.x + o.w && o.x < x + w && y < o.y + o.h && o.y < y + h &&
		    scale120[i] > best)
			best = scale120[i];
	}
	return best;
}

//! The scale (x 120) the window gets where the GNOME extension reports it now,
//! ahead of the fractional-scale protocol. The extension reports device px per
//! stage px (@p device_scale) for the window's monitor: that monitor's scale in
//! mutter's logical layout, but 1 on every monitor in the physical layout
//! (@p physical: stage px = device px), where the window's scale is the highest
//! scale of the monitors it is on (@p under120: dxr_wl_highest_scale_under). 0:
//! cannot be told.
static inline unsigned
dxr_wl_window_scale120(int physical, double device_scale, unsigned under120)
{
	if (physical)
		return under120;
	return device_scale > 0.0 ? (unsigned)dxr_wl_round(120.0 * device_scale) : 0;
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

//! The start-up placement (on by default): what to do with the window the
//! compositor opened where it chose (mutter: on the monitor under the pointer).
enum {
	DXR_WL_STARTUP_EXPIRED = -2, //!< not shown in time: no late jump
	DXR_WL_STARTUP_PLACED = -1,  //!< the app or the user placed it already: theirs
	DXR_WL_STARTUP_MOVE = 1,     //!< centre it on the 3D panel
	DXR_WL_STARTUP_THERE = 2,    //!< it is on the panel already: leave it
	DXR_WL_STARTUP_WAIT = 3,     //!< not shown yet: decide once it is
};

//! @p placed: the app (its own position, a move to the panel) or the user (a drag)
//! placed the window this run. @p shown: the extension reports it mapped, with its
//! frame (x, y, w, h) and monitor (@p mon; w <= 0 = not reported: the frame's centre
//! decides). A decision put off until the window is shown is dropped after
//! @p until_ns (0 = no limit), so a window that shows late (an extension that came
//! back minutes later) does not jump.
static inline int
dxr_wl_startup_action(int placed, int shown, DxrWlRect4 panel, DxrWlRect4 mon, int x, int y, int w, int h,
                      long long now_ns, long long until_ns)
{
	if (placed)
		return DXR_WL_STARTUP_PLACED;
	if (until_ns && now_ns > until_ns)
		return DXR_WL_STARTUP_EXPIRED;
	if (!shown)
		return DXR_WL_STARTUP_WAIT;
	int there = mon.w > 0 ? dxr_wl_monitor_is(mon, panel) : dxr_wl_window_on_rect(panel, x, y, w, h);
	return there ? DXR_WL_STARTUP_THERE : DXR_WL_STARTUP_MOVE;
}

//! A move onto another monitor keeps the window's LOGICAL size (the size it opened
//! with: the player restores the size it saved where it was last, normally the 3D
//! panel) instead of its DEVICE size, when nobody asked for a size (@p asked: a
//! size the app asked for stands) and the move decides where the window opens:
//!  - @p startup: a start-up move (dxr_wl_is_start_move): the start-up placement,
//!    or the app's own move to the panel at start (DisplayXRTargetDisplay), while
//!    neither the app nor the user has placed the window, so the window is still
//!    as it opened, however late it comes (a slow first frame);
//!  - @p window_new: any other placement while the window is new (#361's restore
//!    window: shown less than 3 s ago, not moved by the user): a later move to the
//!    panel or a saved position the app restores. An app can ask for those at any
//!    time; a later one keeps the device size, as on X11.
static inline int
dxr_wl_keeps_opening_size(int asked, int startup, int window_new)
{
	return !asked && (startup || window_new);
}

//! #361's restore window, the window "is new": a saved position the app restores is
//! checked against the 3D panel, and a placement keeps the size the window opened
//! with (dxr_wl_keeps_opening_size's @p window_new), while it is open. It is the
//! app's start, so it is measured in the time the app has been RUNNING since its
//! window was shown, not on the wall clock: a slow first frame (shader warm-up, a
//! stalled main thread) would otherwise close it before the app's first frames, and
//! with them its own placement (a saved position restored on its first frame, then
//! DisplayXRTargetDisplay's move), have run. dxr_wl_running_after_frame adds one
//! frame to @p running_ns; the window is open while @p running_ns is at most
//! @p running_limit_ns and the wall-clock time since the window was shown
//! (@p shown_ns) at most @p wall_limit_ns (an app paused for minutes does not keep it
//! open). The user moving the window closes it too (not here).
static inline int
dxr_wl_restore_window_open(long long running_ns, long long shown_ns, long long running_limit_ns,
                           long long wall_limit_ns)
{
	return running_ns <= running_limit_ns && shown_ns <= wall_limit_ns;
}

//! One frame of the app's: the time since the previous one (@p prev_ns, 0 = none:
//! nothing to add), but at most @p max_gap_ns, so a frame that took longer (a stall)
//! counts as a normal one would. Returns the new running time.
static inline long long
dxr_wl_running_after_frame(long long running_ns, long long prev_ns, long long now_ns, long long max_gap_ns)
{
	if (prev_ns <= 0 || now_ns <= prev_ns)
		return running_ns;
	long long d = now_ns - prev_ns;
	return running_ns + (d < max_gap_ns ? d : max_gap_ns);
}

//! A move is a start-up move (dxr_wl_keeps_opening_size's @p startup): one made as
//! part of the app's start (@p at_start: the start-up placement, which only moves a
//! window nobody has placed, or DisplayXRTargetDisplay's move once the session runs,
//! which says so on the native call) while neither the app nor the user has placed
//! the window yet (@p placed). A move the app asks for later (MoveNow) is not one.
static inline int
dxr_wl_is_start_move(int at_start, int placed)
{
	return at_start && !placed;
}

//! What a move of the window to the 3D panel does (move_player_to_panel, behind
//! dxr_wl_move_player_to_panel and dxr_wl_move_player_to_panel_at_start), which it
//! applies as it is: @p at_start: the _at_start entry point (DisplayXRTargetDisplay's
//! move once the session runs); @p placed: the app or the user placed the window
//! before; @p there: the window is on the panel already.
typedef struct DxrWlPanelMove {
	int move;       //!< centre the window on the panel (0: it is there: leave it)
	int start_move; //!< as a start-up move (s_startup_move): it keeps the size it opened with
} DxrWlPanelMove;

static inline DxrWlPanelMove
dxr_wl_panel_move(int at_start, int placed, int there)
{
	DxrWlPanelMove m;
	m.move = !there;
	m.start_move = m.move && dxr_wl_is_start_move(at_start, placed);
	return m;
}

//! Fit a window's logical size *lw x *lh into 95 % of an output (stage px
//! out_w x out_h), keeping its aspect: a window the compositor cannot fit gets
//! squeezed or maximized, and the player then resets it to the desktop size (and
//! saves that). @p stage_per_px: stage px per logical px, 1 in logical layout, the
//! output's integer scale in physical layout.
static inline void
dxr_wl_fit_output(int *lw, int *lh, int out_w, int out_h, double stage_per_px)
{
	if (out_w <= 0 || out_h <= 0 || *lw <= 0 || *lh <= 0)
		return;
	if (stage_per_px <= 0.0)
		stage_per_px = 1.0;
	double max_w = 0.95 * out_w / stage_per_px, max_h = 0.95 * out_h / stage_per_px;
	double f = 1.0;
	if (*lw > max_w)
		f = max_w / *lw;
	if (*lh * f > max_h)
		f = max_h / *lh;
	if (f < 1.0) {
		*lw = (int)(*lw * f); // (positive: truncation = floor)
		*lh = (int)(*lh * f);
	}
}

//! The size the window will have on an output once moved there, stage px: what
//! centring it there or judging a saved position there has to go by.
//! @p keep_logical (dxr_wl_keeps_opening_size): the move keeps its logical size
//! lw x lh; else it keeps the device size dw x dh (the app's request, or the size
//! where its scale was first known; <= 0 = none: lw x lh). @p t120: the output's
//! device px per logical px x 120 (0 = unknown: taken as 1); @p cur120: the
//! window's now. On another scale the player is asked for the size anew, fitted
//! into the output (dxr_wl_fit_output); on the same one the window keeps its size.
//! @p physical: mutter's physical layout (stage px = device px), else logical
//! (stage px = logical px). out_w x out_h: the output's stage size.
static inline void
dxr_wl_size_after_move(int keep_logical, int lw, int lh, int dw, int dh, unsigned t120, unsigned cur120, int physical,
                       int out_w, int out_h, int *res_w, int *res_h)
{
	double s = t120 ? t120 / 120.0 : 1.0;
	int w = lw, h = lh; // logical px there
	if (!keep_logical && dw > 0 && dh > 0) {
		w = dxr_wl_round(dw / s);
		h = dxr_wl_round(dh / s);
	}
	if (t120 && t120 != cur120)
		dxr_wl_fit_output(&w, &h, out_w, out_h, physical ? s : 1.0);
	*res_w = physical ? dxr_wl_round(w * s) : w;
	*res_h = physical ? dxr_wl_round(h * s) : h;
}

//! DISPLAYXR_PANEL_PLACEMENT turns the start-up placement off: "0", "off", "no" or
//! "false", any case. Unset, empty or anything else: on.
static inline int
dxr_startup_placement_env_off(const char *v)
{
	static const char *const offs[] = {"0", "off", "no", "false"};
	if (!v || !v[0])
		return 0;
	for (unsigned i = 0; i < sizeof(offs) / sizeof(offs[0]); i++) {
		const char *a = v, *b = offs[i];
		while (*a && *b && (*a >= 'A' && *a <= 'Z' ? *a + 32 : *a) == *b) {
			a++;
			b++;
		}
		if (!*a && !*b)
			return 1;
	}
	return 0;
}

#ifdef __cplusplus
}
#endif
