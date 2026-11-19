// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

using System;

namespace DisplayXR
{
    /// <summary>
    /// The arithmetic of the editor Game-view glue (#420): where, inside the matched Game-view
    /// pane, Unity draws the render target, and therefore where the invisible weave anchor goes.
    /// Pure C# (no UnityEngine) so it is unit-tested in CI (<c>Tests~/GameViewPlacement</c>);
    /// <c>DisplayXRProviderDriver</c> gathers the inputs (Win32 rects, reflection into the
    /// GameView) and owns the latches and the logging.
    ///
    /// All sizes are PHYSICAL px of the pane's own monitor, except the <c>*Pt</c> inputs, which
    /// are Unity points; <c>ptToPx</c> is the pane's own points-to-px, never the global
    /// pixelsPerPoint (a 300 % secondary with a 100 % primary has 3 and 1).
    /// Rounding reproduces Unity's Mathf: Round / RoundToInt are Math.Round (to even) on the float.
    /// </summary>
    internal static class DisplayXRGameViewPlacement
    {
        /// <summary>(#740) Unity draws the docked RT above a bottom margin. HW-measured as 4 px at
        /// 250 % (1.6 pt); really DockArea.kBottomBorders = 2 pt less rounding.</summary>
        public const float RtBottomMarginPt = 1.6f;

        /// <summary>The plausibility bound of the #740 X latch, and the horizontal slack the
        /// chrome-slack path accepts when the pane's scale is implausible.</summary>
        public const int ChromeSlackMaxPx = 64;

        /// <summary>Vertical slack that is rounding, not a letterbox: the vertical chrome is
        /// already out of the area (the toolbar term holds the tab, the toolbar AND the bottom
        /// border), so a Free Aspect RT matches it to 0 px at 300 %, -1 at 250 %.</summary>
        public const int RoundingSlackMaxPx = 2;

        /// <summary>DockArea.kBottomBorders: the border BELOW a docked, non-maximized pane's
        /// render area. It is inside the host height, so host - drawRect height = tab strip +
        /// Game-view toolbar + this.</summary>
        public const float DockBottomBorderPt = 2f;

        /// <summary>DockArea's side borders, 1 pt left and right of a docked pane's render area
        /// (host width - drawRect width).</summary>
        public const float DockSideBordersPt = 2f;

        /// <summary>
        /// (#420) Where the docked anchor sits relative to the screen row that shows the RT's
        /// row 0, in physical px; 0 = on it. A docked anchor is placed at that row computed from
        /// Unity's own rule (<see cref="RtRow0Y"/>), plus this. 0 is the #740 design target (the
        /// woven texture lands where the weaver thinks it is) and what the hardware-verified 250 %
        /// docked placement (#421) already had. The old #740 term (<see cref="RtBottomMarginPt"/>)
        /// and the letterbox arithmetic on top of it put the anchor anywhere from 3 rows above to
        /// 3 rows below row 0, by scale and pane height (Free Aspect at 300 %: 0, +1 or +2 by the
        /// pane height mod 3). Measured 2026-10-27: +2 on a 300 % main DS1, with crosstalk seen in
        /// the right view; +1 on a 300 % secondary; -1..+1 at 150 %; +3 looked inverted on 10-26.
        /// One constant for every scale and pane: if the eye prefers another row, change it here.
        /// </summary>
        public const int DockedRowBiasPx = 0;

        static float Round(float v) => (float)Math.Round(v);
        static int RoundToInt(float v) => (int)Math.Round(v);
        static int FloorToInt(float v) => (int)Math.Floor(v);

        /// <summary>ptToPx is MEASURED (pane px / pane pt, e.g. 2213 / 737.6666 = 3.0000003) while
        /// Unity converts with the exact backing scale; a whole-percent Windows scale is a multiple
        /// of 0.01, so snap to it, otherwise a Round(x.5) tie can land on the wrong side.</summary>
        static float SnapScale(float ptToPx)
        {
            float snapped = Round(ptToPx * 100f) / 100f;
            return Math.Abs(snapped - ptToPx) < 1e-3f ? snapped : ptToPx;
        }

