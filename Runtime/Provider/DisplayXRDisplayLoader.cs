// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0

using System.Collections.Generic;
using UnityEngine;
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
    }
}
