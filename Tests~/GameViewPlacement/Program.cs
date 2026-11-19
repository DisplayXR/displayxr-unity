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

    // (#471) The phase source appended to the "GameView 1:1:" line: which of the texture target
    // rect and the anchor window phases the weave, from the native state (unit test of the text,
    // not a hardware pin).
    static void PhaseSource()
    {
        string P(bool chained, int why, uint flags, bool reported, bool matched, int mismatch) =>
            DisplayXRGameViewPlacement.PhaseSourceText(chained, why, flags, reported, matched, mismatch, 1020, 406, 1021, 406);
        Check(P(true, 0, DisplayXRGameViewPlacement.TargetStateWindowlessPhase, true, true, 0)
              == "texture target rect, WINDOWLESS_PHASE | woven origin 1020,406 (= requested)", "phase: windowless, as requested");
        Check(P(true, 0, DisplayXRGameViewPlacement.TargetStateWindowlessPhase, true, true, 1)
              == "texture target rect, WINDOWLESS_PHASE | woven origin 1020,406 but requested 1021,406 (DIFFERS)", "phase: windowless, origin differs");
        Check(P(true, 0, DisplayXRGameViewPlacement.TargetStateWindowlessPhase | DisplayXRGameViewPlacement.TargetStateClamped, true, true, 4)
              .StartsWith("texture target rect, WINDOWLESS_PHASE CLAMPED |"), "phase: clamped is named");
        Check(P(true, 0, DisplayXRGameViewPlacement.TargetStateWindowlessPhase, false, false, 0)
              == "texture target rect, WINDOWLESS_PHASE | nothing woven yet", "phase: before the first weave");
        // Where the pushed rect came from (#459): the calibration, or the computed placement.
        Check(DisplayXRGameViewPlacement.TargetRectSourceText(2) == " | target rect: measured", "rect source: measured");
        Check(DisplayXRGameViewPlacement.TargetRectSourceText(1) == " | target rect: computed", "rect source: computed");
        Check(DisplayXRGameViewPlacement.TargetRectSourceText(0) == " | target rect: none pushed", "rect source: none");
        Check(P(true, 0, DisplayXRGameViewPlacement.TargetStateAnchorFallback, true, true, 0).StartsWith("anchor window (runtime ANCHOR_FALLBACK"),
              "phase: the runtime fell back to the anchor");
        // A session that ignores the binding (D3D12 today): no flags, nothing reported.
        Check(P(true, 0, 0, false, false, 0) == "anchor window (the runtime did not bind the texture target)", "phase: binding ignored");
        Check(P(false, 4, 0, false, false, 0) == "anchor window (texture target not chained: the runtime predates XR_DXR_win32_window_binding 9)",
              "phase: older runtime");
        Check(P(false, 2, 0, false, false, 0).Contains("DISPLAYXR_PROV_TEXTURE_TARGET=0"), "phase: opt-out");
        for (int why = 1; why <= 9; why++)
            Check(P(false, why, 0xFF, true, true, 7).StartsWith("anchor window (texture target not chained: "), "phase: not chained is always the anchor (" + why + ")");
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
        // The target is Unity's (859.5 px at 250 % fits an 859.5 px area), not the RT's rounded rows.
        double fit = Math.Min(1, Math.Min(wPt / rtW, hPt / targetH));
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

    // ==== (#459) Is the Game view drawn 1:1, and the whole-point-and-pixel size ====
    // NOTHING BELOW IS A HARDWARE PIN. Two kinds of check, kept apart:
    //   WholePixelDefinitions()  unit tests of exact arithmetic (steps, sizes, line counts).
    //   ModelConsistency()       the plug-in's MODEL of Unity checked against itself and against
    //                            numbers reported from another plug-in's 175 % screen captures. Those
    //                            captures give pixels on screen, not Unity's inputs, so the panes and
    //                            targets fed in here are reconstructed with this suite's own chrome
    //                            constants (1 pt side borders, 24 pt tab, 21 pt toolbar, 2 pt bottom
    //                            border): agreement shows the model is self-consistent, not that it
    //                            is right. docs~/architecture/editor-game-view-placement.md lists
    //                            what a hardware session has to confirm.
    // The texture size Unity allocates is its target rounded half AWAY FROM ZERO
    // (PlayModeView.RenderView in the 6000.4 editor assembly): UnityRt() below.
    static int UnityRt(double targetPx) => (int)Math.Round(targetPx, MidpointRounding.AwayFromZero);
    // What the glue computes today: Mathf.RoundToInt, i.e. halves to even.
    static int GlueRt(double targetPx) => (int)Math.Round(targetPx, MidpointRounding.ToEven);

    // (#459) A zoomed Game view: what GVFIT may write, the degraded placement, the takeover.
    // The geometry is the hardware session of 2026-11-18 (175 %, docked, the view on
    // "4K UHD" 3840x2160 fitted at 0.2786 into a 641.43 x 343.86 pt area): the log read
    // zoom=(0.278587967,1.0002315), anchor at pane +28,+-700, a black weave.
    static void ZoomedView()
    {
        const float Fit4K = 0.278587967f;
        float fitRow(int h) => h / (h - 0.5f);

        // -- GVFIT's decision: valid at zoom 1 only, never anisotropic otherwise.
        Check(DisplayXRGameViewPlacement.IsZoomOne(1f) && DisplayXRGameViewPlacement.IsZoomOne(1.00005f)
              && !DisplayXRGameViewPlacement.IsZoomOne(1.0002315f) && !DisplayXRGameViewPlacement.IsZoomOne(Fit4K)
              && !DisplayXRGameViewPlacement.IsZoomOne(2f) && !DisplayXRGameViewPlacement.IsZoomOne(0f),
              "IsZoomOne: 1 is 1; GVFIT's own 1.0002 stretch, 0.2786, 2 and 0 are not");
        Check(!DisplayXRGameViewPlacement.IsScaledView(0f) && !DisplayXRGameViewPlacement.IsScaledView(1f)
              && DisplayXRGameViewPlacement.IsScaledView(Fit4K) && DisplayXRGameViewPlacement.IsScaledView(2.747999f),
              "IsScaledView: an unknown zoom (0) is treated as 1");
        Check(DisplayXRGameViewPlacement.FitZoomY(1f, 860) == fitRow(860) && DisplayXRGameViewPlacement.FitZoomY(1f, 2160) == fitRow(2160),
              "GVFIT at zoom 1: rows / (rows - 0.5), as before");
        Check(DisplayXRGameViewPlacement.FitZoomY(Fit4K, 2160) == Fit4K,
              "GVFIT on the hardware case (4K at 0.2786): scale.y = scale.x, not 1.0002");
        Check(DisplayXRGameViewPlacement.FitZoomY(2f, 860) == 2f && DisplayXRGameViewPlacement.FitZoomY(2.747999f, 860) == 2.747999f,
              "GVFIT with the Scale slider at 2x / 2.75x (250 % logs): scale.y = scale.x");
        Check(DisplayXRGameViewPlacement.FitZoomY(1f, 0) == 1f && DisplayXRGameViewPlacement.FitZoomY(1f, 1) == 1f,
              "GVFIT without a texture: uniform");
        foreach (float zx in new[] { 0.1f, 0.25f, Fit4K, 0.5f, 0.999f, 1.001f, 1.5f, 2f, 2.747999f, 5f, 12.5f })
            foreach (int rows in new[] { 0, 1, 2, 595, 860, 1080, 2160, 4320 })
                Check(DisplayXRGameViewPlacement.FitZoomY(zx, rows) == zx, $"GVFIT never makes zoom {zx:R} anisotropic ({rows} rows)");
        foreach (int rows in new[] { 2, 595, 860, 1080, 2160 })
        {
            float fy = DisplayXRGameViewPlacement.FitZoomY(1f, rows);
            Check(fy > 1f && fy < 1.5f && Math.Abs(rows / (double)fy - (rows - 0.5)) < 1e-3, $"GVFIT at zoom 1, {rows} rows: the fit");
        }
        // What is put back afterwards is Unity's uniform zoom, whatever was written.
        Check(DisplayXRGameViewPlacement.RestoredZoomY(1f) == 1f, "restore after the fit: scale.y = 1");
        Check(DisplayXRGameViewPlacement.RestoredZoomY(Fit4K) == Fit4K && DisplayXRGameViewPlacement.RestoredZoomY(2f) == 2f,
              "restore of a zoomed view (the stretched zoom the old code left in the layout): scale.y = scale.x");

        // -- The hardware pane. Host 643.43 x 390.86 pt at 1.75 = 1126 x 684 px.
        var g = new Geo { Name = "175% docked 4K fitted", L = 627, T = 186, W = 1126, H = 684, Hw = 1126 / 1.75f, Hh = 684 / 1.75f,
                          RtW = 3840, RtH = 2160, Scale = 1.75f, Target = 2160f };
        var p = PaneOf(g);
        int areaTop = p.Top + p.ToolbarPx, areaH = p.ClientH - p.ToolbarPx;

        // Without the zoom (what ran on hardware): the zoom-1 arithmetic centres the UNSCALED
        // 2160 px target and puts the anchor far above the pane.
        DisplayXRGameViewPlacement.Place(p, 0, 0, out int ox, out int oy, out int ow, out int oh);
        Check(oy - p.Top < -600 && oy - p.Top > -800 && ow == 1070 && oh == 602,
              $"{g.Name}: REGRESSION PIN - the zoom-1 path puts the anchor at pane +{ox - p.Left},+{oy - p.Top} {ow}x{oh} (hardware logged +28,+-700 1070x602)");
        int kx = ox, ky = oy, kw = ow, kh = oh;
        Check(!DisplayXRGameViewPlacement.KeepInsidePane(p, ref kx, ref ky, ref kw, ref kh)
              && kx == p.Left && ky == areaTop && kw == p.ClientW && kh == areaH,
              $"{g.Name}: an anchor off the pane is replaced by the render area");

        // With the zoom: the scaled picture, centred, inside the render area.
        p.ZoomX = Fit4K;
        DisplayXRGameViewPlacement.Place(p, 0, 0, out int x, out int y, out int w, out int h);
        Check(w == 1070 && h == 602, $"{g.Name}: scaled size {w}x{h} == 3840x2160 x 0.2786 (1070x602, the logged canvas)");
        Check(x >= p.Left && y >= areaTop && x + w <= p.Left + p.ClientW && y + h <= areaTop + areaH,
              $"{g.Name}: scaled anchor pane +{x - p.Left},+{y - p.Top} lies inside the render area");
        Check(x - p.Left == (1126 - 1070) / 2 && y - areaTop == (areaH - 602) / 2, $"{g.Name}: scaled picture is centred");
        int ix = x, iy = y, iw = w, ih = h;
        Check(DisplayXRGameViewPlacement.KeepInsidePane(p, ref ix, ref iy, ref iw, ref ih) && ix == x && iy == y && iw == w && ih == h,
              $"{g.Name}: the scaled anchor is kept as it is");

        // Zoom above 1 (Free Aspect, Scale slider at 2x): a crop that fills the area.
        var g2 = new Geo { Name = "250% docked Free", L = 1017, T = 293, W = 1733, H = 977, Hw = 1733 / 2.5f, Hh = 977 / 2.5f,
                           RtW = 1728, RtH = 860, Scale = 2.5f, Target = 859.5f };
        var p2 = PaneOf(g2);
        p2.ZoomX = 2f;
        DisplayXRGameViewPlacement.Place(p2, 3, 0, out x, out y, out w, out h);
        Check(x == p2.Left && y == p2.Top + p2.ToolbarPx && w == p2.ClientW && h == p2.ClientH - p2.ToolbarPx,
              $"{g2.Name} at 2x: the anchor is the whole render area ({w}x{h})");

        // Zoom 1, or unknown: nothing changes (the 250 % reference still lands on (1020,406)).
        foreach (float zx in new[] { 0f, 1f })
        {
            p2.ZoomX = zx;
            DisplayXRGameViewPlacement.Place(p2, 3, 0, out x, out y, out w, out h);
            Check(x == 1020 && y == 406 && w == 1728 && h == 860, $"{g2.Name} zoom {zx}: reference placement ({x},{y} {w}x{h}) unchanged");
            int ux = x, uy = y, uw = w, uh = h;
            Check(DisplayXRGameViewPlacement.KeepInsidePane(p2, ref ux, ref uy, ref uw, ref uh) && ux == x && uy == y,
                  $"{g2.Name} zoom {zx}: the reference anchor counts as inside the pane");
        }

        // -- The takeover decision.
        Check(DisplayXRGameViewPlacement.TakeoverFor(1f, 1f) == DisplayXRGameViewPlacement.Takeover.None
              && DisplayXRGameViewPlacement.TakeoverFor(0f, 0.3f) == DisplayXRGameViewPlacement.Takeover.None,
              "takeover: zoom 1 (or unknown) is left alone");
        Check(DisplayXRGameViewPlacement.TakeoverFor(Fit4K, Math.Min(1122.5f / 3840, 601.75f / 2160)) == DisplayXRGameViewPlacement.Takeover.FreeAspect,
              "takeover: 4K in a 1122x602 px area cannot be shown at 1x -> Free Aspect");
        Check(DisplayXRGameViewPlacement.TakeoverFor(2f, 1f) == DisplayXRGameViewPlacement.Takeover.SnapZoom
              && DisplayXRGameViewPlacement.TakeoverFor(0.5f, 1.4f) == DisplayXRGameViewPlacement.Takeover.SnapZoom,
              "takeover: Free Aspect at 2x, or a fixed size that fits -> zoom 1, the size is kept");
    }

    // ---- (#459) The owner of the Game view's size and zoom during Play ----------------------
    // A model of the Game view as far as the owner is concerned: a size list (built-in sizes,
    // then custom ones), a selected index, a uniform zoom and a scroll; Unity's rule that a
    // Fixed Resolution larger than the area is fitted (zoom below 1) while the zoom sits on
    // its default. The steps the owner asks for are applied to it the way the editor side
    // does, and at the end of Play the recorded pre-Play state is put back.
    sealed class FakeView
    {
        public struct Size { public string Name; public int W, H; }   // W == 0: Free Aspect / an aspect ratio
        public readonly System.Collections.Generic.List<Size> Sizes = new System.Collections.Generic.List<Size>
        {
            new Size { Name = "Free Aspect" }, new Size { Name = "16:9" }, new Size { Name = "16:10" },
            new Size { Name = "Full HD", W = 1920, H = 1080 }, new Size { Name = "WXGA", W = 1366, H = 768 },
            new Size { Name = "QHD", W = 2560, H = 1440 }, new Size { Name = "4K UHD", W = 3840, H = 2160 },
        };
        public const string Ours = "DisplayXR whole-pixel";
        public int Selected; public float Zoom = 1f; public float ScrollX, ScrollY;
        public float AreaWPt, AreaHPt, Scale;
        // The pre-Play record (what the editor side writes to Library/ before its first change).
        public bool HaveRecord; public int RecIndex; public float RecZoom, RecScrollX, RecScrollY; public string RecName;
        public int MidPlayRestores;   // must stay 0: the pre-Play size never comes back during Play

        public int OursIndex() => Sizes.FindIndex(z => z.Name == Ours);
        public float Fit()
        {
            Size z = Sizes[Selected];
            return z.W <= 0 ? 1f : Math.Min(AreaWPt * Scale / z.W, AreaHPt * Scale / z.H);
        }
        // Unity: a size that does not fit is shown at its fit; it cannot be shown larger.
        public void UnityRefit() { float f = Fit(); if (f < 1f) Zoom = f; }
        public void Resize(float wPt, float hPt) { AreaWPt = wPt; AreaHPt = hPt; UnityRefit(); }
        public void UserPicks(int index) { Selected = index; float f = Fit(); Zoom = f < 1f ? f : 1f; }

        public DisplayXRGameViewOwner.Obs Look(double now, bool wholePixelOn, bool takeoverOn = true) => new DisplayXRGameViewOwner.Obs
        {
            Selected = Selected, OursIndex = OursIndex(), ZoomX = Zoom, FitScale = Fit(), Scale = Scale,
            AreaWPt = AreaWPt, AreaHPt = AreaHPt, WholePixelOn = wholePixelOn, TakeoverOn = takeoverOn, Now = now,
        };

        void Record()
        {
            if (HaveRecord) return;
            HaveRecord = true; RecIndex = Selected; RecZoom = Zoom; RecScrollX = ScrollX; RecScrollY = ScrollY; RecName = Sizes[Selected].Name;
        }

        public void Carry(DisplayXRGameViewOwner owner, DisplayXRGameViewOwner.Step st)
        {
            switch (st.Kind)
            {
            case DisplayXRGameViewOwner.Act.None: return;
            case DisplayXRGameViewOwner.Act.StandDown: break;
            case DisplayXRGameViewOwner.Act.ZoomOne: Record(); Zoom = 1f; ScrollX = ScrollY = 0f; break;
            case DisplayXRGameViewOwner.Act.FreeAspect: Record(); Selected = 0; Zoom = 1f; ScrollX = ScrollY = 0f; break;
            case DisplayXRGameViewOwner.Act.WholePixel:
                Record();
                int i = OursIndex();
                if (i < 0) { Sizes.Add(new Size { Name = Ours, W = st.WPx, H = st.HPx }); i = Sizes.Count - 1; }
                else Sizes[i] = new Size { Name = Ours, W = st.WPx, H = st.HPx };
                Selected = i; Zoom = 1f; ScrollX = ScrollY = 0f;
                break;
            }
            owner.Applied(st, Selected);
        }

        // Play ends: the pre-Play state back, whatever happened in between; our entry removed.
        public void EndPlay(DisplayXRGameViewOwner owner)
        {
            int i = OursIndex();
            if (HaveRecord) { Selected = RecIndex; Zoom = RecZoom; ScrollX = RecScrollX; ScrollY = RecScrollY; }
            else if (i >= 0 && Selected == i) Selected = 0;
            if (i >= 0) Sizes.RemoveAt(i);
            HaveRecord = false;
            owner.Reset();
        }
    }

    // Run the owner on the view for `seconds`, polling every 0.1 s as the editor side does.
    // Returns the steps that did something; counts stand-downs and re-fits.
    static System.Collections.Generic.List<DisplayXRGameViewOwner.Step> Run(DisplayXRGameViewOwner owner, FakeView v, ref double now,
                                                                             double seconds, bool wholePixelOn, bool takeoverOn = true)
    {
        var done = new System.Collections.Generic.List<DisplayXRGameViewOwner.Step>();
        for (double end = now + seconds; now < end; now += 0.1)
        {
            var st = owner.Next(v.Look(now, wholePixelOn, takeoverOn));
            if (st.Kind == DisplayXRGameViewOwner.Act.None) continue;
            // The invariant behind the hardware bug: no step of the owner ever puts the
            // pre-Play size back during Play (a non-zero recorded index selected again).
            bool hadRecord = v.HaveRecord; int before = v.Selected;
            v.Carry(owner, st);
            if (hadRecord && v.RecIndex != 0 && v.Selected == v.RecIndex && before != v.RecIndex) v.MidPlayRestores++;
            done.Add(st);
        }
        return done;
    }

    static int Count(System.Collections.Generic.List<DisplayXRGameViewOwner.Step> steps, DisplayXRGameViewOwner.Act kind)
    {
        int n = 0;
        foreach (var st in steps) if (st.Kind == kind) n++;
        return n;
    }

    static void OwnerStateMachine()
    {
        const DisplayXRGameViewOwner.Act ZoomOne = DisplayXRGameViewOwner.Act.ZoomOne,
              FreeAspect = DisplayXRGameViewOwner.Act.FreeAspect, WholePixel = DisplayXRGameViewOwner.Act.WholePixel,
              StandDown = DisplayXRGameViewOwner.Act.StandDown;

        // ---- A. The hardware session of 2026-11-18, 175 %: Play from "4K UHD" at its fit,
        //         takeover, whole-pixel, then splitter drags in both directions.
        {
            var v = new FakeView { Scale = 1.75f, Selected = 6, ScrollX = 3f, ScrollY = -7f };
            v.Resize(641.429f, 343.857f);
            float fit0 = v.Zoom;
            Check(Math.Abs(fit0 - 0.2786f) < 1e-3, $"A: the model shows 4K UHD at its fit ({fit0:F4}; hardware 0.2786)");
            var owner = new DisplayXRGameViewOwner();
            double now = 0;

            var s1 = Run(owner, v, ref now, 0.3, wholePixelOn: false);
            Check(s1.Count == 1 && s1[0].Kind == FreeAspect && v.Selected == 0 && v.Zoom == 1f && owner.Held == DisplayXRGameViewOwner.Holds.FreeAspect,
                  "A: a 4K view that cannot be shown at 1x is taken over: Free Aspect, zoom 1");
            Check(v.HaveRecord && v.RecIndex == 6 && v.RecZoom == fit0 && v.RecScrollX == 3f && v.RecScrollY == -7f,
                  "A: the pre-Play size, zoom and scroll were recorded before the first change");
            Check(Run(owner, v, ref now, 2.0, wholePixelOn: false).Count == 0, "A: the plug-in's own Free Aspect is not a user pick; nothing more to do");

            // The calibration measured a view that is not 1:1 at 175 %: the whole-pixel size is wanted.
            var s2 = Run(owner, v, ref now, 1.0, wholePixelOn: true);
            Check(s2.Count == 1 && s2[0].Kind == WholePixel && s2[0].WPx == 1120 && s2[0].HPx == 595 && !s2[0].Refit,
                  $"A: whole-pixel 1120x595 selected once the area settled (hardware: 1120x595); got {(s2.Count > 0 ? s2[0].WPx + "x" + s2[0].HPx : "nothing")}");
            Check(v.Selected == v.OursIndex() && v.Zoom == 1f && owner.Held == DisplayXRGameViewOwner.Holds.WholePixel, "A: the view is on the plug-in's entry at zoom 1");
            Check(Run(owner, v, ref now, 2.0, wholePixelOn: true).Count == 0, "A: its own entry is not a user pick");

            // First drag: the pane grows (hardware: 709.429 pt -> 1239x595).
            v.Resize(709.429f, 343.857f);
            var s3 = Run(owner, v, ref now, 1.0, wholePixelOn: true);
            Check(s3.Count == 1 && s3[0].Kind == WholePixel && s3[0].WPx == 1239 && s3[0].HPx == 595,
                  "A: first drag (wider): resized to 1239x595 after the settle time (hardware: 1239x595)");

            // Second drag: the pane SHRINKS under the held size, in steps, as a splitter drag
            // does (hardware host widths 698.29, 685.14, 664.57, 656.57 pt; the area is 2 pt less).
            int standDowns = 0, freeAspects = 0, refits = 0;
            foreach (float hostW in new[] { 698.2857f, 685.1428f, 664.5715f, 656.5714f, 649.7143f })
            {
                v.Resize(hostW - 2f, 343.857f);
                bool scaledBefore = v.Zoom < 1f;
                var st = Run(owner, v, ref now, 0.1, wholePixelOn: true);
                standDowns += Count(st, StandDown); freeAspects += Count(st, FreeAspect);
                foreach (var x in st) if (x.Kind == WholePixel && x.Refit) refits++;
                Check(v.Selected == v.OursIndex() && v.Zoom == 1f && v.Fit() >= 1f,
                      $"A: second drag, host {hostW} pt: still on the plug-in's entry, zoom 1, fits (it was {(scaledBefore ? "scaled" : "fitting")} for one poll at most)");
            }
            Check(standDowns == 0 && freeAspects == 0, $"A: the shrinking drag never selects Free Aspect ({freeAspects}) and is never read as a user pick ({standDowns}) - the hardware bug");
            Check(refits >= 4, $"A: a held size that stops fitting is re-fitted at once ({refits} re-fits over 5 widths)");
            var s4 = Run(owner, v, ref now, 1.0, wholePixelOn: true);
            Check(Count(s4, StandDown) == 0 && v.Selected == v.OursIndex(), "A: after the drag settles the view is still on the plug-in's entry");
            int wantW = (int)Math.Floor(647.7143 / 4 + 0.001) * 4 * 175 / 100;
            Check(owner.AppliedWPx == wantW && owner.AppliedHPx == 595, $"A: settled on {owner.AppliedWPx}x{owner.AppliedHPx} (want {wantW}x595)");

            // N more resizes in a row, both directions, height too.
            int n = 0;
            foreach (var (w, h) in new[] { (700f, 343.857f), (520f, 343.857f), (900f, 500f), (901f, 500f), (300f, 200f), (641.429f, 343.857f), (640f, 100f), (1200f, 620f) })
            {
                v.Resize(w, h);
                var st = Run(owner, v, ref now, 1.0, wholePixelOn: true);
                DisplayXRGameViewPlacement.WholePixelSize(w, h, 1.75f, out _, out _, out int ew, out int eh);
                Check(Count(st, StandDown) == 0 && Count(st, FreeAspect) == 0 && v.Selected == v.OursIndex() && v.Zoom == 1f
                      && owner.AppliedWPx == ew && owner.AppliedHPx == eh,
                      $"A: resize {++n} to {w}x{h} pt: whole-pixel {owner.AppliedWPx}x{owner.AppliedHPx} (want {ew}x{eh}), zoom 1, no stand-down");
            }
            Check(v.MidPlayRestores == 0 && !owner.UserPicked, "A: the pre-Play size never came back during Play");

            v.EndPlay(owner);
            Check(v.Selected == 6 && v.Zoom == fit0 && v.ScrollX == 3f && v.ScrollY == -7f && v.OursIndex() < 0 && v.Sizes.Count == 7,
                  "A: Play end restores exactly the pre-Play state (4K UHD, its zoom, its scroll) and removes the entry");
            Check(owner.Held == DisplayXRGameViewOwner.Holds.Nothing && !owner.Changed && !owner.UserPicked, "A: the owner is reset");
        }

        // ---- B. From Free Aspect at zoom 1 (the common case), 175 %.
        {
            var v = new FakeView { Scale = 1.75f };
            v.Resize(641.429f, 343.857f);
            var owner = new DisplayXRGameViewOwner();
            double now = 0;
            Check(Run(owner, v, ref now, 2.0, wholePixelOn: false).Count == 0 && !v.HaveRecord && !owner.Changed,
                  "B: Free Aspect at zoom 1 without the whole-pixel size: untouched, nothing recorded");
            var st = Run(owner, v, ref now, 1.0, wholePixelOn: true);
            Check(st.Count == 1 && st[0].Kind == WholePixel && v.RecIndex == 0 && v.RecZoom == 1f, "B: whole-pixel selected; pre-Play = Free Aspect at 1x");
            for (int i = 0; i < 6; i++)
            {
                v.Resize(500f + 37f * i * (i % 2 == 0 ? 1 : -1) + 200f, 343.857f - 11f * i);
                var r = Run(owner, v, ref now, 1.0, wholePixelOn: true);
                Check(Count(r, StandDown) == 0 && Count(r, FreeAspect) == 0 && v.Selected == v.OursIndex() && v.Zoom == 1f,
                      $"B: resize {i + 1}: on the plug-in's entry {owner.AppliedWPx}x{owner.AppliedHPx}, zoom 1");
            }
            v.EndPlay(owner);
            Check(v.Selected == 0 && v.Zoom == 1f && v.OursIndex() < 0, "B: Play end: Free Aspect at 1x, entry removed");
        }

        // ---- C. Free Aspect with the Scale slider at 2x: zoom only, the size is kept.
        {
            var v = new FakeView { Scale = 2.5f, Zoom = 2f, ScrollX = -120f, ScrollY = 44f };
            v.AreaWPt = 691.2f; v.AreaHPt = 343.8f;
            var owner = new DisplayXRGameViewOwner();
            double now = 0;
            var st = Run(owner, v, ref now, 0.3, wholePixelOn: false);
            Check(st.Count == 1 && st[0].Kind == ZoomOne && v.Selected == 0 && v.Zoom == 1f && owner.Held == DisplayXRGameViewOwner.Holds.Zoom,
                  "C: zoom 2x on Free Aspect: zoom set to 1, size kept");
            v.EndPlay(owner);
            Check(v.Selected == 0 && v.Zoom == 2f && v.ScrollX == -120f && v.ScrollY == 44f, "C: Play end: zoom 2x and the scroll are back");
        }

        // ---- D. A real user pick during Play, from each held state.
        foreach (string held in new[] { "FreeAspect", "WholePixel" })
        {
            var v = new FakeView { Scale = 1.75f, Selected = 6 };
            v.Resize(641.429f, 343.857f);
            float fit0 = v.Zoom;
            var owner = new DisplayXRGameViewOwner();
            double now = 0;
            Run(owner, v, ref now, 1.0, wholePixelOn: held == "WholePixel");
            Run(owner, v, ref now, 1.0, wholePixelOn: held == "WholePixel");
            Check(owner.Held.ToString() == held, $"D/{held}: set-up holds {owner.Held}");
            v.UserPicks(3);   // Full HD: does not fit either, Unity shows it scaled
            var st = Run(owner, v, ref now, 2.0, wholePixelOn: held == "WholePixel");
            Check(st.Count == 1 && st[0].Kind == StandDown && owner.UserPicked, $"D/{held}: a user pick switches the feature off, once ({st.Count} steps)");
            Check(v.Selected == 3 && v.MidPlayRestores == 0, $"D/{held}: the user's pick is left alone; the pre-Play size does NOT come back during Play");
            v.Resize(700f, 343.857f);
            v.UserPicks(0);
            Check(Run(owner, v, ref now, 2.0, wholePixelOn: true).Count == 0 && v.Selected == 0, $"D/{held}: nothing more happens until Play ends, whatever the user does");
            v.EndPlay(owner);
            Check(v.Selected == 6 && v.Zoom == fit0 && v.OursIndex() < 0, $"D/{held}: Play end still restores the pre-Play 4K UHD and its zoom");
        }

        // ---- E. The user deletes the plug-in's entry from the size menu.
        {
            var v = new FakeView { Scale = 1.75f };
            v.Resize(641.429f, 343.857f);
            var owner = new DisplayXRGameViewOwner();
            double now = 0;
            Run(owner, v, ref now, 1.0, wholePixelOn: true);
            v.Sizes.RemoveAt(v.OursIndex()); v.Selected = 6; v.UnityRefit();   // Unity clamps to the last entry
            var st = Run(owner, v, ref now, 1.0, wholePixelOn: true);
            Check(st.Count == 1 && st[0].Kind == StandDown, "E: a deleted entry is a user action: stand down, do not re-create it");
            v.EndPlay(owner);
            Check(v.Selected == 0 && v.Zoom == 1f, "E: Play end: Free Aspect at 1x");
        }

        // ---- F. The user zooms during Play: put right twice, then theirs.
        {
            var v = new FakeView { Scale = 1.75f };
            v.Resize(641.429f, 343.857f);
            var owner = new DisplayXRGameViewOwner();
            double now = 0;
            Run(owner, v, ref now, 1.0, wholePixelOn: true);
            int idx = v.Selected;
            v.Zoom = 2f;
            var a = Run(owner, v, ref now, 0.3, wholePixelOn: true);
            Check(a.Count == 1 && a[0].Kind == ZoomOne && v.Zoom == 1f && v.Selected == idx, "F: a zoom on the held whole-pixel size is set back to 1 (the size is kept)");
            v.Zoom = 3f;
            Run(owner, v, ref now, 0.3, wholePixelOn: true);
            v.Zoom = 2f;
            var c = Run(owner, v, ref now, 1.0, wholePixelOn: true);
            Check(c.Count == 1 && c[0].Kind == StandDown && v.Zoom == 2f && v.Selected == idx, "F: the third time it is the user's: stand down, size and zoom left alone");
            v.EndPlay(owner);
            Check(v.Selected == 0 && v.Zoom == 1f && v.OursIndex() < 0, "F: Play end restores Free Aspect at 1x");
        }

        // ---- G. Opted out of the takeover: a zoomed view is left alone; a held whole-pixel
        //         size is still re-fitted (that is the plug-in's own size, not a takeover).
        {
            var v = new FakeView { Scale = 1.75f, Selected = 6 };
            v.Resize(641.429f, 343.857f);
            var owner = new DisplayXRGameViewOwner();
            double now = 0;
            Check(Run(owner, v, ref now, 3.0, wholePixelOn: true, takeoverOn: false).Count == 0 && v.Selected == 6 && !v.HaveRecord,
                  "G: takeover off: a scaled 4K view is not touched (no whole-pixel either: the view is not at zoom 1)");
            v.UserPicks(0);
            var st = Run(owner, v, ref now, 1.0, wholePixelOn: true, takeoverOn: false);
            Check(st.Count == 1 && st[0].Kind == WholePixel, "G: at zoom 1 the whole-pixel size is selected as usual");
            v.Resize(500f, 343.857f);
            var rf = Run(owner, v, ref now, 0.1, wholePixelOn: true, takeoverOn: false);
            Check(rf.Count == 1 && rf[0].Kind == WholePixel && rf[0].Refit && v.Zoom == 1f, "G: takeover off: a shrinking pane still re-fits the held size");
            v.EndPlay(owner);
        }

        // ---- H. Whole display scale: nothing to select; a held size is given up for Free Aspect.
        {
            var v = new FakeView { Scale = 1.75f };
            v.Resize(641.429f, 343.857f);
            var owner = new DisplayXRGameViewOwner();
            double now = 0;
            Run(owner, v, ref now, 1.0, wholePixelOn: true);
            v.Scale = 2f; v.Resize(560f, 300f);   // the editor moved to a 200 % monitor
            var st = Run(owner, v, ref now, 1.0, wholePixelOn: true);
            Check(Count(st, StandDown) == 0 && v.Selected == 0 && v.Zoom == 1f && owner.Held == DisplayXRGameViewOwner.Holds.FreeAspect,
                  "H: at a whole scale the held whole-pixel size is given up for Free Aspect (1:1 there), not for the pre-Play size");
            var v2 = new FakeView { Scale = 2f }; v2.Resize(560f, 300f);
            var o2 = new DisplayXRGameViewOwner(); double n2 = 0;
            Check(Run(o2, v2, ref n2, 2.0, wholePixelOn: true).Count == 0 && !v2.HaveRecord, "H: at a whole scale an untouched view stays untouched");
            v.EndPlay(owner);
            Check(v.Selected == 0 && v.OursIndex() < 0, "H: Play end removes the entry");
        }

        // ---- J. The harness pass "rzfree" (175 %, from Free Aspect): at the third resize the
        //         pane was 961 px for a 549.7142 pt host. 961 / 549.7142 = 1.748181, which did
        //         not snap to 1.75, so the scale read "not a whole percent": the held size was
        //         given up for Free Aspect and nothing was selected again for the whole step.
        {
            float raw = 961 / 549.7142f;
            Check(DisplayXRGameViewPlacement.WholePixelStepPt(raw) == 0, $"J: the bare quotient {raw:R} is not a whole percent (the cause)");
            Check(DisplayXRGameViewPlacement.PaneScale(961, 549.7142f) == 1.75f, "J: PaneScale(961 px, 549.7142 pt) == 1.75 (one pixel of rounding)");
            Check(DisplayXRGameViewPlacement.PaneScale(962, 549.7142f) == 1.75f && DisplayXRGameViewPlacement.PaneScale(1242, 709.7142f) == 1.75f
                  && DisplayXRGameViewPlacement.PaneScale(2213, 737.6666f) == 3f && DisplayXRGameViewPlacement.PaneScale(1733, 693.2f) == 2.5f
                  && DisplayXRGameViewPlacement.PaneScale(1476, 984f) == 1.5f && DisplayXRGameViewPlacement.PaneScale(400, 229f) == 1.75f,
                  "J: PaneScale on logged panes at 175 / 300 / 250 / 150 %");
            float custom = DisplayXRGameViewPlacement.PaneScale(1333, 1000f);
            Check(Math.Abs(custom - 1.333f) < 1e-6 && DisplayXRGameViewPlacement.WholePixelStepPt(custom) == 0
                  && DisplayXRGameViewPlacement.PaneScale(0, 500f) == 0f && DisplayXRGameViewPlacement.PaneScale(500, 0f) == 0f,
                  "J: a custom scale (133.3 %) is not forced onto a whole percent; bad inputs give 0");
            int wrong = 0;
            foreach (float sc in new[] { 1.25f, 1.5f, 1.75f, 2f, 2.25f, 2.5f, 3f })
                for (float host = 160f; host < 1400f; host += 0.5714285f)
                    foreach (int px in new[] { (int)Math.Floor(host * sc), (int)Math.Round(host * sc), (int)Math.Ceiling(host * sc) })
                        if (DisplayXRGameViewPlacement.PaneScale(px, host) != sc) wrong++;
            Check(wrong == 0, $"J: PaneScale recovers the scale for every pane width from 160 to 1400 pt, px floored / rounded / ceiled ({wrong} wrong)");

            // The owner, fed what the harness pass saw.
            var v = new FakeView { Scale = 1.75f };
            v.Resize(684.857f, 343.857f);
            var owner = new DisplayXRGameViewOwner();
            double now = 0;
            Run(owner, v, ref now, 1.0, wholePixelOn: true);
            v.Resize(739.714f, 343.857f); Run(owner, v, ref now, 1.0, wholePixelOn: true);
            v.Resize(616.857f, 343.857f); Run(owner, v, ref now, 1.0, wholePixelOn: true);
            Check(owner.AppliedWPx == 1078 && owner.AppliedHPx == 595, $"J: steps 1-2 as in the pass: {owner.AppliedWPx}x{owner.AppliedHPx} (1078x595)");
            // Step 3 with the scale as it read before the fix.
            v.Scale = raw; v.Resize(547.7142f, 343.857f);
            var bad = Run(owner, v, ref now, 1.0, wholePixelOn: true);
            Check(Count(bad, FreeAspect) == 0 && Count(bad, StandDown) == 0 && v.Selected == v.OursIndex() && owner.Held == DisplayXRGameViewOwner.Holds.WholePixel,
                  "J: a scale that reads 'not a whole percent' while a size is held does not give the size up (it did: Free Aspect for the whole step)");
            // ...and with the scale as PaneScale reads it: re-fitted at once.
            v.Scale = DisplayXRGameViewPlacement.PaneScale(961, 549.7142f);
            var good = Run(owner, v, ref now, 0.1, wholePixelOn: true);
            Check(good.Count == 1 && good[0].Kind == WholePixel && good[0].Refit && good[0].WPx == 952 && good[0].HPx == 595 && v.Zoom == 1f,
                  $"J: step 3 (x0.8, 547.71 pt): re-fitted to 952x595 at once (the 4K-start pass did: 952x595); got {(good.Count > 0 ? good[0].WPx + "x" + good[0].HPx : "nothing")}");
            v.EndPlay(owner);
            Check(v.Selected == 0 && v.Zoom == 1f && v.OursIndex() < 0, "J: Play end: Free Aspect at 1x");
        }

        // ---- K. The scroll after Play: checked exactly while the view area is the size it was
        //         recorded in; not at all once the pane was resized during Play (hardware:
        //         "scroll (359,171.36) instead of (342.43,171.93) after 28 re-applications" -
        //         Unity re-centres it to half the NEW area on every OnGUI).
        {
            Check(DisplayXRGameViewPlacement.ScrollIsComparable(684.857f, 343.857f, 684.857f, 343.857f),
                  "K: same view area: the recorded scroll is checked exactly");
            Check(DisplayXRGameViewPlacement.ScrollIsComparable(684.857f, 343.857f, 684.86f, 343.86f),
                  "K: the same area to float noise is the same area");
            Check(!DisplayXRGameViewPlacement.ScrollIsComparable(684.857f, 343.857f, 718f, 342.714f),
                  "K: hardware pass 1 (684.86 pt area at capture, 718 pt at restore): the scroll is not checked");
            Check(!DisplayXRGameViewPlacement.ScrollIsComparable(718f, 342.714f, 723.714f, 342.714f)
                  && !DisplayXRGameViewPlacement.ScrollIsComparable(718f, 342.714f, 718f, 343.857f),
                  "K: hardware pass 2 (718 -> 723.71 pt), and a change of height alone: not checked");
            Check(!DisplayXRGameViewPlacement.ScrollIsComparable(684.857f, 343.857f, 685.4286f, 343.857f),
                  "K: half a point is a different area (the harness main-window round trip)");
            Check(DisplayXRGameViewPlacement.ScrollIsComparable(0f, 0f, 718f, 342.714f) && DisplayXRGameViewPlacement.ScrollIsComparable(684f, 0f, 1f, 1f),
                  "K: a record without an area (an older build) keeps the exact check");
            // The numbers behind it: a centred view scroll is half its area.
            Check(Math.Abs(684.857 / 2 - 342.43) < 0.01 && Math.Abs(718.0 / 2 - 359.0) < 0.01 && Math.Abs(723.714 / 2 - 361.86) < 0.01,
                  "K: the logged scrolls 342.43 / 359 / 361.86 are half of 684.86 / 718 / 723.71 pt");
        }

        // ---- I. Settle: a growing drag costs one size change, not one per poll.
        {
            var v = new FakeView { Scale = 1.75f };
            v.Resize(641.429f, 343.857f);
            var owner = new DisplayXRGameViewOwner();
            double now = 0;
            Run(owner, v, ref now, 1.0, wholePixelOn: true);
            int changes = 0;
            for (int i = 1; i <= 20; i++) { v.Resize(641.429f + 9f * i, 343.857f); changes += Run(owner, v, ref now, 0.1, wholePixelOn: true).Count; }
            Check(changes == 0, $"I: 20 polls of a growing drag: no size change while it moves ({changes})");
            Check(Run(owner, v, ref now, 1.0, wholePixelOn: true).Count == 1, "I: one size change when it settles");
            Check(Run(owner, v, ref now, 3.0, wholePixelOn: true).Count == 0, "I: none after that");
            v.EndPlay(owner);
        }
    }

    static void WholePixelDefinitions()
    {
        // The step: the smallest number of points that is EXACTLY a whole number of pixels.
        foreach (var (scale, q, px) in new[]
        {
            (1f, 1, 1), (1.25f, 4, 5), (1.5f, 2, 3), (1.75f, 4, 7), (2f, 1, 2), (2.25f, 4, 9), (2.5f, 2, 5), (3f, 1, 3),
            (3.0000003f, 1, 3), (1.7499999f, 4, 7), (1.4999998f, 2, 3),   // measured scales (pane px / host pt)
            (1.1f, 10, 11), (1.2f, 5, 6),
            // Custom Windows scales a loose tolerance got wrong (3 pt at 133 % is 3.99 px, 7 pt at
            // 143 % is 10.01 px, 3 pt at 167 % is 5.01 px; 199 / 201 / 299 / 301 % are not whole scales).
            (1.33f, 100, 133), (1.43f, 100, 143), (1.67f, 100, 167),
            (1.99f, 100, 199), (2.01f, 100, 201), (2.99f, 100, 299), (3.01f, 100, 301),
            (1.3333f, 0, 0), (0f, 0, 0), (9f, 0, 0),                      // not a whole percent / implausible
        })
        {
            int got = DisplayXRGameViewPlacement.WholePixelStepPt(scale);
            Check(got == q, $"WholePixelStepPt({scale:R}) == {q} (got {got})");
            if (q > 0)
                Check(q * (int)Math.Round(scale * 100.0) == px * 100, $"{q} pt at {scale:R} is exactly {px} px");
        }
        // ...for every whole percent: a true whole number of pixels, and no smaller step is one.
        {
            int bad = 0; string first = null;
            for (int pct = 51; pct <= 799; pct++)
            {
                int q = DisplayXRGameViewPlacement.WholePixelStepPt(pct / 100f);
                bool ok = q >= 1 && q <= DisplayXRGameViewPlacement.WholePixelMaxStepPt && q * pct % 100 == 0;
                for (int smaller = 1; ok && smaller < q; smaller++)
                    if (smaller * pct % 100 == 0) ok = false;
                if (!ok) { bad++; first ??= $"{pct} % -> {q}"; }
            }
            Check(bad == 0, $"WholePixelStepPt is the smallest exact step at every whole percent 51-799 ({bad} are not; {first})");
        }

        // The size: the largest multiple of the step that fits the view area.
        foreach (var (scale, vw, vh, wPt, hPt, wPx, hPx) in new[]
        {
            (1.75f, 443.9f,    647.5f,    440, 644,  770, 1127),
            (1.75f, 450f,      653f,      448, 652,  784, 1141),
            (1.75f, 1099.143f, 631.4286f, 1096, 628, 1918, 1099),
            (1.5f,  1469.333f, 719.6667f, 1468, 718, 2202, 1077),
            (1.25f, 982f,      407.4f,    980, 404, 1225,  505),
            (2.25f, 801.7f,    400f,      800, 400, 1800,  900),
            (2.5f,  691.2f,    343.8f,    690, 342, 1725,  855),
            (1.75f, 440f,      644f,      440, 644,  770, 1127),   // an exact fit keeps it all
            (1.7499999f, 443.9f, 647.5f,  440, 644,  770, 1127),   // measured-scale noise
            (1.1f,  1005f,     603f,      1000, 600, 1100,  660),
            (1.33f, 1050f,     699f,      1000, 600, 1330,  798),  // 100 pt steps: a wide margin, but exact
        })
        {
            bool ok = DisplayXRGameViewPlacement.WholePixelSize(vw, vh, scale, out int a, out int b, out int c, out int d);
            Check(ok && a == wPt && b == hPt && c == wPx && d == hPx,
                  $"WholePixelSize({vw}x{vh} pt at {scale:R}) == {wPt}x{hPt} pt = {wPx}x{hPx} px (got {ok} {a}x{b} {c}x{d})");
        }
        foreach (float scale in new[] { 1f, 2f, 3f, 1.3333f, 0f })
            Check(!DisplayXRGameViewPlacement.WholePixelSize(1000f, 600f, scale, out _, out _, out int zw, out int zh) && zw == 0 && zh == 0,
                  $"WholePixelSize at {scale:R}: nothing to select (a whole scale, or not a whole percent)");
        Check(!DisplayXRGameViewPlacement.WholePixelSize(3f, 600f, 1.75f, out _, out _, out _, out _), "an area narrower than one step has no size");
        Check(!DisplayXRGameViewPlacement.WholePixelSize(99f, 600f, 1.33f, out _, out _, out _, out _), "133 %: an area under 100 pt has no size");
        // ...for every area a sweep tries: whole points, exactly whole pixels, a margin under one step.
        {
            int bad = 0; string first = null;
            foreach (int pct in new[] { 110, 125, 133, 150, 175, 225, 250 })
            {
                float scale = pct / 100f;
                int q = DisplayXRGameViewPlacement.WholePixelStepPt(scale);
                for (int pw = 400; pw <= 2400; pw += 7)
                    for (int ph = 300; ph <= 1250; ph += 13)
                    {
                        float vw = pw / scale - 2f, vh = ph / scale - 47f;   // a docked pane's area
                        bool fits = vw + 0.001f * q >= q && vh + 0.001f * q >= q;
                        bool got = DisplayXRGameViewPlacement.WholePixelSize(vw, vh, scale, out int wPt, out int hPt, out int wPx, out int hPx);
                        if (!fits) { if (got) { bad++; first ??= $"{pct} %: a size for {vw}x{vh}"; } continue; }
                        bool ok = got && wPt % q == 0 && hPt % q == 0 && wPt <= vw + 0.01f * q && hPt <= vh + 0.01f * q
                                  && vw - wPt < q && vh - hPt < q
                                  && wPt * pct == wPx * 100 && hPt * pct == hPx * 100;
                        if (!ok) { bad++; first ??= $"{pct} %: {vw}x{vh} pt -> {wPt}x{hPt} pt = {wPx}x{hPx} px"; }
                    }
            }
            Check(bad == 0, $"whole-pixel sizes at 110-250 % are whole points and exactly whole pixels within one step of the area ({bad} are not; {first})");
        }

        // Line counts.
        Check(DisplayXRGameViewPlacement.LinesDroppedOrDoubled(878.4, 880) == -2 && DisplayXRGameViewPlacement.LinesDroppedOrDoubled(881.0, 880) == 1
              && DisplayXRGameViewPlacement.LinesDroppedOrDoubled(879.5, 880) == -1 && DisplayXRGameViewPlacement.LinesDroppedOrDoubled(880.25, 880) == 1
              && DisplayXRGameViewPlacement.LinesDroppedOrDoubled(880.01, 880) == 0 && DisplayXRGameViewPlacement.LinesDroppedOrDoubled(880, 0) == 0,
              "LinesDroppedOrDoubled counts whole lines (any sub-pixel mismatch is up to one) and ignores float noise");
        Check(DisplayXRGameViewPlacement.IsOneToOne(880.01, 880) && !DisplayXRGameViewPlacement.IsOneToOne(880.25, 880)
              && !DisplayXRGameViewPlacement.IsOneToOne(879.5, 880) && !DisplayXRGameViewPlacement.IsOneToOne(880, 0),
              "IsOneToOne: a quarter pixel is a mismatch, float noise is not");
        Check(DisplayXRGameViewPlacement.LinesShown(1501.5, 879.5) == 879 && DisplayXRGameViewPlacement.LinesShown(1508.5, 770) == 770
              && DisplayXRGameViewPlacement.LinesShown(366.0, 880.5) == 881,
              "LinesShown: each edge on the next whole pixel at or after it");

        // A pane border in px (half up; x.25 rounds DOWN here).
        foreach (var (pt, scale, want) in new[] { (1f, 2.5f, 3), (1f, 3f, 3), (1f, 2f, 2), (1f, 1.5f, 2), (1f, 1.75f, 2), (1f, 1.25f, 1), (1f, 1f, 1), (0f, 2.5f, 0), (0f, 1.75f, 0) })
            Check(DisplayXRGameViewPlacement.BorderPx(pt, scale) == want, $"BorderPx({pt} pt at {scale:R}) == {want}");
    }

    static void ModelConsistency()
    {
        // ---- F1 / F2, predicted: a docked Free Aspect pane at every scale. 2 pt of side borders, 47 pt
        // of chrome above and below, a whole-pixel pane, the texture = UnityRt(area). ----
        // dW: drawn - texture columns at zoom 1; dFit: drawn - texture rows under GVFIT (h / (h - 0.5));
        // dRaw: the same at zoom 1.
        foreach (var (scale, dW, dFit, dRaw) in new[]
        {
            (1f,    0.0,  0.5,  0.0),
            (1.25f, -0.5, 0.75, 0.25),
            (1.5f,  0.0,  0.0,  -0.5),
            (1.75f, -0.5, 0.25, -0.25),
            (2f,    0.0,  0.5,  0.0),
            (2.25f, -0.5, 0.75, 0.25),
            (2.5f,  0.0,  0.0,  -0.5),
            (3f,    0.0,  0.5,  0.0),
        })
        {
            int bad = 0, n = 0; string first = null;
            for (int pw = 900; pw <= 2400; pw += 7)
                for (int ph = 700; ph <= 1250; ph += 13)
                {
                    double tw = pw - 2.0 * scale, th = ph - 47.0 * scale;   // Unity's target = the area, px
                    int rtW = UnityRt(tw), rtH = UnityRt(th);
                    double w = DisplayXRGameViewPlacement.DrawnPx(tw, 1.0) - rtW;
                    double fit = DisplayXRGameViewPlacement.DrawnPx(th, rtH / (rtH - 0.5)) - rtH;
                    double raw = DisplayXRGameViewPlacement.DrawnPx(th, 1.0) - rtH;
                    n++;
                    // GVFIT's stretch is not exactly +0.5: (h + f) * h / (h - 0.5) - h, within 1e-3 here.
                    bool ok = Math.Abs(w - dW) < 1e-9 && Math.Abs(fit - dFit) < 2e-3 && Math.Abs(raw - dRaw) < 1e-9
                              && DisplayXRGameViewPlacement.IsOneToOne(tw, rtW) == (dW == 0.0)
                              && DisplayXRGameViewPlacement.IsOneToOne(th * rtH / (rtH - 0.5), rtH) == (dFit == 0.0)
                              && DisplayXRGameViewPlacement.IsOneToOne(th, rtH) == (dRaw == 0.0);
                    if (!ok) { bad++; first ??= $"{pw}x{ph}: {w:F3} {fit:F3} {raw:F3}"; }
                }
            Check(bad == 0, $"MODEL {scale * 100} % docked Free Aspect: columns {dW:+0.##;-0.##;0}, rows under GVFIT {dFit:+0.##;-0.##;0}, rows at zoom 1 {dRaw:+0.##;-0.##;0} px vs the texture ({bad} of {n} panes differ; {first})");
        }

        // ---- F4, predicted: the glue's Mathf.RoundToInt (halves to even) against Unity's texture
        // (halves away from zero). They differ exactly where the area ends on x.5 with an even floor:
        // about every other pane height at 150 % and 250 %, never at the other scales' heights. ----
        foreach (var (scale, want) in new[] { (1f, 0), (1.25f, 0), (1.5f, 275), (1.75f, 0), (2f, 0), (2.5f, 276), (3f, 0) })
        {
            int differ = 0, n = 0;
            for (int ph = 700; ph <= 1250; ph++) { n++; double th = ph - 47.0 * scale; if (GlueRt(th) != UnityRt(th)) differ++; }
            Check(differ == want, $"MODEL {scale * 100} %: glue rows != Unity's texture rows at {differ} of {n} pane heights (want {want})");
        }
        // ...and in the columns at 125 / 175 / 225 % (2 pt of borders = x.5 px).
        foreach (var (scale, some) in new[] { (1.25f, true), (1.5f, false), (1.75f, true), (2.25f, true), (2.5f, false) })
        {
            int differ = 0;
            for (int pw = 900; pw <= 2400; pw++) { double tw = pw - 2.0 * scale; if (GlueRt(tw) != UnityRt(tw)) differ++; }
            Check((differ > 0) == some, $"MODEL {scale * 100} %: glue columns != Unity's texture columns at {differ} pane widths");
        }
        // The hardware-verified 250 % dev pane (#421) is not one of them: 859.5 -> 860 both ways, and
        // drawn 1:1 on both axes with its logged zoom (1, 860 / 859.5). The diagnostic is silent there.
        {
            float zoomY = 860f / (860f - 0.5f);
            double dw = DisplayXRGameViewPlacement.DrawnPx(1728f, 1f), dh = DisplayXRGameViewPlacement.DrawnPx(859.5f, zoomY);
            Check(GlueRt(859.5) == 860 && UnityRt(859.5) == 860 && GlueRt(860.5) == 860 && UnityRt(860.5) == 861,
                  "250 %: 859.5 rounds to 860 both ways; 860.5 is 860 for the glue, 861 for Unity");
            Check(DisplayXRGameViewPlacement.IsOneToOne(dw, 1728) && DisplayXRGameViewPlacement.IsOneToOne(dh, 860),
                  $"250 % dev pane: drawn {dw}x{dh} for 1728x860 is 1:1");
        }

        // ---- F3, predicted: a pane docked at the editor window's left or right edge has ONE side
        // border. The Free Aspect anchor is pane left + CentringX(slack); the texture starts at the
        // real left border. 250 %: slack is always 2. 300 %: always 3. ----
        foreach (var (scale, slackWant, anchor, leftEdgeTexture, rightEdgeTexture) in new[] { (2.5f, 2, 1, 0, 3), (3f, 3, 2, 0, 3) })
        {
            int bad = 0;
            for (int pw = 900; pw <= 2400; pw++)
            {
                int slack = pw - UnityRt(pw - 1.0 * scale);   // one 1 pt border
                if (slack != slackWant || DisplayXRGameViewPlacement.CentringX(slack) != anchor) bad++;
            }
            Check(bad == 0 && DisplayXRGameViewPlacement.BorderPx(0f, scale) == leftEdgeTexture && DisplayXRGameViewPlacement.BorderPx(1f, scale) == rightEdgeTexture,
                  $"MODEL {scale * 100} % edge-docked: anchor +{anchor}; texture at +{leftEdgeTexture} (far left: nudge {leftEdgeTexture - anchor}) or +{rightEdgeTexture} (far right: nudge +{rightEdgeTexture - anchor})");
        }

        // ---- F5: the code is inconsistent at a quarter pixel. X (BorderPx, LetterboxOffsets) rounds
        // x.25 DOWN; Y (RtRow0Y) and LinesShown take the next pixel UP. ----
        Check(DisplayXRGameViewPlacement.BorderPx(1f, 1.25f) == 1 && DisplayXRGameViewPlacement.LinesShown(0.0, 1.25) == 2,
              "x.25: BorderPx rounds 1.25 px down to 1, LinesShown's edge rule takes it up to 2");

        // ---- The 175 % numbers reported from another plug-in's captures, RECONSTRUCTED inputs ----
        // Reported: an 880 px wide Free Aspect image drawn 879 px wide (column 440 missing); a 1924 px
        // render area in a 1927 px docked pane. Reconstruction: the area is 880 - 0.5 = 879.5 px (the
        // model's x.5 at 175 %), from edge 1501.5.
        Check(DisplayXRGameViewPlacement.LinesDroppedOrDoubled(DisplayXRGameViewPlacement.DrawnPx(879.5, 1.0), 880) == -1
              && DisplayXRGameViewPlacement.LinesShown(1501.5, 879.5) == 879,
              "MODEL 175 %: an 879.5 px area for an 880 px texture -> one column dropped, 879 shown (reported: 879)");
        Check(UnityRt(1927 - 2 * 1.75) == 1924 && !DisplayXRGameViewPlacement.IsOneToOne(1927 - 2 * 1.75, 1924),
              "MODEL 175 %: a 1927 px pane has a 1923.5 px area and a 1924 px texture (reported: 1924 in 1927), not 1:1");
        // Under GVFIT a whole-pixel Fixed Resolution would be stretched: why that view skips GVFIT.
        Check(DisplayXRGameViewPlacement.LinesDroppedOrDoubled(DisplayXRGameViewPlacement.DrawnPx(1127, 1127.0 / 1126.5), 1127) == 1,
              "MODEL 175 %: 1127 rows under the GVFIT stretch are drawn 1127.5 px high (one doubled)");

        // Reported: a 770x1127 px Fixed Resolution (440x644 pt) at zoom 1 had its first column at 1509
        // and rows 223..1349. The pane below is NOT measured: it is derived backwards from those
        // numbers with this suite's chrome constants (1 pt border + 5 pt centring -> left 1498, 452 pt
        // wide; 7.875 px below the image above a 2 pt border -> top 137, 700 pt high). So this checks
        // only that Place() inverts that derivation, i.e. that the whole-pixel path uses the same
        // rules as the rest of the model (Unity's whole-point rounding, no GVFIT stretch).
        var wp = new DisplayXRGameViewPlacement.Pane
        {
            Left = 1498, Top = 137, ClientW = 791, ClientH = 1225,
            ToolbarPx = DisplayXRGameViewPlacement.ToolbarPx(700f, 653f, 791 / 452f),
            RtW = 770, RtH = 1127, TargetHPx = 1127f, DockedNonMaximized = true, HaveDrawRect = true,
            HostWPt = 452f, DrawWPt = 450f, DrawHPt = 653f, HostHPt = 700f, PtToPx = 791 / 452f, RoundsBottomEdge = true,
            WholePixelFixedSize = true, HaveLeftBorder = true, LeftBorderPt = 1f,
        };
        DisplayXRGameViewPlacement.Place(wp, 0, 0, out int wx, out int wy, out int ww, out int wh);
        Check(wx == 1509 && wy == 223 && ww == 770 && wh == 1127,
              $"MODEL 175 % whole-pixel 770x1127 (reconstructed pane): anchor ({wx},{wy} {ww}x{wh}) == (1509,223 770x1127)");
        // The same pane with the GVFIT stretch left in the bottom-edge rounding lands two rows lower:
        // the stretch must be out of the arithmetic when it is out of the view.
        var wpDefault = wp; wpDefault.WholePixelFixedSize = false;
        DisplayXRGameViewPlacement.Place(wpDefault, 0, 0, out _, out int dy, out _, out _);
        Check(dy == 225, $"MODEL 175 % 770x1127 with the GVFIT stretch modelled: y {dy} == 225");

        // A whole-pixel size within the dock's chrome slack is still a letterbox (Unity's whole-point
        // X), never the #740 pixel centring: 1929 px pane, area 1100.29 pt -> 1100 pt = 1925 px.
        var thinWp = new DisplayXRGameViewPlacement.Pane
        {
            Left = 100, Top = 137, ClientW = 1929, ClientH = 1225,
            ToolbarPx = DisplayXRGameViewPlacement.ToolbarPx(700f, 653f, 1.75f),
            RtW = 1925, RtH = 1141, TargetHPx = 1141f, DockedNonMaximized = true, HaveDrawRect = true,
            HostWPt = 1929 / 1.75f, DrawWPt = 1929 / 1.75f - 2f, DrawHPt = 653f, HostHPt = 700f, PtToPx = 1.75f, RoundsBottomEdge = true,
        };
        Check(DisplayXRGameViewPlacement.IsChromeSlackOnly(thinWp), "MODEL a 4 px slack Fixed Resolution is chrome slack by default");
        thinWp.WholePixelFixedSize = true; thinWp.HaveLeftBorder = true; thinWp.LeftBorderPt = 1f;
        Check(!DisplayXRGameViewPlacement.IsChromeSlackOnly(thinWp), "MODEL ...and a letterbox as the whole-pixel view");
        DisplayXRGameViewPlacement.Place(thinWp, 99, 99, out int tx, out _, out int tw2, out int th2);
        Check(tx == 102 && tw2 == 1925 && th2 == 1141, $"MODEL ...at pane + 1 pt (1.75 px -> column +2): x {tx} == 102, {tw2}x{th2}");

        // Whole-pixel view docked at the editor window's LEFT edge: no left border, only the right
        // one, so host - draw = 1 pt. With the real border (0) the image starts 1 pt in (centring) =
        // column +2; assuming two equal half-point borders puts it at +3.
        var farLeft = new DisplayXRGameViewPlacement.Pane
        {
            Left = 0, Top = 137, ClientW = 790, ClientH = 1225,
            ToolbarPx = DisplayXRGameViewPlacement.ToolbarPx(700f, 653f, 1.75f),
            RtW = 784, RtH = 1141, TargetHPx = 1141f, DockedNonMaximized = true, HaveDrawRect = true,
            HostWPt = 790 / 1.75f, DrawWPt = 790 / 1.75f - 1f, DrawHPt = 653f, HostHPt = 700f, PtToPx = 1.75f, RoundsBottomEdge = true,
            WholePixelFixedSize = true, HaveLeftBorder = true, LeftBorderPt = 0f,
        };
        DisplayXRGameViewPlacement.Place(farLeft, 0, 0, out int flx, out _, out _, out _);
        Check(flx == 2, $"MODEL 175 % whole-pixel far-left pane with its real border (0 pt): x {flx} == 2");
        farLeft.HaveLeftBorder = false;
        DisplayXRGameViewPlacement.Place(farLeft, 0, 0, out int flx2, out _, out _, out _);
        Check(flx2 == 3, $"MODEL ...and with the border unknown (two equal borders assumed): x {flx2} == 3");
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

        // ---- Thin left/right bars are a letterbox, not chrome: the chrome-slack path accepts only
        // the dock's two side borders + rounding, so a 7-64 px bar gets Unity's whole-point X instead
        // of the #740 pixel centring, which was 1-2 px off it (logged 2026-10-27). ----
        foreach (var (scale, want) in new[] { (3f, 8), (3.0000003f, 8), (2.5f, 7), (1.5f, 5), (1.4999998f, 5), (1.25f, 4), (0f, 64) })
            Check(DisplayXRGameViewPlacement.SideChromeSlackMaxPx(scale) == want, $"SideChromeSlackMaxPx({scale:R}) == {want}");
        // Free Aspect leaves exactly the side borders and keeps its path: 6 px at 300 % (main and
        // secondary), 5 at 250 %, 3 at 150 %, 2 at 125 %.
        var b1 = new Geo { Name = "B1 150% docked Free", L = 726, T = 118, W = 2207, H = 1150, Hw = 1471.333f, Hh = 766.6667f,
                           RtW = 2204, RtH = 1080, Target = 1079.5f, Scale = 1.5f };
        var c1 = new Geo { Name = "C1 300% secondary Free", L = -3113, T = 233, W = 2206, H = 1020, Hw = 735.3334f, Hh = 340f,
                           RtW = 2200, RtH = 879, Target = 879f, Scale = 3f };
        var f125 = new Geo { Name = "125% docked Free", L = 480, T = 160, W = 1230, H = 568, Hw = 1230 / 1.25f, Hh = 568 / 1.25f,
                             RtW = 1228, RtH = 509, Target = (568 / 1.25f - 47f) * 1.25f, Scale = 1.25f };
        foreach (var g in new[] { v1, c1, d250, b1, f125 })
            Check(DisplayXRGameViewPlacement.IsChromeSlackOnly(PaneOf(g)), g.Name + $" (slack {g.W - g.RtW}) is chrome slack");
        // ...for every pane width and height a model sweep tries, 125-300 %.
        {
            int notCs = 0;
            foreach (float scale in new[] { 1.25f, 1.5f, 1.75f, 2f, 2.25f, 2.5f, 3f })
                for (int pw = 900; pw <= 2400; pw += 7)
                    for (int ph = 700; ph <= 1250; ph += 13)
                    {
                        float hw = pw / scale, hh = ph / scale;
                        var g = new Geo { Name = "Free", L = 0, T = 160, W = pw, H = ph, Hw = hw, Hh = hh, Scale = scale,
                                          RtW = (int)Math.Round((hw - 2f) * scale), RtH = (int)Math.Round((hh - 47f) * scale),
                                          Target = (hh - 47f) * scale };
                        if (!DisplayXRGameViewPlacement.IsChromeSlackOnly(PaneOf(g))) notCs++;
                    }
            Check(notCs == 0, $"Free Aspect at 125-300 % is always chrome slack ({notCs} panes are not)");
        }
        // The logged cases. 300 % (427-A, 25 px of slack): #427 put X at 1357 (pixel centring 13),
        // Unity at the pane's 1 pt border + Round(3.17 pt) = 4 pt = 12 px. 150 % (427-B, 49 px, a
        // pane from the same drag as B4): 990 vs Unity's (1 + 15) pt = 24 px.
        var lr25 = Logged("300% 16:9 12/13 px bars", 1344, 1589, 1021, 529.6667f, 340.3333f, 1564, 880);
        var lr49 = new Geo { Name = "150% 16:9 24/25 px bars", L = 965, T = 118, W = 1968, H = 1150, Hw = 1312f, Hh = 766.6667f,
                             RtW = 1919, RtH = 1080, Target = 1079.5f, Scale = 1.5f };
        foreach (var g in new[] { lr25, lr49 })
            Check(!DisplayXRGameViewPlacement.IsChromeSlackOnly(PaneOf(g)), g.Name + " is not chrome slack");
        Pinned(lr25, 1356, 367, 1564, 880);
        Pinned(lr49,  989, 186, 1919, 1080);
        // Every bar from the side borders + 1 up to the old 64 px bound lands on Unity's first column
        // (a whole-point edge on an x.5 px rounds up, as B4 shows) at the RT's size. At 250 % the
        // 16:9 RT is one row taller than the area (860 vs 859: the 117.5 px chrome rounds up) and
        // must not be fitted down for it: Unity fits its 859.5 px target. Where the centring is an
        // exact half point (300 %: slack 9, 15, ...), float noise picks Unity's side; either is taken.
        foreach (var (scale, l, t, ph, hh, rtW, rtH, target) in new[]
        {
            (3f,   1344, 233, 1021, 340.3333f, 1564,  880,  880f),
            (2.5f, 1017, 293,  977, 390.8f,    1528,  860,  859.5f),
            (1.5f,  965, 118, 1150, 766.6667f, 1919, 1080, 1079.5f),
        })
        {
            int bad = 0; string first = null;
            for (int slack = DisplayXRGameViewPlacement.SideChromeSlackMaxPx(scale) + 1; slack <= 64; slack++)
            {
                var g = new Geo { Name = $"{scale * 100}% bars, slack {slack}", L = l, T = t, W = rtW + slack, H = ph,
                                  Hw = (rtW + slack) / scale, Hh = hh, RtW = rtW, RtH = rtH, Target = target, Scale = scale };
                Place(g, out int x, out int y, out int w, out int h);
                UnityOrigin(g, out double ox, out _, out int row0);
                int col = (int)Math.Floor(ox + 0.5 + 1e-6);
                double half = (slack / (double)scale - 2) / 2;               // the centring, exact
                bool tie = Math.Abs(half - Math.Floor(half) - 0.5) < 1e-9;
                int Col(double pt) => l + (int)Math.Floor((1 + pt) * scale + 0.5);
                bool onCol = x == col || (tie && (x == Col(Math.Floor(half)) || x == Col(Math.Ceiling(half))));
                if (DisplayXRGameViewPlacement.IsChromeSlackOnly(PaneOf(g)) || !onCol || y != row0 + DisplayXRGameViewPlacement.DockedRowBiasPx
                    || w != rtW || h != rtH)
                { bad++; first ??= $"{g.Name}: ({x},{y} {w}x{h}), Unity ({col},{row0} {rtW}x{rtH})"; }
            }
            Check(bad == 0, $"{scale * 100}% left/right bars up to 64 px on Unity's first column and row 0 ({bad} off; {first})");
        }

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

        ZoomedView();
        OwnerStateMachine();
        WholePixelDefinitions();
        ModelConsistency();
        PhaseSource();

        Console.WriteLine($"GameViewPlacement: {s_pass} passed, {s_fail} failed");
        Environment.Exit(s_fail == 0 ? 0 : 1);
    }
}