        /// <summary>
        /// (#459) The pane's points-to-px from its own two widths: physical px (the pane
        /// window, read under per-monitor-v2 awareness) over Unity points (the host view).
        /// The px width is a WHOLE number while points x scale is not, so the quotient is off
        /// by up to one pixel's worth: 961 px / 549.7142 pt = 1.74818 at 175 % (549.7142 x
        /// 1.75 = 961.99997, which Unity made 961). That is 0.0018 from 1.75, outside the fixed
        /// 0.001 the snap below allows, so on a pane narrower than about 1000 px the scale read
        /// "not a whole percent": no whole-pixel step, and a held whole-pixel size was given up
        /// (harness, 175 %: the view sat on Free Aspect, not 1:1, for a whole resize step).
        /// So: a scale that predicts the pane's px width to within one pixel IS the scale -
        /// the nearest multiple of 25 % first (what Windows offers; on a pane under about
        /// 200 pt one pixel is more than half a percent, and the nearest whole percent can be
        /// the wrong neighbour), then the nearest whole percent. Otherwise (a custom scale such
        /// as 133.33 %) the quotient is returned.
        /// </summary>
        public static float PaneScale(int panePx, float hostPt)
        {
            if (panePx <= 0 || !(hostPt > 1f)) return 0f;
            float measured = panePx / hostPt;
            float quarter = Round(measured * 4f) / 4f;
            if (quarter > 0f && Math.Abs(quarter * hostPt - panePx) <= 1.0001f) return quarter;
            float percent = Round(measured * 100f) / 100f;
            return Math.Abs(percent * hostPt - panePx) <= 1.0001f ? percent : measured;
        }

        /// <summary>The #740 Y term: the anchor moves UP by the dock's bottom margin. Negative.
        /// Since #420 only the fallback (no draw rect, or a GL device) still uses it.</summary>
        public static int BottomMarginPx(float ptToPx) => -RoundToInt(RtBottomMarginPt * ptToPx);

        /// <summary>The #740 X term: Unity centres the RT in the pane; with an odd slack the
        /// extra pixel goes LEFT of the RT (5 px of slack measured as +3).</summary>
        public static int CentringX(int slackPx) => (slackPx + 1) / 2;

        /// <summary>Tab strip + Game-view toolbar + (docked) bottom border, from Unity's own
        /// HostView.screenPosition height and zoom-area drawRect height. The chrome is whole
        /// points and the scale is snapped, so a tie (47 pt x 1.5 = 70.5 px) always rounds the
        /// same way; with the measured scale it went 70 or 71 with float noise (2026-10-27).</summary>
        public static int ToolbarPx(float hostHPt, float drawHPt, float ptToPx) =>
            RoundToInt((float)Math.Round(hostHPt - drawHPt, 2) * SnapScale(ptToPx));

        /// <summary>One matched pane, as the driver sees it this frame.</summary>
        public struct Pane
        {
            public int Left, Top, ClientW, ClientH; // the pane's window origin and client size
            public int ToolbarPx;                   // ToolbarPx(...) or the driver's fallback
            public int RtW, RtH;                    // Unity's render-target size
            public float TargetHPx;                 // its height as Unity reports it, px (0 = RtH):
                                                    // a Free Aspect / left-right-bar target is the
                                                    // area, e.g. 1079.5 at 150 %
            public bool DockedNonMaximized;         // docked in the main window, not maximized
            public bool HaveDrawRect;               // the drawRect reflection resolved
            public float HostWPt, DrawWPt, DrawHPt; // Unity points (valid when HaveDrawRect)
            public float HostHPt;                   // HostView.screenPosition height, points
            public float PtToPx;
            public bool RoundsBottomEdge;           // SystemInfo.graphicsUVStartsAtTop
            // (#459) Opt-in whole-pixel experiment only; all false / 0 = the placement above,
            // unchanged. WholePixelFixedSize: the view shows the plug-in's Fixed Resolution
            // size at zoom 1 with GVFIT off, so it is always a letterbox (never chrome slack)
            // and its rows are not stretched. LeftBorderPt (valid when HaveLeftBorder): the
            // pane's real left border, EditorWindow.position.x - HostView.screenPosition.x.
            public bool WholePixelFixedSize;
            public bool HaveLeftBorder;
            public float LeftBorderPt;
            // (#459) The view's zoom (GameView.m_ZoomArea.m_Scale.x), 0 = not known (treated
            // as 1). Anything but 1 means Unity draws the texture SCALED: see PlaceScaled.
            public float ZoomX;
        }

        /// <summary>True when the docked anchor's Y comes from <see cref="RtRow0Y"/>: a docked,
        /// non-maximized pane on a UV-starts-at-top device with Unity's draw rect at hand.</summary>
        static bool UsesRtRow0(in Pane p) =>
            p.DockedNonMaximized && p.HaveDrawRect && p.RoundsBottomEdge && p.RtH > 1
            && p.PtToPx > 0.5f && p.PtToPx < 8f && p.DrawHPt > 1f
            && p.HostHPt - p.DrawHPt > DockBottomBorderPt;

