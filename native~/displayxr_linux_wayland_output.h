// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// A removed output's wl_output binding (displayxr_linux_wayland.c,
// registry_global_remove): kept, never destroyed, its user data marked removed (#408).
//
// The compositor sends wl_surface.leave for EVERY binding the client holds of an
// output that goes away, so also for ours to the player's own window surface.
// That event sits on the player's queue until SDL dispatches it on its main
// thread. If our binding is destroyed by then, libwayland hands SDL leave(NULL),
// and Unity's SDL reads the output's user data before checking it: SIGSEGV.
//
// No release is safe while the connection lives: we cannot see the player's
// queue drained, and its main thread can stall for seconds (a scene load), so a
// delay is only a guess. Nor does one buy anything: at version 2 wl_output has no
// destructor request, so the compositor keeps the resource until the connection
// closes and libwayland keeps a zombie for its id either way. So the binding stays
// for the life of the connection, one small proxy per removed output. Its user
// data is a marker: valid memory for SDL's handler (which reads it and then
// ignores a binding that is not its own), and nothing our listeners write into.
//
// Header-only so that the protocol test (tests/test_wayland_output_remove.c) runs
// the very same steps.

#pragma once

#include <wayland-client.h>

//! What a removed output's wl_output points its user data at.
static const char s_dxr_wl_removed_output = 0;

//! wl_registry.global_remove of an output we bound: keep the binding, marked removed.
//! Its other objects (the xdg-output) are the caller's to destroy.
static inline void
dxr_wl_output_retire(struct wl_output *output)
{
	wl_output_set_user_data(output, (void *)&s_dxr_wl_removed_output);
}

//! An output listener's user data, or NULL for a removed output's binding: its
//! events (none are expected) change nothing.
static inline void *
dxr_wl_output_live(void *data)
{
	return data == (const void *)&s_dxr_wl_removed_output ? NULL : data;
}
