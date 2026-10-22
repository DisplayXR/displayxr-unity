// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Side-step for a native-Wayland player on GNOME: when the window the user is
// working in covers the app's content, the app's OWN window steps aside, and it
// goes back once its place is free. Exports: displayxr_linux_side_step_set,
// _set_body_rect, _status (displayxr_exports.h). The decision and the parsing are
// pure (displayxr_linux_side_step.h); this file is the worker around them.
//
// ONLY WHILE GRANTED. Every poll first asks the runtime whether the app's desktop
// control is Granted (dxr_prov_desktop_get_state: the very query behind
// DisplayXRDesktop.State, which DisplayXRNative.LinuxWindows / LinuxWindowAt /
// LinuxPointer answer behind; the #389 consent policy, runtime ADR-056). Unless it
// is Granted nothing below is read and the window stays where it is. Suspended
// (the screen is locked; the user's kill switch reads the same): Paused /
// ScreenLocked, the app's own home and whether it is aside are kept, the id of the
// window it stepped aside for is dropped; it carries on once granted again. Any
// other no (never asked, denied, turned off by the user, a resumed kill switch, a
// runtime without XR_DXR_desktop_control): Paused / NotGranted, and what the
// decision remembered is forgotten. A glide asks again before each step. The pure
// part (dxr_ss_poll, dxr_ss_glide) holds that order; the unit test counts the reads.
//
// WHAT IS READ (only while granted), AND WHERE IT GOES
//  - The DisplayXR GNOME extension's window list (GetAllWindows, else GetWindows):
//    frames, focus, minimized / maximized / fullscreen, the move grab flag, the
//    window type, and the app id only to recognise the desktop-icons window. Read
//    over the main plug-in's own session-bus connection
//    (displayxr_linux_desktop_info.c), every DXR_SS_POLL_MS, by this worker.
//    No titles (the extension withholds them from this connection, and this
//    code never asks for them).
//  - org.gnome.ScreenSaver.GetActive: whether the screen is locked.
//  - The app's content inside its window: the click-through silhouette the
//    transparent overlay already pushes (displayxr_set_overlay_hit_mask), or a
//    rect the app sets, or the whole window.
// None of the other windows' facts leave this file: they are not stored past one
// tick (except the id of the window the app stepped aside for, to tell when its
// place is free of it, and only while the grant holds), not logged, and not
// exported. The app gets its own window's state only (DisplayXRSideStepStatus;
// docs~/architecture/side-step.md).
//
// WHAT IS MOVED: only the app's own window, through the Wayland library's own
// placement (dxr_wl_set_player_position_stage: the GNOME extension's MoveWindow,
// which moves the caller's own window only). Never another window.
//
// THREADS. One worker, created at the first enable and kept (it sleeps while
// disabled). The exports take a mutex and copy; they never wait on D-Bus.

#define _GNU_SOURCE
#include "displayxr_linux_side_step.h"
#include "displayxr_linux_wayland.h"
#include "displayxr_exports.h"
#include "displayxr_extensions.h"                             // XR_DESKTOP_CONTROL_STATE_GRANTED_DXR
#include "displayxr_xrprovider/displayxr_provider_desktop.h" // dxr_prov_desktop_get_state: the gate

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

extern void dxr_prov_file_log(const char *s);
//! displayxr_linux_desktop_info.c: the extension's window list (malloc'd JSON) /
//! whether the screen is locked (1, 0, -1 cannot tell).
char *dxr_linux_desktop_info_window_list_json(void);
int dxr_linux_desktop_info_screen_locked(void);
//! displayxr_linux.c: the plug-in's own right-drag of the window is running.
int dxr_linux_own_pointer_drag(void);

#define DXR_SS_POLL_MS 250          // granted: a poll (the gate, then the reads) this often
#define DXR_SS_CLOSED_POLL_MS 1000  // not granted: only the gate is asked, this often
#define DXR_SS_SIL_EVERY_NS (100 * 1000000LL) // silhouette grid refresh, at most

