// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Native-Wayland Unity player (EXPERIMENT): the weave goes into a sub-surface of
// the player's own window.
//
// The player's wl_display / wl_surface come from the capture layer
// (displayxr_xrprovider/displayxr_provider_wl_capture.cpp). Everything here runs
// on the player's connection but on a PRIVATE event queue, so the player's SDL
// (which dispatches the default queue on its main thread) never sees our events
// and we never dispatch its. Wayland requests are thread-safe; events for our
// objects land on our queue and only we dispatch it.
//
// Why a sub-surface: it moves with its parent and stacks above it, so a weave
// drawn into it covers the player's window without a second toplevel, and the
// compositor's window geometry for this process is the player's window, which is
// exactly where the weave is.
//
// Why viewporter + fractional-scale: the player draws its window at LOGICAL size
// (buffer scale 1, measured), but the weave must be 1:1 with the panel's DEVICE
// pixels or it is resampled. The runtime presents a device-sized buffer into our
// surface; wp_viewport maps it onto the logical window size, and
// wp_fractional_scale tells us the real (fractional) scale to size it with.

#define _GNU_SOURCE // memfd_create
#include <wayland-client.h>
#include "viewporter-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"

#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

struct dxr_wl {
	struct wl_display *display; // the player's connection (not ours to close)
	struct wl_event_queue *queue;
	struct wl_display *display_wrapper; // the display, on our queue
	struct wl_registry *registry;
	struct wl_compositor *compositor;
	struct wl_subcompositor *subcompositor;
	struct wl_shm *shm;
	struct wp_viewporter *viewporter;
	struct wp_fractional_scale_manager_v1 *frac_manager;

	struct wl_surface *surface;
	struct wl_subsurface *subsurface;
	struct wp_viewport *viewport;
	struct wp_fractional_scale_v1 *frac;
	struct wl_buffer *probe_buffer;

	uint32_t scale120;   // preferred scale x 120 (wp_fractional_scale), 0 = not told yet
	int logical_w, logical_h;
	int device_w, device_h;
	int geometry_dirty;  // device size changed since the provider last read it
};

static struct dxr_wl s_wl;
// Guards the size fields: the player's render thread reports resizes, the
// provider's frame thread polls.
static pthread_mutex_t s_wl_mutex = PTHREAD_MUTEX_INITIALIZER;

static void
registry_global(void *data, struct wl_registry *reg, uint32_t name, const char *iface, uint32_t version)
{
	struct dxr_wl *w = data;
	if (strcmp(iface, wl_compositor_interface.name) == 0)
		w->compositor = wl_registry_bind(reg, name, &wl_compositor_interface, version < 4 ? version : 4);
	else if (strcmp(iface, wl_subcompositor_interface.name) == 0)
		w->subcompositor = wl_registry_bind(reg, name, &wl_subcompositor_interface, 1);
	else if (strcmp(iface, wl_shm_interface.name) == 0)
		w->shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
	else if (strcmp(iface, wp_viewporter_interface.name) == 0)
		w->viewporter = wl_registry_bind(reg, name, &wp_viewporter_interface, 1);
	else if (strcmp(iface, wp_fractional_scale_manager_v1_interface.name) == 0)
		w->frac_manager = wl_registry_bind(reg, name, &wp_fractional_scale_manager_v1_interface, 1);
}

static void
registry_global_remove(void *data, struct wl_registry *reg, uint32_t name)
{
	(void)data;
	(void)reg;
	(void)name;
}

static const struct wl_registry_listener s_registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static void
update_device_size(void)
{
	double scale = s_wl.scale120 ? s_wl.scale120 / 120.0 : 1.0;
	int dw = (int)lround(s_wl.logical_w * scale);
	int dh = (int)lround(s_wl.logical_h * scale);
	if (dw != s_wl.device_w || dh != s_wl.device_h) {
		s_wl.device_w = dw;
		s_wl.device_h = dh;
		s_wl.geometry_dirty = 1;
		fprintf(stderr, "[DisplayXR-WL] weave surface: logical %dx%d x scale %.4f -> buffer %dx%d\n",
		        s_wl.logical_w, s_wl.logical_h, scale, dw, dh);
		fflush(stderr);
	}
}

