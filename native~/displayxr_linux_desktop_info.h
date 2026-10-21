// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Read-only desktop info for a Linux app (GNOME): the monitors, the windows (no
// titles), the pointer and the X root scale. The exports are in displayxr_exports.h
// and the D-Bus side in displayxr_linux_desktop_info.c; this header holds the records
// they fill (the layout DisplayXRNative.cs mirrors) and the pure parsing and
// arithmetic, so that is unit-tested without a desktop
// (native~/tests/test_linux_desktop_info.c).
//
// COORDINATES. Everything is in mutter's STAGE coordinates (its global desktop
// coordinates, what the DisplayXR GNOME extension reports): logical px in mutter's
// logical layout mode (fractional scaling; the default on GNOME 50), device px in
// its physical layout mode. "X root" coordinates (what an X11 client, and the
// plug-in's window-position API, speak) are the stage times the X root scale:
// 1 without XWayland scaling, N (an integer) with mutter's xwayland-native-scaling.
//
// NO TITLES. Window titles and UI text are deliberately not read: they come from
// DisplayXRDesktop (XR_DXR_desktop_control), after the user's consent. What is here
// is what the GNOME extension's read-only members give any client without consent
// (runtime spec wayland-window-geometry.md §12.4): frames, focus, pid, app id,
// monitor geometry and scale, the pointer position.

#pragma once

#include "displayxr_linux_json.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

//! One monitor (a mutter logical monitor), displayxr_linux_monitors(). 56 bytes.
typedef struct DisplayXRLinuxMonitor {
	int32_t x, y;          //  0  top-left, stage px
	int32_t width, height; //  8  stage px
	float scale;           // 16  mutter's monitor scale (1.0, 1.25, 1.333..., 2.0, ...)
	int32_t primary;       // 20  1 = the primary monitor
	uint8_t connector[32]; // 24  UTF-8, NUL-terminated ("eDP-1", "HDMI-1", ...)
} DisplayXRLinuxMonitor;

//! One window, displayxr_linux_windows(). 168 bytes. No title, by design.
typedef struct DisplayXRLinuxWindow {
	uint64_t id;           //   0  mutter's window id (extension 16+); 0 = not reported
	int32_t x, y;          //   8  frame top-left, stage px
	int32_t width, height; //  16  frame size, stage px
	int32_t pid;           //  24  0 = not known
	int32_t monitor;       //  28  index into displayxr_linux_monitors()' list; -1 = none / not known
	int32_t focused;       //  32  1 = it has the keyboard focus
	int32_t minimized;     //  36  1 = minimized (extension 15+)
	uint8_t app_id[128];   //  40  UTF-8, NUL-terminated (WM_CLASS / Wayland app id), cut at a character
} DisplayXRLinuxWindow;

#define DXR_DI_MAX_MONITORS 16

//! The monitor whose rect is (x, y, w, h), to a pixel of rounding; -1 = none.
static inline int
dxr_di_monitor_index(const DisplayXRLinuxMonitor *mons, int n, int x, int y, int w, int h)
{
	if (w <= 0 || h <= 0)
		return -1;
	for (int i = 0; i < n; i++) {
		if (abs(mons[i].x - x) <= 1 && abs(mons[i].y - y) <= 1 && abs(mons[i].width - w) <= 1 &&
		    abs(mons[i].height - h) <= 1)
			return i;
	}
	return -1;
}

//! A logical monitor's size in stage px from its monitor's current mode (device px):
//! turned by its transform (1, 3, 5, 7 = a quarter turn), then divided by its scale
//! in the logical layout mode (mutter rounds that to the nearest px). The physical
//! layout mode keeps device px.
static inline void
dxr_di_logical_size(int mode_w, int mode_h, double scale, unsigned transform, int physical_layout, int *out_w,
                    int *out_h)
{
	int w = mode_w, h = mode_h;
	if (transform & 1) {
		w = mode_h;
		h = mode_w;
	}
	if (!physical_layout && scale > 0.0) {
		w = (int)lround(w / scale);
		h = (int)lround(h / scale);
	}
	*out_w = w;
	*out_h = h;
}

//! X root px per stage px measured on one rect that is the same place in both
//! spaces (the 3D panel: the runtime reports it in X root px from XRandR, mutter in
//! stage px). The factor must scale the origin and the size alike, to a pixel of
//! rounding; it is snapped to an integer when it is one to 1 %. 0 = they do not
//! agree (another monitor layout, or not the same output).
static inline float
dxr_di_xroot_scale_from_rect(int xr_x, int xr_y, int xr_w, int xr_h, int st_x, int st_y, int st_w, int st_h)
{
	if (xr_w <= 0 || xr_h <= 0 || st_w <= 0 || st_h <= 0)
		return 0.0f;
	double k = (double)xr_w / st_w;
	if (k < 0.25 || k > 8.0)
		return 0.0f;
	double tol = k > 1.0 ? k : 1.0;
	if (fabs(xr_h - st_h * k) > tol || fabs(xr_x - st_x * k) > tol || fabs(xr_y - st_y * k) > tol)
		return 0.0f;
	double r = floor(k + 0.5);
	return (float)(fabs(k - r) < 0.01 ? r : k);
}

