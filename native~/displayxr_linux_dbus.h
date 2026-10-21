// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// libdbus-1, loaded at run time, for the Linux code that talks to the session bus:
//  - the native-Wayland support library's client of the DisplayXR GNOME extension
//    (displayxr_linux_wayland_dbus.c, in libdisplayxr_unity_wayland.so);
//  - the main plug-in's read-only desktop info (displayxr_linux_desktop_info.c).
// Both libraries compile this file in (hidden visibility), so each has its own copy
// of the table.
//
// Why dlopen: a link dependency would make the plug-in fail to load wherever
// libdbus-1 is missing, and CI pins both libraries' NEEDED lists. Only the handful
// of entry points below are used; the types are declared here with the layouts
// libdbus-1 documents as stable (DBusError) or as opaque storage (DBusMessageIter).

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct DxrDBusConnection DxrDBusConnection;
typedef struct DxrDBusMessage DxrDBusMessage;
typedef struct DxrDBusError { // DBusError's layout
	const char *name;
	const char *message;
	unsigned int dummy1 : 1, dummy2 : 1, dummy3 : 1, dummy4 : 1, dummy5 : 1;
	void *padding1;
} DxrDBusError;
//! Storage for a DBusMessageIter (72 bytes on LP64; libdbus only writes through
//! the pointer, so more room than that is harmless).
typedef struct DxrDBusMessageIter {
	void *opaque[16];
} DxrDBusMessageIter;
typedef int (*DxrDBusFilter)(DxrDBusConnection *, DxrDBusMessage *, void *);

#define DXR_DBUS_BUS_SESSION 0
#define DXR_DBUS_TYPE_INVALID 0
#define DXR_DBUS_TYPE_BOOLEAN ((int)'b')
#define DXR_DBUS_TYPE_INT32 ((int)'i')
#define DXR_DBUS_TYPE_UINT32 ((int)'u')
#define DXR_DBUS_TYPE_DOUBLE ((int)'d')
#define DXR_DBUS_TYPE_STRING ((int)'s')
#define DXR_DBUS_TYPE_ARRAY ((int)'a')
#define DXR_DBUS_TYPE_VARIANT ((int)'v')
#define DXR_DBUS_TYPE_STRUCT ((int)'r')
#define DXR_DBUS_TYPE_DICT_ENTRY ((int)'e')
#define DXR_DBUS_HANDLER_RESULT_NOT_YET_HANDLED 1
#define DXR_DBUS_DISPATCH_DATA_REMAINS 0

//! The entry points used, resolved from libdbus-1.so.3.
typedef struct DxrDBus {
	uint32_t (*threads_init_default)(void);
	void (*error_init)(DxrDBusError *);
	void (*error_free)(DxrDBusError *);
	DxrDBusConnection *(*bus_get_private)(int, DxrDBusError *);
	void (*set_exit_on_disconnect)(DxrDBusConnection *, uint32_t);
	uint32_t (*get_is_connected)(DxrDBusConnection *);
	void (*close)(DxrDBusConnection *);
	void (*connection_unref)(DxrDBusConnection *);
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
	uint32_t (*iter_init)(DxrDBusMessage *, DxrDBusMessageIter *);
	int (*iter_get_arg_type)(DxrDBusMessageIter *);
	void (*iter_recurse)(DxrDBusMessageIter *, DxrDBusMessageIter *);
	uint32_t (*iter_next)(DxrDBusMessageIter *);
	void (*iter_get_basic)(DxrDBusMessageIter *, void *);
} DxrDBus;

//! libdbus-1, loaded on the first call (thread-safe; later calls are cheap).
//! NULL when it cannot be loaded; then *why (if given) says why, e.g.
//! "libdbus-1 not found". The caller logs it with what it cannot do without.
const DxrDBus *
dxr_dbus_load(const char **why);

#ifdef __cplusplus
}
#endif
