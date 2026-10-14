// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Depth-aware cursor, Depth source: copy a small patch of the scene depth around the
// cursor into a 32x32 RGBAFloat target that C# reads back asynchronously (never waits).
// Each texel stores (raw depth, sample u, sample v, stamp): carrying its own sample UV
// makes the result independent of render-target row order, and the stamp lets C# pair
// a readback with the eye it was rendered from - and reject a patch that was not drawn.
//
// Plain HLSL with NO render-pipeline includes on purpose: this file lives in Resources/
// (so builds include it) and must compile in BiRP, URP and HDRP projects alike - a
// pipeline include here would break the others (see CLAUDE.md, the #130 revert).
Shader "Hidden/DisplayXR/CursorDepthPatch"
{
    SubShader
    {
        Tags { "RenderType" = "Opaque" }
        ZTest Always
        ZWrite Off
        Cull Off
        Blend Off

        HLSLINCLUDE
        float4 _DXRPatchRect;      // x, y, w, h: bottom-left UV over the eye viewport
        float4 _DXRUVScale;        // viewport UV -> texture UV scale (xy)
        float  _DXRStamp;          // frame stamp, echoed into .w
        float  _DXRFlipY;          // diagnostic: sample at 1 - v (texture stored upside down)
        float  _DXRCursorEyeGate;  // BiRP multi-pass: 1 only in the left-eye pass
        float  _DXRSlice;          // array pass: slice (0 = left eye)

        struct v2f
        {
            float4 pos : SV_POSITION;
            float2 uv  : TEXCOORD0;
        };

        // Fullscreen triangle from the vertex id: no mesh, no matrices.
        v2f vert(uint id : SV_VertexID)
        {
            v2f o;
            float2 t = float2((id << 1) & 2, id & 2);
            o.pos = float4(t * 2.0 - 1.0, 0.0, 1.0);
            o.uv = t;
            return o;
        }

        float2 SampleUV(float2 patchUV, out float2 texUV)
        {
            float2 vp = _DXRPatchRect.xy + patchUV * _DXRPatchRect.zw; // viewport UV (image)
            float2 s = vp;
            if (_DXRFlipY > 0.5) s.y = 1.0 - s.y;
            texUV = s * _DXRUVScale.xy;
            return vp;
        }
        ENDHLSL

        // Pass 0: Texture2D source (URP / HDRP non-array, multi-pass).
        Pass
        {
            HLSLPROGRAM
            #pragma vertex vert
            #pragma fragment frag
            #pragma target 3.5
            Texture2D<float> _DXRDepthSrc;
            SamplerState dxr_point_clamp_sampler;
            float4 frag(v2f i) : SV_Target
            {
                float2 tuv;
                float2 vp = SampleUV(i.uv, tuv);
                float raw = _DXRDepthSrc.SampleLevel(dxr_point_clamp_sampler, tuv, 0);
                return float4(raw, vp, _DXRStamp);
            }
            ENDHLSL
        }

        // Pass 1: Texture2DArray source (URP / HDRP single-pass instanced).
        Pass
        {
            HLSLPROGRAM
            #pragma vertex vert
            #pragma fragment frag
            #pragma target 3.5
            #pragma require 2darray
            Texture2DArray<float> _DXRDepthSrcArray;
            SamplerState dxr_point_clamp_sampler;
            float4 frag(v2f i) : SV_Target
            {
                float2 tuv;
                float2 vp = SampleUV(i.uv, tuv);
                float raw = _DXRDepthSrcArray.SampleLevel(dxr_point_clamp_sampler, float3(tuv, _DXRSlice), 0);
                return float4(raw, vp, _DXRStamp);
            }
            ENDHLSL
        }

        // Pass 2: BiRP. Samples the camera's own _CameraDepthTexture global; runs in
        // both multi-pass eye passes, and keeps only the left one (the eye C# unprojects).
        Pass
        {
            HLSLPROGRAM
            #pragma vertex vert
            #pragma fragment frag
            #pragma target 3.5
            Texture2D<float> _CameraDepthTexture;
            SamplerState dxr_point_clamp_sampler;
            float4 frag(v2f i) : SV_Target
            {
                if (_DXRCursorEyeGate < 0.5) discard;
                float2 tuv;
                float2 vp = SampleUV(i.uv, tuv);
                float raw = _CameraDepthTexture.SampleLevel(dxr_point_clamp_sampler, tuv, 0);
                return float4(raw, vp, _DXRStamp);
            }
            ENDHLSL
        }
    }
    Fallback Off
}
