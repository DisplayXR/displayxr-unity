// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Native-Wayland player: D-Bus client of the DisplayXR GNOME Shell extension.
// See displayxr_linux_wayland_dbus.h.

#include "displayxr_linux_wayland_dbus.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct DxrDBusConnection DxrDBusConnection;
typedef struct DxrDBusMessage DxrDBusMessage;
typedef struct DxrDBusError { // DBusError's layout
	const char *name;
	const char *message;
	unsigned int dummy1 : 1, dummy2 : 1, dummy3 : 1, dummy4 : 1, dummy5 : 1;
	void *padding1;
} DxrDBusError;

#define DXR_DBUS_BUS_SESSION 0
#define DXR_DBUS_TYPE_INVALID 0
#define DXR_DBUS_TYPE_BOOLEAN ((int)'b')
#define DXR_DBUS_TYPE_INT32 ((int)'i')
#define DXR_DBUS_TYPE_UINT32 ((int)'u')
#define DXR_DBUS_TYPE_STRING ((int)'s')

#define DXR_EXT_NAME "org.displayxr.WindowGeometry"
#define DXR_EXT_PLACEMENT_PATH "/org/displayxr/WindowPlacement"
#define DXR_EXT_PLACEMENT_IFACE "org.displayxr.WindowPlacement1"
#define DXR_EXT_GEOMETRY_PATH "/org/displayxr/WindowGeometry"
#define DXR_EXT_GEOMETRY_IFACE "org.displayxr.WindowGeometry1"

static struct {
	int ok;
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
	DxrDBusConnection *conn;
} s_dbus;
static pthread_once_t s_dbus_once = PTHREAD_ONCE_INIT;

static void
dbus_init(void)
{
	void *lib = dlopen("libdbus-1.so.3", RTLD_NOW | RTLD_LOCAL);
	if (!lib) {
		fprintf(stderr, "[DisplayXR-WL] libdbus-1 not found: no window drag or placement\n");
		return;
	}
#define DXR_DBUS_SYM(field, name) *(void **)&s_dbus.field = dlsym(lib, name)
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
#undef DXR_DBUS_SYM
	if (!s_dbus.threads_init_default || !s_dbus.error_init || !s_dbus.error_free || !s_dbus.bus_get_private ||
	    !s_dbus.set_exit_on_disconnect || !s_dbus.new_method_call || !s_dbus.append_args ||
	    !s_dbus.send_with_reply_and_block || !s_dbus.get_args || !s_dbus.unref)
		return;
	s_dbus.threads_init_default();
	DxrDBusError err;
	s_dbus.error_init(&err);
	s_dbus.conn = s_dbus.bus_get_private(DXR_DBUS_BUS_SESSION, &err);
	if (!s_dbus.conn) {
		fprintf(stderr, "[DisplayXR-WL] session bus unavailable: %s\n", err.message ? err.message : "?");
		s_dbus.error_free(&err);
		return;
	}
	// A bus connection _exit()s the process when the bus goes away, by default.
	s_dbus.set_exit_on_disconnect(s_dbus.conn, 0);
	s_dbus.ok = 1;
}

//! A method call to the extension, or NULL.
static DxrDBusMessage *
ext_call(const char *path, const char *iface, const char *method)
{
	pthread_once(&s_dbus_once, dbus_init);
	return s_dbus.ok ? s_dbus.new_method_call(DXR_EXT_NAME, path, iface, method) : NULL;
}

//! Send it (consumes msg); the reply, or NULL after logging why.
static DxrDBusMessage *
ext_send(DxrDBusMessage *msg, const char *method)
{
	DxrDBusError err;
	s_dbus.error_init(&err);
	DxrDBusMessage *reply = s_dbus.send_with_reply_and_block(s_dbus.conn, msg, 500, &err);
	s_dbus.unref(msg);
	if (!reply)
		fprintf(stderr, "[DisplayXR-WL] %s failed: %s (is the DisplayXR GNOME extension on?)\n", method,
		        err.message ? err.message : "?");
	s_dbus.error_free(&err);
	return reply;
}

