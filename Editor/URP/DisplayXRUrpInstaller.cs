// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Editor wiring for the two URP renderer features (#127/#129):
//
//   1. Setup URP Projection Fix  — adds the always-on KooimaProjectionFixFeature
//      (off-axis projection injection) to the project's UniversalRendererData. This
//      is UNIVERSAL: every URP DisplayXR app needs it for correct off-center Kooima.
//      Normally auto-wired by DisplayXRUrpAutoWire; this menu/headless entry point
//      lets you (re)apply it explicitly or in a batch build.
//
//   2. Setup URP Foreground Clip — OPT-IN, for transparent-overlay apps only.
//      Creates a material from the DisplayXR/ForegroundClipURP shader and adds
//      Unity's built-in FullScreenPassRendererFeature pointed at it. A normal 3D
//      URP app should NOT run this.
//
// Both entry points are idempotent and callable headlessly (interactive=false
// suppresses dialogs) so a Player can be built with the features pre-attached:
//   Unity.exe -batchmode -executeMethod DisplayXR.Editor.URP.DisplayXRUrpInstaller.SetupProjectionFixHeadless
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
using DisplayXR.URP;

namespace DisplayXR.Editor.URP
{
    public static class DisplayXRUrpInstaller
    {
        const string kProjFixName = "Kooima Projection Fix";
        const string kClipShaderName = "DisplayXR/ForegroundClipURP";
        const string kClipFeatureName = "DisplayXR Foreground Clip";
        const string kClipMaterialPath = "Assets/DisplayXR/DXRForegroundClip.mat";

        // ---- Projection fix (universal) -------------------------------------

        [MenuItem("DisplayXR/Setup URP Projection Fix")]
        static void SetupProjectionFixMenu() => InstallProjectionFix(interactive: true);

        /// <summary>Headless entry point for -batchmode -executeMethod.</summary>
        public static void SetupProjectionFixHeadless() => InstallProjectionFix(interactive: false);

        /// <summary>
        /// Add KooimaProjectionFixFeature to the URP renderer if it isn't already
        /// there. Returns true if present (newly added or already wired).
        /// </summary>
        public static bool InstallProjectionFix(bool interactive)
        {
            var rendererData = FindRendererData();
            if (rendererData == null)
            {
                const string msg = "URP renderer asset (UniversalRendererData) not found. " +
                    "Assign a URP pipeline asset in Project Settings > Graphics, then retry.";
                Report(interactive, "Projection Fix", msg, error: true);
                return false;
            }

            bool ok;
            try { ok = EnsureFeature<KooimaProjectionFixFeature>(rendererData, kProjFixName, null); }
            catch (Exception e)
            {
                Debug.LogWarning("[DisplayXRUrp] projection-fix wiring failed: " + e.Message);
                ok = false;
            }

            Report(interactive, "Projection Fix",
                ok ? "Kooima Projection Fix is wired into " + rendererData.name + "."
                   : "Couldn't wire the projection fix automatically — add a "
                     + "'Kooima Projection Fix (Render Feature)' to your URP renderer manually.",
                error: !ok);
            return ok;
        }

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

        /// <summary>
        /// True if the renderer already has a feature of type T. Cheap check used by
        /// the auto-wire bootstrap so it doesn't re-add on every domain reload.
        /// </summary>
        internal static bool HasFeature<T>(ScriptableRendererData rendererData)
            where T : ScriptableRendererFeature
        {
            foreach (var f in rendererData.rendererFeatures)
                if (f is T) return true;
            return false;
        }

        // Generic add for a DisplayXR-authored feature (no material). Mirrors URP's
        // "Add Renderer Feature": instantiate, AddObjectToAsset, register in
        // m_RendererFeatures + m_RendererFeatureMap. Idempotent.
        static bool EnsureFeature<T>(ScriptableRendererData rendererData, string displayName,
                                     Action<T> configure) where T : ScriptableRendererFeature
        {
            if (HasFeature<T>(rendererData)) return true;

            var so = new SerializedObject(rendererData);
            var features = so.FindProperty("m_RendererFeatures");
            var map = so.FindProperty("m_RendererFeatureMap");
            if (features == null || map == null) return false;

            var feature = ScriptableObject.CreateInstance<T>();
            if (feature == null) return false;
            feature.name = displayName;
            configure?.Invoke(feature);

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
