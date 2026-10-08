// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Native-Wayland player: client of the DisplayXR GNOME Shell extension, on one
// worker thread. See displayxr_linux_wayland_dbus.h.

#define _GNU_SOURCE // pipe2
#include "displayxr_linux_wayland_dbus.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 *
 * libdbus, loaded at run time.
 *
 */

typedef struct DxrDBusConnection DxrDBusConnection;
typedef struct DxrDBusMessage DxrDBusMessage;
typedef struct DxrDBusError { // DBusError's layout
	const char *name;
	const char *message;
	unsigned int dummy1 : 1, dummy2 : 1, dummy3 : 1, dummy4 : 1, dummy5 : 1;
	void *padding1;
} DxrDBusError;
typedef int (*DxrDBusFilter)(DxrDBusConnection *, DxrDBusMessage *, void *);

#define DXR_DBUS_BUS_SESSION 0
#define DXR_DBUS_TYPE_INVALID 0
#define DXR_DBUS_TYPE_BOOLEAN ((int)'b')
#define DXR_DBUS_TYPE_INT32 ((int)'i')
#define DXR_DBUS_TYPE_UINT32 ((int)'u')
#define DXR_DBUS_TYPE_STRING ((int)'s')
#define DXR_DBUS_HANDLER_RESULT_NOT_YET_HANDLED 1
#define DXR_DBUS_DISPATCH_DATA_REMAINS 0

#define DXR_EXT_NAME "org.displayxr.WindowGeometry"
#define DXR_EXT_PLACEMENT_PATH "/org/displayxr/WindowPlacement"
#define DXR_EXT_PLACEMENT_IFACE "org.displayxr.WindowPlacement1"
#define DXR_EXT_GEOMETRY_PATH "/org/displayxr/WindowGeometry"
#define DXR_EXT_GEOMETRY_IFACE "org.displayxr.WindowGeometry1"

static struct {
	uint32_t (*threads_init_default)(void);
	void (*error_init)(DxrDBusError *);
	void (*error_free)(DxrDBusError *);
	DxrDBusConnection *(*bus_get_private)(int, DxrDBusError *);
	void (*set_exit_on_disconnect)(DxrDBusConnection *, uint32_t);
	DxrDBusMessage *(*new_method_call)(const char *, const char *, const char *, const char *);
	uint32_t (*append_args)(DxrDBusMessage *, int, ...);
	DxrDBusMessage *(*send_with_reply_and_block)(DxrDBusConnection *, DxrDBusMessage *, int, DxrDBusError *);
	uint32_t (*get_args)(DxrDBusMessage *, DxrDBusError *, int, ...);
	void (*unref)(DxrDBusMessage *);
	void (*add_match)(DxrDBusConnection *, const char *, DxrDBusError *);
	uint32_t (*add_filter)(DxrDBusConnection *, DxrDBusFilter, void *, void (*)(void *));
	uint32_t (*read_write)(DxrDBusConnection *, int);
	int (*dispatch)(DxrDBusConnection *);
	uint32_t (*get_unix_fd)(DxrDBusConnection *, int *);
	uint32_t (*is_signal)(DxrDBusMessage *, const char *, const char *);
	uint32_t (*name_has_owner)(DxrDBusConnection *, const char *, DxrDBusError *);
	DxrDBusConnection *conn;
} s_dbus;

