// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for displayxr_cursor_depth_math.h - the pure part of the provider's
// depth-aware cursor. Header-only, no OpenXR / Unity / test framework:
//   c++ -std=c++17 -I native~/displayxr_xrprovider native~/tests/cursor_depth_math_test.cpp && ./a.out
// or `-DDISPLAYXR_BUILD_TESTS=ON` and `ctest` in the CMake build.

#include "displayxr_cursor_depth_math.h"

#include <math.h>
#include <stdio.h>

using namespace dxr_cursor;

static int s_fail = 0;
static int s_pass = 0;

#define CHECK(cond)                                                                  \
	do {                                                                             \
		if (cond) {                                                                  \
			s_pass++;                                                                \
		} else {                                                                     \
			s_fail++;                                                                \
			printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                   \
		}                                                                            \
	} while (0)

static bool near(float a, float b, float eps = 1e-5f) { return fabsf(a - b) <= eps; }
static bool near3(Vec3 a, Vec3 b, float eps = 1e-5f)
{
	return near(a.x, b.x, eps) && near(a.y, b.y, eps) && near(a.z, b.z, eps);
}

// A symmetric Kooima-like view: eye at (ex, 0, d) looking down -Z at a canvas of
// w x h centred on the origin (OpenXR locate space).
static View make_view(float ex, float d, float w, float h)
{
	View v;
	v.position = {ex, 0.0f, d};
	v.orientation = {0, 0, 0, 1};
	v.angle_left = atanf((-w / 2 - ex) / d);
	v.angle_right = atanf((w / 2 - ex) / d);
	v.angle_up = atanf((h / 2) / d);
	v.angle_down = atanf((-h / 2) / d);
	return v;
}

// Intersect a ray with the canvas plane z = 0.
static Vec3 hit_canvas(Ray r)
{
	const float t = -r.origin.z / r.dir.z;
	return {r.origin.x + t * r.dir.x, r.origin.y + t * r.dir.y, 0.0f};
}

static void test_gate()
{
	// Below v5 the runtime cannot draw: nothing is ever chained.
	for (uint32_t v = 0; v < 5; v++) {
		CHECK(!chain_allowed(v, SOURCE_MEASURED));
		CHECK(!chain_allowed(v, SOURCE_APP_POINT));
	}
	// v5, v6: the draw request with an app point, but no measured disparity.
	CHECK(chain_allowed(5, SOURCE_APP_POINT));
	CHECK(chain_allowed(6, SOURCE_APP_POINT));
	CHECK(!chain_allowed(5, SOURCE_MEASURED));
	CHECK(!chain_allowed(6, SOURCE_MEASURED));
	// v7+: both.
	CHECK(chain_allowed(7, SOURCE_MEASURED));
	CHECK(chain_allowed(7, SOURCE_APP_POINT));
	CHECK(chain_allowed(8, SOURCE_MEASURED));
	// Unknown source never chains.
	CHECK(!chain_allowed(7, 42));
}