static pthread_mutex_t s_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_cond;
static int s_cond_ready;
static int s_thread_started;
static int s_enabled;
static unsigned s_settings_gen;
static DxrSsSettings s_settings;
static DisplayXRSideStepStatus s_status;

// The app's content. Its own lock: the silhouette arrives on the render path.
static pthread_mutex_t s_body_mutex = PTHREAD_MUTEX_INITIALIZER;
static int s_sil_known;          // a silhouette was pushed (transparent overlay, click-through)
static DxrSsBody s_sil;
static long long s_sil_at;       // when it was last taken (throttle)
static int s_app_rect_valid;     // the app's own rect, window device px
static int s_app_x, s_app_y, s_app_w, s_app_h;
static volatile int s_want_body; // enabled: take the silhouette

static long long
now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void
ss_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void
ss_log(const char *fmt, ...)
{
	char m[400];
	int n = snprintf(m, sizeof(m), "[DisplayXR-LNX] side-step: ");
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(m + n, sizeof(m) - (size_t)n, fmt, ap);
	va_end(ap);
	fputs(m, stderr);
	dxr_prov_file_log(m);
}

static const char *
reason_text(int r)
{
	switch (r) {
	case DXR_SS_REASON_NOT_NATIVE_WAYLAND: return "not a native-Wayland player (X11 mode): side-step does nothing";
	case DXR_SS_REASON_NO_SESSION: return "waiting for the native-Wayland XR session";
	case DXR_SS_REASON_NO_EXTENSION:
		return "no window list from the DisplayXR GNOME extension (not installed, disabled, not GNOME): waiting";
	case DXR_SS_REASON_NO_WINDOW: return "the app's window is not shown: waiting";
	case DXR_SS_REASON_SETTLING: return "waiting for the window to stay put";
	case DXR_SS_REASON_LOCKED:
		return "the screen is locked (desktop control suspended): paused, nothing is read, home is kept";
	case DXR_SS_REASON_USER_MOVING: return "the user is moving the window: paused (where they leave it becomes home)";
	case DXR_SS_REASON_MINIMIZED: return "the window is minimized: paused";
	case DXR_SS_REASON_MAXIMIZED: return "the window is maximized: paused";
	case DXR_SS_REASON_FULLSCREEN: return "the window is fullscreen: paused";
	case DXR_SS_REASON_TOO_MANY_MOVES:
		return "covered, but it already stepped aside 6 times in the last minute: staying for now";
	case DXR_SS_REASON_NO_FREE_SPOT: return "covered, but no place on this monitor is free: staying";
	case DXR_SS_REASON_NOT_GRANTED:
		return "desktop control is not granted (DisplayXRDesktop.State): paused, nothing is read, the window stays "
		       "where it is";
	default: return "watching";
	}
}

/*
 *
 * The app's content.
 *
 */

//! displayxr_linux.c, from displayxr_set_overlay_hit_mask: the silhouette's rects
//! (XRectangle layout: int16 x, y; uint16 width, height), window device px, in a
//! window dst_w x dst_h. n < 0: no silhouette any more (the whole window counts).
//! Cheap unless side-step is on; refreshed at most every 100 ms.
void dxr_linux_side_step_note_silhouette(const void *rects, int n, int dst_w, int dst_h);

