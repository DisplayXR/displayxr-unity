// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Side-step: the app's own window steps aside when the window the user is working
// in covers the app's content, and goes back once its place is free again (Linux,
// native-Wayland player on GNOME with the DisplayXR GNOME extension). The worker
// and the exports are in displayxr_linux_side_step.c; this header holds the pure
// part (no D-Bus, no Wayland, no clock), so it is unit-tested on its own
// (native~/tests/test_linux_side_step.c):
//   - the desktop-control gate in front of every poll, and the glide that stops
//     when the grant ends (both through DxrSsOps, so the test counts the reads),
//   - reading the extension's window list into the few facts the decision needs,
//   - which other window counts,
//   - how much of the app's content a rect covers,
//   - where to step to,
//   - the decision itself, one tick at a time.
//
// CONSENT. Side-step runs only while the app's desktop control is GRANTED (the
// #389 gate, runtime ADR-056: the same synchronous runtime query that
// DisplayXRNative.LinuxWindows / LinuxWindowAt / LinuxPointer answer behind).
// dxr_ss_poll asks the gate first on every poll and reads nothing about other
// windows (nor the lock state) unless it says yes. Suspended (the screen is locked):
// it holds still and drops the id of the window it stepped aside for, but keeps the
// app's own home and whether it is aside, and carries on from there once granted
// again. Any other no: everything the decision remembered (home, the window it
// stepped aside for) is forgotten. Either way the window stays where it is
// (docs~/architecture/side-step.md).
//
// PRIVACY. Other windows' frames and focus are read here, inside the plug-in, and
// never leave it: no other window's id, pid, app id, title or rect is stored past
// one tick (except the id of the window the app stepped aside for, to tell when
// it is gone, and only while the grant holds), logged, or handed to the app. The
// app learns only facts about its OWN window.
//
// COORDINATES. Everything is in mutter's stage coordinates, what the extension
// reports (logical px in its logical layout mode). The app's content ("body") is
// held as fractions of its window, so it does not depend on the window's scale.

#pragma once

#include "displayxr_linux_json.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

//! The settings the app passes (displayxr_linux_side_step_set). 28 bytes; the C#
//! mirror is DisplayXRSideStep.NativeSettings.
typedef struct DisplayXRSideStepSettings {
	float trigger;        //  0  share of the app's content covered that counts (0..1)
	float dwell_s;        //  4  covered this long before stepping aside
	float return_delay_s; //  8  home free this long before going back
	float glide_s;        // 12  a move glides this long; 0 = one jump
	int32_t return_home;  // 16  1 = go back home once it is free
	int32_t side;         // 20  preferred side, DXR_SS_SIDE_*
	int32_t margin;       // 24  stage px kept from the other window and the monitor's edge
} DisplayXRSideStepSettings;

//! What the app learns (displayxr_linux_side_step_status): its OWN window only. 68
//! bytes; the C# mirror is DisplayXRSideStep.NativeStatus.
typedef struct DisplayXRSideStepStatus {
	int32_t state;             //  0  DXR_SS_STATE_*
	int32_t reason;            //  4  DXR_SS_REASON_*
	float covered;             //  8  share of the app's content covered now (0..1)
	int32_t aside;             // 12  1 = away from home (stepped aside, or moving)
	int32_t window_valid;      // 16  the four below are known
	int32_t win_x, win_y;      // 20  the app's window frame, stage px
	int32_t win_w, win_h;      // 28
	int32_t home_valid;        // 36  home is known
	int32_t home_x, home_y;    // 40  home, stage px
	int32_t home_xroot_valid;  // 48  home in X root px is known (the X root scale is)
	int32_t home_xroot_x;      // 52  home in X root px: displayxr_get/set_overlay_position's space
	int32_t home_xroot_y;      // 56
	uint32_t aside_count;      // 60  steps aside that ended (one more each time)
	uint32_t home_count;       // 64  returns home that ended
} DisplayXRSideStepStatus;

//! A rect, stage px. w or h <= 0 = empty.
typedef struct DxrSsRect {
	int x, y, w, h;
} DxrSsRect;

//! Cells per side of the body grid.
#define DXR_SS_GRID 32

//! Where the app's content is inside its window: per cell of a DXR_SS_GRID square
//! grid laid over the window, the share of the cell the content covers (0..255).
//! valid = 0: no content shape is known, the whole window counts.
typedef struct DxrSsBody {
	int valid;
	uint8_t cell[DXR_SS_GRID * DXR_SS_GRID];
} DxrSsBody;

//! Settings (the C# DisplayXRSideStep properties). dxr_ss_clamp_settings keeps them sane.
typedef struct DxrSsSettings {
	float trigger;        //!< share of the app's content covered that counts (0.15)
	float dwell_s;        //!< covered this long before stepping aside (0.6 s)
	float return_delay_s; //!< home free this long before going back (2 s)
	float glide_s;        //!< a move glides this long; 0 = one jump (0.3 s)
	int return_home;      //!< 1 = go back home once it is free; 0 = the new place becomes home
	int side;             //!< preferred side, DXR_SS_SIDE_*
	int margin;           //!< stage px kept from the other window and the monitor's edge (24)
} DxrSsSettings;

#define DXR_SS_SIDE_AUTO 0
#define DXR_SS_SIDE_LEFT 1
#define DXR_SS_SIDE_RIGHT 2
#define DXR_SS_SIDE_ABOVE 3
#define DXR_SS_SIDE_BELOW 4

// Fixed timing (not settings).
#define DXR_SS_START_S 2.0     //!< after the session's window shows: its start-up placement first
#define DXR_SS_SETTLE_S 1.0    //!< a window that moved (the user, the app) is home once it stayed this long
#define DXR_SS_LAND_S 2.0      //!< our move: wait this long for it to land, then take where it is
#define DXR_SS_GAP_S 1.0       //!< at least this long between one of our moves landing and the next
#define DXR_SS_CAP_MOVES 6     //!< at most this many steps aside ...
#define DXR_SS_CAP_WINDOW_S 60.0 //!< ... in this long; then it stays (DXR_SS_REASON_TOO_MANY_MOVES).
                                 //!< Returns home are not capped: each follows a step aside and
                                 //!< ends it, so the cap never strands the window away from home.
#define DXR_SS_LANDED_PX 2     //!< within this of the target = landed