        /// <summary>
        /// (#420) The screen row that shows row 0 of the RT in a docked pane on a
        /// UV-starts-at-top device (D3D11/12, Vulkan, Metal). Unity's rule, as every docked
        /// capture at 300 % and 150 % shows (2026-10-26/27): GameView.OnGUI centres the target
        /// in the zoom area, whose top is the tab strip + toolbar below the pane top; the plug-in's
        /// GVFIT sets the zoom's scale.y to RtH / (RtH - 0.5); the flipped rect's y, i.e. the
        /// BOTTOM edge, is rounded to whole points; the RT's RtH rows then end on the first device
        /// row at or below that edge. At 150 % the edge is often x.5 px, which the old
        /// letterbox arithmetic (no GVFIT term in the rounding, a pixel-rounded difference) put
        /// one row too high (B2, R7) or too low. The height Unity centres is its target height
        /// (<see cref="Pane.TargetHPx"/>), which is the area's, x.5 px at 150 %, for Free Aspect
        /// and left/right bars, and whole px for top/bottom bars.
        /// </summary>
        public static int RtRow0Y(in Pane p)
        {
            double s = SnapScale(p.PtToPx);
            double targetHPt = (p.TargetHPx > 0f ? p.TargetHPx : p.RtH) / s;
            double topPt = p.HostHPt - p.DrawHPt - DockBottomBorderPt;  // tab strip + toolbar
            double stretch = p.WholePixelFixedSize ? 1.0                 // (#459) zoom 1, GVFIT off
                                                   : p.RtH / (p.RtH - 0.5); // GVFIT scale.y
            double bottomPt = Math.Round((p.DrawHPt + targetHPt * stretch) * 0.5);
            double bottomPx = p.Top + (topPt + bottomPt) * s;
            return (int)Math.Ceiling(bottomPx - 1e-3) - p.RtH;
        }

        /// <summary>(#420) The most horizontal slack that is dock chrome: the two side borders
        /// plus rounding (8 px at 300 %, 7 at 250 %, 5 at 150 %). A Free Aspect RT is the area's
        /// width, so it leaves the borders (6 / 5 / 3 px); anything wider is a left/right bar.</summary>
        public static int SideChromeSlackMaxPx(float ptToPx) =>
            ptToPx > 0.5f && ptToPx < 8f
                ? RoundToInt(DockSideBordersPt * SnapScale(ptToPx)) + RoundingSlackMaxPx
                : ChromeSlackMaxPx;

        /// <summary>True when the RT fills the pane up to dock chrome (Free Aspect): the
        /// HW-verified #740 bottom-aligned placement applies instead of letterbox centring.
        /// Thin left/right bars are a letterbox: up to 64 px of them used to count as chrome, and
        /// the #740 pixel centring then put X 1-2 px off Unity's whole-point rule (#420).</summary>
        public static bool IsChromeSlackOnly(in Pane p)
        {
            int areaH = p.ClientH - p.ToolbarPx;
            return !p.WholePixelFixedSize && p.DockedNonMaximized && p.RtW > 0 && p.RtH > 0
                && p.ClientW - p.RtW >= 0 && p.ClientW - p.RtW <= SideChromeSlackMaxPx(p.PtToPx)
                && areaH - p.RtH >= -RoundingSlackMaxPx && areaH - p.RtH <= RoundingSlackMaxPx;
        }

