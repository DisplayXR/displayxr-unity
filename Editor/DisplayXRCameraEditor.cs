// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

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
        private SerializedProperty m_AuthoredFov;
        private SerializedProperty m_PostProcessAntiAliasing;
        private SerializedProperty m_LogEyeTracking;

        void OnEnable()
        {
            if (target == null) return;
            m_IpdFactor = serializedObject.FindProperty("ipdFactor");
            m_ParallaxFactor = serializedObject.FindProperty("parallaxFactor");
            m_InvConvergenceDistance = serializedObject.FindProperty("invConvergenceDistance");
            m_AuthoredFov = serializedObject.FindProperty("m_AuthoredFov");
            m_PostProcessAntiAliasing = serializedObject.FindProperty("postProcessAntiAliasing");
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
            EditorGUILayout.LabelField("Field of View", EditorStyles.boldLabel);
            using (new EditorGUI.DisabledScope(true))
            {
                float f = m_AuthoredFov.floatValue;
                EditorGUILayout.FloatField(
                    new GUIContent("Authored FOV (serialized)",
                        "The FOV this rig projects with, captured from the Camera in edit mode " +
                        "and serialized so it survives scene loads during a live session (#274). " +
                        "Edit the Camera's Field of View to change it. 0 = not captured yet — " +
                        "save the scene once."),
                    f);
            }
            if (m_AuthoredFov.floatValue < 1.0f)
                EditorGUILayout.HelpBox("Authored FOV not captured yet. Save the scene once so the rig " +
                    "seeds from a serialized value instead of the XR-overwritten Camera FOV (#274).",
                    MessageType.Info);

            EditorGUILayout.Space();
            EditorGUILayout.LabelField("Rendering", EditorStyles.boldLabel);
            EditorGUILayout.PropertyField(m_PostProcessAntiAliasing,
                new GUIContent("Post-Process AA (FXAA)",
                    "Unity drops MSAA on the XR eye RT, so silhouettes alias. " +
                    "FXAA restores soft edges post-resolve. Disable if not needed."));

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

            // Runtime info (Provider / Standalone Preview / OpenXR hook)
            if (Application.isPlaying &&
                DisplayXREditorStatus.TryGetEyeTracking(out bool tracked, out Vector3 leftEye,
                                                        out Vector3 rightEye, out bool hasPositions))
            {
                EditorGUILayout.Space();
                EditorGUILayout.LabelField("Runtime Status", EditorStyles.boldLabel);
                EditorGUILayout.LabelField("Eye Tracked", tracked ? "Yes" : "No");
                if (hasPositions)
                {
                    EditorGUILayout.Vector3Field("Left Eye", leftEye);
                    EditorGUILayout.Vector3Field("Right Eye", rightEye);
                }
            }

            serializedObject.ApplyModifiedProperties();
        }

        private void DrawDisplayInfoBox()
        {
            if (!DisplayXREditorStatus.TryGetDisplayInfo(out var info, out var source))
            {
                EditorGUILayout.HelpBox(
                    "Display info not available. Connect a DisplayXR runtime via the " +
                    "Display Provider (Play Mode / built app), the standalone Preview window, " +
                    "or the OpenXR hook.",
                    MessageType.Info);
                return;
            }

            EditorGUILayout.BeginVertical(EditorStyles.helpBox);
            EditorGUILayout.LabelField(
                "Connected Display (" + DisplayXREditorStatus.SourceLabel(source) + ")",
                EditorStyles.boldLabel);
            EditorGUILayout.LabelField("Resolution", $"{info.displayPixelWidth} x {info.displayPixelHeight}");
            EditorGUILayout.LabelField("Physical Size",
                $"{info.displayWidthMeters * 100:F1} x {info.displayHeightMeters * 100:F1} cm");
            EditorGUILayout.LabelField("Nominal Viewer",
                $"({info.nominalViewerX * 1000:F0}, {info.nominalViewerY * 1000:F0}, {info.nominalViewerZ * 1000:F0}) mm");
            EditorGUILayout.EndVertical();
        }
    }
}
