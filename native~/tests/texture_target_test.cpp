// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Unit test of the XR_DXR_win32_window_binding v9 texture target binding
// (displayxr_provider_texture_target.cpp, #471) against a fake runtime and an
// injected window resolver. What it pins:
//   - GATING: nothing is chained - not the system-properties struct, not the
//     binding, not the per-frame struct, not the state struct - unless the runtime
//     ADVERTISES spec >= 9, its system properties say supportsTextureTarget, the
//     session is eligible (editor texture mode: a shared texture, no readback
//     callback), a rect was pushed, its reference window resolves, and the opt-out
//     is not set. The plug-in's floor (spec 7) is therefore unaffected.
//   - CHAINING: the binding goes on XrWin32WindowBindingCreateInfoDXR::next with the
//     resolved reference window + client rect; windowHandle (the anchor) and
//     whatever was already chained are left alone; a refused xrCreateSession takes
//     the binding off again.
//   - PER FRAME: one resolve per display time, so a frame's locates and its
//     xrEndFrame carry identical values; (0,0) is a valid offset; an unresolvable
//     or cleared rect chains nothing (the runtime would fail the whole frame).
//   - STATE: one log line per state change, one WARN per change of what disagrees,
//     and a runtime that ignores the binding (D3D12 today: flags 0, nothing
//     reported) is reported as such without a warning while frames keep flowing.
//   - RESTART: the pushed rect survives a session stop / start; the decision does not.
// No Unity, no Win32, no real runtime.
//
//   c++ -std=c++17 -I native~/displayxr_xrprovider -I <openxr include>
//       native~/tests/texture_target_test.cpp
//       native~/displayxr_xrprovider/displayxr_provider_texture_target.cpp

#include "displayxr_provider_texture_target.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

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

// ---------------------------------------------------------------- fakes

static std::vector<std::string> g_log;
static void
test_log(const char *fmt, ...)
{
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	g_log.push_back(buf);
}
static int
log_count(const char *needle)
{
	int n = 0;
	for (const std::string &l : g_log)
		if (l.find(needle) != std::string::npos) n++;
	return n;
}

static void *const kPane = (void *)(uintptr_t)0x1111;
static void *const kRoot = (void *)(uintptr_t)0x2222;
static void *const kAnchor = (void *)(uintptr_t)0x3333;
static void *const kSharedTex = (void *)(uintptr_t)0x4444;

// The fake editor layout: the pane's client origin sits at (pane_in_root) in the
// top-level window's client area, whose origin is at (root_on_screen).
struct FakeWindows
{
	bool alive = true;
	int32_t pane_in_root_x = 1017, pane_in_root_y = 293;
	int32_t root_on_screen_x = 0, root_on_screen_y = 0;
	int calls = 0;
} W;

static int
fake_resolve(void *pane, int32_t x, int32_t y, DxrTtResolved *out)
{
	W.calls++;
	if (pane == nullptr) { // desktop px
		out->reference_window = nullptr;
		out->x = out->screen_x = x;
		out->y = out->screen_y = y;
		return 1;
	}
	if (!W.alive || pane != kPane) return 0;
	out->reference_window = kRoot;
	out->x = W.pane_in_root_x + x;
	out->y = W.pane_in_root_y + y;
	out->screen_x = W.root_on_screen_x + out->x;
	out->screen_y = W.root_on_screen_y + out->y;
	return 1;
}

static void
set_env(const char *value) // NULL = unset
{
#ifdef _WIN32
	_putenv_s("DISPLAYXR_PROV_TEXTURE_TARGET", value ? value : "");
#else
	if (value) setenv("DISPLAYXR_PROV_TEXTURE_TARGET", value, 1);
	else unsetenv("DISPLAYXR_PROV_TEXTURE_TARGET");
#endif
}

