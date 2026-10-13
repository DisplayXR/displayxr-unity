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

	if (s_failed) {
		fprintf(stderr, "test_wayland_place: %d check(s) failed\n", s_failed);
		return 1;
	}
	printf("test_wayland_place: all checks passed\n");
	return 0;
}
