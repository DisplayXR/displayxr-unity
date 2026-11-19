// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for DisplayXRGameViewCalibration - the position-coded marker of the editor Game-view
// calibration and the decoding of 1-px screen strips (#459). Hand-rolled (no test framework).
// Exit code 0 = all passed.
//
// Everything here is SYNTHETIC: a "screen" is built in memory from the marker definition, drawn the
// ways Unity can draw the Game view's texture (texel for pixel at any origin, a column dropped or
// doubled by a rect half a pixel narrower / wider than the texture, filtered, partly covered,
// clipped by the desktop's edge) and through the colour changes a capture can add (dimmed, a lifted
// black level, red and blue exchanged). Whether the real marker reaches the real screen this way is
// a hardware check (docs~/architecture/editor-game-view-placement.md), not this one.

using System;
using Cal = DisplayXR.DisplayXRGameViewCalibration;

static class Program
{
    static int s_pass, s_fail;
    const uint Grey = 0x383838u;   // the Game view's margin colour: not a marker colour

    static void Check(bool ok, string what)
    {
        if (ok) s_pass++;
        else { s_fail++; Console.WriteLine("FAIL " + what); }
    }

    // A horizontal strip of `count` screen pixels starting at screen coordinate `start`, across an
    // image `texW` wide whose texel 0 is at screen coordinate `origin`. texelAt maps (screen pixel
    // relative to the origin) -> texel shown, or -1 for background; null = texel for pixel.
    static uint[] Row(int start, int count, int origin, int texW, int y, Func<int, int> texelAt = null, int drawn = -1)
    {
        var px = new uint[count];
        if (drawn < 0) drawn = texW;
        for (int i = 0; i < count; i++)
        {
            int rel = start + i - origin;
            int t = rel < 0 || rel >= drawn ? -1 : (texelAt != null ? texelAt(rel) : rel);
            px[i] = t < 0 || t >= texW ? Grey : Cal.Rgb(Cal.MarkerIndex(t, y, texW));
        }
        return px;
    }

    // A vertical strip down column `x` of the image (row 0 at screen coordinate `origin`).
    static uint[] Col(int start, int count, int origin, int texW, int texH, int x, Func<int, int> texelAt = null, int drawn = -1)
    {
        var px = new uint[count];
        if (drawn < 0) drawn = texH;
        for (int i = 0; i < count; i++)
        {
            int rel = start + i - origin;
            int t = rel < 0 || rel >= drawn ? -1 : (texelAt != null ? texelAt(rel) : rel);
            px[i] = t < 0 || t >= texH ? Grey : Cal.Rgb(Cal.MarkerIndex(x, t, texW));
        }
        return px;
    }

    static Cal.Axis DecodeRow(uint[] px, int start, int texW)
    {
        Cal.BandStarts(texW, out int b0, out int b1);
        return Cal.DecodeAxis(px, px.Length, start, texW, b0, b1, Cal.BandWidth);
    }

    static Cal.Axis DecodeCol(uint[] px, int start, int texH) =>
        Cal.DecodeAxis(px, px.Length, start, texH, -1, -1, 0);

    // Point-filtered draw of `tex` texels into `drawn` pixels (what Unity does at zoom >= 1 when
    // the rect is not the texture's size): pixel p shows the texel under its centre.
    static Func<int, int> Nearest(int tex, int drawn) => p => (int)Math.Floor((p + 0.5) * tex / drawn);