        /// <summary>
        /// The anchor rect. <paramref name="rtCentX"/> / <paramref name="rtCentY"/> are the
        /// driver's latched #740 terms (CentringX of the pane slack, BottomMarginPx of ptToPx in
        /// steady state); rtCentX is read only on the chrome-slack path, rtCentY only for a
        /// docked, non-maximized pane that cannot use <see cref="RtRow0Y"/> (no draw rect, GL).
        /// </summary>
        public static void Place(in Pane p, int rtCentX, int rtCentY, out int x, out int y, out int w, out int h)
        {
            x = p.Left;
            y = p.Top + p.ToolbarPx;
            w = p.ClientW;
            h = p.ClientH - p.ToolbarPx;
            if (p.RtW <= 0 || p.RtH <= 0 || w <= 0 || h <= 0) return;
            if (IsScaledView(p.ZoomX)) { PlaceScaled(p, out x, out y, out w, out h); return; }
            bool row0 = UsesRtRow0(p);

            if (IsChromeSlackOnly(p))
            {
                x = p.Left + rtCentX;                               // Unity centres the RT horizontally
                y = row0 ? RtRow0Y(p) + DockedRowBiasPx             // the row of the RT's row 0
                         : p.Top + (p.ClientH - p.RtH) + rtCentY;   // ...above a bottom margin
                w = p.RtW;
                h = p.RtH;
                return;
            }

            // Letterbox: Unity never upscales the RT, it fits it into the area (min 1) and
            // centres it, leaving grey bars. Docked on D3D, an RT up to RoundingSlackMaxPx
            // taller than the area is rounding, not a fit: Unity fits its TARGET, which for
            // left/right bars is the area's own height (859.5 px in an 859 px area at 250 %),
            // and GVFIT owns the rows. Fitting the RT's 860 rows drew the anchor 2 px narrow.
            int fitAreaH = row0 && p.RtH > h && p.RtH - h <= RoundingSlackMaxPx ? p.RtH : h;
            float fit = Math.Min(1f, Math.Min((float)w / p.RtW, (float)fitAreaH / p.RtH));
            int fitW = RoundToInt(p.RtW * fit);
            int fitH = RoundToInt(p.RtH * fit);
            if (fitW <= 0 || fitH <= 0 || (fitW == w && fitH == h)) return;
            int offX = (w - fitW) / 2;   // fallback: pixel centring
            int offY = (h - fitH) / 2;
            if (p.HaveDrawRect && p.PtToPx > 0.5f && p.PtToPx < 8f)
                LetterboxOffsets(p.HostWPt, p.DrawWPt, p.DrawHPt, fitW, fitH, p.PtToPx, p.RoundsBottomEdge,
                                 ref offX, ref offY,
                                 p.WholePixelFixedSize && p.HaveLeftBorder ? p.LeftBorderPt : -1f);
            x += offX;
            y += offY;
            w = fitW;
            h = fitH;
            // Docked: the row Unity really shows the RT's row 0 on (the RT's rows, never fitted
            // in Y: GVFIT owns scale.y). Fallback: the toolbar-inset area has the right height
            // but sits one dock bottom border too low (that border is inside ToolbarPx yet lies
            // BELOW the area); the #740 term moves it up. Maximized: no bottom border, and this
            // path is unchanged; floating: its extra 2 pt is a TOP border.
            if (row0) y = RtRow0Y(p) + DockedRowBiasPx;
            else if (p.DockedNonMaximized) y += rtCentY;
        }

        /// <summary>
        /// Where Unity itself puts a letterboxed RT inside the render area. GameView.OnGUI
        /// centres the target in the zoom area, then rounds the draw rect to WHOLE POINTS:
        /// <c>drawRect.x = Mathf.Round(x); drawRect.y = Mathf.Round(y)</c>; on a UV-starts-at-top
        /// device (D3D11/12, Vulkan, Metal: <paramref name="roundsBottomEdge"/>) the rect is
        /// flipped first, so the y it rounds is the BOTTOM edge. Measured maximized 16:9 on a
        /// 300 % DS1: pixel centring (3840 - 2823) / 2 = 508, Unity Round(169.5 pt) = 170 pt =
        /// 510 px. Inputs are Unity's own numbers: hostWPt = HostView.screenPosition width,
        /// drawWPt / drawHPt = the zoom-area drawRect, so (hostWPt - drawWPt) / 2 is the dock's
        /// side border (1 pt docked, 0 maximized / floating). Y comes back RELATIVE to a
        /// full-area RT under the same rule, so a letterbox keeps exactly the anchor-to-content
        /// relation of the HW-verified Free Aspect placement. Leaves the fallback values
        /// untouched when the inputs are implausible.
        /// </summary>
        public static void LetterboxOffsets(float hostWPt, float drawWPt, float drawHPt, int fitW, int fitH,
                                            float ptToPx, bool roundsBottomEdge, ref int offX, ref int offY,
                                            float leftBorderPt = -1f)
        {
            // (#459) leftBorderPt >= 0: the pane's real left border (whole-pixel experiment);
            // otherwise the two side borders are assumed equal, which a pane docked at the
            // editor window's left or right edge is not (it has one border, not two).
            float side = leftBorderPt >= 0f ? leftBorderPt : (hostWPt - drawWPt) * 0.5f;
            if (!(side >= 0f && side <= 4f) || !(drawHPt > 1f)) return;
            ptToPx = SnapScale(ptToPx);
            float rtWPt = fitW / ptToPx, rtHPt = fitH / ptToPx;
            // pt -> px rounds HALF UP: at 250 % a whole-point edge can fall on x.5 px and Unity's RT
            // then starts on the NEXT pixel (#740: 5 px of slack measured as +3, not +2).
            offX = FloorToInt((side + Round((drawWPt - rtWPt) * 0.5f)) * ptToPx + 0.5f);
            float yPt = roundsBottomEdge ? Round((drawHPt + rtHPt) * 0.5f) - rtHPt
                                         : Round((drawHPt - rtHPt) * 0.5f);
            float fullPt = roundsBottomEdge ? Round(drawHPt) - drawHPt : 0f;
            offY = FloorToInt((yPt - fullPt) * ptToPx + 0.5f);
        }

