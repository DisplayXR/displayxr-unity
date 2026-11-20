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
    /// the screen read-back and the logging. Also here, pure for the same reason: where the
    /// marker is drawn (thin strips), which marker each attempt draws (<see cref="PlanStrip"/>),
    /// when a layout counts as settled (<see cref="Settled"/>) and the layouts already measured
    /// (<see cref="LayoutCache"/>).
    ///
    /// THE MARKER. While it is measured the native mirror blit draws this pattern over the
    /// woven image, into the very render texture the woven image goes to, so it reaches the
    /// screen through exactly the path the weave takes; by default only on the two thin strips
    /// that are read back (<see cref="InMarker"/>), the woven image everywhere else. Each texel is one of six SATURATED
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
    /// The HLSL in <c>native~/displayxr_xrprovider/displayxr_gvcal_shader.h</c>
    /// (<c>PSMain</c>, the <c>mk</c> branch) draws the same function; keep the two in step
    /// (<c>native~/tests/gvcal_shader_test.cpp</c> renders it and checks).
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

        // ---- Where to DRAW: thin strips ----
        //
        // The decoder only ever looks at one screen row and one screen column, so the marker
        // only has to be there: a horizontal strip around the row that is read and a vertical
        // strip around the column that is read, each a few texels thick. Everywhere else the
        // woven image stays. Inside a strip a texel shows exactly what the whole-image marker
        // shows there (MarkerIndex), so a strip read on a thin marker is pixel for pixel the
        // strip read on the whole-image one and DecodeAxis does not know the difference.
        //
        // The thickness is the tolerance for the COMPUTED position: the horizontal strip is
        // read at "computed top + row", so it decodes while the computed top is within
        // StripHalfPx of the real one. Past that the read row shows the scene, the strip
        // reads "no marker" (a scene cannot decode: see DecodeAxis), and the next attempt
        // draws the whole image (PlanStrip). The vertical strip is read at the column the
        // horizontal one just MEASURED; a line dropped or doubled before it moves it by at
        // most NearOriginPx, which is inside the half thickness.

        /// <summary><c>strip</c> value for the whole-image marker.</summary>
        public const int StripFull = -1;
        /// <summary>Texels drawn on each side of the row / column that is read: a strip is
        /// 2 * 6 + 1 = 13 texels thick. Must stay below BandWidth / 2 (the vertical strip
        /// lives inside a row-coded band) and above NearOriginPx.</summary>
        public const int StripHalfPx = 6;
        /// <summary>What <see cref="MarkerIndexAt"/> answers where the marker is not drawn.</summary>
        public const int Scene = 6;

        /// <summary>Is texel (x, y) of a <paramref name="texW"/> x <paramref name="texH"/>
        /// texture part of the marker? <paramref name="strip"/>: <see cref="StripFull"/> for
        /// the whole image, else 0 / 1 for the k-th horizontal strip plus the k-th vertical
        /// strip, <paramref name="half"/> texels on each side of the line that is read. The
        /// HLSL (<c>PSMain</c>, <c>st</c>) draws the same test.</summary>
        public static bool InMarker(int x, int y, int texW, int texH, int strip, int half)
        {
            if (strip < 0) return true;
            BandStarts(texW, out int b0, out int b1);
            int hr = HorizontalStripRow(strip, texH), vc = VerticalStripColumn(strip, b0, b1, BandWidth);
            return Math.Abs(y - hr) <= half || Math.Abs(x - vc) <= half;
        }

        /// <summary>The colour index (0..5) drawn at texel (x, y), or <see cref="Scene"/>
        /// where the woven image is left alone.</summary>
        public static int MarkerIndexAt(int x, int y, int texW, int texH, int strip, int half) =>
            InMarker(x, y, texW, texH, strip, half) ? MarkerIndex(x, y, texW) : Scene;

        /// <summary>FNV-1a over <see cref="MarkerIndexAt"/> of every texel, row by row. The
        /// native shader test (<c>native~/tests/gvcal_shader_test.cpp</c>) renders the HLSL
        /// and pins the same numbers, which ties the shader to this definition.</summary>
        public static uint MarkerHash(int texW, int texH, int strip, int half)
        {
            uint h = 2166136261u;
            for (int y = 0; y < texH; y++)
                for (int x = 0; x < texW; x++)
                    h = (h ^ (uint)MarkerIndexAt(x, y, texW, texH, strip, half)) * 16777619u;
            return h;
        }

        // ---- Which marker, and when ----

        /// <summary>Attempts per layout.</summary>
        public const int MaxAttempts = 3;

        /// <summary>Why an attempt failed.</summary>
        public enum Fail
        {
            /// <summary>The marker was never drawn (no native blit, a view that is not repainting).</summary>
            NotDrawn,
            /// <summary>Drawn, but the strip that was read shows no marker.</summary>
            Missed,
            /// <summary>The marker is there but something is over part of the strip.</summary>
            Covered,
        }

        /// <summary>
        /// What the <paramref name="attempt"/>-th attempt (0-based) at a layout draws: thin
        /// strip pair 0 first, and again as long as nothing was drawn at all; after a
        /// COVERED strip the other pair, which is elsewhere on the image; after a strip that
        /// was drawn and MISSED, and from the third attempt on, the whole image, which is the
        /// marker the measurement was proven with and does not depend on the computed
        /// position being close.
        /// <paramref name="forceFull"/>: <c>DISPLAYXR_PROV_GV_CALIB_FULL=1</c>.
        /// </summary>
        public static int PlanStrip(int attempt, Fail last, bool forceFull)
        {
            if (forceFull) return StripFull;
            if (attempt <= 0 || last == Fail.NotDrawn) return 0;
            if (attempt >= 2) return StripFull;
            return last == Fail.Covered ? 1 : StripFull;
        }

        /// <summary>How long after a failed attempt the next one starts. A thin marker that
        /// was drawn but not found on the read row (the computed position is more than the
        /// strip's half thickness off) is followed by the whole image at once: until then the
        /// anchor is the computed one. Everything else waits 2 s, as before: a window over
        /// the Game view, a view that is not repainting.</summary>
        public static double RetryDelaySeconds(int strip, Fail fail) =>
            strip >= 0 && fail == Fail.Missed ? 0.2 : 2.0;

        // The native mirror blit's reasons (DXR_MB_* in
        // native~/displayxr_xrprovider/displayxr_display_provider.cpp).
        public const int MirrorNeverCalled = 0, MirrorDrawn = 1, MirrorNotD3D12 = 7, MirrorDrawnNoMarker = 15;
        /// <summary>After this many mirror queries that offered the native blit without one
        /// call, Unity is not going to call it (native: DXR_MB_NEVER_CALLED_AFTER).</summary>
        public const int MirrorNeverCalledAfter = 240;

        /// <summary>
        /// Why the marker was not drawn, from the native mirror blit's own state
        /// (<c>dxr_prov_get_native_mirror_status</c>): the reason it last ended with, how many
        /// mirror queries offered the native blit to Unity this session and how many times
        /// Unity called it. <paramref name="reason"/> below 0: the plug-in binary has no such
        /// export. The marker is drawn in that blit and nowhere else, so each case names what
        /// would have to be different.
        /// </summary>
        public static string NativeMirrorReasonText(int reason, int offered, int calls)
        {
            string counts = " [native mirror blit: offered " + offered + " times, called " + calls + " times, reason " + reason + "]";
            if (reason < 0)
                return "the native plug-in does not report why (an older binary)";
            if (calls == 0)
                return (offered == 0
                    ? "Unity has not asked for a mirror blit that the plug-in could offer its native blit for (no woven texture yet, DISPLAYXR_PROV_NATIVE_MIRROR=0, or the Game view is not repainting)"
                    : offered < MirrorNeverCalledAfter
                        ? "Unity was offered the native mirror blit and has not called it so far"
                        : "Unity was offered the native mirror blit and never calls it: this render pipeline (the Built-in one) blits the woven texture itself, so no marker can be drawn here and the placement is the computed one")
                    + counts;
            switch (reason)
            {
                case MirrorDrawn:
                    return "the native mirror blit draws, but drew no frame while the marker was requested (is the Game view repainting? Play paused, or the editor in the background)" + counts;
                case 2: return "DISPLAYXR_PROV_NATIVE_MIRROR=0 turns the native mirror blit off" + counts;
                case 3: return "no woven texture is registered with Unity" + counts;
                case 4: case 5: return "Unity called the native mirror blit without a render target it can draw into" + counts;
                case 6: return "the plug-in has no IUnityInterfaces" + counts;
                case MirrorNotD3D12: return "the editor is not on D3D12 or D3D11 (no native mirror blit for this renderer; the marker in it is D3D12 only)" + counts;
                case 8: return "Unity's D3D12 interface returned no device" + counts;
                case 9: return "Unity's mirror render target could not be resolved to a D3D12 texture" + counts;
                case 10: return "the woven texture is not open on Unity's D3D12 device" + counts;
                case 11: return "the woven canvas is empty" + counts;
                case 12: return "the blit pipeline could not be built (shader or pipeline state)" + counts;
                case 13: return "the blit's command list could not be created" + counts;
                case 14: return "this platform has no native mirror blit" + counts;
                case MirrorDrawnNoMarker: return "the native mirror blit draws on D3D11, which draws no marker (the calibration marker is D3D12 only): the placement is the computed one" + counts;
                default: return "the native mirror blit did not draw" + counts;
            }
        }

        /// <summary>The longest a settled layout waits for the mouse button to come up.</summary>
        public const double MaxButtonHoldSeconds = 3.0;

        /// <summary>
        /// May a layout that has been unchanged for <paramref name="heldSeconds"/> be measured
        /// now? Not before <paramref name="settleSeconds"/>; and not while a mouse button is
        /// down, because then it is a splitter drag that paused, and the next move would
        /// measure again: the marker is shown once, when the drag ends. The wait for the
        /// button is bounded (<see cref="MaxButtonHoldSeconds"/> past the settle time), so a
        /// button held for another reason does not leave the layout unmeasured.
        /// </summary>
        public static bool Settled(double heldSeconds, double settleSeconds, bool buttonDown) =>
            heldSeconds >= settleSeconds && (!buttonDown || heldSeconds >= settleSeconds + MaxButtonHoldSeconds);

        // ---- Measured layouts, remembered ----

        /// <summary>
        /// Everything the place of the image inside its pane can depend on. All px are
        /// physical. Two rules read it:
        /// <see cref="SameLayout"/> (the layout being shown has not changed: the measurement
        /// in use stays in use; the pane's position is NOT part of it, the image does not move
        /// inside its pane when the editor window moves) and <see cref="SameCached"/> (a
        /// measurement taken earlier may be used WITHOUT measuring: everything, the pane's
        /// position included, because here nothing is checked on screen and a wrong origin is
        /// an inverted image).
        /// </summary>
        public struct LayoutKey
        {
            public long Pane, Monitor;          // window / monitor handles
            public int PaneLeft, PaneTop;       // the pane's origin on the desktop
            public int PaneW, PaneH;            // its client size
            public int Dx, Dy, W, H;            // the COMPUTED image rect, relative to the pane
            public bool Maximized, WholePixel;  // the kind of size selected
            public float Scale;                 // the pane's points-to-px

            public bool SameLayout(in LayoutKey o) =>
                Pane == o.Pane && Monitor == o.Monitor && PaneW == o.PaneW && PaneH == o.PaneH
                && Dx == o.Dx && Dy == o.Dy && W == o.W && H == o.H
                && Maximized == o.Maximized && WholePixel == o.WholePixel && Scale == o.Scale;

            public bool SameCached(in LayoutKey o) =>
                SameLayout(o) && PaneLeft == o.PaneLeft && PaneTop == o.PaneTop;
        }

        /// <summary>One measured layout: the texture the marker was drawn into and where its
        /// texel 0 was found, relative to the pane.</summary>
        public struct CacheEntry
        {
            public LayoutKey Key;
            public int TexW, TexH;
            public int OriginDx, OriginDy;
        }

        /// <summary>
        /// The layouts measured in this editor session (kept as text in the editor's session
        /// state, so it outlives the domain reload of every Play). Only clean measurements
        /// are put in: both axes 1:1. A hit needs <see cref="LayoutKey.SameCached"/> AND the
        /// texture now being drawn to be the size the entry was measured with. Oldest out
        /// past <see cref="Capacity"/>.
        /// </summary>
        public sealed class LayoutCache
        {
            public const int Capacity = 32;
            const string kVersion = "gvcal1";
            readonly List<CacheEntry> m_entries = new List<CacheEntry>();

            public int Count => m_entries.Count;

            public bool TryGet(in LayoutKey key, int texW, int texH, out CacheEntry entry)
            {
                for (int i = m_entries.Count - 1; i >= 0; i--)
                {
                    CacheEntry e = m_entries[i];
                    if (!e.Key.SameCached(key)) continue;
                    if (texW <= 0 || texH <= 0 || e.TexW != texW || e.TexH != texH) break;
                    entry = e;
                    return true;
                }
                entry = default;
                return false;
            }

            /// <summary>Add or replace the entry of <paramref name="e"/>'s layout.</summary>
            public void Put(in CacheEntry e)
            {
                for (int i = m_entries.Count - 1; i >= 0; i--)
                    if (m_entries[i].Key.SameCached(e.Key)) m_entries.RemoveAt(i);
                m_entries.Add(e);
                while (m_entries.Count > Capacity) m_entries.RemoveAt(0);
            }

            public void Clear() => m_entries.Clear();

            public string Serialize()
            {
                var ci = System.Globalization.CultureInfo.InvariantCulture;
                var sb = new System.Text.StringBuilder(kVersion);
                foreach (CacheEntry e in m_entries)
                {
                    LayoutKey k = e.Key;
                    sb.Append(';').Append(k.Pane.ToString(ci)).Append(',').Append(k.Monitor.ToString(ci))
                      .Append(',').Append(k.PaneLeft.ToString(ci)).Append(',').Append(k.PaneTop.ToString(ci))
                      .Append(',').Append(k.PaneW.ToString(ci)).Append(',').Append(k.PaneH.ToString(ci))
                      .Append(',').Append(k.Dx.ToString(ci)).Append(',').Append(k.Dy.ToString(ci))
                      .Append(',').Append(k.W.ToString(ci)).Append(',').Append(k.H.ToString(ci))
                      .Append(',').Append(k.Maximized ? '1' : '0').Append(',').Append(k.WholePixel ? '1' : '0')
                      .Append(',').Append(k.Scale.ToString("R", ci))
                      .Append(',').Append(e.TexW.ToString(ci)).Append(',').Append(e.TexH.ToString(ci))
                      .Append(',').Append(e.OriginDx.ToString(ci)).Append(',').Append(e.OriginDy.ToString(ci));
                }
                return sb.ToString();
            }

            /// <summary>The cache <see cref="Serialize"/> wrote. Anything that does not parse
            /// exactly (another version, a damaged entry) is left out: a layout that is not in
            /// the cache is simply measured.</summary>
            public static LayoutCache Parse(string text)
            {
                var cache = new LayoutCache();
                if (string.IsNullOrEmpty(text)) return cache;
                string[] parts = text.Split(';');
                if (parts[0] != kVersion) return cache;
                var ci = System.Globalization.CultureInfo.InvariantCulture;
                const System.Globalization.NumberStyles ints = System.Globalization.NumberStyles.AllowLeadingSign;
                for (int p = 1; p < parts.Length; p++)
                {
                    string[] f = parts[p].Split(',');
                    if (f.Length != 17) continue;
                    var e = new CacheEntry();
                    bool ok = long.TryParse(f[0], ints, ci, out e.Key.Pane) & long.TryParse(f[1], ints, ci, out e.Key.Monitor)
                        & int.TryParse(f[2], ints, ci, out e.Key.PaneLeft) & int.TryParse(f[3], ints, ci, out e.Key.PaneTop)
                        & int.TryParse(f[4], ints, ci, out e.Key.PaneW) & int.TryParse(f[5], ints, ci, out e.Key.PaneH)
                        & int.TryParse(f[6], ints, ci, out e.Key.Dx) & int.TryParse(f[7], ints, ci, out e.Key.Dy)
                        & int.TryParse(f[8], ints, ci, out e.Key.W) & int.TryParse(f[9], ints, ci, out e.Key.H)
                        & (f[10] == "0" || f[10] == "1") & (f[11] == "0" || f[11] == "1")
                        & float.TryParse(f[12], System.Globalization.NumberStyles.Float, ci, out e.Key.Scale)
                        & int.TryParse(f[13], ints, ci, out e.TexW) & int.TryParse(f[14], ints, ci, out e.TexH)
                        & int.TryParse(f[15], ints, ci, out e.OriginDx) & int.TryParse(f[16], ints, ci, out e.OriginDy);
                    if (!ok || e.TexW < MinTexels || e.TexH < MinTexels) continue;
                    e.Key.Maximized = f[10] == "1"; e.Key.WholePixel = f[11] == "1";
                    cache.Put(e);
                }
                return cache;
            }
        }
    }
}
