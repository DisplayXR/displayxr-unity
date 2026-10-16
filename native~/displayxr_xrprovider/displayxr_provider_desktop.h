// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// XR_DXR_desktop_control binding for the display provider (runtime ADR-056): the
// app's AI agent observes and operates the user's desktop — lists windows, finds
// what is under the pointer, reads UI text, activates / moves / closes windows,
// types, presses keys and clicks into other apps.
//
// The consent belongs to the APP. The runtime runs every operation inside this
// process through the OS's own permission path (macOS Accessibility, the GNOME
// RemoteDesktop portal, a DisplayXR prompt on Windows), so the OS asks the user
// about this app by name. Nothing here re-exports that to other processes — and the
// C# side must never route it through DisplayXRMcp (any local process can call MCP
// tools; ADR-056 D4).
//
// Threading. Observation calls (state, enumerate, pointer, pick, text-at) and
// submit/request are thin synchronous pass-throughs called from the C# main thread;
// the runtime guarantees they never block on an action in flight. Action results and
// consent changes arrive as events through xrPollEvent, which the provider pumps on
// the GRAPHICS thread (Windows/Linux) or the main thread (macOS); this module copies
// them into a bounded queue that C# drains every LateUpdate. A separate call lock
// serialises the C# pass-throughs against session destroy, so no runtime call is
// ever made with a session that is being destroyed.
//
// Every string crossing the C# boundary is UTF-8 bytes (Windows DllImport defaults
// to the ANSI code page, which would mangle titles, typed text and messages).
//
// The Dxr* structs below are the C# ABI: blittable, fixed layout, no pointers.
// Their sizes and offsets are pinned by static_asserts in the .cpp and mirrored by
// the unsafe structs in Runtime/DisplayXRDesktop.cs.

#pragma once

#include <stdint.h>
#include "displayxr_provider_session.h" // DISPLAYXR_EXPORT

#include <openxr/openxr.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- Return codes -----------------------------------------------------------
//
// Every export that talks to the runtime returns the runtime's XrResult unchanged
// (0 = XR_SUCCESS; XR_ERROR_DESKTOP_CONTROL_NOT_GRANTED_DXR, ..._REFUSED_DXR,
// XR_ERROR_VALIDATION_FAILURE, XR_ERROR_LIMIT_REACHED, ...). The binding's own
// failures use values far outside every XrResult range so they can never be
// confused with one (the MCP binding's -1/-2 collide with
// XR_ERROR_VALIDATION_FAILURE / XR_ERROR_RUNTIME_FAILURE; do not copy that).
#define DXR_DESKTOP_E_UNAVAILABLE (-2000000001)  //!< extension not advertised / not enabled / entry points missing
#define DXR_DESKTOP_E_NO_SESSION (-2000000002)   //!< no XR session right now
#define DXR_DESKTOP_E_BAD_ARGUMENT (-2000000003) //!< null pointer where one is required

//! Event kinds returned by dxr_prov_desktop_next_event.
#define DXR_DESKTOP_EVENT_ACTION_COMPLETED 1
#define DXR_DESKTOP_EVENT_STATE_CHANGED 2

// ---- Blittable C# ABI structs -------------------------------------------------

//! XrDesktopWindowDXR minus type/next, reordered for natural alignment. 432 bytes.
typedef struct DxrDesktopWindow
{
	uint64_t window_id;        //   0  never 0 for a real window
	uint64_t parent_window_id; //   8  owner of a dialog / transient; 0 if none
	uint64_t state;            //  16  XrDesktopWindowStateFlagsDXR
	uint32_t pid;              //  24
	uint32_t z_order;          //  28  0 = top-most of the listed windows
	int32_t x, y;              //  32  frame, desktop coordinates
	int32_t width, height;     //  40
	uint8_t app_id[128];       //  48  UTF-8, NUL-terminated
	uint8_t title[256];        // 176  UTF-8, NUL-terminated
} DxrDesktopWindow;

//! XrDesktopPickDXR minus type/next. 448 bytes.
typedef struct DxrDesktopPick
{
	int32_t x, y;            //  0  desktop coordinates
	int32_t kind;            //  8  XrDesktopPickKindDXR
	int32_t reserved;        // 12
	DxrDesktopWindow window; // 16  valid when kind == WINDOW (0)
} DxrDesktopPick;