void
dxr_linux_side_step_note_silhouette(const void *rects, int n, int dst_w, int dst_h)
{
	if (!__atomic_load_n(&s_want_body, __ATOMIC_RELAXED))
		return;
	long long t = now_ns();
	if (n >= 0 && t - __atomic_load_n(&s_sil_at, __ATOMIC_RELAXED) < DXR_SS_SIL_EVERY_NS)
		return;
	__atomic_store_n(&s_sil_at, t, __ATOMIC_RELAXED);
	if (n < 0 || dst_w <= 0 || dst_h <= 0) {
		pthread_mutex_lock(&s_body_mutex);
		s_sil_known = 0;
		pthread_mutex_unlock(&s_body_mutex);
		return;
	}
	typedef struct {
		int16_t x, y;
		uint16_t width, height;
	} XRect;
	float acc[DXR_SS_GRID * DXR_SS_GRID];
	memset(acc, 0, sizeof(acc));
	const XRect *r = (const XRect *)rects;
	for (int i = 0; i < n; i++)
		dxr_ss_body_add_rect(acc, r[i].x, r[i].y, (double)r[i].x + r[i].width, (double)r[i].y + r[i].height, dst_w,
		                     dst_h);
	DxrSsBody b;
	dxr_ss_body_from_acc(&b, acc);
	pthread_mutex_lock(&s_body_mutex);
	s_sil = b;
	s_sil_known = 1;
	pthread_mutex_unlock(&s_body_mutex);
}

//! The body for this tick: the app's rect, else the silhouette, else none (the
//! whole window). 1 when *out is set.
static int
current_body(DxrSsBody *out, const char **what)
{
	int dw = 0, dh = 0;
	pthread_mutex_lock(&s_body_mutex);
	int app = s_app_rect_valid;
	int ax = s_app_x, ay = s_app_y, aw = s_app_w, ah = s_app_h;
	int sil = s_sil_known;
	if (!app && sil)
		*out = s_sil;
	pthread_mutex_unlock(&s_body_mutex);
	if (app) {
		displayxr_get_overlay_size(&dw, &dh); // the window, device px (the rect's space)
		if (dw > 0 && dh > 0) {
			float acc[DXR_SS_GRID * DXR_SS_GRID];
			memset(acc, 0, sizeof(acc));
			dxr_ss_body_add_rect(acc, ax, ay, (double)ax + aw, (double)ay + ah, dw, dh);
			dxr_ss_body_from_acc(out, acc);
			*what = "the app's rect";
			return 1;
		}
	}
	if (sil) {
		*what = "the silhouette";
		return 1;
	}
	*what = "the whole window";
	return 0;
}

/*
 *
 * Worker.
 *
 */

/*
 *
 * What a poll reads and does (DxrSsOps): the real calls.
 *
 */

//! The #389 gate: the app's desktop control is Granted right now. The same query
//! DisplayXRDesktop.State makes (a synchronous runtime call, about 1.4 ms, evaluated
//! afresh each time: the user's deny, the kill switch and a screen lock count at
//! once), so side-step and DisplayXRNative.LinuxWindows open and close together. Any
//! failure (no session, a runtime without XR_DXR_desktop_control) is "no". Any thread
//! (the provider serialises it with its other desktop calls and session teardown).
static int
op_granted(void *ctx)
{
	(void)ctx;
	int32_t state = XR_DESKTOP_CONTROL_STATE_UNSUPPORTED_DXR;
	uint64_t caps = 0;
	if (dxr_prov_desktop_get_state(&state, &caps) < 0)
		return DXR_SS_DC_NO;
	return state == XR_DESKTOP_CONTROL_STATE_GRANTED_DXR     ? DXR_SS_DC_GRANTED
	       : state == XR_DESKTOP_CONTROL_STATE_SUSPENDED_DXR ? DXR_SS_DC_SUSPENDED
	                                                         : DXR_SS_DC_NO;
}

static char *
op_window_list(void *ctx)
{
	(void)ctx;
	return dxr_linux_desktop_info_window_list_json();
}

static int
op_screen_locked(void *ctx)
{
	(void)ctx;
	return dxr_linux_desktop_info_screen_locked();
}

static int
op_own_drag(void *ctx)
{
	(void)ctx;
	return dxr_linux_own_pointer_drag();
}

static int
op_still_on(void *ctx)
{
	(void)ctx;
	return __atomic_load_n(&s_enabled, __ATOMIC_RELAXED);
}

