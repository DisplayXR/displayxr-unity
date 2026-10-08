// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Native-Wayland player: the D-Bus client of the DisplayXR GNOME Shell extension
// (window-geometry@displayxr.org), for the window controls a Wayland client cannot
// do for itself. Part of libdisplayxr_unity_wayland.so; libdbus is dlopen'd.
//
// Every call blocks on a round trip to gnome-shell (500 ms timeout) and is
// thread-safe. They only ever act on this process's own window: the extension
// takes the caller's PID from the bus connection.

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

//! Move our window's frame to (x, y), logical stage px. 1 when moved.
int dxr_wl_ext_move_window(int x, int y);
//! Start a compositor-driven move that follows the pointer while `button` is
//! held. 1 when started.
int dxr_wl_ext_begin_pointer_drag(unsigned button);
void dxr_wl_ext_end_pointer_drag(void);
//! Our window's frame origin, logical stage px. 1 when known.
int dxr_wl_ext_frame_origin(int *out_x, int *out_y);

#ifdef __cplusplus
}
#endif