static int
dbus_load(void)
{
	void *lib = dlopen("libdbus-1.so.3", RTLD_NOW | RTLD_LOCAL);
	if (!lib) {
		fprintf(stderr, "[DisplayXR-WL] libdbus-1 not found: no window drag or placement\n");
		return 0;
	}
#define DXR_DBUS_SYM(field, name)                                                                                  \
	do {                                                                                                       \
		*(void **)&s_dbus.field = dlsym(lib, name);                                                       \
		if (!s_dbus.field) {                                                                              \
			fprintf(stderr, "[DisplayXR-WL] libdbus-1 lacks %s: no window drag or placement\n", name); \
			return 0;                                                                                 \
		}                                                                                                 \
	} while (0)
	DXR_DBUS_SYM(threads_init_default, "dbus_threads_init_default");
	DXR_DBUS_SYM(error_init, "dbus_error_init");
	DXR_DBUS_SYM(error_free, "dbus_error_free");
	DXR_DBUS_SYM(bus_get_private, "dbus_bus_get_private");
	DXR_DBUS_SYM(set_exit_on_disconnect, "dbus_connection_set_exit_on_disconnect");
	DXR_DBUS_SYM(new_method_call, "dbus_message_new_method_call");
	DXR_DBUS_SYM(append_args, "dbus_message_append_args");
	DXR_DBUS_SYM(send_with_reply_and_block, "dbus_connection_send_with_reply_and_block");
	DXR_DBUS_SYM(get_args, "dbus_message_get_args");
	DXR_DBUS_SYM(unref, "dbus_message_unref");
	DXR_DBUS_SYM(add_match, "dbus_bus_add_match");
	DXR_DBUS_SYM(add_filter, "dbus_connection_add_filter");
	DXR_DBUS_SYM(read_write, "dbus_connection_read_write");
	DXR_DBUS_SYM(dispatch, "dbus_connection_dispatch");
	DXR_DBUS_SYM(get_unix_fd, "dbus_connection_get_unix_fd");
	DXR_DBUS_SYM(is_signal, "dbus_message_is_signal");
	DXR_DBUS_SYM(name_has_owner, "dbus_bus_name_has_owner");
#undef DXR_DBUS_SYM
	s_dbus.threads_init_default();
	return 1;
}

/*
 *
 * The extension's JSON (GetWindows / WindowsChanged): just enough of a parser to
 * walk it without being fooled by a window title.
 *
 */

static const char *
json_ws(const char *p, const char *end)
{
	while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
		p++;
	return p;
}

