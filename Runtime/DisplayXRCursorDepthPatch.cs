// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
using Unity.Collections;
using UnityEngine;
using UnityEngine.Rendering;
using UnityEngine.XR;

namespace DisplayXR
{
    /// <summary>
    /// The depth-aware cursor's <b>Depth</b> source: read Unity's own scene depth under the
    /// cursor. Internal plumbing for <see cref="DisplayXRCursorDepth"/>.
    ///
    /// <para>Per frame (main thread, from <c>DisplayXRCursorDepth.Tick</c>):</para>
    /// <list type="number">
    /// <item>Work out the patch to sample (a square around the cursor body, in the left eye's
    /// viewport), stamp it with the frame number, and remember the eye it will be rendered
    /// with.</item>
    /// <item>The active pipeline's hook draws the patch into a 32x32 RGBAFloat target while the
    /// camera renders, from the camera depth texture: BiRP here (a camera command buffer), URP
    /// and HDRP in their guarded sub-assemblies (a render-graph pass / a global custom pass).</item>
    /// <item>Next frame, request an <see cref="AsyncGPUReadback"/> of that target. It never
    /// waits on the GPU; the result arrives one or two frames later.</item>
    /// <item>The callback takes the nearest depth, unprojects it through the eye it was
    /// rendered with (<see cref="DisplayXRCursorDepthMath"/>), and publishes a point.</item>
    /// </list>
    /// <para>Anything missing (no depth texture, the pass never ran, no readback) simply means
    /// no fresh result, and Auto uses measured disparity instead.</para>
    /// </summary>
    internal static class DisplayXRCursorDepthPatch
    {
        internal const int Size = 32;
        const int kRing = 16;            // snapshots kept, by stamp
        const int kMaxInFlight = 3;      // readbacks outstanding
        const int kFreshFrames = 8;      // a result older than this is no evidence

        internal enum Pipeline { BiRP, URP, HDRP, Other }

        // ---- State the pipeline hooks read --------------------------------------------

        /// <summary>The cursor uses depth (Depth or Auto, enabled). Hooks keep their depth
        /// texture switched on while this holds, so the pointer entering/leaving the canvas never
        /// reallocates pipeline textures.</summary>
        internal static bool Active { get; private set; }

        /// <summary>Draw the patch this frame (Active, and the pointer is on the canvas).</summary>
        internal static bool DrawThisFrame { get; private set; }

        internal static Camera TargetCamera { get; private set; }
        internal static Material Material { get; private set; }
        internal static RenderTexture Target { get; private set; }

        internal static readonly int IdPatchRect = Shader.PropertyToID("_DXRPatchRect");
        internal static readonly int IdUVScale = Shader.PropertyToID("_DXRUVScale");
        internal static readonly int IdStamp = Shader.PropertyToID("_DXRStamp");
        internal static readonly int IdFlipY = Shader.PropertyToID("_DXRFlipY");
        internal static readonly int IdEyeGate = Shader.PropertyToID("_DXRCursorEyeGate");
        internal static readonly int IdSlice = Shader.PropertyToID("_DXRSlice");
        internal static readonly int IdSrc = Shader.PropertyToID("_DXRDepthSrc");
        internal static readonly int IdSrcArray = Shader.PropertyToID("_DXRDepthSrcArray");

        /// <summary>Patch rect (bottom-left UV over the eye viewport) for this frame.</summary>
        internal static Vector4 PatchRect { get; private set; }
        /// <summary>Viewport UV -> texture UV scale for BiRP/URP (the rendered fraction of the
        /// eye texture). HDRP uses its RTHandle scale instead.</summary>
        internal static Vector2 ViewportScale { get; private set; } = Vector2.one;
        internal static float Stamp { get; private set; }
        internal static float FlipY { get; private set; }

