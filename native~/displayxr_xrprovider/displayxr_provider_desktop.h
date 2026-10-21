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
// them into a bounded queue that C# drains every LateUpdate (a waitable capture's
// completion goes to its waiter instead, below). A separate call lock serialises the
// C# pass-throughs against session destroy, so no runtime call is ever made with a
// session that is being destroyed; the capture calls may come from any thread.
//
// Every string crossing the C# boundary is UTF-8 bytes (Windows DllImport defaults
// to the ANSI code page, which would mangle titles, typed text and messages).
//
// Screen capture (spec v2, runtime spec §10) is a second consent SCOPE next to
// CONTROL (= everything in spec v1). It exists only when the runtime ADVERTISES
// spec version >= 2 and resolves the three capture entry points; against an older
// runtime every capture export returns DXR_DESKTOP_E_NO_CAPTURE and the v1 surface
// behaves exactly as before. A capture is asynchronous like an action: the
// completion is queued from the poll loop, and C# fetches the image (which the
// runtime keeps for 10 s) from the main thread when it drains that event.
//
// A WAITABLE capture (dxr_prov_desktop_capture_waitable + _capture_wait) is the
// same capture with its completion routed to the thread that waits for it instead
// of the event queue, so any thread can block on it, Unity's main thread too. That
// thread cannot just wait for the regular poll: on Windows/Linux the graphics
// thread polls from Unity's per-frame XR callback, which the render thread runs
// only for a frame the main thread has submitted, so a blocked main thread stops
// the poll (and on macOS the main thread IS the poller). The waiting thread
// therefore polls the runtime's event queue itself (the session module's pump,
// dxr_desktop_set_waiter_pump): desktop events are routed here as usual, every
// other event is handed back to the regular poller, in order. The pump is
// serialised with the regular poll and with session teardown, never blocks, and
// on macOS runs on the main thread only (the runtime's poll drives AppKit there).
// So on macOS a worker thread's wait ends only through the main thread's own
// per-frame poll: if the main thread is blocked on that worker (a join), nothing
// polls, and the worker waits out its whole timeout before either moves on. On
// Windows/Linux a worker polls for itself and has no such dependency.
//
// Spec v3 (runtime spec §11, ADR-056 Amendment 3) adds held keys / buttons (KEY_DOWN / KEY_UP /
// BUTTON_DOWN / BUTTON_UP), modifiers held around a CLICK / DRAG / SCROLL (a chained
// XrDesktopActionModifiersDXR), the shell target (XR_DESKTOP_TARGET_SHELL_DXR) and
// clipboard text (SET / GET_CLIPBOARD_TEXT + xrGetDesktopClipboardTextDXR). All of it
// exists only when the runtime ADVERTISES spec version >= 3. Against an older runtime
// dxr_prov_desktop_submit returns DXR_DESKTOP_E_NO_V3 untried for every v3 action type,
// for the shell target and for non-zero modifiers — never sending the modifiers struct
// to a v2 runtime matters most: it would IGNORE the struct and run a plain click.
// Clipboard text never reaches a log here.
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
#define DXR_DESKTOP_E_NO_CAPTURE (-2000000004)   //!< the runtime's extension predates spec v2 (no screen capture)
#define DXR_DESKTOP_E_TIMEOUT (-2000000005)      //!< C# only: a blocking capture's wait ran out (never returned here)
#define DXR_DESKTOP_E_NO_V3 (-2000000006)        //!< the runtime's extension predates spec v3 (held input, shell, clipboard)

//! Spec v3: the reserved target id of input for the desktop shell (= XR_DESKTOP_TARGET_SHELL_DXR).
#define DXR_DESKTOP_TARGET_SHELL 0xFFFFFFFFFFFFFFFFull

//! Event kinds returned by dxr_prov_desktop_next_event.
#define DXR_DESKTOP_EVENT_ACTION_COMPLETED 1
#define DXR_DESKTOP_EVENT_STATE_CHANGED 2
#define DXR_DESKTOP_EVENT_CAPTURE_COMPLETED 3     //!< spec v2
#define DXR_DESKTOP_EVENT_CAPTURE_STATE_CHANGED 4 //!< spec v2: the CAPTURE scope's state

//! Scope bits of dxr_prov_desktop_request_scopes (= XrDesktopControlScopeFlagsDXR).
#define DXR_DESKTOP_SCOPE_CONTROL 0x1ull
#define DXR_DESKTOP_SCOPE_CAPTURE 0x2ull
#define DXR_DESKTOP_SCOPE_ASK_AGAIN 0x100ull

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
//! pointers on the submit call. 64 bytes. `modifiers` (spec v3) took the former
//! `reserved` slot, so an older caller that zeroed it sends no modifiers.
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
	uint32_t modifiers;                 // 60  spec v3: XrDesktopKeyModifierFlagsDXR around CLICK/DRAG/SCROLL; 0 = none
} DxrDesktopAction;

