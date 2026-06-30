// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0

using System.Collections.Generic;
using UnityEngine;
using UnityEngine.Rendering;
using UnityEngine.XR;
using UnityEngine.XR.Management;

namespace DisplayXR
{
    /// <summary>
    /// XRLoader for the custom DisplayXR IUnityXRDisplay Display Provider (epic
    /// #166). Registering it under XR Plug-in Management is what makes the
    /// "DisplayXR Display" subsystem a real toggle and ships
    /// Runtime/UnitySubsystemsManifest.json in builds automatically — replacing the
    /// M1 [RuntimeInitializeOnLoadMethod] bootstrap + manifest hand-copy.
    ///
    /// The native provider (registered from UnityPluginLoad) implements the
    /// subsystem; this loader creates/starts/stops it and owns the lifecycle of the
    /// per-frame <see cref="DisplayXRProviderDriver"/>. Display-only (no input
    /// subsystem) — by design (#166): apps render stereo 3D while using normal
    /// Unity input.
    /// </summary>
    public sealed class DisplayXRDisplayLoader : XRLoaderHelper
    {
        // Must match Runtime/UnitySubsystemsManifest.json's display "id".
        const string k_DisplayId = "DisplayXR Display";

        static readonly List<XRDisplaySubsystemDescriptor> s_DisplayDescriptors = new();

        /// <summary>The created display subsystem (null until Initialize succeeds).</summary>
        public XRDisplaySubsystem DisplaySubsystem => GetLoadedSubsystem<XRDisplaySubsystem>();

        public override bool Initialize()
        {
            // Pick the stereo render mode BEFORE creating/starting the subsystem so
            // the native GfxStart (which reads it) sees the right value (#166 task #8).
            // SPI is correct only on URP+Windows+D3D12; on BiRP it renders opaque
            // geometry wrong (skybox-only), so gate to MultiPass there. Mirrors the
            // hook path's DisplayXRFeature.IsUrpWindowsD3D12 (the provider can't use
            // OpenXRRuntime.name — Unity's OpenXR loader isn't active in provider mode).
            bool spi = IsSinglePassEligible();
            DisplayXRProviderNative.dxr_prov_set_single_pass(spi ? 1 : 0);
            Debug.Log("[DisplayXR] Provider render mode: " + (spi ? "Single-Pass-Instanced" : "MultiPass")
                      + " (pipeline=" + (GraphicsSettings.currentRenderPipeline != null
                          ? GraphicsSettings.currentRenderPipeline.GetType().Name : "Built-in")
                      + ", gfx=" + SystemInfo.graphicsDeviceType + ")");

            CreateSubsystem<XRDisplaySubsystemDescriptor, XRDisplaySubsystem>(
                s_DisplayDescriptors, k_DisplayId);
            if (DisplaySubsystem == null)
            {
                Debug.LogError("[DisplayXR] Failed to create the '" + k_DisplayId +
                    "' display subsystem. Is the native plugin present and " +
                    "UnitySubsystemsManifest.json shipped?");
                return false;
            }
            return true;
        }

        public override bool Start()
        {
            StartSubsystem<XRDisplaySubsystem>();
            DisplayXRProviderDriver.EnsureInstance();
            return true;
        }

        public override bool Stop()
        {
            DisplayXRProviderDriver.DestroyInstance();
            StopSubsystem<XRDisplaySubsystem>();
            return true;
        }

        public override bool Deinitialize()
        {
            DestroySubsystem<XRDisplaySubsystem>();
            return base.Deinitialize();
        }

        /// <summary>
        /// Whether to drive the provider in Single-Pass-Instanced mode. SPI is correct
        /// only on URP + Windows + D3D12; elsewhere (notably BiRP, where Unity builds
        /// each eye's off-center projection MultiPass-only) it renders opaque geometry
        /// wrong, so we fall back to MultiPass. Mirrors the platform half of the hook
        /// path's <c>DisplayXRFeature.IsUrpWindowsD3D12</c>; the runtime-version half is
        /// already implied (the provider hard-requires a recent runtime). Dev overrides:
        /// <c>DISPLAYXR_FORCE_SPI=1</c> / <c>DISPLAYXR_FORCE_MULTIPASS=1</c>.
        /// </summary>
        static bool IsSinglePassEligible()
        {
            if (System.Environment.GetEnvironmentVariable("DISPLAYXR_FORCE_MULTIPASS") == "1")
                return false;
            if (System.Environment.GetEnvironmentVariable("DISPLAYXR_FORCE_SPI") == "1")
                return true;

            var plat = Application.platform;
            bool isWindows = plat == RuntimePlatform.WindowsPlayer
                          || plat == RuntimePlatform.WindowsEditor;
            if (!isWindows) return false;
            if (SystemInfo.graphicsDeviceType != GraphicsDeviceType.Direct3D12) return false;
            var rp = GraphicsSettings.currentRenderPipeline;
            var typeName = rp != null ? rp.GetType().FullName : null;
            return typeName != null && typeName.Contains("Universal");
        }
    }
}
