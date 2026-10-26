// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Unit test for the native-Wayland placement arithmetic (#361):
// displayxr_linux_wayland_place.h. Built and run by CTest on Linux.

#include "displayxr_linux_wayland_place.h"

#include <stdio.h>

static int s_failed;

#define CHECK(cond)                                                                                                \
	do {                                                                                                       \
		if (!(cond)) {                                                                                     \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                            \
			s_failed++;                                                                                \
		}                                                                                                  \
	} while (0)

int
main(void)
{
	// #361's box: Odyssey on HDMI-1 at stage (0,0) 2560x1440 @150 %, laptop eDP-1
	// at stage (2560,0) 1728x1080 @166.7 %; XWayland scale 2 (X root = stage x 2).
	const DxrWlRect4 odyssey = {0, 0, 2560, 1440};
	const DxrWlRect4 laptop = {2560, 0, 1728, 1080};
	const int n = 2;
	// The window: 680x1368 device px, 453x912 stage px on the Odyssey.
	const int ww = 453, wh = 912;

	// X root -> stage.
	CHECK(dxr_wl_x11_to_stage(5882, 2) == 2941);
	CHECK(dxr_wl_x11_to_stage(382, 2) == 191);
	CHECK(dxr_wl_x11_to_stage(5883, 2) == 2942); // rounds to nearest
	CHECK(dxr_wl_x11_to_stage(-10, 2) == -5);
	CHECK(dxr_wl_x11_to_stage(7, 0) == 7);       // no scale: unchanged

	// The saved position from the earlier layout (a DS1 at 200 %, its window at
	// stage (2941,191)) lands on today's laptop: stale.
	CHECK(!dxr_wl_saved_position_on_panel(odyssey, 5882, 382, n, ww, wh));
	CHECK(dxr_wl_saved_position_on_panel(laptop, 5882, 382, n, ww, wh));
	// The same without a known size (top-left rule).
	CHECK(!dxr_wl_saved_position_on_panel(odyssey, 5882, 382, n, 0, 0));

	// A position saved on the Odyssey today (centred: stage (1053,264)) is kept.
	CHECK(dxr_wl_saved_position_on_panel(odyssey, 2106, 528, n, ww, wh));
	// (0,0) is a legitimate position: the panel's top-left.
	CHECK(dxr_wl_saved_position_on_panel(odyssey, 0, 0, n, ww, wh));
	// Mostly off the right edge of the Odyssey: its centre is on the laptop.
	CHECK(!dxr_wl_saved_position_on_panel(odyssey, 2 * 2400, 0, n, ww, wh));
	// Its top-left on the Odyssey but its centre just over the edge.
	CHECK(!dxr_wl_saved_position_on_panel(odyssey, 2 * (2560 - ww / 2), 0, n, ww, wh));
	CHECK(dxr_wl_saved_position_on_panel(odyssey, 2 * (2560 - ww / 2 - 1), 0, n, ww, wh));
	// Above / left of the whole desktop.
	CHECK(!dxr_wl_saved_position_on_panel(odyssey, -2000, -2000, n, ww, wh));
	// No scale: cannot be converted, never "on the panel".
	CHECK(!dxr_wl_saved_position_on_panel(odyssey, 2106, 528, 0, ww, wh));
	// No panel rect.
	DxrWlRect4 none = {0, 0, 0, 0};
	CHECK(!dxr_wl_saved_position_on_panel(none, 0, 0, n, ww, wh));

	// Window on a monitor: centre rule.
	CHECK(dxr_wl_window_on_rect(odyssey, 1053, 264, ww, wh));
	CHECK(!dxr_wl_window_on_rect(odyssey, 2941, 191, 340, 684));
	CHECK(dxr_wl_window_on_rect(laptop, 2941, 191, 340, 684));

	// Monitor identity, to a pixel of rounding.
	CHECK(dxr_wl_monitor_is(odyssey, odyssey));
	DxrWlRect4 odyssey_rounded = {0, 0, 2561, 1439};
	CHECK(dxr_wl_monitor_is(odyssey_rounded, odyssey));
	CHECK(!dxr_wl_monitor_is(laptop, odyssey));
	CHECK(!dxr_wl_monitor_is(none, odyssey));
	CHECK(!dxr_wl_monitor_is(odyssey, none));

	// A size handed to the player, checked once it has had time to apply it. The
	// failing run: DS1 left of the laptop at 150 %, the app's 1200x966 px sent as
	// 800x644 logical, and the player back at 600x483 within the same frame.
	const long long until = 2000000000LL;
	CHECK(dxr_wl_sent_size_outcome(until - 1, until, 800, 644, 600, 483) == 0);  // still settling
	CHECK(dxr_wl_sent_size_outcome(until, until, 800, 644, 800, 644) == 1);      // kept
	CHECK(dxr_wl_sent_size_outcome(until, until, 800, 644, 600, 483) == -1);     // reverted: ask again
	CHECK(dxr_wl_sent_size_outcome(until + 5, until, 800, 644, 800, 483) == -1); // half of it
	CHECK(dxr_wl_sent_size_outcome(until, until, 0, 0, 600, 483) == 1);          // nothing sent

	// The start-up placement. The harness's casting case: the panel (Meta-0) at
	// stage (0,0) 1920x1080, a casting tool's virtual monitor at (2880,0) 1920x1080
	// with the pointer on it, where mutter maps the 640x424 window.
	const DxrWlRect4 panel = {0, 0, 1920, 1080};
	const DxrWlRect4 virt = {2880, 0, 1920, 1080};
	const DxrWlRect4 nomon = {0, 0, 0, 0};
	const long long t0 = 1000000000LL, lim = t0 + 5000000000LL;
	// Opened on the virtual monitor: move it.
	CHECK(dxr_wl_startup_action(0, 1, panel, virt, 3520, 328, 640, 424, t0, lim) == DXR_WL_STARTUP_MOVE);
	// Opened on the panel (the pointer was there): leave it.
	CHECK(dxr_wl_startup_action(0, 1, panel, panel, 640, 328, 640, 424, t0, lim) == DXR_WL_STARTUP_THERE);
	// No monitor reported (older extension): the frame's centre decides.
	CHECK(dxr_wl_startup_action(0, 1, panel, nomon, 640, 328, 640, 424, t0, lim) == DXR_WL_STARTUP_THERE);
	CHECK(dxr_wl_startup_action(0, 1, panel, nomon, 3520, 328, 640, 424, t0, lim) == DXR_WL_STARTUP_MOVE);
	// Straddling the panel's right edge: top-left on the panel, centre past it.
	CHECK(dxr_wl_startup_action(0, 1, panel, nomon, 1700, 0, 640, 424, t0, lim) == DXR_WL_STARTUP_MOVE);
	// The app (a restored position, DisplayXRTargetDisplay) or the user placed it: theirs, wherever it is.
	CHECK(dxr_wl_startup_action(1, 1, panel, virt, 3520, 328, 640, 424, t0, lim) == DXR_WL_STARTUP_PLACED);
	CHECK(dxr_wl_startup_action(1, 0, panel, nomon, 0, 0, 0, 0, t0, lim) == DXR_WL_STARTUP_PLACED);
	// Not shown yet: wait, but not past the limit; shown after the limit: no late jump.
	CHECK(dxr_wl_startup_action(0, 0, panel, nomon, 0, 0, 0, 0, t0, lim) == DXR_WL_STARTUP_WAIT);
	CHECK(dxr_wl_startup_action(0, 0, panel, nomon, 0, 0, 0, 0, lim + 1, lim) == DXR_WL_STARTUP_EXPIRED);
	CHECK(dxr_wl_startup_action(0, 1, panel, virt, 3520, 328, 640, 424, lim + 1, lim) == DXR_WL_STARTUP_EXPIRED);
	CHECK(dxr_wl_startup_action(0, 1, panel, virt, 3520, 328, 640, 424, lim, lim) == DXR_WL_STARTUP_MOVE);
	// No limit (0): decided whenever it is shown.
	CHECK(dxr_wl_startup_action(0, 1, panel, virt, 3520, 328, 640, 424, lim * 10, 0) == DXR_WL_STARTUP_MOVE);
	// The panel at a negative origin (left of the primary), the window on the primary.
	const DxrWlRect4 left = {-1920, 0, 1920, 1080};
	const DxrWlRect4 primary = {0, 0, 2560, 1440};
	CHECK(dxr_wl_startup_action(0, 1, left, primary, 960, 508, 640, 424, t0, lim) == DXR_WL_STARTUP_MOVE);
	CHECK(dxr_wl_startup_action(0, 1, left, left, -1280, 328, 640, 424, t0, lim) == DXR_WL_STARTUP_THERE);

	// Which moves keep the size the window opened with (logical) rather than its
	// device size. The start-up placement does, also after #361's 3 s restore window
	// (a slow first frame delays the session's second frame past it).
	CHECK(dxr_wl_keeps_opening_size(0, 1, 1));
	CHECK(dxr_wl_keeps_opening_size(0, 1, 0));
	// Another placement while the window is new (a move to the panel, a restore).
	CHECK(dxr_wl_keeps_opening_size(0, 0, 1));
	// ... but not later: as on X11, a later move keeps the device size.
	CHECK(!dxr_wl_keeps_opening_size(0, 0, 0));
	// A size the app asked for always stands.
	CHECK(!dxr_wl_keeps_opening_size(1, 1, 1));
	CHECK(!dxr_wl_keeps_opening_size(1, 1, 0));
	CHECK(!dxr_wl_keeps_opening_size(1, 0, 1));
	// Which moves are start-up moves (one rule for all of them): the start-up
	// placement and DisplayXRTargetDisplay's move at start, while nobody has placed
	// the window. A move the app asks for later (MoveNow) is not one, nor is a move at
	// start after the app (a restored position) or the user (a drag) placed it.
	CHECK(dxr_wl_is_start_move(1, 0));
	CHECK(!dxr_wl_is_start_move(1, 1));
	CHECK(!dxr_wl_is_start_move(0, 0));
	CHECK(!dxr_wl_is_start_move(0, 1));
	// What a move to the panel does, as move_player_to_panel applies it: the _at_start
	// entry point (DisplayXRTargetDisplay once the session runs) on a window nobody has
	// placed is a start-up move (s_startup_move: the opening size is kept); MoveNow is
	// not; nor is a move at start after the app or the user placed the window; a window
	// on the panel already is left alone.
	DxrWlPanelMove pm = dxr_wl_panel_move(1, 0, 0);
	CHECK(pm.move && pm.start_move);
	pm = dxr_wl_panel_move(0, 0, 0);
	CHECK(pm.move && !pm.start_move);
	pm = dxr_wl_panel_move(1, 1, 0);
	CHECK(pm.move && !pm.start_move);
	pm = dxr_wl_panel_move(1, 0, 1);
	CHECK(!pm.move && !pm.start_move);
	pm = dxr_wl_panel_move(0, 1, 1);
	CHECK(!pm.move && !pm.start_move);
	// DisplayXRTargetDisplay at start after a slow first frame (the window shown more
	// than 3 s ago: restore window closed) keeps the opening size; MoveNow then does
	// not; the app's size stands either way.
	CHECK(dxr_wl_keeps_opening_size(0, dxr_wl_is_start_move(1, 0), 0));
	CHECK(!dxr_wl_keeps_opening_size(0, dxr_wl_is_start_move(0, 0), 0));
	CHECK(!dxr_wl_keeps_opening_size(1, dxr_wl_is_start_move(1, 0), 0));

	// Fitting into 95 % of an output, aspect kept (to a pixel: the limit is not exact
	// in binary floating point).
	int fw = 600, fh = 480;
	dxr_wl_fit_output(&fw, &fh, 1920, 1080, 1.0);
	CHECK(fw == 600 && fh == 480); // fits: unchanged
	fw = 1900, fh = 1000;
	dxr_wl_fit_output(&fw, &fh, 1920, 1080, 1.0);
	CHECK(abs(fw - 1824) <= 1 && abs(fh - 960) <= 1); // too wide
	fw = 1000, fh = 1900;
	dxr_wl_fit_output(&fw, &fh, 1920, 1080, 1.0);
	CHECK(abs(fw - 540) <= 1 && abs(fh - 1026) <= 1); // too tall
	fw = 1900, fh = 1000;
	dxr_wl_fit_output(&fw, &fh, 3840, 2160, 2.0); // physical layout, scale 2: the same logical limit
	CHECK(abs(fw - 1824) <= 1 && abs(fh - 960) <= 1);
	fw = 1900, fh = 1000;
	dxr_wl_fit_output(&fw, &fh, 0, 0, 1.0); // no output: unchanged
	CHECK(fw == 1900 && fh == 1000);

	// The size the window has on the panel once moved there. The harness layout: a
	// laptop at 133 % (scale x 120 = 160), the panel 3840x2160 at 200 % (240), stage
	// 1920x1080 at (1440,0), logical layout. The window opened on the laptop at
	// 600x480 logical px, 800x640 device px (its baseline).
	int sw = 0, sh = 0;
	dxr_wl_size_after_move(1, 600, 480, 800, 640, 240, 160, 0, 1920, 1080, &sw, &sh);
	CHECK(sw == 600 && sh == 480); // a move that keeps the opening size: the logical size
	dxr_wl_size_after_move(0, 600, 480, 800, 640, 240, 160, 0, 1920, 1080, &sw, &sh);
	CHECK(sw == 400 && sh == 320); // a later move keeps the device size
	dxr_wl_size_after_move(0, 600, 480, 1200, 960, 240, 160, 0, 1920, 1080, &sw, &sh);
	CHECK(sw == 600 && sh == 480); // the app asked for 1200x960 device px
	dxr_wl_size_after_move(0, 600, 480, 1200, 960, 240, 240, 0, 1920, 1080, &sw, &sh);
	CHECK(sw == 600 && sh == 480); // on the panel's scale already
	dxr_wl_size_after_move(0, 600, 480, 0, 0, 240, 160, 0, 1920, 1080, &sw, &sh);
	CHECK(sw == 600 && sh == 480); // no device size kept yet: the logical size
	// Too big for the panel: fitted, as the player is asked for the size anew there ...
	dxr_wl_size_after_move(1, 1900, 1000, 2533, 1333, 240, 160, 0, 1920, 1080, &sw, &sh);
	CHECK(abs(sw - 1824) <= 1 && abs(sh - 960) <= 1);
	// ... but not when its scale does not change (no new size is asked for).
	dxr_wl_size_after_move(1, 1900, 1000, 3800, 2000, 240, 240, 0, 1920, 1080, &sw, &sh);
	CHECK(sw == 1900 && sh == 1000);
	// Physical layout (stage px = device px), the panel at scale 2, the window from a
	// scale-1 monitor: 600x480 logical, 600x480 device.
	dxr_wl_size_after_move(1, 600, 480, 600, 480, 240, 120, 1, 3840, 2160, &sw, &sh);
	CHECK(sw == 1200 && sh == 960);
	dxr_wl_size_after_move(0, 600, 480, 600, 480, 240, 120, 1, 3840, 2160, &sw, &sh);
	CHECK(sw == 600 && sh == 480);
	// The panel's scale unknown: taken as 1.
	dxr_wl_size_after_move(0, 600, 480, 800, 640, 0, 160, 0, 1920, 1080, &sw, &sh);
	CHECK(sw == 800 && sh == 640);

	// A saved position near the panel's edge, judged on that size (#361): stage
	// (3110,40), X root (6220,80) with XWayland scale 2. At the device-kept 400x320
	// its centre (3310) is on the panel; at the 600x480 the window actually gets
	// there, its centre (3410) is past the panel's right edge (3360): stale.
	const DxrWlRect4 panel200 = {1440, 0, 1920, 1080};
	dxr_wl_size_after_move(1, 600, 480, 800, 640, 240, 160, 0, 1920, 1080, &sw, &sh);
	CHECK(dxr_wl_saved_position_on_panel(panel200, 6220, 80, 2, 400, 320));
	CHECK(!dxr_wl_saved_position_on_panel(panel200, 6220, 80, 2, sw, sh));
	// And at the left edge the other way round: stage (1190,40), centre 1390 (off) at
	// 400x320, 1490 (on) at 600x480.
	CHECK(!dxr_wl_saved_position_on_panel(panel200, 2380, 80, 2, 400, 320));
	CHECK(dxr_wl_saved_position_on_panel(panel200, 2380, 80, 2, sw, sh));
	// ... and that window, honoured there, is sized for the panel, the output under
	// its centre (where mutter puts it), not for the laptop under its top-left. The
	// output is judged at the size the window will have on each output (here its
	// logical size everywhere: a move that keeps it, in logical layout).
	const DxrWlRect4 outs[3] = {{0, 0, 1440, 900}, {0, 0, 0, 0}, {1440, 0, 1920, 1080}}; // laptop, a free slot, panel
	const unsigned sc[3] = {160, 0, 240};
	const int w600[3] = {600, 0, 600}, h480[3] = {480, 0, 480};
	CHECK(dxr_wl_output_for_window(outs, w600, h480, sc, 3, 1190, 40) == 2);
	// Device-kept, it would be 400x320 on the panel: its centre would stay on the laptop.
	const int wkept[3] = {600, 0, 400}, hkept[3] = {480, 0, 320};
	CHECK(dxr_wl_output_for_window(outs, wkept, hkept, sc, 3, 1190, 40) == 0);
	const int w0[3] = {0, 0, 0};
	CHECK(dxr_wl_output_for_window(outs, w0, w0, sc, 3, 1190, 40) == 0);     // no size: the top-left decides
	CHECK(dxr_wl_output_for_window(outs, w600, h480, sc, 3, 2100, 300) == 2); // centred on the panel
	// Its centre past the desktop's right edge: the output under its top-left.
	CHECK(dxr_wl_output_for_window(outs, w600, h480, sc, 3, 3110, 40) == 2);
	// Off the desktop altogether.
	CHECK(dxr_wl_output_for_window(outs, w600, h480, sc, 3, -900, -900) == -1);
	CHECK(dxr_wl_output_for_window(outs, w600, h480, sc, 0, 100, 100) == -1);

	// mutter's physical layout (stage px = device px): a 100 % laptop at (0,0)
	// 1920x1080 and the 200 % panel at (1920,0) 3840x2160. The 600x480 window is
	// 600x480 stage px on the laptop and 1200x960 on the panel (its logical size
	// kept). A saved position at stage (1470,100): at the laptop's size its centre
	// (1770) is on the laptop, at the panel's (2070) on the panel, and the saved-
	// position check, judging at the panel's size, honours it as on the panel. The
	// move is sized for the panel too: of the two, the higher scale, which mutter
	// gives a window partly on the panel. (Judged at today's size, the move was sized
	// for the laptop and the window flapped between 600 and 300 px wide.)
	const DxrWlRect4 phys[2] = {{0, 0, 1920, 1080}, {1920, 0, 3840, 2160}};
	const unsigned psc[2] = {120, 240};
	const int pw[2] = {600, 1200}, ph[2] = {480, 960};
	CHECK(dxr_wl_saved_position_on_panel(phys[1], 1470, 100, 1, pw[1], ph[1]));
	CHECK(dxr_wl_output_for_window(phys, pw, ph, psc, 2, 1470, 100) == 1);
	// The same with the outputs listed the other way round.
	const DxrWlRect4 phys_r[2] = {{1920, 0, 3840, 2160}, {0, 0, 1920, 1080}};
	const unsigned psc_r[2] = {240, 120};
	const int pw_r[2] = {1200, 600}, ph_r[2] = {960, 480};
	CHECK(dxr_wl_output_for_window(phys_r, pw_r, ph_r, psc_r, 2, 1470, 100) == 0);
	// Further left (1220): on the laptop at both sizes; the check calls it stale.
	CHECK(dxr_wl_output_for_window(phys, pw, ph, psc, 2, 1220, 100) == 0);
	CHECK(!dxr_wl_saved_position_on_panel(phys[1], 1220, 100, 1, pw[1], ph[1]));
	// Further right (1720): on the panel at both sizes.
	CHECK(dxr_wl_output_for_window(phys, pw, ph, psc, 2, 1720, 100) == 1);

	// The scale the window gets where the extension reports it. Logical layout: the
	// extension's device px per stage px for its monitor. Physical layout: that figure
	// is 1 everywhere; the window gets the highest scale of the monitors it is on
	// (it flapped at 2 while 150 px of it were on the panel, its centre on the laptop).
	CHECK(dxr_wl_window_scale120(0, 2.0, 0) == 240);
	CHECK(dxr_wl_window_scale120(0, 4.0 / 3.0, 0) == 160);
	CHECK(dxr_wl_window_scale120(0, 0.0, 240) == 0); // not reported
	CHECK(dxr_wl_window_scale120(1, 1.0, 240) == 240);
	CHECK(dxr_wl_window_scale120(1, 1.0, 0) == 0);
	CHECK(dxr_wl_highest_scale_under(phys, psc, 2, 3240, 600, 1200, 1008) == 240); // centred on the panel
	CHECK(dxr_wl_highest_scale_under(phys, psc, 2, 660, 304, 600, 504) == 120);    // on the laptop
	CHECK(dxr_wl_highest_scale_under(phys, psc, 2, 1470, 100, 600, 504) == 240);   // 150 px on the panel
	CHECK(dxr_wl_highest_scale_under(phys, psc, 2, 1470, 100, 300, 264) == 120);   // shrunk off it
	CHECK(dxr_wl_highest_scale_under(phys, psc, 2, 1620, 100, 300, 264) == 120);   // touching the edge only
	CHECK(dxr_wl_highest_scale_under(phys, psc, 2, -900, -900, 300, 264) == 0);    // on none
	CHECK(dxr_wl_highest_scale_under(phys, psc, 2, 0, 0, 0, 0) == 0);              // no frame

	// #361's restore window, in the app's running time. A frame adds the time since
	// the previous one, at most the gap limit (250 ms).
	const long long ms = 1000000LL, gap = 250 * ms, run3 = 3000 * ms, wall20 = 20000 * ms;
	CHECK(dxr_wl_running_after_frame(0, 0, 5000 * ms, gap) == 0);                   // no previous frame
	CHECK(dxr_wl_running_after_frame(0, 1000 * ms, 1016 * ms, gap) == 16 * ms);     // a 60 Hz frame
	CHECK(dxr_wl_running_after_frame(100 * ms, 1000 * ms, 5500 * ms, gap) == 350 * ms); // a 4.5 s stall: 250 ms
	CHECK(dxr_wl_running_after_frame(100 * ms, 1000 * ms, 1000 * ms, gap) == 100 * ms); // no time passed
	CHECK(dxr_wl_restore_window_open(2900 * ms, 10000 * ms, run3, wall20));
	CHECK(!dxr_wl_restore_window_open(3100 * ms, 4000 * ms, run3, wall20));
	CHECK(!dxr_wl_restore_window_open(500 * ms, 21000 * ms, run3, wall20)); // paused for long: closed anyway
	// The late-restore case (#431 follow-up): the window shown, 0.5 s of frames,
	// a 4.5 s slow frame, then the app restores its saved position on its next
	// frame. 5 s on the clock (the old 3 s rule: closed), 0.75 s of running: open.
	long long run = 0, prev = 1000 * ms;
	for (long long t = 1016 * ms; t <= 1500 * ms; t += 16 * ms) {
		run = dxr_wl_running_after_frame(run, prev, t, gap);
		prev = t;
	}
	run = dxr_wl_running_after_frame(run, prev, prev + 4500 * ms, gap);
	CHECK(run < 800 * ms);
	CHECK(dxr_wl_restore_window_open(run, 5000 * ms, run3, wall20));
	// So the restore is checked against the panel and keeps the opening size, and so
	// does DisplayXRTargetDisplay's move right after it, though the restore placed the
	// window (not a start-up move by dxr_wl_is_start_move, but the window is new).
	CHECK(dxr_wl_keeps_opening_size(0, 0, dxr_wl_restore_window_open(run, 5000 * ms, run3, wall20)));
	CHECK(dxr_wl_keeps_opening_size(0, dxr_wl_is_start_move(1, 1),
	                                dxr_wl_restore_window_open(run, 5000 * ms, run3, wall20)));
	// A move the app asks for 3 s of frames later is not part of its start.
	CHECK(!dxr_wl_keeps_opening_size(0, 0, dxr_wl_restore_window_open(run + run3, 9000 * ms, run3, wall20)));

	// DISPLAYXR_PANEL_PLACEMENT.
	CHECK(!dxr_startup_placement_env_off(NULL));
	CHECK(!dxr_startup_placement_env_off(""));
	CHECK(dxr_startup_placement_env_off("0"));
	CHECK(dxr_startup_placement_env_off("off"));
	CHECK(dxr_startup_placement_env_off("OFF"));
	CHECK(dxr_startup_placement_env_off("No"));
	CHECK(dxr_startup_placement_env_off("false"));
	CHECK(!dxr_startup_placement_env_off("1"));
	CHECK(!dxr_startup_placement_env_off("on"));
	CHECK(!dxr_startup_placement_env_off("offf"));
	CHECK(!dxr_startup_placement_env_off("of"));
	CHECK(!dxr_startup_placement_env_off("00"));

	if (s_failed) {
		fprintf(stderr, "test_wayland_place: %d check(s) failed\n", s_failed);
		return 1;
	}
	printf("test_wayland_place: all checks passed\n");
	return 0;
}