//! One queued runtime event. 312 bytes (the capture fields were appended in spec v2;
//! the first 280 bytes are unchanged).
typedef struct DxrDesktopEvent
{
	uint64_t action_id; //  0  ACTION_COMPLETED: the action id; CAPTURE_COMPLETED: the capture id
	int32_t kind;       //  8  DXR_DESKTOP_EVENT_*
	int32_t result;     // 12  *_COMPLETED: the XrResult
	int32_t state;      // 16  *STATE_CHANGED: XrDesktopControlStateDXR
	int32_t reserved;   // 20
	uint8_t message[256]; // 24  *_COMPLETED: UTF-8 detail, NUL-terminated
	uint32_t width, height;     // 280  CAPTURE_COMPLETED: image size (0 on failure)
	int32_t src_x, src_y;       // 288  CAPTURE_COMPLETED: what the image covers, desktop coordinates
	int32_t src_width, src_height; // 296
	int64_t frame_time;         // 304  CAPTURE_COMPLETED: XrTime the pixels were current
} DxrDesktopEvent;

//! XrDesktopCaptureInfoDXR minus type/next. 40 bytes.
typedef struct DxrDesktopCaptureInfo
{
	uint64_t window_id;           //  0  WINDOW
	int32_t target;               //  8  XrDesktopCaptureTargetDXR
	int32_t x, y;                 // 12  REGION, desktop coordinates
	int32_t width, height;        // 20
	uint32_t max_width, max_height; // 28  1..8192: scaled down to fit, never up
	int32_t reserved;             // 36
} DxrDesktopCaptureInfo;

// ---- Provider-internal hooks (called from displayxr_provider_session.cpp) ----

typedef void (*DxrDesktopLogFn)(const char *fmt, ...);

//! After xrCreateInstance: soft-resolve the entry points. @p spec_version is the
//! extensionVersion the runtime advertised, 0 when XR_DXR_desktop_control was not
//! enabled on the instance. The seven v1 entry points gate the binding; the three
//! capture ones are resolved only for spec_version >= 2 and gate capture alone; the
//! v3 surface needs spec_version >= 3 (and the clipboard fetch its entry point).
void dxr_desktop_on_instance(XrInstance instance, PFN_xrGetInstanceProcAddr gipa, uint32_t spec_version,
                             DxrDesktopLogFn log);
//! After xrCreateSession.
void dxr_desktop_on_session_created(XrSession session);
//! Before xrDestroySession: forget the session and drop queued events (the runtime
//! drops its queued actions without events on destroy, spec §4).
void dxr_desktop_on_session_destroying(void);
//! From the xrPollEvent loop: returns 1 if @p ev was a desktop-control event
//! (queued, handed to its waiter, or dropped), else 0. Any thread.
int dxr_desktop_handle_event(const XrEventDataBuffer *ev);

//! Polls the runtime's event queue on the CALLING thread for a capture waiter and
//! returns the number of events it took (0 when it may not poll from this thread or
//! another thread is polling right now). Never blocks; routes desktop events
//! through dxr_desktop_handle_event.
typedef int (*DxrDesktopPumpFn)(void);

//! The session module's pump (NULL = none: waiters rely on the regular poll alone).
void dxr_desktop_set_waiter_pump(DxrDesktopPumpFn pump);

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

//! Spec v2 request: ask for @p scopes (DXR_DESKTOP_SCOPE_*; 0 = CONTROL) in one prompt,
//! with an optional @p parent_window_utf8 ("wayland:<handle>" / "x11:<hex>", passed
//! verbatim; NULL or "" = none). Against a v1 runtime: CONTROL is requested exactly as
//! dxr_prov_desktop_request does (no chain — ASK_AGAIN and the parent are dropped), and
//! a request for CAPTURE without CONTROL returns DXR_DESKTOP_E_NO_CAPTURE untried.
DISPLAYXR_EXPORT int dxr_prov_desktop_request_scopes(const uint8_t *reason_utf8, uint64_t scopes,
                                                     const uint8_t *parent_window_utf8);

//! 1 when a session is up and the runtime has spec-v2 screen capture.
DISPLAYXR_EXPORT int dxr_prov_desktop_capture_available(void);

//! The CAPTURE scope's state. On any failure *state is UNSUPPORTED (0).
DISPLAYXR_EXPORT int dxr_prov_desktop_get_capture_state(int32_t *state);

//! Start one capture. On success *capture_id is never 0 and exactly one
//! CAPTURE_COMPLETED event follows while the session exists.
DISPLAYXR_EXPORT int dxr_prov_desktop_capture(const DxrDesktopCaptureInfo *info, uint64_t *capture_id);

