// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading.Tasks;
using UnityEngine;

namespace DisplayXR
{
    /// <summary>
    /// Expose your app to AI agents as <b>MCP tools</b> (<c>XR_DXR_mcp_tools</c>).
    ///
    /// <para>
    /// The DisplayXR runtime hosts an MCP (Model Context Protocol) server inside every app
    /// process. Out of the box it only answers questions about the display and the window.
    /// With this class the app adds its own verbs — <c>play_animation</c>, <c>say</c>,
    /// <c>load_model</c> — so any MCP client (Claude, a voice assistant, a test script) can
    /// operate the app through the same standard protocol, without the app linking an MCP
    /// library, opening a port, or knowing which agent is on the other end.
    /// </para>
    ///
    /// <code>
    /// void Start()
    /// {
    ///     DisplayXRMcp.SetAppId("my-avatar");          // = the .displayxr.json manifest id
    ///     DisplayXRMcp.RegisterTool(
    ///         "wave",
    ///         "Make the character wave at the user. Returns {\"ok\":true}.",
    ///         "{\"type\":\"object\",\"properties\":{}}",
    ///         args => { animator.SetTrigger("Wave"); return "{\"ok\":true}"; });
    /// }
    /// </code>
    ///
    /// <para><b>Threading.</b> Handlers always run on Unity's main thread (drained from the
    /// provider driver's <c>LateUpdate</c>), so they may touch any Unity object. An async
    /// handler may <c>await</c>; the result is submitted when its task completes.</para>
    ///
    /// <para><b>Answer within 5 seconds.</b> The runtime fails a call to the agent after
    /// 5 s. Long work (a spoken reply, a download) should start, then return
    /// <c>{"started":true}</c> and expose a status tool, rather than block.</para>
    ///
    /// <para><b>Errors.</b> Throw from a handler to fail the call: the agent receives a tool
    /// error carrying the exception message.</para>
    ///
    /// <para><b>When nothing happens.</b> Registration is a no-op (and returns false from
    /// <see cref="IsAvailable"/>) when the runtime is older than the extension or the machine
    /// has MCP turned off (Windows: the DisplayXR MCP Tools installer; Linux:
    /// <c>~/.config/displayxr/capabilities/mcp/Enabled</c> = <c>1</c>; any OS:
    /// <c>DISPLAYXR_MCP=1</c>). Never make app behaviour depend on it. Tools may be
    /// registered at any time, including before the XR session starts; they are re-applied
    /// on every session (re)start.</para>
    ///
    /// <para><b>Security.</b> Any process of the same user on the machine can connect to the
    /// app's MCP endpoint and call its tools. Expose verbs scoped to <i>your app</i>. Do not
    /// re-export privileged capabilities the app holds on the user's behalf (file access,
    /// typing into other apps, a remote-desktop session the user approved for this app):
    /// that would hand them to every local process.</para>
    /// </summary>
    public static class DisplayXRMcp
    {
        /// <summary>A synchronous tool handler: JSON arguments in, JSON object out.</summary>
        public delegate string Handler(string argsJson);

        /// <summary>An asynchronous tool handler: JSON arguments in, JSON object out.</summary>
        public delegate Task<string> AsyncHandler(string argsJson);

        private static readonly Dictionary<string, AsyncHandler> s_Handlers = new Dictionary<string, AsyncHandler>();
        private static bool s_Missing; // native binary predates the binding

        /// <summary>
        /// True while a session is up, the runtime supports the extension and the machine's
        /// MCP capability is on — i.e. agents can actually see the registered tools.
        /// </summary>
        public static bool IsAvailable
        {
            get
            {
                if (s_Missing) return false;
                try { return Native.dxr_prov_mcp_state() == 1; }
                catch (EntryPointNotFoundException) { s_Missing = true; return false; }
                catch (DllNotFoundException) { s_Missing = true; return false; }
            }
        }

        /// <summary>
        /// Set the app id agents see tools under (<c>&lt;app-id&gt;__&lt;tool&gt;</c> through the
        /// workspace aggregator). Must match <c>^[a-z0-9][a-z0-9-]{0,31}$</c> and the <c>id</c>
        /// in the app's <c>.displayxr.json</c> manifest. Call once, before registering tools.
        /// </summary>
        public static bool SetAppId(string appId)
        {
            int r = Call(() => Native.dxr_prov_mcp_set_app_id(Utf8(appId)));
            if (r == -2) Debug.LogError($"[DisplayXR] MCP: invalid app id '{appId}' (want ^[a-z0-9][a-z0-9-]{{0,31}}$)");
            return r >= 0;
        }

        /// <summary>
        /// Register (or replace) a tool with a synchronous handler.
        /// </summary>
        /// <param name="name">Bare tool name, <c>^[a-z0-9][a-z0-9_-]{0,62}$</c>, no <c>__</c>.</param>
        /// <param name="description">Agent-facing documentation: what it does, what the
        /// arguments mean, units, side effects. The agent decides whether to call the tool
        /// from this text alone.</param>
        /// <param name="inputSchemaJson">JSON Schema of the arguments object, or null for none.</param>
        /// <param name="handler">Runs on the main thread; returns a JSON object.</param>
        /// <returns>False only for an invalid name or description. True also when MCP is
        /// unavailable (the tool is stored and applied if a later session has it).</returns>
        public static bool RegisterTool(string name, string description, string inputSchemaJson, Handler handler)
        {
            if (handler == null) throw new ArgumentNullException(nameof(handler));
            return RegisterTool(name, description, inputSchemaJson, args => Task.FromResult(handler(args)));
        }

