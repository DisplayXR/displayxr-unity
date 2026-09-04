// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

using System.IO;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;

namespace DisplayXR.Samples.Editor
{
    /// <summary>
    /// Adds the "Create Scene Content" button to <see cref="BasicSceneSetup"/>.
    ///
    /// It authors the sample's cubes, floor and light as REAL scene objects —
    /// selectable, editable, saveable, undoable — and then removes the setup
    /// component, which has done its job. That is the difference between a sample you
    /// can look at and one you can fork: on the runtime-only path there is nothing in
    /// the scene to select, and anything changed during Play is discarded on Stop.
    ///
    /// THE MATERIAL IS THE WHOLE TRICK. The runtime path builds materials with
    /// `new Material(...)`, an in-memory object — fine, because nothing is ever saved.
    /// Author objects with a material like that and the saved scene references
    /// something that does not exist on disk, so the content returns missing/magenta
    /// on reload. So this writes a material ASSET per distinct colour next to the
    /// scene, reusing it if already present.
    ///
    /// Nothing authored is committed to the package: the button runs in the user's
    /// project with their pipeline active, so `Shader.Find` resolves to URP/Lit or
    /// Standard exactly as the runtime path would. The shipped scene stays
    /// pipeline-agnostic, which is the constraint #261 established.
    /// </summary>
    [CustomEditor(typeof(BasicSceneSetup))]
    internal sealed class BasicSceneSetupEditor : UnityEditor.Editor
    {
        public override void OnInspectorGUI()
        {
            DrawDefaultInspector();

            EditorGUILayout.Space();
            EditorGUILayout.HelpBox(
                "Press Play and the content is built at runtime, then discarded on Stop.\n\n" +
                "Create Scene Content authors it into the scene instead — real objects you " +
                "can select, move and save — and removes this component.",
                MessageType.Info);

            using (new EditorGUI.DisabledScope(Application.isPlaying))
            {
                if (GUILayout.Button("Create Scene Content", GUILayout.Height(24)))
                    CreateContent((BasicSceneSetup)target);
            }
        }

        private static void CreateContent(BasicSceneSetup setup)
        {
            var scene = setup.gameObject.scene;
            if (!scene.IsValid() || string.IsNullOrEmpty(scene.path))
            {
                Debug.LogWarning("[BasicSceneSetup] Save the scene first — the materials are " +
                                 "written next to the scene file.", setup);
                return;
            }

            string folder = MaterialFolder(scene.path);
            if (folder == null)
            {
                Debug.LogWarning("[BasicSceneSetup] Could not work out where to put the materials " +
                                 "(is the scene saved inside Assets/?).", setup);
                return;
            }

            int group = Undo.GetCurrentGroup();
            Undo.SetCurrentGroupName("Create Scene Content");

            var cam = Camera.main;
            if (cam != null)
            {
                Undo.RecordObject(cam.transform, "Create Scene Content");
                cam.transform.position = Vector3.zero;
                cam.transform.rotation = Quaternion.identity;
            }

            int count = 0;
            count += Create(scene, folder, "NearCube", PrimitiveType.Cube,
                            new Vector3(-0.3f, 0f, 0.3f), Vector3.one * 0.15f, Color.red);
            count += Create(scene, folder, "MidCube", PrimitiveType.Cube,
                            new Vector3(0f, 0f, 0.5f), Vector3.one * 0.2f, Color.green);
            count += Create(scene, folder, "FarCube", PrimitiveType.Cube,
                            new Vector3(0.3f, 0f, 1.0f), Vector3.one * 0.25f, new Color(0.2f, 0.4f, 1f));
            count += Create(scene, folder, "Floor", PrimitiveType.Plane,
                            new Vector3(0f, -0.3f, 0.5f), new Vector3(0.3f, 1f, 0.3f),
                            new Color(0.3f, 0.3f, 0.3f), rotate: false);

            var lightGo = new GameObject("DirectionalLight");
            var light = lightGo.AddComponent<Light>();
            light.type = LightType.Directional;
            lightGo.transform.rotation = Quaternion.Euler(50f, -30f, 0f);
            MoveToScene(lightGo, scene);
            Undo.RegisterCreatedObjectUndo(lightGo, "Create Scene Content");
            count++;

            // The component exists to produce this content. With the content authored
            // it would only be a way to create a second copy on the next Play. If it was
            // alone on its GameObject — the shipped scene's "Scene Setup" object — take
            // the object too rather than leaving an empty one behind.
            var host = setup.gameObject;
            bool hostIsOnlyForSetup =
                host.GetComponents<Component>().Length == 2 &&   // Transform + this
                host.transform.childCount == 0;
            if (hostIsOnlyForSetup)
                Undo.DestroyObjectImmediate(host);
            else
                Undo.DestroyObjectImmediate(setup);

            Undo.CollapseUndoOperations(group);
            EditorSceneManager.MarkSceneDirty(scene);
            AssetDatabase.SaveAssets();

            Debug.Log($"[BasicSceneSetup] Authored {count} objects into '{scene.name}', " +
                      $"materials in {folder}. Save the scene to keep them.");
        }

