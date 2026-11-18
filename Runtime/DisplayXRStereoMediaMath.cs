// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

using System;

namespace DisplayXR
{
    /// <summary>How the two eye images are packed into one stereo picture.</summary>
    public enum DisplayXRStereoLayout
    {
        /// <summary>Left eye in the left half, right eye in the right half.</summary>
        SideBySide = 0,
        /// <summary>Right eye in the left half, left eye in the right half.</summary>
        SideBySideSwapped = 1,
        /// <summary>Left eye in the top half, right eye in the bottom half.</summary>
        TopBottom = 2,
        /// <summary>Right eye in the top half, left eye in the bottom half.</summary>
        TopBottomSwapped = 3,
        /// <summary>One picture, shown to both eyes.</summary>
        Mono = 4,
    }

    /// <summary>How a picture is fitted into the frame it is shown in.</summary>
    public enum DisplayXRStereoFit
    {
        /// <summary>Whole picture visible, aspect kept; the frame is not filled on one axis.</summary>
        Fit = 0,
        /// <summary>Frame filled, aspect kept; the picture is cropped on one axis.</summary>
        Fill = 1,
        /// <summary>Frame filled, whole picture visible; the aspect is not kept.</summary>
        Stretch = 2,
    }

    /// <summary>What the texel values of a media texture mean.</summary>
    public enum DisplayXRStereoEncoding
    {
        /// <summary>sRGB-encoded colour, which is what photos and video hold.</summary>
        SRGB = 0,
        /// <summary>Linear light, for example a texture rendered by a Linear project.</summary>
        Linear = 1,
    }

    /// <summary>What a texture's shape suggests about the chosen layout.</summary>
    public enum DisplayXRStereoLayoutHint
    {
        /// <summary>Nothing looks wrong.</summary>
        Ok = 0,
        /// <summary>Each eye image comes out too narrow (side-by-side) or too wide (top-bottom):
        /// the picture is probably packed at half resolution.</summary>
        LooksHalfResolution = 1,
        /// <summary>Half resolution is set, but each eye image comes out too wide (side-by-side)
        /// or too tall (top-bottom): the picture is probably packed at full resolution.</summary>
        LooksFullResolution = 2,
        /// <summary>Layout is Mono, but the texture is wide enough to be a side-by-side pair.</summary>
        LooksSideBySide = 3,
        /// <summary>Layout is Mono, but the texture has the shape of a top-bottom pair.</summary>
        LooksTopBottom = 4,
    }

    /// <summary>
    /// Pure math for <c>DisplayXRStereoMedia</c>: which part of a stereo picture each eye
    /// samples, how the picture is fitted into its frame, and which colour conversion the
    /// shader has to apply. No UnityEngine types on purpose - it is unit-tested outside Unity
    /// (<c>Tests~/StereoMediaMath</c>, run in CI).
    ///
    /// <para>Conventions.</para>
    /// <list type="bullet">
    /// <item><b>Image space</b> is the picture as a viewer sees it: u runs left to right,
    /// v runs bottom to top, both 0..1. "Top half" is v in [0.5, 1].</item>
    /// <item><b>Texture space</b> is what the sampler takes. For a texture that follows
    /// Unity's convention (imported textures, render textures Unity rendered into, a
    /// VideoPlayer's output) it equals image space. For a texture stored upside down
    /// (<c>flipVertical</c>) v is mirrored: v_texture = 1 - v_image.</item>
    /// <item>An <see cref="EyeMapping"/> turns the quad's own UV (0,0 bottom-left to 1,1
    /// top-right of what is drawn) into texture space: uv_texture = uv_quad * scale + offset.</item>
    /// </list>
    /// </summary>
    public static class DisplayXRStereoMediaMath
    {
        /// <summary>A rectangle in image space (origin bottom-left).</summary>
        public struct UVRect
        {
            public float x, y, width, height;

            public UVRect(float x, float y, float width, float height)
            {
                this.x = x; this.y = y; this.width = width; this.height = height;
            }
        }

