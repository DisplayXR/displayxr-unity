// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Unit test for the side-step's pure part (displayxr_linux_side_step.h): the
// records' layout, settings, the content grid and its coverage, the place to step
// to, the extension's window list, the decision tick by tick (dwell, stepping
// aside, following, going home, the user's moves, pauses, the move cap), and the
// desktop-control gate in front of every poll and glide step (nothing read while
// not granted; a revoke stops it aside or mid-glide; a re-grant starts afresh).
// Built and run by CTest on Linux (no D-Bus, no desktop).

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

//! No work area reported (an extension before v19).
static const DxrSsRect NOWORK = {0, 0, 0, 0};

static int
same_rect(DxrSsRect a, DxrSsRect b)
{
	return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
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
	CHECK(dxr_ss_find_spot(NULL, win, doc, mon, NOWORK, 24, DXR_SS_SIDE_AUTO, &x, &y, &cov));
	CHECK(cov == 0.0f && x == 2480 + 600 + 24 && y == 298);
	CHECK(x >= mon.x && x + 600 <= mon.x + mon.w && y >= mon.y && y + 484 <= mon.y + mon.h);

	// A preferred side wins among equally free places (here: the corners on that side).
	CHECK(dxr_ss_find_spot(NULL, win, doc, mon, NOWORK, 24, DXR_SS_SIDE_BELOW, &x, &y, &cov));
	CHECK(cov == 0.0f && y + 484 == 1080 - 24 && x == 3840 - 24 - 600);
	CHECK(dxr_ss_find_spot(NULL, win, doc, mon, NOWORK, 24, DXR_SS_SIDE_ABOVE, &x, &y, &cov));
	CHECK(cov == 0.0f && y == 24);
	// ... but never a more covered one: left of it is off the monitor (clamped back
	// onto the document), so LEFT still gets a free place on the right.
	CHECK(dxr_ss_find_spot(NULL, win, doc, mon, NOWORK, 24, DXR_SS_SIDE_LEFT, &x, &y, &cov));
	CHECK(cov == 0.0f && x >= 2480 + 600);

	// Cramped (a big document, the prototype's DS1 geometry): no place is quite
	// free; the least covered one is offered, and the caller decides (below half
	// the trigger here: a corner, 5.5 %).
	DxrSsRect big = R(2480, 260, 800, 560);
	CHECK(dxr_ss_find_spot(NULL, win, big, mon, NOWORK, 24, DXR_SS_SIDE_AUTO, &x, &y, &cov));
	CHECK(cov > 0.05f && cov < 0.06f);

	// With room on the left, the shorter move wins.
	DxrSsRect doc2 = R(2700, 260, 500, 560);
	CHECK(dxr_ss_find_spot(NULL, win, doc2, mon, NOWORK, 24, DXR_SS_SIDE_AUTO, &x, &y, &cov));
	CHECK(cov == 0.0f && x == 2700 - 24 - 600 && y == 298);

	// The content's box, not the window, is what keeps the margin.
	DxrSsBody lower = body_frac(0.25, 0.5, 0.75, 1.0); // 150..450 x 242..484 of the window
	CHECK(dxr_ss_find_spot(&lower, win, doc2, mon, NOWORK, 24, DXR_SS_SIDE_LEFT, &x, &y, &cov));
	CHECK(cov == 0.0f && x + 150 + 300 == 2700 - 24);

	// The covering window fills the monitor (maximized): nowhere is free.
	CHECK(dxr_ss_find_spot(NULL, win, mon, mon, NOWORK, 24, DXR_SS_SIDE_AUTO, &x, &y, &cov));
	CHECK(cov >= 0.99f);
	// No monitor reported: no other place.
	CHECK(!dxr_ss_find_spot(NULL, win, doc, R(0, 0, 0, 0), NOWORK, 24, DXR_SS_SIDE_AUTO, &x, &y, &cov));
	// No content at all: nothing to move.
	DxrSsBody empty;
	memset(&empty, 0, sizeof(empty));
	empty.valid = 1;
	CHECK(!dxr_ss_find_spot(&empty, win, doc, mon, NOWORK, 24, DXR_SS_SIDE_AUTO, &x, &y, &cov));
}

