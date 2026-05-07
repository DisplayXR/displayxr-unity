// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0

using UnityEngine;

namespace DisplayXR.Samples
{
    /// <summary>
    /// Creates a rotating crate on a grid floor with a directional light.
    /// Cameras and stereo rigs should be set up manually in the scene.
    /// </summary>
    public class DisplaySceneSetup : MonoBehaviour
    {
        [Tooltip("Crate rotation speed in degrees per second")]
        public float crateRotationSpeed = 30f;

        private Transform m_Crate;

        void Start()
        {
            // Don't recreate if scene already has content
            if (FindAnyObjectByType<MeshRenderer>() != null)
                return;

            CreateSceneObjects();
        }

        void Update()
        {
            if (m_Crate != null)
                m_Crate.Rotate(Vector3.up, crateRotationSpeed * Time.deltaTime);
        }

        private void CreateSceneObjects()
        {
            // --- Crate at origin ---
            var crate = GameObject.CreatePrimitive(PrimitiveType.Cube);
            crate.name = "Crate";
            crate.transform.position = Vector3.zero;
            crate.transform.localScale = new Vector3(0.06f, 0.06f, 0.06f);
            m_Crate = crate.transform;

            var crateMat = CreateMaterial(new Color(0.6f, 0.4f, 0.2f));
            var tex = LoadCrateTexture();
            if (tex != null)
                crateMat.mainTexture = tex;
            crate.GetComponent<Renderer>().material = crateMat;

            // --- Ground grid ---
            var ground = GameObject.CreatePrimitive(PrimitiveType.Quad);
            ground.name = "GroundGrid";
            ground.transform.position = new Vector3(0f, -0.03f, 0f);
            ground.transform.rotation = Quaternion.Euler(90f, 0f, 0f);
            ground.transform.localScale = Vector3.one;

            var gridMat = CreateMaterial(new Color(0.4f, 0.4f, 0.4f));
            gridMat.mainTextureScale = new Vector2(20f, 20f);

            var checker = new Texture2D(2, 2, TextureFormat.RGBA32, false);
            checker.filterMode = FilterMode.Point;
            checker.wrapMode = TextureWrapMode.Repeat;
            var light = new Color(0.5f, 0.5f, 0.5f);
            var dark = new Color(0.3f, 0.3f, 0.3f);
            checker.SetPixels(new[] { light, dark, dark, light });
            checker.Apply();
            gridMat.mainTexture = checker;
            ground.GetComponent<Renderer>().material = gridMat;

            // --- Key light ---
            var keyLightGo = new GameObject("KeyLight");
            var keyLight = keyLightGo.AddComponent<Light>();
            keyLight.type = LightType.Directional;
            keyLight.color = new Color(1f, 0.95f, 0.85f);
            keyLight.intensity = 1.2f;
            keyLightGo.transform.rotation = Quaternion.Euler(45f, -30f, 0f);
        }

        private static Material CreateMaterial(Color color)
        {
            var shader = Shader.Find("Universal Render Pipeline/Lit")
                      ?? Shader.Find("Standard");
            if (shader == null)
            {
                var fallback = new Material(Shader.Find("Hidden/InternalErrorShader"));
                fallback.color = color;
                return fallback;
            }
            var mat = new Material(shader);
            if (mat.HasProperty("_BaseColor"))
                mat.SetColor("_BaseColor", color);
            if (mat.HasProperty("_Color"))
                mat.SetColor("_Color", color);
            return mat;
        }

        private Texture2D LoadCrateTexture()
        {
            // Try Resources folder first
            var tex = Resources.Load<Texture2D>("Wood_Crate_001_basecolor");
            if (tex != null) return tex;

            // Search all loaded textures (works if the texture is imported anywhere in the project)
            var allTex = Resources.FindObjectsOfTypeAll<Texture2D>();
            foreach (var t in allTex)
            {
                if (t.name == "Wood_Crate_001_basecolor")
                    return t;
            }

#if UNITY_EDITOR
            // Editor-only: load directly from asset database
            var guids = UnityEditor.AssetDatabase.FindAssets("Wood_Crate_001_basecolor t:Texture2D");
            foreach (var guid in guids)
            {
                var path = UnityEditor.AssetDatabase.GUIDToAssetPath(guid);
                var loaded = UnityEditor.AssetDatabase.LoadAssetAtPath<Texture2D>(path);
                if (loaded != null) return loaded;
            }
#endif

            Debug.LogWarning("[DisplayXR] Crate texture not found. Using solid color fallback.");
            return null;
        }
    }
}
