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
//
// SPI EXPERIMENT (experiment/spi-single-pass, DISPLAYXR_SPI_EXPERIMENTAL): the same
// "re-push via command buffer" idea is the one mechanism NOT gated to MultiPass, so
// it's the only credible route to Single Pass Instanced. Under SPI a single pass
// carries BOTH eyes (indexed by unity_StereoEyeIndex), so instead of one
// SetViewProjectionMatrices we write both slots of the unity_StereoMatrix* arrays.
// Whether the engine honours that override vs re-binding its own fov-built stereo
// cbuffer is unproven — this branch exists to A/B it on hardware. SPI is inherently
// a 2-views-in-an-array mode, so the array push is gated on xr.viewCount == 2.
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
            // MultiPass uses (view, proj); SPI uses the stereo* arrays. The two
            // render funcs read disjoint fields — the unused ones stay default.
            class PassData
            {
                public Matrix4x4 view;
                public Matrix4x4 proj;
                public Matrix4x4[] stereoV;
                public Matrix4x4[] stereoP;
                public Matrix4x4[] stereoVP;
                public Matrix4x4[] stereoInvVP;
            }

            // Built-in stereo shader constant arrays (UnityStereoGlobals cbuffer).
            // Shaders read unity_StereoMatrix*[unity_StereoEyeIndex] under instanced
            // stereo. Whether SetGlobalMatrixArray overrides these for subsequent
            // draws is exactly what the SPI A/B is meant to find out (see below).
            static readonly int s_StereoV     = Shader.PropertyToID("unity_StereoMatrixV");
            static readonly int s_StereoP     = Shader.PropertyToID("unity_StereoMatrixP");
            static readonly int s_StereoVP    = Shader.PropertyToID("unity_StereoMatrixVP");
            static readonly int s_StereoInvVP = Shader.PropertyToID("unity_StereoMatrixInvVP");

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

                var xr = cameraData.xr;

                // Runtime eye_world positions, used to match each XR view index to the
                // L or R eye. eye_world (not URP's head-pose-compensated view #115) —
                // see the long note on the view below.
                Vector3 eyeL = FlipZ(lv).inverse.GetColumn(3);
                Vector3 eyeR = FlipZ(rv).inverse.GetColumn(3);

                // Foreground clip: when the opt-in DisplayXR/ForegroundClipURP pass is
                // active, the runtime bakes the (small) per-eye foreground far into
                // leftProj/rightProj. Don't let that clip the RENDER — the scene must
                // render fully so the shader can do the per-eye cut, and the projection
                // far must agree with _ZBufferParams (camera near/far), which the clip
                // shader uses to reconstruct eyeZ. So rebuild only the depth terms to the
                // camera's far, keeping the off-axis shear (m00/m02) intact. Inert when
                // the clip is off (then the proj already carries the camera far).
                bool fgClip = Shader.GetGlobalVector(s_ForegroundFarId).z > 0.5f;
                float camFar = cameraData.camera.farClipPlane;
                Matrix4x4 projL = fgClip ? RebuildDepthToFar(lp, camFar) : lp;
                Matrix4x4 projR = fgClip ? RebuildDepthToFar(rp, camFar) : rp;

                // Use the runtime eye_world VIEW too — NOT URP's view. URP's view
                // comes from the head-pose-compensated pose (#115), which diverges
                // from the eye_world position the projection is built for as the
                // head moves off-centre. Pairing eye_world proj with URP's view made
                // the rendered parallax track the head FASTER than the click-through
                // silhouette (which uses eye_world view+proj, same as BiRP) — so the
                // tiger slid out from under its own silhouette/mask. Pairing eye_world
                // view AND proj makes URP render identical to BiRP and the silhouette.
                // FlipZ → Unity convention (the rig's FlipViewZ before SetStereoView).
                Matrix4x4 viewL = FlipZ(lv);
                Matrix4x4 viewR = FlipZ(rv);