//! State, as the exports report it (DisplayXRSideStepStatus.state; C# SideStepState).
#define DXR_SS_STATE_OFF 0
#define DXR_SS_STATE_UNSUPPORTED 1
#define DXR_SS_STATE_WAITING 2
#define DXR_SS_STATE_PAUSED 3
#define DXR_SS_STATE_HOME 4
#define DXR_SS_STATE_MOVING 5
#define DXR_SS_STATE_ASIDE 6

//! Why (DisplayXRSideStepStatus.reason; C# SideStepReason).
#define DXR_SS_REASON_NONE 0
#define DXR_SS_REASON_PLATFORM 1          //!< not a Linux player (C# only)
#define DXR_SS_REASON_NOT_NATIVE_WAYLAND 2 //!< an X11 (XWayland) player
#define DXR_SS_REASON_NO_SESSION 3        //!< no native-Wayland XR session (weave) yet
#define DXR_SS_REASON_NO_EXTENSION 4      //!< no window list from the DisplayXR GNOME extension
#define DXR_SS_REASON_NO_WINDOW 5         //!< the app's window is not listed (not shown yet)
#define DXR_SS_REASON_SETTLING 6          //!< waiting for the window to stay put (start-up, a move)
#define DXR_SS_REASON_LOCKED 7            //!< the screen is locked
#define DXR_SS_REASON_USER_MOVING 8       //!< the user is moving (or resizing) the window
#define DXR_SS_REASON_MINIMIZED 9
#define DXR_SS_REASON_MAXIMIZED 10
#define DXR_SS_REASON_FULLSCREEN 11
#define DXR_SS_REASON_TOO_MANY_MOVES 12   //!< DXR_SS_CAP_MOVES steps aside in DXR_SS_CAP_WINDOW_S: staying
#define DXR_SS_REASON_NO_FREE_SPOT 13     //!< covered, but no place on this monitor is free
#define DXR_SS_REASON_NATIVE_MISSING 14   //!< (C# only) a native plug-in without side-step
#define DXR_SS_REASON_NOT_GRANTED 15      //!< the app's desktop control is not Granted: nothing is read

//! Events of one tick (dxr_ss_step), for the log and the counters.
#define DXR_SS_EV_NONE 0
#define DXR_SS_EV_NEW_HOME 1     //!< the window stayed put at a place it was not moved to by us: home
#define DXR_SS_EV_LANDED_ASIDE 2 //!< our step aside landed (or ended where the compositor kept it)
#define DXR_SS_EV_LANDED_HOME 3  //!< our return home landed
#define DXR_SS_EV_NO_SPOT 4      //!< covered, nowhere free (once per streak)
#define DXR_SS_EV_CAPPED 5       //!< covered, but the move cap is reached (once per streak)
#define DXR_SS_EV_ABORTED 6      //!< the user took the window during our move

//! What one tick of the window list says, reduced to what the decision needs.
typedef struct DxrSsInput {
	double now;      //!< seconds, monotonic
	int locked;      //!< the screen is locked
	int own_valid;   //!< the app's window is listed
	DxrSsRect own;   //!< its frame
	DxrSsRect mon;   //!< its monitor (w = 0: not reported)
	int own_minimized, own_maximized, own_fullscreen;
	int own_moving;  //!< a move/resize grab, or the plug-in's own pointer drag
	int focus_valid; //!< another app's window that counts has the focus ...
	DxrSsRect focus; //!< ... its frame ...
	uint64_t focus_id; //!< ... and mutter's id for it (0 = not reported)
	int tracked_valid; //!< the window we stepped aside for is still there (not minimized)
	DxrSsRect tracked;
	const DxrSsBody *body; //!< NULL or !valid: the whole window
} DxrSsInput;

//! The decision's memory between ticks.
typedef struct DxrSsState {
	int phase;      //!< DXR_SS_PHASE_*
	double start_at; //!< no decision before this (<0: not started)
	int have_home;
	int home_x, home_y;
	int last_valid, last_x, last_y; //!< where the window was seen, not counting our moves
	double stable_since;
	double in_way_since;     //!< <0: not covered
	double home_clear_since; //!< <0: home not free
	int target_x, target_y, target_home;
	double move_since;
	double next_move_at;
	double moves[DXR_SS_CAP_MOVES]; //!< when our last steps aside were made (ring)
	int n_moves;
	uint64_t aside_for; //!< mutter's id of the window we stepped aside for; 0 none
	int streak_reported; //!< NO_SPOT / CAPPED said once for this covered streak
	float covered;  //!< the last tick's share of the app's content covered
	int reason;     //!< the last tick's DXR_SS_REASON_*
} DxrSsState;

#define DXR_SS_PHASE_SETTLING 0 //!< waiting for the window to stay put; then it is home
#define DXR_SS_PHASE_HOME 1
#define DXR_SS_PHASE_MOVING 2
#define DXR_SS_PHASE_ASIDE 3

//! One tick's outcome.
typedef struct DxrSsDecision {
	int move;          //!< move the window to (x, y) now
	int x, y;
	int home;          //!< that move is the way back home
	int event;         //!< DXR_SS_EV_*
	float covered;     //!< the share covered that made it step aside
	float spot_covered; //!< the share covered at the place it steps to
} DxrSsDecision;

static inline void
dxr_ss_default_settings(DxrSsSettings *s)
{
	s->trigger = 0.15f;
	s->dwell_s = 0.6f;
	s->return_delay_s = 2.0f;
	s->glide_s = 0.3f;
	s->return_home = 1;
	s->side = DXR_SS_SIDE_AUTO;
	s->margin = 24;
}

//! Keep settings in range (NaN and nonsense from the app included).
static inline void
dxr_ss_clamp_settings(DxrSsSettings *s)
{
	if (!(s->trigger >= 0.01f))
		s->trigger = 0.01f;
	if (s->trigger > 1.0f)
		s->trigger = 1.0f;
	if (!(s->dwell_s >= 0.0f))
		s->dwell_s = 0.0f;
	if (s->dwell_s > 60.0f)
		s->dwell_s = 60.0f;
	if (!(s->return_delay_s >= 0.0f))
		s->return_delay_s = 0.0f;
	if (s->return_delay_s > 600.0f)
		s->return_delay_s = 600.0f;
	if (!(s->glide_s >= 0.0f))
		s->glide_s = 0.0f;
	if (s->glide_s > 2.0f)
		s->glide_s = 2.0f;
	s->return_home = s->return_home ? 1 : 0;
	if (s->side < DXR_SS_SIDE_AUTO || s->side > DXR_SS_SIDE_BELOW)
		s->side = DXR_SS_SIDE_AUTO;
	if (s->margin < 0)
		s->margin = 0;
	if (s->margin > 512)
		s->margin = 512;
}