        // ---- (#459) A zoomed Game view, and what GVFIT may write ---------------------------
        // Everything above assumes the view's zoom is 1: the texture is drawn at its own
        // size. A Fixed Resolution larger than the view area (Unity fits it: zoom below 1) or
        // a Scale slider off 1x draws it SCALED. Then no texel lands on one panel pixel, the
        // weave cannot be right wherever the anchor goes, and the zoom-1 arithmetic is not
        // merely imprecise but wrong: RtRow0Y centres the UNSCALED target, which for a
        // 3840x2160 texture fitted at 0.279 into a 1122x602 px area put the anchor 700 px
        // ABOVE the pane (hardware session, 175 %), and the weave went black.

        /// <summary>A zoom this close to 1 is 1 (Unity writes exactly 1; GVFIT's own row
        /// stretch is 1 + 0.5 / rows, on Y only, and never read through here).</summary>
        public const float ZoomOneTol = 1e-4f;

        /// <summary>True when the view's zoom on X is 1: the texture is drawn unscaled. This is
        /// the only state in which the image can be texel for pixel, GVFIT's row fit is valid
        /// and the on-screen marker can be decoded.</summary>
        public static bool IsZoomOne(float zoomX) => Math.Abs(zoomX - 1f) <= ZoomOneTol;

        /// <summary>True when the zoom is KNOWN (above 0) and is not 1.</summary>
        public static bool IsScaledView(float zoomX) => zoomX > 0f && !IsZoomOne(zoomX);

        /// <summary>
        /// The value GVFIT keeps in the zoom area's <c>m_Scale.y</c>. Unity's own zoom is
        /// uniform (y == x). At zoom 1 with a texture of <paramref name="rtRows"/> rows the
        /// row fit applies: rows / (rows - 0.5). At ANY other zoom the fit is not valid and
        /// the answer is <paramref name="zoomX"/>: Unity's uniform zoom, untouched - which
        /// also puts back a stretch written earlier (the view was zoomed after the fit went
        /// on, or an older plug-in left it in the saved layout). Writing the fit regardless
        /// of x made the zoom anisotropic: (0.279, 1.0002) drew a 4K picture 3.6x too tall.
        /// </summary>
        public static float FitZoomY(float zoomX, int rtRows) =>
            IsZoomOne(zoomX) && rtRows > 1 ? rtRows / (rtRows - 0.5f) : zoomX;

        /// <summary>What GVFIT leaves behind when it stops (Play ends, a domain reload, dock /
        /// undock, the editor quits, or the fit is switched off): Unity's uniform zoom.</summary>
        public static float RestoredZoomY(float zoomX) => zoomX;

        /// <summary>
        /// The anchor rect for a view whose zoom is not 1. The picture cannot be 1:1, so this
        /// only has to be SAFE and roughly where Unity draws: the texture x the zoom, centred
        /// in the render area and cut to it (a zoom above 1 shows a centred crop that fills
        /// the area). Always inside the render area, never above or left of the pane.
        /// </summary>
        public static void PlaceScaled(in Pane p, out int x, out int y, out int w, out int h)
        {
            int areaW = p.ClientW, areaH = p.ClientH - p.ToolbarPx;
            x = p.Left; y = p.Top + p.ToolbarPx; w = areaW; h = areaH;
            if (areaW <= 0 || areaH <= 0 || p.RtW <= 0 || p.RtH <= 0 || !(p.ZoomX > 0f)) return;
            int drawnW = Math.Max(1, (int)Math.Round((double)p.RtW * p.ZoomX));
            int drawnH = Math.Max(1, (int)Math.Round((double)p.RtH * p.ZoomX));
            w = Math.Min(areaW, drawnW);
            h = Math.Min(areaH, drawnH);
            x += (areaW - w) / 2;
            y += (areaH - h) / 2;
        }

        /// <summary>The view area counts as the same size within this many points.</summary>
        public const float SameAreaTolPt = 0.05f;

