// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// (#459) The calibration marker AS THE SHADER DRAWS IT, against its definition.
//
// The decoder is unit-tested (Tests~/GameViewCalibration) against the C# definition of the
// marker, DisplayXRGameViewCalibration.MarkerIndexAt. What reaches the screen is drawn by HLSL
// (displayxr_gvcal_shader.h). This test compiles that very source, renders it OFF SCREEN on the
// software rasteriser (D3D11 WARP: no window, no swap chain, nothing presented), reads the
// target back and compares, texel by texel, with a C++ transcription of the definition; and it
// pins the FNV-1a hashes that Tests~/GameViewCalibration pins for the C# definition
// (MarkerHash), so the three cannot drift apart unnoticed.
//
// Covered: the whole-image marker, both thin strip pairs, no marker (the woven image
// untouched everywhere), the constants gvcal_fill_constants produces, and that outside the
// strips the output is the source texture's colour.
// (#485) Also covered: the marker STAMP for Unity's own blit (displayxr_gvcal_stamp.h) - the
// decision and the hand-out rule, and the stamp draw itself: the woven canvas drawn through a
// viewport into a texture the size of the woven one, with the constants the provider uses.
// Inside the canvas every texel is the source texel, byte for byte, or the marker laid out in
// CANVAS texels; outside it nothing is drawn. Also into a TYPELESS target through a UNORM view
// (a Linear project's declaration).
// NOT covered: D3D12 root constants versus this D3D11 constant buffer (same HLSL cbuffer, laid
// out by the same compiler), the D3D12 stamp's command list and resource states, what Unity's
// own blit does with the stamped texture, and the path from the mirror RT to the screen.
//
// Windows only. Exit code 0 = passed (or no D3D11 software device: skipped, said so).

#include <d3d11.h>
#include <d3dcompiler.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "displayxr_gvcal_shader.h"
#include "displayxr_gvcal_stamp.h"   // (#485) the marker for Unity's own blit
#include "displayxr_mirror_colour.h" // (#480) the decode the shader's fl.x applies

static int s_pass = 0, s_fail = 0;
static void check(bool ok, const char *what)
{
	if (ok) s_pass++;
	else { s_fail++; printf("FAIL %s\n", what); }
}

// ---- DisplayXRGameViewCalibration, transcribed ----
static int ref_code(int t)
{
	static const int pow5[5] = {1, 5, 25, 125, 625};
	int r = t % 6;
	if (r == 0) return 5;
	return (t / 6 / pow5[r - 1]) % 5;
}
static int ref_marker_index(int x, int y, int w)
{
	int b0 = w / 3 / 6 * 6, b1 = 2 * w / 3 / 6 * 6;
	bool band = (x >= b0 && x < b0 + 24) || (x >= b1 && x < b1 + 24);
	return ref_code(band ? y : x);
}
static bool ref_in_marker(int x, int y, int w, int h, int strip, int half)
{
	if (strip < 0) return true;
	int b0 = w / 3 / 6 * 6, b1 = 2 * w / 3 / 6 * 6;
	int hr = h * (strip + 1) / 3, vc = (strip == 0 ? b0 : b1) + 12;
	int dy = y - hr, dx = x - vc;
	return (dy >= -half && dy <= half) || (dx >= -half && dx <= half);
}
// 0..5 marker colours, 6 = the scene. mode_off: no marker requested.
static int ref_index_at(int x, int y, int w, int h, int strip, int half, bool mode_off)
{
	if (mode_off || !ref_in_marker(x, y, w, h, strip, half)) return 6;
	return ref_marker_index(x, y, w);
}

// The scene the source texture holds: one flat colour that is not a marker colour.
static const uint8_t kSceneR = 64, kSceneG = 128, kSceneB = 191;

