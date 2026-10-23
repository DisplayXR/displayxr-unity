// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Protocol test for a removed output's wl_output binding (#408)
// (displayxr_linux_wayland_output.h). Built and run by CTest on Linux.
//
// An in-process compositor (libwayland-server: wl_compositor + wl_output) and one
// client connection with two parties on it, as in a native-Wayland player:
//  - "SDL" on the default queue: its own wl_output binding and a window surface;
//  - "ours" on a private queue: our own wl_output binding (the library's slots)
//    and a weave-like surface.
// The output goes away the way mutter removes one: wl_surface.leave for every
// binding of it to every surface, then wl_registry.global_remove. Our queue is
// dispatched first (as the provider's frame thread does), then SDL's.
//  - control: our binding destroyed on global_remove -> SDL's surface gets leave(NULL),
//    which Unity's SDL dereferences (the crash);
//  - the library's way (dxr_wl_output_retire): no leave(NULL); the binding matches
//    no slot; an output added afterwards takes the slot; events on the kept binding
//    change nothing.

#include "displayxr_linux_wayland_output.h"

#include <wayland-client.h>
#include <wayland-server.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

static int s_failed;

#define CHECK(cond)                                                                                                \
	do {                                                                                                       \
		if (!(cond)) {                                                                                     \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                            \
			s_failed++;                                                                                \
		}                                                                                                  \
	} while (0)

#define MAX_RES 16

/*
 * The compositor.
 */

static struct {
	struct wl_display *display;
	struct wl_event_loop *loop;
	struct wl_client *client;
	struct wl_global *output; // the current output global
	struct wl_resource *surfaces[MAX_RES];
	int n_surfaces;
	struct wl_resource *outputs[MAX_RES]; // bindings of the current output global
	int n_outputs;
	struct wl_resource *old_outputs[MAX_RES]; // bindings of the removed one (inert)
	int n_old_outputs;
} srv;

static void
srv_resource_destroy(struct wl_client *c, struct wl_resource *r)
{
	(void)c;
	wl_resource_destroy(r);
}

static const struct wl_surface_interface s_surface_impl = {.destroy = srv_resource_destroy};

static void
srv_create_surface(struct wl_client *c, struct wl_resource *r, uint32_t id)
{
	struct wl_resource *s = wl_resource_create(c, &wl_surface_interface, wl_resource_get_version(r), id);
	wl_resource_set_implementation(s, &s_surface_impl, NULL, NULL);
	srv.surfaces[srv.n_surfaces++] = s;
}

static const struct wl_compositor_interface s_compositor_impl = {.create_surface = srv_create_surface};

static void
srv_bind_compositor(struct wl_client *c, void *data, uint32_t version, uint32_t id)
{
	(void)data;
	struct wl_resource *r = wl_resource_create(c, &wl_compositor_interface, version, id);
	wl_resource_set_implementation(r, &s_compositor_impl, NULL, NULL);
}

static const struct wl_output_interface s_output_impl = {.release = srv_resource_destroy};

static void
srv_bind_output(struct wl_client *c, void *data, uint32_t version, uint32_t id)
{
	(void)data;
	struct wl_resource *r = wl_resource_create(c, &wl_output_interface, version, id);
	wl_resource_set_implementation(r, &s_output_impl, NULL, NULL);
	srv.outputs[srv.n_outputs++] = r;
	wl_output_send_geometry(r, 0, 0, 600, 340, WL_OUTPUT_SUBPIXEL_UNKNOWN, "make", "model",
	                        WL_OUTPUT_TRANSFORM_NORMAL);
	wl_output_send_mode(r, WL_OUTPUT_MODE_CURRENT, 3840, 2160, 60000);
	if (version >= 2) {
		wl_output_send_scale(r, 2);
		wl_output_send_done(r);
	}
}

static void
srv_add_output(void)
{
	srv.n_outputs = 0;
	srv.output = wl_global_create(srv.display, &wl_output_interface, 3, NULL, srv_bind_output);
}

