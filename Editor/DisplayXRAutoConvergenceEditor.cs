// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

using UnityEditor;
using UnityEngine;
using DisplayXR;

namespace DisplayXR.Editor
{
    [CustomEditor(typeof(DisplayXRAutoConvergence))]
    public class DisplayXRAutoConvergenceEditor : UnityEditor.Editor
    {
        SerializedProperty m_Mode, m_DriveStrength, m_MinDist, m_MaxDist, m_TimeConstant, m_DepthScale,
            m_FrontCap, m_BehindCap, m_LayerMask, m_IgnoreNearer, m_RayMax, m_RayCols, m_RayRows,
            m_Target, m_SamplePeriod, m_Percentile, m_Blend, m_DeadBand;
        static bool s_Advanced;

        void OnEnable()
        {
            if (target == null) return;
            m_Mode = serializedObject.FindProperty("mode");
            m_DriveStrength = serializedObject.FindProperty("driveStereoStrength");
            m_MinDist = serializedObject.FindProperty("minConvergenceDistance");
            m_MaxDist = serializedObject.FindProperty("maxConvergenceDistance");
            m_TimeConstant = serializedObject.FindProperty("timeConstant");
            m_DepthScale = serializedObject.FindProperty("depthScale");
            m_FrontCap = serializedObject.FindProperty("frontCapDegrees");
            m_BehindCap = serializedObject.FindProperty("behindCapDegrees");
            m_LayerMask = serializedObject.FindProperty("layerMask");
            m_IgnoreNearer = serializedObject.FindProperty("ignoreNearerThan");
            m_RayMax = serializedObject.FindProperty("raycastMaxDistance");
            m_RayCols = serializedObject.FindProperty("rayColumns");
            m_RayRows = serializedObject.FindProperty("rayRows");
            m_Target = serializedObject.FindProperty("target");
            m_SamplePeriod = serializedObject.FindProperty("samplePeriod");
            m_Percentile = serializedObject.FindProperty("percentile");
            m_Blend = serializedObject.FindProperty("blend");
            m_DeadBand = serializedObject.FindProperty("deadBand");
        }