        /// <summary>
        /// (#459) Can the Game view's scroll (ZoomableArea.m_Translation) recorded before Play
        /// be expected back after it? Only when the view AREA is the size it was then. The
        /// scroll is not free: on every OnGUI GameView.EnforceZoomAreaConstraints re-centres
        /// content that is smaller than the area (shownArea.x = -shownArea.width / 2, i.e. a
        /// translation of HALF THE AREA: 342.43 pt for a 684.86 pt area) and clamps the rest
        /// to the content. After a splitter drag during Play the recorded value is therefore
        /// no longer one Unity will hold (hardware: "scroll (359,171.36) instead of
        /// (342.43,171.93) after 28 re-applications" - 359 is half of the new 718 pt area).
        /// Then the size and zoom are restored and Unity places the scroll. A record without
        /// an area (written by an older build) keeps the exact check.
        /// </summary>
        public static bool ScrollIsComparable(float recordedWPt, float recordedHPt, float nowWPt, float nowHPt)
        {
            if (!(recordedWPt > 0f) || !(recordedHPt > 0f)) return true;
            return Math.Abs(nowWPt - recordedWPt) <= SameAreaTolPt && Math.Abs(nowHPt - recordedHPt) <= SameAreaTolPt;
        }

        /// <summary>How far outside the pane an anchor may sit before it is called wrong: the
        /// zoom-1 placement legitimately leaves the render area by a rounding pixel or two.</summary>
        public const int PaneSlackPx = 8;

        /// <summary>
        /// True when the anchor rect lies in the pane (within <see cref="PaneSlackPx"/>).
        /// Otherwise it is replaced by the pane's render area: an anchor off the pane is
        /// never right, and on hardware it showed a black Game view instead of a picture.
        /// </summary>
        public static bool KeepInsidePane(in Pane p, ref int x, ref int y, ref int w, ref int h)
        {
            int s = PaneSlackPx;
            bool inside = x >= p.Left - s && y >= p.Top - s
                       && x + w <= p.Left + p.ClientW + s && y + h <= p.Top + p.ClientH + s;
            if (inside) return true;
            x = p.Left; y = p.Top + p.ToolbarPx;
            w = p.ClientW; h = p.ClientH - p.ToolbarPx;
            return false;
        }

        /// <summary>What a Game view that cannot be 1:1 needs done to it during Play.</summary>
        public enum Takeover
        {
            None,           // zoom 1: nothing
            SnapZoom,       // the selected size fits the view at 1x: set the zoom to 1
            FreeAspect,     // it does not (Unity's smallest zoom for it is below 1): Free Aspect, zoom 1
        }

        /// <summary>
        /// (#459) Decide what to do with a Game view whose zoom is not 1. <paramref name="zoomX"/>:
        /// its zoom; <paramref name="fitScale"/>: the largest zoom at which the selected size
        /// still fits the view area (area / target on the tighter axis; 1 or more for Free
        /// Aspect and for a size that fits, below 1 for a fixed size larger than the area -
        /// Unity will not let that one be shown at 1x).
        /// </summary>
        public static Takeover TakeoverFor(float zoomX, float fitScale)
        {
            if (!IsScaledView(zoomX)) return Takeover.None;
            return fitScale >= 1f - ZoomOneTol ? Takeover.SnapZoom : Takeover.FreeAspect;
        }

        // ---- (#459) Is the Game view drawn 1:1, and the whole-point-and-pixel size ----
        // The weave is only right if every texel of the Game view's render texture lands on
        // exactly one panel pixel. Unity draws the texture into targetInView, whose size in
        // px is its TARGET size (fractional for Free Aspect: the area) times the zoom area's
        // scale, with point filtering; the texture's own size is whole px. Where the two
        // differ, a column (row) is dropped or doubled and everything past it sits one pixel
        // off, which on a lenticular panel is the other eye.
        // Unity allocates the texture as its target rounded half AWAY FROM ZERO
        // (PlayModeView.RenderView, read from the 6000.4 editor assembly), so for a docked
        // Free Aspect view (2 pt of side borders, 47 pt of chrome above and below):
        //   Windows scale   area W   area H   texture vs area     rows drawn under GVFIT
        //   100 200 300 %   whole    whole    equal               h + 0.5
        //   150 250 %       whole    x.5      H 0.5 taller        h (exact)
        //   175 %           x.5      x.75     W 0.5, H 0.25 over  h + 0.25
        //   125 225 %       x.5      x.25     W 0.5 over, H 0.25 under   h + 0.75
        // GVFIT stretches Y by h / (h - 0.5): exact for the half row of 150 / 250 % and nothing
        // else. These are arithmetic; points x scale in float can land a hair below x.5 and
        // round the other way, which is why the driver's diagnostic reads the REAL texture.
        // See docs~/architecture/editor-game-view-placement.md.

        /// <summary>A drawn size within this of the texture size counts as equal: the smallest
        /// real mismatch is a quarter pixel (125 %), float noise is ~1e-3.</summary>
        public const double OneToOneTolPx = 0.05;

