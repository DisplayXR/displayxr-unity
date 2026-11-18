// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for DisplayXRStereoMediaMath - the UV / fit / layout math behind
// DisplayXRStereoMedia. Hand-rolled (no test framework), like the other Tests~ projects.
// Exit code 0 = all passed.

using System;
using DisplayXR;
using M = DisplayXR.DisplayXRStereoMediaMath;

static class Program
{
    static int s_pass, s_fail;

    static void Check(bool ok, string what)
    {
        if (ok) s_pass++;
        else { s_fail++; Console.WriteLine("FAIL " + what); }
    }

    static bool Near(float a, float b, float eps = 1e-5f) => Math.Abs(a - b) <= eps;

    static bool RectIs(M.UVRect r, float x, float y, float w, float h) =>
        Near(r.x, x) && Near(r.y, y) && Near(r.width, w) && Near(r.height, h);

    // What the shader does: quad UV -> texture UV (before the clamp).
    static void Map(M.EyeMapping m, float u, float v, out float tu, out float tv)
    {
        tu = u * m.scaleX + m.offsetX;
        tv = v * m.scaleY + m.offsetY;
    }

    // The texture-space rectangle an eye covers: (uMin, vMin, uMax, vMax) plus whether the
    // quad's top edge samples the larger v.
    static void Covered(M.EyeMapping m, out float u0, out float v0, out float u1, out float v1, out bool topIsHighV)
    {
        Map(m, 0f, 0f, out float au, out float av);
        Map(m, 1f, 1f, out float bu, out float bv);
        u0 = Math.Min(au, bu); u1 = Math.Max(au, bu);
        v0 = Math.Min(av, bv); v1 = Math.Max(av, bv);
        topIsHighV = bv > av;
    }

    static bool Covers(M.EyeMapping m, float u0, float v0, float u1, float v1)
    {
        Covered(m, out float a, out float b, out float c, out float d, out _);
        return Near(a, u0) && Near(b, v0) && Near(c, u1) && Near(d, v1);
    }

    static M.Solution Plain(DisplayXRStereoLayout layout, bool flip = false, bool stereo = true) =>
        M.Solve(3840, 1080, layout, false, 1f, DisplayXRStereoFit.Stretch, 0f, 0f, flip, stereo);

    static void TestEyeRects()
    {
        // Image space: v = 1 is the TOP of the picture.
        Check(RectIs(M.EyeRect(DisplayXRStereoLayout.SideBySide, false), 0f, 0f, 0.5f, 1f), "SBS left eye = left half");
        Check(RectIs(M.EyeRect(DisplayXRStereoLayout.SideBySide, true), 0.5f, 0f, 0.5f, 1f), "SBS right eye = right half");
        Check(RectIs(M.EyeRect(DisplayXRStereoLayout.SideBySideSwapped, false), 0.5f, 0f, 0.5f, 1f), "SBS swapped left eye = right half");
        Check(RectIs(M.EyeRect(DisplayXRStereoLayout.SideBySideSwapped, true), 0f, 0f, 0.5f, 1f), "SBS swapped right eye = left half");
        Check(RectIs(M.EyeRect(DisplayXRStereoLayout.TopBottom, false), 0f, 0.5f, 1f, 0.5f), "TB left eye = top half");
        Check(RectIs(M.EyeRect(DisplayXRStereoLayout.TopBottom, true), 0f, 0f, 1f, 0.5f), "TB right eye = bottom half");
        Check(RectIs(M.EyeRect(DisplayXRStereoLayout.TopBottomSwapped, false), 0f, 0f, 1f, 0.5f), "TB swapped left eye = bottom half");
        Check(RectIs(M.EyeRect(DisplayXRStereoLayout.TopBottomSwapped, true), 0f, 0.5f, 1f, 0.5f), "TB swapped right eye = top half");
        Check(RectIs(M.EyeRect(DisplayXRStereoLayout.Mono, false), 0f, 0f, 1f, 1f), "Mono left eye = whole picture");
        Check(RectIs(M.EyeRect(DisplayXRStereoLayout.Mono, true), 0f, 0f, 1f, 1f), "Mono right eye = whole picture");

        // The two eyes of every stereo layout tile the picture exactly once.
        foreach (DisplayXRStereoLayout l in Enum.GetValues(typeof(DisplayXRStereoLayout)))
        {
            if (l == DisplayXRStereoLayout.Mono) continue;
            var a = M.EyeRect(l, false);
            var b = M.EyeRect(l, true);
            Check(Near(a.width * a.height + b.width * b.height, 1f), l + ": the two eye rects cover the whole picture");
            Check(!(Near(a.x, b.x) && Near(a.y, b.y)), l + ": the two eye rects differ");
        }
    }

