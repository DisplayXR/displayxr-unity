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
// The window controls a Wayland client cannot do for itself (moving its toplevel,
// learning its position) go through the DisplayXR GNOME Shell extension
// (window-geometry@displayxr.org) over D-Bus: displayxr_linux_wayland_dbus.c.
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
#include <time.h>
#include <unistd.h>

#define DXR_WL_MAX_OUTPUTS 8

// An output: its device-pixel mode (wl_output) and its logical rect and connector
// name (xdg-output), to find the 3D panel and place the window on it.
struct dxr_wl_output {
	uint32_t registry_name; // 0 = free slot
	struct wl_output *output;
	struct zxdg_output_v1 *xdg;
	int mode_w, mode_h;
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
	struct dxr_wl_output *on_output; // the output the weave is on (wl_surface.enter)
	int transparent;

	uint32_t scale120; // preferred scale x 120, 0 = not told yet
	uint32_t expect_scale120; // the scale of the output we just moved the window to, until told
	int logical_w, logical_h;
	int device_w, device_h;
	int geometry_dirty; // device size changed since the provider last read it

	// Window size the app asked for (displayxr_resize_overlay), DEVICE px, and the
	// LOGICAL size still to apply (C# Screen.SetResolution).
	int wanted_device_w, wanted_device_h;
	int pending_w, pending_h;

	// Where the window is (its frame origin, logical px), as far as we know: our own
	// moves, a query after a drag and before a resize, and the app's position reads.
	int pos_valid, pos_x, pos_y;
	double pos_at;
	// Holding the window at a position for a few seconds: the compositor places a
	// window when it maps it (wherever the pointer is), which overrides a move made
	// before the player's first frame, and again when the player re-maps it to apply
	// a new size. So after each of our moves, and after each re-attach, put it back
	// if it lands elsewhere. A scale change seen meanwhile is acted on afterwards.
	int restore_pending, restore_x, restore_y, restore_moves;
	double restore_until, restore_next;
	int rescale_after_restore;
	int size_fixups; // re-requests of a size the player reverted, since the app's request
	int dragging;
} s_wl;

// Guards s_wl: the provider's frame thread polls, C#'s main thread asks for
// resizes, positions and input regions.
static pthread_mutex_t s_wl_mutex = PTHREAD_MUTEX_INITIALIZER;

// The main plugin's side (displayxr_linux_wayland_lib.h), set once by dxr_wl_lib_init.
static const DxrWlHost *s_host;

static double
now_s(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/*
 *
 * Outputs.
 *
 */

static void
out_geometry(void *d, struct wl_output *o, int32_t x, int32_t y, int32_t pw, int32_t ph, int32_t sub,
             const char *make, const char *model, int32_t tr)
{
	(void)d; (void)o; (void)x; (void)y; (void)pw; (void)ph; (void)sub; (void)make; (void)model; (void)tr;
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
static void out_scale(void *d, struct wl_output *o, int32_t f) { (void)d; (void)o; (void)f; }

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
		if (s_wl.on_output == out)
			s_wl.on_output = NULL;
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
			fprintf(stderr, "[DisplayXR-WL] output '%s': %dx%d px, logical %dx%d at (%d,%d)\n", o->name, o->mode_w,
			        o->mode_h, o->lw, o->lh, o->lx, o->ly);
	}
	return s_wl.compositor && s_wl.subcompositor;
}

