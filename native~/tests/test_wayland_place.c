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
