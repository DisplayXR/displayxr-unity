// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

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
        private SerializedProperty m_PostProcessAntiAliasing;
        private SerializedProperty m_LogEyeTracking;

        void OnEnable()
        {
            if (target == null) return;
            m_IpdFactor = serializedObject.FindProperty("ipdFactor");
            m_ParallaxFactor = serializedObject.FindProperty("parallaxFactor");
            m_PerspectiveFactor = serializedObject.FindProperty("perspectiveFactor");
            m_VirtualDisplayHeight = serializedObject.FindProperty("virtualDisplayHeight");
            m_PostProcessAntiAliasing = serializedObject.FindProperty("postProcessAntiAliasing");
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

            DrawFramingPreviewBox();

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
                if (DisplayXREditorStatus.TryGetDisplayInfo(out var info, out _))
                {
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
                m_PerspectiveFactor.floatValue = 1.0f;
                m_VirtualDisplayHeight.floatValue = 0f;
            }

            // Runtime eye tracking info (Provider / Standalone Preview / OpenXR hook)
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

        // (#265) Framing preview toggle. Outside Play there is no provider and no Kooima,
        // so Unity renders this rig's camera sitting ON the display plane — the Game view
        // shows the wrong viewpoint until you press Play. The preview overrides the
        // camera's view matrix for rendering only; it never touches the transform, because
        // for THIS rig the transform is the display plane itself and moving it would change
        // what the scene means in Play.
        private void DrawFramingPreviewBox()
        {
            EditorGUILayout.Space();
            bool on = DisplayXRDisplayFramingPreview.Enabled;
            bool now = EditorGUILayout.ToggleLeft(
                new GUIContent("Edit-Mode Framing Preview",
                    "Frame the Game view as if a 2D camera were viewing the virtual display, " +
                    "so authoring matches Play. Rendering-only — nothing is serialized. " +
                    "Editor-wide setting, not part of the scene."),
                on);
            if (now != on) DisplayXRDisplayFramingPreview.Enabled = now;

            EditorGUILayout.HelpBox(
                now
                    ? "The Game view is framed as if viewing the virtual display. This is a " +
                      "FRAMING preview only — stereo is real in Play Mode, which runs the " +
                      "provider and is the actual preview."
                    : "Preview off: outside Play the Game view renders from the display plane " +
                      "itself, so the framing will not match Play Mode.",
                MessageType.None);
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
