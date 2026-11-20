// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Render scale (#475). See displayxr_provider_render_scale.h for the contract.
// Platform-neutral: no graphics API, no window system - only the arithmetic, the
// precedence of the three sources, the settle timer and the log lines. The session
// TU asks this one "what size?" and reports back "this is what I allocated".

#include "displayxr_provider_render_scale.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mutex>

static_assert(sizeof(DxrRenderScaleState) == 12 * sizeof(int32_t), "DxrRenderScaleState layout is mirrored in C#");
static_assert(sizeof(float) == sizeof(int32_t), "DxrRenderScaleState assumes 32-bit float");

namespace {

std::mutex s_mutex;

// ---- Survive a session restart ----
float s_app = 0.0f;      // dxr_prov_set_render_scale; 0 = not set
float s_hint = 0.0f;     // Unity's last hint; 0 = none received yet
float s_env = 0.0f;      // DISPLAYXR_RENDER_SCALE; 0 = not set
int s_app_clamped = 0;
int s_env_clamped = 0;
DxrRsLogFn s_log = nullptr;

// ---- Per session ----
struct Session
{
	uint32_t max_w = 0, max_h = 0;
	bool hint_seen = false;    // a hint arrived in THIS session
	bool target_valid = false; // target/pending hold a resolved request
	float target = 1.0f;
	float pending = 1.0f;
	uint64_t pending_since = 0;
	bool pending_timed = false; // pending_since is a real tick time
	// What was last logged as "the request", so the line appears once per change.
	float logged_request = -1.0f;
	int logged_source = -1;
	// Last primary allocation.
	float applied = 0.0f;
	uint32_t base_w = 0, base_h = 0, w = 0, h = 0;
	uint32_t alloc_count = 0;
	bool limit_clamped = false;
};
Session s_s;

void
rs_log(const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;

void
rs_log(const char *fmt, ...)
{
	if (s_log == nullptr) return;
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	s_log("%s", buf);
}

const char *
source_name(int source)
{
	switch (source) {
	case DXR_RS_SOURCE_UNITY: return "unity";
	case DXR_RS_SOURCE_APP: return "app";
	case DXR_RS_SOURCE_ENV: return "env";
	default: return "default";
	}
}

// The request, by precedence. Lock held.
float
resolve_locked(int *out_source, int *out_range_clamped)
{
	int source = DXR_RS_SOURCE_DEFAULT;
	float v = 1.0f;
	int clamped = 0;
	if (s_env > 0.0f) {
		v = s_env; source = DXR_RS_SOURCE_ENV; clamped = s_env_clamped;
	} else if (s_app > 0.0f) {
		v = s_app; source = DXR_RS_SOURCE_APP; clamped = s_app_clamped;
	} else if (s_hint > 0.0f) {
		int c = 0;
		v = dxr_rs_sanitize(s_hint, &c);
		if (v <= 0.0f) v = 1.0f;
		source = DXR_RS_SOURCE_UNITY; clamped = c;
	}
	if (out_source) *out_source = source;
	if (out_range_clamped) *out_range_clamped = clamped;
	return v;
}

// One line per change of (request, source). Lock held.
void
log_request_locked(float request, int source, int range_clamped)
{
	if (request == s_s.logged_request && source == s_s.logged_source) return;
	s_s.logged_request = request;
	s_s.logged_source = source;
	rs_log("[DisplayXR-PROV] render scale: request %.2f (source=%s)%s\n", (double)request, source_name(source),
	       range_clamped ? " [out of range, clamped to 0.25..2.00]" : "");
}

// Make target/pending valid from the current request. Lock held.
void
ensure_target_locked(void)
{
	if (s_s.target_valid) return;
	int source = 0, clamped = 0;
	const float req = resolve_locked(&source, &clamped);
	s_s.target = req;
	s_s.pending = req;
	s_s.pending_timed = false;
	s_s.target_valid = true;
	log_request_locked(req, source, clamped);
}

// base -> size for `scale` under the limits. Lock held. Returns true when the limit bit.
bool
scale_extent_locked(float scale, uint32_t base_w, uint32_t base_h, uint32_t *out_w, uint32_t *out_h)
{
	if (base_w == 0 || base_h == 0) { *out_w = 0; *out_h = 0; return false; }
	// Exactly 1.0 is the identity - never rounded, never limited.
	if (!(scale > 0.0f) || scale == 1.0f) { *out_w = base_w; *out_h = base_h; return false; }

	double w = floor((double)base_w * (double)scale + 0.5);
	double h = floor((double)base_h * (double)scale + 0.5);
	if (w < 1.0) w = 1.0;
	if (h < 1.0) h = 1.0;

	bool limited = false;
	// The limit only ever takes back what a scale above 1 added: it never makes the
	// texture smaller than the unscaled base (which is what was allocated before
	// this feature existed, whatever the runtime said its maximum was).
	if (scale > 1.0f) {
		double f = 1.0;
		if (s_s.max_w > 0 && w > (double)s_s.max_w) f = fmin(f, (double)s_s.max_w / w);
		if (s_s.max_h > 0 && h > (double)s_s.max_h) f = fmin(f, (double)s_s.max_h / h);
		if (f < 1.0) {
			limited = true;
			// Rounded, not floored: w * f <= max by construction, so the rounded value
			// cannot exceed the (integer) maximum, and the binding side lands ON it
			// instead of one pixel short through floating-point error.
			w = floor(w * f + 0.5);
			h = floor(h * f + 0.5);
			if (s_s.max_w > 0 && w > (double)s_s.max_w) w = (double)s_s.max_w;
			if (s_s.max_h > 0 && h > (double)s_s.max_h) h = (double)s_s.max_h;
			if (w < 1.0) w = 1.0;
			if (h < 1.0) h = 1.0;
			if (w < (double)base_w || h < (double)base_h) { w = (double)base_w; h = (double)base_h; }
		}
	}
	*out_w = (uint32_t)w;
	*out_h = (uint32_t)h;
	return limited;
}

} // namespace

// ============================================================================
// Pure helpers
// ============================================================================

float
dxr_rs_sanitize(float scale, int *out_clamped)
{
	if (out_clamped) *out_clamped = 0;
	if (!isfinite(scale) || !(scale > 0.0f)) return 0.0f;
	float v = scale;
	// Quantise first, so 1.0049 is "1.00" and takes the identity path.
	v = (float)(floor((double)v * 100.0 + 0.5) / 100.0);
	if (v < DXR_RS_MIN) { v = DXR_RS_MIN; if (out_clamped) *out_clamped = 1; }
	if (v > DXR_RS_MAX) { v = DXR_RS_MAX; if (out_clamped) *out_clamped = 1; }
	return v;
}

int
dxr_rs_scale_extent(float scale, uint32_t base_width, uint32_t base_height, uint32_t *out_width, uint32_t *out_height)
{
	uint32_t w = 0, h = 0;
	bool limited;
	{
		std::lock_guard<std::mutex> lk(s_mutex);
		limited = scale_extent_locked(scale, base_width, base_height, &w, &h);
	}
	if (out_width) *out_width = w;
	if (out_height) *out_height = h;
	return limited ? 1 : 0;
}

// ============================================================================
// C# surface
// ============================================================================

void
dxr_prov_set_render_scale(float scale)
{
	std::lock_guard<std::mutex> lk(s_mutex);
	int clamped = 0;
	const float v = dxr_rs_sanitize(scale, &clamped);
	if (v == s_app) return;
	s_app = v;
	s_app_clamped = clamped;
	if (v > 0.0f)
		rs_log("[DisplayXR-PROV] render scale: app set %.2f%s\n", (double)v,
		       clamped ? " [out of range, clamped to 0.25..2.00]" : "");
	else
		rs_log("[DisplayXR-PROV] render scale: app override cleared (following Unity's hint)\n");
}

float
dxr_prov_get_render_scale_override(void)
{
	std::lock_guard<std::mutex> lk(s_mutex);
	return s_app;
}

int
dxr_prov_get_render_scale_state(DxrRenderScaleState *out)
{
	if (out == nullptr) return 0;
	std::lock_guard<std::mutex> lk(s_mutex);
	memset(out, 0, sizeof(*out));
	int source = 0, clamped = 0;
	out->requested = resolve_locked(&source, &clamped);
	out->target = s_s.target_valid ? s_s.target : out->requested;
	out->applied = s_s.applied;
	out->source = source;
	out->base_width = s_s.base_w;
	out->base_height = s_s.base_h;
	out->width = s_s.w;
	out->height = s_s.h;
	out->max_width = s_s.max_w;
	out->max_height = s_s.max_h;
	out->alloc_count = s_s.alloc_count;
	out->flags = (clamped ? DXR_RS_FLAG_RANGE_CLAMPED : 0u) | (s_s.limit_clamped ? DXR_RS_FLAG_LIMIT_CLAMPED : 0u);
	return 1;
}

// ============================================================================
// Session lifecycle
// ============================================================================

void
dxr_rs_on_session_starting(DxrRsLogFn log)
{
	std::lock_guard<std::mutex> lk(s_mutex);
	s_log = log;
	s_s = Session();

	// Re-read every session: an editor harness may change it between Play runs.
	s_env = 0.0f;
	s_env_clamped = 0;
	const char *e = getenv("DISPLAYXR_RENDER_SCALE");
	if (e != nullptr && e[0] != '\0') {
		char *end = nullptr;
		const double d = strtod(e, &end);
		const float v = (end != e) ? dxr_rs_sanitize((float)d, &s_env_clamped) : 0.0f;
		if (v > 0.0f) {
			s_env = v;
			rs_log("[DisplayXR-PROV] render scale: DISPLAYXR_RENDER_SCALE=%s -> %.2f (overrides the app and Unity)\n",
			       e, (double)v);
		} else {
			rs_log("[DisplayXR-PROV] render scale: DISPLAYXR_RENDER_SCALE=%s ignored (not a positive number)\n", e);
		}
	}
}

void
dxr_rs_on_session_stopped(void)
{
	std::lock_guard<std::mutex> lk(s_mutex);
	s_s = Session();
}

void
dxr_rs_on_system(XrInstance instance, XrSystemId system_id, PFN_xrGetInstanceProcAddr gipa, uint32_t system_max_width,
                 uint32_t system_max_height)
{
	uint32_t view_w = 0, view_h = 0;
	if (gipa != nullptr && instance != XR_NULL_HANDLE) {
		PFN_xrVoidFunction fn = nullptr;
		if (XR_SUCCEEDED(gipa(instance, "xrEnumerateViewConfigurationViews", &fn)) && fn != nullptr) {
			PFN_xrEnumerateViewConfigurationViews enumerate = (PFN_xrEnumerateViewConfigurationViews)fn;
			XrViewConfigurationView views[8];
			for (XrViewConfigurationView &v : views) {
				memset(&v, 0, sizeof(v));
				v.type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
			}
			uint32_t count = 0;
			if (XR_SUCCEEDED(enumerate(instance, system_id, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 8, &count,
			                           views))) {
				if (count > 8) count = 8;
				for (uint32_t i = 0; i < count; i++) {
					const uint32_t mw = views[i].maxImageRectWidth, mh = views[i].maxImageRectHeight;
					if (mw > 0 && (view_w == 0 || mw < view_w)) view_w = mw;
					if (mh > 0 && (view_h == 0 || mh < view_h)) view_h = mh;
				}
			}
		}
	}

	std::lock_guard<std::mutex> lk(s_mutex);
	uint32_t mw = view_w, mh = view_h;
	if (system_max_width > 0 && (mw == 0 || system_max_width < mw)) mw = system_max_width;
	if (system_max_height > 0 && (mh == 0 || system_max_height < mh)) mh = system_max_height;
	s_s.max_w = mw;
	s_s.max_h = mh;
	rs_log("[DisplayXR-PROV] render scale: runtime maximum image %ux%u (view configuration %ux%u, system %ux%u; "
	       "0 = not reported)\n",
	       mw, mh, view_w, view_h, system_max_width, system_max_height);
}

// ============================================================================
// Per frame
// ============================================================================

void
dxr_rs_set_unity_hint(float scale)
{
	std::lock_guard<std::mutex> lk(s_mutex);
	const float v = (isfinite(scale) && scale > 0.0f) ? scale : 1.0f;
	s_hint = v;
	if (!s_s.hint_seen) {
		// The first hint of a session is not a "change" to wait out: the swapchain may
		// have been created before Unity said anything, so take the value at once.
		s_s.hint_seen = true;
		s_s.target_valid = false;
		ensure_target_locked();
	}
}

void
dxr_rs_tick(uint64_t now_ns)
{
	std::lock_guard<std::mutex> lk(s_mutex);
	ensure_target_locked();
	int source = 0, clamped = 0;
	const float req = resolve_locked(&source, &clamped);
	log_request_locked(req, source, clamped);
	if (req != s_s.pending) {
		s_s.pending = req;
		s_s.pending_since = now_ns;
		s_s.pending_timed = true;
	} else if (!s_s.pending_timed) {
		// Pending was seeded without a clock (ensure_target); start timing it now.
		s_s.pending_since = now_ns;
		s_s.pending_timed = true;
	}
	if (s_s.pending != s_s.target && now_ns - s_s.pending_since >= DXR_RS_SETTLE_NS) {
		rs_log("[DisplayXR-PROV] render scale: target %.2f -> %.2f (request held for %u ms)\n", (double)s_s.target,
		       (double)s_s.pending, (unsigned)(DXR_RS_SETTLE_NS / 1000000ull));
		s_s.target = s_s.pending;
	}
}

float
dxr_rs_target(void)
{
	std::lock_guard<std::mutex> lk(s_mutex);
	ensure_target_locked();
	return s_s.target;
}

void
dxr_rs_note_primary_allocated(float scale, uint32_t base_width, uint32_t base_height, uint32_t width, uint32_t height)
{
	std::lock_guard<std::mutex> lk(s_mutex);
	uint32_t ew = 0, eh = 0;
	const bool limited = scale_extent_locked(scale, base_width, base_height, &ew, &eh);
	int source = 0;
	(void)resolve_locked(&source, nullptr);
	s_s.base_w = base_width;
	s_s.base_h = base_height;
	s_s.w = width;
	s_s.h = height;
	s_s.applied = base_width > 0 ? (float)width / (float)base_width : 0.0f;
	s_s.limit_clamped = limited;
	s_s.alloc_count++;
	char note[160] = "";
	if (limited)
		snprintf(note, sizeof(note), " [%.2f asked; reduced to the runtime maximum %ux%u]", (double)scale, s_s.max_w,
		         s_s.max_h);
	rs_log("[DisplayXR-PROV] render scale: eye texture %ux%u = base %ux%u x %.2f (source=%s, allocation #%u)%s\n",
	       width, height, base_width, base_height, (double)s_s.applied, source_name(source), s_s.alloc_count, note);
}
