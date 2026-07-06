// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Editor wiring for the opt-in URP Foreground Clip renderer feature (#57/#129).
//
//   Setup URP Foreground Clip — OPT-IN, for transparent-overlay apps only.
//   Creates a material from the DisplayXR/ForegroundClipURP shader and adds
//   Unity's built-in FullScreenPassRendererFeature pointed at it. A normal 3D
//   URP app should NOT run this.
//
// (There is no longer a "URP Projection Fix" feature to install: the provider now
// hands Unity a full per-eye projection matrix, so URP consumes the off-center
// Kooima frustum correctly with no RendererFeature — same as BiRP/HDRP.)
//
// The entry point is idempotent and callable headlessly (interactive=false
// suppresses dialogs) so a Player can be built with the feature pre-attached:
//   Unity.exe -batchmode -executeMethod DisplayXR.Editor.URP.DisplayXRUrpInstaller.SetupForegroundClipHeadless
//
// The renderer-asset edits mirror exactly what URP's "Add Renderer Feature" button
// does (instantiate, AddObjectToAsset, register in m_RendererFeatures +
// m_RendererFeatureMap) and are wrapped so a failure never corrupts the asset.

using System;
using System.IO;
using UnityEditor;
using UnityEngine;
using UnityEngine.Rendering;
using UnityEngine.Rendering.Universal;

namespace DisplayXR.Editor.URP
{
    public static class DisplayXRUrpInstaller
    {
        const string kClipShaderName = "DisplayXR/ForegroundClipURP";
        const string kClipFeatureName = "DisplayXR Foreground Clip";
        const string kClipMaterialPath = "Assets/DisplayXR/DXRForegroundClip.mat";

        // ---- Foreground clip (opt-in) ---------------------------------------

        [MenuItem("DisplayXR/Setup URP Foreground Clip")]
        static void SetupForegroundClipMenu() => InstallForegroundClip(interactive: true);

        /// <summary>Headless entry point for -batchmode -executeMethod.</summary>
        public static void SetupForegroundClipHeadless() => InstallForegroundClip(interactive: false);

        /// <summary>
        /// Create the clip material and wire Unity's FullScreenPassRendererFeature.
        /// Returns true if the renderer feature was wired.
        /// </summary>
        public static bool InstallForegroundClip(bool interactive)
        {
            var shader = Shader.Find(kClipShaderName);
            if (shader == null)
            {
                Report(interactive, "Foreground Clip",
                    $"Shader '{kClipShaderName}' not found (it ships in the DisplayXR URP " +
                    "assembly; make sure URP is installed and the package imported cleanly).",
                    error: true);
                return false;
            }

            // 1. Material (reliable). Lives in the user's project so it can be edited.
            Directory.CreateDirectory(Path.GetDirectoryName(kClipMaterialPath));
            var mat = AssetDatabase.LoadAssetAtPath<Material>(kClipMaterialPath);
            if (mat == null)
            {
                mat = new Material(shader) { name = "DXRForegroundClip" };
                AssetDatabase.CreateAsset(mat, kClipMaterialPath);
            }
            else if (mat.shader != shader)
            {
                mat.shader = shader;
                EditorUtility.SetDirty(mat);
            }
            AssetDatabase.SaveAssets();

            // 2. Renderer feature (best effort).
            var rendererData = FindRendererData();
            string manual =
                "Manual wiring (3 clicks):\n" +
                "  1. Select your URP Renderer asset (UniversalRendererData)\n" +
                "  2. Add Renderer Feature → Full Screen Pass Renderer Feature\n" +
                "  3. Pass Material = DXRForegroundClip,\n" +
                "     Injection Point = Before Rendering Post Processing,\n" +
                "     Requirements = Depth";

            if (rendererData == null)
            {
                Report(interactive, "Foreground Clip",
                    "Material created at " + kClipMaterialPath + ".\n\n" +
                    "Couldn't find a URP renderer asset; assign a URP pipeline asset, then:\n\n" + manual,
                    error: false);
                return false;
            }

            bool added;
            try { added = TryAddFullScreenFeature(rendererData, mat); }
            catch (Exception e)
            {
                Debug.LogWarning("[DisplayXRUrp] foreground-clip wiring failed: " + e.Message);
                added = false;
            }

            Report(interactive, "Foreground Clip",
                added ? "Material + Full Screen Pass feature wired into " + rendererData.name + "."
                      : "Material created at " + kClipMaterialPath + ".\n\n" +
                        "Auto-wiring the renderer feature didn't complete — do it manually:\n\n" + manual,
                error: false);
            return added;
        }