// An output pixel (R8G8B8A8) as 0..5, 6 = scene, -1 = neither.
static int classify(const uint8_t *p)
{
	auto close_to = [](int a, int b) { return a >= b - 2 && a <= b + 2; };
	if (close_to(p[0], kSceneR) && close_to(p[1], kSceneG) && close_to(p[2], kSceneB)) return 6;
	bool r = p[0] == 255, g = p[1] == 255, b = p[2] == 255;
	if ((p[0] != 0 && !r) || (p[1] != 0 && !g) || (p[2] != 0 && !b)) return -1;
	if (r && !g && !b) return 0;
	if (!r && g && !b) return 1;
	if (!r && !g && b) return 2;
	if (r && g && !b) return 3;
	if (!r && g && b) return 4;
	if (r && !g && b) return 5;
	return -1;
}

struct Gpu {
	ID3D11Device *dev = nullptr;
	ID3D11DeviceContext *ctx = nullptr;
	ID3D11VertexShader *vs = nullptr;
	ID3D11PixelShader *ps = nullptr;
	ID3D11ShaderResourceView *srv = nullptr;
	ID3D11SamplerState *smp = nullptr;
	ID3D11Buffer *cb = nullptr;
};

static bool gpu_init(Gpu &g)
{
	D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0, got;
	if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &fl, 1, D3D11_SDK_VERSION,
	                             &g.dev, &got, &g.ctx)))
		return false;
	ID3DBlob *vs = nullptr, *ps = nullptr, *err = nullptr;
	if (FAILED(D3DCompile(kDxrMirrorBlitHLSL, strlen(kDxrMirrorBlitHLSL), nullptr, nullptr, nullptr,
	                      "VSMain", "vs_5_0", 0, 0, &vs, &err)) ||
	    FAILED(D3DCompile(kDxrMirrorBlitHLSL, strlen(kDxrMirrorBlitHLSL), nullptr, nullptr, nullptr,
	                      "PSMain", "ps_5_0", 0, 0, &ps, &err))) {
		printf("FAIL the shader does not compile: %s\n", err ? (const char *)err->GetBufferPointer() : "?");
		s_fail++;
		return false;
	}
	g.dev->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g.vs);
	g.dev->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &g.ps);
	vs->Release(); ps->Release();

	// The "woven" source: 8x8 of the scene colour, BGRA like the real one.
	std::vector<uint8_t> px(8 * 8 * 4);
	for (size_t i = 0; i < px.size(); i += 4) { px[i] = kSceneB; px[i + 1] = kSceneG; px[i + 2] = kSceneR; px[i + 3] = 255; }
	D3D11_TEXTURE2D_DESC td = {};
	td.Width = td.Height = 8; td.MipLevels = td.ArraySize = 1; td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	td.SampleDesc.Count = 1; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	D3D11_SUBRESOURCE_DATA sd = {px.data(), 8 * 4, 0};
	ID3D11Texture2D *src = nullptr;
	if (FAILED(g.dev->CreateTexture2D(&td, &sd, &src))) return false;
	g.dev->CreateShaderResourceView(src, nullptr, &g.srv);
	src->Release();
	D3D11_SAMPLER_DESC sm = {};
	sm.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	sm.AddressU = sm.AddressV = sm.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	g.dev->CreateSamplerState(&sm, &g.smp);
	D3D11_BUFFER_DESC bd = {};
	bd.ByteWidth = GVCAL_CONSTANTS * 4; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	g.dev->CreateBuffer(&bd, nullptr, &g.cb);
	return g.vs && g.ps && g.srv && g.smp && g.cb;
}

