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
// objects land on our queue, which only the provider's frame thread dispatches.
// Requests on the player's own surface are only made while the capture layer's
// lock is held (the player may destroy that surface at any time otherwise).
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

#include "displayxr_linux_wayland_dbus.h"
#include "displayxr_linux_wayland_lib.h"

#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define DXR_WL_MAX_OUTPUTS 8
#define DXR_WL_MAX_CORRECTIONS 3 // re-placements per map: never fight the compositor

enum { DXR_LAYOUT_UNKNOWN, DXR_LAYOUT_LOGICAL, DXR_LAYOUT_PHYSICAL };

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
	struct zxdg_output_manager_v1 *xdg_output_manager;
	struct dxr_wl_output outputs[DXR_WL_MAX_OUTPUTS];

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

	// The DisplayXR GNOME extension, and our window as it last reported it.
	int ext_available;
	unsigned ext_caps;
	char ext_layout[16]; // "logical", "physical", "" (not reported)
	DxrWlExtWindow win;
	int drag_state; // 0 none, 1 asked for, 2 dragging

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

	// The 3D panel as the runtime reports it in X root coordinates: the space the
	// app-facing position API speaks (displayxr_get/set_overlay_position).
	char x11_connector[64];
	int x11_valid, x11_x, x11_y, x11_w, x11_h;
	int last_x11_valid, last_x11_x, last_x11_y; // the last position we could convert

	unsigned warned; // one-time log bits (DXR_WARN_*)
} s_wl;

#define DXR_WARN_LAYOUT 1u
#define DXR_WARN_X11 2u
#define DXR_WARN_DRAG 4u
#define DXR_WARN_GAVE_UP 8u

