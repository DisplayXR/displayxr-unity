// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Window Mode sample, the decisions (#477): what a remembered window is, how it is written
// to and read from one PlayerPrefs string, which remembered values are safe to apply, and
// when the window has stopped changing after a mode switch.
//
// PURE C# - no UnityEngine. DisplayXRWindowModeSample.cs gathers the numbers from Unity's
// Screen API and calls in here; Tests~/WindowModeRules calls in with the numbers written
// out, so the parts that cannot be tried on a single-monitor machine are still tested.
//
// ONE COORDINATE SPACE. Every number in a DisplayXRSavedWindow was read from Unity's Screen
// API (Screen.width / height, Screen.mainWindowPosition, DisplayInfo) and is only ever
// handed back to it (Screen.SetResolution, Screen.MoveMainWindowTo). None of them is
// compared with, added to or converted into a number from the plug-in's native side, which
// answers in true physical pixels: on a mixed-DPI multi-monitor machine the two disagree
// by the ratio of the scale factors even when both are right.

using System;
using System.Collections.Generic;
using System.Globalization;

namespace DisplayXR.Samples
{
    /// <summary>
    /// The windowed size and position an app remembers, and whether it was full screen.
    /// The position is relative to the top-left corner of <see cref="display"/>, as
    /// <c>Screen.mainWindowPosition</c> reports it, so (0, 0) is an ordinary position:
    /// "no position" is <see cref="hasPosition"/> = false, never a value.
    /// </summary>
    public struct DisplayXRSavedWindow
    {
        public bool fullscreen;

        public bool hasSize;
        public int width, height;

        public bool hasPosition;
        public int x, y;
        /// <summary>The display the position is relative to: its index in the display
        /// layout, its name and its size when the position was read.</summary>
        public int displayIndex;
        public int displayWidth, displayHeight;
        public string display;
    }

    /// <summary>A display as the sample sees it (a <c>UnityEngine.DisplayInfo</c> without Unity).</summary>
    public struct DisplayXRDisplayEntry
    {
        public string name;
        public int width, height;

        public DisplayXRDisplayEntry(string name, int width, int height)
        {
            this.name = name; this.width = width; this.height = height;
        }
    }

    public static class DisplayXRWindowModeRules
    {
        /// <summary>The smallest window the plug-in's own resize calls allow (DXR_MIN_WINDOW_PX).</summary>
        public const int MinWindowPx = 200;
        public const int MaxWindowPx = 16384;
        /// <summary>How much of the window's top strip must stay on its display for the
        /// position to be worth restoring: enough to grab the title bar.</summary>
        public const int MinReachablePx = 64;
        /// <summary>How far above the display's top edge the window's top may be: a
        /// maximized or snapped window sits a few pixels outside (its invisible frame).</summary>
        public const int TopSlackPx = 32;

        const string Version = "1";
        const char Sep = '|';

        // ---- what is safe to keep ------------------------------------------------

        public static bool IsPlausibleSize(int width, int height)
        {
            return width >= MinWindowPx && height >= MinWindowPx
                && width <= MaxWindowPx && height <= MaxWindowPx;
        }

        /// <summary>
        /// Whether a window of <paramref name="width"/> at (<paramref name="x"/>,
        /// <paramref name="y"/>) on a display of the given size can be reached by the
        /// user: part of its top strip is on the display. A minimized window reports a
        /// position far outside every display (about -32000 on Windows); a window left on
        /// a display that has since shrunk can be past its edge. Neither is saved or
        /// restored. (0, 0) and small negative values are ordinary positions.
        /// </summary>
        public static bool IsPositionReachable(int x, int y, int width, int displayWidth, int displayHeight)
        {
            if (width <= 0 || displayWidth <= 0 || displayHeight <= 0) return false;
            int reach = Math.Min(MinReachablePx, width);
            bool horizontally = x + width >= reach && x <= displayWidth - reach;
            bool vertically = y >= -TopSlackPx && y <= displayHeight - reach;
            return horizontally && vertically;
        }

        /// <summary>
        /// The display a remembered position belongs to, as an index into
        /// <paramref name="displays"/>, or -1 when it is not connected any more or is not
        /// the same display (another size): the position is then not restored and the
        /// window stays where the OS put it. Two displays with the same name and size are
        /// told apart by the remembered index.
        /// </summary>
        public static int FindDisplay(DisplayXRSavedWindow saved, IList<DisplayXRDisplayEntry> displays)
        {
            if (!saved.hasPosition || displays == null || string.IsNullOrEmpty(saved.display)) return -1;
            int first = -1;
            for (int i = 0; i < displays.Count; i++)
            {
                var d = displays[i];
                if (d.name != saved.display || d.width != saved.displayWidth || d.height != saved.displayHeight)
                    continue;
                if (i == saved.displayIndex) return i;
                if (first < 0) first = i;
            }
            return first;
        }

