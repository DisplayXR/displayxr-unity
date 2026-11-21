// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// What DisplayXRGameViewCalibrator needs from Unity, from the driver and from the native plug-in,
// replaced by a simulated world (#459): a clock the test moves, a log it can read, and a "screen"
// on which the Game view's image is drawn at a known origin with whatever marker the calibrator
// asked for. So the calibrator's own state machine (when it shows a marker, which one, how many
// screen reads, when it reuses a measurement) runs in CI exactly as it is shipped.
//
// The world is deliberately simple and says so: a marker that was requested is "on screen" once
// it has been drawn for World.LatencyFrames frames, and a screen read takes World.ReadSeconds.
// What a real compositor does is a hardware check.

using System;
using System.Collections.Generic;
using Cal = DisplayXR.DisplayXRGameViewCalibration;

namespace UnityEngine
{
    public static class Debug
    {
        public static readonly List<string> Lines = new List<string>();
        public static void Log(object o) => Lines.Add(o.ToString());
        public static void LogWarning(object o) => Lines.Add("WARNING " + o);
    }

    public static class Time
    {
        public static double realtimeSinceStartupAsDouble;
    }

    public static class Mathf
    {
        public static int Max(int a, int b) => Math.Max(a, b);
        public static int Clamp(int v, int lo, int hi) => Math.Min(Math.Max(v, lo), hi);
    }

    public struct Vector2Int
    {
        public int x, y;
        public Vector2Int(int x, int y) { this.x = x; this.y = y; }
    }
}

namespace DisplayXR
{
    /// <summary>The simulated screen and native plug-in.</summary>
    internal static class World
    {
        // The image as it really is on screen.
        public static int OriginX, OriginY, TexW, TexH;
        public static int DropColumnAt = -1;         // >= 0: the image is drawn one column narrower (not 1:1)
        public static int CoverTop = 0, CoverBottom = -1;   // screen rows covered by another window...
        public static int CoverLeft = int.MinValue, CoverRight = int.MaxValue;   // ...between these columns
        public static bool Draws = true;             // false: the native blit does not run (D3D11, paused)
        public static int MirrorReason = 0, MirrorOffered = 0, MirrorCalls = 0;   // dxr_prov_get_native_mirror_status
        // dxr_prov_get_gv_marker_path (#485): which draw carries the marker, and the stamp's account.
        public static int MarkerPath = 1, StampStatus = 3, Stamps = 0, Handed = 0;
        public static bool OldBinary;                // no dxr_prov_get_gv_marker_path export
        public static bool OnScreen = true;          // false: the marker is drawn but never reaches the screen
        public static int LatencyFrames = 1;         // marker frames drawn before it is on screen
        public static double ReadSeconds = 1.0 / 60.0;

        // What the calibrator asked for, and what it cost.
        public static bool MarkerOn;
        public static int Strip, Half, FramesDrawn;
        public static int Reads, Requests, MaxFramesDrawn, FullRequests;
        public static readonly List<int> RequestedStrips = new List<int>();

        public static void Reset()
        {
            OriginX = OriginY = 0; TexW = TexH = 0; DropColumnAt = -1; CoverTop = 0; CoverBottom = -1;
            CoverLeft = int.MinValue; CoverRight = int.MaxValue;
            Draws = true; LatencyFrames = 1; MarkerOn = false; Strip = 0; Half = 0; FramesDrawn = 0;
            MarkerPath = 1; StampStatus = 3; Stamps = 0; Handed = 0; OldBinary = false; OnScreen = true;
            Reads = Requests = MaxFramesDrawn = FullRequests = 0;
            RequestedStrips.Clear();
        }

        /// <summary>The render of one frame: the marker, if requested, is drawn once more.</summary>
        public static void RenderFrame()
        {
            if (MarkerOn && Draws) { FramesDrawn++; MaxFramesDrawn = Math.Max(MaxFramesDrawn, FramesDrawn); }
        }

