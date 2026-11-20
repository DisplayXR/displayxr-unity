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
    /// (<see cref="DisplayXRGameViewCalibration"/>) over the woven image - two thin strips,
    /// one along the screen row and one along the screen column that are read back - until
    /// those two 1-px strips of the screen have been read, and the anchor goes where the
    /// marker really is. If the marker cannot be read, the computed position stays.
    ///
    /// HOW LONG THE MARKER IS UP. A screen read waits for the desktop compositor: it costs one
    /// display refresh whatever its length, two reads cannot overlap (measured: two threads
    /// take two refreshes), and a rectangle costs more with its area. So the floor is "the
    /// marker reaches the screen" plus one read per axis. Nothing is waited for on top of
    /// that: the first read starts with the first marker frame, a read that comes too early
    /// says "no marker" and is repeated, and the marker comes down with the second axis.
    ///
    /// A layout that was measured before in this editor session is not measured again
    /// (<see cref="Cal.LayoutCache"/>): no marker at all. A layout is only measured once it
    /// has settled AND the mouse button is up (<see cref="Cal.Settled"/>).
    ///
    /// Windows editor, docked (texture) mode, D3D12 native mirror blit only: that is where
    /// the marker is drawn. Everywhere else nothing is shown and nothing changes.
    /// Opt out with <c>DISPLAYXR_PROV_GV_CALIBRATE=0</c>. A/B switches:
    /// <c>DISPLAYXR_PROV_GV_CALIB_FULL=1</c> draws the marker over the whole image, as before;
    /// <c>DISPLAYXR_PROV_GV_CALIB_CACHE=0</c> measures every layout, every time.
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

        // What makes the measurement IN USE stay in use: Cal.LayoutKey.SameLayout, everything
        // except where the pane is on its monitor (the image's place INSIDE its pane does not
        // move with the editor window). A change of any of these re-calibrates: another pane
        // or monitor, a resized pane or texture, a different computed placement (layout, size
        // selection, zoom, toolbar), maximize, the whole-pixel size, the display scale. Dock /
        // undock and a session restart go through Abort, which drops the measurement in use;
        // the layout is then looked up in the cache (SameCached: the position too) before it
        // is measured again.

        const double kShowMaxSeconds   = 0.30;  // give up (and take the marker down) after this
        const double kThinShowMaxSeconds = 0.15; // ...or this long after thin strips were first drawn
        const int    kBackToBackReads  = 3;    // reads that follow each other without a gap...
        const double kReadGapSeconds   = 0.03;  // ...then this between reads (each blocks the editor)
        const int    kMaxAttempts      = Cal.MaxAttempts;   // per layout
        const int    kMarkerMaxMs      = 500;   // native takes the marker down by itself after this
        const double kHitLogGapSeconds = 5.0;   // the same cache hit is not logged again sooner

        static int s_gate = -1, s_fullGate = -1, s_cacheGate = -1, s_readDelayMs = -1;

        /// <summary><c>DISPLAYXR_PROV_GV_CALIB_FULL=1</c>: the marker over the whole image.</summary>
        static bool FullForced()
        {
            if (s_fullGate < 0)
                s_fullGate = System.Environment.GetEnvironmentVariable("DISPLAYXR_PROV_GV_CALIB_FULL") == "1" ? 1 : 0;
            return s_fullGate == 1;
        }

        /// <summary>False when <c>DISPLAYXR_PROV_GV_CALIB_CACHE=0</c>.</summary>
        static bool CacheEnabled()
        {
            if (s_cacheGate < 0)
                s_cacheGate = System.Environment.GetEnvironmentVariable("DISPLAYXR_PROV_GV_CALIB_CACHE") == "0" ? 0 : 1;
            return s_cacheGate == 1;
        }

        /// <summary><c>DISPLAYXR_PROV_GV_CALIB_DELAY_MS</c>: wait this long after the first
        /// marker frame before the first screen read (0..250, default 0: a read that comes
        /// too early costs one refresh that would have been waited anyway).</summary>
        static double ReadDelaySeconds()
        {
            if (s_readDelayMs < 0)
            {
                s_readDelayMs = 0;
                if (int.TryParse(System.Environment.GetEnvironmentVariable("DISPLAYXR_PROV_GV_CALIB_DELAY_MS"), out int ms))
                    s_readDelayMs = Mathf.Clamp(ms, 0, 250);
            }
            return s_readDelayMs / 1000.0;
        }

        // The layouts measured in this editor session (Cal.LayoutCache), kept in the editor's
        // session state so the domain reload of every Play does not empty it.
        const string kSessCache = "DisplayXR.GameViewCalibration.Cache";
        static Cal.LayoutCache s_cache;
        static Cal.LayoutCache Cache()
        {
            if (s_cache == null) s_cache = Cal.LayoutCache.Parse(DisplayXRProviderDriver.SessionGetString(kSessCache));
            return s_cache;
        }

        // Mouse buttons, as keys (not a geometry call: no DPI space involved). Both buttons:
        // with the buttons exchanged the splitter is dragged with the physical right one.
        [System.Runtime.InteropServices.DllImport("user32.dll")] static extern short GetAsyncKeyState(int vKey);
        /// <summary>Replaces the button query in the unit tests (<c>Tests~/GameViewCalibration</c>).</summary>
        internal static System.Func<bool> MouseButtonDownForTests;
        static bool MouseButtonDown()
        {
            if (MouseButtonDownForTests != null) return MouseButtonDownForTests();
            try { return (GetAsyncKeyState(0x01) & 0x8000) != 0 || (GetAsyncKeyState(0x02) & 0x8000) != 0; }
            catch { return false; }
        }

        // The layout that was calibrated last, and what came of it.
        static bool s_have, s_measured;
        static Cal.LayoutKey s_key;
        static int  s_dx, s_dy, s_imgW, s_imgH;   // measured origin relative to the pane; image size
        static int  s_attempts;
        static Cal.Fail s_lastFail;               // why the last attempt at this layout failed
        static double s_retryAt;
        static bool s_hitLogged;
        static Cal.LayoutKey s_hitLogKey;
        static double s_hitLogAt;

        // FALLBACK WITH A REMEMBERED OFFSET. Where the image lands inside its pane is Unity's
        // rounding of a centred rect: a function of the display scale, of the margins around
        // the image (pane minus image, per axis), of where the computed rect sits in the pane,
        // and of the kind of size (whole-pixel / maximized). It does not depend on the image's
        // absolute size or on where the pane is. So "measured minus computed" from an earlier
        // successful calibration with exactly those inputs is the same error the computed
        // position has now, and when the marker cannot be read (a window over the Game view
        // after a resize) it is applied to the computed position instead of leaving the
        // computed position bare. Any difference in those inputs: no offset, computed as is.
        // Kept for the domain (a dock / undock does not invalidate arithmetic).
        static readonly System.Collections.Generic.Dictionary<string, Vector2Int> s_offsets =
            new System.Collections.Generic.Dictionary<string, Vector2Int>();
        static bool s_fallbackOffset;        // the layout in s_key uses a remembered offset
        static int  s_fbDx, s_fbDy;          // anchor relative to the pane, with that offset

        static string OffsetKey(in Cal.LayoutKey k) =>
            k.Scale.ToString("R", System.Globalization.CultureInfo.InvariantCulture) + "|" + (k.WholePixel ? 1 : 0) + "|" +
            (k.Maximized ? 1 : 0) + "|" + (k.PaneW - k.W) + "|" + (k.PaneH - k.H) + "|" + k.Dx + "|" + k.Dy;

        // A layout that differs from the calibrated one, waiting to settle.
        static bool s_pendHave;
        static Cal.LayoutKey s_pend;
        static double s_pendSince;

        // The marker is up.
        static bool s_showing;
        static Cal.LayoutKey s_showKey;
        static int  s_showStrip;                  // Cal.StripFull, or the thin strip pair drawn
        static double s_showSince, s_firstFrameAt, s_nextReadAt;
        static string s_why;
        static bool s_whyCovered;
        static uint[] s_strip;
        static int s_readCount;                   // full marker: which strip of a pair is read first
        static int s_reads;                       // screen reads of this showing

        /// <summary>False when <c>DISPLAYXR_PROV_GV_CALIBRATE=0</c>.</summary>
        internal static bool Enabled()
        {
            if (s_gate < 0)
                s_gate = System.Environment.GetEnvironmentVariable("DISPLAYXR_PROV_GV_CALIBRATE") == "0" ? 0 : 1;
            return s_gate == 1;
        }

        /// <summary>Off until the next domain reload (the native side is not there).</summary>
        internal static void Disable()
        {
            s_gate = 0;
            s_showing = false; s_have = false; s_measured = false; s_pendHave = false; s_fallbackOffset = false;
            AppliedMeasured = false;
        }

        /// <summary>True while the anchor returned by the last <see cref="Tick"/> was measured.</summary>
        internal static bool AppliedMeasured { get; private set; }
        /// <summary>True once an attempt ended without the marker having been drawn at all,
        /// until a marker is drawn: nothing can be measured on screen here (the Built-in
        /// pipeline, D3D11, a view that does not repaint). The driver then selects the
        /// whole-pixel size from the predicted geometry instead of waiting for a
        /// measurement.</summary>
        internal static bool MarkerUnavailable { get; private set; }
        /// <summary>Why (valid while <see cref="MarkerUnavailable"/>).</summary>
        internal static string MarkerUnavailableWhy => s_unavailableWhy ?? "";
        static string s_unavailableWhy;
        /// <summary>The measured image size (valid while <see cref="AppliedMeasured"/>).</summary>
        internal static int MeasuredW => s_imgW;
        internal static int MeasuredH => s_imgH;

        /// <summary>Take the marker down and drop the measurement (the glue stopped, the view
        /// went floating, the session is restarting): the next docked layout is measured
        /// afresh.</summary>
        internal static void Abort()
        {
            AppliedMeasured = false;
            s_pendHave = false; s_have = false; s_measured = false; s_fallbackOffset = false;
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
            var key = new Cal.LayoutKey
            {
                Pane = l.Pane.ToInt64(), Monitor = l.Monitor.ToInt64(),
                PaneLeft = l.PaneLeft, PaneTop = l.PaneTop, PaneW = l.PaneW, PaneH = l.PaneH,
                Dx = l.X - l.PaneLeft, Dy = l.Y - l.PaneTop, W = l.W, H = l.H,
                Maximized = l.Maximized, WholePixel = l.WholePixel, Scale = l.Scale,
            };
            double now = Time.realtimeSinceStartupAsDouble;

            if (s_showing)
            {
                if (key.SameLayout(s_showKey)) Poll(l, now);
                else
                {
                    // The layout moved under the marker: take it down, wait for it to settle.
                    s_showing = false;
                    DisplayXRProviderNative.dxr_prov_set_gv_calib_marker(0, 0);
                }
            }

            if (s_have && key.SameLayout(s_key))
            {
                s_pendHave = false;
                if (!s_measured && !s_showing && s_attempts < kMaxAttempts && now >= s_retryAt)
                    Begin(key, now);
            }
            else if (!s_showing)
            {
                // Another layout. Measured before, at this very place? Then that is the
                // answer, now, with no marker (also in the middle of a drag: nothing is
                // shown). Else it waits until it has settled and the mouse button is up.
                if (TryReuse(l, key, now)) { }
                else if (!s_pendHave || !key.SameLayout(s_pend)) { s_pend = key; s_pendHave = true; s_pendSince = now; }
                else if (now - s_pendSince >= DisplayXRProviderDriver.GameViewSettleSeconds
                         && Cal.Settled(now - s_pendSince, DisplayXRProviderDriver.GameViewSettleSeconds, MouseButtonDown()))
                {
                    s_attempts = 0; s_lastFail = Cal.Fail.NotDrawn;
                    Begin(key, now);
                }
            }

            if (s_have && s_measured && key.SameLayout(s_key))
            {
                x = l.PaneLeft + s_dx;
                y = l.PaneTop + s_dy;
                AppliedMeasured = true;
            }
            else if (s_have && !s_measured && s_fallbackOffset && key.SameLayout(s_key))
            {
                // Not measured: the computed position plus the offset measured earlier for
                // the same scale, margins and kind of size.
                x = l.PaneLeft + s_fbDx;
                y = l.PaneTop + s_fbDy;
            }
        }

        // The layout was measured before in this editor session, where it is now, into a
        // texture of the size being drawn now: use that origin. No marker, no screen read.
        static bool TryReuse(in Layout l, in Cal.LayoutKey key, double now)
        {
            if (!CacheEnabled() || FullForced()) return false;
            if (Cache().Count == 0) return false;
            DisplayXRProviderNative.dxr_prov_get_gv_mirror_size(out int tw, out int th);
            if (!Cache().TryGet(key, tw, th, out Cal.CacheEntry e)) return false;

            s_have = true; s_key = key; s_measured = true; s_pendHave = false;
            s_dx = e.OriginDx; s_dy = e.OriginDy; s_imgW = e.TexW; s_imgH = e.TexH;
            s_fallbackOffset = false; s_attempts = 0; s_lastFail = Cal.Fail.NotDrawn;
            if (s_hitLogged && key.SameCached(s_hitLogKey) && now - s_hitLogAt < kHitLogGapSeconds) return true;
            s_hitLogged = true; s_hitLogKey = key; s_hitLogAt = now;
            Debug.Log(MeasuredLine(l, key, e.TexW, e.TexH, "W 1:1, H 1:1", false, 0, 0.0) +
                      " | marker none | reads 0 | cache hit (" + Cache().Count + " layouts measured this editor session)");
            return true;
        }

        static void Begin(in Cal.LayoutKey key, double now)
        {
            s_showing = true; s_showKey = key; s_pendHave = false;
            s_showSince = now; s_firstFrameAt = -1.0; s_nextReadAt = 0.0;
            s_why = null; s_whyCovered = false; s_reads = 0;
            int strip = Cal.PlanStrip(s_attempts, s_lastFail, FullForced());
            // The whole image after a thin pair that failed: read the OTHER strip row first
            // (the first one may be under a window from end to end, which reads as "no marker").
            if (strip < 0 && s_attempts > 0 && s_showStrip >= 0) s_readCount = s_showStrip + 1;
            s_showStrip = strip;
            DisplayXRProviderNative.dxr_prov_set_gv_calib_marker2(1, kMarkerMaxMs, s_showStrip, Cal.StripHalfPx);
        }

        static void Poll(in Layout l, double now)
        {
            int frames = DisplayXRProviderNative.dxr_prov_get_gv_calib_marker(
                out int mw, out int mh, out int band0, out int band1, out int bandW);
            if (frames >= 1 && s_firstFrameAt < 0.0) s_firstFrameAt = now;
            if (frames >= 1) MarkerUnavailable = false;

            // Read as soon as the marker has been drawn once. A read blocks until the next
            // composition, so one that comes before the marker is on screen costs a refresh
            // that had to pass anyway; it says "no marker" and the next frame reads again.
            if (frames >= 1 && now - s_firstFrameAt >= ReadDelaySeconds() && now >= s_nextReadAt)
            {
                bool ok = TryMeasure(l, mw, mh, band0, band1, bandW, out Cal.Axis ax, out Cal.Axis ay, out s_why, out s_whyCovered);
                if (ok)
                {
                    Finish(l, true, ax, ay, mw, mh, frames, Time.realtimeSinceStartupAsDouble);
                    return;
                }
                now = Time.realtimeSinceStartupAsDouble;   // the reads took time
                s_nextReadAt = s_reads < kBackToBackReads ? 0.0 : now + kReadGapSeconds;
            }
            // A thin marker that is drawn but not found is not waited for as long: the whole
            // image follows (Cal.PlanStrip), and that one does not need the computed row.
            if (now - s_showSince > kShowMaxSeconds
                || (s_showStrip >= 0 && frames > 0 && now - s_firstFrameAt > kThinShowMaxSeconds))
            {
                if (frames == 0)
                {
                    // Say WHICH of the things the marker needs is missing: the native blit
                    // keeps the reason it last ended with.
                    int reason = -1, offered = 0, calls = 0;
                    try { reason = DisplayXRProviderNative.dxr_prov_get_native_mirror_status(out offered, out calls); }
                    catch (System.EntryPointNotFoundException) { }
                    s_unavailableWhy = Cal.NativeMirrorReasonText(reason, offered, calls);
                    MarkerUnavailable = true;
                    s_why = "the marker was not drawn: " + s_unavailableWhy;
                }
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
        // A thin marker has one strip per axis (pair s_showStrip); the whole-image marker can
        // be read on either strip of each pair.
        static bool TryMeasure(in Layout l, int mw, int mh, int band0, int band1, int bandW,
                               out Cal.Axis ax, out Cal.Axis ay, out string why, out bool covered)
        {
            ax = default; ay = default; why = null; covered = false;
            if (mw < Cal.MinTexels || mh < Cal.MinTexels || mw > Cal.MaxTexels || mh > Cal.MaxTexels)
            { why = "the Game view's texture is " + mw + "x" + mh + " px, outside what the marker covers"; return false; }
            int margin = Cal.StripMarginPx;
            int lenX = mw + 2 * margin, lenY = mh + 2 * margin;
            int need = Mathf.Max(lenX, lenY);
            if (s_strip == null || s_strip.Length < need) s_strip = new uint[need];
            bool thin = s_showStrip >= 0;
            int strips = thin ? 1 : Cal.StripCount;

            // Columns: horizontal strips, on any row of the image. A screen read costs one
            // display refresh, so: stop at the first clean strip, and while a strip shows no
            // marker at all (it is not on screen yet) do not read the other one; successive
            // attempts start on alternate strips, so a strip that is wholly covered does not
            // hide the other for good.
            int x0 = l.X - margin;
            bool read = false;
            int first = thin ? s_showStrip : s_readCount++ % Cal.StripCount;
            for (int i = 0; i < strips; i++)
            {
                int k = (first + i) % Cal.StripCount;
                int row = l.Y + Cal.HorizontalStripRow(k, mh);
                s_reads++;
                if (DisplayXRProviderNative.displayxr_read_screen_strip(x0, row, lenX, 0, s_strip) <= 0) continue;
                var a = Cal.DecodeAxis(s_strip, lenX, x0, mw, band0, band1, bandW);
                ax = read ? Cal.Best(ax, a) : a;
                read = true;
                if (ax.Status == Cal.Status.OneToOne || a.Status == Cal.Status.NotFound) break;
            }
            if (!read) { why = "the screen could not be read"; return false; }
            if (!ax.Measured) { why = Describe("horizontal", ax); covered = ax.Status == Cal.Status.Unreadable; return false; }

            // Rows: vertical strips down the middle of the row-coded bands, at the column
            // the horizontal strips just measured.
            int y0 = l.Y - margin;
            read = false;
            for (int i = 0; i < strips; i++)
            {
                int k = (first + i) % Cal.StripCount;
                int col = ax.Origin + Cal.VerticalStripColumn(k, band0, band1, bandW);
                s_reads++;
                if (DisplayXRProviderNative.displayxr_read_screen_strip(col, y0, lenY, 1, s_strip) <= 0) continue;
                var a = Cal.DecodeAxis(s_strip, lenY, y0, mh, -1, -1, 0);
                ay = read ? Cal.Best(ay, a) : a;
                read = true;
                if (ay.Status == Cal.Status.OneToOne) break;
            }
            if (!read) { why = "the screen could not be read"; return false; }
            if (!ay.Measured) { why = Describe("vertical", ay); covered = ay.Status == Cal.Status.Unreadable; return false; }
            return true;
        }

        // What was drawn, for the log: "marker strips h-row 198 v-col 384 13 px" / "marker full image".
        static string MarkerText(int strip, int mw, int mh)
        {
            if (strip < 0) return "marker full image";
            if (mw <= 0 || mh <= 0) return "marker strips (pair " + strip + ") " + (2 * Cal.StripHalfPx + 1) + " px";
            Cal.BandStarts(mw, out int b0, out int b1);
            return "marker strips h-row " + Cal.HorizontalStripRow(strip, mh) + " v-col " +
                   Cal.VerticalStripColumn(strip, b0, b1, Cal.BandWidth) + " " + (2 * Cal.StripHalfPx + 1) + " px";
        }

        // The "measured" line up to and including "the anchor now uses the measured origin"
        // (its format is read by test harnesses: only ever append to it). s_dx / s_dy hold
        // the origin.
        static string MeasuredLine(in Layout l, in Cal.LayoutKey key, int mw, int mh, string axes, bool swapped,
                                   int frames, double ms)
        {
            int cdx = key.Dx, cdy = key.Dy, ddx = s_dx - cdx, ddy = s_dy - cdy;
            return "[DisplayXR] GameView calibration: measured image origin pane +" + s_dx + ",+" + s_dy +
                   " (screen " + (l.PaneLeft + s_dx) + "," + (l.PaneTop + s_dy) + "), computed pane +" + cdx + ",+" + cdy +
                   (ddx == 0 && ddy == 0 ? " (agree)" : " (measured - computed = " + ddx.ToString("+0;-0;0") + "," +
                                                        ddy.ToString("+0;-0;0") + " px)") +
                   " | " + axes +
                   " | texture " + mw + "x" + mh + (mw != l.W || mh != l.H ? " (glue computed " + l.W + "x" + l.H + ")" : "") +
                   (swapped ? " | red and blue arrive exchanged" : "") +
                   " | " + frames + " marker frames, " + ms.ToString("0") + " ms" +
                   " | the anchor now uses the measured origin";
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
            // What the marker cost on screen: what was drawn, the screen reads, and the time
            // from the first marker frame seen to here.
            string facts = " | " + MarkerText(s_showStrip, mw, mh) + " | reads " + s_reads +
                           (s_firstFrameAt >= 0.0 ? ", " + ((now - s_firstFrameAt) * 1000.0).ToString("0") + " ms after the first marker frame" : "");
            if (!ok)
            {
                s_lastFail = frames <= 0 ? Cal.Fail.NotDrawn : s_whyCovered ? Cal.Fail.Covered : Cal.Fail.Missed;
                double retry = Cal.RetryDelaySeconds(s_showStrip, s_lastFail);
                s_retryAt = now + retry;
                s_fallbackOffset = s_offsets.TryGetValue(OffsetKey(s_showKey), out Vector2Int off);
                if (s_fallbackOffset) { s_fbDx = cdx + off.x; s_fbDy = cdy + off.y; }
                Debug.Log("[DisplayXR] GameView calibration: fell back to the computed origin (pane +" +
                          cdx + ",+" + cdy + ")" +
                          (s_fallbackOffset ? " corrected by the offset measured earlier for the same scale, margins and kind of size (" +
                                              off.x.ToString("+0;-0;0") + "," + off.y.ToString("+0;-0;0") + " px): anchor at pane +" + s_fbDx + ",+" + s_fbDy
                                            : "") +
                          " - " + (s_why ?? "the marker could not be read") +
                          " [attempt " + s_attempts + " of " + kMaxAttempts +
                          (s_attempts < kMaxAttempts
                              ? ", retrying in " + retry.ToString("0.#", System.Globalization.CultureInfo.InvariantCulture) + " s" +
                                (Cal.PlanStrip(s_attempts, s_lastFail, FullForced()) < 0 && s_showStrip >= 0 ? " with the whole-image marker" : "") + "]"
                              : "]") +
                          facts + " | " + frames + " marker frames, " + ((now - s_showSince) * 1000.0).ToString("0") + " ms");
                return;
            }

            s_dx = ax.Origin - l.PaneLeft; s_dy = ay.Origin - l.PaneTop;
            s_imgW = mw; s_imgH = mh;
            s_fallbackOffset = false; s_lastFail = Cal.Fail.NotDrawn;
            s_offsets[OffsetKey(s_showKey)] = new Vector2Int(s_dx - cdx, s_dy - cdy);
            bool oneX = ax.Status == Cal.Status.OneToOne, oneY = ay.Status == Cal.Status.OneToOne;

            // Remember a clean measurement for this editor session, at the place the pane is
            // NOW (the showing key is from when the marker went up).
            bool cached = false;
            if (oneX && oneY && CacheEnabled())
            {
                Cal.LayoutKey ck = s_showKey; ck.PaneLeft = l.PaneLeft; ck.PaneTop = l.PaneTop;
                Cache().Put(new Cal.CacheEntry { Key = ck, TexW = mw, TexH = mh, OriginDx = s_dx, OriginDy = s_dy });
                DisplayXRProviderDriver.SessionSetString(kSessCache, Cache().Serialize());
                cached = true;
            }
            Debug.Log(MeasuredLine(l, s_showKey, mw, mh, AxisText("W", "column", ax) + ", " + AxisText("H", "row", ay),
                                   ax.SwappedRB || ay.SwappedRB, frames, (now - s_showSince) * 1000.0) +
                      facts + " | cache miss" + (cached ? " (stored)" : !CacheEnabled() ? " (cache off)" : " (not stored: not 1:1)"));

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