// The fake runtime's xrGetSystemProperties: fills the struct if it is in the chain.
static bool
runtime_fill_system_properties(void *chain, bool supports, bool windowless)
{
	for (XrBaseOutStructure *n = (XrBaseOutStructure *)chain; n != nullptr; n = n->next) {
		if (n->type == XR_TYPE_SYSTEM_WIN32_TEXTURE_TARGET_PROPERTIES_DXR) {
			auto *p = (XrSystemWin32TextureTargetPropertiesDXR *)n;
			p->supportsTextureTarget = supports ? XR_TRUE : XR_FALSE;
			p->windowlessPhase = windowless ? XR_TRUE : XR_FALSE;
			return true;
		}
	}
	return false;
}

template <typename T>
static const T *
find_in(const void *chain, XrStructureType type)
{
	for (const XrBaseInStructure *n = (const XrBaseInStructure *)chain; n != nullptr; n = n->next)
		if (n->type == type) return (const T *)n;
	return nullptr;
}

// Bring a session up to "about to xrCreateSession". Returns whether the system
// properties struct was chained.
static bool
start_session(uint32_t advertised_spec, bool supports, bool windowless)
{
	dxr_tt_on_session_starting(test_log);
	dxr_tt_on_extension_version(advertised_spec);
	int tail = 0; // stands for the rest of the system-properties chain
	void *head = dxr_tt_chain_system_properties(&tail);
	const bool chained = head != &tail;
	if (chained) {
		CHECK(runtime_fill_system_properties(head, supports, windowless));
		CHECK(((XrSystemWin32TextureTargetPropertiesDXR *)head)->next == &tail);
	}
	dxr_tt_on_system_properties(1);
	return chained;
}

static XrWin32WindowBindingCreateInfoDXR
editor_texture_binding()
{
	XrWin32WindowBindingCreateInfoDXR wb = {};
	wb.type = XR_TYPE_WIN32_WINDOW_BINDING_CREATE_INFO_DXR;
	wb.windowHandle = kAnchor;
	wb.sharedTextureHandle = kSharedTex;
	return wb;
}

static void
push_default_rect()
{
	// The 250 % reference layout: image at pane +3,+113, 1728x860.
	dxr_prov_set_texture_target_rect(kPane, 3, 113, 1728, 860, DXR_TT_RECT_VALID);
}

// Nothing of the per-frame protocol may be chained on a session without the binding.
static void
check_frame_inert()
{
	XrWin32TextureTargetFrameDXR f;
	int tail = 0;
	CHECK(dxr_tt_chain_frame(&f, &tail, 1000, 0) == &tail);
	CHECK(dxr_tt_chain_frame(&f, &tail, 1000, 1) == &tail);
	XrWin32TextureTargetStateDXR st;
	CHECK(dxr_tt_chain_frame_state(&st, &tail) == &tail);
}

// ---------------------------------------------------------------- tests