    static void TestMappingPerLayout()
    {
        var s = Plain(DisplayXRStereoLayout.SideBySide);
        Check(Covers(s.left, 0f, 0f, 0.5f, 1f), "SBS: left eye samples u 0..0.5");
        Check(Covers(s.right, 0.5f, 0f, 1f, 1f), "SBS: right eye samples u 0.5..1");
        Check(Near(s.frameX, 1f) && Near(s.frameY, 1f), "Stretch draws the whole frame");

        s = Plain(DisplayXRStereoLayout.SideBySideSwapped);
        Check(Covers(s.left, 0.5f, 0f, 1f, 1f), "SBS swapped: left eye samples u 0.5..1");
        Check(Covers(s.right, 0f, 0f, 0.5f, 1f), "SBS swapped: right eye samples u 0..0.5");

        s = Plain(DisplayXRStereoLayout.TopBottom);
        Check(Covers(s.left, 0f, 0.5f, 1f, 1f), "TB: left eye samples v 0.5..1 (top)");
        Check(Covers(s.right, 0f, 0f, 1f, 0.5f), "TB: right eye samples v 0..0.5 (bottom)");

        s = Plain(DisplayXRStereoLayout.TopBottomSwapped);
        Check(Covers(s.left, 0f, 0f, 1f, 0.5f), "TB swapped: left eye samples v 0..0.5");
        Check(Covers(s.right, 0f, 0.5f, 1f, 1f), "TB swapped: right eye samples v 0.5..1");

        s = Plain(DisplayXRStereoLayout.Mono);
        Check(Covers(s.left, 0f, 0f, 1f, 1f) && Covers(s.right, 0f, 0f, 1f, 1f), "Mono: both eyes sample everything");

        // Orientation: the quad's bottom-left is the picture's bottom-left.
        s = Plain(DisplayXRStereoLayout.SideBySide);
        Map(s.right, 0f, 0f, out float u, out float v);
        Check(Near(u, 0.5f) && Near(v, 0f), "SBS right eye: quad (0,0) -> texture (0.5, 0)");
        Map(s.right, 1f, 1f, out u, out v);
        Check(Near(u, 1f) && Near(v, 1f), "SBS right eye: quad (1,1) -> texture (1, 1)");
        Covered(s.left, out _, out _, out _, out _, out bool up);
        Check(up, "unflipped: the quad's top samples the larger v");
    }

    static void TestMonoFallback()
    {
        foreach (DisplayXRStereoLayout l in Enum.GetValues(typeof(DisplayXRStereoLayout)))
        {
            var st = Plain(l);
            var mono = Plain(l, stereo: false);
            Covered(st.left, out float a, out float b, out float c, out float d, out _);
            Check(Covers(mono.left, a, b, c, d), l + ": mono fallback keeps the left image for the left eye");
            Check(Covers(mono.right, a, b, c, d), l + ": mono fallback gives the right eye the LEFT image");
            Check(Near(mono.right.clampMinX, st.left.clampMinX) && Near(mono.right.clampMaxX, st.left.clampMaxX),
                  l + ": mono fallback clamps the right eye to the left image");
        }
        // A depth shift does nothing in the mono fallback (it would only crop).
        var shifted = M.Solve(3840, 1080, DisplayXRStereoLayout.SideBySide, false, 1f, DisplayXRStereoFit.Stretch, 0f, 0.1f, false, false);
        Check(Covers(shifted.left, 0f, 0f, 0.5f, 1f) && Covers(shifted.right, 0f, 0f, 0.5f, 1f), "mono fallback ignores the depth shift");
    }

