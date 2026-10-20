// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// XR_DXR_mcp_tools binding for the display provider: lets a Unity app register its
// own MCP tools so an AI agent can operate the app (play an animation, speak a
// line), not just inspect its window.
//
// Threading. The runtime pushes an XrEventDataMCPToolCallDXR when an agent calls a
// tool, and waits up to 5 s for xrSubmitMCPToolResultDXR. The provider pumps
// xrPollEvent on the GRAPHICS thread (Windows/Linux) or the main thread (macOS), so
// the event is never handled where app code runs. Instead, this module fetches the
// call's arguments at poll time and queues {callId, toolName, args}; the C# driver
// drains the queue on the main thread every LateUpdate and runs the handler there,
// then submits the result. The runtime's five entry points are internally locked
// (one global lock in oxr_mcp_app_tools.c), so submitting from the main thread
// while the graphics thread polls is safe.
//
// Registration outlives the session. The app id and tool list are stored here and
// re-applied on every session create, so an editor dock/undock or a session restart
// does not silently drop the app's tools (the v2.9.0 lesson: state pushed before
// session start must survive a restart).
//
// All strings crossing the C# boundary are UTF-8 byte buffers, never ANSI-marshalled
// .NET strings: on Windows the default DllImport string marshalling is the ANSI code
// page, which mangles any non-ASCII description or result (e.g. Chinese).

#pragma once

#include <stdint.h>
#include "displayxr_provider_session.h" // DISPLAYXR_EXPORT

#include <openxr/openxr.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- Provider-internal hooks (called from displayxr_provider_session.cpp) ----

typedef void (*DxrMcpLogFn)(const char *fmt, ...);

//! After xrCreateInstance: soft-resolve the five entry points. @p enabled says
//! whether XR_DXR_mcp_tools was enabled on the instance.
void dxr_mcp_on_instance(XrInstance instance, PFN_xrGetInstanceProcAddr gipa, int enabled, DxrMcpLogFn log);
//! After xrCreateSession: apply the stored app id and re-register every tool.
void dxr_mcp_on_session_created(XrSession session);
//! Before xrDestroySession: forget the session and drop queued calls (the runtime
//! fails their agents itself on destroy).
void dxr_mcp_on_session_destroying(void);
//! From the xrPollEvent loop: returns 1 if @p ev was a tool call (queued), else 0.
int dxr_mcp_handle_event(const XrEventDataBuffer *ev);

// ---- C# surface (DisplayXRProviderNative / DisplayXRMcp) ----
//
// Return codes are XrResult values (0 = XR_SUCCESS), plus the binding's own codes
// below — chosen far outside the XrResult range so a real runtime error (e.g.
// XR_ERROR_VALIDATION_FAILURE = -1) can never be mistaken for one of them:
#define DXR_MCP_STORED            2000000001  //!< stored; applied when the session (re)starts
#define DXR_MCP_ERR_UNAVAILABLE (-2000000001) //!< extension not available / not enabled
#define DXR_MCP_ERR_BAD_ARGUMENT (-2000000002) //!< null, too long, invalid name, buffer too small

//! 1 when a session is up, the extension is enabled AND the runtime's MCP server is
//! running (the capability gate passed); 0 otherwise. A pure query, no side effects.
DISPLAYXR_EXPORT int dxr_prov_mcp_state(void);

//! Set the app id (`^[a-z0-9][a-z0-9-]{0,31}$`, = the .displayxr.json manifest id).
DISPLAYXR_EXPORT int dxr_prov_mcp_set_app_id(const uint8_t *app_id_utf8);

//! Register (or replace) a tool. @p schema_utf8 may be NULL (= `{"type":"object"}`).
DISPLAYXR_EXPORT int dxr_prov_mcp_register_tool(const uint8_t *name_utf8, const uint8_t *description_utf8,
                                                const uint8_t *schema_utf8);

//! Unregister a tool (also from the stored list).
DISPLAYXR_EXPORT int dxr_prov_mcp_unregister_tool(const uint8_t *name_utf8);

//! Peek the oldest queued call. Returns 1 and fills the id and the byte sizes
//! (each including the NUL) when one is queued, else 0.
DISPLAYXR_EXPORT int dxr_prov_mcp_peek_call(uint64_t *call_id, uint32_t *name_size, uint32_t *args_size);

//! Pop the oldest queued call into the caller's buffers (sizes from peek).
//! Returns 1 on success, 0 if the queue is empty or @p call_id is not at its head,
//! DXR_MCP_ERR_BAD_ARGUMENT if a buffer is too small.
DISPLAYXR_EXPORT int dxr_prov_mcp_pop_call(uint64_t call_id, uint8_t *name_buf, uint32_t name_cap, uint8_t *args_buf,
                                           uint32_t args_cap);

//! Answer a call. @p result_json_utf8 should be a JSON object (NULL = `{}`).
//! @p success = 0 (a failed handler, e.g. `{"error":"<message>"}`) is delivered as an MCP
//! tool result carrying `"isError":true` plus the app's payload, NOT as XR_FALSE: the
//! runtime drops the payload of an XR_FALSE submit and the agent would only see a
//! generic JSON-RPC "tool handler failed". See mark_error() in the .cpp.
DISPLAYXR_EXPORT int dxr_prov_mcp_submit_result(uint64_t call_id, int success, const uint8_t *result_json_utf8);

#ifdef __cplusplus
}
#endif