static int
op_move(void *ctx, int x, int y, int last)
{
	(void)ctx;
	return dxr_wl_set_player_position_stage(x, y, last);
}

static void
op_wait_s(void *ctx, double seconds)
{
	(void)ctx;
	long long ns = (long long)(seconds * 1e9);
	struct timespec ts = {(time_t)(ns / 1000000000LL), (long)(ns % 1000000000LL)};
	while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
	}
}

static const DxrSsOps s_ops = {
    NULL, op_granted, op_window_list, op_screen_locked, op_own_drag, op_still_on, op_move, op_wait_s,
};

//! Publish a tick's status; `ev` (DXR_SS_EV_LANDED_*) bumps its counter in the same
//! copy, so the app never sees a new count with the previous tick's window rect.
static void
publish(const DisplayXRSideStepStatus *st, int ev)
{
	pthread_mutex_lock(&s_mutex);
	uint32_t ac = s_status.aside_count, hc = s_status.home_count;
	s_status = *st;
	s_status.aside_count = ac + (ev == DXR_SS_EV_LANDED_ASIDE);
	s_status.home_count = hc + (ev == DXR_SS_EV_LANDED_HOME);
	pthread_mutex_unlock(&s_mutex);
}

typedef struct {
	DxrSsState st;
	DxrSsGate gate;              // the desktop-control grant, as the last poll found it
	int last_state, last_reason; // logged
	int started;                 // the decision runs (a native-Wayland session's window)
	int not_granted;             // the last tick ended at the gate (nothing read)
	int own_valid;               // the app's own window, as last seen (kept while locked)
	DxrSsRect own;
	const char *body_what;       // logged when it changes
} Worker;

//! A state / reason pair worth one log line when it changes.
static void
note_state(Worker *w, int state, int reason)
{
	if (state == w->last_state && reason == w->last_reason)
		return;
	int was_reason = w->last_reason;
	w->last_state = state;
	w->last_reason = reason;
	// Watching again after a pause: say so; plain HOME / MOVING / ASIDE flips are
	// covered by the move lines.
	if (reason != DXR_SS_REASON_NONE)
		ss_log("%s\n", reason_text(reason));
	else if (was_reason != DXR_SS_REASON_NONE && was_reason != DXR_SS_REASON_SETTLING &&
	         was_reason != DXR_SS_REASON_NO_FREE_SPOT && was_reason != -1)
		ss_log("watching again\n");
}

//! The app's own window's facts the decision keeps: its last seen frame, home (stage
//! and X root px), and whether it is away from home.
static void
fill_own(const Worker *w, DisplayXRSideStepStatus *out)
{
	const DxrSsState *st = &w->st;
	out->window_valid = w->own_valid;
	out->win_x = w->own.x;
	out->win_y = w->own.y;
	out->win_w = w->own.w;
	out->win_h = w->own.h;
	out->home_valid = st->have_home;
	out->home_x = st->home_x;
	out->home_y = st->home_y;
	float k = st->have_home ? displayxr_linux_xroot_scale() : 0.0f;
	if (k > 0.0f) {
		out->home_xroot_valid = 1;
		out->home_xroot_x = (int32_t)lroundf(st->home_x * k);
		out->home_xroot_y = (int32_t)lroundf(st->home_y * k);
	}
	out->aside = st->phase == DXR_SS_PHASE_ASIDE || st->phase == DXR_SS_PHASE_MOVING;
}

//! Not granted: holding still, and nothing about the window is reported but that
//! (no rect, no home, Covered 0, no events). The event counters are kept.
static void
publish_not_granted(Worker *w)
{
	DisplayXRSideStepStatus out;
	memset(&out, 0, sizeof(out));
	w->own_valid = 0;
	out.state = DXR_SS_STATE_PAUSED;
	out.reason = DXR_SS_REASON_NOT_GRANTED;
	note_state(w, out.state, out.reason);
	publish(&out, DXR_SS_EV_NONE);
}