static inline void
dxr_ss_reset(DxrSsState *st)
{
	memset(st, 0, sizeof(*st));
	st->phase = DXR_SS_PHASE_SETTLING;
	st->start_at = -1.0;
	st->in_way_since = -1.0;
	st->home_clear_since = -1.0;
}

/*
 *
 * Geometry.
 *
 */

static inline long long
dxr_ss_area(DxrSsRect r)
{
	return r.w > 0 && r.h > 0 ? (long long)r.w * r.h : 0;
}

static inline double
dxr_ss_overlap_d(double ax, double ay, double aw, double ah, DxrSsRect b)
{
	double l = ax > b.x ? ax : b.x, t = ay > b.y ? ay : b.y;
	double r = ax + aw < b.x + b.w ? ax + aw : b.x + b.w;
	double bt = ay + ah < b.y + b.h ? ay + ah : b.y + b.h;
	return r > l && bt > t ? (r - l) * (bt - t) : 0.0;
}

//! Mark the content rect [x0, x1) x [y0, y1), in px of a window w x h, in the grid
//! (adds the share of each cell it covers; overlapping rects saturate at a full cell).
static inline void
dxr_ss_body_add_rect(float *acc, double x0, double y0, double x1, double y1, double w, double h)
{
	if (w <= 0 || h <= 0)
		return;
	if (x0 < 0)
		x0 = 0;
	if (y0 < 0)
		y0 = 0;
	if (x1 > w)
		x1 = w;
	if (y1 > h)
		y1 = h;
	if (x1 <= x0 || y1 <= y0)
		return;
	double cw = w / DXR_SS_GRID, ch = h / DXR_SS_GRID;
	int i0 = (int)floor(x0 / cw), i1 = (int)ceil(x1 / cw);
	int j0 = (int)floor(y0 / ch), j1 = (int)ceil(y1 / ch);
	if (i1 > DXR_SS_GRID)
		i1 = DXR_SS_GRID;
	if (j1 > DXR_SS_GRID)
		j1 = DXR_SS_GRID;
	for (int j = j0; j < j1; j++) {
		double cy0 = j * ch, cy1 = cy0 + ch;
		double oy = (y1 < cy1 ? y1 : cy1) - (y0 > cy0 ? y0 : cy0);
		if (oy <= 0)
			continue;
		for (int i = i0; i < i1; i++) {
			double cx0 = i * cw, cx1 = cx0 + cw;
			double ox = (x1 < cx1 ? x1 : cx1) - (x0 > cx0 ? x0 : cx0);
			if (ox > 0)
				acc[j * DXR_SS_GRID + i] += (float)(ox * oy / (cw * ch));
		}
	}
}

//! The grid from accumulated shares (dxr_ss_body_add_rect).
static inline void
dxr_ss_body_from_acc(DxrSsBody *b, const float *acc)
{
	b->valid = 1;
	for (int k = 0; k < DXR_SS_GRID * DXR_SS_GRID; k++) {
		float v = acc[k];
		b->cell[k] = v >= 1.0f ? 255 : v <= 0.0f ? 0 : (uint8_t)lroundf(v * 255.0f);
	}
}

//! The content's bounding box, as offsets into a window of size w x h (stage px).
//! 0 when the body has no content.
static inline int
dxr_ss_body_bbox(const DxrSsBody *b, int w, int h, DxrSsRect *out)
{
	if (!b || !b->valid) {
		out->x = out->y = 0;
		out->w = w;
		out->h = h;
		return w > 0 && h > 0;
	}
	int i0 = DXR_SS_GRID, i1 = -1, j0 = DXR_SS_GRID, j1 = -1;
	for (int j = 0; j < DXR_SS_GRID; j++)
		for (int i = 0; i < DXR_SS_GRID; i++)
			if (b->cell[j * DXR_SS_GRID + i]) {
				if (i < i0)
					i0 = i;
				if (i > i1)
					i1 = i;
				if (j < j0)
					j0 = j;
				if (j > j1)
					j1 = j;
			}
	if (i1 < 0)
		return 0;
	int x0 = (int)floor((double)i0 * w / DXR_SS_GRID), x1 = (int)ceil((double)(i1 + 1) * w / DXR_SS_GRID);
	int y0 = (int)floor((double)j0 * h / DXR_SS_GRID), y1 = (int)ceil((double)(j1 + 1) * h / DXR_SS_GRID);
	out->x = x0;
	out->y = y0;
	out->w = x1 - x0;
	out->h = y1 - y0;
	return out->w > 0 && out->h > 0;
}

//! The share (0..1) of the app's content, with its window's frame at `win`, that
//! `r` covers. The whole window counts when the body is not known; 0 when the body
//! has no content or `r` is empty.
static inline float
dxr_ss_coverage(const DxrSsBody *b, DxrSsRect win, DxrSsRect r)
{
	if (win.w <= 0 || win.h <= 0 || r.w <= 0 || r.h <= 0)
		return 0.0f;
	if (!b || !b->valid)
		return (float)(dxr_ss_overlap_d(win.x, win.y, win.w, win.h, r) / ((double)win.w * win.h));
	// Quick out: no overlap with the window at all.
	if (dxr_ss_overlap_d(win.x, win.y, win.w, win.h, r) <= 0.0)
		return 0.0f;
	double cw = (double)win.w / DXR_SS_GRID, ch = (double)win.h / DXR_SS_GRID;
	double total = 0.0, hit = 0.0;
	for (int j = 0; j < DXR_SS_GRID; j++) {
		for (int i = 0; i < DXR_SS_GRID; i++) {
			int v = b->cell[j * DXR_SS_GRID + i];
			if (!v)
				continue;
			total += v;
			hit += v * dxr_ss_overlap_d(win.x + i * cw, win.y + j * ch, cw, ch, r) / (cw * ch);
		}
	}
	return total > 0.0 ? (float)(hit / total) : 0.0f;
}

