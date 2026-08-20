// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;

namespace DisplayXR
{
    /// <summary>
    /// Pre-session availability probe (#257): "can this machine run DisplayXR at all?",
    /// answerable BEFORE XR-Management initialises anything — no OpenXR instance, no
    /// session, no native subsystem. Pure managed, safe from a
    /// <c>[RuntimeInitializeOnLoadMethod(RuntimeInitializeLoadType.SubsystemRegistration)]</c>
    /// bootstrap, which is the deadline for an app that wants ONE build to run 3D where
    /// a runtime is installed and plain 2D everywhere else:
    /// <code>
    /// [RuntimeInitializeOnLoadMethod(RuntimeInitializeLoadType.SubsystemRegistration)]
    /// static void PickMode()
    /// {
    ///     if (!DisplayXRRuntime.IsInstalled)
    ///     {
    ///         XRGeneralSettings.Instance.InitManagerOnStart = false; // skip XR entirely
    ///         MyApp.Use2DContent();
    ///     }
    /// }
    /// </code>
    /// <para>
    /// <see cref="DisplayXRProvider.IsRunning"/> answers a DIFFERENT question — it is
    /// post-session and only becomes true once the provider has a live OpenXR session,
    /// which is far too late to choose content. Use this type for the choice and
    /// <c>IsRunning</c> for "is stereo live right now".
    /// </para>
    /// <para>
    /// The plugin degrades gracefully on its own if an app does nothing: the XR loader
    /// runs this same probe in <c>Initialize()</c> and declines, so Unity renders
    /// normally into its own window (see the 2.13.0 changelog).
    /// </para>
    /// </summary>
    public static class DisplayXRRuntime
    {
        /// <summary>
        /// True when an OpenXR runtime manifest is resolvable on this machine — the
        /// precondition for the DisplayXR provider to get as far as
        /// <c>xrCreateInstance</c>. Deliberately the SAME resolution the native session
        /// start performs (<c>ps_resolve_runtime_json</c>): <c>XR_RUNTIME_JSON</c> if
        /// set, else the OS's active-runtime record; the manifest file must exist.
        /// <para>
        /// It does not (and cannot, pre-instance) verify that the resolved runtime is a
        /// DisplayXR one advertising <c>XR_DXR_view_rig</c> — a machine whose active
        /// OpenXR runtime is some other vendor's reports <c>true</c> here and then fails
        /// at session start. That is the same blind spot the native resolver has; the
        /// value of this API is the common case, where "no runtime record at all" is
        /// exactly the field failure it guards against.
        /// </para>
        /// Result is cached on first use — a runtime installed mid-process is not picked up.
        /// </summary>
        public static bool IsInstalled
        {
            get { EnsureProbed(); return s_Path != null; }
        }

        /// <summary>
        /// The OpenXR runtime manifest path <see cref="IsInstalled"/> resolved, or null
        /// when nothing resolved. Useful in diagnostics ("which runtime am I about to
        /// talk to?"); never needed to make the 2D-vs-3D decision.
        /// </summary>
        public static string ResolvedManifestPath
        {
            get { EnsureProbed(); return s_Path; }
        }

        /// <summary>
        /// Whether a managed probe exists for this platform at all. False means
        /// <see cref="IsInstalled"/> is uninformative (always false) and callers must NOT
        /// read a false as "no runtime" — the XR loader treats it as "don't decline".
        /// </summary>
        internal static bool ProbeSupported
        {
#if UNITY_STANDALONE_WIN || UNITY_EDITOR_WIN || UNITY_STANDALONE_OSX || UNITY_EDITOR_OSX || UNITY_STANDALONE_LINUX || UNITY_EDITOR_LINUX
            get { return true; }
#else
            get { return false; }
#endif
        }

        static bool   s_Probed;
        static string s_Path;

        static void EnsureProbed()
        {
            if (s_Probed) return;
            s_Probed = true;
            s_Path = Probe();
        }

        // Mirror of native ps_resolve_runtime_json (displayxr_provider_session.cpp).
        // The two must stay in step: the loader declines on this answer, so a probe
        // stricter than the native resolver would refuse sessions that would have
        // worked, and a looser one would re-open the invisible-window failure.
        static string Probe()
        {
            // XR_RUNTIME_JSON wins outright — OpenXR loader semantics. When it is set
            // the loader does NOT fall back to the OS record, so an env var pointing at
            // a missing file means "no runtime", not "look elsewhere".
            string env = Environment.GetEnvironmentVariable("XR_RUNTIME_JSON");
            if (!string.IsNullOrEmpty(env))
                return SafeExists(env) ? env : null;

#if UNITY_STANDALONE_WIN || UNITY_EDITOR_WIN
            // HKCU (per-user override) before HKLM (machine install), same order as
            // the OpenXR loader and the native resolver.
            string reg = ReadActiveRuntime(HKEY_CURRENT_USER)
                      ?? ReadActiveRuntime(HKEY_LOCAL_MACHINE);
            return (reg != null && SafeExists(reg)) ? reg : null;
#elif UNITY_STANDALONE_OSX || UNITY_EDITOR_OSX || UNITY_STANDALONE_LINUX || UNITY_EDITOR_LINUX
            // The single fixed path the native resolver's non-Windows branch checks.
            return SafeExists(k_UnixActiveRuntime) ? k_UnixActiveRuntime : null;
#else
            return null;
#endif
        }

        static bool SafeExists(string path)
        {
            try { return File.Exists(path); }
            catch { return false; }
        }

#if UNITY_STANDALONE_OSX || UNITY_EDITOR_OSX || UNITY_STANDALONE_LINUX || UNITY_EDITOR_LINUX
        const string k_UnixActiveRuntime = "/usr/local/share/openxr/1/active_runtime.json";
#endif

#if UNITY_STANDALONE_WIN || UNITY_EDITOR_WIN
        // advapi32 directly rather than Microsoft.Win32.Registry: that type is not part
        // of the .NET Standard profile a player is built against, so referencing it from
        // a Runtime assembly is a build-configuration hazard. RegGetValueW is the exact
        // call the native resolver makes.
        static readonly IntPtr HKEY_CURRENT_USER  = new IntPtr(unchecked((int)0x80000001));
        static readonly IntPtr HKEY_LOCAL_MACHINE = new IntPtr(unchecked((int)0x80000002));
        const uint RRF_RT_REG_SZ = 0x00000002;
        const int  ERROR_SUCCESS = 0;

        [DllImport("advapi32.dll", CharSet = CharSet.Unicode, EntryPoint = "RegGetValueW")]
        static extern int RegGetValueW(IntPtr hkey, string subKey, string value,
                                       uint flags, IntPtr type,
                                       StringBuilder data, ref uint dataSizeBytes);

        static string ReadActiveRuntime(IntPtr root)
        {
            try
            {
                var sb = new StringBuilder(1024);
                uint cb = (uint)(sb.Capacity * sizeof(char));
                int rc = RegGetValueW(root, @"SOFTWARE\Khronos\OpenXR\1", "ActiveRuntime",
                                      RRF_RT_REG_SZ, IntPtr.Zero, sb, ref cb);
                if (rc != ERROR_SUCCESS) return null;
                string s = sb.ToString();
                return string.IsNullOrEmpty(s) ? null : s;
            }
            catch (DllNotFoundException) { return null; }
            catch (EntryPointNotFoundException) { return null; }
        }
#endif
    }
}