        /// <summary>Hooks fill a property block with the per-frame values + the source.</summary>
        internal static void FillProperties(MaterialPropertyBlock mpb, Vector2 uvScale)
        {
            mpb.SetVector(IdPatchRect, PatchRect);
            mpb.SetVector(IdUVScale, new Vector4(uvScale.x, uvScale.y, 0f, 0f));
            mpb.SetFloat(IdStamp, Stamp);
            mpb.SetFloat(IdFlipY, FlipY);
            mpb.SetFloat(IdEyeGate, 1f);
            mpb.SetFloat(IdSlice, 0f); // left eye
        }

        /// <summary>A hook reports that it recorded the patch draw for this frame.</summary>
        internal static void NoteRecorded(Pipeline p)
        {
            s_LastRecordedFrame = Time.frameCount;
            SetAvailability(p, null);
        }

        /// <summary>A hook reports why it could NOT draw (null clears). Logged on change.</summary>
        internal static void SetAvailability(Pipeline p, string unavailableReason)
        {
            if (unavailableReason == s_Unavailable && p == s_AvailPipeline) return;
            s_Unavailable = unavailableReason;
            s_AvailPipeline = p;
            if (unavailableReason != null) s_EverResult = false;
            if (unavailableReason != null)
                Debug.Log($"[DisplayXR] Depth-aware cursor: scene depth unavailable on {p} ({unavailableReason}) " +
                          "- Auto uses measured disparity.");
            else
                Debug.Log($"[DisplayXR] Depth-aware cursor: reading scene depth on {p}.");
        }

        // ---- Results -------------------------------------------------------------------

        /// <summary>A depth result arrived recently and was drawn recently.</summary>
        internal static bool Fresh =>
            s_ResultArrivalFrame >= 0 && Time.frameCount - s_ResultArrivalFrame <= kFreshFrames &&
            Time.frameCount - s_ResultStampFrame <= kFreshFrames + kMaxInFlight + 2;

        /// <summary>
        /// Scene depth has produced results this session and no hook has reported it unavailable
        /// since. Auto stays on the depth branch while this holds, even across a brief gap (the
        /// pointer re-entering the canvas, a dropped readback): it then sends "nothing under the
        /// cursor" for a frame or two - a slow sink - instead of swapping to measured disparity
        /// and back, which could feed the runtime an old measured patch.
        /// </summary>
        internal static bool Working => s_EverResult && s_Unavailable == null;

        /// <summary>The latest result: content under the cursor, and its Unity-world point.</summary>
        internal static bool ResultHasContent { get; private set; }
        internal static Vector3 ResultPoint { get; private set; }

        // ---- Implementation ------------------------------------------------------------

        struct Snapshot
        {
            public int frame;
            public bool valid;
            public float[] pos, quat, proj;
            public bool reversedZ;
        }

        static readonly Snapshot[] s_Ring = new Snapshot[kRing];
        static readonly float[] s_Raw = new float[Size * Size];
        static readonly float[] s_U = new float[Size * Size];
        static readonly float[] s_V = new float[Size * Size];
        static Action<AsyncGPUReadbackRequest> s_Callback;
        static int s_InFlight;
        static int s_LastRecordedFrame = -100;
        static int s_ResultArrivalFrame = -1;
        static int s_ResultStampFrame = -1;
        static string s_Unavailable = "";
        static Pipeline s_AvailPipeline = Pipeline.Other;
        static bool s_Disabled;           // shader / format missing: never try again
        static bool s_ProbeLogged;
        static bool s_EverResult;
        static int s_ActiveFrames;

        // BiRP
        static CommandBuffer s_BirpCmd;
        static Camera s_BirpCam;
        static bool s_BirpSetDepthMode;
        static DepthTextureMode s_BirpPrevMode;

        internal static Pipeline CurrentPipeline()
        {
            var rp = GraphicsSettings.currentRenderPipeline;
            if (rp == null) return Pipeline.BiRP;
            string n = rp.GetType().Name;
            if (n.Contains("Universal")) return Pipeline.URP;
            if (n.Contains("HDRender")) return Pipeline.HDRP;
            return Pipeline.Other;
        }

