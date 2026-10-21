// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// XR_DXR_desktop_control binding — see displayxr_provider_desktop.h for the model.

#include "displayxr_provider_desktop.h"
#include "../displayxr_extensions.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <vector>

#include <stddef.h>
#include <string.h>

// The C# structs in Runtime/DisplayXRDesktop.cs mirror these byte for byte.
static_assert(sizeof(DxrDesktopWindow) == 432, "DxrDesktopWindow layout is C# ABI");
static_assert(offsetof(DxrDesktopWindow, x) == 32 && offsetof(DxrDesktopWindow, app_id) == 48 &&
                  offsetof(DxrDesktopWindow, title) == 176,
              "DxrDesktopWindow layout is C# ABI");
static_assert(sizeof(DxrDesktopPick) == 448 && offsetof(DxrDesktopPick, window) == 16,
              "DxrDesktopPick layout is C# ABI");
static_assert(sizeof(DxrDesktopAction) == 64 && offsetof(DxrDesktopAction, click_count) == 48 &&
                  offsetof(DxrDesktopAction, modifiers) == 60,
              "DxrDesktopAction layout is C# ABI");
static_assert(sizeof(DxrDesktopEvent) == 312 && offsetof(DxrDesktopEvent, message) == 24 &&
                  offsetof(DxrDesktopEvent, width) == 280 && offsetof(DxrDesktopEvent, src_x) == 288 &&
                  offsetof(DxrDesktopEvent, frame_time) == 304,
              "DxrDesktopEvent layout is C# ABI");
static_assert(sizeof(DxrDesktopCaptureInfo) == 40 && offsetof(DxrDesktopCaptureInfo, target) == 8 &&
                  offsetof(DxrDesktopCaptureInfo, max_width) == 28,
              "DxrDesktopCaptureInfo layout is C# ABI");
static_assert(sizeof(((DxrDesktopWindow *)0)->app_id) == XR_MAX_DESKTOP_APP_ID_SIZE_DXR &&
                  sizeof(((DxrDesktopWindow *)0)->title) == XR_MAX_DESKTOP_TITLE_SIZE_DXR &&
                  sizeof(((DxrDesktopEvent *)0)->message) == XR_MAX_DESKTOP_MESSAGE_SIZE_DXR,
              "string sizes follow the extension header");
// Spec v3: the mirror must match the runtime's XR_DXR_desktop_control.h (spec §11.1).
static_assert(sizeof(XrDesktopActionModifiersDXR) == 24 && offsetof(XrDesktopActionModifiersDXR, next) == 8 &&
                  offsetof(XrDesktopActionModifiersDXR, modifiers) == 16,
              "XrDesktopActionModifiersDXR layout follows the extension header");
static_assert(XR_TYPE_DESKTOP_ACTION_MODIFIERS_DXR == (XrStructureType)1004999421, "spec v3 structure type");
static_assert(XR_DESKTOP_ACTION_TYPE_KEY_DOWN_DXR == 16 && XR_DESKTOP_ACTION_TYPE_KEY_UP_DXR == 17 &&
                  XR_DESKTOP_ACTION_TYPE_BUTTON_DOWN_DXR == 18 && XR_DESKTOP_ACTION_TYPE_BUTTON_UP_DXR == 19 &&
                  XR_DESKTOP_ACTION_TYPE_SET_CLIPBOARD_TEXT_DXR == 20 &&
                  XR_DESKTOP_ACTION_TYPE_GET_CLIPBOARD_TEXT_DXR == 21,
              "spec v3 action types");
static_assert(XR_DESKTOP_TARGET_SHELL_DXR == UINT64_MAX && DXR_DESKTOP_TARGET_SHELL == XR_DESKTOP_TARGET_SHELL_DXR,
              "spec v3 shell target");
static_assert(XR_MAX_DESKTOP_CLIPBOARD_TEXT_SIZE_DXR == 1048576, "spec v3 clipboard limit");

