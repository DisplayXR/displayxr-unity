// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0

using UnityEditor;
using UnityEngine;
using DisplayXR;

namespace DisplayXR.Editor
{
    [CustomEditor(typeof(DisplayXRDisplay))]
    public class DisplayXRDisplayEditor : UnityEditor.Editor
    {
        private SerializedProperty m_IpdFactor;
        private SerializedProperty m_ParallaxFactor;
        private SerializedProperty m_PerspectiveFactor;
        private SerializedProperty m_VirtualDisplayHeight;
        private SerializedProperty m_LogEyeTracking;

        void OnEnable()
        {
            if (target == null) return;
            m_IpdFactor = serializedObject.FindProperty("ipdFactor");
            m_ParallaxFactor = serializedObject.FindProperty("parallaxFactor");
            m_PerspectiveFactor = serializedObject.FindProperty("perspectiveFactor");
            m_VirtualDisplayHeight = serializedObject.FindProperty("virtualDisplayHeight");
            m_LogEyeTracking = serializedObject.FindProperty("logEyeTracking");
        }

        public override void OnInspectorGUI()
        {
            serializedObject.Update();

            EditorGUILayout.HelpBox(
                "Display-Centric mode: the camera's transform represents the virtual display pose. " +
                "Camera FOV is ignored — the display geometry determines the frustum. " +
                "Best for tabletop, AR-like, and object-focused setups.",
                MessageType.Info);

            // Display info header
            DrawDisplayInfoBox();

            EditorGUILayout.Space();
            EditorGUILayout.LabelField("Stereo Tunables", EditorStyles.boldLabel);

            EditorGUILayout.PropertyField(m_IpdFactor,
                new GUIContent("IPD Factor", "Scales inter-eye distance. 1.0 = natural."));
            EditorGUILayout.PropertyField(m_ParallaxFactor,
                new GUIContent("Parallax Factor", "Scales eye X/Y offset from display center."));
            EditorGUILayout.PropertyField(m_PerspectiveFactor,
                new GUIContent("Perspective Factor", "Scales perceived depth. 1.0 = natural."));

            EditorGUILayout.Space();
            EditorGUILayout.LabelField("Display Parameters", EditorStyles.boldLabel);

            EditorGUILayout.PropertyField(m_VirtualDisplayHeight,
                new GUIContent("Virtual Display Height (m)",
                    "Virtual display height in meters. 0 = use physical display height."));

            // Show computed display size
            {
                var feature = DisplayXRFeature.Instance;
                if (feature != null && feature.DisplayInfo.isValid)
                {
                    var info = feature.DisplayInfo;
                    float h = m_VirtualDisplayHeight.floatValue > 0
                        ? m_VirtualDisplayHeight.floatValue
                        : info.displayHeightMeters;
                    float w = info.displayWidthMeters * (h / info.displayHeightMeters);
                    EditorGUI.indentLevel++;
                    EditorGUILayout.LabelField(" ", $"{w * 100:F1} x {h * 100:F1} cm (virtual)");
                    EditorGUI.indentLevel--;
                }
            }

            EditorGUILayout.Space();
            EditorGUILayout.PropertyField(m_LogEyeTracking);

            // Reset button
            EditorGUILayout.Space();
            if (GUILayout.Button("Reset to Defaults"))
            {
                m_IpdFactor.floatValue = 1.0f;
                m_ParallaxFactor.floatValue = 1.0f;
                m_PerspectiveFactor.floatValue = 1.0f;
                m_VirtualDisplayHeight.floatValue = 0f;
            }

            // Runtime eye tracking info
            if (Application.isPlaying && DisplayXRFeature.Instance != null)
            {
                EditorGUILayout.Space();
                EditorGUILayout.LabelField("Runtime Status", EditorStyles.boldLabel);

                var feature = DisplayXRFeature.Instance;
                EditorGUILayout.LabelField("Eye Tracked", feature.IsEyeTracked ? "Yes" : "No");
                EditorGUILayout.Vector3Field("Left Eye", feature.LeftEyePosition);
                EditorGUILayout.Vector3Field("Right Eye", feature.RightEyePosition);
            }

            serializedObject.ApplyModifiedProperties();
        }

        private void DrawDisplayInfoBox()
        {
            var feature = DisplayXRFeature.Instance;
            if (feature == null || !feature.DisplayInfo.isValid)
            {
                EditorGUILayout.HelpBox(
                    "Display info not available. DisplayXRFeature must be active with a connected runtime.",
                    MessageType.Info);
                return;
            }

            var info = feature.DisplayInfo;
            EditorGUILayout.BeginVertical(EditorStyles.helpBox);
            EditorGUILayout.LabelField("Connected Display", EditorStyles.boldLabel);
            EditorGUILayout.LabelField("Resolution", $"{info.displayPixelWidth} x {info.displayPixelHeight}");
            EditorGUILayout.LabelField("Physical Size",
                $"{info.displayWidthMeters * 100:F1} x {info.displayHeightMeters * 100:F1} cm");
            EditorGUILayout.LabelField("Nominal Viewer",
                $"({info.nominalViewerX * 1000:F0}, {info.nominalViewerY * 1000:F0}, {info.nominalViewerZ * 1000:F0}) mm");
            EditorGUILayout.EndVertical();
        }
    }
}
