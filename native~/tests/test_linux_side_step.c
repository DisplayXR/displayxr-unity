// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Unit test for the side-step's pure part (displayxr_linux_side_step.h): the
// records' layout, settings, the content grid and its coverage, the place to step
// to, the extension's window list, and the decision tick by tick (dwell, stepping
// aside, following, going home, the user's moves, pauses, the move cap). Built and
// run by CTest on Linux (no D-Bus, no desktop).

#include "displayxr_linux_side_step.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

static int s_failed;

#define CHECK(cond)                                                                                                \
	do {                                                                                                       \
		if (!(cond)) {                                                                                     \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                            \
			s_failed++;                                                                                \
		}                                                                                                  \
	} while (0)

#define NEAR(a, b, eps) (fabs((double)(a) - (double)(b)) <= (eps))

static DxrSsRect
R(int x, int y, int w, int h)
{
	DxrSsRect r = {x, y, w, h};
	return r;
}

//! A body that is the rect (fractions of the window) fx0..fx1 x fy0..fy1.
static DxrSsBody
body_frac(double fx0, double fy0, double fx1, double fy1)
{
	float acc[DXR_SS_GRID * DXR_SS_GRID];
	memset(acc, 0, sizeof(acc));
	dxr_ss_body_add_rect(acc, fx0 * 1000, fy0 * 1000, fx1 * 1000, fy1 * 1000, 1000, 1000);
	DxrSsBody b;
	dxr_ss_body_from_acc(&b, acc);
	return b;
}

static void
test_layout(void)
{
	// The C# mirrors (DisplayXRSideStep.cs) depend on these.
	CHECK(sizeof(DisplayXRSideStepSettings) == 28);
	CHECK(offsetof(DisplayXRSideStepSettings, return_home) == 16);
	CHECK(offsetof(DisplayXRSideStepSettings, margin) == 24);
	CHECK(sizeof(DisplayXRSideStepStatus) == 68);
	CHECK(offsetof(DisplayXRSideStepStatus, covered) == 8);
	CHECK(offsetof(DisplayXRSideStepStatus, win_x) == 20);
	CHECK(offsetof(DisplayXRSideStepStatus, home_valid) == 36);
	CHECK(offsetof(DisplayXRSideStepStatus, home_xroot_valid) == 48);
	CHECK(offsetof(DisplayXRSideStepStatus, aside_count) == 60);
	CHECK(offsetof(DisplayXRSideStepStatus, home_count) == 64);
}

static void
test_settings(void)
{
	DxrSsSettings s;
	dxr_ss_default_settings(&s);
	CHECK(NEAR(s.trigger, 0.15, 1e-6) && NEAR(s.dwell_s, 0.6, 1e-6) && NEAR(s.return_delay_s, 2.0, 1e-6));
	CHECK(s.return_home == 1 && s.side == DXR_SS_SIDE_AUTO && s.margin == 24);
	s.trigger = NAN;
	s.dwell_s = -3.0f;
	s.return_delay_s = 1e9f;
	s.glide_s = 50.0f;
	s.return_home = 7;
	s.side = 9;
	s.margin = -5;
	dxr_ss_clamp_settings(&s);
	CHECK(NEAR(s.trigger, 0.01, 1e-6));
	CHECK(s.dwell_s == 0.0f && s.return_delay_s == 600.0f && s.glide_s == 2.0f);
	CHECK(s.return_home == 1 && s.side == DXR_SS_SIDE_AUTO && s.margin == 0);
	s.trigger = 3.0f;
	dxr_ss_clamp_settings(&s);
	CHECK(s.trigger == 1.0f);
}