//! The monitor's work area (extension v19): corners and the clamp keep out from
//! under the top bar and a dock that stays visible; without it, as before.
static void
test_work_area(void)
{
	DxrSsRect mon = R(0, 0, 1920, 1080);
	DxrSsRect bar = R(0, 32, 1920, 1048);         // a 32 px top bar
	DxrSsRect bar_dock = R(64, 32, 1856, 1048);   // and a 64 px dock on the left
	DxrSsRect bar_bottom = R(0, 32, 1920, 976);   // and a 72 px dock at the bottom

	// The usable area: the work area cut to the monitor, else the whole monitor.
	CHECK(same_rect(dxr_ss_usable_area(mon, NOWORK), mon));
	CHECK(same_rect(dxr_ss_usable_area(mon, bar), bar));
	CHECK(same_rect(dxr_ss_usable_area(mon, R(-100, 32, 3000, 2000)), R(0, 32, 1920, 1048)));
	CHECK(same_rect(dxr_ss_usable_area(mon, R(1920, 0, 1920, 1080)), mon)); // off the monitor
	CHECK(same_rect(dxr_ss_usable_area(mon, R(0, 32, 0, 1048)), mon));      // empty
	CHECK(same_rect(dxr_ss_usable_area(mon, R(0, 32, 1920, -5)), mon));
	CHECK(same_rect(dxr_ss_usable_area(NOWORK, bar), NOWORK)); // no monitor: nothing to cut to

	int x = 0, y = 0;
	float cov = 1.0f;
	// A top corner: below the top bar, `margin` px from it (right above the document
	// is not free any more: the bar pushes that place down onto it).
	DxrSsRect win = R(700, 400, 400, 300);
	DxrSsRect doc = R(600, 300, 700, 500);
	CHECK(dxr_ss_find_spot(NULL, win, doc, mon, bar, 24, DXR_SS_SIDE_ABOVE, &x, &y, &cov));
	CHECK(cov == 0.0f && y >= bar.y + 24 && y == 32 + 24);
	// Without it, as before: right above the document, clamped to the monitor's top
	// edge, under the bar.
	CHECK(dxr_ss_find_spot(NULL, win, doc, mon, NOWORK, 24, DXR_SS_SIDE_ABOVE, &x, &y, &cov));
	CHECK(cov == 0.0f && x == 700 && y == 0);
	// The top-left corner with a dock on the left: right of the dock, below the bar.
	CHECK(dxr_ss_find_spot(NULL, win, doc, mon, bar_dock, 24, DXR_SS_SIDE_ABOVE, &x, &y, &cov));
	CHECK(cov == 0.0f && x == 64 + 24 && y == 32 + 24);
	CHECK(dxr_ss_find_spot(NULL, win, doc, mon, bar, 24, DXR_SS_SIDE_ABOVE, &x, &y, &cov));
	CHECK(cov == 0.0f && x == 24 && y == 32 + 24); // no dock: 24 px from the monitor's left
	// A bottom corner with a dock at the bottom: above the dock.
	CHECK(dxr_ss_find_spot(NULL, win, doc, mon, bar_bottom, 24, DXR_SS_SIDE_BELOW, &x, &y, &cov));
	CHECK(cov == 0.0f && y + 300 == 32 + 976 - 24);
	CHECK(dxr_ss_find_spot(NULL, win, doc, mon, NOWORK, 24, DXR_SS_SIDE_BELOW, &x, &y, &cov));
	CHECK(cov == 0.0f && y + 300 == 1080 - 24); // as before: on the dock

	// Beside the covering window: clamped to the work area too. The window sits at
	// the very top (under the bar); stepping left of the document keeps its y, which
	// the work area moves below the bar.
	DxrSsRect top = R(600, 0, 400, 300);
	DxrSsRect doc_r = R(800, 0, 600, 500);
	CHECK(dxr_ss_find_spot(NULL, top, doc_r, mon, bar, 24, DXR_SS_SIDE_LEFT, &x, &y, &cov));
	CHECK(cov == 0.0f && x == 800 - 24 - 400 && y == 32);
	CHECK(dxr_ss_find_spot(NULL, top, doc_r, mon, NOWORK, 24, DXR_SS_SIDE_LEFT, &x, &y, &cov));
	CHECK(cov == 0.0f && x == 800 - 24 - 400 && y == 0); // as before
	// Left of a document near the dock: never onto the dock.
	DxrSsRect mid = R(300, 400, 400, 300);
	DxrSsRect doc_l = R(200, 300, 1000, 500);
	for (int side = DXR_SS_SIDE_AUTO; side <= DXR_SS_SIDE_BELOW; side++) {
		if (dxr_ss_find_spot(NULL, mid, doc_l, mon, bar_dock, 24, side, &x, &y, &cov))
			CHECK(x >= 64 && y >= 32 && x + 400 <= 64 + 1856 && y + 300 <= 32 + 1048);
		else
			CHECK(0);
	}

	// Every spot, every side, many covering windows: always inside the work area.
	int n = 0;
	for (int kx = 0; kx < 1920; kx += 160)
		for (int ky = 0; ky < 1080; ky += 120)
			for (int side = DXR_SS_SIDE_AUTO; side <= DXR_SS_SIDE_BELOW; side++) {
				DxrSsRect k = R(kx, ky, 500, 400);
				if (!dxr_ss_find_spot(NULL, mid, k, mon, bar_bottom, 24, side, &x, &y, &cov))
					continue;
				n++;
				CHECK(x >= 0 && y >= 32 && x + 400 <= 1920 && y + 300 <= 32 + 976);
			}
	CHECK(n > 500);

	// No work area, a work area that is the whole monitor, an empty one or one off the
	// monitor: exactly the results from before.
	DxrSsRect same_as_mon[4] = {NOWORK, mon, R(0, 32, 0, 0), R(5000, 0, 100, 100)};
	int diffs = 0;
	for (int kx = 0; kx < 1920; kx += 240)
		for (int ky = 0; ky < 1080; ky += 180)
			for (int side = DXR_SS_SIDE_AUTO; side <= DXR_SS_SIDE_BELOW; side++) {
				DxrSsRect k = R(kx, ky, 700, 500);
				int x0 = 0, y0 = 0;
				float c0 = 0.0f;
				int f0 = dxr_ss_find_spot(NULL, mid, k, mon, NOWORK, 24, side, &x0, &y0, &c0);
				for (int i = 1; i < 4; i++) {
					int x1 = 0, y1 = 0;
					float c1 = 0.0f;
					int f1 = dxr_ss_find_spot(NULL, mid, k, mon, same_as_mon[i], 24, side, &x1, &y1, &c1);
					if (f1 != f0 || (f0 && (x1 != x0 || y1 != y0 || c1 != c0)))
						diffs++;
				}
			}
	CHECK(diffs == 0);

	// Too tall for the work area: as far below the bar as fits, still on the monitor.
	DxrSsRect tall = R(900, 10, 600, 1060);
	CHECK(dxr_ss_find_spot(NULL, tall, R(800, 0, 800, 1080), mon, bar, 24, DXR_SS_SIDE_AUTO, &x, &y, &cov));
	CHECK(y == 1080 - 1060 && y + 1060 <= 1080);

	// The decision passes it on: a step aside into a top corner lands below the bar.
	DxrSsInput in;
	memset(&in, 0, sizeof(in));
	DxrSsState st;
	DxrSsSettings s;
	DxrSsDecision d;
	dxr_ss_reset(&st);
	dxr_ss_default_settings(&s);
	s.side = DXR_SS_SIDE_ABOVE;
	in.own_valid = 1;
	in.own = win;
	in.mon = mon;
	in.work = bar;
	int moved = 0;
	for (double t = 0.25; t < 6.0 && !moved; t += 0.25) {
		in.now = t;
		if (t > 3.0) {
			in.focus_valid = 1;
			in.focus = doc;
			in.focus_id = 7;
		}
		dxr_ss_step(&st, &s, &in, &d);
		moved = d.move;
	}
	CHECK(moved && !d.home && d.y == 32 + 24);
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
	CHECK(same_rect(in.work, NOWORK)); // an extension before v19: no work area

	// Extension v19: the monitor's work area, wherever it sits in the object.
	const char *wa = "{\"version\": 1, \"windows\": [{\"id\": 14, \"pid\": 100, \"frame\": [10, 40, 100, 100],"
	                 " \"monitor\": {\"x\": 0, \"y\": 0, \"w\": 1920, \"h\": 1080, \"scale\": 1,"
	                 " \"device_scale\": 1, \"work_area\": [0, 32, 1920, 1048]}}]}";
	CHECK(dxr_ss_parse_windows(wa, 100, 0, &in));
	CHECK(same_rect(in.mon, R(0, 0, 1920, 1080)) && same_rect(in.work, R(0, 32, 1920, 1048)));
	const char *wa_first = "{\"version\": 1, \"windows\": [{\"pid\": 100, \"frame\": [10, 40, 100, 100],"
	                       " \"monitor\": {\"work_area\": [64, 32, 1856, 1048], \"x\": 0, \"y\": 0, \"w\": 1920,"
	                       " \"h\": 1080}}]}";
	CHECK(dxr_ss_parse_windows(wa_first, 100, 0, &in));
	CHECK(same_rect(in.mon, R(0, 0, 1920, 1080)) && same_rect(in.work, R(64, 32, 1856, 1048)));
	// Not four numbers: ignored (the whole monitor), the rest still read.
	const char *wa_bad = "{\"version\": 1, \"windows\": [{\"pid\": 100, \"frame\": [10, 40, 100, 100],"
	                     " \"monitor\": {\"x\": 0, \"y\": 0, \"w\": 1920, \"h\": 1080,"
	                     " \"work_area\": [0, 32, 1920]}}]}";
	CHECK(dxr_ss_parse_windows(wa_bad, 100, 0, &in));
	CHECK(in.own_valid && same_rect(in.mon, R(0, 0, 1920, 1080)) && same_rect(in.work, NOWORK));
	// Only the app's own window's monitor: another window's work area is not read.
	const char *wa_other = "{\"version\": 1, \"windows\": [{\"pid\": 100, \"frame\": [10, 40, 100, 100],"
	                       " \"monitor\": {\"x\": 0, \"y\": 0, \"w\": 1920, \"h\": 1080}},"
	                       "{\"pid\": 200, \"focus\": true, \"frame\": [0, 0, 50, 50], \"monitor\": {\"x\": 0,"
	                       " \"y\": 0, \"w\": 1920, \"h\": 1080, \"work_area\": [0, 32, 1920, 1048]}}]}";
	CHECK(dxr_ss_parse_windows(wa_other, 100, 0, &in));
	CHECK(in.focus_valid && same_rect(in.work, NOWORK));
	CHECK(!dxr_ss_parse_windows(NULL, 100, 0, &in) && same_rect(in.work, NOWORK));
	CHECK(dxr_ss_parse_windows(json, 100, 4000000000ull, &in)); // as above, for what follows
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

/*
 *
 * The desktop-control gate (dxr_ss_poll / dxr_ss_glide), with fake reads that are
 * counted: the worker's poll, exactly, around a small world built as the
 * extension's JSON.
 *
 */

typedef struct {
	DxrSsState st;
	DxrSsGate g;
	DxrSsSettings s;
	DxrSsOps ops;
	double t;
	int granted;           // the runtime's answer: DXR_SS_DC_NO / _GRANTED / _SUSPENDED (a lock)
	int revoke_at_move;    // > 0: the grant ends right after this many moves (a revoke mid-glide) ...
	int revoke_to;         // ... and the answer becomes this (DXR_SS_DC_NO, or _SUSPENDED: a lock)
	DxrSsRect own, mon, doc;
	int doc_on;            // the document is open ...
	int doc_unfocused;     // ... and (1) does not have the focus
	int n_ask, n_list, n_lock, n_moves;
	char calls[4096];      // 'g' granted, 'n' no, 's' suspended, 'L' window list, 'K' lock state, 'M' move
	int n_calls;
	int events[16], gates[5];
	DxrSsPoll last;
	int glide_r;           // the last glide's result (-1 none)
} Gw;

static void
gw_call(Gw *w, char c)
{
	if (w->n_calls < (int)sizeof(w->calls) - 1)
		w->calls[w->n_calls++] = c;
}

static int
gw_granted(void *ctx)
{
	Gw *w = ctx;
	w->n_ask++;
	gw_call(w, w->granted == DXR_SS_DC_GRANTED ? 'g' : w->granted == DXR_SS_DC_SUSPENDED ? 's' : 'n');
	return w->granted;
}

static char *
gw_list(void *ctx)
{
	Gw *w = ctx;
	w->n_list++;
	gw_call(w, 'L');
	char *j = malloc(1024);
	int n = snprintf(j, 1024,
	                 "{\"version\": 1, \"windows\": [{\"id\": 14, \"pid\": 100, \"focus\": false, "
	                 "\"frame\": [%d, %d, %d, %d], \"monitor\": {\"x\": %d, \"y\": %d, \"w\": %d, \"h\": %d}}",
	                 w->own.x, w->own.y, w->own.w, w->own.h, w->mon.x, w->mon.y, w->mon.w, w->mon.h);
	if (w->doc_on)
		n += snprintf(j + n, 1024 - (size_t)n,
		              ", {\"id\": 42, \"pid\": 200, \"app_id\": \"doc\", \"focus\": %s, \"frame\": [%d, %d, %d, %d]}",
		              w->doc_unfocused ? "false" : "true", w->doc.x, w->doc.y, w->doc.w, w->doc.h);
	snprintf(j + n, 1024 - (size_t)n, "]}");
	return j;
}

static int
gw_lock(void *ctx)
{
	Gw *w = ctx;
	w->n_lock++;
	gw_call(w, 'K');
	return 0;
}

static int
gw_no(void *ctx)
{
	(void)ctx;
	return 0;
}

static int
gw_on(void *ctx)
{
	(void)ctx;
	return 1;
}

static int
gw_move(void *ctx, int x, int y, int last)
{
	(void)last;
	Gw *w = ctx;
	w->n_moves++;
	gw_call(w, 'M');
	w->own.x = x; // lands at once
	w->own.y = y;
	if (w->revoke_at_move > 0 && w->n_moves == w->revoke_at_move)
		w->granted = w->revoke_to;
	return 1;
}

static void
gw_wait(void *ctx, double s)
{
	Gw *w = ctx;
	w->t += s;
}

static void
gw_init(Gw *w)
{
	memset(w, 0, sizeof(*w));
	dxr_ss_reset(&w->st);
	dxr_ss_default_settings(&w->s);
	w->ops.ctx = w;
	w->ops.granted = gw_granted;
	w->ops.window_list = gw_list;
	w->ops.screen_locked = gw_lock;
	w->ops.own_drag = gw_no;
	w->ops.still_on = gw_on;
	w->ops.move = gw_move;
	w->ops.wait_s = gw_wait;
	w->own = R(2580, 298, 600, 484);
	w->mon = R(1920, 0, 1920, 1080);
	w->doc = R(2480, 260, 800, 560); // over the window, room on its right
	w->glide_r = -1;
}

//! The worker's loop: a poll every 0.25 s, then the glide its decision asks for.
static void
gw_run(Gw *w, double dt)
{
	double end = w->t + dt;
	while (w->t + 1e-9 < end) {
		w->t += 0.25;
		dxr_ss_poll(&w->st, &w->g, &w->s, &w->ops, w->t, 100, NULL, &w->last);
		w->gates[w->last.gate]++;
		if (w->last.read && w->last.d.event)
			w->events[w->last.d.event]++;
		if (w->last.read && w->last.d.move)
			w->glide_r = dxr_ss_glide(&w->g, &w->st, &w->s, &w->ops, w->own.x, w->own.y, w->last.d.x, w->last.d.y);
	}
}

//! Every read of other windows (L, K) and every move (M) came after a 'g' (granted)
//! with no 'n' (not granted) or 's' (suspended) in between.
static int
gw_reads_gated(const Gw *w)
{
	char gate = 0;
	for (int i = 0; i < w->n_calls; i++) {
		char c = w->calls[i];
		if (c == 'g' || c == 'n' || c == 's')
			gate = c;
		else if (gate != 'g')
			return 0;
	}
	return 1;
}

static void
test_gate_never_granted(void)
{
	// Never asked / denied / a runtime without the extension: nothing is ever read,
	// whatever covers the window.
	Gw w;
	gw_init(&w);
	w.doc_on = 1;
	gw_run(&w, 10.0);
	CHECK(w.n_ask == 40 && w.n_list == 0 && w.n_lock == 0 && w.n_moves == 0);
	CHECK(!w.last.granted && !w.last.read && w.last.gate == DXR_SS_GATE_SAME && w.gates[DXR_SS_GATE_CLOSED] == 0);
	CHECK(w.st.reason == DXR_SS_REASON_NOT_GRANTED && dxr_ss_report_state(&w.st) == DXR_SS_STATE_PAUSED);
	CHECK(w.st.covered == 0.0f && !w.st.have_home && w.st.aside_for == 0);
	for (int e = 1; e < 16; e++)
		CHECK(w.events[e] == 0);
}

static void
test_gate_granted_scenario(void)
{
	// Granted from the start: the #400 scenario, unchanged: one ask, then the reads,
	// every poll.
	Gw w;
	gw_init(&w);
	w.granted = 1;
	gw_run(&w, 3.0);
	CHECK(w.gates[DXR_SS_GATE_OPENED] == 1 && w.st.have_home && w.st.home_x == 2580 && w.st.home_y == 298);
	CHECK(w.n_list == 12 && w.n_lock == 12);
	w.doc_on = 1;
	gw_run(&w, 1.0);
	CHECK(w.glide_r == DXR_SS_GLIDE_DONE && w.n_moves == 8); // 0.3 s glide: 8 steps
	gw_run(&w, 0.25);
	CHECK(w.events[DXR_SS_EV_LANDED_ASIDE] == 1 && w.st.phase == DXR_SS_PHASE_ASIDE && w.st.aside_for == 42);
	w.doc_on = 0;
	gw_run(&w, 3.0);
	CHECK(w.events[DXR_SS_EV_LANDED_HOME] == 1 && w.own.x == 2580 && w.own.y == 298);
	CHECK(gw_reads_gated(&w));
}

static void
test_gate_revoke_aside_and_regrant(void)
{
	Gw w;
	gw_init(&w);
	w.granted = 1;
	gw_run(&w, 3.0);
	w.doc_on = 1;
	gw_run(&w, 1.5); // the dwell, the glide, and the poll that sees it landed
	CHECK(w.st.phase == DXR_SS_PHASE_ASIDE && w.st.aside_for == 42);
	int ax = w.own.x, ay = w.own.y, moves = w.n_moves, lists = w.n_list, locks = w.n_lock;
	CHECK(ax != 2580);

	// Revoked (the user's deny, a resumed kill switch: anything but suspended) while
	// aside: the next poll stops, reads nothing, forgets home and the window it
	// stepped aside for.
	w.granted = 0;
	gw_run(&w, 0.25);
	CHECK(w.last.gate == DXR_SS_GATE_CLOSED && !w.last.granted);
	CHECK(w.n_list == lists && w.n_lock == locks);
	CHECK(w.st.reason == DXR_SS_REASON_NOT_GRANTED && dxr_ss_report_state(&w.st) == DXR_SS_STATE_PAUSED);
	CHECK(!w.st.have_home && w.st.aside_for == 0 && w.st.covered == 0.0f && w.st.phase == DXR_SS_PHASE_SETTLING);
	// The document closes: no way back home (nothing is read; the window stays).
	w.doc_on = 0;
	gw_run(&w, 10.0);
	CHECK(w.n_moves == moves && w.n_list == lists && w.n_lock == locks && w.own.x == ax && w.own.y == ay);
	CHECK(w.gates[DXR_SS_GATE_CLOSED] == 1 && w.events[DXR_SS_EV_LANDED_HOME] == 0);

	// Granted again: afresh; where the window is (aside) becomes home, no move back.
	w.granted = 1;
	gw_run(&w, 0.25);
	CHECK(w.last.gate == DXR_SS_GATE_OPENED && w.last.read && w.st.reason == DXR_SS_REASON_SETTLING);
	gw_run(&w, 2.5);
	CHECK(w.st.have_home && w.st.home_x == ax && w.st.home_y == ay && w.st.phase == DXR_SS_PHASE_HOME);
	CHECK(w.events[DXR_SS_EV_NEW_HOME] == 2);
	gw_run(&w, 5.0);
	CHECK(w.n_moves == moves && w.own.x == ax && w.own.y == ay);
	// ... and it works again from the new home.
	w.doc = R(ax - 20, ay - 18, 640, 520);
	w.doc_on = 1;
	gw_run(&w, 1.5);
	CHECK(w.n_moves > moves && w.st.phase == DXR_SS_PHASE_ASIDE && w.st.home_x == ax);
	CHECK(gw_reads_gated(&w));
}

static void
test_gate_revoke_mid_glide(void)
{
	// A slow glide (1 s, 25 steps); the grant ends after its 5th step: the 6th is not
	// made, the window stays where the 5th put it, nothing lands, everything is forgotten.
	Gw w;
	gw_init(&w);
	w.granted = 1;
	w.s.glide_s = 1.0f;
	gw_run(&w, 3.0);
	w.revoke_at_move = 5;
	w.doc_on = 1;
	gw_run(&w, 0.5); // the dwell is not over yet
	CHECK(w.n_moves == 0);
	gw_run(&w, 0.5); // the step aside starts and is cut short
	int lists = w.n_list;
	CHECK(w.glide_r == DXR_SS_GLIDE_NOT_GRANTED && w.n_moves == 5);
	CHECK((w.own.x != 2580 || w.own.y != 298) && (w.own.x != w.last.d.x || w.own.y != w.last.d.y)); // part way
	CHECK(w.g.open == 0 && w.st.reason == DXR_SS_REASON_NOT_GRANTED && !w.st.have_home && w.st.aside_for == 0);
	int x = w.own.x, y = w.own.y;
	gw_run(&w, 5.0);
	CHECK(w.n_moves == 5 && w.n_list == lists && w.own.x == x && w.own.y == y);
	CHECK(w.events[DXR_SS_EV_LANDED_ASIDE] == 0 && w.events[DXR_SS_EV_LANDED_HOME] == 0);
	// The glide closed the gate itself: the polls after it report no new closing.
	CHECK(w.gates[DXR_SS_GATE_CLOSED] == 0);
	CHECK(gw_reads_gated(&w));

	// Granted again: where the glide stopped becomes home.
	w.granted = 1;
	w.revoke_at_move = 0;
	w.doc_on = 0;
	gw_run(&w, 3.0);
	CHECK(w.st.have_home && w.st.home_x == x && w.st.home_y == y && w.n_moves == 5);
}

static void
test_gate_keeps_the_cap(void)
{
	// Our steps aside still count for the cap across a revoke and a re-grant.
	Gw w;
	gw_init(&w);
	w.granted = 1;
	w.s.glide_s = 0.0f;
	gw_run(&w, 3.0);
	w.doc_on = 1;
	gw_run(&w, 1.25);
	CHECK(w.st.n_moves == 1);
	w.granted = 0;
	gw_run(&w, 0.5);
	w.granted = 1;
	gw_run(&w, 0.25);
	CHECK(w.last.gate == DXR_SS_GATE_OPENED && w.st.n_moves == 1 && dxr_ss_recent_moves(&w.st, w.t) == 1);
}

static void
test_gate_new_session(void)
{
	// The worker clears the gate when the session ends (the grant is asked for again
	// with the next one): the first granted poll after it starts afresh, whatever the
	// decision remembered.
	Gw w;
	gw_init(&w);
	w.granted = 1;
	gw_run(&w, 3.0);
	CHECK(w.st.have_home && w.gates[DXR_SS_GATE_OPENED] == 1);
	w.g.open = 0;
	gw_run(&w, 0.25);
	CHECK(w.last.gate == DXR_SS_GATE_OPENED && !w.st.have_home && w.st.reason == DXR_SS_REASON_SETTLING);
}

/*
 *
 * A screen lock (the runtime says Suspended): hold still and read nothing, drop the
 * id of the window it stepped aside for, keep the app's own home and whether it is
 * aside; carry on from there after the unlock.
 *
 */

//! Granted, home, then a document over it: aside (and landed).
static void
gw_aside(Gw *w)
{
	gw_init(w);
	w->granted = DXR_SS_DC_GRANTED;
	gw_run(w, 3.0);
	w->doc_on = 1;
	gw_run(w, 1.5);
}

static void
test_lock_keeps_home_aside(void)
{
	Gw w;
	gw_aside(&w);
	CHECK(w.st.phase == DXR_SS_PHASE_ASIDE && w.st.aside_for == 42);
	int ax = w.own.x, ay = w.own.y, lists = w.n_list, locks = w.n_lock, moves = w.n_moves;

	// Locked: nothing is read, nothing moves; home and "aside" are kept, the id is dropped.
	w.granted = DXR_SS_DC_SUSPENDED;
	gw_run(&w, 0.25);
	CHECK(w.last.gate == DXR_SS_GATE_SUSPENDED && !w.last.granted && w.g.open && w.g.suspended);
	CHECK(w.st.reason == DXR_SS_REASON_LOCKED && dxr_ss_report_state(&w.st) == DXR_SS_STATE_PAUSED);
	CHECK(w.st.have_home && w.st.home_x == 2580 && w.st.home_y == 298 && w.st.phase == DXR_SS_PHASE_ASIDE);
	CHECK(w.st.aside_for == 0 && w.st.covered == 0.0f);
	w.doc_on = 0; // the document closes while the screen is locked
	gw_run(&w, 10.0);
	CHECK(w.n_list == lists && w.n_lock == locks && w.n_moves == moves && w.own.x == ax && w.own.y == ay);
	CHECK(w.gates[DXR_SS_GATE_SUSPENDED] == 1 && w.gates[DXR_SS_GATE_CLOSED] == 0);
	CHECK(w.st.have_home && w.st.phase == DXR_SS_PHASE_ASIDE);

	// Unlocked: carries on from the kept home (no settling, no new home); home is free,
	// so back after ReturnDelay.
	w.granted = DXR_SS_DC_GRANTED;
	gw_run(&w, 0.25);
	CHECK(w.last.gate == DXR_SS_GATE_RESUMED && w.last.read && !w.g.suspended);
	CHECK(w.st.reason == DXR_SS_REASON_NONE && w.st.phase == DXR_SS_PHASE_ASIDE && w.st.home_x == 2580);
	gw_run(&w, 3.0);
	CHECK(w.events[DXR_SS_EV_LANDED_HOME] == 1 && w.own.x == 2580 && w.own.y == 298);
	CHECK(w.events[DXR_SS_EV_NEW_HOME] == 1 && w.gates[DXR_SS_GATE_OPENED] == 1);
	CHECK(gw_reads_gated(&w));
}

static void
test_lock_then_only_the_focused_window_counts(void)
{
	// Unlocked with the focused document still over home: stays aside. The document
	// loses the focus but stays put: the id it stepped aside for was dropped at the
	// lock, so only the focused window counts now, and home counts as free.
	Gw w;
	gw_aside(&w);
	int ax = w.own.x, moves = w.n_moves;
	w.granted = DXR_SS_DC_SUSPENDED;
	gw_run(&w, 2.0);
	w.granted = DXR_SS_DC_GRANTED;
	gw_run(&w, 5.0);
	CHECK(w.st.phase == DXR_SS_PHASE_ASIDE && w.n_moves == moves && w.own.x == ax);
	w.doc_unfocused = 1;
	gw_run(&w, 3.0);
	CHECK(w.events[DXR_SS_EV_LANDED_HOME] == 1 && w.own.x == 2580 && w.own.y == 298);

	// Without a lock the unfocused document still holds it aside (#400's rule).
	Gw v;
	gw_aside(&v);
	v.doc_unfocused = 1;
	gw_run(&v, 5.0);
	CHECK(v.st.phase == DXR_SS_PHASE_ASIDE && v.events[DXR_SS_EV_LANDED_HOME] == 0);
}

static void
test_lock_at_home(void)
{
	Gw w;
	gw_init(&w);
	w.granted = DXR_SS_DC_GRANTED;
	gw_run(&w, 3.0);
	CHECK(w.st.phase == DXR_SS_PHASE_HOME);
	w.granted = DXR_SS_DC_SUSPENDED;
	gw_run(&w, 5.0);
	CHECK(w.st.reason == DXR_SS_REASON_LOCKED && w.st.have_home && w.st.phase == DXR_SS_PHASE_HOME);
	w.granted = DXR_SS_DC_GRANTED;
	gw_run(&w, 0.25);
	CHECK(w.last.gate == DXR_SS_GATE_RESUMED && w.st.phase == DXR_SS_PHASE_HOME && w.st.reason == DXR_SS_REASON_NONE);
	CHECK(w.events[DXR_SS_EV_NEW_HOME] == 1 && w.st.home_x == 2580 && w.st.home_y == 298);
	// ... and it works at once (no start-up delay, no settling).
	w.doc_on = 1;
	gw_run(&w, 1.5);
	CHECK(w.st.phase == DXR_SS_PHASE_ASIDE && w.st.home_x == 2580);
}

static void
test_lock_mid_glide(void)
{
	// The screen locks after step 5 of a 25-step glide: step 6 is not made, the window
	// stays where step 5 put it, nothing lands; home is kept and it counts as aside.
	Gw w;
	gw_init(&w);
	w.granted = DXR_SS_DC_GRANTED;
	w.s.glide_s = 1.0f;
	gw_run(&w, 3.0);
	w.revoke_at_move = 5;
	w.revoke_to = DXR_SS_DC_SUSPENDED;
	w.doc_on = 1;
	gw_run(&w, 1.0);
	CHECK(w.glide_r == DXR_SS_GLIDE_SUSPENDED && w.n_moves == 5);
	CHECK((w.own.x != 2580 || w.own.y != 298) && (w.own.x != w.last.d.x || w.own.y != w.last.d.y));
	CHECK(w.g.open && w.g.suspended && w.st.reason == DXR_SS_REASON_LOCKED);
	CHECK(w.st.have_home && w.st.home_x == 2580 && w.st.phase == DXR_SS_PHASE_ASIDE && w.st.aside_for == 0);
	CHECK(w.st.last_x == w.own.x && w.st.last_y == w.own.y);
	int lists = w.n_list;
	w.doc_on = 0;
	gw_run(&w, 5.0);
	CHECK(w.n_moves == 5 && w.n_list == lists && w.events[DXR_SS_EV_LANDED_ASIDE] == 0);
	// Unlocked: not a move by someone else (no new home); home is free: back.
	w.revoke_at_move = 0;
	w.granted = DXR_SS_DC_GRANTED;
	w.s.glide_s = 0.3f;
	gw_run(&w, 0.25);
	CHECK(w.last.gate == DXR_SS_GATE_RESUMED && w.st.reason == DXR_SS_REASON_NONE);
	gw_run(&w, 3.0);
	CHECK(w.events[DXR_SS_EV_LANDED_HOME] == 1 && w.own.x == 2580 && w.own.y == 298);
	CHECK(w.events[DXR_SS_EV_NEW_HOME] == 1);

	// ReturnHome off: a step aside cut short by a lock ends where it is, which becomes
	// home after the unlock (as a landed one would); no move.
	Gw v;
	gw_init(&v);
	v.granted = DXR_SS_DC_GRANTED;
	v.s.glide_s = 1.0f;
	v.s.return_home = 0;
	gw_run(&v, 3.0);
	v.revoke_at_move = 5;
	v.revoke_to = DXR_SS_DC_SUSPENDED;
	v.doc_on = 1;
	gw_run(&v, 1.0);
	CHECK(v.glide_r == DXR_SS_GLIDE_SUSPENDED && v.st.phase == DXR_SS_PHASE_SETTLING);
	int x = v.own.x, y = v.own.y;
	v.doc_on = 0;
	v.revoke_at_move = 0;
	v.granted = DXR_SS_DC_GRANTED;
	gw_run(&v, 2.0);
	CHECK(v.st.phase == DXR_SS_PHASE_HOME && v.st.home_x == x && v.st.home_y == y && v.n_moves == 5);
}

static void
test_lock_then_revoke_forgets(void)
{
	// Suspended, then any other no (the kill switch's resume reads Denied; a deny):
	// everything is forgotten after all. The grant again: afresh.
	Gw w;
	gw_aside(&w);
	int ax = w.own.x, ay = w.own.y;
	w.granted = DXR_SS_DC_SUSPENDED;
	gw_run(&w, 2.0);
	CHECK(w.st.have_home);
	w.granted = DXR_SS_DC_NO;
	gw_run(&w, 0.25);
	CHECK(w.last.gate == DXR_SS_GATE_CLOSED && !w.g.open && !w.g.suspended);
	CHECK(!w.st.have_home && w.st.reason == DXR_SS_REASON_NOT_GRANTED);
	w.granted = DXR_SS_DC_GRANTED;
	gw_run(&w, 3.0);
	CHECK(w.gates[DXR_SS_GATE_OPENED] == 2 && w.st.home_x == ax && w.st.home_y == ay);

	// Never granted, the screen locks (the runtime says Suspended before "not
	// requested"): nothing to keep; NotGranted, as before.
	Gw v;
	gw_init(&v);
	v.granted = DXR_SS_DC_SUSPENDED;
	gw_run(&v, 2.0);
	CHECK(!v.g.open && !v.g.suspended && v.st.reason == DXR_SS_REASON_NOT_GRANTED && v.n_list == 0);
	CHECK(v.gates[DXR_SS_GATE_SUSPENDED] == 0);
}

int
main(void)
{
	test_layout();
	test_settings();
	test_coverage();
	test_find_spot();
	test_work_area();
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
	test_gate_never_granted();
	test_gate_granted_scenario();
	test_gate_revoke_aside_and_regrant();
	test_gate_revoke_mid_glide();
	test_gate_keeps_the_cap();
	test_gate_new_session();
	test_lock_keeps_home_aside();
	test_lock_then_only_the_focused_window_counts();
	test_lock_at_home();
	test_lock_mid_glide();
	test_lock_then_revoke_forgets();
	if (s_failed) {
		fprintf(stderr, "test_linux_side_step: %d check(s) failed\n", s_failed);
		return 1;
	}
	printf("test_linux_side_step: all checks passed\n");
	return 0;
}
