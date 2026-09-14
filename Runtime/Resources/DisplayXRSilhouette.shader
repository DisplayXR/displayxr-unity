// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Per-pixel silhouette mask shader (issue #57 Approach B+).
//
// DisplayXRTransparentOverlay uses this to render each clickable
// renderer to a small R8 RenderTexture each frame. Pixels with any
// rasterized geometry get red=1; everything else stays at the clear
// color (0). The mask is read back via AsyncGPUReadback, walked into a
// run-length-encoded set of RECTs in native, and applied as
// SetWindowRgn — outside the silhouette the OS treats our window as if
// it didn't exist (cross-process click-through with full fidelity).
//
// The SAME readback has a second consumer: it is chained to the runtime
// as XrContentMaskDXR (XR_DXR_depth_budget v3+), telling the runtime
// which patch of desktop to measure when deciding this overlay's rear
// depth budget. See the mask invariant on the z override below.
//
// Renderer-agnostic: ZWrite/ZTest off + Cull off so it rasterizes
// regardless of depth, backface culling, or material setup. We only
// care "did anything draw here?" — the answer drives whether the OS
// hit-tests this pixel as ours or as the desktop's.
//
// We use a CUSTOM _DXRViewProj uniform instead of UNITY_MATRIX_VP.
// With an active XR session, Unity's render pipeline overrides the
// view+projection matrices that DrawRenderer normally consumes
// (CommandBuffer.SetViewProjectionMatrices gets ignored). _DXRViewProj
// is a property Unity doesn't know about, so SetGlobalMatrix sticks —
// and two render passes with different matrices produce a real
// boolean union, not an aliased single pass. unity_ObjectToWorld is
// still per-renderer and not affected by stereo state.

Shader "Hidden/DisplayXR/Silhouette"
{
    SubShader
    {
        Tags { "RenderType" = "Opaque" "Queue" = "Geometry" }
        Pass
        {
            ZWrite Off
            ZTest Always
            Cull Off
            Lighting Off

            CGPROGRAM
            #pragma vertex vert
            #pragma fragment frag
            #include "UnityCG.cginc"

            float4x4 _DXRViewProj;

            struct appdata { float4 vertex : POSITION; };
            struct v2f { float4 pos : SV_POSITION; };

            v2f vert(appdata v)
            {
                v2f o;
                float4 worldPos = mul(unity_ObjectToWorld, v.vertex);
                o.pos = mul(_DXRViewProj, worldPos);
                // LOAD-BEARING TWICE OVER — DO NOT REMOVE, AND DO NOT
                // REPLACE THIS PASS WITH A POST-CLIP ALPHA READBACK.
                //
                // Pin clip-space z to mid-frustum (z = 0.5w → z_ndc = 0.5,
                // inside both D3D [0,1] and GL [-1,1] ranges) so the
                // projection's near/far planes never clip foreground/back
                // geometry out of the mask. _DXRViewProj bakes in near=0.05/
                // far=100 (the cached Kooima matrices), which are TIGHTER than
                // the camera's actual render clip — without this, the tiger's
                // closest parts (cheek, arm) get sliced into wedge-shaped
                // click-through holes that the woven render doesn't have. The
                // mask only needs x/y coverage; depth is irrelevant here
                // (ZTest Always, no depth buffer), so overriding z is safe.
                //
                // That was reason one (the click-through region). Reason
                // two: this z override is ALSO the v4 invariant of
                // XR_DXR_depth_budget, whose mask this readback feeds. The
                // spec requires the content mask to be the silhouette the
                // content WOULD have at an unrestricted budget — rasterised
                // ignoring the far clip. Because this pass draws pre-clip
                // GEOMETRY with the far defeated (here) and the depth-budget
                // foreground clip is a separate fragment-discard pass over the
                // camera's colour target (_DXRForegroundFar / _DXRRearOffset),
                // this plugin already satisfies v4 by construction and cannot
                // enter the oscillation of displayxr-runtime#1470, where the
                // mask is derived from the RENDERED (post-far-clip) content and
                // so feeds the clip state back into the runtime's measurement:
                // clipped → small silhouette over quiet desktop → budget opens
                // → rear half appears → silhouette grows over busy desktop →
                // budget closes → round again, ~1 Hz on a static desktop.
                //
                // The click-through region may keep post-clip alpha (it is about
                // which pixels were painted); the depth-budget mask may not. The
                // two consumers share one artefact here ONLY because that
                // artefact is pre-clip geometry. Sourcing this mask from the
                // swapchain alpha to "save a pass" would reintroduce #1470.
                o.pos.z = o.pos.w * 0.5;
                return o;
            }

            fixed4 frag(v2f i) : SV_Target
            {
                return fixed4(1, 0, 0, 1);
            }
            ENDCG
        }
    }
}