    static void CodeDefinition()
    {
        bool sync = true, digits = true, unique = true;
        for (int t = 0; t < Cal.MaxTexels; t++)
        {
            int c = Cal.Code(t);
            if ((t % Cal.BlockLen == 0) != (c == Cal.Sync)) sync = false;
            if (c < 0 || c > 5) digits = false;
        }
        // A block's five digits give back its index.
        for (int b = 0; b < Cal.MaxTexels / Cal.BlockLen; b++)
        {
            int v = 0, m = 1;
            for (int d = 1; d < Cal.BlockLen; d++) { v += Cal.Code(b * Cal.BlockLen + d) * m; m *= Cal.DigitBase; }
            if (v != b) unique = false;
        }
        Check(sync, "the sync colour is on every block's first texel and nowhere else");
        Check(digits, "every texel is one of six colours");
        Check(unique, "a block's digits are its index");

        bool bands = true;
        for (int w = Cal.MinTexels; w <= 8192; w++)
        {
            Cal.BandStarts(w, out int b0, out int b1);
            if (b0 % Cal.BlockLen != 0 || b1 % Cal.BlockLen != 0 || b0 < Cal.BlockLen
                || b0 + Cal.BandWidth > b1 || b1 + Cal.BandWidth > w - Cal.BlockLen) bands = false;
        }
        Check(bands, "the two bands sit on block boundaries, apart, inside the texture, for every width");
        Check(Cal.MarkerIndex(10, 77, 1728) == Cal.Code(10), "outside the bands a texel shows its column's code");
        Cal.BandStarts(1728, out int bb0, out int bb1);
        Check(Cal.MarkerIndex(bb0, 77, 1728) == Cal.Code(77) && Cal.MarkerIndex(bb1 + Cal.BandWidth - 1, 77, 1728) == Cal.Code(77)
              && Cal.MarkerIndex(bb0 - 1, 77, 1728) == Cal.Code(bb0 - 1) && Cal.MarkerIndex(bb1 + Cal.BandWidth, 77, 1728) == Cal.Code(bb1 + Cal.BandWidth),
              "inside a band a texel shows its row's code");
    }

    static void Classification()
    {
        for (int i = 0; i < 6; i++)
            Check(Cal.Classify(Cal.Rgb(i)) == i, "pure colour " + i + " classifies as itself");
        Check(Cal.Classify(0x000000u) == -1 && Cal.Classify(0xFFFFFFu) == -1 && Cal.Classify(Grey) == -1
              && Cal.Classify(0x808080u) == -1, "black, white and grey are not marker colours");
        Check(Cal.Classify(0xFF3C28u) == 0, "a red with lifted green / blue is still red");
        Check(Cal.Classify(0x7F0000u) == 0 && Cal.Classify(0x00C8C8u) == 4, "dimmed colours still classify");
        Check(Cal.Classify(0x400000u) == -1, "too dark to trust");
        Check(Cal.Classify(0xFF8000u) == -1, "orange (a half-on channel) is not a marker colour");
        Check(Cal.Classify(0xFF0000u, swapRB: true) == 2 && Cal.Classify(0xFFFF00u, swapRB: true) == 4
              && Cal.Classify(0xFF00FFu, swapRB: true) == 5 && Cal.Classify(0x00FF00u, swapRB: true) == 1,
              "red / blue exchange: red<->blue, yellow<->cyan, magenta and green stay");
        Check(Cal.Classify(0xFFFF0000u) == 0, "the upper byte is ignored");
    }