        // ---- Shared renderer-asset plumbing ---------------------------------

        internal static ScriptableRendererData FindRendererData()
        {
            foreach (var guid in AssetDatabase.FindAssets("t:UniversalRendererData"))
            {
                var path = AssetDatabase.GUIDToAssetPath(guid);
                var data = AssetDatabase.LoadAssetAtPath<ScriptableRendererData>(path);
                if (data != null) return data;
            }
            return null;
        }

        static bool TryAddFullScreenFeature(ScriptableRendererData rendererData, Material mat)
        {
            var so = new SerializedObject(rendererData);
            var features = so.FindProperty("m_RendererFeatures");
            var map = so.FindProperty("m_RendererFeatureMap");
            if (features == null || map == null) return false;

            // Already installed? (idempotent)
            for (int i = 0; i < features.arraySize; i++)
            {
                var existing = features.GetArrayElementAtIndex(i).objectReferenceValue;
                if (existing is FullScreenPassRendererFeature f && f.name == kClipFeatureName)
                {
                    ConfigureClipFeature(f, mat);
                    EditorUtility.SetDirty(rendererData);
                    AssetDatabase.SaveAssets();
                    return true;
                }
            }

            var feature = ScriptableObject.CreateInstance<FullScreenPassRendererFeature>();
            if (feature == null) return false;
            feature.name = kClipFeatureName;
            ConfigureClipFeature(feature, mat);

            AssetDatabase.AddObjectToAsset(feature, rendererData);

            int idx = features.arraySize;
            features.InsertArrayElementAtIndex(idx);
            features.GetArrayElementAtIndex(idx).objectReferenceValue = feature;
            map.InsertArrayElementAtIndex(idx);
            if (AssetDatabase.TryGetGUIDAndLocalFileIdentifier(feature, out _, out long localId))
                map.GetArrayElementAtIndex(idx).longValue = localId;

            so.ApplyModifiedProperties();
            EditorUtility.SetDirty(rendererData);
            AssetDatabase.SaveAssets();
            AssetDatabase.ImportAsset(AssetDatabase.GetAssetPath(rendererData));
            return true;
        }

        // FullScreenPassRendererFeature exposes these as PUBLIC fields in URP 17.
        // URP 17's InjectionPoint enum has no AfterRenderingTransparents — the overlay
        // content is opaque, so depth+color are fully populated by the time transparents
        // finish, and BeforeRenderingPostProcessing is the first slot after them.
        static void ConfigureClipFeature(FullScreenPassRendererFeature feature, Material mat)
        {
            feature.passMaterial = mat;
            feature.injectionPoint = FullScreenPassRendererFeature.InjectionPoint.BeforeRenderingPostProcessing;
            feature.requirements = ScriptableRenderPassInput.Depth;
            feature.fetchColorBuffer = true;  // binds camera color to _BlitTexture for passthrough
            feature.passIndex = 0;
            EditorUtility.SetDirty(feature);
        }

        static void Report(bool interactive, string title, string msg, bool error)
        {
            if (interactive) EditorUtility.DisplayDialog("DisplayXR URP — " + title, msg, "OK");
            else if (error) Debug.LogWarning("[DisplayXRUrp] " + title + ": " + msg);
            else Debug.Log("[DisplayXRUrp] " + title + ": " + msg);
        }
    }
}