static void
test_coverage(void)
{
	DxrSsRect win = R(1000, 200, 600, 480);
	// The whole window (no body known): plain rect overlap.
	CHECK(NEAR(dxr_ss_coverage(NULL, win, R(1000, 200, 300, 480)), 0.5, 1e-6));
	CHECK(NEAR(dxr_ss_coverage(NULL, win, R(0, 0, 4000, 4000)), 1.0, 1e-6));
	CHECK(dxr_ss_coverage(NULL, win, R(0, 0, 100, 100)) == 0.0f);
	CHECK(dxr_ss_coverage(NULL, win, R(1000, 200, 0, 480)) == 0.0f);

	// The content is the lower half of the window (an avatar under an empty band).
	DxrSsBody lower = body_frac(0.0, 0.5, 1.0, 1.0);
	CHECK(dxr_ss_coverage(&lower, win, R(1000, 200, 600, 240)) < 0.001f); // the empty band only
	CHECK(NEAR(dxr_ss_coverage(&lower, win, R(1000, 440, 600, 240)), 1.0, 0.001));
	CHECK(NEAR(dxr_ss_coverage(&lower, win, R(1000, 440, 300, 240)), 0.5, 0.001));
	// Scale-free: the same body on a window twice the size.
	CHECK(NEAR(dxr_ss_coverage(&lower, R(0, 0, 1200, 960), R(0, 480, 600, 480)), 0.5, 0.001));

	// A body that does not fill whole cells (fractional edges).
	DxrSsBody odd = body_frac(0.1, 0.1, 0.37, 0.73);
	double bx0 = 1000 + 0.1 * 600, bx1 = 1000 + 0.37 * 600;
	double by0 = 200 + 0.1 * 480, by1 = 200 + 0.73 * 480;
	DxrSsRect left_half = R((int)bx0, (int)by0, (int)((bx1 - bx0) / 2), (int)(by1 - by0) + 1);
	CHECK(NEAR(dxr_ss_coverage(&odd, win, left_half), 0.5, 0.05));

	// Silhouette rects as the hit mask sends them (device px of a 1200 x 960 window):
	// overlapping rects saturate at a full cell.
	float acc[DXR_SS_GRID * DXR_SS_GRID];
	memset(acc, 0, sizeof(acc));
	dxr_ss_body_add_rect(acc, 600, 480, 1200, 960, 1200, 960);
	dxr_ss_body_add_rect(acc, 600, 480, 1200, 960, 1200, 960);
	dxr_ss_body_add_rect(acc, -50, -50, 10, 10, 1200, 960); // clipped to the window
	DxrSsBody sil;
	dxr_ss_body_from_acc(&sil, acc);
	CHECK(sil.cell[DXR_SS_GRID * DXR_SS_GRID - 1] == 255);
	CHECK(sil.cell[0] > 0 && sil.cell[0] < 255);
	CHECK(dxr_ss_coverage(&sil, win, R(1300, 440, 300, 240)) > 0.95f);

	// An empty body: nothing to cover.
	DxrSsBody empty;
	memset(&empty, 0, sizeof(empty));
	empty.valid = 1;
	CHECK(dxr_ss_coverage(&empty, win, R(0, 0, 4000, 4000)) == 0.0f);
	DxrSsRect bb;
	CHECK(!dxr_ss_body_bbox(&empty, 600, 480, &bb));

	// The bounding box, in stage px of the window.
	CHECK(dxr_ss_body_bbox(&lower, 600, 480, &bb));
	CHECK(bb.x == 0 && bb.y == 240 && bb.w == 600 && bb.h == 240);
	CHECK(dxr_ss_body_bbox(NULL, 600, 480, &bb) && bb.x == 0 && bb.y == 0 && bb.w == 600 && bb.h == 480);
}

