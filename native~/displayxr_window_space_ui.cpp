// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Window-space UI overlay (issue #67) — implementation.
//
// See displayxr_window_space_ui.h for the architecture overview. This file now
// owns only the single pending Unity texture + layer descriptor (set from C#).
// The custom IUnityXRDisplay provider reads that pending state via
// displayxr_window_space_ui_get_pending and drives its OWN window-space
// composition layer from its own session/device (ps_submit_wsui in
// displayxr_provider_session.cpp).
//
// The former hooked/standalone submission paths (which acquired/copied/released
// swapchain images through the GraphicsBackend abstraction + s_real_* hook
// function-pointers) were removed in the Task-3 hook-backend cleanup (#166) —
// they had no live callers once the OpenXR-hook and SA sessions were gone.

#include "displayxr_window_space_ui.h"

#include "displayxr_extensions.h"
#include "displayxr_shared_state.h"
#include "displayxr_native_shared.h"

#include <stdio.h>
#include <string.h>

namespace {

// --- Pending state (set from Unity C# game thread) ---------------------------
struct PendingState {
	void * volatile native_tex;
	volatile int width;
	volatile int height;
	volatile float x;
	volatile float y;
	volatile float w;
	volatile float h;
	volatile float disparity;
};

PendingState s_pending = {};

} // anonymous namespace

// =============================================================================
// C ABI exports — called from Unity C# via P/Invoke
// =============================================================================

extern "C" void
displayxr_window_space_ui_set_texture(void *nativeTex, int width, int height)
{
	s_pending.native_tex = nativeTex;
	s_pending.width = width;
	s_pending.height = height;
	displayxr_log("[DisplayXR] wsui_set_texture: tex=%p %dx%d\n",
	    nativeTex, width, height);
}

extern "C" void
displayxr_window_space_ui_set_layer(float x, float y,
                                     float width, float height,
                                     float disparity)
{
	s_pending.x = x;
	s_pending.y = y;
	s_pending.w = width;
	s_pending.h = height;
	s_pending.disparity = disparity;
}

extern "C" void
displayxr_window_space_ui_clear(void)
{
	s_pending.native_tex = nullptr;
	displayxr_log("[DisplayXR] wsui_clear\n");
}

extern "C" int
displayxr_window_space_ui_get_pending(void **out_tex, int *out_tex_w, int *out_tex_h,
                                      float *out_x, float *out_y,
                                      float *out_lw, float *out_lh, float *out_disp)
{
	void *tex = s_pending.native_tex;
	if (out_tex)   *out_tex   = tex;
	if (out_tex_w) *out_tex_w = s_pending.width;
	if (out_tex_h) *out_tex_h = s_pending.height;
	if (out_x)     *out_x     = s_pending.x;
	if (out_y)     *out_y     = s_pending.y;
	if (out_lw)    *out_lw    = s_pending.w;
	if (out_lh)    *out_lh    = s_pending.h;
	if (out_disp)  *out_disp  = s_pending.disparity;
	return (tex != nullptr && s_pending.width > 0 && s_pending.height > 0) ? 1 : 0;
}
