// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

using UnityEngine;

namespace DisplayXR.Samples
{
    /// <summary>
    /// URP smoke test for the stereo rig camera callbacks. Identical scene layout
    /// to BasicScene, but assumes the project is configured with URP — verifies that
    /// DisplayXRCamera/DisplayXRDisplay drive stereo via RenderPipelineManager
    /// .beginCameraRendering rather than Camera.onPreRender (which URP does not fire).
    ///
    /// Two ways to get the content. Press Play and it is built at runtime, then
    /// discarded on Stop. Or press <b>Create Scene Content</b> in the inspector, which
    /// authors the same objects into the scene for real (selectable, editable,
    /// saveable, undoable) and removes this component.
    /// </summary>
    public class URPBasicSceneSetup : MonoBehaviour
    {
        // True when this GameObject's own scene already has visible geometry.
        //
        // Scoped to our scene on purpose: a plain FindAnyObjectByType<MeshRenderer>()
        // also scans the DontDestroyOnLoad scene, where the DisplayXR boot splash
        // (on by default, spawned at BeforeSceneLoad) builds its logo quads with
        // MeshRenderer — so the guard tripped on the splash and the sample created
        // nothing at all unless you disabled it (issue #262).
        bool SceneHasContent()
        {
            var scene = gameObject.scene;
            foreach (var r in FindObjectsByType<MeshRenderer>(FindObjectsSortMode.None))
                if (r.gameObject.scene == scene)
                    return true;
            return false;
        }

        void Start()
        {
            if (SceneHasContent())
                return;

            var cam = Camera.main;
            if (cam != null)
            {
                cam.transform.position = Vector3.zero;
                cam.transform.rotation = Quaternion.identity;
            }

            CreateCube("NearCube", new Vector3(-0.3f, 0f, 0.3f), 0.15f, Color.red);
            CreateCube("MidCube", new Vector3(0f, 0f, 0.5f), 0.2f, Color.green);
            CreateCube("FarCube", new Vector3(0.3f, 0f, 1.0f), 0.25f, new Color(0.2f, 0.4f, 1f));

            var floor = GameObject.CreatePrimitive(PrimitiveType.Plane);
            floor.name = "Floor";
            floor.transform.position = new Vector3(0f, -0.3f, 0.5f);
            floor.transform.localScale = new Vector3(0.3f, 1f, 0.3f);
            floor.GetComponent<Renderer>().material = CreateMaterial(new Color(0.3f, 0.3f, 0.3f));

            var lightGo = new GameObject("DirectionalLight");
            var light = lightGo.AddComponent<Light>();
            light.type = LightType.Directional;
            lightGo.transform.rotation = Quaternion.Euler(50f, -30f, 0f);
        }

        private void CreateCube(string name, Vector3 position, float size, Color color)
        {
            var cube = GameObject.CreatePrimitive(PrimitiveType.Cube);
            cube.name = name;
            cube.transform.position = position;
            cube.transform.localScale = Vector3.one * size;
            cube.transform.rotation = Quaternion.Euler(15f, 30f, 0f);
            cube.GetComponent<Renderer>().material = CreateMaterial(color);
        }

        private static Material CreateMaterial(Color color)
        {
            var shader = Shader.Find("Universal Render Pipeline/Lit");
            if (shader == null)
            {
                Debug.LogWarning("[URPBasicSceneSetup] URP/Lit shader not found. " +
                    "Install com.unity.render-pipelines.universal and assign a URP " +
                    "Render Pipeline Asset in Project Settings > Graphics.");
                shader = Shader.Find("Standard");
            }
            var mat = new Material(shader);
            if (mat.HasProperty("_BaseColor"))
                mat.SetColor("_BaseColor", color);
            if (mat.HasProperty("_Color"))
                mat.SetColor("_Color", color);
            return mat;
        }
    }
}
