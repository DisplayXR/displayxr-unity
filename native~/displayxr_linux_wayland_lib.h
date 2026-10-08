// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// The interface between the main plugin (libdisplayxr_unity.so) and its Wayland
// support library (libdisplayxr_unity_wayland.so).
//
// Why two libraries: everything that talks Wayland links libwayland-client, and a
// link dependency would make the main plugin fail to load wherever that library
// is missing, X11 players and the editor included. So the Wayland code lives in
// its own library, which the main plugin dlopens only once the capture layer has
// found a native-Wayland player window (displayxr_linux_wayland_shim.c). The two
// always ship together; DXR_WL_LIB_ABI catches a stale copy.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct wl_display;
struct wl_surface;

#define DXR_WL_LIB_ABI 2
#define DXR_WL_LIB_NAME "libdisplayxr_unity_wayland.so"
#define DXR_WL_LIB_INIT "dxr_wl_lib_init"

//! What the library needs from the main plugin: the player's window, as the
//! capture layer (displayxr_provider_wl_capture.cpp) recorded it.
typedef struct DxrWlHost {
	uint32_t abi;
	int (*unity_surface)(struct wl_display **out_display, struct wl_surface **out_surface);
	int (*unity_swapchain_size)(int *out_w, int *out_h);
	unsigned (*player_surface_generation)(void);
	//! Use the player's wl_surface only between these (displayxr_linux_wayland.h).
	struct wl_surface *(*lock_player_surface)(unsigned *out_generation);
	void (*unlock_player_surface)(void);
} DxrWlHost;

//! What the library provides: the weave and window-control half of
//! displayxr_linux_wayland.h, one entry per function there.
typedef struct DxrWlApi {
	uint32_t abi;
	int (*weave_surface_create)(struct wl_display *display, struct wl_surface *parent, int lw, int lh,
	                            struct wl_surface **out_surface, int *out_w, int *out_h);
	void (*weave_destroy)(void);
	void (*weave_set_transparent)(int transparent);
	int (*weave_poll)(int *out_w, int *out_h);
	int (*weave_active)(void);
	int (*weave_device_size)(int *out_w, int *out_h);
	double (*ui_scale)(void);
	int (*click_through_wanted)(void);
	int (*set_player_input_region)(const void *rects, int n);
	int (*begin_pointer_drag)(unsigned button);
	void (*end_pointer_drag)(void);
	void (*request_player_size)(int device_w, int device_h);
	int (*take_player_size)(int *out_w, int *out_h);
	int (*move_player_to_panel)(const char *connector, int panel_w, int panel_h);
	int (*get_player_position_x11)(int *out_x, int *out_y);
	int (*set_player_position_x11)(int x, int y);
	unsigned (*player_generation)(void);
} DxrWlApi;

//! The library's one export. NULL on an ABI mismatch.
typedef const DxrWlApi *(*PFN_dxr_wl_lib_init)(const DxrWlHost *host);

#ifdef __cplusplus
}
#endif
