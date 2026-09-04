// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

using UnityEngine;

namespace DisplayXR.Samples
{
    /// <summary>
    /// Creates a minimal stereo test scene with colored cubes at varying depths.
    /// Attach to any GameObject, or use the included BasicScene.unity scene file
    /// (where it sits on the "Scene Setup" object next to the Main Camera).
    ///
    /// Two ways to get the content. Press Play and it is built at runtime, then
    /// discarded on Stop — the behaviour this sample has always had. Or press
    /// <b>Create Scene Content</b> in the inspector, which authors the same objects
    /// into the scene for real (selectable, editable, saveable, undoable) and removes
    /// this component. Use the button if you want to move a cube, retint the floor,
    /// or build your own scene on top of the layout.
    /// </summary>
    public class BasicSceneSetup : MonoBehaviour
    {
        void Start()
        {
            // Only create objects if OUR scene is empty (no cubes present).
            //
            // Scope the check to this GameObject's scene on purpose. A plain
            // FindAnyObjectByType<MeshRenderer>() also scans the DontDestroyOnLoad
            // scene, where the DisplayXR boot splash (on by default, spawned at
            // BeforeSceneLoad) builds its logo quads with MeshRenderer — so the
            // guard tripped on the splash and this sample created nothing at all
            // unless you disabled the splash (issue #262).
            var scene = gameObject.scene;
            foreach (var mr in FindObjectsByType<MeshRenderer>(FindObjectsSortMode.None))
                if (mr.gameObject.scene == scene)
                    return;

            // Reset Main Camera to origin so it faces the test objects
            var cam = Camera.main;
            if (cam != null)
            {
                cam.transform.position = Vector3.zero;
                cam.transform.rotation = Quaternion.identity;
            }

            // Put the convergence plane on the mid cube (0.5 m) so the content
            // straddles the display: near cube in front of the glass, far cube behind
            // it. Left at its default of 0 the rig converges at infinity (parallel
            // projection) and everything here reads as pure pop-out.
            var rig = cam != null ? cam.GetComponent<DisplayXRCamera>() : null;
            if (rig != null && rig.invConvergenceDistance == 0f)
                rig.invConvergenceDistance = 1f / 0.5f;

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
