// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

#if UNITY_EDITOR
using UnityEditor;
using UnityEditor.Build;
using UnityEditor.Build.Reporting;
using UnityEngine;

namespace DisplayXR.Samples.Editor
{
    /// <summary>
    /// Ensures URP/Lit is registered in Project Settings > Graphics > Always Included
    /// Shaders before any build. URPBasicSceneSetup resolves URP/Lit at runtime via
    /// Shader.Find, which Unity's build-time shader stripper can't see — without a
    /// static reference, URP/Lit is dropped from standalone builds and the sample
    /// renders as the magenta error material. (Issue #72.)
    ///
    /// Hook is also exposed as a menu item so users can register manually without
    /// triggering a build.
    /// </summary>
    class URPBasicSceneShaderRegistration : IPreprocessBuildWithReport
    {
        const string ShaderName = "Universal Render Pipeline/Lit";

        public int callbackOrder => 0;

        public void OnPreprocessBuild(BuildReport report) => Register();

        [MenuItem("Tools/DisplayXR/Register URP∕Lit in Always Included Shaders")]
        static void RegisterMenu() => Register();

        static void Register()
        {
            var shader = Shader.Find(ShaderName);
            if (shader == null)
            {
                Debug.LogWarning($"[URPBasicScene] Cannot register '{ShaderName}': shader not found. Is URP installed?");
                return;
            }

            var assets = AssetDatabase.LoadAllAssetsAtPath("ProjectSettings/GraphicsSettings.asset");
            if (assets == null || assets.Length == 0)
            {
                Debug.LogWarning("[URPBasicScene] Cannot load GraphicsSettings.asset.");
                return;
            }

            var so = new SerializedObject(assets[0]);
            var includedShaders = so.FindProperty("m_AlwaysIncludedShaders");
            if (includedShaders == null) return;

            for (int i = 0; i < includedShaders.arraySize; i++)
            {
                if (includedShaders.GetArrayElementAtIndex(i).objectReferenceValue == shader)
                    return;
            }

            int idx = includedShaders.arraySize;
            includedShaders.InsertArrayElementAtIndex(idx);
            includedShaders.GetArrayElementAtIndex(idx).objectReferenceValue = shader;
            so.ApplyModifiedProperties();
            AssetDatabase.SaveAssets();

            Debug.Log($"[URPBasicScene] Registered '{ShaderName}' in Project Settings > Graphics > Always Included Shaders so it ships in standalone builds.");
        }
    }
}
#endif