//! One past the JSON value at p, or NULL if it is cut short.
static const char *
json_skip(const char *p, const char *end)
{
	if (p >= end)
		return NULL;
	if (*p == '"') {
		for (p++; p < end; p++) {
			if (*p == '\\')
				p++;
			else if (*p == '"')
				return p + 1;
		}
		return NULL;
	}
	if (*p == '{' || *p == '[') {
		int depth = 0;
		for (; p < end; p++) {
			if (*p == '"') {
				p = json_skip(p, end);
				if (!p)
					return NULL;
				p--;
			} else if (*p == '{' || *p == '[') {
				depth++;
			} else if (*p == '}' || *p == ']') {
				if (--depth == 0)
					return p + 1;
			}
		}
		return NULL;
	}
	while (p < end && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\n')
		p++;
	return p;
}

//! The value of `key` at the top level of the object [obj, end), or NULL.
static const char *
json_field(const char *obj, const char *end, const char *key)
{
	const char *p = json_ws(obj, end);
	if (p >= end || *p != '{')
		return NULL;
	size_t key_len = strlen(key);
	for (p++;;) {
		p = json_ws(p, end);
		if (p >= end || *p != '"')
			return NULL;
		const char *k = p + 1;
		const char *after = json_skip(p, end);
		if (!after)
			return NULL;
		int match = (size_t)(after - 1 - k) == key_len && !memcmp(k, key, key_len);
		p = json_ws(after, end);
		if (p >= end || *p != ':')
			return NULL;
		const char *value = json_ws(p + 1, end);
		if (match)
			return value;
		p = json_skip(value, end);
		if (!p)
			return NULL;
		p = json_ws(p, end);
		if (p < end && *p == ',')
			p++;
		else
			return NULL;
	}
}

//! A JSON string value into out (no unescaping; the values we read have none).
static void
json_string(const char *v, const char *end, char *out, size_t size)
{
	out[0] = 0;
	if (!v || v >= end || *v != '"')
		return;
	const char *close = json_skip(v, end);
	if (!close)
		return;
	size_t n = (size_t)(close - 1 - (v + 1));
	if (n >= size)
		n = size - 1;
	memcpy(out, v + 1, n);
	out[n] = 0;
}

//! Our window and mutter's layout mode from a snapshot.
static void
parse_snapshot(const char *json, DxrWlExtWindow *w, char *layout, size_t layout_size)
{
	memset(w, 0, sizeof(*w));
	const char *end = json + strlen(json);
	json_string(json_field(json, end, "layout_mode"), end, layout, layout_size);
	const char *p = json_field(json, end, "windows");
	if (!p || *p != '[')
		return;
	long self = (long)getpid();
	for (p++;;) {
		p = json_ws(p, end);
		if (p >= end || *p != '{')
			return;
		const char *obj_end = json_skip(p, end);
		if (!obj_end)
			return;
		const char *pid = json_field(p, obj_end, "pid");
		if (pid && strtol(pid, NULL, 10) == self) {
			DxrWlExtWindow c = {0};
			c.present = 1;
			const char *frame = json_field(p, obj_end, "frame");
			if (!frame || sscanf(frame, "[ %d , %d , %d , %d", &c.x, &c.y, &c.w, &c.h) != 4)
				c.w = c.h = 0;
			const char *moving = json_field(p, obj_end, "moving");
			c.moving = moving && !strncmp(moving, "true", 4);
			const char *mon = json_field(p, obj_end, "monitor");
			const char *mon_end = mon ? json_skip(mon, obj_end) : NULL;
			const char *ds = mon_end ? json_field(mon, mon_end, "device_scale") : NULL;
			c.device_scale = ds ? strtod(ds, NULL) : 0.0;
			// Unity has one window; should there be more, the one mutter has mapped.
			if (!w->present || (w->w * w->h == 0 && c.w * c.h > 0))
				*w = c;
		}
		p = json_ws(obj_end, end);
		if (p < end && *p == ',')
			p++;
		else
			return;
	}
}

/*
 *
 * The worker.
 *
 */

static const DxrWlExtCallbacks *s_cb;
static pthread_once_t s_start_once = PTHREAD_ONCE_INIT;
static int s_wake[2] = {-1, -1};

// Requests from other threads. Moves are coalesced: only the latest one counts.
static pthread_mutex_t s_job_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct {
	int move;
	int move_x, move_y;
	int begin;
	unsigned button;
	int end;
} s_job;

// What the filter saw (worker thread only).
static int s_owner_changed;
static char *s_latest_json; // the newest WindowsChanged payload, unprocessed

static int s_available = -1; // the extension owns its bus name (1), not (0), not known yet (-1)
static unsigned s_caps;
static char s_layout[16];

static void
wake(void)
{
	if (s_wake[1] >= 0) {
		char c = 1;
		ssize_t r = write(s_wake[1], &c, 1);
		(void)r;
	}
}

static int
filter(DxrDBusConnection *conn, DxrDBusMessage *msg, void *data)
{
	(void)conn;
	(void)data;
	if (s_dbus.is_signal(msg, DXR_EXT_GEOMETRY_IFACE, "WindowsChanged")) {
		DxrDBusError err;
		s_dbus.error_init(&err);
		const char *json = NULL;
		if (s_dbus.get_args(msg, &err, DXR_DBUS_TYPE_STRING, &json, DXR_DBUS_TYPE_INVALID) && json) {
			free(s_latest_json);
			s_latest_json = strdup(json);
		}
		s_dbus.error_free(&err);
	} else if (s_dbus.is_signal(msg, "org.freedesktop.DBus", "NameOwnerChanged")) {
		s_owner_changed = 1;
	}
	return DXR_DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

//! A method call on the extension; the reply or NULL. Quiet: the availability
//! state already says when the extension is not there.
static DxrDBusMessage *
call(const char *path, const char *iface, const char *method, int first_type, ...)
{
	DxrDBusMessage *msg = s_dbus.new_method_call(DXR_EXT_NAME, path, iface, method);
	if (!msg)
		return NULL;
	if (first_type != DXR_DBUS_TYPE_INVALID) {
		va_list ap;
		va_start(ap, first_type);
		// dbus_message_append_args is variadic; forward up to three typed args.
		int t1 = first_type;
		void *a1 = va_arg(ap, void *);
		int t2 = va_arg(ap, int);
		void *a2 = t2 != DXR_DBUS_TYPE_INVALID ? va_arg(ap, void *) : NULL;
		int t3 = t2 != DXR_DBUS_TYPE_INVALID ? va_arg(ap, int) : DXR_DBUS_TYPE_INVALID;
		void *a3 = t3 != DXR_DBUS_TYPE_INVALID ? va_arg(ap, void *) : NULL;
		va_end(ap);
		if (t3 != DXR_DBUS_TYPE_INVALID)
			s_dbus.append_args(msg, t1, a1, t2, a2, t3, a3, DXR_DBUS_TYPE_INVALID);
		else if (t2 != DXR_DBUS_TYPE_INVALID)
			s_dbus.append_args(msg, t1, a1, t2, a2, DXR_DBUS_TYPE_INVALID);
		else
			s_dbus.append_args(msg, t1, a1, DXR_DBUS_TYPE_INVALID);
	}
	DxrDBusError err;
	s_dbus.error_init(&err);
	DxrDBusMessage *reply = s_dbus.send_with_reply_and_block(s_dbus.conn, msg, 1000, &err);
	s_dbus.unref(msg);
	if (!reply && s_available > 0)
		fprintf(stderr, "[DisplayXR-WL] %s failed: %s\n", method, err.message ? err.message : "?");
	s_dbus.error_free(&err);
	return reply;
}

static int
reply_bool(DxrDBusMessage *reply)
{
	if (!reply)
		return 0;
	DxrDBusError err;
	s_dbus.error_init(&err);
	uint32_t b = 0;
	int ok = s_dbus.get_args(reply, &err, DXR_DBUS_TYPE_BOOLEAN, &b, DXR_DBUS_TYPE_INVALID) && b;
	s_dbus.error_free(&err);
	s_dbus.unref(reply);
	return ok;
}

//! Read our window afresh and report it.
static void
snapshot(int after_move, int moved)
{
	DxrDBusMessage *reply = call(DXR_EXT_GEOMETRY_PATH, DXR_EXT_GEOMETRY_IFACE, "GetWindows", DXR_DBUS_TYPE_INVALID);
	if (!reply)
		return;
	DxrDBusError err;
	s_dbus.error_init(&err);
	const char *json = NULL;
	if (s_dbus.get_args(reply, &err, DXR_DBUS_TYPE_STRING, &json, DXR_DBUS_TYPE_INVALID) && json) {
		DxrWlExtWindow w;
		parse_snapshot(json, &w, s_layout, sizeof(s_layout));
		s_cb->window(&w, after_move, moved);
	}
	s_dbus.error_free(&err);
	s_dbus.unref(reply);
}

//! The extension's bus name got (or lost) an owner, or we are just starting.
static void
check_owner(void)
{
	DxrDBusError err;
	s_dbus.error_init(&err);
	int owned = s_dbus.name_has_owner(s_dbus.conn, DXR_EXT_NAME, &err) ? 1 : 0;
	s_dbus.error_free(&err);
	if (owned == s_available)
		return;
	s_available = owned;
	s_caps = 0;
	if (owned) {
		DxrDBusMessage *reply =
		    call(DXR_EXT_PLACEMENT_PATH, DXR_EXT_PLACEMENT_IFACE, "GetPlacementCapabilities", DXR_DBUS_TYPE_INVALID);
		if (reply) { // extension version 6+; older ones have no capabilities to offer
			DxrDBusError e2;
			s_dbus.error_init(&e2);
			uint32_t caps = 0;
			if (s_dbus.get_args(reply, &e2, DXR_DBUS_TYPE_UINT32, &caps, DXR_DBUS_TYPE_INVALID))
				s_caps = caps;
			s_dbus.error_free(&e2);
			s_dbus.unref(reply);
		}
		// The layout mode comes with the first snapshot; report it after that.
		DxrDBusMessage *r2 = call(DXR_EXT_GEOMETRY_PATH, DXR_EXT_GEOMETRY_IFACE, "GetWindows", DXR_DBUS_TYPE_INVALID);
		DxrWlExtWindow w = {0};
		if (r2) {
			DxrDBusError e3;
			s_dbus.error_init(&e3);
			const char *json = NULL;
			if (s_dbus.get_args(r2, &e3, DXR_DBUS_TYPE_STRING, &json, DXR_DBUS_TYPE_INVALID) && json)
				parse_snapshot(json, &w, s_layout, sizeof(s_layout));
			s_dbus.error_free(&e3);
			s_dbus.unref(r2);
		}
		fprintf(stderr, "[DisplayXR-WL] DisplayXR GNOME extension: available (capabilities 0x%x, layout %s)\n", s_caps,
		        s_layout[0] ? s_layout : "not reported");
		s_cb->availability(1, s_caps, s_layout);
		s_cb->window(&w, 0, 0);
	} else {
		fprintf(stderr, "[DisplayXR-WL] DisplayXR GNOME extension: not available (not installed, disabled, or the "
		                "screen is locked): no window placement or drag until it is\n");
		DxrWlExtWindow none = {0};
		s_cb->availability(0, 0, "");
		s_cb->window(&none, 0, 0);
	}
}

static void
run_jobs(void)
{
	pthread_mutex_lock(&s_job_mutex);
	typeof(s_job) job = s_job;
	memset(&s_job, 0, sizeof(s_job));
	pthread_mutex_unlock(&s_job_mutex);

	uint32_t pid = 0; // "the caller"
	if (job.begin) {
		int started = 0;
		if (s_available > 0 && (s_caps & DXR_WL_EXT_CAP_POINTER_DRAG)) {
			uint32_t b = job.button;
			started = reply_bool(call(DXR_EXT_PLACEMENT_PATH, DXR_EXT_PLACEMENT_IFACE, "BeginPointerDrag",
			                          DXR_DBUS_TYPE_UINT32, &pid, DXR_DBUS_TYPE_UINT32, &b, DXR_DBUS_TYPE_INVALID));
		}
		s_cb->drag(started);
	}
	if (job.end && s_available > 0 && (s_caps & DXR_WL_EXT_CAP_POINTER_DRAG)) {
		DxrDBusMessage *r = call(DXR_EXT_PLACEMENT_PATH, DXR_EXT_PLACEMENT_IFACE, "EndPointerDrag",
		                         DXR_DBUS_TYPE_UINT32, &pid, DXR_DBUS_TYPE_INVALID);
		if (r)
			s_dbus.unref(r);
		snapshot(0, 0); // where the user left it
	}
	if (job.move && s_available > 0) {
		int32_t x = job.move_x, y = job.move_y;
		int moved = reply_bool(call(DXR_EXT_PLACEMENT_PATH, DXR_EXT_PLACEMENT_IFACE, "MoveWindow", DXR_DBUS_TYPE_UINT32,
		                            &pid, DXR_DBUS_TYPE_INT32, &x, DXR_DBUS_TYPE_INT32, &y, DXR_DBUS_TYPE_INVALID));
		snapshot(1, moved); // where it actually landed (mutter may constrain it)
	} else if (job.move) {
		DxrWlExtWindow none = {0};
		s_cb->window(&none, 1, 0);
	}
}

static int
connect_bus(void)
{
	DxrDBusError err;
	s_dbus.error_init(&err);
	s_dbus.conn = s_dbus.bus_get_private(DXR_DBUS_BUS_SESSION, &err);
	if (!s_dbus.conn) {
		fprintf(stderr, "[DisplayXR-WL] session bus unavailable: %s\n", err.message ? err.message : "?");
		s_dbus.error_free(&err);
		return 0;
	}
	// A bus connection _exit()s the process when the bus goes away, by default.
	s_dbus.set_exit_on_disconnect(s_dbus.conn, 0);
	s_dbus.add_filter(s_dbus.conn, filter, NULL, NULL);
	s_dbus.add_match(s_dbus.conn,
	                 "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',"
	                 "member='NameOwnerChanged',arg0='" DXR_EXT_NAME "'",
	                 &err);
	s_dbus.add_match(s_dbus.conn, "type='signal',interface='" DXR_EXT_GEOMETRY_IFACE "',member='WindowsChanged'", &err);
	s_dbus.error_free(&err);
	return 1;
}

static void *
worker(void *arg)
{
	(void)arg;
	if (!dbus_load() || !connect_bus()) {
		// No placement at all: answer every request as refused.
		for (;;) {
			struct pollfd p = {s_wake[0], POLLIN, 0};
			if (poll(&p, 1, -1) > 0) {
				char buf[64];
				while (read(s_wake[0], buf, sizeof(buf)) > 0) {
				}
				run_jobs();
			}
		}
	}
	int fd = -1;
	s_dbus.get_unix_fd(s_dbus.conn, &fd);
	check_owner();
	for (;;) {
		// Everything already read must be dispatched before waiting on the socket
		// again (a blocking call can leave messages queued).
		while (s_dbus.dispatch(s_dbus.conn) == DXR_DBUS_DISPATCH_DATA_REMAINS) {
		}
		if (s_owner_changed) {
			s_owner_changed = 0;
			check_owner();
		}
		if (s_latest_json) {
			char *json = s_latest_json;
			s_latest_json = NULL;
			DxrWlExtWindow w;
			parse_snapshot(json, &w, s_layout, sizeof(s_layout));
			free(json);
			if (s_available > 0)
				s_cb->window(&w, 0, 0);
		}
		run_jobs();
		while (s_dbus.dispatch(s_dbus.conn) == DXR_DBUS_DISPATCH_DATA_REMAINS) {
		}
		if (s_owner_changed || s_latest_json)
			continue;
		struct pollfd p[2] = {{fd, POLLIN, 0}, {s_wake[0], POLLIN, 0}};
		if (poll(p, 2, -1) > 0) {
			if (p[1].revents) {
				char buf[64];
				while (read(s_wake[0], buf, sizeof(buf)) > 0) {
				}
			}
			if (p[0].revents)
				s_dbus.read_write(s_dbus.conn, 0);
		}
	}
	return NULL;
}

static void
start(void)
{
	if (pipe2(s_wake, O_CLOEXEC | O_NONBLOCK) != 0) {
		s_wake[0] = s_wake[1] = -1;
		fprintf(stderr, "[DisplayXR-WL] cannot create the extension worker's pipe: no window placement or drag\n");
		return;
	}
	pthread_t t;
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	if (pthread_create(&t, &attr, worker, NULL) != 0)
		fprintf(stderr, "[DisplayXR-WL] cannot start the extension worker: no window placement or drag\n");
	else
		pthread_setname_np(t, "dxr-wl-ext");
	pthread_attr_destroy(&attr);
}

void
dxr_wl_ext_start(const DxrWlExtCallbacks *callbacks)
{
	if (!s_cb)
		s_cb = callbacks;
	pthread_once(&s_start_once, start);
}

void
dxr_wl_ext_move_window(int x, int y)
{
	pthread_mutex_lock(&s_job_mutex);
	s_job.move = 1;
	s_job.move_x = x;
	s_job.move_y = y;
	pthread_mutex_unlock(&s_job_mutex);
	wake();
}

void
dxr_wl_ext_begin_pointer_drag(unsigned button)
{
	pthread_mutex_lock(&s_job_mutex);
	s_job.begin = 1;
	s_job.button = button;
	s_job.end = 0;
	pthread_mutex_unlock(&s_job_mutex);
	wake();
}

void
dxr_wl_ext_end_pointer_drag(void)
{
	pthread_mutex_lock(&s_job_mutex);
	s_job.end = 1;
	pthread_mutex_unlock(&s_job_mutex);
	wake();
}