    static void OneToOne()
    {
        const int m = Cal.StripMarginPx;
        // Texture sizes seen on hardware: 1120x595 (175 %, whole-pixel size), 1728x860 (250 %),
        // 1924 (175 % Free Aspect), plus the smallest and a 4K one.
        foreach (int w in new[] { 128, 595, 860, 1120, 1728, 1924, 3840 })
            foreach (int origin in new[] { 0, 1, 2, 1020, -1913, 5120 })
                foreach (int err in new[] { 0, 1, -1, 7, -30 })   // computed origin - true origin
                {
                    int start = origin + err - m, count = w + 2 * m;
                    var a = DecodeRow(Row(start, count, origin, w, 200), start, w);
                    Check(a.Status == Cal.Status.OneToOne && a.Origin == origin && a.Mismatches == 0 && !a.SwappedRB,
                          $"1:1 row: width {w}, origin {origin}, computed off by {err} -> {a.Status} at {a.Origin}");
                }

        // The measured 175 % case: pane at 1500, the image one pixel in while the glue computed two.
        {
            int pane = 1500, w = 1120, computed = pane + 2, start = computed - m;
            var a = DecodeRow(Row(start, w + 2 * m, pane + 1, w, 198), start, w);
            Check(a.Status == Cal.Status.OneToOne && a.Origin - pane == 1 && a.Origin - computed == -1,
                  "175 %: image at pane +1 against a computed +2 is measured as +1 (delta -1)");
        }

        // Rows: a vertical strip anywhere inside either band, for many widths.
        bool cols = true;
        for (int w = 128; w <= 4000 && cols; w += 37)
        {
            int h = 595 + w % 300;
            Cal.BandStarts(w, out int b0, out int b1);
            for (int k = 0; k < Cal.StripCount; k++)
                foreach (int off in new[] { -11, 0, 11 })
                {
                    int x = Cal.VerticalStripColumn(k, b0, b1, Cal.BandWidth) + off;
                    int origin = 406, start = origin + 3 - m;
                    var a = DecodeCol(Col(start, h + 2 * m, origin, w, h, x), start, h);
                    if (a.Status != Cal.Status.OneToOne || a.Origin != origin) cols = false;
                }
            // ...and a row of the same image on each strip row.
            for (int k = 0; k < Cal.StripCount; k++)
            {
                int start = 77 - m;
                var a = DecodeRow(Row(start, w + 2 * m, 77, w, Cal.HorizontalStripRow(k, h)), start, w);
                if (a.Status != Cal.Status.OneToOne || a.Origin != 77) cols = false;
            }
        }
        Check(cols, "1:1 rows and columns decode for widths 128..4000, strips anywhere in a band");
        Check(Cal.HorizontalStripRow(0, 600) == 200 && Cal.HorizontalStripRow(1, 600) == 400, "strip rows at 1/3 and 2/3");
    }

    static void NotOneToOne()
    {
        const int m = Cal.StripMarginPx;
        foreach (int w in new[] { 880, 1120, 1924 })
        {
            int origin = 1502, start = origin - m, count = w + 2 * m;

            // Rect half a pixel narrower than the texture: one column dropped mid-image (F2).
            var a = DecodeRow(Row(start, count, origin, w, 100, Nearest(w, w - 1), w - 1), start, w);
            Check(a.Status == Cal.Status.NotOneToOne && a.HasOther && Math.Abs(a.OtherOrigin - a.Origin) == 1
                  && (a.Origin == origin || a.Origin == origin - 1),
                  $"a dropped column (width {w}) is measured as not 1:1 with the two halves one pixel apart");
            Check(a.FirstBadTexel > w / 4 && a.FirstBadTexel < 3 * w / 4 || a.Origin == origin - 1,
                  $"...and the break is reported mid-image (texel {a.FirstBadTexel})");

            // Half a pixel wider: one column doubled.
            a = DecodeRow(Row(start, count, origin, w, 100, Nearest(w, w + 1), w + 1), start, w);
            Check(a.Status == Cal.Status.NotOneToOne && (a.HasOther || a.EdgeDoubled),
                  $"a doubled column (width {w}) is measured as not 1:1");

            // The first column missing (the image starts one pixel late and shows texel 1 there).
            a = DecodeRow(Row(start, count, origin, w, 100, p => p == 0 ? -1 : p), start, w);
            Check(a.Status == Cal.Status.NotOneToOne && a.Origin == origin && a.FirstBadTexel == 0,
                  $"a missing first column (width {w}) is not 1:1, origin unchanged");
            // The last column missing.
            a = DecodeRow(Row(start, count, origin, w, 100, null, w - 1), start, w);
            Check(a.Status == Cal.Status.NotOneToOne && a.Origin == origin && a.FirstBadTexel == w - 1,
                  $"a missing last column (width {w}) is not 1:1");
            // The last column doubled.
            a = DecodeRow(Row(start, count, origin, w, 100, p => Math.Min(p, w - 1), w + 1), start, w);
            Check(a.Status == Cal.Status.NotOneToOne && a.Origin == origin && a.EdgeDoubled,
                  $"a doubled last column (width {w}) is not 1:1");
        }

        // Rows under a stretch (GVFIT's h / (h - 0.5) at a whole scale draws h + 0.5 rows: F1).
        {
            int w = 1728, h = 880, origin = 300, start = origin - m;
            Cal.BandStarts(w, out int b0, out int b1);
            int x = Cal.VerticalStripColumn(0, b0, b1, Cal.BandWidth);
            var a = DecodeCol(Col(start, h + 2 * m, origin, w, h, x, Nearest(h, h + 1), h + 1), start, h);
            Check(a.Status == Cal.Status.NotOneToOne, "a doubled row is measured as not 1:1");
            a = DecodeCol(Col(start, h + 2 * m, origin, w, h, x, Nearest(h, h - 1), h - 1), start, h);
            Check(a.Status == Cal.Status.NotOneToOne, "a dropped row is measured as not 1:1");
        }
    }

