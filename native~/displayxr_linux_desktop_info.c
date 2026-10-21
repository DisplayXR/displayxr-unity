// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Read-only desktop info for a Linux app on GNOME: displayxr_linux_monitors,
// displayxr_linux_windows, displayxr_linux_pointer, displayxr_linux_xroot_scale.
// What is reported, in which coordinates, and why there are no titles:
// displayxr_linux_desktop_info.h.
//
// WHERE IT COMES FROM
//  - Monitors: mutter's org.gnome.Mutter.DisplayConfig.GetCurrentState, its logical
//    monitors (position, scale, primary, connectors; the size from the current mode
//    and the layout mode). The DisplayXR GNOME extension has no monitor list.
//  - Windows and the pointer: the DisplayXR GNOME extension's read-only members
//    (org.displayxr.WindowGeometry1: GetAllWindows, version 16, else GetWindows;
//    GetPointer, version 16).
//  - The X root scale: the 3D panel's rect as the runtime reported it in X root px
//    (set by the XR session) against the same output's stage rect — the comparison
//    the native-Wayland player's window-position API converts with
//    (displayxr_linux_wayland.c, x11_scale_locked). Without it, on an X11 player, the
//    X root window's size against the monitors' bounding box.
//
// It lives in the main plug-in, on its own private session-bus connection with
// libdbus dlopened (displayxr_linux_dbus.h), so a native-Wayland and an X11
// (XWayland) player get the same answers; nothing here needs the Wayland library.
//
// THREADS. Any thread. One mutex covers the connection and the caches (the XR
// session's panel rect has its own, so session start never waits for a refresh). A
// result is kept for a short time (windows and the pointer 100 ms, monitors and the
// X root scale 250 ms), so per-frame polling mostly reads the cache. The call that
// refreshes waits for a round trip to gnome-shell (about a millisecond; a windows
// refresh may also refresh the monitors), and each D-Bus call gives up after
// DXR_DI_TIMEOUT_MS. A failed call is not retried for a second, and a call that
// timed out stops every call for a second, so a missing extension or a stalled shell
// costs at most one wait per second. Meanwhile windows and the pointer report
// nothing; the monitor list keeps its last answer.

#define _GNU_SOURCE
#include "displayxr_linux_desktop_info.h"
#include "displayxr_linux_dbus.h"
#include "displayxr_linux_wayland.h" // dxr_wl_forced_wayland
#include "displayxr_exports.h"

#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

extern void dxr_prov_file_log(const char *s);
//! The X root window's size (displayxr_linux.c). 1 when known.
int displayxr_linux_x_root_size(int *out_w, int *out_h);

#define DXR_DI_TIMEOUT_MS 250
#define DXR_DI_MS 1000000LL
#define DXR_DI_TTL_FAST (100 * DXR_DI_MS) // windows, pointer
#define DXR_DI_TTL_SLOW (250 * DXR_DI_MS) // monitors, X root scale
#define DXR_DI_RETRY (1000 * DXR_DI_MS)   // after a failed call
#define DXR_DI_BUS_RETRY (5000 * DXR_DI_MS)

#define DXR_EXT_NAME "org.displayxr.WindowGeometry"
#define DXR_EXT_PATH "/org/displayxr/WindowGeometry"
#define DXR_EXT_IFACE "org.displayxr.WindowGeometry1"
#define DXR_DC_NAME "org.gnome.Mutter.DisplayConfig"
#define DXR_DC_PATH "/org/gnome/Mutter/DisplayConfig"

//! How a call went.
enum call_error {
	CALL_OK = 0,
	CALL_NO_SERVICE,    //!< nobody owns the name (the extension disabled, another desktop)
	CALL_NO_METHOD,     //!< the service lacks the method (an older extension)
	CALL_TIMEOUT,       //!< no answer within DXR_DI_TIMEOUT_MS
	CALL_FAILED,        //!< anything else (no bus, a malformed reply, ...)
};

static pthread_mutex_t s_mutex = PTHREAD_MUTEX_INITIALIZER;
static const DxrDBus *s_db;
static DxrDBusConnection *s_conn;
static long long s_conn_retry_at; // no connection until then
static long long s_stall_until;   // a call timed out: gnome-shell is not answering, ask nothing until then