static void
srv_enter_all(void)
{
	for (int s = 0; s < srv.n_surfaces; s++)
		for (int o = 0; o < srv.n_outputs; o++)
			wl_surface_send_enter(srv.surfaces[s], srv.outputs[o]);
}

//! As mutter removes an output: a leave for every binding of it to every
//! surface, then the global goes; its bindings stay (inert) until released.
static void
srv_remove_output(void)
{
	for (int s = 0; s < srv.n_surfaces; s++)
		for (int o = 0; o < srv.n_outputs; o++)
			wl_surface_send_leave(srv.surfaces[s], srv.outputs[o]);
	wl_global_remove(srv.output);
	memcpy(srv.old_outputs, srv.outputs, sizeof(srv.outputs));
	srv.n_old_outputs = srv.n_outputs;
	srv.n_outputs = 0;
}

/*
 * The client: the connection, and a queue that only reads (prepare / read
 * without dispatching anything).
 */

static struct wl_display *s_conn;
static struct wl_event_queue *s_read_q;

//! Requests to the compositor, its events into the client's queues (not dispatched).
static void
pump(void)
{
	for (int i = 0; i < 3; i++) {
		wl_display_flush(s_conn);
		wl_event_loop_dispatch(srv.loop, 0);
		wl_display_flush_clients(srv.display);
		if (wl_display_prepare_read_queue(s_conn, s_read_q) == 0)
			wl_display_read_events(s_conn);
	}
}

/*
 * "SDL": the default queue.
 */

static char s_sdl_tag; // SDL tells its own outputs by a proxy tag; here by user data
static struct {
	struct wl_registry *registry;
	struct wl_compositor *compositor;
	struct wl_output *output;
	struct wl_surface *surface;
	int leaves, leaves_null, leaves_ours;
} sdl;

static void
sdl_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t version)
{
	(void)d;
	(void)version;
	if (!strcmp(iface, wl_compositor_interface.name)) {
		sdl.compositor = wl_registry_bind(r, name, &wl_compositor_interface, 4);
	} else if (!strcmp(iface, wl_output_interface.name)) {
		sdl.output = wl_registry_bind(r, name, &wl_output_interface, 2);
		wl_output_set_user_data(sdl.output, &s_sdl_tag); // no listener: its events are dropped
	}
}

static void
sdl_global_remove(void *d, struct wl_registry *r, uint32_t name)
{
	(void)d;
	(void)r;
	(void)name;
}

static const struct wl_registry_listener s_sdl_registry = {.global = sdl_global, .global_remove = sdl_global_remove};

static void
sdl_enter(void *d, struct wl_surface *s, struct wl_output *o)
{
	(void)d;
	(void)s;
	(void)o;
}

//! Unity's SDL reads wl_output_get_user_data(output) first: NULL is its crash.
static void
sdl_leave(void *d, struct wl_surface *s, struct wl_output *o)
{
	(void)d;
	(void)s;
	sdl.leaves++;
	if (!o) {
		sdl.leaves_null++;
		return;
	}
	if (wl_output_get_user_data(o) != &s_sdl_tag)
		sdl.leaves_ours++; // not its own: SDL ignores it
}

static const struct wl_surface_listener s_sdl_surface = {.enter = sdl_enter, .leave = sdl_leave};

/*
 * "Ours": a private queue, the library's slot table in small.
 */

struct slot {
	uint32_t registry_name; // 0 = free
	struct wl_output *output;
	int wl_scale, mode_w;
};

static struct {
	int keep; // retire (the library) or destroy (the control)
	struct wl_event_queue *q;
	struct wl_display *wrapper;
	struct wl_registry *registry;
	struct wl_compositor *compositor;
	struct wl_surface *surface;
	struct slot slots[4];
	unsigned on; // slots the surface is on
	struct wl_output *kept[MAX_RES];
	int n_kept;
} ours;

