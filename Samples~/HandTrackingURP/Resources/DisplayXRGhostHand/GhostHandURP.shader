// Ghost Hand — Universal Render Pipeline (hand-written HLSL, not Shader Graph).
//
// A pass-for-pass port of GhostHand.shader (itself derived from Ultraleap's
// "GenericHandShader", Apache License 2.0 — see THIRD-PARTY-NOTICES.md): depth
// prime (ColorMask 0, the URP spelling of "Blend Zero One"), the front-culled
// outline extruded along the view-space normal in clip space, and the body with
// the HDR fresnel rim. Lighting uses the URP main light + SH instead of
// ForwardBase. All three passes are untagged (SRPDefaultUnlit) so URP draws them
// in shader order for the material; SRP-batcher compatible (UnityPerMaterial),
// XR single-pass-instanced safe (stereo macros on every pass).
//
// Ships in the separate "Hand Tracking (URP Ghost Hand)" sample, not the main Hand
// Tracking sample: its URP #includes fail to resolve in a Built-in / HDRP project, and a
// sample import compiles every shader in it (field report on PR #379).
Shader "DisplayXR/Samples/GhostHandURP"
{
	Properties
	{
		[NoScaleOffset] _MainTex("Texture", 2D) = "white" {}
		[HDR]_MainColor("Main Color", Color) = (2.67,2.67,2.67,0.078)

		[MaterialToggle] _useOutline("Use Outline", Float) = 1
		[HDR]_OutlineColor("Outline Color", Color) = (1,1,1,0.35)
		_Outline("Outline width", Range(0,.2)) = .002

		[MaterialToggle] _useLighting("Use Lighting", Float) = 0
		_LightIntensity("Light Intensity", Range(0,1)) = 0.5

		[MaterialToggle] _useFresnel("Use Fresnel", Float) = 1
		[HDR]_FresnelColor("Fresnel Color", Color) = (22.4,22.4,22.4,0.098)
		_FresnelPower("Fresnel Power", Range(0,1)) = 0.629
	}

	SubShader
	{
		// Belt and braces: this shader lives in its own sample ("Hand Tracking (URP Ghost
		// Hand)") so Built-in / HDRP projects that import only the main Hand Tracking sample
		// never see it. If it IS imported into a project without URP, this block makes Unity
		// skip the SubShader before compiling it — the URP includes below are never opened,
		// so the Console shows one "no SubShader meets the package requirements" warning
		// instead of a Core.hlsl include error. Must be the SubShader's first declaration.
		PackageRequirements { "com.unity.render-pipelines.universal" }
		Tags { "RenderPipeline" = "UniversalPipeline" "Queue" = "Transparent" "RenderType" = "Transparent" "IgnoreProjector" = "True" }
		ZWrite On
		Blend SrcAlpha OneMinusSrcAlpha

		HLSLINCLUDE
		#include "Packages/com.unity.render-pipelines.universal/ShaderLibrary/Core.hlsl"
		#include "Packages/com.unity.render-pipelines.universal/ShaderLibrary/Lighting.hlsl"

		CBUFFER_START(UnityPerMaterial)
			float4 _MainColor;
			float  _Outline;
			float4 _OutlineColor;
			float4 _FresnelColor;
			float  _FresnelPower;
			float  _LightIntensity;
			float  _useLighting;
			float  _useFresnel;
			float  _useOutline;
		CBUFFER_END

		TEXTURE2D(_MainTex);
		SAMPLER(sampler_MainTex);

		struct Attributes
		{
			float4 positionOS : POSITION;
			float3 normalOS   : NORMAL;
			float2 uv         : TEXCOORD0;
			UNITY_VERTEX_INPUT_INSTANCE_ID
		};

		struct Varyings
		{
			float4 positionCS : SV_POSITION;
			float2 uv         : TEXCOORD0;
			float3 normalWS   : TEXCOORD1;
			float3 positionWS : TEXCOORD2;
			UNITY_VERTEX_INPUT_INSTANCE_ID
			UNITY_VERTEX_OUTPUT_STEREO
		};

		Varyings VertCommon(Attributes v, float outline)
		{
			Varyings o = (Varyings)0;
			UNITY_SETUP_INSTANCE_ID(v);
			UNITY_TRANSFER_INSTANCE_ID(v, o);
			UNITY_INITIALIZE_VERTEX_OUTPUT_STEREO(o);

			o.positionWS = TransformObjectToWorld(v.positionOS.xyz);
			o.normalWS   = TransformObjectToWorldNormal(v.normalOS);
			o.positionCS = TransformWorldToHClip(o.positionWS);
			o.uv         = v.uv;
			if (outline > 0.0)
			{
				// Same construction as the Built-in version: view-space normal projected
				// into clip space, scaled by the outline width.
				float3 normalVS = TransformWorldToViewDir(o.normalWS);
				float2 offset   = mul((float2x2)UNITY_MATRIX_P, normalVS.xy);
				o.positionCS.xy += offset * outline;
			}
			return o;
		}

		float FresnelEffect(float3 normalWS, float3 viewDirWS, float power)
		{
			return pow(1.0 - saturate(dot(normalize(normalWS), normalize(viewDirWS))), power);
		}
		ENDHLSL

		// 1. depth prime
		Pass
		{
			Name "DepthPrime"
			Cull Back
			ColorMask 0
			ZWrite On

			HLSLPROGRAM
			#pragma vertex   VertPrime
			#pragma fragment FragPrime
			#pragma multi_compile_instancing

			Varyings VertPrime(Attributes v) { return VertCommon(v, 0.0); }
			half4 FragPrime(Varyings i) : SV_Target
			{
				UNITY_SETUP_STEREO_EYE_INDEX_POST_VERTEX(i);
				return 0;
			}
			ENDHLSL
		}

		// 2. outline
		Pass
		{
			Name "Outline"
			Cull Front
			ZWrite On

			HLSLPROGRAM
			#pragma vertex   VertOutline
			#pragma fragment FragOutline
			#pragma multi_compile_instancing

			Varyings VertOutline(Attributes v) { return VertCommon(v, _useOutline > 0.5 ? _Outline : 0.0); }
			half4 FragOutline(Varyings i) : SV_Target
			{
				UNITY_SETUP_STEREO_EYE_INDEX_POST_VERTEX(i);
				return SAMPLE_TEXTURE2D(_MainTex, sampler_MainTex, i.uv) * _OutlineColor;
			}
			ENDHLSL
		}

		// 3. body
		Pass
		{
			Name "Body"
			Cull Back
			ZWrite On

			HLSLPROGRAM
			#pragma vertex   VertBody
			#pragma fragment FragBody
			#pragma multi_compile_instancing

			Varyings VertBody(Attributes v) { return VertCommon(v, 0.0); }
			half4 FragBody(Varyings i) : SV_Target
			{
				UNITY_SETUP_STEREO_EYE_INDEX_POST_VERTEX(i);
				half4 col = SAMPLE_TEXTURE2D(_MainTex, sampler_MainTex, i.uv) * _MainColor;
				float3 n = normalize(i.normalWS);
				if (_useLighting > 0.5)
				{
					Light mainLight = GetMainLight();
					half  nl   = saturate(dot(n, mainLight.direction));
					half3 diff = nl * mainLight.color + SampleSH(n);
					col.rgb *= diff * _LightIntensity;
				}
				if (_useFresnel > 0.5)
				{
					float3 viewDirWS = GetWorldSpaceNormalizeViewDir(i.positionWS);
					col.rgb *= _FresnelColor.rgb * FresnelEffect(n, viewDirWS, _FresnelPower) * _FresnelColor.a;
				}
				return col;
			}
			ENDHLSL
		}
	}
	Fallback Off
}