    static void TestFlippedV()
    {
        // A texture stored upside down: the picture's top half is at v 0..0.5 in texture space.
        var s = Plain(DisplayXRStereoLayout.TopBottom, flip: true);
        Check(Covers(s.left, 0f, 0f, 1f, 0.5f), "TB flipped: left eye (picture top) samples texture v 0..0.5");
        Check(Covers(s.right, 0f, 0.5f, 1f, 1f), "TB flipped: right eye (picture bottom) samples texture v 0.5..1");
        Covered(s.left, out _, out _, out _, out _, out bool up);
        Check(!up, "TB flipped: the quad's top samples the smaller v (picture comes out upright)");
        Map(s.left, 0f, 1f, out _, out float vTop);
        Map(s.left, 0f, 0f, out _, out float vBottom);
        Check(Near(vTop, 0f) && Near(vBottom, 0.5f), "TB flipped: left eye top row = texture v 0, bottom row = v 0.5");
        Check(Near(s.left.clampMinY, 0f) && Near(s.left.clampMaxY, 0.5f), "TB flipped: left clamp follows the flipped half");
        Check(Near(s.right.clampMinY, 0.5f) && Near(s.right.clampMaxY, 1f), "TB flipped: right clamp follows the flipped half");

        s = Plain(DisplayXRStereoLayout.TopBottomSwapped, flip: true);
        Check(Covers(s.left, 0f, 0.5f, 1f, 1f), "TB swapped flipped: left eye (picture bottom) samples texture v 0.5..1");
        Check(Covers(s.right, 0f, 0f, 1f, 0.5f), "TB swapped flipped: right eye samples texture v 0..0.5");

        // Side by side: the halves do not move, only the rows reverse.
        s = Plain(DisplayXRStereoLayout.SideBySide, flip: true);
        Check(Covers(s.left, 0f, 0f, 0.5f, 1f) && Covers(s.right, 0.5f, 0f, 1f, 1f), "SBS flipped: halves unchanged");
        Map(s.left, 0f, 1f, out _, out vTop);
        Check(Near(vTop, 0f), "SBS flipped: quad top samples texture v 0");

        // Flipping twice (a flipped texture the shader flips again) is the identity:
        // v'' = 1 - (v * -h + (1 - y)) = v * h + y.
        var plain = Plain(DisplayXRStereoLayout.TopBottom);
        var flipped = Plain(DisplayXRStereoLayout.TopBottom, flip: true);
        Map(flipped.left, 0.3f, 0.7f, out _, out float fv);
        Map(plain.left, 0.3f, 0.7f, out _, out float pv);
        Check(Near(1f - fv, pv), "flip is exactly v -> 1 - v");
    }

    static void TestAspect()
    {
        Check(Near(M.EyeAspect(3840, 1080, DisplayXRStereoLayout.SideBySide, false, 1f), 16f / 9f), "full-width SBS 3840x1080 -> 16:9 per eye");
        Check(Near(M.EyeAspect(1920, 1080, DisplayXRStereoLayout.SideBySide, true, 1f), 16f / 9f), "half-width SBS 1920x1080 -> 16:9 per eye");
        Check(Near(M.EyeAspect(1920, 1080, DisplayXRStereoLayout.SideBySide, false, 1f), 8f / 9f), "1920x1080 SBS taken as full width -> 8:9 per eye");
        Check(Near(M.EyeAspect(1920, 1080, DisplayXRStereoLayout.SideBySideSwapped, true, 1f), 16f / 9f), "half-width applies to swapped SBS too");
        Check(Near(M.EyeAspect(1920, 2160, DisplayXRStereoLayout.TopBottom, false, 1f), 16f / 9f), "full-height TB 1920x2160 -> 16:9 per eye");
        Check(Near(M.EyeAspect(1920, 1080, DisplayXRStereoLayout.TopBottom, true, 1f), 16f / 9f), "half-height TB 1920x1080 -> 16:9 per eye");
        Check(Near(M.EyeAspect(1920, 1080, DisplayXRStereoLayout.TopBottomSwapped, false, 1f), 32f / 9f), "1920x1080 TB taken as full height -> 32:9 per eye");
        Check(Near(M.EyeAspect(1920, 1080, DisplayXRStereoLayout.Mono, false, 1f), 16f / 9f), "mono 1920x1080 -> 16:9");
        Check(Near(M.EyeAspect(1920, 1080, DisplayXRStereoLayout.Mono, true, 1f), 16f / 9f), "half-resolution is meaningless for mono");
        // Non-square pixels: 1440x1080 with 4:3 pixels is a 16:9 picture.
        Check(Near(M.EyeAspect(1440, 1080, DisplayXRStereoLayout.Mono, false, 4f / 3f), 16f / 9f), "pixel aspect 4:3 widens 1440x1080 to 16:9");
        Check(Near(M.EyeAspect(1920, 1080, DisplayXRStereoLayout.Mono, false, 0f), 16f / 9f), "pixel aspect 0 = square pixels");
        Check(M.EyeAspect(0, 1080, DisplayXRStereoLayout.Mono, false, 1f) == 0f, "unknown size -> aspect 0");
        Check(Near(Plain(DisplayXRStereoLayout.SideBySide).pictureAspect, 16f / 9f), "Solve reports the eye aspect");
    }

