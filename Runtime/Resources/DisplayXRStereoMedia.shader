// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Stereo media (#463): shows one half of a side-by-side / top-bottom picture to each eye.
// Driven entirely by DisplayXRStereoMedia - every rectangle below is computed in C#
// (DisplayXRStereoMediaMath, unit-tested in CI); the shader only picks the eye's set.
//
// ONE SHADER FOR BiRP, URP AND HDRP - on purpose, and only because all of this holds:
//  * It is written against the built-in include (UnityCG.cginc) and has NO render-pipeline
//    include, so it may live in Resources/ (builds include it) without breaking a project
//    that lacks URP or HDRP (see CLAUDE.md, the #130 revert). Same rule as CursorDepthPatch.
//  * A pass with no LightMode tag is "SRPDefaultUnlit", which URP draws in its opaque and
//    transparent passes and HDRP lists in its forward pass names ("a fall back to legacy
//    unlit rendering"). It is how Sprites/Default - the boot splash's shader - renders in
//    all three pipelines.
//  * Single-pass instanced: URP and HDRP both publish the built-in stereo constants
//    (unity_StereoMatrixVP / unity_StereoMatrixP, XRBuiltinShaderConstants) that
//    UnityCG.cginc's UNITY_MATRIX_VP / UNITY_MATRIX_P read, and set STEREO_INSTANCING_ON,
//    which "#pragma multi_compile_instancing" declares.
//
// EYE INDEX. Read in the VERTEX stage, where it is defined in every path:
//  * single-pass instanced (URP / HDRP): UNITY_SETUP_INSTANCE_ID sets
//    unity_StereoEyeIndex = instanceID & 1 (the engine doubles the instance count);
//  * multi-pass (BiRP): unity_StereoEyeIndex is a per-camera constant the engine sets for
//    each eye's pass;
//  * no stereo at all: the same constant, 0 -> the left picture.
// The eye's UV mapping travels to the fragment stage as interpolants, so the fragment
// stage needs no eye index (UNITY_SETUP_STEREO_EYE_INDEX_POST_VERTEX is still called, as
// every stereo shader should).
Shader "DisplayXR/StereoMedia"
{
    Properties
    {
        _MainTex ("Media", 2D) = "black" {}
        _Color ("Tint", Color) = (1, 1, 1, 1)
        // uv_texture = uv_quad * xy + zw, per eye.
        [HideInInspector] _DXRMediaSTL ("Left UV scale / offset", Vector) = (0.5, 1, 0, 0)
        [HideInInspector] _DXRMediaSTR ("Right UV scale / offset", Vector) = (0.5, 1, 0.5, 0)
        // uv_texture clamp, min xy / max zw, per eye.
        [HideInInspector] _DXRMediaClampL ("Left UV clamp", Vector) = (0, 0, 0.5, 1)
        [HideInInspector] _DXRMediaClampR ("Right UV clamp", Vector) = (0.5, 0, 1, 1)
        // Quad: size in object units. Full screen: fraction of the view covered (0..1).
        [HideInInspector] _DXRMediaFrame ("Frame", Vector) = (1, 1, 0, 0)
        [HideInInspector] _DXRMediaFullScreen ("Full screen", Float) = 0
        // 0 none, 1 sRGB -> linear, 2 linear -> sRGB.
        [HideInInspector] _DXRMediaColor ("Colour conversion", Float) = 0
        // 0 opaque, 1 premultiplied source, 2 straight-alpha source.
        [HideInInspector] _DXRMediaAlpha ("Alpha mode", Float) = 0
        [HideInInspector] _SrcBlend ("Src blend", Float) = 1
        [HideInInspector] _DstBlend ("Dst blend", Float) = 0
        [HideInInspector] _ZWrite ("Z write", Float) = 1
        [HideInInspector] _ZTest ("Z test", Float) = 4
    }

    SubShader
    {
        Tags { "Queue" = "Geometry" "RenderType" = "Opaque" "IgnoreProjector" = "True" "PreviewType" = "Plane" }

        Pass
        {
            Cull Off
            ZWrite [_ZWrite]
            ZTest [_ZTest]
            Blend [_SrcBlend] [_DstBlend]

            CGPROGRAM
            #pragma vertex vert
            #pragma fragment frag
            #pragma target 3.5
            #pragma multi_compile_instancing
            #include "UnityCG.cginc"

            sampler2D _MainTex;
            float4 _MainTex_TexelSize;
            fixed4 _Color;
            float4 _DXRMediaSTL;
            float4 _DXRMediaSTR;
            float4 _DXRMediaClampL;
            float4 _DXRMediaClampR;
            float4 _DXRMediaFrame;
            float _DXRMediaFullScreen;
            float _DXRMediaColor;
            float _DXRMediaAlpha;

            struct appdata
            {
                float4 vertex : POSITION;   // unit quad, xy in [-0.5, 0.5]
                float2 uv : TEXCOORD0;      // (0,0) bottom-left .. (1,1) top-right
                UNITY_VERTEX_INPUT_INSTANCE_ID
            };

            struct v2f
            {
                float4 pos : SV_POSITION;
                float2 uv : TEXCOORD0;
                float4 uvClamp : TEXCOORD1; // the same at every vertex, so it interpolates to itself
                UNITY_VERTEX_OUTPUT_STEREO
            };

            v2f vert(appdata v)
            {
                v2f o;
                UNITY_SETUP_INSTANCE_ID(v);
                UNITY_INITIALIZE_OUTPUT(v2f, o);
                UNITY_INITIALIZE_VERTEX_OUTPUT_STEREO(o);

                if (_DXRMediaFullScreen > 0.5)
                {
                    // Straight to clip space: no view, no projection, so no parallax and no
                    // head-coupled motion - each eye's target gets its half directly.
                    float2 ndc = v.vertex.xy * 2.0 * _DXRMediaFrame.xy;
                    // "Picture up" must be "scene up". Whether clip +Y is up depends on the
                    // API and on whether this pass renders into a texture; the projection
                    // matrix of this very pass already encodes the answer (Unity negates
                    // its Y row when it flips), so take the sign from it instead of
                    // re-deriving it from _ProjectionParams, which each pipeline sets its
                    // own way. A perspective projection has m11 > 0 unflipped.
                    float up = UNITY_MATRIX_P[1][1] < 0.0 ? -1.0 : 1.0;
                    // On the far plane: Background order z-tests against the scene (LEqual,
                    // like a skybox), Foreground order ignores depth.
                    #if defined(UNITY_REVERSED_Z)
                    float farZ = 0.0;
                    #else
                    float farZ = 1.0;
                    #endif
                    o.pos = float4(ndc.x, ndc.y * up, farZ, 1.0);
                }
                else
                {
                    o.pos = UnityObjectToClipPos(float4(v.vertex.xy * _DXRMediaFrame.xy, 0.0, 1.0));
                }

                // 0 = left. More than two views (a future N-view mode): every view but the
                // first gets the right picture.
                bool rightEye = unity_StereoEyeIndex != 0;
                float4 st = rightEye ? _DXRMediaSTR : _DXRMediaSTL;
                o.uv = v.uv * st.xy + st.zw;
                o.uvClamp = rightEye ? _DXRMediaClampR : _DXRMediaClampL;
                return o;
            }

            float3 SRGBToLinear(float3 c)
            {
                c = max(c, 0.0);
                float3 lo = c / 12.92;
                float3 hi = pow((c + 0.055) / 1.055, 2.4);
                return float3(c.r <= 0.04045 ? lo.r : hi.r, c.g <= 0.04045 ? lo.g : hi.g, c.b <= 0.04045 ? lo.b : hi.b);
            }

            float3 LinearToSRGB(float3 c)
            {
                c = max(c, 0.0);
                float3 lo = c * 12.92;
                float3 hi = 1.055 * pow(c, 1.0 / 2.4) - 0.055;
                return float3(c.r <= 0.0031308 ? lo.r : hi.r, c.g <= 0.0031308 ? lo.g : hi.g, c.b <= 0.0031308 ? lo.b : hi.b);
            }

            float4 frag(v2f i) : SV_Target
            {
                UNITY_SETUP_STEREO_EYE_INDEX_POST_VERTEX(i);

                // Never filter across the seam between the two halves.
                float2 uv = clamp(i.uv, i.uvClamp.xy, i.uvClamp.zw);
                // Unity marks a texture it stores upside down with a negative texel height.
                // The rectangles above are in the upright texture's space, so mirror last.
                #if UNITY_UV_STARTS_AT_TOP
                if (_MainTex_TexelSize.y < 0.0)
                    uv.y = 1.0 - uv.y;
                #endif

                float4 c = tex2D(_MainTex, uv);
                bool isOpaque = _DXRMediaAlpha < 0.5;
                bool premultipliedSource = _DXRMediaAlpha > 0.5 && _DXRMediaAlpha < 1.5;
                float a = isOpaque ? 1.0 : c.a;
                float3 rgb = c.rgb;

                if (_DXRMediaColor > 0.5)
                {
                    // The transfer curve applies to straight colour.
                    if (premultipliedSource)
                        rgb = a > 1e-4 ? rgb / a : float3(0.0, 0.0, 0.0);
                    rgb = _DXRMediaColor < 1.5 ? SRGBToLinear(rgb) : LinearToSRGB(rgb);
                    if (premultipliedSource)
                        rgb *= a;
                }

                rgb *= _Color.rgb;
                if (!isOpaque)
                {
                    // Output is premultiplied (Blend One OneMinusSrcAlpha), which is also
                    // what an alpha-native transparent overlay wants in the target.
                    if (!premultipliedSource)
                        rgb *= a;
                    rgb *= _Color.a;
                    a *= _Color.a;
                }
                return float4(rgb, a);
            }
            ENDCG
        }
    }
    Fallback Off
}