static void
out_geometry(void *d, struct wl_output *o, int32_t x, int32_t y, int32_t pw, int32_t ph, int32_t sub,
             const char *make, const char *model, int32_t transform)
{
	(void)d; (void)o; (void)x; (void)y; (void)pw; (void)ph; (void)sub; (void)make; (void)model; (void)transform;
}

static void
out_mode(void *d, struct wl_output *o, uint32_t flags, int32_t w, int32_t h, int32_t refresh)
{
	(void)o; (void)h; (void)refresh;
	struct slot *s = dxr_wl_output_live(d);
	if (s && (flags & WL_OUTPUT_MODE_CURRENT))
		s->mode_w = w;
}

static void out_done(void *d, struct wl_output *o) { (void)d; (void)o; }

static void
out_scale(void *d, struct wl_output *o, int32_t factor)
{
	(void)o;
	struct slot *s = dxr_wl_output_live(d);
	if (s)
		s->wl_scale = factor;
}

static const struct wl_output_listener s_out_listener = {
    .geometry = out_geometry, .mode = out_mode, .done = out_done, .scale = out_scale};

static void
ours_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t version)
{
	(void)d;
	(void)version;
	if (!strcmp(iface, wl_compositor_interface.name)) {
		ours.compositor = wl_registry_bind(r, name, &wl_compositor_interface, 4);
	} else if (!strcmp(iface, wl_output_interface.name)) {
		for (int i = 0; i < 4; i++) {
			struct slot *s = &ours.slots[i];
			if (s->registry_name)
				continue;
			memset(s, 0, sizeof(*s));
			s->registry_name = name;
			s->output = wl_registry_bind(r, name, &wl_output_interface, 2);
			wl_output_add_listener(s->output, &s_out_listener, s);
			break;
		}
	}
}

static void
ours_global_remove(void *d, struct wl_registry *r, uint32_t name)
{
	(void)d;
	(void)r;
	for (int i = 0; i < 4; i++) {
		struct slot *s = &ours.slots[i];
		if (s->registry_name != name)
			continue;
		ours.on &= ~(1u << i);
		if (ours.keep) {
			dxr_wl_output_retire(s->output);
			ours.kept[ours.n_kept++] = s->output;
		} else {
			wl_output_destroy(s->output);
		}
		memset(s, 0, sizeof(*s));
	}
}

static const struct wl_registry_listener s_ours_registry = {.global = ours_global, .global_remove = ours_global_remove};

static int
slot_of(const struct wl_output *o)
{
	for (int i = 0; o && i < 4; i++)
		if (ours.slots[i].registry_name && ours.slots[i].output == o)
			return i;
	return -1;
}

static void
ours_enter(void *d, struct wl_surface *s, struct wl_output *o)
{
	(void)d;
	(void)s;
	int i = slot_of(o);
	if (i >= 0)
		ours.on |= 1u << i;
}

static void
ours_leave(void *d, struct wl_surface *s, struct wl_output *o)
{
	(void)d;
	(void)s;
	int i = slot_of(o);
	if (i >= 0)
		ours.on &= ~(1u << i);
}

static const struct wl_surface_listener s_ours_surface = {.enter = ours_enter, .leave = ours_leave};

static void
dispatch_ours_then_sdl(void)
{
	wl_display_dispatch_queue_pending(s_conn, ours.q);
	wl_display_dispatch_pending(s_conn);
}