//! The scalar fields of XrDesktopActionDXR; text/keys travel as separate UTF-8
//! pointers on the submit call. 64 bytes.
typedef struct DxrDesktopAction
{
	uint64_t target_window_id;          //  0
	int32_t action_type;                //  8  XrDesktopActionTypeDXR
	int32_t rect_x, rect_y;             // 12  MOVE_RESIZE
	int32_t rect_width, rect_height;    // 20  (0 = keep size)
	int32_t x, y;                       // 28  pointer actions
	int32_t x2, y2;                     // 36  DRAG end
	int32_t button;                     // 44  XrDesktopPointerButtonDXR
	uint32_t click_count;               // 48
	int32_t scroll_x, scroll_y;         // 52  notches, +right / +down
	int32_t reserved;                   // 60
} DxrDesktopAction;

//! One queued runtime event. 280 bytes.
typedef struct DxrDesktopEvent
{
	uint64_t action_id; //  0  ACTION_COMPLETED
	int32_t kind;       //  8  DXR_DESKTOP_EVENT_*
	int32_t result;     // 12  ACTION_COMPLETED: the action's XrResult
	int32_t state;      // 16  STATE_CHANGED: XrDesktopControlStateDXR
	int32_t reserved;   // 20
	uint8_t message[256]; // 24  ACTION_COMPLETED: UTF-8 detail, NUL-terminated
} DxrDesktopEvent;

// ---- Provider-internal hooks (called from displayxr_provider_session.cpp) ----

typedef void (*DxrDesktopLogFn)(const char *fmt, ...);

//! After xrCreateInstance: soft-resolve the seven entry points. @p enabled says
//! whether XR_DXR_desktop_control was enabled on the instance.
void dxr_desktop_on_instance(XrInstance instance, PFN_xrGetInstanceProcAddr gipa, int enabled, DxrDesktopLogFn log);
//! After xrCreateSession.
void dxr_desktop_on_session_created(XrSession session);
//! Before xrDestroySession: forget the session and drop queued events (the runtime
//! drops its queued actions without events on destroy, spec §4).
void dxr_desktop_on_session_destroying(void);
//! From the xrPollEvent loop: returns 1 if @p ev was a desktop-control event
//! (queued or dropped), else 0.
int dxr_desktop_handle_event(const XrEventDataBuffer *ev);

// ---- C# surface (DisplayXRDesktop) ----------------------------------------------

//! 1 when a session is up and the extension is enabled with all seven entry points.
DISPLAYXR_EXPORT int dxr_prov_desktop_available(void);

//! Bumped on every session create and destroy. C# fails the tasks of actions
//! submitted under an older generation ("session ended").
DISPLAYXR_EXPORT uint32_t dxr_prov_desktop_session_generation(void);

//! Current consent state + backend capabilities. On any failure *state is
//! UNSUPPORTED (0) and *capabilities 0.
DISPLAYXR_EXPORT int dxr_prov_desktop_get_state(int32_t *state, uint64_t *capabilities);

//! Ask for consent. @p reason_utf8 may be NULL or empty (no reason shown).
DISPLAYXR_EXPORT int dxr_prov_desktop_request(const uint8_t *reason_utf8);

//! Two-call idiom, straight through to xrEnumerateDesktopWindowsDXR: capacity 0
//! (windows may be NULL) returns the count; a short capacity returns
//! XR_ERROR_SIZE_INSUFFICIENT with the required count.
DISPLAYXR_EXPORT int dxr_prov_desktop_enumerate_windows(uint32_t capacity, uint32_t *count, DxrDesktopWindow *windows);

DISPLAYXR_EXPORT int dxr_prov_desktop_get_pointer(DxrDesktopPick *pick);
DISPLAYXR_EXPORT int dxr_prov_desktop_pick(int32_t x, int32_t y, DxrDesktopPick *pick);

//! Two-call idiom over xrGetDesktopTextAtDXR; @p count includes the NUL.
DISPLAYXR_EXPORT int dxr_prov_desktop_text_at(int32_t x, int32_t y, uint32_t capacity, uint32_t *count,
                                              uint8_t *buffer_utf8);

//! Queue an action. @p text_utf8 (TYPE_TEXT) and @p keys_utf8 (KEY_CHORD) may be
//! NULL when the action does not use them. On success *action_id is never 0.
DISPLAYXR_EXPORT int dxr_prov_desktop_submit(const DxrDesktopAction *action, const uint8_t *text_utf8,
                                             const uint8_t *keys_utf8, uint64_t *action_id);

//! Pop the oldest queued event. Returns 1 and fills @p ev, or 0 when empty.
DISPLAYXR_EXPORT int dxr_prov_desktop_next_event(DxrDesktopEvent *ev);

#ifdef __cplusplus
}
#endif
