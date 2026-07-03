// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

#if UNITY_EDITOR
using System.Collections.Generic;
using UnityEditor;
using UnityEditor.XR.Management.Metadata;
using UnityEngine;

namespace DisplayXR
{
    /// <summary>
    /// XR Plug-in Management metadata for the custom DisplayXR Display Provider
    /// (epic #166). Registering this makes "DisplayXR Display" appear as a toggle
    /// under Project Settings > XR Plug-in Management (Standalone). Enabling it
    /// adds <see cref="DisplayXRDisplayLoader"/> to the active loaders, and Unity's
    /// XR-Management build processor then ships the loader +
    /// Runtime/UnitySubsystemsManifest.json automatically (no hand-copy).
    /// </summary>
    class DisplayXRDisplayPackage : IXRPackage
    {
        class LoaderMetadata : IXRLoaderMetadata
        {
            public string loaderName { get; set; }
            public string loaderType { get; set; }
            public List<BuildTargetGroup> supportedBuildTargets { get; set; }
        }

        class PackageMetadata : IXRPackageMetadata
        {
            public string packageName { get; set; }
            public string packageId { get; set; }
            public string settingsType { get; set; }
            public List<IXRLoaderMetadata> loaderMetadata { get; set; }
        }

        static readonly IXRPackageMetadata s_Metadata = new PackageMetadata
        {
            packageName = "DisplayXR",
            packageId = "com.displayxr.unity",
            settingsType = typeof(DisplayXRDisplaySettings).FullName,
            loaderMetadata = new List<IXRLoaderMetadata>
            {
                new LoaderMetadata
                {
                    loaderName = "DisplayXR Display",
                    loaderType = typeof(DisplayXRDisplayLoader).FullName,
                    // M2: Windows / D3D12. Standalone covers the Win64 player.
                    supportedBuildTargets = new List<BuildTargetGroup>
                    {
                        BuildTargetGroup.Standalone
                    }
                }
            }
        };

        public IXRPackageMetadata metadata => s_Metadata;

        public bool PopulateNewSettingsInstance(ScriptableObject obj) => true;
    }
}
#endif