static struct {
	long long until;
	int ok, n;
	DisplayXRLinuxMonitor m[DXR_DI_MAX_MONITORS];
} s_mon;

static struct {
	long long until;
	int ok, n, cap;
	DisplayXRLinuxWindow *w;
} s_win;

static struct {
	long long until;
	int ok, x, y;
} s_ptr;

static struct {
	long long until;
	float k;
	unsigned panel_gen; // the panel rect k was computed with
	int logged;         // k was logged at least once
	float k_log;        // the last k logged
} s_xroot;

// The 3D panel's rect in X root px, as the runtime reported it (XrDisplayDesktopInfoDXR).
// Its own lock: the session thread sets it while an app thread may be in a refresh.
typedef struct {
	int valid;
	char connector[160];
	int x, y, w, h;
	unsigned gen; // bumps on every set: the X root scale is computed again
} DiPanel;
static pthread_mutex_t s_panel_mutex = PTHREAD_MUTEX_INITIALIZER;
static DiPanel s_panel;

// Log-once state: what the service did last time (-1 = not asked yet).
static int s_ext_state = -1;  // 1 answered, 0 not on the bus / failing
static int s_dc_state = -1;   // DisplayConfig, same
static int s_ext_no_all;      // GetAllWindows unknown: the extension predates version 16
static int s_ext_no_pointer;  // GetPointer unknown, logged
static int s_bus_logged;

static long long
now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void
di_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void
di_log(const char *fmt, ...)
{
	char m[512];
	int n = snprintf(m, sizeof(m), "[DisplayXR-LNX] desktop info: ");
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(m + n, sizeof(m) - (size_t)n, fmt, ap);
	va_end(ap);
	fputs(m, stderr);
	dxr_prov_file_log(m);
}

static DxrDBusConnection *
conn_locked(long long now)
{
	if (s_conn && !s_db->get_is_connected(s_conn)) {
		s_db->close(s_conn);
		s_db->connection_unref(s_conn);
		s_conn = NULL;
		s_conn_retry_at = now + DXR_DI_RETRY;
		di_log("the session bus connection was lost\n");
	}
	if (s_conn || now < s_conn_retry_at)
		return s_conn;
	if (!s_db) {
		const char *why = NULL;
		s_db = dxr_dbus_load(&why);
		if (!s_db) {
			di_log("%s: no monitors, windows or pointer\n", why ? why : "libdbus-1 not loaded");
			s_conn_retry_at = LLONG_MAX; // never: the library will not appear
			return NULL;
		}
	}
	DxrDBusError err;
	s_db->error_init(&err);
	s_conn = s_db->bus_get_private(DXR_DBUS_BUS_SESSION, &err);
	if (!s_conn) {
		if (!s_bus_logged) {
			char why[160];
			snprintf(why, sizeof(why), "%s", err.message ? err.message : "?");
			for (size_t n = strlen(why); n > 0 && (why[n - 1] == '\n' || why[n - 1] == ' '); n--)
				why[n - 1] = 0;
			di_log("session bus unavailable (%s): no monitors, windows or pointer\n", why);
		}
		s_bus_logged = 1;
		s_conn_retry_at = now + DXR_DI_BUS_RETRY;
	} else {
		// A bus connection _exit()s the process when the bus goes away, by default.
		s_db->set_exit_on_disconnect(s_conn, 0);
	}
	s_db->error_free(&err);
	return s_conn;
}

static enum call_error
error_kind(const DxrDBusError *err)
{
	const char *n = err->name ? err->name : "";
	if (!strcmp(n, "org.freedesktop.DBus.Error.ServiceUnknown") ||
	    !strcmp(n, "org.freedesktop.DBus.Error.NameHasNoOwner"))
		return CALL_NO_SERVICE;
	if (!strcmp(n, "org.freedesktop.DBus.Error.UnknownMethod") ||
	    !strcmp(n, "org.freedesktop.DBus.Error.UnknownObject") ||
	    !strcmp(n, "org.freedesktop.DBus.Error.UnknownInterface"))
		return CALL_NO_METHOD;
	if (!strcmp(n, "org.freedesktop.DBus.Error.NoReply") || !strcmp(n, "org.freedesktop.DBus.Error.Timeout"))
		return CALL_TIMEOUT;
	return CALL_FAILED;
}

