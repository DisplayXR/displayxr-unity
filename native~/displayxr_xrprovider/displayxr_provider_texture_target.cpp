// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// XR_DXR_win32_window_binding SPEC_VERSION 9 - texture target binding - for the
// display provider's editor texture mode. See displayxr_provider_texture_target.h
// for the contract, the gates, the DPI rule and the threading.
//
// The logic is platform-neutral (the fake-runtime test runs it on any host); only
// the default resolver touches Win32, behind _WIN32.

#include "displayxr_provider_texture_target.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mutex>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

// The C# mirror (DisplayXRProviderNative.DxrTextureTargetState) is 15 ints, in order.
static_assert(sizeof(DxrTextureTargetState) == 15 * sizeof(int32_t), "DxrTextureTargetState layout is mirrored in C#");

namespace {

//! The spec version that introduced the texture target structs. Deliberately not
//! spelled XR_DXR_*_SPEC_VERSION: those constants are the plug-in's MINIMUM-REQUIRED
//! claims, and this one is an optional capability gate.
const uint32_t kTextureTargetSpec = 9;

//! Frames a mismatch must stay away before it is reported as gone (a window being
//! dragged can disagree for a frame at a time; do not log each one).
const int kAgreeFramesToClear = 30;

void
log_nop(const char *, ...)
{
}

struct PushedRect
{
	void *pane = nullptr;
	int32_t x = 0, y = 0, w = 0, h = 0;
	uint32_t flags = 0;
	bool valid = false;
};

struct SentFrame
{
	bool used = false;
	XrTime time = 0;
	void *reference = nullptr;
	XrRect2Di rect = {};
	int32_t screen_x = 0, screen_y = 0;
};

struct Session
{
	DxrTtLogFn log = log_nop;
	bool started = false; // between on_session_starting and on_session_stopped
	bool opt_out = false;
	uint32_t spec = 0;

	XrSystemWin32TextureTargetPropertiesDXR sys_props = {};
	bool sys_chained = false;
	bool sys_supports = false;
	bool sys_windowless = false;

	XrWin32TextureTargetBindingDXR binding = {};
	bool chained = false;
	int why_not = DXR_TT_WHY_NO_SESSION;

	// One resolve per frame, shared by that frame's locates and its xrEndFrame.
	bool cache_have = false;
	bool cache_ok = false;
	XrTime cache_time = 0;
	SentFrame cache = {};
	bool skip_logged = false; // "frame target not sent" episode

	SentFrame history[8];
	uint32_t history_next = 0;

