// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0

using System.Collections;
using UnityEngine;
using UnityEngine.SceneManagement;

namespace DisplayXR
{
    /// <summary>
    /// DisplayXR-branded boot splash. Renders the logo + a "for Unity" subtitle
    /// on the display's zero-disparity plane (ZDP) — the physical display
    /// surface, where content shows identically to both eyes (no parallax) and
    /// is razor-sharp on the lenticular panel.
    ///
    /// Place this on a Camera that also has a <see cref="DisplayXRDisplay"/>
    /// rig. Because the rig transform IS the display surface, the artwork is
    /// childed to it at local z=0: exactly the ZDP. All meshes are given
    /// oversized bounds so Unity's CPU frustum culling (which uses the camera
    /// transform, sitting on this very plane, not the overridden eye
    /// projection) never drops them — the geometry stays flat at z=0 and the
    /// native Kooima eye projection renders it at true zero disparity.
    ///
    /// The splash camera renders ONLY the splash artwork (a dedicated layer) and
    /// clears the rest of the eye target to <see cref="background"/>; it never
    /// re-renders the app's scene geometry.
    ///
    /// Two modes:
    /// - <b>Scene mode</b> (<see cref="overlayMode"/> = false, the SplashScreen
    ///   sample): the splash is the only content; when done, loads
    ///   <see cref="nextScene"/>.
    /// - <b>Overlay mode</b> (<see cref="overlayMode"/> = true, the opt-in
    ///   auto-splash): spawned at boot over the app's first scene (which loads
    ///   underneath); the opaque clear occludes it while the logo plays, then
    ///   the splash destroys itself to reveal the app.
    /// </summary>
    [RequireComponent(typeof(Camera))]
    [RequireComponent(typeof(DisplayXRDisplay))]
    public class DisplayXRSplash : MonoBehaviour
    {
        [Header("Logo")]
        [Tooltip("Logo texture (white-on-transparent). If null, loaded from " +
                 "Resources/displayxr_white (shipped with the plugin).")]
        public Texture2D logo;

        [Tooltip("Logo height as a fraction of the display height. 0.4 = 40% " +
                 "of the screen.")]
        [Range(0.05f, 1f)]
        public float logoHeightFraction = 0.4f;

        [Tooltip("Subtitle image (white-on-transparent), scaled to the logo " +
                 "width and drawn under it. If null, loaded from Resources/for_unity.")]
        public Texture2D subtitle;

        [Header("Background")]
        public Color background = Color.black;

        [Header("Timing (seconds)")]
        public float fadeIn = 0.4f;
        public float hold = 1.2f;
        public float fadeOut = 0.4f;

        [Header("Mode")]
        [Tooltip("Overlay mode: occlude an already-loaded scene and destroy self " +
                 "when done (auto-splash). When false, load Next Scene at the end " +
                 "(SplashScreen sample).")]
        public bool overlayMode = false;

        [Tooltip("Scene mode only: scene to load when the splash finishes (must " +
                 "be in Build Settings). Empty = load the next build index.")]
        public string nextScene = "";

        // Dedicated layer the splash artwork lives on; the splash camera renders
        // only this layer so it never re-renders the app's scene geometry. Layer
        // 31 is the least-used user layer.
        const int k_SplashLayer = 31;

        static readonly Bounds k_HugeBounds = new Bounds(Vector3.zero, Vector3.one * 1e4f);

        private DisplayXRFeature m_Feature;
        private Material m_LogoMat;
        private Material m_SubMat;

        IEnumerator Start()
        {
            var cam = GetComponent<Camera>();
            // Render ONLY the splash artwork; clear everything else to the
            // background so the app scene behind is fully occluded.
            cam.clearFlags = CameraClearFlags.SolidColor;
            cam.backgroundColor = background;
            cam.cullingMask = 1 << k_SplashLayer;
            if (overlayMode)
                cam.depth = 100; // render after the app's scene cameras

            if (logo == null)
                logo = Resources.Load<Texture2D>("displayxr_white");
            if (subtitle == null)
                subtitle = Resources.Load<Texture2D>("for_unity");

            m_Feature = DisplayXRFeature.Instance;

            // Wait for the runtime to report display geometry so we can size the
            // logo to the physical display. Fall back after ~2 s.
            float t = 0f;
            while ((m_Feature == null || !m_Feature.DisplayInfo.isValid) && t < 2f)
            {
                m_Feature = m_Feature ?? DisplayXRFeature.Instance;
                t += Time.deltaTime;
                yield return null;
            }

            float displayH = (m_Feature != null && m_Feature.DisplayInfo.isValid)
                ? m_Feature.DisplayInfo.displayHeightMeters
                : 0.19f;

            BuildArtwork(displayH);

            // Fade the logo/subtitle in → hold → out.
            yield return Fade(0f, 1f, fadeIn);
            yield return new WaitForSeconds(hold);
            yield return Fade(1f, 0f, fadeOut);

            Finish();
        }