// Render a w x h target with the given constants; out = w * h * 4 bytes, RGBA, row by row.
static bool gpu_render(Gpu &g, int w, int h, const unsigned mk[4], const unsigned st[4], std::vector<uint8_t> &out,
                       bool decode = false)
{
	D3D11_TEXTURE2D_DESC td = {};
	td.Width = (UINT)w; td.Height = (UINT)h; td.MipLevels = td.ArraySize = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	td.SampleDesc.Count = 1; td.BindFlags = D3D11_BIND_RENDER_TARGET;
	ID3D11Texture2D *rt = nullptr, *stage = nullptr;
	ID3D11RenderTargetView *rtv = nullptr;
	if (FAILED(g.dev->CreateTexture2D(&td, nullptr, &rt))) return false;
	g.dev->CreateRenderTargetView(rt, nullptr, &rtv);
	td.BindFlags = 0; td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	g.dev->CreateTexture2D(&td, nullptr, &stage);
	bool ok = false;
	if (rtv && stage) {
		struct { float src[4]; unsigned mk[4]; unsigned st[4]; unsigned fl[4]; } c;
		c.src[0] = 0.0f; c.src[1] = 0.0f; c.src[2] = 1.0f; c.src[3] = 1.0f;
		memcpy(c.mk, mk, sizeof(c.mk)); memcpy(c.st, st, sizeof(c.st));
		c.fl[0] = decode ? 1u : 0u; c.fl[1] = c.fl[2] = c.fl[3] = 0; // (#480) shader decode
		g.ctx->UpdateSubresource(g.cb, 0, nullptr, &c, 0, 0);
		const float red[4] = {1, 0, 0, 1};
		g.ctx->ClearRenderTargetView(rtv, red);
		g.ctx->OMSetRenderTargets(1, &rtv, nullptr);
		D3D11_VIEWPORT vp = {0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f};
		g.ctx->RSSetViewports(1, &vp);
		g.ctx->IASetInputLayout(nullptr);
		g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		g.ctx->VSSetShader(g.vs, nullptr, 0);
		g.ctx->PSSetShader(g.ps, nullptr, 0);
		g.ctx->VSSetConstantBuffers(0, 1, &g.cb);
		g.ctx->PSSetConstantBuffers(0, 1, &g.cb);
		g.ctx->PSSetShaderResources(0, 1, &g.srv);
		g.ctx->PSSetSamplers(0, 1, &g.smp);
		g.ctx->Draw(3, 0);
		g.ctx->CopyResource(stage, rt);
		D3D11_MAPPED_SUBRESOURCE m = {};
		if (SUCCEEDED(g.ctx->Map(stage, 0, D3D11_MAP_READ, 0, &m))) {
			out.resize((size_t)w * h * 4);
			for (int y = 0; y < h; y++)
				memcpy(&out[(size_t)y * w * 4], (const uint8_t *)m.pData + (size_t)y * m.RowPitch, (size_t)w * 4);
			g.ctx->Unmap(stage, 0);
			ok = true;
		}
	}
	if (stage) stage->Release();
	if (rtv) rtv->Release();
	rt->Release();
	return ok;
}

// ---- (#485) the marker stamp ----
// The woven texture of the stamp test: every texel its own colour, none of them a marker
// colour (all channels 16..195), so a texel that moved or was filtered is seen.
static void woven_texel(int x, int y, uint8_t bgra[4])
{
	bgra[0] = (uint8_t)(16 + (x * 13 + y * 17) % 180);
	bgra[1] = (uint8_t)(16 + (x * 5 + y * 11) % 180);
	bgra[2] = (uint8_t)(16 + (x * 7 + y * 3) % 180);
	bgra[3] = 255;
}