        /// <summary>Register (or replace) a tool with an asynchronous handler.</summary>
        public static bool RegisterTool(string name, string description, string inputSchemaJson, AsyncHandler handler)
        {
            if (handler == null) throw new ArgumentNullException(nameof(handler));
            int r = Call(() => Native.dxr_prov_mcp_register_tool(Utf8(name), Utf8(description),
                                                                inputSchemaJson == null ? null : Utf8(inputSchemaJson)));
            if (r == -2)
            {
                Debug.LogError($"[DisplayXR] MCP: invalid tool '{name}' (name must match ^[a-z0-9][a-z0-9_-]{{0,62}}$ " +
                               "with no '__', and the description must not be empty)");
                return false;
            }
            s_Handlers[name] = handler;
            return true;
        }

        /// <summary>Remove a tool. In-flight calls to it fail to the agent.</summary>
        public static void UnregisterTool(string name)
        {
            s_Handlers.Remove(name);
            Call(() => Native.dxr_prov_mcp_unregister_tool(Utf8(name)));
        }

        /// <summary>
        /// Quote a .NET string as a JSON string literal (with the quotes), for building
        /// results by hand: <c>"{\"text\":" + DisplayXRMcp.JsonString(s) + "}"</c>.
        /// Non-ASCII text (Chinese, emoji) passes through unescaped, as JSON allows.
        /// </summary>
        public static string JsonString(string s)
        {
            if (s == null) return "null";
            var sb = new StringBuilder(s.Length + 2);
            sb.Append('"');
            foreach (char c in s)
            {
                switch (c)
                {
                    case '"': sb.Append("\\\""); break;
                    case '\\': sb.Append("\\\\"); break;
                    case '\n': sb.Append("\\n"); break;
                    case '\r': sb.Append("\\r"); break;
                    case '\t': sb.Append("\\t"); break;
                    default:
                        if (c < 0x20) sb.Append("\\u").Append(((int)c).ToString("x4"));
                        else sb.Append(c);
                        break;
                }
            }
            sb.Append('"');
            return sb.ToString();
        }

        // ------------------------------------------------------------------ dispatch

        /// <summary>
        /// Drain queued agent calls and run their handlers. Called by the provider driver
        /// every LateUpdate on the main thread; apps do not call this.
        /// </summary>
        internal static void Pump()
        {
            if (s_Missing) return;
            try
            {
                // Bounded: the native queue holds at most 16; never spin on a bad entry.
                for (int i = 0; i < 16; i++)
                {
                    if (Native.dxr_prov_mcp_peek_call(out ulong id, out uint nameSize, out uint argsSize) != 1) return;
                    var nameBuf = new byte[nameSize];
                    var argsBuf = new byte[argsSize];
                    if (Native.dxr_prov_mcp_pop_call(id, nameBuf, nameSize, argsBuf, argsSize) != 1) return;
                    Dispatch(id, FromUtf8(nameBuf), FromUtf8(argsBuf));
                }
            }
            catch (EntryPointNotFoundException) { s_Missing = true; }
            catch (DllNotFoundException) { s_Missing = true; }
        }

        private static async void Dispatch(ulong id, string name, string args)
        {
            if (!s_Handlers.TryGetValue(name, out AsyncHandler handler))
            {
                // E.g. after a domain reload in the editor: the native side still lists the
                // tool, but its handler is gone until the app registers it again.
                Submit(id, false, "{\"error\":" + JsonString($"no handler for '{name}' (app not ready)") + "}");
                return;
            }
            string result;
            try
            {
                result = await handler(args);
            }
            catch (Exception e)
            {
                Debug.LogWarning($"[DisplayXR] MCP tool '{name}' failed: {e.Message}");
                Submit(id, false, "{\"error\":" + JsonString(e.Message) + "}");
                return;
            }
            Submit(id, true, string.IsNullOrEmpty(result) ? "{}" : result);
        }

        private static void Submit(ulong id, bool ok, string json)
        {
            Call(() => Native.dxr_prov_mcp_submit_result(id, ok ? 1 : 0, Utf8(json)));
        }

        // ------------------------------------------------------------------ interop

        private static int Call(Func<int> f)
        {
            if (s_Missing) return -1;
            try { return f(); }
            catch (EntryPointNotFoundException) { s_Missing = true; }
            catch (DllNotFoundException) { s_Missing = true; }
            return -1;
        }

        // NUL-terminated UTF-8. Strings are passed as bytes, never as marshalled .NET
        // strings: Windows DllImport defaults to the ANSI code page, which would mangle any
        // non-ASCII description or result.
        private static byte[] Utf8(string s)
        {
            if (s == null) return null;
            int n = Encoding.UTF8.GetByteCount(s);
            var b = new byte[n + 1];
            Encoding.UTF8.GetBytes(s, 0, s.Length, b, 0);
            return b;
        }

        private static string FromUtf8(byte[] b)
        {
            int n = Array.IndexOf(b, (byte)0);
            return Encoding.UTF8.GetString(b, 0, n < 0 ? b.Length : n);
        }

        private static class Native
        {
            private const string LibName = "displayxr_unity";

            [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
            public static extern int dxr_prov_mcp_state();

            [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
            public static extern int dxr_prov_mcp_set_app_id(byte[] appIdUtf8);

            [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
            public static extern int dxr_prov_mcp_register_tool(byte[] nameUtf8, byte[] descriptionUtf8, byte[] schemaUtf8);

            [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
            public static extern int dxr_prov_mcp_unregister_tool(byte[] nameUtf8);

            [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
            public static extern int dxr_prov_mcp_peek_call(out ulong callId, out uint nameSize, out uint argsSize);

            [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
            public static extern int dxr_prov_mcp_pop_call(ulong callId, byte[] nameBuf, uint nameCap, byte[] argsBuf, uint argsCap);

            [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
            public static extern int dxr_prov_mcp_submit_result(ulong callId, int success, byte[] resultJsonUtf8);
        }
    }
}