// Guards s_wl: the provider's frame thread polls, C#'s main thread asks for
// resizes, positions and input regions, the extension worker reports. Lock order:
// this one, then the capture layer's player lock, then the worker's job lock.
static pthread_mutex_t s_wl_mutex = PTHREAD_MUTEX_INITIALIZER;

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
registry_global(void *data, struct wl_registry *reg, uint32_t name, const char *iface, uint32_t version)
{
	(void)data;
	if (strcmp(iface, wl_compositor_interface.name) == 0) {
		s_wl.compositor = wl_registry_bind(reg, name, &wl_compositor_interface, version < 4 ? version : 4);
	} else if (strcmp(iface, wl_subcompositor_interface.name) == 0) {
		s_wl.subcompositor = wl_registry_bind(reg, name, &wl_subcompositor_interface, 1);
	} else if (strcmp(iface, wl_shm_interface.name) == 0) {
		s_wl.shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
	} else if (strcmp(iface, wp_viewporter_interface.name) == 0) {
		s_wl.viewporter = wl_registry_bind(reg, name, &wp_viewporter_interface, 1);
	} else if (strcmp(iface, wp_fractional_scale_manager_v1_interface.name) == 0) {
		s_wl.frac_manager = wl_registry_bind(reg, name, &wp_fractional_scale_manager_v1_interface, 1);
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
	attach_to_parent_locked(player);
	s_wl.parent_generation = generation;
	s_host->unlock_player_surface();
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
	s_wl.geometry_dirty = 0; // the create-time size goes into XrWaylandSurfaceGeometryDXR
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
	s_wl.frac = NULL;
	s_wl.viewport = NULL;
	s_wl.subsurface = NULL;
	s_wl.surface = NULL;
	s_wl.probe_buffer = NULL;
	s_wl.parent = NULL;
	s_wl.transparent = 0;
	s_wl.placing = s_wl.move_inflight = s_wl.rescale_after_place = 0;
	s_wl.intent_blocked = 0;
	s_wl.size_fixups = 0;
	s_wl.wanted_device_w = s_wl.wanted_device_h = 0;
	s_wl.wanted_by_app = 0;
	s_wl.sent_w = s_wl.sent_h = 0;
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
	wl_display_dispatch_queue_pending(s_wl.display, s_wl.queue);
	int dirty = s_wl.geometry_dirty;
	if (dirty) {
		s_wl.geometry_dirty = 0;
		*out_w = s_wl.device_w;
		*out_h = s_wl.device_h;
	}
	pthread_mutex_unlock(&s_wl_mutex);
	return dirty;
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
		*out_w = s_wl.sent_w = s_wl.pending_w;
		*out_h = s_wl.sent_h = s_wl.pending_h;
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

//! A placement ended (the window is where it should be, or where the compositor
//! insists on): act on a scale change seen meanwhile, and re-ask for the size the
//! app wants if the player came back at another one (twice in a row at most, so a
//! player that keeps refusing a size cannot make us re-map it forever).
static void
place_done_locked(void)
{
	s_wl.placing = 0;
	s_wl.move_inflight = 0;
	s_wl.rescale_after_place = 0;
	follow_window_scale_locked();
	// (Not while a size handed to the player has not reached its window yet: that
	// is a resize in progress, not one reverted. And not before the protocol has
	// caught up with the new output's scale: the scale change re-asks then, and a
	// resize right as the window lands on another output is one the player tends to
	// revert.)
	if (s_wl.size_fixups < 2 && !s_wl.sent_w && !s_wl.expect_scale120) {
		int had = s_wl.pending_w;
		rerequest_player_size_locked();
		if (s_wl.pending_w && !had) {
			s_wl.size_fixups++;
			fprintf(stderr, "[DisplayXR-WL] the window is %dx%d logical, not the %dx%d wanted: asking again\n",
			        s_wl.logical_w, s_wl.logical_h, s_wl.pending_w, s_wl.pending_h);
		}
	}
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
	fprintf(stderr, "[DisplayXR-WL] %s: window -> (%d,%d)\n", why, s_wl.intent_x, s_wl.intent_y);
	dxr_wl_ext_move_window(s_wl.intent_x, s_wl.intent_y);
}

static void
ext_availability(int available, unsigned caps, const char *layout)
{
	pthread_mutex_lock(&s_wl_mutex);
	s_wl.ext_available = available;
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

static void
ext_window(const DxrWlExtWindow *w, int after_move, int moved)
{
	pthread_mutex_lock(&s_wl_mutex);
	int was_mapped = win_mapped(&s_wl.win);
	int size_changed = was_mapped && (w->w != s_wl.win.w || w->h != s_wl.win.h);
	s_wl.win = *w;
	int mapped = win_mapped(w);
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
		pthread_mutex_unlock(&s_wl_mutex);
		return;
	}
	if (!mapped) {
		// Not on screen (yet): its position is not where it will show.
		pthread_mutex_unlock(&s_wl_mutex);
		return;
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
		// compositor uninvited: follow it.
		s_wl.intent_centred_on[0] = 0;
		s_wl.intent_valid = 1;
		s_wl.intent_x = w->x;
		s_wl.intent_y = w->y;
	}
	if (!s_wl.placing && !s_wl.move_inflight)
		follow_window_scale_locked();
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

static const DxrWlExtCallbacks s_ext_callbacks = {
    .availability = ext_availability,
    .window = ext_window,
    .drag = ext_drag,
};

static int
dxr_wl_begin_pointer_drag(unsigned button)
{
	pthread_mutex_lock(&s_wl_mutex);
	int ok = s_wl.surface && s_wl.ext_available && (s_wl.ext_caps & DXR_WL_EXT_CAP_POINTER_DRAG);
	if (ok)
		s_wl.drag_state = 1;
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
	pthread_mutex_unlock(&s_wl_mutex);
	if (was) {
		dxr_wl_ext_end_pointer_drag();
		fprintf(stderr, "[DisplayXR-WL] window drag: end\n");
	}
}

//! The top-left that centres the window on `panel` at the size it will have there:
//! the size the app asked for (device px) in the panel's stage px.
static void
panel_centre_locked(const struct dxr_wl_output *panel, int layout, int *out_x, int *out_y)
{
	uint32_t p120 = output_scale120_locked(panel, layout);
	double stage_per_device = layout == DXR_LAYOUT_PHYSICAL ? 1.0 : 120.0 / (p120 ? p120 : 120);
	int sw, sh;
	if (s_wl.wanted_device_w > 0 && s_wl.wanted_device_h > 0) {
		sw = (int)lround(s_wl.wanted_device_w * stage_per_device);
		sh = (int)lround(s_wl.wanted_device_h * stage_per_device);
	} else {
		double scale = layout == DXR_LAYOUT_PHYSICAL ? p120 / 120.0 : 1.0; // surface px -> stage px
		sw = (int)lround(s_wl.logical_w * scale);
		sh = (int)lround(s_wl.logical_h * scale);
	}
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
	char why[96];
	snprintf(why, sizeof(why), "the app's position X (%d,%d)", x, y);
	set_intent_locked((int)lround((double)x / n), (int)lround((double)y / n), why);
	pthread_mutex_unlock(&s_wl_mutex);
	return 1;
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
};

__attribute__((visibility("default"))) const DxrWlApi *
dxr_wl_lib_init(const DxrWlHost *host)
{
	if (!host || host->abi != DXR_WL_LIB_ABI || !host->unity_surface || !host->unity_swapchain_size ||
	    !host->player_surface_generation || !host->lock_player_surface || !host->unlock_player_surface)
		return NULL;
	s_host = host;
	return &s_api;
}
