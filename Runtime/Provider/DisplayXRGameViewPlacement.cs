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

        /// <summary>Most dock chrome the horizontal "chrome slack" path accepts (and the
        /// plausibility bound of the #740 X latch).</summary>
        public const int ChromeSlackMaxPx = 64;

        /// <summary>Vertical slack that is rounding, not a letterbox: the vertical chrome is
        /// already out of the area (the toolbar term holds the tab, the toolbar AND the bottom
        /// border), so a Free Aspect RT matches it to 0 px at 300 %, -1 at 250 %.</summary>
        public const int RoundingSlackMaxPx = 2;

        static float Round(float v) => (float)Math.Round(v);
        static int RoundToInt(float v) => (int)Math.Round(v);
        static int FloorToInt(float v) => (int)Math.Floor(v);

        /// <summary>The #740 Y term: the anchor moves UP by the dock's bottom margin. Negative.</summary>
        public static int BottomMarginPx(float ptToPx) => -RoundToInt(RtBottomMarginPt * ptToPx);

        /// <summary>The #740 X term: Unity centres the RT in the pane; with an odd slack the
        /// extra pixel goes LEFT of the RT (5 px of slack measured as +3).</summary>
        public static int CentringX(int slackPx) => (slackPx + 1) / 2;

        /// <summary>Tab strip + Game-view toolbar + (docked) bottom border, from Unity's own
        /// HostView.screenPosition height and zoom-area drawRect height.</summary>
        public static int ToolbarPx(float hostHPt, float drawHPt, float ptToPx) =>
            RoundToInt((hostHPt - drawHPt) * ptToPx);

        /// <summary>One matched pane, as the driver sees it this frame.</summary>
        public struct Pane
        {
            public int Left, Top, ClientW, ClientH; // the pane's window origin and client size
            public int ToolbarPx;                   // ToolbarPx(...) or the driver's fallback
            public int RtW, RtH;                    // Unity's render-target size
            public bool DockedNonMaximized;         // docked in the main window, not maximized
            public bool HaveDrawRect;               // the drawRect reflection resolved
            public float HostWPt, DrawWPt, DrawHPt; // Unity points (valid when HaveDrawRect)
            public float PtToPx;
            public bool RoundsBottomEdge;           // SystemInfo.graphicsUVStartsAtTop
        }

        /// <summary>True when the RT fills the pane up to dock chrome (Free Aspect): the
        /// HW-verified #740 bottom-aligned placement applies instead of letterbox centring.</summary>
        public static bool IsChromeSlackOnly(in Pane p)
        {
            int areaH = p.ClientH - p.ToolbarPx;
            return p.DockedNonMaximized && p.RtW > 0 && p.RtH > 0
                && p.ClientW - p.RtW >= 0 && p.ClientW - p.RtW <= ChromeSlackMaxPx
                && areaH - p.RtH >= -RoundingSlackMaxPx && areaH - p.RtH <= RoundingSlackMaxPx;
        }

        /// <summary>
        /// The anchor rect. <paramref name="rtCentX"/> / <paramref name="rtCentY"/> are the
        /// driver's latched #740 terms (CentringX of the pane slack, BottomMarginPx of ptToPx in
        /// steady state); rtCentX is read only on the chrome-slack path, rtCentY only for a
        /// docked, non-maximized pane.
        /// </summary>
        public static void Place(in Pane p, int rtCentX, int rtCentY, out int x, out int y, out int w, out int h)
        {
            x = p.Left;
            y = p.Top + p.ToolbarPx;
            w = p.ClientW;
            h = p.ClientH - p.ToolbarPx;
            if (p.RtW <= 0 || p.RtH <= 0 || w <= 0 || h <= 0) return;

            if (IsChromeSlackOnly(p))
            {
                x = p.Left + rtCentX;                               // Unity centres the RT horizontally
                y = p.Top + (p.ClientH - p.RtH) + rtCentY;          // ...above a bottom margin
                w = p.RtW;
                h = p.RtH;
                return;
            }

            // Letterbox: Unity never upscales the RT, it fits it into the area (min 1) and
            // centres it, leaving grey bars.
            float fit = Math.Min(1f, Math.Min((float)w / p.RtW, (float)h / p.RtH));
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
            // The toolbar-inset area has the right height but sits one dock bottom border too
            // low (that border is inside ToolbarPx yet lies BELOW the area); the same #740 term
            // the chrome-slack path uses moves it up. Maximized: no bottom border; floating:
            // its extra 2 pt is a TOP border (kFloatingWindowTopBorderWidth).
            if (p.DockedNonMaximized) y += rtCentY;
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
            // ptToPx is MEASURED (pane px / pane pt, e.g. 2213 / 737.6666 = 3.0000003) while Unity
            // converts with the exact backing scale; a whole-percent Windows scale is a multiple of
            // 0.01, so snap to it, otherwise a Round(x.5) tie can land on the wrong side.
            float snapped = Round(ptToPx * 100f) / 100f;
            if (Math.Abs(snapped - ptToPx) < 1e-3f) ptToPx = snapped;
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
