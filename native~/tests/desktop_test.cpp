// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Unit test of the XR_DXR_desktop_control binding (displayxr_provider_desktop.cpp)
// against a fake runtime: entry-point resolution, pass-through of every
// observation call, every submit field marshalled, UTF-8 (Chinese + emoji) intact
// both ways, events queued FIFO / bounded / coalesced / filtered by session, and
// clean failure codes with no extension or no session. Spec v2 screen capture: the
// scope / parent-window chain (parent passed verbatim, spaces included), every
// capture field marshalled, the two-call image fetch, capture events, and a v1
// runtime (or missing capture entry points) leaving v1 untouched and capture
// reporting E_NO_CAPTURE. No Unity, no real runtime.
//
//   c++ -std=c++17 -I native~/displayxr_xrprovider -I <openxr include>
//       native~/tests/desktop_test.cpp native~/displayxr_xrprovider/displayxr_provider_desktop.cpp
//   (one command; see the "Desktop control binding tests" step in build-native.yml)

#include "displayxr_provider_desktop.h"
#include "../displayxr_extensions.h"

#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

static int g_fail = 0;
static int g_checks = 0;
#define CHECK(c)                                                                                                       \
	do {                                                                                                               \
		g_checks++;                                                                                                    \
		if (!(c)) {                                                                                                    \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                               \
			g_fail++;                                                                                                  \
		}                                                                                                              \
	} while (0)

// ---------------------------------------------------------------- fake runtime

static const char *kTitleZh = "记事本 — 笔记.txt 😀";
static const char *kAppZh = "微信";
static const char *kTextZh = "你好，世界 👋🏽 café";

struct Fake
{
	bool omit_submit = false; // gipa does not resolve xrSubmitDesktopActionDXR
	bool omit_capture_image = false; // gipa does not resolve xrGetDesktopCaptureImageDXR
	// Request chain as seen by the runtime (spec v2).
	bool request_had_scope = false, request_had_parent = false;
	uint64_t request_scopes = 0;
	std::string request_parent;
	// Capture.
	XrDesktopControlStateDXR capture_state = XR_DESKTOP_CONTROL_STATE_NOT_REQUESTED_DXR;
	XrDesktopCaptureInfoDXR last_capture;
	std::vector<uint8_t> image;
	uint64_t image_id = 0;
	XrResult force = XR_SUCCESS; // returned by every observation call when failing
	XrSession last_session = XR_NULL_HANDLE;
	XrDesktopControlStateDXR state = XR_DESKTOP_CONTROL_STATE_GRANTED_DXR;
	XrDesktopControlCapabilityFlagsDXR caps = 0x3F;
	bool request_had_info = false;
	std::string reason;
	std::vector<XrDesktopWindowDXR> windows;
	XrOffset2Di last_point = {0, 0};
	XrDesktopPickKindDXR pick_kind = XR_DESKTOP_PICK_KIND_WINDOW_DXR;
	std::string text_at = kTextZh;
	XrDesktopActionDXR last_action;
	std::string last_text, last_keys;
	bool last_text_null = true, last_keys_null = true;
	uint64_t next_id = 1000;
	int calls = 0;
} F;

