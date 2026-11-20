// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// DisplayXRGameViewCalibrator, the shipped file, run against the simulated world of Stubs.cs
// (#459): which marker it asks for and for how many frames, how many screen reads a measurement
// takes, when a measured layout is reused without a marker, and what it logs.

using System;
using System.Reflection;
using System.Text.RegularExpressions;
using DisplayXR;
using UnityEngine;
using Cal = DisplayXR.DisplayXRGameViewCalibration;
using Calibrator = DisplayXR.DisplayXRGameViewCalibrator;

static class CalibratorTests
{
    static Action<bool, string> Check;
    const double Dt = 0.005;   // an editor running at 200 frames a second
    static bool s_button;

    // The line a harness reads, as it was before this change: the new line must still match it.
    static readonly Regex kMeasured = new Regex(
        @"^\[DisplayXR\] GameView calibration: measured image origin pane \+(-?\d+),\+(-?\d+) \(screen (-?\d+),(-?\d+)\), " +
        @"computed pane \+(-?\d+),\+(-?\d+) (\(agree\)|\(measured - computed = [+-]?\d+,[+-]?\d+ px\)) \| W .+, H .+ \| " +
        @"texture (\d+)x(\d+)( \(glue computed \d+x\d+\))? \| (\d+) marker frames, (\d+) ms \| the anchor now uses the measured origin");
    static readonly Regex kFellBack = new Regex(
        @"^\[DisplayXR\] GameView calibration: fell back to the computed origin \(pane \+(-?\d+),\+(-?\d+)\).* - .+ \[attempt (\d) of 3(, retrying in [\d.]+ s[^\]]*)?\]");

    // A domain reload: every static of the calibrator back to its initial value. The editor's
    // session state (DisplayXRProviderDriver.Session) is NOT part of the domain.
    static void DomainReload()
    {
        foreach (FieldInfo f in typeof(Calibrator).GetFields(BindingFlags.Static | BindingFlags.NonPublic | BindingFlags.Public))
        {
            if (f.IsLiteral) continue;
            if (f.Name == "s_offsets") { ((System.Collections.IDictionary)f.GetValue(null)).Clear(); continue; }
            if (f.IsInitOnly) continue;
            object v = f.FieldType.IsValueType ? Activator.CreateInstance(f.FieldType) : null;
            if (f.Name == "s_gate" || f.Name == "s_fullGate" || f.Name == "s_cacheGate" || f.Name == "s_readDelayMs") v = -1;
            f.SetValue(null, v);
        }
        foreach (PropertyInfo p in typeof(Calibrator).GetProperties(BindingFlags.Static | BindingFlags.NonPublic | BindingFlags.Public))
            if (p.CanWrite) p.SetValue(null, false);
        Calibrator.MouseButtonDownForTests = () => s_button;
    }

    static void NewEditorSession()
    {
        Environment.SetEnvironmentVariable("DISPLAYXR_PROV_GV_CALIBRATE", null);
        Environment.SetEnvironmentVariable("DISPLAYXR_PROV_GV_CALIB_FULL", null);
        Environment.SetEnvironmentVariable("DISPLAYXR_PROV_GV_CALIB_CACHE", null);
        Environment.SetEnvironmentVariable("DISPLAYXR_PROV_GV_CALIB_DELAY_MS", null);
        DisplayXRProviderDriver.Session.Clear();
        DisplayXRProviderDriver.WholePixelRequests.Clear();
        Debug.Lines.Clear();
        World.Reset();
        s_button = false;
        Time.realtimeSinceStartupAsDouble = 100.0;
        DomainReload();
    }

    // The 175 % hardware layout: pane at (1500, 1217), image really at pane +1,+110, computed +2,+110.
    static Calibrator.Layout Layout(int paneLeft = 1500, int paneTop = 1217, int w = 1120, int h = 595,
                                    int cdx = 2, int cdy = 110, int paneW = 1124)
    {
        return new Calibrator.Layout
        {
            Pane = new IntPtr(0x2A0F1C), Monitor = new IntPtr(0x10001),
            PaneLeft = paneLeft, PaneTop = paneTop, PaneW = paneW, PaneH = 740,
            X = paneLeft + cdx, Y = paneTop + cdy, W = w, H = h,
            Maximized = false, WholePixel = true, Scale = 1.75f,
        };
    }