    static void TestFit()
    {
        const DisplayXRStereoLayout sbs = DisplayXRStereoLayout.SideBySide;

        // 16:9 picture in a 16:9 frame: every mode draws everything.
        foreach (DisplayXRStereoFit f in Enum.GetValues(typeof(DisplayXRStereoFit)))
        {
            var e = M.Solve(3840, 1080, sbs, false, 1f, f, 16f / 9f, 0f, false, true);
            Check(Near(e.frameX, 1f) && Near(e.frameY, 1f) && Covers(e.left, 0f, 0f, 0.5f, 1f), f + ": matching aspect is the identity");
        }

        // 16:9 picture in a 16:10 frame (narrower): Fit letterboxes, Fill crops the sides.
        float frame = 16f / 10f;
        var s = M.Solve(3840, 1080, sbs, false, 1f, DisplayXRStereoFit.Fit, frame, 0f, false, true);
        Check(Near(s.frameX, 1f) && Near(s.frameY, 0.9f), "Fit 16:9 in 16:10: full width, 90% height");
        Check(Covers(s.left, 0f, 0f, 0.5f, 1f) && Covers(s.right, 0.5f, 0f, 1f, 1f), "Fit shows the whole eye image");
        Check(Near(s.frameX * frame / s.frameY, 16f / 9f), "Fit: the drawn rectangle has the picture's aspect");

        s = M.Solve(3840, 1080, sbs, false, 1f, DisplayXRStereoFit.Fill, frame, 0f, false, true);
        Check(Near(s.frameX, 1f) && Near(s.frameY, 1f), "Fill draws the whole frame");
        // Visible fraction 0.9 of each half, centred: left 0.025..0.475, right 0.525..0.975.
        Check(Covers(s.left, 0.025f, 0f, 0.475f, 1f), "Fill 16:9 in 16:10: left eye crops 5% each side of its half");
        Check(Covers(s.right, 0.525f, 0f, 0.975f, 1f), "Fill 16:9 in 16:10: right eye crops 5% each side of its half");

        s = M.Solve(3840, 1080, sbs, false, 1f, DisplayXRStereoFit.Stretch, frame, 0f, false, true);
        Check(Near(s.frameX, 1f) && Near(s.frameY, 1f) && Covers(s.left, 0f, 0f, 0.5f, 1f), "Stretch: whole frame, whole image");

        // 4:3 picture in a 16:9 frame (wider): Fit pillarboxes, Fill crops top and bottom.
        frame = 16f / 9f;
        s = M.Solve(2880, 1080, sbs, false, 1f, DisplayXRStereoFit.Fit, frame, 0f, false, true);
        Check(Near(s.frameX, 0.75f) && Near(s.frameY, 1f), "Fit 4:3 in 16:9: 75% width, full height");
        s = M.Solve(2880, 1080, sbs, false, 1f, DisplayXRStereoFit.Fill, frame, 0f, false, true);
        Check(Covers(s.left, 0f, 0.125f, 0.5f, 0.875f), "Fill 4:3 in 16:9: crops 12.5% top and bottom");

        // Fill crop on a top-bottom pair stays inside each eye's half.
        s = M.Solve(1440, 2160, DisplayXRStereoLayout.TopBottom, false, 1f, DisplayXRStereoFit.Fill, frame, 0f, false, true);
        Check(Covers(s.left, 0f, 0.5625f, 1f, 0.9375f), "Fill 4:3 TB in 16:9: left eye crops inside the top half");
        Check(Covers(s.right, 0f, 0.0625f, 1f, 0.4375f), "Fill 4:3 TB in 16:9: right eye crops inside the bottom half");

        // Half-width SBS fits by its DISPLAY aspect, not its stored one.
        s = M.Solve(1920, 1080, sbs, true, 1f, DisplayXRStereoFit.Fit, 16f / 9f, 0f, false, true);
        Check(Near(s.frameX, 1f) && Near(s.frameY, 1f), "half-width SBS 1920x1080 fills a 16:9 frame exactly");
        s = M.Solve(1920, 1080, sbs, false, 1f, DisplayXRStereoFit.Fit, 16f / 9f, 0f, false, true);
        Check(Near(s.frameX, 0.5f) && Near(s.frameY, 1f), "the same texture taken as full width is half as wide");

        // No frame: nothing to fit against.
        s = M.Solve(3840, 1080, sbs, false, 1f, DisplayXRStereoFit.Fill, 0f, 0f, false, true);
        Check(Near(s.frameX, 1f) && Near(s.frameY, 1f) && Covers(s.left, 0f, 0f, 0.5f, 1f), "frame aspect 0: identity");
        // Unknown texture size: identity, and no NaN.
        s = M.Solve(0, 0, sbs, false, 1f, DisplayXRStereoFit.Fit, 16f / 9f, 0f, false, true);
        Check(Near(s.frameX, 1f) && Near(s.frameY, 1f) && Covers(s.left, 0f, 0f, 0.5f, 1f), "unknown texture size: identity");
    }

