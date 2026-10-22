// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Native-Wayland player (Linux, `-force-wayland`): the weave goes into a
// sub-surface of the player's own window. See displayxr_linux_wayland.c for the
// design and displayxr_xrprovider/displayxr_provider_wl_capture.cpp for how the
// player's Wayland window is found.
//
// The capture functions are in the main plugin. The weave and window-control
// functions are implemented in libdisplayxr_unity_wayland.so (the only binary
// that links libwayland-client); the main plugin's definitions of them are the
// forwarders in displayxr_linux_wayland_shim.c, which load that library on the
// first dxr_wl_weave_surface_create.
//
// Every entry point is inert (returns 0 / 1.0 / does nothing) unless the player
// runs natively on Wayland and the weave was bound to its window, so the X11,
// Windows and macOS paths never reach this code with any effect.

#pragma once

#include "displayxr_linux_wayland_lib.h" // DXR_WL_OWN_*

#ifdef __cplusplus
extern "C" {
#endif

struct wl_display;
struct wl_surface;

// --- Capture layer (displayxr_provider_wl_capture.cpp) ---------------------

//! Arm the capture layer from XRSDKPreInit, before Unity creates its real
//! VkInstance. No-op unless the player was started with -force-wayland.
void dxr_wl_capture_install(void);
//! Take the layer out of the environment again (idempotent). Called once Unity's
//! graphics device exists, and before the XR session creates the runtime's
//! instances. `why` is logged.
void dxr_wl_capture_disarm(const char *why);
//! The player was started with -force-wayland and capture is not disabled
//! (DXR_WL_CAPTURE_DISABLE=1).
int dxr_wl_capture_wanted(void);
//! The player was started with -force-wayland (a native-Wayland player), whether
//! or not the capture is disabled.
int dxr_wl_forced_wayland(void);
//! Warn (once) if `what`, a P/Invoke entry point, runs in a copy of the plugin
//! that did not arm the capture (Unity loads the library twice).
void dxr_wl_capture_note_caller(const char *what);
//! The build is marked as a transparent overlay app, so the player's window is
//! presented with alpha (DisplayXRManifestSettings; <Data>/DisplayXR/linux_player.json).
int dxr_wl_capture_transparent_window(void);

//! The player's window: its Wayland connection and surface. 1 when known.
int dxr_wl_unity_surface(struct wl_display **out_display, struct wl_surface **out_surface);

//! The player's window size in logical px (its swapchain extent). 1 when known.
int dxr_wl_unity_swapchain_size(int *out_w, int *out_h);

//! Bumps every time the player makes a new VkSurfaceKHR for its window (it does on
//! a resolution change, and may recreate the wl_surface itself).
unsigned dxr_wl_player_surface_generation(void);
//! 1 when the player's last window swapchain is presented with alpha (the capture
//! layer made it PRE_MULTIPLIED for a transparent overlay app), 0 when opaque.
int dxr_wl_player_presents_alpha(void);
//! Lock the player's window record and return its wl_surface (NULL while it has
//! none) and generation. Requests on that surface are safe until
//! dxr_wl_unlock_player_surface(): the capture layer forgets the surface under the
//! same lock before the player can destroy it. Always unlock, also on NULL. Lock
//! order: the Wayland library's own lock first, then this one; and never call the
//! other capture accessors while holding it.
struct wl_surface *dxr_wl_lock_player_surface(unsigned *out_generation);
void dxr_wl_unlock_player_surface(void);

// --- Weave sub-surface (libdisplayxr_unity_wayland.so, via the shim) -------

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
//! The pointer is over the player's window (its input region): 1, elsewhere 0, not known -1.
int dxr_wl_pointer_inside(void);

// --- Window controls (called from displayxr_linux.c) -----------------------

int dxr_wl_click_through_wanted(void);
//! The player's input region, rects layout-compatible with XRectangle, in
//! LOGICAL px; n < 0 resets it to the whole window.
int dxr_wl_set_player_input_region(const void *rects, int n);
int dxr_wl_begin_pointer_drag(unsigned button);
void dxr_wl_end_pointer_drag(void);
void dxr_wl_request_player_size(int device_w, int device_h);
int dxr_wl_take_player_size(int *out_w, int *out_h);
//! Put the window on the 3D panel's output (connector name; its mode in device px
//! only when exactly one output has it), centred. 1 when it is there or will be
//! once mapped; placement is asynchronous (the GNOME extension's worker).
int dxr_wl_move_player_to_panel(const char *connector, int panel_w, int panel_h);
//! The window's position / a new position in X root coordinates (the space the
//! X11 path saves and restores). get: 1 when known now; otherwise the out values
//! are the last known position (untouched if there never was one). set: a
//! position set just after start (a restored one) that does not lie on the 3D
//! panel is ignored and the window is centred on the panel instead (#361).
int dxr_wl_get_player_position_x11(int *out_x, int *out_y);
int dxr_wl_set_player_position_x11(int x, int y);
//! The runtime's 3D panel rect in X root px (XrDisplayDesktopInfoDXR): what X root
//! coordinates are converted through.
void dxr_wl_set_x11_panel_rect(const char *connector, int x, int y, int w, int h);
//! Bumps when the player recreated its window (the input region must be re-sent).
unsigned dxr_wl_player_generation(void);

//! What the DisplayXR GNOME extension (version 15) can do for the player's own
//! window right now, which a Wayland client cannot do for itself: DXR_WL_OWN_*
//! bits (displayxr_linux_wayland_lib.h). 0 without a native-Wayland session or
//! without the extension (X11 mode: the app's own EWMH calls work there).
int dxr_wl_own_window_caps(void);
//! Keep the window above other windows, or stop. It is state, so it is remembered
//! (also before the first session) and sent until the extension confirms it: once
//! the window is shown, again after a refusal (screen locked, the user dragging),
//! and whenever the window is shown anew (the extension came back, the player
//! recreated its window). 1 when the extension can do it now, 0 when it cannot
//! right now (remembered either way); the result is logged.
int dxr_wl_set_keep_above(int above);
//! Bring the window to the front with keyboard focus, restoring it if minimized
//! and switching to its workspace. 1 when the request was sent.
int dxr_wl_activate_window(void);
//! Minimize the window, or restore it (without focus: activate gives focus too).
//! 1 when the request was sent.
int dxr_wl_set_minimized(int minimized);
//! The window's state as the extension last reported it: DXR_WL_OWN_KEEP_ABOVE
//! when kept above, DXR_WL_OWN_MINIMIZE when minimized — whoever did it (the app,
//! the user from the dock or the window menu). -1 when not known (no
//! native-Wayland session, no extension, or one older than version 15).
int dxr_wl_window_state(void);

//! The player's window as an xdg-foreign handle, "wayland:<handle>", for another
//! process to attach a window to it: an xdg-desktop-portal dialog takes it as its
//! parent_window, so GNOME opens the dialog on top of the app's window. The
//! library exports the window itself, each time it sees it shown anew; this only
//! reads the result (no request to the compositor; it may wait briefly while a
//! session is starting, which holds the library's lock for one round trip). Writes it
//! to @p buf and returns 1; 0 (buf "") without a native-Wayland session, when the
//! compositor has no zxdg_exporter_v2, while the window is hidden or minimized,
//! and from the moment the compositor dropped the export until the window is
//! shown again (a frame or two after it is).
int dxr_wl_export_handle(char *buf, int len);

//! Files dragged onto the player's window (a file manager's drag and drop), for
//! the app. enable 1: bind our own wl_data_device on the player's connection;
//! 0: stop. What that does to SDL's clipboard (from the first time the player
//! loses the focus with drops on, SDL gets no clipboard offers for the rest of the
//! run, and the plug-in's clipboard below replaces it) is in
//! displayxr_linux_wayland.c, "File drops". Remembered
//! when asked before the first native-Wayland session (applied once the weave
//! binds the window). Returns 1 when drops are on now, 0 when remembered (or
//! turned off), -1 when unsupported: an X11 / XWayland player (XDND goes to the
//! window owner's X connection, Unity's), or a compositor without
//! wl_data_device_manager version 3.
int dxr_wl_file_drop_enable(int enable);
//! Takes the oldest drop event (DxrWlDropEvent, displayxr_linux_wayland_lib.h)
//! into @p event and its paths (UTF-8, each NUL-terminated, then one more NUL)
//! into @p paths. Returns the event's kind; 0 when there is none; -1 when
//! @p paths_len is smaller than event->paths_bytes (the event stays queued: call
//! again with that much room). Any thread; it reads a queue the frame thread fills.
int dxr_wl_file_drop_next(DxrWlDropEvent *event, char *paths, int paths_len);

//! The plug-in's own clipboard (displayxr_linux_clipboard.h): DXR_CLIP_STATE_* bits.
int dxr_wl_clipboard_state(void);
//! Reads the clipboard's text (UTF-8) into @p buf, NUL-terminated: its length, or a
//! DXR_CLIP_* error (DXR_CLIP_PENDING after @p timeout_ms: call again; DXR_CLIP_SPACE:
//! @p needed says how big). Never call it with a timeout on Unity's main thread.
int dxr_wl_clipboard_read(char *buf, int len, int timeout_ms, int *needed);
//! Puts @p len bytes of UTF-8 text on the clipboard (0: empties it): 1, or a DXR_CLIP_* error.
int dxr_wl_clipboard_write(const char *text, int len);

#ifdef __cplusplus
}
#endif