	bool have_state = false;
	uint64_t state_flags = 0;
	bool reported = false;
	bool state_measured = false; // the rect source the last state line was logged with
	XrWin32TextureTargetStateDXR last = {};
	bool matched = false;
	int32_t requested_x = 0, requested_y = 0;
	int mismatch = 0;
	int agree_frames = 0;
	bool end_failure_logged = false;
};

std::mutex s_mx;
PushedRect s_rect;                  // survives session restarts
DxrTtResolveFn s_resolver = nullptr; // tests; nullptr = platform default
Session s;

#ifdef _WIN32
int
resolve_win32(void *pane_hwnd, int32_t x, int32_t y, DxrTtResolved *out)
{
	// Every read in true physical px, whatever the host process's DPI awareness
	// (the editor's is not ours to assume - see CLAUDE.md, "DPI space").
	DPI_AWARENESS_CONTEXT prev = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
	int ok = 0;
	if (pane_hwnd == nullptr) {
		out->reference_window = nullptr;
		out->x = out->screen_x = x;
		out->y = out->screen_y = y;
		ok = 1;
	} else {
		HWND pane = (HWND)pane_hwnd;
		HWND root = IsWindow(pane) ? GetAncestor(pane, GA_ROOT) : nullptr;
		DWORD pid = 0;
		if (root != nullptr && IsWindow(root)) GetWindowThreadProcessId(root, &pid);
		// The runtime refuses a reference window that is not a live top-level window
		// of the calling process (XR_ERROR_VALIDATION_FAILURE), so never send one.
		if (root != nullptr && pid == GetCurrentProcessId()) {
			POINT pt = {(LONG)x, (LONG)y};
			if (ClientToScreen(pane, &pt) && ScreenToClient(root, &pt)) {
				out->reference_window = (void *)root;
				out->x = (int32_t)pt.x;
				out->y = (int32_t)pt.y;
				// The desktop origin this rect stands for is DEFINED as what the runtime
				// will compute from it: ClientToScreen(reference, offset) under PMv2.
				// For a per-monitor-aware host (Unity's editor) that is the point we
				// started from; a DPI-virtualized host's window coordinates are quantized
				// to its logical pixels, so there the round trip is the honest value
				// (measured: 175 % unaware host, (599,410) -> client (73,59) -> (597,408)).
				if (ClientToScreen(root, &pt)) {
					out->screen_x = (int32_t)pt.x;
					out->screen_y = (int32_t)pt.y;
					ok = 1;
				}
			}
		}
	}
	if (prev) SetThreadDpiAwarenessContext(prev);
	return ok;
}
#endif

int
resolve(void *pane_hwnd, int32_t x, int32_t y, DxrTtResolved *out)
{
	if (s_resolver != nullptr) return s_resolver(pane_hwnd, x, y, out);
#ifdef _WIN32
	return resolve_win32(pane_hwnd, x, y, out);
#else
	(void)pane_hwnd; (void)x; (void)y; (void)out;
	return 0; // the texture target is a Win32 binding
#endif
}

//! Resolve the pushed rect now. Caller holds s_mx.
bool
resolve_pushed(SentFrame *out)
{
	if (!s_rect.valid) return false;
	DxrTtResolved r = {};
	if (!resolve(s_rect.pane, s_rect.x, s_rect.y, &r)) return false;
	out->used = true;
	out->reference = r.reference_window;
	out->rect.offset.x = r.x;
	out->rect.offset.y = r.y;
	out->rect.extent.width = s_rect.w;
	out->rect.extent.height = s_rect.h;
	out->screen_x = r.screen_x;
	out->screen_y = r.screen_y;
	return true;
}

const char *
phase_source(uint64_t flags)
{
	if (flags & XR_WIN32_TEXTURE_TARGET_STATE_WINDOWLESS_PHASE_BIT_DXR) return "WINDOWLESS_PHASE (the target rect)";
	if (flags & XR_WIN32_TEXTURE_TARGET_STATE_ANCHOR_FALLBACK_BIT_DXR) return "ANCHOR_FALLBACK (the anchor window)";
	return "NOT BOUND by the runtime (the anchor window, as before spec 9)";
}

//! Where the pushed rect came from: the on-screen calibration or the computed placement.
const char *
rect_source(uint32_t rect_flags)
{
	return (rect_flags & DXR_TT_RECT_MEASURED) ? "measured" : "computed";
}

} // namespace

