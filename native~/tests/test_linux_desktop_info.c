// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Unit test for the Linux desktop info's parsing and arithmetic:
// displayxr_linux_desktop_info.h and displayxr_linux_json.h. Built and run by CTest
// on Linux (no D-Bus, no desktop).

#include "displayxr_linux_desktop_info.h"

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

static const char *
str(const uint8_t *s)
{
	return (const char *)s;
}

static DisplayXRLinuxMonitor
mon(int x, int y, int w, int h, float scale, int primary, const char *connector)
{
	DisplayXRLinuxMonitor m;
	memset(&m, 0, sizeof(m));
	m.x = x;
	m.y = y;
	m.width = w;
	m.height = h;
	m.scale = scale;
	m.primary = primary;
	snprintf((char *)m.connector, sizeof(m.connector), "%s", connector);
	return m;
}

static void
test_layout(void)
{
	// The C# mirrors (DisplayXRNative.cs) depend on these.
	CHECK(sizeof(DisplayXRLinuxMonitor) == 56);
	CHECK(sizeof(DisplayXRLinuxWindow) == 168);
	CHECK(offsetof(DisplayXRLinuxWindow, app_id) == 40);
	CHECK(offsetof(DisplayXRLinuxMonitor, connector) == 24);
}

static void
test_logical_size(void)
{
	int w, h;
	// Logical layout: device px / scale, rounded (mutter's roundf).
	dxr_di_logical_size(2560, 1440, 4.0 / 3.0, 0, 0, &w, &h);
	CHECK(w == 1920 && h == 1080);
	dxr_di_logical_size(2560, 1440, 1.3333333730697632, 0, 0, &w, &h); // as a float on the wire
	CHECK(w == 1920 && h == 1080);
	dxr_di_logical_size(3840, 2160, 2.0, 0, 0, &w, &h);
	CHECK(w == 1920 && h == 1080);
	dxr_di_logical_size(2880, 1800, 5.0 / 3.0, 0, 0, &w, &h);
	CHECK(w == 1728 && h == 1080);
	// A quarter turn swaps the sides first.
	dxr_di_logical_size(3840, 2160, 2.0, 1, 0, &w, &h);
	CHECK(w == 1080 && h == 1920);
	dxr_di_logical_size(1920, 1080, 1.0, 7, 0, &w, &h); // flipped + 270
	CHECK(w == 1080 && h == 1920);
	dxr_di_logical_size(1920, 1080, 1.0, 2, 0, &w, &h); // 180: no swap
	CHECK(w == 1920 && h == 1080);
	// Physical layout: the stage is device px whatever the scale.
	dxr_di_logical_size(3840, 2160, 2.0, 0, 1, &w, &h);
	CHECK(w == 3840 && h == 2160);
}

static void
test_xroot_scale(void)
{
	// xwayland-native-scaling at 1x + 2x: X root = stage x 2.
	CHECK(dxr_di_xroot_scale_from_rect(2560, 0, 3840, 2160, 1280, 0, 1920, 1080) == 2.0f);
	// No XWayland scaling: the same numbers.
	CHECK(dxr_di_xroot_scale_from_rect(1280, 0, 1920, 1080, 1280, 0, 1920, 1080) == 1.0f);
	// A pixel of rounding is fine (fractional scale, odd stage sizes).
	CHECK(dxr_di_xroot_scale_from_rect(3457, 1, 3456, 2160, 1728, 0, 1728, 1080) == 2.0f);
	// Size scaled but not the origin: another layout, not the same output.
	CHECK(dxr_di_xroot_scale_from_rect(0, 0, 3840, 2160, 1280, 0, 1920, 1080) == 0.0f);
	// Width and height disagree.
	CHECK(dxr_di_xroot_scale_from_rect(0, 0, 3840, 1080, 0, 0, 1920, 1080) == 0.0f);
	// Nothing to compare.
	CHECK(dxr_di_xroot_scale_from_rect(0, 0, 0, 0, 0, 0, 1920, 1080) == 0.0f);
	CHECK(dxr_di_xroot_scale_from_rect(0, 0, 1920, 1080, 0, 0, 0, 0) == 0.0f);
	// A non-integer factor that agrees everywhere is kept as it is.
	float k = dxr_di_xroot_scale_from_rect(0, 0, 2400, 1350, 0, 0, 1920, 1080);
	CHECK(k > 1.249f && k < 1.251f);

	// From the X root size: 1280x800 @1 + 1920x1080 @2 (stage 960x540) side by side.
	DisplayXRLinuxMonitor m[2] = {mon(0, 0, 1280, 800, 1.0f, 1, "Meta-0"), mon(1280, 0, 960, 540, 2.0f, 0, "Meta-1")};
	CHECK(dxr_di_xroot_scale_from_root(4480, 1600, m, 2) == 2.0f);
	CHECK(dxr_di_xroot_scale_from_root(2240, 800, m, 2) == 1.0f);
	CHECK(dxr_di_xroot_scale_from_root(4480, 800, m, 2) == 0.0f); // not a uniform scale
	CHECK(dxr_di_xroot_scale_from_root(4480, 1600, m, 0) == 0.0f);
}