    static void Place(in Calibrator.Layout l, int dx = 1, int dy = 110)
    {
        World.OriginX = l.PaneLeft + dx; World.OriginY = l.PaneTop + dy; World.TexW = l.W; World.TexH = l.H;
    }

    // One editor frame: the glue ticks, then the frame is rendered.
    static void Frame(in Calibrator.Layout l, out int x, out int y)
    {
        x = l.X; y = l.Y;
        Calibrator.Tick(l, ref x, ref y);
        World.RenderFrame();
        Time.realtimeSinceStartupAsDouble += Dt;
    }

    // Run frames for `seconds`. Returns the anchor of the last frame.
    static void Run(in Calibrator.Layout l, double seconds, out int x, out int y)
    {
        x = l.X; y = l.Y;
        double end = Time.realtimeSinceStartupAsDouble + seconds;
        while (Time.realtimeSinceStartupAsDouble < end) Frame(l, out x, out y);
    }

    static int Count(string part) { int n = 0; foreach (string s in Debug.Lines) if (s.Contains(part)) n++; return n; }
    static string Last() => Debug.Lines.Count > 0 ? Debug.Lines[Debug.Lines.Count - 1] : "";

    static void FirstMeasurement()
    {
        NewEditorSession();
        var l = Layout(); Place(l);

        Run(l, 0.34, out int x, out int y);
        Check(World.Requests == 0 && x == l.X && y == l.Y && !Calibrator.AppliedMeasured,
              "nothing is shown, and the computed anchor is used, until the layout has held 0.35 s");
        Run(l, 0.30, out x, out y);
        Check(World.Requests == 1 && World.RequestedStrips[0] == 0 && World.Half == Cal.StripHalfPx && World.FullRequests == 0,
              "the first marker is thin strip pair 0, 6 texels each side - never the whole image");
        Check(!World.MarkerOn, "the marker is down again");
        Check(x == l.PaneLeft + 1 && y == l.PaneTop + 110 && Calibrator.AppliedMeasured
              && Calibrator.MeasuredW == 1120 && Calibrator.MeasuredH == 595,
              "the anchor is the measured origin (pane +1,+110 against a computed +2,+110)");
        Check(World.Reads == 2, $"one screen read per axis ({World.Reads} reads)");
        Check(World.MaxFramesDrawn <= 2, $"the marker was drawn for at most the frame before the read and the frame of the read ({World.MaxFramesDrawn})");

        Check(Debug.Lines.Count == 1 && kMeasured.IsMatch(Last()), "one line, in the format the harness reads: " + Last());
        Match m = kMeasured.Match(Last());
        Check(m.Groups[1].Value == "1" && m.Groups[2].Value == "110" && m.Groups[3].Value == "1501" && m.Groups[4].Value == "1327"
              && m.Groups[5].Value == "2" && m.Groups[8].Value == "1120" && m.Groups[9].Value == "595", "...with the measured numbers");
        Cal.BandStarts(1120, out int b0, out _);
        Check(Last().EndsWith(" | marker strips h-row " + Cal.HorizontalStripRow(0, 595) + " v-col " + (b0 + Cal.BandWidth / 2) +
                              " 13 px | reads 2, 33 ms after the first marker frame | cache miss (stored) | marker drawn by the native mirror blit (D3D12)"),
              "...followed by what was drawn, the reads and the cache state");

        // Nothing more happens while the layout stays.
        Run(l, 3.0, out x, out y);
        Check(World.Requests == 1 && Debug.Lines.Count == 1 && x == l.PaneLeft + 1, "a measured layout is left alone");

        // The editor window moves: the measurement moves with the pane, no marker.
        var moved = Layout(paneLeft: 900, paneTop: 300); Place(moved);
        Run(moved, 1.0, out x, out y);
        Check(World.Requests == 1 && x == 901 && y == 410 && Calibrator.AppliedMeasured, "a moved editor window keeps the measurement, as before");
    }

