// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
//
// Per-eye foreground-only clip for the Built-in RP under the custom display
// provider (#166). BiRP analog of DisplayXR/ForegroundClipURP.
//
// WHY THIS EXISTS
// Under the provider Unity fills each eye's projection depth range from a single
// Camera.farClipPlane (shared by both eyes), so a projection-level far can't sit on
// the display plane for both eyes at once — off-axis the L/R clip edges diverge.
// This pass enforces the clip per-eye in screen space instead: reconstruct each
// fragment's view-space eye Z from the depth texture and discard (transparent
// black) anything farther than THIS eye's foreground far. The current eye's far is
// pushed as a scalar (_DXRClipFar) by DisplayXRForegroundClipBiRP before each eye's
// blit — it is the native per-eye display-plane distance (dxr_prov_get_eye_clip),
// which is exactly the far_eff Kooima used. So each eye clips on the plane and no
// per-eye matrix is needed (an OnRenderImage blit clobbers UNITY_MATRIX_I_V, unlike
// URP's FullScreenPass; the eye is selected C#-side via Camera.stereoActiveEye).
//
// Pure Built-in RP (no URP #includes) → safe to keep in Resources (unlike the URP
// shader, the #130 revert): it compiles in every build with no URP package.
Shader "Hidden/DisplayXR/ForegroundClipBiRP"
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
            UNITY_DECLARE_DEPTH_TEXTURE(_CameraDepthTexture);
            float _DXRClipEnable; // 0/1 — live rig toggle
            float _DXRClipFar;    // this eye's foreground far (world units)

            fixed4 frag(v2f_img i) : SV_Target
            {
                fixed4 col = tex2D(_MainTex, i.uv);
                if (_DXRClipEnable < 0.5) return col;

                // Per-eye view-space distance from the eye to this fragment. The depth
                // texture is per-eye (multipass), and _ZBufferParams' near/far are shared
                // by both eyes, so eyeZ is per-eye-correct. Empty background reads the far
                // plane → large eyeZ → cut, which is what the empty overlay wants.
                float rawDepth = SAMPLE_DEPTH_TEXTURE(_CameraDepthTexture, i.uv);
                float eyeZ = LinearEyeDepth(rawDepth);

                // Beyond the virtual display plane → cut it away (color AND alpha zeroed,
                // matching ForegroundClipURP — alpha-only leaves the geometry visible).
                if (eyeZ > _DXRClipFar)
                    return fixed4(0.0, 0.0, 0.0, 0.0);

                return col;
            }
            ENDCG
        }
    }
    Fallback Off
}
