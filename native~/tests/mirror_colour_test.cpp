// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for displayxr_mirror_colour.h - the colour policy of the editor Game-view
// mirror of the runtime-woven texture (#480). Header-only, no Unity / D3D / runtime:
//   c++ -std=c++17 -I native~/displayxr_xrprovider native~/tests/mirror_colour_test.cpp && ./a.out
// or `-DDISPLAYXR_BUILD_TESTS=ON` and `ctest` in the CMake build.
//
// What it pins:
//   - the ONE case that needs a decoding source is a Linear project on a format-honest
//     runtime; a Gamma project and a passthrough runtime keep the plain declaration they
//     have always had (those are correct today and must not change);
//   - with the policy applied, every case shows the authored byte; without it, the Linear /
//     format-honest case shows exactly one extra encode (128 -> 188, 38 -> 108);
//   - the native blit decodes only into an encoding render-target view, and never decodes
//     linear bytes;
//   - decode-then-encode is the identity on every 8-bit value, so the fix cannot shift a
//     pixel of the woven interlace.

#include "displayxr_mirror_colour.h"

#include <stdio.h>
#include <stdlib.h>

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

// What the Game view shows for a colour authored as `authored`, through Unity's own
// (blitParams) mirror blit. `apply_policy` = 0 reproduces the pre-fix declaration.
static int unity_blit_screen(int authored, int linear, int honest, int apply_policy)
{
	const int woven = dxr_mirror_model_woven_byte(authored, linear, honest);
	const int decodes = apply_policy ? dxr_mirror_source_wants_srgb(linear, honest) : 0;
	return dxr_mirror_model_screen_byte(woven, decodes, /*target_encodes=*/linear);
}

// The same through the native shader blit into a render-target view that is / is not _SRGB.
static int native_blit_screen(int authored, int linear, int honest, int rt_srgb, int apply_policy)
{
	const int woven = dxr_mirror_model_woven_byte(authored, linear, honest);
	const int enc = dxr_mirror_woven_is_encoded(linear, honest);
	const int decodes = apply_policy ? dxr_mirror_native_blit_decodes(enc, rt_srgb) : 0;
	return dxr_mirror_model_screen_byte(woven, decodes, rt_srgb);
}

static void test_policy_truth_table()
{
	// (linear, honest) -> woven encoded?, source wants sRGB?
	CHECK(dxr_mirror_woven_is_encoded(0, 0) == 1); // Gamma, passthrough: Unity's encoded bytes
	CHECK(dxr_mirror_woven_is_encoded(0, 1) == 1); // Gamma, honest
	CHECK(dxr_mirror_woven_is_encoded(1, 0) == 0); // Linear, passthrough: LINEAR values
	CHECK(dxr_mirror_woven_is_encoded(1, 1) == 1); // Linear, honest

	CHECK(dxr_mirror_source_wants_srgb(0, 0) == 0);
	CHECK(dxr_mirror_source_wants_srgb(0, 1) == 0); // Gamma is correct today: unchanged
	CHECK(dxr_mirror_source_wants_srgb(1, 0) == 0); // linear bytes + Unity's encode: unchanged
	CHECK(dxr_mirror_source_wants_srgb(1, 1) == 1); // the one washed-out case

	// Native blit: decode only encoded bytes, only into an encoding view.
	CHECK(dxr_mirror_native_blit_decodes(1, 1) == 1);
	CHECK(dxr_mirror_native_blit_decodes(1, 0) == 0);
	CHECK(dxr_mirror_native_blit_decodes(0, 1) == 0);
	CHECK(dxr_mirror_native_blit_decodes(0, 0) == 0);
}