        /// <summary>The largest step (points) <see cref="WholePixelStepPt"/> can return: at a
        /// whole-percent scale 100 points are always a whole number of pixels.</summary>
        public const int WholePixelMaxStepPt = 100;

        /// <summary>The pane's scale as Unity uses it: the measured px / pt snapped to a whole
        /// percent (see SnapScale).</summary>
        public static float SnappedScale(float ptToPx) => SnapScale(ptToPx);

        /// <summary>The size, in px, of the rect Unity draws the render texture into along one
        /// axis: the target size in px (GetMainPlayModeViewTargetSize, fractional for Free
        /// Aspect) times the zoom area's scale on that axis. Equal to targetInView's size times
        /// the pane's scale, without depending on EditorGUIUtility.pixelsPerPoint, which is
        /// only valid inside OnGUI.</summary>
        public static double DrawnPx(double targetPx, double zoomScale) => targetPx * zoomScale;

        /// <summary>True when a texture <paramref name="rtPx"/> wide (high) is drawn into a rect
        /// of the same size, i.e. texel for pixel.</summary>
        public static bool IsOneToOne(double drawnPx, int rtPx) =>
            rtPx > 0 && Math.Abs(drawnPx - rtPx) < OneToOneTolPx;

        /// <summary>How many columns (rows) the mismatch costs at most: negative = dropped (the
        /// rect is smaller than the texture), positive = doubled, 0 = drawn 1:1. Whether a
        /// sub-pixel mismatch really costs one depends on where the rect's edge falls
        /// (<see cref="LinesShown"/>).</summary>
        public static int LinesDroppedOrDoubled(double drawnPx, int rtPx)
        {
            double d = drawnPx - rtPx;
            if (rtPx <= 0 || Math.Abs(d) < OneToOneTolPx) return 0;
            int n = (int)Math.Ceiling(Math.Abs(d) - OneToOneTolPx);
            return d < 0 ? -n : n;
        }

        /// <summary>How many device columns (rows) a drawn rect covers when it starts at the
        /// fractional pixel <paramref name="edgePx"/>: each edge lands on the next whole pixel
        /// at or after it (the rule a 175 % screen capture showed for the first column: edge
        /// 1501.5 -> column 1502, 1326.75 -> 1327, and the one <see cref="RtRow0Y"/> uses for the
        /// last row). An 879.5 px rect from 1501.5 covers 879 columns of an 880 px texture.</summary>
        public static int LinesShown(double edgePx, double drawnPx) =>
            (int)Math.Ceiling(edgePx + drawnPx - 1e-3) - (int)Math.Ceiling(edgePx - 1e-3);

        /// <summary>A pane border of <paramref name="borderPt"/> points in whole px, with the
        /// half-up rule of <see cref="LetterboxOffsets"/> (1 pt at 250 % = 2.5 px -> 3, the
        /// measured #740 value). The quarter-pixel case (x.25 at 125 % / 175 %) is unmeasured,
        /// and this rounds it DOWN while <see cref="RtRow0Y"/> and <see cref="LinesShown"/> take
        /// the next pixel UP: X and Y disagree there by construction.</summary>
        public static int BorderPx(float borderPt, float ptToPx) =>
            FloorToInt(borderPt * SnapScale(ptToPx) + 0.5f);

        // The pane's scale as a whole percent (175 for 1.75), or 0 when it is not one (the
        // measured px / pt is within 0.1 % of a whole percent on any real pane) or implausible.
        static int ScalePercent(float ptToPx)
        {
            double pct = (double)ptToPx * 100.0;
            int whole = (int)Math.Round(pct);
            if (!(pct > 50.0 && pct < 800.0) || Math.Abs(pct - whole) > 0.1) return 0;
            return whole;
        }

        static int Gcd(int a, int b) { while (b != 0) { int t = a % b; a = b; b = t; } return a; }

        /// <summary>The smallest number of points that is EXACTLY a whole number of pixels at
        /// this scale: 100 / gcd(percent, 100). 1 at 100 / 200 / 300 %, 2 at 150 % (3 px) and
        /// 250 % (5 px), 4 at 125 % (5 px), 175 % (7 px) and 225 % (9 px), 10 at 110 %, 100 at
        /// a custom 133 % (3 pt would be 3.99 px: not whole). Integer arithmetic on the whole
        /// percent, no tolerance. 0 when the scale is not a whole percent or is implausible.</summary>
        public static int WholePixelStepPt(float ptToPx)
        {
            int pct = ScalePercent(ptToPx);
            return pct == 0 ? 0 : 100 / Gcd(pct, 100);
        }

