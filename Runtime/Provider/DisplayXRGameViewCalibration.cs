// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;

namespace DisplayXR
{
    /// <summary>
    /// (#459) The position-coded marker of the editor Game-view calibration, and how a 1-px
    /// strip of the screen is decoded back into "which screen pixel shows texel 0" and "is the
    /// image drawn texel for pixel". Pure C# (no UnityEngine) so it is unit-tested in CI
    /// (<c>Tests~/GameViewCalibration</c>); <c>DisplayXRGameViewCalibrator</c> owns the timing,
    /// the screen read-back and the logging.
    ///
    /// THE MARKER. For a few frames the native mirror blit draws this pattern instead of the
    /// woven image, into the very render texture the woven image goes to, so it reaches the
    /// screen through exactly the path the weave takes. Each texel is one of six SATURATED
    /// colours (every channel fully on or fully off), so a Gamma / Linear mismatch on the way
    /// to the screen cannot move one into another, and a swapped red / blue is detected
    /// (<see cref="Axis.SwappedRB"/>).
    ///
    /// The colour of a texel is <see cref="Code"/> of its COLUMN, except inside two vertical
    /// bands (<see cref="BandStarts"/>, <see cref="BandWidth"/> texels wide), where it is
    /// <see cref="Code"/> of its ROW. So a horizontal strip, on any row, reads the column code
    /// across the whole width (the two bands are skipped), and a vertical strip inside a band
    /// reads the row code across the whole height.
    ///
    /// THE CODE is self-locating: texels come in blocks of <see cref="BlockLen"/>; the first
    /// is the sync colour (never used for anything else), the other five are the block's
    /// index in base 5, least significant digit first. Any block read off the screen
    /// therefore names its own texel, and (screen position - texel) is the screen position
    /// of texel 0. Every block votes; the image is 1:1 when they all agree and every single
    /// pixel of the strip shows the colour its texel should have.
    ///
    /// The HLSL in <c>native~/displayxr_xrprovider/displayxr_display_provider.cpp</c>
    /// (<c>PSMain</c>, the <c>mk</c> branch) draws the same function; keep the two in step.
    /// </summary>
    internal static class DisplayXRGameViewCalibration
    {
        /// <summary>Texels per block: one sync texel and <see cref="Digits"/> digits.</summary>
        public const int BlockLen = 6;
        public const int Digits = 5;
        public const int DigitBase = 5;
        /// <summary>Colour index of the sync texel (magenta: the same under a red / blue swap).</summary>
        public const int Sync = 5;
        /// <summary>Width of each row-coded band, in texels: four whole blocks.</summary>
        public const int BandWidth = 4 * BlockLen;
        /// <summary>The smallest and largest texture side the code covers (5^5 blocks).</summary>
        public const int MinTexels = 128;
        public const int MaxTexels = BlockLen * 3125;

        /// <summary>Blocks that must agree before a strip counts as showing the marker.</summary>
        public const int MinVotes = 3;
        /// <summary>A second origin this close to the main one is the same image past a dropped
        /// or doubled line; anything farther is noise.</summary>
        public const int NearOriginPx = 4;
        /// <summary>The most pixels a genuine not-1:1 draw leaves showing neither part of the
        /// image (a dropped first or last line shows the background there). More than this
        /// and something else is covering the strip.</summary>
        public const int MaxUnexplainedPx = 4;

        static readonly int[] s_pow5 = { 1, 5, 25, 125, 625 };

        /// <summary>The colour index (0..5) of texel <paramref name="t"/> along a coded axis.</summary>
        public static int Code(int t)
        {
            int r = t % BlockLen;
            if (r == 0) return Sync;
            return (t / BlockLen / s_pow5[r - 1]) % DigitBase;
        }

        /// <summary>Where the two row-coded bands start, in texels, for a texture
        /// <paramref name="texW"/> wide: near one third and two thirds, on block boundaries.</summary>
        public static void BandStarts(int texW, out int band0, out int band1)
        {
            band0 = texW / 3 / BlockLen * BlockLen;
            band1 = 2 * texW / 3 / BlockLen * BlockLen;
        }

        static bool InBand(int t, int band0, int band1, int bandW) =>
            (band0 >= 0 && t >= band0 && t < band0 + bandW) || (band1 >= 0 && t >= band1 && t < band1 + bandW);

        /// <summary>The colour index the marker shows at texel (x, y) of a texture
        /// <paramref name="texW"/> wide.</summary>
        public static int MarkerIndex(int x, int y, int texW)
        {
            BandStarts(texW, out int b0, out int b1);
            return Code(InBand(x, b0, b1, BandWidth) ? y : x);
        }