//! X root px per stage px from the X root window's size: the X screen covers every
//! monitor, so it is the monitors' bounding box times the factor. Same rules as
//! dxr_di_xroot_scale_from_rect.
static inline float
dxr_di_xroot_scale_from_root(int root_w, int root_h, const DisplayXRLinuxMonitor *mons, int n)
{
	if (n <= 0 || root_w <= 0 || root_h <= 0)
		return 0.0f;
	int x0 = mons[0].x, y0 = mons[0].y, x1 = mons[0].x + mons[0].width, y1 = mons[0].y + mons[0].height;
	for (int i = 1; i < n; i++) {
		if (mons[i].x < x0)
			x0 = mons[i].x;
		if (mons[i].y < y0)
			y0 = mons[i].y;
		if (mons[i].x + mons[i].width > x1)
			x1 = mons[i].x + mons[i].width;
		if (mons[i].y + mons[i].height > y1)
			y1 = mons[i].y + mons[i].height;
	}
	return dxr_di_xroot_scale_from_rect(0, 0, root_w, root_h, 0, 0, x1 - x0, y1 - y0);
}

//! An unsigned 64-bit JSON number (mutter's window ids exceed INT32_MAX); 0 when
//! the value is not a non-negative integer.
static inline uint64_t
dxr_di_json_u64(const char *v, const char *end)
{
	uint64_t r = 0;
	if (!v || v >= end || *v < '0' || *v > '9')
		return 0;
	const char *start = v;
	for (; v < end && *v >= '0' && *v <= '9'; v++)
		r = r * 10u + (uint64_t)(*v - '0');
	if (v < end && (*v == '.' || *v == 'e' || *v == 'E')) { // written as a double: read it as one
		double d = dxr_json_number(start, end, 0.0);
		return d >= 0.0 && d < 18446744073709551616.0 ? (uint64_t)d : 0;
	}
	return r;
}

//! An integer field of a JSON object (rounded); `fallback` when absent.
static inline int
dxr_di_int(const char *obj, const char *end, const char *key, int fallback)
{
	double d = dxr_json_number(dxr_json_field(obj, end, key), end, (double)fallback);
	return (int)lround(d);
}

//! The extension's JSON reply is schema version 1 (a consumer refuses a version it
//! does not understand rather than misread it).
static inline int
dxr_di_version_ok(const char *json, const char *end)
{
	return dxr_di_int(json, end, "version", -1) == 1;
}

//! One window entry of the extension's JSON, the object [obj, obj_end), into a
//! record. `mons` (n_mons) maps its monitor rect to an index.
static inline void
dxr_di_parse_window_entry(const char *q, const char *obj_end, DisplayXRLinuxWindow *w,
                          const DisplayXRLinuxMonitor *mons, int n_mons)
{
	memset(w, 0, sizeof(*w));
	w->id = dxr_di_json_u64(dxr_json_field(q, obj_end, "id"), obj_end);
	w->pid = dxr_di_int(q, obj_end, "pid", 0);
	dxr_json_string_utf8(dxr_json_field(q, obj_end, "app_id"), obj_end, (char *)w->app_id, sizeof(w->app_id));
	w->focused = dxr_json_bool(dxr_json_field(q, obj_end, "focus"), obj_end, 0);
	w->minimized = dxr_json_bool(dxr_json_field(q, obj_end, "minimized"), obj_end, 0);
	const char *f = dxr_json_field(q, obj_end, "frame");
	if (f && *f == '[') {
		// Exactly four numbers, or no frame at all.
		int v[4] = {0, 0, 0, 0};
		const char *e = f + 1;
		int k = 0;
		for (; k < 4; k++) {
			e = dxr_json_ws(e, obj_end);
			double d = dxr_json_number(e, obj_end, NAN);
			if (isnan(d))
				break;
			v[k] = (int)lround(d);
			e = dxr_json_skip(e, obj_end);
			if (!e)
				break;
			e = dxr_json_ws(e, obj_end);
			if (e >= obj_end || *e != (k < 3 ? ',' : ']'))
				break;
			e++;
		}
		if (k == 4) {
			w->x = v[0];
			w->y = v[1];
			w->width = v[2];
			w->height = v[3];
		}
	}
	w->monitor = -1;
	const char *m = dxr_json_field(q, obj_end, "monitor");
	const char *m_end = m && *m == '{' ? dxr_json_skip(m, obj_end) : NULL;
	if (m_end)
		w->monitor = dxr_di_monitor_index(mons, n_mons, dxr_di_int(m, m_end, "x", 0), dxr_di_int(m, m_end, "y", 0),
		                                  dxr_di_int(m, m_end, "w", 0), dxr_di_int(m, m_end, "h", 0));
}

