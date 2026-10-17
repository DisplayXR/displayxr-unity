// Ghost Hand — Built-in Render Pipeline (CG).
//
// Derived from "Ultraleap/GenericHandShader" (Ultraleap Unity Plugin 5.4.0,
// Copyright (C) Ultraleap, Inc. 2011-2021, Apache License 2.0 — see the sample's
// THIRD-PARTY-NOTICES.md and LICENSE-Ultraleap-Apache-2.0.md). Changes: renamed;
// defaults; the URP counterpart is GhostHandURP.shader. Three passes, in order:
//   1. depth prime — writes depth only (Blend Zero One), so the transparent hand
//      self-occludes instead of showing its back faces through the palm;
//   2. outline — front faces culled, vertices pushed along the view-space normal
//      in clip space by _Outline, tinted _OutlineColor (hairline rim);
//   3. body — texture × _MainColor, optional Lambert+SH lighting, and the HDR
//      fresnel rim (_FresnelColor, power _FresnelPower) that makes the hand glow
//      at grazing angles while the palm stays almost clear.
Shader "DisplayXR/Samples/GhostHand"
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

	CGINCLUDE
	#include "UnityCG.cginc"

	float4 _MainColor;
	float _Outline;
	float4 _OutlineColor;
	sampler2D _MainTex;
	float4 _FresnelColor;
	float _FresnelPower;
	float _LightIntensity;
	float _useLighting;
	float _useFresnel;
	float _useOutline;

	struct v2f
	{
		float2 uv : TEXCOORD0;
		float4 pos : SV_POSITION;
		float3 worldNormal : NORMAL;
		float3 viewDir : TEXCOORD1;
		fixed4 diff : COLOR0;
		UNITY_VERTEX_OUTPUT_STEREO
	};
	ENDCG

	SubShader
	{
		Tags {"Queue" = "Transparent" "IgnoreProjector" = "True" "RenderType" = "Transparent" "LightMode" = "ForwardBase"}
		ZWrite On
		Blend SrcAlpha OneMinusSrcAlpha

		// 1. depth prime
		Pass
		{
			Cull Back
			Blend Zero One
		}

		// 2. outline
		Pass
		{
			Cull Front

			CGPROGRAM
			#pragma vertex vert
			#pragma fragment frag

			v2f vert(appdata_base v)
			{
				v2f o;
				UNITY_SETUP_INSTANCE_ID(v);
				UNITY_INITIALIZE_OUTPUT(v2f, o);
				UNITY_INITIALIZE_VERTEX_OUTPUT_STEREO(o);

				o.pos = UnityObjectToClipPos(v.vertex);
				float3 norm = mul((float3x3)UNITY_MATRIX_IT_MV, v.normal);
				float2 offset = TransformViewToProjection(norm.xy);
				o.uv = v.texcoord;
				o.pos.xy = _useOutline ? o.pos.xy + offset * _Outline : o.pos.xy;
				return o;
			}

			half4 frag(v2f i) : SV_Target
			{
				fixed4 col = tex2D(_MainTex, i.uv) * _OutlineColor;
				return col;
			}
			ENDCG
		}

		// 3. body
		Pass
		{
			CGPROGRAM
			#pragma vertex vert
			#pragma fragment frag
			#pragma multi_compile_fwdbase nolightmap nodirlightmap nodynlightmap novertexlight
			#include "UnityLightingCommon.cginc"

			v2f vert(appdata_base v)
			{
				v2f o;
				UNITY_SETUP_INSTANCE_ID(v);
				UNITY_INITIALIZE_OUTPUT(v2f, o);
				UNITY_INITIALIZE_VERTEX_OUTPUT_STEREO(o);

				o.pos = UnityObjectToClipPos(v.vertex);
				o.uv = v.texcoord;
				half3 worldNormal = UnityObjectToWorldNormal(v.normal);
				half nl = max(0, dot(worldNormal, _WorldSpaceLightPos0.xyz));
				o.diff = nl * _LightColor0;
				o.diff.rgb += ShadeSH9(half4(worldNormal, 1));
				o.worldNormal = worldNormal;
				o.viewDir = WorldSpaceViewDir(v.vertex);
				return o;
			}

			// The fresnel function Shader Graph uses.
			float Unity_FresnelEffect_float(float3 Normal, float3 ViewDir, float Power)
			{
				return pow((1.0 - saturate(dot(normalize(Normal), normalize(ViewDir)))), Power);
			}

			half4 frag(v2f i) : SV_Target
			{
				fixed4 col = tex2D(_MainTex, i.uv);
				col *= _MainColor;
				if (_useLighting)
					col.rgb *= (i.diff * _LightIntensity);
				if (_useFresnel)
					col.rgb *= _FresnelColor.rgb * Unity_FresnelEffect_float(i.worldNormal, i.viewDir, _FresnelPower) * _FresnelColor.a;
				return col;
			}
			ENDCG
		}
	}
	Fallback Off
}
