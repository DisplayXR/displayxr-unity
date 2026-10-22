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

#define DXR_WL_LIB_ABI 10
#define DXR_WL_LIB_NAME "libdisplayxr_unity_wayland.so"
#define DXR_WL_LIB_INIT "dxr_wl_lib_init"

//! dxr_wl_own_window_caps() bits: what the GNOME extension can do for the
//! player's own window right now (extension version 15).
#define DXR_WL_OWN_KEEP_ABOVE 1 //!< dxr_wl_set_keep_above
#define DXR_WL_OWN_ACTIVATE 2   //!< dxr_wl_activate_window
#define DXR_WL_OWN_MINIMIZE 4   //!< dxr_wl_set_minimized

//! File drops (dxr_wl_file_drop_next): what happened.
#define DXR_WL_DROP_ENTER 1 //!< files dragged onto the window (paths known)
#define DXR_WL_DROP_MOVE 2  //!< they moved over it (the latest position only)
#define DXR_WL_DROP_LEAVE 3 //!< they left it, or the drag was cancelled
#define DXR_WL_DROP_DROP 4  //!< they were dropped on it (paths)
//! DxrWlDropEvent.flags: which coordinates are known.
#define DXR_WL_DROP_HAS_ROOT 1  //!< x, y: X root px
#define DXR_WL_DROP_HAS_STAGE 2 //!< stage_x, stage_y: mutter's stage px

//! One file-drop event. Ten int32, no padding: it is also the layout C# reads
//! (displayxr_linux_file_drop_next; Runtime/DisplayXRNative.cs).
typedef struct DxrWlDropEvent {
	int32_t kind;               //!< DXR_WL_DROP_*
	int32_t flags;              //!< DXR_WL_DROP_HAS_*
	int32_t x, y;               //!< the pointer, X root px (HAS_ROOT)
	int32_t stage_x, stage_y;   //!< the pointer, stage px (HAS_STAGE)
	int32_t window_x, window_y; //!< the pointer in the window, device px from its top-left (always)
	int32_t path_count;         //!< ENTER / DROP: the files
	int32_t paths_bytes;        //!< ENTER / DROP: the paths' size, below; 0 otherwise
} DxrWlDropEvent;

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
	int (*player_presents_alpha)(void);
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
	void (*set_x11_panel_rect)(const char *connector, int x, int y, int w, int h);
	int (*pointer_inside)(void);
	int (*own_window_caps)(void);
	int (*set_keep_above)(int above);
	int (*activate_window)(void);
	int (*set_minimized)(int minimized);
	int (*window_state)(void);
	int (*export_handle)(char *buf, int len);
	int (*file_drop_enable)(int enable);
	int (*file_drop_next)(DxrWlDropEvent *event, char *paths, int paths_len);
	int (*clipboard_state)(void);
	int (*clipboard_read)(char *buf, int len, int timeout_ms, int *needed);
	int (*clipboard_write)(const char *text, int len);
	int (*set_player_position_stage)(int x, int y, int log); // ABI 10: the side-step's moves
} DxrWlApi;

//! The library's one export. NULL on an ABI mismatch.
typedef const DxrWlApi *(*PFN_dxr_wl_lib_init)(const DxrWlHost *host);

#ifdef __cplusplus
}
#endif