static inline int
dxr_ss_clampi(int v, int lo, int hi)
{
	if (hi < lo)
		return lo; // the window is wider than the monitor: its left / top edge
	return v < lo ? lo : v > hi ? hi : v;
}

#define DXR_SS_N_SPOTS 8

//! Where to step to: beside the covering window `k` (left, right, above, below) or
//! in a corner of the monitor `mon`, the content `margin` px from either; the whole
//! window stays on the monitor (a window whose centre crosses to another monitor
//! would change scale, and the plug-in's own placement check would move it back).
//! Least covered first; then the preferred side (DXR_SS_SIDE_*); then the shortest
//! move. A place within DXR_SS_LANDED_PX of where the window is does not count.
//! Returns 1 and the window's new top-left in (*x, *y), with the share covered
//! there in *cov; 0 when there is no other place.
static inline int
dxr_ss_find_spot(const DxrSsBody *b, DxrSsRect win, DxrSsRect k, DxrSsRect mon, int margin, int side, int *x,
                 int *y, float *cov)
{
	DxrSsRect bb;
	if (!dxr_ss_body_bbox(b, win.w, win.h, &bb))
		return 0;
	if (mon.w <= 0 || mon.h <= 0)
		mon = win; // no monitor reported: nowhere to go but where it is
	int m = margin;
	// The body's top-left (stage) for each candidate, then the window's.
	int bx = win.x + bb.x, by = win.y + bb.y;
	struct {
		int x, y, side_a, side_b;
	} c[DXR_SS_N_SPOTS] = {
	    {k.x - m - bb.w, by, DXR_SS_SIDE_LEFT, -1},
	    {k.x + k.w + m, by, DXR_SS_SIDE_RIGHT, -1},
	    {bx, k.y - m - bb.h, DXR_SS_SIDE_ABOVE, -1},
	    {bx, k.y + k.h + m, DXR_SS_SIDE_BELOW, -1},
	    {mon.x + m, mon.y + m, DXR_SS_SIDE_LEFT, DXR_SS_SIDE_ABOVE},
	    {mon.x + mon.w - m - bb.w, mon.y + m, DXR_SS_SIDE_RIGHT, DXR_SS_SIDE_ABOVE},
	    {mon.x + m, mon.y + mon.h - m - bb.h, DXR_SS_SIDE_LEFT, DXR_SS_SIDE_BELOW},
	    {mon.x + mon.w - m - bb.w, mon.y + mon.h - m - bb.h, DXR_SS_SIDE_RIGHT, DXR_SS_SIDE_BELOW},
	};
	int found = 0, best_x = 0, best_y = 0, best_pref = 0;
	float best_cov = 0.0f;
	double best_dist = 0.0;
	for (int i = 0; i < DXR_SS_N_SPOTS; i++) {
		int wx = dxr_ss_clampi(c[i].x - bb.x, mon.x, mon.x + mon.w - win.w);
		int wy = dxr_ss_clampi(c[i].y - bb.y, mon.y, mon.y + mon.h - win.h);
		if (abs(wx - win.x) <= DXR_SS_LANDED_PX && abs(wy - win.y) <= DXR_SS_LANDED_PX)
			continue;
		DxrSsRect at = {wx, wy, win.w, win.h};
		float cv = dxr_ss_coverage(b, at, k);
		int pref = side != DXR_SS_SIDE_AUTO && (c[i].side_a == side || c[i].side_b == side);
		double dist = hypot((double)(wx - win.x), (double)(wy - win.y));
		int better;
		if (!found)
			better = 1;
		else if (cv < best_cov - 0.01f)
			better = 1;
		else if (cv > best_cov + 0.01f)
			better = 0;
		else if (pref != best_pref)
			better = pref;
		else
			better = dist < best_dist;
		if (better) {
			found = 1;
			best_x = wx;
			best_y = wy;
			best_cov = cv;
			best_pref = pref;
			best_dist = dist;
		}
	}
	if (!found)
		return 0;
	*x = best_x;
	*y = best_y;
	*cov = best_cov;
	return 1;
}

/*
 *
 * The extension's window list.
 *
 */

//! Mutter's window types (Meta.WindowType) that count as a window the user works
//! in: normal, dialog, modal dialog, utility. GetAllWindows lists only these;
//! GetWindows only normal ones (no "window_type" then).
static inline int
dxr_ss_type_counts(int type)
{
	return type == 0 || type == 3 || type == 4 || type == 7;
}

//! A window that IS the desktop: Ubuntu's desktop-icons window ("ding"), a NORMAL
//! window before mutter 49 that covers the whole monitor and takes the focus when
//! the user clicks the desktop. It never counts.
static inline int
dxr_ss_is_desktop_app(const char *app_id)
{
	return !strcmp(app_id, "com.rastersoft.ding") || !strcmp(app_id, "desktop-icons-ng") ||
	       !strcmp(app_id, "nautilus-desktop");
}

static inline DxrSsRect
dxr_ss_json_rect4(const char *f, const char *end)
{
	DxrSsRect r = {0, 0, 0, 0};
	if (!f || *f != '[')
		return r;
	int v[4] = {0, 0, 0, 0};
	const char *e = f + 1;
	for (int k = 0; k < 4; k++) {
		e = dxr_json_ws(e, end);
		double d = dxr_json_number(e, end, NAN);
		if (isnan(d))
			return r;
		v[k] = (int)lround(d);
		e = dxr_json_skip(e, end);
		if (!e)
			return r;
		e = dxr_json_ws(e, end);
		if (e >= end || *e != (k < 3 ? ',' : ']'))
			return r;
		e++;
	}
	r.x = v[0];
	r.y = v[1];
	r.w = v[2];
	r.h = v[3];
	return r;
}

static inline uint64_t
dxr_ss_json_u64(const char *v, const char *end)
{
	if (!v || v >= end || *v < '0' || *v > '9')
		return 0;
	uint64_t r = 0;
	const char *start = v;
	for (; v < end && *v >= '0' && *v <= '9'; v++)
		r = r * 10u + (uint64_t)(*v - '0');
	if (v < end && (*v == '.' || *v == 'e' || *v == 'E')) {
		double d = dxr_json_number(start, end, 0.0);
		return d >= 0.0 && d < 18446744073709551616.0 ? (uint64_t)d : 0;
	}
	return r;
}