    // Where a picture point (u in the eye image) lands on the quad for an eye: the inverse of the mapping.
    static float OnQuad(M.EyeMapping m, float eyeStartU, float eyeWidthU, float pictureU)
    {
        float textureU = eyeStartU + eyeWidthU * pictureU;
        return (textureU - m.offsetX) / m.scaleX;
    }

    static void TestDepthShift()
    {
        const DisplayXRStereoLayout sbs = DisplayXRStereoLayout.SideBySide;
        var s = M.Solve(3840, 1080, sbs, false, 1f, DisplayXRStereoFit.Stretch, 0f, 0.1f, false, true);
        // Positive shift: left eye window [0.1, 1] of its image, right eye window [0, 0.9].
        Check(Covers(s.left, 0.05f, 0f, 0.5f, 1f), "shift +10%: left eye samples the right 90% of the left half");
        Check(Covers(s.right, 0.5f, 0f, 0.95f, 1f), "shift +10%: right eye samples the left 90% of the right half");
        float xl = OnQuad(s.left, 0f, 0.5f, 0.5f);
        float xr = OnQuad(s.right, 0.5f, 0.5f, 0.5f);
        Check(xl < xr, "shift > 0: a picture point is further LEFT for the left eye (uncrossed = behind the screen)");
        Check(Near(xr - xl, 0.1f / 0.9f), "shift +10%: the added parallax is 10% of the image width, in cropped-quad units");
        Check(Near(s.pictureAspect, 16f / 9f * 0.9f), "shift narrows the displayed picture by |shift|");

        s = M.Solve(3840, 1080, sbs, false, 1f, DisplayXRStereoFit.Stretch, 0f, -0.1f, false, true);
        Check(Covers(s.left, 0f, 0f, 0.45f, 1f) && Covers(s.right, 0.55f, 0f, 1f, 1f), "shift -10%: the windows swap sides");
        xl = OnQuad(s.left, 0f, 0.5f, 0.5f);
        xr = OnQuad(s.right, 0.5f, 0.5f, 0.5f);
        Check(xl > xr, "shift < 0: crossed = in front of the screen");

        // The shift follows the EYE, not the half the eye is stored in.
        s = M.Solve(3840, 1080, DisplayXRStereoLayout.SideBySideSwapped, false, 1f, DisplayXRStereoFit.Stretch, 0f, 0.1f, false, true);
        Check(Covers(s.left, 0.55f, 0f, 1f, 1f) && Covers(s.right, 0f, 0f, 0.45f, 1f), "shift on swapped SBS follows the eye");
        // Top-bottom shifts horizontally too.
        s = M.Solve(1920, 2160, DisplayXRStereoLayout.TopBottom, false, 1f, DisplayXRStereoFit.Stretch, 0f, 0.1f, false, true);
        Check(Covers(s.left, 0.1f, 0.5f, 1f, 1f) && Covers(s.right, 0f, 0f, 0.9f, 0.5f), "shift on TB is horizontal");

        // Clamped, and never sampling outside the eye's half.
        s = M.Solve(3840, 1080, sbs, false, 1f, DisplayXRStereoFit.Stretch, 0f, 5f, false, true);
        Check(Covers(s.left, 0.125f, 0f, 0.5f, 1f), "shift is clamped to MaxDepthShift");
        for (float sh = -0.3f; sh <= 0.3f; sh += 0.05f)
        {
            s = M.Solve(3840, 1080, sbs, false, 1f, DisplayXRStereoFit.Fill, 1f, sh, false, true);
            Covered(s.left, out float a, out _, out float c, out _, out _);
            Covered(s.right, out float e, out _, out float g, out _, out _);
            Check(a >= -1e-6f && c <= 0.5f + 1e-6f && e >= 0.5f - 1e-6f && g <= 1f + 1e-6f, $"shift {sh:F2}: both windows stay inside their half");
        }

        // Mono has one image: nothing to shift.
        s = M.Solve(1920, 1080, DisplayXRStereoLayout.Mono, false, 1f, DisplayXRStereoFit.Stretch, 0f, 0.1f, false, true);
        Check(Covers(s.left, 0f, 0f, 1f, 1f) && Covers(s.right, 0f, 0f, 1f, 1f), "Mono ignores the depth shift");
    }