//! A method call with no arguments; the reply (unref it) or NULL with *out_err set.
static DxrDBusMessage *
call_locked(long long now, const char *dest, const char *path, const char *iface, const char *method,
            enum call_error *out_err)
{
	*out_err = CALL_FAILED;
	DxrDBusConnection *c = conn_locked(now);
	if (!c)
		return NULL;
	if (now < s_stall_until) {
		*out_err = CALL_TIMEOUT; // one wait per refresh at most, not one per call
		return NULL;
	}
	DxrDBusMessage *msg = s_db->new_method_call(dest, path, iface, method);
	if (!msg)
		return NULL;
	DxrDBusError err;
	s_db->error_init(&err);
	DxrDBusMessage *reply = s_db->send_with_reply_and_block(c, msg, DXR_DI_TIMEOUT_MS, &err);
	s_db->unref(msg);
	*out_err = reply ? CALL_OK : error_kind(&err);
	s_db->error_free(&err);
	if (*out_err == CALL_TIMEOUT)
		s_stall_until = now_ns() + DXR_DI_RETRY;
	// Drop whatever else arrived (the bus's NameAcquired): nothing reads this
	// connection's queue but these calls.
	while (s_db->dispatch(c) == DXR_DBUS_DISPATCH_DATA_REMAINS) {
	}
	return reply;
}

//! A call on the extension returning its JSON string: a malloc'd copy, or NULL.
static char *
ext_json_locked(long long now, const char *method, enum call_error *out_err)
{
	DxrDBusMessage *reply = call_locked(now, DXR_EXT_NAME, DXR_EXT_PATH, DXR_EXT_IFACE, method, out_err);
	if (!reply)
		return NULL;
	char *copy = NULL;
	DxrDBusError err;
	s_db->error_init(&err);
	const char *json = NULL;
	if (s_db->get_args(reply, &err, DXR_DBUS_TYPE_STRING, &json, DXR_DBUS_TYPE_INVALID) && json)
		copy = strdup(json);
	else
		*out_err = CALL_FAILED;
	s_db->error_free(&err);
	s_db->unref(reply);
	return copy;
}

//! Log when the extension appears or goes away (once per change, not per call).
static void
ext_note_locked(enum call_error e)
{
	if (!s_conn)
		return; // no bus at all: conn_locked() said so
	int state = e == CALL_OK || e == CALL_NO_METHOD;
	if (state == s_ext_state)
		return;
	int was = s_ext_state;
	s_ext_state = state;
	if (state) {
		di_log("the DisplayXR GNOME extension answers\n");
		if (was == 0) { // back after it was away: it may have been updated meanwhile
			s_ext_no_all = 0;
			s_ext_no_pointer = 0;
		}
	} else if (e == CALL_TIMEOUT) {
		di_log("the DisplayXR GNOME extension did not answer within %d ms: no windows or pointer for now\n",
		       DXR_DI_TIMEOUT_MS);
	} else {
		di_log("the DisplayXR GNOME extension is not on the session bus (not installed, disabled, or not a "
		       "GNOME session): no windows or pointer\n");
	}
}

/*
 *
 * Monitors: org.gnome.Mutter.DisplayConfig.GetCurrentState
 *   (u serial, a((ssss)a(siiddada{sv})a{sv}) monitors,
 *    a(iiduba(ssss)a{sv}) logical_monitors, a{sv} properties)
 *
 */

static int
it_type(DxrDBusMessageIter *it)
{
	return s_db->iter_get_arg_type(it);
}