static void test_screen_to_uv()
{
	Vec2 uv;
	// Full screen 1920x1080: Unity's bottom-left origin -> top-left, v down.
	CHECK(screen_to_canvas_uv(0, 1080, 1920, 1080, 0, 0, 0, 0, 0, &uv));
	CHECK(near(uv.x, 0.0f) && near(uv.y, 0.0f)); // top-left
	CHECK(screen_to_canvas_uv(1920, 0, 1920, 1080, 0, 0, 0, 0, 0, &uv));
	CHECK(near(uv.x, 1.0f) && near(uv.y, 1.0f)); // bottom-right
	CHECK(screen_to_canvas_uv(960, 270, 1920, 1080, 0, 0, 0, 0, 0, &uv));
	CHECK(near(uv.x, 0.5f) && near(uv.y, 0.75f));
	CHECK(uv_in_canvas(uv));

	// App zone (client px, top-left origin): bottom half of a 1920x1080 window.
	// A pointer in the top half is OFF that canvas.
	CHECK(screen_to_canvas_uv(960, 270, 1920, 1080, 1, 0, 540, 1920, 540, &uv));
	CHECK(near(uv.x, 0.5f) && near(uv.y, 0.5f)); // y_top = 810 -> (810-540)/540
	CHECK(screen_to_canvas_uv(960, 810, 1920, 1080, 1, 0, 540, 1920, 540, &uv));
	CHECK(uv.y < 0.0f && !uv_in_canvas(uv));
	// Zone with an x offset.
	CHECK(screen_to_canvas_uv(1500, 540, 1920, 1080, 1, 1000, 0, 500, 1080, &uv));
	CHECK(near(uv.x, 1.0f) && near(uv.y, 0.5f));

	// A scaled native-Wayland output (200 %): Unity's 600x483 LOGICAL screen is a
	// 1200x966 px window whose app zone is its bottom 75 % (0,242 1200x724 px). The
	// pointer must reach client px before the zone mapping.
	{
		float mx = 300.0f, my = 241.5f, sw = 600.0f, sh = 483.0f;
		screen_to_client_px(1200.0f, 966.0f, &mx, &my, &sw, &sh);
		CHECK(near(mx, 600.0f) && near(my, 483.0f) && near(sw, 1200.0f) && near(sh, 966.0f));
		CHECK(screen_to_canvas_uv(mx, my, sw, sh, 1, 0, 242, 1200, 724, &uv));
		CHECK(near(uv.x, 0.5f) && near(uv.y, (483.0f - 242.0f) / 724.0f) && uv_in_canvas(uv));
		// Unscaled, the same pointer maps to the wrong place (the bug this fixes).
		CHECK(screen_to_canvas_uv(300.0f, 241.5f, 600.0f, 483.0f, 1, 0, 242, 1200, 724, &uv));
		CHECK(near(uv.x, 0.25f) && !near(uv.y, (483.0f - 242.0f) / 724.0f));
		// Unknown client size: unchanged.
		float a = 10.0f, b = 20.0f, c = 600.0f, d = 483.0f;
		screen_to_client_px(0.0f, 966.0f, &a, &b, &c, &d);
		CHECK(near(a, 10.0f) && near(b, 20.0f) && near(c, 600.0f) && near(d, 483.0f));
	}

	// Degenerate inputs never produce a UV.
	CHECK(!screen_to_canvas_uv(1, 1, 0, 1080, 0, 0, 0, 0, 0, &uv));
	CHECK(!screen_to_canvas_uv(1, 1, 1920, 0, 0, 0, 0, 0, 0, &uv));
	CHECK(!screen_to_canvas_uv(1, 1, 1920, 1080, 1, 0, 0, 0, 100, &uv));
	CHECK(!screen_to_canvas_uv(NAN, 1, 1920, 1080, 0, 0, 0, 0, 0, &uv));

	// Off-screen pointers are reported, not clamped.
	CHECK(screen_to_canvas_uv(-10, 500, 1920, 1080, 0, 0, 0, 0, 0, &uv));
	CHECK(!uv_in_canvas(uv));
}

static void test_flip_z()
{
	const Vec3 p = {1.0f, 2.0f, 3.0f};
	CHECK(near3(flip_z(p), {1.0f, 2.0f, -3.0f}));
	CHECK(near3(flip_z(flip_z(p)), p)); // its own inverse
}

static void test_footprint_uv()
{
	Vec2 pts[kFootprintMaxPoints];
	const uint32_t n = footprint_uv({0.5f, 0.5f}, 0.03f, 16.0f / 9.0f, pts);
	CHECK(n == 10); // hotspot + 8 ring + body centre
	CHECK(near(pts[0].x, 0.5f) && near(pts[0].y, 0.5f));
	// Body centre: hotspot + (0.3 h / aspect, 0.5 h).
	CHECK(near(pts[9].x, 0.5f + 0.3f * 0.03f / (16.0f / 9.0f)));
	CHECK(near(pts[9].y, 0.5f + 0.5f * 0.03f));
	// Ring points lie at radius 0.75 h around the centre (in aspect-corrected units).
	for (int k = 1; k <= 8; k++) {
		const float du = (pts[k].x - pts[9].x) * (16.0f / 9.0f);
		const float dv = pts[k].y - pts[9].y;
		CHECK(near(sqrtf(du * du + dv * dv), 0.75f * 0.03f, 1e-5f));
	}
	// A centred body has no separate centre point.
	CHECK(footprint_uv({0.5f, 0.5f}, 0.03f, 1.0f, pts, Vec2{0, 0}) == 9);
	// Bad aspect falls back to 1, still produces points.
	CHECK(footprint_uv({0.5f, 0.5f}, 0.03f, 0.0f, pts) == 10);
}