/*
 *
 * The weave sub-surface.
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

// The window changed scale (e.g. from the laptop onto the 3D panel): keep the size
// the app asked for in DEVICE px, as on X11, by asking for the logical size that
// gives it at the new scale.
static void
rerequest_player_size_locked(void)
{
	if (s_wl.wanted_device_w <= 0 || s_wl.wanted_device_h <= 0)
		return;
	// Just moved to another output: size for its scale now, not for the one we are
	// leaving (the compositor reports the new scale only once the window is there).
	uint32_t s120 = s_wl.expect_scale120 ? s_wl.expect_scale120 : s_wl.scale120;
	double scale = s120 ? s120 / 120.0 : 1.0;
	int lw = (int)lround(s_wl.wanted_device_w / scale);
	int lh = (int)lround(s_wl.wanted_device_h / scale);
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
	// The window reached the output we sent it to; or, with no hold in progress, the
	// user took it elsewhere. (Mid-hold it may pass over another output first.)
	if (scale == s_wl.expect_scale120 || !s_wl.restore_pending)
		s_wl.expect_scale120 = 0;
	if (scale == s_wl.scale120)
		return;
	s_wl.scale120 = scale;
	update_device_size_locked();
	if (s_wl.restore_pending)
		s_wl.rescale_after_restore = 1; // passing over another output while being put back
	else
		rerequest_player_size_locked();
}

static const struct wp_fractional_scale_v1_listener s_frac_listener = {
    .preferred_scale = frac_preferred_scale,
};

static struct dxr_wl_output *
output_for(struct wl_output *o)
{
	for (int i = 0; i < DXR_WL_MAX_OUTPUTS; i++)
		if (s_wl.outputs[i].registry_name && s_wl.outputs[i].output == o)
			return &s_wl.outputs[i];
	return NULL;
}

static void
surface_enter(void *d, struct wl_surface *s, struct wl_output *o)
{
	(void)d;
	(void)s;
	s_wl.on_output = output_for(o);
}

static void
surface_leave(void *d, struct wl_surface *s, struct wl_output *o)
{
	(void)d;
	(void)s;
	if (s_wl.on_output && s_wl.on_output == output_for(o))
		s_wl.on_output = NULL;
}

static const struct wl_surface_listener s_surface_listener = {
    .enter = surface_enter,
    .leave = surface_leave,
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

//! Make our surface a sub-surface of `parent`, at its top-left, above it.
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
	// Requests on the player's surface only while it is locked (H3: the player may
	// destroy it at any time otherwise). It may also have changed since the
	// provider read it: use the current one.
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
	wl_surface_add_listener(s_wl.surface, &s_surface_listener, NULL);
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
	// and commits) so the compositor reports the scale of the output it is on.
	s_wl.probe_buffer = make_probe_buffer();
	if (s_wl.probe_buffer) {
		wl_surface_attach(s_wl.surface, s_wl.probe_buffer, 0, 0);
		wl_surface_damage_buffer(s_wl.surface, 0, 0, 1, 1);
	}
	wl_surface_commit(s_wl.surface);
	for (int i = 0; i < 20 && s_wl.frac && !s_wl.scale120; i++) {
		wl_display_roundtrip_queue(display, s_wl.queue);
		if (!s_wl.scale120) {
			struct timespec ts = {0, 10 * 1000 * 1000};
			nanosleep(&ts, NULL);
		}
	}
	if (!s_wl.scale120)
		fprintf(stderr, "[DisplayXR-WL] no fractional scale reported; assuming 1.0 (the weave may not be 1:1)\n");
	update_device_size_locked();
	s_wl.geometry_dirty = 0; // the create-time size goes into XrWaylandSurfaceGeometryDXR
	wl_display_flush(display);
	*out_surface = s_wl.surface;
	*out_w = s_wl.device_w;
	*out_h = s_wl.device_h;
	pthread_mutex_unlock(&s_wl_mutex);
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
	s_wl.on_output = NULL;
	s_wl.transparent = 0;
	s_wl.restore_pending = 0;
	s_wl.rescale_after_restore = 0;
	s_wl.size_fixups = 0;
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

static int placement_move(int x, int y); // below
static int query_frame_origin(int *out_x, int *out_y);

static void
hold_position_locked(int x, int y, double seconds)
{
	double t = now_s();
	s_wl.restore_pending = 1;
	s_wl.restore_x = x;
	s_wl.restore_y = y;
	s_wl.restore_moves = 0;
	s_wl.restore_until = t + seconds;
	s_wl.restore_next = t + 0.1;
}

static void
finish_restore_locked(void)
{
	s_wl.restore_pending = 0;
	s_wl.rescale_after_restore = 0;
	s_wl.expect_scale120 = 0; // where it ended up, the compositor has told us the scale
	// A re-mapped player window can come back at its previous size: ask again for
	// the one the app wants (a no-op when it has it), but only twice in a row, so a
	// player that keeps refusing a size cannot make us re-map it forever.
	if (s_wl.size_fixups < 2) {
		int had = s_wl.pending_w;
		rerequest_player_size_locked();
		if (s_wl.pending_w && !had) {
			s_wl.size_fixups++;
			fprintf(stderr, "[DisplayXR-WL] the window is %dx%d logical, not the %dx%d wanted: asking again\n",
			        s_wl.logical_w, s_wl.logical_h, s_wl.pending_w, s_wl.pending_h);
		}
	}
}

//! One step of putting a re-mapped window back (no lock held: D-Bus round trips).
static void
restore_step(int rx, int ry)
{
	int x = 0, y = 0;
	if (!query_frame_origin(&x, &y) || (x == rx && y == ry))
		return; // not mapped yet, or where it belongs
	pthread_mutex_lock(&s_wl_mutex);
	int go = s_wl.restore_pending && !s_wl.dragging && s_wl.restore_moves < 3;
	if (go)
		s_wl.restore_moves++;
	pthread_mutex_unlock(&s_wl_mutex);
	if (!go)
		return;
	fprintf(stderr, "[DisplayXR-WL] the compositor placed the window at (%d,%d): back to (%d,%d)\n", x, y, rx, ry);
	placement_move(rx, ry);
}

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
		int attached = 0, recreated = 0;
		if (player) { // else: between surfaces, retry
			recreated = player != s_wl.parent;
			attach_to_parent_locked(player);
			s_wl.parent_generation = generation;
			attached = 1;
		}
		s_host->unlock_player_surface();
		if (attached) {
			wl_display_flush(s_wl.display);
			if (recreated)
				fprintf(stderr, "[DisplayXR-WL] the player recreated its window: weave re-attached\n");
			if (s_wl.restore_pending) // re-mapped mid-hold: hold on for longer
				hold_position_locked(s_wl.restore_x, s_wl.restore_y, 3.0);
			else if (s_wl.pos_valid && !s_wl.dragging)
				hold_position_locked(s_wl.pos_x, s_wl.pos_y, 3.0);
		}
	}
	// The player's window size (its swapchain extent, logical px).
	int lw = 0, lh = 0;
	if (s_host->unity_swapchain_size(&lw, &lh) && (lw != s_wl.logical_w || lh != s_wl.logical_h)) {
		s_wl.logical_w = lw;
		s_wl.logical_h = lh;
		wp_viewport_set_destination(s_wl.viewport, lw, lh);
		update_device_size_locked();
		wl_display_flush(s_wl.display);
	}
	wl_display_dispatch_queue_pending(s_wl.display, s_wl.queue);
	int restore = 0, rx = 0, ry = 0;
	if (s_wl.restore_pending) {
		double t = now_s();
		if (t > s_wl.restore_until || s_wl.dragging) {
			finish_restore_locked();
		} else if (t >= s_wl.restore_next) {
			s_wl.restore_next = t + 0.25;
			restore = 1;
			rx = s_wl.restore_x;
			ry = s_wl.restore_y;
		}
	}
	int dirty = s_wl.geometry_dirty;
	if (dirty) {
		s_wl.geometry_dirty = 0;
		*out_w = s_wl.device_w;
		*out_h = s_wl.device_h;
	}
	pthread_mutex_unlock(&s_wl_mutex);
	if (restore)
		restore_step(rx, ry);
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

static void
dxr_wl_request_player_size(int device_w, int device_h)
{
	pthread_mutex_lock(&s_wl_mutex);
	s_wl.wanted_device_w = device_w;
	s_wl.wanted_device_h = device_h;
	s_wl.size_fixups = 0;
	rerequest_player_size_locked();
	pthread_mutex_unlock(&s_wl_mutex);
}

static int
dxr_wl_take_player_size(int *out_w, int *out_h)
{
	pthread_mutex_lock(&s_wl_mutex);
	int have = s_wl.surface && s_wl.pending_w > 0 && s_wl.pending_h > 0;
	if (have) {
		*out_w = s_wl.pending_w;
		*out_h = s_wl.pending_h;
		s_wl.pending_w = s_wl.pending_h = 0;
	}
	pthread_mutex_unlock(&s_wl_mutex);
	// The player re-maps its window when it applies the size, somewhere else: note
	// where it is now, so dxr_wl_weave_poll() can put it back.
	int x = 0, y = 0;
	if (have && query_frame_origin(&x, &y)) {
		pthread_mutex_lock(&s_wl_mutex);
		s_wl.pos_valid = 1;
		s_wl.pos_x = x;
		s_wl.pos_y = y;
		s_wl.pos_at = now_s();
		pthread_mutex_unlock(&s_wl_mutex);
	}
	return have;
}

/*
 *
 * Window controls through the DisplayXR GNOME Shell extension
 * (displayxr_linux_wayland_dbus.c), and what they tell us about our position.
 *
 */

