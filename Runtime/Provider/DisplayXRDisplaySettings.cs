// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

using System;
using UnityEngine;

namespace DisplayXR
{
    /// <summary>
    /// Build/runtime settings for the DisplayXR display provider (epic #166).
    /// Minimal for M2 — the provider reads display geometry, modes, and tunables
    /// from the runtime at session start, so there is nothing per-project to
    /// configure yet. It exists as the settings type referenced by the package
    /// metadata (Editor/Provider/DisplayXRDisplayPackage) so the provider registers
    /// as an XR Plug-in Management toggle and Unity ships the loader +
    /// UnitySubsystemsManifest.json in builds (no more hand-copy).
    ///
    /// NOTE: the [XRConfigurationData] attribute (which would add a settings panel)
    /// is deliberately NOT applied — it lives in the editor-only
    /// Unity.XR.Management editor assembly, which this package's Runtime scripts
    /// (predefined Assembly-CSharp, no asmdef) cannot reference even under
    /// UNITY_EDITOR. The provider has no per-project settings, so a panel isn't
    /// needed; the loader toggle comes from the IXRPackage metadata.
    /// </summary>
    [Serializable]
    public class DisplayXRDisplaySettings : ScriptableObject
    {
        /// <summary>EditorBuildSettings config object key (also the runtime lookup key).</summary>
        public const string SettingsKey = "com.displayxr.unity.display_settings";

        /// <summary>Populated by Unity from the build settings; available at runtime.</summary>
        public static DisplayXRDisplaySettings s_RuntimeInstance;

        void Awake() => s_RuntimeInstance = this;
    }
}
