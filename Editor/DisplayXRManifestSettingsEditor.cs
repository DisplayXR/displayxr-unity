// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0

using System.Collections.Generic;
using UnityEditor;
using UnityEditor.Build;
using UnityEngine;

namespace DisplayXR.Editor
{
    [CustomEditor(typeof(DisplayXRManifestSettings))]
    public class DisplayXRManifestSettingsEditor : UnityEditor.Editor
    {
        // Opt-out scripting define. The boot splash is ON by default; this
        // define compiles out the spawner (DisplayXRSplashBootstrap) and tells
        // the build processor to leave Unity's stock splash alone.
        const string NoSplashDefine = "DISPLAYXR_NO_SPLASH";
        // Direct shortcut so users don't have to hunt through Project Settings
        // (which can be empty if the OpenXR feature isn't checked) or guess
        // where the asset lives in Assets/.
        [MenuItem("Window/DisplayXR/Manifest Settings")]
        public static void OpenManifestSettings()
        {
            var settings = DisplayXRManifestSettings.Find()
                ?? DisplayXRManifestSettings.GetOrCreate();
            Selection.activeObject = settings;
            EditorGUIUtility.PingObject(settings);
        }

        private SerializedProperty appName;
        private SerializedProperty category;
        private SerializedProperty displayMode;
        private SerializedProperty description;
        private SerializedProperty icon;
        private SerializedProperty icon3D;
        private SerializedProperty icon3DLayout;
        private SerializedProperty registerWithDisplayXR;
        private SerializedProperty disableBootSplash;

        private void OnEnable()
        {
            appName = serializedObject.FindProperty("appName");
            category = serializedObject.FindProperty("category");
            displayMode = serializedObject.FindProperty("displayMode");
            description = serializedObject.FindProperty("description");
            icon = serializedObject.FindProperty("icon");
            icon3D = serializedObject.FindProperty("icon3D");
            icon3DLayout = serializedObject.FindProperty("icon3DLayout");
            registerWithDisplayXR = serializedObject.FindProperty("registerWithDisplayXR");
            disableBootSplash = serializedObject.FindProperty("disableBootSplash");
        }

        public override void OnInspectorGUI()
        {
            serializedObject.Update();

            EditorGUILayout.HelpBox(
                "These settings control the .displayxr.json sidecar file generated " +
                "next to your built executable. Any DisplayXR-compatible workspace " +
                "controller (the DisplayXR Shell is the reference) uses this file to " +
                "discover and display your app.",
                MessageType.Info);

            EditorGUILayout.Space();

            // --- Required ---
            EditorGUILayout.LabelField("Required", EditorStyles.boldLabel);

            EditorGUILayout.PropertyField(appName, new GUIContent("App Name",
                "Display name on the launcher tile. Leave empty to use Application.productName."));
            if (string.IsNullOrWhiteSpace(appName.stringValue))
            {
                EditorGUI.indentLevel++;
                EditorGUILayout.LabelField("Default", Application.productName, EditorStyles.miniLabel);
                EditorGUI.indentLevel--;
            }

            EditorGUILayout.LabelField("Type", "3d (always for Unity apps)", EditorStyles.miniLabel);

            EditorGUILayout.Space();

            // --- Optional ---
            EditorGUILayout.LabelField("Optional", EditorStyles.boldLabel);

            EditorGUILayout.PropertyField(category, new GUIContent("Category"));
            EditorGUILayout.PropertyField(displayMode, new GUIContent("Display Mode",
                "Preferred display rendering mode at launch."));
            EditorGUILayout.PropertyField(description, new GUIContent("Description",
                "One-line description for tooltips (max 256 chars)."));
            if (description.stringValue.Length > 256)
            {
                EditorGUILayout.HelpBox(
                    $"Description is {description.stringValue.Length} chars (max 256). It will be truncated on build.",
                    MessageType.Warning);
            }

            EditorGUILayout.Space();

            // --- Icons ---
            EditorGUILayout.LabelField("Icons", EditorStyles.boldLabel);

            EditorGUILayout.PropertyField(icon, new GUIContent("2D Icon",
                "PNG tile icon (512x512 recommended)."));

            EditorGUILayout.PropertyField(icon3D, new GUIContent("3D SBS Icon",
                "Stereoscopic side-by-side tile icon (1024x512 recommended). Enables 3D tile in launcher."));

            if (icon3D.objectReferenceValue != null)
            {
                EditorGUI.indentLevel++;
                EditorGUILayout.PropertyField(icon3DLayout, new GUIContent("Stereo Layout"));
                EditorGUI.indentLevel--;
            }

            EditorGUILayout.Space();

            // --- Distribution ---
            EditorGUILayout.LabelField("Distribution", EditorStyles.boldLabel);
            EditorGUILayout.PropertyField(registerWithDisplayXR, new GUIContent("Register with DisplayXR",
                "Also write a registered manifest to %LOCALAPPDATA%\\DisplayXR\\apps\\ so DisplayXR-compatible " +
                "workspace controllers (including the DisplayXR Shell) discover this build without needing it under Program Files."));

            EditorGUILayout.Space();

            // --- Boot Splash ---
            EditorGUILayout.LabelField("Boot Splash", EditorStyles.boldLabel);
            EditorGUILayout.HelpBox(
                "DisplayXR shows a branded boot splash on the zero-disparity plane by " +
                "default, and disables Unity's stock splash in standalone builds. " +
                "Unity 6 permits disabling the stock splash on all license tiers.",
                MessageType.Info);
            EditorGUI.BeginChangeCheck();
            EditorGUILayout.PropertyField(disableBootSplash, new GUIContent("Disable DisplayXR Splash",
                "Opt out: keep Unity's stock splash and skip the DisplayXR splash " +
                "(sets the DISPLAYXR_NO_SPLASH define)."));
            bool toggleChanged = EditorGUI.EndChangeCheck();

            serializedObject.ApplyModifiedProperties();

            if (toggleChanged)
                ApplyOptOutState(disableBootSplash.boolValue);
        }

        /// <summary>
        /// Apply the opt-out choice: add/remove the DISPLAYXR_NO_SPLASH define,
        /// which compiles the boot-splash spawner out and tells the build
        /// processor to leave Unity's stock splash alone. (When opted in — the
        /// default — the build processor disables the stock splash per build, so
        /// we never persistently mutate the user's PlayerSettings here.)
        /// </summary>
        private static void ApplyOptOutState(bool optedOut)
        {
            var nbt = NamedBuildTarget.Standalone;
            var defines = new List<string>(
                PlayerSettings.GetScriptingDefineSymbols(nbt)
                    .Split(new[] { ';' }, System.StringSplitOptions.RemoveEmptyEntries));
            bool has = defines.Contains(NoSplashDefine);
            if (optedOut && !has) defines.Add(NoSplashDefine);
            else if (!optedOut && has) defines.Remove(NoSplashDefine);
            PlayerSettings.SetScriptingDefineSymbols(nbt, string.Join(";", defines));

            Debug.Log($"[DisplayXR] Boot splash {(optedOut ? "DISABLED (opt-out)" : "enabled (default)")} " +
                      $"— define {(optedOut ? "added" : "removed")}.");
        }
    }
}