extern "C" {

void
dxr_prov_set_texture_target_rect(void *pane_hwnd, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t flags)
{
	std::lock_guard<std::mutex> lk(s_mx);
	const bool valid = (flags & DXR_TT_RECT_VALID) != 0 && w > 0 && h > 0;
	if (valid != s_rect.valid || (valid && pane_hwnd != s_rect.pane)) {
		if (valid)
			s.log("[DisplayXR-PROV] texture target: rect pushed - pane %p (%d,%d %dx%d) [%s]\n", pane_hwnd, (int)x,
			      (int)y, (int)w, (int)h, rect_source(flags));
		else
			s.log("[DisplayXR-PROV] texture target: rect cleared\n");
	} else if (valid && ((flags ^ s_rect.flags) & DXR_TT_RECT_MEASURED) != 0) {
		// The same pane, another source: the calibration took over, or was dropped.
		s.log("[DisplayXR-PROV] texture target: rect now %s - pane %p (%d,%d %dx%d), was (%d,%d %dx%d) [%s]\n",
		      rect_source(flags), pane_hwnd, (int)x, (int)y, (int)w, (int)h, (int)s_rect.x, (int)s_rect.y,
		      (int)s_rect.w, (int)s_rect.h, rect_source(s_rect.flags));
	}
	s_rect.valid = valid;
	s_rect.pane = valid ? pane_hwnd : nullptr;
	s_rect.x = valid ? x : 0;
	s_rect.y = valid ? y : 0;
	s_rect.w = valid ? w : 0;
	s_rect.h = valid ? h : 0;
	s_rect.flags = valid ? flags : 0;
}

int
dxr_prov_get_texture_target_state(DxrTextureTargetState *out)
{
	std::lock_guard<std::mutex> lk(s_mx);
	if (out != nullptr) {
		memset(out, 0, sizeof(*out));
		out->chained = s.chained ? 1 : 0;
		out->why_not = s.chained ? DXR_TT_WHY_CHAINED : s.why_not;
		out->reported = s.reported ? 1 : 0;
		out->flags = (uint32_t)s.state_flags;
		out->woven_x = s.last.wovenScreenOrigin.x;
		out->woven_y = s.last.wovenScreenOrigin.y;
		out->requested_x = s.requested_x;
		out->requested_y = s.requested_y;
		out->matched = s.matched ? 1 : 0;
		out->mismatch = s.mismatch;
		out->rect_x = s.last.wovenTargetRect.offset.x;
		out->rect_y = s.last.wovenTargetRect.offset.y;
		out->rect_w = s.last.wovenTargetRect.extent.width;
		out->rect_h = s.last.wovenTargetRect.extent.height;
		out->windowless_predicted = s.sys_windowless ? 1 : 0;
	}
	return s.chained ? 1 : 0;
}

void
dxr_tt_set_resolver(DxrTtResolveFn fn)
{
	std::lock_guard<std::mutex> lk(s_mx);
	s_resolver = fn;
}

void
dxr_tt_on_session_starting(DxrTtLogFn log)
{
	std::lock_guard<std::mutex> lk(s_mx);
	s = Session();
	s.log = log != nullptr ? log : log_nop;
	s.started = true;
	// getenv, not GetEnvironmentVariable: C# sets it through dxr_prov_set_env
	// (_putenv_s), and a launcher's environment reaches the CRT table at startup.
	const char *e = getenv("DISPLAYXR_PROV_TEXTURE_TARGET");
	s.opt_out = (e != nullptr && e[0] == '0' && e[1] == '\0');
}

void
dxr_tt_on_extension_version(uint32_t advertised_spec_version)
{
	std::lock_guard<std::mutex> lk(s_mx);
	s.spec = advertised_spec_version;
}

void *
dxr_tt_chain_system_properties(void *next)
{
	std::lock_guard<std::mutex> lk(s_mx);
	s.sys_chained = false;
	if (!s.started || s.opt_out || s.spec < kTextureTargetSpec) return next;
	s.sys_props = {};
	s.sys_props.type = XR_TYPE_SYSTEM_WIN32_TEXTURE_TARGET_PROPERTIES_DXR;
	s.sys_props.next = next;
	s.sys_chained = true;
	return &s.sys_props;
}

void
dxr_tt_on_system_properties(int succeeded)
{
	std::lock_guard<std::mutex> lk(s_mx);
	if (!s.sys_chained) return;
	s.sys_supports = succeeded && s.sys_props.supportsTextureTarget == XR_TRUE;
	s.sys_windowless = succeeded && s.sys_props.windowlessPhase == XR_TRUE;
	s.log("[DisplayXR-PROV] texture target: runtime spec v%u, supportsTextureTarget=%d windowlessPhase=%d "
	      "(a prediction; the per-frame state is the authority)\n",
	      (unsigned)s.spec, s.sys_supports ? 1 : 0, s.sys_windowless ? 1 : 0);
}

int
dxr_tt_bind_session(XrWin32WindowBindingCreateInfoDXR *wb, int eligible)
{
	std::lock_guard<std::mutex> lk(s_mx);
	s.chained = false;
	// The binding only describes a texture target: without a shared texture (or with
	// the deprecated readback callback) the runtime fails xrCreateSession on it.
	if (!s.started || wb == nullptr || !eligible || wb->sharedTextureHandle == nullptr ||
	    wb->readbackCallback != nullptr) {
		s.why_not = DXR_TT_WHY_NOT_ELIGIBLE; // silent: every player and present-mode session lands here
		return 0;
	}
	if (s.opt_out) {
		s.why_not = DXR_TT_WHY_OPT_OUT;
		s.log("[DisplayXR-PROV] texture target: OFF (DISPLAYXR_PROV_TEXTURE_TARGET=0) - anchor window only\n");
		return 0;
	}
	if (s.spec < kTextureTargetSpec) {
		s.why_not = DXR_TT_WHY_RUNTIME_SPEC;
		s.log("[DisplayXR-PROV] texture target: runtime advertises XR_DXR_win32_window_binding v%u (needs %u) - "
		      "anchor window only\n",
		      (unsigned)s.spec, (unsigned)kTextureTargetSpec);
		return 0;
	}
	if (!s.sys_supports) {
		s.why_not = DXR_TT_WHY_NOT_SUPPORTED;
		s.log("[DisplayXR-PROV] texture target: the system does not support it (supportsTextureTarget=0) - "
		      "anchor window only\n");
		return 0;
	}
	SentFrame f = {};
	if (!s_rect.valid) {
		s.why_not = DXR_TT_WHY_NO_RECT;
		s.log("[DisplayXR-PROV] texture target: no rect pushed before the session started - anchor window only "
		      "for this session\n");
		return 0;
	}
	if (!resolve_pushed(&f)) {
		s.why_not = DXR_TT_WHY_BAD_REFERENCE;
		s.log("[DisplayXR-PROV] texture target: pane %p has no live top-level window in this process - anchor "
		      "window only for this session\n",
		      s_rect.pane);
		return 0;
	}
	s.binding = {};
	s.binding.type = XR_TYPE_WIN32_TEXTURE_TARGET_BINDING_DXR;
	s.binding.next = wb->next;
	s.binding.referenceWindow = f.reference;
	s.binding.initialTargetRect = f.rect;
	wb->next = &s.binding;
	s.chained = true;
	s.why_not = DXR_TT_WHY_CHAINED;
	s.log("[DisplayXR-PROV] texture target: binding CHAINED - reference window %p, initial rect (%d,%d %dx%d) "
	      "in its client px = desktop (%d,%d); pane %p +(%d,%d) [%s]; anchor window %p kept as the fallback\n",
	      f.reference, (int)f.rect.offset.x, (int)f.rect.offset.y, (int)f.rect.extent.width,
	      (int)f.rect.extent.height, (int)f.screen_x, (int)f.screen_y, s_rect.pane, (int)s_rect.x, (int)s_rect.y,
	      rect_source(s_rect.flags), wb->windowHandle);
	return 1;
}

int
dxr_tt_unbind_after_create_failure(XrWin32WindowBindingCreateInfoDXR *wb)
{
	std::lock_guard<std::mutex> lk(s_mx);
	if (!s.chained || wb == nullptr || wb->next != &s.binding) return 0;
	wb->next = s.binding.next;
	s.chained = false;
	s.why_not = DXR_TT_WHY_CREATE_REFUSED;
	s.log("[DisplayXR-PROV] texture target: WARN xrCreateSession failed with the binding chained - retrying "
	      "without it (anchor window only)\n");
	return 1;
}

const void *
dxr_tt_chain_frame(XrWin32TextureTargetFrameDXR *storage, const void *next, XrTime display_time, int at_frame_end)
{
	std::lock_guard<std::mutex> lk(s_mx);
	if (!s.chained || storage == nullptr) return next;
	if (!s.cache_have || s.cache_time != display_time) {
		s.cache = {};
		s.cache_ok = resolve_pushed(&s.cache);
		s.cache.time = display_time;
		s.cache_time = display_time;
		s.cache_have = true;
	}
	if (!s.cache_ok) {
		// Never send a rect the runtime would refuse (a dead reference window or an
		// empty rect fails the whole xrEndFrame). Omitted, it reuses the last one.
		if (!s.skip_logged) {
			s.skip_logged = true;
			s.log("[DisplayXR-PROV] texture target: no valid target this frame (%s) - not sent, the runtime "
			      "reuses the last one\n",
			      s_rect.valid ? "the pane's window is gone" : "no rect pushed");
		}
		return next;
	}
	if (s.skip_logged) {
		s.skip_logged = false;
		s.log("[DisplayXR-PROV] texture target: target valid again - reference window %p rect (%d,%d %dx%d)\n",
		      s.cache.reference, (int)s.cache.rect.offset.x, (int)s.cache.rect.offset.y,
		      (int)s.cache.rect.extent.width, (int)s.cache.rect.extent.height);
	}
	*storage = {};
	storage->type = XR_TYPE_WIN32_TEXTURE_TARGET_FRAME_DXR;
	storage->next = next;
	storage->referenceWindow = s.cache.reference;
	storage->targetRect = s.cache.rect;
	storage->flags = 0;
	if (at_frame_end) {
		s.history[s.history_next] = s.cache;
		s.history_next = (s.history_next + 1) % (uint32_t)(sizeof(s.history) / sizeof(s.history[0]));
	}
	return storage;
}

void
dxr_tt_on_frame_end_result(XrResult result)
{
	std::lock_guard<std::mutex> lk(s_mx);
	if (!s.chained) return;
	if (XR_FAILED(result) && !s.end_failure_logged) {
		s.end_failure_logged = true;
		s.log("[DisplayXR-PROV] texture target: WARN xrEndFrame failed (%d) on a frame carrying the target "
		      "(reference window destroyed mid-frame?); set DISPLAYXR_PROV_TEXTURE_TARGET=0 if it persists\n",
		      (int)result);
	} else if (XR_SUCCEEDED(result)) {
		s.end_failure_logged = false;
	}
}

void *
dxr_tt_chain_frame_state(XrWin32TextureTargetStateDXR *storage, void *next)
{
	std::lock_guard<std::mutex> lk(s_mx);
	if (!s.chained || storage == nullptr) return next;
	*storage = {};
	storage->type = XR_TYPE_WIN32_TEXTURE_TARGET_STATE_DXR;
	storage->next = next;
	return storage;
}

void
dxr_tt_on_frame_state(const XrWin32TextureTargetStateDXR *st)
{
	std::lock_guard<std::mutex> lk(s_mx);
	if (!s.chained || st == nullptr) return;
	const bool reported = st->wovenTargetRect.extent.width > 0 && st->wovenTargetRect.extent.height > 0;
	// The rect source (measured / computed) is part of the logged state: a calibration
	// that takes over mid-session logs the line again, with the origin it moved to.
	const bool measured = (s_rect.flags & DXR_TT_RECT_MEASURED) != 0;
	const bool changed =
	    !s.have_state || st->flags != s.state_flags || reported != s.reported || measured != s.state_measured;
	s.have_state = true;
	s.state_measured = measured;
	s.state_flags = st->flags;
	s.reported = reported;
	s.last = *st;

	// The frame the report is about, as this side sent it.
	const SentFrame *sent = nullptr;
	if (reported) {
		for (const SentFrame &f : s.history)
			if (f.used && f.time == st->wovenDisplayTime) { sent = &f; break; }
	}
	s.matched = sent != nullptr;
	int mismatch = 0;
	if (st->flags & XR_WIN32_TEXTURE_TARGET_STATE_CLAMPED_BIT_DXR) mismatch |= DXR_TT_MISMATCH_CLAMPED;
	if (sent != nullptr) {
		s.requested_x = sent->screen_x;
		s.requested_y = sent->screen_y;
		if (st->wovenScreenOrigin.x != sent->screen_x || st->wovenScreenOrigin.y != sent->screen_y)
			mismatch |= DXR_TT_MISMATCH_ORIGIN;
		const bool clamped = (mismatch & DXR_TT_MISMATCH_CLAMPED) != 0;
		if (st->wovenTargetRect.offset.x != sent->rect.offset.x ||
		    st->wovenTargetRect.offset.y != sent->rect.offset.y ||
		    (!clamped && (st->wovenTargetRect.extent.width != sent->rect.extent.width ||
		                  st->wovenTargetRect.extent.height != sent->rect.extent.height)))
			mismatch |= DXR_TT_MISMATCH_RECT;
	}

	if (changed) {
		char extra[128];
		snprintf(extra, sizeof(extra), "%s%s%s, target rect %s",
		         (st->flags & XR_WIN32_TEXTURE_TARGET_STATE_SEGMENTED_BIT_DXR) ? " SEGMENTED" : "",
		         (st->flags & XR_WIN32_TEXTURE_TARGET_STATE_FLAT_SEGMENT_BIT_DXR) ? " FLAT_SEGMENT" : "",
		         (st->flags & XR_WIN32_TEXTURE_TARGET_STATE_CLAMPED_BIT_DXR) ? " CLAMPED" : "",
		         rect_source(s_rect.flags));
		if (!reported) {
			s.log("[DisplayXR-PROV] texture target state: phase source = %s%s (flags 0x%x); no woven frame "
			      "reported yet\n",
			      phase_source(st->flags), extra, (unsigned)st->flags);
		} else if (sent != nullptr) {
			s.log("[DisplayXR-PROV] texture target state: phase source = %s%s (flags 0x%x); woven origin (%d,%d) "
			      "rect (%d,%d %dx%d) vs requested origin (%d,%d) rect (%d,%d %dx%d)\n",
			      phase_source(st->flags), extra, (unsigned)st->flags, (int)st->wovenScreenOrigin.x,
			      (int)st->wovenScreenOrigin.y, (int)st->wovenTargetRect.offset.x,
			      (int)st->wovenTargetRect.offset.y, (int)st->wovenTargetRect.extent.width,
			      (int)st->wovenTargetRect.extent.height, (int)sent->screen_x, (int)sent->screen_y,
			      (int)sent->rect.offset.x, (int)sent->rect.offset.y, (int)sent->rect.extent.width,
			      (int)sent->rect.extent.height);
		} else {
			s.log("[DisplayXR-PROV] texture target state: phase source = %s%s (flags 0x%x); woven origin (%d,%d) "
			      "rect (%d,%d %dx%d) (requested: frame not in the sent history)\n",
			      phase_source(st->flags), extra, (unsigned)st->flags, (int)st->wovenScreenOrigin.x,
			      (int)st->wovenScreenOrigin.y, (int)st->wovenTargetRect.offset.x,
			      (int)st->wovenTargetRect.offset.y, (int)st->wovenTargetRect.extent.width,
			      (int)st->wovenTargetRect.extent.height);
		}
	}

	// One WARN per change of what disagrees; "agrees again" only once it has held.
	if (mismatch != 0) {
		s.agree_frames = 0;
		if (mismatch != s.mismatch) {
			s.mismatch = mismatch;
			char req[128] = "";
			if (sent != nullptr)
				snprintf(req, sizeof(req), ", requested origin (%d,%d) rect (%d,%d %dx%d)", (int)sent->screen_x,
				         (int)sent->screen_y, (int)sent->rect.offset.x, (int)sent->rect.offset.y,
				         (int)sent->rect.extent.width, (int)sent->rect.extent.height);
			s.log("[DisplayXR-PROV] texture target: WARN the runtime wove %s%s%s- woven origin (%d,%d) rect "
			      "(%d,%d %dx%d)%s\n",
			      (mismatch & DXR_TT_MISMATCH_ORIGIN) ? "at another ORIGIN " : "",
			      (mismatch & DXR_TT_MISMATCH_RECT) ? "for another RECT " : "",
			      (mismatch & DXR_TT_MISMATCH_CLAMPED) ? "CLAMPED to the shared texture " : "",
			      (int)st->wovenScreenOrigin.x, (int)st->wovenScreenOrigin.y, (int)st->wovenTargetRect.offset.x,
			      (int)st->wovenTargetRect.offset.y, (int)st->wovenTargetRect.extent.width,
			      (int)st->wovenTargetRect.extent.height, req);
		}
	} else if (s.mismatch != 0 && reported && ++s.agree_frames >= kAgreeFramesToClear) {
		s.mismatch = 0;
		s.agree_frames = 0;
		s.log("[DisplayXR-PROV] texture target: the runtime weaves at the requested origin and rect again\n");
	}
}

void
dxr_tt_on_session_stopped(void)
{
	std::lock_guard<std::mutex> lk(s_mx);
	DxrTtLogFn log = s.log;
	s = Session();
	s.log = log; // the setter keeps logging between sessions
}

} // extern "C"