static void
test_gating()
{
	DxrTextureTargetState out;

	// Before any session.
	CHECK(dxr_prov_get_texture_target_state(&out) == 0);
	check_frame_inert();

	// Older runtime (spec 8): no struct of v9 reaches it, anywhere.
	push_default_rect();
	CHECK(!start_session(8, true, true));
	XrWin32WindowBindingCreateInfoDXR wb = editor_texture_binding();
	CHECK(dxr_tt_bind_session(&wb, 1) == 0);
	CHECK(wb.next == nullptr);
	CHECK(wb.windowHandle == kAnchor && wb.sharedTextureHandle == kSharedTex);
	CHECK(dxr_prov_get_texture_target_state(&out) == 0 && out.why_not == DXR_TT_WHY_RUNTIME_SPEC);
	check_frame_inert();
	dxr_tt_on_session_stopped();

	// Extension absent altogether.
	CHECK(!start_session(0, true, true));
	wb = editor_texture_binding();
	CHECK(dxr_tt_bind_session(&wb, 1) == 0 && wb.next == nullptr);
	dxr_tt_on_session_stopped();

	// Spec 9, but the system says no (an IPC / service-mode runtime).
	CHECK(start_session(9, false, false));
	wb = editor_texture_binding();
	CHECK(dxr_tt_bind_session(&wb, 1) == 0 && wb.next == nullptr);
	CHECK(dxr_prov_get_texture_target_state(&out) == 0 && out.why_not == DXR_TT_WHY_NOT_SUPPORTED);
	check_frame_inert();
	dxr_tt_on_session_stopped();

	// Spec 9 + supported, but the session is not editor texture mode.
	CHECK(start_session(9, true, true));
	wb = editor_texture_binding();
	CHECK(dxr_tt_bind_session(&wb, 0) == 0 && wb.next == nullptr); // built player / present mode
	CHECK(dxr_prov_get_texture_target_state(&out) == 0 && out.why_not == DXR_TT_WHY_NOT_ELIGIBLE);
	wb = editor_texture_binding();
	wb.sharedTextureHandle = nullptr; // the runtime would fail xrCreateSession on the binding
	CHECK(dxr_tt_bind_session(&wb, 1) == 0 && wb.next == nullptr);
	check_frame_inert();
	dxr_tt_on_session_stopped();

	// Eligible, but no rect was pushed: (0,0,0,0) is never read as a rect.
	dxr_prov_set_texture_target_rect(nullptr, 0, 0, 0, 0, 0);
	CHECK(start_session(9, true, true));
	wb = editor_texture_binding();
	CHECK(dxr_tt_bind_session(&wb, 1) == 0 && wb.next == nullptr);
	CHECK(dxr_prov_get_texture_target_state(&out) == 0 && out.why_not == DXR_TT_WHY_NO_RECT);
	dxr_tt_on_session_stopped();

	// VALID without a size is still "no rect".
	dxr_prov_set_texture_target_rect(kPane, 3, 113, 0, 860, DXR_TT_RECT_VALID);
	CHECK(start_session(9, true, true));
	wb = editor_texture_binding();
	CHECK(dxr_tt_bind_session(&wb, 1) == 0 && wb.next == nullptr);
	dxr_tt_on_session_stopped();

	// The pane has no live top-level window: never hand the runtime a reference it
	// would refuse the session for.
	push_default_rect();
	W.alive = false;
	CHECK(start_session(9, true, true));
	wb = editor_texture_binding();
	CHECK(dxr_tt_bind_session(&wb, 1) == 0 && wb.next == nullptr);
	CHECK(dxr_prov_get_texture_target_state(&out) == 0 && out.why_not == DXR_TT_WHY_BAD_REFERENCE);
	dxr_tt_on_session_stopped();
	W.alive = true;

	// Opt-out: not even the system-properties struct is chained.
	set_env("0");
	CHECK(!start_session(9, true, true));
	wb = editor_texture_binding();
	CHECK(dxr_tt_bind_session(&wb, 1) == 0 && wb.next == nullptr);
	CHECK(dxr_prov_get_texture_target_state(&out) == 0 && out.why_not == DXR_TT_WHY_OPT_OUT);
	check_frame_inert();
	dxr_tt_on_session_stopped();
	set_env("1"); // anything but "0" leaves it on
	CHECK(start_session(9, true, true));
	dxr_tt_on_session_stopped();
	set_env(nullptr);
}