        void Finish()
        {
            if (overlayMode)
            {
                // Reveal the scene loaded underneath. Destroying our rig clears
                // DisplayXRRigManager.SplashActive, so the app's rig resumes.
                StartCoroutine(FlushAndDestroy());
                return;
            }
            if (!string.IsNullOrEmpty(nextScene))
            {
                SceneManager.LoadScene(nextScene);
                return;
            }
            int next = gameObject.scene.buildIndex + 1;
            if (next > 0 && next < SceneManager.sceneCountInBuildSettings)
                SceneManager.LoadScene(next);
        }

        // Overlay teardown: before destroying the splash rig, hold it a few extra
        // frames rendering ONLY a full-eye black clear (cullingMask=0, depth on
        // top). The boot splash is a transient 2nd projection rig; if it vanishes
        // in one frame it can orphan a stale atlas region (frozen content the main
        // rig never reclaims). Overwriting the full eye buffer to black across all
        // buffered swapchain images first means no foreign content is left frozen.
        IEnumerator FlushAndDestroy()
        {
            var cam = GetComponent<Camera>();
            if (cam != null)
            {
                cam.cullingMask = 0;                            // draw nothing but the clear
                cam.clearFlags = CameraClearFlags.SolidColor;
                cam.backgroundColor = Color.black;
                cam.depth = 100;                                // on top of the app cameras
            }
            // Hold longer than any plausible swapchain image count (2-3) so every
            // buffered eye image is overwritten before the rig goes away.
            for (int i = 0; i < 6; i++)
                yield return new WaitForEndOfFrame();
            Destroy(gameObject);
        }

        void BuildArtwork(float displayH)
        {
            // Combo parent on the ZDP; children laid out so the group is centered.
            var combo = new GameObject("Splash Combo").transform;
            combo.SetParent(transform, false);
            combo.localPosition = Vector3.zero;
            combo.localRotation = Quaternion.identity;
            combo.gameObject.layer = k_SplashLayer;

            float logoH = Mathf.Max(0.001f, logoHeightFraction * displayH);
            float logoW = logoH * Aspect(logo, 1f);

            // --- Logo quad ---
            m_LogoMat = MakeQuadGO("Logo", combo, logoW, logoH, logo, out Transform logoT);

            // --- Subtitle: same width as the logo, height from its aspect ---
            float subH = 0f;
            Transform subT = null;
            if (subtitle != null)
            {
                float subW = logoW;
                subH = subW / Aspect(subtitle, 1f);
                m_SubMat = MakeQuadGO("Subtitle", combo, subW, subH, subtitle, out subT);
            }

            // --- Center the combo vertically: logo on top, subtitle below ---
            float gap = subH > 0f ? 0.12f * logoH : 0f;
            float comboH = logoH + gap + subH;
            if (logoT != null)
                logoT.localPosition = new Vector3(0, comboH * 0.5f - logoH * 0.5f, 0);
            if (subT != null)
                subT.localPosition = new Vector3(0, -comboH * 0.5f + subH * 0.5f, 0);
        }

        static float Aspect(Texture2D t, float fallback)
            => (t != null && t.height > 0) ? (float)t.width / t.height : fallback;

        // Builds a flat textured quad on the ZDP (z=0), on the splash layer, with
        // oversized bounds so Unity's frustum culling (camera transform sits on
        // this plane) never drops it. Returns the material (for fading).
        Material MakeQuadGO(string name, Transform parent, float w, float h,
                            Texture2D tex, out Transform xf)
        {
            var go = new GameObject(name);
            go.layer = k_SplashLayer;
            go.transform.SetParent(parent, false);
            go.AddComponent<MeshFilter>().sharedMesh = MakeQuad(w, h);
            var mat = new Material(Shader.Find("Sprites/Default"));
            mat.mainTexture = tex;
            mat.color = new Color(1, 1, 1, 0);
            go.AddComponent<MeshRenderer>().sharedMaterial = mat;
            xf = go.transform;
            return mat;
        }

        static Mesh MakeQuad(float w, float h)
        {
            float hw = w * 0.5f, hh = h * 0.5f;
            var mesh = new Mesh { name = "DisplayXRSplashQuad" };
            mesh.vertices = new[]
            {
                new Vector3(-hw, -hh, 0f), new Vector3(hw, -hh, 0f),
                new Vector3(hw,  hh, 0f), new Vector3(-hw,  hh, 0f),
            };
            mesh.uv = new[]
            {
                new Vector2(0, 0), new Vector2(1, 0),
                new Vector2(1, 1), new Vector2(0, 1),
            };
            mesh.triangles = new[] { 0, 2, 1, 0, 3, 2 };
            mesh.bounds = k_HugeBounds;
            return mesh;
        }

        IEnumerator Fade(float from, float to, float dur)
        {
            if (dur <= 0f) { SetAlpha(to); yield break; }
            float t = 0f;
            while (t < dur)
            {
                t += Time.deltaTime;
                SetAlpha(Mathf.Lerp(from, to, t / dur));
                yield return null;
            }
            SetAlpha(to);
        }

        void SetAlpha(float a)
        {
            SetMatAlpha(m_LogoMat, a);
            SetMatAlpha(m_SubMat, a);
        }

        static void SetMatAlpha(Material m, float a)
        {
            if (m == null) return;
            var c = m.color; c.a = a; m.color = c;
        }
    }
}
