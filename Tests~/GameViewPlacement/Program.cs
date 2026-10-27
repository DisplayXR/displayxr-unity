// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for DisplayXRGameViewPlacement - where the editor Game-view glue anchors the weave (#420).
// Hand-rolled (no test framework) so it also runs from a bare `csc` + the .NET runtime that ships
// inside the Unity editor. Exit code 0 = all passed.
//
// HARDWARE: the panes logged on an Acer DS1 (Windows, Unity 6000.4.6f1) at 300 % (main and secondary)
// and 150 %, with the rows and columns where physical-pixel captures showed Unity drawing the RT
// (2026-10-27). These are the ground truth: a docked anchor must sit on the screen row that shows the
// RT's row 0 (+ DockedRowBiasPx), and on the RT's first column.
// PINNED: the same rule for the 10-26 panes, the replay's 125 % / 150 % / 250 % panes and the #427
// review case (300 % secondary, 100 % primary), each also checked against Unity's own rule,
// implemented independently below:
//    DockArea borders (L/R 1 pt, top 24 pt, docked bottom 2 pt; maximized: top 24 only), Game-view
//    toolbar 21 pt, GameView.OnGUI centring the target in the zoom area and rounding the FLIPPED
//    rect's x / y (= the bottom edge on D3D) to whole points, default zoom = min(1, area / target)
//    in X, and in Y the plug-in's GVFIT scale h / (h - 0.5), which replaces the default there. The
//    target height is Unity's mainTargetSize: the area (x.5 px at 150 %) for Free Aspect and
//    left/right bars, whole px for top/bottom bars. The RT's h rows end on the
//    first device row at or below the rounded bottom edge: every docked capture at 300 % and 150 %
//    shows exactly that (at 150 % the edge is often x.5 px; quarter-px edges at 125 % / 175 % are
//    modelled, not measured).
// A docked anchor must equal that row 0 (+ DockedRowBiasPx); a maximized one (a path #420 leaves
// alone) must land in [-0.5, +1.5] px of the model's RT origin; X within 0.75 px (the #740
// (slack + 1) / 2 rule on a quarter-pixel border).

using System;
using DisplayXR;

static class Program
{
    static int s_pass, s_fail;
    // The logged Unity point sizes carry 4 decimals (340.3333), so the model's origin is off by ~3e-5 px.
    const double Tol = 0.01;

    static void Check(bool ok, string what)
    {
        if (ok) s_pass++;
        else { s_fail++; Console.WriteLine("FAIL " + what); }
    }

    struct Geo
    {
        public string Name; public int L, T, W, H; public float Hw, Hh; public int RtW, RtH; public bool Max;
        public float Target; // Unity's target height (mainTargetSize), px; 0 = RtH
        public float Scale;  // Unity's backing scale for the pane (the model's input; the code measures W / Hw)
    }

    static DisplayXRGameViewPlacement.Pane PaneOf(Geo g, bool roundsBottomEdge = true, bool haveDraw = true)
    {
        float ptToPx = g.W / g.Hw;
        float drawH = g.Hh - (g.Max ? 45f : 47f), drawW = g.Hw - (g.Max ? 0f : 2f);
        return new DisplayXRGameViewPlacement.Pane
        {
            Left = g.L, Top = g.T, ClientW = g.W, ClientH = g.H,
            ToolbarPx = DisplayXRGameViewPlacement.ToolbarPx(g.Hh, drawH, ptToPx),
            RtW = g.RtW, RtH = g.RtH, TargetHPx = g.Target, DockedNonMaximized = !g.Max,
            HaveDrawRect = haveDraw, HostWPt = g.Hw, DrawWPt = drawW, DrawHPt = drawH, HostHPt = g.Hh,
            PtToPx = ptToPx, RoundsBottomEdge = roundsBottomEdge,
        };
    }

    // What the driver's #740 latches hold in steady state for this pane.
    static void Place(Geo g, out int x, out int y, out int w, out int h, bool roundsBottomEdge = true, bool haveDraw = true)
    {
        var p = PaneOf(g, roundsBottomEdge, haveDraw);
        int slack = g.W - g.RtW;
        int centX = DisplayXRGameViewPlacement.IsChromeSlackOnly(p) && slack > 0 ? DisplayXRGameViewPlacement.CentringX(slack) : 0;
        int centY = p.DockedNonMaximized ? DisplayXRGameViewPlacement.BottomMarginPx(p.PtToPx) : 0;
        DisplayXRGameViewPlacement.Place(p, centX, centY, out x, out y, out w, out h);
    }