namespace {

// Two locks, never nested the other way round:
//  g_call_mx — held across every runtime call made for C#, and by session
//              create/destroy, so a pass-through never runs against a session that
//              is being destroyed. Runtime calls here never wait on actions, so the
//              hold is short (text-at: tens of ms at worst).
//  g_mx      — the event queue + session handle + waitable captures; taken by
//              the poll thread and by waiting threads. Never held across a runtime
//              call or a pump (the pump takes g_mx itself, through handle_event).
std::mutex g_call_mx;
std::mutex g_mx;

DxrDesktopLogFn g_log = nullptr;
int g_enabled = 0; // extension enabled on the instance and all seven PFNs resolved
uint32_t g_spec_version = 0; // what the runtime ADVERTISED (0 = not enabled)
int g_capture = 0; // spec >= 2 and the three capture PFNs resolved (implies g_enabled)
int g_v3 = 0;      // spec >= 3 (implies g_enabled): held input, modifiers, the shell target, clipboard SET
XrSession g_session = XR_NULL_HANDLE;
uint32_t g_generation = 0;
std::deque<DxrDesktopEvent> g_queue;
uint32_t g_dropped = 0;

// The runtime queues at most 64 actions per session, each ending in exactly one
// completion, and state events coalesce to one queued entry below — so a healthy
// stream never comes near this. It only bounds a misbehaving one.
constexpr size_t kMaxQueued = 128;

PFN_xrGetDesktopControlStateDXR pfn_get_state = nullptr;
PFN_xrRequestDesktopControlDXR pfn_request = nullptr;
PFN_xrEnumerateDesktopWindowsDXR pfn_enumerate = nullptr;
PFN_xrGetDesktopPointerDXR pfn_get_pointer = nullptr;
PFN_xrPickDesktopDXR pfn_pick = nullptr;
PFN_xrGetDesktopTextAtDXR pfn_text_at = nullptr;
PFN_xrSubmitDesktopActionDXR pfn_submit = nullptr;
// Spec v2 (screen capture).
PFN_xrGetDesktopCaptureStateDXR pfn_capture_state = nullptr;
PFN_xrCaptureDesktopDXR pfn_capture = nullptr;
PFN_xrGetDesktopCaptureImageDXR pfn_capture_image = nullptr;
// Spec v3 (clipboard text fetch).
PFN_xrGetDesktopClipboardTextDXR pfn_clipboard_text = nullptr;

// Waitable captures (dxr_prov_desktop_capture_waitable), under g_mx. The runtime has
// at most one capture in flight per session, so this holds one live entry plus, at
// worst, a few given up on whose completion has not come yet.
struct Waiter
{
	uint64_t id;
	bool done;      // ev holds the completion
	bool ended;     // the session ended first: it never completes
	bool abandoned; // nobody waits any more: drop the completion
	DxrDesktopEvent ev;
};
std::vector<Waiter> g_waiters;
constexpr size_t kMaxWaiters = 16;
// A waitable start is between its runtime call and filing the id it got: its
// completion may already sit in g_queue, so next_event must not hand that out yet.
int g_claiming = 0;
std::condition_variable g_cv;      // with g_mx: a waiter's entry changed
DxrDesktopPumpFn g_pump = nullptr; // the session module's poll-on-this-thread
// How long a waiting thread sleeps between its own polls. A completion another
// thread routes wakes it at once.
constexpr int kPumpSliceMs = 4;

#define DESK_LOG(...)                                                                                                  \
	do {                                                                                                               \
		if (g_log) g_log(__VA_ARGS__);                                                                                 \
	} while (0)

// Copy a fixed, possibly unterminated runtime char array into a NUL-terminated byte
// array of the same size (the last byte is always 0).
template <size_t N>
void
copy_str(uint8_t (&dst)[N], const char (&src)[N])
{
	size_t n = strnlen(src, N - 1);
	memcpy(dst, src, n);
	memset(dst + n, 0, N - n);
}

void
to_c_abi(const XrDesktopWindowDXR &w, DxrDesktopWindow *o)
{
	memset(o, 0, sizeof(*o));
	o->window_id = w.windowId;
	o->parent_window_id = w.parentWindowId;
	o->state = w.state;
	o->pid = w.pid;
	o->z_order = w.zOrder;
	o->x = w.frame.offset.x;
	o->y = w.frame.offset.y;
	o->width = w.frame.extent.width;
	o->height = w.frame.extent.height;
	copy_str(o->app_id, w.appId);
	copy_str(o->title, w.title);
}

void
init_window(XrDesktopWindowDXR *w)
{
	memset(w, 0, sizeof(*w));
	w->type = XR_TYPE_DESKTOP_WINDOW_DXR;
}

void
init_pick(XrDesktopPickDXR *p)
{
	memset(p, 0, sizeof(*p));
	p->type = XR_TYPE_DESKTOP_PICK_DXR;
	init_window(&p->window);
}

void
pick_to_c_abi(const XrDesktopPickDXR &p, DxrDesktopPick *o)
{
	memset(o, 0, sizeof(*o));
	o->x = p.point.x;
	o->y = p.point.y;
	o->kind = (int32_t)p.kind;
	if (p.kind == XR_DESKTOP_PICK_KIND_WINDOW_DXR) to_c_abi(p.window, &o->window);
}

// Resolve the session for a C# call. Caller holds g_call_mx.
int
session_for_call(XrSession *s)
{
	std::lock_guard<std::mutex> lk(g_mx);
	if (!g_enabled) return DXR_DESKTOP_E_UNAVAILABLE;
	if (g_session == XR_NULL_HANDLE) return DXR_DESKTOP_E_NO_SESSION;
	*s = g_session;
	return 0;
}

// As session_for_call, for the spec-v2 capture calls. Caller holds g_call_mx.
int
capture_session_for_call(XrSession *s)
{
	int r = session_for_call(s);
	if (r != 0) return r;
	std::lock_guard<std::mutex> lk(g_mx);
	return g_capture ? 0 : DXR_DESKTOP_E_NO_CAPTURE;
}

// g_mx held. Pointers are valid until g_waiters changes.
Waiter *
find_waiter(uint64_t id)
{
	for (Waiter &w : g_waiters)
		if (w.id == id) return &w;
	return nullptr;
}

// g_mx held.
void
erase_waiter(uint64_t id)
{
	for (auto it = g_waiters.begin(); it != g_waiters.end(); ++it) {
		if (it->id == id) {
			g_waiters.erase(it);
			return;
		}
	}
}

void
to_capture_info(const DxrDesktopCaptureInfo *info, XrDesktopCaptureInfoDXR *ci)
{
	memset(ci, 0, sizeof(*ci));
	ci->type = XR_TYPE_DESKTOP_CAPTURE_INFO_DXR;
	ci->target = (XrDesktopCaptureTargetDXR)info->target;
	ci->windowId = info->window_id;
	ci->region.offset.x = info->x;
	ci->region.offset.y = info->y;
	ci->region.extent.width = info->width;
	ci->region.extent.height = info->height;
	ci->maxWidth = info->max_width;
	ci->maxHeight = info->max_height;
}

// True for what only a spec-v3 runtime understands. A v2 runtime refuses the new action
// types at submit, completes the shell target WINDOW_LOST — and IGNORES a chained
// modifiers struct, running the plain click: the reason this is checked here, untried.
bool
needs_v3(const DxrDesktopAction &a)
{
	return a.action_type >= XR_DESKTOP_ACTION_TYPE_KEY_DOWN_DXR || a.target_window_id == DXR_DESKTOP_TARGET_SHELL ||
	       a.modifiers != 0;
}

const char *
cstr(const uint8_t *p)
{
	return reinterpret_cast<const char *>(p);
}

const char *
state_name(XrDesktopControlStateDXR s)
{
	switch (s) {
	case XR_DESKTOP_CONTROL_STATE_UNSUPPORTED_DXR: return "unsupported";
	case XR_DESKTOP_CONTROL_STATE_NOT_REQUESTED_DXR: return "not-requested";
	case XR_DESKTOP_CONTROL_STATE_PENDING_DXR: return "pending";
	case XR_DESKTOP_CONTROL_STATE_GRANTED_DXR: return "granted";
	case XR_DESKTOP_CONTROL_STATE_DENIED_DXR: return "denied";
	case XR_DESKTOP_CONTROL_STATE_DISABLED_BY_USER_DXR: return "disabled-by-user";
	case XR_DESKTOP_CONTROL_STATE_SUSPENDED_DXR: return "suspended";
	default: return "?";
	}
}

} // namespace