static void
test_chaining_and_frames()
{
	g_log.clear();
	push_default_rect();
	CHECK(start_session(9, true, true));
	XrWin32WindowBindingCreateInfoDXR wb = editor_texture_binding();
	int prior = 0; // something already chained on the window binding
	wb.next = &prior;
	CHECK(dxr_tt_bind_session(&wb, 1) == 1);

	const auto *tb = (const XrWin32TextureTargetBindingDXR *)wb.next;
	CHECK(tb != nullptr && tb->type == XR_TYPE_WIN32_TEXTURE_TARGET_BINDING_DXR);
	CHECK(tb->next == &prior);                      // nothing dropped from the chain
	CHECK(tb->referenceWindow == kRoot);            // the top-level window, not the pane
	CHECK(tb->initialTargetRect.offset.x == 1020 && tb->initialTargetRect.offset.y == 406);
	CHECK(tb->initialTargetRect.extent.width == 1728 && tb->initialTargetRect.extent.height == 860);
	CHECK(wb.windowHandle == kAnchor);              // the anchor stays as the fallback
	CHECK(wb.sharedTextureHandle == kSharedTex && wb.readbackCallback == nullptr);
	CHECK(log_count("binding CHAINED") == 1);

	DxrTextureTargetState out;
	CHECK(dxr_prov_get_texture_target_state(&out) == 1 && out.chained == 1 && out.why_not == DXR_TT_WHY_CHAINED);
	CHECK(out.windowless_predicted == 1 && out.reported == 0);

	// One frame: two locates (primary + an extra zone) and the xrEndFrame.
	W.calls = 0;
	int tail = 0;
	XrWin32TextureTargetFrameDXR l1, l2, e1;
	CHECK(dxr_tt_chain_frame(&l1, &tail, 5000, 0) == &l1);
	// The layout changes mid-frame: the frame keeps the values it located with.
	W.pane_in_root_x += 40;
	dxr_prov_set_texture_target_rect(kPane, 9, 9, 100, 100, DXR_TT_RECT_VALID);
	CHECK(dxr_tt_chain_frame(&l2, &tail, 5000, 0) == &l2);
	CHECK(dxr_tt_chain_frame(&e1, &tail, 5000, 1) == &e1);
	CHECK(W.calls == 1); // one resolve per display time
	for (const XrWin32TextureTargetFrameDXR *f : {&l1, &l2, &e1}) {
		CHECK(f->type == XR_TYPE_WIN32_TEXTURE_TARGET_FRAME_DXR && f->next == &tail);
		CHECK(f->referenceWindow == kRoot && f->flags == 0);
		CHECK(f->targetRect.offset.x == 1020 && f->targetRect.offset.y == 406);
		CHECK(f->targetRect.extent.width == 1728 && f->targetRect.extent.height == 860);
	}
	// The next frame picks the new values up.
	XrWin32TextureTargetFrameDXR e2;
	CHECK(dxr_tt_chain_frame(&e2, nullptr, 6000, 1) == &e2);
	CHECK(e2.targetRect.offset.x == 1017 + 40 + 9 && e2.targetRect.offset.y == 293 + 9);
	CHECK(e2.targetRect.extent.width == 100 && e2.targetRect.extent.height == 100);
	W.pane_in_root_x -= 40;

	// (0,0) is a legitimate offset, in the pane and on the desktop.
	dxr_prov_set_texture_target_rect(kPane, 0, 0, 640, 480, DXR_TT_RECT_VALID | DXR_TT_RECT_MEASURED);
	XrWin32TextureTargetFrameDXR e3;
	CHECK(dxr_tt_chain_frame(&e3, nullptr, 7000, 1) == &e3);
	CHECK(e3.targetRect.offset.x == 1017 && e3.targetRect.offset.y == 293 && e3.referenceWindow == kRoot);
	dxr_prov_set_texture_target_rect(nullptr, 0, 0, 640, 480, DXR_TT_RECT_VALID);
	CHECK(dxr_tt_chain_frame(&e3, nullptr, 7001, 1) == &e3);
	CHECK(e3.referenceWindow == nullptr && e3.targetRect.offset.x == 0 && e3.targetRect.offset.y == 0);

	// The pane's window dies (layout reset): nothing is sent - a destroyed reference
	// fails the whole xrEndFrame - and it is said once, not every frame.
	push_default_rect();
	W.alive = false;
	XrWin32TextureTargetFrameDXR e4;
	CHECK(dxr_tt_chain_frame(&e4, &tail, 8000, 1) == &tail);
	CHECK(dxr_tt_chain_frame(&e4, &tail, 8001, 1) == &tail);
	CHECK(log_count("no valid target this frame") == 1);
	W.alive = true;
	CHECK(dxr_tt_chain_frame(&e4, &tail, 8002, 1) == &e4);
	CHECK(log_count("target valid again") == 1);

	// Cleared from managed code: nothing is sent either.
	dxr_prov_set_texture_target_rect(nullptr, 0, 0, 0, 0, 0);
	CHECK(dxr_tt_chain_frame(&e4, &tail, 8003, 1) == &tail);
	push_default_rect();
	CHECK(dxr_tt_chain_frame(&e4, &tail, 8004, 1) == &e4);

	// A refused xrEndFrame is reported once per episode.
	dxr_tt_on_frame_end_result(XR_ERROR_VALIDATION_FAILURE);
	dxr_tt_on_frame_end_result(XR_ERROR_VALIDATION_FAILURE);
	CHECK(log_count("xrEndFrame failed") == 1);
	dxr_tt_on_frame_end_result(XR_SUCCESS);

	dxr_tt_on_session_stopped();
	check_frame_inert();
}