//! A GetAllWindows / GetWindows reply (schema version 1) into `in`: the app's own
//! window (pid `own_pid`; the largest normal one when it has several), the focused
//! window of another app if it counts, and the window `tracked_id` (we stepped
//! aside for it) if it is still there and not minimized. What counts: another
//! process's window, of a type the user works in (dxr_ss_type_counts), not the
//! desktop window (dxr_ss_is_desktop_app), not minimized, with a frame. Nothing
//! else of other windows is kept. Returns 1, or 0 when the reply cannot be read
//! (`in` then says no window at all). Leaves now / locked / body alone.
static inline int
dxr_ss_parse_windows(const char *json, int own_pid, uint64_t tracked_id, DxrSsInput *in)
{
	in->own_valid = in->focus_valid = in->tracked_valid = 0;
	in->own_minimized = in->own_maximized = in->own_fullscreen = in->own_moving = 0;
	in->focus_id = 0;
	memset(&in->own, 0, sizeof(in->own));
	memset(&in->mon, 0, sizeof(in->mon));
	memset(&in->focus, 0, sizeof(in->focus));
	memset(&in->tracked, 0, sizeof(in->tracked));
	if (!json)
		return 0;
	const char *end = json + strlen(json);
	double ver = dxr_json_number(dxr_json_field(json, end, "version"), end, -1.0);
	if (ver != 1.0)
		return 0;
	const char *p = dxr_json_field(json, end, "windows");
	if (!p || *p != '[')
		return 0;
	long long own_area = -1;
	for (const char *q = p + 1;;) {
		q = dxr_json_ws(q, end);
		if (q < end && *q == ']')
			break;
		if (q >= end || *q != '{')
			return 0;
		const char *o = dxr_json_skip(q, end);
		if (!o)
			return 0;
		int pid = (int)lround(dxr_json_number(dxr_json_field(q, o, "pid"), o, 0.0));
		int type = (int)lround(dxr_json_number(dxr_json_field(q, o, "window_type"), o, 0.0));
		int minimized = dxr_json_bool(dxr_json_field(q, o, "minimized"), o, 0);
		DxrSsRect frame = dxr_ss_json_rect4(dxr_json_field(q, o, "frame"), o);
		if (pid == own_pid && own_pid > 0) {
			// The app's own window: the largest normal one.
			if (type == 0 && dxr_ss_area(frame) > own_area) {
				own_area = dxr_ss_area(frame);
				in->own_valid = frame.w > 0 && frame.h > 0;
				in->own = frame;
				in->own_minimized = minimized;
				in->own_maximized = dxr_json_bool(dxr_json_field(q, o, "maximized"), o, 0);
				in->own_fullscreen = dxr_json_bool(dxr_json_field(q, o, "fullscreen"), o, 0);
				in->own_moving = dxr_json_bool(dxr_json_field(q, o, "moving"), o, 0);
				memset(&in->mon, 0, sizeof(in->mon));
				const char *m = dxr_json_field(q, o, "monitor");
				const char *m_end = m && *m == '{' ? dxr_json_skip(m, o) : NULL;
				if (m_end) {
					in->mon.x = (int)lround(dxr_json_number(dxr_json_field(m, m_end, "x"), m_end, 0.0));
					in->mon.y = (int)lround(dxr_json_number(dxr_json_field(m, m_end, "y"), m_end, 0.0));
					in->mon.w = (int)lround(dxr_json_number(dxr_json_field(m, m_end, "w"), m_end, 0.0));
					in->mon.h = (int)lround(dxr_json_number(dxr_json_field(m, m_end, "h"), m_end, 0.0));
				}
			}
		} else if (!minimized && frame.w > 0 && frame.h > 0 && dxr_ss_type_counts(type)) {
			char app_id[64];
			dxr_json_string_utf8(dxr_json_field(q, o, "app_id"), o, app_id, sizeof(app_id));
			if (!dxr_ss_is_desktop_app(app_id)) {
				uint64_t id = dxr_ss_json_u64(dxr_json_field(q, o, "id"), o);
				if (dxr_json_bool(dxr_json_field(q, o, "focus"), o, 0)) {
					in->focus_valid = 1;
					in->focus = frame;
					in->focus_id = id;
				}
				if (tracked_id && id == tracked_id) {
					in->tracked_valid = 1;
					in->tracked = frame;
				}
			}
			memset(app_id, 0, sizeof(app_id));
		}
		q = dxr_json_ws(o, end);
		if (q < end && *q == ',')
			q++;
		else if (q < end && *q == ']')
			break;
		else
			return 0;
	}
	return 1;
}

/*
 *
 * The decision.
 *
 */

//! Our steps aside in the last DXR_SS_CAP_WINDOW_S.
static inline int
dxr_ss_recent_moves(const DxrSsState *st, double now)
{
	int n = 0;
	int k = st->n_moves < DXR_SS_CAP_MOVES ? st->n_moves : DXR_SS_CAP_MOVES;
	for (int i = 0; i < k; i++)
		if (now - st->moves[i] < DXR_SS_CAP_WINDOW_S)
			n++;
	return n;
}

static inline void
dxr_ss_note_move(DxrSsState *st, double now)
{
	st->moves[st->n_moves % DXR_SS_CAP_MOVES] = now;
	st->n_moves++;
}

static inline void
dxr_ss_start_move(DxrSsState *st, DxrSsDecision *d, double now, int x, int y, int home)
{
	d->move = 1;
	d->x = x;
	d->y = y;
	d->home = home;
	st->phase = DXR_SS_PHASE_MOVING;
	st->target_x = x;
	st->target_y = y;
	st->target_home = home;
	st->move_since = now;
	st->in_way_since = -1.0;
	st->home_clear_since = -1.0;
	if (!home)
		dxr_ss_note_move(st, now);
}

