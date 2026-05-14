// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0

using UnityEngine;

namespace DisplayXR.Samples
{
    /// <summary>
    /// (issue #57) Demo scene for alpha-native transparent overlay mode.
    /// Spawns a single capsule "avatar" with a subtle breathing animation
    /// over a fully transparent background. On a Windows or macOS standalone
    /// build, the DisplayXRTransparentOverlay component on the camera lets
    /// the desktop show through every alpha=0 region.
    ///
    /// Attach to any GameObject in an empty scene and press Play.
    /// </summary>
    public class TransparentAvatarSetup : MonoBehaviour
    {
        [Tooltip("Display-centric virtual display height in meters. Smaller = more zoom.")]
        public float virtualDisplayHeight = 0.25f;

        [Tooltip("Breathing animation amplitude (fractional scale).")]
        public float breathAmplitude = 0.04f;

        [Tooltip("Breathing animation frequency in Hz.")]
        public float breathRate = 0.4f;

        // Must fire BEFORE xrCreateSession so the runtime sees the
        // transparent-background flag when the win32 (or cocoa) window-
        // binding extension is wired up; RequestTransparentSession writes
        // to plugin shared state that's read in xrCreateSession.
        [RuntimeInitializeOnLoadMethod(RuntimeInitializeLoadType.SubsystemRegistration)]
        static void EnableTransparentSession()
        {
            DisplayXR.DisplayXRTransparentOverlay.RequestTransparentSession();
        }

        private GameObject m_Capsule;
        private Vector3 m_BaseScale;

        void Start()
        {
            // Skip if scene already has objects (e.g. user pre-built it).
            if (FindAnyObjectByType<MeshRenderer>() != null)
                return;

            // Reset Main Camera + add the transparent overlay component.
            var cam = Camera.main;
            if (cam == null)
            {
                Debug.LogError("[TransparentAvatarSetup] No Main Camera found");
                return;
            }
            cam.transform.position = Vector3.zero;
            cam.transform.rotation = Quaternion.identity;

            // Display-centric stereo rig (issue #57 spec — virtual display ~25 cm).
            // Add to an empty GameObject so the rig and camera stay separate.
            var rigGo = new GameObject("DisplayXR Rig");
            var rig   = rigGo.AddComponent<DisplayXR.DisplayXRDisplay>();
            rig.virtualDisplayHeight = virtualDisplayHeight;

            // Capsule "avatar" — slightly forward so it pops out of screen.
            m_Capsule = GameObject.CreatePrimitive(PrimitiveType.Capsule);
            m_Capsule.name = "Avatar";
            m_Capsule.transform.position = new Vector3(0f, 0f, 0.4f);
            m_Capsule.transform.localScale = new Vector3(0.12f, 0.16f, 0.12f);
            m_BaseScale = m_Capsule.transform.localScale;
            m_Capsule.GetComponent<Renderer>().material = CreateMaterial(new Color(0.95f, 0.75f, 0.6f));

            // Directional light.
            var lightGo = new GameObject("DirectionalLight");
            var light = lightGo.AddComponent<Light>();
            light.type = LightType.Directional;
            lightGo.transform.rotation = Quaternion.Euler(50f, -30f, 0f);

            // Transparent overlay (alpha-native). Camera clear flips to
            // SolidColor (0,0,0,0); the runtime composes the desktop under
            // each tile pre-weave so transparent regions show through.
            var overlay = cam.gameObject.GetComponent<DisplayXR.DisplayXRTransparentOverlay>();
            if (overlay == null)
                overlay = cam.gameObject.AddComponent<DisplayXR.DisplayXRTransparentOverlay>();
            overlay.clickableRenderers = new Renderer[] { m_Capsule.GetComponent<Renderer>() };
        }

        void Update()
        {
            if (m_Capsule == null)
                return;
            float k = 1f + Mathf.Sin(Time.time * 2f * Mathf.PI * breathRate) * breathAmplitude;
            m_Capsule.transform.localScale = m_BaseScale * k;
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
    }
}
