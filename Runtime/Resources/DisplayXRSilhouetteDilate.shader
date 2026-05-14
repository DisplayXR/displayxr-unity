// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
//
// Morphological dilation pass for the per-pixel silhouette mask
// (issue #57 Approach B+).
//
// Reads an R8 silhouette texture (red=1 where any view rasterized
// geometry, 0 elsewhere) and writes a dilated R8 — each output pixel
// = max(red) over a 5x5 neighborhood in the source. Equivalent to
// "expand the silhouette by 2 pixels on every side".
//
// Why dilate the union mask:
//   1. Cube edges in the source mask have sub-pixel rasterization
//      precision; the SetWindowRgn boundary is bit-flat (in/out), so
//      AA pixels falling just outside the rasterized silhouette get
//      clipped. Dilation pulls those pixels inside the region.
//   2. Safety margin against per-eye disparity that exceeds what the
//      union covers (rare on 2-view, but cheap insurance).
//   3. Visually smooths staircase artifacts from low-res masks.
//
// Used via Graphics.Blit(src, dst, dilateMat) after the silhouette
// pass and before AsyncGPUReadback.

Shader "Hidden/DisplayXR/SilhouetteDilate"
{
    Properties
    {
        _MainTex ("Source", 2D) = "black" {}
    }
    SubShader
    {
        Tags { "RenderType" = "Opaque" }
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

            sampler2D _MainTex;
            float4 _MainTex_TexelSize; // (1/w, 1/h, w, h)

            struct appdata { float4 vertex : POSITION; float2 uv : TEXCOORD0; };
            struct v2f { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

            v2f vert(appdata v)
            {
                v2f o;
                o.pos = UnityObjectToClipPos(v.vertex);
                o.uv = v.uv;
                return o;
            }

            fixed4 frag(v2f i) : SV_Target
            {
                // 5x5 max kernel. Unrolled — small enough that the
                // compiler picks a sensible schedule and we don't
                // pay for dynamic loop overhead. Step in source-
                // texel units so the kernel size is resolution-
                // independent.
                float2 t = _MainTex_TexelSize.xy;
                float m = 0;
                [unroll] for (int dy = -2; dy <= 2; dy++)
                {
                    [unroll] for (int dx = -2; dx <= 2; dx++)
                    {
                        float v = tex2D(_MainTex, i.uv + float2(dx, dy) * t).r;
                        m = max(m, v);
                    }
                }
                return fixed4(m, 0, 0, 1);
            }
            ENDCG
        }
    }
}
