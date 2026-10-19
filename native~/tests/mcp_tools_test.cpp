// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Unit test of the XR_DXR_mcp_tools binding (displayxr_provider_mcp.cpp) against a
// fake runtime: registration survives a session restart, calls queue in order and
// carry UTF-8 intact, results reach the runtime, a failed handler reaches the agent as
// an isError result carrying its message, the capability gate is honoured,
// and bad input is refused before it reaches the runtime. No Unity, no real runtime.
//
//   c++ -std=c++17 -I native~/displayxr_xrprovider -I <openxr include>
//       native~/tests/mcp_tools_test.cpp native~/displayxr_xrprovider/displayxr_provider_mcp.cpp
//   (one command; see the "MCP tools binding tests" step in build-native.yml)

#include "displayxr_provider_mcp.h"
#include "../displayxr_extensions.h"

#include <map>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

static int g_fail = 0;
#define CHECK(c)                                                                                                       \
	do {                                                                                                               \
		if (!(c)) {                                                                                                    \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                               \
			g_fail++;                                                                                                  \
		}                                                                                                              \
	} while (0)

// ---------------------------------------------------------------- fake runtime

struct Fake
{
	bool gate_on = true;
	std::string app_id;
	std::map<std::string, std::string> tools; // name -> description
	std::map<uint64_t, std::string> args;     // pending call args
	std::vector<std::pair<uint64_t, std::string>> results;
	std::vector<bool> result_ok;
	int registers = 0;
} F;

static XrResult XRAPI_CALL
f_set_app(XrSession, const XrMCPAppInfoDXR *i)
{
	if (!F.gate_on) return XR_ERROR_FEATURE_UNSUPPORTED;
	F.app_id = i->appId;
	return XR_SUCCESS;
}
static XrResult XRAPI_CALL
f_register(XrSession, const XrMCPToolInfoDXR *t)
{
	if (!F.gate_on) return XR_ERROR_FEATURE_UNSUPPORTED;
	if (F.tools.count(t->name)) return XR_ERROR_NAME_DUPLICATED;
	F.tools[t->name] = t->description;
	F.registers++;
	return XR_SUCCESS;
}
static XrResult XRAPI_CALL
f_unregister(XrSession, const char *n)
{
	if (!F.gate_on) return XR_ERROR_FEATURE_UNSUPPORTED;
	F.tools.erase(n);
	return XR_SUCCESS;
}
static XrResult XRAPI_CALL
f_get_args(XrSession, uint64_t id, uint32_t cap, uint32_t *count, char *buf)
{
	auto it = F.args.find(id);
	if (it == F.args.end()) return XR_ERROR_VALIDATION_FAILURE;
	*count = (uint32_t)it->second.size() + 1;
	if (cap < *count) return XR_ERROR_SIZE_INSUFFICIENT;
	memcpy(buf, it->second.c_str(), *count);
	return XR_SUCCESS;
}
static XrResult XRAPI_CALL
f_submit(XrSession, uint64_t id, XrBool32 ok, const char *json)
{
	F.results.push_back({id, json});
	F.result_ok.push_back(ok == XR_TRUE);
	return XR_SUCCESS;
}
static XrResult XRAPI_CALL
f_gipa(XrInstance, const char *name, PFN_xrVoidFunction *fn)
{
	*fn = nullptr;
	if (!strcmp(name, "xrSetMCPAppInfoDXR")) *fn = (PFN_xrVoidFunction)f_set_app;
	if (!strcmp(name, "xrRegisterMCPToolDXR")) *fn = (PFN_xrVoidFunction)f_register;
	if (!strcmp(name, "xrUnregisterMCPToolDXR")) *fn = (PFN_xrVoidFunction)f_unregister;
	if (!strcmp(name, "xrGetMCPToolCallArgsDXR")) *fn = (PFN_xrVoidFunction)f_get_args;
	if (!strcmp(name, "xrSubmitMCPToolResultDXR")) *fn = (PFN_xrVoidFunction)f_submit;
	return *fn ? XR_SUCCESS : XR_ERROR_FUNCTION_UNSUPPORTED;
}

static void
quiet(const char *, ...)
{}

static const uint8_t *
u8(const char *s)
{
	return reinterpret_cast<const uint8_t *>(s);
}

static void
push_call(uint64_t id, const char *tool, const char *args)
{
	F.args[id] = args;
	XrEventDataBuffer ev = {XR_TYPE_EVENT_DATA_BUFFER};
	XrEventDataMCPToolCallDXR *c = (XrEventDataMCPToolCallDXR *)&ev;
	c->type = XR_TYPE_EVENT_DATA_MCP_TOOL_CALL_DXR;
	c->callId = id;
	strncpy(c->toolName, tool, sizeof(c->toolName) - 1);
	c->argsSize = (uint32_t)strlen(args) + 1;
	CHECK(dxr_mcp_handle_event(&ev) == 1);
}