static XrResult XRAPI_CALL
f_get_state(XrSession s, XrDesktopControlStateDXR *st, XrDesktopControlCapabilityFlagsDXR *caps)
{
	F.calls++;
	F.last_session = s;
	if (F.force != XR_SUCCESS) return F.force;
	*st = F.state;
	*caps = F.caps;
	return XR_SUCCESS;
}
static XrResult XRAPI_CALL
f_request(XrSession s, const XrDesktopControlRequestInfoDXR *info)
{
	F.calls++;
	F.last_session = s;
	F.request_had_info = info != nullptr;
	F.reason = (info && info->reason) ? info->reason : "";
	if (info && info->type != XR_TYPE_DESKTOP_CONTROL_REQUEST_INFO_DXR) return XR_ERROR_VALIDATION_FAILURE;
	F.request_had_scope = F.request_had_parent = false;
	F.request_scopes = 0;
	F.request_parent.clear();
	for (const XrBaseInStructure *n = info ? (const XrBaseInStructure *)info->next : nullptr; n; n = n->next) {
		if (n->type == XR_TYPE_DESKTOP_CONTROL_REQUEST_SCOPE_DXR) {
			F.request_had_scope = true;
			F.request_scopes = ((const XrDesktopControlRequestScopeDXR *)n)->scopes;
		} else if (n->type == XR_TYPE_DESKTOP_CONTROL_PARENT_WINDOW_DXR) {
			const char *pw = ((const XrDesktopControlParentWindowDXR *)n)->parentWindow;
			F.request_had_parent = true;
			F.request_parent = pw ? pw : "";
		} else {
			return XR_ERROR_VALIDATION_FAILURE; // nothing else is ever chained
		}
	}
	F.state = XR_DESKTOP_CONTROL_STATE_PENDING_DXR;
	return XR_SUCCESS;
}
static XrResult XRAPI_CALL
f_enumerate(XrSession s, uint32_t cap, uint32_t *count, XrDesktopWindowDXR *out)
{
	F.calls++;
	F.last_session = s;
	if (F.force != XR_SUCCESS) return F.force;
	*count = (uint32_t)F.windows.size();
	if (cap == 0) return XR_SUCCESS;
	if (cap < F.windows.size()) return XR_ERROR_SIZE_INSUFFICIENT;
	for (size_t i = 0; i < F.windows.size(); i++) {
		if (out[i].type != XR_TYPE_DESKTOP_WINDOW_DXR) return XR_ERROR_VALIDATION_FAILURE;
		XrDesktopWindowDXR w = F.windows[i];
		w.next = out[i].next;
		out[i] = w;
	}
	return XR_SUCCESS;
}
static XrResult
fill_pick(XrOffset2Di pt, XrDesktopPickDXR *pick)
{
	if (pick->type != XR_TYPE_DESKTOP_PICK_DXR) return XR_ERROR_VALIDATION_FAILURE;
	pick->point = pt;
	pick->kind = F.pick_kind;
	if (F.pick_kind == XR_DESKTOP_PICK_KIND_WINDOW_DXR && !F.windows.empty()) pick->window = F.windows[0];
	return XR_SUCCESS;
}
static XrResult XRAPI_CALL
f_get_pointer(XrSession s, XrDesktopPickDXR *pick)
{
	F.calls++;
	F.last_session = s;
	if (F.force != XR_SUCCESS) return F.force;
	return fill_pick({-1920, 37}, pick); // a monitor left of the primary: negative x
}
static XrResult XRAPI_CALL
f_pick(XrSession s, XrOffset2Di pt, XrDesktopPickDXR *pick)
{
	F.calls++;
	F.last_session = s;
	F.last_point = pt;
	if (F.force != XR_SUCCESS) return F.force;
	return fill_pick(pt, pick);
}
static XrResult XRAPI_CALL
f_text_at(XrSession s, XrOffset2Di pt, uint32_t cap, uint32_t *count, char *buf)
{
	F.calls++;
	F.last_session = s;
	F.last_point = pt;
	if (F.force != XR_SUCCESS) return F.force;
	*count = (uint32_t)F.text_at.size() + 1;
	if (cap == 0) return XR_SUCCESS;
	if (cap < *count) return XR_ERROR_SIZE_INSUFFICIENT;
	memcpy(buf, F.text_at.c_str(), *count);
	return XR_SUCCESS;
}
static XrResult XRAPI_CALL
f_submit(XrSession s, const XrDesktopActionDXR *a, uint64_t *id)
{
	F.calls++;
	F.last_session = s;
	if (F.force != XR_SUCCESS) return F.force;
	if (a->type != XR_TYPE_DESKTOP_ACTION_DXR || a->targetWindowId == 0) return XR_ERROR_VALIDATION_FAILURE;
	F.last_action = *a;
	F.last_text_null = a->text == nullptr;
	F.last_keys_null = a->keys == nullptr;
	F.last_text = a->text ? a->text : "";
	F.last_keys = a->keys ? a->keys : "";
	*id = F.next_id++;
	return XR_SUCCESS;
}
static XrResult XRAPI_CALL
f_capture_state(XrSession s, XrDesktopControlStateDXR *st)
{
	F.calls++;
	F.last_session = s;
	if (F.force != XR_SUCCESS) return F.force;
	*st = F.capture_state;
	return XR_SUCCESS;
}
static XrResult XRAPI_CALL
f_capture(XrSession s, const XrDesktopCaptureInfoDXR *info, uint64_t *id)
{
	F.calls++;
	F.last_session = s;
	if (F.force != XR_SUCCESS) return F.force;
	if (info->type != XR_TYPE_DESKTOP_CAPTURE_INFO_DXR || info->next != nullptr) return XR_ERROR_VALIDATION_FAILURE;
	if (info->maxWidth == 0 || info->maxWidth > 8192) return XR_ERROR_VALIDATION_FAILURE;
	F.last_capture = *info;
	*id = F.next_id++;
	return XR_SUCCESS;
}
static XrResult XRAPI_CALL
f_capture_image(XrSession s, uint64_t id, uint32_t cap, uint32_t *count, uint8_t *rgba)
{
	F.calls++;
	F.last_session = s;
	if (id == 0 || id != F.image_id) return XR_ERROR_VALIDATION_FAILURE;
	*count = (uint32_t)F.image.size();
	if (cap == 0) return XR_SUCCESS;
	if (cap < F.image.size()) return XR_ERROR_SIZE_INSUFFICIENT;
	memcpy(rgba, F.image.data(), F.image.size());
	F.image_id = 0; // copied out: the runtime frees it
	return XR_SUCCESS;
}
static XrResult XRAPI_CALL
f_gipa(XrInstance, const char *name, PFN_xrVoidFunction *fn)
{
	*fn = nullptr;
	if (!strcmp(name, "xrGetDesktopControlStateDXR")) *fn = (PFN_xrVoidFunction)f_get_state;
	if (!strcmp(name, "xrRequestDesktopControlDXR")) *fn = (PFN_xrVoidFunction)f_request;
	if (!strcmp(name, "xrEnumerateDesktopWindowsDXR")) *fn = (PFN_xrVoidFunction)f_enumerate;
	if (!strcmp(name, "xrGetDesktopPointerDXR")) *fn = (PFN_xrVoidFunction)f_get_pointer;
	if (!strcmp(name, "xrPickDesktopDXR")) *fn = (PFN_xrVoidFunction)f_pick;
	if (!strcmp(name, "xrGetDesktopTextAtDXR")) *fn = (PFN_xrVoidFunction)f_text_at;
	if (!strcmp(name, "xrSubmitDesktopActionDXR") && !F.omit_submit) *fn = (PFN_xrVoidFunction)f_submit;
	if (!strcmp(name, "xrGetDesktopCaptureStateDXR")) *fn = (PFN_xrVoidFunction)f_capture_state;
	if (!strcmp(name, "xrCaptureDesktopDXR")) *fn = (PFN_xrVoidFunction)f_capture;
	if (!strcmp(name, "xrGetDesktopCaptureImageDXR") && !F.omit_capture_image)
		*fn = (PFN_xrVoidFunction)f_capture_image;
	return *fn ? XR_SUCCESS : XR_ERROR_FUNCTION_UNSUPPORTED;
}

static void
quiet(const char *, ...)
{}

static const uint8_t *
u8(const char *s)
{
	return reinterpret_cast<const uint8_t *>(s);
}

static std::string
str(const uint8_t *p)
{
	return std::string(reinterpret_cast<const char *>(p));
}