static void
test_find_spot(void)
{
	DxrSsRect mon = R(1920, 0, 1920, 1080);
	DxrSsRect win = R(2580, 298, 600, 484);
	int x = 0, y = 0;
	float cov = 1.0f;
	// A document over the window, with room on its right: there, the shortest free move.
	DxrSsRect doc = R(2480, 260, 600, 560);
	CHECK(dxr_ss_find_spot(NULL, win, doc, mon, 24, DXR_SS_SIDE_AUTO, &x, &y, &cov));
	CHECK(cov == 0.0f && x == 2480 + 600 + 24 && y == 298);
	CHECK(x >= mon.x && x + 600 <= mon.x + mon.w && y >= mon.y && y + 484 <= mon.y + mon.h);

	// A preferred side wins among equally free places (here: the corners on that side).
	CHECK(dxr_ss_find_spot(NULL, win, doc, mon, 24, DXR_SS_SIDE_BELOW, &x, &y, &cov));
	CHECK(cov == 0.0f && y + 484 == 1080 - 24 && x == 3840 - 24 - 600);
	CHECK(dxr_ss_find_spot(NULL, win, doc, mon, 24, DXR_SS_SIDE_ABOVE, &x, &y, &cov));
	CHECK(cov == 0.0f && y == 24);
	// ... but never a more covered one: left of it is off the monitor (clamped back
	// onto the document), so LEFT still gets a free place on the right.
	CHECK(dxr_ss_find_spot(NULL, win, doc, mon, 24, DXR_SS_SIDE_LEFT, &x, &y, &cov));
	CHECK(cov == 0.0f && x >= 2480 + 600);

	// Cramped (a big document, the prototype's DS1 geometry): no place is quite
	// free; the least covered one is offered, and the caller decides (below half
	// the trigger here: a corner, 5.5 %).
	DxrSsRect big = R(2480, 260, 800, 560);
	CHECK(dxr_ss_find_spot(NULL, win, big, mon, 24, DXR_SS_SIDE_AUTO, &x, &y, &cov));
	CHECK(cov > 0.05f && cov < 0.06f);

	// With room on the left, the shorter move wins.
	DxrSsRect doc2 = R(2700, 260, 500, 560);
	CHECK(dxr_ss_find_spot(NULL, win, doc2, mon, 24, DXR_SS_SIDE_AUTO, &x, &y, &cov));
	CHECK(cov == 0.0f && x == 2700 - 24 - 600 && y == 298);

	// The content's box, not the window, is what keeps the margin.
	DxrSsBody lower = body_frac(0.25, 0.5, 0.75, 1.0); // 150..450 x 242..484 of the window
	CHECK(dxr_ss_find_spot(&lower, win, doc2, mon, 24, DXR_SS_SIDE_LEFT, &x, &y, &cov));
	CHECK(cov == 0.0f && x + 150 + 300 == 2700 - 24);

	// The covering window fills the monitor (maximized): nowhere is free.
	CHECK(dxr_ss_find_spot(NULL, win, mon, mon, 24, DXR_SS_SIDE_AUTO, &x, &y, &cov));
	CHECK(cov >= 0.99f);
	// No monitor reported: no other place.
	CHECK(!dxr_ss_find_spot(NULL, win, doc, R(0, 0, 0, 0), 24, DXR_SS_SIDE_AUTO, &x, &y, &cov));
	// No content at all: nothing to move.
	DxrSsBody empty;
	memset(&empty, 0, sizeof(empty));
	empty.valid = 1;
	CHECK(!dxr_ss_find_spot(&empty, win, doc, mon, 24, DXR_SS_SIDE_AUTO, &x, &y, &cov));
}