//! The value of `key` in the a{sv} at `dict` (positioned on the array), if it has
//! type `type`; 1 when found.
static int
dict_get(DxrDBusMessageIter *dict, const char *key, int type, void *out)
{
	if (it_type(dict) != DXR_DBUS_TYPE_ARRAY)
		return 0;
	DxrDBusMessageIter entries;
	s_db->iter_recurse(dict, &entries);
	while (it_type(&entries) == DXR_DBUS_TYPE_DICT_ENTRY) {
		DxrDBusMessageIter entry, var;
		s_db->iter_recurse(&entries, &entry);
		const char *k = NULL;
		if (it_type(&entry) == DXR_DBUS_TYPE_STRING) {
			s_db->iter_get_basic(&entry, (void *)&k);
			if (k && !strcmp(k, key) && s_db->iter_next(&entry) && it_type(&entry) == DXR_DBUS_TYPE_VARIANT) {
				s_db->iter_recurse(&entry, &var);
				if (it_type(&var) == type) {
					s_db->iter_get_basic(&var, out);
					return 1;
				}
				return 0;
			}
		}
		if (!s_db->iter_next(&entries))
			break;
	}
	return 0;
}

//! The first string of the (ssss) struct at `spec` (a monitor spec: its connector).
static void
spec_connector(DxrDBusMessageIter *spec, char *out, size_t size)
{
	out[0] = 0;
	if (it_type(spec) != DXR_DBUS_TYPE_STRUCT)
		return;
	DxrDBusMessageIter f;
	s_db->iter_recurse(spec, &f);
	const char *s = NULL;
	if (it_type(&f) == DXR_DBUS_TYPE_STRING) {
		s_db->iter_get_basic(&f, (void *)&s);
		snprintf(out, size, "%s", s ? s : "");
	}
}

//! Advance `it` past one field of the expected type; 0 if it is not there.
static int
expect_next(DxrDBusMessageIter *it, int type)
{
	return it_type(it) == type && s_db->iter_next(it);
}