//! Two-call idiom, in BYTES, over xrGetDesktopCaptureImageDXR (count = width*height*4,
//! top row first). The runtime keeps the image until a call copies it out. Any thread.
DISPLAYXR_EXPORT int dxr_prov_desktop_capture_image(uint64_t capture_id, uint32_t capacity, uint32_t *count,
                                                    uint8_t *rgba);

//! As dxr_prov_desktop_capture, but the completion is kept for
//! dxr_prov_desktop_capture_wait instead of being queued for
//! dxr_prov_desktop_next_event. The runtime's limits apply unchanged: a second
//! capture while one is in flight returns XR_ERROR_LIMIT_REACHED at once; starts
//! closer than 250 ms are accepted and delayed. Any thread.
DISPLAYXR_EXPORT int dxr_prov_desktop_capture_waitable(const DxrDesktopCaptureInfo *info, uint64_t *capture_id);

//! Wait up to @p timeout_ms for a waitable capture's completion, polling the
//! runtime's event queue from this thread meanwhile (see the header comment).
//! Returns 1 and fills @p ev (what dxr_prov_desktop_next_event would have returned,
//! kind CAPTURE_COMPLETED), after which the capture is forgotten; 0 while it is
//! still in flight (wait again, or abandon it); DXR_DESKTOP_E_NO_SESSION when the
//! session ended (it will never complete; forgotten); DXR_DESKTOP_E_BAD_ARGUMENT for
//! an id that is not a waitable capture (any more). Any thread; returns within
//! @p timeout_ms plus one non-blocking poll. On a macOS worker thread the poll is
//! the main thread's (see the header comment): a main thread blocked on this
//! thread means 0 after the full @p timeout_ms, every time.
DISPLAYXR_EXPORT int dxr_prov_desktop_capture_wait(uint64_t capture_id, uint32_t timeout_ms, DxrDesktopEvent *ev);

//! Give up on a waitable capture: its completion, if it still comes, is dropped
//! (the runtime frees the image after 10 s or with the next capture). Any thread.
DISPLAYXR_EXPORT void dxr_prov_desktop_capture_abandon(uint64_t capture_id);

//! Two-call idiom, straight through to xrEnumerateDesktopWindowsDXR: capacity 0
//! (windows may be NULL) returns the count; a short capacity returns
//! XR_ERROR_SIZE_INSUFFICIENT with the required count.
DISPLAYXR_EXPORT int dxr_prov_desktop_enumerate_windows(uint32_t capacity, uint32_t *count, DxrDesktopWindow *windows);

DISPLAYXR_EXPORT int dxr_prov_desktop_get_pointer(DxrDesktopPick *pick);
DISPLAYXR_EXPORT int dxr_prov_desktop_pick(int32_t x, int32_t y, DxrDesktopPick *pick);

//! Two-call idiom over xrGetDesktopTextAtDXR; @p count includes the NUL.
DISPLAYXR_EXPORT int dxr_prov_desktop_text_at(int32_t x, int32_t y, uint32_t capacity, uint32_t *count,
                                              uint8_t *buffer_utf8);

//! Queue an action. @p text_utf8 (TYPE_TEXT, SET_CLIPBOARD_TEXT) and @p keys_utf8
//! (KEY_CHORD, KEY_DOWN, KEY_UP) may be NULL when the action does not use them. On
//! success *action_id is never 0. Spec v3 action types, the shell target and non-zero
//! modifiers return DXR_DESKTOP_E_NO_V3 untried unless the runtime advertised spec v3;
//! non-zero modifiers are then chained as an XrDesktopActionModifiersDXR.
DISPLAYXR_EXPORT int dxr_prov_desktop_submit(const DxrDesktopAction *action, const uint8_t *text_utf8,
                                             const uint8_t *keys_utf8, uint64_t *action_id);

//! The extensionVersion the runtime advertised for XR_DXR_desktop_control while the
//! binding is enabled (0 when not, or with no session). Apps gate spec-v3 features on >= 3.
DISPLAYXR_EXPORT uint32_t dxr_prov_desktop_spec_version(void);

//! Spec v3: two-call idiom, in BYTES (the NUL included), over
//! xrGetDesktopClipboardTextDXR for a GET_CLIPBOARD_TEXT that completed with success.
//! The runtime keeps the text until a call copies it out (a size query keeps it).
//! DXR_DESKTOP_E_NO_V3 against an older runtime. The buffer is always NUL-terminated.
DISPLAYXR_EXPORT int dxr_prov_desktop_clipboard_text(uint64_t action_id, uint32_t capacity, uint32_t *count,
                                                     uint8_t *buffer_utf8);

//! Pop the oldest queued event. Returns 1 and fills @p ev, or 0 when empty.
DISPLAYXR_EXPORT int dxr_prov_desktop_next_event(DxrDesktopEvent *ev);

#ifdef __cplusplus
}
#endif