static void test_view_ray()
{
	// Canvas 0.4 x 0.3 m at z = 0, eye 0.6 m in front, off-axis by +0.03 m.
	const View v = make_view(0.03f, 0.6f, 0.4f, 0.3f);
	CHECK(near(canvas_aspect(v), 0.4f / 0.3f, 1e-4f));
	// The four corners and the centre of the canvas, as seen through this view.
	struct { float u, v, x, y; } cases[] = {
	    {0.0f, 0.0f, -0.2f, 0.15f},  {1.0f, 0.0f, 0.2f, 0.15f}, {0.0f, 1.0f, -0.2f, -0.15f},
	    {1.0f, 1.0f, 0.2f, -0.15f},  {0.5f, 0.5f, 0.0f, 0.0f},  {0.25f, 0.75f, -0.1f, -0.075f},
	};
	for (const auto &c : cases) {
		const Ray r = view_ray(v, c.u, c.v);
		CHECK(near(sqrtf(dot(r.dir, r.dir)), 1.0f));
		CHECK(near3(hit_canvas(r), {c.x, c.y, 0.0f}, 1e-4f));
	}
	// A rotated view (yaw 90 deg about +Y): the -Z forward becomes -X.
	View rv = make_view(0.0f, 1.0f, 2.0f, 2.0f);
	const float s = sinf(0.785398163f);
	rv.orientation = {0.0f, s, 0.0f, s}; // 90 deg about Y
	const Ray rr = view_ray(rv, 0.5f, 0.5f);
	CHECK(near3(rr.dir, {-1.0f, 0.0f, 0.0f}, 1e-5f));
}

static void test_footprint_rays()
{
	View views[2] = {make_view(-0.032f, 0.6f, 0.4f, 0.3f), make_view(0.032f, 0.6f, 0.4f, 0.3f)};
	Ray rays[kFootprintMaxRays];
	CHECK(footprint_rays(views, 1, {0.5f, 0.5f}, 0.03f, rays) == 0); // 2D: nothing to hint
	CHECK(footprint_rays(nullptr, 2, {0.5f, 0.5f}, 0.03f, rays) == 0);
	const uint32_t n = footprint_rays(views, 2, {0.5f, 0.5f}, 0.03f, rays);
	CHECK(n == 20);
	// Rays come in (left, right) pairs through the SAME canvas point: both eyes'
	// rays through the hotspot meet the canvas plane at the hotspot.
	CHECK(near3(rays[0].origin, views[0].position));
	CHECK(near3(rays[1].origin, views[1].position));
	CHECK(near3(hit_canvas(rays[0]), hit_canvas(rays[1]), 1e-5f));
	CHECK(near3(hit_canvas(rays[0]), {0.0f, 0.0f, 0.0f}, 1e-5f));
	for (uint32_t i = 0; i + 1 < n; i += 2) CHECK(near3(hit_canvas(rays[i]), hit_canvas(rays[i + 1]), 1e-5f));
}

static void test_nearest_hit()
{
	Vec3 out = {};
	// Identity view looks down -Z: nearer the viewer = LARGER z = smaller dot(p, -Z).
	const Vec3 hits[] = {{0, 0, -0.10f}, {0, 0, 0.05f}, {0, 0, 0.02f}, {NAN, 0, 1.0f}};
	CHECK(nearest_hit(hits, 4, {0, 0, 0, 1}, &out));
	CHECK(near3(out, {0, 0, 0.05f}));
	CHECK(!nearest_hit(hits, 0, {0, 0, 0, 1}, &out));
	const Vec3 bad[] = {{NAN, NAN, NAN}};
	CHECK(!nearest_hit(bad, 1, {0, 0, 0, 1}, &out));

	// End to end: Unity-world hits from the C# side are flipped into the locate space,
	// then the nearest is chosen. Unity +Z is INTO the screen, so a Unity point at
	// z = -0.05 (towards the viewer) is the nearest.
	const Vec3 unity_hits[] = {{0, 0, 0.10f}, {0, 0, -0.05f}};
	Vec3 oxr[2] = {flip_z(unity_hits[0]), flip_z(unity_hits[1])};
	CHECK(nearest_hit(oxr, 2, {0, 0, 0, 1}, &out));
	CHECK(near3(flip_z(out), unity_hits[1]));
}

int main()
{
	test_gate();
	test_screen_to_uv();
	test_flip_z();
	test_footprint_uv();
	test_view_ray();
	test_footprint_rays();
	test_nearest_hit();
	printf("cursor_depth_math: %d passed, %d failed\n", s_pass, s_fail);
	return s_fail == 0 ? 0 : 1;
}