        /// <summary>
        /// One eye's sampling: uv_texture = uv_quad * scale + offset, then clamped to
        /// [clampMin, clampMax] so bilinear filtering never reads the other eye's half.
        /// </summary>
        public struct EyeMapping
        {
            public float scaleX, scaleY, offsetX, offsetY;
            public float clampMinX, clampMinY, clampMaxX, clampMaxY;
        }

        /// <summary>The result of <see cref="Solve"/>.</summary>
        public struct Solution
        {
            public EyeMapping left, right;
            /// <summary>Fraction of the frame the drawn picture covers, per axis (0..1).
            /// 1,1 except in <see cref="DisplayXRStereoFit.Fit"/>.</summary>
            public float frameX, frameY;
            /// <summary>Displayed width / height of one eye's picture, before fitting.</summary>
            public float pictureAspect;
        }

        /// <summary>Largest depth shift, as a fraction of the eye image width.</summary>
        public const float MaxDepthShift = 0.25f;

        /// <summary>
        /// Which part of the picture belongs to an eye, in image space.
        /// </summary>
        public static UVRect EyeRect(DisplayXRStereoLayout layout, bool rightEye)
        {
            switch (layout)
            {
                case DisplayXRStereoLayout.SideBySide:
                    return rightEye ? new UVRect(0.5f, 0f, 0.5f, 1f) : new UVRect(0f, 0f, 0.5f, 1f);
                case DisplayXRStereoLayout.SideBySideSwapped:
                    return rightEye ? new UVRect(0f, 0f, 0.5f, 1f) : new UVRect(0.5f, 0f, 0.5f, 1f);
                case DisplayXRStereoLayout.TopBottom:
                    return rightEye ? new UVRect(0f, 0f, 1f, 0.5f) : new UVRect(0f, 0.5f, 1f, 0.5f);
                case DisplayXRStereoLayout.TopBottomSwapped:
                    return rightEye ? new UVRect(0f, 0.5f, 1f, 0.5f) : new UVRect(0f, 0f, 1f, 0.5f);
                default:
                    return new UVRect(0f, 0f, 1f, 1f);
            }
        }

        /// <summary>
        /// Displayed width / height of one eye's picture.
        /// <paramref name="halfResolution"/> means each eye image was squeezed to half its
        /// size along the packing axis (half-width side-by-side, half-height top-bottom), so
        /// it is shown at the aspect of the whole texture. <paramref name="pixelAspect"/> is
        /// the width / height of one stored pixel (1 for square pixels).
        /// Returns 0 when the size is unknown.
        /// </summary>
        public static float EyeAspect(int textureWidth, int textureHeight, DisplayXRStereoLayout layout,
                                      bool halfResolution, float pixelAspect)
        {
            if (textureWidth <= 0 || textureHeight <= 0) return 0f;
            float par = pixelAspect > 0f ? pixelAspect : 1f;
            float whole = (float)textureWidth / textureHeight * par;
            if (halfResolution) return whole;
            switch (layout)
            {
                case DisplayXRStereoLayout.SideBySide:
                case DisplayXRStereoLayout.SideBySideSwapped:
                    return whole * 0.5f;
                case DisplayXRStereoLayout.TopBottom:
                case DisplayXRStereoLayout.TopBottomSwapped:
                    return whole * 2f;
                default:
                    return whole;
            }
        }

