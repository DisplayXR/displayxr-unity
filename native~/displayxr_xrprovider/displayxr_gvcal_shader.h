// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// (#459) The native mirror blit's shaders, with the editor Game-view calibration marker.
//
// In its own header so that the provider (displayxr_display_provider.cpp, D3D12) and the
// shader test (tests/gvcal_shader_test.cpp, D3D11 on the software rasteriser) compile the
// very same source: the test renders the marker off screen and compares it with the
// definition in Runtime/Provider/DisplayXRGameViewCalibration.cs (Code / MarkerIndex /
// InMarker), which is what the decoder is unit-tested against.
//
// The constant block is 16 32-bit values, in this order (root constants on D3D12, a constant
// buffer on D3D11):
//   float4 srcRect   the woven canvas inside its texture, in UV (x, y, w, h)
//   uint4  mk        (mode, band 0 start, band 1 start, band width)
//                      mode 0 = no marker, 1 = marker over the whole image, 2 = thin strips
//   uint4  st        thin strips: (row, half, column, half) - the marker is drawn on the rows
//                      within `half` of `row` and on the columns within `half` of `column`
//   uint4  fl        (#480) fl.x != 0: decode the woven sample (sRGB EOTF) before it is
//                      stored. Set when the woven bytes are ENCODED and the target view
//                      encodes on store (an _SRGB format): displayxr_mirror_colour.h.
//                    (#485) fl.y, fl.z: the target pixel that is the marker's texel (0, 0).
//                      0, 0 for the native mirror blit (the image fills its target). The
//                      marker stamp (displayxr_gvcal_stamp.h) draws the woven canvas into a
//                      texture the size of the woven one, through a viewport on the canvas
//                      rect, and passes the canvas origin here, so the marker is laid out
//                      in CANVAS texels there exactly as it is in target texels here.
//                    (#485) fl.w != 0: the target is the size of the source and each pixel
//                      shows the source texel at its own position: Load it instead of
//                      sampling (an exact copy). The stamp sets it; the native blit, whose
//                      target is another texture of another size, never does.
#pragma once

#define GVCAL_BLOCK      6                 // texels per block (sync + 5 base-5 digits)
#define GVCAL_BAND_W     (4 * GVCAL_BLOCK) // width of each row-coded band
#define GVCAL_MIN_SIDE   128
#define GVCAL_STRIPS     2                 // strip pairs (DisplayXRGameViewCalibration.StripCount)
#define GVCAL_HALF_MAX   (GVCAL_BAND_W / 2 - 1) // a vertical strip stays inside its band
#define GVCAL_CONSTANTS  16               // 32-bit values in the constant block

// DisplayXRGameViewCalibration.BandStarts / HorizontalStripRow / VerticalStripColumn.
static inline int gvcal_band0(int w) { return w / 3 / GVCAL_BLOCK * GVCAL_BLOCK; }
static inline int gvcal_band1(int w) { return 2 * w / 3 / GVCAL_BLOCK * GVCAL_BLOCK; }
static inline int gvcal_strip_row(int k, int h) { return h * (k + 1) / (GVCAL_STRIPS + 1); }
static inline int gvcal_strip_col(int k, int w) { return (k == 0 ? gvcal_band0(w) : gvcal_band1(w)) + GVCAL_BAND_W / 2; }

// Fill mk / st for a w x h target. strip: < 0 = the whole image, 0 / 1 = the k-th strip pair.
static inline void gvcal_fill_constants(int w, int h, int strip, int half, unsigned mk[4], unsigned st[4])
{
	if (half < 0) half = 0;
	if (half > GVCAL_HALF_MAX) half = GVCAL_HALF_MAX;
	if (strip >= GVCAL_STRIPS) strip = GVCAL_STRIPS - 1;
	mk[0] = strip < 0 ? 1u : 2u;
	mk[1] = (unsigned)gvcal_band0(w);
	mk[2] = (unsigned)gvcal_band1(w);
	mk[3] = GVCAL_BAND_W;
	st[0] = st[1] = st[2] = st[3] = 0;
	if (strip >= 0) {
		st[0] = (unsigned)gvcal_strip_row(strip, h); st[1] = (unsigned)half;
		st[2] = (unsigned)gvcal_strip_col(strip, w); st[3] = (unsigned)half;
	}
}