    static void TheMarkerIsNotOnScreenYet()
    {
        NewEditorSession();
        var l = Layout(); Place(l);
        World.LatencyFrames = 3;   // the compositor shows it two frames late
        Run(l, 0.8, out int x, out _);
        Check(World.Requests == 1 && x == l.PaneLeft + 1 && Count("measured image origin") == 1,
              "a read that comes before the marker is on screen is simply repeated");
        Check(World.Reads >= 3 && World.Reads <= 5, $"...at one read per frame until it is there ({World.Reads} reads)");
        Check(World.MaxFramesDrawn <= 5, $"...and the marker comes down with the read that found it ({World.MaxFramesDrawn} frames)");
    }

    static void ReuseWithoutAMarker()
    {
        NewEditorSession();
        var l = Layout(); Place(l);
        Run(l, 0.8, out int x, out int y);
        Check(World.Requests == 1, "measured once");

        // The session restarts (or the glue stops and starts): the measurement in use is dropped...
        Calibrator.Abort();
        Frame(l, out x, out y);
        Check(World.Requests == 1 && x == l.PaneLeft + 1 && y == l.PaneTop + 110 && Calibrator.AppliedMeasured,
              "...and the same layout is the measured one again on the very next frame, with no marker");
        Check(Debug.Lines.Count == 2 && kMeasured.IsMatch(Last()) && Last().Contains(" | 0 marker frames, 0 ms | ")
              && Last().EndsWith(" | marker none | reads 0 | cache hit (1 layouts measured this editor session)"),
              "the reuse is logged in the same format: " + Last());
        Check(World.Reads == 2, "no screen read for a reuse");
        Calibrator.Abort();
        Run(l, 1.0, out x, out y);
        Check(Debug.Lines.Count == 2 && x == l.PaneLeft + 1, "the same reuse within 5 s is not logged twice");

        // Play stops and starts: a domain reload. The cache is in the editor's session state.
        DomainReload();
        Time.realtimeSinceStartupAsDouble += 60.0;
        Frame(l, out x, out y);
        Check(World.Requests == 1 && x == l.PaneLeft + 1 && Calibrator.AppliedMeasured && Count("cache hit") == 2,
              "the next Play starts on the measured origin, with no marker");

        // The texture is not the measured one (yet): not a hit; it becomes one when it is.
        DomainReload();
        World.TexW = 1118;
        Frame(l, out x, out y);
        Check(!Calibrator.AppliedMeasured && x == l.X, "a texture of another size: the cached origin is not used");
        World.TexW = 1120;
        Frame(l, out x, out y);
        Check(Calibrator.AppliedMeasured && World.Requests == 1, "...and is used as soon as the texture is the measured size");

        // The same layout one pixel to the right, after a restart: measured, not reused.
        DomainReload();
        var moved = Layout(paneLeft: 1501); Place(moved);
        Run(moved, 0.8, out x, out y);
        Check(World.Requests == 2 && x == 1502 && Last().Contains("cache miss (stored)") && Last().Contains("2 marker frames") == (World.MaxFramesDrawn == 2),
              "the pane somewhere else is measured again (the cache does not assume the position is irrelevant)");
        // ...and both places are now known.
        DomainReload();
        Frame(l, out x, out y);
        Check(x == l.PaneLeft + 1 && World.Requests == 2 && Last().Contains("cache hit (2 layouts"), "both places are reused afterwards");

        // A resize to a size never seen is measured; back to the first size is not.
        var wide = Layout(w: 1190, paneW: 1194); Place(wide);
        Run(wide, 0.8, out x, out y);
        Check(World.Requests == 3 && Calibrator.AppliedMeasured, "a new size is measured");
        Place(l);
        Frame(l, out x, out y);
        Check(World.Requests == 3 && x == l.PaneLeft + 1 && Calibrator.AppliedMeasured, "back at a measured size: reused at once, no marker");

        // DISPLAYXR_PROV_GV_CALIB_CACHE=0: every layout is measured, every time.
        Environment.SetEnvironmentVariable("DISPLAYXR_PROV_GV_CALIB_CACHE", "0");
        DomainReload();
        Run(l, 0.8, out x, out y);
        Check(World.Requests == 4 && x == l.PaneLeft + 1 && Last().EndsWith("cache miss (cache off) | marker drawn by the native mirror blit (D3D12)"), "DISPLAYXR_PROV_GV_CALIB_CACHE=0 measures a known layout again");
        Environment.SetEnvironmentVariable("DISPLAYXR_PROV_GV_CALIB_CACHE", null);
    }