        /// <summary>
        /// Everything the shader needs for one picture.
        /// </summary>
        /// <param name="frameAspect">Width / height of the frame the picture is fitted into
        /// (the quad, or the view in full-screen mode). 0 or less = no frame: the picture
        /// keeps its own shape (as <see cref="DisplayXRStereoFit.Stretch"/> does).</param>
        /// <param name="depthShift">Fraction of the eye image width, clamped to
        /// +-<see cref="MaxDepthShift"/>. Positive moves the whole picture BEHIND the screen
        /// (left image shifted left, right image shifted right), negative in front. Both eye
        /// images are cropped by the same |depthShift| so no edge is ever sampled outside
        /// its half. Ignored when <paramref name="stereo"/> is false.</param>
        /// <param name="flipVertical">The texture is stored upside down.</param>
        /// <param name="stereo">False = mono fallback: both eyes get the LEFT image.</param>
        public static Solution Solve(int textureWidth, int textureHeight, DisplayXRStereoLayout layout,
                                     bool halfResolution, float pixelAspect, DisplayXRStereoFit fit,
                                     float frameAspect, float depthShift, bool flipVertical, bool stereo)
        {
            bool twoImages = stereo && layout != DisplayXRStereoLayout.Mono;
            float s = twoImages ? Clamp(depthShift, -MaxDepthShift, MaxDepthShift) : 0f;
            float a = Math.Abs(s);

            float eyeAspect = EyeAspect(textureWidth, textureHeight, layout, halfResolution, pixelAspect);
            float picture = eyeAspect * (1f - a);

            // Fit: how much of the frame is drawn, and how much of the picture is visible.
            float frameX = 1f, frameY = 1f, visibleX = 1f, visibleY = 1f;
            if (frameAspect > 0f && picture > 0f)
            {
                if (fit == DisplayXRStereoFit.Fit)
                {
                    if (picture >= frameAspect) frameY = frameAspect / picture;
                    else frameX = picture / frameAspect;
                }
                else if (fit == DisplayXRStereoFit.Fill)
                {
                    if (picture >= frameAspect) visibleX = frameAspect / picture;
                    else visibleY = picture / frameAspect;
                }
            }

            Solution r;
            r.frameX = frameX;
            r.frameY = frameY;
            r.pictureAspect = picture;
            r.left = MapEye(layout, false, s, a, visibleX, visibleY, flipVertical, textureWidth, textureHeight);
            r.right = MapEye(layout, twoImages, s, a, visibleX, visibleY, flipVertical, textureWidth, textureHeight);
            return r;
        }

        static EyeMapping MapEye(DisplayXRStereoLayout layout, bool rightEye, float s, float a,
                                 float visibleX, float visibleY, bool flipVertical,
                                 int textureWidth, int textureHeight)
        {
            UVRect eye = EyeRect(layout, rightEye);

            // Depth shift: a window of width (1 - a) inside the eye image. For s > 0 the left
            // eye's window starts at s and the right eye's at 0, so a picture point lands
            // further left for the left eye than for the right one - uncrossed, behind the
            // screen.
            float start = rightEye ? (a - s) * 0.5f : (a + s) * 0.5f;
            float windowX = eye.x + eye.width * start;
            float windowW = eye.width * (1f - a);

            // Fill crops symmetrically about the centre.
            float x = windowX + windowW * (1f - visibleX) * 0.5f;
            float w = windowW * visibleX;
            float y = eye.y + eye.height * (1f - visibleY) * 0.5f;
            float h = eye.height * visibleY;

            float yLo = eye.y, yHi = eye.y + eye.height;

            EyeMapping m;
            m.scaleX = w;
            m.offsetX = x;
            if (flipVertical)
            {
                // v_texture = 1 - v_image.
                m.scaleY = -h;
                m.offsetY = 1f - y;
                float lo = 1f - yHi;
                yHi = 1f - yLo;
                yLo = lo;
            }
            else
            {
                m.scaleY = h;
                m.offsetY = y;
            }

            // Half a texel inside the eye's own half: the bilinear footprint stays in it.
            float insetX = textureWidth > 0 ? 0.5f / textureWidth : 0f;
            float insetY = textureHeight > 0 ? 0.5f / textureHeight : 0f;
            m.clampMinX = eye.x + insetX;
            m.clampMaxX = eye.x + eye.width - insetX;
            m.clampMinY = yLo + insetY;
            m.clampMaxY = yHi - insetY;
            return m;
        }

        /// <summary>The shader does nothing to the sampled colour.</summary>
        public const int ColorNone = 0;
        /// <summary>The shader decodes sRGB to linear.</summary>
        public const int ColorDecodeSRGB = 1;
        /// <summary>The shader encodes linear to sRGB.</summary>
        public const int ColorEncodeSRGB = 2;

