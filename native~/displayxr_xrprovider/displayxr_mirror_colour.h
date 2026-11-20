// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Colour policy for the editor Game-view mirror of the runtime-woven texture.
//
// In editor texture mode the runtime weaves into a shared B8G8R8A8_UNORM texture and Unity
// shows it in the Game view through the XR mirror blit. Three things decide whether that
// blit is colour-correct, and they are decided in three different places:
//
//   1. WHAT THE WOVEN BYTES ARE. A format-honest runtime (ADR-044) hands its display
//      processor an ENCODED atlas and the processor emits ENCODED pixels, whatever the
//      project's colour space, so the woven texture holds encoded (display-referred) bytes
//      in a UNORM format. An older, passthrough runtime writes whatever Unity rendered: a
//      Linear project's LINEAR values (the docked swapchain was UNORM there), a Gamma
//      project's encoded ones.
//   2. WHAT UNITY'S MIRROR BLIT DOES. It samples the source as declared and stores into the
//      Game-view target as that target is declared. In a Linear project the target encodes
//      on store; in a Gamma project nothing converts.
//   3. WHAT UNITY IS TOLD THE SOURCE IS. Declared plain, it is sampled raw.
//
// Encoded bytes + raw sample + encoding store = encoded twice = washed out. That is a
// Linear project on a format-honest runtime, and nothing else. The fix is to have the
// source sampled through a DECODING view there (Unity: kUnityXRRenderTextureFlagsSRGB on a
// texture whose format allows an _SRGB view; the native blit: decode in its pixel shader
// when its render-target view encodes), so decode + encode is the identity and the Game
// view shows the woven bytes unchanged.
//
// Pure, header-only, no platform or Unity dependency: tested by
// native~/tests/mirror_colour_test.cpp.

#pragma once

#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

/// 1 when the woven texture holds ENCODED bytes, 0 when it holds linear values.
static inline int
dxr_mirror_woven_is_encoded(int linear_project, int runtime_format_honest)
{
	return (runtime_format_honest || !linear_project) ? 1 : 0;
}

/// 1 when the texture Unity's own mirror blit (blitParams) samples must be declared sRGB,
/// i.e. sampled through a decoding view. Only where the bytes are encoded AND Unity's store
/// into the Game-view target encodes.
static inline int
dxr_mirror_source_wants_srgb(int linear_project, int runtime_format_honest)
{
	return (linear_project && dxr_mirror_woven_is_encoded(linear_project, runtime_format_honest)) ? 1 : 0;
}

/// 1 when the native shader blit must decode what it samples: its render-target view
/// encodes on store (an _SRGB format) and the bytes it samples are already encoded.
static inline int
dxr_mirror_native_blit_decodes(int woven_is_encoded, int target_view_is_srgb)
{
	return (woven_is_encoded && target_view_is_srgb) ? 1 : 0;
}

/// DXGI_FORMAT ids of the 8-bit RGBA/BGRA families (numeric, so the header needs no dxgi).
static inline int
dxr_mirror_dxgi_is_srgb(int dxgi_format)
{
	return (dxgi_format == 29 /* R8G8B8A8_UNORM_SRGB */ || dxgi_format == 91 /* B8G8R8A8_UNORM_SRGB */) ? 1 : 0;
}
static inline int
dxr_mirror_dxgi_is_typeless(int dxgi_format)
{
	return (dxgi_format == 27 /* R8G8B8A8_TYPELESS */ || dxgi_format == 90 /* B8G8R8A8_TYPELESS */) ? 1 : 0;
}

/// The view format the native blit stores through, for a Game-view target of this format.
/// A typed target can only be viewed as itself. A TYPELESS one (not itself a view format) is
/// a texture Unity reads through an _SRGB view, so its storage must end up ENCODED: encoded
/// woven bytes go in raw through the UNORM member, linear ones are encoded by the _SRGB one.
static inline int
dxr_mirror_dxgi_view_format(int target_dxgi_format, int woven_is_encoded)
{
	if (target_dxgi_format == 27) return woven_is_encoded ? 28 : 29; // R8G8B8A8_TYPELESS
	if (target_dxgi_format == 90) return woven_is_encoded ? 87 : 91; // B8G8R8A8_TYPELESS
	return target_dxgi_format;
}

// --- The transfer function, and a model of the whole chain (tests + the expected values
// --- quoted in the docs; no production code path calls the model).

/// sRGB OETF (IEC 61966-2-1): linear [0,1] -> encoded [0,1].
static inline double
dxr_mirror_srgb_encode(double l)
{
	if (l <= 0.0) return 0.0;
	if (l >= 1.0) return 1.0;
	return (l <= 0.0031308) ? l * 12.92 : 1.055 * pow(l, 1.0 / 2.4) - 0.055;
}

/// sRGB EOTF: encoded [0,1] -> linear [0,1].
static inline double
dxr_mirror_srgb_decode(double e)
{
	if (e <= 0.0) return 0.0;
	if (e >= 1.0) return 1.0;
	return (e <= 0.04045) ? e / 12.92 : pow((e + 0.055) / 1.055, 2.4);
}

static inline int
dxr_mirror_to_byte(double v)
{
	if (v <= 0.0) return 0;
	if (v >= 1.0) return 255;
	return (int)(v * 255.0 + 0.5);
}

/// The byte the Game view shows for a woven-texture byte, given how the blit treats it.
///   source_decodes:  the source is sampled through a decoding (sRGB) view / shader decode.
///   target_encodes:  the store into the Game-view target encodes (Linear project, or an
///                    _SRGB render-target view).
static inline int
dxr_mirror_model_screen_byte(int woven_byte, int source_decodes, int target_encodes)
{
	double v = (double)woven_byte / 255.0;
	if (source_decodes) v = dxr_mirror_srgb_decode(v);
	if (target_encodes) v = dxr_mirror_srgb_encode(v);
	return dxr_mirror_to_byte(v);
}

/// The byte the woven texture holds for a colour AUTHORED as the display-referred byte
/// `authored` (what the artist picked; Unity linearises it in a Linear project).
/// A format-honest runtime always delivers the authored byte; a passthrough runtime
/// delivers Unity's stored value, which is linear in a Linear project (docked swapchain
/// was UNORM there).
static inline int
dxr_mirror_model_woven_byte(int authored, int linear_project, int runtime_format_honest)
{
	if (dxr_mirror_woven_is_encoded(linear_project, runtime_format_honest)) return authored;
	return dxr_mirror_to_byte(dxr_mirror_srgb_decode((double)authored / 255.0));
}

#ifdef __cplusplus
}
#endif
