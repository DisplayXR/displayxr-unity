// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the Window Mode sample's pure C# half (#477):
// Samples~/WindowMode/DisplayXRWindowModeRules.cs. Hand-rolled (no test framework) like the
// other Tests~ projects. Exit code 0 = all passed.
//
// What is NOT covered here, because it needs a Unity player on real monitors: reading the
// window out of Unity's Screen API, the mode switch itself, the move back to a remembered
// display, and the fade.

using System;
using System.Collections.Generic;
using DisplayXR.Samples;

using Rules = DisplayXR.Samples.DisplayXRWindowModeRules;

static class Program
{
    static int s_pass, s_fail;

    static void Check(bool ok, string what)
    {
        if (ok) s_pass++;
        else { s_fail++; Console.WriteLine("FAIL " + what); }
    }

    static DisplayXRSavedWindow Window(int w, int h, int x, int y, string display = "Panel",
                                       int dw = 3840, int dh = 2160, int index = 1, bool fullscreen = false)
    {
        return new DisplayXRSavedWindow
        {
            fullscreen = fullscreen,
            hasSize = true, width = w, height = h,
            hasPosition = true, x = x, y = y,
            displayIndex = index, displayWidth = dw, displayHeight = dh, display = display,
        };
    }

    static bool Same(DisplayXRSavedWindow a, DisplayXRSavedWindow b)
    {
        return a.fullscreen == b.fullscreen && a.hasSize == b.hasSize && a.width == b.width && a.height == b.height
            && a.hasPosition == b.hasPosition && a.x == b.x && a.y == b.y && a.displayIndex == b.displayIndex
            && a.displayWidth == b.displayWidth && a.displayHeight == b.displayHeight
            && (a.display ?? "") == (b.display ?? "");
    }

    // ---- sizes and positions ------------------------------------------------------

    static void TestSize()
    {
        Check(Rules.IsPlausibleSize(1280, 720), "an ordinary size");
        Check(Rules.IsPlausibleSize(Rules.MinWindowPx, Rules.MinWindowPx), "the smallest window the plug-in allows");
        Check(!Rules.IsPlausibleSize(Rules.MinWindowPx - 1, 720), "narrower than the minimum");
        Check(!Rules.IsPlausibleSize(1280, 0), "a 0-high window (minimized, not sized yet)");
        Check(!Rules.IsPlausibleSize(-1280, 720), "a negative size");
        Check(Rules.IsPlausibleSize(7680, 4320), "an 8K window");
        Check(!Rules.IsPlausibleSize(Rules.MaxWindowPx + 1, 720), "wider than any display");
    }

    static void TestPosition()
    {
        const int dw = 3840, dh = 2160;
        // (0, 0) is where a window on the display's corner sits: a position, not "none".
        Check(Rules.IsPositionReachable(0, 0, 1280, dw, dh), "(0,0) is a legitimate position");
        Check(Rules.IsPositionReachable(100, 50, 1280, dw, dh), "inside the display");
        Check(Rules.IsPositionReachable(-8, -8, 1280, dw, dh), "a maximized window's invisible frame");
        Check(Rules.IsPositionReachable(-7, 0, 1280, dw, dh), "snapped to the left edge");
        Check(Rules.IsPositionReachable(-1000, 10, 1280, dw, dh), "mostly off the left, title bar still reachable");
        Check(!Rules.IsPositionReachable(-1250, 10, 1280, dw, dh), "30 px left on the display is not reachable");
        Check(Rules.IsPositionReachable(dw - Rules.MinReachablePx, 10, 1280, dw, dh), "just reachable at the right edge");
        Check(!Rules.IsPositionReachable(dw - 10, 10, 1280, dw, dh), "past the right edge");
        Check(!Rules.IsPositionReachable(100, dh - 10, 1280, dw, dh), "title bar below the display");
        Check(!Rules.IsPositionReachable(100, -200, 1280, dw, dh), "title bar above the display");
        // What a minimized window reports on Windows.
        Check(!Rules.IsPositionReachable(-32000, -32000, 1280, dw, dh), "a minimized window's position is not kept");
        Check(!Rules.IsPositionReachable(0, 0, 1280, 0, 0), "no display size, no verdict");
        Check(!Rules.IsPositionReachable(0, 0, 0, dw, dh), "no window width, no verdict");
        // A window narrower than the reach still counts when all of it is on the display.
        Check(Rules.IsPositionReachable(0, 0, 40, dw, dh), "a very narrow window, fully on the display");
    }