// ---------------------------------------------------------------- provider hooks

void
dxr_desktop_on_instance(XrInstance instance, PFN_xrGetInstanceProcAddr gipa, uint32_t spec_version, DxrDesktopLogFn log)
{
	std::lock_guard<std::mutex> clk(g_call_mx);
	std::lock_guard<std::mutex> lk(g_mx);
	g_log = log;
	g_enabled = 0;
	g_capture = 0;
	g_v3 = 0;
	g_spec_version = 0;
	pfn_clipboard_text = nullptr;
	pfn_capture_state = nullptr;
	pfn_capture = nullptr;
	pfn_capture_image = nullptr;
	pfn_get_state = nullptr;
	pfn_request = nullptr;
	pfn_enumerate = nullptr;
	pfn_get_pointer = nullptr;
	pfn_pick = nullptr;
	pfn_text_at = nullptr;
	pfn_submit = nullptr;
	if (spec_version == 0 || !instance || !gipa) return;

	auto resolve = [&](const char *name) {
		PFN_xrVoidFunction fn = nullptr;
		if (XR_FAILED(gipa(instance, name, &fn))) fn = nullptr;
		return fn;
	};
	pfn_get_state = (PFN_xrGetDesktopControlStateDXR)resolve("xrGetDesktopControlStateDXR");
	pfn_request = (PFN_xrRequestDesktopControlDXR)resolve("xrRequestDesktopControlDXR");
	pfn_enumerate = (PFN_xrEnumerateDesktopWindowsDXR)resolve("xrEnumerateDesktopWindowsDXR");
	pfn_get_pointer = (PFN_xrGetDesktopPointerDXR)resolve("xrGetDesktopPointerDXR");
	pfn_pick = (PFN_xrPickDesktopDXR)resolve("xrPickDesktopDXR");
	pfn_text_at = (PFN_xrGetDesktopTextAtDXR)resolve("xrGetDesktopTextAtDXR");
	pfn_submit = (PFN_xrSubmitDesktopActionDXR)resolve("xrSubmitDesktopActionDXR");

	g_enabled = pfn_get_state && pfn_request && pfn_enumerate && pfn_get_pointer && pfn_pick && pfn_text_at &&
	            pfn_submit;
	g_spec_version = g_enabled ? spec_version : 0;
	// Screen capture only from a runtime that advertises spec v2: a v1 runtime would not
	// understand the chained scope / parent structs, so they are never sent to it.
	if (g_enabled && spec_version >= 2) {
		pfn_capture_state = (PFN_xrGetDesktopCaptureStateDXR)resolve("xrGetDesktopCaptureStateDXR");
		pfn_capture = (PFN_xrCaptureDesktopDXR)resolve("xrCaptureDesktopDXR");
		pfn_capture_image = (PFN_xrGetDesktopCaptureImageDXR)resolve("xrGetDesktopCaptureImageDXR");
		g_capture = pfn_capture_state && pfn_capture && pfn_capture_image;
	}
	// Held input, modifiers, the shell target and the clipboard only from a runtime that
	// advertises spec v3 (see needs_v3). The clipboard fetch has its own entry point; a v3
	// runtime without it still holds keys, and a GET is refused untried.
	if (g_enabled && spec_version >= 3) {
		g_v3 = 1;
		pfn_clipboard_text = (PFN_xrGetDesktopClipboardTextDXR)resolve("xrGetDesktopClipboardTextDXR");
	}
	DESK_LOG("[DisplayXR-PROV] desktop control: %s (spec v%u, screen capture %s, held input / shell / clipboard %s)\n",
	         g_enabled ? "extension enabled" : "entry points missing", spec_version, g_capture ? "on" : "off",
	         g_v3 ? (pfn_clipboard_text ? "on" : "on, no clipboard fetch") : "off");
}

