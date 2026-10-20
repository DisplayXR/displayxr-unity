// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// XR_DXR_mcp_tools binding — see displayxr_provider_mcp.h for the threading model.

#include "displayxr_provider_mcp.h"
#include "../displayxr_extensions.h"

#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include <string.h>

namespace {

struct StoredTool
{
	std::string name, description, schema;
};

struct QueuedCall
{
	uint64_t id;
	std::string name, args;
};

// One lock for everything below. Taken by the graphics thread (poll) and the main
// thread (C# calls); never held across a runtime call that could block on agents
// (registration broadcasts tools/list_changed, so those run outside it).
std::mutex g_mx;

DxrMcpLogFn g_log = nullptr;
int g_enabled = 0;   // extension enabled on the instance and all five PFNs resolved
int g_gate_off = 0;  // the runtime answered XR_ERROR_FEATURE_UNSUPPORTED (capability off)
XrSession g_session = XR_NULL_HANDLE;

std::string g_app_id;
std::vector<StoredTool> g_tools;
std::deque<QueuedCall> g_queue;

// The runtime holds at most 16 pending calls per process; anything beyond that is
// failed to the agent there. Mirror the bound so a stalled main thread cannot grow
// this queue without limit.
constexpr size_t kMaxQueued = 16;
// Arguments are agent-authored JSON; refuse anything absurd rather than allocate it.
constexpr uint32_t kMaxArgsBytes = 1u << 20;

PFN_xrSetMCPAppInfoDXR pfn_set_app_info = nullptr;
PFN_xrRegisterMCPToolDXR pfn_register = nullptr;
PFN_xrUnregisterMCPToolDXR pfn_unregister = nullptr;
PFN_xrGetMCPToolCallArgsDXR pfn_get_args = nullptr;
PFN_xrSubmitMCPToolResultDXR pfn_submit = nullptr;

#define MCP_LOG(...)                                                                                                   \
	do {                                                                                                               \
		if (g_log) g_log(__VA_ARGS__);                                                                                 \
	} while (0)

// Bare tool name per the spec: ^[a-z0-9][a-z0-9_-]{0,62}$, no "__". Checked here so a
// bad name fails at the C# call with a clear code instead of at the next restart.
bool
name_valid(const char *s)
{
	if (!s || !*s) return false;
	size_t n = strlen(s);
	if (n > 63) return false;
	for (size_t i = 0; i < n; i++) {
		char c = s[i];
		bool alnum = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
		if (i == 0 ? !alnum : !(alnum || c == '_' || c == '-')) return false;
	}
	return strstr(s, "__") == nullptr;
}

bool
app_id_valid(const char *s)
{
	if (!s || !*s) return false;
	size_t n = strlen(s);
	if (n > 32) return false;
	for (size_t i = 0; i < n; i++) {
		char c = s[i];
		bool alnum = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
		if (i == 0 ? !alnum : !(alnum || c == '-')) return false;
	}
	return true;
}

void
note_result(XrResult r, const char *what, const char *name)
{
	if (r == XR_ERROR_FEATURE_UNSUPPORTED) {
		if (!g_gate_off)
			MCP_LOG("[DisplayXR-PROV] MCP: capability is off on this machine (no agent surface; "
			        "enable the DisplayXR MCP capability or set DISPLAYXR_MCP=1)\n");
		g_gate_off = 1;
	} else if (XR_FAILED(r)) {
		MCP_LOG("[DisplayXR-PROV] MCP: %s '%s' failed (XrResult %d)\n", what, name ? name : "", (int)r);
	}
}

XrResult
apply_app_id(XrSession s, const std::string &id)
{
	XrMCPAppInfoDXR info = {XR_TYPE_MCP_APP_INFO_DXR};
	strncpy(info.appId, id.c_str(), sizeof(info.appId) - 1);
	XrResult r = pfn_set_app_info(s, &info);
	note_result(r, "set app id", id.c_str());
	return r;
}

XrResult
apply_tool(XrSession s, const StoredTool &t)
{
	XrMCPToolInfoDXR info = {XR_TYPE_MCP_TOOL_INFO_DXR};
	info.name = t.name.c_str();
	info.description = t.description.c_str();
	info.inputSchemaJson = t.schema.empty() ? nullptr : t.schema.c_str();
	XrResult r = pfn_register(s, &info);
	if (r == XR_ERROR_NAME_DUPLICATED) {
		// Replace semantics: the runtime refuses duplicates, so swap it out.
		pfn_unregister(s, t.name.c_str());
		r = pfn_register(s, &info);
	}
	note_result(r, "register tool", t.name.c_str());
	return r;
}

const char *
cstr(const uint8_t *p)
{
	return reinterpret_cast<const char *>(p);
}

// A failed call is answered as a SUCCESSFUL submit whose JSON carries "isError":true.
//
// Why not xrSubmitMCPToolResultDXR(..., XR_FALSE, ...): the runtime (oxr_mcp_app_tools.c,
// runtime <= v2.47) discards resultJson on XR_FALSE and hands the MCP framework a NULL
// result, which the framework answers as a bare JSON-RPC error {-32000, "tool handler
// failed"}. The handler's message never reaches the agent, and the agent cannot tell a
// tool-level failure ("unknown behavior 'nope'" — retry with other arguments) from a
// transport failure. Submitting XR_TRUE keeps the message; "isError":true marks it the
// way MCP marks a tool error (CallToolResult.isError), so an agent reading the payload
// sees the failure, and a framework that lifts the flag onto the result (the runtime-side
// fix) needs no change here.
//
// @p json is what the app passed (a JSON object such as {"error":"..."}; NULL/empty
// allowed). Returns that object with "isError":true as its first member; a non-object
// payload is wrapped as {"isError":true,"error":<payload>}.
std::string
mark_error(const char *json)
{
	static const char kFlag[] = "{\"isError\":true";
	const char *p = json ? json : "";
	while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
	if (!*p) return std::string(kFlag) + ",\"error\":\"tool failed\"}";
	if (*p != '{') return std::string(kFlag) + ",\"error\":" + p + "}";
	const char *body = p + 1;
	const char *q = body;
	while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') q++;
	if (*q == '}') return std::string(kFlag) + ",\"error\":\"tool failed\"" + q; // {} -> generic message
	return std::string(kFlag) + "," + body;
}

XrResult
submit_error(XrSession s, uint64_t id, const char *json)
{
	std::string out = mark_error(json);
	return pfn_submit(s, id, XR_TRUE, out.c_str());
}

} // namespace