static void
test_parse(void)
{
	DxrSsInput in;
	// Own window (pid 100), a focused editor, the desktop-icons window, a minimized
	// window, and a window with a title full of JSON syntax (an older extension sends
	// titles to everyone; they are skipped, never read).
	const char *json =
	    "{\"version\": 1, \"layout_mode\": \"logical\", \"windows\": ["
	    "{\"id\": 11, \"pid\": 7, \"app_id\": \"com.rastersoft.ding\", \"focus\": false, \"frame\": [1920, 0, 1920, 1080]},"
	    "{\"id\": 4000000000, \"pid\": 200, \"app_id\": \"org.gnome.TextEditor\", \"title\": \"a \\\"}],{\\\" b\","
	    " \"focus\": true, \"frame\": [2480, 260, 800, 560], \"minimized\": false},"
	    "{\"id\": 13, \"pid\": 300, \"app_id\": \"x\", \"focus\": false, \"minimized\": true, \"frame\": [0, 0, 10, 10]},"
	    "{\"id\": 14, \"pid\": 100, \"app_id\": \"Unity\", \"focus\": false, \"moving\": true, \"maximized\": false,"
	    " \"fullscreen\": false, \"frame\": [2580, 298, 600, 484],"
	    " \"monitor\": {\"x\": 1920, \"y\": 0, \"w\": 1920, \"h\": 1080, \"scale\": 2, \"device_scale\": 2}}"
	    "]}";
	CHECK(dxr_ss_parse_windows(json, 100, 4000000000ull, &in));
	CHECK(in.own_valid && in.own.x == 2580 && in.own.y == 298 && in.own.w == 600 && in.own.h == 484);
	CHECK(in.own_moving == 1 && in.own_minimized == 0);
	CHECK(in.mon.x == 1920 && in.mon.w == 1920 && in.mon.h == 1080);
	CHECK(in.focus_valid && in.focus.x == 2480 && in.focus.w == 800 && in.focus_id == 4000000000ull);
	CHECK(in.tracked_valid && in.tracked.y == 260);

	// The desktop window focused (a click on the desktop) does not count.
	const char *ding = "{\"version\": 1, \"windows\": ["
	                   "{\"id\": 11, \"pid\": 7, \"app_id\": \"com.rastersoft.ding\", \"focus\": true, \"frame\": [0, 0, 1920, 1080]},"
	                   "{\"id\": 14, \"pid\": 100, \"focus\": false, \"frame\": [10, 10, 100, 100]}]}";
	CHECK(dxr_ss_parse_windows(ding, 100, 0, &in));
	CHECK(in.own_valid && !in.focus_valid && !in.tracked_valid);

	// The app's own windows never count, even focused; a minimized focused window
	// neither; menus and docks neither (types 6, 2), dialogs do (3).
	const char *own_focus = "{\"version\": 1, \"windows\": ["
	                        "{\"id\": 1, \"pid\": 100, \"focus\": true, \"frame\": [0, 0, 500, 500]},"
	                        "{\"id\": 2, \"pid\": 100, \"window_type\": 3, \"focus\": false, \"frame\": [0, 0, 900, 900]}]}";
	CHECK(dxr_ss_parse_windows(own_focus, 100, 0, &in));
	CHECK(in.own_valid && in.own.w == 500 && !in.focus_valid); // the normal one, not the bigger dialog
	const char *menu = "{\"version\": 1, \"windows\": [{\"id\": 5, \"pid\": 9, \"window_type\": 6, \"focus\": true, "
	                   "\"frame\": [0, 0, 50, 50]}]}";
	CHECK(dxr_ss_parse_windows(menu, 100, 0, &in) && !in.focus_valid && !in.own_valid);
	const char *dialog = "{\"version\": 1, \"windows\": [{\"id\": 5, \"pid\": 9, \"window_type\": 3, \"focus\": true, "
	                     "\"frame\": [0, 0, 50, 50]}]}";
	CHECK(dxr_ss_parse_windows(dialog, 100, 0, &in) && in.focus_valid);
	const char *mini = "{\"version\": 1, \"windows\": [{\"id\": 5, \"pid\": 9, \"focus\": true, \"minimized\": true, "
	                   "\"frame\": [0, 0, 50, 50]}]}";
	CHECK(dxr_ss_parse_windows(mini, 100, 5, &in) && !in.focus_valid && !in.tracked_valid);

	// Another schema version, malformed text, nothing: not read.
	CHECK(!dxr_ss_parse_windows("{\"version\": 2, \"windows\": []}", 100, 0, &in));
	CHECK(!dxr_ss_parse_windows("{\"version\": 1, \"windows\": [{\"pid\": 1,", 100, 0, &in));
	CHECK(!dxr_ss_parse_windows(NULL, 100, 0, &in));
	CHECK(!in.own_valid && !in.focus_valid);
	CHECK(dxr_ss_parse_windows("{\"version\": 1, \"windows\": []}", 100, 0, &in) && !in.own_valid);
}