static void
frac_preferred_scale(void *data, struct wp_fractional_scale_v1 *frac, uint32_t scale)
{
	(void)data;
	(void)frac;
	if (scale == s_wl.scale120)
		return;
	s_wl.scale120 = scale;
	update_device_size();
}

static const struct wp_fractional_scale_v1_listener s_frac_listener = {
    .preferred_scale = frac_preferred_scale,
};

//! Bind the globals we need on the player's connection, on our own queue.
static int
dxr_wl_connect(struct wl_display *display)
{
	if (s_wl.registry)
		return s_wl.compositor && s_wl.subcompositor;
	s_wl.display = display;
	s_wl.queue = wl_display_create_queue(display);
	s_wl.display_wrapper = wl_proxy_create_wrapper(display);
	wl_proxy_set_queue((struct wl_proxy *)s_wl.display_wrapper, s_wl.queue);
	s_wl.registry = wl_display_get_registry(s_wl.display_wrapper);
	wl_registry_add_listener(s_wl.registry, &s_registry_listener, &s_wl);
	if (wl_display_roundtrip_queue(display, s_wl.queue) < 0) {
		fprintf(stderr, "[DisplayXR-WL] roundtrip on the player's connection failed\n");
		return 0;
	}
	fprintf(stderr, "[DisplayXR-WL] globals on the player's connection: compositor=%p subcompositor=%p shm=%p "
	        "viewporter=%p fractional_scale=%p\n", (void *)s_wl.compositor, (void *)s_wl.subcompositor,
	        (void *)s_wl.shm, (void *)s_wl.viewporter, (void *)s_wl.frac_manager);
	return s_wl.compositor && s_wl.subcompositor;
}

//! A width x height ARGB8888 shm buffer filled with one premultiplied colour,
//! plus a solid border of `border_argb` (0 = no border).
static struct wl_buffer *
make_shm_buffer(int width, int height, uint32_t fill_argb, uint32_t border_argb)
{
	if (!s_wl.shm)
		return NULL;
	int stride = width * 4;
	size_t size = (size_t)stride * (size_t)height;
	int fd = memfd_create("dxr-wl", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, (off_t)size) < 0) {
		if (fd >= 0)
			close(fd);
		return NULL;
	}
	uint32_t *px = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (px == MAP_FAILED) {
		close(fd);
		return NULL;
	}
	const int border = border_argb ? 8 : 0;
	for (int y = 0; y < height; y++)
		for (int x = 0; x < width; x++) {
			int edge = x < border || y < border || x >= width - border || y >= height - border;
			px[y * width + x] = edge ? border_argb : fill_argb;
		}
	munmap(px, size);
	struct wl_shm_pool *pool = wl_shm_create_pool(s_wl.shm, fd, (int32_t)size);
	struct wl_buffer *buf = wl_shm_pool_create_buffer(pool, 0, width, height, stride, WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	return buf;
}

//! A sub-surface of `parent` at its top-left, above it, taking no input.
static void
create_subsurface(struct wl_surface *parent)
{
	s_wl.surface = wl_compositor_create_surface(s_wl.compositor);
	s_wl.subsurface = wl_subcompositor_get_subsurface(s_wl.subcompositor, s_wl.surface, parent);
	wl_subsurface_set_position(s_wl.subsurface, 0, 0);
	wl_subsurface_place_above(s_wl.subsurface, parent);
	// Desync: our commits (the runtime's presents) show at once, without waiting
	// for the player's own commits.
	wl_subsurface_set_desync(s_wl.subsurface);
	// Pointer input goes through to the player's window underneath.
	struct wl_region *empty = wl_compositor_create_region(s_wl.compositor);
	wl_surface_set_input_region(s_wl.surface, empty);
	wl_region_destroy(empty);
}

/*
 *
 * Exports.
 *
 */