static void
note_position(int x, int y)
{
	pthread_mutex_lock(&s_wl_mutex);
	s_wl.pos_valid = 1;
	s_wl.pos_x = x;
	s_wl.pos_y = y;
	s_wl.pos_at = now_s();
	pthread_mutex_unlock(&s_wl_mutex);
}

static int
placement_move(int x, int y)
{
	int moved = dxr_wl_ext_move_window(x, y);
	if (moved)
		note_position(x, y);
	return moved;
}

static int
query_frame_origin(int *out_x, int *out_y)
{
	return dxr_wl_ext_frame_origin(out_x, out_y);
}

static int
dxr_wl_begin_pointer_drag(unsigned button)
{
	int started = dxr_wl_ext_begin_pointer_drag(button);
	if (started) {
		pthread_mutex_lock(&s_wl_mutex);
		s_wl.dragging = 1;
		pthread_mutex_unlock(&s_wl_mutex);
	}
	fprintf(stderr, "[DisplayXR-WL] window drag: %s\n", started ? "start" : "refused");
	return started;
}

static void
dxr_wl_end_pointer_drag(void)
{
	dxr_wl_ext_end_pointer_drag();
	int x = 0, y = 0;
	int have = query_frame_origin(&x, &y); // where the user left it
	pthread_mutex_lock(&s_wl_mutex);
	s_wl.dragging = 0;
	pthread_mutex_unlock(&s_wl_mutex);
	if (have)
		note_position(x, y);
	fprintf(stderr, "[DisplayXR-WL] window drag: end at (%d,%d)\n", x, y);
}