//! Suspended (the screen is locked): holding still. The app's own window's facts
//! stay as they were (home, aside, its last seen frame); Covered 0, no events.
static void
publish_locked(Worker *w)
{
	DisplayXRSideStepStatus out;
	memset(&out, 0, sizeof(out));
	out.state = DXR_SS_STATE_PAUSED;
	out.reason = DXR_SS_REASON_LOCKED;
	fill_own(w, &out);
	note_state(w, out.state, out.reason);
	publish(&out, DXR_SS_EV_NONE);
}

static void
tick(Worker *w, const DxrSsSettings *s)
{
	DisplayXRSideStepStatus out;
	memset(&out, 0, sizeof(out));
	w->not_granted = 0;
	if (!dxr_wl_forced_wayland()) {
		out.state = DXR_SS_STATE_UNSUPPORTED;
		out.reason = DXR_SS_REASON_NOT_NATIVE_WAYLAND;
		note_state(w, out.state, out.reason);
		publish(&out, DXR_SS_EV_NONE);
		return;
	}
	if (!dxr_wl_weave_active()) {
		// No session (yet, or any more): start afresh with the next one, and forget
		// the last session's silhouette (the next one pushes its own). The grant is
		// asked for again with the next session.
		if (w->started) {
			dxr_ss_reset(&w->st);
			pthread_mutex_lock(&s_body_mutex);
			s_sil_known = 0;
			pthread_mutex_unlock(&s_body_mutex);
		}
		w->started = 0;
		w->own_valid = 0;
		memset(&w->gate, 0, sizeof(w->gate));
		out.state = DXR_SS_STATE_WAITING;
		out.reason = DXR_SS_REASON_NO_SESSION;
		note_state(w, out.state, out.reason);
		publish(&out, DXR_SS_EV_NONE);
		return;
	}

	// The gate first: dxr_ss_poll reads nothing (no window list, no lock state) unless
	// the app's desktop control is Granted right now.
	DxrSsBody body;
	const char *what = NULL;
	int have_body = current_body(&body, &what);
	DxrSsPoll p;
	dxr_ss_poll(&w->st, &w->gate, s, &s_ops, (double)now_ns() / 1e9, (int)getpid(), have_body ? &body : NULL, &p);
	if (p.gate == DXR_SS_GATE_CLOSED)
		ss_log("desktop control ended: stopped where it is; home forgotten until it is granted again\n");
	else if (p.gate == DXR_SS_GATE_OPENED)
		ss_log("desktop control granted: starting afresh (where the window stays put becomes home)\n");
	else if (p.gate == DXR_SS_GATE_SUSPENDED)
		ss_log("desktop control suspended (the screen is locked): holding still, home kept\n");
	else if (p.gate == DXR_SS_GATE_RESUMED)
		ss_log("desktop control granted again: carrying on from home%s\n",
		       w->st.phase == DXR_SS_PHASE_ASIDE ? " (aside: back once home is free)" : "");
	if (!p.granted) {
		w->not_granted = 1;
		if (w->gate.suspended)
			publish_locked(w);
		else
			publish_not_granted(w);
		return;
	}
	if (!p.read) {
		out.state = DXR_SS_STATE_WAITING;
		out.reason = DXR_SS_REASON_NO_EXTENSION;
		note_state(w, out.state, out.reason);
		publish(&out, DXR_SS_EV_NONE);
		return;
	}
	w->started = 1;
	w->own_valid = p.in.own_valid;
	w->own = p.in.own;
	if (what != w->body_what) {
		ss_log("the app's content is %s\n", what);
		w->body_what = what;
	}

	const DxrSsInput *in = &p.in;
	const DxrSsDecision *d = &p.d;
	DxrSsState *st = &w->st;
	switch (d->event) {
	case DXR_SS_EV_NEW_HOME: ss_log("home is (%d,%d)\n", st->home_x, st->home_y); break;
	case DXR_SS_EV_LANDED_ASIDE:
		ss_log("stepped aside: the window is at (%d,%d)%s\n", in->own.x, in->own.y,
		       st->phase == DXR_SS_PHASE_HOME ? " (not going back: this is home now)" : "");
		break;
	case DXR_SS_EV_LANDED_HOME: ss_log("back home at (%d,%d)\n", in->own.x, in->own.y); break;
	case DXR_SS_EV_ABORTED: ss_log("the user took the window during the move\n"); break;
	default: break;
	}

	int state = dxr_ss_report_state(st);
	note_state(w, state, st->reason);
	out.state = state;
	out.reason = st->reason;
	out.covered = st->covered;
	fill_own(w, &out);
	if (d->move)
		out.state = DXR_SS_STATE_MOVING;
	publish(&out, d->event);

	if (!d->move)
		return;
	if (d->home)
		ss_log("home (%d,%d) is free again: going back\n", d->x, d->y);
	else
		ss_log("%.0f %% of the app's content covered for %.1f s: stepping aside to (%d,%d) (covered there %.0f %%)\n",
		       d->covered * 100.0, (double)s->dwell_s, d->x, d->y, d->spot_covered * 100.0);
	int r = dxr_ss_glide(&w->gate, st, s, &s_ops, in->own.x, in->own.y, d->x, d->y);
	if (r == DXR_SS_GLIDE_NOT_GRANTED) {
		// The grant ended during the glide: it stopped where it is, and the gate is
		// closed (everything forgotten). Say so now, not at the next poll.
		ss_log("desktop control ended during a move: stopped where it is; home forgotten until it is granted "
		       "again\n");
		w->not_granted = 1;
		publish_not_granted(w);
	} else if (r == DXR_SS_GLIDE_SUSPENDED) {
		// The screen locked during the glide: it stopped where its last step put the
		// window (own data: kept as its frame), home kept. Say so now.
		ss_log("desktop control suspended during a move (the screen is locked): stopped where it is, home kept\n");
		w->own.x = st->last_x;
		w->own.y = st->last_y;
		w->not_granted = 1;
		publish_locked(w);
	} else if (r == DXR_SS_GLIDE_CANNOT_MOVE) {
		static int s_warned;
		if (!s_warned)
			ss_log("the window cannot be moved right now (the Wayland library has no extension or window)\n");
		s_warned = 1;
	}
}