static bool
pop(std::string &name, std::string &args, uint64_t &id)
{
	uint32_t ns = 0, as = 0;
	if (dxr_prov_mcp_peek_call(&id, &ns, &as) != 1) return false;
	std::vector<uint8_t> nb(ns), ab(as);
	if (dxr_prov_mcp_pop_call(id, nb.data(), ns, ab.data(), as) != 1) return false;
	name = (const char *)nb.data();
	args = (const char *)ab.data();
	return true;
}

int
main()
{
	XrInstance inst = (XrInstance)(uintptr_t)1;
	XrSession s1 = (XrSession)(uintptr_t)11, s2 = (XrSession)(uintptr_t)12;

	// --- Before any session: stored, reported as "applied later" ---------------
	dxr_mcp_on_instance(inst, f_gipa, 1, quiet);
	CHECK(dxr_prov_mcp_state() == 0);
	CHECK(dxr_prov_mcp_set_app_id(u8("lenovo-avatar")) == DXR_MCP_STORED);
	CHECK(dxr_prov_mcp_register_tool(u8("wave"), u8("Wave at the user."), nullptr) == DXR_MCP_STORED);
	CHECK(F.tools.empty());

	// --- Bad input never reaches the runtime ------------------------------------
	CHECK(dxr_prov_mcp_set_app_id(u8("Lenovo Avatar")) == DXR_MCP_ERR_BAD_ARGUMENT);
	CHECK(dxr_prov_mcp_register_tool(u8("Wave"), u8("x"), nullptr) == DXR_MCP_ERR_BAD_ARGUMENT);
	CHECK(dxr_prov_mcp_register_tool(u8("a__b"), u8("x"), nullptr) == DXR_MCP_ERR_BAD_ARGUMENT);
	CHECK(dxr_prov_mcp_register_tool(u8("ok"), u8(""), nullptr) == DXR_MCP_ERR_BAD_ARGUMENT);

	// --- Session 1: the stored id + tool are applied ----------------------------
	dxr_mcp_on_session_created(s1);
	CHECK(dxr_prov_mcp_state() == 1);
	CHECK(F.app_id == "lenovo-avatar");
	CHECK(F.tools.count("wave") == 1);

	// Live registration, with a non-ASCII description; then replace it.
	const char *zh = "让角色说一句话 😀";
	CHECK(dxr_prov_mcp_register_tool(u8("say"), u8(zh), u8("{\"type\":\"object\"}")) == 0);
	CHECK(F.tools["say"] == zh);
	CHECK(dxr_prov_mcp_register_tool(u8("say"), u8("Say a line."), nullptr) == 0);
	CHECK(F.tools["say"] == "Say a line.");

	// --- Calls queue FIFO, args intact (UTF-8), results reach the runtime -------
	push_call(101, "wave", "{}");
	push_call(102, "say", "{\"text\":\"你好\"}");
	std::string name, args;
	uint64_t id = 0;
	CHECK(pop(name, args, id) && id == 101 && name == "wave" && args == "{}");
	CHECK(pop(name, args, id) && id == 102 && name == "say" && args == "{\"text\":\"你好\"}");
	CHECK(!pop(name, args, id));
	CHECK(dxr_prov_mcp_submit_result(102, 1, u8("{\"said\":\"你好\"}")) == XR_SUCCESS);
	CHECK(F.results.back().first == 102 && F.results.back().second == "{\"said\":\"你好\"}" && F.result_ok.back());
	// A failed handler is a tool RESULT flagged isError, never an XR_FALSE submit: the
	// runtime drops an XR_FALSE payload and the agent would get only a generic JSON-RPC
	// "tool handler failed" instead of the handler's message.
	CHECK(dxr_prov_mcp_submit_result(101, 0, nullptr) == XR_SUCCESS);
	CHECK(F.results.back().second == "{\"isError\":true,\"error\":\"tool failed\"}" && F.result_ok.back());

	// --- Handler errors carry the message (UTF-8 intact) as an isError result ---
	// This is exactly what DisplayXRMcp.Dispatch submits when a C# handler throws.
	push_call(110, "wave", "{\"behavior\":\"nope\"}");
	CHECK(pop(name, args, id) && id == 110);
	CHECK(dxr_prov_mcp_submit_result(110, 0, u8("{\"error\":\"unknown behavior 'nope'\"}")) == XR_SUCCESS);
	CHECK(F.results.back().first == 110 && F.result_ok.back());
	CHECK(F.results.back().second == "{\"isError\":true,\"error\":\"unknown behavior 'nope'\"}");
	CHECK(dxr_prov_mcp_submit_result(111, 0, u8("{\"error\":\"未知动作 'nope' 😀\"}")) == XR_SUCCESS);
	CHECK(F.results.back().second == "{\"isError\":true,\"error\":\"未知动作 'nope' 😀\"}" && F.result_ok.back());
	// Extra members and leading whitespace survive; {} and "" get a generic message;
	// a non-object payload is wrapped rather than spliced.
	CHECK(dxr_prov_mcp_submit_result(112, 0, u8("  {\"error\":\"x\",\"code\":7}")) == XR_SUCCESS);
	CHECK(F.results.back().second == "{\"isError\":true,\"error\":\"x\",\"code\":7}");
	CHECK(dxr_prov_mcp_submit_result(113, 0, u8("{ }")) == XR_SUCCESS);
	CHECK(F.results.back().second == "{\"isError\":true,\"error\":\"tool failed\"}");
	CHECK(dxr_prov_mcp_submit_result(114, 0, u8("")) == XR_SUCCESS);
	CHECK(F.results.back().second == "{\"isError\":true,\"error\":\"tool failed\"}");
	CHECK(dxr_prov_mcp_submit_result(115, 0, u8("\"bad\"")) == XR_SUCCESS);
	CHECK(F.results.back().second == "{\"isError\":true,\"error\":\"bad\"}" && F.result_ok.back());
	// A successful result is passed through untouched (no isError added).
	CHECK(dxr_prov_mcp_submit_result(116, 1, u8("{\"ok\":true}")) == XR_SUCCESS);
	CHECK(F.results.back().second == "{\"ok\":true}" && F.result_ok.back());

	// pop with the wrong id or a short buffer leaves the queue intact
	push_call(103, "wave", "{\"n\":1}");
	uint8_t small[2];
	uint8_t big[64];
	CHECK(dxr_prov_mcp_pop_call(999, big, 64, big, 64) == 0);
	CHECK(dxr_prov_mcp_pop_call(103, small, 2, big, 64) == DXR_MCP_ERR_BAD_ARGUMENT);
	CHECK(pop(name, args, id) && id == 103);

	// The queue is bounded: the 17th call is failed to the agent at once.
	size_t before = F.results.size();
	for (uint64_t i = 0; i < 17; i++) push_call(200 + i, "wave", "{}");
	CHECK(F.results.size() == before + 1 && F.results.back().first == 216);
	CHECK(F.results.back().second == "{\"isError\":true,\"error\":\"app is busy\"}" && F.result_ok.back());
	for (int i = 0; i < 16; i++) CHECK(pop(name, args, id));

	// --- Unregister removes from the runtime and from the stored list -----------
	CHECK(dxr_prov_mcp_unregister_tool(u8("say")) == XR_SUCCESS);
	CHECK(F.tools.count("say") == 0);

	// --- Restart: queued calls dropped, tools re-applied to the new session -----
	push_call(300, "wave", "{}");
	dxr_mcp_on_session_destroying();
	CHECK(dxr_prov_mcp_state() == 0);
	CHECK(dxr_prov_mcp_peek_call(&id, nullptr, nullptr) == 0);
	CHECK(dxr_prov_mcp_submit_result(300, 1, nullptr) == DXR_MCP_ERR_UNAVAILABLE);
	F.tools.clear();
	F.app_id.clear();
	dxr_mcp_on_instance(inst, f_gipa, 1, quiet);
	dxr_mcp_on_session_created(s2);
	CHECK(F.app_id == "lenovo-avatar");
	CHECK(F.tools.size() == 1 && F.tools.count("wave") == 1);

	// --- Capability off: everything inert, state 0 ------------------------------
	dxr_mcp_on_session_destroying();
	F.gate_on = false;
	F.tools.clear();
	dxr_mcp_on_instance(inst, f_gipa, 1, quiet);
	dxr_mcp_on_session_created(s1);
	CHECK(dxr_prov_mcp_state() == 0);
	CHECK(F.tools.empty());

	// --- Extension not enabled (older runtime): never calls the runtime ---------
	dxr_mcp_on_session_destroying();
	F.gate_on = true;
	int regs = F.registers;
	dxr_mcp_on_instance(inst, f_gipa, 0, quiet);
	dxr_mcp_on_session_created(s1);
	CHECK(dxr_prov_mcp_state() == 0);
	CHECK(dxr_prov_mcp_register_tool(u8("late"), u8("x"), nullptr) == DXR_MCP_ERR_UNAVAILABLE);
	CHECK(F.registers == regs);

	if (g_fail) {
		fprintf(stderr, "%d check(s) failed\n", g_fail);
		return 1;
	}
	printf("mcp_tools_test: all checks passed\n");
	return 0;
}
