// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
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
