// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Pure, platform-neutral math for the provider's depth-aware cursor
// (XR_DXR_cursor_depth, ADR-046 + Amendments 2 and 3). No OpenXR calls, no
// Unity, no globals: everything here is unit-tested by
// native~/tests/cursor_depth_math_test.cpp.
//
// The rules are the SAME as displayxr-common's dxr::CursorDepth (common/cursor_depth.h,
// v2.31.0) - footprint shape, view ray, nearest-hit choice - so a Unity app's cursor
// behaves exactly like a native demo's. They are re-stated here rather than taken from
// displayxr-common because this plug-in has no source dependency on any DisplayXR repo
// (it fetches only Khronos headers); keep the two in step.
//
// Coordinate conventions:
//   - Unity screen: pixels, origin BOTTOM-left (Input.mousePosition / Mouse.position).
//   - Canvas UV: what XrCursorDepthHintDXR::cursorUV wants - [0,1]^2 over the canvas
//     the locate frames (the window client area, or the zone rect for a zone-scoped
//     locate), origin TOP-left, v DOWN.
//   - OpenXR locate space: right-handed, -Z forward. Unity world: left-handed, +Z
//     forward. With a DisplayXR rig pushing its pose, the provider chains that pose as
//     the display pose, so locate space == Unity world with Z negated (the same flip
//     dxr_prov_set_display_pose and set_eye_pose_rig_relative use).

#pragma once

#include <math.h>
#include <stdint.h>

