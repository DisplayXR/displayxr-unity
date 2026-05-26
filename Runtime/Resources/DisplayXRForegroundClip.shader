// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
//
// URP Lit shader with a per-eye foreground clip at the display plane
// (displayxr-unity#129).
//
// Under URP+OpenXR the per-eye projection is rebuilt by Unity's provider from
// XrView.fov + a SINGLE global zNear/zFar, so the plugin's per-view Kooima far
// (the geometric foreground clip BiRP gets for free) is ignored — full per-eye
// projection-matrix control is impossible in URP (XRView is internal readonly;
// every C# seam is overwritten or unreachable). multipass also has no usable
// in-shader eye index (unity_StereoEyeIndex is hardcoded 0). So we clip in the
// shader by comparing each fragment's view-space depth to the view-space depth
// of a single world point on the display plane, both via the current eye's
// view matrix — which makes the per-eye cuts coincide at the display plane and
// each eye use its own far automatically.
//
// The active DisplayXR rig pushes that world point each frame as the global
// float4 _DXRClipPoint, gated by the keyword _DXR_FOREGROUND_CLIP. Fragments
// behind the display plane (farther from the viewer) are discarded. When the
// keyword is off the shader is plain URP Lit, so the same material is correct
// under Built-in RP and when the clip is off.
//
// Base-map URP Lit (PBR) for dynamic (non-lightmapped) content — drop-in for
// stock URP/Lit on the transparent-overlay target. Swap a renderer's material
// shader to this and _BaseMap/_BaseColor carry over by matching property name.