static XrDesktopWindowDXR
make_window(uint64_t id, const char *app, const char *title, int x, int y, int w, int h, uint64_t state, uint32_t z)
{
	XrDesktopWindowDXR win;
	memset(&win, 0, sizeof(win));
	win.type = XR_TYPE_DESKTOP_WINDOW_DXR;
	win.windowId = id;
	win.parentWindowId = id + 1;
	win.pid = 4242 + (uint32_t)id;
	strncpy(win.appId, app, sizeof(win.appId) - 1);
	strncpy(win.title, title, sizeof(win.title) - 1);
	win.frame = {{x, y}, {w, h}};
	win.state = state;
	win.zOrder = z;
	return win;
}

static void
push_completed(XrSession s, uint64_t id, XrResult r, const char *msg)
{
	XrEventDataBuffer ev = {XR_TYPE_EVENT_DATA_BUFFER};
	XrEventDataDesktopActionCompletedDXR *c = (XrEventDataDesktopActionCompletedDXR *)&ev;
	c->type = XR_TYPE_EVENT_DATA_DESKTOP_ACTION_COMPLETED_DXR;
	c->session = s;
	c->actionId = id;
	c->result = r;
	if (msg) strncpy(c->message, msg, sizeof(c->message) - 1);
	CHECK(dxr_desktop_handle_event(&ev) == 1);
}

static void
push_state(XrSession s, XrDesktopControlStateDXR st)
{
	XrEventDataBuffer ev = {XR_TYPE_EVENT_DATA_BUFFER};
	XrEventDataDesktopControlStateChangedDXR *c = (XrEventDataDesktopControlStateChangedDXR *)&ev;
	c->type = XR_TYPE_EVENT_DATA_DESKTOP_CONTROL_STATE_CHANGED_DXR;
	c->session = s;
	c->state = st;
	CHECK(dxr_desktop_handle_event(&ev) == 1);
}

static void
push_capture_state(XrSession s, XrDesktopControlStateDXR st)
{
	XrEventDataBuffer ev = {XR_TYPE_EVENT_DATA_BUFFER};
	XrEventDataDesktopCaptureStateChangedDXR *c = (XrEventDataDesktopCaptureStateChangedDXR *)&ev;
	c->type = XR_TYPE_EVENT_DATA_DESKTOP_CAPTURE_STATE_CHANGED_DXR;
	c->session = s;
	c->state = st;
	CHECK(dxr_desktop_handle_event(&ev) == 1);
}

static DxrDesktopAction
action(int type, uint64_t target)
{
	DxrDesktopAction a;
	memset(&a, 0, sizeof(a));
	a.action_type = type;
	a.target_window_id = target;
	return a;
}