        static bool EnsureResources()
        {
            if (s_Disabled) return false;
            if (Material == null)
            {
                var sh = Shader.Find("Hidden/DisplayXR/CursorDepthPatch");
                if (sh == null || !sh.isSupported)
                {
                    s_Disabled = true;
                    SetAvailability(CurrentPipeline(), "patch shader missing or unsupported");
                    return false;
                }
                Material = new Material(sh) { hideFlags = HideFlags.HideAndDontSave };
            }
            if (Target == null)
            {
                if (!SystemInfo.SupportsRenderTextureFormat(RenderTextureFormat.ARGBFloat))
                {
                    s_Disabled = true;
                    SetAvailability(CurrentPipeline(), "ARGBFloat render textures unsupported");
                    return false;
                }
                Target = new RenderTexture(Size, Size, 0, RenderTextureFormat.ARGBFloat, RenderTextureReadWrite.Linear)
                {
                    name = "DisplayXR Cursor Depth Patch",
                    filterMode = FilterMode.Point,
                    useMipMap = false,
                    hideFlags = HideFlags.HideAndDontSave,
                };
                Target.Create();
                s_Callback = OnReadback;
            }
            return true;
        }

        /// <summary>
        /// Called once per frame by DisplayXRCursorDepth while the session runs.
        /// <paramref name="use"/>: the selected source wants depth (Depth or Auto, enabled).
        /// </summary>
        internal static void Tick(bool use)
        {
            Active = use && EnsureResources();
            DrawThisFrame = false;
            Camera cam = DisplayXRRigManager.ActiveCamera != null ? DisplayXRRigManager.ActiveCamera : Camera.main;
            TargetCamera = Active ? cam : null;
            var pipeline = CurrentPipeline();

            if (!Active || cam == null)
            {
                DetachBiRP();
                return;
            }

            if (++s_ActiveFrames == 60) LogFillOutDepthProbe(pipeline);

            // Readback of what LAST frame drew (requested before this frame renders).
            if (s_InFlight < kMaxInFlight && Time.frameCount - s_LastRecordedFrame <= 2)
            {
                s_InFlight++;
                AsyncGPUReadback.Request(Target, 0, TextureFormat.RGBAFloat, s_Callback);
            }

            // This frame's patch.
            if (DisplayXRProviderNative.dxr_prov_cursor_get_canvas_uv(out float cu, out float cv) == 0 ||
                DisplayXRProviderNative.dxr_prov_cursor_get_eye(out var eye) == 0)
            {
                DetachBiRPIfNot(pipeline);
                return;
            }
            // Remember the eye this frame renders with (ring slot reused: no per-frame garbage).
            ref Snapshot snap = ref s_Ring[Time.frameCount % kRing];
            snap.pos ??= new float[3];
            snap.quat ??= new float[4];
            snap.proj ??= new float[16];
            float[] proj = snap.proj;
            unsafe
            {
                for (int i = 0; i < 16; i++) proj[i] = eye.projection[i];
                for (int i = 0; i < 3; i++) snap.pos[i] = eye.position[i];
                for (int i = 0; i < 4; i++) snap.quat[i] = eye.orientation[i];
                ViewportScale = new Vector2(eye.viewport[0], eye.viewport[1]);
            }
            snap.reversedZ = SystemInfo.usesReversedZBuffer;
            snap.frame = Time.frameCount;
            snap.valid = true;

            float aspect = proj[0] != 0f ? proj[5] / proj[0] : 1f; // (r-l)/(t-b) of the frustum
            DisplayXRCursorDepthMath.PatchRect(cu, cv, aspect, out float px, out float py, out float pw, out float ph);
            PatchRect = new Vector4(px, py, pw, ph);
            Stamp = Time.frameCount % (1 << 20);
            FlipY = s_FlipYOverride;
            DrawThisFrame = true;

            if (pipeline == Pipeline.BiRP) TickBiRP(cam);
            else DetachBiRP();
            if (pipeline == Pipeline.Other) SetAvailability(pipeline, "custom render pipeline");

            // BiRP draws from a command buffer whose material values are read when it runs.
            Material.SetVector(IdPatchRect, PatchRect);
            Material.SetVector(IdUVScale, new Vector4(ViewportScale.x, ViewportScale.y, 0f, 0f));
            Material.SetFloat(IdStamp, Stamp);
            Material.SetFloat(IdFlipY, FlipY);
        }