/*
 *
 * The decision, as a small world: the app's window, the focused document, and our
 * moves, which land on the next tick unless the compositor refuses them.
 *
 */

typedef struct {
	DxrSsState st;
	DxrSsSettings s;
	DxrSsInput in;
	DxrSsBody body;
	int use_body;
	double t;
	int refuse_moves; // the compositor keeps the window where it is
	int moves, events[16];
	DxrSsDecision last, last_move;
	int pending, px, py;
} World;

static void
world_init(World *w)
{
	memset(w, 0, sizeof(*w));
	dxr_ss_reset(&w->st);
	dxr_ss_default_settings(&w->s);
	w->in.own_valid = 1;
	w->in.own = R(2580, 298, 600, 484);
	w->in.mon = R(1920, 0, 1920, 1080);
}

//! Advance by dt seconds, ticking every 0.25 s like the worker.
static void
run(World *w, double dt)
{
	double end = w->t + dt;
	while (w->t + 1e-9 < end) {
		w->t += 0.25;
		if (w->pending) { // our last move lands now
			w->pending = 0;
			if (!w->refuse_moves) {
				w->in.own.x = w->px;
				w->in.own.y = w->py;
			}
		}
		w->in.now = w->t;
		w->in.body = w->use_body ? &w->body : NULL;
		dxr_ss_step(&w->st, &w->s, &w->in, &w->last);
		if (w->last.event)
			w->events[w->last.event]++;
		if (w->last.move) {
			w->last_move = w->last;
			w->moves++;
			w->pending = 1;
			w->px = w->last.x;
			w->py = w->last.y;
		}
	}
}

static void
focus(World *w, DxrSsRect r, uint64_t id)
{
	w->in.focus_valid = 1;
	w->in.focus = r;
	w->in.focus_id = id;
	// The tracked window is the one we stepped aside for, if it is this one.
	w->in.tracked_valid = w->st.aside_for && w->st.aside_for == id;
	w->in.tracked = r;
}

static void
no_focus(World *w)
{
	w->in.focus_valid = 0;
}

static void
test_start_and_home(void)
{
	World w;
	world_init(&w);
	run(&w, 1.0);
	CHECK(w.st.reason == DXR_SS_REASON_SETTLING && !w.st.have_home);
	CHECK(dxr_ss_report_state(&w.st) == DXR_SS_STATE_WAITING);
	run(&w, 1.5); // past DXR_SS_START_S (2 s) and settled
	CHECK(w.st.have_home && w.st.home_x == 2580 && w.st.home_y == 298);
	CHECK(w.events[DXR_SS_EV_NEW_HOME] == 1);
	CHECK(dxr_ss_report_state(&w.st) == DXR_SS_STATE_HOME);
	// Not listed (hidden): waiting, nothing else changes.
	w.in.own_valid = 0;
	run(&w, 1.0);
	CHECK(w.st.reason == DXR_SS_REASON_NO_WINDOW && w.st.have_home);
	w.in.own_valid = 1;
	run(&w, 0.25);
	CHECK(w.st.phase == DXR_SS_PHASE_HOME && w.st.reason == DXR_SS_REASON_NONE);
}