// One frame through the protocol; returns the xrEndFrame struct as sent.
static XrWin32TextureTargetFrameDXR
send_frame(XrTime t)
{
	XrWin32TextureTargetFrameDXR f = {};
	CHECK(dxr_tt_chain_frame(&f, nullptr, t, 1) == &f);
	dxr_tt_on_frame_end_result(XR_SUCCESS);
	return f;
}

// The fake runtime's xrWaitFrame reply for the frame sent at @p t.
static void
report(XrTime t, uint64_t flags, int32_t sx, int32_t sy, XrRect2Di rect)
{
	XrWin32TextureTargetStateDXR st;
	int tail = 0;
	CHECK(dxr_tt_chain_frame_state(&st, &tail) == &st);
	CHECK(st.type == XR_TYPE_WIN32_TEXTURE_TARGET_STATE_DXR && st.next == &tail);
	st.flags = flags;
	st.wovenScreenOrigin = {sx, sy};
	st.wovenTargetRect = rect;
	st.wovenDisplayTime = t;
	dxr_tt_on_frame_state(&st);
}

static void
test_state()
{
	const uint64_t WL = XR_WIN32_TEXTURE_TARGET_STATE_WINDOWLESS_PHASE_BIT_DXR;
	const uint64_t AF = XR_WIN32_TEXTURE_TARGET_STATE_ANCHOR_FALLBACK_BIT_DXR;
	const uint64_t CL = XR_WIN32_TEXTURE_TARGET_STATE_CLAMPED_BIT_DXR;
	DxrTextureTargetState out;

	g_log.clear();
	W.root_on_screen_x = 100; // the editor window is not at the desktop origin
	W.root_on_screen_y = 50;
	push_default_rect();
	CHECK(start_session(9, true, true));
	XrWin32WindowBindingCreateInfoDXR wb = editor_texture_binding();
	CHECK(dxr_tt_bind_session(&wb, 1) == 1);

	// Before the first weave the runtime already says how it will phase.
	XrWin32TextureTargetFrameDXR f = send_frame(100);
	report(0, WL, 0, 0, XrRect2Di{});
	CHECK(log_count("texture target state:") == 1 && log_count("WINDOWLESS_PHASE") == 1);
	CHECK(log_count("no woven frame reported yet") == 1);
	CHECK(dxr_prov_get_texture_target_state(&out) == 1 && out.reported == 0 && out.flags == (uint32_t)WL);

	// First woven frame, exactly as requested: one more line (now with the origin), no WARN.
	report(100, WL, 1120, 456, f.targetRect);
	CHECK(log_count("texture target state:") == 2);
	CHECK(log_count("woven origin (1120,456)") == 1 && log_count("requested origin (1120,456)") == 1);
	CHECK(log_count("WARN") == 0);
	CHECK(dxr_prov_get_texture_target_state(&out) == 1 && out.reported == 1 && out.matched == 1);
	CHECK(out.woven_x == 1120 && out.woven_y == 456 && out.requested_x == 1120 && out.requested_y == 456);
	CHECK(out.rect_x == 1020 && out.rect_y == 406 && out.rect_w == 1728 && out.rect_h == 860 && out.mismatch == 0);

	// Steady state: no log at all.
	for (XrTime t = 101; t < 140; t++) {
		f = send_frame(t);
		report(t, WL, 1120, 456, f.targetRect);
	}
	CHECK(log_count("texture target state:") == 2 && log_count("WARN") == 0);

	// The runtime wove one pixel off the requested origin: ONE warning, however long it lasts.
	for (XrTime t = 140; t < 150; t++) {
		f = send_frame(t);
		report(t, WL, 1121, 456, f.targetRect);
	}
	CHECK(log_count("WARN") == 1 && log_count("at another ORIGIN") == 1);
	CHECK(dxr_prov_get_texture_target_state(&out) == 1 && out.mismatch == DXR_TT_MISMATCH_ORIGIN);
	// Back in agreement: said once, and only after it has held.
	f = send_frame(150);
	report(150, WL, 1120, 456, f.targetRect);
	CHECK(log_count("again") == 0);
	for (XrTime t = 151; t < 190; t++) {
		f = send_frame(t);
		report(t, WL, 1120, 456, f.targetRect);
	}
	CHECK(log_count("requested origin and rect again") == 1);
	CHECK(dxr_prov_get_texture_target_state(&out) == 1 && out.mismatch == 0);

	// Clamped to the shared texture: a state change (one line) and one WARN; the
	// smaller extent is the clamp, not a second complaint.
	XrRect2Di clamped = f.targetRect;
	clamped.extent.width = 1500;
	for (XrTime t = 190; t < 195; t++) {
		f = send_frame(t);
		report(t, WL | CL, 1120, 456, clamped);
	}
	CHECK(log_count("texture target state:") == 3 && log_count("CLAMPED to the shared texture") == 1);
	CHECK(log_count("for another RECT") == 0);
	CHECK(dxr_prov_get_texture_target_state(&out) == 1 && out.mismatch == DXR_TT_MISMATCH_CLAMPED);

	// A different rect altogether.
	XrRect2Di other = f.targetRect;
	other.offset.x += 5;
	f = send_frame(195);
	report(195, WL, 1120, 456, other);
	CHECK(log_count("for another RECT") == 1);

	// The display processor lost windowless phasing: the anchor carries the weave.
	f = send_frame(196);
	report(196, AF, 1120, 456, f.targetRect);
	CHECK(log_count("ANCHOR_FALLBACK") == 1);
	CHECK(dxr_prov_get_texture_target_state(&out) == 1 && out.flags == (uint32_t)AF);
	dxr_tt_on_session_stopped();

	// A session that ignores the binding (D3D12 until the runtime honours it there):
	// flags 0 and nothing reported, forever. Said once, never a warning, and the
	// frames keep carrying the target (it is inert on such a session).
	g_log.clear();
	CHECK(start_session(9, true, false));
	wb = editor_texture_binding();
	CHECK(dxr_tt_bind_session(&wb, 1) == 1);
	for (XrTime t = 300; t < 340; t++) {
		f = send_frame(t);
		CHECK(f.referenceWindow == kRoot);
		report(0, 0, 0, 0, XrRect2Di{});
	}
	CHECK(log_count("texture target state:") == 1 && log_count("NOT BOUND by the runtime") == 1);
	CHECK(log_count("WARN") == 0);
	CHECK(dxr_prov_get_texture_target_state(&out) == 1 && out.flags == 0 && out.reported == 0);
	CHECK(out.windowless_predicted == 0 && out.mismatch == 0);
	dxr_tt_on_session_stopped();
	W.root_on_screen_x = W.root_on_screen_y = 0;
}