Shader "DisplayXR/ForegroundClip"
{
    Properties
    {
        [MainTexture] _BaseMap("Base Map", 2D) = "white" {}
        [MainColor]   _BaseColor("Base Color", Color) = (1,1,1,1)
        _Metallic("Metallic", Range(0,1)) = 0.0
        _Smoothness("Smoothness", Range(0,1)) = 0.5
        [Normal] _BumpMap("Normal Map", 2D) = "bump" {}
        [Toggle(_NORMALMAP)] _NormalMapToggle("Use Normal Map", Float) = 0
        [Toggle(_ALPHATEST_ON)] _AlphaClip("Alpha Clip", Float) = 0
        _Cutoff("Alpha Cutoff", Range(0,1)) = 0.5
    }

    SubShader
    {
        Tags
        {
            "RenderType" = "Opaque"
            "RenderPipeline" = "UniversalPipeline"
            "Queue" = "Geometry"
        }

        HLSLINCLUDE
        #include "Packages/com.unity.render-pipelines.universal/ShaderLibrary/Core.hlsl"

        CBUFFER_START(UnityPerMaterial)
            float4 _BaseMap_ST;
            half4  _BaseColor;
            half   _Metallic;
            half   _Smoothness;
            half   _Cutoff;
        CBUFFER_END

        // Global, set via Shader.SetGlobalVector — NOT part of the material
        // CBUFFER. A single WORLD-space point on the display (convergence)
        // plane. We clip by comparing the fragment's view-space depth to this
        // point's view-space depth, BOTH measured with the current eye's view
        // matrix (UNITY_MATRIX_V, set per-eye by URP). multipass has no usable
        // in-shader eye index (unity_StereoEyeIndex is hardcoded 0), so we use
        // the per-eye view matrix instead: for each eye this is the plane
        // through the point perpendicular to that eye's forward, and since all
        // eyes share orientation it is the SAME world plane — so the per-eye
        // cuts coincide and each eye's far falls out automatically (no average,
        // no per-eye scalar).
        float4 _DXRClipPoint;

        void DXRForegroundClip(float3 positionWS)
        {
        #if defined(_DXR_FOREGROUND_CLIP)
            // Unity view space looks down -Z (deeper = more negative). Keep the
            // fragment if it is nearer than (or on) the display plane.
            float vzFrag  = TransformWorldToView(positionWS).z;
            float vzPlane = TransformWorldToView(_DXRClipPoint.xyz).z;
            clip(vzFrag - vzPlane);
        #endif
        }
        ENDHLSL

        // ----------------------------------------------------------------
        Pass
        {
            Name "ForwardLit"
            Tags { "LightMode" = "UniversalForward" }

            HLSLPROGRAM
            #pragma vertex Vertex
            #pragma fragment Fragment

            #pragma shader_feature_local _NORMALMAP
            #pragma shader_feature_local_fragment _ALPHATEST_ON

            #pragma multi_compile _ _DXR_FOREGROUND_CLIP

            // URP lighting keywords (core subset — sufficient for opaque, dynamic,
            // non-lightmapped content; ambient comes from SampleSH).
            #pragma multi_compile _ _MAIN_LIGHT_SHADOWS _MAIN_LIGHT_SHADOWS_CASCADE _MAIN_LIGHT_SHADOWS_SCREEN
            #pragma multi_compile _ _ADDITIONAL_LIGHTS_VERTEX _ADDITIONAL_LIGHTS
            #pragma multi_compile_fragment _ _ADDITIONAL_LIGHT_SHADOWS
            #pragma multi_compile_fragment _ _SHADOWS_SOFT
            #pragma multi_compile_fragment _ _SCREEN_SPACE_OCCLUSION
            #pragma multi_compile_fog

            #include "Packages/com.unity.render-pipelines.universal/ShaderLibrary/Lighting.hlsl"

            TEXTURE2D(_BaseMap);  SAMPLER(sampler_BaseMap);
            TEXTURE2D(_BumpMap);  SAMPLER(sampler_BumpMap);

            struct Attributes
            {
                float4 positionOS : POSITION;
                float3 normalOS   : NORMAL;
                float4 tangentOS  : TANGENT;
                float2 uv         : TEXCOORD0;
            };

            struct Varyings
            {
                float4 positionHCS : SV_POSITION;
                float2 uv          : TEXCOORD0;
                float3 positionWS  : TEXCOORD1;
                float3 normalWS    : TEXCOORD2;
                float4 tangentWS   : TEXCOORD3;
                float  fogCoord    : TEXCOORD4;
            };

            Varyings Vertex(Attributes IN)
            {
                Varyings OUT = (Varyings)0;
                VertexPositionInputs posInputs = GetVertexPositionInputs(IN.positionOS.xyz);
                VertexNormalInputs normInputs = GetVertexNormalInputs(IN.normalOS, IN.tangentOS);

                OUT.positionHCS = posInputs.positionCS;
                OUT.positionWS  = posInputs.positionWS;
                OUT.uv          = TRANSFORM_TEX(IN.uv, _BaseMap);
                OUT.normalWS    = normInputs.normalWS;
                OUT.tangentWS   = float4(normInputs.tangentWS, IN.tangentOS.w * GetOddNegativeScale());
                OUT.fogCoord    = ComputeFogFactor(posInputs.positionCS.z);
                return OUT;
            }

            half4 Fragment(Varyings IN) : SV_Target
            {
                DXRForegroundClip(IN.positionWS);

                half4 baseSample = SAMPLE_TEXTURE2D(_BaseMap, sampler_BaseMap, IN.uv) * _BaseColor;
            #if defined(_ALPHATEST_ON)
                clip(baseSample.a - _Cutoff);
            #endif

            #if defined(_NORMALMAP)
                half3 normalTS = UnpackNormal(SAMPLE_TEXTURE2D(_BumpMap, sampler_BumpMap, IN.uv));
            #else
                half3 normalTS = half3(0, 0, 1);
            #endif

                SurfaceData surfaceData = (SurfaceData)0;
                surfaceData.albedo     = baseSample.rgb;
                surfaceData.alpha      = baseSample.a;
                surfaceData.metallic   = _Metallic;
                surfaceData.smoothness = _Smoothness;
                surfaceData.occlusion  = 1.0h;
                surfaceData.normalTS   = normalTS;

                InputData inputData = (InputData)0;
                inputData.positionWS = IN.positionWS;

                float sgn = IN.tangentWS.w;
                float3 bitangent = sgn * cross(IN.normalWS.xyz, IN.tangentWS.xyz);
                half3x3 tangentToWorld = half3x3(IN.tangentWS.xyz, bitangent, IN.normalWS.xyz);
                inputData.normalWS = TransformTangentToWorld(normalTS, tangentToWorld);
                inputData.normalWS = NormalizeNormalPerPixel(inputData.normalWS);

                inputData.viewDirectionWS = GetWorldSpaceNormalizeViewDir(IN.positionWS);
                inputData.shadowCoord = TransformWorldToShadowCoord(IN.positionWS);
                inputData.fogCoord = IN.fogCoord;
                inputData.bakedGI = SampleSH(inputData.normalWS);
                inputData.normalizedScreenSpaceUV = GetNormalizedScreenSpaceUV(IN.positionHCS);
                inputData.shadowMask = half4(1, 1, 1, 1);

                half4 color = UniversalFragmentPBR(inputData, surfaceData);
                color.rgb = MixFog(color.rgb, IN.fogCoord);
                return color;
            }
            ENDHLSL
        }

        // ----------------------------------------------------------------
        Pass
        {
            Name "ShadowCaster"
            Tags { "LightMode" = "ShadowCaster" }

            ZWrite On
            ZTest LEqual
            ColorMask 0

            HLSLPROGRAM
            #pragma vertex Vertex
            #pragma fragment Fragment
            #pragma shader_feature_local_fragment _ALPHATEST_ON
            #pragma multi_compile _ _DXR_FOREGROUND_CLIP
            #pragma multi_compile_vertex _ _CASTING_PUNCTUAL_LIGHT_SHADOW

            #include "Packages/com.unity.render-pipelines.universal/ShaderLibrary/Shadows.hlsl"

            TEXTURE2D(_BaseMap); SAMPLER(sampler_BaseMap);

            float3 _LightDirection;
            float3 _LightPosition;

            struct Attributes
            {
                float4 positionOS : POSITION;
                float3 normalOS   : NORMAL;
                float2 uv         : TEXCOORD0;
            };

            struct Varyings
            {
                float4 positionHCS : SV_POSITION;
                float2 uv          : TEXCOORD0;
                float3 positionWS  : TEXCOORD1;
            };

            float4 GetShadowCasterPositionCS(float3 positionWS, float3 normalWS)
            {
            #if defined(_CASTING_PUNCTUAL_LIGHT_SHADOW)
                float3 lightDirectionWS = normalize(_LightPosition - positionWS);
            #else
                float3 lightDirectionWS = _LightDirection;
            #endif
                float4 positionCS = TransformWorldToHClip(ApplyShadowBias(positionWS, normalWS, lightDirectionWS));
            #if UNITY_REVERSED_Z
                positionCS.z = min(positionCS.z, UNITY_NEAR_CLIP_VALUE);
            #else
                positionCS.z = max(positionCS.z, UNITY_NEAR_CLIP_VALUE);
            #endif
                return positionCS;
            }

            Varyings Vertex(Attributes IN)
            {
                Varyings OUT;
                float3 positionWS = TransformObjectToWorld(IN.positionOS.xyz);
                float3 normalWS = TransformObjectToWorldNormal(IN.normalOS);
                OUT.positionWS = positionWS;
                OUT.positionHCS = GetShadowCasterPositionCS(positionWS, normalWS);
                OUT.uv = TRANSFORM_TEX(IN.uv, _BaseMap);
                return OUT;
            }

            half4 Fragment(Varyings IN) : SV_Target
            {
                // No foreground clip here: ShadowCaster's view matrix is the
                // light's, so a view-space-depth clip would be wrong. Shadows
                // from behind-plane geometry are harmless for the overlay.
            #if defined(_ALPHATEST_ON)
                half a = SAMPLE_TEXTURE2D(_BaseMap, sampler_BaseMap, IN.uv).a * _BaseColor.a;
                clip(a - _Cutoff);
            #endif
                return 0;
            }
            ENDHLSL
        }

        // ----------------------------------------------------------------
        Pass
        {
            Name "DepthOnly"
            Tags { "LightMode" = "DepthOnly" }

            ZWrite On
            ColorMask R

            HLSLPROGRAM
            #pragma vertex Vertex
            #pragma fragment Fragment
            #pragma shader_feature_local_fragment _ALPHATEST_ON
            #pragma multi_compile _ _DXR_FOREGROUND_CLIP

            TEXTURE2D(_BaseMap); SAMPLER(sampler_BaseMap);

            struct Attributes
            {
                float4 positionOS : POSITION;
                float2 uv         : TEXCOORD0;
            };

            struct Varyings
            {
                float4 positionHCS : SV_POSITION;
                float2 uv          : TEXCOORD0;
                float3 positionWS  : TEXCOORD1;
            };

            Varyings Vertex(Attributes IN)
            {
                Varyings OUT;
                VertexPositionInputs posInputs = GetVertexPositionInputs(IN.positionOS.xyz);
                OUT.positionHCS = posInputs.positionCS;
                OUT.positionWS  = posInputs.positionWS;
                OUT.uv = TRANSFORM_TEX(IN.uv, _BaseMap);
                return OUT;
            }

            half4 Fragment(Varyings IN) : SV_Target
            {
                DXRForegroundClip(IN.positionWS);
            #if defined(_ALPHATEST_ON)
                half a = SAMPLE_TEXTURE2D(_BaseMap, sampler_BaseMap, IN.uv).a * _BaseColor.a;
                clip(a - _Cutoff);
            #endif
                return 0;
            }
            ENDHLSL
        }

        // ----------------------------------------------------------------
        Pass
        {
            Name "DepthNormals"
            Tags { "LightMode" = "DepthNormals" }

            ZWrite On

            HLSLPROGRAM
            #pragma vertex Vertex
            #pragma fragment Fragment
            #pragma shader_feature_local _NORMALMAP
            #pragma shader_feature_local_fragment _ALPHATEST_ON
            #pragma multi_compile _ _DXR_FOREGROUND_CLIP

            #include "Packages/com.unity.render-pipelines.universal/ShaderLibrary/Lighting.hlsl"

            TEXTURE2D(_BaseMap); SAMPLER(sampler_BaseMap);
            TEXTURE2D(_BumpMap); SAMPLER(sampler_BumpMap);

            struct Attributes
            {
                float4 positionOS : POSITION;
                float3 normalOS   : NORMAL;
                float4 tangentOS  : TANGENT;
                float2 uv         : TEXCOORD0;
            };

            struct Varyings
            {
                float4 positionHCS : SV_POSITION;
                float2 uv          : TEXCOORD0;
                float3 positionWS  : TEXCOORD1;
                float3 normalWS    : TEXCOORD2;
            };

            Varyings Vertex(Attributes IN)
            {
                Varyings OUT;
                VertexPositionInputs posInputs = GetVertexPositionInputs(IN.positionOS.xyz);
                VertexNormalInputs normInputs = GetVertexNormalInputs(IN.normalOS, IN.tangentOS);
                OUT.positionHCS = posInputs.positionCS;
                OUT.positionWS  = posInputs.positionWS;
                OUT.normalWS    = normInputs.normalWS;
                OUT.uv = TRANSFORM_TEX(IN.uv, _BaseMap);
                return OUT;
            }

            half4 Fragment(Varyings IN) : SV_Target
            {
                DXRForegroundClip(IN.positionWS);
            #if defined(_ALPHATEST_ON)
                half a = SAMPLE_TEXTURE2D(_BaseMap, sampler_BaseMap, IN.uv).a * _BaseColor.a;
                clip(a - _Cutoff);
            #endif
                float3 normalWS = NormalizeNormalPerPixel(IN.normalWS);
                return half4(normalWS * 0.5 + 0.5, 0.0);
            }
            ENDHLSL
        }
    }

    FallBack "Universal Render Pipeline/Lit"
}