    static void TestClamp()
    {
        // The clamp rectangle is the eye's own half, edge to edge; the shader insets it by
        // SeamInsetTexels for the mip level it samples.
        var s = Plain(DisplayXRStereoLayout.SideBySide);
        Check(Near(s.left.clampMinX, 0f, 1e-7f) && Near(s.left.clampMaxX, 0.5f, 1e-7f), "SBS left clamp = left half");
        Check(Near(s.right.clampMinX, 0.5f, 1e-7f) && Near(s.right.clampMaxX, 1f, 1e-7f), "SBS right clamp = right half");
        Check(Near(s.left.clampMinY, 0f, 1e-7f) && Near(s.left.clampMaxY, 1f, 1e-7f), "SBS clamp covers the full height");

        s = Plain(DisplayXRStereoLayout.TopBottom);
        Check(Near(s.left.clampMinY, 0.5f, 1e-7f) && Near(s.left.clampMaxY, 1f, 1e-7f), "TB left clamp = top half");
        Check(Near(s.right.clampMinY, 0f, 1e-7f) && Near(s.right.clampMaxY, 0.5f, 1e-7f), "TB right clamp = bottom half");

        // A crop (Fill, depth shift) does not shrink the clamp: it only has to keep the eye in its half.
        s = M.Solve(3840, 1080, DisplayXRStereoLayout.SideBySide, false, 1f, DisplayXRStereoFit.Fill, 1f, 0.1f, false, true);
        Check(Near(s.left.clampMinX, 0f, 1e-7f) && Near(s.left.clampMaxX, 0.5f, 1e-7f), "clamp is the eye's half, not the visible window");

        // The clamp does not depend on the stated size (which may be the source size of a
        // texture resized on import): only the aspect does.
        var a = M.Solve(3840, 1080, DisplayXRStereoLayout.SideBySide, false, 1f, DisplayXRStereoFit.Fit, 2f, 0f, false, true);
        var b = M.Solve(4096, 1024, DisplayXRStereoLayout.SideBySide, false, 1f, DisplayXRStereoFit.Fit, 2f, 0f, false, true);
        Check(Near(a.left.clampMaxX, b.left.clampMaxX) && Near(a.right.clampMinX, b.right.clampMinX), "clamp is independent of the stated size");
        Check(Near(a.pictureAspect, 16f / 9f) && Near(b.pictureAspect, 2f), "3840x1080 resized to 4096x1024 on import: aspect 1.78 becomes 2.0 unless the source size is stated");
    }

