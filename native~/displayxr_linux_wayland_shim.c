// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Native-Wayland player: the main plugin's side of the Wayland support library.
//
// The weave and window-control functions of displayxr_linux_wayland.h are
// implemented in libdisplayxr_unity_wayland.so (displayxr_linux_wayland.c),
// which links libwayland-client. This plugin must not: it also loads where that
// library is missing (X11 players, the editor, boxes with no panel). So these
// forward to the library, which is dlopened the first time a weave is created,
// i.e. only once the capture layer has found a native-Wayland player window.
// Until then, and if it cannot be loaded, every function is inert, exactly as
// the library's own functions are with no weave.

#define _GNU_SOURCE // dladdr
#include "displayxr_linux_wayland.h"
#include "displayxr_linux_wayland_lib.h"

#include <dlfcn.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const DxrWlHost s_host = {
    .abi = DXR_WL_LIB_ABI,
    .unity_surface = dxr_wl_unity_surface,
    .unity_swapchain_size = dxr_wl_unity_swapchain_size,
    .player_surface_generation = dxr_wl_player_surface_generation,
    .lock_player_surface = dxr_wl_lock_player_surface,
    .unlock_player_surface = dxr_wl_unlock_player_surface,
};

static const DxrWlApi *s_api; // published once by load_library()
static pthread_once_t s_load_once = PTHREAD_ONCE_INIT;

//! The library next to this plugin. Unity puts a player's plugins in
//! <Data>/Plugins/ and the provider deploy step also in <Data>/Plugins/x86_64/,
//! so look beside this module first, then in x86_64/, then on the search path.
static void *
open_library(char *tried, size_t tried_size)
{
	Dl_info self;
	char dir[PATH_MAX] = "";
	if (dladdr((void *)&open_library, &self) && self.dli_fname) {
		snprintf(dir, sizeof(dir), "%s", self.dli_fname);
		char *slash = strrchr(dir, '/');
		if (slash)
			*slash = 0;
		else
			dir[0] = 0;
	}
	char path[PATH_MAX + 64];
	const char *candidates[] = {"%s/" DXR_WL_LIB_NAME, "%s/x86_64/" DXR_WL_LIB_NAME};
	tried[0] = 0;
	for (size_t i = 0; dir[0] && i < sizeof(candidates) / sizeof(candidates[0]); i++) {
		snprintf(path, sizeof(path), candidates[i], dir);
		if (access(path, F_OK) != 0)
			continue;
		void *lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
		if (lib)
			return lib;
		// Present but not loadable (e.g. libwayland-client.so.0 missing): say why.
		const char *err = dlerror();
		snprintf(tried, tried_size, "%s", err ? err : path);
	}
	void *lib = dlopen(DXR_WL_LIB_NAME, RTLD_NOW | RTLD_LOCAL);
	if (!lib && !tried[0]) {
		const char *err = dlerror();
		snprintf(tried, tried_size, "%s", err ? err : DXR_WL_LIB_NAME " not found");
	}
	return lib;
}

static void
load_library(void)
{
	char why[512];
	void *lib = open_library(why, sizeof(why));
	if (!lib) {
		fprintf(stderr,
		        "[DisplayXR-WL] WARN: native-Wayland player, but the Wayland support library could not be "
		        "loaded (%s): no 3D in the player's window\n",
		        why);
		return;
	}
	PFN_dxr_wl_lib_init init = (PFN_dxr_wl_lib_init)dlsym(lib, DXR_WL_LIB_INIT);
	const DxrWlApi *api = init ? init(&s_host) : NULL;
	if (!api || api->abi != DXR_WL_LIB_ABI) {
		fprintf(stderr, "[DisplayXR-WL] WARN: %s does not match this plugin (a stale copy?): no 3D in the "
		                "player's window\n",
		        DXR_WL_LIB_NAME);
		dlclose(lib);
		return;
	}
	Dl_info info;
	fprintf(stderr, "[DisplayXR-WL] Wayland support library: %s\n",
	        dladdr((void *)init, &info) && info.dli_fname ? info.dli_fname : DXR_WL_LIB_NAME);
	__atomic_store_n(&s_api, api, __ATOMIC_RELEASE);
}

//! The library, if it has been loaded. Never loads it.
static const DxrWlApi *
api(void)
{
	return __atomic_load_n(&s_api, __ATOMIC_ACQUIRE);
}

int
dxr_wl_weave_surface_create(struct wl_display *display, struct wl_surface *parent, int lw, int lh,
                            struct wl_surface **out_surface, int *out_w, int *out_h)
{
	pthread_once(&s_load_once, load_library);
	const DxrWlApi *a = api();
	return a ? a->weave_surface_create(display, parent, lw, lh, out_surface, out_w, out_h) : 0;
}

void
dxr_wl_weave_destroy(void)
{
	const DxrWlApi *a = api();
	if (a)
		a->weave_destroy();
}

void
dxr_wl_weave_set_transparent(int transparent)
{
	const DxrWlApi *a = api();
	if (a)
		a->weave_set_transparent(transparent);
}

int
dxr_wl_weave_poll(int *out_w, int *out_h)
{
	const DxrWlApi *a = api();
	return a ? a->weave_poll(out_w, out_h) : 0;
}

int
dxr_wl_weave_active(void)
{
	const DxrWlApi *a = api();
	return a ? a->weave_active() : 0;
}

int
dxr_wl_weave_device_size(int *out_w, int *out_h)
{
	const DxrWlApi *a = api();
	return a ? a->weave_device_size(out_w, out_h) : 0;
}

double
dxr_wl_ui_scale(void)
{
	const DxrWlApi *a = api();
	return a ? a->ui_scale() : 1.0;
}

int
dxr_wl_click_through_wanted(void)
{
	const DxrWlApi *a = api();
	return a ? a->click_through_wanted() : 0;
}

int
dxr_wl_set_player_input_region(const void *rects, int n)
{
	const DxrWlApi *a = api();
	return a ? a->set_player_input_region(rects, n) : 0;
}

int
dxr_wl_begin_pointer_drag(unsigned button)
{
	const DxrWlApi *a = api();
	return a ? a->begin_pointer_drag(button) : 0;
}

void
dxr_wl_end_pointer_drag(void)
{
	const DxrWlApi *a = api();
	if (a)
		a->end_pointer_drag();
}

void
dxr_wl_request_player_size(int device_w, int device_h)
{
	const DxrWlApi *a = api();
	if (a)
		a->request_player_size(device_w, device_h);
}

int
dxr_wl_take_player_size(int *out_w, int *out_h)
{
	const DxrWlApi *a = api();
	return a ? a->take_player_size(out_w, out_h) : 0;
}

int
dxr_wl_move_player_to_panel(const char *connector, int panel_w, int panel_h)
{
	const DxrWlApi *a = api();
	return a ? a->move_player_to_panel(connector, panel_w, panel_h) : 0;
}

int
dxr_wl_get_player_position_x11(int *out_x, int *out_y)
{
	const DxrWlApi *a = api();
	return a ? a->get_player_position_x11(out_x, out_y) : 0;
}

int
dxr_wl_set_player_position_x11(int x, int y)
{
	const DxrWlApi *a = api();
	return a ? a->set_player_position_x11(x, y) : 0;
}

unsigned
dxr_wl_player_generation(void)
{
	const DxrWlApi *a = api();
	return a ? a->player_generation() : 0;
}

void
dxr_wl_set_x11_panel_rect(const char *connector, int x, int y, int w, int h)
{
	const DxrWlApi *a = api();
	if (a)
		a->set_x11_panel_rect(connector, x, y, w, h);
}
