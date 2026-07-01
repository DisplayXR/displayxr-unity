// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

using UnityEngine;

namespace DisplayXR
{
    /// <summary>
    /// Post-process FXAA pass for a DisplayXR rig camera. Managed by the rig
    /// components (<see cref="DisplayXRCamera"/> / <see cref="DisplayXRDisplay"/>)
    /// via their <c>postProcessAntiAliasing</c> toggle — not added directly by
    /// users, so it carries no AddComponentMenu and hides itself from the
    /// inspector.
    ///
    /// Why this exists: Unity 6 + Built-in RP + MultiPass + OpenXR submits the
    /// XR eye swapchain at sampleCount=1 on both D3D12 and Vulkan, and its eye-RT
    /// MSAA resolve does not preserve fractional alpha. The result is aliased
    /// silhouettes (glaring in alpha-native transparent overlays, where the
    /// aliasing is in the alpha channel). The FXAA shader lerps full RGBA at
    /// detected edges, restoring soft edges post-resolve where MSAA/supersampling
    /// can't. Graphics-API agnostic (pure OnRenderImage), works on D3D12 and
    /// Vulkan alike. Requires MultiPass (single-pass-instanced breaks Blit on the
    /// eye texture array).
    /// </summary>
    [RequireComponent(typeof(Camera))]
    [DisallowMultipleComponent]
    public class DisplayXRPostAA : MonoBehaviour
    {
        private Material m_Material;
        private bool m_Warned;

        void OnEnable() => EnsureMaterial();

        void EnsureMaterial()
        {
            if (m_Material != null) return;
            // Resources.Load first (reliable in player builds — Resources content
            // is always included); Shader.Find as an editor/safety fallback.
            Shader sh = Resources.Load<Shader>("DisplayXRPostAA");
            if (sh == null) sh = Shader.Find("Hidden/DisplayXR/PostAA");
            if (sh != null && sh.isSupported)
            {
                m_Material = new Material(sh) { hideFlags = HideFlags.HideAndDontSave };
            }
            else if (!m_Warned)
            {
                m_Warned = true;
                Debug.LogWarning("[DisplayXR] Post-process AA shader 'Hidden/DisplayXR/PostAA' " +
                                 "missing or unsupported; the FXAA pass is a no-op.", this);
            }
        }

        void OnRenderImage(RenderTexture src, RenderTexture dst)
        {
            if (m_Material != null) Graphics.Blit(src, dst, m_Material);
            else Graphics.Blit(src, dst);
        }

        void OnDestroy()
        {
            if (m_Material != null)
            {
                if (Application.isPlaying) Destroy(m_Material);
                else DestroyImmediate(m_Material);
                m_Material = null;
            }
        }

        /// <summary>
        /// Ensure a (hidden) DisplayXRPostAA exists on <paramref name="go"/> and
        /// return it. Called by the rig components to mirror their
        /// <c>postProcessAntiAliasing</c> flag onto the effect's enabled state.
        /// </summary>
        internal static DisplayXRPostAA Ensure(GameObject go)
        {
            var c = go.GetComponent<DisplayXRPostAA>();
            if (c == null)
            {
                c = go.AddComponent<DisplayXRPostAA>();
                // Implementation detail of the rig — keep it out of the inspector
                // and out of the scene file; the rig re-creates it on enable.
                c.hideFlags = HideFlags.HideInInspector | HideFlags.DontSave;
            }
            return c;
        }
    }
}
