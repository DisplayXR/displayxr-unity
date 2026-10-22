// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Native-Wayland player (`-force-wayland`): the weave goes into a sub-surface of
// the player's own window.
//
// Part of libdisplayxr_unity_wayland.so, which the main plugin dlopens only once
// it has found a native-Wayland player window (displayxr_linux_wayland_lib.h,
// displayxr_linux_wayland_shim.c), so that only this library links libwayland.
//
// The player's wl_display / wl_surface come from the main plugin's capture layer
// (displayxr_xrprovider/displayxr_provider_wl_capture.cpp). Everything here runs
// on the player's connection but on a PRIVATE event queue, so the player's SDL
// (which dispatches the default queue on its main thread) never sees our events
// and we never dispatch its. Wayland requests are thread-safe; the events for our
// objects land on our queue, dispatched only under s_wl_mutex: by the provider's
// frame thread, and once when the weave is created. Requests on the player's own
// surface are only made while the capture layer's lock is held (the player may
// destroy that surface at any time otherwise).
//
// Why a sub-surface: it moves with its parent and stacks above it, so a weave
// drawn into it covers the player's window without a second toplevel, and the
// compositor's window geometry for this process is the player's window, which is
// exactly where the weave is.
//
// Units: the player draws its window at LOGICAL size (buffer scale 1; Unity's
// Wayland window has no HiDPI support), but the weave must be 1:1 with the panel's
// DEVICE pixels. The runtime presents a device-sized buffer into our surface;
// wp_viewport maps it onto the logical window size, and wp_fractional_scale tells
// us the real (fractional) scale. The app-facing window-pixel API stays in device
// px as on X11 and Windows (displayxr_linux.c converts at its edges).
//
// Placement - moving the toplevel, which a Wayland client cannot do itself - goes
// through the DisplayXR GNOME Shell extension (displayxr_linux_wayland_dbus.c, on
// its own thread) in mutter's STAGE coordinates. Mutter places a window by its own
// rules at the moment it maps it (where the pointer is), which overrides any move
// made before, and the player maps its window after its first frame. So placement
// is an intent (where the window should be) enforced once each time the window
// gets mapped, as the extension reports it; any other move, by the user or by the
// compositor, is followed rather than fought.
//
// Two exceptions for the 3D panel (#361). A move there (DisplayXRTargetDisplay) is
// verified: each placement is read back a moment later (GetWindows) and, if the
// window's monitor is not the panel, retried a bounded number of times, until the
// user moves the window themselves. And a position the app restores right at start
// (a saved one, which may come from another monitor layout) is dropped in favour
// of the panel's centre when it does not lie on the panel.
//
// Stage coordinates depend on mutter's layout mode: LOGICAL (fractional scaling;
// GNOME 47+ default) or PHYSICAL (GNOME 46 / Ubuntu 24.04 at an integer scale:
// the stage is device px). xdg-output reports each output's layout rect, i.e. its
// stage rect, in both; the mode only changes how stage px relate to device px.
//
// Every entry point is inert unless a weave sub-surface exists.

#define _GNU_SOURCE // memfd_create
#include <wayland-client.h>
#include "viewporter-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"
#include "xdg-foreign-unstable-v2-client-protocol.h"

#include "displayxr_linux_wayland_dbus.h"
#include "displayxr_linux_wayland_lib.h"
#include "displayxr_linux_wayland_place.h"
#include "displayxr_linux_uri_list.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define DXR_WL_MAX_OUTPUTS 8 // more are not tracked: a window only on them gets no window handle ("")
#define DXR_WL_MAX_CORRECTIONS 3 // re-placements per map: never fight the compositor
#define DXR_WL_VERIFY_TRIES 3      // panel moves re-checked and retried (#361)
#define DXR_WL_VERIFY_DELAY_MS 350 // ... this long after each: ~1 s for all of them
#define DXR_WL_RESTORE_WINDOW_NS 3000000000LL // a saved position is checked until 3 s after the first map
#define DXR_WL_SIZE_SETTLE_NS 1000000000LL    // a size handed to the player is checked 1 s later

#define DXR_WL_DROP_QUEUE 32               // file-drop events kept for the app (oldest dropped past it)
#define DXR_WL_DROP_READ_MAX (1u << 20)    // a longer text/uri-list is not a file list we take
#define DXR_WL_DROP_READ_NS 3000000000LL   // the drag's source has this long to send it
#define DXR_WL_URI_LIST "text/uri-list"

enum { DXR_LAYOUT_UNKNOWN, DXR_LAYOUT_LOGICAL, DXR_LAYOUT_PHYSICAL };

//! A wl_data_offer of our data device (file drops) and what it offers.
struct dxr_wl_offer {
	struct wl_data_offer *offer;
	int uri_list;    // it offers text/uri-list
	uint32_t action; // the drag action the compositor chose (wl_data_offer.action); 0 none
};

//! A file-drop event waiting for the app (dxr_wl_file_drop_next).
struct dxr_wl_drop_ev {
	DxrWlDropEvent e;
	char *paths; // e.paths_bytes bytes (malloc'd); NULL for MOVE / LEAVE
};

// An output: its device-pixel mode, scale and transform (wl_output), and its
// stage rect and connector name (xdg-output), to find the 3D panel and place the
// window on it.
struct dxr_wl_output {
	uint32_t registry_name; // 0 = free slot
	struct wl_output *output;
	struct zxdg_output_v1 *xdg;
	int mode_w, mode_h;
	int wl_scale;  // wl_output.scale (integer)
	int transform; // wl_output_transform; odd = rotated by 90 or 270
	int lx, ly, lw, lh;
	char name[32];
};

static struct {
	struct wl_display *display; // the player's connection (not ours to close)
	struct wl_event_queue *queue;
	struct wl_display *display_wrapper; // the display, on our queue
	struct wl_registry *registry;
	struct wl_compositor *compositor;
	struct wl_subcompositor *subcompositor;
	struct wl_shm *shm;
	struct wp_viewporter *viewporter;
	struct wp_fractional_scale_manager_v1 *frac_manager;
	// Our own wl_pointer on this queue, for the depth cursor: enter/leave of the player's
	// window, i.e. its input region (outside it, the cursor of whatever lies behind shows).
	// -1 not known yet, 0 the pointer is elsewhere, 1 over the window.
	struct wl_seat *seat;
	uint32_t seat_caps;
	struct wl_pointer *pointer;
	int pointer_state;
	struct zxdg_output_manager_v1 *xdg_output_manager;
	struct dxr_wl_output outputs[DXR_WL_MAX_OUTPUTS];
	// The outputs our weave sub-surface is on, one bit per outputs[] slot
	// (wl_surface enter/leave). Only for OUR wl_output proxies: the compositor
	// sends enter/leave once per wl_output the client bound, SDL's included.
	// Non-zero only while the weave is mapped, i.e. while the player's window is
	// shown (a sub-surface is mapped only while its parent is).
	unsigned weave_on;
	unsigned weave_shown_seq; // bumps when weave_on goes from none to an output

	// The player's window exported for other processes (export_update_locked),
	// read by dxr_wl_export_handle.
	struct zxdg_exporter_v2 *exporter;
	struct zxdg_importer_v2 *importer;
	struct zxdg_exported_v2 *exported;
	struct zxdg_imported_v2 *export_watch; // our own import of the handle: told when it dies
	int export_dead;                       // export_watch said the compositor dropped it
	unsigned exported_weave_seq;           // weave_shown_seq the export is current for
	unsigned exported_ext_seq;             // ext_shown_seq the export is current for
	char exported_handle[128];             // "" = no handle (yet)
	unsigned ext_shown_seq; // bumps when the extension reports our window shown anew

	// The weave (NULL surface = no native-Wayland session).
	struct wl_surface *surface;
	struct wl_surface *parent; // the player's window it is a sub-surface of
	unsigned parent_generation; // host player_surface_generation() it was attached at
	unsigned attach_count;      // bumps per (re-)attach: the input region is re-sent
	struct wl_subsurface *subsurface;
	struct wp_viewport *viewport;
	struct wp_fractional_scale_v1 *frac;
	struct wl_buffer *probe_buffer;
	int transparent;
	int opaque_cleared; // logged that the player's opaque region is being cleared

	uint32_t scale120;        // preferred scale x 120, 0 = not told yet
	// The scale sizes are computed for while it is ahead of scale120: the target
	// output's while the window is being placed there, then the scale the extension
	// reports for the window's monitor, until the fractional-scale protocol (which
	// only follows once the window is there) catches up.
	uint32_t expect_scale120;
	int logical_w, logical_h;
	int device_w, device_h;
	int geometry_dirty; // device size changed since the provider last read it

	// Window size the app asked for (displayxr_resize_overlay), DEVICE px, and the
	// LOGICAL size still to apply (C# Screen.SetResolution).
	int wanted_device_w, wanted_device_h;
	int wanted_by_app; // the app asked for it (else it is the baseline, below)
	int pending_w, pending_h;
	int sent_w, sent_h; // the last size handed to C#, until the player's window has it
	int size_fixups;    // re-requests of a size the player reverted, since the app's request
	// The last size handed to C#, checked once the player has had time to apply it:
	// a size it drops or reverts inside one frame never shows in the poll, and
	// sent_w would then say "resize in progress" for good.
	int check_w, check_h;
	long long check_ns;

	// The DisplayXR GNOME extension, and our window as it last reported it.
	int ext_available;
	int ext_known; // the worker has reported the extension's presence at least once
	unsigned ext_caps;
	char ext_layout[16]; // "logical", "physical", "" (not reported)
	DxrWlExtWindow win;
	int drag_state; // 0 none, 1 asked for, 2 dragging
	// Keep-above as the app asked for it (dxr_wl_set_keep_above). It is window STATE,
	// not an action: kept until the extension confirms it (keep_above_send_locked).
	int keep_above_asked;        // the app asked at all (never touch it otherwise)
	int keep_above_wanted;       // what it asked for
	int keep_above_ok;           // the extension confirmed keep_above_wanted
	unsigned keep_above_seq;     // the request last sent; answers to older ones are ignored
	int keep_above_inflight;     // keep_above_seq is not answered yet
	int keep_above_redo;         // the window was shown anew meanwhile: that answer does not count
	int keep_above_logged;       // "done" was logged for keep_above_wanted: re-confirmations are quiet
	int keep_above_refusals;     // in a row: the retry delay; only the first is logged
	long long keep_above_due_ns; // not before (mono_ns); 0 = at the next chance

	// Where the window should be, stage px: where we put it, or where the user (or
	// the compositor, uninvited) moved it since. Enforced when the window gets mapped.
	int intent_valid, intent_x, intent_y;
	char intent_centred_on[32]; // the panel the intent centres the window on, "" if none
	int move_inflight; // we asked the extension to move it; its answer is pending
	int corrections;   // moves since the window was last mapped
	int placing;       // from a correction until its result: scale changes wait
	int rescale_after_place;
	// The window does not fit at the intent yet (it was mapped at a stale size,
	// or is being resized): keep the intent and move it there once its size
	// changes, rather than accept where the compositor keeps it.
	int intent_blocked, blocked_w, blocked_h;

	// Placement verification (#361): the window was sent to the 3D panel (output
	// named verify_panel); after each move, read it back a little later and move
	// it again if its monitor is not the panel, DXR_WL_VERIFY_TRIES times at most.
	// Cleared when it is there, when the user moves it, or when the app moves it
	// off the panel on purpose.
	char verify_panel[32];
	int verify_tries;
	int verify_scheduled;

	// A saved position the app restores (displayxr_set_overlay_position) is
	// checked against the 3D panel until the window has been on screen a moment
	// and the user has not moved it (#361): restore_open, first_map_ns.
	int restore_open;
	long long first_map_ns;
	int drag_end_pending; // log where the user left the window, once reported

	// The 3D panel as the runtime reports it in X root coordinates: the space the
	// app-facing position API speaks (displayxr_get/set_overlay_position).
	char x11_connector[64];
	int x11_valid, x11_x, x11_y, x11_w, x11_h;
	int last_x11_valid, last_x11_x, last_x11_y; // the last position we could convert

	// File drops (see "File drops" below): our own data device for the seat, made
	// only once the app asks (dxr_wl_file_drop_enable), and the drag over us.
	struct wl_data_device_manager *ddm;
	uint32_t ddm_version;
	struct wl_data_device *ddev;
	struct wl_keyboard *keyboard; // tells when the player loses keyboard focus
	int kb_focus;                 // the player has keyboard focus (as last dispatched)
	int drop_wanted;              // the app asked for drops
	int drop_remake;              // make the device again once unfocused (it lost focus)
	unsigned drop_makes;          // devices made this run (logs)
	struct dxr_wl_offer *sel_offer; // the clipboard offer sent to our device (unused, kept as the protocol asks)
	struct {
		struct dxr_wl_offer *o; // the drag's offer; NULL = no drag over the window
		uint32_t serial;        // its enter's
		int files;              // taken: text/uri-list over the player's own surface
		double lx, ly;          // the pointer, surface-local logical px
		int reading, fd;        // the list on its way through a pipe
		char *buf;
		size_t len, cap;
		long long deadline_ns;
		int read_done;
		char *paths; // dxr_uri_list_to_paths' layout, once read
		size_t paths_bytes;
		int path_count;
		int told;    // the app had its ENTER
		int dropped; // dropped; finished once the list is in
	} drag;
	struct dxr_wl_drop_ev drop_q[DXR_WL_DROP_QUEUE];
	int drop_head, drop_count;

	unsigned warned; // one-time log bits (DXR_WARN_*)
} s_wl;

#define DXR_WARN_LAYOUT 1u
#define DXR_WARN_X11 2u
#define DXR_WARN_DRAG 4u
#define DXR_WARN_GAVE_UP 8u
#define DXR_WARN_OWN_KEEP_ABOVE 16u
#define DXR_WARN_OWN_ACTIVATE 32u
#define DXR_WARN_OWN_MINIMIZE 64u
#define DXR_WARN_DROP_NOT_FILES 128u
#define DXR_WARN_DROP_QUEUE 256u
#define DXR_WARN_DROP_UNSUPPORTED 512u
#define DXR_WARN_DROP_NO_KEYBOARD 1024u

// Guards s_wl: the provider's frame thread polls, C#'s main thread asks for
// resizes, positions and input regions, the extension worker reports. Lock order:
// this one, then the capture layer's player lock, then the worker's job lock.
static pthread_mutex_t s_wl_mutex = PTHREAD_MUTEX_INITIALIZER;
// Under s_wl_mutex: the move being placed is a side-step glide's in-between step,
// which is not logged (the side-step logs the move once; dxr_wl_set_player_position_stage).
static int s_quiet_place;

// The main plugin's side (displayxr_linux_wayland_lib.h), set once by dxr_wl_lib_init.
static const DxrWlHost *s_host;

/*
 *
 * Outputs.
 *
 */

static void
out_geometry(void *d, struct wl_output *o, int32_t x, int32_t y, int32_t pw, int32_t ph, int32_t sub,
             const char *make, const char *model, int32_t transform)
{
	(void)o; (void)x; (void)y; (void)pw; (void)ph; (void)sub; (void)make; (void)model;
	struct dxr_wl_output *out = d;
	out->transform = transform;
}

static void
out_mode(void *d, struct wl_output *o, uint32_t flags, int32_t w, int32_t h, int32_t refresh)
{
	(void)o; (void)refresh;
	struct dxr_wl_output *out = d;
	if (flags & WL_OUTPUT_MODE_CURRENT) {
		out->mode_w = w;
		out->mode_h = h;
	}
}

static void out_done(void *d, struct wl_output *o) { (void)d; (void)o; }

static void
out_scale(void *d, struct wl_output *o, int32_t factor)
{
	(void)o;
	struct dxr_wl_output *out = d;
	out->wl_scale = factor;
}

static const struct wl_output_listener s_output_listener = {
    .geometry = out_geometry,
    .mode = out_mode,
    .done = out_done,
    .scale = out_scale,
};

static void
xo_position(void *d, struct zxdg_output_v1 *x, int32_t px, int32_t py)
{
	(void)x;
	struct dxr_wl_output *out = d;
	out->lx = px;
	out->ly = py;
}

static void
xo_size(void *d, struct zxdg_output_v1 *x, int32_t w, int32_t h)
{
	(void)x;
	struct dxr_wl_output *out = d;
	out->lw = w;
	out->lh = h;
}

static void xo_done(void *d, struct zxdg_output_v1 *x) { (void)d; (void)x; }

static void
xo_name(void *d, struct zxdg_output_v1 *x, const char *name)
{
	(void)x;
	struct dxr_wl_output *out = d;
	snprintf(out->name, sizeof(out->name), "%s", name ? name : "");
}