// Stamp canvas (cx, cy, cw, ch) of a tw x th woven texture into a tw x th target, exactly as
// gvstamp_draw_d3d11 does: clear, a viewport on the canvas, dxr_gvstamp_fill_constants.
// typeless: the target is B8G8R8A8_TYPELESS viewed as UNORM (what a Linear project gets).
// strip: -2 = no marker (the stamp is never drawn so, but it shows the copy alone).
static void run_stamp(Gpu &g, int tw, int th, int cx, int cy, int cw, int ch, int strip, int half, bool typeless)
{
	char what[260];
	std::vector<uint8_t> px((size_t)tw * th * 4);
	for (int y = 0; y < th; y++)
		for (int x = 0; x < tw; x++) woven_texel(x, y, &px[((size_t)y * tw + x) * 4]);
	D3D11_TEXTURE2D_DESC td = {};
	td.Width = (UINT)tw; td.Height = (UINT)th; td.MipLevels = td.ArraySize = 1; td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	td.SampleDesc.Count = 1; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	D3D11_SUBRESOURCE_DATA sd = {px.data(), (UINT)tw * 4, 0};
	ID3D11Texture2D *src = nullptr, *rt = nullptr, *stage = nullptr;
	ID3D11ShaderResourceView *srv = nullptr;
	ID3D11RenderTargetView *rtv = nullptr;
	g.dev->CreateTexture2D(&td, &sd, &src);
	if (src) g.dev->CreateShaderResourceView(src, nullptr, &srv);
	td.Format = typeless ? DXGI_FORMAT_B8G8R8A8_TYPELESS : DXGI_FORMAT_B8G8R8A8_UNORM;
	td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
	g.dev->CreateTexture2D(&td, nullptr, &rt);
	D3D11_RENDER_TARGET_VIEW_DESC rv = {};
	rv.Format = DXGI_FORMAT_B8G8R8A8_UNORM; rv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
	if (rt) g.dev->CreateRenderTargetView(rt, &rv, &rtv);
	td.BindFlags = 0; td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	g.dev->CreateTexture2D(&td, nullptr, &stage);
	snprintf(what, sizeof(what), "stamp %dx%d canvas (%d,%d %dx%d) strip %d%s: rendered", tw, th, cx, cy, cw, ch, strip,
	         typeless ? " typeless" : "");
	bool ok = srv && rtv && stage;
	std::vector<uint8_t> out;
	if (ok) {
		struct { float src[4]; unsigned mk[4]; unsigned st[4]; unsigned fl[4]; } c;
		dxr_gvstamp_fill_constants(cx, cy, cw, ch, (unsigned)tw, (unsigned)th, strip < 0 ? -1 : strip, half, c.src, c.mk, c.st, c.fl);
		if (strip == -2) c.mk[0] = 0;
		g.ctx->UpdateSubresource(g.cb, 0, nullptr, &c, 0, 0);
		const float clear[4] = {0, 0, 0, 0};
		g.ctx->ClearRenderTargetView(rtv, clear);
		g.ctx->OMSetRenderTargets(1, &rtv, nullptr);
		D3D11_VIEWPORT vp = {(float)cx, (float)cy, (float)cw, (float)ch, 0.0f, 1.0f};
		g.ctx->RSSetViewports(1, &vp);
		g.ctx->IASetInputLayout(nullptr);
		g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		g.ctx->VSSetShader(g.vs, nullptr, 0);
		g.ctx->PSSetShader(g.ps, nullptr, 0);
		g.ctx->VSSetConstantBuffers(0, 1, &g.cb);
		g.ctx->PSSetConstantBuffers(0, 1, &g.cb);
		g.ctx->PSSetShaderResources(0, 1, &srv);
		g.ctx->PSSetSamplers(0, 1, &g.smp);
		g.ctx->Draw(3, 0);
		ID3D11RenderTargetView *none = nullptr;
		g.ctx->OMSetRenderTargets(1, &none, nullptr);
		g.ctx->PSSetShaderResources(0, 1, &g.srv); // back to the flat source of the other cases
		g.ctx->CopyResource(stage, rt);
		D3D11_MAPPED_SUBRESOURCE m = {};
		ok = SUCCEEDED(g.ctx->Map(stage, 0, D3D11_MAP_READ, 0, &m));
		if (ok) {
			out.resize((size_t)tw * th * 4);
			for (int y = 0; y < th; y++)
				memcpy(&out[(size_t)y * tw * 4], (const uint8_t *)m.pData + (size_t)y * m.RowPitch, (size_t)tw * 4);
			g.ctx->Unmap(stage, 0);
		}
	}
	check(ok, what);
	if (ok) {
		long outside = 0, copied_wrong = 0, marker_wrong = 0, markers = 0;
		int bx = -1, by = -1;
		for (int y = 0; y < th; y++)
			for (int x = 0; x < tw; x++) {
				const uint8_t *p = &out[((size_t)y * tw + x) * 4]; // B, G, R, A
				bool in = x >= cx && x < cx + cw && y >= cy && y < cy + ch;
				if (!in) { if (p[0] || p[1] || p[2] || p[3]) outside++; continue; }
				// The marker is laid out in CANVAS texels: (x - cx, y - cy) of a cw x ch image.
				int want = ref_index_at(x - cx, y - cy, cw, ch, strip, half, strip == -2);
				if (want == 6) {
					uint8_t w4[4]; woven_texel(x, y, w4);
					if (memcmp(p, w4, 4) != 0) { if (copied_wrong++ == 0) { bx = x; by = y; } }
				} else {
					markers++;
					const uint8_t rgba[4] = {p[2], p[1], p[0], p[3]};
					if (classify(rgba) != want) { if (marker_wrong++ == 0) { bx = x; by = y; } }
				}
			}
		snprintf(what, sizeof(what), "stamp %dx%d canvas (%d,%d %dx%d) strip %d%s: nothing is drawn outside the canvas (%ld texels)",
		         tw, th, cx, cy, cw, ch, strip, typeless ? " typeless" : "", outside);
		check(outside == 0, what);
		snprintf(what, sizeof(what), "stamp ... strip %d%s: every other canvas texel is the woven texel, byte for byte (%ld differ, first at %d,%d)",
		         strip, typeless ? " typeless" : "", copied_wrong, bx, by);
		check(copied_wrong == 0, what);
		snprintf(what, sizeof(what), "stamp ... strip %d%s: the marker is the definition's, in canvas texels (%ld of %ld differ, first at %d,%d)",
		         strip, typeless ? " typeless" : "", marker_wrong, markers, bx, by);
		check(marker_wrong == 0 && (strip == -2 ? markers == 0 : markers > 0), what);
		if (strip == -1) check(markers == (long)cw * ch, "stamp: the whole-image marker covers the canvas and only the canvas");
	}
	if (stage) stage->Release();
	if (rtv) rtv->Release();
	if (rt) rt->Release();
	if (srv) srv->Release();
	if (src) src->Release();
}