        /// <summary>A colour index as 0x00RRGGBB: 0 red, 1 green, 2 blue, 3 yellow, 4 cyan,
        /// 5 magenta.</summary>
        public static uint Rgb(int index)
        {
            switch (index)
            {
            case 0: return 0xFF0000u;
            case 1: return 0x00FF00u;
            case 2: return 0x0000FFu;
            case 3: return 0xFFFF00u;
            case 4: return 0x00FFFFu;
            case 5: return 0xFF00FFu;
            default: return 0u;
            }
        }

        /// <summary>
        /// A screen pixel (0x00RRGGBB, upper byte ignored) as a colour index, or -1 when it is
        /// not one of the six: too dark, or a channel that is neither clearly on (at least 70 %
        /// of the brightest) nor clearly off (at most 35 %). Relative to the brightest channel,
        /// so a dimmed or tone-mapped capture still reads.
        /// </summary>
        public static int Classify(uint rgb, bool swapRB = false)
        {
            int r = (int)((rgb >> 16) & 0xFF), g = (int)((rgb >> 8) & 0xFF), b = (int)(rgb & 0xFF);
            if (swapRB) { int t = r; r = b; b = t; }
            int m = Math.Max(r, Math.Max(g, b));
            if (m < 96) return -1;
            int bits = 0;
            for (int i = 0; i < 3; i++)
            {
                int c = i == 0 ? r : i == 1 ? g : b;
                if (c * 100 >= m * 70) bits |= 1 << i;
                else if (c * 100 > m * 35) return -1;
            }
            switch (bits)   // bit 0 red, bit 1 green, bit 2 blue
            {
            case 1: return 0;
            case 2: return 1;
            case 4: return 2;
            case 3: return 3;
            case 6: return 4;
            case 5: return 5;
            default: return -1;   // white / grey
            }
        }

        public enum Status
        {
            /// <summary>No marker on this strip (fewer than <see cref="MinVotes"/> blocks).</summary>
            NotFound,
            /// <summary>The marker is there but part of the strip shows something else: covered
            /// by another window, or drawn filtered. Not a measurement.</summary>
            Unreadable,
            /// <summary>Measured: every texel is on its own screen pixel.</summary>
            OneToOne,
            /// <summary>Measured: the image is there but a line is dropped or doubled.</summary>
            NotOneToOne,
        }

        /// <summary>What one strip says about one axis.</summary>
        public struct Axis
        {
            public Status Status;
            /// <summary>Screen coordinate of texel 0, as most blocks see it.</summary>
            public int Origin;
            /// <summary>Blocks decoded, and how many of them voted for <see cref="Origin"/>.</summary>
            public int Votes, Agreeing;
            /// <summary>Texels of the axis the strip covered (it is clipped at the desktop's edge).</summary>
            public int Covered;
            /// <summary>Covered texels that do not show their colour at <see cref="Origin"/>, and the first of them (-1: none).</summary>
            public int Mismatches, FirstBadTexel;
            /// <summary>Not 1:1: the origin the rest of the image is at (valid when <see cref="HasOther"/>).</summary>
            public bool HasOther;
            public int OtherOrigin;
            /// <summary>Pixels that show neither part of the image.</summary>
            public int Unexplained;
            /// <summary>The line before texel 0 or after the last texel repeats it.</summary>
            public bool EdgeDoubled;
            /// <summary>The strip only decoded with red and blue exchanged.</summary>
            public bool SwappedRB;

            public bool Measured => Status == Status.OneToOne || Status == Status.NotOneToOne;
        }

        /// <summary>The better of two strips of the same axis: a clean 1:1 read cannot be faked
        /// by something covering the strip, so it wins; then a not-1:1 read; then the first.</summary>
        public static Axis Best(Axis a, Axis b)
        {
            if (a.Status == Status.OneToOne) return a;
            if (b.Status == Status.OneToOne) return b;
            if (a.Status == Status.NotOneToOne) return a;
            if (b.Status == Status.NotOneToOne) return b;
            return a.Status == Status.NotFound ? b : a;
        }

        /// <summary>
        /// Decode one strip. <paramref name="rgb"/>[i] is the screen pixel at coordinate
        /// <paramref name="start"/> + i along the axis (0x00RRGGBB; pixels that could not be
        /// read are 0). <paramref name="texLen"/> is the texture's size along the axis.
        /// <paramref name="band0"/> / <paramref name="band1"/> / <paramref name="bandW"/>: the
        /// texel ranges that do not carry this axis's code (the row-coded bands on a
        /// horizontal strip); pass -1, -1, 0 for a vertical strip read inside a band.
        /// </summary>
        public static Axis DecodeAxis(uint[] rgb, int count, int start, int texLen,
                                      int band0, int band1, int bandW)
        {
            var none = new Axis { Status = Status.NotFound, FirstBadTexel = -1 };
            if (rgb == null || count > rgb.Length || count < BlockLen * MinVotes
                || texLen < MinTexels || texLen > MaxTexels) return none;
            var c = new int[count];
            for (int i = 0; i < count; i++) c[i] = Classify(rgb[i]);
            Axis a = DecodeClassified(c, start, texLen, band0, band1, bandW);
            if (a.Measured) return a;
            for (int i = 0; i < count; i++) c[i] = Classify(rgb[i], swapRB: true);
            Axis s = DecodeClassified(c, start, texLen, band0, band1, bandW);
            if (!s.Measured) return a;
            s.SwappedRB = true;
            return s;
        }