static void
test_json_strings(void)
{
	char out[16];
	const char *v = "\"a\\\"b\\\\c\\/\\n\\u00e9\\ud83d\\ude00\"";
	CHECK(dxr_json_string_utf8(v, v + strlen(v), out, sizeof(out)) == 1);
	CHECK(!strcmp(out, "a\"b\\c/\n\xc3\xa9\xf0\x9f\x98\x80"));
	// Cut at a whole character: 4 bytes fit in 5 (with the NUL), the 2-byte é does not.
	const char *w = "\"abc\\u00e9xyz\"";
	char small[5];
	CHECK(dxr_json_string_utf8(w, w + strlen(w), small, sizeof(small)) == 0);
	CHECK(!strcmp(small, "abc"));
	// Raw UTF-8 passes through, and is cut whole too.
	const char *r = "\"ab\xe4\xb8\xad\"";
	char four[5];
	CHECK(dxr_json_string_utf8(r, r + strlen(r), four, sizeof(four)) == 0);
	CHECK(!strcmp(four, "ab"));
	CHECK(dxr_json_string_utf8(r, r + strlen(r), out, sizeof(out)) == 1);
	CHECK(!strcmp(out, "ab\xe4\xb8\xad"));
	// A lone surrogate becomes U+FFFD.
	const char *s = "\"\\ud800x\"";
	CHECK(dxr_json_string_utf8(s, s + strlen(s), out, sizeof(out)) == 1);
	CHECK(!strcmp(out, "\xef\xbf\xbdx"));
	// Not a string, cut short, a bad escape: "".
	const char *n = "42";
	CHECK(dxr_json_string_utf8(n, n + 2, out, sizeof(out)) == 0 && out[0] == 0);
	const char *c = "\"abc";
	CHECK(dxr_json_string_utf8(c, c + 4, out, sizeof(out)) == 0 && out[0] == 0);
	const char *b = "\"a\\qb\"";
	CHECK(dxr_json_string_utf8(b, b + strlen(b), out, sizeof(out)) == 0 && out[0] == 0);
	// 64-bit ids.
	const char *id = "3517497060,";
	CHECK(dxr_di_json_u64(id, id + strlen(id)) == 3517497060ULL);
	const char *big = "18000000000000000000}";
	CHECK(dxr_di_json_u64(big, big + strlen(big)) == 18000000000000000000ULL);
	const char *dbl = "3.5e9]";
	CHECK(dxr_di_json_u64(dbl, dbl + strlen(dbl)) == 3500000000ULL);
	const char *neg = "-1,";
	CHECK(dxr_di_json_u64(neg, neg + strlen(neg)) == 0);
}