void
dxr_desktop_on_session_created(XrSession session)
{
	std::lock_guard<std::mutex> clk(g_call_mx);
	std::lock_guard<std::mutex> lk(g_mx);
	g_session = session;
	g_queue.clear();
	g_waiters.clear(); // all of an older session (ended there; their threads saw that)
	g_generation++;
	g_cv.notify_all();
}

void
dxr_desktop_on_session_destroying(void)
{
	// Taking the call lock waits out any pass-through still running for C#.
	std::lock_guard<std::mutex> clk(g_call_mx);
	std::lock_guard<std::mutex> lk(g_mx);
	g_session = XR_NULL_HANDLE;
	g_queue.clear();
	// The runtime drops a capture in flight without an event: wake its waiter now.
	for (auto it = g_waiters.begin(); it != g_waiters.end();) {
		if (it->abandoned) {
			it = g_waiters.erase(it);
		} else {
			if (!it->done) it->ended = true;
			++it;
		}
	}
	g_generation++;
	g_cv.notify_all();
}

void
dxr_desktop_set_waiter_pump(DxrDesktopPumpFn pump)
{
	std::lock_guard<std::mutex> lk(g_mx);
	g_pump = pump;
}

int
dxr_desktop_handle_event(const XrEventDataBuffer *ev)
{
	if (!ev) return 0;
	DxrDesktopEvent e;
	memset(&e, 0, sizeof(e));
	XrSession from;
	if (ev->type == XR_TYPE_EVENT_DATA_DESKTOP_ACTION_COMPLETED_DXR) {
		const XrEventDataDesktopActionCompletedDXR *c = (const XrEventDataDesktopActionCompletedDXR *)ev;
		from = c->session;
		e.kind = DXR_DESKTOP_EVENT_ACTION_COMPLETED;
		e.action_id = c->actionId;
		e.result = (int32_t)c->result;
		copy_str(e.message, c->message);
	} else if (ev->type == XR_TYPE_EVENT_DATA_DESKTOP_CONTROL_STATE_CHANGED_DXR) {
		const XrEventDataDesktopControlStateChangedDXR *c = (const XrEventDataDesktopControlStateChangedDXR *)ev;
		from = c->session;
		e.kind = DXR_DESKTOP_EVENT_STATE_CHANGED;
		e.state = (int32_t)c->state;
		DESK_LOG("[DisplayXR-PROV] desktop control: state -> %s\n", state_name(c->state));
	} else if (ev->type == XR_TYPE_EVENT_DATA_DESKTOP_CAPTURE_COMPLETED_DXR) {
		const XrEventDataDesktopCaptureCompletedDXR *c = (const XrEventDataDesktopCaptureCompletedDXR *)ev;
		from = c->session;
		e.kind = DXR_DESKTOP_EVENT_CAPTURE_COMPLETED;
		e.action_id = c->captureId;
		e.result = (int32_t)c->result;
		e.width = c->width;
		e.height = c->height;
		e.src_x = c->sourceRect.offset.x;
		e.src_y = c->sourceRect.offset.y;
		e.src_width = c->sourceRect.extent.width;
		e.src_height = c->sourceRect.extent.height;
		e.frame_time = c->frameTime;
		copy_str(e.message, c->message);
	} else if (ev->type == XR_TYPE_EVENT_DATA_DESKTOP_CAPTURE_STATE_CHANGED_DXR) {
		const XrEventDataDesktopCaptureStateChangedDXR *c = (const XrEventDataDesktopCaptureStateChangedDXR *)ev;
		from = c->session;
		e.kind = DXR_DESKTOP_EVENT_CAPTURE_STATE_CHANGED;
		e.state = (int32_t)c->state;
		DESK_LOG("[DisplayXR-PROV] desktop control: capture state -> %s\n", state_name(c->state));
	} else {
		return 0;
	}

	std::lock_guard<std::mutex> lk(g_mx);
	if (g_session == XR_NULL_HANDLE || from != g_session) return 1; // stale session
	if (e.kind == DXR_DESKTOP_EVENT_CAPTURE_COMPLETED) {
		// A waitable capture's completion goes to its waiter, never to C#'s queue.
		Waiter *w = find_waiter(e.action_id);
		if (w != nullptr) {
			if (w->abandoned) {
				erase_waiter(e.action_id);
			} else {
				w->done = true;
				w->ev = e;
				g_cv.notify_all();
			}
			return 1;
		}
	}
	if (e.kind == DXR_DESKTOP_EVENT_STATE_CHANGED || e.kind == DXR_DESKTOP_EVENT_CAPTURE_STATE_CHANGED) {
		// Only the latest consent state matters: keep at most one queued per scope, at the tail.
		for (auto it = g_queue.begin(); it != g_queue.end(); ++it) {
			if (it->kind == e.kind) {
				g_queue.erase(it);
				break;
			}
		}
	}
	if (g_queue.size() >= kMaxQueued) {
		if (g_dropped++ == 0)
			DESK_LOG("[DisplayXR-PROV] desktop control: event queue full (%zu) - is the main thread stalled? "
			         "dropping events\n",
			         kMaxQueued);
		return 1;
	}
	g_queue.push_back(e);
	return 1;
}