static const char *const kDxrMirrorBlitHLSL =
	"cbuffer C : register(b0){ float4 srcRect; uint4 mk; uint4 st; uint4 fl; }\n"
	"Texture2D tex : register(t0); SamplerState smp : register(s0);\n"
	"struct V{ float4 pos:SV_Position; float2 uv:TEXCOORD0; };\n"
	"V VSMain(uint id:SV_VertexID){ V o; float2 t=float2((id<<1)&2, id&2);\n"
	"  o.uv=t; o.pos=float4(t*float2(2,-2)+float2(-1,1),0,1); return o; }\n"
	// (#459) Calibration marker = DisplayXRGameViewCalibration.Code / MarkerIndex / Rgb:
	// blocks of 6 texels, magenta sync then the block index in base 5 (LSB first) as
	// red / green / blue / yellow / cyan; columns everywhere, rows inside the two bands.
	// Where it is drawn = InMarker: everywhere (mk.x == 1), or on the strip rows and strip
	// columns only (mk.x == 2; unsigned: y + half >= row is |y - row| <= half's lower side).
	// The woven sample stays outside the branch (no gradient under flow control).
	"static const uint kPow5[5] = { 1, 5, 25, 125, 625 };\n"
	"uint Code(uint t){ uint r = t % 6; uint v = 5;\n"
	"  if (r != 0) v = ((t / 6) / kPow5[r - 1]) % 5; return v; }\n"
	// (#480) fl.x != 0: the woven bytes are ENCODED and this target view encodes on store
	// (an _SRGB format), so decode here (sRGB EOTF, as dxr_mirror_srgb_decode) and the
	// pair is the identity. The marker colours are 0 / 1, which the curve leaves alone.
	"float4 PSMain(V i):SV_Target{ float2 uv=srcRect.xy + i.uv*srcRect.zw; float4 c = tex.Sample(smp, uv);\n"
	// (#485) fl.w != 0: source and target are the same size and this pixel shows the source
	// texel at its own position, so it is LOADED, not filtered: byte for byte, whatever the
	// precision of the interpolated uv (a filtered 1:1 sample was seen one step off).
	"  if (fl.w != 0) c = tex.Load(int3((int)i.pos.x, (int)i.pos.y, 0));\n"
	"  if (fl.x != 0) { float3 e = saturate(c.rgb);\n"
	"    float3 lo = e / 12.92; float3 hi = pow((e + 0.055) / 1.055, 2.4);\n"
	"    c.rgb = float3(e.r <= 0.04045 ? lo.r : hi.r, e.g <= 0.04045 ? lo.g : hi.g, e.b <= 0.04045 ? lo.b : hi.b); }\n"
	// (#485) fl.yz = the target pixel of the marker's texel (0,0): 0,0 in the native blit.
	"  if (mk.x != 0) { uint x = (uint)i.pos.x - fl.y; uint y = (uint)i.pos.y - fl.z;\n"
	"    bool on = mk.x == 1 || (y + st.y >= st.x && y <= st.x + st.y) || (x + st.w >= st.z && x <= st.z + st.w);\n"
	"    if (on) {\n"
	"      bool band = (x >= mk.y && x < mk.y + mk.w) || (x >= mk.z && x < mk.z + mk.w);\n"
	"      uint k = Code(band ? y : x);\n"
	"      c = float4((k == 0 || k == 3 || k == 5) ? 1.0 : 0.0, (k == 1 || k == 3 || k == 4) ? 1.0 : 0.0,\n"
	"                 (k == 2 || k == 4 || k == 5) ? 1.0 : 0.0, 1.0); } }\n"
	"  return c; }\n";