        /// <summary>
        /// Everything about <paramref name="saved"/> that fails a check is dropped, so
        /// what is left can be applied without looking at it again.
        /// </summary>
        public static DisplayXRSavedWindow Sanitize(DisplayXRSavedWindow saved)
        {
            if (!saved.hasSize || !IsPlausibleSize(saved.width, saved.height))
            {
                saved.hasSize = false;
                saved.width = saved.height = 0;
            }
            // A position without its size cannot be checked, and without its display means nothing.
            if (!saved.hasPosition || !saved.hasSize || string.IsNullOrEmpty(saved.display) ||
                !IsPositionReachable(saved.x, saved.y, saved.width, saved.displayWidth, saved.displayHeight))
            {
                saved.hasPosition = false;
                saved.x = saved.y = 0;
                saved.displayIndex = 0;
                saved.displayWidth = saved.displayHeight = 0;
                saved.display = null;
            }
            return saved;
        }

        // ---- one PlayerPrefs string ------------------------------------------------
        // "1|fullscreen|hasSize|w|h|hasPosition|x|y|displayIndex|displayW|displayH|display name".
        // One string, so a crash between two PlayerPrefs writes cannot leave a size from
        // one run next to a position from another. The display name is last and takes the
        // rest of the string: it may contain the separator.

        public static string Serialize(DisplayXRSavedWindow saved)
        {
            saved = Sanitize(saved);
            var c = CultureInfo.InvariantCulture;
            return string.Join(Sep.ToString(), new[]
            {
                Version,
                saved.fullscreen ? "1" : "0",
                saved.hasSize ? "1" : "0",
                saved.width.ToString(c), saved.height.ToString(c),
                saved.hasPosition ? "1" : "0",
                saved.x.ToString(c), saved.y.ToString(c),
                saved.displayIndex.ToString(c),
                saved.displayWidth.ToString(c), saved.displayHeight.ToString(c),
                saved.display ?? "",
            });
        }

        /// <summary>
        /// False for anything that is not a string <see cref="Serialize"/> wrote (empty,
        /// another version, cut short, not numbers). The result is sanitized.
        /// </summary>
        public static bool TryParse(string text, out DisplayXRSavedWindow saved)
        {
            saved = default(DisplayXRSavedWindow);
            if (string.IsNullOrEmpty(text)) return false;
            string[] p = text.Split(new[] { Sep }, 12);
            if (p.Length != 12 || p[0] != Version) return false;

            bool fullscreen, hasSize, hasPosition;
            int w, h, x, y, index, dw, dh;
            if (!Flag(p[1], out fullscreen) || !Flag(p[2], out hasSize) || !Int(p[3], out w) || !Int(p[4], out h) ||
                !Flag(p[5], out hasPosition) || !Int(p[6], out x) || !Int(p[7], out y) ||
                !Int(p[8], out index) || !Int(p[9], out dw) || !Int(p[10], out dh))
                return false;

            saved.fullscreen = fullscreen;
            saved.hasSize = hasSize; saved.width = w; saved.height = h;
            saved.hasPosition = hasPosition; saved.x = x; saved.y = y;
            saved.displayIndex = index; saved.displayWidth = dw; saved.displayHeight = dh;
            saved.display = p[11];
            saved = Sanitize(saved);
            return true;
        }

        static bool Flag(string s, out bool value)
        {
            value = s == "1";
            return s == "0" || s == "1";
        }

        static bool Int(string s, out int value)
        {
            return int.TryParse(s, NumberStyles.AllowLeadingSign, CultureInfo.InvariantCulture, out value);
        }

        // ---- when a switch is over -------------------------------------------------

        /// <summary>
        /// Counts frames in which the window kept one size. After a mode switch the window
        /// changes size over a few frames and the provider reallocates its swapchain once
        /// the size has held for several more; a fade stays black until this says the
        /// size has held for long enough.
        /// </summary>
        public sealed class Settle
        {
            int m_W, m_H, m_Frames;

            public void Reset() { m_W = m_H = m_Frames = 0; }

            /// <summary>
            /// Feed one frame's size. True once the same non-empty size has been fed for
            /// <paramref name="requiredFrames"/> frames in a row.
            /// </summary>
            public bool Tick(int width, int height, int requiredFrames)
            {
                if (width <= 0 || height <= 0) { Reset(); return false; }
                if (width == m_W && height == m_H) m_Frames++;
                else { m_W = width; m_H = height; m_Frames = 1; }
                return m_Frames >= Math.Max(1, requiredFrames);
            }
        }
    }
}