    // Linear (bilinear) filtering at mip level n, one dimension. Position and inset are in
    // texels of mip 0, measured from the seam into the eye's own half. A texel of mip n is
    // 2^n wide; the texel just across the seam has its centre at -2^n / 2. Returns the
    // weight that texel gets - what leaks in from the other eye.
    static float OtherEyeWeight(float distanceFromSeam, int mip)
    {
        float size = (float)Math.Pow(2, mip);
        float w = 1f - (distanceFromSeam + 0.5f * size) / size;
        return w > 0f ? w : 0f;
    }

    static void TestSeamInset()
    {
        for (int mip = 0; mip <= 3; mip++)
        {
            float inset = M.SeamInsetTexels(mip);
            Check(Near(inset, 0.5f * (float)Math.Pow(2, mip)), $"mip {mip}: inset = half a texel of that mip ({inset} base texels)");
            Check(OtherEyeWeight(inset, mip) == 0f, $"mip {mip}: at the inset the other eye's texel has zero weight");
            // What a constant half base texel leaves: (2^n - 1) / 2^(n+1).
            float leak = OtherEyeWeight(0.5f, mip);
            float expected = ((float)Math.Pow(2, mip) - 1f) / (float)Math.Pow(2, mip + 1);
            Check(Near(leak, expected), $"mip {mip}: a fixed half-texel inset leaks {expected:F4} of the other eye (got {leak:F4})");
            Check(mip == 0 ? leak == 0f : leak > 0f, $"mip {mip}: the fixed inset is only enough at mip 0");
        }
        // Trilinear blends the two neighbouring mips: the coarser one decides.
        Check(Near(M.SeamInsetTexels(1.3f), 2f), "mip 1.3 rounds up to mip 2");
        Check(Near(M.SeamInsetTexels(2f), 2f), "an exact level is not rounded further");
        Check(Near(M.SeamInsetTexels(0.01f), 1f), "any minification takes mip 1's inset");
        Check(Near(M.SeamInsetTexels(-2f), 0.5f) && Near(M.SeamInsetTexels(0f), 0.5f), "magnification: half a base texel");
        Check(Near(M.SeamInsetTexels(float.NaN), 0.5f), "NaN level: half a base texel");
        for (float l = 0f; l <= 3f; l += 0.25f)
        {
            float inset = M.SeamInsetTexels(l);
            Check(OtherEyeWeight(inset, (int)Math.Floor(l)) == 0f && OtherEyeWeight(inset, (int)Math.Ceiling(l)) == 0f,
                  $"level {l:F2}: no leak in either mip trilinear filtering blends");
        }
    }

    static void TestColor()
    {
        const DisplayXRStereoEncoding srgb = DisplayXRStereoEncoding.SRGB, lin = DisplayXRStereoEncoding.Linear;
        Check(M.ColorTransform(true, true, srgb) == M.ColorNone, "Linear project, sRGB format, encoded media: the sampler decodes");
        Check(M.ColorTransform(true, false, srgb) == M.ColorDecodeSRGB, "Linear project, UNORM format, encoded media: the shader decodes");
        Check(M.ColorTransform(false, true, srgb) == M.ColorNone, "Gamma project, encoded media: pass through");
        Check(M.ColorTransform(false, false, srgb) == M.ColorNone, "Gamma project, UNORM, encoded media: pass through");
        Check(M.ColorTransform(true, false, lin) == M.ColorNone, "Linear project, UNORM, linear data: pass through");
        Check(M.ColorTransform(true, true, lin) == M.ColorEncodeSRGB, "Linear project, sRGB format, linear data: undo the sampler's decode");
        Check(M.ColorTransform(false, false, lin) == M.ColorEncodeSRGB, "Gamma project, linear data: the shader encodes");
        Check(M.ColorTransform(false, true, lin) == M.ColorEncodeSRGB, "Gamma project, sRGB format, linear data: the shader encodes");
    }