// The decision and the hand-out rule: pure, no device.
static void stamp_logic()
{
	const unsigned S = DXR_GS_SETTLE_SUBMITS;
	check(dxr_gvstamp_decide(1, 0, S, 1, 1, 0, 0, 1120, 595, 3840, 2160) == DXR_GS_STAMPED, "decide: Unity's own blit is live, a canvas inside its texture: stamp");
	check(dxr_gvstamp_decide(0, 0, S, 1, 1, 0, 0, 1120, 595, 3840, 2160) == DXR_GS_DISABLED, "decide: the opt-out wins over everything");
	check(dxr_gvstamp_decide(1, 1, S, 1, 1, 0, 0, 1120, 595, 3840, 2160) == DXR_GS_NATIVE_LIVE, "decide: not while Unity calls the native blit (the marker is drawn there)");
	check(dxr_gvstamp_decide(1, 0, S, 0, 1, 0, 0, 1120, 595, 3840, 2160) == DXR_GS_NO_BACKEND, "decide: not on a renderer without a stamp");
	check(dxr_gvstamp_decide(1, 0, S, 1, 0, 0, 0, 1120, 595, 3840, 2160) == DXR_GS_NO_WOVEN, "decide: not without a mirror texture registered with Unity");
	check(dxr_gvstamp_decide(1, 0, S - 1, 1, 1, 0, 0, 1120, 595, 3840, 2160) == DXR_GS_SETTLING, "decide: not in a session's first frames (the native blit may still come)");
	check(dxr_gvstamp_decide(1, 0, S, 1, 1, 0, 0, 0, 0, 3840, 2160) == DXR_GS_CANVAS, "decide: an empty canvas");
	check(dxr_gvstamp_decide(1, 0, S, 1, 1, 0, 0, 127, 595, 3840, 2160) == DXR_GS_CANVAS, "decide: a canvas too small for the marker");
	check(dxr_gvstamp_decide(1, 0, S, 1, 1, 3000, 0, 1120, 595, 3840, 2160) == DXR_GS_CANVAS, "decide: a canvas that leaves its texture");
	check(dxr_gvstamp_decide(1, 0, S, 1, 1, -1, 0, 1120, 595, 3840, 2160) == DXR_GS_CANVAS, "decide: a canvas at a negative origin");
	check(dxr_gvstamp_decide(1, 0, S, 1, 1, 2720, 1565, 1120, 595, 3840, 2160) == DXR_GS_STAMPED, "decide: a canvas that ends on the texture's last texel");

	check(dxr_gvstamp_native_live(1, 100, 99) == 1 && dxr_gvstamp_native_live(1, 100, 97) == 1, "native live: it ran within the last frames");
	check(dxr_gvstamp_native_live(1, 100, 96) == 0, "native live: not once it has stopped for 4 submits");
	check(dxr_gvstamp_native_live(0, 100, 0) == 0 && dxr_gvstamp_native_live(0, 2, 0) == 0, "native live: never seen is not live, also in the first frames");

	check(dxr_gvstamp_hand_out(1, 7, 7, 1, 0, 0, 1120, 595, 0, 0, 1120, 595) == 1, "hand out: stamped for this request and this canvas");
	check(dxr_gvstamp_hand_out(0, 7, 7, 1, 0, 0, 1120, 595, 0, 0, 1120, 595) == 0, "hand out: not once the request is over (the woven texture is back at once)");
	check(dxr_gvstamp_hand_out(1, 8, 7, 1, 0, 0, 1120, 595, 0, 0, 1120, 595) == 0, "hand out: not a texture stamped for the request before (it holds the other marker)");
	check(dxr_gvstamp_hand_out(1, 7, 0, 1, 0, 0, 1120, 595, 0, 0, 1120, 595) == 0, "hand out: not before the first stamp");
	check(dxr_gvstamp_hand_out(1, 0, 0, 1, 0, 0, 1120, 595, 0, 0, 1120, 595) == 0, "hand out: request 0 is no request");
	check(dxr_gvstamp_hand_out(1, 7, 7, 0, 0, 0, 1120, 595, 0, 0, 1120, 595) == 0, "hand out: not without a texture");
	check(dxr_gvstamp_hand_out(1, 7, 7, 1, 0, 0, 1122, 595, 0, 0, 1120, 595) == 0, "hand out: not for a canvas other than the stamped one (the srcRect would not fit)");
	check(dxr_gvstamp_hand_out(1, 7, 7, 1, 4, 0, 1120, 595, 0, 0, 1120, 595) == 0, "hand out: not for a canvas that moved");

	float src[4]; unsigned mk[4], st[4], fl[4];
	dxr_gvstamp_fill_constants(0, 0, 1120, 595, 3840, 2160, 0, 6, src, mk, st, fl);
	unsigned nmk[4], nst[4];
	gvcal_fill_constants(1120, 595, 0, 6, nmk, nst);
	check(memcmp(mk, nmk, sizeof(mk)) == 0 && memcmp(st, nst, sizeof(st)) == 0,
	      "constants: the stamp lays the marker out for the canvas exactly as the native blit does for a mirror target of that size");
	check(fl[0] == 0 && fl[1] == 0 && fl[2] == 0 && fl[3] == 1, "constants: no decode, marker origin 0,0 for a canvas at the texture's corner, texels loaded not filtered");
	check(src[0] == 0.0f && src[1] == 0.0f && src[2] == 1120.0f / 3840.0f && src[3] == 595.0f / 2160.0f, "constants: the srcRect is the canvas");
	dxr_gvstamp_fill_constants(40, 24, 300, 200, 512, 384, -1, 6, src, mk, st, fl);
	check(mk[0] == 1 && fl[1] == 40 && fl[2] == 24, "constants: a canvas off the corner carries its origin; whole image");
}