static void
test_restart_and_refusal()
{
	g_log.clear();
	push_default_rect();

	// Session 1.
	CHECK(start_session(9, true, true));
	XrWin32WindowBindingCreateInfoDXR wb = editor_texture_binding();
	CHECK(dxr_tt_bind_session(&wb, 1) == 1);
	send_frame(10);

	// Undock: the session restarts in PRESENT mode (no shared texture) - no binding.
	dxr_tt_on_session_stopped();
	CHECK(start_session(9, true, true));
	XrWin32WindowBindingCreateInfoDXR present = {};
	present.type = XR_TYPE_WIN32_WINDOW_BINDING_CREATE_INFO_DXR;
	present.windowHandle = kAnchor;
	CHECK(dxr_tt_bind_session(&present, 0) == 0 && present.next == nullptr);
	check_frame_inert();

	// Re-dock: a third session, and NO new push from managed code. The binding and
	// the last rect are applied again.
	dxr_tt_on_session_stopped();
	CHECK(start_session(9, true, true));
	wb = editor_texture_binding();
	CHECK(dxr_tt_bind_session(&wb, 1) == 1);
	const auto *tb = find_in<XrWin32TextureTargetBindingDXR>(wb.next, XR_TYPE_WIN32_TEXTURE_TARGET_BINDING_DXR);
	CHECK(tb != nullptr && tb->referenceWindow == kRoot);
	CHECK(tb != nullptr && tb->initialTargetRect.offset.x == 1020 && tb->initialTargetRect.extent.width == 1728);
	XrWin32TextureTargetFrameDXR f = send_frame(20);
	CHECK(f.targetRect.offset.x == 1020 && f.targetRect.offset.y == 406);
	// The previous session's state did not leak into this one.
	DxrTextureTargetState out;
	CHECK(dxr_prov_get_texture_target_state(&out) == 1 && out.reported == 0 && out.flags == 0);

	// The runtime refuses the session with the binding chained: it comes off again,
	// the chain is as it was, and the retried session runs without it.
	dxr_tt_on_session_stopped();
	CHECK(start_session(9, true, true));
	wb = editor_texture_binding();
	int prior = 0;
	wb.next = &prior;
	CHECK(dxr_tt_bind_session(&wb, 1) == 1 && wb.next != &prior);
	CHECK(dxr_tt_unbind_after_create_failure(&wb) == 1);
	CHECK(wb.next == &prior && wb.windowHandle == kAnchor);
	CHECK(dxr_tt_unbind_after_create_failure(&wb) == 0); // nothing left to take off
	CHECK(dxr_prov_get_texture_target_state(&out) == 0 && out.why_not == DXR_TT_WHY_CREATE_REFUSED);
	check_frame_inert();
	dxr_tt_on_session_stopped();
}