    static void SplitterDrag()
    {
        NewEditorSession();
        var l = Layout(); Place(l);
        Run(l, 0.8, out _, out _);
        int before = World.Requests;

        // The splitter moves for a second, rests for 1.5 s with the button down, moves again,
        // is released.
        s_button = true;
        Calibrator.Layout d = l;
        for (int i = 0; i < 200; i++) { d = Layout(w: 1120 + i, paneW: 1124 + i); Place(d); Frame(d, out _, out _); }
        Run(d, 1.5, out int x, out int y);
        Check(World.Requests == before && x == d.X, "a drag that pauses with the button down shows no marker");
        for (int i = 200; i < 300; i++) { d = Layout(w: 1120 + i, paneW: 1124 + i); Place(d); Frame(d, out _, out _); }
        Check(World.Requests == before, "nor while it moves");
        Run(d, 0.2, out _, out _);
        s_button = false;
        Run(d, 0.1, out _, out _);
        Check(World.Requests == before, "released before the size has held 0.35 s: still waiting");
        Run(d, 0.6, out x, out y);
        Check(World.Requests == before + 1 && x == d.PaneLeft + 1 && Calibrator.AppliedMeasured, "one marker for the whole drag, after the release");

        // A button held for another reason does not block a measurement for ever.
        s_button = true;
        var other = Layout(w: 1000, paneW: 1004); Place(other);
        Run(other, 0.35 + Cal.MaxButtonHoldSeconds - 0.1, out _, out _);
        Check(World.Requests == before + 1, "a held button delays the measurement...");
        Run(other, 0.6, out x, out _);
        Check(World.Requests == before + 2 && x == other.PaneLeft + 1, "...by at most 3 s");
    }

    static void ThinStripMissed()
    {
        NewEditorSession();
        // The computed top is 10 px off: the read row is not on the 13 px strip.
        var l = Layout(cdy: 120); Place(l);
        Run(l, 0.35 + 0.16 + 0.05, out int x, out int y);
        Check(World.Requests == 1 && !World.MarkerOn && Count("fell back") == 1 && kFellBack.IsMatch(Debug.Lines[0])
              && Debug.Lines[0].Contains("no marker on the horizontal strips")
              && Debug.Lines[0].Contains("[attempt 1 of 3, retrying in 0.2 s with the whole-image marker]"),
              "a thin strip that is drawn but not on the read row gives up after 0.15 s: " + (Debug.Lines.Count > 0 ? Debug.Lines[0] : ""));
        Run(l, 0.6, out x, out y);
        Check(World.Requests == 2 && World.RequestedStrips[1] == Cal.StripFull && x == l.PaneLeft + 1 && y == l.PaneTop + 110
              && Calibrator.AppliedMeasured && Last().Contains(" | marker full image | "),
              "...and the whole-image marker measures it 0.2 s later: nothing the old marker measured is lost");
    }