//! GetCurrentState into monitor records; their count, or -1 on a reply it cannot read.
static int
parse_display_config(DxrDBusMessage *reply, DisplayXRLinuxMonitor *out, int max)
{
	struct {
		char connector[32];
		int w, h;
	} modes[DXR_DI_MAX_MONITORS * 2];
	int n_modes = 0;
	struct {
		int x, y;
		double scale;
		uint32_t transform, primary;
		char connector[32];
	} lms[DXR_DI_MAX_MONITORS];
	int n_lms = 0;

	DxrDBusMessageIter top;
	if (!s_db->iter_init(reply, &top) || !expect_next(&top, DXR_DBUS_TYPE_UINT32))
		return -1;

	// Monitors: each one's current mode, by connector.
	if (it_type(&top) != DXR_DBUS_TYPE_ARRAY)
		return -1;
	DxrDBusMessageIter mons;
	s_db->iter_recurse(&top, &mons);
	while (it_type(&mons) == DXR_DBUS_TYPE_STRUCT) {
		DxrDBusMessageIter mon;
		s_db->iter_recurse(&mons, &mon);
		char conn[32];
		spec_connector(&mon, conn, sizeof(conn));
		if (s_db->iter_next(&mon) && it_type(&mon) == DXR_DBUS_TYPE_ARRAY) {
			DxrDBusMessageIter list;
			s_db->iter_recurse(&mon, &list);
			while (it_type(&list) == DXR_DBUS_TYPE_STRUCT) {
				DxrDBusMessageIter mode;
				s_db->iter_recurse(&list, &mode);
				int32_t w = 0, h = 0;
				uint32_t current = 0;
				if (expect_next(&mode, DXR_DBUS_TYPE_STRING) && it_type(&mode) == DXR_DBUS_TYPE_INT32) {
					s_db->iter_get_basic(&mode, &w);
					s_db->iter_next(&mode);
					if (it_type(&mode) == DXR_DBUS_TYPE_INT32) {
						s_db->iter_get_basic(&mode, &h);
						// refresh (d), preferred scale (d), supported scales (ad), then the properties
						if (s_db->iter_next(&mode) && expect_next(&mode, DXR_DBUS_TYPE_DOUBLE) &&
						    expect_next(&mode, DXR_DBUS_TYPE_DOUBLE) && expect_next(&mode, DXR_DBUS_TYPE_ARRAY))
							dict_get(&mode, "is-current", DXR_DBUS_TYPE_BOOLEAN, &current);
					}
				}
				if (current && n_modes < (int)(sizeof(modes) / sizeof(modes[0]))) {
					snprintf(modes[n_modes].connector, sizeof(modes[n_modes].connector), "%s", conn);
					modes[n_modes].w = w;
					modes[n_modes].h = h;
					n_modes++;
				}
				if (!s_db->iter_next(&list))
					break;
			}
		}
		if (!s_db->iter_next(&mons))
			break;
	}
	if (!s_db->iter_next(&top) || it_type(&top) != DXR_DBUS_TYPE_ARRAY)
		return -1;

	// Logical monitors.
	DxrDBusMessageIter lmi;
	s_db->iter_recurse(&top, &lmi);
	while (it_type(&lmi) == DXR_DBUS_TYPE_STRUCT && n_lms < DXR_DI_MAX_MONITORS) {
		DxrDBusMessageIter f;
		s_db->iter_recurse(&lmi, &f);
		int32_t x = 0, y = 0;
		double scale = 1.0;
		uint32_t transform = 0, primary = 0;
		int ok = 0;
		if (it_type(&f) == DXR_DBUS_TYPE_INT32) {
			s_db->iter_get_basic(&f, &x);
			s_db->iter_next(&f);
		}
		if (it_type(&f) == DXR_DBUS_TYPE_INT32) {
			s_db->iter_get_basic(&f, &y);
			s_db->iter_next(&f);
			if (it_type(&f) == DXR_DBUS_TYPE_DOUBLE) {
				s_db->iter_get_basic(&f, &scale);
				s_db->iter_next(&f);
				if (it_type(&f) == DXR_DBUS_TYPE_UINT32) {
					s_db->iter_get_basic(&f, &transform);
					s_db->iter_next(&f);
					if (it_type(&f) == DXR_DBUS_TYPE_BOOLEAN) {
						s_db->iter_get_basic(&f, &primary);
						s_db->iter_next(&f);
						ok = it_type(&f) == DXR_DBUS_TYPE_ARRAY;
					}
				}
			}
		}
		if (ok) {
			DxrDBusMessageIter specs;
			s_db->iter_recurse(&f, &specs);
			lms[n_lms].x = x;
			lms[n_lms].y = y;
			lms[n_lms].scale = scale;
			lms[n_lms].transform = transform;
			lms[n_lms].primary = primary;
			spec_connector(&specs, lms[n_lms].connector, sizeof(lms[n_lms].connector)); // the first: mirrors share the rect
			n_lms++;
		}
		if (!s_db->iter_next(&lmi))
			break;
	}

	// Properties: the layout mode (1 logical, 2 physical). Absent: logical, which
	// is also what every monitor at scale 1 amounts to.
	uint32_t layout = 1;
	if (s_db->iter_next(&top))
		dict_get(&top, "layout-mode", DXR_DBUS_TYPE_UINT32, &layout);

	int n = 0;
	for (int i = 0; i < n_lms && n < max; i++) {
		int mw = 0, mh = 0;
		for (int k = 0; k < n_modes; k++) {
			if (!strcmp(modes[k].connector, lms[i].connector)) {
				mw = modes[k].w;
				mh = modes[k].h;
				break;
			}
		}
		if (mw <= 0 || mh <= 0)
			continue; // no current mode: nothing shows there
		DisplayXRLinuxMonitor *m = &out[n++];
		memset(m, 0, sizeof(*m));
		m->x = lms[i].x;
		m->y = lms[i].y;
		dxr_di_logical_size(mw, mh, lms[i].scale, lms[i].transform, layout == 2, &m->width, &m->height);
		m->scale = (float)lms[i].scale;
		m->primary = lms[i].primary ? 1 : 0;
		snprintf((char *)m->connector, sizeof(m->connector), "%s", lms[i].connector);
	}
	return n;
}

