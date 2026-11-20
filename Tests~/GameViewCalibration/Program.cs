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

    // ---- Thin strips: the marker only where it is read (#459) ----

    // A deterministic "woven scene" pixel: anything but the marker's own pattern. Saturated
    // colours on purpose (the worst case for the classifier), from a hash of the position.
    static uint ScenePixel(int x, int y)
    {
        uint h = (uint)(x * 73856093) ^ (uint)(y * 19349663);
        h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
        return (h & 1) != 0 ? Cal.Rgb((int)(h >> 8) % 6) : (h & 0xFFFFFFu);
    }

    // What the screen shows at SCREEN pixel (sx, sy) when a texW x texH image whose texel (0,0)
    // is at screen (ox, oy) is drawn texel for pixel with the marker of `strip` / `half`.
    static uint Screen(int sx, int sy, int ox, int oy, int texW, int texH, int strip, int half)
    {
        int x = sx - ox, y = sy - oy;
        if (x < 0 || y < 0 || x >= texW || y >= texH) return Grey;
        int k = Cal.MarkerIndexAt(x, y, texW, texH, strip, half);
        return k == Cal.Scene ? ScenePixel(x, y) : Cal.Rgb(k);
    }

    // The calibrator's two reads on such a screen, as it makes them: a horizontal strip at
    // "computed top + strip row" from "computed left - margin", then a vertical strip at
    // "measured left + strip column" from "computed top - margin".
    static bool Measure(int ox, int oy, int cx, int cy, int texW, int texH, int strip, int half,
                        out Cal.Axis ax, out Cal.Axis ay)
    {
        const int m = Cal.StripMarginPx;
        int k = strip < 0 ? 0 : strip;
        Cal.BandStarts(texW, out int b0, out int b1);
        ay = default;
        int x0 = cx - m, row = cy + Cal.HorizontalStripRow(k, texH);
        var h = new uint[texW + 2 * m];
        for (int i = 0; i < h.Length; i++) h[i] = Screen(x0 + i, row, ox, oy, texW, texH, strip, half);
        ax = Cal.DecodeAxis(h, h.Length, x0, texW, b0, b1, Cal.BandWidth);
        if (!ax.Measured) return false;
        int y0 = cy - m, col = ax.Origin + Cal.VerticalStripColumn(k, b0, b1, Cal.BandWidth);
        var v = new uint[texH + 2 * m];
        for (int i = 0; i < v.Length; i++) v[i] = Screen(col, y0 + i, ox, oy, texW, texH, strip, half);
        ay = Cal.DecodeAxis(v, v.Length, y0, texH, -1, -1, 0);
        return ay.Measured;
    }

    static void ThinStrips()
    {
        int half = Cal.StripHalfPx;
        Check(half > Cal.NearOriginPx && half < Cal.BandWidth / 2, "the strip's half thickness is between the near-origin slack and half a band");

        // Where the marker is: the whole image, or the two strips and nothing else.
        {
            int w = 1120, h = 595;
            Cal.BandStarts(w, out int b0, out int b1);
            bool full = true, thin = true;
            int inside = 0;
            for (int k = 0; k < Cal.StripCount; k++)
            {
                int hr = Cal.HorizontalStripRow(k, h), vc = Cal.VerticalStripColumn(k, b0, b1, Cal.BandWidth);
                for (int y = 0; y < h; y++)
                    for (int x = 0; x < w; x++)
                    {
                        if (!Cal.InMarker(x, y, w, h, Cal.StripFull, half)) full = false;
                        bool want = (y >= hr - half && y <= hr + half) || (x >= vc - half && x <= vc + half);
                        bool got = Cal.InMarker(x, y, w, h, k, half);
                        if (want != got) thin = false;
                        if (got && k == 0) inside++;
                        if (got && Cal.MarkerIndexAt(x, y, w, h, k, half) != Cal.MarkerIndex(x, y, w)) thin = false;
                        if (!got && Cal.MarkerIndexAt(x, y, w, h, k, half) != Cal.Scene) thin = false;
                    }
            }
            Check(full, "the whole-image marker covers every texel");
            Check(thin, "a thin marker is the whole-image marker on its two strips and the scene elsewhere");
            int t = 2 * half + 1;
            Check(inside == t * w + t * h - t * t, "a thin marker covers one row strip and one column strip, 13 texels thick");
            Check(inside * 100 < 4 * w * h, $"...which is under 4 % of a 1120x595 image ({inside * 100.0 / (w * h):0.0} %)");
        }

        // The measurement from thin strips equals the one from the whole image, for every size
        // seen on hardware, both strip pairs, and a computed position off by up to the half
        // thickness in y and by anything the margin allows in x.
        bool same = true, exact = true;
        foreach (var (w, h) in new[] { (1120, 595), (1728, 860), (1924, 1020), (128, 128), (3840, 2160), (880, 495) })
            foreach (int strip in new[] { 0, 1 })
                foreach (int ex in new[] { 0, 1, -1, 2, -7, 30 })
                    foreach (int ey in new[] { 0, 1, -1, half, -half })
                    {
                        int ox = 1501, oy = 1327;
                        bool okT = Measure(ox, oy, ox + ex, oy + ey, w, h, strip, half, out var tx, out var ty);
                        bool okF = Measure(ox, oy, ox + ex, oy + ey, w, h, Cal.StripFull, half, out var fx, out var fy);
                        if (!okT || !okF) { same = false; continue; }
                        if (tx.Status != Cal.Status.OneToOne || ty.Status != Cal.Status.OneToOne || tx.Origin != ox || ty.Origin != oy) exact = false;
                        if (tx.Origin != fx.Origin || ty.Origin != fy.Origin || tx.Status != fx.Status || ty.Status != fy.Status
                            || tx.Votes != fx.Votes || ty.Votes != fy.Votes || tx.Covered != fx.Covered || ty.Covered != fy.Covered) same = false;
                    }
        Check(exact, "thin strips: origin exact and 1:1 on both axes, computed position off by up to 6 px in y");
        Check(same, "thin strips give the same origin, verdict, votes and coverage as the whole-image marker");

        // The 175 % hardware case through thin strips: image at pane +1,+110, computed +2,+110.
        {
            int pane = 1500, top = 1217;
            bool ok = Measure(pane + 1, top + 110, pane + 2, top + 110, 1120, 595, 0, half, out var ax, out var ay);
            Check(ok && ax.Origin - pane == 1 && ay.Origin - top == 110 && ax.Status == Cal.Status.OneToOne && ay.Status == Cal.Status.OneToOne,
                  "175 %: thin strips measure pane +1,+110 against a computed +2,+110");
        }

        // The computed row is past the strip: the read row shows the scene, and a scene is
        // never a measurement - not a wrong origin, no marker at all.
        bool never = true, notFound = true;
        foreach (var (w, h) in new[] { (1120, 595), (1728, 860), (3840, 2160) })
            foreach (int strip in new[] { 0, 1 })
                foreach (int ey in new[] { half + 1, -(half + 1), 20, -37, 100 })
                {
                    if (Measure(900, 400, 900, 400 + ey, w, h, strip, half, out var ax, out _)) never = false;
                    if (ax.Status != Cal.Status.NotFound) notFound = false;
                }
        Check(never, "a read row off the thin strip is never a measurement");
        Check(notFound, "...it reads as 'no marker', which is what sends the next attempt to the whole image");
        // ...and the whole-image marker measures that same case.
        Check(Measure(900, 400, 900, 400 + 37, 1728, 860, Cal.StripFull, half, out var gx, out var gy)
              && gx.Origin == 900 && gy.Origin == 400, "the whole-image marker measures with the computed row 37 px off");

        // Not 1:1 through thin strips: a dropped column before the vertical strip moves the
        // strip by one pixel on screen; the vertical read at the measured column still lands
        // inside it (the half thickness is larger than the near-origin slack).
        {
            const int m = Cal.StripMarginPx;
            int w = 1120, h = 595, ox = 1502, oy = 300, k = 1;
            Cal.BandStarts(w, out int b0, out int b1);
            var near = Nearest(w, w - 1);
            int row = oy + Cal.HorizontalStripRow(k, h), x0 = ox - m;
            Func<int, int, uint> screen = (sx, sy) =>
            {
                int rx = sx - ox, y = sy - oy;
                if (rx < 0 || rx >= w - 1 || y < 0 || y >= h) return Grey;
                int x = near(rx);
                int idx = Cal.MarkerIndexAt(x, y, w, h, k, half);
                return idx == Cal.Scene ? ScenePixel(x, y) : Cal.Rgb(idx);
            };
            var hs = new uint[w + 2 * m];
            for (int i = 0; i < hs.Length; i++) hs[i] = screen(x0 + i, row);
            var ax = Cal.DecodeAxis(hs, hs.Length, x0, w, b0, b1, Cal.BandWidth);
            Check(ax.Status == Cal.Status.NotOneToOne && ax.HasOther, "thin strips: a dropped column is measured as not 1:1");
            int col = ax.Origin + Cal.VerticalStripColumn(k, b0, b1, Cal.BandWidth), y0 = oy - m;
            var vs = new uint[h + 2 * m];
            for (int i = 0; i < vs.Length; i++) vs[i] = screen(col, y0 + i);
            var ay = Cal.DecodeAxis(vs, vs.Length, y0, h, -1, -1, 0);
            Check(ay.Status == Cal.Status.OneToOne && ay.Origin == oy, "...and the rows are still measured (1:1) at the column that was found");
        }

        // A dropped row through the thin vertical strip.
        {
            const int m = Cal.StripMarginPx;
            int w = 1728, h = 880, ox = 40, oy = 300, k = 0;
            Cal.BandStarts(w, out int b0, out int b1);
            var near = Nearest(h, h - 1);
            int col = ox + Cal.VerticalStripColumn(k, b0, b1, Cal.BandWidth), y0 = oy - m;
            var vs = new uint[h + 2 * m];
            for (int i = 0; i < vs.Length; i++)
            {
                int ry = y0 + i - oy;
                int idx = ry < 0 || ry >= h - 1 ? -1 : Cal.MarkerIndexAt(col - ox, near(ry), w, h, k, half);
                vs[i] = idx < 0 ? Grey : Cal.Rgb(idx);
            }
            Check(Cal.DecodeAxis(vs, vs.Length, y0, h, -1, -1, 0).Status == Cal.Status.NotOneToOne, "thin strips: a dropped row is measured as not 1:1");
        }

        // The numbers the native shader test pins for the HLSL (native~/tests/gvcal_shader_test.cpp).
        Check(Cal.MarkerHash(300, 200, Cal.StripFull, 6) == 0xD8CD3005u && Cal.MarkerHash(300, 200, 0, 6) == 0xB91460BBu
              && Cal.MarkerHash(300, 200, 1, 6) == 0xBCC352AFu && Cal.MarkerHash(1120, 595, Cal.StripFull, 6) == 0xE0A30EBAu
              && Cal.MarkerHash(1120, 595, 0, 6) == 0x4FABC829u && Cal.MarkerHash(1120, 595, 1, 6) == 0x09F212EDu,
              "the marker hashes are the ones the shader test renders: " +
              $"{Cal.MarkerHash(300, 200, -1, 6):X8} {Cal.MarkerHash(300, 200, 0, 6):X8} {Cal.MarkerHash(300, 200, 1, 6):X8} " +
              $"{Cal.MarkerHash(1120, 595, -1, 6):X8} {Cal.MarkerHash(1120, 595, 0, 6):X8} {Cal.MarkerHash(1120, 595, 1, 6):X8}");
    }

    // ---- Which marker, and when (#459) ----

    static void Plan()
    {
        var none = Cal.Fail.NotDrawn; var missed = Cal.Fail.Missed; var covered = Cal.Fail.Covered;
        Check(Cal.PlanStrip(0, none, false) == 0 && Cal.PlanStrip(0, covered, false) == 0 && Cal.PlanStrip(0, missed, false) == 0,
              "the first attempt draws thin strip pair 0");
        Check(Cal.PlanStrip(1, covered, false) == 1, "after a covered strip: the other pair, elsewhere on the image");
        Check(Cal.PlanStrip(1, missed, false) == Cal.StripFull, "after a strip that was drawn and not found: the whole image");
        Check(Cal.PlanStrip(1, none, false) == 0 && Cal.PlanStrip(2, none, false) == 0,
              "after a marker that was never drawn: the same thin pair again (nothing was learnt)");
        Check(Cal.PlanStrip(2, covered, false) == Cal.StripFull && Cal.PlanStrip(2, missed, false) == Cal.StripFull
              && Cal.PlanStrip(7, covered, false) == Cal.StripFull, "the last attempt after a marker that was drawn is the whole image");
        bool forced = true;
        for (int a = 0; a < 4; a++)
            foreach (var f in new[] { none, missed, covered })
                if (Cal.PlanStrip(a, f, true) != Cal.StripFull) forced = false;
        Check(forced, "DISPLAYXR_PROV_GV_CALIB_FULL=1: every attempt draws the whole image");
        Check(Cal.MaxAttempts == 3, "three attempts per layout, as before");

        Check(Cal.RetryDelaySeconds(0, missed) < 0.5, "a thin marker drawn but not found: the whole image follows at once");
        Check(Cal.RetryDelaySeconds(0, covered) == 2.0 && Cal.RetryDelaySeconds(1, covered) == 2.0, "a covered strip waits 2 s (a window is over the Game view)");
        Check(Cal.RetryDelaySeconds(0, none) == 2.0, "a marker that was not drawn waits 2 s");
        // Why it was not drawn (the native mirror blit's reason, offers and calls).
        Check(Cal.NativeMirrorReasonText(-1, 0, 0).Contains("older binary"), "no export: says so");
        Check(Cal.NativeMirrorReasonText(0, 0, 0).Contains("has not asked"), "nothing offered yet");
        Check(Cal.NativeMirrorReasonText(0, 30, 0).Contains("has not called it so far"), "offered a few times, not called yet");
        Check(Cal.NativeMirrorReasonText(0, Cal.MirrorNeverCalledAfter, 0).Contains("never calls it"), "offered for seconds, never called");
        Check(Cal.NativeMirrorReasonText(Cal.MirrorDrawn, 400, 400).Contains("drew no frame while the marker was requested"), "draws, but not now");
        Check(Cal.NativeMirrorReasonText(Cal.MirrorNotD3D12, 400, 400).Contains("not on D3D12"), "not D3D12");
        Check(Cal.NativeMirrorReasonText(Cal.MirrorDrawnNoMarker, 400, 400).Contains("draws on D3D11"), "D3D11: the blit draws, the marker is D3D12 only (#483)");
        Check(Cal.NativeMirrorReasonText(9, 400, 400).Contains("could not be resolved") && Cal.NativeMirrorReasonText(9, 400, 400).Contains("reason 9"),
              "a render target the interface cannot resolve, with the raw numbers");
        for (int r = 2; r <= 15; r++)
            Check(Cal.NativeMirrorReasonText(r, 10, 10).Length > 20, "reason " + r + " has a text");
        Check(Cal.RetryDelaySeconds(Cal.StripFull, missed) == 2.0 && Cal.RetryDelaySeconds(Cal.StripFull, covered) == 2.0
              && Cal.RetryDelaySeconds(Cal.StripFull, none) == 2.0, "the whole-image marker retries after 2 s, as before");

        // Settle: never before the settle time; not while a button is down (a paused splitter
        // drag); but not for ever.
        const double s = 0.35;
        Check(!Cal.Settled(0.0, s, false) && !Cal.Settled(0.34, s, false) && Cal.Settled(0.35, s, false) && Cal.Settled(9, s, false),
              "button up: measured once the layout has held 0.35 s");
        Check(!Cal.Settled(0.35, s, true) && !Cal.Settled(1.0, s, true) && !Cal.Settled(s + Cal.MaxButtonHoldSeconds - 0.01, s, true),
              "button down: a layout that paused mid-drag is not measured");
        Check(Cal.Settled(s + Cal.MaxButtonHoldSeconds, s, true), "...but a button held for another reason does not block it for ever");
        Check(!Cal.Settled(0.1, s, true), "button down before the settle time: not measured");
        // A splitter drag: sizes change every 16 ms for a second, pause 1 s with the button
        // down, move again, release. Measurements started: exactly one, after the release.
        {
            int started = 0; double pendSince = 0; int size = 0, lastSize = -1; bool have = false;
            for (int f = 0; f < 400 && !have; f++)
            {
                double t = f * 0.016;
                bool down = t < 3.0;
                if (t < 1.0 || (t >= 2.0 && t < 3.0)) size = f;   // moving
                if (size != lastSize) { lastSize = size; pendSince = t; continue; }
                if (Cal.Settled(t - pendSince, s, down)) { started++; have = true; Check(t >= 3.0 && t < 3.4, "the one measurement of a drag comes within 0.35 s of the release"); }
            }
            Check(started == 1, "a splitter drag with a pause in it is measured once");
        }
    }

    // ---- The layouts measured this session (#459) ----

    static Cal.LayoutKey Key175() => new Cal.LayoutKey
    {
        Pane = 0x2A0F1C, Monitor = 0x10001, PaneLeft = 1500, PaneTop = 1217, PaneW = 1124, PaneH = 740,
        Dx = 2, Dy = 110, W = 1120, H = 595, Maximized = false, WholePixel = true, Scale = 1.75f,
    };

    static void Cache()
    {
        var k = Key175();
        Check(k.SameLayout(k) && k.SameCached(k), "a layout is itself");

        // Every field makes another layout; the position only for the cache.
        var variants = new (string, Func<Cal.LayoutKey, Cal.LayoutKey>)[]
        {
            ("pane", x => { x.Pane++; return x; }), ("monitor", x => { x.Monitor++; return x; }),
            ("pane width", x => { x.PaneW++; return x; }), ("pane height", x => { x.PaneH--; return x; }),
            ("computed x", x => { x.Dx++; return x; }), ("computed y", x => { x.Dy--; return x; }),
            ("image width", x => { x.W++; return x; }), ("image height", x => { x.H++; return x; }),
            ("maximized", x => { x.Maximized = !x.Maximized; return x; }),
            ("whole-pixel size", x => { x.WholePixel = !x.WholePixel; return x; }),
            ("scale", x => { x.Scale = 1.5f; return x; }),
        };
        foreach (var (name, f) in variants)
        {
            var o = f(k);
            Check(!k.SameLayout(o) && !o.SameLayout(k) && !k.SameCached(o), $"another {name} is another layout");
        }
        foreach (var (name, f) in new (string, Func<Cal.LayoutKey, Cal.LayoutKey>)[]
                 { ("left", x => { x.PaneLeft++; return x; }), ("top", x => { x.PaneTop -= 400; return x; }) })
        {
            var o = f(k);
            Check(k.SameLayout(o), $"a pane moved ({name}) is the same layout: the measurement in use stays");
            Check(!k.SameCached(o) && !o.SameCached(k), $"...but not a cache hit: nothing is checked on screen there");
        }

        var c = new Cal.LayoutCache();
        Check(!c.TryGet(k, 1120, 595, out _), "an empty cache has nothing");
        c.Put(new Cal.CacheEntry { Key = k, TexW = 1120, TexH = 595, OriginDx = 1, OriginDy = 110 });
        Check(c.TryGet(k, 1120, 595, out var e) && e.OriginDx == 1 && e.OriginDy == 110 && e.TexW == 1120 && e.TexH == 595,
              "the measured layout is found, with its origin");
        Check(!c.TryGet(k, 1121, 595, out _) && !c.TryGet(k, 1120, 594, out _), "a texture of another size is not a hit");
        Check(!c.TryGet(k, 0, 0, out _), "a texture that is not drawn yet is not a hit");
        bool miss = true;
        foreach (var (_, f) in variants) if (c.TryGet(f(k), 1120, 595, out _)) miss = false;
        var moved = k; moved.PaneLeft += 1;
        if (c.TryGet(moved, 1120, 595, out _)) miss = false;
        moved = k; moved.PaneTop += 1;
        if (c.TryGet(moved, 1120, 595, out _)) miss = false;
        Check(miss, "a layout that differs in any one field, or by one pixel of position, is a miss");

        // Measured again: the entry is replaced, not doubled.
        c.Put(new Cal.CacheEntry { Key = k, TexW = 1120, TexH = 595, OriginDx = 2, OriginDy = 111 });
        Check(c.Count == 1 && c.TryGet(k, 1120, 595, out e) && e.OriginDx == 2 && e.OriginDy == 111, "a layout measured again replaces its entry");

        // Text round trip (the editor's session state), negative coordinates and an awkward scale.
        var k2 = k; k2.PaneLeft = -1913; k2.PaneTop = -7; k2.Scale = 1.7499999f; k2.Maximized = true; k2.WholePixel = false;
        k2.Pane = unchecked((long)0xFFFFFFFF80001234UL); k2.Dx = -3;
        c.Put(new Cal.CacheEntry { Key = k2, TexW = 1924, TexH = 1020, OriginDx = -2, OriginDy = 39 });
        string text = c.Serialize();
        var r = Cal.LayoutCache.Parse(text);
        Check(r.Count == 2 && r.TryGet(k, 1120, 595, out e) && e.OriginDx == 2 && e.OriginDy == 111
              && r.TryGet(k2, 1924, 1020, out e) && e.OriginDx == -2 && e.OriginDy == 39, "the cache survives its text form");
        Check(r.Serialize() == text, "...which is stable");
        var kScale = k2; kScale.Scale = 1.75f;
        Check(!r.TryGet(kScale, 1924, 1020, out _), "a scale that differs in the last digit is not a hit after the round trip");
        var prev = System.Globalization.CultureInfo.CurrentCulture;
        System.Globalization.CultureInfo.CurrentCulture = new System.Globalization.CultureInfo("de-DE");
        string german = c.Serialize();
        var rg = Cal.LayoutCache.Parse(german);
        System.Globalization.CultureInfo.CurrentCulture = prev;
        Check(german == text && rg.Count == 2, "the text form does not depend on the machine's number format");

        // Damaged or foreign text: nothing, or only the entries that parse.
        Check(Cal.LayoutCache.Parse(null).Count == 0 && Cal.LayoutCache.Parse("").Count == 0
              && Cal.LayoutCache.Parse("garbage").Count == 0 && Cal.LayoutCache.Parse("gvcal0;" + text.Substring(7)).Count == 0,
              "no text, foreign text and another version give an empty cache");
        Check(Cal.LayoutCache.Parse(text + ";1,2,3").Count == 2 && Cal.LayoutCache.Parse(text.Replace(",1.75,", ",x,")).Count == 1
              && Cal.LayoutCache.Parse(text.Replace(",1120,595,2,111", ",1120,595,2")).Count == 1,
              "a damaged entry is dropped, the others stay");
        Check(Cal.LayoutCache.Parse(text.Replace(",1120,595,2,111", ",12,595,2,111")).Count == 1, "an entry with an impossible texture is dropped");

        // Bounded: the oldest goes.
        var big = new Cal.LayoutCache();
        for (int i = 0; i < Cal.LayoutCache.Capacity + 5; i++)
        {
            var ki = k; ki.PaneW = 1000 + i;
            big.Put(new Cal.CacheEntry { Key = ki, TexW = 1120, TexH = 595, OriginDx = i, OriginDy = 0 });
        }
        var oldest = k; oldest.PaneW = 1000;
        var newest = k; newest.PaneW = 1000 + Cal.LayoutCache.Capacity + 4;
        Check(big.Count == Cal.LayoutCache.Capacity && !big.TryGet(oldest, 1120, 595, out _) && big.TryGet(newest, 1120, 595, out _),
              "the cache is bounded; the oldest layout is forgotten first");
        Check(Cal.LayoutCache.Parse(big.Serialize()).Count == Cal.LayoutCache.Capacity, "a full cache round-trips");
    }

    static void Main()
    {
        CodeDefinition();
        Classification();
        OneToOne();
        NotOneToOne();
        NotAMeasurement();
        ThroughTheCapture();
        ThinStrips();
        Plan();
        Cache();
        CalibratorTests.RunAll(Check);

        Console.WriteLine($"GameViewCalibration: {s_pass} passed, {s_fail} failed");
        Environment.Exit(s_fail == 0 ? 0 : 1);
    }
}