        public override void OnInspectorGUI()
        {
            var ac = (DisplayXRAutoConvergence)target;
            serializedObject.Update();

            if (ac.GetComponent<DisplayXRCamera>() == null)
            {
                EditorGUILayout.HelpBox(
                    ac.GetComponent<DisplayXRDisplay>() != null
                        ? "This is a display-centric rig: its screen plane is the authored virtual display, " +
                          "so there is nothing to converge. Auto Convergence does nothing here."
                        : "Auto Convergence drives a camera-centric rig. Add a DisplayXR Camera-Centric Rig " +
                          "(DisplayXRCamera) to this camera.",
                    MessageType.Warning);
            }
            else
            {
                EditorGUILayout.HelpBox(
                    "Keeps the screen plane on the subject as the camera moves: about a fifth of the frame " +
                    "sits in front of the screen. Call ResetConvergence() after a camera teleport, a cut or " +
                    "a scene load - cuts are not auto-detected.",
                    MessageType.Info);
            }

            EditorGUILayout.PropertyField(m_Mode);
            var mode = (DisplayXRAutoConvergence.Mode)m_Mode.enumValueIndex;
            if (mode == DisplayXRAutoConvergence.Mode.Target)
                EditorGUILayout.PropertyField(m_Target);
            EditorGUILayout.PropertyField(m_DriveStrength,
                new GUIContent("Drive Stereo Strength", m_DriveStrength.tooltip));

            EditorGUILayout.Space();
            EditorGUILayout.LabelField("Convergence", EditorStyles.boldLabel);
            EditorGUILayout.PropertyField(m_MinDist, new GUIContent("Min Distance (m)", m_MinDist.tooltip));
            EditorGUILayout.PropertyField(m_MaxDist, new GUIContent("Max Distance (m)", m_MaxDist.tooltip));
            if (m_MaxDist.floatValue < m_MinDist.floatValue) m_MaxDist.floatValue = m_MinDist.floatValue;
            EditorGUILayout.PropertyField(m_TimeConstant, new GUIContent("Time Constant (s)", m_TimeConstant.tooltip));

            EditorGUILayout.Space();
            EditorGUILayout.LabelField("Comfort", EditorStyles.boldLabel);
            EditorGUILayout.PropertyField(m_DepthScale, new GUIContent("User Depth Scale", m_DepthScale.tooltip));
            EditorGUILayout.PropertyField(m_FrontCap, new GUIContent("Front Cap (deg)", m_FrontCap.tooltip));
            EditorGUILayout.PropertyField(m_BehindCap, new GUIContent("Behind Cap (deg)", m_BehindCap.tooltip));
            if (!m_DriveStrength.boolValue)
                EditorGUILayout.HelpBox("Drive Stereo Strength is off: the caps only shape the dead band and the " +
                                        "Scene-view comfort volume; the rig's IPD Factor is left alone.", MessageType.None);

            EditorGUILayout.Space();
            EditorGUILayout.LabelField("Sampling", EditorStyles.boldLabel);
            if (mode == DisplayXRAutoConvergence.Mode.Auto || mode == DisplayXRAutoConvergence.Mode.Raycast)
            {
                EditorGUILayout.PropertyField(m_LayerMask, new GUIContent("Raycast Layers", m_LayerMask.tooltip));
                EditorGUILayout.PropertyField(m_RayMax, new GUIContent("Raycast Max Distance", m_RayMax.tooltip));
            }
            if (mode != DisplayXRAutoConvergence.Mode.Target)
                EditorGUILayout.PropertyField(m_IgnoreNearer, new GUIContent("Ignore Nearer Than (m)", m_IgnoreNearer.tooltip));

            s_Advanced = EditorGUILayout.Foldout(s_Advanced, "Advanced", true);
            if (s_Advanced)
            {
                EditorGUI.indentLevel++;
                EditorGUILayout.PropertyField(m_SamplePeriod, new GUIContent("Sample Period (s)", m_SamplePeriod.tooltip));
                EditorGUILayout.PropertyField(m_Percentile);
                EditorGUILayout.PropertyField(m_Blend);
                EditorGUILayout.PropertyField(m_DeadBand);
                EditorGUILayout.PropertyField(m_RayCols);
                EditorGUILayout.PropertyField(m_RayRows);
                EditorGUI.indentLevel--;
            }

            serializedObject.ApplyModifiedProperties();

            // ---- Debug readout (Play Mode) ----
            EditorGUILayout.Space();
            EditorGUILayout.LabelField("Status", EditorStyles.boldLabel);
            if (!Application.isPlaying)
            {
                EditorGUILayout.LabelField("Runs in Play Mode (the provider session).", EditorStyles.miniLabel);
                return;
            }
            using (new EditorGUI.DisabledScope(true))
            {
                string state = !ac.isActiveAndEnabled ? "disabled"
                    : !ac.IsDriving ? "idle (no session, not the active rig, or no confident sample yet)"
                    : ac.IsHolding ? "holding (last sample rejected)" : "driving";
                EditorGUILayout.LabelField("State", state);
                EditorGUILayout.LabelField("Source", ac.Source.ToString());
                EditorGUILayout.LabelField("Convergence",
                    $"{Fmt(ac.ConvergenceDistance)} m  ->  {Fmt(ac.TargetConvergenceDistance)} m");
                EditorGUILayout.LabelField("Stereo Strength",
                    $"{ac.StereoStrength:F2}  (authored {ac.AuthoredIpdFactor:F2})");
                EditorGUILayout.LabelField("Disparity near / far",
                    $"{ac.NearDisparityPercent:F2} % / {ac.FarDisparityPercent:+0.00;-0.00} %  of view width");
                EditorGUILayout.LabelField("Caps front / behind",
                    $"{ac.FrontCapPercent:F2} % / {ac.BehindCapPercent:F2} %");
                EditorGUILayout.LabelField("Samples",
                    $"{ac.ValidSamples} with depth of {ac.TotalSamples}, confidence {ac.Confidence:F2}");
            }
            if (GUILayout.Button("Reset Convergence (snap next sample)")) ac.ResetConvergence();
            Repaint(); // live readout
        }

        static string Fmt(float meters) => float.IsInfinity(meters) ? "infinity" : meters.ToString("F2");
    }

    /// <summary>DisplayXR > Gizmos > Comfort Volume: show the front / behind comfort limits
    /// with the rig gizmos (editor preference, on by default).</summary>
    static class DisplayXRComfortVolumeMenu
    {
        const string kMenu = "DisplayXR/Gizmos/Comfort Volume";

        [MenuItem(kMenu)]
        static void Toggle()
        {
            DisplayXRGizmoHelpers.ShowComfortVolume = !DisplayXRGizmoHelpers.ShowComfortVolume;
            SceneView.RepaintAll();
        }

        [MenuItem(kMenu, validate = true)]
        static bool Validate()
        {
            Menu.SetChecked(kMenu, DisplayXRGizmoHelpers.ShowComfortVolume);
            return true;
        }
    }
}