    static void StripCovered()
    {
        NewEditorSession();
        var l = Layout(); Place(l);
        // Another window over part of the upper third of the image, where strip pair 0's row is.
        int row0 = l.PaneTop + 110 + Cal.HorizontalStripRow(0, 595);
        World.CoverTop = row0 - 40; World.CoverBottom = row0 + 40;
        World.CoverLeft = World.OriginX + 100; World.CoverRight = World.OriginX + 300;
        Run(l, 0.35 + 0.4, out int x, out _);
        Check(World.Requests == 1 && Count("fell back") == 1 && kFellBack.IsMatch(Debug.Lines[0])
              && Debug.Lines[0].Contains("partly covered") && Debug.Lines[0].Contains("[attempt 1 of 3, retrying in 2 s]"),
              "a partly covered strip: fall back, retry in 2 s as before: " + Debug.Lines[0]);
        Check(x == l.X && !World.MarkerOn, "...on the computed origin meanwhile");
        Run(l, 2.5, out x, out _);
        Check(World.Requests == 2 && World.RequestedStrips[1] == 1 && Calibrator.AppliedMeasured && x == l.PaneLeft + 1
              && World.FullRequests == 0, "the second attempt draws the other strip pair, elsewhere on the image, and measures");

        // A window across the whole width of the strip row: the row shows no marker at all,
        // which cannot be told from a strip that is not where it was computed. The whole
        // image follows, read on the other row first.
        NewEditorSession();
        Place(l);
        World.CoverTop = row0 - 40; World.CoverBottom = row0 + 40;
        Run(l, 0.35 + 0.16 + 0.05, out x, out _);
        Check(World.Requests == 1 && Count("fell back") == 1 && Debug.Lines[0].Contains("retrying in 0.2 s with the whole-image marker"),
              "a strip row under a window from end to end reads as 'no marker': the whole image is next");
        World.CoverBottom = -1;   // the window is gone
        Run(l, 0.6, out x, out _);
        Check(World.Requests == 2 && World.RequestedStrips[1] == Cal.StripFull && Calibrator.AppliedMeasured && x == l.PaneLeft + 1,
              "...and measures");

        // Something across the image for good defeats all three attempts; then it stops.
        NewEditorSession();
        Place(l);
        World.CoverTop = l.PaneTop + 110 + 250; World.CoverBottom = World.CoverTop + 60;
        Run(l, 8.0, out x, out _);
        Check(World.Requests == 3 && Count("fell back") == 3 && !Calibrator.AppliedMeasured && x == l.X
              && Last().Contains("[attempt 3 of 3]"), "a window across the image: three attempts, then the computed origin stays: " + Last());
        Run(l, 5.0, out x, out _);
        Check(World.Requests == 3, "no fourth attempt");
        // The window goes away and the layout changes: measured.
        World.CoverBottom = -1;
        var n = Layout(w: 1100, paneW: 1104); Place(n);
        Run(n, 0.8, out x, out _);
        Check(Calibrator.AppliedMeasured && x == n.PaneLeft + 1, "the next layout is measured normally");
    }

    static void NotDrawn()
    {
        NewEditorSession();
        var l = Layout(); Place(l);
        World.Draws = false;   // D3D11, or a Game view that is not repainting
        Run(l, 0.35 + 0.4, out int x, out _);
        Check(World.Reads == 0 && Count("the marker was not drawn") == 1 && Last().Contains("[attempt 1 of 3, retrying in 2 s]") && x == l.X,
              "a marker that is never drawn: no screen read, computed origin, retry in 2 s: " + Last());
        Run(l, 2.5, out x, out _);
        Check(World.Requests == 2 && World.RequestedStrips[1] == 0 && World.FullRequests == 0, "...and the retry is thin strips again, not the whole image");
        World.Draws = true;    // the view repaints again
        Run(l, 2.5, out x, out _);
        Check(World.Requests == 3 && World.RequestedStrips[2] == 0 && Calibrator.AppliedMeasured && x == l.PaneLeft + 1,
              "once it is drawn, the third attempt measures through thin strips");
        Check(!Calibrator.MarkerUnavailable, "...and the marker counts as available again");

        // The line says WHY, from the native blit's own account. The Built-in pipeline: the
        // native blit is offered on every mirror query and never called.
        NewEditorSession();
        l = Layout(); Place(l);
        World.Draws = false; World.MirrorReason = Cal.MirrorNeverCalled; World.MirrorOffered = 900; World.MirrorCalls = 0;
        Run(l, 0.35 + 0.4, out x, out _);
        Check(Count("the marker was not drawn: Unity was offered the native mirror blit and never calls it") == 1
              && Last().Contains("offered 900 times, called 0 times") && Calibrator.MarkerUnavailable
              && Calibrator.MarkerUnavailableWhy.Contains("never calls it"),
              "never called: the line names the render pipeline's own blit, and the marker is unavailable: " + Last());
        // D3D11: called, but there is no D3D12 interface.
        NewEditorSession();
        l = Layout(); Place(l);
        World.Draws = false; World.MirrorReason = Cal.MirrorNotD3D12; World.MirrorOffered = 500; World.MirrorCalls = 500;
        Run(l, 0.35 + 0.4, out x, out _);
        Check(Count("the marker was not drawn: the editor is not on D3D12") == 1, "not D3D12: said so: " + Last());
        World.MirrorReason = 0; World.MirrorOffered = 0; World.MirrorCalls = 0;
    }