/*
 *
 * Placement: onto the 3D panel, and positions in X root coordinates.
 *
 */

//! The scale (x 120) of the output containing logical point (x, y); 0 if none.
static uint32_t
scale120_at_locked(int x, int y)
{
	for (int i = 0; i < DXR_WL_MAX_OUTPUTS; i++) {
		const struct dxr_wl_output *o = &s_wl.outputs[i];
		if (o->registry_name && o->lw > 0 && x >= o->lx && x < o->lx + o->lw && y >= o->ly &&
		    y < o->ly + o->lh)
			return (uint32_t)lround(120.0 * o->mode_w / o->lw);
	}
	return 0;
}

//! Move the window, then hold it there (see restore_pending). No lock held.
static int
place_and_hold(int x, int y)
{
	int moved = placement_move(x, y);
	if (!moved)
		return 0;
	pthread_mutex_lock(&s_wl_mutex);
	if (s_wl.surface) {
		hold_position_locked(x, y, 5.0);
		uint32_t target120 = scale120_at_locked(x, y);
		if (target120 && target120 != s_wl.scale120) {
			s_wl.expect_scale120 = target120;
			rerequest_player_size_locked(); // a size asked for at the old scale
		}
	}
	pthread_mutex_unlock(&s_wl_mutex);
	return 1;
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
	// By connector name first (unambiguous), by mode if the name is unknown.
	struct dxr_wl_output *panel = NULL;
	for (int i = 0; i < DXR_WL_MAX_OUTPUTS && connector && *connector; i++)
		if (s_wl.outputs[i].registry_name && !strcmp(s_wl.outputs[i].name, connector))
			panel = &s_wl.outputs[i];
	for (int i = 0; i < DXR_WL_MAX_OUTPUTS && !panel; i++)
		if (s_wl.outputs[i].registry_name && s_wl.outputs[i].mode_w == panel_w &&
		    s_wl.outputs[i].mode_h == panel_h && s_wl.outputs[i].lw > 0)
			panel = &s_wl.outputs[i];
	if (!panel) {
		pthread_mutex_unlock(&s_wl_mutex);
		fprintf(stderr, "[DisplayXR-WL] 3D panel: no output named '%s' or with a %dx%d mode\n",
		        connector ? connector : "", panel_w, panel_h);
		return 0;
	}
	if (s_wl.on_output == panel) {
		// Already there: leave it, as the X11 and Windows paths do.
		pthread_mutex_unlock(&s_wl_mutex);
		return 1;
	}
	// Centre the window as it will be there: a size the app already asked for in
	// device px becomes a different logical size at the panel's scale.
	uint32_t panel120 = panel->lw > 0 ? (uint32_t)lround(120.0 * panel->mode_w / panel->lw) : 0;
	double panel_scale = panel120 ? panel120 / 120.0 : 1.0;
	int lw = s_wl.logical_w, lh = s_wl.logical_h;
	if (s_wl.wanted_device_w > 0 && s_wl.wanted_device_h > 0) {
		lw = (int)lround(s_wl.wanted_device_w / panel_scale);
		lh = (int)lround(s_wl.wanted_device_h / panel_scale);
	}
	int x = panel->lx + (panel->lw - lw) / 2;
	int y = panel->ly + (panel->lh - lh) / 2;
	if (x < panel->lx)
		x = panel->lx;
	if (y < panel->ly)
		y = panel->ly;
	char name[sizeof(panel->name)];
	memcpy(name, panel->name, sizeof(name));
	int plw = panel->lw, plh = panel->lh, plx = panel->lx, ply = panel->ly;
	pthread_mutex_unlock(&s_wl_mutex);

	int moved = place_and_hold(x, y);
	fprintf(stderr, "[DisplayXR-WL] 3D panel '%s' (%dx%d logical at %d,%d): window -> (%d,%d) %s\n", name, plw, plh,
	        plx, ply, x, y, moved ? "moved" : "NOT moved");
	return moved;
}