// ---------------------------------------------------------------- provider hooks

void
dxr_mcp_on_instance(XrInstance instance, PFN_xrGetInstanceProcAddr gipa, int enabled, DxrMcpLogFn log)
{
	std::lock_guard<std::mutex> lk(g_mx);
	g_log = log;
	g_enabled = 0;
	g_gate_off = 0;
	pfn_set_app_info = nullptr;
	pfn_register = nullptr;
	pfn_unregister = nullptr;
	pfn_get_args = nullptr;
	pfn_submit = nullptr;
	if (!enabled || !instance || !gipa) return;

	PFN_xrVoidFunction fn = nullptr;
	gipa(instance, "xrSetMCPAppInfoDXR", &fn);
	pfn_set_app_info = (PFN_xrSetMCPAppInfoDXR)fn;
	fn = nullptr;
	gipa(instance, "xrRegisterMCPToolDXR", &fn);
	pfn_register = (PFN_xrRegisterMCPToolDXR)fn;
	fn = nullptr;
	gipa(instance, "xrUnregisterMCPToolDXR", &fn);
	pfn_unregister = (PFN_xrUnregisterMCPToolDXR)fn;
	fn = nullptr;
	gipa(instance, "xrGetMCPToolCallArgsDXR", &fn);
	pfn_get_args = (PFN_xrGetMCPToolCallArgsDXR)fn;
	fn = nullptr;
	gipa(instance, "xrSubmitMCPToolResultDXR", &fn);
	pfn_submit = (PFN_xrSubmitMCPToolResultDXR)fn;

	g_enabled = pfn_set_app_info && pfn_register && pfn_unregister && pfn_get_args && pfn_submit;
	MCP_LOG("[DisplayXR-PROV] MCP tools: %s\n", g_enabled ? "extension enabled" : "entry points missing");
}

