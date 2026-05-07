// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0

using UnityEditor;
using UnityEngine;
using DisplayXR;

namespace DisplayXR.Editor
{
    [CustomEditor(typeof(DisplayXRCamera))]
    public class DisplayXRCameraEditor : UnityEditor.Editor
    {
        private SerializedProperty m_IpdFactor;
        private SerializedProperty m_ParallaxFactor;
        private SerializedProperty m_InvConvergenceDistance;
        private SerializedProperty m_LogEyeTracking;

        void OnEnable()
        {
            if (target == null) return;
            m_IpdFactor = serializedObject.FindProperty("ipdFactor");
            m_ParallaxFactor = serializedObject.FindProperty("parallaxFactor");
            m_InvConvergenceDistance = serializedObject.FindProperty("invConvergenceDistance");
            m_LogEyeTracking = serializedObject.FindProperty("logEyeTracking");
        }

        public override void OnInspectorGUI()
        {
            serializedObject.Update();

            EditorGUILayout.HelpBox(
                "Camera-Centric mode: the camera's transform is the viewer pose and its " +
                "vertical FOV is the rendering FOV. Inverse convergence distance controls " +
                "the screen plane depth. Best for first-person and free-camera setups.",
                MessageType.Info);

            DrawDisplayInfoBox();

            EditorGUILayout.Space();
            EditorGUILayout.LabelField("Stereo Tunables", EditorStyles.boldLabel);

            EditorGUILayout.PropertyField(m_IpdFactor,
                new GUIContent("IPD Factor", "Scales inter-eye distance. 1.0 = natural."));
            EditorGUILayout.PropertyField(m_ParallaxFactor,
                new GUIContent("Parallax Factor", "Scales eye offset from viewing center."));

            EditorGUILayout.Space();
            EditorGUILayout.LabelField("Convergence", EditorStyles.boldLabel);

            EditorGUILayout.PropertyField(m_InvConvergenceDistance,
                new GUIContent("Inv. Convergence Distance",
                    "1/meters. 0 = infinity (parallel projection). Higher = screen closer."));

            // Show distance in parenthesis
            float invd = m_InvConvergenceDistance.floatValue;
            EditorGUI.indentLevel++;
            if (invd > 0.001f)
            {
                float dist = 1.0f / invd;
                EditorGUILayout.LabelField(" ", $"({dist:F2} m)");
            }
            else
            {
                EditorGUILayout.LabelField(" ", "(\u221E)");
            }
            EditorGUI.indentLevel--;

            EditorGUILayout.Space();
            EditorGUILayout.PropertyField(m_LogEyeTracking);

            // Reset button
            EditorGUILayout.Space();
            if (GUILayout.Button("Reset to Defaults"))
            {
                m_IpdFactor.floatValue = 1.0f;
                m_ParallaxFactor.floatValue = 1.0f;
                m_InvConvergenceDistance.floatValue = 0f;
            }

            // Runtime info
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
