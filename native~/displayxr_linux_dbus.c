// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// libdbus-1, loaded at run time. See displayxr_linux_dbus.h.

#include "displayxr_linux_dbus.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>

static DxrDBus s_table;
static const DxrDBus *s_loaded;
static const char *s_why = "libdbus-1 not loaded";
static char s_why_buf[96];
static pthread_once_t s_once = PTHREAD_ONCE_INIT;

static void
load(void)
{
	void *lib = dlopen("libdbus-1.so.3", RTLD_NOW | RTLD_LOCAL);
	if (!lib) {
		s_why = "libdbus-1 not found";
		return;
	}
#define DXR_DBUS_SYM(field, name)                                                                                  \
	do {                                                                                                       \
		*(void **)&s_table.field = dlsym(lib, name);                                                      \
		if (!s_table.field) {                                                                             \
			snprintf(s_why_buf, sizeof(s_why_buf), "libdbus-1 lacks %s", name);                       \
			s_why = s_why_buf;                                                                        \
			return;                                                                                   \
		}                                                                                                 \
	} while (0)
	DXR_DBUS_SYM(threads_init_default, "dbus_threads_init_default");
	DXR_DBUS_SYM(error_init, "dbus_error_init");
	DXR_DBUS_SYM(error_free, "dbus_error_free");
	DXR_DBUS_SYM(bus_get_private, "dbus_bus_get_private");
	DXR_DBUS_SYM(set_exit_on_disconnect, "dbus_connection_set_exit_on_disconnect");
	DXR_DBUS_SYM(get_is_connected, "dbus_connection_get_is_connected");
	DXR_DBUS_SYM(close, "dbus_connection_close");
	DXR_DBUS_SYM(connection_unref, "dbus_connection_unref");
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
	DXR_DBUS_SYM(iter_init, "dbus_message_iter_init");
	DXR_DBUS_SYM(iter_get_arg_type, "dbus_message_iter_get_arg_type");
	DXR_DBUS_SYM(iter_recurse, "dbus_message_iter_recurse");
	DXR_DBUS_SYM(iter_next, "dbus_message_iter_next");
	DXR_DBUS_SYM(iter_get_basic, "dbus_message_iter_get_basic");
#undef DXR_DBUS_SYM
	// Connections are used from more than one thread (the desktop info answers any caller).
	s_table.threads_init_default();
	s_loaded = &s_table;
}

const DxrDBus *
dxr_dbus_load(const char **why)
{
	pthread_once(&s_once, load);
	if (!s_loaded && why)
		*why = s_why;
	return s_loaded;
}