static void xo_description(void *d, struct zxdg_output_v1 *x, const char *n) { (void)d; (void)x; (void)n; }

static const struct zxdg_output_v1_listener s_xdg_output_listener = {
    .logical_position = xo_position,
    .logical_size = xo_size,
    .done = xo_done,
    .name = xo_name,
    .description = xo_description,
};

static void
output_add_xdg(struct dxr_wl_output *out)
{
	if (out->xdg || !s_wl.xdg_output_manager)
		return;
	out->xdg = zxdg_output_manager_v1_get_xdg_output(s_wl.xdg_output_manager, out->output);
	zxdg_output_v1_add_listener(out->xdg, &s_xdg_output_listener, out);
}

//! The output's mode in its layout orientation (rotated by 90/270: swapped).
static void
output_mode_oriented(const struct dxr_wl_output *o, int *w, int *h)
{
	int rotated = o->transform & 1;
	*w = rotated ? o->mode_h : o->mode_w;
	*h = rotated ? o->mode_w : o->mode_h;
}

//! Mutter's layout mode: what the extension reports, cross-checked with what the
//! outputs show (an output at a scale above 1 whose stage rect is its whole mode is
//! in physical layout; one whose rect is the mode over its scale, logical).
static int
layout_locked(void)
{
	int inferred = DXR_LAYOUT_UNKNOWN, any_scaled = 0;
	for (int i = 0; i < DXR_WL_MAX_OUTPUTS; i++) {
		const struct dxr_wl_output *o = &s_wl.outputs[i];
		if (!o->registry_name || o->lw <= 0 || o->mode_w <= 0)
			continue;
		int mw, mh;
		output_mode_oriented(o, &mw, &mh);
		double ratio = (double)mw / o->lw;
		if (o->wl_scale <= 1 && ratio < 1.01)
			continue; // at scale 1 both layouts agree
		any_scaled = 1;
		int mode = ratio < 1.01 ? DXR_LAYOUT_PHYSICAL : DXR_LAYOUT_LOGICAL;
		if (inferred == DXR_LAYOUT_UNKNOWN)
			inferred = mode;
		else if (inferred != mode)
			return DXR_LAYOUT_UNKNOWN; // the outputs disagree
	}
	if (!any_scaled)
		inferred = DXR_LAYOUT_LOGICAL;
	int reported = !strcmp(s_wl.ext_layout, "logical")    ? DXR_LAYOUT_LOGICAL
	               : !strcmp(s_wl.ext_layout, "physical") ? DXR_LAYOUT_PHYSICAL
	                                                       : DXR_LAYOUT_UNKNOWN;
	if (reported != DXR_LAYOUT_UNKNOWN && inferred != DXR_LAYOUT_UNKNOWN && reported != inferred)
		return DXR_LAYOUT_UNKNOWN;
	return reported != DXR_LAYOUT_UNKNOWN ? reported : inferred;
}

//! The output's device px per stage px, x 120; 0 when the layout is unknown.
static uint32_t
output_scale120_locked(const struct dxr_wl_output *o, int layout)
{
	int mw, mh;
	output_mode_oriented(o, &mw, &mh);
	if (layout == DXR_LAYOUT_LOGICAL && o->lw > 0)
		return (uint32_t)lround(120.0 * mw / o->lw);
	if (layout == DXR_LAYOUT_PHYSICAL)
		return 120u * (uint32_t)(o->wl_scale > 0 ? o->wl_scale : 1);
	return 0;
}

//! The output whose stage rect contains (x, y), or NULL.
static struct dxr_wl_output *
output_at_locked(int x, int y)
{
	for (int i = 0; i < DXR_WL_MAX_OUTPUTS; i++) {
		struct dxr_wl_output *o = &s_wl.outputs[i];
		if (o->registry_name && o->lw > 0 && x >= o->lx && x < o->lx + o->lw && y >= o->ly && y < o->ly + o->lh)
			return o;
	}
	return NULL;
}

//! The output with this connector name, or NULL.
static struct dxr_wl_output *
output_named_locked(const char *name)
{
	for (int i = 0; i < DXR_WL_MAX_OUTPUTS && name && *name; i++)
		if (s_wl.outputs[i].registry_name && !strcmp(s_wl.outputs[i].name, name))
			return &s_wl.outputs[i];
	return NULL;
}

/*
 *
 * Registry.
 *
 */

static void
ptr_enter(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y)
{
	(void)d; (void)p; (void)serial; (void)surface; (void)x; (void)y;
	s_wl.pointer_state = 1; // only our own surfaces send us enter
}

static void
ptr_leave(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *surface)
{
	(void)d; (void)p; (void)serial; (void)surface;
	s_wl.pointer_state = 0;
}

static void
ptr_motion(void *d, struct wl_pointer *p, uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
	(void)d; (void)p; (void)time; (void)x; (void)y;
}

static void
ptr_button(void *d, struct wl_pointer *p, uint32_t serial, uint32_t time, uint32_t button, uint32_t state)
{
	(void)d; (void)p; (void)serial; (void)time; (void)button; (void)state;
}

static void
ptr_axis(void *d, struct wl_pointer *p, uint32_t time, uint32_t axis, wl_fixed_t value)
{
	(void)d; (void)p; (void)time; (void)axis; (void)value;
}

// wl_seat version 1: only these five pointer events are ever sent.
static const struct wl_pointer_listener s_pointer_listener = {
    .enter = ptr_enter,
    .leave = ptr_leave,
    .motion = ptr_motion,
    .button = ptr_button,
    .axis = ptr_axis,
};

static void drop_keyboard_locked(void);

static void
seat_capabilities(void *d, struct wl_seat *seat, uint32_t caps)
{
	(void)d;
	s_wl.seat_caps = caps;
	drop_keyboard_locked(); // file drops follow the keyboard focus, if they are on
	if ((caps & WL_SEAT_CAPABILITY_POINTER) && !s_wl.pointer) {
		s_wl.pointer = wl_seat_get_pointer(seat);
		wl_pointer_add_listener(s_wl.pointer, &s_pointer_listener, NULL);
	} else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && s_wl.pointer) {
		wl_pointer_destroy(s_wl.pointer);
		s_wl.pointer = NULL;
		s_wl.pointer_state = -1;
	}
}

static void
seat_name(void *d, struct wl_seat *seat, const char *name)
{
	(void)d; (void)seat; (void)name;
}

static const struct wl_seat_listener s_seat_listener = {
    .capabilities = seat_capabilities,
    .name = seat_name,
};

static void
registry_global(void *data, struct wl_registry *reg, uint32_t name, const char *iface, uint32_t version)
{
	(void)data;
	if (strcmp(iface, wl_compositor_interface.name) == 0) {
		s_wl.compositor = wl_registry_bind(reg, name, &wl_compositor_interface, version < 4 ? version : 4);
	} else if (strcmp(iface, wl_subcompositor_interface.name) == 0) {
		s_wl.subcompositor = wl_registry_bind(reg, name, &wl_subcompositor_interface, 1);
	} else if (strcmp(iface, wl_shm_interface.name) == 0) {
		s_wl.shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
	} else if (strcmp(iface, wl_seat_interface.name) == 0 && !s_wl.seat) {
		s_wl.pointer_state = -1;
		s_wl.seat = wl_registry_bind(reg, name, &wl_seat_interface, 1);
		wl_seat_add_listener(s_wl.seat, &s_seat_listener, NULL);
	} else if (strcmp(iface, wp_viewporter_interface.name) == 0) {
		s_wl.viewporter = wl_registry_bind(reg, name, &wp_viewporter_interface, 1);
	} else if (strcmp(iface, wp_fractional_scale_manager_v1_interface.name) == 0) {
		s_wl.frac_manager = wl_registry_bind(reg, name, &wp_fractional_scale_manager_v1_interface, 1);
	} else if (strcmp(iface, wl_data_device_manager_interface.name) == 0 && !s_wl.ddm) {
		// Bound, but no data device made until the app asks for file drops: the
		// device is what changes things for SDL (see "File drops").
		s_wl.ddm_version = version < 3 ? version : 3;
		s_wl.ddm = wl_registry_bind(reg, name, &wl_data_device_manager_interface, s_wl.ddm_version);
	} else if (strcmp(iface, zxdg_exporter_v2_interface.name) == 0) {
		s_wl.exporter = wl_registry_bind(reg, name, &zxdg_exporter_v2_interface, 1);
	} else if (strcmp(iface, zxdg_importer_v2_interface.name) == 0) {
		s_wl.importer = wl_registry_bind(reg, name, &zxdg_importer_v2_interface, 1);
	} else if (strcmp(iface, zxdg_output_manager_v1_interface.name) == 0) {
		s_wl.xdg_output_manager =
		    wl_registry_bind(reg, name, &zxdg_output_manager_v1_interface, version < 3 ? version : 3);
		for (int i = 0; i < DXR_WL_MAX_OUTPUTS; i++)
			if (s_wl.outputs[i].registry_name)
				output_add_xdg(&s_wl.outputs[i]);
	} else if (strcmp(iface, wl_output_interface.name) == 0) {
		for (int i = 0; i < DXR_WL_MAX_OUTPUTS; i++) {
			struct dxr_wl_output *out = &s_wl.outputs[i];
			if (out->registry_name)
				continue;
			memset(out, 0, sizeof(*out));
			out->registry_name = name;
			out->wl_scale = 1;
			out->output = wl_registry_bind(reg, name, &wl_output_interface, version < 2 ? version : 2);
			wl_output_add_listener(out->output, &s_output_listener, out);
			output_add_xdg(out);
			break;
		}
	}
}

static void
registry_global_remove(void *data, struct wl_registry *reg, uint32_t name)
{
	(void)data;
	(void)reg;
	for (int i = 0; i < DXR_WL_MAX_OUTPUTS; i++) {
		struct dxr_wl_output *out = &s_wl.outputs[i];
		if (out->registry_name != name)
			continue;
		s_wl.weave_on &= ~(1u << i); // its leave can no longer reach us
		if (out->xdg)
			zxdg_output_v1_destroy(out->xdg);
		wl_output_destroy(out->output);
		memset(out, 0, sizeof(*out));
	}
}

static const struct wl_registry_listener s_registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

//! Bind the globals we need on the player's connection, on our own queue.
static int
connect_locked(struct wl_display *display)
{
	if (s_wl.registry)
		return s_wl.display == display && s_wl.compositor && s_wl.subcompositor;
	s_wl.display = display;
	s_wl.queue = wl_display_create_queue(display);
	s_wl.display_wrapper = wl_proxy_create_wrapper(display);
	wl_proxy_set_queue((struct wl_proxy *)s_wl.display_wrapper, s_wl.queue);
	s_wl.registry = wl_display_get_registry(s_wl.display_wrapper);
	wl_registry_add_listener(s_wl.registry, &s_registry_listener, NULL);
	// Twice: the globals, then the outputs' mode / xdg-output events.
	if (wl_display_roundtrip_queue(display, s_wl.queue) < 0 || wl_display_roundtrip_queue(display, s_wl.queue) < 0) {
		fprintf(stderr, "[DisplayXR-WL] roundtrip on the player's connection failed\n");
		return 0;
	}
	for (int i = 0; i < DXR_WL_MAX_OUTPUTS; i++) {
		const struct dxr_wl_output *o = &s_wl.outputs[i];
		if (o->registry_name)
			fprintf(stderr, "[DisplayXR-WL] output '%s': %dx%d px (scale %d, transform %d), stage %dx%d at (%d,%d)\n",
			        o->name, o->mode_w, o->mode_h, o->wl_scale, o->transform, o->lw, o->lh, o->lx, o->ly);
	}
	return s_wl.compositor && s_wl.subcompositor;
}

/*
 *
 * The weave sub-surface and its size.
 *
 */

static void
update_device_size_locked(void)
{
	double scale = s_wl.scale120 ? s_wl.scale120 / 120.0 : 1.0;
	int dw = (int)lround(s_wl.logical_w * scale);
	int dh = (int)lround(s_wl.logical_h * scale);
	if (dw != s_wl.device_w || dh != s_wl.device_h) {
		s_wl.device_w = dw;
		s_wl.device_h = dh;
		s_wl.geometry_dirty = 1;
		fprintf(stderr, "[DisplayXR-WL] weave: window %dx%d logical x %.4f -> buffer %dx%d px\n", s_wl.logical_w,
		        s_wl.logical_h, scale, dw, dh);
	}
}

static struct dxr_wl_output *output_at_locked(int x, int y);
static int layout_locked(void);

//! Fit a logical window size into 95 % of the output the window is going to (the
//! intent's) or else is on, keeping its aspect.
static void
clamp_to_destination_locked(int *lw, int *lh)
{
	struct dxr_wl_output *o = NULL;
	if (s_wl.intent_valid)
		o = output_at_locked(s_wl.intent_x, s_wl.intent_y);
	if (!o && s_wl.win.present && s_wl.win.w > 0)
		o = output_at_locked(s_wl.win.x + s_wl.win.w / 2, s_wl.win.y + s_wl.win.h / 2);
	if (!o || o->lw <= 0 || o->lh <= 0 || *lw <= 0 || *lh <= 0)
		return;
	// Stage px per logical (surface) px: 1 in logical layout, the scale in physical.
	double stage = 1.0;
	if (layout_locked() == DXR_LAYOUT_PHYSICAL)
		stage = o->wl_scale > 0 ? o->wl_scale : 1;
	double max_w = 0.95 * o->lw / stage, max_h = 0.95 * o->lh / stage;
	double f = 1.0;
	if (*lw > max_w)
		f = max_w / *lw;
	if (*lh * f > max_h)
		f = max_h / *lh;
	if (f < 1.0) {
		*lw = (int)floor(*lw * f);
		*lh = (int)floor(*lh * f);
	}
}

//! The size to keep, in device px, is the app's request; until it makes one, the
//! window's size where its scale was first known. Moving to an output with another
//! scale then keeps the device size, as on X11, not the logical one (which would
//! grow a window 4/3 moving from a 150 % laptop onto a 200 % panel).
static void
baseline_wanted_size_locked(void)
{
	if (s_wl.wanted_device_w <= 0 && s_wl.scale120 && s_wl.device_w > 0 && s_wl.device_h > 0) {
		s_wl.wanted_device_w = s_wl.device_w;
		s_wl.wanted_device_h = s_wl.device_h;
	}
}

// The window changed scale (e.g. from the laptop onto the 3D panel): keep the size
// wanted in DEVICE px, as on X11, by asking for the logical size that gives it at
// the new scale.
static void
rerequest_player_size_locked(void)
{
	if (s_wl.wanted_device_w <= 0 || s_wl.wanted_device_h <= 0)
		return;
	// Being moved to another output: size for its scale now, not for the one being
	// left (the compositor reports the new scale only once the window is there).
	uint32_t s120 = s_wl.expect_scale120 ? s_wl.expect_scale120 : s_wl.scale120;
	double scale = s120 ? s120 / 120.0 : 1.0;
	int lw = (int)lround(s_wl.wanted_device_w / scale);
	int lh = (int)lround(s_wl.wanted_device_h / scale);
	// Never bigger than the output it is going to (or is on): a window the
	// compositor cannot fit gets squeezed or maximized, and the player then resets
	// it to the desktop size - and saves that size for its next start.
	clamp_to_destination_locked(&lw, &lh);
	if (lw == s_wl.logical_w && lh == s_wl.logical_h)
		return; // already that size: a no-op resize would still re-map the window
	s_wl.pending_w = lw;
	s_wl.pending_h = lh;
}

static void
frac_preferred_scale(void *data, struct wp_fractional_scale_v1 *frac, uint32_t scale)
{
	(void)data;
	(void)frac;
	// The protocol caught up with the scale we expected (see expect_scale120).
	if (scale == s_wl.expect_scale120)
		s_wl.expect_scale120 = 0;
	if (scale == s_wl.scale120)
		return;
	int first = !s_wl.scale120;
	s_wl.scale120 = scale;
	update_device_size_locked();
	if (first)
		baseline_wanted_size_locked();
	if (s_wl.placing)
		s_wl.rescale_after_place = 1;
	else
		rerequest_player_size_locked();
}

static const struct wp_fractional_scale_v1_listener s_frac_listener = {
    .preferred_scale = frac_preferred_scale,
};

