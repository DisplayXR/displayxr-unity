// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

using UnityEngine;
using Cal = DisplayXR.DisplayXRGameViewCalibration;

namespace DisplayXR
{
    /// <summary>
    /// (#459) On-screen calibration of where the editor Game view's image is drawn.
    ///
    /// The weave's lens phase comes from an invisible anchor window that must sit on the
    /// screen pixel showing texel (0,0) of the Game view's render texture. Computing that
    /// pixel from Unity's layout holds at one display scale and breaks at the next (Unity
    /// snaps fractional points differently by scale and layout: at 175 % the image landed
    /// one pixel left of the computed position, which is the other eye). So it is MEASURED:
    /// once the layout has held for <see cref="DisplayXRProviderDriver.GameViewSettleSeconds"/>
    /// the native mirror blit draws a position-coded marker
    /// (<see cref="DisplayXRGameViewCalibration"/>) instead of the woven image for a few
    /// frames, a few 1-px strips of the screen are read back, and the anchor goes where the
    /// marker really is. If the marker cannot be read, the computed position stays.
    ///
    /// Windows editor, docked (texture) mode, D3D12 native mirror blit only: that is where
    /// the marker is drawn. Everywhere else nothing is shown and nothing changes.
    /// Opt out with <c>DISPLAYXR_PROV_GV_CALIBRATE=0</c>.
    ///
    /// DPI: every coordinate here is PHYSICAL px (the pane rect the driver read under
    /// per-monitor-v2 awareness; the screen is read by native code inside the same pinned
    /// scope). No Win32 geometry call is made from this class.
    /// </summary>
    internal static class DisplayXRGameViewCalibrator
    {
        /// <summary>One frame's layout, as the driver computed it. All physical px.</summary>
        internal struct Layout
        {
            public System.IntPtr Pane;          // the matched pane window
            public System.IntPtr Monitor;       // the monitor it is on (an opaque handle)
            public int PaneLeft, PaneTop;       // its window origin on the desktop
            public int PaneW, PaneH;            // its client size
            public int X, Y, W, H;              // the COMPUTED image rect on the desktop
            public bool Maximized, WholePixel;
            public float Scale;                 // the pane's points-to-px (1.75 at 175 %)
        }

        // What makes a measurement reusable: everything except where the pane is on its
        // monitor (the image's place INSIDE its pane does not move with the editor window).
        // A change of any of these re-calibrates: another pane or monitor, a resized pane or
        // texture, a different computed placement (layout, size selection, zoom, toolbar),
        // maximize, the whole-pixel size, the display scale. Dock / undock and a session
        // restart go through Abort, which drops the measurement.
        struct Key
        {
            public System.IntPtr Pane, Monitor;
            public int PaneW, PaneH, Dx, Dy, W, H;
            public bool Maximized, WholePixel;
            public float Scale;

            public bool Same(in Key o) =>
                Pane == o.Pane && Monitor == o.Monitor && PaneW == o.PaneW && PaneH == o.PaneH && Dx == o.Dx && Dy == o.Dy
                && W == o.W && H == o.H && Maximized == o.Maximized && WholePixel == o.WholePixel
                && Scale == o.Scale;
        }

        const double kShowMaxSeconds   = 0.30;  // give up (and take the marker down) after this
        const double kFirstReadSeconds = 0.05;  // marker drawn -> on screen (one composition)
        const double kReadGapSeconds   = 0.03;
        const double kRetrySeconds     = 2.0;
        const int    kMaxAttempts      = 3;     // per layout
        const int    kMarkerMaxMs      = 500;   // native takes the marker down by itself after this

        static int s_gate = -1;

        // The layout that was calibrated last, and what came of it.
        static bool s_have, s_measured;
        static Key  s_key;
        static int  s_dx, s_dy, s_imgW, s_imgH;   // measured origin relative to the pane; image size
        static int  s_attempts;
        static double s_retryAt;

        // A layout that differs from the calibrated one, waiting to settle.
        static bool s_pendHave;
        static Key  s_pend;
        static double s_pendSince;