        static uint ScenePixel(int x, int y)
        {
            uint h = (uint)(x * 73856093) ^ (uint)(y * 19349663);
            h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
            return (h & 1) != 0 ? Cal.Rgb((int)(h >> 8) % 6) : (h & 0xFFFFFFu);
        }

        public static uint Screen(int sx, int sy)
        {
            if (sy >= CoverTop && sy <= CoverBottom && sx >= CoverLeft && sx <= CoverRight) return 0xF0F0F0u;
            int rx = sx - OriginX, y = sy - OriginY;
            int drawnW = DropColumnAt >= 0 ? TexW - 1 : TexW;
            if (rx < 0 || y < 0 || rx >= drawnW || y >= TexH) return 0x383838u;
            int x = DropColumnAt >= 0 && rx >= DropColumnAt ? rx + 1 : rx;
            bool visible = MarkerOn && Draws && OnScreen && FramesDrawn >= LatencyFrames;
            int k = visible ? Cal.MarkerIndexAt(x, y, TexW, TexH, Strip, Half) : Cal.Scene;
            return k == Cal.Scene ? ScenePixel(x, y) : Cal.Rgb(k);
        }
    }

    internal static class DisplayXRProviderNative
    {
        public static void dxr_prov_set_gv_calib_marker(int enable, int maxMs) => dxr_prov_set_gv_calib_marker2(enable, maxMs, Cal.StripFull, 0);

        public static void dxr_prov_set_gv_calib_marker2(int enable, int maxMs, int strip, int half)
        {
            if (enable == 0) { World.MarkerOn = false; return; }
            World.MarkerOn = true; World.Strip = strip; World.Half = half; World.FramesDrawn = 0;
            World.Requests++;
            if (strip < 0) World.FullRequests++;
            World.RequestedStrips.Add(strip);
        }

        public static int dxr_prov_get_gv_calib_marker(out int w, out int h, out int band0, out int band1, out int bandW)
        {
            w = World.FramesDrawn > 0 ? World.TexW : 0; h = World.FramesDrawn > 0 ? World.TexH : 0;
            Cal.BandStarts(w, out band0, out band1);
            bandW = Cal.BandWidth;
            return World.MarkerOn ? World.FramesDrawn : 0;
        }

        // The native blit's own account: World.MirrorReason etc., set by the test.
        public static int dxr_prov_get_native_mirror_status(out int offered, out int calls)
        {
            offered = World.MirrorOffered; calls = World.MirrorCalls;
            return World.MirrorReason;
        }

        // Which draw carried the marker: World.MarkerPath etc., set by the test.
        public static int dxr_prov_get_gv_marker_path(out int stampStatus, out int stamps, out int handed)
        {
            if (World.OldBinary) throw new EntryPointNotFoundException("dxr_prov_get_gv_marker_path");
            stampStatus = World.StampStatus; stamps = World.Stamps; handed = World.Handed;
            return World.FramesDrawn > 0 ? World.MarkerPath : Cal.PathNone;
        }

        public static int dxr_prov_get_gv_mirror_size(out int w, out int h)
        {
            w = World.Draws ? World.TexW : 0; h = World.Draws ? World.TexH : 0;
            return World.Draws ? 1 : 0;
        }

        public static int displayxr_read_screen_strip(int x, int y, int len, int vertical, uint[] outRgb)
        {
            World.Reads++;
            UnityEngine.Time.realtimeSinceStartupAsDouble += World.ReadSeconds;
            for (int i = 0; i < len; i++)
                outRgb[i] = World.Screen(vertical != 0 ? x : x + i, vertical != 0 ? y + i : y) | 0xFF000000u;
            return len;
        }
    }

    internal static class DisplayXRProviderDriver
    {
        public const double GameViewSettleSeconds = 0.35;
        public static readonly Dictionary<string, string> Session = new Dictionary<string, string>();
        public static readonly List<string> WholePixelRequests = new List<string>();
        public static void RequestWholePixelAuto(string why) => WholePixelRequests.Add(why);
        public static string SessionGetString(string key) => Session.TryGetValue(key, out string v) ? v : null;
        public static void SessionSetString(string key, string value) => Session[key] = value;
    }
}