//! Step 3 of the experiment: a test pattern in a sub-surface of the player's
//! window, buffer scale 1. (DXR_WL_TEST_SUBSURFACE=1)
int
dxr_wl_test_subsurface(struct wl_display *display, struct wl_surface *parent, int width, int height)
{
	if (s_wl.subsurface)
		return 1;
	if (!display || !parent || width <= 0 || height <= 0 || !dxr_wl_connect(display))
		return 0;
	create_subsurface(parent);
	struct wl_buffer *buf = make_shm_buffer(width, height, 0x40400040u, 0xFFFF00FFu);
	if (buf) {
		wl_surface_attach(s_wl.surface, buf, 0, 0);
		wl_surface_damage(s_wl.surface, 0, 0, width, height);
	}
	wl_surface_commit(s_wl.surface);
	wl_display_flush(display);
	fprintf(stderr, "[DisplayXR-WL] test sub-surface on the player's window: %dx%d buffer (scale 1) at (0,0)%s\n",
	        width, height, buf ? "" : " (NO test buffer: wl_shm missing)");
	fflush(stderr);
	return 1;
}

//! The weave target: a sub-surface of the player's window, sized to its LOGICAL
//! size (the player's swapchain extent) through a viewport, with the DEVICE size
//! the runtime should present at returned in out_w/out_h. Must run before
//! xrCreateSession: it maps the surface once with a transparent 1x1 buffer so the
//! compositor places it on an output and tells us the scale; after the session
//! exists only the runtime's WSI attaches and commits.
int
dxr_wl_weave_surface_create(struct wl_display *display, struct wl_surface *parent, int logical_w, int logical_h,
                            struct wl_surface **out_surface, int *out_w, int *out_h)
{
	if (!display || !parent || logical_w <= 0 || logical_h <= 0 || !dxr_wl_connect(display))
		return 0;
	if (!s_wl.viewporter) {
		fprintf(stderr, "[DisplayXR-WL] the compositor has no wp_viewporter: cannot weave 1:1\n");
		return 0;
	}
	if (!s_wl.surface) {
		create_subsurface(parent);
		s_wl.viewport = wp_viewporter_get_viewport(s_wl.viewporter, s_wl.surface);
		if (s_wl.frac_manager) {
			s_wl.frac = wp_fractional_scale_manager_v1_get_fractional_scale(s_wl.frac_manager, s_wl.surface);
			wp_fractional_scale_v1_add_listener(s_wl.frac, &s_frac_listener, NULL);
		}
	}
	s_wl.logical_w = logical_w;
	s_wl.logical_h = logical_h;
	wp_viewport_set_destination(s_wl.viewport, logical_w, logical_h);

	// Map it once so the compositor reports the scale of the output it is on.
	s_wl.probe_buffer = make_shm_buffer(1, 1, 0x00000000u, 0);
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
	update_device_size();
	s_wl.geometry_dirty = 0; // the create-time size goes into XrWaylandSurfaceGeometryDXR
	wl_display_flush(display);

	*out_surface = s_wl.surface;
	*out_w = s_wl.device_w;
	*out_h = s_wl.device_h;
	return 1;
}

//! The player's window was resized (its swapchain extent, logical px). The
//! viewport takes effect with the runtime's next present.
void
dxr_wl_weave_on_player_resize(int logical_w, int logical_h)
{
	if (!s_wl.viewport || logical_w <= 0 || logical_h <= 0)
		return;
	pthread_mutex_lock(&s_wl_mutex);
	if (logical_w != s_wl.logical_w || logical_h != s_wl.logical_h) {
		s_wl.logical_w = logical_w;
		s_wl.logical_h = logical_h;
		wp_viewport_set_destination(s_wl.viewport, logical_w, logical_h);
		update_device_size();
		wl_display_flush(s_wl.display);
	}
	pthread_mutex_unlock(&s_wl_mutex);
}

//! Per frame: dispatch our queue (scale changes). Returns 1, with the new device
//! size, when the runtime must be told (xrSetWaylandSurfaceGeometryDXR).
int
dxr_wl_weave_poll(int *out_w, int *out_h)
{
	if (!s_wl.surface)
		return 0;
	pthread_mutex_lock(&s_wl_mutex);
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