        // The marker is up.
        static bool s_showing;
        static Key  s_showKey;
        static double s_showSince, s_firstFrameAt, s_nextReadAt;
        static string s_why;
        static uint[] s_strip;
        static int s_readCount;

        /// <summary>False when <c>DISPLAYXR_PROV_GV_CALIBRATE=0</c>.</summary>
        internal static bool Enabled()
        {
            if (s_gate < 0)
                s_gate = System.Environment.GetEnvironmentVariable("DISPLAYXR_PROV_GV_CALIBRATE") == "0" ? 0 : 1;
            return s_gate == 1;
        }

        /// <summary>True while the anchor returned by the last <see cref="Tick"/> was measured.</summary>
        internal static bool AppliedMeasured { get; private set; }
        /// <summary>The measured image size (valid while <see cref="AppliedMeasured"/>).</summary>
        internal static int MeasuredW => s_imgW;
        internal static int MeasuredH => s_imgH;

        /// <summary>Take the marker down and drop the measurement (the glue stopped, the view
        /// went floating, the session is restarting): the next docked layout is measured
        /// afresh.</summary>
        internal static void Abort()
        {
            AppliedMeasured = false;
            s_pendHave = false; s_have = false; s_measured = false;
            if (!s_showing) return;
            s_showing = false;
            try { DisplayXRProviderNative.dxr_prov_set_gv_calib_marker(0, 0); } catch { }
        }

        /// <summary>
        /// Once per frame, with the computed image rect. Replaces <paramref name="x"/> /
        /// <paramref name="y"/> (the anchor) with the measured origin when there is one for
        /// this layout; starts, polls and ends calibrations.
        /// </summary>
        internal static void Tick(in Layout l, ref int x, ref int y)
        {
            AppliedMeasured = false;
            if (!Enabled()) return;
            var key = new Key
            {
                Pane = l.Pane, Monitor = l.Monitor, PaneW = l.PaneW, PaneH = l.PaneH,
                Dx = l.X - l.PaneLeft, Dy = l.Y - l.PaneTop, W = l.W, H = l.H,
                Maximized = l.Maximized, WholePixel = l.WholePixel, Scale = l.Scale,
            };
            double now = Time.realtimeSinceStartupAsDouble;

            if (s_showing)
            {
                if (key.Same(s_showKey)) Poll(l, now);
                else
                {
                    // The layout moved under the marker: take it down, wait for it to settle.
                    s_showing = false;
                    DisplayXRProviderNative.dxr_prov_set_gv_calib_marker(0, 0);
                }
            }

            if (s_have && key.Same(s_key))
            {
                s_pendHave = false;
                if (!s_measured && !s_showing && s_attempts < kMaxAttempts && now >= s_retryAt)
                    Begin(key, now);
            }
            else if (!s_showing)
            {
                if (!s_pendHave || !key.Same(s_pend)) { s_pend = key; s_pendHave = true; s_pendSince = now; }
                else if (now - s_pendSince >= DisplayXRProviderDriver.GameViewSettleSeconds)
                {
                    s_attempts = 0;
                    Begin(key, now);
                }
            }

            if (s_have && s_measured && key.Same(s_key))
            {
                x = l.PaneLeft + s_dx;
                y = l.PaneTop + s_dy;
                AppliedMeasured = true;
            }
        }

        static void Begin(in Key key, double now)
        {
            s_showing = true; s_showKey = key; s_pendHave = false;
            s_showSince = now; s_firstFrameAt = -1.0; s_nextReadAt = 0.0;
            s_why = null;
            DisplayXRProviderNative.dxr_prov_set_gv_calib_marker(1, kMarkerMaxMs);
        }

