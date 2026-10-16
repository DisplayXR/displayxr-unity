// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// XR_DXR_desktop_control binding — see displayxr_provider_desktop.h for the model.

#include "displayxr_provider_desktop.h"
#include "../displayxr_extensions.h"

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
static_assert(sizeof(DxrDesktopAction) == 64 && offsetof(DxrDesktopAction, click_count) == 48,
              "DxrDesktopAction layout is C# ABI");
static_assert(sizeof(DxrDesktopEvent) == 280 && offsetof(DxrDesktopEvent, message) == 24,
              "DxrDesktopEvent layout is C# ABI");
static_assert(sizeof(((DxrDesktopWindow *)0)->app_id) == XR_MAX_DESKTOP_APP_ID_SIZE_DXR &&
                  sizeof(((DxrDesktopWindow *)0)->title) == XR_MAX_DESKTOP_TITLE_SIZE_DXR &&
                  sizeof(((DxrDesktopEvent *)0)->message) == XR_MAX_DESKTOP_MESSAGE_SIZE_DXR,
              "string sizes follow the extension header");

namespace {

// Two locks, never nested the other way round:
//  g_call_mx — held across every runtime call made for C#, and by session
//              create/destroy, so a pass-through never runs against a session that
//              is being destroyed. Runtime calls here never wait on actions, so the
//              hold is short (text-at: tens of ms at worst).
//  g_mx      — the event queue + session handle; taken by the poll thread. Never
//              held across a runtime call.
std::mutex g_call_mx;
std::mutex g_mx;

DxrDesktopLogFn g_log = nullptr;
int g_enabled = 0; // extension enabled on the instance and all seven PFNs resolved
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
dxr_desktop_on_instance(XrInstance instance, PFN_xrGetInstanceProcAddr gipa, int enabled, DxrDesktopLogFn log)
{
	std::lock_guard<std::mutex> clk(g_call_mx);
	std::lock_guard<std::mutex> lk(g_mx);
	g_log = log;
	g_enabled = 0;
	pfn_get_state = nullptr;
	pfn_request = nullptr;
	pfn_enumerate = nullptr;
	pfn_get_pointer = nullptr;
	pfn_pick = nullptr;
	pfn_text_at = nullptr;
	pfn_submit = nullptr;
	if (!enabled || !instance || !gipa) return;

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
	DESK_LOG("[DisplayXR-PROV] desktop control: %s\n", g_enabled ? "extension enabled" : "entry points missing");
}

void
dxr_desktop_on_session_created(XrSession session)
{
	std::lock_guard<std::mutex> clk(g_call_mx);
	std::lock_guard<std::mutex> lk(g_mx);
	g_session = session;
	g_queue.clear();
	g_generation++;
}

void
dxr_desktop_on_session_destroying(void)
{
	// Taking the call lock waits out any pass-through still running for C#.
	std::lock_guard<std::mutex> clk(g_call_mx);
	std::lock_guard<std::mutex> lk(g_mx);
	g_session = XR_NULL_HANDLE;
	g_queue.clear();
	g_generation++;
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
	} else {
		return 0;
	}

	std::lock_guard<std::mutex> lk(g_mx);
	if (g_session == XR_NULL_HANDLE || from != g_session) return 1; // stale session
	if (e.kind == DXR_DESKTOP_EVENT_STATE_CHANGED) {
		// Only the latest consent state matters: keep at most one queued, at the tail.
		for (auto it = g_queue.begin(); it != g_queue.end(); ++it) {
			if (it->kind == DXR_DESKTOP_EVENT_STATE_CHANGED) {
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
	return (int)pfn_submit(s, &a, action_id); // the runtime copies the strings at submit
}

int
dxr_prov_desktop_next_event(DxrDesktopEvent *ev)
{
	if (!ev) return 0;
	std::lock_guard<std::mutex> lk(g_mx);
	if (g_queue.empty()) return 0;
	*ev = g_queue.front();
	g_queue.pop_front();
	return 1;
}