    static void NotAMeasurement()
    {
        const int m = Cal.StripMarginPx;
        int w = 1728, origin = 1020, start = origin - m, count = w + 2 * m;

        // Nothing there: a grey screen, and a noisy one.
        var grey = new uint[count];
        for (int i = 0; i < count; i++) grey[i] = Grey;
        Check(DecodeRow(grey, start, w).Status == Cal.Status.NotFound, "a grey strip has no marker");
        var rng = new Random(459);
        var noise = new uint[count];
        for (int i = 0; i < count; i++) noise[i] = (uint)rng.Next(0x1000000);
        Check(!DecodeRow(noise, start, w).Measured, "random pixels are not a measurement");
        var sat = new uint[count];
        for (int i = 0; i < count; i++) sat[i] = Cal.Rgb(rng.Next(6));
        Check(!DecodeRow(sat, start, w).Measured, "random marker colours are not a measurement");

        // Another window over part of the Game view.
        var px = Row(start, count, origin, w, 100);
        for (int i = 700; i < 1000; i++) px[i] = 0xF0F0F0u;
        var a = DecodeRow(px, start, w);
        Check(a.Status == Cal.Status.Unreadable && a.Origin == origin, "a covered stretch makes the strip unreadable, not 'not 1:1'");
        // ...while the other strip of the pair, uncovered, decides.
        var clean = DecodeRow(Row(start, count, origin, w, 300), start, w);
        Check(Cal.Best(a, clean).Status == Cal.Status.OneToOne && Cal.Best(clean, a).Status == Cal.Status.OneToOne,
              "Best: a clean 1:1 strip wins over a covered one, in either order");
        Check(Cal.Best(a, a).Status == Cal.Status.Unreadable && !Cal.Best(a, default).Measured, "Best: two unreadable strips stay unreadable");
        var bad = DecodeRow(Row(start, count, origin, w, 100, null, w - 1), start, w);
        Check(Cal.Best(a, bad).Status == Cal.Status.NotOneToOne && Cal.Best(bad, clean).Status == Cal.Status.OneToOne,
              "Best: 1:1 > not 1:1 > unreadable");

        // Filtered (zoom below 1, or Low Resolution): neighbours blend.
        var sharp = Row(start, count, origin, w, 100);
        var blur = new uint[count];
        for (int i = 0; i + 1 < count; i++)
        {
            uint p = sharp[i], q = sharp[i + 1];
            blur[i] = ((((p >> 16) & 0xFF) + ((q >> 16) & 0xFF)) / 2 << 16) | ((((p >> 8) & 0xFF) + ((q >> 8) & 0xFF)) / 2 << 8)
                      | (((p & 0xFF) + (q & 0xFF)) / 2);
        }
        Check(!DecodeRow(blur, start, w).Measured, "a blended (filtered) marker is not a measurement");
        // Drawn at half size (Low Resolution Aspect Ratios at 200 %).
        Check(!DecodeRow(Row(start, count, origin, w, 100, p => p * 2, w / 2), start, w).Measured,
              "a marker drawn at half size is not a measurement");

        // Bad inputs.
        Check(Cal.DecodeAxis(null, 10, 0, w, -1, -1, 0).Status == Cal.Status.NotFound, "null strip");
        Check(Cal.DecodeAxis(sharp, count + 1, start, w, -1, -1, 0).Status == Cal.Status.NotFound, "count past the buffer");
        Check(Cal.DecodeAxis(sharp, count, start, 100, -1, -1, 0).Status == Cal.Status.NotFound, "a texture smaller than the marker covers");
    }