//! The reply's boolean (consumes reply): 1 / 0.
static int
ext_reply_bool(DxrDBusMessage *reply)
{
	DxrDBusError err;
	s_dbus.error_init(&err);
	uint32_t b = 0;
	int ok = s_dbus.get_args(reply, &err, DXR_DBUS_TYPE_BOOLEAN, &b, DXR_DBUS_TYPE_INVALID) && b;
	s_dbus.error_free(&err);
	s_dbus.unref(reply);
	return ok;
}

// pid 0 = "the caller": the extension takes the PID from the bus connection and
// only ever moves the caller's own window.

int
dxr_wl_ext_move_window(int x, int y)
{
	DxrDBusMessage *msg = ext_call(DXR_EXT_PLACEMENT_PATH, DXR_EXT_PLACEMENT_IFACE, "MoveWindow");
	if (!msg)
		return 0;
	uint32_t pid = 0;
	int32_t ix = x, iy = y;
	s_dbus.append_args(msg, DXR_DBUS_TYPE_UINT32, &pid, DXR_DBUS_TYPE_INT32, &ix, DXR_DBUS_TYPE_INT32, &iy,
	                   DXR_DBUS_TYPE_INVALID);
	DxrDBusMessage *reply = ext_send(msg, "MoveWindow");
	return reply ? ext_reply_bool(reply) : 0;
}

int
dxr_wl_ext_begin_pointer_drag(unsigned button)
{
	DxrDBusMessage *msg = ext_call(DXR_EXT_PLACEMENT_PATH, DXR_EXT_PLACEMENT_IFACE, "BeginPointerDrag");
	if (!msg)
		return 0;
	uint32_t pid = 0, b = button;
	s_dbus.append_args(msg, DXR_DBUS_TYPE_UINT32, &pid, DXR_DBUS_TYPE_UINT32, &b, DXR_DBUS_TYPE_INVALID);
	DxrDBusMessage *reply = ext_send(msg, "BeginPointerDrag");
	return reply ? ext_reply_bool(reply) : 0;
}

void
dxr_wl_ext_end_pointer_drag(void)
{
	DxrDBusMessage *msg = ext_call(DXR_EXT_PLACEMENT_PATH, DXR_EXT_PLACEMENT_IFACE, "EndPointerDrag");
	if (!msg)
		return;
	uint32_t pid = 0;
	s_dbus.append_args(msg, DXR_DBUS_TYPE_UINT32, &pid, DXR_DBUS_TYPE_INVALID);
	DxrDBusMessage *reply = ext_send(msg, "EndPointerDrag");
	if (reply)
		s_dbus.unref(reply);
}

int
dxr_wl_ext_frame_origin(int *out_x, int *out_y)
{
	DxrDBusMessage *msg = ext_call(DXR_EXT_GEOMETRY_PATH, DXR_EXT_GEOMETRY_IFACE, "GetWindows");
	if (!msg)
		return 0;
	DxrDBusMessage *reply = ext_send(msg, "GetWindows");
	if (!reply)
		return 0;
	DxrDBusError err;
	s_dbus.error_init(&err);
	const char *json = NULL;
	int found = 0;
	if (s_dbus.get_args(reply, &err, DXR_DBUS_TYPE_STRING, &json, DXR_DBUS_TYPE_INVALID) && json) {
		// {"windows":[{"pid":N,...,"frame":[x,y,w,h],...},...]}: find our pid's.
		long self = (long)getpid();
		for (const char *p = strstr(json, "\"pid\""); p && !found; p = strstr(p + 5, "\"pid\"")) {
			const char *c = strchr(p, ':');
			if (!c || strtol(c + 1, NULL, 10) != self)
				continue;
			const char *f = strstr(p, "\"frame\"");
			const char *b = f ? strchr(f, '[') : NULL;
			if (b && sscanf(b + 1, " %d , %d", out_x, out_y) == 2)
				found = 1;
		}
	}
	s_dbus.error_free(&err);
	s_dbus.unref(reply);
	return found;
}
