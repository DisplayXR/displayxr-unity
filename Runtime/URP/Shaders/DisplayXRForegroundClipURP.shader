// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Per-eye foreground-only clip for URP (opt-in transparent-overlay clip; #57/#129).
//
// WHY THIS EXISTS
// URP builds each eye's projection from views[i].fov + Camera.farClipPlane and
// IGNORES Camera.SetStereoProjectionMatrix — so the plugin's per-eye foreground
// far (which BiRP injects straight into the projection matrix) can't reach URP
// that way. The only projection-level lever is the single Camera.farClipPlane,
// shared by both eyes => wrong for the off-eye, because in display-centric mode
// each eye sees the virtual display at a DIFFERENT depth (its own eye.z after the
// m2v scaling + ipd/parallax modifiers).
//
// This pass enforces the clip per-eye in screen space instead: after the scene
// renders, reconstruct each fragment's view-space eye Z from the depth texture
// and discard (write transparent black) anything farther than this eye's
// foreground far. The two per-eye fars arrive via the _DXRForegroundFar global,
// set each frame by the rig (DisplayXRDisplay/DisplayXRCamera) from the native
// per-view fars (leftProj/rightProj m23/(m22+1) — the exact far_eff Kooima used).
// Each eye uses ITS OWN far, so there is no single-far approximation.
//
// OPT-IN: this clip is for transparent-overlay apps only. Wire it via
// DisplayXR > Setup URP Foreground Clip (creates a material from this shader and
// adds Unity's built-in FullScreenPassRendererFeature to the renderer). A normal
// 3D URP app should NOT add it. The shared off-axis projection fix
// (KooimaProjectionFixFeature) is a separate, always-on feature.
//
// Authored for Unity's built-in FullScreenPassRendererFeature (URP 17 / Unity 6):
// inject BeforeRenderingPostProcessing (URP 17's InjectionPoint enum has no
// AfterRenderingTransparents — this is the first slot after the transparent queue;
// the opaque overlay is fully in depth+color by then), Requirements = Depth. The
// feature binds the camera color to _BlitTexture and Vert/Varyings come from
// URP's Blit.hlsl.
//
// NOTE: this shader URP-#includes Core.hlsl; it must stay OUT of Runtime/Resources/
// (a Resources shader force-compiles in every build and breaks BiRP-only projects
// that have no URP package — the #130 revert). It is only ever compiled when the
// opt-in installer creates a material referencing it inside a URP project.
Shader "DisplayXR/ForegroundClipURP"
{
    SubShader
    {
        Tags { "RenderType" = "Opaque" "RenderPipeline" = "UniversalPipeline" }
        ZWrite Off ZTest Always Cull Off

        Pass
        {
            Name "DisplayXRForegroundClip"

            HLSLPROGRAM
            #pragma vertex Vert
            #pragma fragment Frag

            #include "Packages/com.unity.render-pipelines.universal/ShaderLibrary/Core.hlsl"
            // Vert / Varyings / _BlitTexture / sampler_LinearClamp / SAMPLE_TEXTURE2D_X
            #include "Packages/com.unity.render-pipelines.core/Runtime/Utilities/Blit.hlsl"
            // SampleSceneDepth + _CameraDepthTexture
            #include "Packages/com.unity.render-pipelines.universal/ShaderLibrary/DeclareDepthTexture.hlsl"

            // x = left eye far_eff, y = right eye far_eff, z = enable (0/1), w = unused.
            float4 _DXRForegroundFar;
            // World positions of the two eyes (w unused), published by the rig from its
            // FlipViewZ'd view matrices. Used to pick which eye's far applies in
            // MULTIPASS, where unity_StereoEyeIndex is a compile-time 0 (see below).
            float4 _DXREyePosL;
            float4 _DXREyePosR;

            half4 Frag(Varyings input) : SV_Target
            {
                UNITY_SETUP_STEREO_EYE_INDEX_POST_VERTEX(input);

                half4 col = SAMPLE_TEXTURE2D_X(_BlitTexture, sampler_LinearClamp, input.texcoord);

                // Disabled → pure passthrough (lets the rig toggle the clip live).
                if (_DXRForegroundFar.z < 0.5h)
                    return col;

                // View-space distance from the eye to this fragment, in world units.
                // Background (no geometry) reads the far plane → very large eyeZ →
                // clipped to transparent, which is already what the empty overlay wants.
                // _ZBufferParams is per-eye in multipass, so eyeZ is per-eye-correct.
                float rawDepth = SampleSceneDepth(input.texcoord);
                float eyeZ = LinearEyeDepth(rawDepth, _ZBufferParams);

                // Per-eye far. unity_StereoEyeIndex is NOT usable: in multipass (forced
                // by the Kooima path) URP's TextureXR.hlsl #defines it to a literal 0,
                // so it can never select the right eye. UNITY_MATRIX_I_V *is* per-eye in
                // multipass, so identify the current eye by its world position and pick
                // the nearer of the rig's two published eye positions.
                float3 curEye = mul(UNITY_MATRIX_I_V, float4(0.0, 0.0, 0.0, 1.0)).xyz;
                float dL = dot(curEye - _DXREyePosL.xyz, curEye - _DXREyePosL.xyz);
                float dR = dot(curEye - _DXREyePosR.xyz, curEye - _DXREyePosR.xyz);
                float farEff = (dL <= dR) ? _DXRForegroundFar.x : _DXRForegroundFar.y;

                // Behind the virtual display plane → cut it away (color AND alpha
                // zeroed; alpha-only left the geometry visible — the #129 failure).
                if (eyeZ > farEff)
                    return half4(0.0h, 0.0h, 0.0h, 0.0h);

                return col;
            }
            ENDHLSL
        }
    }
    Fallback Off
}