        private static int Create(UnityEngine.SceneManagement.Scene scene, string folder,
                                  string name, PrimitiveType primitive,
                                  Vector3 position, Vector3 scale, Color color, bool rotate = true)
        {
            var go = GameObject.CreatePrimitive(primitive);
            go.name = name;
            go.transform.position = position;
            go.transform.localScale = scale;
            if (rotate) go.transform.rotation = Quaternion.Euler(15f, 30f, 0f);
            go.GetComponent<Renderer>().sharedMaterial = MaterialAsset(folder, color);
            MoveToScene(go, scene);
            Undo.RegisterCreatedObjectUndo(go, "Create Scene Content");
            return 1;
        }

        // CreatePrimitive drops objects into the ACTIVE scene, which is not necessarily
        // the one the setup component lives in (additively loaded scenes).
        private static void MoveToScene(GameObject go, UnityEngine.SceneManagement.Scene scene)
        {
            if (go.scene != scene)
                UnityEngine.SceneManagement.SceneManager.MoveGameObjectToScene(go, scene);
        }

        // Materials live next to the scene so they travel with it, and a second press
        // in a different scene folder does not collide.
        private static string MaterialFolder(string scenePath)
        {
            string dir = Path.GetDirectoryName(scenePath)?.Replace('\\', '/');
            if (string.IsNullOrEmpty(dir) || !dir.StartsWith("Assets")) return null;
            string folder = dir + "/Materials";
            if (!AssetDatabase.IsValidFolder(folder))
                AssetDatabase.CreateFolder(dir, "Materials");
            return folder;
        }

        // One asset per distinct colour, reused on a second press rather than
        // accumulating duplicates.
        //
        // Undo does not remove these. Asset creation is not part of the scene's undo
        // stack in Unity, so Ctrl+Z takes the objects back out and leaves the .mat
        // files on disk — which is what you want anyway: press the button again and
        // they are picked up rather than rewritten.
        private static Material MaterialAsset(string folder, Color color)
        {
            string path = $"{folder}/BasicScene_{ColorUtility.ToHtmlStringRGB(color)}.mat";
            var existing = AssetDatabase.LoadAssetAtPath<Material>(path);
            if (existing != null) return Tint(existing, color);

            var shader = Shader.Find("Universal Render Pipeline/Lit") ?? Shader.Find("Standard");
            var mat = Tint(new Material(shader), color);
            AssetDatabase.CreateAsset(mat, path);
            return mat;
        }

        private static Material Tint(Material mat, Color color)
        {
            if (mat.HasProperty("_BaseColor")) mat.SetColor("_BaseColor", color); // URP
            if (mat.HasProperty("_Color")) mat.SetColor("_Color", color);         // Built-in
            return mat;
        }
    }
}
