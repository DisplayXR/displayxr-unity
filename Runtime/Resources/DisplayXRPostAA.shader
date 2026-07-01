// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Alpha-aware mini-FXAA post pass for DisplayXR rig cameras.
//
// Unity 6 + Built-in RP + MultiPass + OpenXR submits the XR eye swapchain at
// sampleCount=1 (confirmed D3D12 AND Vulkan), and even with an internal MSAA
// eye RT Unity's resolve does not preserve fractional alpha — so silhouettes
// (especially in alpha-native transparent overlays) land with binary alpha and
// alias. This is a luma-edge FXAA that lerps the FULL RGBA texel along the
// detected edge, so it softens the alpha channel too, sidestepping Unity's
// broken eye-RT alpha resolve. Driven by DisplayXRPostAA.
Shader "Hidden/DisplayXR/PostAA"
{
    Properties { _MainTex ("Texture", 2D) = "white" {} }
    SubShader
    {
        Cull Off ZWrite Off ZTest Always
        Pass
        {
            CGPROGRAM
            #pragma vertex vert_img
            #pragma fragment frag
            #include "UnityCG.cginc"

            sampler2D _MainTex;
            float4 _MainTex_TexelSize;

            #define FXAA_SPAN_MAX 8.0
            #define FXAA_REDUCE_MUL (1.0/8.0)
            #define FXAA_REDUCE_MIN (1.0/128.0)

            fixed luma(fixed3 rgb) { return dot(rgb, fixed3(0.299, 0.587, 0.114)); }

            fixed4 frag(v2f_img i) : SV_Target
            {
                float2 ts = _MainTex_TexelSize.xy;
                float2 uv = i.uv;

                fixed4 cM    = tex2D(_MainTex, uv);
                fixed3 rgbNW = tex2D(_MainTex, uv + float2(-1,-1) * ts).rgb;
                fixed3 rgbNE = tex2D(_MainTex, uv + float2( 1,-1) * ts).rgb;
                fixed3 rgbSW = tex2D(_MainTex, uv + float2(-1, 1) * ts).rgb;
                fixed3 rgbSE = tex2D(_MainTex, uv + float2( 1, 1) * ts).rgb;
                fixed3 rgbM  = cM.rgb;

                fixed lNW = luma(rgbNW), lNE = luma(rgbNE);
                fixed lSW = luma(rgbSW), lSE = luma(rgbSE);
                fixed lM  = luma(rgbM);
                fixed lMin = min(lM, min(min(lNW, lNE), min(lSW, lSE)));
                fixed lMax = max(lM, max(max(lNW, lNE), max(lSW, lSE)));

                float2 dir;
                dir.x = -((lNW + lNE) - (lSW + lSE));
                dir.y =  ((lNW + lSW) - (lNE + lSE));

                float dirReduce = max((lNW + lNE + lSW + lSE) * 0.25 * FXAA_REDUCE_MUL,
                                      FXAA_REDUCE_MIN);
                float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);
                dir = clamp(dir * rcpDirMin, -FXAA_SPAN_MAX, FXAA_SPAN_MAX) * ts;

                fixed4 rA = 0.5 * (tex2D(_MainTex, uv + dir * (1.0/3.0 - 0.5))
                                 + tex2D(_MainTex, uv + dir * (2.0/3.0 - 0.5)));
                fixed4 rB = rA * 0.5 + 0.25 * (tex2D(_MainTex, uv + dir * -0.5)
                                             + tex2D(_MainTex, uv + dir *  0.5));
                fixed lB = luma(rB.rgb);
                return (lB < lMin || lB > lMax) ? rA : rB;
            }
            ENDCG
        }
    }
    Fallback Off
}