        /// <summary>
        /// The colour conversion the shader must apply so that the picture reaches the
        /// render target in the form the project renders in.
        /// <para>
        /// A Linear project shades in linear light: the sampler has to deliver linear
        /// values. It does that by itself only for a texture whose FORMAT is sRGB
        /// (<paramref name="textureFormatIsSRGB"/>); sRGB-encoded bytes in a non-sRGB format
        /// (a UNORM render texture a video is decoded into, an external texture) arrive
        /// still encoded and are decoded here. A Gamma project shades in encoded values and
        /// the sampler never converts, so encoded media passes through untouched.
        /// </para>
        /// <para>
        /// <see cref="DisplayXRStereoEncoding.Linear"/> is the reverse case: values that are
        /// already linear light. They pass through in a Linear project unless the format is
        /// sRGB (the sampler then "decodes" what was never encoded, which is undone here),
        /// and are encoded for a Gamma project.
        /// </para>
        /// </summary>
        public static int ColorTransform(bool linearProject, bool textureFormatIsSRGB, DisplayXRStereoEncoding encoding)
        {
            if (encoding == DisplayXRStereoEncoding.SRGB)
            {
                if (!linearProject) return ColorNone;
                return textureFormatIsSRGB ? ColorNone : ColorDecodeSRGB;
            }
            if (!linearProject) return ColorEncodeSRGB;
            return textureFormatIsSRGB ? ColorEncodeSRGB : ColorNone;
        }

        /// <summary>
        /// A plausibility check for the inspector: does the texture's shape fit the layout?
        /// It only ever suggests - a 4:3 or portrait source can trip it legitimately.
        /// </summary>
        public static DisplayXRStereoLayoutHint DiagnoseLayout(int textureWidth, int textureHeight,
                                                               DisplayXRStereoLayout layout, bool halfResolution)
        {
            if (textureWidth <= 0 || textureHeight <= 0) return DisplayXRStereoLayoutHint.Ok;
            float whole = (float)textureWidth / textureHeight;
            switch (layout)
            {
                case DisplayXRStereoLayout.SideBySide:
                case DisplayXRStereoLayout.SideBySideSwapped:
                    // 1920x1080 holding two squeezed 16:9 images: each eye 0.89 if taken as full width.
                    if (!halfResolution && whole * 0.5f < 1.0f) return DisplayXRStereoLayoutHint.LooksHalfResolution;
                    // 3840x1080 holding two full 16:9 images: 3.56 if taken as squeezed.
                    if (halfResolution && whole > 2.9f) return DisplayXRStereoLayoutHint.LooksFullResolution;
                    return DisplayXRStereoLayoutHint.Ok;
                case DisplayXRStereoLayout.TopBottom:
                case DisplayXRStereoLayout.TopBottomSwapped:
                    // 1920x1080 holding two squeezed 16:9 images: each eye 3.56 if taken as full height.
                    if (!halfResolution && whole * 2f > 3.0f) return DisplayXRStereoLayoutHint.LooksHalfResolution;
                    // 1920x2160 holding two full 16:9 images: 0.89 if taken as squeezed.
                    if (halfResolution && whole < 1.0f) return DisplayXRStereoLayoutHint.LooksFullResolution;
                    return DisplayXRStereoLayoutHint.Ok;
                default:
                    if (whole >= 3.0f) return DisplayXRStereoLayoutHint.LooksSideBySide;
                    if (whole >= 0.85f && whole <= 0.93f) return DisplayXRStereoLayoutHint.LooksTopBottom;
                    return DisplayXRStereoLayoutHint.Ok;
            }
        }

        /// <summary>
        /// Size of the plane a symmetric camera frustum cuts at <paramref name="distance"/>:
        /// height = 2 * distance * tan(vertical FOV / 2), width = height * aspect. Used to size
        /// a quad on a camera-centric rig's convergence plane.
        /// </summary>
        public static void FrustumPlaneSize(float tanHalfVerticalFov, float distance, float aspect,
                                            out float width, out float height)
        {
            height = 2f * distance * tanHalfVerticalFov;
            width = height * aspect;
        }

        static float Clamp(float v, float lo, float hi) => v < lo ? lo : (v > hi ? hi : v);
    }
}