void
dxr_mcp_on_session_created(XrSession session)
{
	std::string app_id;
	std::vector<StoredTool> tools;
	{
		std::lock_guard<std::mutex> lk(g_mx);
		g_session = session;
		g_queue.clear();
		if (!g_enabled) return;
		app_id = g_app_id;
		tools = g_tools;
	}
	// Outside the lock: registration broadcasts tools/list_changed to agents.
	if (!app_id.empty()) apply_app_id(session, app_id);
	for (const StoredTool &t : tools) {
		if (g_gate_off) break; // one FEATURE_UNSUPPORTED answers for all of them
		apply_tool(session, t);
	}
	if (!tools.empty() && !g_gate_off)
		MCP_LOG("[DisplayXR-PROV] MCP: %zu app tool(s) registered%s%s\n", tools.size(),
		        app_id.empty() ? "" : " as ", app_id.c_str());
}

void
dxr_mcp_on_session_destroying(void)
{
	std::lock_guard<std::mutex> lk(g_mx);
	g_session = XR_NULL_HANDLE;
	g_queue.clear();
}

int
dxr_mcp_handle_event(const XrEventDataBuffer *ev)
{
	if (!ev || ev->type != XR_TYPE_EVENT_DATA_MCP_TOOL_CALL_DXR) return 0;
	const XrEventDataMCPToolCallDXR *call = (const XrEventDataMCPToolCallDXR *)ev;

	XrSession s;
	{
		std::lock_guard<std::mutex> lk(g_mx);
		s = g_session;
	}
	if (!g_enabled || s == XR_NULL_HANDLE) return 1;

	std::string args;
	uint32_t size = call->argsSize;
	if (size > kMaxArgsBytes) {
		submit_error(s, call->callId, "{\"error\":\"arguments too large\"}");
		return 1;
	}
	if (size > 0) {
		std::vector<char> buf(size);
		uint32_t got = 0;
		if (XR_FAILED(pfn_get_args(s, call->callId, size, &got, buf.data()))) {
			// The call timed out or was withdrawn before we got here; nothing to answer.
			return 1;
		}
		args.assign(buf.data(), got > 0 ? strnlen(buf.data(), got) : 0);
	}
	if (args.empty()) args = "{}";

	char name[XR_MAX_MCP_TOOL_NAME_SIZE_DXR + 1] = {0};
	memcpy(name, call->toolName, XR_MAX_MCP_TOOL_NAME_SIZE_DXR);

	bool full;
	{
		std::lock_guard<std::mutex> lk(g_mx);
		full = g_queue.size() >= kMaxQueued;
		if (!full) g_queue.push_back(QueuedCall{call->callId, name, std::move(args)});
	}
	if (full) submit_error(s, call->callId, "{\"error\":\"app is busy\"}");
	return 1;
}

// ---------------------------------------------------------------- C# surface

int
dxr_prov_mcp_state(void)
{
	std::lock_guard<std::mutex> lk(g_mx);
	return (g_enabled && !g_gate_off && g_session != XR_NULL_HANDLE) ? 1 : 0;
}

int
dxr_prov_mcp_set_app_id(const uint8_t *app_id_utf8)
{
	const char *id = cstr(app_id_utf8);
	if (!app_id_valid(id)) return DXR_MCP_ERR_BAD_ARGUMENT;
	XrSession s;
	{
		std::lock_guard<std::mutex> lk(g_mx);
		g_app_id = id;
		s = g_session;
		if (!g_enabled) return s ? DXR_MCP_ERR_UNAVAILABLE : DXR_MCP_STORED;
	}
	if (s == XR_NULL_HANDLE) return DXR_MCP_STORED;
	return (int)apply_app_id(s, id);
}