        static void Poll(in Layout l, double now)
        {
            int frames = DisplayXRProviderNative.dxr_prov_get_gv_calib_marker(
                out int mw, out int mh, out int band0, out int band1, out int bandW);
            if (frames >= 1 && s_firstFrameAt < 0.0) s_firstFrameAt = now;

            if (frames >= 2 && now - s_firstFrameAt >= kFirstReadSeconds && now >= s_nextReadAt)
            {
                s_nextReadAt = now + kReadGapSeconds;
                if (TryMeasure(l, mw, mh, band0, band1, bandW, out Cal.Axis ax, out Cal.Axis ay, out s_why))
                {
                    Finish(l, true, ax, ay, mw, mh, frames, now);
                    return;
                }
            }
            if (now - s_showSince > kShowMaxSeconds)
            {
                if (frames == 0)
                    s_why = "the marker was not drawn (it needs the D3D12 native mirror blit and a repainting Game view)";
                Finish(l, false, default, default, mw, mh, frames, now);
            }
        }

        static string Describe(string axis, in Cal.Axis a)
        {
            switch (a.Status)
            {
            case Cal.Status.NotFound:
                return "no marker on the " + axis + " strips (" + a.Votes + " blocks read)";
            case Cal.Status.Unreadable:
                return "the " + axis + " strips are partly covered or filtered (" + a.Unexplained +
                       " px show neither the marker nor a shifted copy of it; another window over the Game view?)";
            default:
                return null;
            }
        }

        // Read the strips and decode them. False (with why) when either axis has no measurement.
        static bool TryMeasure(in Layout l, int mw, int mh, int band0, int band1, int bandW,
                               out Cal.Axis ax, out Cal.Axis ay, out string why)
        {
            ax = default; ay = default; why = null;
            if (mw < Cal.MinTexels || mh < Cal.MinTexels || mw > Cal.MaxTexels || mh > Cal.MaxTexels)
            { why = "the Game view's texture is " + mw + "x" + mh + " px, outside what the marker covers"; return false; }
            int margin = Cal.StripMarginPx;
            int lenX = mw + 2 * margin, lenY = mh + 2 * margin;
            int need = Mathf.Max(lenX, lenY);
            if (s_strip == null || s_strip.Length < need) s_strip = new uint[need];

            // Columns: horizontal strips, on any row of the image. A screen read costs
            // ~15 ms, so: stop at the first clean strip, and while a strip shows no marker at
            // all (it is not on screen yet) do not read the other one; successive attempts
            // start on alternate strips, so a strip that is wholly covered does not hide the
            // other for good.
            int x0 = l.X - margin;
            bool read = false;
            int first = s_readCount++ % Cal.StripCount;
            for (int i = 0; i < Cal.StripCount; i++)
            {
                int k = (first + i) % Cal.StripCount;
                int row = l.Y + Cal.HorizontalStripRow(k, mh);
                if (DisplayXRProviderNative.displayxr_read_screen_strip(x0, row, lenX, 0, s_strip) <= 0) continue;
                var a = Cal.DecodeAxis(s_strip, lenX, x0, mw, band0, band1, bandW);
                ax = read ? Cal.Best(ax, a) : a;
                read = true;
                if (ax.Status == Cal.Status.OneToOne || a.Status == Cal.Status.NotFound) break;
            }
            if (!read) { why = "the screen could not be read"; return false; }
            if (!ax.Measured) { why = Describe("horizontal", ax); return false; }

            // Rows: vertical strips down the middle of the row-coded bands, at the column
            // the horizontal strips just measured.
            int y0 = l.Y - margin;
            read = false;
            for (int i = 0; i < Cal.StripCount; i++)
            {
                int k = (first + i) % Cal.StripCount;
                int col = ax.Origin + Cal.VerticalStripColumn(k, band0, band1, bandW);
                if (DisplayXRProviderNative.displayxr_read_screen_strip(col, y0, lenY, 1, s_strip) <= 0) continue;
                var a = Cal.DecodeAxis(s_strip, lenY, y0, mh, -1, -1, 0);
                ay = read ? Cal.Best(ay, a) : a;
                read = true;
                if (ay.Status == Cal.Status.OneToOne) break;
            }
            if (!read) { why = "the screen could not be read"; return false; }
            if (!ay.Measured) { why = Describe("vertical", ay); return false; }
            return true;
        }