//! A 1x1 fully transparent shm buffer: maps the sub-surface before the session so
//! the compositor places it on an output and reports the scale.
static struct wl_buffer *
make_probe_buffer(void)
{
	if (!s_wl.shm)
		return NULL;
	int fd = memfd_create("dxr-wl-probe", MFD_CLOEXEC);
	if (fd < 0)
		return NULL;
	if (ftruncate(fd, 4) < 0) { // one zeroed ARGB8888 pixel
		close(fd);
		return NULL;
	}
	struct wl_shm_pool *pool = wl_shm_create_pool(s_wl.shm, fd, 4);
	struct wl_buffer *buf = wl_shm_pool_create_buffer(pool, 0, 1, 1, 4, WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	return buf;
}

_Static_assert(DXR_WL_MAX_OUTPUTS <= 32, "weave_on has one bit per output slot");

//! The outputs[] slot of one of OUR wl_output proxies, or -1: another of the
//! client's (SDL's), or NULL (one destroyed meanwhile).
static int
output_slot(const struct wl_output *output)
{
	if (!output)
		return -1;
	for (int i = 0; i < DXR_WL_MAX_OUTPUTS; i++)
		if (s_wl.outputs[i].registry_name && s_wl.outputs[i].output == output)
			return i;
	return -1;
}

static void
weave_enter(void *data, struct wl_surface *surface, struct wl_output *output)
{
	(void)data;
	(void)surface;
	int i = output_slot(output);
	if (i < 0)
		return;
	if (!s_wl.weave_on)
		s_wl.weave_shown_seq++; // on screen again: the window was shown anew
	s_wl.weave_on |= 1u << i;
}

static void
weave_leave(void *data, struct wl_surface *surface, struct wl_output *output)
{
	(void)data;
	(void)surface;
	int i = output_slot(output);
	if (i >= 0)
		s_wl.weave_on &= ~(1u << i);
}

//! enter / leave only: the compositor is bound at version 4 or lower.
static const struct wl_surface_listener s_weave_surface_listener = {
    .enter = weave_enter,
    .leave = weave_leave,
};

//! Make our surface a sub-surface of `parent`, at its top-left, above it. The
//! caller holds the player lock (parent is the player's surface).
static void
attach_to_parent_locked(struct wl_surface *parent)
{
	if (s_wl.subsurface)
		wl_subsurface_destroy(s_wl.subsurface);
	s_wl.subsurface = wl_subcompositor_get_subsurface(s_wl.subcompositor, s_wl.surface, parent);
	wl_subsurface_set_position(s_wl.subsurface, 0, 0);
	wl_subsurface_place_above(s_wl.subsurface, parent);
	// Desync: the runtime's presents show at once, without waiting for the
	// player's own commits.
	wl_subsurface_set_desync(s_wl.subsurface);
	s_wl.parent = parent;
	s_wl.attach_count++;
}

static const DxrWlExtCallbacks s_ext_callbacks; // below
static int win_mapped(const DxrWlExtWindow *w);
static void keep_above_send_locked(void);
static void keep_above_reset_locked(void);
static long long mono_ns(void);
static void stop_verify_locked(void);
static void schedule_verify_locked(void);
static void fix_player_size_locked(void);
static void export_update_locked(void);
static void export_drop_locked(void);
static int drop_supported_locked(void);
static void drop_device_make_locked(const char *why, int first);
static void drop_device_destroy_locked(void);
static void drop_poll_locked(void);

static int
dxr_wl_weave_surface_create(struct wl_display *display, struct wl_surface *parent, int lw, int lh,
                            struct wl_surface **out_surface, int *out_w, int *out_h)
{
	if (!display || !parent || lw <= 0 || lh <= 0)
		return 0;
	pthread_mutex_lock(&s_wl_mutex);
	int ok = connect_locked(display);
	if (ok && !s_wl.viewporter) {
		fprintf(stderr, "[DisplayXR-WL] the compositor has no wp_viewporter: cannot weave 1:1\n");
		ok = 0;
	}
	if (!ok) {
		pthread_mutex_unlock(&s_wl_mutex);
		return 0;
	}
	// The player's surface may have changed since the provider read it: use the
	// current one, and only while it is locked.
	unsigned generation = 0;
	struct wl_surface *player = s_host->lock_player_surface(&generation);
	if (!player) {
		s_host->unlock_player_surface();
		pthread_mutex_unlock(&s_wl_mutex);
		fprintf(stderr, "[DisplayXR-WL] the player has no window surface right now: no weave\n");
		return 0;
	}
	if (player != parent)
		fprintf(stderr, "[DisplayXR-WL] the player's window surface changed since it was read: using the current one\n");
	s_wl.surface = wl_compositor_create_surface(s_wl.compositor);
	s_wl.weave_on = 0;
	wl_surface_add_listener(s_wl.surface, &s_weave_surface_listener, NULL);
	attach_to_parent_locked(player);
	s_wl.parent_generation = generation;
	s_host->unlock_player_surface();
	keep_above_reset_locked(); // the player may have a new window since the last session
	// Pointer input goes through to the player's window underneath.
	struct wl_region *empty = wl_compositor_create_region(s_wl.compositor);
	wl_surface_set_input_region(s_wl.surface, empty);
	wl_region_destroy(empty);
	s_wl.viewport = wp_viewporter_get_viewport(s_wl.viewporter, s_wl.surface);
	if (s_wl.frac_manager) {
		s_wl.frac = wp_fractional_scale_manager_v1_get_fractional_scale(s_wl.frac_manager, s_wl.surface);
		wp_fractional_scale_v1_add_listener(s_wl.frac, &s_frac_listener, NULL);
	}
	s_wl.logical_w = lw;
	s_wl.logical_h = lh;
	wp_viewport_set_destination(s_wl.viewport, lw, lh);

	// Map it once (before the session: from then on only the runtime's WSI attaches
	// and commits) so the compositor reports the scale of the output it is on. One
	// round trip, no waiting: if the scale is not in yet, start from the scale of the
	// window's monitor as the extension last reported it (or 1), and correct it when
	// it arrives (the provider republishes the geometry).
	s_wl.probe_buffer = make_probe_buffer();
	if (s_wl.probe_buffer) {
		wl_surface_attach(s_wl.surface, s_wl.probe_buffer, 0, 0);
		wl_surface_damage_buffer(s_wl.surface, 0, 0, 1, 1);
	}
	wl_surface_commit(s_wl.surface);
	wl_display_roundtrip_queue(display, s_wl.queue);
	if (!s_wl.scale120) {
		if (s_wl.win.device_scale > 0.0)
			s_wl.scale120 = (uint32_t)lround(120.0 * s_wl.win.device_scale);
		fprintf(stderr, "[DisplayXR-WL] no fractional scale yet; starting at %.4f until it arrives\n",
		        s_wl.scale120 ? s_wl.scale120 / 120.0 : 1.0);
	}
	update_device_size_locked();
	baseline_wanted_size_locked();
	// A new session: a saved position the app restores now is checked (#361).
	s_wl.restore_open = 1;
	s_wl.first_map_ns = win_mapped(&s_wl.win) ? mono_ns() : 0;
	s_wl.drag_end_pending = 0;
	stop_verify_locked();
	s_wl.geometry_dirty = 0; // the create-time size goes into XrWaylandSurfaceGeometryDXR
	export_update_locked();  // the round trip above has told whether the window is shown
	if (s_wl.drop_wanted && drop_supported_locked())
		drop_device_make_locked("the 3D session has bound the window", 1);
	wl_display_flush(display);
	*out_surface = s_wl.surface;
	*out_w = s_wl.device_w;
	*out_h = s_wl.device_h;
	pthread_mutex_unlock(&s_wl_mutex);
	dxr_wl_ext_start(&s_ext_callbacks);
	return 1;
}

static void
dxr_wl_weave_destroy(void)
{
	pthread_mutex_lock(&s_wl_mutex);
	if (!s_wl.surface) {
		pthread_mutex_unlock(&s_wl_mutex);
		return;
	}
	struct wl_surface *player = s_host->lock_player_surface(NULL);
	if (player)
		wl_surface_set_input_region(player, NULL); // the whole window catches again
	s_host->unlock_player_surface();
	if (s_wl.frac)
		wp_fractional_scale_v1_destroy(s_wl.frac);
	if (s_wl.viewport)
		wp_viewport_destroy(s_wl.viewport);
	if (s_wl.subsurface)
		wl_subsurface_destroy(s_wl.subsurface);
	wl_surface_destroy(s_wl.surface);
	if (s_wl.probe_buffer)
		wl_buffer_destroy(s_wl.probe_buffer);
	// No weave, no export: revoke the handle (the next session exports anew).
	export_drop_locked();
	// Nothing dispatches our queue between sessions: no drops either (the next
	// session makes the device again if the app still wants them).
	drop_device_destroy_locked();
	s_wl.frac = NULL;
	s_wl.viewport = NULL;
	s_wl.subsurface = NULL;
	s_wl.surface = NULL;
	s_wl.weave_on = 0;
	s_wl.probe_buffer = NULL;
	s_wl.parent = NULL;
	s_wl.transparent = 0;
	s_wl.opaque_cleared = 0;
	s_wl.placing = s_wl.move_inflight = s_wl.rescale_after_place = 0;
	s_wl.intent_blocked = 0;
	s_wl.restore_open = 0;
	s_wl.drag_end_pending = 0;
	stop_verify_locked();
	s_wl.size_fixups = 0;
	s_wl.wanted_device_w = s_wl.wanted_device_h = 0;
	s_wl.wanted_by_app = 0;
	s_wl.sent_w = s_wl.sent_h = 0;
	s_wl.check_w = s_wl.check_h = 0;
	s_wl.scale120 = 0;
	s_wl.expect_scale120 = 0;
	s_wl.logical_w = s_wl.logical_h = 0;
	s_wl.device_w = s_wl.device_h = 0;
	s_wl.geometry_dirty = 0;
	wl_display_flush(s_wl.display);
	pthread_mutex_unlock(&s_wl_mutex);
	fprintf(stderr, "[DisplayXR-WL] weave sub-surface destroyed\n");
}

static void
dxr_wl_weave_set_transparent(int transparent)
{
	pthread_mutex_lock(&s_wl_mutex);
	s_wl.transparent = transparent;
	pthread_mutex_unlock(&s_wl_mutex);
}

static int
dxr_wl_weave_active(void)
{
	pthread_mutex_lock(&s_wl_mutex);
	int active = s_wl.surface != NULL;
	pthread_mutex_unlock(&s_wl_mutex);
	return active;
}

//! Per frame on the provider's frame thread. No D-Bus here: placement runs on the
//! extension worker.
static int
dxr_wl_weave_poll(int *out_w, int *out_h)
{
	pthread_mutex_lock(&s_wl_mutex);
	if (!s_wl.surface) {
		pthread_mutex_unlock(&s_wl_mutex);
		return 0;
	}
	// The player made a new window surface (it does on a resolution change, and
	// may recreate the wl_surface itself, possibly at the same address): re-attach.
	// The sub-surface of a destroyed parent is inert, and re-attaching to a live
	// one is harmless (it maps again on the runtime's next present).
	if (s_host->player_surface_generation() != s_wl.parent_generation) {
		unsigned generation = 0;
		struct wl_surface *player = s_host->lock_player_surface(&generation);
		int recreated = 0;
		if (player) { // else: between surfaces, retry
			recreated = player != s_wl.parent;
			attach_to_parent_locked(player);
			s_wl.parent_generation = generation;
		}
		s_host->unlock_player_surface();
		if (player) {
			wl_display_flush(s_wl.display);
			if (recreated)
				fprintf(stderr, "[DisplayXR-WL] the player recreated its window: weave re-attached\n");
		}
		// A new surface (even at the old address) may be a new mutter window:
		// keep-above is to be applied again. The reports may merge its unmap and map
		// into one, so this does not wait for them.
		if (player)
			keep_above_reset_locked();
	}
	keep_above_send_locked();
	// A player window presented with alpha must not be declared opaque (#367).
	// Unity's SDL marks its whole window opaque (wl_surface.set_opaque_region) when
	// it creates it and again on every configure (resize, focus change). The
	// compositor then paints nothing behind the window, so wherever the weave is
	// transparent the screen keeps what it last showed there: the avatar's earlier
	// frames pile up on black. A Leia weave hides this (it is opaque, the desktop
	// baked in); sim-display's is not. A configure can re-mark it at any time, so
	// the region is cleared every frame; it applies at the player's next commit.
	// Gated on what the capture layer actually made of the player's swapchain, not
	// the session flag: SDL's claim is wrong exactly when that swapchain carries
	// alpha, and right for an opaque one, whatever the weave is.
	if (s_host->player_presents_alpha()) {
		struct wl_surface *player = s_host->lock_player_surface(NULL);
		if (player)
			wl_surface_set_opaque_region(player, NULL);
		s_host->unlock_player_surface();
		if (player) {
			wl_display_flush(s_wl.display);
			if (!s_wl.opaque_cleared) {
				s_wl.opaque_cleared = 1;
				fprintf(stderr, "[DisplayXR-WL] the player's window is kept non-opaque (it presents alpha)\n");
			}
		}
	}
	// The player's window size (its swapchain extent, logical px).
	int lw = 0, lh = 0;
	if (s_host->unity_swapchain_size(&lw, &lh) && (lw != s_wl.logical_w || lh != s_wl.logical_h)) {
		s_wl.logical_w = lw;
		s_wl.logical_h = lh;
		if (lw == s_wl.sent_w && lh == s_wl.sent_h)
			s_wl.sent_w = s_wl.sent_h = 0; // the player applied what it was sent
		wp_viewport_set_destination(s_wl.viewport, lw, lh);
		update_device_size_locked();
		wl_display_flush(s_wl.display);
	}
	// The last size handed to the player, once it has had time to apply it.
	if (s_wl.check_w) {
		int outcome = dxr_wl_sent_size_outcome(mono_ns(), s_wl.check_ns, s_wl.check_w, s_wl.check_h, s_wl.logical_w,
		                                       s_wl.logical_h);
		if (outcome < 0) {
			fprintf(stderr, "[DisplayXR-WL] the player did not keep the %dx%d it was sent: its window is %dx%d\n",
			        s_wl.check_w, s_wl.check_h, s_wl.logical_w, s_wl.logical_h);
			if (s_wl.sent_w == s_wl.check_w && s_wl.sent_h == s_wl.check_h)
				s_wl.sent_w = s_wl.sent_h = 0; // no longer a resize in progress
			fix_player_size_locked();
		}
		if (outcome)
			s_wl.check_w = s_wl.check_h = 0;
	}
	wl_display_dispatch_queue_pending(s_wl.display, s_wl.queue);
	export_update_locked(); // right after the events that tell the window was shown anew
	drop_poll_locked();     // a dragged file list arriving, a data device to make again
	int dirty = s_wl.geometry_dirty;
	if (dirty) {
		s_wl.geometry_dirty = 0;
		*out_w = s_wl.device_w;
		*out_h = s_wl.device_h;
	}
	pthread_mutex_unlock(&s_wl_mutex);
	return dirty;
}

//! The pointer is over the player's window (its input region): 1, elsewhere 0, not known -1.
static int
dxr_wl_pointer_inside(void)
{
	pthread_mutex_lock(&s_wl_mutex);
	int v = s_wl.surface && s_wl.pointer ? s_wl.pointer_state : -1;
	pthread_mutex_unlock(&s_wl_mutex);
	return v;
}

static double
dxr_wl_ui_scale(void)
{
	pthread_mutex_lock(&s_wl_mutex);
	double scale = s_wl.surface && s_wl.scale120 ? s_wl.scale120 / 120.0 : 1.0;
	pthread_mutex_unlock(&s_wl_mutex);
	return scale;
}

static int
dxr_wl_weave_device_size(int *out_w, int *out_h)
{
	pthread_mutex_lock(&s_wl_mutex);
	int have = s_wl.surface && s_wl.device_w > 0 && s_wl.device_h > 0;
	if (have) {
		*out_w = s_wl.device_w;
		*out_h = s_wl.device_h;
	}
	pthread_mutex_unlock(&s_wl_mutex);
	return have;
}

static unsigned
dxr_wl_player_generation(void)
{
	pthread_mutex_lock(&s_wl_mutex);
	unsigned n = s_wl.attach_count;
	pthread_mutex_unlock(&s_wl_mutex);
	return n;
}

/*
 *
 * Click-through and window size.
 *
 */

static int
dxr_wl_click_through_wanted(void)
{
	pthread_mutex_lock(&s_wl_mutex);
	int wanted = s_wl.surface && s_wl.transparent;
	pthread_mutex_unlock(&s_wl_mutex);
	return wanted;
}

//! Layout-compatible with XRectangle (displayxr_linux.c's LinXRect).
typedef struct DxrWlRect {
	short x, y;
	unsigned short w, h;
} DxrWlRect;

static int
dxr_wl_set_player_input_region(const void *rects, int n)
{
	pthread_mutex_lock(&s_wl_mutex);
	if (!s_wl.surface || !s_wl.compositor) {
		pthread_mutex_unlock(&s_wl_mutex);
		return 0;
	}
	struct wl_surface *player = s_host->lock_player_surface(NULL);
	if (player) {
		// Double-buffered: applies at the player's next commit (it presents every frame).
		if (n < 0) {
			wl_surface_set_input_region(player, NULL);
		} else {
			struct wl_region *region = wl_compositor_create_region(s_wl.compositor);
			const DxrWlRect *r = (const DxrWlRect *)rects;
			for (int i = 0; i < n; i++)
				wl_region_add(region, r[i].x, r[i].y, r[i].w, r[i].h);
			wl_surface_set_input_region(player, region);
			wl_region_destroy(region);
		}
	}
	s_host->unlock_player_surface();
	if (player)
		wl_display_flush(s_wl.display);
	pthread_mutex_unlock(&s_wl_mutex);
	return player != NULL; // NULL: the player is between window surfaces
}

// Placement (below).
static void panel_centre_locked(const struct dxr_wl_output *panel, int layout, int *out_x, int *out_y);
static void set_intent_locked(int x, int y, const char *why);
static void stop_verify_locked(void);
static int x11_scale_locked(void);
static int layout_or_warn_locked(void);

static void
dxr_wl_request_player_size(int device_w, int device_h)
{
	pthread_mutex_lock(&s_wl_mutex);
	s_wl.wanted_device_w = device_w;
	s_wl.wanted_device_h = device_h;
	s_wl.wanted_by_app = 1;
	s_wl.size_fixups = 0;
	rerequest_player_size_locked();
	// Centred on the 3D panel (and not moved since): keep it centred at the new size.
	struct dxr_wl_output *panel = output_named_locked(s_wl.intent_centred_on);
	int layout = panel ? layout_locked() : DXR_LAYOUT_UNKNOWN;
	if (panel && layout != DXR_LAYOUT_UNKNOWN) {
		int x, y;
		panel_centre_locked(panel, layout, &x, &y);
		if (x != s_wl.intent_x || y != s_wl.intent_y) {
			char name[sizeof(s_wl.intent_centred_on)];
			memcpy(name, s_wl.intent_centred_on, sizeof(name));
			char why[96];
			snprintf(why, sizeof(why), "3D panel '%s', re-centred for %dx%d px", name, device_w, device_h);
			set_intent_locked(x, y, why);
			memcpy(s_wl.intent_centred_on, name, sizeof(name));
		}
	}
	pthread_mutex_unlock(&s_wl_mutex);
}

static int
dxr_wl_take_player_size(int *out_w, int *out_h)
{
	pthread_mutex_lock(&s_wl_mutex);
	int have = s_wl.surface && s_wl.pending_w > 0 && s_wl.pending_h > 0;
	if (have) {
		*out_w = s_wl.sent_w = s_wl.check_w = s_wl.pending_w;
		*out_h = s_wl.sent_h = s_wl.check_h = s_wl.pending_h;
		s_wl.check_ns = mono_ns() + DXR_WL_SIZE_SETTLE_NS;
		s_wl.pending_w = s_wl.pending_h = 0;
	}
	pthread_mutex_unlock(&s_wl_mutex);
	return have;
}

/*
 *
 * Placement, driven by what the GNOME extension reports (worker thread).
 *
 */

static int
win_mapped(const DxrWlExtWindow *w)
{
	return w->present && w->w > 0 && w->h > 0;
}

static long long
mono_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static DxrWlRect4
output_rect(const struct dxr_wl_output *o)
{
	DxrWlRect4 r = {o->lx, o->ly, o->lw, o->lh};
	return r;
}

//! The window is on `panel`, as the extension reports it: its monitor is the
//! panel's output (or, from an extension that reports no monitor, its centre is
//! inside the panel's stage rect).
static int
win_on_output(const DxrWlExtWindow *w, const struct dxr_wl_output *panel)
{
	if (!win_mapped(w) || !panel)
		return 0;
	if (w->mon_w > 0) {
		DxrWlRect4 m = {w->mon_x, w->mon_y, w->mon_w, w->mon_h};
		return dxr_wl_monitor_is(m, output_rect(panel));
	}
	return dxr_wl_window_on_rect(output_rect(panel), w->x, w->y, w->w, w->h);
}

//! Re-read the window a little later to check it reached the 3D panel (#361).
static void
schedule_verify_locked(void)
{
	if (!s_wl.verify_panel[0] || s_wl.verify_scheduled || s_wl.drag_state)
		return;
	s_wl.verify_scheduled = 1;
	dxr_wl_ext_verify_after(DXR_WL_VERIFY_DELAY_MS);
}

static void
stop_verify_locked(void)
{
	s_wl.verify_panel[0] = 0;
	s_wl.verify_tries = 0;
	s_wl.verify_scheduled = 0;
}

//! A position the app sets now is (still) a restore of a saved one: the window
//! has not been on screen long, and the user has not moved it.
static int
restore_open_locked(void)
{
	if (s_wl.restore_open && s_wl.first_map_ns && mono_ns() - s_wl.first_map_ns > DXR_WL_RESTORE_WINDOW_NS)
		s_wl.restore_open = 0;
	return s_wl.restore_open;
}

//! The user took the window (a drag, Super+drag, the keyboard): placement is
//! theirs from now on.
static void
user_moved_locked(void)
{
	s_wl.restore_open = 0;
	stop_verify_locked();
}

//! Where the window is now, the extension says what scale its monitor has, ahead of
//! the fractional-scale protocol: compute sizes for that.
static void
follow_window_scale_locked(void)
{
	if (!win_mapped(&s_wl.win) || s_wl.win.device_scale <= 0.0)
		return;
	uint32_t t120 = (uint32_t)lround(120.0 * s_wl.win.device_scale);
	s_wl.expect_scale120 = t120 != s_wl.scale120 ? t120 : 0;
}

//! Re-ask for the size the app wants if the player's window is at another one
//! (twice in a row at most, so a player that keeps refusing a size cannot make us
//! re-map it forever). Not while a size handed to the player has not reached its
//! window yet: that is a resize in progress, not one reverted. Not during a
//! placement: its end calls this. And not before the protocol has caught up with a
//! new output's scale: the scale change re-asks then, and a resize right as the
//! window lands on another output is one the player tends to revert.
static void
fix_player_size_locked(void)
{
	if (s_wl.size_fixups >= 2 || s_wl.sent_w || s_wl.placing || s_wl.move_inflight || s_wl.expect_scale120)
		return;
	int had = s_wl.pending_w;
	rerequest_player_size_locked();
	if (s_wl.pending_w && !had) {
		s_wl.size_fixups++;
		fprintf(stderr, "[DisplayXR-WL] the window is %dx%d logical, not the %dx%d wanted: asking again\n",
		        s_wl.logical_w, s_wl.logical_h, s_wl.pending_w, s_wl.pending_h);
	}
}

//! A placement ended (the window is where it should be, or where the compositor
//! insists on): act on a scale change seen meanwhile, and re-ask for the size the
//! app wants if the player came back at another one.
static void
place_done_locked(void)
{
	s_wl.placing = 0;
	s_wl.move_inflight = 0;
	s_wl.rescale_after_place = 0;
	follow_window_scale_locked();
	fix_player_size_locked();
}

//! Ask the extension to move the window to the intent; its answer comes back as a
//! window report with after_move set.
static void
place_locked(const char *why)
{
	if (!s_wl.ext_available || !s_wl.intent_valid || !s_wl.surface)
		return;
	if (s_wl.corrections >= DXR_WL_MAX_CORRECTIONS) {
		if (!(s_wl.warned & DXR_WARN_GAVE_UP))
			fprintf(stderr, "[DisplayXR-WL] the compositor keeps placing the window elsewhere: leaving it there\n");
		s_wl.warned |= DXR_WARN_GAVE_UP;
		s_wl.intent_x = s_wl.win.x;
		s_wl.intent_y = s_wl.win.y;
		place_done_locked();
		schedule_verify_locked(); // a move to the 3D panel gets its own bounded retries
		return;
	}
	// Size requests use the target output's scale from here on.
	int layout = layout_locked();
	struct dxr_wl_output *target = output_at_locked(s_wl.intent_x, s_wl.intent_y);
	uint32_t t120 = target && layout != DXR_LAYOUT_UNKNOWN ? output_scale120_locked(target, layout) : 0;
	if (t120 && t120 != s_wl.scale120)
		s_wl.expect_scale120 = t120;
	// Too big for the target as it is (the player mapped it at a stale size):
	// moving it there now only gets it squeezed or maximized by the compositor,
	// which the player then fights. Get the size right first, and move it once it
	// is (the window report after the resize, or its re-map, calls back here).
	// If not even the size it is getting fits, move it anyway.
	if (target && (s_wl.win.w > target->lw || s_wl.win.h > target->lh)) {
		rerequest_player_size_locked();
		int coming_w = s_wl.pending_w ? s_wl.pending_w : s_wl.sent_w;
		int coming_h = s_wl.pending_w ? s_wl.pending_h : s_wl.sent_h;
		double stage = layout == DXR_LAYOUT_PHYSICAL ? (t120 ? t120 / 120.0 : 1.0) : 1.0; // surface -> stage
		int coming_fits = coming_w > 0 && lround(coming_w * stage) <= target->lw && lround(coming_h * stage) <= target->lh;
		if (coming_fits) {
			if (!s_wl.intent_blocked)
				fprintf(stderr, "[DisplayXR-WL] %s; the window (%dx%d) does not fit at (%d,%d) until it is %dx%d: "
				                "moving it once it is\n",
				        why, s_wl.win.w, s_wl.win.h, s_wl.intent_x, s_wl.intent_y, coming_w, coming_h);
			s_wl.intent_blocked = 1;
			s_wl.blocked_w = s_wl.win.w;
			s_wl.blocked_h = s_wl.win.h;
			return;
		}
	}
	s_wl.intent_blocked = 0;
	s_wl.corrections++;
	s_wl.move_inflight = 1;
	s_wl.placing = 1;
	if (!s_quiet_place)
		fprintf(stderr, "[DisplayXR-WL] %s: window -> (%d,%d)\n", why, s_wl.intent_x, s_wl.intent_y);
	dxr_wl_ext_move_window(s_wl.intent_x, s_wl.intent_y);
}

static void
ext_availability(int available, unsigned caps, const char *layout)
{
	pthread_mutex_lock(&s_wl_mutex);
	s_wl.ext_available = available;
	s_wl.ext_known = 1;
	s_wl.ext_caps = caps;
	snprintf(s_wl.ext_layout, sizeof(s_wl.ext_layout), "%s", layout ? layout : "");
	if (!available) {
		s_wl.placing = s_wl.move_inflight = s_wl.rescale_after_place = 0;
		s_wl.expect_scale120 = 0;
		memset(&s_wl.win, 0, sizeof(s_wl.win));
	} else if (!(caps & DXR_WL_EXT_CAP_POINTER_DRAG)) {
		fprintf(stderr, "[DisplayXR-WL] the GNOME extension has no pointer drag (needs version 9): right-drag "
		                "will not move the window\n");
	}
	pthread_mutex_unlock(&s_wl_mutex);
}

//! The window may be a new mutter window (recreated, a new session, shown anew by
//! the extension): keep-above is to be applied again, and an answer still on its
//! way (it was for the old window) must not count as done.
static void
keep_above_reset_locked(void)
{
	if (!s_wl.keep_above_asked || !s_wl.keep_above_wanted)
		return;
	s_wl.keep_above_ok = 0;
	s_wl.keep_above_due_ns = 0;
	if (s_wl.keep_above_inflight)
		s_wl.keep_above_redo = 1;
}

//! Send the keep-above the app asked for while the extension has not confirmed
//! it (also between XR sessions: it is the app's window either way). Called from
//! the per-frame poll and on every report of the window, so a
//! request is sent once the window is shown and again after a refusal (the
//! screen locked, the user dragging it). Never during a move or our own drag,
//! never with a request still on its way, and after a refusal not before 1, 2,
//! 4, 8, 16, then every 30 s: a refusal that keeps holding costs one quiet round
//! trip per 30 s. A (re)mapped or recreated window, a drag that ended and a new
//! request from the app clear the delay.
static void
keep_above_send_locked(void)
{
	if (!s_wl.keep_above_asked || s_wl.keep_above_ok || s_wl.keep_above_inflight ||
	    !win_mapped(&s_wl.win) || s_wl.win.moving || s_wl.drag_state || !s_wl.ext_available ||
	    !(s_wl.ext_caps & DXR_WL_EXT_CAP_KEEP_ABOVE) || mono_ns() < s_wl.keep_above_due_ns)
		return;
	s_wl.keep_above_inflight = 1;
	dxr_wl_ext_own_window(DXR_WL_EXT_OWN_KEEP_ABOVE, s_wl.keep_above_wanted, ++s_wl.keep_above_seq);
}

//! A report of our window. Shown anew (mapped again, or a new mutter window: a
//! NULL-buffer unmap gives the toplevel a new one, which the next show maps) is
//! evidence for the export (export_update_locked).
static void
take_window_locked(const DxrWlExtWindow *w)
{
	if (win_mapped(w) && (!win_mapped(&s_wl.win) || (w->id && w->id != s_wl.win.id)))
		s_wl.ext_shown_seq++;
	s_wl.win = *w;
}

static void drop_focus_report_locked(const DxrWlExtWindow *w);

static void
ext_window(const DxrWlExtWindow *w, int after_move, int moved)
{
	pthread_mutex_lock(&s_wl_mutex);
	drop_focus_report_locked(w); // before s_wl.win takes the new report
	int was_mapped = win_mapped(&s_wl.win);
	int was_moving = s_wl.win.moving;
	int size_changed = was_mapped && (w->w != s_wl.win.w || w->h != s_wl.win.h);
	take_window_locked(w);
	int mapped = win_mapped(w);
	export_update_locked();
	// Keep-above lives on a mutter window, and lasts as long as our bus connection
	// to the extension: a window shown anew (mapped again, or reported again by an
	// extension that went away and came back) has lost it. A drag that ended is
	// when a request refused during it can go through.
	if (mapped && !was_mapped)
		keep_above_reset_locked();
	if (was_moving && !w->moving)
		s_wl.keep_above_due_ns = 0;
	keep_above_send_locked();
	if (!s_wl.surface) {
		pthread_mutex_unlock(&s_wl_mutex);
		return;
	}
	if (after_move) {
		// Our move's result. Where the compositor kept the window elsewhere: if it
		// does not fit there yet (mapped at a stale size, or a resize is on its
		// way), keep the intent for when it does; otherwise (its other
		// constraints) accept where it is.
		s_wl.move_inflight = 0;
		if (!moved)
			fprintf(stderr, "[DisplayXR-WL] the extension did not move the window (a move grab in progress?)\n");
		if (moved && mapped && (w->x != s_wl.intent_x || w->y != s_wl.intent_y)) {
			struct dxr_wl_output *target = output_at_locked(s_wl.intent_x, s_wl.intent_y);
			int fits = target && w->w <= target->lw && w->h <= target->lh;
			if (!fits || s_wl.sent_w || s_wl.pending_w) {
				fprintf(stderr, "[DisplayXR-WL] the window (%dx%d) does not fit at (%d,%d) yet: moving it there "
				                "once it is resized\n",
				        w->w, w->h, s_wl.intent_x, s_wl.intent_y);
				s_wl.intent_blocked = 1;
				s_wl.blocked_w = w->w;
				s_wl.blocked_h = w->h;
				s_wl.placing = 0; // size requests keep the target's scale (expect_scale120)
				rerequest_player_size_locked();
				pthread_mutex_unlock(&s_wl_mutex);
				return;
			}
			fprintf(stderr, "[DisplayXR-WL] the compositor kept the window at (%d,%d), not (%d,%d): leaving it there\n",
			        w->x, w->y, s_wl.intent_x, s_wl.intent_y);
		}
		if (mapped && (w->x != s_wl.intent_x || w->y != s_wl.intent_y)) {
			s_wl.intent_centred_on[0] = 0;
			s_wl.intent_valid = 1;
			s_wl.intent_x = w->x;
			s_wl.intent_y = w->y;
		}
		place_done_locked();
		schedule_verify_locked(); // did it stay on the 3D panel? (#361)
		pthread_mutex_unlock(&s_wl_mutex);
		return;
	}
	if (!mapped) {
		// Not on screen (yet): its position is not where it will show.
		pthread_mutex_unlock(&s_wl_mutex);
		return;
	}
	if (!s_wl.first_map_ns)
		s_wl.first_map_ns = mono_ns();
	if (s_wl.drag_end_pending && !w->moving && !s_wl.drag_state) {
		// Where the user left it, in the units the app saves (#361).
		s_wl.drag_end_pending = 0;
		struct dxr_wl_output *on = output_at_locked(w->x + w->w / 2, w->y + w->h / 2);
		int n = x11_scale_locked();
		if (n)
			fprintf(stderr, "[DisplayXR-WL] window drag: left at (%d,%d) on '%s' = position X (%d,%d) (XWayland "
			                "scale %d), what displayxr_get_overlay_position reports now\n",
			        w->x, w->y, on ? on->name : "?", w->x * n, w->y * n, n);
		else
			fprintf(stderr, "[DisplayXR-WL] window drag: left at (%d,%d) on '%s' (no X root conversion)\n", w->x,
			        w->y, on ? on->name : "?");
	}
	if (!was_mapped) {
		// Just (re)mapped: the compositor has placed it by its own rules.
		s_wl.corrections = 0;
		s_wl.intent_blocked = 0;
		if (s_wl.intent_valid && (w->x != s_wl.intent_x || w->y != s_wl.intent_y)) {
			char why[96];
			snprintf(why, sizeof(why), "the compositor placed the window at (%d,%d)", w->x, w->y);
			place_locked(why);
		} else {
			s_wl.intent_valid = 1;
			s_wl.intent_x = w->x;
			s_wl.intent_y = w->y;
			place_done_locked();
		}
		if (!s_wl.move_inflight)
			schedule_verify_locked(); // (a move's result schedules it otherwise)
		pthread_mutex_unlock(&s_wl_mutex);
		return;
	}
	if (s_wl.intent_blocked) {
		if (w->moving || s_wl.drag_state) {
			s_wl.intent_blocked = 0; // the user is moving it: follow (below)
		} else if (w->w != s_wl.blocked_w || w->h != s_wl.blocked_h) {
			s_wl.intent_blocked = 0;
			s_wl.corrections = 0;
			if (w->x != s_wl.intent_x || w->y != s_wl.intent_y) {
				char why[96];
				snprintf(why, sizeof(why), "resized to %dx%d", w->w, w->h);
				place_locked(why);
				pthread_mutex_unlock(&s_wl_mutex);
				return;
			}
		} else {
			// Still waiting for the resize: wherever the compositor keeps it
			// meanwhile is not where it should be.
			pthread_mutex_unlock(&s_wl_mutex);
			return;
		}
	}
	if (!s_wl.move_inflight && (w->x != s_wl.intent_x || w->y != s_wl.intent_y) && !w->moving &&
	    !s_wl.drag_state && s_wl.intent_valid && (size_changed || s_wl.sent_w || s_wl.pending_w)) {
		// Moved together with a resize: the player (which resets its window when it
		// crosses to another display mid-resize) or the compositor fitting it, not
		// the user. Put it back.
		char why[96];
		snprintf(why, sizeof(why), "moved to (%d,%d) by a resize to %dx%d", w->x, w->y, w->w, w->h);
		place_locked(why);
		pthread_mutex_unlock(&s_wl_mutex);
		return;
	}
	if (!s_wl.move_inflight && (w->x != s_wl.intent_x || w->y != s_wl.intent_y)) {
		// Moved by the user (our drag, Super+drag, the keyboard) or by the
		// compositor uninvited: follow it. The user's move ends any placement we
		// were still verifying; an uninvited one is checked against the panel.
		s_wl.intent_centred_on[0] = 0;
		s_wl.intent_valid = 1;
		s_wl.intent_x = w->x;
		s_wl.intent_y = w->y;
		if (w->moving || s_wl.drag_state)
			user_moved_locked();
		else
			schedule_verify_locked();
	}
	if (!s_wl.placing && !s_wl.move_inflight)
		follow_window_scale_locked();
	pthread_mutex_unlock(&s_wl_mutex);
}

//! The delayed read-back of a move to the 3D panel (#361): there, or move it
//! again (bounded).
static void
ext_verify(const DxrWlExtWindow *w)
{
	pthread_mutex_lock(&s_wl_mutex);
	s_wl.verify_scheduled = 0;
	struct dxr_wl_output *panel = s_wl.surface ? output_named_locked(s_wl.verify_panel) : NULL;
	if (!panel) {
		stop_verify_locked();
		pthread_mutex_unlock(&s_wl_mutex);
		return;
	}
	if (w->moving || s_wl.drag_state) {
		user_moved_locked();
		pthread_mutex_unlock(&s_wl_mutex);
		return;
	}
	// A placement still under way (a move's answer pending, a resize awaited, the
	// window not shown yet): its own result schedules the next check.
	if (s_wl.move_inflight || s_wl.intent_blocked || !win_mapped(w) || !win_mapped(&s_wl.win)) {
		pthread_mutex_unlock(&s_wl_mutex);
		return;
	}
	if (win_on_output(w, panel)) {
		fprintf(stderr, "[DisplayXR-WL] placement verified: LANDED on the 3D panel '%s' (frame %d,%d %dx%d) after "
		                "%d retr%s\n",
		        panel->name, w->x, w->y, w->w, w->h, s_wl.verify_tries, s_wl.verify_tries == 1 ? "y" : "ies");
		stop_verify_locked();
		pthread_mutex_unlock(&s_wl_mutex);
		return;
	}
	if (s_wl.verify_tries >= DXR_WL_VERIFY_TRIES) {
		fprintf(stderr, "[DisplayXR-WL] placement: the window is NOT on the panel after %d tries (frame %d,%d %dx%d, "
		                "monitor %d,%d %dx%d; 3D panel '%s' %d,%d %dx%d): leaving it there\n",
		        s_wl.verify_tries, w->x, w->y, w->w, w->h, w->mon_x, w->mon_y, w->mon_w, w->mon_h, panel->name,
		        panel->lx, panel->ly, panel->lw, panel->lh);
		stop_verify_locked();
		pthread_mutex_unlock(&s_wl_mutex);
		return;
	}
	int layout = layout_or_warn_locked();
	if (layout == DXR_LAYOUT_UNKNOWN) {
		stop_verify_locked();
		pthread_mutex_unlock(&s_wl_mutex);
		return;
	}
	s_wl.verify_tries++;
	// Back to the intent if that is on the panel (a saved position there), else
	// the panel's centre.
	int x = s_wl.intent_x, y = s_wl.intent_y;
	int centre = !s_wl.intent_valid || !dxr_wl_window_on_rect(output_rect(panel), x, y, w->w, w->h);
	if (centre)
		panel_centre_locked(panel, layout, &x, &y);
	char name[sizeof(s_wl.verify_panel)];
	memcpy(name, s_wl.verify_panel, sizeof(name));
	char why[160];
	snprintf(why, sizeof(why), "verify: the window is at (%d,%d), not on the 3D panel '%s'; moving it again (try %d/%d)",
	         w->x, w->y, name, s_wl.verify_tries, DXR_WL_VERIFY_TRIES);
	take_window_locked(w);
	s_wl.corrections = 0; // a fresh budget per retry; the retries themselves are bounded
	set_intent_locked(x, y, why);
	if (centre)
		memcpy(s_wl.intent_centred_on, name, sizeof(s_wl.intent_centred_on));
	pthread_mutex_unlock(&s_wl_mutex);
}

static void
ext_drag(int started)
{
	pthread_mutex_lock(&s_wl_mutex);
	if (s_wl.drag_state == 1)
		s_wl.drag_state = started ? 2 : 0;
	pthread_mutex_unlock(&s_wl_mutex);
	fprintf(stderr, "[DisplayXR-WL] window drag: %s\n", started ? "start" : "refused");
}

static void
ext_own_window(int op, int value, unsigned seq, int ok)
{
	static const char *const what[] = {"keep above", "activate", "minimize"};
	const char *name = op >= 0 && op <= DXR_WL_EXT_OWN_MINIMIZE ? what[op] : "?";
	if (op == DXR_WL_EXT_OWN_KEEP_ABOVE) {
		pthread_mutex_lock(&s_wl_mutex);
		int latest = seq == s_wl.keep_above_seq; // else superseded: ignored
		int log = 1;
		if (latest && s_wl.keep_above_redo) {
			// For a window that has since been shown anew: send again for the new one.
			s_wl.keep_above_inflight = 0;
			s_wl.keep_above_redo = 0;
			s_wl.keep_above_ok = 0;
		} else if (latest) {
			s_wl.keep_above_inflight = 0;
			// The app may have asked for the other value meanwhile: then it is not done.
			s_wl.keep_above_ok = ok && value == s_wl.keep_above_wanted;
			if (ok) {
				s_wl.keep_above_refusals = 0;
				// Re-sent for a window shown anew (a resize recreates the player's
				// surface): the same state again, not worth a line each time.
				log = !(s_wl.keep_above_logged && s_wl.keep_above_ok);
				if (s_wl.keep_above_ok)
					s_wl.keep_above_logged = 1;
			} else {
				log = s_wl.keep_above_refusals == 0;
				int n = s_wl.keep_above_refusals++;
				long long delay_s = n < 5 ? 1LL << n : 30;
				s_wl.keep_above_due_ns = mono_ns() + delay_s * 1000000000LL;
			}
		}
		pthread_mutex_unlock(&s_wl_mutex);
		if (log)
			fprintf(stderr, "[DisplayXR-WL] window %s %s: %s%s\n", name, value ? "on" : "off",
			        ok ? "done" : "refused", !ok && latest ? " (sent again later, quietly)" : "");
	} else if (op == DXR_WL_EXT_OWN_ACTIVATE) {
		// Extension 17+ gives focus only when the user's latest input went to the app
		// (else GNOME flags the window as wanting attention); 15/16 refuse only while
		// locked or during a drag. Never re-sent: a later request is the app's call.
		fprintf(stderr, "[DisplayXR-WL] window %s: %s\n", name,
		        ok ? "done" : "refused (the user is in another app, the screen is locked, or a drag runs)");
	} else {
		fprintf(stderr, "[DisplayXR-WL] window %s %s: %s\n", name, value ? "on" : "off", ok ? "done" : "refused");
	}
}

static const DxrWlExtCallbacks s_ext_callbacks = {
    .availability = ext_availability,
    .window = ext_window,
    .drag = ext_drag,
    .verify = ext_verify,
    .own_window = ext_own_window,
};

/*
 * The window's own state (extension version 15): what the window menu offers
 * the user and an app expects to ask for itself — keep it above, bring it to the
 * front, minimize and restore it. A Wayland client has no protocol for any of it
 * (no keep-above; xdg-activation of a background app only flashes it; nothing
 * undoes set_minimized), so the extension does it, for this process's window
 * only. Each answer is logged by ext_own_window.
 */

//! Whether the extension offers @p cap now; logs once per kind (@p warn) when it
//! does not — once the worker has said whether the extension is there (right after
//! a session starts it may not have yet). @p remembered: the request is kept and
//! sent once it can be.
static int
own_window_ready_locked(unsigned cap, const char *what, unsigned warn, int remembered)
{
	if (s_wl.ext_available && (s_wl.ext_caps & cap))
		return 1;
	if (s_wl.ext_known && !(s_wl.warned & warn)) {
		fprintf(stderr, "[DisplayXR-WL] window %s %s (%s)\n", what,
		        remembered ? "remembered, not possible right now" : "unavailable",
		        !s_wl.ext_available ? "no DisplayXR GNOME extension"
		                            : "the GNOME extension is older than version 15");
		s_wl.warned |= warn;
	}
	return 0;
}

static int
dxr_wl_own_window_caps(void)
{
	pthread_mutex_lock(&s_wl_mutex);
	int caps = 0;
	if (s_wl.ext_available) {
		if (s_wl.ext_caps & DXR_WL_EXT_CAP_KEEP_ABOVE)
			caps |= DXR_WL_OWN_KEEP_ABOVE;
		if (s_wl.ext_caps & DXR_WL_EXT_CAP_ACTIVATE)
			caps |= DXR_WL_OWN_ACTIVATE;
		if (s_wl.ext_caps & DXR_WL_EXT_CAP_MINIMIZE)
			caps |= DXR_WL_OWN_MINIMIZE;
	}
	pthread_mutex_unlock(&s_wl_mutex);
	return caps;
}

static int
dxr_wl_set_keep_above(int above)
{
	pthread_mutex_lock(&s_wl_mutex);
	int want = above ? 1 : 0;
	int ready = own_window_ready_locked(DXR_WL_EXT_CAP_KEEP_ABOVE, "keep above", DXR_WARN_OWN_KEEP_ABOVE, 1);
	// The same request again (an app may set it every frame) changes nothing — not
	// even a refusal's back-off — unless the window reports otherwise: then the user
	// cleared it (window menu) and the app asks for it anew.
	int reported_differs = s_wl.win.present && s_wl.win.above >= 0 && s_wl.win.above != want;
	if (s_wl.keep_above_asked && s_wl.keep_above_wanted == want && !(s_wl.keep_above_ok && reported_differs)) {
		pthread_mutex_unlock(&s_wl_mutex);
		return ready;
	}
	// Remembered in any case: keep_above_send_locked sends it now, once the window is
	// shown, or once the extension can (a request still on its way is answered
	// first, and the answer to it then does not count as this one's).
	s_wl.keep_above_asked = 1;
	s_wl.keep_above_wanted = want;
	s_wl.keep_above_ok = 0;
	s_wl.keep_above_logged = 0;
	s_wl.keep_above_refusals = 0;
	s_wl.keep_above_due_ns = 0;
	keep_above_send_locked();
	pthread_mutex_unlock(&s_wl_mutex);
	return ready;
}

//! Activate / minimize: actions, not state — sent now or not at all, never again.
static int
own_window_action(unsigned cap, const char *what, unsigned warn, int op, int value)
{
	pthread_mutex_lock(&s_wl_mutex);
	int ok = own_window_ready_locked(cap, what, warn, 0);
	if (ok)
		dxr_wl_ext_own_window(op, value, 0);
	pthread_mutex_unlock(&s_wl_mutex);
	return ok;
}

static int
dxr_wl_activate_window(void)
{
	return own_window_action(DXR_WL_EXT_CAP_ACTIVATE, "activate", DXR_WARN_OWN_ACTIVATE, DXR_WL_EXT_OWN_ACTIVATE, 1);
}

/*
 * The player's window for other processes (xdg-foreign): a portal dialog given
 * its handle as parent_window opens on top of the app's window.
 *
 * Exporting a surface that has no window is a protocol error, which ends the
 * PLAYER's connection, and nothing tells us in time that the window went away.
 * mutter drops the window the moment the client destroys its xdg_toplevel or
 * attaches a NULL buffer, but sends our sub-surface no leave for it (GNOME 50,
 * either way: none while the window stays hidden; a leave and an enter come
 * together when it is shown again). The extension lists windows by their
 * actors, which outlive the window for its close effect. So "on an output" is
 * no proof that the window is there.
 *
 * So the export is made only right after the window was SEEN SHOWN ANEW: our
 * weave entering an output again (dispatched on the frame thread) or the
 * extension reporting it mapped again or as a new mutter window (its worker),
 * and only while both still say it is shown. dxr_wl_export_handle (the app's
 * call) never sends anything: it reads what was made here. The compositor drops
 * the export when the window is unmapped and sends the exporter nothing; our own
 * import of the handle hears it (zxdg_imported_v2.destroyed), and the handle is
 * "" from then until the window is shown anew. The case left: a window shown
 * and hidden again (its toplevel destroyed) within the frame or the D-Bus round
 * trip it takes us to see the show.
 *
 * Unity's player can get there: in Unity 6000.4, SDL_HideWindow destroys the
 * xdg_toplevel (it unrefs the libdecor frame; without libdecor it destroys the
 * toplevel itself) and keeps the wl_surface until SDL_DestroyWindow. Of Unity's
 * own window operations, only quitting did that in a protocol trace (after the
 * XR session, and so this export, had ended); resolution and fullscreen
 * changes, maximize and minimize keep the toplevel.
 */

static void
export_watch_destroyed(void *data, struct zxdg_imported_v2 *imported)
{
	(void)data;
	if (imported != s_wl.export_watch || s_wl.export_dead)
		return;
	s_wl.export_dead = 1;
	s_wl.exported_handle[0] = 0;
	fprintf(stderr, "[DisplayXR-WL] the compositor dropped the window's export (hidden or closed): no handle "
	                "until it is shown again\n");
}

static const struct zxdg_imported_v2_listener s_export_watch_listener = {
    .destroyed = export_watch_destroyed,
};

static void
exported_on_handle(void *data, struct zxdg_exported_v2 *exported, const char *handle)
{
	(void)data;
	if (exported != s_wl.exported || s_wl.export_dead)
		return;
	int n = snprintf(s_wl.exported_handle, sizeof(s_wl.exported_handle), "%s", handle ? handle : "");
	if (n <= 0 || (size_t)n >= sizeof(s_wl.exported_handle)) {
		s_wl.exported_handle[0] = 0; // empty, or cut short: not a handle we can pass on
		fprintf(stderr, "[DisplayXR-WL] the compositor's window handle is %s: not used\n", n <= 0 ? "empty" : "too long");
		return;
	}
	fprintf(stderr, "[DisplayXR-WL] the player's window is exported for portal dialogs\n");
	// export_update_locked starts watching it.
}

static const struct zxdg_exported_v2_listener s_exported_listener = {
    .handle = exported_on_handle,
};

static void
export_drop_locked(void)
{
	if (s_wl.export_watch)
		zxdg_imported_v2_destroy(s_wl.export_watch);
	if (s_wl.exported)
		zxdg_exported_v2_destroy(s_wl.exported);
	s_wl.export_watch = NULL;
	s_wl.exported = NULL;
	s_wl.export_dead = 0;
	s_wl.exported_handle[0] = 0;
}

//! The export still stands. Without an importer to tell, it is taken to have
//! died with any show seen since it was made (the compositor drops it on unmap).
static int
export_alive_locked(void)
{
	if (!s_wl.exported || s_wl.export_dead)
		return 0;
	return s_wl.importer ||
	       (s_wl.exported_weave_seq == s_wl.weave_shown_seq && s_wl.exported_ext_seq == s_wl.ext_shown_seq);
}

//! The window is on screen, as far as we can tell: the weave is on an output and,
//! where the extension reports the window, it says mapped and not minimized.
static int
export_window_shown_locked(void)
{
	if (!s_wl.weave_on)
		return 0;
	return !s_wl.ext_available || (win_mapped(&s_wl.win) && s_wl.win.minimized != 1);
}

//! After every dispatch of our queue and every report of the window: export the
//! window if it was seen shown anew since the export it has died (or since there
//! was none), and is still shown. See the comment above.
static void
export_update_locked(void)
{
	if (!s_wl.exporter || !s_wl.surface)
		return;
	// Watch the export: importing a handle is harmless (an unknown one is answered
	// with `destroyed` at once), and every importer is told when the export dies.
	if (s_wl.exported && !s_wl.export_dead && s_wl.exported_handle[0] && s_wl.importer && !s_wl.export_watch) {
		s_wl.export_watch = zxdg_importer_v2_import_toplevel(s_wl.importer, s_wl.exported_handle);
		zxdg_imported_v2_add_listener(s_wl.export_watch, &s_export_watch_listener, NULL);
		wl_display_flush(s_wl.display);
	}
	if (export_alive_locked()) {
		// Shows seen while it stands (a minimized window restored, a report of the
		// window it was made for) are not new: only one after it died is.
		s_wl.exported_weave_seq = s_wl.weave_shown_seq;
		s_wl.exported_ext_seq = s_wl.ext_shown_seq;
		return;
	}
	int shown_anew = s_wl.exported_weave_seq != s_wl.weave_shown_seq || s_wl.exported_ext_seq != s_wl.ext_shown_seq;
	if (!shown_anew || !export_window_shown_locked())
		return;
	export_drop_locked();
	unsigned generation = 0;
	struct wl_surface *player = s_host->lock_player_surface(&generation);
	// Only the window the weave is attached to: its enter/leave are what we saw.
	if (player && player == s_wl.parent && generation == s_wl.parent_generation) {
		s_wl.exported = zxdg_exporter_v2_export_toplevel(s_wl.exporter, player);
		zxdg_exported_v2_add_listener(s_wl.exported, &s_exported_listener, NULL);
		s_wl.exported_weave_seq = s_wl.weave_shown_seq;
		s_wl.exported_ext_seq = s_wl.ext_shown_seq;
	}
	s_host->unlock_player_surface();
	if (s_wl.exported)
		wl_display_flush(s_wl.display); // the handle comes with a later dispatch
}

//! Reads what export_update_locked made: no request, no dispatch, no round trip of
//! its own. It takes s_wl_mutex, so it may wait while dxr_wl_weave_surface_create
//! holds it across the session's first round trip. Right after the window is hidden it may still
//! give the old handle, until the frame thread reads the compositor's notice (a
//! frame or so; a dialog given that handle opens unparented, as with "").
static int
dxr_wl_export_handle(char *buf, int len)
{
	if (!buf || len <= 0)
		return 0;
	buf[0] = 0;
	pthread_mutex_lock(&s_wl_mutex);
	// Only a watched export counts: an unwatched one may already be gone unseen.
	int watched = s_wl.export_watch || !s_wl.importer;
	int known = s_wl.surface && export_alive_locked() && watched && s_wl.exported_handle[0] &&
	            export_window_shown_locked();
	if (known) {
		int n = snprintf(buf, (size_t)len, "wayland:%s", s_wl.exported_handle);
		known = n > 0 && n < len;
	}
	if (!known)
		buf[0] = 0;
	pthread_mutex_unlock(&s_wl_mutex);
	return known;
}

static int
dxr_wl_window_state(void)
{
	pthread_mutex_lock(&s_wl_mutex);
	const DxrWlExtWindow *w = &s_wl.win;
	int state = -1;
	if (s_wl.ext_available && w->present && w->minimized >= 0 && w->above >= 0)
		state = (w->above ? DXR_WL_OWN_KEEP_ABOVE : 0) | (w->minimized ? DXR_WL_OWN_MINIMIZE : 0);
	pthread_mutex_unlock(&s_wl_mutex);
	return state;
}

static int
dxr_wl_set_minimized(int minimized)
{
	return own_window_action(DXR_WL_EXT_CAP_MINIMIZE, "minimize", DXR_WARN_OWN_MINIMIZE, DXR_WL_EXT_OWN_MINIMIZE,
	                         minimized ? 1 : 0);
}

static int
dxr_wl_begin_pointer_drag(unsigned button)
{
	pthread_mutex_lock(&s_wl_mutex);
	int ok = s_wl.surface && s_wl.ext_available && (s_wl.ext_caps & DXR_WL_EXT_CAP_POINTER_DRAG);
	if (ok) {
		s_wl.drag_state = 1;
		user_moved_locked();
	}
	else if (!(s_wl.warned & DXR_WARN_DRAG))
		fprintf(stderr, "[DisplayXR-WL] window drag unavailable (%s)\n",
		        s_wl.ext_available ? "the GNOME extension has no pointer drag" : "no DisplayXR GNOME extension");
	if (!ok)
		s_wl.warned |= DXR_WARN_DRAG;
	pthread_mutex_unlock(&s_wl_mutex);
	if (ok)
		dxr_wl_ext_begin_pointer_drag(button);
	return ok;
}

static void
dxr_wl_end_pointer_drag(void)
{
	pthread_mutex_lock(&s_wl_mutex);
	int was = s_wl.drag_state;
	s_wl.drag_state = 0;
	s_wl.keep_above_due_ns = 0; // a keep-above refused during the drag can go through now
	if (was)
		s_wl.drag_end_pending = 1; // the snapshot after EndPointerDrag logs where it was left
	pthread_mutex_unlock(&s_wl_mutex);
	if (was) {
		dxr_wl_ext_end_pointer_drag();
		fprintf(stderr, "[DisplayXR-WL] window drag: end\n");
	}
}

//! The size the window will have on `panel`, stage px: the size the app asked for
//! (device px) in the panel's stage px, else its current size.
static void
window_size_on_locked(const struct dxr_wl_output *panel, int layout, int *out_w, int *out_h)
{
	uint32_t p120 = output_scale120_locked(panel, layout);
	double stage_per_device = layout == DXR_LAYOUT_PHYSICAL ? 1.0 : 120.0 / (p120 ? p120 : 120);
	if (s_wl.wanted_device_w > 0 && s_wl.wanted_device_h > 0) {
		*out_w = (int)lround(s_wl.wanted_device_w * stage_per_device);
		*out_h = (int)lround(s_wl.wanted_device_h * stage_per_device);
	} else {
		double scale = layout == DXR_LAYOUT_PHYSICAL ? p120 / 120.0 : 1.0; // surface px -> stage px
		*out_w = (int)lround(s_wl.logical_w * scale);
		*out_h = (int)lround(s_wl.logical_h * scale);
	}
}

//! The top-left that centres the window on `panel` at the size it will have there.
static void
panel_centre_locked(const struct dxr_wl_output *panel, int layout, int *out_x, int *out_y)
{
	int sw, sh;
	window_size_on_locked(panel, layout, &sw, &sh);
	int x = panel->lx + (panel->lw - sw) / 2;
	int y = panel->ly + (panel->lh - sh) / 2;
	*out_x = x < panel->lx ? panel->lx : x;
	*out_y = y < panel->ly ? panel->ly : y;
}

//! Where the window should go: size requests use that output's scale from now on,
//! and it is moved there now if it is on screen, else as soon as it is mapped.
static void
set_intent_locked(int x, int y, const char *why)
{
	s_wl.intent_centred_on[0] = 0; // (the panel move sets it again right after)
	s_wl.intent_valid = 1;
	s_wl.intent_x = x;
	s_wl.intent_y = y;
	int layout = layout_locked();
	struct dxr_wl_output *target = output_at_locked(x, y);
	uint32_t t120 = target && layout != DXR_LAYOUT_UNKNOWN ? output_scale120_locked(target, layout) : 0;
	if (t120 && t120 != s_wl.scale120) {
		s_wl.expect_scale120 = t120;
		// A size the app asked for, at the old scale: re-ask for the target's now.
		// (A baseline is only re-asked once the window is there: the app usually
		// asks for its own size right after placing it.)
		if (s_wl.wanted_by_app)
			rerequest_player_size_locked();
	}
	if (win_mapped(&s_wl.win))
		place_locked(why);
	else
		fprintf(stderr, "[DisplayXR-WL] %s: window -> (%d,%d) once it is mapped\n", why, x, y);
}

//! Placement needs the layout mode; say so once when it cannot be told.
static int
layout_or_warn_locked(void)
{
	int layout = layout_locked();
	if (layout == DXR_LAYOUT_UNKNOWN && !(s_wl.warned & DXR_WARN_LAYOUT)) {
		fprintf(stderr, "[DisplayXR-WL] mutter's layout mode cannot be told (extension says '%s', the outputs "
		                "disagree): no window placement\n",
		        s_wl.ext_layout);
		s_wl.warned |= DXR_WARN_LAYOUT;
	}
	return layout;
}

static int
dxr_wl_move_player_to_panel(const char *connector, int panel_w, int panel_h)
{
	pthread_mutex_lock(&s_wl_mutex);
	if (!s_wl.surface) {
		pthread_mutex_unlock(&s_wl_mutex);
		return 0;
	}
	wl_display_dispatch_queue_pending(s_wl.display, s_wl.queue);
	// By connector name (unambiguous); by mode only when exactly one output has it.
	struct dxr_wl_output *panel = output_named_locked(connector);
	if (!panel) {
		int matches = 0;
		for (int i = 0; i < DXR_WL_MAX_OUTPUTS; i++) {
			struct dxr_wl_output *o = &s_wl.outputs[i];
			if (o->registry_name && o->mode_w == panel_w && o->mode_h == panel_h && o->lw > 0) {
				panel = o;
				matches++;
			}
		}
		if (matches > 1)
			panel = NULL;
	}
	if (!panel) {
		pthread_mutex_unlock(&s_wl_mutex);
		fprintf(stderr, "[DisplayXR-WL] 3D panel: no single output named '%s' or with a %dx%d mode\n",
		        connector ? connector : "", panel_w, panel_h);
		return 0;
	}
	int layout = layout_or_warn_locked();
	if (layout == DXR_LAYOUT_UNKNOWN || !s_wl.ext_available) {
		pthread_mutex_unlock(&s_wl_mutex);
		if (layout != DXR_LAYOUT_UNKNOWN)
			fprintf(stderr, "[DisplayXR-WL] 3D panel '%s': no DisplayXR GNOME extension to move the window\n",
			        panel->name);
		return 0;
	}
	// Already there: leave it, as the X11 and Windows paths do.
	if (win_mapped(&s_wl.win)) {
		int cx = s_wl.win.x + s_wl.win.w / 2, cy = s_wl.win.y + s_wl.win.h / 2;
		if (output_at_locked(cx, cy) == panel) {
			pthread_mutex_unlock(&s_wl_mutex);
			return 1;
		}
	}
	// From here on, every placement is read back until the window is there (#361).
	snprintf(s_wl.verify_panel, sizeof(s_wl.verify_panel), "%s", panel->name);
	s_wl.verify_tries = 0;
	int x, y;
	panel_centre_locked(panel, layout, &x, &y);
	char why[96];
	snprintf(why, sizeof(why), "3D panel '%s' (stage %dx%d at %d,%d)", panel->name, panel->lw, panel->lh, panel->lx,
	         panel->ly);
	set_intent_locked(x, y, why);
	snprintf(s_wl.intent_centred_on, sizeof(s_wl.intent_centred_on), "%s", panel->name);
	pthread_mutex_unlock(&s_wl_mutex);
	return 1;
}

static void
dxr_wl_set_x11_panel_rect(const char *connector, int x, int y, int w, int h)
{
	pthread_mutex_lock(&s_wl_mutex);
	snprintf(s_wl.x11_connector, sizeof(s_wl.x11_connector), "%s", connector ? connector : "");
	s_wl.x11_valid = w > 0 && h > 0;
	s_wl.x11_x = x;
	s_wl.x11_y = y;
	s_wl.x11_w = w;
	s_wl.x11_h = h;
	pthread_mutex_unlock(&s_wl_mutex);
}

//! X root px per stage px, 0 when it cannot be told. The app-facing position API
//! speaks X root coordinates (what the X11 path reads and restores). Under
//! XWayland they are the stage x N: N = the largest scale rounded up with
//! mutter's xwayland-native-scaling in logical layout, 1 without it or in physical
//! layout. Rather than guess which, compare the runtime's panel rect (X root px,
//! from XRandR) with the same panel's stage rect.
static int
x11_scale_locked(void)
{
	struct dxr_wl_output *panel = s_wl.x11_valid ? output_named_locked(s_wl.x11_connector) : NULL;
	int n = 0;
	if (panel && panel->lw > 0) {
		n = (int)lround((double)s_wl.x11_w / panel->lw);
		if (n < 1 || abs(s_wl.x11_w - panel->lw * n) > 1 || abs(s_wl.x11_h - panel->lh * n) > 1 ||
		    abs(s_wl.x11_x - panel->lx * n) > 1 || abs(s_wl.x11_y - panel->ly * n) > 1)
			n = 0;
	}
	if (!n && !(s_wl.warned & DXR_WARN_X11)) {
		fprintf(stderr, "[DisplayXR-WL] window positions cannot be converted to X root coordinates (%s): the "
		                "saved-position API reports them as unknown\n",
		        !s_wl.x11_valid ? "the runtime reported no panel rect"
		        : !panel        ? "no output matches the runtime's panel"
		                        : "the runtime's panel rect does not match the output's");
		s_wl.warned |= DXR_WARN_X11;
	}
	return n;
}

static int
dxr_wl_get_player_position_x11(int *out_x, int *out_y)
{
	pthread_mutex_lock(&s_wl_mutex);
	int known = 0;
	if (s_wl.surface && s_wl.ext_available && (win_mapped(&s_wl.win) || s_wl.intent_valid)) {
		int sx = win_mapped(&s_wl.win) ? s_wl.win.x : s_wl.intent_x;
		int sy = win_mapped(&s_wl.win) ? s_wl.win.y : s_wl.intent_y;
		int n = x11_scale_locked();
		if (n) {
			s_wl.last_x11_valid = 1;
			s_wl.last_x11_x = sx * n;
			s_wl.last_x11_y = sy * n;
			known = 1;
		}
	}
	// Unknown now: answer the last position we could tell (an app that saves what
	// it reads keeps a real one), and say it is not current.
	if (s_wl.last_x11_valid) {
		*out_x = s_wl.last_x11_x;
		*out_y = s_wl.last_x11_y;
	}
	pthread_mutex_unlock(&s_wl_mutex);
	return known;
}

static int
dxr_wl_set_player_position_x11(int x, int y)
{
	pthread_mutex_lock(&s_wl_mutex);
	int n = s_wl.surface ? x11_scale_locked() : 0;
	int layout = n ? layout_or_warn_locked() : DXR_LAYOUT_UNKNOWN;
	if (!n || layout == DXR_LAYOUT_UNKNOWN) {
		pthread_mutex_unlock(&s_wl_mutex);
		return 0;
	}
	// A saved position restored at start (#361): one saved under another monitor
	// layout can lie anywhere today. Off the 3D panel, it is stale: centre on the
	// panel instead. (Only while the window is new and the user has not moved it;
	// later moves are the app's to make.)
	struct dxr_wl_output *panel = output_named_locked(s_wl.x11_connector);
	if (panel && restore_open_locked()) {
		int sw, sh;
		window_size_on_locked(panel, layout, &sw, &sh);
		if (!dxr_wl_saved_position_on_panel(output_rect(panel), x, y, n, sw, sh)) {
			fprintf(stderr, "[DisplayXR-WL] saved position (%d,%d) is outside the 3D panel's rect — ignoring, "
			                "centring on %s\n",
			        x, y, panel->name);
			int cx, cy;
			panel_centre_locked(panel, layout, &cx, &cy);
			if (!s_wl.intent_valid || cx != s_wl.intent_x || cy != s_wl.intent_y ||
			    strcmp(s_wl.intent_centred_on, panel->name)) {
				char why[96];
				snprintf(why, sizeof(why), "3D panel '%s' (stage %dx%d at %d,%d)", panel->name, panel->lw,
				         panel->lh, panel->lx, panel->ly);
				set_intent_locked(cx, cy, why);
				snprintf(s_wl.intent_centred_on, sizeof(s_wl.intent_centred_on), "%s", panel->name);
			}
			if (strcmp(s_wl.verify_panel, panel->name)) {
				snprintf(s_wl.verify_panel, sizeof(s_wl.verify_panel), "%s", panel->name);
				s_wl.verify_tries = 0;
			}
			// Already on screen and no move under way: check it is there.
			if (win_mapped(&s_wl.win) && !s_wl.move_inflight)
				schedule_verify_locked();
			pthread_mutex_unlock(&s_wl_mutex);
			return 1;
		}
	}
	int sx = dxr_wl_x11_to_stage(x, n), sy = dxr_wl_x11_to_stage(y, n);
	// A new place the app asks for gets a fresh correction budget: the cap stops a
	// fight with a compositor that keeps placing the window elsewhere, not an app that
	// moves its own window again and again (one stepping aside and back used it up).
	if (!s_wl.intent_valid || sx != s_wl.intent_x || sy != s_wl.intent_y) {
		s_wl.corrections = 0;
		s_wl.warned &= ~DXR_WARN_GAVE_UP;
	}
	char why[96];
	snprintf(why, sizeof(why), "the app's position X (%d,%d)", x, y);
	set_intent_locked(sx, sy, why);
	// Moved off the panel on purpose: nothing to verify.
	struct dxr_wl_output *vp = output_named_locked(s_wl.verify_panel);
	if (vp && !dxr_wl_window_on_rect(output_rect(vp), sx, sy, s_wl.win.w, s_wl.win.h))
		stop_verify_locked();
	pthread_mutex_unlock(&s_wl_mutex);
	return 1;
}

//! The side-step's move (displayxr_linux_side_step.c): stage px, straight to the
//! intent. Like an app's move, a new place gets a fresh correction budget (#382), and
//! a move off the panel being verified ends that check. Only the glide's last step
//! (@p log) is logged.
static int
dxr_wl_set_player_position_stage(int x, int y, int log)
{
	pthread_mutex_lock(&s_wl_mutex);
	int ok = s_wl.surface && s_wl.ext_available;
	if (ok) {
		if (!s_wl.intent_valid || x != s_wl.intent_x || y != s_wl.intent_y) {
			s_wl.corrections = 0;
			s_wl.warned &= ~DXR_WARN_GAVE_UP;
		}
		s_quiet_place = !log;
		set_intent_locked(x, y, "side-step");
		s_quiet_place = 0;
		struct dxr_wl_output *vp = output_named_locked(s_wl.verify_panel);
		if (vp && !dxr_wl_window_on_rect(output_rect(vp), x, y, s_wl.win.w, s_wl.win.h))
			stop_verify_locked();
	}
	pthread_mutex_unlock(&s_wl_mutex);
	return ok;
}

/*
 *
 * File drops: files dragged onto the player's window from a file manager, for
 * the app (dxr_wl_file_drop_enable / dxr_wl_file_drop_next).
 *
 * Unity's SDL handles Wayland drag and drop itself, but it is linked statically
 * and exports nothing, so its drop events cannot be reached. So this library
 * makes its own wl_data_device for the seat, on our queue, and is the drop
 * target. Opt-in, because of what it costs.
 *
 * The cost. Mutter (50) keeps ONE data device per client: making one unlinks
 * the client's previous one (get_data_device), and an unlinked device is sent
 * neither drags nor clipboard offers again — also after ours is released. So
 * from the moment drops are on, SDL gets no new clipboard offers, for the rest
 * of the run: GUIUtility.systemCopyBuffer can no longer be relied on for what
 * other apps copy. Measured on GNOME 50.1: it read "" for every other app's copy
 * when SDL had never had an offer, and the current text when it still held one
 * from before (mutter answers an old offer with the current clipboard). Unity's
 * own copies still read back. Its writes still reach the compositor (a set_selection on an
 * unlinked device is still the seat's; mutter takes it only from the focused
 * client, drops or not). An app that turns drops on reads the clipboard
 * elsewhere (the RemoteDesktop portal's Clipboard, which also works without
 * keyboard focus). SDL starts no drags in Unity's player, so nothing else is lost.
 *
 * Mutter only unlinks a device that is on its list of UNFOCUSED clients' devices:
 * made while the player has keyboard focus, ours and SDL's both stay linked, and
 * after the next focus change a drag goes to whichever comes first in mutter's
 * list — SDL's, at the first one. So the device is made again each time the
 * player loses focus: ours released first, then a new one made, which unlinks
 * the only other one the player has, SDL's. Our own wl_keyboard (on our queue,
 * made with the first device) says when; on a seat without a keyboard, the GNOME
 * extension's report of the window does (it reports every focus change). While
 * the player has focus, drags go to ours anyway (mutter looks among the
 * unfocused clients' devices first). A drag that starts in the frame between the
 * focus loss and the remake goes to SDL.
 *
 * A drag over the window: on enter, an offer of text/uri-list over the player's
 * own surface (not a client-side title bar, whose offset we do not know) is
 * accepted for COPY only (a MOVE would have the file manager delete the file)
 * and the list is asked for at once through a non-blocking pipe, read by the
 * frame thread's poll (DXR_WL_DROP_READ_NS at most). The app hears ENTER once
 * the paths are known (a list with no local file is refused, and the app hears
 * nothing), then MOVEs (coalesced), then LEAVE or DROP. A drop is finished
 * (wl_data_offer.finish) only with the COPY action the compositor chose: mutter
 * answers a premature or action-less finish with a protocol error, which would
 * end the PLAYER's connection.
 *
 * Coordinates: surface-local logical px from the protocol; the window px the app
 * gets are device px (x the fractional scale), as the rest of the window API; the
 * desktop point is the window's main surface on the stage (the extension's
 * "buffer" rect) plus the surface point, then x the XWayland scale for X root
 * px (x11_scale_locked, as the position API). Without the extension, or without
 * the runtime's panel rect, the event says which of them it lacks.
 *
 * Everything runs under s_wl_mutex: the handlers on whichever thread dispatches
 * our queue (the frame thread; the main thread in dxr_wl_move_player_to_panel),
 * the app's calls on any thread.
 *
 */

static void drag_clear_locked(int tell_leave);
static void drop_complete_locked(void);

static void
offer_offer(void *data, struct wl_data_offer *offer, const char *mime_type)
{
	(void)offer;
	struct dxr_wl_offer *o = data;
	if (mime_type && !strcmp(mime_type, DXR_WL_URI_LIST))
		o->uri_list = 1;
}

static void
offer_source_actions(void *data, struct wl_data_offer *offer, uint32_t source_actions)
{
	(void)data;
	(void)offer;
	(void)source_actions;
}

static void
offer_action(void *data, struct wl_data_offer *offer, uint32_t dnd_action)
{
	(void)offer;
	struct dxr_wl_offer *o = data;
	o->action = dnd_action;
}

static const struct wl_data_offer_listener s_offer_listener = {
    .offer = offer_offer,
    .source_actions = offer_source_actions,
    .action = offer_action,
};

static void
offer_destroy(struct dxr_wl_offer *o)
{
	if (!o)
		return;
	wl_data_offer_destroy(o->offer);
	free(o);
}

//! The pointer's position in the event: window px always, stage and X root px when known.
static void
drop_coords_locked(DxrWlDropEvent *e)
{
	double scale = s_wl.scale120 ? s_wl.scale120 / 120.0 : 1.0;
	e->window_x = dxr_wl_round(s_wl.drag.lx * scale);
	e->window_y = dxr_wl_round(s_wl.drag.ly * scale);
	e->flags = 0;
	const DxrWlExtWindow *w = &s_wl.win;
	if (!s_wl.ext_available || !win_mapped(w))
		return;
	// The main surface's top-left; an extension that does not report it gives the
	// frame, which is off by a client-side title bar (Unity's borderless window has none).
	int ox = w->bw > 0 ? w->bx : w->x, oy = w->bw > 0 ? w->by : w->y;
	// Stage px per surface px: 1 in logical layout; the output's integer scale in
	// physical layout (the surface is drawn at buffer scale 1, scaled up there).
	int k = 1;
	if (layout_locked() == DXR_LAYOUT_PHYSICAL && w->bw > 0 && s_wl.logical_w > 0) {
		k = dxr_wl_round((double)w->bw / s_wl.logical_w);
		if (k < 1)
			k = 1;
	}
	int n = x11_scale_locked();
	int rx = 0, ry = 0;
	int root = dxr_wl_surface_point(ox, s_wl.drag.lx, k, n, &e->stage_x, &rx);
	dxr_wl_surface_point(oy, s_wl.drag.ly, k, n, &e->stage_y, &ry);
	e->flags |= DXR_WL_DROP_HAS_STAGE;
	if (root) {
		e->x = rx;
		e->y = ry;
		e->flags |= DXR_WL_DROP_HAS_ROOT;
	}
}

static void
drop_push_locked(int kind, const char *paths, size_t bytes, int count)
{
	// A MOVE replaces one still queued: the app wants where the pointer is now.
	if (kind == DXR_WL_DROP_MOVE && s_wl.drop_count) {
		struct dxr_wl_drop_ev *last = &s_wl.drop_q[(s_wl.drop_head + s_wl.drop_count - 1) % DXR_WL_DROP_QUEUE];
		if (last->e.kind == DXR_WL_DROP_MOVE) {
			drop_coords_locked(&last->e);
			return;
		}
	}
	if (s_wl.drop_count == DXR_WL_DROP_QUEUE) {
		// Nobody drains the queue: keep the latest.
		struct dxr_wl_drop_ev *old = &s_wl.drop_q[s_wl.drop_head];
		free(old->paths);
		old->paths = NULL;
		s_wl.drop_head = (s_wl.drop_head + 1) % DXR_WL_DROP_QUEUE;
		s_wl.drop_count--;
		if (!(s_wl.warned & DXR_WARN_DROP_QUEUE))
			fprintf(stderr, "[DisplayXR-WL] file drop: the app does not take its drop events "
			                "(displayxr_linux_file_drop_next): the oldest are dropped\n");
		s_wl.warned |= DXR_WARN_DROP_QUEUE;
	}
	struct dxr_wl_drop_ev *ev = &s_wl.drop_q[(s_wl.drop_head + s_wl.drop_count) % DXR_WL_DROP_QUEUE];
	memset(ev, 0, sizeof(*ev));
	ev->e.kind = kind;
	drop_coords_locked(&ev->e);
	if (paths && bytes > 0) {
		ev->paths = malloc(bytes);
		if (!ev->paths)
			return; // out of memory: the event is lost
		memcpy(ev->paths, paths, bytes);
		ev->e.paths_bytes = (int32_t)bytes;
		ev->e.path_count = count;
	}
	s_wl.drop_count++;
}

//! Where the drop point is, for the log.
static void
drop_log_locked(const char *what, int count)
{
	DxrWlDropEvent e = {0};
	drop_coords_locked(&e);
	char where[96] = "";
	if (e.flags & DXR_WL_DROP_HAS_ROOT)
		snprintf(where, sizeof(where), ", X root (%d,%d)", e.x, e.y);
	else if (e.flags & DXR_WL_DROP_HAS_STAGE)
		snprintf(where, sizeof(where), ", stage (%d,%d), no X root", e.stage_x, e.stage_y);
	fprintf(stderr, "[DisplayXR-WL] file drop: %s %d file%s at window (%d,%d) px%s\n", what, count,
	        count == 1 ? "" : "s", e.window_x, e.window_y, where);
}

//! The drag is over (or never was ours): forget it. @p tell_leave: the app heard
//! ENTER, so it hears LEAVE.
static void
drag_clear_locked(int tell_leave)
{
	if (tell_leave && s_wl.drag.told) {
		drop_push_locked(DXR_WL_DROP_LEAVE, NULL, 0, 0);
		fprintf(stderr, "[DisplayXR-WL] file drop: the files left the window (or the drag was cancelled)\n");
	}
	if (s_wl.drag.reading)
		close(s_wl.drag.fd);
	free(s_wl.drag.buf);
	free(s_wl.drag.paths);
	offer_destroy(s_wl.drag.o);
	memset(&s_wl.drag, 0, sizeof(s_wl.drag));
}

static const char *
self_host(void)
{
	static char host[256];
	static int done;
	if (!done) {
		if (gethostname(host, sizeof(host) - 1) != 0)
			host[0] = 0;
		host[sizeof(host) - 1] = 0;
		done = 1;
	}
	return host[0] ? host : NULL;
}

//! Read what has arrived of the dragged file list (non-blocking); once it is all
//! in (or the source gave up), tell the app or refuse the drag.
static void
drop_read_locked(void)
{
	if (!s_wl.drag.reading)
		return;
	int done = 0;
	const char *failed = NULL;
	for (;;) {
		if (s_wl.drag.cap - s_wl.drag.len < 4096) {
			if (s_wl.drag.cap >= DXR_WL_DROP_READ_MAX) {
				failed = "longer than 1 MiB";
				break;
			}
			size_t cap = s_wl.drag.cap ? s_wl.drag.cap * 2 : 16384;
			char *b = realloc(s_wl.drag.buf, cap);
			if (!b) {
				failed = "out of memory";
				break;
			}
			s_wl.drag.buf = b;
			s_wl.drag.cap = cap;
		}
		ssize_t n = read(s_wl.drag.fd, s_wl.drag.buf + s_wl.drag.len, s_wl.drag.cap - s_wl.drag.len);
		if (n > 0) {
			s_wl.drag.len += (size_t)n;
			continue;
		}
		if (n == 0) {
			done = 1;
			break;
		}
		if (errno == EINTR)
			continue;
		if (errno != EAGAIN && errno != EWOULDBLOCK)
			failed = strerror(errno);
		break;
	}
	if (!done && !failed) {
		if (mono_ns() < s_wl.drag.deadline_ns)
			return; // more to come
		failed = "the drag's source did not send it in time";
	}
	close(s_wl.drag.fd);
	s_wl.drag.reading = 0;
	s_wl.drag.read_done = 1;
	size_t list_len = s_wl.drag.len;
	if (done) {
		s_wl.drag.paths = malloc(s_wl.drag.len + 1);
		if (s_wl.drag.paths)
			s_wl.drag.path_count = dxr_uri_list_to_paths(s_wl.drag.buf, s_wl.drag.len, self_host(),
			                                             s_wl.drag.paths, &s_wl.drag.paths_bytes);
	} else {
		fprintf(stderr, "[DisplayXR-WL] file drop: the dragged file list could not be read (%s)\n", failed);
	}
	free(s_wl.drag.buf);
	s_wl.drag.buf = NULL;
	s_wl.drag.len = s_wl.drag.cap = 0;
	if (s_wl.drag.path_count > 0 && !s_wl.drag.dropped) {
		drop_push_locked(DXR_WL_DROP_ENTER, s_wl.drag.paths, s_wl.drag.paths_bytes, s_wl.drag.path_count);
		s_wl.drag.told = 1;
		drop_log_locked("drag entered with", s_wl.drag.path_count);
	} else if (s_wl.drag.path_count == 0) {
		// No local file (links, a remote location): not ours to take.
		if (!s_wl.drag.dropped) {
			wl_data_offer_accept(s_wl.drag.o->offer, s_wl.drag.serial, NULL);
			wl_display_flush(s_wl.display);
		}
		if (done && !(s_wl.warned & DXR_WARN_DROP_NOT_FILES))
			fprintf(stderr, "[DisplayXR-WL] file drop: the dragged list names no local file (%zu bytes): refused\n",
			        list_len);
		if (done)
			s_wl.warned |= DXR_WARN_DROP_NOT_FILES;
	}
	if (s_wl.drag.dropped)
		drop_complete_locked();
}

//! Dropped, and the list is in: hand it to the app, finish the drop, forget it.
static void
drop_complete_locked(void)
{
	int n = s_wl.drag.path_count;
	if (n > 0) {
		if (!s_wl.drag.told) // dropped before the list was in: the app hears both
			drop_push_locked(DXR_WL_DROP_ENTER, s_wl.drag.paths, s_wl.drag.paths_bytes, n);
		drop_push_locked(DXR_WL_DROP_DROP, s_wl.drag.paths, s_wl.drag.paths_bytes, n);
		drop_log_locked("dropped", n);
	}
	// finish only for a drop we took with the COPY action the compositor chose
	// (mutter: a finish without an accepted type and a copy / move action is a
	// protocol error). Otherwise destroying the offer cancels the drop for the source.
	if (n > 0 && s_wl.drag.o->action == WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY)
		wl_data_offer_finish(s_wl.drag.o->offer);
	s_wl.drag.told = 0; // DROP ends it for the app: no LEAVE
	drag_clear_locked(0);
	wl_display_flush(s_wl.display);
}

static void
dd_data_offer(void *data, struct wl_data_device *dev, struct wl_data_offer *offer)
{
	(void)data;
	(void)dev;
	struct dxr_wl_offer *o = calloc(1, sizeof(*o));
	if (!o) {
		wl_data_offer_destroy(offer); // enter / selection then get no offer
		return;
	}
	o->offer = offer;
	wl_data_offer_add_listener(offer, &s_offer_listener, o);
}

static void
dd_enter(void *data, struct wl_data_device *dev, uint32_t serial, struct wl_surface *surface, wl_fixed_t x,
         wl_fixed_t y, struct wl_data_offer *offer)
{
	(void)data;
	(void)dev;
	drag_clear_locked(1); // a drag that never ended for us (cannot happen in order)
	struct dxr_wl_offer *o = offer ? wl_data_offer_get_user_data(offer) : NULL;
	if (!o)
		return; // a drag with no data
	s_wl.drag.o = o;
	s_wl.drag.serial = serial;
	s_wl.drag.lx = wl_fixed_to_double(x);
	s_wl.drag.ly = wl_fixed_to_double(y);
	// Over the player's own surface only: a client-side title bar is another
	// surface, at an offset we do not know (the weave takes no input at all).
	if (!o->uri_list || !surface || surface != s_wl.parent) {
		wl_data_offer_accept(offer, serial, NULL);
		wl_display_flush(s_wl.display);
		return;
	}
	s_wl.drag.files = 1;
	wl_data_offer_accept(offer, serial, DXR_WL_URI_LIST);
	wl_data_offer_set_actions(offer, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
	int fds[2];
	if (pipe2(fds, O_CLOEXEC | O_NONBLOCK) == 0) {
		wl_data_offer_receive(offer, DXR_WL_URI_LIST, fds[1]);
		close(fds[1]);
		s_wl.drag.fd = fds[0];
		s_wl.drag.reading = 1;
		s_wl.drag.deadline_ns = mono_ns() + DXR_WL_DROP_READ_NS;
	} else {
		fprintf(stderr, "[DisplayXR-WL] file drop: no pipe for the file list (%s): drag refused\n", strerror(errno));
		s_wl.drag.files = 0;
		wl_data_offer_accept(offer, serial, NULL);
	}
	wl_display_flush(s_wl.display);
}

static void
dd_leave(void *data, struct wl_data_device *dev)
{
	(void)data;
	(void)dev;
	if (s_wl.drag.dropped)
		return; // after a drop (mutter sends leave then too): the drop ends it
	drag_clear_locked(1);
}

static void
dd_motion(void *data, struct wl_data_device *dev, uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
	(void)data;
	(void)dev;
	(void)time;
	if (!s_wl.drag.o || s_wl.drag.dropped)
		return;
	s_wl.drag.lx = wl_fixed_to_double(x);
	s_wl.drag.ly = wl_fixed_to_double(y);
	if (s_wl.drag.told)
		drop_push_locked(DXR_WL_DROP_MOVE, NULL, 0, 0);
}

static void
dd_drop(void *data, struct wl_data_device *dev)
{
	(void)data;
	(void)dev;
	if (!s_wl.drag.o || s_wl.drag.dropped)
		return;
	if (!s_wl.drag.files) { // not taken (the compositor does not send a drop then)
		drag_clear_locked(0);
		return;
	}
	s_wl.drag.dropped = 1;
	if (s_wl.drag.read_done)
		drop_complete_locked();
	// else drop_read_locked completes it once the list is in
}

static void
dd_selection(void *data, struct wl_data_device *dev, struct wl_data_offer *offer)
{
	(void)data;
	(void)dev;
	// The clipboard, now offered to our device (no longer to SDL's) while the
	// player has focus. Not read: only the latest is kept, as the protocol asks.
	struct dxr_wl_offer *o = offer ? wl_data_offer_get_user_data(offer) : NULL;
	if (s_wl.sel_offer && s_wl.sel_offer != o)
		offer_destroy(s_wl.sel_offer);
	s_wl.sel_offer = o;
}

static const struct wl_data_device_listener s_data_device_listener = {
    .data_offer = dd_data_offer,
    .enter = dd_enter,
    .leave = dd_leave,
    .motion = dd_motion,
    .drop = dd_drop,
    .selection = dd_selection,
};

static void
kb_keymap(void *data, struct wl_keyboard *kb, uint32_t format, int32_t fd, uint32_t size)
{
	(void)data;
	(void)kb;
	(void)format;
	(void)size;
	close(fd);
}

static void
kb_enter(void *data, struct wl_keyboard *kb, uint32_t serial, struct wl_surface *surface, struct wl_array *keys)
{
	(void)data;
	(void)kb;
	(void)serial;
	(void)surface;
	(void)keys;
	s_wl.kb_focus = 1;
}

static void
kb_leave(void *data, struct wl_keyboard *kb, uint32_t serial, struct wl_surface *surface)
{
	(void)data;
	(void)kb;
	(void)serial;
	(void)surface;
	s_wl.kb_focus = 0;
	if (s_wl.ddev)
		s_wl.drop_remake = 1; // see "File drops": now SDL's device can be unlinked
}

static void
kb_key(void *data, struct wl_keyboard *kb, uint32_t serial, uint32_t time, uint32_t key, uint32_t state)
{
	(void)data;
	(void)kb;
	(void)serial;
	(void)time;
	(void)key;
	(void)state;
}

static void
kb_modifiers(void *data, struct wl_keyboard *kb, uint32_t serial, uint32_t depressed, uint32_t latched,
             uint32_t locked, uint32_t group)
{
	(void)data;
	(void)kb;
	(void)serial;
	(void)depressed;
	(void)latched;
	(void)locked;
	(void)group;
}

static void
kb_repeat_info(void *data, struct wl_keyboard *kb, int32_t rate, int32_t delay)
{
	(void)data;
	(void)kb;
	(void)rate;
	(void)delay;
}

// wl_seat version 1: repeat_info is never sent; listed for completeness.
static const struct wl_keyboard_listener s_keyboard_listener = {
    .keymap = kb_keymap,
    .enter = kb_enter,
    .leave = kb_leave,
    .key = kb_key,
    .modifiers = kb_modifiers,
    .repeat_info = kb_repeat_info,
};

//! Our wl_keyboard, once drops have been on (kept: a version-1 seat's keyboard
//! has no release; its few events are dispatched and ignored afterwards).
static void
drop_keyboard_locked(void)
{
	if (!(s_wl.seat_caps & WL_SEAT_CAPABILITY_KEYBOARD)) {
		if (s_wl.keyboard) {
			wl_keyboard_destroy(s_wl.keyboard);
			s_wl.keyboard = NULL;
			s_wl.kb_focus = 0;
		}
		return;
	}
	if (s_wl.keyboard || !s_wl.ddev || !s_wl.seat)
		return;
	s_wl.keyboard = wl_seat_get_keyboard(s_wl.seat);
	wl_keyboard_add_listener(s_wl.keyboard, &s_keyboard_listener, NULL);
}

static int
drop_supported_locked(void)
{
	return s_wl.ddm && s_wl.ddm_version >= 3 && s_wl.seat;
}

//! Whether the player has the focus: 1, 0, or -1 when it cannot be told. From
//! our wl_keyboard (exact) or, on a seat without a keyboard, from the GNOME
//! extension's report of the window (it reports every focus change).
static int
player_focused_locked(void)
{
	if (s_wl.keyboard)
		return s_wl.kb_focus;
	if (s_wl.ext_available && s_wl.win.present && s_wl.win.focus >= 0)
		return s_wl.win.focus;
	return -1;
}

//! A report of our window from the extension: losing the focus is when SDL's
//! data device can be unlinked (used where the seat has no keyboard to tell).
static void
drop_focus_report_locked(const DxrWlExtWindow *w)
{
	if (!s_wl.ddev || s_wl.keyboard || !w->present || w->focus != 0)
		return;
	if (!s_wl.win.present || s_wl.win.focus != 0)
		s_wl.drop_remake = 1;
}

//! Make our data device (again): ours released first, then a new one, which
//! unlinks the player's other one (SDL's) if mutter lists it among the unfocused.
//! @p first: drops were just turned on; if the player may have the focus right
//! now, SDL's device survived, and the device is made again once it has not.
static void
drop_device_make_locked(const char *why, int first)
{
	if (!drop_supported_locked())
		return;
	if (s_wl.ddev) {
		drag_clear_locked(1);
		offer_destroy(s_wl.sel_offer);
		s_wl.sel_offer = NULL;
		wl_data_device_release(s_wl.ddev);
	}
	s_wl.ddev = wl_data_device_manager_get_data_device(s_wl.ddm, s_wl.seat);
	wl_data_device_add_listener(s_wl.ddev, &s_data_device_listener, NULL);
	drop_keyboard_locked();
	s_wl.drop_remake = first && player_focused_locked() != 0;
	s_wl.drop_makes++;
	wl_display_flush(s_wl.display);
	if (s_wl.drop_makes == 1) {
		fprintf(stderr, "[DisplayXR-WL] file drops on (%s): this app's data device replaces SDL's, so from now on "
		                "SDL gets no new clipboard offers: GUIUtility.systemCopyBuffer cannot be relied on for what "
		                "other apps copy, for the rest of this run\n",
		        why);
		if (!s_wl.keyboard && !(s_wl.warned & DXR_WARN_DROP_NO_KEYBOARD))
			fprintf(stderr, "[DisplayXR-WL] file drops: the seat has no keyboard; the focus is followed through the "
			                "GNOME extension%s\n",
			        s_wl.ext_available ? "" : " (not running: a drag may go to SDL after the player had the focus)");
		if (!s_wl.keyboard)
			s_wl.warned |= DXR_WARN_DROP_NO_KEYBOARD;
	} else if (s_wl.drop_makes == 2) {
		fprintf(stderr, "[DisplayXR-WL] file drops: data device made again (%s); quietly from now on\n", why);
	}
}

static void
drop_device_destroy_locked(void)
{
	drag_clear_locked(1);
	offer_destroy(s_wl.sel_offer);
	s_wl.sel_offer = NULL;
	if (s_wl.ddev) {
		wl_data_device_release(s_wl.ddev);
		s_wl.ddev = NULL;
		wl_display_flush(s_wl.display);
	}
	s_wl.drop_remake = 0;
}

//! Per frame, after our queue was dispatched: the file list arriving; the device
//! made again once the player has lost focus (and no drag is over it).
static void
drop_poll_locked(void)
{
	drop_read_locked();
	if (s_wl.drop_remake && s_wl.ddev && !s_wl.drag.o && player_focused_locked() == 0)
		drop_device_make_locked("the player lost the focus", 0);
}

static int
dxr_wl_file_drop_enable(int enable)
{
	pthread_mutex_lock(&s_wl_mutex);
	int want = enable ? 1 : 0, r = 0;
	int changed = want != s_wl.drop_wanted;
	s_wl.drop_wanted = want;
	if (!want) {
		if (s_wl.ddev) {
			drop_device_destroy_locked();
			fprintf(stderr, "[DisplayXR-WL] file drops off (SDL's clipboard does not get other apps' copies back "
			                "until the app restarts)\n");
		}
	} else if (s_wl.registry && !drop_supported_locked()) {
		r = -1;
		if (!(s_wl.warned & DXR_WARN_DROP_UNSUPPORTED))
			fprintf(stderr, "[DisplayXR-WL] file drops unavailable: the compositor has no wl_data_device_manager "
			                "version 3 (it has %u) or no seat\n",
			        s_wl.ddm ? s_wl.ddm_version : 0u);
		s_wl.warned |= DXR_WARN_DROP_UNSUPPORTED;
	} else if (!s_wl.surface) {
		if (changed)
			fprintf(stderr, "[DisplayXR-WL] file drops: on once the 3D session binds the window\n");
	} else {
		if (!s_wl.ddev)
			drop_device_make_locked("the app turned them on", 1);
		r = s_wl.ddev ? 1 : -1;
	}
	pthread_mutex_unlock(&s_wl_mutex);
	return r;
}

static int
dxr_wl_file_drop_next(DxrWlDropEvent *event, char *paths, int paths_len)
{
	if (!event)
		return 0;
	pthread_mutex_lock(&s_wl_mutex);
	if (!s_wl.drop_count) {
		pthread_mutex_unlock(&s_wl_mutex);
		return 0;
	}
	struct dxr_wl_drop_ev *ev = &s_wl.drop_q[s_wl.drop_head];
	*event = ev->e;
	if (ev->e.paths_bytes > 0 && (!paths || paths_len < ev->e.paths_bytes)) {
		pthread_mutex_unlock(&s_wl_mutex);
		return -1; // kept: the caller asks again with event->paths_bytes of room
	}
	if (ev->e.paths_bytes > 0)
		memcpy(paths, ev->paths, (size_t)ev->e.paths_bytes);
	else if (paths && paths_len > 0)
		paths[0] = 0;
	free(ev->paths);
	ev->paths = NULL;
	s_wl.drop_head = (s_wl.drop_head + 1) % DXR_WL_DROP_QUEUE;
	s_wl.drop_count--;
	pthread_mutex_unlock(&s_wl_mutex);
	return event->kind;
}

/*
 *
 * The library's one export (displayxr_linux_wayland_lib.h).
 *
 */

static const DxrWlApi s_api = {
    .abi = DXR_WL_LIB_ABI,
    .weave_surface_create = dxr_wl_weave_surface_create,
    .weave_destroy = dxr_wl_weave_destroy,
    .weave_set_transparent = dxr_wl_weave_set_transparent,
    .weave_poll = dxr_wl_weave_poll,
    .weave_active = dxr_wl_weave_active,
    .weave_device_size = dxr_wl_weave_device_size,
    .ui_scale = dxr_wl_ui_scale,
    .pointer_inside = dxr_wl_pointer_inside,
    .click_through_wanted = dxr_wl_click_through_wanted,
    .set_player_input_region = dxr_wl_set_player_input_region,
    .begin_pointer_drag = dxr_wl_begin_pointer_drag,
    .end_pointer_drag = dxr_wl_end_pointer_drag,
    .request_player_size = dxr_wl_request_player_size,
    .take_player_size = dxr_wl_take_player_size,
    .move_player_to_panel = dxr_wl_move_player_to_panel,
    .get_player_position_x11 = dxr_wl_get_player_position_x11,
    .set_player_position_x11 = dxr_wl_set_player_position_x11,
    .player_generation = dxr_wl_player_generation,
    .set_x11_panel_rect = dxr_wl_set_x11_panel_rect,
    .own_window_caps = dxr_wl_own_window_caps,
    .set_keep_above = dxr_wl_set_keep_above,
    .activate_window = dxr_wl_activate_window,
    .set_minimized = dxr_wl_set_minimized,
    .window_state = dxr_wl_window_state,
    .export_handle = dxr_wl_export_handle,
    .file_drop_enable = dxr_wl_file_drop_enable,
    .file_drop_next = dxr_wl_file_drop_next,
    .set_player_position_stage = dxr_wl_set_player_position_stage,
};

__attribute__((visibility("default"))) const DxrWlApi *
dxr_wl_lib_init(const DxrWlHost *host)
{
	if (!host || host->abi != DXR_WL_LIB_ABI || !host->unity_surface || !host->unity_swapchain_size ||
	    !host->player_surface_generation || !host->lock_player_surface || !host->unlock_player_surface ||
	    !host->player_presents_alpha)
		return NULL;
	s_host = host;
	return &s_api;
}