    // Unity's RT origin (fractional px), the independent model described at the top, and the screen
    // row that shows the RT's row 0 (its h rows end on the first row at or below the bottom edge).
    static void UnityOrigin(Geo g, out double ox, out double oy, out int row0)
    {
        double s = g.Scale, lb = g.Max ? 0 : 1, tb = 24;
        double wPt = g.Hw - (g.Max ? 0 : 2), hPt = g.Hh - (g.Max ? 45 : 47);
        double rtW = g.RtW / s, h = g.RtH, rtH = h / s, stretch = h / (h - 0.5);
        double targetH = (g.Target > 0 ? g.Target : g.RtH) / s;
        // The zoom area's default scale is min(1, area / target) on both axes, but the plug-in's GVFIT
        // then overwrites scale.y every frame with h / (h - 0.5): X is fitted, Y is drawn stretched.
        double fit = Math.Min(1, Math.Min(wPt / rtW, hPt / rtH));
        double xPt = lb + Math.Round(wPt / 2 - rtW * fit / 2);
        double bottom = Math.Round(hPt / 2 + targetH * stretch / 2);
        ox = g.L + xPt * s;
        oy = g.T + (tb + 21 + bottom - rtH * stretch) * s;
        row0 = (int)Math.Ceiling(g.T + (tb + 21 + bottom) * s - 1e-6) - g.RtH;
    }

    static Geo Logged(string name, int l, int w, int h, float hw, float hh, int rtW, int rtH, bool max = false) =>
        new Geo { Name = name, L = l, T = 233, W = w, H = h, Hw = hw, Hh = hh, RtW = rtW, RtH = rtH, Max = max, Scale = 3f };

    static void Pinned(Geo g, int ex, int ey, int ew, int eh)
    {
        Place(g, out int x, out int y, out int w, out int h);
        Check(x == ex && y == ey && w == ew && h == eh,
              $"{g.Name}: got ({x},{y} {w}x{h}), want ({ex},{ey} {ew}x{eh})");
        // ...and that pinned value against Unity's rule (the model above).
        UnityOrigin(g, out double ox, out double oy, out int row0);
        double errX = x - ox, errY = y - oy;
        Check(Math.Abs(errX) <= 0.75 + Tol, $"{g.Name}: X error {errX:F2} within 0.75 px of Unity's RT origin");
        if (g.Max)
            Check(errY >= -0.5 - Tol && errY <= 1.5 + Tol, $"{g.Name}: maximized Y error {errY:F2} in [-0.5, 1.5] vs Unity's RT origin");
        else
            Check(y == row0 + DisplayXRGameViewPlacement.DockedRowBiasPx,
                  $"{g.Name}: docked anchor y {y} on the row of the RT's row 0 ({row0}) + {DisplayXRGameViewPlacement.DockedRowBiasPx}");
    }

    // A hardware case: the logged pane and Unity numbers, and what the capture showed. firstRow /
    // lastRow are the first / last screen rows of the RT; when Unity's zoom-area clip hides the RT's
    // row 0 (docked Free Aspect at 300 %: 879 of 880 rows visible), row 0 is lastRow + 1 - RtH.
    static void Hardware(string name, float scale, int l, int t, int w, int h, float hw, float hh, int rtW, int rtH,
                         float target, bool max, int firstCol, int firstRow, int lastRow)
    {
        var g = new Geo { Name = "HW " + name, L = l, T = t, W = w, H = h, Hw = hw, Hh = hh, RtW = rtW, RtH = rtH,
                          Target = target, Max = max, Scale = scale };
        Place(g, out int x, out int y, out int pw, out int ph);
        Check(x == firstCol, $"{g.Name}: anchor x {x} on the RT's first column {firstCol}");
        if (max)
        {
            // #420 leaves the maximized path alone; the capture's first row is where it was seen right.
            Check(y == firstRow, $"{g.Name}: maximized anchor y {y} on the first RT row {firstRow}");
            return;
        }
        int rtRow0 = lastRow - rtH + 1;
        Check(rtRow0 == firstRow || (rtRow0 == firstRow - 1 && rtRow0 < t + 45 * scale),
              $"{g.Name}: capture shows {rtH} rows ending at {lastRow} (first visible {firstRow})");
        Check(y == rtRow0 + DisplayXRGameViewPlacement.DockedRowBiasPx,
              $"{g.Name}: docked anchor y {y} on the row of the RT's row 0 ({rtRow0}) + {DisplayXRGameViewPlacement.DockedRowBiasPx}");
        // The independent model reproduces the capture.
        UnityOrigin(g, out _, out _, out int row0);
        Check(row0 == rtRow0, $"{g.Name}: model row 0 {row0} == capture {rtRow0}");
        Check(pw == rtW && ph == rtH, $"{g.Name}: anchor size {pw}x{ph} == RT {rtW}x{rtH}");
    }