// A GetAllWindows reply as the extension (version 17) sends it: bottom first. The
// titles are hostile on purpose: quotes, braces, brackets and key names inside a
// title must never be read as structure.
static const char *k_windows =
    "{\"version\":1,\"layout_mode\":\"logical\",\"windows\":["
    "{\"id\":3517497041,\"pid\":1201,\"app_id\":\"org.gnome.Nautilus\",\"title\":\"Home } ] {\\\"app_id\\\":"
    "\\\"fake\\\",\\\"frame\\\":[9,9,9,9]}\",\"focus\":false,\"xwayland\":false,\"frame\":[10,20,800,600],"
    "\"buffer\":[0,10,820,620],\"monitor\":{\"x\":0,\"y\":0,\"w\":1280,\"h\":800,\"scale\":1,\"device_scale\":1},"
    "\"capture_excluded\":false,\"moving\":false,\"minimized\":true,\"above\":false,\"maximized\":false,"
    "\"fullscreen\":false,\"window_type\":0},"
    "{\"id\":4294967301,\"parent_id\":3517497041,\"pid\":1202,\"app_id\":\"caf\\u00e9.app\",\"title\":\"Save "
    "\\\"changes\\\"?\",\"focus\":true,\"xwayland\":true,\"frame\":[1400,100,400,300],\"buffer\":[1400,100,400,300],"
    "\"monitor\":{\"x\":1280,\"y\":0,\"w\":960,\"h\":540,\"scale\":2,\"device_scale\":2},\"minimized\":false,"
    "\"window_type\":3},"
    "{\"id\":7,\"pid\":1203,\"app_id\":\"\",\"title\":\"\",\"focus\":false,\"frame\":[-50,-40,30,20],"
    "\"monitor\":null}"
    "]}";

static void
test_windows(void)
{
	DisplayXRLinuxMonitor m[2] = {mon(0, 0, 1280, 800, 1.0f, 1, "Meta-0"), mon(1280, 0, 960, 540, 2.0f, 0, "Meta-1")};
	DisplayXRLinuxWindow w[4];
	memset(w, 0xAB, sizeof(w));
	int n = dxr_di_parse_windows(k_windows, w, 4, m, 2);
	CHECK(n == 3);
	// Top first: the reply's last entry comes first.
	CHECK(w[0].id == 7 && w[0].pid == 1203 && w[0].monitor == -1 && w[0].app_id[0] == 0);
	CHECK(w[0].x == -50 && w[0].y == -40 && w[0].width == 30 && w[0].height == 20);
	CHECK(w[1].id == 4294967301ULL && w[1].pid == 1202 && w[1].focused == 1 && w[1].minimized == 0);
	CHECK(!strcmp(str(w[1].app_id), "caf\xc3\xa9.app"));
	CHECK(w[1].x == 1400 && w[1].y == 100 && w[1].width == 400 && w[1].height == 300 && w[1].monitor == 1);
	CHECK(w[2].id == 3517497041ULL && w[2].pid == 1201 && w[2].focused == 0 && w[2].minimized == 1);
	CHECK(!strcmp(str(w[2].app_id), "org.gnome.Nautilus")); // not the "fake" in its title
	CHECK(w[2].x == 10 && w[2].y == 20 && w[2].width == 800 && w[2].height == 600 && w[2].monitor == 0);

	// A buffer too small: the count says how many there are; the top ones are filled.
	DisplayXRLinuxWindow one[1];
	CHECK(dxr_di_parse_windows(k_windows, one, 1, m, 2) == 3);
	CHECK(one[0].id == 7);
	CHECK(dxr_di_parse_windows(k_windows, NULL, 0, m, 2) == 3);

	// Without a monitor list every index is -1.
	CHECK(dxr_di_parse_windows(k_windows, w, 4, NULL, 0) == 3);
	CHECK(w[0].monitor == -1 && w[1].monitor == -1 && w[2].monitor == -1);

	// An older extension (no id, minimized, monitor): zeros, not garbage.
	const char *old = "{\"version\":1,\"windows\":[{\"pid\":5,\"app_id\":\"x\",\"title\":\"t\",\"focus\":true,"
	                  "\"frame\":[1,2,3,4]}]}";
	CHECK(dxr_di_parse_windows(old, w, 4, m, 2) == 1);
	CHECK(w[0].id == 0 && w[0].pid == 5 && w[0].focused == 1 && w[0].minimized == 0 && w[0].monitor == -1);
	CHECK(w[0].x == 1 && w[0].y == 2 && w[0].width == 3 && w[0].height == 4);

	// A frame that is not exactly four numbers is no frame (zeros), not half of one.
	const char *shortf[] = {
	    "{\"version\":1,\"windows\":[{\"pid\":5,\"frame\":[1,2]}]}",
	    "{\"version\":1,\"windows\":[{\"pid\":5,\"frame\":[1,2,3,4,5]}]}",
	    "{\"version\":1,\"windows\":[{\"pid\":5,\"frame\":[1,\"2\",3,4]}]}",
	};
	for (size_t i = 0; i < sizeof(shortf) / sizeof(shortf[0]); i++) {
		CHECK(dxr_di_parse_windows(shortf[i], w, 4, m, 2) == 1);
		CHECK(w[0].pid == 5 && w[0].x == 0 && w[0].y == 0 && w[0].width == 0 && w[0].height == 0);
	}
	const char *spaced = "{\"version\":1,\"windows\":[{\"pid\":5,\"frame\":[ 1 , -2 ,3.4, 4 ]}]}";
	CHECK(dxr_di_parse_windows(spaced, w, 4, m, 2) == 1);
	CHECK(w[0].x == 1 && w[0].y == -2 && w[0].width == 3 && w[0].height == 4);

	// Empty, a schema it does not know, malformed, cut short.
	CHECK(dxr_di_parse_windows("{\"version\":1,\"windows\":[]}", w, 4, m, 2) == 0);
	CHECK(dxr_di_parse_windows("{\"version\":1,\"windows\":[ ]}", w, 4, m, 2) == 0);
	CHECK(dxr_di_parse_windows("{\"version\":2,\"windows\":[]}", w, 4, m, 2) == -1);
	CHECK(dxr_di_parse_windows("{\"windows\":[]}", w, 4, m, 2) == -1);
	CHECK(dxr_di_parse_windows("{\"version\":1}", w, 4, m, 2) == -1);
	CHECK(dxr_di_parse_windows("{\"version\":1,\"windows\":[{\"pid\":1}", w, 4, m, 2) == -1);
	CHECK(dxr_di_parse_windows("{\"version\":1,\"windows\":[1,2]}", w, 4, m, 2) == -1);
	CHECK(dxr_di_parse_windows("", w, 4, m, 2) == -1);
	CHECK(dxr_di_parse_windows(NULL, w, 4, m, 2) == -1);

	// The monitor index tolerates a pixel of rounding, no more.
	CHECK(dxr_di_monitor_index(m, 2, 1281, 0, 959, 541) == 1);
	CHECK(dxr_di_monitor_index(m, 2, 1283, 0, 960, 540) == -1);
	CHECK(dxr_di_monitor_index(m, 2, 0, 0, 0, 0) == -1);
}