//! The state as reported (DXR_SS_STATE_*), from the decision's memory.
static inline int
dxr_ss_report_state(const DxrSsState *st)
{
	switch (st->reason) {
	case DXR_SS_REASON_NO_WINDOW:
	case DXR_SS_REASON_SETTLING: return DXR_SS_STATE_WAITING;
	case DXR_SS_REASON_LOCKED:
	case DXR_SS_REASON_USER_MOVING:
	case DXR_SS_REASON_MINIMIZED:
	case DXR_SS_REASON_MAXIMIZED:
	case DXR_SS_REASON_FULLSCREEN:
	case DXR_SS_REASON_TOO_MANY_MOVES:
	case DXR_SS_REASON_NOT_GRANTED: return DXR_SS_STATE_PAUSED;
	default: break;
	}
	if (st->phase == DXR_SS_PHASE_MOVING)
		return DXR_SS_STATE_MOVING;
	if (st->phase == DXR_SS_PHASE_ASIDE)
		return DXR_SS_STATE_ASIDE;
	if (st->phase == DXR_SS_PHASE_HOME)
		return DXR_SS_STATE_HOME;
	return DXR_SS_STATE_WAITING;
}

//! One tick. The rules:
//!  - Nothing while the screen is locked, the window is minimized, maximized or
//!    fullscreen, or the user is moving it (the place they leave it becomes home).
//!  - A window that moved by anyone but us (the user, the app, the compositor) is
//!    home once it has stayed put DXR_SS_SETTLE_S. Our own moves never set home.
//!  - Covered: the focused window of another app covers at least `trigger` of the
//!    app's content for `dwell_s`. Then step to the best free place
//!    (dxr_ss_find_spot) whose share covered is below both `trigger` and half of
//!    the share covered now; none: stay (NO_FREE_SPOT).
//!  - Aside, not covered, `return_home`: home is free (below half of `trigger`)
//!    of the focused window and of the window it stepped aside for, for
//!    `return_delay_s`: go back. Without `return_home` the new place becomes home.
//!  - Our moves: one at a time; a move that does not land within DXR_SS_LAND_S
//!    ends where the window is; DXR_SS_GAP_S between moves; at most
//!    DXR_SS_CAP_MOVES steps aside per DXR_SS_CAP_WINDOW_S (then it stays:
//!    TOO_MANY_MOVES); the way back home is never held by the cap.
static inline void
dxr_ss_step(DxrSsState *st, const DxrSsSettings *s, const DxrSsInput *in, DxrSsDecision *d)
{
	memset(d, 0, sizeof(*d));
	double now = in->now;
	st->covered = 0.0f;
	if (st->start_at < 0.0)
		st->start_at = now + DXR_SS_START_S;
	if (!in->own_valid) {
		st->reason = DXR_SS_REASON_NO_WINDOW;
		st->in_way_since = st->home_clear_since = -1.0;
		return;
	}
	int pause = in->locked          ? DXR_SS_REASON_LOCKED
	            : in->own_moving    ? DXR_SS_REASON_USER_MOVING
	            : in->own_minimized ? DXR_SS_REASON_MINIMIZED
	            : in->own_maximized ? DXR_SS_REASON_MAXIMIZED
	            : in->own_fullscreen ? DXR_SS_REASON_FULLSCREEN
	                                 : 0;
	if (pause) {
		st->reason = pause;
		st->in_way_since = st->home_clear_since = -1.0;
		st->streak_reported = 0;
		if (pause == DXR_SS_REASON_USER_MOVING) {
			// The user has the window: wherever they leave it becomes home.
			if (st->phase == DXR_SS_PHASE_MOVING)
				d->event = DXR_SS_EV_ABORTED;
			st->phase = DXR_SS_PHASE_SETTLING;
			st->last_valid = 0;
		}
		return;
	}

	DxrSsRect own = in->own;
	if (st->phase == DXR_SS_PHASE_MOVING) {
		int landed = abs(own.x - st->target_x) <= DXR_SS_LANDED_PX && abs(own.y - st->target_y) <= DXR_SS_LANDED_PX;
		if (!landed && now - st->move_since < DXR_SS_LAND_S) {
			st->reason = DXR_SS_REASON_NONE;
			return;
		}
		// Landed, or the compositor kept it elsewhere: that is where it is.
		st->last_valid = 1;
		st->last_x = own.x;
		st->last_y = own.y;
		st->stable_since = now;
		st->next_move_at = now + DXR_SS_GAP_S;
		st->streak_reported = 0;
		st->reason = DXR_SS_REASON_NONE;
		if (st->target_home) {
			st->phase = DXR_SS_PHASE_HOME;
			st->home_x = own.x; // (where it is, if the compositor kept it elsewhere)
			st->home_y = own.y;
			st->aside_for = 0;
			d->event = DXR_SS_EV_LANDED_HOME;
		} else if (s->return_home && st->have_home) {
			st->phase = DXR_SS_PHASE_ASIDE;
			d->event = DXR_SS_EV_LANDED_ASIDE;
		} else {
			// Not going back: the new place is home.
			st->phase = DXR_SS_PHASE_HOME;
			st->have_home = 1;
			st->home_x = own.x;
			st->home_y = own.y;
			st->aside_for = 0;
			d->event = DXR_SS_EV_LANDED_ASIDE;
		}
		return;
	}

	// Moved, by anyone but us: wait for it to stay put, then that is home.
	if (!st->last_valid || own.x != st->last_x || own.y != st->last_y) {
		st->last_valid = 1;
		st->last_x = own.x;
		st->last_y = own.y;
		st->stable_since = now;
		st->phase = DXR_SS_PHASE_SETTLING;
		st->in_way_since = st->home_clear_since = -1.0;
		st->reason = DXR_SS_REASON_SETTLING;
		return;
	}
	if (st->phase == DXR_SS_PHASE_SETTLING) {
		if (now < st->start_at || now - st->stable_since < DXR_SS_SETTLE_S) {
			st->reason = DXR_SS_REASON_SETTLING;
			return;
		}
		st->phase = DXR_SS_PHASE_HOME;
		st->have_home = 1;
		st->home_x = own.x;
		st->home_y = own.y;
		st->aside_for = 0;
		st->streak_reported = 0;
		d->event = DXR_SS_EV_NEW_HOME;
		// and decide on this tick too
	}

	st->reason = DXR_SS_REASON_NONE;
	float covered = in->focus_valid ? dxr_ss_coverage(in->body, own, in->focus) : 0.0f;
	st->covered = covered;
	if (covered >= s->trigger) {
		st->home_clear_since = -1.0;
		if (st->in_way_since < 0.0)
			st->in_way_since = now;
		if (now - st->in_way_since < s->dwell_s || now < st->next_move_at)
			return;
		if (dxr_ss_recent_moves(st, now) >= DXR_SS_CAP_MOVES) {
			st->reason = DXR_SS_REASON_TOO_MANY_MOVES;
			if (!st->streak_reported && !d->event)
				d->event = DXR_SS_EV_CAPPED;
			st->streak_reported = 1;
			return;
		}
		int x = 0, y = 0;
		float spot = 1.0f;
		float need = s->trigger < covered * 0.5f ? s->trigger : covered * 0.5f;
		if (dxr_ss_find_spot(in->body, own, in->focus, in->mon, s->margin, s->side, &x, &y, &spot) &&
		    spot < need) {
			d->covered = covered;
			d->spot_covered = spot;
			st->aside_for = in->focus_id;
			st->streak_reported = 0;
			dxr_ss_start_move(st, d, now, x, y, 0);
			st->reason = DXR_SS_REASON_NONE;
			return;
		}
		st->reason = DXR_SS_REASON_NO_FREE_SPOT;
		if (!st->streak_reported && !d->event)
			d->event = DXR_SS_EV_NO_SPOT;
		st->streak_reported = 1;
		return;
	}
	st->in_way_since = -1.0;
	st->streak_reported = 0;

	if (st->phase == DXR_SS_PHASE_ASIDE && s->return_home && st->have_home) {
		DxrSsRect home = {st->home_x, st->home_y, own.w, own.h};
		float hc = in->focus_valid ? dxr_ss_coverage(in->body, home, in->focus) : 0.0f;
		if (in->tracked_valid) {
			float tc = dxr_ss_coverage(in->body, home, in->tracked);
			if (tc > hc)
				hc = tc;
		}
		if (hc >= s->trigger * 0.5f) {
			st->home_clear_since = -1.0;
			return;
		}
		if (st->home_clear_since < 0.0)
			st->home_clear_since = now;
		if (now - st->home_clear_since < s->return_delay_s || now < st->next_move_at)
			return;
		dxr_ss_start_move(st, d, now, st->home_x, st->home_y, 1);
	}
}

