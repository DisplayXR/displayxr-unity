// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

using System;
using UnityEditor;
using UnityEngine;
using DisplayXR;

namespace DisplayXR.Editor
{
    /// <summary>
    /// XR Plugin Management settings page for DisplayXR.
    /// Appears in Project Settings > XR Plug-in Management > OpenXR > DisplayXR.
    /// </summary>
    public class DisplayXRSettingsProvider : SettingsProvider
    {
        private const string SettingsPath = "Project/XR Plug-in Management/OpenXR/DisplayXR";

        public DisplayXRSettingsProvider()
            : base(SettingsPath, SettingsScope.Project)
        {
            keywords = new System.Collections.Generic.HashSet<string>(new[]
            {
                "displayxr", "3d", "display", "openxr", "stereo", "light field"
            });
        }

        [SettingsProvider]
        public static SettingsProvider CreateProvider()
        {
            return new DisplayXRSettingsProvider();
        }

        public override void OnGUI(string searchContext)
        {
            EditorGUILayout.LabelField("DisplayXR Settings", EditorStyles.boldLabel);
            EditorGUILayout.Space();

            // Runtime status
            DrawRuntimeStatus();

            EditorGUILayout.Space();

            // Environment variable info
            DrawEnvironmentInfo();

            EditorGUILayout.Space();

            // Feature status
            DrawFeatureStatus();

            EditorGUILayout.Space();

            // App manifest sidecar
            DrawManifestSection();
        }

        private void DrawRuntimeStatus()
        {
            EditorGUILayout.BeginVertical(EditorStyles.helpBox);
            EditorGUILayout.LabelField("Runtime Status", EditorStyles.boldLabel);

            string runtimeJson = Environment.GetEnvironmentVariable("XR_RUNTIME_JSON");
            string source = "XR_RUNTIME_JSON";

            // If env var not set, try platform-specific discovery
            if (string.IsNullOrEmpty(runtimeJson))
            {
#if UNITY_EDITOR_WIN
                try
                {
                    using (var hklm = Microsoft.Win32.RegistryKey.OpenBaseKey(
                        Microsoft.Win32.RegistryHive.LocalMachine,
                        Microsoft.Win32.RegistryView.Registry64))
                    using (var key = hklm.OpenSubKey(@"Software\Khronos\OpenXR\1"))
                    {
                        if (key != null)
                            runtimeJson = key.GetValue("ActiveRuntime") as string;
                    }
                    if (!string.IsNullOrEmpty(runtimeJson))
                        source = "Registry (Khronos\\OpenXR\\1\\ActiveRuntime)";
                }
                catch { }
#endif
            }

            if (!string.IsNullOrEmpty(runtimeJson))
            {
                EditorGUILayout.LabelField("Source", source);
                EditorGUILayout.LabelField("Runtime JSON", runtimeJson);
                bool exists = System.IO.File.Exists(runtimeJson);
                EditorGUILayout.LabelField("File Exists", exists ? "Yes" : "No");
                if (!exists)
                {
                    EditorGUILayout.HelpBox(
                        "The runtime manifest file does not exist. Check the path.",
                        MessageType.Error);
                }
            }
            else
            {
                EditorGUILayout.HelpBox(
                    "No OpenXR runtime found.\n" +
                    "Install the DisplayXR runtime or set XR_RUNTIME_JSON environment variable.",
                    MessageType.Warning);
            }

            EditorGUILayout.EndVertical();
        }