// ---------------------------------------------------------------- C# surface

int
dxr_prov_desktop_available(void)
{
	std::lock_guard<std::mutex> lk(g_mx);
	return (g_enabled && g_session != XR_NULL_HANDLE) ? 1 : 0;
}

uint32_t
dxr_prov_desktop_session_generation(void)
{
	std::lock_guard<std::mutex> lk(g_mx);
	return g_generation;
}

int
dxr_prov_desktop_get_state(int32_t *state, uint64_t *capabilities)
{
	if (state) *state = XR_DESKTOP_CONTROL_STATE_UNSUPPORTED_DXR;
	if (capabilities) *capabilities = 0;
	if (!state || !capabilities) return DXR_DESKTOP_E_BAD_ARGUMENT;
	std::lock_guard<std::mutex> clk(g_call_mx);
	XrSession s;
	int r = session_for_call(&s);
	if (r != 0) return r;
	XrDesktopControlStateDXR st = XR_DESKTOP_CONTROL_STATE_UNSUPPORTED_DXR;
	XrDesktopControlCapabilityFlagsDXR caps = 0;
	XrResult xr = pfn_get_state(s, &st, &caps);
	if (XR_SUCCEEDED(xr)) {
		*state = (int32_t)st;
		*capabilities = caps;
	}
	return (int)xr;
}

int
dxr_prov_desktop_request(const uint8_t *reason_utf8)
{
	std::lock_guard<std::mutex> clk(g_call_mx);
	XrSession s;
	int r = session_for_call(&s);
	if (r != 0) return r;
	if (!reason_utf8 || !*reason_utf8) return (int)pfn_request(s, nullptr);
	XrDesktopControlRequestInfoDXR info = {XR_TYPE_DESKTOP_CONTROL_REQUEST_INFO_DXR};
	info.reason = cstr(reason_utf8);
	return (int)pfn_request(s, &info);
}