        static void DetachBiRPIfNot(Pipeline p)
        {
            if (p != Pipeline.BiRP) DetachBiRP();
        }

        static readonly float s_FlipYOverride = ReadFlipOverride();
        static float ReadFlipOverride()
        {
            // Diagnostic for the panel run: DISPLAYXR_CURSOR_DEPTH_FLIPY=1 samples the depth
            // texture upside down, in case a pipeline stores it that way on some API.
            var e = Environment.GetEnvironmentVariable("DISPLAYXR_CURSOR_DEPTH_FLIPY");
            return e == "1" ? 1f : 0f;
        }

        // ---- BiRP ------------------------------------------------------------------------

        static void TickBiRP(Camera cam)
        {
            // Forward: only if the camera ALREADY renders a depth texture (never force a
            // depth pre-pass on an app). Deferred: the depth is a by-product of the G-buffer,
            // so asking for it costs a copy, not a pass.
            bool deferred = cam.actualRenderingPath == RenderingPath.DeferredShading;
            if (!deferred && (cam.depthTextureMode & DepthTextureMode.Depth) == 0)
            {
                DetachBiRP();
                SetAvailability(Pipeline.BiRP, "forward rendering and the camera renders no depth texture");
                return;
            }
            if (s_BirpCam != cam)
            {
                DetachBiRP();
                if (deferred && (cam.depthTextureMode & DepthTextureMode.Depth) == 0)
                {
                    s_BirpPrevMode = cam.depthTextureMode;
                    cam.depthTextureMode |= DepthTextureMode.Depth;
                    s_BirpSetDepthMode = true;
                    Debug.Log("[DisplayXR] Depth-aware cursor: enabled the camera depth texture (deferred: no extra pass); restored when the cursor is disabled.");
                }
                s_BirpCmd ??= BuildBiRPCommandBuffer();
                cam.AddCommandBuffer(CameraEvent.BeforeImageEffects, s_BirpCmd);
                Camera.onPreRender += OnBiRPPreRender;
                s_BirpCam = cam;
            }
            NoteRecorded(Pipeline.BiRP);
        }

        static CommandBuffer BuildBiRPCommandBuffer()
        {
            var cmd = new CommandBuffer { name = "DisplayXR Cursor Depth Patch" };
            cmd.SetRenderTarget(Target);
            cmd.DrawProcedural(Matrix4x4.identity, Material, 2, MeshTopology.Triangles, 3, 1);
            cmd.SetRenderTarget(BuiltinRenderTextureType.CameraTarget); // hand the camera its target back
            return cmd;
        }

        // Multi-pass renders the camera once per eye; the command buffer runs in both. Gate
        // the draw to the LEFT eye - the one C# unprojects with.
        static void OnBiRPPreRender(Camera c)
        {
            if (c != s_BirpCam) return;
            bool left = c.stereoActiveEye != Camera.MonoOrStereoscopicEye.Right;
            Shader.SetGlobalFloat(IdEyeGate, left ? 1f : 0f);
        }

        static void DetachBiRP()
        {
            if (s_BirpCam != null)
            {
                if (s_BirpCmd != null) s_BirpCam.RemoveCommandBuffer(CameraEvent.BeforeImageEffects, s_BirpCmd);
                if (s_BirpSetDepthMode) s_BirpCam.depthTextureMode = s_BirpPrevMode;
                Camera.onPreRender -= OnBiRPPreRender;
            }
            s_BirpCam = null;
            s_BirpSetDepthMode = false;
        }

        // ---- Readback ----------------------------------------------------------------------