static void test_dxgi_helpers()
{
	CHECK(dxr_mirror_dxgi_is_srgb(29) && dxr_mirror_dxgi_is_srgb(91));
	CHECK(!dxr_mirror_dxgi_is_srgb(28) && !dxr_mirror_dxgi_is_srgb(87));
	CHECK(!dxr_mirror_dxgi_is_srgb(27) && !dxr_mirror_dxgi_is_srgb(90) && !dxr_mirror_dxgi_is_srgb(0));
	CHECK(dxr_mirror_dxgi_is_typeless(27) && dxr_mirror_dxgi_is_typeless(90));
	CHECK(!dxr_mirror_dxgi_is_typeless(28) && !dxr_mirror_dxgi_is_typeless(29));
	CHECK(!dxr_mirror_dxgi_is_typeless(87) && !dxr_mirror_dxgi_is_typeless(91));
	// The native blit's view. Typed -> itself. TYPELESS -> whichever member leaves ENCODED
	// bytes in storage: UNORM (raw) for encoded woven bytes, _SRGB (encode) for linear ones.
	CHECK(dxr_mirror_dxgi_view_format(27, 1) == 28 && dxr_mirror_dxgi_view_format(90, 1) == 87);
	CHECK(dxr_mirror_dxgi_view_format(27, 0) == 29 && dxr_mirror_dxgi_view_format(90, 0) == 91);
	for (int enc = 0; enc <= 1; enc++) {
		CHECK(dxr_mirror_dxgi_view_format(28, enc) == 28 && dxr_mirror_dxgi_view_format(29, enc) == 29);
		CHECK(dxr_mirror_dxgi_view_format(87, enc) == 87 && dxr_mirror_dxgi_view_format(91, enc) == 91);
		CHECK(dxr_mirror_dxgi_view_format(10, enc) == 10); // anything else is left alone
		for (int f = 0; f < 120; f++)
			CHECK(!dxr_mirror_dxgi_is_typeless(dxr_mirror_dxgi_view_format(f, enc)));
		// Through the chosen view of a TYPELESS target the stored byte is the authored one
		// (encoded woven: raw store, no decode; linear woven: the view's encode).
		for (int t = 27; t <= 90; t += 63) {
			const int view_srgb = dxr_mirror_dxgi_is_srgb(dxr_mirror_dxgi_view_format(t, enc));
			CHECK(dxr_mirror_native_blit_decodes(enc, view_srgb) == 0);
			CHECK(view_srgb == !enc);
		}
	}
}

static void test_transfer_function()
{
	// The oracle values the runtime's own colour probes quote (ADR-044 section 6):
	// one encode of 38 is 108, of 200 is 229.
	CHECK(dxr_mirror_model_screen_byte(38, 0, 1) == 108);
	CHECK(dxr_mirror_model_screen_byte(200, 0, 1) == 229);
	CHECK(dxr_mirror_model_screen_byte(128, 0, 1) == 188);
	CHECK(dxr_mirror_model_screen_byte(128, 1, 0) == 55); // one decode
	CHECK(dxr_mirror_model_screen_byte(0, 0, 1) == 0);
	CHECK(dxr_mirror_model_screen_byte(255, 0, 1) == 255);
	// Decode-then-encode is the identity on every 8-bit value.
	for (int b = 0; b <= 255; b++) {
		CHECK(dxr_mirror_model_screen_byte(b, 1, 1) == b);
		CHECK(dxr_mirror_model_screen_byte(b, 0, 0) == b);
	}
}

static void test_unity_blit_cases()
{
	// Gamma: correct before and after, on either runtime.
	for (int honest = 0; honest <= 1; honest++) {
		for (int b = 0; b <= 255; b++) {
			CHECK(unity_blit_screen(b, 0, honest, 0) == b);
			CHECK(unity_blit_screen(b, 0, honest, 1) == b);
		}
	}
	// Linear + format-honest: the field report. Encoded twice before, exact after.
	CHECK(unity_blit_screen(128, 1, 1, 0) == 188);
	CHECK(unity_blit_screen(38, 1, 1, 0) == 108);
	for (int b = 0; b <= 255; b++) CHECK(unity_blit_screen(b, 1, 1, 1) == b);
	// Linear + passthrough runtime: linear bytes, Unity's single encode. Unchanged, and
	// right to within the 8-bit quantisation of a linear value (coarse in the darks).
	CHECK(unity_blit_screen(128, 1, 0, 0) == unity_blit_screen(128, 1, 0, 1));
	CHECK(abs(unity_blit_screen(128, 1, 0, 1) - 128) <= 1);
	CHECK(abs(unity_blit_screen(200, 1, 0, 1) - 200) <= 1);
}

static void test_native_blit_cases()
{
	for (int b = 0; b <= 255; b++) {
		// Into a UNORM view nothing converts, whatever the project: unchanged.
		CHECK(native_blit_screen(b, 0, 1, 0, 0) == b);
		CHECK(native_blit_screen(b, 0, 1, 0, 1) == b);
		CHECK(native_blit_screen(b, 0, 0, 0, 1) == b);
		// Encoded bytes into an _SRGB view: exact once the shader decodes.
		CHECK(native_blit_screen(b, 1, 1, 1, 1) == b);
		CHECK(native_blit_screen(b, 0, 1, 1, 1) == b);
	}
	CHECK(native_blit_screen(128, 1, 1, 1, 0) == 188); // before
	// Linear bytes (passthrough runtime) into an _SRGB view: the view's encode is the one
	// they need - no shader decode.
	CHECK(native_blit_screen(128, 1, 0, 1, 0) == native_blit_screen(128, 1, 0, 1, 1));
	CHECK(abs(native_blit_screen(128, 1, 0, 1, 1) - 128) <= 1);
}

int main()
{
	test_policy_truth_table();
	test_dxgi_helpers();
	test_transfer_function();
	test_unity_blit_cases();
	test_native_blit_cases();
	printf("mirror_colour_test: %d passed, %d failed\n", s_pass, s_fail);
	return s_fail ? 1 : 0;
}