int
dxr_prov_desktop_request_scopes(const uint8_t *reason_utf8, uint64_t scopes, const uint8_t *parent_window_utf8)
{
	std::lock_guard<std::mutex> clk(g_call_mx);
	XrSession s;
	int r = session_for_call(&s);
	if (r != 0) return r;
	if ((scopes & (DXR_DESKTOP_SCOPE_CONTROL | DXR_DESKTOP_SCOPE_CAPTURE)) == 0) scopes |= DXR_DESKTOP_SCOPE_CONTROL;
	int capture;
	{
		std::lock_guard<std::mutex> lk(g_mx);
		capture = g_capture;
	}
	bool has_reason = reason_utf8 && *reason_utf8;
	XrDesktopControlRequestInfoDXR info = {XR_TYPE_DESKTOP_CONTROL_REQUEST_INFO_DXR};
	info.reason = has_reason ? cstr(reason_utf8) : nullptr;
	if (!capture) {
		// A spec v1 runtime: only CONTROL exists, asked for exactly as v1 did.
		if (!(scopes & DXR_DESKTOP_SCOPE_CONTROL)) return DXR_DESKTOP_E_NO_CAPTURE;
		return (int)pfn_request(s, has_reason ? &info : nullptr);
	}
	XrDesktopControlRequestScopeDXR scope = {XR_TYPE_DESKTOP_CONTROL_REQUEST_SCOPE_DXR};
	scope.scopes = scopes & (DXR_DESKTOP_SCOPE_CONTROL | DXR_DESKTOP_SCOPE_CAPTURE | DXR_DESKTOP_SCOPE_ASK_AGAIN);
	XrDesktopControlParentWindowDXR parent = {XR_TYPE_DESKTOP_CONTROL_PARENT_WINDOW_DXR};
	if (parent_window_utf8 && *parent_window_utf8) {
		// Verbatim: a Wayland handle may contain (leading / trailing) spaces.
		parent.parentWindow = cstr(parent_window_utf8);
		scope.next = &parent;
	}
	info.next = &scope;
	return (int)pfn_request(s, &info);
}

int
dxr_prov_desktop_capture_available(void)
{
	std::lock_guard<std::mutex> lk(g_mx);
	return (g_enabled && g_capture && g_session != XR_NULL_HANDLE) ? 1 : 0;
}

int
dxr_prov_desktop_get_capture_state(int32_t *state)
{
	if (!state) return DXR_DESKTOP_E_BAD_ARGUMENT;
	*state = XR_DESKTOP_CONTROL_STATE_UNSUPPORTED_DXR;
	std::lock_guard<std::mutex> clk(g_call_mx);
	XrSession s;
	int r = capture_session_for_call(&s);
	if (r != 0) return r;
	XrDesktopControlStateDXR st = XR_DESKTOP_CONTROL_STATE_UNSUPPORTED_DXR;
	XrResult xr = pfn_capture_state(s, &st);
	if (XR_SUCCEEDED(xr)) *state = (int32_t)st;
	return (int)xr;
}

int
dxr_prov_desktop_capture(const DxrDesktopCaptureInfo *info, uint64_t *capture_id)
{
	if (!info || !capture_id) return DXR_DESKTOP_E_BAD_ARGUMENT;
	*capture_id = 0;
	std::lock_guard<std::mutex> clk(g_call_mx);
	XrSession s;
	int r = capture_session_for_call(&s);
	if (r != 0) return r;
	XrDesktopCaptureInfoDXR ci;
	to_capture_info(info, &ci);
	return (int)pfn_capture(s, &ci, capture_id);
}

int
dxr_prov_desktop_capture_waitable(const DxrDesktopCaptureInfo *info, uint64_t *capture_id)
{
	if (!info || !capture_id) return DXR_DESKTOP_E_BAD_ARGUMENT;
	*capture_id = 0;
	std::lock_guard<std::mutex> clk(g_call_mx);
	XrSession s;
	int r = capture_session_for_call(&s);
	if (r != 0) return r;
	XrDesktopCaptureInfoDXR ci;
	to_capture_info(info, &ci);
	{
		std::lock_guard<std::mutex> lk(g_mx);
		g_claiming = 1;
	}
	uint64_t id = 0;
	XrResult xr = pfn_capture(s, &ci, &id);

	std::lock_guard<std::mutex> lk(g_mx);
	g_claiming = 0;
	if (XR_SUCCEEDED(xr) && id != 0) {
		if (g_waiters.size() >= kMaxWaiters) {
			// Full of entries nobody collected (a thread that died mid-wait): drop the
			// oldest one nobody can be waiting for, else the oldest.
			auto victim = g_waiters.begin();
			for (auto it = g_waiters.begin(); it != g_waiters.end(); ++it) {
				if (it->abandoned || it->ended) {
					victim = it;
					break;
				}
			}
			g_waiters.erase(victim);
		}
		Waiter w = {};
		w.id = id;
		// The runtime may have finished, and a poll queued the completion for C#,
		// before this line: take it back.
		for (auto it = g_queue.begin(); it != g_queue.end(); ++it) {
			if (it->kind == DXR_DESKTOP_EVENT_CAPTURE_COMPLETED && it->action_id == id) {
				w.done = true;
				w.ev = *it;
				g_queue.erase(it);
				break;
			}
		}
		g_waiters.push_back(w);
		*capture_id = id;
	}
	return (int)xr;
}

