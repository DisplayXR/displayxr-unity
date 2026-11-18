// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

using System.IO;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;

namespace DisplayXR.Samples.Editor
{
    /// <summary>
    /// The Stereo Media sample's editor side:
    /// <list type="bullet">
    /// <item><b>DisplayXR > Samples > Create Stereo Media Scene</b> makes a new scene with
    /// a camera carrying a display-centric rig and a "Stereo Media" object carrying
    /// <see cref="StereoMediaSetup"/>. Press Play and the content is built at runtime.</item>
    /// <item>The <b>Create Scene Content</b> button on <see cref="StereoMediaSetup"/> authors
    /// that content into the scene for real - in the style of the Basic Scene sample -
    /// and removes the setup component.</item>
    /// </list>
    ///
    /// The generated test picture is an in-memory texture on the runtime path. A saved
    /// scene cannot reference that, so the button writes it out as a PNG ASSET next to the
    /// scene (in your project, never in the package) and points the component at the asset.
    /// </summary>
    [CustomEditor(typeof(StereoMediaSetup))]
    internal sealed class StereoMediaSetupEditor : UnityEditor.Editor
    {
        const string k_Menu = "DisplayXR/Samples/Create Stereo Media Scene";

        [MenuItem(k_Menu)]
        static void CreateScene()
        {
            if (!EditorSceneManager.SaveCurrentModifiedScenesIfUserWantsTo()) return;
            var scene = EditorSceneManager.NewScene(NewSceneSetup.DefaultGameObjects, NewSceneMode.Single);

            Camera cam = Camera.main;
            if (cam != null)
            {
                // The rig's transform is the screen: put it at the origin, facing +Z.
                cam.transform.position = Vector3.zero;
                cam.transform.rotation = Quaternion.identity;
                cam.clearFlags = CameraClearFlags.SolidColor;
                cam.backgroundColor = new Color(0.02f, 0.02f, 0.03f, 1f);
                StereoMediaSetup.EnsureRig();
            }

            var go = new GameObject("Stereo Media");
            go.AddComponent<StereoMediaSetup>();
            Selection.activeGameObject = go;
            EditorSceneManager.MarkSceneDirty(scene);
            Debug.Log("[StereoMediaSetup] Scene created. Press Play to see the test picture; save the scene " +
                      "first if you want to use Create Scene Content.");
        }

        [MenuItem(k_Menu, true)]
        static bool CreateSceneValidate() => !Application.isPlaying;

        public override void OnInspectorGUI()
        {
            DrawDefaultInspector();
            var setup = (StereoMediaSetup)target;

            EditorGUILayout.Space();
            EditorGUILayout.HelpBox(
                "Press Play and the content is built at runtime, then discarded on Stop.\n\n" +
                "Create Scene Content authors it into the scene instead - a Stereo Media component you can " +
                "edit and save - and removes this component." +
                (setup.UseVideo ? "" : " The test picture is written as a PNG next to the scene."),
                MessageType.Info);

            using (new EditorGUI.DisabledScope(Application.isPlaying))
            {
                if (GUILayout.Button("Create Scene Content", GUILayout.Height(24)))
                    CreateContent(setup);
            }
        }