        static void OnReadback(AsyncGPUReadbackRequest req)
        {
            s_InFlight = Math.Max(0, s_InFlight - 1);
            if (req.hasError || req.done == false) return;
            NativeArray<float> data;
            try { data = req.GetData<float>(); }
            catch (Exception) { return; }
            int n = Size * Size;
            if (data.Length < n * 4) return;

            // All drawn texels carry the same stamp; texels a gated / skipped draw left alone
            // carry an older one. Use the newest stamp that matches a remembered eye.
            int newest = -1;
            for (int i = 0; i < n; i++)
            {
                int st = (int)data[i * 4 + 3];
                if (st > newest) newest = st;
            }
            if (newest <= 0) return;
            Snapshot snap = s_Ring[newest % kRing];
            if (!snap.valid || snap.frame % (1 << 20) != newest) return;

            int m = 0;
            for (int i = 0; i < n; i++)
            {
                if ((int)data[i * 4 + 3] != newest) continue;
                s_Raw[m] = data[i * 4 + 0];
                s_U[m] = data[i * 4 + 1];
                s_V[m] = data[i * 4 + 2];
                m++;
            }
            int best = DisplayXRCursorDepthMath.NearestTexel(s_Raw, m, snap.reversedZ);
            bool has = false;
            Vector3 world = default;
            if (best >= 0 &&
                DisplayXRCursorDepthMath.UnprojectToView(snap.proj, s_U[best], s_V[best], s_Raw[best], snap.reversedZ,
                                                          out float vx, out float vy, out float vz))
            {
                DisplayXRCursorDepthMath.ViewToSpace(snap.pos, snap.quat, vx, vy, vz,
                                                     out float ox, out float oy, out float oz);
                world = new Vector3(ox, oy, -oz); // OpenXR locate space -> Unity world (rig pose)
                has = true;
            }
            ResultHasContent = has;
            ResultPoint = world;
            s_ResultArrivalFrame = Time.frameCount;
            s_ResultStampFrame = snap.frame;
            s_EverResult = true;
        }

        // ---- shouldFillOutDepth probe (answers the native-route question for free) ------

        static readonly List<XRDisplaySubsystem> s_Displays = new List<XRDisplaySubsystem>();

        static void LogFillOutDepthProbe(Pipeline p)
        {
            if (s_ProbeLogged) return;
            s_ProbeLogged = true;
            try
            {
                SubsystemManager.GetSubsystems(s_Displays);
                foreach (var d in s_Displays)
                {
                    if (!d.running) continue;
                    int count = d.GetRenderPassCount();
                    string fill = "n/a";
                    if (count > 0)
                    {
                        d.GetRenderPass(0, out XRDisplaySubsystem.XRRenderPass rp);
                        fill = rp.shouldFillOutDepth ? "true" : "false";
                    }
                    Debug.Log($"[DisplayXR] Depth-aware cursor probe: XRRenderPass.shouldFillOutDepth={fill} " +
                              $"(pipeline {p}, {SystemInfo.graphicsDeviceType}, renderPasses={count}, " +
                              $"reversedZ={SystemInfo.usesReversedZBuffer})");
                    return;
                }
                Debug.Log($"[DisplayXR] Depth-aware cursor probe: no running XRDisplaySubsystem (pipeline {p}).");
            }
            catch (Exception e)
            {
                Debug.Log("[DisplayXR] Depth-aware cursor probe failed: " + e.Message);
            }
        }

        /// <summary>Session stopped / cursor disabled: drop results and BiRP hooks.</summary>
        internal static void Reset()
        {
            DetachBiRP();
            Active = false;
            DrawThisFrame = false;
            TargetCamera = null;
            ResultHasContent = false;
            s_ResultArrivalFrame = -1;
            s_ResultStampFrame = -1;
            s_ProbeLogged = false;
            s_ActiveFrames = 0;
            s_EverResult = false;
            s_Unavailable = "";
            for (int i = 0; i < kRing; i++) s_Ring[i].valid = false;
        }
    }
}
