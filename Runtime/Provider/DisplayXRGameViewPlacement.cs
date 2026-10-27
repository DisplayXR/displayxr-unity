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
            double stretch = p.RtH / (p.RtH - 0.5);                      // GVFIT scale.y
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
            return p.DockedNonMaximized && p.RtW > 0 && p.RtH > 0
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
                                 ref offX, ref offY);
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
                                            float ptToPx, bool roundsBottomEdge, ref int offX, ref int offY)
        {
            float side = (hostWPt - drawWPt) * 0.5f;
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
    }
}