//! The DS1 prototype run's scenario: a document over the window, then following it,
//! then closing.
static void
test_scenario(void)
{
	World w;
	world_init(&w);
	run(&w, 3.0);
	CHECK(w.st.phase == DXR_SS_PHASE_HOME);

	// Below the trigger: nothing.
	focus(&w, R(2580 + 600 - 60, 298, 800, 560), 42); // 10 % of the window
	run(&w, 3.0);
	CHECK(w.moves == 0 && w.st.covered > 0.05f && w.st.covered < 0.15f);

	// 1. A document over it: not before the dwell, then aside, then landed.
	focus(&w, R(2560, 280, 640, 520), 42);
	run(&w, 0.5);
	CHECK(w.moves == 0);
	run(&w, 0.5);
	CHECK(w.moves == 1 && dxr_ss_report_state(&w.st) == DXR_SS_STATE_MOVING);
	CHECK(w.last_move.covered > 0.99f && w.last_move.spot_covered == 0.0f);
	CHECK(w.st.aside_for == 42);
	run(&w, 0.25);
	CHECK(w.events[DXR_SS_EV_LANDED_ASIDE] == 1 && w.st.phase == DXR_SS_PHASE_ASIDE);
	CHECK(w.in.own.x != 2580 || w.in.own.y != 298);
	CHECK(w.st.home_x == 2580 && w.st.home_y == 298); // our move never sets home
	CHECK(dxr_ss_report_state(&w.st) == DXR_SS_STATE_ASIDE);
	int ax = w.in.own.x, ay = w.in.own.y;

	// Home is still covered by the document: no going back.
	run(&w, 5.0);
	CHECK(w.moves == 1 && w.in.own.x == ax && w.in.own.y == ay);

	// 2. The document follows: aside again (after the dwell and the gap).
	focus(&w, R(ax - 20, ay - 18, 640, 520), 42);
	run(&w, 1.0);
	CHECK(w.moves == 2);
	run(&w, 0.25);
	CHECK(w.events[DXR_SS_EV_LANDED_ASIDE] == 2 && w.st.phase == DXR_SS_PHASE_ASIDE);
	CHECK(w.st.home_x == 2580 && w.st.home_y == 298);

	// 3. The document closes: home after the return delay.
	no_focus(&w);
	w.in.tracked_valid = 0;
	run(&w, 1.75);
	CHECK(w.moves == 2);
	run(&w, 0.5);
	CHECK(w.moves == 3 && w.last_move.home && w.last_move.x == 2580 && w.last_move.y == 298);
	run(&w, 0.25);
	CHECK(w.events[DXR_SS_EV_LANDED_HOME] == 1 && w.st.phase == DXR_SS_PHASE_HOME);
	CHECK(w.in.own.x == 2580 && w.in.own.y == 298 && w.st.aside_for == 0);
	CHECK(w.events[DXR_SS_EV_NEW_HOME] == 1); // home never moved
}

static void
test_home_blocked_by_unfocused(void)
{
	World w;
	world_init(&w);
	run(&w, 3.0);
	focus(&w, R(2480, 260, 800, 560), 42);
	run(&w, 1.25);
	CHECK(w.st.phase == DXR_SS_PHASE_ASIDE);
	// The user clicks the app (focus leaves the document) but the document is still
	// over home: stay aside.
	no_focus(&w);
	w.in.tracked_valid = 1;
	w.in.tracked = R(2480, 260, 800, 560);
	run(&w, 5.0);
	CHECK(w.st.phase == DXR_SS_PHASE_ASIDE && w.moves == 1);
	// The document is minimized / closed: back.
	w.in.tracked_valid = 0;
	run(&w, 2.5);
	CHECK(w.moves == 2 && w.last_move.home);
}