    // ---- which display ------------------------------------------------------------

    static void TestFindDisplay()
    {
        var main = new DisplayXRDisplayEntry("Main", 2560, 1600);
        var panel = new DisplayXRDisplayEntry("Panel", 3840, 2160);
        var saved = Window(1280, 720, 100, 50, "Panel", 3840, 2160, index: 1);

        Check(Rules.FindDisplay(saved, new[] { main, panel }) == 1, "found by name and size at its index");
        Check(Rules.FindDisplay(saved, new[] { panel, main }) == 0, "found after the displays were reordered");
        Check(Rules.FindDisplay(saved, new[] { main }) == -1, "the display is not connected any more");
        Check(Rules.FindDisplay(saved, new[] { main, new DisplayXRDisplayEntry("Panel", 1920, 1080) }) == -1,
              "same name at another resolution is not the display the position was read on");
        Check(Rules.FindDisplay(saved, new[] { main, new DisplayXRDisplayEntry("Other", 3840, 2160) }) == -1,
              "same size under another name is another display");
        Check(Rules.FindDisplay(saved, new DisplayXRDisplayEntry[0]) == -1, "no displays");
        Check(Rules.FindDisplay(saved, null) == -1, "no list");

        // Two identical monitors: the remembered index tells them apart; when it is gone,
        // the first of them.
        var twins = new[] { panel, panel, main };
        Check(Rules.FindDisplay(saved, twins) == 1, "identical displays: the remembered index wins");
        saved.displayIndex = 5;
        Check(Rules.FindDisplay(saved, twins) == 0, "identical displays, stale index: the first match");

        var none = saved; none.hasPosition = false;
        Check(Rules.FindDisplay(none, new[] { main, panel }) == -1, "no remembered position, no display");
        var unnamed = saved; unnamed.display = "";
        Check(Rules.FindDisplay(unnamed, new[] { new DisplayXRDisplayEntry("", 3840, 2160) }) == -1,
              "a display without a name is never matched");
    }

    // ---- the PlayerPrefs string ---------------------------------------------------

    static void TestRoundTrip()
    {
        var cases = new[]
        {
            Window(1280, 720, 100, 50),
            Window(1280, 720, 0, 0),                       // (0,0) survives as a position
            Window(1920, 1080, -8, -8, fullscreen: true),  // negative, and the fullscreen flag
            Window(1280, 720, 300, 40, "DELL U2720Q|left=1", 3840, 2160, 0), // separator in the name
            Window(1280, 720, 300, 40, "\\\\.\\DISPLAY2", 2560, 1440, 2),
        };
        foreach (var w in cases)
        {
            DisplayXRSavedWindow back;
            string text = Rules.Serialize(w);
            Check(Rules.TryParse(text, out back) && Same(w, back), "round trip: " + text);
        }

        // Round trips of a round trip are the same string: what is restored after a
        // relaunch is what was saved, and saving it again changes nothing.
        string once = Rules.Serialize(cases[0]);
        DisplayXRSavedWindow parsed;
        Rules.TryParse(once, out parsed);
        Check(Rules.Serialize(parsed) == once, "serialize(parse(s)) == s");

        // A size alone.
        var sizeOnly = new DisplayXRSavedWindow { hasSize = true, width = 1600, height = 900 };
        DisplayXRSavedWindow sizeBack;
        Check(Rules.TryParse(Rules.Serialize(sizeOnly), out sizeBack) && sizeBack.hasSize && !sizeBack.hasPosition
              && sizeBack.width == 1600 && sizeBack.height == 900, "a size without a position");

        // Nothing at all still parses (as nothing).
        DisplayXRSavedWindow empty;
        Check(Rules.TryParse(Rules.Serialize(default(DisplayXRSavedWindow)), out empty) && !empty.hasSize && !empty.hasPosition
              && !empty.fullscreen, "an empty record");
    }