static void *
worker_main(void *arg)
{
	(void)arg;
	Worker w;
	memset(&w, 0, sizeof(w));
	dxr_ss_reset(&w.st);
	w.last_state = w.last_reason = -1;
	unsigned seen_gen = 0;
	int was_on = 0;
	for (;;) {
		pthread_mutex_lock(&s_mutex);
		while (!s_enabled) {
			if (was_on) {
				was_on = 0;
				dxr_ss_reset(&w.st);
				w.started = 0;
				w.own_valid = 0;
				memset(&w.gate, 0, sizeof(w.gate)); // on again: the grant is asked for afresh
				w.last_state = w.last_reason = -1;
				uint32_t ac = s_status.aside_count, hc = s_status.home_count;
				memset(&s_status, 0, sizeof(s_status));
				s_status.state = DXR_SS_STATE_OFF;
				s_status.aside_count = ac;
				s_status.home_count = hc;
				pthread_mutex_unlock(&s_mutex);
				ss_log("off (the window stays where it is)\n");
				pthread_mutex_lock(&s_mutex);
				continue;
			}
			pthread_cond_wait(&s_cond, &s_mutex);
		}
		DxrSsSettings s = s_settings;
		unsigned gen = s_settings_gen;
		pthread_mutex_unlock(&s_mutex);
		if (!was_on || gen != seen_gen) {
			static const char *sides[] = {"auto", "left", "right", "above", "below"};
			ss_log("%s (trigger %.0f %% for %.1f s; %s; glide %.2f s; side %s; margin %d px)\n",
			       was_on ? "settings" : "on", s.trigger * 100.0, (double)s.dwell_s,
			       s.return_home ? "back home when free" : "no return", (double)s.glide_s, sides[s.side], s.margin);
			was_on = 1;
			seen_gen = gen;
		}
		tick(&w, &s);
		// Sleep until the next poll, or until the settings change. Granted: every
		// DXR_SS_POLL_MS, so a revoke stops it within one poll. Not granted: the next
		// poll only asks the gate (each answer is a runtime call, with the runtime's
		// own lock probe), so once a second; a grant starts it within a second.
		long poll_ms = w.not_granted ? DXR_SS_CLOSED_POLL_MS : DXR_SS_POLL_MS;
		pthread_mutex_lock(&s_mutex);
		struct timespec until;
		clock_gettime(CLOCK_MONOTONIC, &until);
		until.tv_nsec += poll_ms * 1000000L;
		until.tv_sec += until.tv_nsec / 1000000000L;
		until.tv_nsec %= 1000000000L;
		while (s_enabled && s_settings_gen == gen) {
			if (pthread_cond_timedwait(&s_cond, &s_mutex, &until) == ETIMEDOUT)
				break;
		}
		pthread_mutex_unlock(&s_mutex);
	}
	return NULL;
}