        private void DrawEnvironmentInfo()
        {
            EditorGUILayout.BeginVertical(EditorStyles.helpBox);
            EditorGUILayout.LabelField("Testing", EditorStyles.boldLabel);

            string simDisplay = Environment.GetEnvironmentVariable("SIM_DISPLAY_ENABLE");
            if (simDisplay == "1")
            {
                string output = Environment.GetEnvironmentVariable("SIM_DISPLAY_OUTPUT") ?? "sbs";
                EditorGUILayout.LabelField("Simulation Display", $"Enabled (output: {output})");
                EditorGUILayout.HelpBox(
                    "Using simulation display. No physical 3D display hardware required.",
                    MessageType.Info);
            }
            else
            {
                EditorGUILayout.HelpBox(
                    "For testing without hardware, set SIM_DISPLAY_ENABLE=1 and " +
                    "SIM_DISPLAY_OUTPUT=sbs (or anaglyph, blend).",
                    MessageType.Info);
            }

            EditorGUILayout.EndVertical();
        }

        private void DrawFeatureStatus()
        {
            EditorGUILayout.BeginVertical(EditorStyles.helpBox);
            EditorGUILayout.LabelField("Runtime Status", EditorStyles.boldLabel);

            // Status comes from whichever backend is live: the Display Provider
            // (built app / Play Mode after #171), the standalone Preview session, or
            // the OpenXR hook. DisplayXRFeature is null in provider mode, so this no
            // longer keys off it directly (#166 gap #6).
            if (DisplayXREditorStatus.TryGetDisplayInfo(out var info, out var source))
            {
                EditorGUILayout.LabelField("Source", DisplayXREditorStatus.SourceLabel(source));
                EditorGUILayout.LabelField("Connected", "Yes");
                EditorGUILayout.LabelField("Display",
                    $"{info.displayPixelWidth}x{info.displayPixelHeight}");
                EditorGUILayout.LabelField("Physical Size",
                    $"{info.displayWidthMeters * 100:F1} x {info.displayHeightMeters * 100:F1} cm");
                EditorGUILayout.LabelField("Nominal Viewer",
                    $"({info.nominalViewerX * 1000:F0}, {info.nominalViewerY * 1000:F0}, " +
                    $"{info.nominalViewerZ * 1000:F0}) mm");
                EditorGUILayout.LabelField("View Scale",
                    $"{info.recommendedViewScaleX:F2} x {info.recommendedViewScaleY:F2}");
                if (DisplayXREditorStatus.TryGetEyeTracking(out bool tracked, out _, out _, out _))
                    EditorGUILayout.LabelField("Eye Tracking", tracked ? "Active" : "Inactive");
            }
            else
            {
                EditorGUILayout.LabelField("Status", "Not connected");
                EditorGUILayout.HelpBox(
                    "Connect a DisplayXR runtime to see live display info:\n" +
                    "• Enter Play Mode with the DisplayXR Display provider (or the OpenXR " +
                    "hook feature) enabled in XR Plug-in Management, or\n" +
                    "• open Window > DisplayXR > Preview and click Start.",
                    MessageType.Info);
            }

            EditorGUILayout.EndVertical();
        }

        private void DrawManifestSection()
        {
            EditorGUILayout.BeginVertical(EditorStyles.helpBox);
            EditorGUILayout.LabelField("App Manifest", EditorStyles.boldLabel);

            EditorGUILayout.HelpBox(
                "Any DisplayXR-compatible workspace controller (the DisplayXR Shell is the " +
                "reference) discovers apps via a .displayxr.json sidecar file generated next " +
                "to each built executable. Configure the manifest settings below.",
                MessageType.Info);

            var settings = DisplayXRManifestSettings.Find();
            if (settings != null)
            {
                EditorGUILayout.LabelField("App Name", settings.EffectiveName);
                EditorGUILayout.LabelField("Category", settings.category.ToString());
                if (GUILayout.Button("Select Manifest Settings"))
                    Selection.activeObject = settings;
            }
            else
            {
                EditorGUILayout.LabelField("Status", "No settings asset (defaults will be used on build)");
                if (GUILayout.Button("Create Manifest Settings"))
                    Selection.activeObject = DisplayXRManifestSettings.GetOrCreate();
            }

            EditorGUILayout.EndVertical();
        }
    }
}