static void
test_user_moves(void)
{
	World w;
	world_init(&w);
	run(&w, 3.0);
	focus(&w, R(2480, 260, 800, 560), 42);
	run(&w, 1.25);
	CHECK(w.st.phase == DXR_SS_PHASE_ASIDE);
	// The user drags it: paused while dragging, and where it is left is home.
	w.in.own_moving = 1;
	w.in.own.x = 2000;
	w.in.own.y = 600;
	run(&w, 1.0);
	CHECK(w.st.reason == DXR_SS_REASON_USER_MOVING && dxr_ss_report_state(&w.st) == DXR_SS_STATE_PAUSED);
	CHECK(w.moves == 1);
	w.in.own_moving = 0;
	no_focus(&w);
	run(&w, 0.5);
	CHECK(w.st.reason == DXR_SS_REASON_SETTLING);
	run(&w, 1.0);
	CHECK(w.st.phase == DXR_SS_PHASE_HOME && w.st.home_x == 2000 && w.st.home_y == 600);
	CHECK(w.events[DXR_SS_EV_NEW_HOME] == 2 && w.st.aside_for == 0);
	run(&w, 5.0);
	CHECK(w.moves == 1); // no return to the old home

	// The app moves its own window (displayxr_set_overlay_position): home too.
	w.in.own.x = 2100;
	run(&w, 1.5);
	CHECK(w.st.home_x == 2100 && w.st.phase == DXR_SS_PHASE_HOME);

	// A drag that starts during our glide: the user wins.
	focus(&w, R(2000, 500, 800, 560), 43);
	run(&w, 1.0);
	CHECK(w.moves == 2 && w.st.phase == DXR_SS_PHASE_MOVING);
	w.pending = 0; // the glide was stopped
	w.in.own_moving = 1;
	run(&w, 0.25);
	CHECK(w.events[DXR_SS_EV_ABORTED] == 1 && w.st.phase == DXR_SS_PHASE_SETTLING);
}

static void
test_pauses(void)
{
	World w;
	world_init(&w);
	run(&w, 3.0);
	// Locked: covered or not, nothing moves.
	w.in.locked = 1;
	focus(&w, R(2480, 260, 800, 560), 42);
	run(&w, 5.0);
	CHECK(w.moves == 0 && w.st.reason == DXR_SS_REASON_LOCKED);
	CHECK(dxr_ss_report_state(&w.st) == DXR_SS_STATE_PAUSED);
	w.in.locked = 0;
	run(&w, 0.5);
	CHECK(w.moves == 0); // the dwell starts again after the lock
	run(&w, 0.5);
	CHECK(w.moves == 1);
	run(&w, 0.25);

	// Minimized, maximized, fullscreen: paused.
	World v;
	world_init(&v);
	run(&v, 3.0);
	focus(&v, R(2480, 260, 800, 560), 42);
	v.in.own_minimized = 1;
	run(&v, 2.0);
	CHECK(v.moves == 0 && v.st.reason == DXR_SS_REASON_MINIMIZED);
	v.in.own_minimized = 0;
	v.in.own_maximized = 1;
	run(&v, 2.0);
	CHECK(v.moves == 0 && v.st.reason == DXR_SS_REASON_MAXIMIZED);
	v.in.own_maximized = 0;
	v.in.own_fullscreen = 1;
	run(&v, 2.0);
	CHECK(v.moves == 0 && v.st.reason == DXR_SS_REASON_FULLSCREEN);
	v.in.own_fullscreen = 0;
	run(&v, 1.0);
	CHECK(v.moves == 1);
}

static void
test_no_spot_and_refusal(void)
{
	// A maximized document over the whole monitor: nowhere to go, stay.
	World w;
	world_init(&w);
	run(&w, 3.0);
	focus(&w, R(1920, 0, 1920, 1080), 42);
	run(&w, 5.0);
	CHECK(w.moves == 0 && w.st.reason == DXR_SS_REASON_NO_FREE_SPOT);
	CHECK(w.events[DXR_SS_EV_NO_SPOT] == 1); // once per streak
	CHECK(dxr_ss_report_state(&w.st) == DXR_SS_STATE_HOME);

	// The compositor keeps the window where it is: take that after DXR_SS_LAND_S,
	// and do not keep asking (the gap, then the dwell again).
	World v;
	world_init(&v);
	run(&v, 3.0);
	v.refuse_moves = 1;
	focus(&v, R(2480, 260, 800, 560), 42);
	run(&v, 1.0);
	CHECK(v.moves == 1);
	run(&v, 1.0);
	CHECK(v.st.phase == DXR_SS_PHASE_MOVING);
	run(&v, 1.25);
	CHECK(v.st.phase == DXR_SS_PHASE_ASIDE && v.events[DXR_SS_EV_LANDED_ASIDE] == 1);
	CHECK(v.moves == 1);
}

