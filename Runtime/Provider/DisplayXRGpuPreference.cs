// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

using UnityEngine;

namespace DisplayXR
{
    /// <summary>
    /// App-facing GPU selection for the display provider (#242).
    ///
    /// <para><b>Why this exists.</b> The runtime tells a client which adapter to bind
    /// its session device on, defaulting to the high-performance (discrete) GPU. If
    /// Unity's own adapter pick differs, the eye bridge becomes cross-adapter and
    /// presents BLACK with a fully healthy-looking session — the #240 failure mode
    /// that cost a perf study. Since #241 the provider refuses to start in that
    /// state rather than showing black; this class is how the two are kept aligned
    /// in the first place.</para>
    ///
    /// <para><b>Two levers, two lifetimes.</b> Unity's D3D device is created during
    /// process startup, before any app script runs, so <i>nothing at runtime can move
    /// Unity's side</i> — that lever is the per-exe Windows GPU preference written at
    /// build time (Project Settings &gt; XR Plug-in Management &gt; OpenXR &gt;
    /// DisplayXR, or the Target GPU field on the manifest settings asset). The lever
    /// this class drives is the other one: which adapter the <i>runtime</i> suggests,
    /// which is read lazily at session setup and so can still be set from managed code.</para>
    ///
    /// <para><b>Auto (the default) follows Unity.</b> Rather than guessing, the plugin
    /// asks which adapter Unity actually landed on and points the runtime at the same
    /// one. On the ordinary discrete path that resolves to the runtime's existing
    /// default, so Auto changes nothing there; it only takes effect in the case that
    /// is broken without it. On a single-GPU box it is a no-op — the adapters cannot
    /// diverge, so there is nothing to steer.</para>
    ///
    /// <para><b>To request a GPU from app code</b>, set <see cref="Target"/> before XR
    /// initialization — a <c>[RuntimeInitializeOnLoadMethod]</c> with
    /// <c>BeforeSplashScreen</c> or <c>SubsystemRegistration</c> is early enough:</para>
    /// <code>
    /// [RuntimeInitializeOnLoadMethod(RuntimeInitializeLoadType.SubsystemRegistration)]
    /// static void PickGpu() =&gt; DisplayXRGpuPreference.Target = DisplayXRGpuPreference.TargetGpu.Integrated;
    /// </code>
    /// <para>Note this steers only the runtime. For Unity to <i>render</i> on that GPU
    /// too, the build must carry the matching per-exe preference — otherwise the
    /// provider will (correctly) refuse to start on the resulting mismatch. Setting
    /// Target GPU in the editor settings does both.</para>
    /// </summary>
    public static class DisplayXRGpuPreference
    {
        /// <summary>Which GPU the app wants to run on.</summary>
        public enum TargetGpu
        {
            /// <summary>Point the runtime at whichever adapter Unity is already on (default).</summary>
            Auto = 0,
            /// <summary>The discrete GPU.</summary>
            Discrete = 1,
            /// <summary>The integrated GPU.</summary>
            Integrated = 2,
        }

        /// <summary>The runtime env var that steers the runtime's suggested adapter (runtime v2.2.4+).</summary>
        const string k_EnvVar = "DXR_D3D_FORCE_GPU";

        // Native dxr_prov_unity_gpu_class() return values.
        const int k_ClassUnknown = 0, k_ClassIntegrated = 1, k_ClassDiscrete = 2;

        /// <summary>
        /// The app's requested GPU. Set before XR initialization; changing it after the
        /// session is up has no effect (the runtime reads the suggestion once, at
        /// session setup, and Unity's own device is fixed from process startup).
        /// </summary>
        public static TargetGpu Target { get; set; } = TargetGpu.Auto;

        /// <summary>
        /// Resolve <see cref="Target"/> and push it to the runtime. Called by
        /// <see cref="DisplayXRDisplayLoader.Initialize"/> before the subsystem is
        /// created — i.e. before <c>xrCreateInstance</c> loads the runtime DLL, which
        /// is the deadline for the environment write to be observed.
        ///
        /// Respects an externally-set DXR_D3D_FORCE_GPU (a developer debugging with the
        /// env var already set keeps control) and never overrides an explicit request
        /// with the Auto inference.
        /// </summary>
        internal static void Apply()
        {
            if (Application.platform != RuntimePlatform.WindowsPlayer &&
                Application.platform != RuntimePlatform.WindowsEditor)
                return; // hybrid-adapter steering is a Windows/DXGI concern

            // An env var set from outside the process wins — don't fight a developer
            // who is deliberately forcing an adapter from the shell.
            string existing = System.Environment.GetEnvironmentVariable(k_EnvVar);
            if (!string.IsNullOrEmpty(existing))
            {
                Debug.Log("[DisplayXR] Target GPU: " + k_EnvVar + "=" + existing +
                          " already set in the environment — leaving it alone.");
                return;
            }

            string value = null;
            switch (Target)
            {
                case TargetGpu.Discrete:   value = "dgpu"; break;
                case TargetGpu.Integrated: value = "igpu"; break;
                case TargetGpu.Auto:
                    // Follow Unity. 0 (unknown / single-GPU) means there is nothing to
                    // steer — leave the runtime's default suggestion untouched.
                    int cls = SafeUnityGpuClass();
                    if (cls == k_ClassIntegrated) value = "igpu";
                    else if (cls == k_ClassDiscrete) value = "dgpu";
                    break;
            }

            if (value == null)
                return;

            // MUST go through native: the runtime reads this with getenv(), which sees
            // the CRT's cached table — SetEnvironmentVariableW alone does not update it.
            int ok;
            try
            {
                ok = DisplayXRProviderNative.dxr_prov_set_env(k_EnvVar, value);
            }
            catch (System.DllNotFoundException)
            {
                return; // native plugin absent — the loader reports that separately
            }
            catch (System.EntryPointNotFoundException)
            {
                Debug.LogWarning("[DisplayXR] Target GPU: native plugin predates #242 " +
                                 "(no dxr_prov_set_env) — cannot steer the runtime's adapter.");
                return;
            }

            if (ok == 1)
                Debug.Log("[DisplayXR] Target GPU: " + Target + " → " + k_EnvVar + "=" + value +
                          (Target == TargetGpu.Auto ? " (matched to Unity's adapter)" : ""));
            else
                Debug.LogWarning("[DisplayXR] Target GPU: failed to set " + k_EnvVar + "=" + value +
                                 " — the runtime may suggest a different adapter than Unity is on.");
        }

        static int SafeUnityGpuClass()
        {
            try { return DisplayXRProviderNative.dxr_prov_unity_gpu_class(); }
            catch (System.DllNotFoundException) { return k_ClassUnknown; }
            catch (System.EntryPointNotFoundException) { return k_ClassUnknown; }
        }
    }
}