int
dxr_prov_desktop_capture_wait(uint64_t capture_id, uint32_t timeout_ms, DxrDesktopEvent *ev)
{
	if (!ev) return DXR_DESKTOP_E_BAD_ARGUMENT;
	memset(ev, 0, sizeof(*ev));
	using clock = std::chrono::steady_clock;
	const clock::time_point deadline = clock::now() + std::chrono::milliseconds(timeout_ms);
	std::unique_lock<std::mutex> lk(g_mx);
	bool pumped = false;
	for (;;) {
		Waiter *w = find_waiter(capture_id);
		if (w == nullptr || w->abandoned) return DXR_DESKTOP_E_BAD_ARGUMENT;
		if (w->done) {
			*ev = w->ev;
			erase_waiter(capture_id);
			return 1;
		}
		if (w->ended) {
			erase_waiter(capture_id);
			return DXR_DESKTOP_E_NO_SESSION;
		}
		// Take the completion off the runtime's queue on this thread: the thread that
		// normally polls may be the one blocked here (see the header). Once per slice.
		DxrDesktopPumpFn pump = g_pump;
		if (!pumped && pump != nullptr) {
			pumped = true;
			lk.unlock();
			pump();
			lk.lock();
			continue;
		}
		const clock::time_point now = clock::now();
		if (now >= deadline) return 0;
		const clock::time_point slice = now + std::chrono::milliseconds(kPumpSliceMs);
		g_cv.wait_until(lk, slice < deadline ? slice : deadline);
		pumped = false;
	}
}

void
dxr_prov_desktop_capture_abandon(uint64_t capture_id)
{
	std::lock_guard<std::mutex> lk(g_mx);
	Waiter *w = find_waiter(capture_id);
	if (w == nullptr) return;
	if (w->done || w->ended) {
		erase_waiter(capture_id); // the runtime frees an uncollected image itself (10 s / next capture)
	} else {
		w->abandoned = true; // dropped when (if) its completion comes
	}
}

int
dxr_prov_desktop_capture_image(uint64_t capture_id, uint32_t capacity, uint32_t *count, uint8_t *rgba)
{
	if (!count || (capacity > 0 && !rgba)) return DXR_DESKTOP_E_BAD_ARGUMENT;
	*count = 0;
	std::lock_guard<std::mutex> clk(g_call_mx);
	XrSession s;
	int r = capture_session_for_call(&s);
	if (r != 0) return r;
	return (int)pfn_capture_image(s, capture_id, capacity, count, capacity ? rgba : nullptr);
}

int
dxr_prov_desktop_enumerate_windows(uint32_t capacity, uint32_t *count, DxrDesktopWindow *windows)
{
	if (!count || (capacity > 0 && !windows)) return DXR_DESKTOP_E_BAD_ARGUMENT;
	*count = 0;
	std::lock_guard<std::mutex> clk(g_call_mx);
	XrSession s;
	int r = session_for_call(&s);
	if (r != 0) return r;
	if (capacity == 0) return (int)pfn_enumerate(s, 0, count, nullptr);

	std::vector<XrDesktopWindowDXR> tmp(capacity);
	for (XrDesktopWindowDXR &w : tmp) init_window(&w);
	uint32_t n = 0;
	XrResult xr = pfn_enumerate(s, capacity, &n, tmp.data());
	*count = n;
	if (XR_SUCCEEDED(xr)) {
		if (n > capacity) n = capacity; // defensive; a conformant runtime never does this
		for (uint32_t i = 0; i < n; i++) to_c_abi(tmp[i], &windows[i]);
	}
	return (int)xr;
}

int
dxr_prov_desktop_get_pointer(DxrDesktopPick *pick)
{
	if (!pick) return DXR_DESKTOP_E_BAD_ARGUMENT;
	memset(pick, 0, sizeof(*pick));
	std::lock_guard<std::mutex> clk(g_call_mx);
	XrSession s;
	int r = session_for_call(&s);
	if (r != 0) return r;
	XrDesktopPickDXR p;
	init_pick(&p);
	XrResult xr = pfn_get_pointer(s, &p);
	if (XR_SUCCEEDED(xr)) pick_to_c_abi(p, pick);
	return (int)xr;
}

int
dxr_prov_desktop_pick(int32_t x, int32_t y, DxrDesktopPick *pick)
{
	if (!pick) return DXR_DESKTOP_E_BAD_ARGUMENT;
	memset(pick, 0, sizeof(*pick));
	std::lock_guard<std::mutex> clk(g_call_mx);
	XrSession s;
	int r = session_for_call(&s);
	if (r != 0) return r;
	XrDesktopPickDXR p;
	init_pick(&p);
	XrOffset2Di pt = {x, y};
	XrResult xr = pfn_pick(s, pt, &p);
	if (XR_SUCCEEDED(xr)) pick_to_c_abi(p, pick);
	return (int)xr;
}

