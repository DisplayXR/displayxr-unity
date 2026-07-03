// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

using UnityEngine;

namespace DisplayXR
{
    /// <summary>
    /// Per-eye foreground clip for the Built-in RP under the custom display provider
    /// (epic #166). The BiRP analog of the URP <c>DisplayXR/ForegroundClipURP</c>
    /// FullScreenPass. Managed by the rig components
    /// (<see cref="DisplayXRDisplay"/> / <see cref="DisplayXRCamera"/>) — not added by
    /// users, so it carries no AddComponentMenu and hides itself.
    ///
    /// Why this exists: under the provider Unity fills each eye's projection depth from
    /// a single <c>Camera.farClipPlane</c> (shared by both eyes), so a projection-level
    /// far can't sit on the display plane for both eyes at once — off-axis the two eyes'
    /// clip edges diverge. <see cref="OnRenderImage"/> fires per eye in BiRP MultiPass,
    /// so each call pushes THIS eye's foreground far (the native per-eye display-plane
    /// distance, <c>dxr_prov_get_eye_clip</c>) into the shader, which discards fragments
    /// beyond it — both eyes clip exactly on the plane. The eye is selected via
    /// <c>Camera.stereoActiveEye</c> (an OnRenderImage blit clobbers the per-eye
    /// matrices, so we cannot reconstruct the eye in the shader as URP does).
    ///
    /// Provider + BiRP + MultiPass only. Under URP the ForegroundClipURP shader owns
    /// this; on the OpenXR hook path the per-eye far rides the projection matrix
    /// (SetStereoProjectionMatrix), which BiRP honors there.
    /// </summary>
    [RequireComponent(typeof(Camera))]
    [DisallowMultipleComponent]
    public class DisplayXRForegroundClipBiRP : MonoBehaviour
    {
        private Material m_Material;
        private Camera m_Camera;
        private bool m_Warned;
        private static readonly int s_ClipEnableId = Shader.PropertyToID("_DXRClipEnable");
        private static readonly int s_ClipFarId = Shader.PropertyToID("_DXRClipFar");

        /// <summary>Set by the rig each frame: whether the clip should cut this frame
        /// (mirrors <c>foregroundOnlyClip</c>). When false the pass is pure passthrough.</summary>
        internal bool clipEnabled;

        void OnEnable()
        {
            m_Camera = GetComponent<Camera>();
            // BiRP needs the depth texture for the view-space depth reconstruction.
            m_Camera.depthTextureMode |= DepthTextureMode.Depth;
            EnsureMaterial();
        }

        void EnsureMaterial()
        {
            if (m_Material != null) return;
            // Resources.Load first (reliable in player builds); Shader.Find fallback.
            Shader sh = Resources.Load<Shader>("DisplayXRForegroundClipBiRP");
            if (sh == null) sh = Shader.Find("Hidden/DisplayXR/ForegroundClipBiRP");
            if (sh != null && sh.isSupported)
            {
                m_Material = new Material(sh) { hideFlags = HideFlags.HideAndDontSave };
            }
            else if (!m_Warned)
            {
                m_Warned = true;
                Debug.LogWarning("[DisplayXR] Foreground-clip shader 'Hidden/DisplayXR/ForegroundClipBiRP' " +
                                 "missing or unsupported; the BiRP foreground clip is a no-op.", this);
            }
        }

        void OnRenderImage(RenderTexture src, RenderTexture dst)
        {
            if (m_Material == null || !clipEnabled)
            {
                Graphics.Blit(src, dst);
                return;
            }

            // MultiPass fires OnRenderImage once per eye; use THIS eye's far so the clip
            // lands on the display plane for both eyes (a single cam.farClipPlane can't).
            var eye = m_Camera != null ? m_Camera.stereoActiveEye : Camera.MonoOrStereoscopicEye.Mono;
            uint idx = (eye == Camera.MonoOrStereoscopicEye.Right) ? 1u : 0u;
            DisplayXRProviderNative.dxr_prov_get_eye_clip(idx, out float far, out _, out _, out _);
            float nz = m_Camera != null ? m_Camera.nearClipPlane : 0.3f;
            bool valid = far > nz + 0.01f;
            m_Material.SetFloat(s_ClipEnableId, valid ? 1f : 0f);
            m_Material.SetFloat(s_ClipFarId, far);
            Graphics.Blit(src, dst, m_Material);
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
        /// Ensure a (hidden) DisplayXRForegroundClipBiRP exists on <paramref name="go"/>
        /// and return it. Called by the rig components; kept out of the inspector and
        /// scene file (the rig re-creates it on enable, like DisplayXRPostAA).
        /// </summary>
        internal static DisplayXRForegroundClipBiRP Ensure(GameObject go)
        {
            var c = go.GetComponent<DisplayXRForegroundClipBiRP>();
            if (c == null)
            {
                c = go.AddComponent<DisplayXRForegroundClipBiRP>();
                c.hideFlags = HideFlags.HideInInspector | HideFlags.DontSave;
            }
            return c;
        }
    }
}
