// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Window-space UI overlay (issue #67) — implementation.
//
// See displayxr_window_space_ui.h for the architecture overview. This file now
// owns only the pending Unity texture + layer descriptor of each wsui slot (set
// from C#).
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
//
// One entry per window-space layer. Several DisplayXRWindowSpaceUI components can be
// live at once (lenovo-avatar opens two HUD panels together); a single shared entry
// made the last one to register win and the others vanish. Each component acquires
// its own slot; the provider submits one layer per registered slot.
struct PendingState {
	void * volatile native_tex;
	volatile int width;
	volatile int height;
	volatile float x;
	volatile float y;
	volatile float w;
	volatile float h;
	volatile float disparity;
	volatile int in_use; // handed out by acquire_slot
};

PendingState s_pending[DXR_WSUI_MAX_SLOTS] = {};

inline PendingState *
slot_ptr(int slot)
{
	return (slot >= 0 && slot < DXR_WSUI_MAX_SLOTS) ? &s_pending[slot] : nullptr;
}

} // anonymous namespace

// =============================================================================
// C ABI exports — called from Unity C# via P/Invoke
// =============================================================================

extern "C" int
displayxr_window_space_ui_acquire_slot(void)
{
	// Main thread only (component OnEnable), so no atomics needed for the hand-out.
	for (int i = 0; i < DXR_WSUI_MAX_SLOTS; i++) {
		if (!s_pending[i].in_use) {
			s_pending[i] = PendingState{};
			s_pending[i].in_use = 1;
			displayxr_log("[DisplayXR] wsui: slot %d acquired\n", i);
			return i;
		}
	}
	displayxr_log("[DisplayXR] wsui: all %d slots in use — layer not shown\n", DXR_WSUI_MAX_SLOTS);
	return -1;
}

extern "C" void
displayxr_window_space_ui_release_slot(int slot)
{
	PendingState *p = slot_ptr(slot);
	if (!p) return;
	p->native_tex = nullptr;
	p->in_use = 0;
	displayxr_log("[DisplayXR] wsui: slot %d released\n", slot);
}

extern "C" void
displayxr_window_space_ui_set_texture_slot(int slot, void *nativeTex, int width, int height)
{
	PendingState *p = slot_ptr(slot);
	if (!p) return;
	p->native_tex = nativeTex;
	p->width = width;
	p->height = height;
	displayxr_log("[DisplayXR] wsui_set_texture[%d]: tex=%p %dx%d\n", slot, nativeTex, width, height);
}

extern "C" void
displayxr_window_space_ui_set_layer_slot(int slot, float x, float y, float width, float height,
                                         float disparity)
{
	PendingState *p = slot_ptr(slot);
	if (!p) return;
	p->x = x;
	p->y = y;
	p->w = width;
	p->h = height;
	p->disparity = disparity;
}

extern "C" void
displayxr_window_space_ui_clear_slot(int slot)
{
	PendingState *p = slot_ptr(slot);
	if (!p) return;
	p->native_tex = nullptr;
	displayxr_log("[DisplayXR] wsui_clear[%d]\n", slot);
}

extern "C" int
displayxr_window_space_ui_get_pending_slot(int slot, void **out_tex, int *out_tex_w, int *out_tex_h,
                                           float *out_x, float *out_y,
                                           float *out_lw, float *out_lh, float *out_disp)
{
	PendingState *p = slot_ptr(slot);
	void *tex = p ? p->native_tex : nullptr;
	if (out_tex)   *out_tex   = tex;
	if (!p) return 0;
	if (out_tex_w) *out_tex_w = p->width;
	if (out_tex_h) *out_tex_h = p->height;
	if (out_x)     *out_x     = p->x;
	if (out_y)     *out_y     = p->y;
	if (out_lw)    *out_lw    = p->w;
	if (out_lh)    *out_lh    = p->h;
	if (out_disp)  *out_disp  = p->disparity;
	return (tex != nullptr && p->width > 0 && p->height > 0) ? 1 : 0;
}

// --- Single-layer API (pre-slot callers) — slot 0 -----------------------------

extern "C" void
displayxr_window_space_ui_set_texture(void *nativeTex, int width, int height)
{
	displayxr_window_space_ui_set_texture_slot(0, nativeTex, width, height);
}

extern "C" void
displayxr_window_space_ui_set_layer(float x, float y,
                                     float width, float height,
                                     float disparity)
{
	displayxr_window_space_ui_set_layer_slot(0, x, y, width, height, disparity);
}

extern "C" void
displayxr_window_space_ui_clear(void)
{
	displayxr_window_space_ui_clear_slot(0);
}

extern "C" int
displayxr_window_space_ui_get_pending(void **out_tex, int *out_tex_w, int *out_tex_h,
                                      float *out_x, float *out_y,
                                      float *out_lw, float *out_lh, float *out_disp)
{
	return displayxr_window_space_ui_get_pending_slot(0, out_tex, out_tex_w, out_tex_h,
	                                                  out_x, out_y, out_lw, out_lh, out_disp);
}