        static Axis DecodeClassified(int[] c, int start, int texLen, int band0, int band1, int bandW)
        {
            var res = new Axis { Status = Status.NotFound, FirstBadTexel = -1 };
            int count = c.Length;

            // 1. Every block that reads as "sync, five digits" names its texel and votes for
            //    the screen position of texel 0.
            var votes = new Dictionary<int, int>();
            for (int p = 0; p + BlockLen <= count; p++)
            {
                if (c[p] != Sync) continue;
                int block = 0; bool ok = true;
                for (int d = 0; d < Digits && ok; d++)
                {
                    int v = c[p + 1 + d];
                    if (v < 0 || v >= DigitBase) ok = false;
                    else block += v * s_pow5[d];
                }
                if (!ok) continue;
                int t = block * BlockLen;
                if (t + BlockLen > texLen || InBand(t, band0, band1, bandW)) continue;
                int origin = start + p - t;
                votes.TryGetValue(origin, out int n);
                votes[origin] = n + 1;
                res.Votes++;
            }
            if (res.Votes < MinVotes) return res;

            int best = 0, bestN = 0;
            foreach (var kv in votes)
                if (kv.Value > bestN || (kv.Value == bestN && kv.Key < best)) { best = kv.Key; bestN = kv.Value; }
            if (bestN < MinVotes) return res;
            res.Origin = best; res.Agreeing = bestN;

            // A second origin near the first: the part of the image past a dropped / doubled line.
            int other = 0, otherN = 0;
            foreach (var kv in votes)
                if (kv.Key != best && Math.Abs(kv.Key - best) <= NearOriginPx && kv.Value > otherN)
                { other = kv.Key; otherN = kv.Value; }
            res.HasOther = otherN > 0; res.OtherOrigin = other;

            // 2. Every covered texel against the colour it must show at the main origin.
            for (int t = 0; t < texLen; t++)
            {
                int p = best + t - start;
                if (p < 0 || p >= count) continue;
                res.Covered++;
                if (InBand(t, band0, band1, bandW)) continue;
                if (c[p] == Code(t)) continue;
                res.Mismatches++;
                if (res.FirstBadTexel < 0) res.FirstBadTexel = t;
                // Explained by the other part of the image (or by a band, whose colour along
                // this strip is not known)?
                bool explained = false;
                if (res.HasOther)
                {
                    int t2 = start + p - other;
                    explained = t2 >= 0 && t2 < texLen && (InBand(t2, band0, band1, bandW) || c[p] == Code(t2));
                }
                if (!explained) res.Unexplained++;
            }
            // The line just outside each end must not repeat the end texel.
            int pl = best - 1 - start, pr = best + texLen - start;
            if (pl >= 0 && pl < count && c[pl] == Code(0)) res.EdgeDoubled = true;
            if (pr >= 0 && pr < count && !InBand(texLen - 1, band0, band1, bandW) && c[pr] == Code(texLen - 1))
                res.EdgeDoubled = true;

            if (res.Covered * 2 < texLen || res.Unexplained > MaxUnexplainedPx)
            { res.Status = Status.Unreadable; return res; }
            res.Status = res.Mismatches == 0 && !res.HasOther && !res.EdgeDoubled
                ? Status.OneToOne : Status.NotOneToOne;
            return res;
        }

        // ---- Where to read ----

        /// <summary>How far past each end of the computed image a strip is read, so the
        /// marker is found when the computed position is off.</summary>
        public const int StripMarginPx = 48;
        public const int StripCount = 2;

        /// <summary>The texture row (column code: any row will do) the k-th horizontal strip
        /// is aimed at: one third and two thirds down, so one overlay (stats panel, gizmo
        /// label) rarely covers both.</summary>
        public static int HorizontalStripRow(int k, int texH) => texH * (k + 1) / (StripCount + 1);

        /// <summary>The texture column the k-th vertical strip is aimed at: the middle of the
        /// k-th row-coded band.</summary>
        public static int VerticalStripColumn(int k, int band0, int band1, int bandW) =>
            (k == 0 ? band0 : band1) + bandW / 2;
    }
}