    static void ThroughTheCapture()
    {
        const int m = Cal.StripMarginPx;
        int w = 1120, origin = 1501, start = origin - m, count = w + 2 * m;
        var px = Row(start, count, origin, w, 100);

        // Red and blue exchanged on the way.
        var swapped = new uint[count];
        for (int i = 0; i < count; i++)
            swapped[i] = ((px[i] & 0xFF) << 16) | (px[i] & 0x00FF00u) | ((px[i] >> 16) & 0xFF);
        var a = DecodeRow(swapped, start, w);
        Check(a.Status == Cal.Status.OneToOne && a.Origin == origin && a.SwappedRB, "red / blue exchanged: decoded, and reported");

        // Dimmed with a lifted black level (a tone-mapped capture): on = 180, off = 40.
        var dim = new uint[count];
        for (int i = 0; i < count; i++)
        {
            if (px[i] == Grey) { dim[i] = Grey; continue; }
            uint r = ((px[i] >> 16) & 0xFF) != 0 ? 180u : 40u, g = ((px[i] >> 8) & 0xFF) != 0 ? 180u : 40u, b = (px[i] & 0xFF) != 0 ? 180u : 40u;
            dim[i] = (r << 16) | (g << 8) | b;
        }
        a = DecodeRow(dim, start, w);
        Check(a.Status == Cal.Status.OneToOne && a.Origin == origin && !a.SwappedRB, "dimmed, lifted blacks: decoded");

        // The native read marks read pixels with 0xFF in the upper byte and leaves unread ones 0.
        var flagged = new uint[count];
        for (int i = 0; i < count; i++) flagged[i] = px[i] | 0xFF000000u;
        Check(DecodeRow(flagged, start, w).Status == Cal.Status.OneToOne, "the upper byte of a read pixel does not matter");

        // The image runs off the desktop: the pixels past the edge were not read (0).
        var clipped = (uint[])px.Clone();
        for (int i = m + 700; i < count; i++) clipped[i] = 0u;
        a = DecodeRow(clipped, start, w);
        Check(a.Status == Cal.Status.NotOneToOne || a.Status == Cal.Status.Unreadable,
              "an image cut off inside the strip is not reported 1:1");
        // ...whereas a strip that simply ends at the desktop's edge is judged on what it covers.
        var shorter = new uint[m + 700];
        Array.Copy(px, shorter, shorter.Length);
        a = DecodeRow(shorter, start, w);
        Check(a.Status == Cal.Status.OneToOne && a.Origin == origin && a.Covered == 700, "a strip ending at the desktop's edge: 1:1 over the 700 texels it covers");
        var tooShort = new uint[m + 300];
        Array.Copy(px, tooShort, tooShort.Length);
        Check(DecodeRow(tooShort, start, w).Status == Cal.Status.Unreadable, "less than half the image on the desktop: unreadable");
    }

    static void Main()
    {
        CodeDefinition();
        Classification();
        OneToOne();
        NotOneToOne();
        NotAMeasurement();
        ThroughTheCapture();

        Console.WriteLine($"GameViewCalibration: {s_pass} passed, {s_fail} failed");
        Environment.Exit(s_fail == 0 ? 0 : 1);
    }
}