/*
 *
 * Exports.
 *
 */

DISPLAYXR_EXPORT int
displayxr_linux_side_step_set(int enabled, const DisplayXRSideStepSettings *settings)
{
	DxrSsSettings s;
	dxr_ss_default_settings(&s);
	if (settings) {
		s.trigger = settings->trigger;
		s.dwell_s = settings->dwell_s;
		s.return_delay_s = settings->return_delay_s;
		s.glide_s = settings->glide_s;
		s.return_home = settings->return_home;
		s.side = settings->side;
		s.margin = settings->margin;
	}
	dxr_ss_clamp_settings(&s);
	pthread_mutex_lock(&s_mutex);
	if (!s_cond_ready) {
		pthread_condattr_t ca;
		pthread_condattr_init(&ca);
		pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
		pthread_cond_init(&s_cond, &ca);
		pthread_condattr_destroy(&ca);
		s_cond_ready = 1;
	}
	if (memcmp(&s, &s_settings, sizeof(s)) != 0) {
		s_settings = s;
		s_settings_gen++;
	}
	s_enabled = enabled ? 1 : 0;
	__atomic_store_n(&s_want_body, s_enabled, __ATOMIC_RELAXED);
	if (s_enabled && !s_thread_started) {
		pthread_t t;
		pthread_attr_t a;
		pthread_attr_init(&a);
		pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
		if (pthread_create(&t, &a, worker_main, NULL) == 0)
			s_thread_started = 1;
		pthread_attr_destroy(&a);
		if (!s_thread_started) {
			s_enabled = 0;
			pthread_mutex_unlock(&s_mutex);
			ss_log("could not start its thread: off\n");
			return 0;
		}
	}
	if (s_enabled && s_status.state == DXR_SS_STATE_OFF)
		s_status.state = DXR_SS_STATE_WAITING;
	pthread_cond_broadcast(&s_cond);
	pthread_mutex_unlock(&s_mutex);
	return 1;
}

DISPLAYXR_EXPORT void
displayxr_linux_side_step_set_body_rect(int x, int y, int w, int h)
{
	pthread_mutex_lock(&s_body_mutex);
	s_app_rect_valid = w > 0 && h > 0;
	s_app_x = x;
	s_app_y = y;
	s_app_w = w;
	s_app_h = h;
	pthread_mutex_unlock(&s_body_mutex);
}

DISPLAYXR_EXPORT void
displayxr_linux_side_step_status(DisplayXRSideStepStatus *out)
{
	if (!out)
		return;
	pthread_mutex_lock(&s_mutex);
	*out = s_status;
	if (!s_enabled)
		out->state = DXR_SS_STATE_OFF;
	pthread_mutex_unlock(&s_mutex);
}