        static void CreateContent(StereoMediaSetup setup)
        {
            var scene = setup.gameObject.scene;
            if (!scene.IsValid() || string.IsNullOrEmpty(scene.path))
            {
                Debug.LogWarning("[StereoMediaSetup] Save the scene first - the test picture and the cube's " +
                                 "material are written next to the scene file.", setup);
                return;
            }
            string folder = AssetFolder(scene.path);
            if (folder == null)
            {
                Debug.LogWarning("[StereoMediaSetup] Could not work out where to put the assets " +
                                 "(is the scene saved inside Assets/?).", setup);
                return;
            }

            int group = Undo.GetCurrentGroup();
            Undo.SetCurrentGroupName("Create Scene Content");

            Camera cam = Camera.main;
            if (cam != null && cam.GetComponent<DisplayXRDisplay>() == null && cam.GetComponent<DisplayXRCamera>() == null)
            {
                var rig = Undo.AddComponent<DisplayXRDisplay>(cam.gameObject);
                rig.virtualDisplayHeight = 1f;
            }

            Texture pattern = setup.UseVideo ? null : PatternAsset(folder, setup);

            var host = setup.gameObject;
            var media = host.GetComponent<DisplayXRStereoMedia>();
            if (media == null) media = Undo.AddComponent<DisplayXRStereoMedia>(host);
            else Undo.RecordObject(media, "Create Scene Content");
            setup.Configure(media, pattern, cam);
            EditorUtility.SetDirty(media);

            if (setup.addReferenceCube)
            {
                var cube = StereoMediaSetup.CreateReferenceCube(cam, CubeMaterial(folder));
                if (cube != null)
                {
                    if (cube.scene != scene)
                        UnityEngine.SceneManagement.SceneManager.MoveGameObjectToScene(cube, scene);
                    Undo.RegisterCreatedObjectUndo(cube, "Create Scene Content");
                }
            }

            // The component exists to produce this content; left in place it would do
            // nothing (it stops when it finds a Stereo Media component), so take it out.
            Undo.DestroyObjectImmediate(setup);

            Undo.CollapseUndoOperations(group);
            EditorSceneManager.MarkSceneDirty(scene);
            AssetDatabase.SaveAssets();
            Debug.Log($"[StereoMediaSetup] Authored the Stereo Media content into '{scene.name}', assets in {folder}. " +
                      "Save the scene to keep it.");
        }

        // Assets live next to the scene so they travel with it.
        static string AssetFolder(string scenePath)
        {
            string dir = Path.GetDirectoryName(scenePath)?.Replace('\\', '/');
            if (string.IsNullOrEmpty(dir) || !dir.StartsWith("Assets")) return null;
            string folder = dir + "/StereoMedia";
            if (!AssetDatabase.IsValidFolder(folder))
                AssetDatabase.CreateFolder(dir, "StereoMedia");
            return folder;
        }

        // The generated picture as a PNG asset, rewritten on every press so a changed
        // resolution or layout takes effect.
        static Texture2D PatternAsset(string folder, StereoMediaSetup setup)
        {
            string path = $"{folder}/StereoMediaTestPattern_{setup.patternLayout}.png";
            Texture2D generated = StereoMediaTestPattern.Create(
                setup.eyeResolution.x, setup.eyeResolution.y, setup.patternLayout);
            File.WriteAllBytes(path, generated.EncodeToPNG());
            DestroyImmediate(generated);
            AssetDatabase.ImportAsset(path, ImportAssetOptions.ForceUpdate);

            // A picture to look at, not a material map: keep its exact size and pixels.
            if (AssetImporter.GetAtPath(path) is TextureImporter importer)
            {
                importer.textureType = TextureImporterType.Default;
                importer.sRGBTexture = true;
                importer.mipmapEnabled = false;
                importer.npotScale = TextureImporterNPOTScale.None;
                importer.wrapMode = TextureWrapMode.Clamp;
                importer.maxTextureSize = 8192;
                importer.textureCompression = TextureImporterCompression.Uncompressed;
                importer.alphaSource = TextureImporterAlphaSource.None;
                importer.SaveAndReimport();
            }
            return AssetDatabase.LoadAssetAtPath<Texture2D>(path);
        }

        static Material CubeMaterial(string folder)
        {
            string path = $"{folder}/StereoMediaReferenceCube.mat";
            var existing = AssetDatabase.LoadAssetAtPath<Material>(path);
            if (existing != null) return existing;
            var mat = new Material(StereoMediaSetup.FindLitShader());
            StereoMediaSetup.Tint(mat, new Color(0.9f, 0.5f, 0.1f));
            AssetDatabase.CreateAsset(mat, path);
            return mat;
        }
    }
}