//! One run: connect, both parties up, the window on the output, the output goes
//! (and, kept, comes back).
static void
run(int keep)
{
	memset(&srv, 0, sizeof(srv));
	memset(&sdl, 0, sizeof(sdl));
	memset(&ours, 0, sizeof(ours));
	ours.keep = keep;

	int fds[2];
	CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) == 0);
	srv.display = wl_display_create();
	srv.loop = wl_display_get_event_loop(srv.display);
	wl_global_create(srv.display, &wl_compositor_interface, 4, NULL, srv_bind_compositor);
	srv_add_output();
	srv.client = wl_client_create(srv.display, fds[0]);
	s_conn = wl_display_connect_to_fd(fds[1]);
	CHECK(srv.client && s_conn);
	s_read_q = wl_display_create_queue(s_conn);

	sdl.registry = wl_display_get_registry(s_conn);
	wl_registry_add_listener(sdl.registry, &s_sdl_registry, NULL);
	ours.q = wl_display_create_queue(s_conn);
	ours.wrapper = wl_proxy_create_wrapper(s_conn);
	wl_proxy_set_queue((struct wl_proxy *)ours.wrapper, ours.q);
	ours.registry = wl_display_get_registry(ours.wrapper);
	wl_registry_add_listener(ours.registry, &s_ours_registry, NULL);
	pump();
	dispatch_ours_then_sdl(); // the globals: both bind the output
	pump();
	dispatch_ours_then_sdl(); // the outputs' events
	CHECK(sdl.compositor && sdl.output && ours.compositor);
	CHECK(ours.slots[0].output && ours.slots[0].wl_scale == 2 && ours.slots[0].mode_w == 3840);

	sdl.surface = wl_compositor_create_surface(sdl.compositor);
	wl_surface_add_listener(sdl.surface, &s_sdl_surface, NULL);
	ours.surface = wl_compositor_create_surface(ours.compositor);
	wl_surface_add_listener(ours.surface, &s_ours_surface, NULL);
	pump();
	CHECK(srv.n_surfaces == 2 && srv.n_outputs == 2);
	srv_enter_all();
	pump();
	dispatch_ours_then_sdl();
	CHECK(ours.on == 1u);

	srv_remove_output();
	pump();
	dispatch_ours_then_sdl();
	CHECK(ours.on == 0); // our surface's leave (and global_remove) cleared it
	CHECK(!ours.slots[0].registry_name); // the slot is free
	CHECK(sdl.leaves == 2); // one per binding: SDL's and ours
	if (!keep) {
		// The control: the bug this guards against (if it ever stops reproducing,
		// libwayland changed and this test must be revisited).
		CHECK(sdl.leaves_null == 1);
	} else {
		CHECK(sdl.leaves_null == 0);
		CHECK(sdl.leaves_ours == 1);
		CHECK(ours.n_kept == 1 && dxr_wl_output_live(wl_output_get_user_data(ours.kept[0])) == NULL);

		// An output comes back: the next binding takes the freed slot, never the kept one.
		srv_add_output();
		pump();
		dispatch_ours_then_sdl(); // the new global (both bind it)
		pump();
		dispatch_ours_then_sdl(); // its events
		CHECK(ours.slots[0].registry_name && ours.slots[0].output != ours.kept[0]);
		CHECK(ours.slots[0].wl_scale == 2);
		srv_enter_all();
		pump();
		dispatch_ours_then_sdl();
		CHECK(ours.on == 1u);
		// Events on the kept binding (a compositor that still sends some) change no slot.
		for (int o = 0; o < srv.n_old_outputs; o++) {
			wl_output_send_scale(srv.old_outputs[o], 7);
			wl_output_send_done(srv.old_outputs[o]);
		}
		for (int o = 0; o < srv.n_old_outputs; o++) // a late leave of the kept binding, to our surface
			if (wl_resource_get_id(srv.old_outputs[o]) == wl_proxy_get_id((struct wl_proxy *)ours.kept[0]))
				wl_surface_send_leave(srv.surfaces[1], srv.old_outputs[o]);
		pump();
		dispatch_ours_then_sdl();
		CHECK(ours.slots[0].wl_scale == 2);
		CHECK(ours.on == 1u); // matched no slot
		CHECK(slot_of(ours.kept[0]) < 0);
	}

	wl_display_disconnect(s_conn);
	wl_display_destroy(srv.display);
}

int
main(void)
{
	run(0);
	run(1);
	if (s_failed) {
		fprintf(stderr, "%d check(s) failed\n", s_failed);
		return 1;
	}
	printf("wayland_output_remove: all checks passed\n");
	return 0;
}
