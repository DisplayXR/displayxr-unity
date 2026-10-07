// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Native-Wayland player (Linux, `-force-wayland`): the weave goes into a
// sub-surface of the player's own window. See displayxr_linux_wayland.c for the
// design and displayxr_xrprovider/displayxr_provider_wl_capture.cpp for how the
// player's Wayland window is found.
//
// Every entry point is inert (returns 0 / 1.0 / does nothing) unless the player
// runs natively on Wayland and the weave was bound to its window, so the X11,
// Windows and macOS paths never reach this code with any effect.

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

struct wl_display;
struct wl_surface;

// --- Capture layer (displayxr_provider_wl_capture.cpp) ---------------------

//! Arm the capture layer from XRSDKPreInit, before Unity creates its real
//! VkInstance. No-op unless the player was started with -force-wayland.
void dxr_wl_capture_install(void);

//! The player's window: its Wayland connection and surface. 1 when known.
int dxr_wl_unity_surface(struct wl_display **out_display, struct wl_surface **out_surface);

//! The player's window size in logical px (its swapchain extent). 1 when known.
int dxr_wl_unity_swapchain_size(int *out_w, int *out_h);

//! Bumps every time the player makes a new VkSurfaceKHR for its window (it does on
//! a resolution change, and may recreate the wl_surface itself).
unsigned dxr_wl_player_surface_generation(void);

// --- Weave sub-surface (displayxr_linux_wayland.c) -------------------------

//! Create the weave sub-surface of the player's window (logical size lw x lh)
//! and return it with the device-pixel buffer size the runtime should present.
int dxr_wl_weave_surface_create(struct wl_display *display, struct wl_surface *parent, int lw, int lh,
                                struct wl_surface **out_surface, int *out_w, int *out_h);
//! Tear the weave down after xrDestroySession (the runtime's VkSurface is gone).
void dxr_wl_weave_destroy(void);
void dxr_wl_weave_set_transparent(int transparent);
//! Per frame (provider frame thread): follow the player's window size, scale and
//! recreation. 1, with the new device size, when the runtime must be told.
int dxr_wl_weave_poll(int *out_w, int *out_h);
//! The weave is bound to the player's window (native-Wayland session).
int dxr_wl_weave_active(void);
//! The weave buffer size, device px. 0 when not active.
int dxr_wl_weave_device_size(int *out_w, int *out_h);
//! Device px per logical px of the player's window; 1.0 when not active.
double dxr_wl_ui_scale(void);

// --- Window controls (called from displayxr_linux.c) -----------------------

int dxr_wl_click_through_wanted(void);
//! The player's input region, rects layout-compatible with XRectangle, in
//! LOGICAL px; n < 0 resets it to the whole window.
int dxr_wl_set_player_input_region(const void *rects, int n);
int dxr_wl_begin_pointer_drag(unsigned button);
void dxr_wl_end_pointer_drag(void);
void dxr_wl_request_player_size(int device_w, int device_h);
int dxr_wl_take_player_size(int *out_w, int *out_h);
//! Centre the window on the 3D panel's output (connector name, else its mode in
//! device px). 1 when it is on the panel (already, or moved).
int dxr_wl_move_player_to_panel(const char *connector, int panel_w, int panel_h);
//! The window's position / a new position in X root coordinates (the space the
//! X11 path saves and restores), converted through the XWayland scale.
int dxr_wl_get_player_position_x11(int *out_x, int *out_y);
int dxr_wl_set_player_position_x11(int x, int y);
//! Bumps when the player recreated its window (the input region must be re-sent).
unsigned dxr_wl_player_generation(void);

#ifdef __cplusplus
}
#endif
