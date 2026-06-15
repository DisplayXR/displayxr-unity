// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
//
// Inject the runtime's correct per-eye projection into URP (the URP adapter of the
// Kooima projection pipeline; #127/#396).
//
// Unity's URP builds each eye's projection from XrView.fov and gets it WRONG for
// strongly off-center frustums (head x<0) — the eyes over-separate and the image
// shifts/deforms. URP also ignores Camera.SetStereoProjectionMatrix (Unity
// #1328435), so the BiRP adapter's override (DisplayXRDisplay/DisplayXRCamera) can't
// reach URP. But in MULTIPASS, URP feeds the projection to shaders via
// cmd.SetViewProjectionMatrices in ScriptableRenderer.SetCameraMatrices, pushed ONCE
// per eye-pass during camera setup (not per draw). So a RendererFeature pass injected
// just before opaque geometry can RE-PUSH the correct projection and it sticks for the
// draws.
//
// The correct per-eye projection is the SAME source of truth both pipelines use:
// DisplayXRFeature.GetStereoMatrices (leftProj/rightProj) — the exact matrices BiRP
// renders from (and that render correctly both sides). We keep URP's own (correct)
// view matrix and replace only the projection. The current eye is identified by
// matching the XRPass view position to the nearer of the two runtime eye positions
// (multipass => one view per pass).
//
// REQUIRES URP 17 / Unity 6 (RenderGraph API). The whole assembly is gated by
// DISPLAYXR_URP (versionDefines, URP >= 17.0.0); BiRP-only and older-URP projects
// never compile it. Wire via DisplayXR > Setup URP Projection Fix (auto-wired by
// DisplayXRUrpAutoWire when a URP DisplayXR rig is present), or add manually.
using UnityEngine;
using UnityEngine.Rendering;
using UnityEngine.Rendering.Universal;
using UnityEngine.Rendering.RenderGraphModule;
using DisplayXR;

namespace DisplayXR.URP
{
    public class KooimaProjectionFixFeature : ScriptableRendererFeature
    {
        class KooimaProjPass : ScriptableRenderPass
        {
            class PassData { public Matrix4x4 view; public Matrix4x4 proj; }

            static DisplayXRFeature s_feature;
            static readonly int s_ForegroundFarId = Shader.PropertyToID("_DXRForegroundFar");
            int m_LogCount;

            public override void RecordRenderGraph(RenderGraph renderGraph, ContextContainer frameData)
            {
                var cameraData = frameData.Get<UniversalCameraData>();
                if (cameraData == null || !cameraData.xr.enabled) return;

                if (s_feature == null) s_feature = DisplayXRFeature.Instance;
                if (s_feature == null) return;
                if (!s_feature.GetStereoMatrices(out Matrix4x4 lv, out Matrix4x4 lp,
                                                 out Matrix4x4 rv, out Matrix4x4 rp))
                    return;

                // Startup guard: the first few frames GetStereoMatrices can return
                // not-yet-ready (identity / NaN) matrices. Applying a NaN projection
                // would flash/break those frames — skip until the matrices are real.
                if (!IsFinite(lp) || !IsFinite(rp)) return;

                // Identify the current eye (multipass: xr has one view this pass) by
                // matching its world position to the nearer runtime eye.
                var xr = cameraData.xr;
                Vector3 curEye = xr.GetViewMatrix(0).inverse.GetColumn(3);
                Vector3 eyeL = FlipZ(lv).inverse.GetColumn(3);
                Vector3 eyeR = FlipZ(rv).inverse.GetColumn(3);
                bool isLeft = (curEye - eyeL).sqrMagnitude <= (curEye - eyeR).sqrMagnitude;
                Matrix4x4 correctProj = isLeft ? lp : rp;

                // Foreground clip: when the opt-in DisplayXR/ForegroundClipURP pass is
                // active, the runtime bakes the (small) per-eye foreground far into
                // leftProj/rightProj. Don't let that clip the RENDER — the scene must
                // render fully so the shader can do the per-eye cut, and the projection
                // far must agree with _ZBufferParams (camera near/far), which the clip
                // shader uses to reconstruct eyeZ. So rebuild only the depth terms to the
                // camera's far, keeping the off-axis shear (m00/m02) intact. Inert when
                // the clip is off (then the proj already carries the camera far).
                if (Shader.GetGlobalVector(s_ForegroundFarId).z > 0.5f)
                {
                    float near = correctProj.m23 / (correctProj.m22 - 1f);  // recover near
                    float far = cameraData.camera.farClipPlane;             // full render far
                    if (far > near && near > 0f)
                    {
                        // GL projection depth terms (clip z in [-1, 1]).
                        correctProj.m22 = -(far + near) / (far - near);
                        correctProj.m23 = -2.0f * far * near / (far - near);
                    }
                }

                // Keep URP's view (it's correct, from the provider); replace only the
                // projection. SetViewProjectionMatrices expects the non-GPU projection.
                Matrix4x4 view = xr.GetViewMatrix(0);

                if (m_LogCount < 4)
                {
                    m_LogCount++;
                    Debug.Log($"[KooimaProjFix] eye={(isLeft ? "L" : "R")} " +
                              $"urpProj.m02={xr.GetProjMatrix(0).m02:F4} -> correct.m02={correctProj.m02:F4}");
                }

                using (var builder = renderGraph.AddUnsafePass<PassData>("KooimaProjectionFix", out var passData))
                {
                    passData.view = view;
                    passData.proj = correctProj;
                    builder.AllowPassCulling(false);
                    builder.SetRenderFunc((PassData d, UnsafeGraphContext ctx) =>
                    {
                        var cmd = CommandBufferHelpers.GetNativeCommandBuffer(ctx.cmd);
                        cmd.SetViewProjectionMatrices(d.view, d.proj);
                    });
                }
            }

            // Negate column 2 (Z) of a view matrix — the OpenXR->Unity handedness flip
            // the rig (DisplayXRDisplay/DisplayXRCamera.FlipViewZ) applies before
            // SetStereoViewMatrix, so the eye world positions match the rendered ones.
            static Matrix4x4 FlipZ(Matrix4x4 m)
            {
                m.m02 = -m.m02; m.m12 = -m.m12; m.m22 = -m.m22; m.m32 = -m.m32;
                return m;
            }

            // True only if the projection is real and non-degenerate (m00/m11 != 0,
            // no NaN/Inf). Filters the not-yet-ready startup matrices.
            static bool IsFinite(Matrix4x4 m)
            {
                float s = m.m00 + m.m11 + m.m22 + m.m23 + m.m02 + m.m12;
                return !float.IsNaN(s) && !float.IsInfinity(s)
                       && Mathf.Abs(m.m00) > 1e-6f && Mathf.Abs(m.m11) > 1e-6f;
            }
        }

        KooimaProjPass m_Pass;

        public override void Create()
        {
            m_Pass = new KooimaProjPass
            {
                // After URP's camera setup (which pushes the wrong XR projection) and
                // right before opaque geometry, so our matrices are live for the draws.
                renderPassEvent = RenderPassEvent.BeforeRenderingOpaques
            };
        }

        public override void AddRenderPasses(ScriptableRenderer renderer, ref RenderingData renderingData)
        {
            renderer.EnqueuePass(m_Pass);
        }
    }
}