static int
monitors_locked(long long now)
{
	if (now < s_mon.until)
		return s_mon.ok;
	enum call_error e;
	DxrDBusMessage *reply = call_locked(now, DXR_DC_NAME, DXR_DC_PATH, DXR_DC_NAME, "GetCurrentState", &e);
	if (!reply && e == CALL_TIMEOUT && s_mon.ok) {
		// gnome-shell is not answering right now: monitors rarely change, so keep
		// the last list rather than report none, and ask again in a second.
		s_mon.until = now + DXR_DI_RETRY;
		return 1;
	}
	DisplayXRLinuxMonitor fresh[DXR_DI_MAX_MONITORS];
	int n = reply ? parse_display_config(reply, fresh, DXR_DI_MAX_MONITORS) : -1;
	if (reply)
		s_db->unref(reply);
	if (n != (s_mon.ok ? s_mon.n : -1) || (n > 0 && memcmp(fresh, s_mon.m, sizeof(fresh[0]) * (size_t)n)))
		s_win.until = 0; // the cached windows' monitor indexes refer to the old list
	if (n > 0)
		memcpy(s_mon.m, fresh, sizeof(fresh[0]) * (size_t)n);
	s_mon.ok = n >= 0;
	s_mon.n = n > 0 ? n : 0;
	s_mon.until = now + (s_mon.ok ? DXR_DI_TTL_SLOW : DXR_DI_RETRY);
	if (s_conn && s_mon.ok != s_dc_state) { // (no bus at all: conn_locked() said so)
		s_dc_state = s_mon.ok;
		if (s_mon.ok)
			di_log("mutter's DisplayConfig answers: %d monitor(s)\n", s_mon.n);
		else if (reply)
			di_log("mutter's DisplayConfig sent a reply this cannot read: no monitor list\n");
		else
			di_log("mutter's DisplayConfig is not available (%s): no monitor list\n",
			       e == CALL_TIMEOUT ? "no answer in time" : "not a GNOME session?");
	}
	return s_mon.ok;
}

static int
windows_locked(long long now)
{
	if (now < s_win.until)
		return s_win.ok;
	monitors_locked(now); // for each window's monitor index
	enum call_error e = CALL_FAILED;
	char *json = NULL;
	if (!s_ext_no_all) {
		json = ext_json_locked(now, "GetAllWindows", &e);
		if (!json && e == CALL_NO_METHOD)
			s_ext_no_all = 1; // version < 16: normal windows only
	}
	if (!json && s_ext_no_all)
		json = ext_json_locked(now, "GetWindows", &e);
	ext_note_locked(e);
	int n = -1;
	if (json) {
		n = dxr_di_parse_windows(json, s_win.w, s_win.cap, s_mon.m, s_mon.ok ? s_mon.n : 0);
		if (n > s_win.cap) {
			DisplayXRLinuxWindow *grown = (DisplayXRLinuxWindow *)realloc(s_win.w, sizeof(*grown) * (size_t)n);
			if (grown) {
				s_win.w = grown;
				s_win.cap = n;
				n = dxr_di_parse_windows(json, s_win.w, s_win.cap, s_mon.m, s_mon.ok ? s_mon.n : 0);
			} else {
				n = s_win.cap; // out of memory: keep what fitted
			}
		}
		static int s_bad_logged;
		if (n < 0 && !s_bad_logged) {
			s_bad_logged = 1;
			di_log("the DisplayXR GNOME extension's window list could not be read (a schema version other "
			       "than 1?): no windows\n");
		}
		free(json);
	}
	s_win.ok = n >= 0;
	s_win.n = n > 0 ? n : 0;
	s_win.until = now + (s_win.ok ? DXR_DI_TTL_FAST : DXR_DI_RETRY);
	return s_win.ok;
}

static int
pointer_locked(long long now)
{
	if (now < s_ptr.until)
		return s_ptr.ok;
	enum call_error e = CALL_FAILED;
	char *json = ext_json_locked(now, "GetPointer", &e);
	ext_note_locked(e);
	if (!json && e == CALL_NO_METHOD && !s_ext_no_pointer) {
		s_ext_no_pointer = 1;
		di_log("the DisplayXR GNOME extension predates version 16: no pointer position\n");
	}
	int answered = json != NULL;
	// A reply without a position (the screen is locked) is an answer: ask again soon.
	s_ptr.ok = answered && dxr_di_parse_pointer(json, &s_ptr.x, &s_ptr.y);
	s_ptr.until = now + (answered ? DXR_DI_TTL_FAST : DXR_DI_RETRY);
	free(json);
	return s_ptr.ok;
}