static void
test_move_cap(void)
{
	// A window that keeps following: at most DXR_SS_CAP_MOVES steps aside a minute.
	World w;
	world_init(&w);
	run(&w, 3.0);
	for (int i = 0; i < 12; i++) {
		focus(&w, R(w.in.own.x - 100, w.in.own.y - 38, 800, 560), 42);
		run(&w, 2.0);
	}
	CHECK(w.moves == DXR_SS_CAP_MOVES);
	CHECK(w.st.reason == DXR_SS_REASON_TOO_MANY_MOVES && dxr_ss_report_state(&w.st) == DXR_SS_STATE_PAUSED);
	CHECK(w.events[DXR_SS_EV_CAPPED] == 1);
	// The way back home is not capped: the document closes, it goes home.
	no_focus(&w);
	w.in.tracked_valid = 0;
	run(&w, 2.5);
	CHECK(w.moves == DXR_SS_CAP_MOVES + 1 && w.last_move.home);
	run(&w, 0.25);
	CHECK(w.st.phase == DXR_SS_PHASE_HOME && w.in.own.x == 2580 && w.in.own.y == 298);
	// Covered again within the minute: still capped. A minute after the first
	// step aside, it may step aside again.
	focus(&w, R(2560, 280, 640, 520), 44);
	run(&w, 2.0);
	CHECK(w.moves == DXR_SS_CAP_MOVES + 1 && w.st.reason == DXR_SS_REASON_TOO_MANY_MOVES);
	run(&w, 40.0);
	CHECK(w.moves == DXR_SS_CAP_MOVES + 2 && !w.last_move.home);
}

static void
test_no_return_home(void)
{
	World w;
	world_init(&w);
	w.s.return_home = 0;
	run(&w, 3.0);
	focus(&w, R(2480, 260, 800, 560), 42);
	run(&w, 1.25);
	CHECK(w.events[DXR_SS_EV_LANDED_ASIDE] == 1 && w.st.phase == DXR_SS_PHASE_HOME);
	CHECK(w.st.home_x == w.in.own.x && w.st.home_y == w.in.own.y);
	no_focus(&w);
	run(&w, 5.0);
	CHECK(w.moves == 1);
}

static void
test_body_band(void)
{
	// The content is the lower half (the avatar); a document over the empty band
	// above it does not count, one over the avatar does.
	World w;
	world_init(&w);
	w.use_body = 1;
	w.body = body_frac(0.2, 0.5, 0.8, 1.0);
	run(&w, 3.0);
	focus(&w, R(2400, 0, 1000, 298 + 230), 42); // down to just above the content
	run(&w, 3.0);
	CHECK(w.moves == 0 && w.st.covered < 0.01f);
	focus(&w, R(2400, 0, 1000, 298 + 400), 42);
	run(&w, 1.0);
	CHECK(w.moves == 1);
	// It steps so the CONTENT clears the document by the margin (the band may stay under it).
	run(&w, 0.25);
	DxrSsRect bb;
	dxr_ss_body_bbox(&w.body, 600, 484, &bb);
	int content_top = w.in.own.y + bb.y;
	int content_left = w.in.own.x + bb.x, content_right = content_left + bb.w;
	CHECK(content_top >= 298 + 400 + 24 || content_right <= 2400 - 24 || content_left >= 3400 + 24);
}

int
main(void)
{
	test_layout();
	test_settings();
	test_coverage();
	test_find_spot();
	test_parse();
	test_start_and_home();
	test_scenario();
	test_home_blocked_by_unfocused();
	test_user_moves();
	test_pauses();
	test_no_spot_and_refusal();
	test_move_cap();
	test_no_return_home();
	test_body_band();
	if (s_failed) {
		fprintf(stderr, "test_linux_side_step: %d check(s) failed\n", s_failed);
		return 1;
	}
	printf("test_linux_side_step: all checks passed\n");
	return 0;
}