namespace dxr_cursor {

struct Vec2 { float x, y; };
struct Vec3 { float x, y, z; };
struct Quat { float x, y, z, w; };
struct Ray  { Vec3 origin; Vec3 dir; };

//! One located Kooima view: eye pose + fov tangents' angles (radians).
struct View {
	Vec3 position;
	Quat orientation;
	float angle_left, angle_right, angle_up, angle_down;
};

// ---- Spec-version gate ------------------------------------------------------

//! Which depth source the app asked for (the C# DisplayXRCursorDepth.Source values).
enum Source { SOURCE_MEASURED = 0, SOURCE_APP_POINT = 1 };

//! The runtime-drawn cursor (XrCursorDepthDrawInfoDXR / willDraw) appeared in v5.
static const uint32_t kMinSpecRuntimeDraw = 5;
//! XR_CURSOR_DEPTH_SOURCE_MEASURED_DISPARITY_DXR appeared in v7.
static const uint32_t kMinSpecMeasured = 7;

/*!
 * May this frame chain the cursor structs at all?  The provider never draws a cursor
 * itself, so everything rests on the runtime drawing it: below v5 nothing is chained
 * (the plain OS cursor, exactly as without the feature). MEASURED additionally needs
 * v7; against a v5/v6 runtime it degrades to "nothing chained", never to an
 * APP_POINT hint with no point.
 */
inline bool chain_allowed(uint32_t advertised_spec, int source)
{
	if (advertised_spec < kMinSpecRuntimeDraw) return false;
	if (source == SOURCE_MEASURED) return advertised_spec >= kMinSpecMeasured;
	return source == SOURCE_APP_POINT;
}

// ---- Pointer -> canvas UV -----------------------------------------------------

/*!
 * Unity screen units -> the window's client pixels, in place. App zone rects are in client
 * pixels; Unity's pointer and Screen.* are the same unit on Windows and macOS, but LOGICAL
 * pixels (client / scale) on a scaled native-Wayland output, so a zone-scoped mapping
 * scales them first. Leaves everything as is when either size is unknown (<= 0).
 */
inline void screen_to_client_px(float client_w, float client_h, float *mouse_x, float *mouse_y_up,
                                float *screen_w, float *screen_h)
{
	if (!(client_w > 0.0f) || !(client_h > 0.0f) || !(*screen_w > 0.0f) || !(*screen_h > 0.0f)) return;
	*mouse_x *= client_w / *screen_w;
	*mouse_y_up *= client_h / *screen_h;
	*screen_w = client_w;
	*screen_h = client_h;
}

/*!
 * Unity screen pixels (origin bottom-left) -> canvas UV (origin top-left, v down).
 *
 * @param zone_valid  1 when the locate is zone-scoped on an APP-authored zone; the zone
 *                    rect is then in the same client pixels as the screen (top-left
 *                    origin), which is how DisplayXRProvider.SetZoneRect documents it.
 *                    0: the canvas is the whole screen.
 * @return false when the screen is degenerate. The UV may lie outside [0,1]: the
 *         pointer is then off the canvas and the caller chains nothing.
 */
inline bool screen_to_canvas_uv(float mouse_x, float mouse_y_up, float screen_w, float screen_h,
                                int zone_valid, float zx, float zy, float zw, float zh, Vec2 *out)
{
	if (!(screen_w > 0.0f) || !(screen_h > 0.0f) || out == 0) return false;
	const float px = mouse_x;
	const float py = screen_h - mouse_y_up; // -> top-left origin
	if (zone_valid) {
		if (!(zw > 0.0f) || !(zh > 0.0f)) return false;
		out->x = (px - zx) / zw;
		out->y = (py - zy) / zh;
	} else {
		out->x = px / screen_w;
		out->y = py / screen_h;
	}
	return isfinite(out->x) && isfinite(out->y);
}

inline bool uv_in_canvas(Vec2 uv)
{
	return uv.x >= 0.0f && uv.x <= 1.0f && uv.y >= 0.0f && uv.y <= 1.0f;
}

// ---- Unity <-> OpenXR -----------------------------------------------------------

//! Unity world (LH, +Z fwd) <-> OpenXR locate space (RH, -Z fwd). Its own inverse.
inline Vec3 flip_z(Vec3 p) { return Vec3{p.x, p.y, -p.z}; }

// ---- Footprint ------------------------------------------------------------------

//! The default arrow hangs down-right of its tip: footprint centre = tip + (0.3 h / aspect, 0.5 h).
static const Vec2 kArrowBodyOffset = {0.3f, 0.5f};
//! Runtime default cursor height, fraction of the canvas height.
static const float kDefaultCursorHeight = 0.03f;
static const uint32_t kFootprintMaxPoints = 10;
static const uint32_t kFootprintMaxRays = 2 * kFootprintMaxPoints;

/*!
 * The cursor FOOTPRINT in canvas UV: the hotspot, then 8 points on a circle of radius
 * 0.75 x height around the sprite body (hotspot + body offset x height), then the body
 * centre. Same rule as the runtime's depth search and dxr::cursorFootprintUV.
 */
inline uint32_t footprint_uv(Vec2 uv, float height_frac, float canvas_aspect,
                             Vec2 out[kFootprintMaxPoints], Vec2 body = kArrowBodyOffset)
{
	if (!(canvas_aspect > 0.0f) || !isfinite(canvas_aspect)) canvas_aspect = 1.0f;
	uint32_t n = 0;
	out[n++] = uv;
	const float cu = uv.x + body.x * height_frac / canvas_aspect;
	const float cv = uv.y + body.y * height_frac;
	const float r = 0.75f * height_frac;
	for (int k = 0; k < 8; k++) {
		const float a = (float)k * 0.785398163f;
		out[n++] = Vec2{cu + r * cosf(a) / canvas_aspect, cv + r * sinf(a)};
	}
	if (body.x != 0.0f || body.y != 0.0f) out[n++] = Vec2{cu, cv};
	return n;
}

inline Vec3 rotate(Quat q, Vec3 v)
{
	const float tx = 2.0f * (q.y * v.z - q.z * v.y);
	const float ty = 2.0f * (q.z * v.x - q.x * v.z);
	const float tz = 2.0f * (q.x * v.y - q.y * v.x);
	return Vec3{v.x + q.w * tx + (q.y * tz - q.z * ty), v.y + q.w * ty + (q.z * tx - q.x * tz),
	            v.z + q.w * tz + (q.x * ty - q.y * tx)};
}

inline float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

//! Canvas aspect (w/h) a Kooima view frames: its fov tangent extents. 0 if degenerate.
inline float canvas_aspect(const View &v)
{
	const float w = tanf(v.angle_right) - tanf(v.angle_left);
	const float h = tanf(v.angle_up) - tanf(v.angle_down);
	return (h > 0.0f && w > 0.0f) ? w / h : 0.0f;
}

/*!
 * Ray from a located view through canvas point (u, v): every DisplayXR view is a Kooima
 * frustum framing the canvas, so (u, v) maps linearly onto the fov tangents. Exact for
 * display rigs, camera rigs and any meters-to-virtual scale. Same as dxr::cursorViewRay.
 */
inline Ray view_ray(const View &view, float u, float v)
{
	const float tl = tanf(view.angle_left), tr = tanf(view.angle_right);
	const float tu = tanf(view.angle_up), td = tanf(view.angle_down);
	Vec3 d = rotate(view.orientation, Vec3{tl + u * (tr - tl), tu - v * (tu - td), -1.0f});
	const float len = sqrtf(dot(d, d));
	if (len > 0.0f) d = Vec3{d.x / len, d.y / len, d.z / len};
	return Ray{view.position, d};
}

/*!
 * Rays from the two OUTER views through every footprint point, in the locate space.
 * @return the ray count (0 when fewer than two views: 2D has nothing to hint).
 */
inline uint32_t footprint_rays(const View *views, uint32_t view_count, Vec2 uv, float height_frac,
                               Ray out[kFootprintMaxRays])
{
	if (views == 0 || view_count < 2) return 0;
	Vec2 pts[kFootprintMaxPoints];
	const uint32_t np = footprint_uv(uv, height_frac, canvas_aspect(views[0]), pts);
	const View *outer[2] = {&views[0], &views[view_count - 1]};
	uint32_t n = 0;
	for (uint32_t i = 0; i < np; i++)
		for (int k = 0; k < 2; k++) out[n++] = view_ray(*outer[k], pts[i].x, pts[i].y);
	return n;
}

/*!
 * The hint's nearestPoint from a set of hits (locate space): the one nearest the
 * viewer along the view's forward axis (smallest dot(p, forward)), the same choice
 * dxr::CursorDepth makes. Non-finite points are skipped.
 * @return false when no usable hit.
 */
inline bool nearest_hit(const Vec3 *hits, uint32_t count, Quat view_orientation, Vec3 *out)
{
	const Vec3 fwd = rotate(view_orientation, Vec3{0.0f, 0.0f, -1.0f});
	bool have = false;
	float best = 0.0f;
	for (uint32_t i = 0; i < count; i++) {
		const Vec3 p = hits[i];
		if (!isfinite(p.x) || !isfinite(p.y) || !isfinite(p.z)) continue;
		const float d = dot(p, fwd);
		if (!have || d < best) {
			best = d;
			*out = p;
			have = true;
		}
	}
	return have;
}

// ---- Custom cursor sprite (XrCursorDepthDrawInfoDXR, spec v5; #369) ------------

//! XrCursorDepthDrawInfoDXR (and so a custom sprite) appeared with the runtime draw, v5.
static const uint32_t kMinSpecSprite = kMinSpecRuntimeDraw;

//! Largest sprite edge the provider will make a swapchain for. A cursor is tens of
//! pixels; anything bigger is almost certainly the wrong texture.
static const uint32_t kSpriteMaxEdge = 1024;

inline bool sprite_allowed(uint32_t advertised_spec) { return advertised_spec >= kMinSpecSprite; }

inline bool sprite_size_ok(uint32_t w, uint32_t h)
{
	return w > 0 && h > 0 && w <= kSpriteMaxEdge && h <= kSpriteMaxEdge;
}

/*!
 * The hotspot as XrCursorDepthDrawInfoDXR::hotSpot wants it: normalised [0,1]^2 over
 * the sprite, origin TOP-left, v DOWN. The input is in texture pixels from the TOP-left
 * corner - Unity's Cursor.SetCursor convention - so this is a plain divide (no flip),
 * clamped onto the sprite. Non-finite components go to 0. A degenerate size gives (0,0).
 */
inline Vec2 sprite_hotspot_uv(float hx_px, float hy_px, float w, float h)
{
	Vec2 r = {0.0f, 0.0f};
	if (!(w > 0.0f) || !(h > 0.0f)) return r;
	r.x = isfinite(hx_px) ? hx_px / w : 0.0f;
	r.y = isfinite(hy_px) ? hy_px / h : 0.0f;
	r.x = r.x < 0.0f ? 0.0f : (r.x > 1.0f ? 1.0f : r.x);
	r.y = r.y < 0.0f ? 0.0f : (r.y > 1.0f ? 1.0f : r.y);
	return r;
}

/*!
 * XrCursorDepthDrawInfoDXR::sizeMeters from what the app asked for.
 *   > 0 (finite): that height on the panel, metres.
 *   == 0, or non-finite: 0 = the runtime's default custom-sprite size (19 OS points).
 *   < 0: the texture's own pixel height on the panel - what a hardware cursor of the
 *        same texture measures - = tex_h x the panel's metres per pixel; 0 (the runtime
 *        default) when the panel geometry is unknown.
 */
inline float sprite_size_meters(float requested, uint32_t tex_h, float display_h_m, uint32_t display_px_h)
{
	if (!isfinite(requested) || requested == 0.0f) return 0.0f;
	if (requested > 0.0f) return requested;
	if (tex_h == 0 || display_px_h == 0 || !(display_h_m > 0.0f) || !isfinite(display_h_m)) return 0.0f;
	return (float)tex_h * display_h_m / (float)display_px_h;
}

//! Swapchain format numbering of the session's graphics API (the ids collide across APIs).
enum SpriteApi { SPRITE_API_DXGI = 0, SPRITE_API_VULKAN = 1, SPRITE_API_METAL = 2 };

//! The 8-bit UNORM / _SRGB format id for an RGBA- or BGRA-ordered sprite in @p api.
inline int64_t sprite_format_id(int api, bool bgra, bool srgb)
{
	switch (api) {
	case SPRITE_API_DXGI: return bgra ? (srgb ? 91 : 87) : (srgb ? 29 : 28);
	case SPRITE_API_VULKAN: return bgra ? (srgb ? 50 : 44) : (srgb ? 43 : 37);
	case SPRITE_API_METAL: return bgra ? (srgb ? 81 : 80) : (srgb ? 71 : 70);
	default: return 0;
	}
}

/*!
 * Pick the sprite swapchain format among the runtime's @p offered formats. The copy into
 * the sprite is a raw byte copy, so the channel order must be the source's (@p bgra);
 * @p srgb says whether those bytes are sRGB-ENCODED colour (an sRGB Unity texture, or any
 * texture in a Gamma-space project) - then the _SRGB format, which the runtime decodes on
 * sample - or linear values (UNORM). Falls back to the other encoding of the same order
 * (a byte-exact copy, slightly off colour) before giving up.
 * @return the format id, or 0 when neither is offered (no sprite: the default look).
 */
inline int64_t sprite_pick_format(int api, bool bgra, bool srgb, const int64_t *offered, uint32_t count)
{
	const int64_t want = sprite_format_id(api, bgra, srgb);
	const int64_t alt = sprite_format_id(api, bgra, !srgb);
	for (uint32_t i = 0; i < count; i++)
		if (offered[i] == want) return want;
	for (uint32_t i = 0; i < count; i++)
		if (offered[i] == alt) return alt;
	return 0;
}

/*!
 * The render-thread "filled" event carries the sprite generation modulo this, so a fill
 * recorded for an older SetSprite is never taken for the newest one. 16 is ample: the
 * render thread trails the main thread by one or two frames.
 */
static const uint32_t kSpriteGenSlots = 16;
inline uint32_t sprite_gen_slot(uint32_t gen) { return gen % kSpriteGenSlots; }

} // namespace dxr_cursor
