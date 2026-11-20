// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Unit test of the render scale (displayxr_provider_render_scale.cpp, #475) against
// a fake runtime. What it pins:
//   - SIZE: the size a swapchain is created at for a given scale - identity at
//     exactly 1.0 (bit for bit, no limit applied), round(base x scale) otherwise,
//     never below 1 px, reduced UNIFORMLY to the runtime's maximum image size and
//     never below the unscaled base.
//   - LIMITS: read from xrEnumerateViewConfigurationViews (maxImageRect) and the
//     system's maxSwapchainImage; a runtime that answers neither limits nothing.
//   - RANGE: 0.25..2.0, quantised to 1/100; NaN / inf / <= 0 mean "not set".
//   - PRECEDENCE: environment > app > Unity's hint > 1.0, and clearing one falls
//     back to the next.
//   - NO THRASH: a changed request reallocates only after it has held still (and
//     then after the session's own stable-frames debounce); an animated scale
//     reallocates exactly once, when it stops; the first hint of a session is taken
//     without the settle wait.
//   - RESTART: the app's value and Unity's last hint survive a session stop/start,
//     so the restarted session creates its swapchain at the scaled size at once.
// The "session" here is a small model of the provider's own sequence (create at
// session start, per frame: hint -> tick -> target size -> debounce -> recreate),
// driving a fake xrCreateSwapchain. No Unity, no graphics API, no real runtime.
//
//   c++ -std=c++17 -I native~/displayxr_xrprovider -I <openxr include>
//       native~/tests/render_scale_test.cpp
//       native~/displayxr_xrprovider/displayxr_provider_render_scale.cpp

#include "displayxr_provider_render_scale.h"
#include "displayxr_mirror_canvas.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
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

static void
set_env(const char *name, const char *value)
{
#ifdef _WIN32
	_putenv_s(name, value ? value : "");
#else
	if (value) setenv(name, value, 1);
	else unsetenv(name);
#endif
}

// The fake runtime: what it reports as its limits, and every swapchain it was asked for.
struct FakeRuntime
{
	bool has_view_enumeration = true;
	uint32_t view_max_w = 3840, view_max_h = 2160;
	uint32_t sys_max_w = 16384, sys_max_h = 16384;
	std::vector<XrSwapchainCreateInfo> created;
};
static FakeRuntime g_rt;
static XrInstance const kInstance = (XrInstance)(uintptr_t)0x1001;
static XrSystemId const kSystem = (XrSystemId)7;