    // (#485) The marker through Unity's own blit (the Built-in pipeline), and a marker that is
    // drawn but does not reach the screen.
    static void MarkerPaths()
    {
        // The Built-in pipeline: the native blit is never called, the marker is stamped into
        // the texture Unity blits, and the measurement is the ordinary one.
        NewEditorSession();
        var l = Layout(); Place(l);
        World.MirrorReason = Cal.MirrorNeverCalled; World.MirrorOffered = 900; World.MirrorCalls = 0;
        World.MarkerPath = Cal.PathStamp; World.StampStatus = Cal.StampStamped; World.Stamps = 3; World.Handed = 2;
        Run(l, 0.8, out int x, out int y);
        Check(World.Requests == 1 && Calibrator.AppliedMeasured && x == l.PaneLeft + 1 && y == l.PaneTop + 110 && !Calibrator.MarkerUnavailable,
              "Unity's own blit carries the marker: measured exactly as through the native blit");
        Check(kMeasured.IsMatch(Last()) && Last().EndsWith(" | cache miss (stored) | marker drawn by Unity's own blit (stamped into its source texture 3 times, handed to 2 mirror queries)"),
              "...and the line says which path drew it, appended after everything it had: " + Last());

        // The same on D3D11's native blit (URP + D3D11).
        NewEditorSession();
        Place(l);
        World.MarkerPath = Cal.PathNativeD3D11;
        Run(l, 0.8, out x, out _);
        Check(Calibrator.AppliedMeasured && Last().EndsWith(" | marker drawn by the native mirror blit (D3D11)"), "the D3D11 native blit: measured, and named: " + Last());

        // A native binary without the export: measured, nothing appended.
        NewEditorSession();
        Place(l);
        World.OldBinary = true;
        Run(l, 0.8, out x, out _);
        Check(Calibrator.AppliedMeasured && Last().EndsWith(" | cache miss (stored)"), "an older native binary: no path is reported and nothing is appended: " + Last());

        // Neither path draws: the Built-in pipeline with the stamp switched off. Both accounts.
        NewEditorSession();
        Place(l);
        World.Draws = false; World.MirrorReason = Cal.MirrorNeverCalled; World.MirrorOffered = 900; World.MirrorCalls = 0;
        World.StampStatus = Cal.StampDisabled;
        Run(l, 0.35 + 0.4, out x, out _);
        Check(Count("the marker was not drawn: Unity was offered the native mirror blit and never calls it") == 1
              && Last().Contains("; and the marker for Unity's own blit of that texture: DISPLAYXR_PROV_GV_CALIB_STAMP=0 turns it off")
              && Calibrator.MarkerUnavailable && x == l.X && kFellBack.IsMatch(Last()),
              "never called and the stamp switched off: the line gives both reasons, the marker is unavailable: " + Last());
        Check(!Last().Contains("marker drawn by"), "...and names no path, since nothing was drawn");

        // The marker is stamped and handed to Unity's blit, and nothing of it shows on screen
        // (Unity does not take the texture, or does something else with it). That is said,
        // and from the first such attempt the marker counts as unavailable, so the driver's
        // prediction is not kept waiting for a measurement.
        NewEditorSession();
        Place(l);
        World.OnScreen = false;
        World.MirrorReason = Cal.MirrorNeverCalled; World.MirrorOffered = 900; World.MirrorCalls = 0;
        World.MarkerPath = Cal.PathStamp; World.StampStatus = Cal.StampStamped; World.Stamps = 30; World.Handed = 30;
        Run(l, 0.35 + 0.16 + 0.05, out x, out _);
        Check(World.Requests == 1 && Count("fell back") == 1 && Last().Contains("no marker on the horizontal strips")
              && Last().EndsWith(" | marker drawn by Unity's own blit (stamped into its source texture 30 times, handed to 30 mirror queries)"),
              "drawn and not on screen: the truthful 'no marker' line, with the path that drew it: " + Last());
        Check(Calibrator.MarkerUnavailable && Calibrator.MarkerUnavailableWhy.Contains("none of it showed on the screen")
              && Calibrator.MarkerUnavailableWhy.Contains("Unity's own blit") && x == l.X,
              "...the marker is unavailable from that attempt on, with the reason: " + Calibrator.MarkerUnavailableWhy);
        Run(l, 0.5, out x, out _);
        Check(World.Requests == 2 && World.RequestedStrips[1] == Cal.StripFull && Calibrator.MarkerUnavailable,
              "the whole image is tried next and, not showing either, leaves it unavailable (a drawn frame alone does not make it available)");
        Run(l, 6.0, out x, out _);
        Check(World.Requests == 3 && Last().Contains("[attempt 3 of 3]") && Calibrator.MarkerUnavailable && !Calibrator.AppliedMeasured && x == l.X,
              "three attempts, then the computed origin stays and the marker stays unavailable");
        // It reaches the screen in another layout: measured, available again.
        World.OnScreen = true;
        var n = Layout(w: 1100, paneW: 1104); Place(n);
        Run(n, 0.8, out x, out _);
        Check(Calibrator.AppliedMeasured && !Calibrator.MarkerUnavailable, "once a layout measures, the marker is available again");

        // A partly covered marker is on screen: not "unavailable".
        NewEditorSession();
        Place(l);
        World.OnScreen = false;
        Run(l, 0.35 + 0.16 + 0.05, out x, out _);
        Check(Calibrator.MarkerUnavailable, "(a missed marker first)");
        World.OnScreen = true;
        int row = l.PaneTop + 110;
        World.CoverTop = row; World.CoverBottom = row + 594;
        World.CoverLeft = World.OriginX + 100; World.CoverRight = World.OriginX + 300;
        Run(l, 0.8, out x, out _);
        Check(Count("partly covered") == 1 && !Calibrator.MarkerUnavailable,
              "a marker that is found partly covered is on screen: measurable, so not unavailable: " + Last());
        World.MirrorReason = 0; World.MirrorOffered = 0; World.MirrorCalls = 0;
    }