int
main()
{
	XrInstance inst = (XrInstance)(uintptr_t)1;
	XrSession s1 = (XrSession)(uintptr_t)11, s2 = (XrSession)(uintptr_t)12;
	int32_t st = -1;
	uint64_t caps = 99, id = 7;
	uint32_t count = 0;
	DxrDesktopPick pick;
	DxrDesktopEvent ev;
	DxrDesktopAction a = action(XR_DESKTOP_ACTION_TYPE_ACTIVATE_DXR, 5);

	// --- No instance yet / extension never enabled: clean failures, no runtime calls
	CHECK(dxr_prov_desktop_available() == 0);
	CHECK(dxr_prov_desktop_get_state(&st, &caps) == DXR_DESKTOP_E_UNAVAILABLE);
	CHECK(st == XR_DESKTOP_CONTROL_STATE_UNSUPPORTED_DXR && caps == 0);
	dxr_desktop_on_instance(inst, f_gipa, 0, quiet); // runtime did not advertise it
	dxr_desktop_on_session_created(s1);
	CHECK(dxr_prov_desktop_available() == 0);
	CHECK(dxr_prov_desktop_request(u8("x")) == DXR_DESKTOP_E_UNAVAILABLE);
	CHECK(dxr_prov_desktop_enumerate_windows(0, &count, nullptr) == DXR_DESKTOP_E_UNAVAILABLE && count == 0);
	CHECK(dxr_prov_desktop_get_pointer(&pick) == DXR_DESKTOP_E_UNAVAILABLE);
	CHECK(dxr_prov_desktop_pick(1, 2, &pick) == DXR_DESKTOP_E_UNAVAILABLE);
	CHECK(dxr_prov_desktop_text_at(1, 2, 0, &count, nullptr) == DXR_DESKTOP_E_UNAVAILABLE);
	CHECK(dxr_prov_desktop_submit(&a, nullptr, nullptr, &id) == DXR_DESKTOP_E_UNAVAILABLE && id == 0);
	CHECK(F.calls == 0);
	dxr_desktop_on_session_destroying();

	// --- One entry point missing: the whole binding stays off -------------------
	F.omit_submit = true;
	dxr_desktop_on_instance(inst, f_gipa, 1, quiet);
	dxr_desktop_on_session_created(s1);
	CHECK(dxr_prov_desktop_available() == 0);
	CHECK(dxr_prov_desktop_get_state(&st, &caps) == DXR_DESKTOP_E_UNAVAILABLE);
	CHECK(F.calls == 0);
	dxr_desktop_on_session_destroying();
	F.omit_submit = false;

	// --- Enabled, but no session: E_NO_SESSION ---------------------------------
	dxr_desktop_on_instance(inst, f_gipa, 1, quiet);
	CHECK(dxr_prov_desktop_available() == 0);
	CHECK(dxr_prov_desktop_get_state(&st, &caps) == DXR_DESKTOP_E_NO_SESSION);
	CHECK(dxr_prov_desktop_submit(&a, nullptr, nullptr, &id) == DXR_DESKTOP_E_NO_SESSION);
	CHECK(F.calls == 0);
	// Bad arguments never reach the runtime.
	CHECK(dxr_prov_desktop_get_state(nullptr, &caps) == DXR_DESKTOP_E_BAD_ARGUMENT);
	CHECK(dxr_prov_desktop_enumerate_windows(4, &count, nullptr) == DXR_DESKTOP_E_BAD_ARGUMENT);
	CHECK(dxr_prov_desktop_get_pointer(nullptr) == DXR_DESKTOP_E_BAD_ARGUMENT);
	CHECK(dxr_prov_desktop_submit(nullptr, nullptr, nullptr, &id) == DXR_DESKTOP_E_BAD_ARGUMENT);
	CHECK(dxr_prov_desktop_text_at(0, 0, 8, &count, nullptr) == DXR_DESKTOP_E_BAD_ARGUMENT);

	// --- Session 1 ----------------------------------------------------------------
	uint32_t gen0 = dxr_prov_desktop_session_generation();
	dxr_desktop_on_session_created(s1);
	CHECK(dxr_prov_desktop_session_generation() == gen0 + 1);
	CHECK(dxr_prov_desktop_available() == 1);

	// State + capabilities pass straight through, on the right session.
	CHECK(dxr_prov_desktop_get_state(&st, &caps) == XR_SUCCESS);
	CHECK(st == XR_DESKTOP_CONTROL_STATE_GRANTED_DXR && caps == 0x3F && F.last_session == s1);

	// Request: a UTF-8 reason reaches the runtime intact; none = NULL info.
	const char *reason = "让 Lumo 为你打开应用并输入文字 ✨";
	CHECK(dxr_prov_desktop_request(u8(reason)) == XR_SUCCESS);
	CHECK(F.request_had_info && F.reason == reason);
	CHECK(dxr_prov_desktop_request(nullptr) == XR_SUCCESS && !F.request_had_info);
	CHECK(dxr_prov_desktop_request(u8("")) == XR_SUCCESS && !F.request_had_info);
	F.state = XR_DESKTOP_CONTROL_STATE_GRANTED_DXR;

	// --- Enumerate: two-call, every field, UTF-8 titles, truncation --------------
	F.windows.push_back(make_window(0x1234567890ull, kAppZh, kTitleZh, -1920, -8, 1280, 720, 0x1 | 0x10, 0));
	F.windows.push_back(make_window(77, "org.gnome.TextEditor", "notes.txt", 100, 200, 640, 480, 0x2, 1));
	XrDesktopWindowDXR longw = make_window(78, "app", "", 0, 0, 1, 1, 0, 2);
	memset(longw.title, 'A', sizeof(longw.title)); // unterminated: the binding must terminate it
	memset(longw.appId, 'B', sizeof(longw.appId));
	F.windows.push_back(longw);

	CHECK(dxr_prov_desktop_enumerate_windows(0, &count, nullptr) == XR_SUCCESS && count == 3);
	std::vector<DxrDesktopWindow> wins(3);
	CHECK(dxr_prov_desktop_enumerate_windows(2, &count, wins.data()) == XR_ERROR_SIZE_INSUFFICIENT && count == 3);
	CHECK(dxr_prov_desktop_enumerate_windows(3, &count, wins.data()) == XR_SUCCESS && count == 3);
	CHECK(wins[0].window_id == 0x1234567890ull && wins[0].parent_window_id == 0x1234567891ull);
	CHECK(wins[0].pid == 4242 + (uint32_t)0x1234567890ull && wins[0].z_order == 0 && wins[0].state == 0x11);
	CHECK(wins[0].x == -1920 && wins[0].y == -8 && wins[0].width == 1280 && wins[0].height == 720);
	CHECK(str(wins[0].title) == kTitleZh && str(wins[0].app_id) == kAppZh);
	CHECK(wins[1].window_id == 77 && str(wins[1].app_id) == "org.gnome.TextEditor" && wins[1].z_order == 1);
	CHECK(wins[1].state == 0x2 && wins[1].x == 100 && wins[1].height == 480);
	CHECK(wins[2].title[255] == 0 && strlen((const char *)wins[2].title) == 255);
	CHECK(wins[2].app_id[127] == 0 && strlen((const char *)wins[2].app_id) == 127);

	// --- Pointer / pick -----------------------------------------------------------
	CHECK(dxr_prov_desktop_get_pointer(&pick) == XR_SUCCESS);
	CHECK(pick.x == -1920 && pick.y == 37 && pick.kind == XR_DESKTOP_PICK_KIND_WINDOW_DXR);
	CHECK(pick.window.window_id == 0x1234567890ull && str(pick.window.title) == kTitleZh);
	CHECK(dxr_prov_desktop_pick(-5, 1440, &pick) == XR_SUCCESS);
	CHECK(F.last_point.x == -5 && F.last_point.y == 1440 && pick.x == -5 && pick.y == 1440);
	F.pick_kind = XR_DESKTOP_PICK_KIND_SHELL_DXR;
	CHECK(dxr_prov_desktop_pick(10, 1070, &pick) == XR_SUCCESS);
	CHECK(pick.kind == XR_DESKTOP_PICK_KIND_SHELL_DXR && pick.window.window_id == 0 && pick.window.title[0] == 0);
	F.pick_kind = XR_DESKTOP_PICK_KIND_WINDOW_DXR;

	// --- Text at: two-call, UTF-8 ---------------------------------------------------
	CHECK(dxr_prov_desktop_text_at(300, 400, 0, &count, nullptr) == XR_SUCCESS);
	CHECK(count == strlen(kTextZh) + 1 && F.last_point.x == 300 && F.last_point.y == 400);
	std::vector<uint8_t> tb(count);
	CHECK(dxr_prov_desktop_text_at(300, 400, 4, &count, tb.data()) == XR_ERROR_SIZE_INSUFFICIENT);
	CHECK(dxr_prov_desktop_text_at(300, 400, (uint32_t)tb.size(), &count, tb.data()) == XR_SUCCESS);
	CHECK(str(tb.data()) == kTextZh);
	F.text_at = ""; // an element without accessible text: count 1, empty, not an error
	CHECK(dxr_prov_desktop_text_at(1, 1, 0, &count, nullptr) == XR_SUCCESS && count == 1);

	// --- Not granted: the runtime's code passes through untouched ----------------
	F.force = XR_ERROR_DESKTOP_CONTROL_NOT_GRANTED_DXR;
	CHECK(dxr_prov_desktop_enumerate_windows(0, &count, nullptr) == XR_ERROR_DESKTOP_CONTROL_NOT_GRANTED_DXR);
	CHECK(dxr_prov_desktop_get_pointer(&pick) == XR_ERROR_DESKTOP_CONTROL_NOT_GRANTED_DXR);
	CHECK(dxr_prov_desktop_get_state(&st, &caps) == XR_ERROR_DESKTOP_CONTROL_NOT_GRANTED_DXR && st == 0 && caps == 0);
	CHECK(dxr_prov_desktop_submit(&a, nullptr, nullptr, &id) == XR_ERROR_DESKTOP_CONTROL_NOT_GRANTED_DXR && id == 0);
	F.force = XR_SUCCESS;

	// --- Submit: every field of every action shape reaches the runtime ----------
	DxrDesktopAction m = action(XR_DESKTOP_ACTION_TYPE_MOVE_RESIZE_DXR, 77);
	m.rect_x = -2560;
	m.rect_y = 30;
	m.rect_width = 800;
	m.rect_height = 0;
	CHECK(dxr_prov_desktop_submit(&m, nullptr, nullptr, &id) == XR_SUCCESS && id == 1000);
	CHECK(F.last_action.type == XR_TYPE_DESKTOP_ACTION_DXR && F.last_action.next == nullptr);
	CHECK(F.last_action.actionType == XR_DESKTOP_ACTION_TYPE_MOVE_RESIZE_DXR && F.last_action.targetWindowId == 77);
	CHECK(F.last_action.rect.offset.x == -2560 && F.last_action.rect.offset.y == 30);
	CHECK(F.last_action.rect.extent.width == 800 && F.last_action.rect.extent.height == 0);
	CHECK(F.last_text_null && F.last_keys_null);

	DxrDesktopAction t = action(XR_DESKTOP_ACTION_TYPE_TYPE_TEXT_DXR, 0x1234567890ull);
	CHECK(dxr_prov_desktop_submit(&t, u8(kTextZh), nullptr, &id) == XR_SUCCESS && id == 1001);
	CHECK(F.last_action.actionType == XR_DESKTOP_ACTION_TYPE_TYPE_TEXT_DXR);
	CHECK(F.last_action.targetWindowId == 0x1234567890ull);
	CHECK(!F.last_text_null && F.last_text == kTextZh && F.last_keys_null);

	DxrDesktopAction k = action(XR_DESKTOP_ACTION_TYPE_KEY_CHORD_DXR, 77);
	CHECK(dxr_prov_desktop_submit(&k, nullptr, u8("ctrl+shift+s"), &id) == XR_SUCCESS && id == 1002);
	CHECK(F.last_keys == "ctrl+shift+s" && F.last_text_null);

	DxrDesktopAction c = action(XR_DESKTOP_ACTION_TYPE_CLICK_DXR, 77);
	c.x = -100;
	c.y = 250;
	c.button = XR_DESKTOP_POINTER_BUTTON_RIGHT_DXR;
	c.click_count = 2;
	CHECK(dxr_prov_desktop_submit(&c, nullptr, nullptr, &id) == XR_SUCCESS);
	CHECK(F.last_action.actionType == XR_DESKTOP_ACTION_TYPE_CLICK_DXR && F.last_action.point.x == -100);
	CHECK(F.last_action.point.y == 250 && F.last_action.button == XR_DESKTOP_POINTER_BUTTON_RIGHT_DXR);
	CHECK(F.last_action.clickCount == 2);

	DxrDesktopAction d = action(XR_DESKTOP_ACTION_TYPE_DRAG_DXR, 77);
	d.x = 1;
	d.y = 2;
	d.x2 = 3001;
	d.y2 = -4;
	d.button = XR_DESKTOP_POINTER_BUTTON_MIDDLE_DXR;
	CHECK(dxr_prov_desktop_submit(&d, nullptr, nullptr, &id) == XR_SUCCESS);
	CHECK(F.last_action.point.x == 1 && F.last_action.point.y == 2);
	CHECK(F.last_action.point2.x == 3001 && F.last_action.point2.y == -4);
	CHECK(F.last_action.button == XR_DESKTOP_POINTER_BUTTON_MIDDLE_DXR);

	DxrDesktopAction sc = action(XR_DESKTOP_ACTION_TYPE_SCROLL_DXR, 77);
	sc.x = 50;
	sc.y = 60;
	sc.scroll_x = -2;
	sc.scroll_y = 5;
	CHECK(dxr_prov_desktop_submit(&sc, nullptr, nullptr, &id) == XR_SUCCESS);
	CHECK(F.last_action.actionType == XR_DESKTOP_ACTION_TYPE_SCROLL_DXR);
	CHECK(F.last_action.scrollX == -2 && F.last_action.scrollY == 5 && F.last_action.point.x == 50);

	DxrDesktopAction pm = action(XR_DESKTOP_ACTION_TYPE_POINTER_MOVE_DXR, 77);
	pm.x = 7;
	pm.y = 8;
	CHECK(dxr_prov_desktop_submit(&pm, nullptr, nullptr, &id) == XR_SUCCESS);
	CHECK(F.last_action.actionType == XR_DESKTOP_ACTION_TYPE_POINTER_MOVE_DXR && F.last_action.point.y == 8);
	const int window_ops[] = {XR_DESKTOP_ACTION_TYPE_ACTIVATE_DXR, XR_DESKTOP_ACTION_TYPE_MINIMIZE_DXR,
	                          XR_DESKTOP_ACTION_TYPE_RESTORE_DXR, XR_DESKTOP_ACTION_TYPE_MAXIMIZE_DXR,
	                          XR_DESKTOP_ACTION_TYPE_CLOSE_DXR};
	for (int op : window_ops) {
		DxrDesktopAction w = action(op, 77);
		CHECK(dxr_prov_desktop_submit(&w, nullptr, nullptr, &id) == XR_SUCCESS);
		CHECK((int)F.last_action.actionType == op);
	}
	// A runtime validation failure passes through and yields no id.
	DxrDesktopAction bad = action(XR_DESKTOP_ACTION_TYPE_ACTIVATE_DXR, 0);
	CHECK(dxr_prov_desktop_submit(&bad, nullptr, nullptr, &id) == XR_ERROR_VALIDATION_FAILURE && id == 0);

	// --- Events: FIFO, UTF-8 messages, state coalescing, session filter ---------
	CHECK(dxr_prov_desktop_next_event(&ev) == 0);
	push_completed(s1, 1000, XR_SUCCESS, "");
	push_state(s1, XR_DESKTOP_CONTROL_STATE_SUSPENDED_DXR);
	const char *refused = "target is not under the point (found 微信: 聊天 🗨)";
	push_completed(s1, 1001, XR_ERROR_DESKTOP_CONTROL_REFUSED_DXR, refused);
	push_state(s1, XR_DESKTOP_CONTROL_STATE_GRANTED_DXR); // replaces the queued SUSPENDED
	push_completed(s2, 9999, XR_SUCCESS, "stale");         // another session: dropped
	{
		XrEventDataBuffer other = {XR_TYPE_EVENT_DATA_BUFFER};
		other.type = XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED;
		CHECK(dxr_desktop_handle_event(&other) == 0); // not ours: left for the provider
		CHECK(dxr_desktop_handle_event(nullptr) == 0);
	}
	{
		XrEventDataBuffer raw = {XR_TYPE_EVENT_DATA_BUFFER};
		XrEventDataDesktopActionCompletedDXR *cc = (XrEventDataDesktopActionCompletedDXR *)&raw;
		cc->type = XR_TYPE_EVENT_DATA_DESKTOP_ACTION_COMPLETED_DXR;
		cc->session = s1;
		cc->actionId = 1002;
		cc->result = XR_ERROR_DESKTOP_WINDOW_LOST_DXR;
		memset(cc->message, 'M', sizeof(cc->message)); // unterminated
		CHECK(dxr_desktop_handle_event(&raw) == 1);
	}
	CHECK(dxr_prov_desktop_next_event(&ev) == 1 && ev.kind == DXR_DESKTOP_EVENT_ACTION_COMPLETED);
	CHECK(ev.action_id == 1000 && ev.result == XR_SUCCESS && ev.message[0] == 0);
	CHECK(dxr_prov_desktop_next_event(&ev) == 1 && ev.action_id == 1001);
	CHECK(ev.result == XR_ERROR_DESKTOP_CONTROL_REFUSED_DXR && str(ev.message) == refused);
	CHECK(dxr_prov_desktop_next_event(&ev) == 1 && ev.kind == DXR_DESKTOP_EVENT_STATE_CHANGED);
	CHECK(ev.state == XR_DESKTOP_CONTROL_STATE_GRANTED_DXR);
	CHECK(dxr_prov_desktop_next_event(&ev) == 1 && ev.action_id == 1002);
	CHECK(ev.result == XR_ERROR_DESKTOP_WINDOW_LOST_DXR && ev.message[255] == 0);
	CHECK(strlen((const char *)ev.message) == 255);
	CHECK(dxr_prov_desktop_next_event(&ev) == 0);
	CHECK(dxr_prov_desktop_next_event(nullptr) == 0);

	// Bounded: 128 completions fit, the 129th is dropped; order is kept.
	for (uint64_t i = 0; i < 130; i++) push_completed(s1, 5000 + i, XR_SUCCESS, nullptr);
	int popped = 0;
	bool in_order = true;
	while (dxr_prov_desktop_next_event(&ev) == 1) {
		if (ev.action_id != 5000 + (uint64_t)popped) in_order = false;
		popped++;
	}
	CHECK(popped == 128 && in_order);

	// --- Session destroy: queue cleared, generation bumped, calls refused --------
	push_completed(s1, 6000, XR_SUCCESS, nullptr);
	push_state(s1, XR_DESKTOP_CONTROL_STATE_DENIED_DXR);
	uint32_t gen1 = dxr_prov_desktop_session_generation();
	dxr_desktop_on_session_destroying();
	CHECK(dxr_prov_desktop_session_generation() == gen1 + 1);
	CHECK(dxr_prov_desktop_next_event(&ev) == 0);
	CHECK(dxr_prov_desktop_available() == 0);
	int before = F.calls;
	CHECK(dxr_prov_desktop_get_state(&st, &caps) == DXR_DESKTOP_E_NO_SESSION && st == 0);
	CHECK(dxr_prov_desktop_submit(&a, nullptr, nullptr, &id) == DXR_DESKTOP_E_NO_SESSION);
	push_completed(s1, 6001, XR_SUCCESS, nullptr); // a straggler after destroy: dropped
	CHECK(dxr_prov_desktop_next_event(&ev) == 0);
	CHECK(F.calls == before);

	// --- Restart: the new session is used ---------------------------------------
	dxr_desktop_on_instance(inst, f_gipa, 1, quiet);
	dxr_desktop_on_session_created(s2);
	CHECK(dxr_prov_desktop_get_state(&st, &caps) == XR_SUCCESS && F.last_session == s2);
	push_completed(s1, 7000, XR_SUCCESS, nullptr); // the old session's: dropped
	push_completed(s2, 7001, XR_SUCCESS, nullptr);
	CHECK(dxr_prov_desktop_next_event(&ev) == 1 && ev.action_id == 7001);
	dxr_desktop_on_session_destroying();

	// ================================================================ spec v2: capture

	DxrDesktopCaptureInfo ci;
	memset(&ci, 0, sizeof(ci));
	ci.target = XR_DESKTOP_CAPTURE_TARGET_REGION_DXR;
	ci.max_width = ci.max_height = 512;
	ci.width = ci.height = 10;

	// --- A spec v1 runtime (even one that would resolve the capture PFNs): v1 untouched,
	// capture E_NO_CAPTURE, never a chained struct.
	dxr_desktop_on_instance(inst, f_gipa, 1, quiet);
	dxr_desktop_on_session_created(s1);
	CHECK(dxr_prov_desktop_available() == 1 && dxr_prov_desktop_capture_available() == 0);
	before = F.calls;
	st = 42;
	CHECK(dxr_prov_desktop_get_capture_state(&st) == DXR_DESKTOP_E_NO_CAPTURE && st == 0);
	CHECK(dxr_prov_desktop_capture(&ci, &id) == DXR_DESKTOP_E_NO_CAPTURE && id == 0);
	CHECK(dxr_prov_desktop_capture_image(1, 0, &count, nullptr) == DXR_DESKTOP_E_NO_CAPTURE);
	CHECK(dxr_prov_desktop_request_scopes(nullptr, DXR_DESKTOP_SCOPE_CAPTURE, u8("x11:4a00007")) ==
	      DXR_DESKTOP_E_NO_CAPTURE);
	CHECK(F.calls == before);
	CHECK(dxr_prov_desktop_request_scopes(u8("why"), DXR_DESKTOP_SCOPE_CONTROL | DXR_DESKTOP_SCOPE_CAPTURE |
	                                                     DXR_DESKTOP_SCOPE_ASK_AGAIN,
	                                      u8("x11:4a00007")) == XR_SUCCESS);
	CHECK(F.request_had_info && F.reason == "why" && !F.request_had_scope && !F.request_had_parent);
	CHECK(dxr_prov_desktop_request_scopes(nullptr, DXR_DESKTOP_SCOPE_CONTROL, u8("x11:1")) == XR_SUCCESS);
	CHECK(!F.request_had_info);
	CHECK(dxr_prov_desktop_request(u8("v1")) == XR_SUCCESS && F.reason == "v1" && !F.request_had_scope);
	dxr_desktop_on_session_destroying();

	// --- v2 advertised but one capture entry point missing: v1 on, capture off ----
	F.omit_capture_image = true;
	dxr_desktop_on_instance(inst, f_gipa, 2, quiet);
	dxr_desktop_on_session_created(s1);
	CHECK(dxr_prov_desktop_available() == 1 && dxr_prov_desktop_capture_available() == 0);
	CHECK(dxr_prov_desktop_capture(&ci, &id) == DXR_DESKTOP_E_NO_CAPTURE);
	CHECK(dxr_prov_desktop_get_state(&st, &caps) == XR_SUCCESS);
	dxr_desktop_on_session_destroying();
	F.omit_capture_image = false;

	// --- v2: no session / bad arguments ------------------------------------------
	dxr_desktop_on_instance(inst, f_gipa, 2, quiet);
	CHECK(dxr_prov_desktop_capture_available() == 0);
	CHECK(dxr_prov_desktop_capture(&ci, &id) == DXR_DESKTOP_E_NO_SESSION);
	CHECK(dxr_prov_desktop_get_capture_state(nullptr) == DXR_DESKTOP_E_BAD_ARGUMENT);
	CHECK(dxr_prov_desktop_capture(nullptr, &id) == DXR_DESKTOP_E_BAD_ARGUMENT);
	CHECK(dxr_prov_desktop_capture_image(1, 4, &count, nullptr) == DXR_DESKTOP_E_BAD_ARGUMENT);

	// --- v2 session -------------------------------------------------------------
	dxr_desktop_on_session_created(s2);
	CHECK(dxr_prov_desktop_capture_available() == 1);
	CHECK(dxr_prov_desktop_get_capture_state(&st) == XR_SUCCESS && st == XR_DESKTOP_CONTROL_STATE_NOT_REQUESTED_DXR);
	CHECK(F.last_session == s2);

	// Request: scopes + ASK_AGAIN + a Wayland handle with spaces (leading, inner,
	// trailing) and a quote, passed byte for byte.
	const char *wl = "wayland: aB3 \"x~ Q9 ";
	CHECK(dxr_prov_desktop_request_scopes(u8("看屏幕"), DXR_DESKTOP_SCOPE_CONTROL | DXR_DESKTOP_SCOPE_CAPTURE |
	                                                        DXR_DESKTOP_SCOPE_ASK_AGAIN,
	                                      u8(wl)) == XR_SUCCESS);
	CHECK(F.request_had_info && F.reason == "看屏幕" && F.request_had_scope);
	CHECK(F.request_scopes == (0x1 | 0x2 | 0x100) && F.request_had_parent && F.request_parent == wl);
	// No reason, no parent, scopes 0 = CONTROL (the v1 default), never an empty parent struct.
	CHECK(dxr_prov_desktop_request_scopes(nullptr, 0, u8("")) == XR_SUCCESS);
	CHECK(F.request_had_info && F.reason.empty() && F.request_had_scope && F.request_scopes == 0x1);
	CHECK(!F.request_had_parent);
	CHECK(dxr_prov_desktop_request_scopes(nullptr, DXR_DESKTOP_SCOPE_CAPTURE, nullptr) == XR_SUCCESS);
	CHECK(F.request_scopes == 0x2 && !F.request_had_parent);
	// Unknown bits never reach the runtime.
	CHECK(dxr_prov_desktop_request_scopes(nullptr, 0x2 | 0x80, u8("x11:4a00007")) == XR_SUCCESS);
	CHECK(F.request_scopes == 0x2 && F.request_parent == "x11:4a00007");
	// The v1 export is unchanged against a v2 runtime too: no chain.
	CHECK(dxr_prov_desktop_request(u8("r")) == XR_SUCCESS && !F.request_had_scope && !F.request_had_parent);

	// Capture: every field marshalled.
	DxrDesktopCaptureInfo w = ci;
	w.target = XR_DESKTOP_CAPTURE_TARGET_WINDOW_DXR;
	w.window_id = 0xABCDEF0123ull;
	w.x = -1920;
	w.y = -8;
	w.width = 1280;
	w.height = 720;
	w.max_width = 640;
	w.max_height = 8192;
	uint64_t cid = 0;
	CHECK(dxr_prov_desktop_capture(&w, &cid) == XR_SUCCESS && cid != 0);
	CHECK(F.last_capture.target == XR_DESKTOP_CAPTURE_TARGET_WINDOW_DXR && F.last_capture.windowId == 0xABCDEF0123ull);
	CHECK(F.last_capture.region.offset.x == -1920 && F.last_capture.region.offset.y == -8);
	CHECK(F.last_capture.region.extent.width == 1280 && F.last_capture.region.extent.height == 720);
	CHECK(F.last_capture.maxWidth == 640 && F.last_capture.maxHeight == 8192);
	DxrDesktopCaptureInfo badc = ci;
	badc.max_width = 0;
	CHECK(dxr_prov_desktop_capture(&badc, &id) == XR_ERROR_VALIDATION_FAILURE && id == 0);
	F.force = XR_ERROR_DESKTOP_CONTROL_NOT_GRANTED_DXR;
	CHECK(dxr_prov_desktop_capture(&ci, &id) == XR_ERROR_DESKTOP_CONTROL_NOT_GRANTED_DXR && id == 0);
	F.force = XR_ERROR_LIMIT_REACHED;
	CHECK(dxr_prov_desktop_capture(&ci, &id) == XR_ERROR_LIMIT_REACHED);
	F.force = XR_SUCCESS;

	// Completion event: every field; capture state coalesces apart from CONTROL's.
	{
		XrEventDataBuffer raw = {XR_TYPE_EVENT_DATA_BUFFER};
		XrEventDataDesktopCaptureCompletedDXR *cc = (XrEventDataDesktopCaptureCompletedDXR *)&raw;
		cc->type = XR_TYPE_EVENT_DATA_DESKTOP_CAPTURE_COMPLETED_DXR;
		cc->session = s2;
		cc->captureId = cid;
		cc->result = XR_SUCCESS;
		cc->width = 3;
		cc->height = 2;
		cc->sourceRect = {{-1920, -8}, {1280, 720}};
		cc->frameTime = 123456789012345ll;
		strncpy(cc->message, "ok ✓", sizeof(cc->message) - 1);
		push_capture_state(s2, XR_DESKTOP_CONTROL_STATE_PENDING_DXR);
		push_state(s2, XR_DESKTOP_CONTROL_STATE_GRANTED_DXR);
		CHECK(dxr_desktop_handle_event(&raw) == 1);
		push_capture_state(s2, XR_DESKTOP_CONTROL_STATE_GRANTED_DXR); // replaces PENDING only
		push_capture_state(s1, XR_DESKTOP_CONTROL_STATE_DENIED_DXR);  // stale session: dropped
	}
	CHECK(dxr_prov_desktop_next_event(&ev) == 1 && ev.kind == DXR_DESKTOP_EVENT_STATE_CHANGED);
	CHECK(ev.state == XR_DESKTOP_CONTROL_STATE_GRANTED_DXR);
	CHECK(dxr_prov_desktop_next_event(&ev) == 1 && ev.kind == DXR_DESKTOP_EVENT_CAPTURE_COMPLETED);
	CHECK(ev.action_id == cid && ev.result == XR_SUCCESS && ev.width == 3 && ev.height == 2);
	CHECK(ev.src_x == -1920 && ev.src_y == -8 && ev.src_width == 1280 && ev.src_height == 720);
	CHECK(ev.frame_time == 123456789012345ll && str(ev.message) == "ok ✓");
	CHECK(dxr_prov_desktop_next_event(&ev) == 1 && ev.kind == DXR_DESKTOP_EVENT_CAPTURE_STATE_CHANGED);
	CHECK(ev.state == XR_DESKTOP_CONTROL_STATE_GRANTED_DXR);
	CHECK(dxr_prov_desktop_next_event(&ev) == 0);

	// Image: two-call in bytes, short buffer keeps it, a copy frees it, other ids fail.
	F.image.clear();
	for (int i = 0; i < 3 * 2 * 4; i++) F.image.push_back((uint8_t)(i * 7));
	F.image_id = cid;
	CHECK(dxr_prov_desktop_capture_image(cid, 0, &count, nullptr) == XR_SUCCESS && count == 24);
	std::vector<uint8_t> img(24);
	CHECK(dxr_prov_desktop_capture_image(cid, 8, &count, img.data()) == XR_ERROR_SIZE_INSUFFICIENT);
	CHECK(dxr_prov_desktop_capture_image(cid + 1, 24, &count, img.data()) == XR_ERROR_VALIDATION_FAILURE);
	CHECK(dxr_prov_desktop_capture_image(cid, 24, &count, img.data()) == XR_SUCCESS && count == 24);
	CHECK(img == F.image);
	CHECK(dxr_prov_desktop_capture_image(cid, 24, &count, img.data()) == XR_ERROR_VALIDATION_FAILURE);

	// Session end: capture refused, capture state UNSUPPORTED.
	dxr_desktop_on_session_destroying();
	CHECK(dxr_prov_desktop_capture_available() == 0);
	CHECK(dxr_prov_desktop_get_capture_state(&st) == DXR_DESKTOP_E_NO_SESSION && st == 0);

	if (g_fail) {
		fprintf(stderr, "%d of %d check(s) failed\n", g_fail, g_checks);
		return 1;
	}
	printf("desktop_test: all %d checks passed\n", g_checks);
	return 0;
}