// X root px per logical px. Under mutter's xwayland-native-scaling (GNOME 47+,
// Ubuntu's default) X root coordinates are logical x N, N = the largest output
// scale rounded up. That is the space the X11 path reads and restores window
// positions in, so a position an app saved under either backend means the same
// place under the other. (With the feature off, X root is logical and N would be
// 1; not handled.)
static int
xwayland_scale_locked(void)
{
	double max_scale = 1.0;
	for (int i = 0; i < DXR_WL_MAX_OUTPUTS; i++) {
		const struct dxr_wl_output *o = &s_wl.outputs[i];
		if (o->registry_name && o->lw > 0 && o->mode_w > 0) {
			double sc = (double)o->mode_w / o->lw;
			if (sc > max_scale)
				max_scale = sc;
		}
	}
	return (int)ceil(max_scale - 1e-3);
}

static int
dxr_wl_get_player_position_x11(int *out_x, int *out_y)
{
	pthread_mutex_lock(&s_wl_mutex);
	if (!s_wl.surface) {
		pthread_mutex_unlock(&s_wl_mutex);
		return 0;
	}
	// Apps read this every frame: ask the compositor at most once a second, and
	// answer from what we know otherwise (our moves, drags and resizes update it).
	int refresh = !s_wl.pos_valid || now_s() - s_wl.pos_at > 1.0;
	pthread_mutex_unlock(&s_wl_mutex);
	int qx = 0, qy = 0;
	int queried = refresh && query_frame_origin(&qx, &qy);
	pthread_mutex_lock(&s_wl_mutex);
	if (refresh) // also on failure: don't retry every frame
		s_wl.pos_at = now_s();
	if (queried && !s_wl.restore_pending) { // mid-restore it is about to be back
		s_wl.pos_valid = 1;
		s_wl.pos_x = qx;
		s_wl.pos_y = qy;
	}
	int have = s_wl.pos_valid;
	int lx = s_wl.restore_pending ? s_wl.restore_x : s_wl.pos_x;
	int ly = s_wl.restore_pending ? s_wl.restore_y : s_wl.pos_y;
	int n = xwayland_scale_locked();
	pthread_mutex_unlock(&s_wl_mutex);
	if (have) {
		*out_x = lx * n;
		*out_y = ly * n;
	}
	return have;
}

static int
dxr_wl_set_player_position_x11(int x, int y)
{
	pthread_mutex_lock(&s_wl_mutex);
	if (!s_wl.surface) {
		pthread_mutex_unlock(&s_wl_mutex);
		return 0;
	}
	int n = xwayland_scale_locked();
	pthread_mutex_unlock(&s_wl_mutex);
	int lx = (int)lround((double)x / n), ly = (int)lround((double)y / n);
	int moved = place_and_hold(lx, ly); // the app's placement replaces any hold
	fprintf(stderr, "[DisplayXR-WL] window -> X (%d,%d) = logical (%d,%d) %s\n", x, y, lx, ly,
	        moved ? "moved" : "NOT moved");
	return moved;
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
