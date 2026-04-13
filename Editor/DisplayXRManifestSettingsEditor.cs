// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0

using UnityEditor;
using UnityEngine;

namespace DisplayXR.Editor
{
    [CustomEditor(typeof(DisplayXRManifestSettings))]
    public class DisplayXRManifestSettingsEditor : UnityEditor.Editor
    {
        private SerializedProperty appName;
        private SerializedProperty category;
        private SerializedProperty displayMode;
        private SerializedProperty description;
        private SerializedProperty icon;
        private SerializedProperty icon3D;
        private SerializedProperty icon3DLayout;

        private void OnEnable()
        {
            appName = serializedObject.FindProperty("appName");
            category = serializedObject.FindProperty("category");
            displayMode = serializedObject.FindProperty("displayMode");
            description = serializedObject.FindProperty("description");
            icon = serializedObject.FindProperty("icon");
            icon3D = serializedObject.FindProperty("icon3D");
            icon3DLayout = serializedObject.FindProperty("icon3DLayout");
        }

        public override void OnInspectorGUI()
        {
            serializedObject.Update();

            EditorGUILayout.HelpBox(
                "These settings control the .displayxr.json sidecar file generated " +
                "next to your built executable. The DisplayXR shell's spatial launcher " +
                "uses this file to discover and display your app.",
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

            serializedObject.ApplyModifiedProperties();
        }
    }
}
