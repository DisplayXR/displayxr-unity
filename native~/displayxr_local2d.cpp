// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Local2D overlay (#439/#491) — implementation. See displayxr_local2d.h.
//
// This file now owns only the single pending Unity texture + pixel rect (set
// from C#). Under the custom IUnityXRDisplay provider the Local2D layer is
// driven by the provider itself (ps_submit_local2d in
// displayxr_provider_session.cpp, fed by dxr_prov_get_local2d_bridge /
// dxr_prov_set_local2d_rect); these setters remain for the C# P/Invoke surface.
//
// The former hooked submission path (which acquired/copied/released swapchain
// images through the GraphicsBackend abstraction + s_real_* hook
// function-pointers) was removed in the Task-3 hook-backend cleanup (#166) — it
// had no live callers once the OpenXR-hook session was gone.

#include "displayxr_local2d.h"

#include "displayxr_extensions.h"
#include "displayxr_shared_state.h"
#include "displayxr_native_shared.h"

#include <stdio.h>
#include <string.h>

namespace {

struct PendingState {
	void * volatile native_tex;
	volatile int width;
	volatile int height;
	volatile int rect_x;
	volatile int rect_y;
	volatile int rect_w;
	volatile int rect_h;
};

PendingState s_pending = {};

} // anonymous namespace

// =============================================================================
// C ABI exports
// =============================================================================

extern "C" void
displayxr_local2d_set_texture(void *unity_native_tex, int width, int height)
{
	s_pending.native_tex = unity_native_tex;
	s_pending.width = width;
	s_pending.height = height;
	displayxr_log("[DisplayXR] local2d_set_texture: tex=%p %dx%d\n",
	    unity_native_tex, width, height);
}

extern "C" void
displayxr_local2d_set_rect(int x, int y, int width, int height)
{
	s_pending.rect_x = x;
	s_pending.rect_y = y;
	s_pending.rect_w = width;
	s_pending.rect_h = height;
}

extern "C" void
displayxr_local2d_clear(void)
{
	s_pending.native_tex = nullptr;
	displayxr_log("[DisplayXR] local2d_clear\n");
}

// Read the pending Unity texture (mirrors displayxr_window_space_ui_get_pending).
// The provider's Metal Local2D arm (ps_submit_local2d) blits this straight into its
// overlay swapchain image (same device). The destination pixel rect is NOT read here
// — it comes from provider state (dxr_prov_set_local2d_rect). Returns 1 iff a texture
// of non-zero size is registered.
extern "C" int
displayxr_local2d_get_pending(void **out_tex, int *out_w, int *out_h)
{
	void *tex = s_pending.native_tex;
	if (out_tex) *out_tex = tex;
	if (out_w)   *out_w   = s_pending.width;
	if (out_h)   *out_h   = s_pending.height;
	return (tex != nullptr && s_pending.width > 0 && s_pending.height > 0) ? 1 : 0;
}
