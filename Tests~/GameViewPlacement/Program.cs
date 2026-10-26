// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for DisplayXRGameViewPlacement - where the editor Game-view glue anchors the weave (#420).
// Hand-rolled (no test framework) so it also runs from a bare `csc` + the .NET runtime that ships
// inside the Unity editor. Exit code 0 = all passed.
//
// PINNED: the anchors for the geometry logged on a 300 % DS1 (Windows run 2026-10-26, Unity
// 6000.4.6f1), where physical-pixel captures showed where Unity really draws the RT, plus the
// replay's 125 % / 150 % / 250 % panes and the #427 review case (300 % secondary, 100 % primary).
// Each pinned anchor is also checked against Unity's own rule, implemented independently below. The
// model reproduces every physical-pixel capture taken at 300 %; at other scales it is unverified on
// hardware (sub-pixel overflow of a fractional area and the integer-px window sizes are modelled,
// not measured), which is why only the measured or replayed panes are pinned here:
//    DockArea borders (L/R 1 pt, top 24 pt, docked bottom 2 pt; maximized: top 24 only), Game-view
//    toolbar 21 pt, GameView.OnGUI centring the target in the zoom area and rounding the FLIPPED
//    rect's x / y (= the bottom edge on D3D) to whole points, default zoom = min(1, area / target)
//    in X, and in Y the plug-in's GVFIT scale h / (h - 0.5), which replaces the default there.
// The eye-verified views on that rig sit at +2.5 px (docked Free Aspect) and +1.5 px (maximized)
// below Unity's RT origin; +3.5 was already judged wrong. So a pinned docked anchor must land in
// [-0.5, +2.5] (an integer anchor vs a half-pixel origin can round either way), a maximized one in
// [-0.5, +1.5], and X within 0.75 px (the #740 (slack + 1) / 2 rule on a quarter-pixel border).

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
        public float Scale; // Unity's backing scale for the pane (the model's input; the code measures W / Hw)
    }

    static DisplayXRGameViewPlacement.Pane PaneOf(Geo g, bool roundsBottomEdge = true, bool haveDraw = true)
    {
        float ptToPx = g.W / g.Hw;
        float drawH = g.Hh - (g.Max ? 45f : 47f), drawW = g.Hw - (g.Max ? 0f : 2f);
        return new DisplayXRGameViewPlacement.Pane
        {
            Left = g.L, Top = g.T, ClientW = g.W, ClientH = g.H,
            ToolbarPx = DisplayXRGameViewPlacement.ToolbarPx(g.Hh, drawH, ptToPx),
            RtW = g.RtW, RtH = g.RtH, DockedNonMaximized = !g.Max,
            HaveDrawRect = haveDraw, HostWPt = g.Hw, DrawWPt = drawW, DrawHPt = drawH,
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

    // Unity's RT origin (fractional px), the independent model described at the top.
    static void UnityOrigin(Geo g, out double ox, out double oy)
    {
        double s = g.Scale, lb = g.Max ? 0 : 1, tb = 24;
        double wPt = g.Hw - (g.Max ? 0 : 2), hPt = g.Hh - (g.Max ? 45 : 47);
        double rtW = g.RtW / s, h = g.RtH, rtH = h / s, stretch = h / (h - 0.5);
        // The zoom area's default scale is min(1, area / target) on both axes, but the plug-in's GVFIT
        // then overwrites scale.y every frame with h / (h - 0.5): X is fitted, Y is drawn stretched.
        double fit = Math.Min(1, Math.Min(wPt / rtW, hPt / rtH));
        double xPt = lb + Math.Round(wPt / 2 - rtW * fit / 2);
        double bottom = Math.Round(hPt / 2 + rtH * stretch / 2);
        ox = g.L + xPt * s;
        oy = g.T + (tb + 21 + bottom - rtH * stretch) * s;
    }

    static Geo Logged(string name, int l, int w, int h, float hw, float hh, int rtW, int rtH, bool max = false, int dx = 0) =>
        new Geo { Name = name, L = l + dx, T = 233, W = w, H = h, Hw = hw, Hh = hh, RtW = rtW, RtH = rtH, Max = max, Scale = 3f };

    static void Pinned(Geo g, int ex, int ey, int ew, int eh)
    {
        Place(g, out int x, out int y, out int w, out int h);
        Check(x == ex && y == ey && w == ew && h == eh,
              $"{g.Name}: got ({x},{y} {w}x{h}), want ({ex},{ey} {ew}x{eh})");
        // ...and that pinned value against Unity's rule (the model above).
        UnityOrigin(g, out double ox, out double oy);
        double errX = x - ox, errY = y - oy, hi = g.Max ? 1.5 : 2.5;
        Check(Math.Abs(errX) <= 0.75 + Tol, $"{g.Name}: X error {errX:F2} within 0.75 px of Unity's RT origin");
        Check(errY >= -0.5 - Tol && errY <= hi + Tol, $"{g.Name}: Y error {errY:F2} in [-0.5, {hi}] vs Unity's RT origin");
    }

    static void Main()
    {
        // ---- PINNED: 300 % DS1, the logged panes (1026-150721 Editor.log) ----
        var v1   = Logged("V1 docked Free",            719, 2213, 1021, 737.6666f, 340.3333f, 2207, 880);
        var v4a  = Logged("V4a docked 16:9 L/R bars",  719, 2213, 1021, 737.6666f, 340.3333f, 1564, 880);
        var v4b  = Logged("V4b docked 16:9 tall",     2301,  631, 1021, 210.3333f, 340.3333f,  625, 352);
        var thin = Logged("V4c docked thin T/B 23/22",1442, 1490, 1021, 496.6667f, 340.3333f, 1484, 835);
        var tb60 = Logged("V4c docked T/B 60/58",     1571, 1361, 1021, 453.6666f, 340.3333f, 1355, 762);
        var ramp = Logged("docked T/B 6/6",           1382, 1550, 1021, 516.6666f, 340.3333f, 1544, 868);
        var m169 = Logged("maximized 16:9",              0, 3840, 1723, 1280f,     574.3334f, 2823, 1588, max: true);
        var mFree= Logged("maximized Free",              0, 3840, 1723, 1280f,     574.3334f, 3840, 1588, max: true);
        Pinned(v1,   722, 369, 2207, 880);   // HW: looks right (Unity RT origin 366.5, first visible row 368)
        Pinned(v4a, 1043, 369, 1564, 880);   // v2.33.0 had 374 (inverted)
        Pinned(v4b, 2304, 633,  625, 352);   // v2.33.0 had 638 (inverted); RT measured at 631
        Pinned(thin,1445, 393, 1484, 835);   // v2.33.0 had 414 (bottom-aligned as "chrome"); RT at 391
        Pinned(tb60,1574, 430, 1355, 762);   // v2.33.0 had 433; RT at 428
        Pinned(ramp,1385, 375, 1544, 868);
        Pinned(m169, 510, 368, 2823, 1588);  // v2.33.0 had 508; Unity Round(169.5 pt) = 170 pt = 510 px
        Pinned(mFree,  0, 368, 3840, 1588);

        // Which path: Free Aspect is chrome slack, any real letterbox is not, maximized never is.
        Check(DisplayXRGameViewPlacement.IsChromeSlackOnly(PaneOf(v1)), "V1 is chrome slack");
        foreach (var g in new[] { v4a, v4b, thin, tb60, ramp, m169, mFree })
            Check(!DisplayXRGameViewPlacement.IsChromeSlackOnly(PaneOf(g)), g.Name + " is not chrome slack");

        // #421's hardware-verified 250 % docked placement must not move (mainSize 859.5 -> RT 860 vs a
        // 859 px area: the -1 px is rounding, still chrome slack).
        var d250 = new Geo { Name = "250% docked Free (#421)", L = 1017, T = 293, W = 1733, H = 977, Hw = 693.2f, Hh = 390.8f, RtW = 1728, RtH = 860, Scale = 2.5f };
        Check(DisplayXRGameViewPlacement.IsChromeSlackOnly(PaneOf(d250)), "250% Free is chrome slack");
        Pinned(d250, 1020, 406, 1728, 860);

        // ---- The review case (#427): a 300 % secondary with a 100 % primary. The global
        // pixelsPerPoint is 1.0 there; the pane's own scale is 3. Same anchors, shifted. ----
        Check(DisplayXRGameViewPlacement.BottomMarginPx(1f) == -2, "global 1.0 would give -2");
        foreach (var (g, ex, ey) in new[] { (v1, 722, 369), (v4a, 1043, 369), (v4b, 2304, 633), (thin, 1445, 393), (tb60, 1574, 430) })
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

        // ---- Measured ptToPx noise must not flip the 169.5 pt tie (maximized 16:9). Only ptToPx
        // varies: a noisy HOST width is noise Unity's own draw rect shares, so Unity rounds with it. ----
        foreach (float ptToPx in new[] { 3.0000003f, 2.9999997f, 3f })
        {
            int offX = -1, offY = -1;
            DisplayXRGameViewPlacement.LetterboxOffsets(1280f, 1280f, 529.3334f, 2823, 1588, ptToPx, true, ref offX, ref offY);
            Check(offX == 510, $"maximized 16:9 at ptToPx {ptToPx:R}: offX {offX} == 510");
        }

        // ---- No draw rect (Unity version drift): pixel centring + the docked margin ----
        Place(v4a, out int fx, out int fy, out _, out _, haveDraw: false);
        Check(fx == 1043 && fy == 369, $"V4a without draw rect: ({fx},{fy}) == (1043,369)");

        // ---- GL (UV origin at the bottom): Unity rounds the TOP edge; no full-area correction ----
        {
            int offX = -1, offY = -1;
            DisplayXRGameViewPlacement.LetterboxOffsets(210.3333f, 208.3333f, 293.3333f, 625, 352, 3f, false, ref offX, ref offY);
            int wantY = (int)Math.Floor(Math.Round((293.3333 - 352 / 3.0) * 0.5) * 3 + 0.5);
            Check(offX == 3 && offY == wantY, $"GL letterbox offsets ({offX},{offY}) == (3,{wantY})");
        }

        // ---- PINNED, the replay's other scales (Unity points = pane px / scale, as a window is) ----
        foreach (var (name, scale, l, w, h, rtW, rtH, max, ex, ey) in new[]
        {
            ("150% docked Free",          1.5f,  480, 1476,  681, 1473,  610, false,  482, 229),
            ("150% docked 16:9 L/R",      1.5f,  480, 1476,  681, 1084,  610, false,  677, 229),
            ("150% docked 16:9 T/B",      1.5f, 1534,  630,  681,  627,  353, false, 1536, 357),
            ("150% docked thin T/B",      1.5f,  961, 1034,  681, 1031,  580, false,  963, 244),
            ("150% docked T/B odd width", 1.5f, 1500,  656,  681,  653,  367, false, 1502, 350),
            ("150% maximized 16:9",       1.5f,    0, 2560, 1200, 2012, 1132, true,   275, 228),
            ("125% docked Free",          1.25f, 480, 1230,  568, 1228,  509, false,  481, 217),
            ("125% docked 16:9 L/R",      1.25f, 480, 1230,  568,  905,  509, false,  643, 217),
            ("125% docked 16:9 T/B",      1.25f,1534,  525,  568,  522,  294, false, 1535, 325),
            ("125% docked thin T/B",      1.25f, 961,  854,  568,  852,  479, false,  962, 232),
            ("125% docked T/B odd width", 1.25f,1500,  546,  568,  544,  306, false, 1501, 319),
            ("125% maximized 16:9",       1.25f,   0, 2133, 1000, 1678,  944, true,   228, 216),
            ("250% docked 16:9 T/B",      2.5f, 1017, 1050,  977, 1045,  588, false, 1020, 543),
        })
        {
            var g = new Geo { Name = name, L = l, T = 160, W = w, H = h, Hw = w / scale, Hh = h / scale, RtW = rtW, RtH = rtH, Max = max, Scale = scale };
            if (name.StartsWith("250%")) g.T = 293;
            Pinned(g, ex, ey, rtW, rtH);
        }

        Console.WriteLine($"GameViewPlacement: {s_pass} passed, {s_fail} failed");
        Environment.Exit(s_fail == 0 ? 0 : 1);
    }
}