// strip: -2 = no marker requested, -1 = whole image, 0 / 1 = thin pair. expect_hash 0 = not pinned.
static void run_case(Gpu &g, int w, int h, int strip, int half, uint32_t expect_hash)
{
	char what[200];
	unsigned mk[4] = {0, 0, 0, 0}, st[4] = {0, 0, 0, 0};
	bool off = strip == -2;
	if (!off) gvcal_fill_constants(w, h, strip, half, mk, st);
	std::vector<uint8_t> px;
	if (!gpu_render(g, w, h, mk, st, px)) {
		snprintf(what, sizeof(what), "%dx%d strip %d: rendered", w, h, strip);
		check(false, what);
		return;
	}
	long bad = 0, in_marker = 0;
	int bx = -1, by = -1, bgot = 0, bwant = 0;
	uint32_t hash = 2166136261u;
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			int got = classify(&px[((size_t)y * w + x) * 4]);
			int want = ref_index_at(x, y, w, h, strip, half, off);
			if (want != 6) in_marker++;
			if (got != want) { if (bad++ == 0) { bx = x; by = y; bgot = got; bwant = want; } }
			hash = (hash ^ (uint32_t)(got < 0 ? 255 : got)) * 16777619u;
		}
	snprintf(what, sizeof(what), "%dx%d strip %d half %d: every texel is the definition's (%ld differ, first at %d,%d: drew %d, want %d)",
	         w, h, strip, half, bad, bx, by, bgot, bwant);
	check(bad == 0, what);
	if (expect_hash != 0) {
		snprintf(what, sizeof(what), "%dx%d strip %d: hash %08X is the one Tests~/GameViewCalibration pins (%08X)",
		         w, h, strip, hash, expect_hash);
		check(hash == expect_hash, what);
	}
	if (off) {
		snprintf(what, sizeof(what), "%dx%d no marker: the woven image everywhere", w, h);
		check(in_marker == 0 && bad == 0, what);
	} else if (strip >= 0) {
		long t = 2 * half + 1;
		snprintf(what, sizeof(what), "%dx%d strip %d: %ld marker texels = one %ld-texel row strip + one column strip", w, h, strip, in_marker, t);
		check(in_marker == t * w + t * h - t * t, what);
	}
}