/*
 *
 * One poll, behind the desktop-control gate, and the glide.
 *
 */

//! What the gate answers (DxrSsOps.granted).
#define DXR_SS_DC_NO 0        //!< not granted: never asked, pending, denied, turned off by the user, no extension
#define DXR_SS_DC_GRANTED 1   //!< granted
#define DXR_SS_DC_SUSPENDED 2 //!< granted, but suspended: the screen is locked (or the user's kill switch is on)

//! What a poll reads and does, as calls: displayxr_linux_side_step.c wires the real
//! ones, the unit test fakes them (and counts them). window_list and screen_locked
//! are the only reads of other windows and of the desktop; dxr_ss_poll makes them
//! only after `granted` said DXR_SS_DC_GRANTED on that same poll.
typedef struct DxrSsOps {
	void *ctx;
	//! The app's desktop control right now (DXR_SS_DC_*): the #389 gate, the same
	//! synchronous runtime query DisplayXRNative.LinuxWindows answers behind
	//! (dxr_prov_desktop_get_state: XR_DESKTOP_CONTROL_STATE_GRANTED_DXR /
	//! _SUSPENDED_DXR / anything else).
	int (*granted)(void *ctx);
	char *(*window_list)(void *ctx); //!< the extension's window list (malloc'd JSON; NULL = none)
	int (*screen_locked)(void *ctx); //!< 1 locked, 0 not, -1 cannot tell
	int (*own_drag)(void *ctx);      //!< the plug-in's own right-drag of the window is running
	int (*still_on)(void *ctx);      //!< side-step is still enabled
	//! Move the app's OWN window's frame to (x, y), stage px; `last`: the glide's last
	//! step. 1 when asked, 0 when it cannot be moved.
	int (*move)(void *ctx, int x, int y, int last);
	void (*wait_s)(void *ctx, double seconds); //!< between glide steps
} DxrSsOps;

//! The gate's memory between polls.
typedef struct DxrSsGate {
	int open;      //!< granted on the last poll (or glide step) that asked, or suspended since
	int suspended; //!< ... and suspended now (the screen is locked): holding still, home kept
} DxrSsGate;

#define DXR_SS_GATE_SAME 0      //!< as on the last poll
#define DXR_SS_GATE_OPENED 1    //!< granted now, not before: started afresh (the window's place becomes home)
#define DXR_SS_GATE_CLOSED 2    //!< not granted now, granted (or suspended) before: stopped, everything forgotten
#define DXR_SS_GATE_SUSPENDED 3 //!< suspended now, granted before: holding still, home kept
#define DXR_SS_GATE_RESUMED 4   //!< granted again after a suspension: carrying on from the kept home

//! One poll's outcome.
typedef struct DxrSsPoll {
	int gate;     //!< DXR_SS_GATE_*
	int granted;  //!< the grant held: the window list was asked for
	int read;     //!< ... and understood (0: no window list, DXR_SS_REASON_NO_EXTENSION)
	DxrSsInput in; //!< valid when read
	DxrSsDecision d; //!< valid when read
} DxrSsPoll;

//! Start afresh, but keep the times of our own recent steps aside: the move cap
//! (DXR_SS_CAP_MOVES a minute) still counts across a revoke, a lock, a re-grant.
static inline void
dxr_ss_forget(DxrSsState *st)
{
	double moves[DXR_SS_CAP_MOVES];
	memcpy(moves, st->moves, sizeof(moves));
	int n = st->n_moves;
	dxr_ss_reset(st);
	memcpy(st->moves, moves, sizeof(moves));
	st->n_moves = n;
}

//! The grant is gone: forget everything the decision remembered (home, the place it
//! was going to, the window it stepped aside for). The window stays where it is;
//! when the grant comes back, the decision starts afresh, as after turning
//! side-step off and on (the place the window stays put at becomes home).
static inline void
dxr_ss_gate_close(DxrSsGate *g, DxrSsState *st)
{
	g->open = 0;
	g->suspended = 0;
	dxr_ss_forget(st);
	st->reason = DXR_SS_REASON_NOT_GRANTED;
}