    static void TestSanitize()
    {
        // Serialize never writes a value that would fail the checks on the way back in.
        DisplayXRSavedWindow back;
        Check(Rules.TryParse(Rules.Serialize(Window(1280, 720, -32000, -32000)), out back) && back.hasSize && !back.hasPosition
              && back.x == 0 && back.y == 0 && string.IsNullOrEmpty(back.display),
              "a minimized window's position is dropped, its size kept");
        Check(Rules.TryParse(Rules.Serialize(Window(10, 10, 100, 50)), out back) && !back.hasSize && !back.hasPosition,
              "an implausible size takes its position with it");
        Check(Rules.TryParse(Rules.Serialize(Window(1280, 720, 100, 50, display: "")), out back) && back.hasSize && !back.hasPosition,
              "a position without a display name is dropped");
        Check(Rules.TryParse(Rules.Serialize(Window(1280, 720, 100, 50, dw: 0, dh: 0)), out back) && !back.hasPosition,
              "a position without its display's size is dropped");

        // The same on the way in: a hand-edited or damaged string cannot put the window
        // somewhere unreachable.
        Check(Rules.TryParse("1|0|1|1280|720|1|-32000|-32000|0|3840|2160|Panel", out back) && back.hasSize && !back.hasPosition,
              "parse drops an unreachable position");
        Check(Rules.TryParse("1|1|1|99999|720|0|0|0|0|0|0|", out back) && back.fullscreen && !back.hasSize,
              "parse drops an implausible size and keeps the mode");
    }

    static void TestParseRejects()
    {
        DisplayXRSavedWindow w;
        foreach (var bad in new[]
        {
            null, "", "garbage", "1", "1|0|1|1280|720",                      // cut short
            "2|0|1|1280|720|1|100|50|1|3840|2160|Panel",                     // another version
            "1|x|1|1280|720|1|100|50|1|3840|2160|Panel",                     // flag is not 0/1
            "1|0|1|12.5|720|1|100|50|1|3840|2160|Panel",                     // not an integer
            "1|0|1|1280|720|1|100|50|1|3840||Panel",                         // empty number
            "1|0|1|1 280|720|1|100|50|1|3840|2160|Panel",                    // digit grouping
        })
            Check(!Rules.TryParse(bad, out w) && !w.hasSize && !w.hasPosition, "rejected: " + (bad ?? "(null)"));
    }

    // ---- settle -------------------------------------------------------------------

    static void TestSettle()
    {
        var s = new Rules.Settle();
        Check(!s.Tick(1920, 1080, 3), "frame 1 of 3");
        Check(!s.Tick(1920, 1080, 3), "frame 2 of 3");
        Check(s.Tick(1920, 1080, 3), "settled on frame 3");
        Check(s.Tick(1920, 1080, 3), "and stays settled");

        Check(!s.Tick(3840, 2160, 3), "a new size starts over");
        Check(!s.Tick(3840, 2160, 3), "frame 2 at the new size");
        Check(!s.Tick(3840, 2159, 3), "one pixel is a change");
        Check(!s.Tick(3840, 2159, 3) && s.Tick(3840, 2159, 3), "settles at the last size");

        Check(!s.Tick(0, 0, 3), "an empty size (minimized, no window yet) never settles");
        Check(!s.Tick(3840, 2159, 3), "and the count restarts after it");

        s.Reset();
        Check(!s.Tick(1280, 720, 2) && s.Tick(1280, 720, 2), "Reset starts over");
        var one = new Rules.Settle();
        Check(one.Tick(1280, 720, 1), "one frame is enough when asked");
        Check(new Rules.Settle().Tick(1280, 720, 0), "a required count below 1 is 1");
    }

    static int Main()
    {
        TestSize();
        TestPosition();
        TestFindDisplay();
        TestRoundTrip();
        TestSanitize();
        TestParseRejects();
        TestSettle();
        Console.WriteLine($"window_mode_rules (C#): {s_pass} passed, {s_fail} failed");
        return s_fail == 0 ? 0 : 1;
    }
}