int
dxr_prov_desktop_text_at(int32_t x, int32_t y, uint32_t capacity, uint32_t *count, uint8_t *buffer_utf8)
{
	if (!count || (capacity > 0 && !buffer_utf8)) return DXR_DESKTOP_E_BAD_ARGUMENT;
	*count = 0;
	std::lock_guard<std::mutex> clk(g_call_mx);
	XrSession s;
	int r = session_for_call(&s);
	if (r != 0) return r;
	XrOffset2Di pt = {x, y};
	XrResult xr = pfn_text_at(s, pt, capacity, count, capacity ? reinterpret_cast<char *>(buffer_utf8) : nullptr);
	// Never hand C# an unterminated buffer, whatever the runtime wrote.
	if (XR_SUCCEEDED(xr) && capacity > 0) buffer_utf8[capacity - 1] = 0;
	return (int)xr;
}

int
dxr_prov_desktop_submit(const DxrDesktopAction *action, const uint8_t *text_utf8, const uint8_t *keys_utf8,
                        uint64_t *action_id)
{
	if (!action || !action_id) return DXR_DESKTOP_E_BAD_ARGUMENT;
	*action_id = 0;
	std::lock_guard<std::mutex> clk(g_call_mx);
	XrSession s;
	int r = session_for_call(&s);
	if (r != 0) return r;

	if (needs_v3(*action)) {
		std::lock_guard<std::mutex> lk(g_mx);
		if (!g_v3) return DXR_DESKTOP_E_NO_V3;
		if (action->action_type == XR_DESKTOP_ACTION_TYPE_GET_CLIPBOARD_TEXT_DXR && !pfn_clipboard_text)
			return DXR_DESKTOP_E_NO_V3; // the text could never be fetched
	}

	XrDesktopActionDXR a;
	memset(&a, 0, sizeof(a));
	a.type = XR_TYPE_DESKTOP_ACTION_DXR;
	a.actionType = (XrDesktopActionTypeDXR)action->action_type;
	a.targetWindowId = action->target_window_id;
	a.rect.offset.x = action->rect_x;
	a.rect.offset.y = action->rect_y;
	a.rect.extent.width = action->rect_width;
	a.rect.extent.height = action->rect_height;
	a.point.x = action->x;
	a.point.y = action->y;
	a.point2.x = action->x2;
	a.point2.y = action->y2;
	a.button = (XrDesktopPointerButtonDXR)action->button;
	a.clickCount = action->click_count;
	a.scrollX = action->scroll_x;
	a.scrollY = action->scroll_y;
	a.text = text_utf8 ? cstr(text_utf8) : nullptr;
	a.keys = keys_utf8 ? cstr(keys_utf8) : nullptr;
	// Spec v3 (gated above): the modifiers held around a CLICK / DRAG / SCROLL. The runtime
	// validates the action type and the bits (XR_ERROR_VALIDATION_FAILURE).
	XrDesktopActionModifiersDXR mods;
	memset(&mods, 0, sizeof(mods));
	if (action->modifiers != 0) {
		mods.type = XR_TYPE_DESKTOP_ACTION_MODIFIERS_DXR;
		mods.modifiers = action->modifiers;
		a.next = &mods;
	}
	return (int)pfn_submit(s, &a, action_id); // the runtime copies the strings at submit
}

uint32_t
dxr_prov_desktop_spec_version(void)
{
	std::lock_guard<std::mutex> lk(g_mx);
	return (g_enabled && g_session != XR_NULL_HANDLE) ? g_spec_version : 0;
}

int
dxr_prov_desktop_clipboard_text(uint64_t action_id, uint32_t capacity, uint32_t *count, uint8_t *buffer_utf8)
{
	if (!count || (capacity > 0 && !buffer_utf8)) return DXR_DESKTOP_E_BAD_ARGUMENT;
	*count = 0;
	std::lock_guard<std::mutex> clk(g_call_mx);
	XrSession s;
	int r = session_for_call(&s);
	if (r != 0) return r;
	{
		std::lock_guard<std::mutex> lk(g_mx);
		if (!g_v3 || !pfn_clipboard_text) return DXR_DESKTOP_E_NO_V3;
	}
	XrResult xr = pfn_clipboard_text(s, action_id, capacity, count,
	                                 capacity ? reinterpret_cast<char *>(buffer_utf8) : nullptr);
	// Never hand C# an unterminated buffer, whatever the runtime wrote.
	if (XR_SUCCEEDED(xr) && capacity > 0) buffer_utf8[capacity - 1] = 0;
	return (int)xr;
}

int
dxr_prov_desktop_next_event(DxrDesktopEvent *ev)
{
	if (!ev) return 0;
	std::lock_guard<std::mutex> lk(g_mx);
	if (g_queue.empty()) return 0;
	// A waitable start is filing its id and this may be its completion: next frame.
	if (g_claiming && g_queue.front().kind == DXR_DESKTOP_EVENT_CAPTURE_COMPLETED) return 0;
	*ev = g_queue.front();
	g_queue.pop_front();
	return 1;
}