    static void TestDiagnose()
    {
        const DisplayXRStereoLayoutHint ok = DisplayXRStereoLayoutHint.Ok;
        Check(M.DiagnoseLayout(3840, 1080, DisplayXRStereoLayout.SideBySide, false) == ok, "3840x1080 full-width SBS: ok");
        Check(M.DiagnoseLayout(1920, 1080, DisplayXRStereoLayout.SideBySide, true) == ok, "1920x1080 half-width SBS: ok");
        Check(M.DiagnoseLayout(1920, 1080, DisplayXRStereoLayout.SideBySide, false) == DisplayXRStereoLayoutHint.LooksHalfResolution, "1920x1080 SBS as full width: looks half-width");
        Check(M.DiagnoseLayout(3840, 1080, DisplayXRStereoLayout.SideBySideSwapped, true) == DisplayXRStereoLayoutHint.LooksFullResolution, "3840x1080 SBS as half width: looks full-width");
        Check(M.DiagnoseLayout(2880, 1080, DisplayXRStereoLayout.SideBySide, false) == ok, "2880x1080 (two 4:3 images): ok");
        Check(M.DiagnoseLayout(1920, 2160, DisplayXRStereoLayout.TopBottom, false) == ok, "1920x2160 full-height TB: ok");
        Check(M.DiagnoseLayout(1920, 1080, DisplayXRStereoLayout.TopBottom, true) == ok, "1920x1080 half-height TB: ok");
        Check(M.DiagnoseLayout(1920, 1080, DisplayXRStereoLayout.TopBottom, false) == DisplayXRStereoLayoutHint.LooksHalfResolution, "1920x1080 TB as full height: looks half-height");
        Check(M.DiagnoseLayout(1920, 2160, DisplayXRStereoLayout.TopBottomSwapped, true) == DisplayXRStereoLayoutHint.LooksFullResolution, "1920x2160 TB as half height: looks full-height");
        Check(M.DiagnoseLayout(1920, 1080, DisplayXRStereoLayout.Mono, false) == ok, "1920x1080 mono: ok");
        Check(M.DiagnoseLayout(3840, 1080, DisplayXRStereoLayout.Mono, false) == DisplayXRStereoLayoutHint.LooksSideBySide, "3840x1080 mono: looks side-by-side");
        Check(M.DiagnoseLayout(1920, 2160, DisplayXRStereoLayout.Mono, false) == DisplayXRStereoLayoutHint.LooksTopBottom, "1920x2160 mono: looks top-bottom");
        Check(M.DiagnoseLayout(1080, 1920, DisplayXRStereoLayout.Mono, false) == ok, "1080x1920 portrait mono: ok");
        Check(M.DiagnoseLayout(0, 0, DisplayXRStereoLayout.SideBySide, false) == ok, "unknown size: no hint");
    }

    static void TestFrustumPlane()
    {
        // 90 degree vertical FOV at 0.5 m: 1 m tall.
        M.FrustumPlaneSize(1f, 0.5f, 16f / 9f, out float w, out float h);
        Check(Near(h, 1f) && Near(w, 16f / 9f), "frustum plane: 90 deg at 0.5 m is 1 m tall, aspect wide");
        M.FrustumPlaneSize((float)Math.Tan(30.0 * Math.PI / 180.0), 2f, 1f, out w, out h);
        Check(Near(h, 2.3094f, 1e-4f) && Near(w, h), "frustum plane: 60 deg at 2 m is 2.309 m tall");
    }

    static int Main()
    {
        TestEyeRects();
        TestMappingPerLayout();
        TestMonoFallback();
        TestFlippedV();
        TestAspect();
        TestFit();
        TestDepthShift();
        TestClamp();
        TestSeamInset();
        TestColor();
        TestDiagnose();
        TestFrustumPlane();

        Console.WriteLine($"StereoMediaMath: {s_pass} passed, {s_fail} failed");
        return s_fail == 0 ? 0 : 1;
    }
}
