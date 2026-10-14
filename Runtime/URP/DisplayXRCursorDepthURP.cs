// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

using System.Collections.Generic;
using UnityEngine;
using UnityEngine.Rendering;
using UnityEngine.Rendering.RenderGraphModule;
using UnityEngine.Rendering.Universal;

namespace DisplayXR.URP
{
    /// <summary>
    /// URP hook for the depth-aware cursor's Depth source: while the cursor uses depth, turn
    /// on the camera depth texture for the active camera (restored afterwards) and enqueue a
    /// render-graph pass that copies the 32x32 patch under the cursor out of
    /// <c>cameraDepthTexture</c>. No renderer-feature setup needed in the project.
    /// </summary>
    static class DisplayXRCursorDepthURP
    {
        static CursorDepthPatchPass s_Pass;
        static readonly Dictionary<Camera, CameraOverrideOption> s_Restore = new Dictionary<Camera, CameraOverrideOption>();
        static bool s_LoggedEnable;

        [RuntimeInitializeOnLoadMethod(RuntimeInitializeLoadType.AfterAssembliesLoaded)]
        static void Init()
        {
            RenderPipelineManager.beginCameraRendering -= OnBeginCamera;
            RenderPipelineManager.beginCameraRendering += OnBeginCamera;
        }

        static void OnBeginCamera(ScriptableRenderContext ctx, Camera cam)
        {
            if (!DisplayXRCursorDepthPatch.Active || cam != DisplayXRCursorDepthPatch.TargetCamera)
            {
                RestoreAll(DisplayXRCursorDepthPatch.Active ? DisplayXRCursorDepthPatch.TargetCamera : null);
                return;
            }
            var data = cam.GetUniversalAdditionalCameraData();
            if (data == null || data.scriptableRenderer == null) return;

            // The camera depth texture is what the pass reads. Keep it on for as long as the
            // cursor uses depth (not just while the pointer is over the canvas), so URP never
            // reallocates it as the pointer comes and goes.
            if (data.requiresDepthOption != CameraOverrideOption.On && !s_Restore.ContainsKey(cam))
            {
                s_Restore[cam] = data.requiresDepthOption;
                data.requiresDepthOption = CameraOverrideOption.On;
                if (!s_LoggedEnable)
                {
                    s_LoggedEnable = true;
                    Debug.Log("[DisplayXR] Depth-aware cursor: enabled URP's Depth Texture on camera '" + cam.name +
                              "' (restored when the cursor is disabled).");
                }
            }
            if (!DisplayXRCursorDepthPatch.DrawThisFrame) return;
            s_Pass ??= new CursorDepthPatchPass();
            data.scriptableRenderer.EnqueuePass(s_Pass);
        }

        // Put back every camera's depth option except the one still in use.
        static void RestoreAll(Camera keep)
        {
            if (s_Restore.Count == 0) return;
            List<Camera> done = null;
            foreach (var kv in s_Restore)
            {
                if (kv.Key == keep) continue;
                if (kv.Key != null)
                {
                    var d = kv.Key.GetUniversalAdditionalCameraData();
                    if (d != null) d.requiresDepthOption = kv.Value;
                }
                (done ??= new List<Camera>()).Add(kv.Key);
            }
            if (done != null) foreach (var c in done) s_Restore.Remove(c);
        }

        sealed class CursorDepthPatchPass : ScriptableRenderPass
        {
            RTHandle m_Target;
            readonly MaterialPropertyBlock m_Props = new MaterialPropertyBlock();

            class PassData
            {
                public TextureHandle depth;
                public Material material;
                public int pass;
                public MaterialPropertyBlock props;
                public bool array;
            }

            public CursorDepthPatchPass()
            {
                // After every point URP may copy the depth (after opaques or after
                // transparents), before post-processing.
                renderPassEvent = RenderPassEvent.BeforeRenderingPostProcessing;
            }

            public override void RecordRenderGraph(RenderGraph renderGraph, ContextContainer frameData)
            {
                var target = DisplayXRCursorDepthPatch.Target;
                var material = DisplayXRCursorDepthPatch.Material;
                if (target == null || material == null) return;
                var resources = frameData.Get<UniversalResourceData>();
                TextureHandle depth = resources.cameraDepthTexture;
                if (!depth.IsValid())
                {
                    DisplayXRCursorDepthPatch.SetAvailability(DisplayXRCursorDepthPatch.Pipeline.URP,
                                                              "no camera depth texture this frame");
                    return;
                }
                if (m_Target == null || m_Target.rt != target)
                {
                    m_Target?.Release();
                    m_Target = RTHandles.Alloc(target);
                }
                TextureDesc desc = renderGraph.GetTextureDesc(depth);
                bool array = desc.dimension == TextureDimension.Tex2DArray;
                DisplayXRCursorDepthPatch.FillProperties(m_Props, DisplayXRCursorDepthPatch.ViewportScale);
                TextureHandle dst = renderGraph.ImportTexture(m_Target);

                using (var builder = renderGraph.AddRasterRenderPass<PassData>("DisplayXR Cursor Depth Patch", out var pd))
                {
                    pd.depth = depth;
                    pd.material = material;
                    pd.pass = array ? 1 : 0;
                    pd.props = m_Props;
                    pd.array = array;
                    builder.UseTexture(depth, AccessFlags.Read);
                    builder.SetRenderAttachment(dst, 0, AccessFlags.Write);
                    builder.AllowPassCulling(false);
                    builder.SetRenderFunc((PassData d, RasterGraphContext c) =>
                    {
                        RTHandle src = d.depth;
                        d.props.SetTexture(d.array ? DisplayXRCursorDepthPatch.IdSrcArray : DisplayXRCursorDepthPatch.IdSrc, src);
                        c.cmd.DrawProcedural(Matrix4x4.identity, d.material, d.pass, MeshTopology.Triangles, 3, 1, d.props);
                    });
                }
                DisplayXRCursorDepthPatch.NoteRecorded(DisplayXRCursorDepthPatch.Pipeline.URP);
            }
        }
    }
}