static float
xroot_scale_locked(long long now)
{
	pthread_mutex_lock(&s_panel_mutex);
	DiPanel panel = s_panel;
	pthread_mutex_unlock(&s_panel_mutex);
	if (now < s_xroot.until && panel.gen == s_xroot.panel_gen)
		return s_xroot.k;
	float k = 0.0f;
	const char *how = "unknown";
	if (monitors_locked(now)) {
		for (int i = 0; panel.valid && i < s_mon.n && k == 0.0f; i++) {
			const DisplayXRLinuxMonitor *m = &s_mon.m[i];
			if (!strcmp((const char *)m->connector, panel.connector)) {
				k = dxr_di_xroot_scale_from_rect(panel.x, panel.y, panel.w, panel.h, m->x, m->y, m->width,
				                                 m->height);
				how = "the 3D panel's X root rect";
			}
		}
		// An X11 player only: a native-Wayland one must not open an X connection
		// (it would start XWayland on demand).
		int rw = 0, rh = 0;
		if (k == 0.0f && !dxr_wl_forced_wayland() && displayxr_linux_x_root_size(&rw, &rh)) {
			k = dxr_di_xroot_scale_from_root(rw, rh, s_mon.m, s_mon.n);
			how = "the X root window's size";
		}
	}
	s_xroot.k = k;
	s_xroot.until = now + DXR_DI_TTL_SLOW;
	s_xroot.panel_gen = panel.gen;
	if (!s_xroot.logged || s_xroot.k_log != k) {
		if (k > 0.0f)
			di_log("X root px per desktop px: %g (from %s)\n", (double)k, how);
		else
			di_log("X root px per desktop px: not known\n");
		s_xroot.logged = 1;
		s_xroot.k_log = k;
	}
	return k;
}

/*
 *
 * Plug-in internal.
 *
 */

//! The XR session's panel rect in X root px (XrDisplayDesktopInfoDXR); w <= 0 = none.
//! Called by displayxr_provider_session.cpp (its own extern "C" declaration).
void dxr_linux_desktop_info_set_panel(const char *connector, int x, int y, int w, int h);

void
dxr_linux_desktop_info_set_panel(const char *connector, int x, int y, int w, int h)
{
	pthread_mutex_lock(&s_panel_mutex);
	s_panel.valid = connector && connector[0] && w > 0 && h > 0;
	snprintf(s_panel.connector, sizeof(s_panel.connector), "%s", connector ? connector : "");
	s_panel.x = x;
	s_panel.y = y;
	s_panel.w = w;
	s_panel.h = h;
	s_panel.gen++;
	pthread_mutex_unlock(&s_panel_mutex);
}

/*
 *
 * Exports.
 *
 */

DISPLAYXR_EXPORT int
displayxr_linux_monitors(DisplayXRLinuxMonitor *buf, int len)
{
	pthread_mutex_lock(&s_mutex);
	int n = monitors_locked(now_ns()) ? s_mon.n : -1;
	if (n > 0 && buf && len > 0)
		memcpy(buf, s_mon.m, sizeof(*buf) * (size_t)(n < len ? n : len));
	pthread_mutex_unlock(&s_mutex);
	return n;
}

DISPLAYXR_EXPORT int
displayxr_linux_windows(DisplayXRLinuxWindow *buf, int len)
{
	pthread_mutex_lock(&s_mutex);
	int n = windows_locked(now_ns()) ? s_win.n : -1;
	if (n > 0 && buf && len > 0)
		memcpy(buf, s_win.w, sizeof(*buf) * (size_t)(n < len ? n : len));
	pthread_mutex_unlock(&s_mutex);
	return n;
}

DISPLAYXR_EXPORT int
displayxr_linux_pointer(int *x, int *y)
{
	pthread_mutex_lock(&s_mutex);
	int ok = pointer_locked(now_ns());
	if (ok) {
		if (x)
			*x = s_ptr.x;
		if (y)
			*y = s_ptr.y;
	}
	pthread_mutex_unlock(&s_mutex);
	return ok;
}

DISPLAYXR_EXPORT float
displayxr_linux_xroot_scale(void)
{
	pthread_mutex_lock(&s_mutex);
	float k = xroot_scale_locked(now_ns());
	pthread_mutex_unlock(&s_mutex);
	return k;
}