static void
test_window_at(void)
{
	DisplayXRLinuxMonitor m[2] = {mon(0, 0, 1280, 800, 1.0f, 1, "Meta-0"), mon(1280, 0, 960, 540, 2.0f, 0, "Meta-1")};
	DisplayXRLinuxWindow w;
	// A window (its entry, with a hostile title and a popup's parent_id).
	const char *hit = "{\"version\":1,\"x\":1500,\"y\":200,\"layout_mode\":\"logical\",\"over\":\"window\","
	                  "\"window\":{\"id\":4294967302,\"parent_id\":4294967301,\"pid\":102,\"app_id\":\"firefox\","
	                  "\"title\":\"over\\\":\\\"desktop\",\"focus\":false,\"frame\":[1400,100,300,200],"
	                  "\"monitor\":{\"x\":1280,\"y\":0,\"w\":960,\"h\":540,\"scale\":2},\"minimized\":false},"
	                  "\"window_type\":10}";
	memset(&w, 0xAB, sizeof(w));
	CHECK(dxr_di_parse_window_at(hit, &w, m, 2) == 1);
	CHECK(w.id == 4294967302ULL && w.pid == 102 && !strcmp(str(w.app_id), "firefox") && w.monitor == 1);
	CHECK(w.x == 1400 && w.y == 100 && w.width == 300 && w.height == 200 && w.focused == 0);
	// Not an app window: the desktop (also a desktop window such as DING), shell UI,
	// outside every monitor, the lock screen.
	CHECK(dxr_di_parse_window_at("{\"version\":1,\"x\":5,\"y\":5,\"over\":\"desktop\",\"window\":null}", &w, m,
	                             2) == 0);
	CHECK(dxr_di_parse_window_at("{\"version\":1,\"x\":5,\"y\":5,\"over\":\"desktop\",\"window\":{\"id\":9,"
	                             "\"pid\":3,\"app_id\":\"com.rastersoft.ding\",\"frame\":[0,0,1280,800]}}",
	                             &w, m, 2) == 0);
	CHECK(dxr_di_parse_window_at("{\"version\":1,\"x\":5,\"y\":5,\"over\":\"shell\",\"window\":null}", &w, m,
	                             2) == 0);
	CHECK(dxr_di_parse_window_at("{\"version\":1,\"x\":-5,\"y\":5,\"over\":\"outside\"}", &w, m, 2) == 0);
	CHECK(dxr_di_parse_window_at("{\"version\":1,\"locked\":true}", &w, m, 2) == 0);
	// Cannot be read.
	CHECK(dxr_di_parse_window_at("{\"version\":2,\"over\":\"window\",\"window\":{}}", &w, m, 2) == -1);
	CHECK(dxr_di_parse_window_at("{\"version\":1,\"x\":5}", &w, m, 2) == -1);
	CHECK(dxr_di_parse_window_at(NULL, &w, m, 2) == -1);
	CHECK(dxr_di_parse_window_at("{\"version\":1,\"over\":\"window\",\"window\":null}", &w, m, 2) == 0);

	// The fallback without GetWindowAt: the first (topmost) frame holding the point.
	DisplayXRLinuxWindow list[8];
	// k_windows, top first: [0] id 7 at (-50,-40) 30x20; [1] the dialog at (1400,100)
	// 400x300; [2] Nautilus at (10,20) 800x600, minimized.
	CHECK(dxr_di_parse_windows(k_windows, list, 8, m, 2) == 3);
	CHECK(dxr_di_window_at_frames(list, 3, 1500, 200) == 1);
	CHECK(dxr_di_window_at_frames(list, 3, 100, 100) == -1);  // only Nautilus holds it, and it is minimized
	CHECK(dxr_di_window_at_frames(list, 3, -40, -30) == 0);
	CHECK(dxr_di_window_at_frames(list, 3, -20, -30) == -1);  // the right edge is exclusive
	CHECK(dxr_di_window_at_frames(list, 3, 1799, 399) == 1);  // the bottom-right pixel is inside
	CHECK(dxr_di_window_at_frames(list, 3, 1800, 200) == -1);
	CHECK(dxr_di_window_at_frames(list, 0, 1500, 200) == -1);
	list[2].minimized = 0;
	CHECK(dxr_di_window_at_frames(list, 3, 100, 100) == 2);
	// Overlap: the topmost (lowest index) wins.
	list[0].x = 1450;
	list[0].y = 150;
	list[0].width = 100;
	list[0].height = 100;
	CHECK(dxr_di_window_at_frames(list, 3, 1500, 200) == 0);
	CHECK(dxr_di_window_at_frames(list, 3, 1600, 200) == 1);
}

