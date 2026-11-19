// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

using UnityEditor;

namespace DisplayXR.Editor
{
    /// <summary>
    /// Play Mode side of the settings asset's Render Scale (#475). A built player reads
    /// the value the build stored next to its data; the editor has no such file, so the
    /// asset's current value is handed to the provider when Play Mode starts - before the
    /// display loader initializes, so the eye textures are created at that size.
    ///
    /// <para>Pushed from two places because either can be the one that runs: the static
    /// constructor after the domain reload that entering Play Mode triggers, and the
    /// ExitingEditMode notification when domain reload on Play is turned off.</para>
    /// </summary>
    [InitializeOnLoad]
    static class DisplayXRRenderScaleSettings
    {
        static DisplayXRRenderScaleSettings()
        {
            EditorApplication.playModeStateChanged += OnPlayModeStateChanged;
            if (EditorApplication.isPlayingOrWillChangePlaymode) Push();
        }

        static void OnPlayModeStateChanged(PlayModeStateChange change)
        {
            if (change == PlayModeStateChange.ExitingEditMode) Push();
        }

        static void Push()
        {
            var settings = DisplayXRManifestSettings.Find();
            DisplayXRProvider.SetSettingsRenderScale(settings != null ? settings.renderScale : DisplayXRRenderScaleMath.Unset);
        }
    }
}