//! The window list from a GetAllWindows / GetWindows reply, TOP FIRST: the extension
//! lists mutter's window actors (global.get_window_actors()), which mutter keeps in
//! stacking order, bottom first. Writes up to `max` records and returns how many
//! windows there are (more than `max` = cut), or -1 when the reply cannot be read.
//! `mons` (n_mons) maps each window's monitor rect to an index.
static inline int
dxr_di_parse_windows(const char *json, DisplayXRLinuxWindow *out, int max, const DisplayXRLinuxMonitor *mons,
                     int n_mons)
{
	if (!json)
		return -1;
	const char *end = json + strlen(json);
	if (!dxr_di_version_ok(json, end))
		return -1;
	const char *p = dxr_json_field(json, end, "windows");
	if (!p || *p != '[')
		return -1;
	// Two passes: count, then fill from the top (the last entry) down.
	int total = 0;
	const char *first = p + 1;
	for (const char *q = first;;) {
		q = dxr_json_ws(q, end);
		if (q < end && *q == ']')
			break;
		if (q >= end || *q != '{')
			return -1;
		const char *obj_end = dxr_json_skip(q, end);
		if (!obj_end)
			return -1;
		total++;
		q = dxr_json_ws(obj_end, end);
		if (q < end && *q == ',')
			q++;
		else if (q < end && *q == ']')
			break;
		else
			return -1;
	}
	int i = 0;
	for (const char *q = first; i < total;) {
		q = dxr_json_ws(q, end);
		const char *obj_end = dxr_json_skip(q, end);
		int slot = total - 1 - i; // top first
		if (slot < max)
			dxr_di_parse_window_entry(q, obj_end, &out[slot], mons, n_mons);
		i++;
		q = dxr_json_ws(obj_end, end);
		if (q < end && *q == ',')
			q++;
	}
	return total;
}

//! The app window a click at a point would meet, from a GetWindowAt reply
//! (extension version 16): 1 and *out when it is an app window ("over": "window");
//! 0 when it is not (the desktop background or a desktop window, shell UI, no
//! monitor there, the screen locked); -1 when the reply cannot be read.
static inline int
dxr_di_parse_window_at(const char *json, DisplayXRLinuxWindow *out, const DisplayXRLinuxMonitor *mons, int n_mons)
{
	if (!json)
		return -1;
	const char *end = json + strlen(json);
	if (!dxr_di_version_ok(json, end))
		return -1;
	if (dxr_json_bool(dxr_json_field(json, end, "locked"), end, 0))
		return 0;
	char over[16];
	dxr_json_string(dxr_json_field(json, end, "over"), end, over, sizeof(over));
	if (strcmp(over, "window") != 0)
		return over[0] ? 0 : -1;
	const char *w = dxr_json_field(json, end, "window");
	const char *w_end = w && *w == '{' ? dxr_json_skip(w, end) : NULL;
	if (!w_end)
		return 0;
	dxr_di_parse_window_entry(w, w_end, out, mons, n_mons);
	return 1;
}

//! Without GetWindowAt (an extension older than version 16): the first window of a
//! top-first list whose frame holds (x, y), minimized ones skipped; -1 = none. It
//! cannot see input regions (a click-through window counts) or shell UI.
static inline int
dxr_di_window_at_frames(const DisplayXRLinuxWindow *wins, int n, int x, int y)
{
	for (int i = 0; i < n; i++) {
		const DisplayXRLinuxWindow *w = &wins[i];
		if (!w->minimized && w->width > 0 && w->height > 0 && x >= w->x && x < w->x + w->width && y >= w->y &&
		    y < w->y + w->height)
			return i;
	}
	return -1;
}

//! The pointer from a GetPointer reply, stage px. 0 when there is none to give: the
//! screen is locked ({"locked": true}), or the reply cannot be read.
static inline int
dxr_di_parse_pointer(const char *json, int *out_x, int *out_y)
{
	if (!json)
		return 0;
	const char *end = json + strlen(json);
	if (!dxr_di_version_ok(json, end) || dxr_json_bool(dxr_json_field(json, end, "locked"), end, 0))
		return 0;
	const char *x = dxr_json_field(json, end, "x");
	const char *y = dxr_json_field(json, end, "y");
	if (!x || !y)
		return 0;
	*out_x = (int)lround(dxr_json_number(x, end, 0.0));
	*out_y = (int)lround(dxr_json_number(y, end, 0.0));
	return 1;
}

#ifdef __cplusplus
}
#endif