        /// <summary>
        /// The largest Game-view size that fits a view area of <paramref name="viewWPt"/> x
        /// <paramref name="viewHPt"/> points (GameView.viewInWindow) and is a whole number of
        /// points AND of pixels on both axes: a multiple of <see cref="WholePixelStepPt"/>. A
        /// Fixed Resolution of that size at zoom 1 is drawn texel for pixel (measured at 175 %:
        /// 770x1127 px = 440x644 pt, every column on screen); it leaves a margin of less than
        /// one step. False when the scale needs no such size (a whole scale: step 1), is not a
        /// whole percent (step 0), or the area is smaller than one step.
        /// </summary>
        public static bool WholePixelSize(float viewWPt, float viewHPt, float ptToPx,
                                          out int wPt, out int hPt, out int wPx, out int hPx)
        {
            wPt = hPt = wPx = hPx = 0;
            int q = WholePixelStepPt(ptToPx);
            if (q <= 1) return false;
            int pct = ScalePercent(ptToPx);
            wPt = (int)Math.Floor(viewWPt / q + 0.001) * q;
            hPt = (int)Math.Floor(viewHPt / q + 0.001) * q;
            if (wPt <= 0 || hPt <= 0) { wPt = hPt = 0; return false; }
            wPx = wPt * pct / 100;   // exact: wPt is a multiple of the step
            hPx = hPt * pct / 100;
            return true;
        }

        // ---- Texture target binding (#471): what phases the weave ---------------------
        // The numbers below mirror native~/displayxr_xrprovider/displayxr_provider_texture_target.h
        // (DXR_TT_WHY_*, DXR_TT_MISMATCH_*) and the runtime's state flags.

        /// <summary>XrWin32TextureTargetStateFlagsDXR: phased from the target rect, no window.</summary>
        public const uint TargetStateWindowlessPhase = 0x1;
        /// <summary>...: the display processor cannot phase windowless; the anchor carried the weave.</summary>
        public const uint TargetStateAnchorFallback = 0x2;
        public const uint TargetStateSegmented = 0x4;
        public const uint TargetStateFlatSegment = 0x8;
        public const uint TargetStateClamped = 0x10;

        /// <summary>
        /// The phase source, as appended to the <c>GameView 1:1:</c> line. With
        /// <paramref name="chained"/> false the anchor window phases the weave, as before
        /// spec 9, and <paramref name="whyNot"/> (DXR_TT_WHY_*) says why. Chained, the
        /// runtime's reported <paramref name="flags"/> decide: WINDOWLESS_PHASE = the target
        /// rect itself; ANCHOR_FALLBACK = the anchor window, by the runtime's choice; neither
        /// = the runtime ignored the binding (a session that cannot honour it).
        /// </summary>
        public static string PhaseSourceText(bool chained, int whyNot, uint flags, bool reported,
                                             bool matched, int mismatch, int wovenX, int wovenY,
                                             int requestedX, int requestedY)
        {
            if (!chained)
            {
                string why;
                switch (whyNot)
                {
                    case 1: why = "no session"; break;
                    case 2: why = "DISPLAYXR_PROV_TEXTURE_TARGET=0"; break;
                    case 3: why = "not the editor texture mode"; break;
                    case 4: why = "the runtime predates XR_DXR_win32_window_binding 9"; break;
                    case 5: why = "the runtime does not support it here"; break;
                    case 6: why = "no rect before the session started"; break;
                    case 7: why = "no top-level window for the pane"; break;
                    case 8: why = "the runtime refused the binding"; break;
                    default: why = "reason " + whyNot; break;
                }
                return "anchor window (texture target not chained: " + why + ")";
            }
            string s;
            if ((flags & TargetStateWindowlessPhase) != 0) s = "texture target rect, WINDOWLESS_PHASE";
            else if ((flags & TargetStateAnchorFallback) != 0) s = "anchor window (runtime ANCHOR_FALLBACK: the display processor cannot phase without a window)";
            else s = "anchor window (the runtime did not bind the texture target)";
            if ((flags & TargetStateSegmented) != 0) s += " SEGMENTED";
            if ((flags & TargetStateFlatSegment) != 0) s += " FLAT_SEGMENT";
            if ((flags & TargetStateClamped) != 0) s += " CLAMPED";
            if (reported)
            {
                s += " | woven origin " + wovenX + "," + wovenY;
                if (matched)
                    s += (mismatch & 1) != 0 ? " but requested " + requestedX + "," + requestedY + " (DIFFERS)" : " (= requested)";
                if ((mismatch & 2) != 0) s += " rect DIFFERS";
            }
            else if (flags != 0) s += " | nothing woven yet";
            return s;
        }
    }
}
