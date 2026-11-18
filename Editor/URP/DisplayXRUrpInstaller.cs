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
using System.Collections.Generic;
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

            // 2. Renderer feature (best effort), on every renderer asset the project
            // actually renders the clip rigs with - not on whichever asset a search
            // happens to return first (#462).
            bool guessed;
            var targets = ResolveClipRenderers(out guessed);
            var rendererData = targets.Count > 0 ? targets[0] : null;
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

            bool added = true;
            var names = new List<string>();
            foreach (var target in targets)
            {
                names.Add(target.name);
                try { added &= TryAddFullScreenFeature(target, mat); }
                catch (Exception e)
                {
                    Debug.LogWarning("[DisplayXRUrp] foreground-clip wiring failed on " + target.name + ": " + e.Message);
                    added = false;
                }
            }

            string wiredInto = string.Join(", ", names.ToArray());
            Report(interactive, "Foreground Clip",
                added ? "Material + Full Screen Pass feature wired into " + wiredInto + "." +
                        (guessed
                            ? "\n\nNo URP pipeline asset is assigned in Graphics or Quality settings, so " +
                              "this is simply the first URP renderer asset found in the project. Check " +
                              "it is the one your pipeline asset uses."
                            : "")
                      : "Material created at " + kClipMaterialPath + ".\n\n" +
                        "Auto-wiring the renderer feature didn't complete — do it manually:\n\n" + manual,
                error: false);
            return added;
        }

        // ---- Shared renderer-asset plumbing ---------------------------------

        /// <summary>
        /// Last resort only: the first URP renderer asset a project search returns. It
        /// says nothing about which asset is in use - see <see cref="ResolveClipRenderers"/>.
        /// </summary>
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
        /// The renderer assets the foreground clip has to be on: for every URP pipeline
        /// asset the project can run with (the active one, the Graphics default, and each
        /// quality level's override), the renderer each clip rig camera in the open
        /// scenes selects - or the pipeline's default renderer when no such rig is open.
        /// <para>
        /// <paramref name="guessed"/> is true when no URP pipeline asset is assigned
        /// anywhere and the result is only the first renderer asset found in the project.
        /// </para>
        /// </summary>
        internal static List<ScriptableRendererData> ResolveClipRenderers(out bool guessed)
        {
            // -1 = "the pipeline asset's default renderer", as on the camera.
            var rendererIndices = new List<int>();
            foreach (var camera in DisplayXRProjectValidation.FindRigCamerasWithForegroundClip())
            {
                int index = -1;
                var extra = camera.GetComponent<UniversalAdditionalCameraData>();
                if (extra != null)
                {
                    var prop = new SerializedObject(extra).FindProperty("m_RendererIndex");
                    if (prop != null) index = prop.intValue;
                }
                if (!rendererIndices.Contains(index)) rendererIndices.Add(index);
            }
            if (rendererIndices.Count == 0) rendererIndices.Add(-1);

            var pipelines = new List<UniversalRenderPipelineAsset>();
            AddPipeline(pipelines, GraphicsSettings.currentRenderPipeline);
            AddPipeline(pipelines, GraphicsSettings.defaultRenderPipeline);
            int levels = QualitySettings.names.Length;
            for (int i = 0; i < levels; i++)
                AddPipeline(pipelines, QualitySettings.GetRenderPipelineAssetAt(i));

            var result = new List<ScriptableRendererData>();
            foreach (var pipeline in pipelines)
            {
                var renderers = pipeline.rendererDataList;
                if (renderers.Length == 0) continue;
                int defaultIndex = 0;
                var prop = new SerializedObject(pipeline).FindProperty("m_DefaultRendererIndex");
                if (prop != null) defaultIndex = prop.intValue;
                foreach (int wanted in rendererIndices)
                {
                    // URP itself falls back to the default renderer for a bad index.
                    int index = wanted >= 0 && wanted < renderers.Length ? wanted : defaultIndex;
                    if (index < 0 || index >= renderers.Length) index = 0;
                    var data = renderers[index];
                    if (data != null && !result.Contains(data)) result.Add(data);
                }
            }

            guessed = false;
            if (result.Count == 0)
            {
                var first = FindRendererData();
                if (first != null)
                {
                    result.Add(first);
                    guessed = true;
                }
            }
            return result;
        }

        static void AddPipeline(List<UniversalRenderPipelineAsset> list, RenderPipelineAsset asset)
        {
            var urp = asset as UniversalRenderPipelineAsset;
            if (urp != null && !list.Contains(urp)) list.Add(urp);
        }

        /// <summary>
        /// The renderer assets from <see cref="ResolveClipRenderers"/> that do not carry
        /// the foreground-clip pass yet. Empty = wired everywhere it is needed.
        /// </summary>
        internal static List<ScriptableRendererData> RenderersMissingClip(out bool guessed, out int resolved)
        {
            var targets = ResolveClipRenderers(out guessed);
            resolved = targets.Count;
            var missing = new List<ScriptableRendererData>();
            foreach (var target in targets)
                if (FindClipFeature(target) == null)
                    missing.Add(target);
            return missing;
        }

        /// <summary>
        /// The foreground-clip Full Screen Pass feature on a renderer asset, or null. It is
        /// the one this installer adds (found by its name) or one wired by hand (found by
        /// its material's shader). The single definition of "is the clip pass wired" -
        /// the installer's own idempotence check and the project-validation rule both ask
        /// here.
        /// </summary>
        internal static FullScreenPassRendererFeature FindClipFeature(ScriptableRendererData rendererData)
        {
            if (rendererData == null) return null;
            foreach (var feature in rendererData.rendererFeatures)
            {
                var pass = feature as FullScreenPassRendererFeature;
                if (pass == null) continue;
                if (pass.name == kClipFeatureName) return pass;
                if (pass.passMaterial != null && pass.passMaterial.shader != null &&
                    pass.passMaterial.shader.name == kClipShaderName)
                    return pass;
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
            var installed = FindClipFeature(rendererData);
            if (installed != null)
            {
                // A pass someone wired by hand (it runs the clip shader, but it is not
                // the feature this installer creates): it is theirs. Leave its material,
                // injection point and requirements exactly as they set them.
                if (installed.name != kClipFeatureName)
                {
                    Debug.Log("[DisplayXRUrp] " + rendererData.name + " already has a Full Screen Pass running " +
                              kClipShaderName + " ('" + installed.name + "') - left as it is.");
                    return true;
                }
                ConfigureClipFeature(installed, mat);
                EditorUtility.SetDirty(rendererData);
                AssetDatabase.SaveAssets();
                return true;
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