//! Suspended (the screen is locked): hold still. The app's own home and whether it
//! is aside are kept (its own window's data); the id of the window it stepped aside
//! for is dropped (nothing about other windows is kept while nothing may be read),
//! and so are the timers that need reads. After the unlock the normal rules carry
//! on from the kept home: aside, it goes back once home is free of the focused window.
static inline void
dxr_ss_suspend(DxrSsGate *g, DxrSsState *st)
{
	g->suspended = 1;
	st->aside_for = 0;
	st->in_way_since = st->home_clear_since = -1.0;
	st->streak_reported = 0;
	st->covered = 0.0f;
	st->reason = DXR_SS_REASON_LOCKED;
}

//! One poll (the worker makes one every DXR_SS_POLL_MS). First the gate; nothing
//! is read (no window list, no lock state) unless it says granted. Suspended after a
//! grant -> holding still, home kept (dxr_ss_suspend; reason LOCKED). Any other no ->
//! the decision's memory is forgotten if the grant held before (reason NOT_GRANTED).
//! Granted -> the window list, the lock state, the decision (`now` seconds; `body`
//! NULL = the whole window).
static inline void
dxr_ss_poll(DxrSsState *st, DxrSsGate *g, const DxrSsSettings *s, const DxrSsOps *ops, double now, int own_pid,
            const DxrSsBody *body, DxrSsPoll *p)
{
	memset(p, 0, sizeof(*p));
	int dc = ops->granted(ops->ctx);
	if (dc == DXR_SS_DC_SUSPENDED && g->open) {
		p->gate = g->suspended ? DXR_SS_GATE_SAME : DXR_SS_GATE_SUSPENDED;
		dxr_ss_suspend(g, st);
		return;
	}
	if (dc != DXR_SS_DC_GRANTED) {
		p->gate = g->open ? DXR_SS_GATE_CLOSED : DXR_SS_GATE_SAME;
		dxr_ss_gate_close(g, st);
		return;
	}
	if (!g->open) {
		g->open = 1;
		dxr_ss_forget(st); // afresh: no home from before the grant
		p->gate = DXR_SS_GATE_OPENED;
	} else if (g->suspended) {
		p->gate = DXR_SS_GATE_RESUMED; // carry on from the kept home
	}
	g->suspended = 0;
	p->granted = 1;
	char *json = ops->window_list(ops->ctx);
	int ok = json && dxr_ss_parse_windows(json, own_pid, st->aside_for, &p->in);
	free(json);
	if (!ok) {
		st->covered = 0.0f;
		st->reason = DXR_SS_REASON_NO_EXTENSION;
		return;
	}
	p->read = 1;
	p->in.now = now;
	p->in.locked = ops->screen_locked(ops->ctx) == 1;
	p->in.own_moving = p->in.own_moving || ops->own_drag(ops->ctx);
	p->in.body = body;
	dxr_ss_step(st, s, &p->in, &p->d);
}

#define DXR_SS_GLIDE_STEP_S 0.04 //!< one glide step per this, roughly (at most 30 steps)

#define DXR_SS_GLIDE_DONE 0        //!< all steps asked; the last one is the target
#define DXR_SS_GLIDE_STOPPED 1     //!< side-step turned off, or the user took the window: it stays where it is
#define DXR_SS_GLIDE_NOT_GRANTED 2 //!< the grant ended: it stays where it is, the gate is closed (forgotten)
#define DXR_SS_GLIDE_CANNOT_MOVE 3 //!< the Wayland library cannot move the window
#define DXR_SS_GLIDE_SUSPENDED 4   //!< the screen locked: it stays where it is, home kept (dxr_ss_suspend)

//! Glide the app's own window from (fx, fy) to (tx, ty), stage px, in s->glide_s
//! (ease-out; the last step is the target). Before each step: still on, the user
//! not dragging it, and still GRANTED (the same gate as the poll's); the first
//! that fails stops it where it is. Stopped for the grant: the gate is closed
//! (dxr_ss_gate_close). Stopped by a suspension (a lock): the move is over where
//! the last step put the window, home is kept, and the window counts as away from
//! home (with ReturnHome off: where it stays becomes home, as a landed step aside
//! would). Returns DXR_SS_GLIDE_*.
static inline int
dxr_ss_glide(DxrSsGate *g, DxrSsState *st, const DxrSsSettings *s, const DxrSsOps *ops, int fx, int fy, int tx,
             int ty)
{
	double seconds = s->glide_s;
	int steps = seconds > 0.0 ? (int)lround(seconds / DXR_SS_GLIDE_STEP_S) : 1;
	if (steps < 1)
		steps = 1;
	if (steps > 30)
		steps = 30;
	double step_s = steps > 1 ? seconds / steps : 0.0;
	int at_x = fx, at_y = fy; // where the last step asked the window to be
	for (int i = 1; i <= steps; i++) {
		if (!ops->still_on(ops->ctx) || ops->own_drag(ops->ctx))
			return DXR_SS_GLIDE_STOPPED;
		int dc = ops->granted(ops->ctx);
		if (dc == DXR_SS_DC_SUSPENDED && g->open) {
			if (st->phase == DXR_SS_PHASE_MOVING) {
				st->last_valid = 1; // where it is now is ours, not a move by someone else
				st->last_x = at_x;
				st->last_y = at_y;
				st->phase = st->have_home && s->return_home ? DXR_SS_PHASE_ASIDE : DXR_SS_PHASE_SETTLING;
			}
			dxr_ss_suspend(g, st);
			return DXR_SS_GLIDE_SUSPENDED;
		}
		if (dc != DXR_SS_DC_GRANTED) {
			dxr_ss_gate_close(g, st);
			return DXR_SS_GLIDE_NOT_GRANTED;
		}
		double t = (double)i / steps, e = 1.0 - (1.0 - t) * (1.0 - t);
		int x = i == steps ? tx : (int)lround(fx + (tx - fx) * e);
		int y = i == steps ? ty : (int)lround(fy + (ty - fy) * e);
		if (!ops->move(ops->ctx, x, y, i == steps))
			return DXR_SS_GLIDE_CANNOT_MOVE;
		at_x = x;
		at_y = y;
		if (i < steps)
			ops->wait_s(ops->ctx, step_s);
	}
	return DXR_SS_GLIDE_DONE;
}

#ifdef __cplusplus
}
#endif