    static void Main()
    {
        // ---- HARDWARE 2026-10-27: DS1 3840x2160, Unity 6000.4.6f1, the pane as logged at the capture
        // ("glue matched pane ... phys=", the GameView host points, mainTargetSize) and the capture's
        // RT edges. Docked anchors used to sit +2 rows below row 0 at 300 % main, +1 at 300 %
        // secondary and -1..+1 at 150 % (B2, R7: -1). ----
        //       name                          scale  L      T    W     H     host w     host h     RtW   RtH  target    max    col   first last
        Hardware("A1 300% main docked Free",    3f,   726,  233, 2207, 1021, 735.6667f, 340.3333f, 2201, 880,    880f, false,   729, 368, 1246);
        Hardware("A2 300% main 16:9 L/R bars",  3f,   726,  233, 2207, 1021, 735.6667f, 340.3333f, 1564, 880,    880f, false, 1047, 368, 1246);
        Hardware("A3 300% main 16:9 T/B",       3f,  2245,  233,  688, 1021, 229.3334f, 340.3333f,  682, 384,    384f, false, 2248, 617, 1000);
        Hardware("A4 300% main thin T/B",       3f,  1422,  233, 1511, 1021, 503.6667f, 340.3333f, 1505, 847,    847f, false, 1425, 385, 1231);
        Hardware("A5 300% main 5 px T/B",       3f,  1380,  233, 1553, 1021, 517.6667f, 340.3333f, 1547, 870,    870f, false, 1383, 374, 1243);
        Hardware("A5b 300% main 60 px T/B",     3f,  1567,  233, 1366, 1021, 455.3334f, 340.3333f, 1360, 765,    765f, false, 1570, 425, 1189);
        Hardware("R1 300% main T/B",            3f,  1535,  233, 1397, 1021, 465.6667f, 340.3333f, 1391, 782,    782f, false, 1538, 417, 1198);
        Hardware("A6 300% maximized 16:9",      3f,     0,  233, 3840, 1723, 1280f,     574.3333f, 2823,1588,   1588f, true,  510, 368, -1);
        Hardware("A7 300% maximized Free",      3f,     0,  233, 3840, 1723, 1280f,     574.3333f, 3840,1588,   1588f, true,     0, 368, -1);
        Hardware("C1 300% secondary Free",      3f, -3113,  233, 2206, 1020, 735.3334f, 340f,      2200, 879,    879f, false, -3110, 368, 1246);
        Hardware("C2 300% secondary T/B",       3f, -2228,  233, 1321, 1020, 440.3334f, 340f,      1315, 740,    740f, false,-2225, 438, 1177);
        Hardware("C3 300% secondary thin T/B",  3f, -2446,  233, 1539, 1020, 513f,      340f,      1533, 862,    862f, false,-2443, 376, 1237);
        Hardware("B1 150% docked Free",         1.5f, 726,  118, 2207, 1150, 1471.333f, 766.6667f, 2204,1080, 1079.5f, false,   728, 186, 1265);
        Hardware("B2 150% T/B",                 1.5f,1454,  118, 1479, 1150, 986.0001f, 766.6667f, 1476, 830,    830f, false, 1456, 311, 1140);
        Hardware("B3 150% thin T/B",            1.5f,1075,  118, 1858, 1150, 1238.667f, 766.6667f, 1855,1043,   1043f, false, 1077, 205, 1247);
        Hardware("B4 150% L/R bars",            1.5f, 843,  118, 2090, 1150, 1393.333f, 766.6667f, 1919,1080, 1079.5f, false,  929, 186, 1265);
        Hardware("R2 150% T/B",                 1.5f,1324,  118, 1608, 1150, 1072f,     766.6667f, 1605, 903,    903f, false, 1326, 274, 1176);
        Hardware("R3 150% T/B",                 1.5f,1580,  118, 1352, 1150, 901.3333f, 766.6667f, 1349, 759,    759f, false, 1582, 346, 1104);
        Hardware("R4 150% T/B",                 1.5f,1929,  118, 1003, 1150, 668.6666f, 766.6667f, 1000, 562,    562f, false, 1931, 444, 1005);
        Hardware("R5 150% thin T/B",            1.5f,1093,  118, 1839, 1150, 1226f,     766.6667f, 1836,1033,   1033f, false, 1095, 209, 1241);
        Hardware("R6 150% T/B",                 1.5f,1233,  118, 1699, 1150, 1132.667f, 766.6667f, 1696, 954,    954f, false, 1235, 249, 1202);
        Hardware("R7 150% T/B",                 1.5f,1378,  118, 1554, 1150, 1036f,     766.6667f, 1551, 872,    872f, false, 1380, 290, 1161);

        // ---- PINNED: 300 % DS1, the logged panes (1026-150721 Editor.log) ----
        var v1   = Logged("V1 docked Free",            719, 2213, 1021, 737.6666f, 340.3333f, 2207, 880);
        var v4a  = Logged("V4a docked 16:9 L/R bars",  719, 2213, 1021, 737.6666f, 340.3333f, 1564, 880);
        var v4b  = Logged("V4b docked 16:9 tall",     2301,  631, 1021, 210.3333f, 340.3333f,  625, 352);
        var thin = Logged("V4c docked thin T/B 23/22",1442, 1490, 1021, 496.6667f, 340.3333f, 1484, 835);
        var tb60 = Logged("V4c docked T/B 60/58",     1571, 1361, 1021, 453.6666f, 340.3333f, 1355, 762);
        var ramp = Logged("docked T/B 6/6",           1382, 1550, 1021, 516.6666f, 340.3333f, 1544, 868);
        var m169 = Logged("maximized 16:9",              0, 3840, 1723, 1280f,     574.3334f, 2823, 1588, max: true);
        var mFree= Logged("maximized Free",              0, 3840, 1723, 1280f,     574.3334f, 3840, 1588, max: true);
        Pinned(v1,   722, 367, 2207, 880);   // HW: 879 rows visible from 368 -> row 0 at 367, under the clip
        Pinned(v4a, 1043, 367, 1564, 880);   // v2.33.0 had 374 (inverted), #427 369
        Pinned(v4b, 2304, 631,  625, 352);   // v2.33.0 had 638 (inverted), #427 633; RT measured at 631
        Pinned(thin,1445, 391, 1484, 835);   // v2.33.0 had 414 (bottom-aligned as "chrome"), #427 393; RT at 391
        Pinned(tb60,1574, 428, 1355, 762);   // v2.33.0 had 433, #427 430; RT at 428
        Pinned(ramp,1385, 373, 1544, 868);
        Pinned(m169, 510, 368, 2823, 1588);  // v2.33.0 had 508; Unity Round(169.5 pt) = 170 pt = 510 px
        Pinned(mFree,  0, 368, 3840, 1588);

        // Which path: Free Aspect is chrome slack, any real letterbox is not, maximized never is.
        Check(DisplayXRGameViewPlacement.IsChromeSlackOnly(PaneOf(v1)), "V1 is chrome slack");
        foreach (var g in new[] { v4a, v4b, thin, tb60, ramp, m169, mFree })
            Check(!DisplayXRGameViewPlacement.IsChromeSlackOnly(PaneOf(g)), g.Name + " is not chrome slack");

        // #421's hardware-verified 250 % docked placement must not move (mainSize 859.5 -> RT 860 vs a
        // 859 px area: the -1 px is rounding, still chrome slack). It already sat on the RT's row 0.
        var d250 = new Geo { Name = "250% docked Free (#421)", L = 1017, T = 293, W = 1733, H = 977, Hw = 693.2f, Hh = 390.8f, RtW = 1728, RtH = 860, Target = 859.5f, Scale = 2.5f };
        Check(DisplayXRGameViewPlacement.IsChromeSlackOnly(PaneOf(d250)), "250% Free is chrome slack");
        Pinned(d250, 1020, 406, 1728, 860);

        // A 16:9 RT 2 px shorter than the area (1 px bars) takes the chrome-slack path, but Unity
        // centres ITS height (1078), not the area's (1079.5): 2 rows apart at 150 %.
        var near = new Geo { Name = "150% 16:9 1 px bars (chrome slack)", L = 1000, T = 118, W = 1919, H = 1150, Hw = 1279.333f, Hh = 766.6667f,
                             RtW = 1916, RtH = 1078, Target = 1078f, Scale = 1.5f };
        Check(DisplayXRGameViewPlacement.IsChromeSlackOnly(PaneOf(near)), near.Name + " is chrome slack");
        Pinned(near, 1002, 186, 1916, 1078);

        // ---- The review case (#427): a 300 % secondary with a 100 % primary. The global
        // pixelsPerPoint is 1.0 there; the pane's own scale is 3. Same anchors, shifted. ----
        Check(DisplayXRGameViewPlacement.BottomMarginPx(1f) == -2, "global 1.0 would give -2");
        foreach (var (g, ex, ey) in new[] { (v1, 722, 367), (v4a, 1043, 367), (v4b, 2304, 631), (thin, 1445, 391), (tb60, 1574, 428) })
        {
            var s = g; s.L += 1920; s.Name = g.Name + " on a 300% secondary";
            Pinned(s, ex + 1920, ey, g.RtW, g.RtH);
        }

        // ---- The #740 terms ----
        foreach (var (scale, want) in new[] { (1f, -2), (1.25f, -2), (1.5f, -2), (1.75f, -3), (2f, -3), (2.5f, -4), (3f, -5), (3.0000003f, -5), (2.9999997f, -5) })
            Check(DisplayXRGameViewPlacement.BottomMarginPx(scale) == want, $"BottomMarginPx({scale}) == {want}");
        Check(DisplayXRGameViewPlacement.CentringX(5) == 3 && DisplayXRGameViewPlacement.CentringX(6) == 3
              && DisplayXRGameViewPlacement.CentringX(7) == 4, "CentringX gives the extra pixel to the left");
        Check(DisplayXRGameViewPlacement.ToolbarPx(340.3333f, 293.3333f, 3.0000003f) == 141, "docked toolbar 47 pt = 141 px at 300%");
        Check(DisplayXRGameViewPlacement.ToolbarPx(574.3334f, 529.3334f, 3f) == 135, "maximized toolbar 45 pt = 135 px at 300%");
        // 47 pt at 150 % is a 70.5 px tie: the measured scale (pane px / host pt) is 1.5 +- 1 ulp, so
        // it used to round to 70 or 71 from frame to frame (R3 / R4 vs the rest on 2026-10-27).
        foreach (float ptToPx in new[] { 1.4999998f, 1.5f, 1.5000001f, 1.5000004f })
            Check(DisplayXRGameViewPlacement.ToolbarPx(766.6667f, 719.6667f, ptToPx) == 70, $"docked toolbar at 150% (ptToPx {ptToPx:R}) == 70");

        // ---- Measured ptToPx noise must not flip the 169.5 pt tie (maximized 16:9). Only ptToPx
        // varies: a noisy HOST width is noise Unity's own draw rect shares, so Unity rounds with it. ----
        foreach (float ptToPx in new[] { 3.0000003f, 2.9999997f, 3f })
        {
            int offX = -1, offY = -1;
            DisplayXRGameViewPlacement.LetterboxOffsets(1280f, 1280f, 529.3334f, 2823, 1588, ptToPx, true, ref offX, ref offY);
            Check(offX == 510, $"maximized 16:9 at ptToPx {ptToPx:R}: offX {offX} == 510");
        }

        // ---- ...nor move a docked pane (B2 at 150 %: the host width is logged to 7 digits) ----
        foreach (float hw in new[] { 986.0000f, 986.0001f, 985.9999f })
        {
            var g = new Geo { Name = "B2 noise", L = 1454, T = 118, W = 1479, H = 1150, Hw = hw, Hh = 766.6667f, RtW = 1476, RtH = 830, Scale = 1.5f };
            Place(g, out int nx, out int ny, out _, out _);
            Check(nx == 1456 && ny == 311, $"B2 with host width {hw:R}: ({nx},{ny}) == (1456,311)");
        }

        // ---- No draw rect (Unity version drift): pixel centring + the docked #740 margin ----
        Place(v4a, out int fx, out int fy, out _, out _, haveDraw: false);
        Check(fx == 1043 && fy == 369, $"V4a without draw rect: ({fx},{fy}) == (1043,369)");

        // ---- GL (UV origin at the bottom): Unity rounds the TOP edge; no full-area correction ----
        {
            int offX = -1, offY = -1;
            DisplayXRGameViewPlacement.LetterboxOffsets(210.3333f, 208.3333f, 293.3333f, 625, 352, 3f, false, ref offX, ref offY);
            int wantY = (int)Math.Floor(Math.Round((293.3333 - 352 / 3.0) * 0.5) * 3 + 0.5);
            Check(offX == 3 && offY == wantY, $"GL letterbox offsets ({offX},{offY}) == (3,{wantY})");
            // ...and a docked GL pane keeps the #740 fallback (its edge rule is unmeasured).
            Place(v4b, out int gx, out int gy, out _, out _, roundsBottomEdge: false);
            Check(gx == 2304 && gy == 233 + 141 + wantY - 5, $"V4b on GL: ({gx},{gy}) == (2304,{233 + 141 + wantY - 5})");
        }

        // ---- PINNED, the replay's other scales (Unity points = pane px / scale, as a window is).
        // 150 % is measured above (HARDWARE); 125 % and 250 % letterboxes are the model's. ----
        foreach (var (name, scale, l, w, h, rtW, rtH, max, ex, ey) in new[]
        {
            ("150% docked Free",          1.5f,  480, 1476,  681, 1473,  610, false,  482, 228),
            ("150% docked 16:9 L/R",      1.5f,  480, 1476,  681, 1084,  610, false,  677, 228),
            ("150% docked 16:9 T/B",      1.5f, 1534,  630,  681,  627,  353, false, 1536, 356),
            ("150% docked thin T/B",      1.5f,  961, 1034,  681, 1031,  580, false,  963, 243),
            ("150% docked T/B odd width", 1.5f, 1500,  656,  681,  653,  367, false, 1502, 350),
            ("150% maximized 16:9",       1.5f,    0, 2560, 1200, 2012, 1132, true,   275, 228),
            ("125% docked Free",          1.25f, 480, 1230,  568, 1228,  509, false,  481, 218),
            ("125% docked 16:9 L/R",      1.25f, 480, 1230,  568,  905,  509, false,  643, 218),
            ("125% docked 16:9 T/B",      1.25f,1534,  525,  568,  522,  294, false, 1535, 325),
            ("125% docked thin T/B",      1.25f, 961,  854,  568,  852,  479, false,  962, 233),
            ("125% docked T/B odd width", 1.25f,1500,  546,  568,  544,  306, false, 1501, 318),
            ("125% maximized 16:9",       1.25f,   0, 2133, 1000, 1678,  944, true,   228, 216),
            ("250% docked 16:9 T/B",      2.5f, 1017, 1050,  977, 1045,  588, false, 1020, 543),
        })
        {
            var g = new Geo { Name = name, L = l, T = 160, W = w, H = h, Hw = w / scale, Hh = h / scale, RtW = rtW, RtH = rtH, Max = max,
                              Scale = scale };
            // Free Aspect and left/right bars: Unity's target is the area, fractional px.
            if (!max && (name.Contains("Free") || name.Contains("L/R"))) g.Target = (g.Hh - 47f) * scale;
            if (name.StartsWith("250%")) g.T = 293;
            Pinned(g, ex, ey, rtW, rtH);
        }

        Console.WriteLine($"GameViewPlacement: {s_pass} passed, {s_fail} failed");
        Environment.Exit(s_fail == 0 ? 0 : 1);
    }
}