        static string AxisText(string name, string line, in Cal.Axis a)
        {
            if (a.Status == Cal.Status.OneToOne) return name + " 1:1";
            string s = name + " NOT 1:1 (";
            if (a.HasOther)
                s += "from " + line + " " + a.FirstBadTexel + " the image is " +
                     (a.OtherOrigin - a.Origin).ToString("+0;-0") + " px off";
            else if (a.FirstBadTexel >= 0)
                s += line + " " + a.FirstBadTexel + " is not shown";
            else
                s += "the end " + line + " is doubled";
            return s + ", " + a.Mismatches + " px differ)";
        }

        static void Finish(in Layout l, bool ok, in Cal.Axis ax, in Cal.Axis ay, int mw, int mh, int frames, double now)
        {
            s_showing = false;
            DisplayXRProviderNative.dxr_prov_set_gv_calib_marker(0, 0);
            s_have = true; s_key = s_showKey; s_measured = ok;
            s_attempts++;
            int cdx = s_showKey.Dx, cdy = s_showKey.Dy;
            if (!ok)
            {
                s_retryAt = now + kRetrySeconds;
                Debug.Log("[DisplayXR] GameView calibration: fell back to the computed origin (pane +" +
                          cdx + ",+" + cdy + ") - " + (s_why ?? "the marker could not be read") +
                          " [attempt " + s_attempts + " of " + kMaxAttempts +
                          (s_attempts < kMaxAttempts ? ", retrying in " + kRetrySeconds.ToString("0") + " s]" : "]"));
                return;
            }

            s_dx = ax.Origin - l.PaneLeft; s_dy = ay.Origin - l.PaneTop;
            s_imgW = mw; s_imgH = mh;
            bool oneX = ax.Status == Cal.Status.OneToOne, oneY = ay.Status == Cal.Status.OneToOne;
            int ddx = s_dx - cdx, ddy = s_dy - cdy;
            Debug.Log("[DisplayXR] GameView calibration: measured image origin pane +" + s_dx + ",+" + s_dy +
                      " (screen " + ax.Origin + "," + ay.Origin + "), computed pane +" + cdx + ",+" + cdy +
                      (ddx == 0 && ddy == 0 ? " (agree)" : " (measured - computed = " + ddx.ToString("+0;-0;0") + "," +
                                                           ddy.ToString("+0;-0;0") + " px)") +
                      " | " + AxisText("W", "column", ax) + ", " + AxisText("H", "row", ay) +
                      " | texture " + mw + "x" + mh + (mw != l.W || mh != l.H ? " (glue computed " + l.W + "x" + l.H + ")" : "") +
                      (ax.SwappedRB || ay.SwappedRB ? " | red and blue arrive exchanged" : "") +
                      " | " + frames + " marker frames, " + ((now - s_showSince) * 1000.0).ToString("0") + " ms" +
                      " | the anchor now uses the measured origin");

            if (oneX && oneY) return;
            // Not texel for pixel: at a fractional scale the whole-pixel size fixes it.
            int step = DisplayXRGameViewPlacement.WholePixelStepPt(l.Scale);
            string pct = (l.Scale * 100f).ToString("0.#", System.Globalization.CultureInfo.InvariantCulture) + " %";
            if (l.WholePixel)
                Debug.Log("[DisplayXR] GameView calibration: the image is not 1:1 although the whole-pixel size is selected (" +
                          pct + "); see docs~/architecture/editor-game-view-placement.md");
            else if (step > 1)
                DisplayXRProviderDriver.RequestWholePixelAuto(
                    "the measured image is not drawn 1:1 " + (oneX ? "vertically" : oneY ? "horizontally" : "on either axis") + " at " + pct);
            else
                Debug.Log("[DisplayXR] GameView calibration: the image is not 1:1 at " + pct + ", where " +
                          (step == 1 ? "a point is already a whole number of pixels" : "the scale is not a whole percent") +
                          ": the whole-pixel size does not apply " +
                          "(rows: see finding F1 in docs~/architecture/editor-game-view-placement.md)");
        }
    }
}