#ifdef _WIN32
// The real resolver, against real (hidden) windows of this process: a borderless
// top-level window with a child "pane" in it. Windows only; no input, nothing shown.
// @p aware: create the windows per-monitor-v2 aware, as Unity's editor windows are
// (coordinates are physical px and exact); else in this test executable's own
// awareness (DPI-unaware: the host a DLL must not assume away).
static void
test_win32_resolver(bool aware)
{
	dxr_tt_set_resolver(nullptr); // the platform default
	HINSTANCE inst = GetModuleHandleW(nullptr);
	DPI_AWARENESS_CONTEXT prev =
	    aware ? SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) : nullptr;
	HWND top = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 300, 200, 800, 600, nullptr, nullptr, inst, nullptr);
	HWND pane = top ? CreateWindowExW(0, L"STATIC", L"", WS_CHILD, 40, 30, 400, 300, top, nullptr, inst, nullptr) : nullptr;
	if (prev) SetThreadDpiAwarenessContext(prev);
	CHECK(top != nullptr && pane != nullptr);
	if (top != nullptr && pane != nullptr) {
		dxr_prov_set_texture_target_rect(pane, 5, 7, 64, 48, DXR_TT_RECT_VALID);
		CHECK(start_session(9, true, true));
		XrWin32WindowBindingCreateInfoDXR wb = editor_texture_binding();
		CHECK(dxr_tt_bind_session(&wb, 1) == 1);
		const auto *tb = find_in<XrWin32TextureTargetBindingDXR>(wb.next, XR_TYPE_WIN32_TEXTURE_TARGET_BINDING_DXR);
		CHECK(tb != nullptr && tb->referenceWindow == (void *)top); // GA_ROOT of the pane
		CHECK(tb != nullptr && tb->initialTargetRect.extent.width == 64 && tb->initialTargetRect.extent.height == 48);
		if (aware && tb != nullptr) {
			// Physical px throughout: pane at (40,30) in the top-level client + (5,7).
			CHECK(tb->initialTargetRect.offset.x == 45 && tb->initialTargetRect.offset.y == 37);
		}

		// What the runtime does with the frame's rect (spec section 4):
		// ClientToScreen(referenceWindow, rect.offset) under per-monitor-v2. The origin
		// this side calls "requested" must be that very point, in either awareness.
		XrWin32TextureTargetFrameDXR f = send_frame(1);
		DPI_AWARENESS_CONTEXT p2 = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
		POINT woven = {(LONG)f.targetRect.offset.x, (LONG)f.targetRect.offset.y};
		ClientToScreen((HWND)f.referenceWindow, &woven);
		if (p2) SetThreadDpiAwarenessContext(p2);
		report(1, XR_WIN32_TEXTURE_TARGET_STATE_WINDOWLESS_PHASE_BIT_DXR, woven.x, woven.y, f.targetRect);
		DxrTextureTargetState out;
		CHECK(dxr_prov_get_texture_target_state(&out) == 1 && out.matched == 1 && out.mismatch == 0);
		CHECK(out.requested_x == woven.x && out.requested_y == woven.y);
		if (aware) CHECK(out.requested_x == 345 && out.requested_y == 237);

		// The pane's window is destroyed: the frame carries nothing.
		DestroyWindow(pane);
		int tail = 0;
		XrWin32TextureTargetFrameDXR g;
		CHECK(dxr_tt_chain_frame(&g, &tail, 2, 1) == &tail);
		dxr_tt_on_session_stopped();

		// A window of ANOTHER process is never a reference window (the runtime would
		// fail xrCreateSession on it).
		HWND foreign = GetShellWindow();
		DWORD pid = 0;
		if (foreign != nullptr) GetWindowThreadProcessId(foreign, &pid);
		if (foreign != nullptr && pid != GetCurrentProcessId()) {
			dxr_prov_set_texture_target_rect(foreign, 0, 0, 64, 48, DXR_TT_RECT_VALID);
			CHECK(start_session(9, true, true));
			wb = editor_texture_binding();
			CHECK(dxr_tt_bind_session(&wb, 1) == 0 && wb.next == nullptr);
			CHECK(dxr_prov_get_texture_target_state(&out) == 0 && out.why_not == DXR_TT_WHY_BAD_REFERENCE);
			dxr_tt_on_session_stopped();
		}
	}
	if (top != nullptr) DestroyWindow(top);
	dxr_prov_set_texture_target_rect(nullptr, 0, 0, 0, 0, 0);
}
#endif

int
main()
{
	dxr_tt_set_resolver(fake_resolve);
	set_env(nullptr);
	test_gating();
	test_chaining_and_frames();
	test_state();
	test_restart_and_refusal();
#ifdef _WIN32
	test_win32_resolver(true);
	test_win32_resolver(false);
#endif
	if (g_fail) {
		fprintf(stderr, "texture_target_test: %d of %d checks FAILED\n", g_fail, g_checks);
		return 1;
	}
	printf("texture_target_test: %d checks passed\n", g_checks);
	return 0;
}
