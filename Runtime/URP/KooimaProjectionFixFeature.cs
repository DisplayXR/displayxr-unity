// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
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
// SINGLE PASS INSTANCED: the same "re-push via command buffer" idea is the one
// mechanism NOT gated to MultiPass, so it also drives SPI. Under SPI a single pass
// carries BOTH eyes (indexed by unity_StereoEyeIndex), so instead of one
// SetViewProjectionMatrices we write both slots of the unity_StereoMatrix* arrays.
// SPI is a 2-views-in-an-array mode, so the array push is gated on xr.viewCount == 2
// (always true for Unity's built-app PRIMARY_STEREO). It additionally requires a
// DisplayXR runtime that samples per-view subImage.imageArrayIndex (the eyes live in
// swapchain array layers 0/1). See docs~/experiments/spi-single-pass.md.
// DisplayXRFeature only leaves SPI selected on URP + Windows + D3D12; elsewhere it
// forces MultiPass.
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

            // Provider-mode matrix read (#166): the provider publishes the per-eye
            // view+proj to the same native shared state the silhouette reads, so we can
            // fetch them here (DisplayXRFeature.GetStereoMatrices needs the inactive hook).
            static readonly float[] s_pLV = new float[16], s_pLP = new float[16],
                                    s_pRV = new float[16], s_pRP = new float[16];

            static bool ProviderStereoMatrices(out Matrix4x4 lv, out Matrix4x4 lp,
                                               out Matrix4x4 rv, out Matrix4x4 rp)
            {
                lv = lp = rv = rp = Matrix4x4.identity;
                DisplayXRNative.displayxr_get_stereo_matrices(s_pLV, s_pLP, s_pRV, s_pRP, out int valid);
                if (valid == 0) return false;
                lv = ToMat(s_pLV); lp = ToMat(s_pLP); rv = ToMat(s_pRV); rp = ToMat(s_pRP);
                return true;
            }

            static Matrix4x4 ToMat(float[] m)
            {
                var r = new Matrix4x4();
                r.m00 = m[0];  r.m10 = m[1];  r.m20 = m[2];  r.m30 = m[3];
                r.m01 = m[4];  r.m11 = m[5];  r.m21 = m[6];  r.m31 = m[7];
                r.m02 = m[8];  r.m12 = m[9];  r.m22 = m[10]; r.m32 = m[11];
                r.m03 = m[12]; r.m13 = m[13]; r.m23 = m[14]; r.m33 = m[15];
                return r;
            }

            public override void RecordRenderGraph(RenderGraph renderGraph, ContextContainer frameData)
            {
                var cameraData = frameData.Get<UniversalCameraData>();
                if (cameraData == null || !cameraData.xr.enabled) return;

                Matrix4x4 lv, lp, rv, rp;
                if (DisplayXRProviderDriver.IsActive)
                {
                    // Provider mode (#166): DisplayXRFeature is inert, so this feature
                    // would no-op and URP would render with its own head-pose-compensated
                    // view (#115), diverging from the silhouette (which reads the
                    // provider's published eye_world view+proj) — the #127 parallax
                    // mismatch (tiger slides out from under its mask when popped out).
                    // Read the provider's published matrices (the SAME ones the silhouette
                    // uses) so URP renders identical to the silhouette.
                    if (!ProviderStereoMatrices(out lv, out lp, out rv, out rp)) return;
                }
                else
                {
                    if (s_feature == null) s_feature = DisplayXRFeature.Instance;
                    if (s_feature == null) return;
                    if (!s_feature.GetStereoMatrices(out lv, out lp, out rv, out rp)) return;
                }

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

                // ---- Single Pass Instanced ----------------------------------------
                // SPI delivers both eyes in ONE pass via a 2-layer texture-array
                // swapchain (eyes in array layers 0/1, indexed by unity_StereoEyeIndex).
                // Re-push the runtime per-eye projection into BOTH stereo array slots —
                // the off-axis (#127) correction for the SPI path.
                //
                // GATE: exactly 2 views in this single pass. The Unity built-app path is
                // always PRIMARY_STEREO, so this holds; a built Unity app renders only
                // 2 views and there is no view synthesis downstream, so it is limited to
                // <=2-view display modes (see hook-chain.md). Requires a DisplayXR
                // runtime that samples per-view subImage.imageArrayIndex — otherwise the
                // compositor reads array layer 0 for both eyes and the image is flat.
                // Verified on Windows/D3D12 (see docs~/experiments/spi-single-pass.md).
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