static void
test_pointer(void)
{
	int x = -1, y = -1;
	CHECK(dxr_di_parse_pointer("{\"version\":1,\"x\":1180,\"y\":640,\"layout_mode\":\"logical\",\"over\":\"window\","
	                           "\"window\":{\"id\":1,\"x\":5,\"title\":\"x\\\":9\"},\"window_type\":0}",
	                           &x, &y) == 1);
	CHECK(x == 1180 && y == 640); // the top-level x / y, not the window's
	CHECK(dxr_di_parse_pointer("{\"version\":1,\"x\":-3,\"y\":0,\"over\":\"outside\",\"window\":null}", &x, &y) == 1);
	CHECK(x == -3 && y == 0);
	CHECK(dxr_di_parse_pointer("{\"version\":1,\"locked\":true}", &x, &y) == 0);
	CHECK(dxr_di_parse_pointer("{\"version\":2,\"x\":1,\"y\":2}", &x, &y) == 0);
	CHECK(dxr_di_parse_pointer("{\"version\":1,\"x\":1}", &x, &y) == 0);
	CHECK(dxr_di_parse_pointer(NULL, &x, &y) == 0);
}

int
main(void)
{
	test_layout();
	test_logical_size();
	test_xroot_scale();
	test_json_strings();
	test_windows();
	test_window_at();
	test_pointer();
	if (s_failed) {
		fprintf(stderr, "test_linux_desktop_info: %d check(s) failed\n", s_failed);
		return 1;
	}
	printf("test_linux_desktop_info: all checks passed\n");
	return 0;
}