int main()
{
	// The constants, against the C# definitions (BandStarts / HorizontalStripRow / VerticalStripColumn).
	unsigned mk[4], st[4];
	gvcal_fill_constants(1120, 595, -1, 6, mk, st);
	check(mk[0] == 1 && mk[1] == 372 && mk[2] == 744 && mk[3] == 24 && st[1] == 0 && st[3] == 0, "constants: whole image, 1120 wide: bands at 372 and 744");
	gvcal_fill_constants(1120, 595, 0, 6, mk, st);
	check(mk[0] == 2 && st[0] == 198 && st[1] == 6 && st[2] == 384 && st[3] == 6, "constants: strip pair 0 of 1120x595 is row 198, column 384");
	gvcal_fill_constants(1120, 595, 1, 6, mk, st);
	check(mk[0] == 2 && st[0] == 396 && st[2] == 756, "constants: strip pair 1 of 1120x595 is row 396, column 756");
	gvcal_fill_constants(1120, 595, 0, 99, mk, st);
	check(st[1] == GVCAL_HALF_MAX && st[3] == GVCAL_HALF_MAX, "constants: the half thickness is held inside the band");
	gvcal_fill_constants(1120, 595, 7, -3, mk, st);
	check(mk[0] == 2 && st[0] == 396 && st[1] == 0, "constants: out-of-range arguments are clamped");

	stamp_logic(); // (#485) pure: runs with or without a device

	Gpu g;
	if (!gpu_init(g)) {
		if (s_fail == 0) { printf("gvcal_shader_test: SKIPPED (no D3D11 software device); %d constant checks passed\n", s_pass); return 0; }
		printf("gvcal_shader_test: %d passed, %d failed\n", s_pass, s_fail);
		return 1;
	}

	// The hashes are DisplayXRGameViewCalibration.MarkerHash, pinned in Tests~/GameViewCalibration.
	run_case(g, 300, 200, -1, 6, 0xD8CD3005u);
	run_case(g, 300, 200, 0, 6, 0xB91460BBu);
	run_case(g, 300, 200, 1, 6, 0xBCC352AFu);
	run_case(g, 1120, 595, -1, 6, 0xE0A30EBAu);
	run_case(g, 1120, 595, 0, 6, 0x4FABC829u);
	run_case(g, 1120, 595, 1, 6, 0x09F212EDu);
	// More sizes and thicknesses, against the transcription only.
	const int sizes[][2] = {{128, 128}, {1728, 860}, {1924, 1020}, {3840, 2160}, {595, 1120}, {131, 257}};
	for (const auto &s : sizes)
		for (int strip = -2; strip <= 1; strip++)
			run_case(g, s[0], s[1], strip, 6, 0);
	run_case(g, 1120, 595, 0, 0, 0);
	run_case(g, 1120, 595, 1, 11, 0);
	run_case(g, 1120, 595, 0, 1, 0);

	// (#480) fl.x = 1: the woven sample is decoded (sRGB EOTF) and the marker is not. The
	// target here is UNORM, so what is read back is the decoded value itself.
	{
		unsigned dmk[4], dst[4];
		gvcal_fill_constants(300, 200, 0, 6, dmk, dst);
		std::vector<uint8_t> px;
		bool ok = gpu_render(g, 300, 200, dmk, dst, px, true);
		check(ok, "decode: rendered");
		if (ok) {
			const uint8_t *scene = &px[(size_t)(5 * 300 + 5) * 4];                  // outside both strips
			const uint8_t *mark = &px[(size_t)(gvcal_strip_row(0, 200) * 300 + 5) * 4]; // on the row strip
			auto near_to = [](int a, int b) { return a >= b - 1 && a <= b + 1; };
			const int want[3] = {dxr_mirror_model_screen_byte(kSceneR, 1, 0), dxr_mirror_model_screen_byte(kSceneG, 1, 0),
			                     dxr_mirror_model_screen_byte(kSceneB, 1, 0)};
			char what[200];
			snprintf(what, sizeof(what), "decode: the scene (%d,%d,%d) is stored as its sRGB decode (%d,%d,%d), drew (%d,%d,%d)",
			         kSceneR, kSceneG, kSceneB, want[0], want[1], want[2], scene[0], scene[1], scene[2]);
			check(near_to(scene[0], want[0]) && near_to(scene[1], want[1]) && near_to(scene[2], want[2]), what);
			check(classify(mark) == ref_marker_index(5, gvcal_strip_row(0, 200), 300), "decode: the marker colours are untouched");
		}
	}

	// (#485) The marker stamp: the canvas at the texture's corner (the default glue) and off
	// it (the zone-glue experiments), thin strips, the whole image, no marker, both
	// declarations of the target.
	run_stamp(g, 512, 384, 0, 0, 300, 200, 0, 6, false);
	run_stamp(g, 512, 384, 0, 0, 300, 200, 1, 6, false);
	run_stamp(g, 512, 384, 0, 0, 300, 200, -1, 6, false);
	run_stamp(g, 512, 384, 0, 0, 300, 200, -2, 6, false);
	run_stamp(g, 512, 384, 40, 24, 300, 200, 0, 6, false);
	run_stamp(g, 512, 384, 40, 24, 300, 200, -1, 6, false);
	run_stamp(g, 512, 384, 212, 184, 300, 200, 1, 6, false); // ends on the last texel
	run_stamp(g, 512, 384, 0, 0, 512, 384, 0, 6, false);     // the canvas is the whole texture
	run_stamp(g, 512, 384, 0, 0, 300, 200, 0, 6, true);
	run_stamp(g, 512, 384, 40, 24, 300, 200, -1, 6, true);
	run_stamp(g, 2048, 1280, 0, 0, 1120, 595, 0, 6, false);  // a size seen on hardware
	run_stamp(g, 2048, 1280, 0, 0, 1121, 595, 1, 6, true);

	printf("gvcal_shader_test: %d passed, %d failed\n", s_pass, s_fail);
	return s_fail == 0 ? 0 : 1;
}