int
dxr_prov_mcp_register_tool(const uint8_t *name_utf8, const uint8_t *description_utf8, const uint8_t *schema_utf8)
{
	const char *name = cstr(name_utf8);
	const char *desc = cstr(description_utf8);
	if (!name_valid(name) || !desc || !*desc) return DXR_MCP_ERR_BAD_ARGUMENT;

	StoredTool t{name, desc, schema_utf8 ? cstr(schema_utf8) : ""};
	XrSession s;
	{
		std::lock_guard<std::mutex> lk(g_mx);
		bool replaced = false;
		for (StoredTool &e : g_tools) {
			if (e.name == t.name) {
				e = t;
				replaced = true;
			}
		}
		if (!replaced) g_tools.push_back(t);
		s = g_session;
		if (!g_enabled) return s ? DXR_MCP_ERR_UNAVAILABLE : DXR_MCP_STORED;
	}
	if (s == XR_NULL_HANDLE) return DXR_MCP_STORED;
	return (int)apply_tool(s, t);
}

int
dxr_prov_mcp_unregister_tool(const uint8_t *name_utf8)
{
	const char *name = cstr(name_utf8);
	if (!name || !*name) return DXR_MCP_ERR_BAD_ARGUMENT;
	XrSession s;
	{
		std::lock_guard<std::mutex> lk(g_mx);
		for (size_t i = 0; i < g_tools.size(); i++) {
			if (g_tools[i].name == name) {
				g_tools.erase(g_tools.begin() + (long)i);
				break;
			}
		}
		s = g_session;
		if (!g_enabled) return s ? DXR_MCP_ERR_UNAVAILABLE : DXR_MCP_STORED;
	}
	if (s == XR_NULL_HANDLE) return DXR_MCP_STORED;
	XrResult r = pfn_unregister(s, name);
	note_result(r, "unregister tool", name);
	return (int)r;
}

int
dxr_prov_mcp_peek_call(uint64_t *call_id, uint32_t *name_size, uint32_t *args_size)
{
	std::lock_guard<std::mutex> lk(g_mx);
	if (g_queue.empty()) return 0;
	const QueuedCall &c = g_queue.front();
	if (call_id) *call_id = c.id;
	if (name_size) *name_size = (uint32_t)c.name.size() + 1;
	if (args_size) *args_size = (uint32_t)c.args.size() + 1;
	return 1;
}

int
dxr_prov_mcp_pop_call(uint64_t call_id, uint8_t *name_buf, uint32_t name_cap, uint8_t *args_buf, uint32_t args_cap)
{
	std::lock_guard<std::mutex> lk(g_mx);
	if (g_queue.empty() || g_queue.front().id != call_id) return 0;
	const QueuedCall &c = g_queue.front();
	if (!name_buf || !args_buf || name_cap < c.name.size() + 1 || args_cap < c.args.size() + 1) return DXR_MCP_ERR_BAD_ARGUMENT;
	memcpy(name_buf, c.name.c_str(), c.name.size() + 1);
	memcpy(args_buf, c.args.c_str(), c.args.size() + 1);
	g_queue.pop_front();
	return 1;
}

int
dxr_prov_mcp_submit_result(uint64_t call_id, int success, const uint8_t *result_json_utf8)
{
	XrSession s;
	{
		std::lock_guard<std::mutex> lk(g_mx);
		s = g_session;
		if (!g_enabled) return DXR_MCP_ERR_UNAVAILABLE;
	}
	if (s == XR_NULL_HANDLE) return DXR_MCP_ERR_UNAVAILABLE; // the session is gone; the runtime already failed the call
	if (!success) return (int)submit_error(s, call_id, cstr(result_json_utf8));
	const char *json = (result_json_utf8 && *result_json_utf8) ? cstr(result_json_utf8) : "{}";
	return (int)pfn_submit(s, call_id, XR_TRUE, json);
}
