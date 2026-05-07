// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0

using UnityEngine;

namespace DisplayXR.Samples
{
    /// <summary>
    /// Creates a minimal stereo test scene with colored cubes at varying depths.
    /// Attach to any GameObject, or use the included BasicScene.unity scene file.
    /// </summary>
    public class BasicSceneSetup : MonoBehaviour
    {
        void Start()
        {
            // Only create objects if the scene is empty (no cubes present)
            if (FindAnyObjectByType<MeshRenderer>() != null)
                return;

            // Reset Main Camera to origin so it faces the test objects
            var cam = Camera.main;
            if (cam != null)
            {
                cam.transform.position = Vector3.zero;
                cam.transform.rotation = Quaternion.identity;
            }

            // Near cube (red) — pops out of screen
            CreateCube("NearCube", new Vector3(-0.3f, 0f, 0.3f), 0.15f, Color.red);

            // Mid cube (green) — at screen plane
            CreateCube("MidCube", new Vector3(0f, 0f, 0.5f), 0.2f, Color.green);

            // Far cube (blue) — behind screen
            CreateCube("FarCube", new Vector3(0.3f, 0f, 1.0f), 0.25f, new Color(0.2f, 0.4f, 1f));

            // Floor plane
            var floor = GameObject.CreatePrimitive(PrimitiveType.Plane);
            floor.name = "Floor";
            floor.transform.position = new Vector3(0f, -0.3f, 0.5f);
            floor.transform.localScale = new Vector3(0.3f, 1f, 0.3f);
            var floorMat = CreateMaterial(new Color(0.3f, 0.3f, 0.3f));
            floor.GetComponent<Renderer>().material = floorMat;

            // Directional light
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

            // Slowly rotate for visual interest
            cube.transform.rotation = Quaternion.Euler(15f, 30f, 0f);

            cube.GetComponent<Renderer>().material = CreateMaterial(color);
        }

        private static Material CreateMaterial(Color color)
        {
            // Try URP shader first (Unity 6 / URP projects), fall back to Built-in
            var shader = Shader.Find("Universal Render Pipeline/Lit")
                      ?? Shader.Find("Standard");
            if (shader == null)
            {
                Debug.LogWarning("[BasicSceneSetup] No suitable shader found; using Unity default material");
                var fallback = new Material(Shader.Find("Hidden/InternalErrorShader"));
                fallback.color = color;
                return fallback;
            }
            var mat = new Material(shader);
            if (mat.HasProperty("_BaseColor"))
                mat.SetColor("_BaseColor", color); // URP uses _BaseColor
            if (mat.HasProperty("_Color"))
                mat.SetColor("_Color", color);      // Built-in uses _Color
            return mat;
        }
    }
}