static XRAPI_ATTR XrResult XRAPI_CALL
fake_enumerate_views(XrInstance instance, XrSystemId system_id, XrViewConfigurationType type, uint32_t capacity,
                     uint32_t *count, XrViewConfigurationView *views)
{
	if (instance != kInstance || system_id != kSystem) return XR_ERROR_HANDLE_INVALID;
	if (type != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
	*count = 2;
	if (capacity == 0) return XR_SUCCESS;
	if (capacity < 2) return XR_ERROR_SIZE_INSUFFICIENT;
	for (uint32_t i = 0; i < 2; i++) {
		if (views[i].type != XR_TYPE_VIEW_CONFIGURATION_VIEW) return XR_ERROR_VALIDATION_FAILURE;
		views[i].recommendedImageRectWidth = 1920;
		views[i].recommendedImageRectHeight = 1080;
		// The right eye reports a looser limit: the tighter one must win.
		views[i].maxImageRectWidth = g_rt.view_max_w + (i == 1 ? 100 : 0);
		views[i].maxImageRectHeight = g_rt.view_max_h + (i == 1 ? 100 : 0);
	}
	return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL
fake_gipa(XrInstance instance, const char *name, PFN_xrVoidFunction *fn)
{
	*fn = nullptr;
	if (instance != kInstance) return XR_ERROR_HANDLE_INVALID;
	if (g_rt.has_view_enumeration && strcmp(name, "xrEnumerateViewConfigurationViews") == 0) {
		*fn = (PFN_xrVoidFunction)fake_enumerate_views;
		return XR_SUCCESS;
	}
	return XR_ERROR_FUNCTION_UNSUPPORTED;
}

static void
fake_create_swapchain(uint32_t w, uint32_t h)
{
	XrSwapchainCreateInfo ci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
	ci.width = w;
	ci.height = h;
	ci.arraySize = 2;
	ci.sampleCount = 1;
	g_rt.created.push_back(ci);
}

// ---------------------------------------------------------------- session model
//
// Mirrors displayxr_provider_session.cpp: ps_create_swapchain (base -> scaled size at
// dxr_rs_target, recorded scale), ps_reconcile_primary (PS_REALLOC_DEBOUNCE stable
// frames), and the per-frame render rect taken at the ALLOCATED scale.

static const uint32_t kDebounce = 6; // PS_REALLOC_DEBOUNCE
static const uint64_t kFrameNs = 16666667ull;

struct Model
{
	uint32_t base_w = 1920, base_h = 1080; // window x view scale
	uint32_t sc_w = 0, sc_h = 0;
	float sc_scale = 0.0f;
	uint32_t pend_w = 0, pend_h = 0, stable = 0;
	uint64_t now = 1000000000ull;
	uint32_t render_w = 0, render_h = 0;
	bool render_matches_swapchain = true; // never a sub-rect while only the scale changes

	void create()
	{
		uint32_t w = base_w, h = base_h;
		const float s = dxr_rs_target();
		dxr_rs_scale_extent(s, base_w, base_h, &w, &h);
		fake_create_swapchain(w, h);
		sc_w = w; sc_h = h; sc_scale = s;
		dxr_rs_note_primary_allocated(s, base_w, base_h, w, h);
	}
	void start()
	{
		dxr_rs_on_session_starting(test_log);
		dxr_rs_on_system(kInstance, kSystem, fake_gipa, g_rt.sys_max_w, g_rt.sys_max_h);
		sc_w = sc_h = 0; pend_w = pend_h = stable = 0;
		create();
	}
	void stop() { dxr_rs_on_session_stopped(); }

	// One PopulateNextFrameDesc. @p hint < 0: Unity delivers no hint this frame.
	void frame(float hint)
	{
		now += kFrameNs;
		if (hint >= 0.0f) dxr_rs_set_unity_hint(hint);
		dxr_rs_tick(now);

		uint32_t tw = 0, th = 0;
		dxr_rs_scale_extent(dxr_rs_target(), base_w, base_h, &tw, &th);
		if (tw == sc_w && th == sc_h) {
			pend_w = pend_h = stable = 0;
		} else if (tw == pend_w && th == pend_h) {
			if (++stable >= kDebounce) { pend_w = pend_h = stable = 0; create(); }
		} else {
			pend_w = tw; pend_h = th; stable = 0;
		}

		dxr_rs_scale_extent(sc_scale > 0.0f ? sc_scale : 1.0f, base_w, base_h, &render_w, &render_h);
		if (render_w > sc_w) render_w = sc_w;
		if (render_h > sc_h) render_h = sc_h;
		if (render_w != sc_w || render_h != sc_h) render_matches_swapchain = false;
	}
	void frames(int n, float hint)
	{
		for (int i = 0; i < n; i++) frame(hint);
	}
};

static size_t
creates(void)
{
	return g_rt.created.size();
}
static bool
last_create_is(uint32_t w, uint32_t h)
{
	return !g_rt.created.empty() && g_rt.created.back().width == w && g_rt.created.back().height == h;
}

// ---------------------------------------------------------------- tests

static void
test_sanitize(void)
{
	int c = -1;
	CHECK(dxr_rs_sanitize(NAN, &c) == 0.0f && c == 0);
	CHECK(dxr_rs_sanitize(INFINITY, &c) == 0.0f);
	CHECK(dxr_rs_sanitize(0.0f, &c) == 0.0f);
	CHECK(dxr_rs_sanitize(-1.0f, &c) == 0.0f);
	CHECK(dxr_rs_sanitize(1.0f, &c) == 1.0f && c == 0);
	CHECK(dxr_rs_sanitize(0.5f, &c) == 0.5f && c == 0);
	CHECK(dxr_rs_sanitize(0.1f, &c) == DXR_RS_MIN && c == 1);
	CHECK(dxr_rs_sanitize(3.0f, &c) == DXR_RS_MAX && c == 1);
	CHECK(dxr_rs_sanitize(DXR_RS_MIN, &c) == DXR_RS_MIN && c == 0);
	CHECK(dxr_rs_sanitize(DXR_RS_MAX, &c) == DXR_RS_MAX && c == 0);
	// Quantised to 1/100: float noise around 1.0 takes the identity path.
	CHECK(dxr_rs_sanitize(1.004f, &c) == 1.0f);
	CHECK(dxr_rs_sanitize(0.996f, &c) == 1.0f);
	CHECK(fabsf(dxr_rs_sanitize(0.504f, &c) - 0.5f) < 1e-6f);
	CHECK(fabsf(dxr_rs_sanitize(0.757f, nullptr) - 0.76f) < 1e-6f);
}

static void
test_extent(void)
{
	g_rt = FakeRuntime();
	dxr_rs_on_session_starting(test_log);
	dxr_rs_on_system(kInstance, kSystem, fake_gipa, g_rt.sys_max_w, g_rt.sys_max_h);
	CHECK(log_count("runtime maximum image 3840x2160 (view configuration 3840x2160, system 16384x16384") == 1);

	uint32_t w = 0, h = 0;
	// Identity at exactly 1.0 - odd sizes, and sizes beyond the runtime's maximum.
	CHECK(dxr_rs_scale_extent(1.0f, 1921, 1079, &w, &h) == 0 && w == 1921 && h == 1079);
	CHECK(dxr_rs_scale_extent(1.0f, 5000, 3000, &w, &h) == 0 && w == 5000 && h == 3000);
	// A zero base stays zero (the session treats it as "no size yet").
	CHECK(dxr_rs_scale_extent(0.5f, 0, 1080, &w, &h) == 0 && w == 0 && h == 0);
	CHECK(dxr_rs_scale_extent(2.0f, 1920, 0, &w, &h) == 0 && w == 0 && h == 0);
	// A non-positive scale is read as 1.
	CHECK(dxr_rs_scale_extent(0.0f, 1920, 1080, &w, &h) == 0 && w == 1920 && h == 1080);

	CHECK(dxr_rs_scale_extent(0.5f, 1920, 1080, &w, &h) == 0 && w == 960 && h == 540);
	CHECK(dxr_rs_scale_extent(1.5f, 1920, 1080, &w, &h) == 0 && w == 2880 && h == 1620);
	CHECK(dxr_rs_scale_extent(0.25f, 1920, 1080, &w, &h) == 0 && w == 480 && h == 270);
	// Rounded to nearest, not floored.
	CHECK(dxr_rs_scale_extent(0.5f, 1921, 1079, &w, &h) == 0 && w == 961 && h == 540);
	CHECK(dxr_rs_scale_extent(0.33f, 1001, 1000, &w, &h) == 0 && w == 330 && h == 330);
	// Never below one pixel.
	CHECK(dxr_rs_scale_extent(0.25f, 1, 1, &w, &h) == 0 && w == 1 && h == 1);

	// Exactly at the maximum: allowed, not reported as limited.
	CHECK(dxr_rs_scale_extent(2.0f, 1920, 1080, &w, &h) == 0 && w == 3840 && h == 2160);
	// Over it: reduced uniformly (aspect kept), reported.
	CHECK(dxr_rs_scale_extent(1.5f, 3000, 2000, &w, &h) == 1 && w == 3240 && h == 2160);
	CHECK(fabs((double)w / h - 1.5) < 1e-3);
	// A reduction that would go below the unscaled base stops at the base.
	CHECK(dxr_rs_scale_extent(2.0f, 1920, 2160, &w, &h) == 1 && w == 1920 && h == 2160);
	CHECK(dxr_rs_scale_extent(1.5f, 5000, 3000, &w, &h) == 1 && w == 5000 && h == 3000);
	// The limit never applies below 1.
	CHECK(dxr_rs_scale_extent(0.9f, 5000, 3000, &w, &h) == 0 && w == 4500 && h == 2700);

	// A runtime with no view enumeration: the system limit alone.
	g_rt.has_view_enumeration = false;
	g_rt.sys_max_w = 4096; g_rt.sys_max_h = 4096;
	g_log.clear();
	dxr_rs_on_session_starting(test_log);
	dxr_rs_on_system(kInstance, kSystem, fake_gipa, g_rt.sys_max_w, g_rt.sys_max_h);
	CHECK(log_count("runtime maximum image 4096x4096 (view configuration 0x0, system 4096x4096") == 1);
	CHECK(dxr_rs_scale_extent(2.0f, 1920, 1080, &w, &h) == 0 && w == 3840 && h == 2160);
	CHECK(dxr_rs_scale_extent(2.0f, 2400, 1350, &w, &h) == 1 && w == 4096 && h == 2304);

	// A runtime that reports nothing at all (and a NULL gipa): nothing is limited.
	dxr_rs_on_session_starting(test_log);
	dxr_rs_on_system(kInstance, kSystem, nullptr, 0, 0);
	CHECK(dxr_rs_scale_extent(2.0f, 6000, 4000, &w, &h) == 0 && w == 12000 && h == 8000);
	// Limits are per session: a new session before on_system has none either.
	dxr_rs_on_session_starting(test_log);
	CHECK(dxr_rs_scale_extent(2.0f, 6000, 4000, &w, &h) == 0 && w == 12000 && h == 8000);
	dxr_rs_on_session_stopped();
	g_rt = FakeRuntime();
}

// The whole life of a scale: nothing asked -> Unity's hint -> an animated change ->
// app override -> restart -> environment override.
static void
test_session_flow(void)
{
	g_rt = FakeRuntime();
	g_log.clear();
	Model m;
	DxrRenderScaleState st;

	// --- Nobody asked: created once at the base size, and it stays there. ---
	m.start();
	CHECK(creates() == 1 && last_create_is(1920, 1080));
	CHECK(dxr_prov_get_render_scale_state(&st) == 1);
	CHECK(st.requested == 1.0f && st.target == 1.0f && st.applied == 1.0f && st.source == DXR_RS_SOURCE_DEFAULT);
	CHECK(st.width == 1920 && st.height == 1080 && st.base_width == 1920 && st.base_height == 1080);
	CHECK(st.max_width == 3840 && st.max_height == 2160 && st.alloc_count == 1 && st.flags == 0);
	CHECK(log_count("render scale: eye texture 1920x1080 = base 1920x1080 x 1.00 (source=default, allocation #1)") == 1);
	m.frames(120, 1.0f);
	CHECK(creates() == 1);
	CHECK(dxr_prov_get_render_scale_state(&st) && st.source == DXR_RS_SOURCE_UNITY && st.requested == 1.0f);

	// --- XRSettings.eyeTextureResolutionScale = 0.5 mid-session. ---
	// Nothing happens until the request has held for the settle time...
	const int settle_frames = (int)(DXR_RS_SETTLE_NS / kFrameNs); // 15 at 60 Hz
	m.frames(settle_frames - 1, 0.5f);
	CHECK(creates() == 1);
	CHECK(dxr_prov_get_render_scale_state(&st) && st.requested == 0.5f && st.target == 1.0f);
	// ...then the session's own stable-frames debounce, then ONE recreate.
	m.frames(2 + (int)kDebounce + 2, 0.5f);
	CHECK(creates() == 2 && last_create_is(960, 540));
	CHECK(dxr_prov_get_render_scale_state(&st) && st.target == 0.5f && st.applied == 0.5f && st.alloc_count == 2);
	CHECK(log_count("render scale: request 0.50 (source=unity)") == 1);
	CHECK(log_count("render scale: target 1.00 -> 0.50 (request held for 250 ms)") == 1);
	CHECK(log_count("render scale: eye texture 960x540 = base 1920x1080 x 0.50 (source=unity, allocation #2)") == 1);
	m.frames(300, 0.5f);
	CHECK(creates() == 2);

	// --- 1.5: supersampling. ---
	m.frames(settle_frames + (int)kDebounce + 4, 1.5f);
	CHECK(creates() == 3 && last_create_is(2880, 1620));

	// --- An ANIMATED scale (1.5 -> 0.5 over two seconds, a new value every frame):
	//     no reallocation while it moves, exactly one when it stops. ---
	for (int i = 0; i < 120; i++) m.frame(1.5f - (float)i / 119.0f);
	CHECK(creates() == 3);
	m.frames(settle_frames + (int)kDebounce + 4, 0.5f);
	CHECK(creates() == 4 && last_create_is(960, 540));
	// A value that steps slower than the frame rate but faster than the settle time
	// (every 5 frames) does not thrash either.
	for (int i = 0; i < 60; i++) m.frame(0.5f + 0.05f * (float)(i / 5));
	CHECK(creates() == 4);
	m.frames(settle_frames + (int)kDebounce + 4, 1.0f);
	CHECK(creates() == 5 && last_create_is(1920, 1080));

	// --- A window resize at a scale: the target follows the base (no settle wait -
	//     that is for the SCALE; a resize has the session's debounce only). ---
	m.frames(settle_frames + (int)kDebounce + 4, 0.5f);
	CHECK(creates() == 6 && last_create_is(960, 540));
	m.base_w = 1280; m.base_h = 720;
	m.frames((int)kDebounce + 2, 0.5f);
	CHECK(creates() == 7 && last_create_is(640, 360));
	m.base_w = 1920; m.base_h = 1080;
	m.frames((int)kDebounce + 2, 0.5f);
	CHECK(creates() == 8 && last_create_is(960, 540));

	// --- The app's value outranks Unity's hint. ---
	dxr_prov_set_render_scale(0.75f);
	CHECK(dxr_prov_get_render_scale_override() == 0.75f);
	CHECK(log_count("render scale: app set 0.75") == 1);
	m.frames(settle_frames + (int)kDebounce + 4, 0.5f); // Unity keeps saying 0.5
	CHECK(creates() == 9 && last_create_is(1440, 810));
	CHECK(dxr_prov_get_render_scale_state(&st) && st.source == DXR_RS_SOURCE_APP && st.requested == 0.75f);
	CHECK(log_count("render scale: eye texture 1440x810 = base 1920x1080 x 0.75 (source=app, allocation #9)") == 1);
	// Setting the same value again is not a change.
	dxr_prov_set_render_scale(0.75f);
	CHECK(log_count("render scale: app set 0.75") == 1);
	// Out of range: clamped, flagged, said once.
	dxr_prov_set_render_scale(9.0f);
	CHECK(dxr_prov_get_render_scale_override() == DXR_RS_MAX);
	CHECK(log_count("render scale: app set 2.00 [out of range, clamped to 0.25..2.00]") == 1);
	m.frames(settle_frames + (int)kDebounce + 4, 0.5f);
	CHECK(creates() == 10 && last_create_is(3840, 2160));
	CHECK(dxr_prov_get_render_scale_state(&st) && (st.flags & DXR_RS_FLAG_RANGE_CLAMPED) != 0 &&
	      (st.flags & DXR_RS_FLAG_LIMIT_CLAMPED) == 0);

	// --- The runtime's maximum bites when the window grows at scale 2. ---
	size_t n = creates();
	m.base_w = 2400; m.base_h = 1500;
	m.frames((int)kDebounce + 2, 0.5f);
	CHECK(creates() == n + 1 && last_create_is(3456, 2160)); // 4800x3000 reduced uniformly by 0.72
	CHECK(dxr_prov_get_render_scale_state(&st) && (st.flags & DXR_RS_FLAG_LIMIT_CLAMPED) != 0);
	CHECK(fabsf(st.applied - 1.44f) < 1e-3f && st.target == 2.0f);
	CHECK(log_count("[2.00 asked; reduced to the runtime maximum 3840x2160]") == 1);
	// A base whose scaled size equals the current allocation reallocates nothing.
	m.base_w = 2400; m.base_h = 1350; // 4800x2700 -> 3840x2160
	m.frames((int)kDebounce + 2, 0.5f);
	CHECK(creates() == n + 2 && last_create_is(3840, 2160));
	m.base_w = 1920; m.base_h = 1080; // 3840x2160 unlimited: the same size
	m.frames((int)kDebounce + 2, 0.5f);
	CHECK(creates() == n + 2);

	// --- RESTART (editor dock <-> undock): the app's value survives, and the new
	//     session creates its swapchain at the scaled size at once - one create. ---
	dxr_prov_set_render_scale(0.5f);
	m.stop();
	CHECK(dxr_prov_get_render_scale_state(&st) && st.alloc_count == 0 && st.applied == 0.0f && st.requested == 0.5f);
	n = creates();
	m.start();
	CHECK(creates() == n + 1 && last_create_is(960, 540));
	m.frames(120, 1.0f);
	CHECK(creates() == n + 1);
	CHECK(dxr_prov_get_render_scale_state(&st) && st.alloc_count == 1 && st.source == DXR_RS_SOURCE_APP);

	// --- Clearing the app's value hands the decision back to Unity's hint. ---
	dxr_prov_set_render_scale(0.0f);
	CHECK(dxr_prov_get_render_scale_override() == 0.0f);
	CHECK(log_count("render scale: app override cleared") == 1);
	m.frames(settle_frames + (int)kDebounce + 4, 1.5f);
	CHECK(creates() == n + 2 && last_create_is(2880, 1620));
	CHECK(dxr_prov_get_render_scale_state(&st) && st.source == DXR_RS_SOURCE_UNITY);

	// --- RESTART with only Unity's hint: its last value is remembered, so the new
	//     session does not start at 1.0 and reallocate a moment later. ---
	m.stop();
	n = creates();
	m.start();
	CHECK(creates() == n + 1 && last_create_is(2880, 1620));
	m.frames(120, 1.5f);
	CHECK(creates() == n + 1);

	// --- A session whose FIRST hint differs from what it was created with (the very
	//     first session of a process: the swapchain exists before Unity says
	//     anything). The first hint is taken without the settle wait. ---
	m.stop();
	n = creates();
	m.start(); // created at the remembered 1.5
	CHECK(creates() == n + 1 && last_create_is(2880, 1620));
	m.frames((int)kDebounce + 2, 0.5f); // only the size debounce, no 250 ms
	CHECK(creates() == n + 2 && last_create_is(960, 540));

	// --- DISPLAYXR_RENDER_SCALE outranks both, and is re-read per session. ---
	m.stop();
	dxr_prov_set_render_scale(0.75f);
	set_env("DISPLAYXR_RENDER_SCALE", "1.25");
	n = creates();
	m.start();
	CHECK(creates() == n + 1 && last_create_is(2400, 1350));
	CHECK(log_count("render scale: DISPLAYXR_RENDER_SCALE=1.25 -> 1.25 (overrides the app and Unity)") == 1);
	CHECK(dxr_prov_get_render_scale_state(&st) && st.source == DXR_RS_SOURCE_ENV && st.requested == 1.25f);
	m.frames(120, 0.5f);
	CHECK(creates() == n + 1);
	m.stop();
	set_env("DISPLAYXR_RENDER_SCALE", "banana");
	m.start();
	CHECK(creates() == n + 2 && last_create_is(1440, 810)); // back to the app's 0.75
	CHECK(log_count("render scale: DISPLAYXR_RENDER_SCALE=banana ignored") == 1);
	m.stop();
	set_env("DISPLAYXR_RENDER_SCALE", nullptr);
	dxr_prov_set_render_scale(0.0f);

	// --- Garbage from Unity (0, NaN) reads as 1.0, never as a tiny texture. ---
	m.start();
	m.frames(settle_frames + (int)kDebounce + 4, 0.0f);
	CHECK(last_create_is(1920, 1080));
	n = creates();
	m.frames(60, NAN);
	CHECK(creates() == n && last_create_is(1920, 1080));
	// And an out-of-range hint is clamped.
	m.frames(settle_frames + (int)kDebounce + 4, 0.01f);
	CHECK(creates() == n + 1 && last_create_is(480, 270));
	m.stop();

	// Every swapchain the fake runtime saw was a 2-slice, 1-sample array.
	for (const XrSwapchainCreateInfo &ci : g_rt.created) {
		CHECK(ci.arraySize == 2 && ci.sampleCount == 1 && ci.width > 0 && ci.height > 0);
	}
}

// A scale change alone must never leave the render rect smaller than the slice
// (Unity's viewport origin vs the submitted imageRect origin is not established for a
// sub-rect - #143), including while the new scale is settling and debouncing.
static void
test_render_rect_matches_slice(void)
{
	g_rt = FakeRuntime();
	Model m;
	dxr_prov_set_render_scale(0.0f);
	m.start();
	const float steps[] = {1.0f, 0.5f, 1.5f, 0.25f, 2.0f, 1.0f, 0.77f};
	for (float s : steps) {
		m.frames(40, s);
		CHECK(m.render_w == m.sc_w && m.render_h == m.sc_h);
	}
	CHECK(m.render_matches_swapchain);
	m.stop();
}

// The editor Game view mirror (and the calibration marker drawn through it) is in
// window / panel pixels: the canvas shown from the woven texture is a function of the
// pane rect, the zone rect, the window and the woven texture ONLY. Run the session at
// every scale - the eye texture changes size each time - and the canvas, and the
// normalised source rect the mirror blit samples, must be the same numbers.
static void
test_mirror_independent_of_scale(void)
{
	g_rt = FakeRuntime();
	dxr_prov_set_render_scale(0.0f);

	// Texture mode at 175 %: a 1222x653 pane, the forced full-pane zone, a 3840x2160
	// woven texture (the layout of the hardware pass that prompted this test).
	DxrMirrorCanvasIn in = {};
	in.probe_enabled = 1;
	in.pane_x = INT32_MIN; in.pane_y = INT32_MIN;
	in.pane_w = 1222; in.pane_h = 653;
	in.zone_valid = 1; in.zone_x = 0; in.zone_y = 0; in.zone_w = 1222; in.zone_h = 653;
	const uint32_t woven_w = 3840, woven_h = 2160;

	Model m;
	m.base_w = 1222; m.base_h = 653; // zone recommended view size == zone extent
	m.start();
	const float scales[] = {1.0f, 0.5f, 1.5f, 0.25f, 2.0f, 0.77f, 1.0f};
	uint32_t seen_eye_w[7];
	int i = 0;
	for (float s : scales) {
		m.frames(40, s);
		seen_eye_w[i++] = m.sc_w;

		int32_t x = -1, y = -1, w = -1, h = -1;
		CHECK(dxr_mirror_canvas(&in, &x, &y, &w, &h) == 1);
		CHECK(x == 0 && y == 0 && w == 1222 && h == 653);
		const float src[4] = {(float)x / woven_w, (float)y / woven_h, (float)w / woven_w, (float)h / woven_h};
		CHECK(src[0] == 0.0f && src[1] == 0.0f && src[2] == 1222.0f / 3840.0f && src[3] == 653.0f / 2160.0f);
	}
	// The eye texture really did change under it: 1222, 611, 1833, 306 (305.5 rounded), 2444, 941, 1222.
	CHECK(seen_eye_w[0] == 1222 && seen_eye_w[1] == 611 && seen_eye_w[2] == 1833 && seen_eye_w[3] == 306);
	CHECK(seen_eye_w[4] == 2444 && seen_eye_w[5] == 941 && seen_eye_w[6] == 1222);
	m.stop();

	// The helper's own cases (unchanged behaviour of dxr_prov_get_woven_canvas).
	int32_t x, y, w, h;
	// Zone-glue: the pane's published position is the canvas origin; (0,0) is a position.
	in.pane_x = 640; in.pane_y = 212;
	CHECK(dxr_mirror_canvas(&in, &x, &y, &w, &h) == 1 && x == 640 && y == 212 && w == 1222 && h == 653);
	in.pane_x = 0; in.pane_y = 0;
	CHECK(dxr_mirror_canvas(&in, &x, &y, &w, &h) == 1 && x == 0 && y == 0);
	// An app-authored sub-pane zone does not shrink the canvas: the pane is shown.
	in.zone_x = 0; in.zone_y = 77; in.zone_w = 1222; in.zone_h = 576;
	CHECK(dxr_mirror_canvas(&in, &x, &y, &w, &h) == 1 && y == 0 && w == 1222 && h == 653);
	// No pane published (or not texture mode): the zone.
	in.pane_w = 0; in.pane_h = 0;
	CHECK(dxr_mirror_canvas(&in, &x, &y, &w, &h) == 1 && x == 0 && y == 77 && w == 1222 && h == 576);
	in.pane_w = 1222; in.pane_h = 653; in.probe_enabled = 0;
	CHECK(dxr_mirror_canvas(&in, &x, &y, &w, &h) == 1 && y == 77 && h == 576);
	// Neither: the caller's fallback - the window, else the whole woven texture.
	in.zone_valid = 0;
	CHECK(dxr_mirror_canvas(&in, &x, &y, &w, &h) == 0 && x == 0 && y == 0 && w == 0 && h == 0);
	dxr_mirror_canvas_fallback(1600, 900, woven_w, woven_h, &w, &h);
	CHECK(w == 1600 && h == 900);
	dxr_mirror_canvas_fallback(0, 0, woven_w, woven_h, &w, &h);
	CHECK(w == 3840 && h == 2160);
}

int
main(void)
{
	set_env("DISPLAYXR_RENDER_SCALE", nullptr);
	test_sanitize();
	test_extent();
	test_session_flow();
	test_render_rect_matches_slice();
	test_mirror_independent_of_scale(); // last: test_session_flow starts from "nobody asked"
	if (g_fail) {
		fprintf(stderr, "render_scale_test: %d of %d checks FAILED\n", g_fail, g_checks);
		return 1;
	}
	printf("render_scale_test: all %d checks passed\n", g_checks);
	return 0;
}
