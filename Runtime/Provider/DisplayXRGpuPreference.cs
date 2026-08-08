// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

using UnityEngine;
using UnityEngine.Rendering; // GraphicsDeviceType — picks the D3D vs Vulkan env var (#247)

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
        const string k_EnvVarD3D = "DXR_D3D_FORCE_GPU";

        /// <summary>
        /// The Vulkan cousin (#247). Same supported contract and same in-process
        /// <c>getenv()</c> caveat (runtime #845). It steers the COMPOSITOR's
        /// VkPhysicalDevice pick, and <c>xrGetVulkanGraphicsDevice2KHR</c> then suggests
        /// the device whose UUID matches the compositor's — so setting this aligns the
        /// runtime's enable2 device with Unity's, exactly as the D3D var does for DXGI.
        /// Without it the provider's cross-adapter guard can only refuse the session.
        /// </summary>
        const string k_EnvVarVk = "DXR_VK_FORCE_GPU";

        /// <summary>
        /// Which env var applies, keyed on the graphics API Unity actually came up on.
        /// Vulkan and D3D are steered by DIFFERENT runtime variables; setting the wrong
        /// one is a silent no-op, which is how a hybrid-laptop black screen would come
        /// back wearing a new hat.
        /// </summary>
        static string EnvVarForCurrentApi()
        {
            return SystemInfo.graphicsDeviceType == GraphicsDeviceType.Vulkan
                ? k_EnvVarVk : k_EnvVarD3D;
        }

        // Native dxr_prov_unity_gpu_class() return values.
        const int k_ClassUnknown = 0, k_ClassIntegrated = 1, k_ClassDiscrete = 2;

        /// <summary>
        /// The app's requested GPU. Set before XR initialization; changing it after the
        /// session is up has no effect (the runtime reads the suggestion once, at
        /// session setup, and Unity's own device is fixed from process startup).
        /// </summary>
        public static TargetGpu Target { get; set; } = TargetGpu.Auto;

        // Was DXR_D3D_FORCE_GPU already set before the plugin ever touched it?
        // Latched on the first Apply() — see the comment there.
        static bool s_ExternalChecked;
        static bool s_ExternallySet;

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

            // An env var set from OUTSIDE the process wins — don't fight a developer
            // deliberately forcing an adapter from the shell.
            //
            // "Outside" is latched on the first call and never re-tested, because the
            // editor is a long-lived process where Initialize() runs once per Play
            // session: from the second Play onward the variable is set — by US — and
            // re-reading it would make the plugin defer to its own previous value and
            // silently ignore a changed Target.
            string envVar = EnvVarForCurrentApi();

            if (!s_ExternalChecked)
            {
                s_ExternalChecked = true;
                s_ExternallySet = !string.IsNullOrEmpty(
                    System.Environment.GetEnvironmentVariable(envVar));
            }
            if (s_ExternallySet)
            {
                Debug.Log("[DisplayXR] Target GPU: " + envVar + "=" +
                          System.Environment.GetEnvironmentVariable(envVar) +
                          " was set in the environment before startup — leaving it alone.");
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
                ok = DisplayXRProviderNative.dxr_prov_set_env(envVar, value);
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
                Debug.Log("[DisplayXR] Target GPU: " + Target + " → " + envVar + "=" + value +
                          (Target == TargetGpu.Auto ? " (matched to Unity's adapter)" : ""));
            else
                Debug.LogWarning("[DisplayXR] Target GPU: failed to set " + envVar + "=" + value +
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
