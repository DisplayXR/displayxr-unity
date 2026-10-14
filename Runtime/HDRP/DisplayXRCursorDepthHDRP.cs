// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

using UnityEngine;
using UnityEngine.Rendering;
using UnityEngine.Rendering.HighDefinition;

namespace DisplayXR.HDRP
{
    /// <summary>
    /// HDRP hook for the depth-aware cursor's Depth source: a global custom pass (registered
    /// in code; no volume in the scene) that copies the 32x32 patch under the cursor out of
    /// HDRP's camera depth buffer before post-processing. HDRP always has a depth buffer, so
    /// nothing has to be switched on.
    /// </summary>
    static class DisplayXRCursorDepthHDRP
    {
        static CursorDepthCustomPass s_Pass;

        [RuntimeInitializeOnLoadMethod(RuntimeInitializeLoadType.AfterAssembliesLoaded)]
        static void Init()
        {
            s_Pass ??= new CursorDepthCustomPass { name = "DisplayXR Cursor Depth Patch" };
            CustomPassVolume.RegisterUniqueGlobalCustomPass(CustomPassInjectionPoint.BeforePostProcess, s_Pass);
        }

        sealed class CursorDepthCustomPass : CustomPass
        {
            RTHandle m_Target;
            readonly MaterialPropertyBlock m_Props = new MaterialPropertyBlock();

            public CursorDepthCustomPass()
            {
                // We bind our own target; HDRP must not bind the camera buffers for us (the
                // depth buffer is what we sample).
                targetColorBuffer = TargetBuffer.None;
                targetDepthBuffer = TargetBuffer.None;
                clearFlags = ClearFlag.None;
            }

            protected override void Execute(CustomPassContext ctx)
            {
                if (!DisplayXRCursorDepthPatch.DrawThisFrame || ctx.hdCamera == null ||
                    ctx.hdCamera.camera != DisplayXRCursorDepthPatch.TargetCamera)
                    return;
                var target = DisplayXRCursorDepthPatch.Target;
                var material = DisplayXRCursorDepthPatch.Material;
                RTHandle depth = ctx.cameraDepthBuffer;
                if (target == null || material == null) return;
                if (depth == null || depth.rt == null)
                {
                    DisplayXRCursorDepthPatch.SetAvailability(DisplayXRCursorDepthPatch.Pipeline.HDRP, "no camera depth buffer");
                    return;
                }
                if (depth.rt.antiAliasing > 1 || depth.rt.bindTextureMS)
                {
                    DisplayXRCursorDepthPatch.SetAvailability(DisplayXRCursorDepthPatch.Pipeline.HDRP,
                                                              "MSAA depth buffer (not sampled)");
                    return;
                }
                if (m_Target == null || m_Target.rt != target)
                {
                    m_Target?.Release();
                    m_Target = RTHandles.Alloc(target);
                }
                bool array = depth.rt.dimension == TextureDimension.Tex2DArray;
                // HDRP's buffers are RTHandles larger than the rendered area; the handle scale
                // maps viewport UV onto the texture (the eye viewport is HDRP's actual size).
                Vector4 s = depth.rtHandleProperties.rtHandleScale;
                DisplayXRCursorDepthPatch.FillProperties(m_Props, new Vector2(s.x, s.y));
                m_Props.SetTexture(array ? DisplayXRCursorDepthPatch.IdSrcArray : DisplayXRCursorDepthPatch.IdSrc, depth);
                CoreUtils.SetRenderTarget(ctx.cmd, m_Target, ClearFlag.None);
                ctx.cmd.DrawProcedural(Matrix4x4.identity, material, array ? 1 : 0, MeshTopology.Triangles, 3, 1, m_Props);
                DisplayXRCursorDepthPatch.NoteRecorded(DisplayXRCursorDepthPatch.Pipeline.HDRP);
            }

            protected override void Cleanup()
            {
                m_Target?.Release();
                m_Target = null;
            }
        }
    }
}