    static void NotOneToOne()
    {
        NewEditorSession();
        var l = Layout(); l.WholePixel = false; Place(l);
        World.DropColumnAt = 500;
        Run(l, 0.8, out int x, out _);
        Check(World.Requests == 1 && Calibrator.AppliedMeasured && Last().Contains("W NOT 1:1") && Last().Contains("cache miss (not stored: not 1:1)"),
              "an image that is not 1:1 is measured through thin strips and not remembered: " + Last());
        Check(DisplayXRProviderDriver.WholePixelRequests.Count == 1, "...and the whole-pixel size is requested, as before");
        Calibrator.Abort();
        Run(l, 0.8, out x, out _);
        Check(World.Requests == 2, "a not-1:1 layout is measured again the next time");
    }

    static void FullMarkerSwitch()
    {
        NewEditorSession();
        Environment.SetEnvironmentVariable("DISPLAYXR_PROV_GV_CALIB_FULL", "1");
        DomainReload();
        var l = Layout(); Place(l);
        Run(l, 0.8, out int x, out _);
        Check(World.Requests == 1 && World.RequestedStrips[0] == Cal.StripFull && x == l.PaneLeft + 1
              && Last().Contains(" | marker full image | reads 2"), "DISPLAYXR_PROV_GV_CALIB_FULL=1: the whole-image marker");
        Calibrator.Abort();
        Run(l, 0.8, out x, out _);
        Check(World.Requests == 2 && World.FullRequests == 2, "...and every layout is measured (no reuse), so A/B runs show the marker each time");
        Environment.SetEnvironmentVariable("DISPLAYXR_PROV_GV_CALIB_FULL", null);

        NewEditorSession();
        Environment.SetEnvironmentVariable("DISPLAYXR_PROV_GV_CALIBRATE", "0");
        DomainReload();
        Run(l, 1.0, out x, out _);
        Check(World.Requests == 0 && x == l.X && Debug.Lines.Count == 0, "DISPLAYXR_PROV_GV_CALIBRATE=0: no marker, computed origin");
        Environment.SetEnvironmentVariable("DISPLAYXR_PROV_GV_CALIBRATE", null);
    }

    public static void RunAll(Action<bool, string> check)
    {
        Check = check;
        FirstMeasurement();
        TheMarkerIsNotOnScreenYet();
        ReuseWithoutAMarker();
        SplitterDrag();
        ThinStripMissed();
        StripCovered();
        NotDrawn();
        MarkerPaths();
        NotOneToOne();
        FullMarkerSwitch();
    }
}