#if DISPLAYXR_SPI_EXPERIMENTAL
                // ---- Single Pass Instanced (experiment/spi-single-pass) ----------
                // GATE: exactly 2 views in this single pass — the precise form of
                // "display max view count == 2" (Unity's render path is always
                // PRIMARY_STEREO; the runtime synthesises 4/8 views downstream).
                //
                // STATUS (hardware diag 2026-06-28, 2d-ui, RTX 3080 SR, D3D12): the
                // PLUGIN + UNITY side are CORRECT for SPI. The remaining blocker is in the
                // RUNTIME compositor and is fixable there (we own the runtime). Evidence:
                //   • With an eye-tracking lock (tracking=1) the whole chain is per-eye
                //     distinct: GetStereoMatrices projL!=projR, Unity's own
                //     xr.GetProjMatrix(0)!=(1), and our pushed sVP0!=sVP1 (periodic log).
                //     Earlier "all mono" captures were tracking=0 — the runtime's correct
                //     nominal-viewer fallback renders flat 2D when no face is locked.
                //   • xrCreateSwapchain shows arrays=2 (Unity made a 2-LAYER texture-array
                //     swapchain); xrEndFrame submits two projection views into ONE
                //     swapchain with subImage.imageArrayIndex 0 (left) and 1 (right),
                //     distinct poses. Textbook-correct SPI submission.
                //   • Yet the woven image is FLAT → the runtime compositor samples array
                //     LAYER 0 for BOTH views, ignoring subImage.imageArrayIndex. MultiPass
                //     works because each eye is its own swapchain (always layer 0); SPI
                //     puts the right eye in layer 1.
                // RUNTIME FIX: per projection view, sample swapchain array layer =
                //   view.subImage.imageArrayIndex (not hardcoded 0) in the Display
                //   Processor / weave. Then SPI renders 3D and this off-axis push (#127)
                //   makes it pixel-correct. See docs~/experiments/spi-single-pass.md.
                if (xr.singlePassEnabled && xr.viewCount == 2)
                {
                    var sV     = new Matrix4x4[2];
                    var sP     = new Matrix4x4[2];
                    var sVP    = new Matrix4x4[2];
                    var sInvVP = new Matrix4x4[2];
                    for (int i = 0; i < 2; i++)
                    {
                        // Unity view index 0 = left eye, 1 = right (runtime view order).
                        bool left = (i == 0);
                        Matrix4x4 view = left ? viewL : viewR;
                        Matrix4x4 gpuProj = GL.GetGPUProjectionMatrix(left ? projL : projR, true);
                        sV[i]     = view;
                        sP[i]     = gpuProj;
                        sVP[i]    = gpuProj * view;
                        sInvVP[i] = sVP[i].inverse;
                    }

                    // PERIODIC diag (every ~120 calls) — fires in steady state so it
                    // catches tracking=1 (the first-N-frames throttle only saw the
                    // no-track startup). Shows where per-eye separation is lost:
                    //   getStereo projL!=projR  → source has separation
                    //   unityProj(0)!=unityProj(1) → Unity surfaces per-eye at record
                    //   sVP0!=sVP1 → our push is distinct
                    if ((m_LogCount++ % 120) == 0)
                    {
                        Vector3 uv0 = xr.GetViewMatrix(0).inverse.GetColumn(3);
                        Vector3 uv1 = xr.GetViewMatrix(1).inverse.GetColumn(3);
                        Debug.Log($"[KooimaProjFix][SPI] getStereo projL.m02={projL.m02:F4} projR.m02={projR.m02:F4} | " +
                                  $"unityProj.m02 0={xr.GetProjMatrix(0).m02:F4} 1={xr.GetProjMatrix(1).m02:F4} | " +
                                  $"unityViewX 0={uv0.x:F4} 1={uv1.x:F4} | " +
                                  $"pushed sVP0.m02={sVP[0].m02:F4} sVP1.m02={sVP[1].m02:F4} d={(sVP[0].m02 - sVP[1].m02):F4}");
                    }

                    // Off-axis correction (#127) for SPI: re-push the runtime per-eye
                    // projection into both stereo array slots. PROVEN distinct + reaching
                    // the instanced draws (the periodic log above shows sVP0 != sVP1 when
                    // tracked). This is NOT what blocks SPI disparity — see the header:
                    // the runtime compositor must honor subImage.imageArrayIndex first.
                    using (var builder = renderGraph.AddUnsafePass<PassData>("KooimaProjectionFixSPI", out var passData))
                    {
                        passData.stereoV = sV;
                        passData.stereoP = sP;
                        passData.stereoVP = sVP;
                        passData.stereoInvVP = sInvVP;
                        builder.AllowPassCulling(false);
                        builder.SetRenderFunc((PassData d, UnsafeGraphContext ctx) =>
                        {
                            var cmd = CommandBufferHelpers.GetNativeCommandBuffer(ctx.cmd);
                            cmd.SetGlobalMatrixArray(s_StereoV,     d.stereoV);
                            cmd.SetGlobalMatrixArray(s_StereoP,     d.stereoP);
                            cmd.SetGlobalMatrixArray(s_StereoVP,    d.stereoVP);
                            cmd.SetGlobalMatrixArray(s_StereoInvVP, d.stereoInvVP);
                        });
                    }
                    return;
                }
#endif
                // ---- MultiPass (default) -----------------------------------------
                // One view per pass: identify this eye and push a single (view, proj).
                Vector3 curEye = xr.GetViewMatrix(0).inverse.GetColumn(3);
                bool isLeft = (curEye - eyeL).sqrMagnitude <= (curEye - eyeR).sqrMagnitude;
                Matrix4x4 correctProj = isLeft ? projL : projR;
                // SetViewProjectionMatrices expects the non-GPU projection.
                Matrix4x4 mpView = isLeft ? viewL : viewR;

                if (m_LogCount < 4)
                {
                    m_LogCount++;
                    Debug.Log($"[KooimaProjFix] eye={(isLeft ? "L" : "R")} " +
                              $"urpProj.m02={xr.GetProjMatrix(0).m02:F4} -> correct.m02={correctProj.m02:F4}");
                }

                using (var builder = renderGraph.AddUnsafePass<PassData>("KooimaProjectionFix", out var passData))
                {
                    passData.view = mpView;
                    passData.proj = correctProj;
                    builder.AllowPassCulling(false);
                    builder.SetRenderFunc((PassData d, UnsafeGraphContext ctx) =>
                    {
                        var cmd = CommandBufferHelpers.GetNativeCommandBuffer(ctx.cmd);
                        cmd.SetViewProjectionMatrices(d.view, d.proj);
                    });
                }
            }

            // Rebuild only the depth (m22/m23) terms of an OpenGL-convention
            // projection (clip z in [-1,1]) so its far plane = far, preserving the
            // off-axis shear (m00/m02). Used when the foreground-clip pass baked a
            // short per-eye far into the projection (see caller).
            static Matrix4x4 RebuildDepthToFar(Matrix4x4 proj, float far)
            {
                float near = proj.m23 / (proj.m22 - 1f);  // recover near
                if (far > near && near > 0f)
                {
                    proj.m22 = -(far + near) / (far - near);
                    proj.m23 = -2.0f * far * near / (far - near);
                }
                return proj;
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
