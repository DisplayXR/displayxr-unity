// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for DisplayXRAutoConvergenceMath (#461): where the screen plane goes, how much
// stereo strength fits the comfort caps, and the temporal follower. Same hand-rolled harness
// as Program.cs; run from its Main.

using System;
using DisplayXR;
using M = DisplayXR.DisplayXRAutoConvergenceMath;

static class AutoConvergenceTests
{
    static int s_pass, s_fail;

    static void Check(bool ok, string what)
    {
        if (ok) s_pass++;
        else { s_fail++; Console.WriteLine("FAIL " + what); }
    }

    static bool Near(float a, float b, float eps = 1e-4f) => Math.Abs(a - b) <= eps;

    const int W = 32, H = 32, N = W * H;
    const float HistMax = 1f / 0.3f; // minimum convergence distance 0.3 m

    // A W x H grid of depths (metres; +infinity = sky, NaN = masked) from a function of (u, vUp).
    static void Grid(Func<float, float, float> depth, out float[] q, out float[] u, out float[] v)
    {
        q = new float[N]; u = new float[N]; v = new float[N];
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
            {
                int i = y * W + x;
                u[i] = (x + 0.5f) / W;
                v[i] = (y + 0.5f) / H;
                float d = depth(u[i], v[i]);
                q[i] = float.IsNaN(d) ? float.NaN : (float.IsPositiveInfinity(d) ? 0f : 1f / d);
            }
    }

    static bool Run(Func<float, float, float> depth, out M.Estimate est, float preferQ = -1f)
    {
        Grid(depth, out var q, out var u, out var v);
        var p = M.Params.Default;
        p.preferQ = preferQ;
        return M.EstimateScreenPlane(q, u, v, N, HistMax, in p, new M.Workspace(N), out est);
    }

    static bool Centre(float u, float v, float half) => Math.Abs(u - 0.5f) < half && Math.Abs(v - 0.5f) < half;

    static void TestFarPlateauAndNearSubject()
    {
        // 50 m scenery filling the frame, a subject at 2 m in the middle (about 14 % of the
        // area). The mean depth and the plain histogram mode both sit on the scenery; the
        // screen plane must not.
        const float far = 50f, near = 2f;
        bool ok = Run((u, v) => Centre(u, v, 0.19f) ? near : far, out var e);
        Check(ok && e.valid, "plateau + subject: valid");
        float dist = 1f / e.screenQ;
        Check(dist < 4f, $"plateau + subject: screen at {dist:F2} m, nowhere near the 50 m plateau");
        Check(Near(e.clumpQ, 1f / near, 0.01f), $"plateau + subject: the clump is the subject (q={e.clumpQ:F3})");
        Check(Near(e.farQ, 1f / far, 1e-4f) && Near(e.nearQ, 1f / near, 1e-4f), "plateau + subject: near/far bounds are the two surfaces");

        // The same with sky instead of scenery: sky is never the clump, and it is the behind bound.
        ok = Run((u, v) => Centre(u, v, 0.19f) ? near : float.PositiveInfinity, out e);
        Check(ok && Near(e.clumpQ, 1f / near, 0.01f), "sky + subject: the clump is the subject");
        Check(e.farQ == 0f, "sky + subject: sky is kept for the behind bound (farQ = 0)");
        Check(e.validSamples < e.samples && e.samples == N, "sky + subject: sky counts as a sample, not as depth");

        // A small subject (6 % of the area): the percentile falls on the scenery, the clump
        // is still the subject, and the blend lands between - still not on the plateau.
        ok = Run((u, v) => Centre(u, v, 0.11f) ? near : far, out e);
        Check(ok && e.screenQ > 1f / far + 0.25f * (1f / near - 1f / far),
              $"small subject: screen pulled off the plateau (q={e.screenQ:F3})");
        Check(e.screenQ <= 1f / near + 1e-4f, "small subject: never nearer than the subject");
    }

    static void TestFlatPlane()
    {
        foreach (float d in new[] { 0.5f, 1.7f, 5f, 20f })
        {
            bool ok = Run((u, v) => d, out var e);
            Check(ok && Near(1f / e.screenQ, d, d * 0.01f), $"flat plane at {d} m: screen on it (got {1f / e.screenQ:F3} m)");
            Check(Near(e.nearQ, 1f / d, 1e-5f) && Near(e.farQ, 1f / d, 1e-5f), $"flat plane at {d} m: no depth range");
            Check(e.confidence > 0.99f, $"flat plane at {d} m: full confidence");
        }
        // Nearer than the histogram's top: still valid, and the true depth is reported.
        bool okNear = Run((u, v) => 0.2f, out var en);
        Check(okNear && Near(en.nearQ, 5f, 1e-4f), "plane nearer than the minimum distance: true nearness kept for the caps");
    }

    static void TestEmptyAndInvalid()
    {
        Check(!Run((u, v) => float.PositiveInfinity, out var e) && !e.valid, "all sky: hold");
        Check(e.samples == N && e.validSamples == 0, "all sky: no valid samples");
        Check(!Run((u, v) => float.NaN, out e) && !e.valid && e.samples == 0, "all masked: hold");
        // Too little real depth (4 texels of 1024, below the 3 % floor): hold.
        Check(!Run((u, v) => (u < 0.07f && v > 0.93f) ? 2f : float.PositiveInfinity, out e) && !e.valid,
              $"a few texels of depth: hold ({e.validSamples} valid)");

        var ws = new M.Workspace(N);
        var p = M.Params.Default;
        Check(!M.EstimateScreenPlane(null, null, null, N, HistMax, in p, ws, out e), "null arrays: hold");
        Check(!M.EstimateScreenPlane(new float[4], new float[4], new float[4], 0, HistMax, in p, ws, out e), "zero samples: hold");
        Check(!M.EstimateScreenPlane(new float[4], new float[4], new float[4], 4, 0f, in p, ws, out e), "no histogram range: hold");
        // More samples than the workspace holds: clamped, not an exception.
        Grid((u, v) => 3f, out var q, out var uu, out var vv);
        Check(M.EstimateScreenPlane(q, uu, vv, N, HistMax, in p, new M.Workspace(100), out e) && e.samples == 100, "workspace smaller than the grid: clamped");
    }

    static void TestCentreWeightAndHysteresis()
    {
        // Two equal-area surfaces in front of a far strip; the bottom is damped, so the top
        // surface is the clump.
        Func<float, float, float> scene = (u, v) => u < 0.1f ? 40f : (v > 0.5f ? 4f : 1.5f);
        Run(scene, out var e);
        Check(Near(e.clumpQ, 1f / 4f, 0.01f), $"bottom damp: the top half wins the clump (q={e.clumpQ:F3})");
        // ...unless the bottom one is the subject already followed.
        Run(scene, out e, preferQ: 1f / 1.5f);
        // With only two surfaces the farther one IS the far plateau, and is never the subject.
        Run((u, v) => v > 0.5f ? 4f : 1.5f, out var two);
        Check(Near(two.clumpQ, 1f / 1.5f, 0.01f), "two surfaces: the far one is the plateau, the near one the subject");
        Check(Near(e.clumpQ, 1f / 1.5f, 0.01f), "hysteresis: the followed clump is kept against a comparable one");
        // A masked first-person weapon (NaN) does not move anything.
        Run((u, v) => 6f, out var plain);
        Run((u, v) => (u > 0.6f && v < 0.35f) ? float.NaN : 6f, out var masked);
        Check(Near(plain.screenQ, masked.screenQ, 1e-4f), "masked samples leave the screen plane alone");
    }

    static void TestDisparityAndCaps()
    {
        // 63 mm baseline, 90 deg horizontal fov: k = 0.063 / 2.
        float k = M.DisparityScale(0.063f, 1f);
        Check(Near(k, 0.0315f, 1e-6f), "disparity scale = baseline / (2 tan(hfov/2))");
        Check(M.DisparityScale(0.063f, 0f) == 0f, "degenerate fov: no disparity scale");
        // Screen at 2 m: content at 2 m has none, at 1 m is in front (negative), at infinity behind.
        Check(Near(M.Disparity(k, 1f, 0.5f, 0.5f), 0f, 1e-7f), "content on the screen plane: zero disparity");
        Check(Near(M.Disparity(k, 1f, 0.5f, 1f), -0.01575f, 1e-6f), "content at 1 m, screen at 2 m: -1.575 % (in front)");
        Check(Near(M.Disparity(k, 1f, 0.5f, 0f), 0.01575f, 1e-6f), "infinity, screen at 2 m: +1.575 % (behind)");
        Check(Near(M.Disparity(k, 0.5f, 0.5f, 0f), 0.007875f, 1e-6f), "disparity is linear in the stereo strength");

        // Caps: 0.6 / 1.2 deg at 0.6 m on a 0.344 m wide view.
        float capF = M.CapFractionFromDegrees(0.6f, 0.6f, 0.344f), capB = M.CapFractionFromDegrees(1.2f, 0.6f, 0.344f);
        Check(Near(capF, 0.018266f, 1e-5f) && Near(capB, 0.036535f, 1e-5f), $"caps in view widths ({capF:F6}, {capB:F6})");
        Check(capB > capF, "more is allowed behind than in front");
        Check(Near(M.CapFractionFromDegrees(0.6f, 0f, 0f), capF, 1e-6f), "no display geometry: the documented fallback");

        // Enforcement: random scenes; the delivered strength never exceeds the authored one
        // and both extremes stay inside their cap.
        var rng = new Random(461);
        bool never = true, inside = true, tight = true;
        for (int i = 0; i < 2000; i++)
        {
            float authored = 0.2f + 2.8f * (float)rng.NextDouble();
            float farQ = 2f * (float)rng.NextDouble();
            float nearQ = farQ + 4f * (float)rng.NextDouble();
            float screenQ = farQ + (nearQ - farQ) * (float)rng.NextDouble();
            float f = M.StrengthForCaps(k, authored, screenQ, nearQ, farQ, capF, capB);
            float dn = M.Disparity(k, f, screenQ, nearQ), df = M.Disparity(k, f, screenQ, farQ);
            if (f > authored + 1e-6f || f < 0f) never = false;
            if (-dn > capF * 1.0001f + 1e-7f || df > capB * 1.0001f + 1e-7f) inside = false;
            // Reduced only when needed: either untouched, or one cap is exactly met.
            bool met = Near(-dn, capF, capF * 1e-3f) || Near(df, capB, capB * 1e-3f);
            if (!(Near(f, authored, 1e-6f) || met)) tight = false;
        }
        Check(never, "strength never above the authored value (never amplifies)");
        Check(inside, "near and far disparities stay inside the front / behind caps");
        Check(tight, "strength is reduced only as far as the tighter cap needs");
        Check(Near(M.StrengthForCaps(k, 1f, 0.5f, 0.6f, 0.45f, capF, capB), 1f, 1e-6f), "a scene inside the caps keeps the authored strength");
        Check(Near(M.StrengthForCaps(k, 1f, 0.5f, 0.5f, 0.5f, capF, capB), 1f, 1e-6f), "a flat scene keeps the authored strength");
        Check(M.StrengthForCaps(k, 1f, 0.5f, 5f, 0f, 0f, 0f) == 0f, "zero caps: no stereo");

        // The comfort range (gizmo) is where the disparity meets the caps.
        M.ComfortRangeQ(k, 1f, 0.5f, capF, capB, out float fq, out float bq);
        Check(Near(-M.Disparity(k, 1f, 0.5f, fq), capF, 1e-6f), "comfort range: front limit sits on the front cap");
        Check(bq < 0f, "comfort range: behind limit beyond infinity when even infinity is inside the cap");
        M.ComfortRangeQ(k, 3f, 2f, capF, capB, out fq, out bq);
        Check(bq > 0f && Near(M.Disparity(k, 3f, 2f, bq), capB, 1e-6f), "comfort range: finite behind limit sits on the behind cap");
    }

    static void TestFollower()
    {
        const float dt = 1f / 60f, tau = 1.2f;

        // First measurement: a fast ramp from the authored value - not a snap, no overshoot.
        var f = new M.Follower();
        f.Reset(0f);
        Check(f.Step(dt, tau) == 0f && !f.have, "no measurement yet: the start value stands");
        f.Measure(1f, 0.01f);
        float first = f.Step(dt, tau);
        Check(first > 0f && first < 0.5f, $"first frame after the first measurement is a ramp, not a snap ({first:F3})");
        float max = first, prev = first;
        bool monotonic = true;
        float at03 = 0f;
        for (int i = 1; i < 600; i++)
        {
            float x = f.Step(dt, tau);
            if (x < prev - 1e-6f) monotonic = false;
            if (x > max) max = x;
            prev = x;
            if (i == 18) at03 = x;
        }
        Check(max <= 1f + 1e-4f, $"after reset: no overshoot (max {max:F5})");
        Check(monotonic, "after reset: monotonic approach");
        Check(at03 > 0.9f, $"after reset: most of the way within 0.3 s ({at03:F3})");
        Check(Near(prev, 1f, 1e-3f), $"after reset: settles on the target ({prev:F5})");

        // In-shot change: slow and critically damped (no overshoot from rest).
        f.Measure(2f, 0.01f); f.Measure(2f, 0.01f); // the median needs two of three
        Check(Near(f.target, 2f, 1e-6f), "median of three: two agreeing measurements move the target");
        float slow03 = 0f; max = 0f;
        for (int i = 0; i < 1200; i++)
        {
            float x = f.Step(dt, tau);
            if (x > max) max = x;
            if (i == 17) slow03 = x;
        }
        Check(slow03 < 1.1f, $"in-shot change is slow ({slow03:F3} after 0.3 s)");
        Check(max <= 2f + 1e-4f && Near(f.x, 2f, 1e-3f), $"in-shot change: settles without overshoot (max {max:F5})");

        // An outlier never moves the target; the dead band ignores small changes.
        f.Measure(9f, 0.01f);
        Check(Near(f.target, 2f, 1e-6f), "a single outlier is rejected by the median");
        f.Measure(2.005f, 0.01f); f.Measure(2.005f, 0.01f); f.Measure(2.005f, 0.01f);
        Check(Near(f.target, 2f, 1e-6f), "changes inside the dead band are ignored");

        // ResetConvergence(): the next measurement is followed fast, again with no overshoot,
        // and the fast phase hands over with zero velocity.
        f.SnapNext();
        f.Measure(0.5f, 0.01f);
        Check(f.fast && Near(f.target, 0.5f, 1e-6f), "snap: the next measurement becomes the target at once");
        float min = f.x;
        for (int i = 0; i < 600; i++)
        {
            float x = f.Step(dt, tau);
            if (x < min) min = x;
            if (i == 0) Check(x > 1.5f, $"snap: still a ramp on the first frame ({x:F3})");
        }
        Check(min >= 0.5f - 1e-4f, $"snap: no overshoot (min {min:F5})");
        Check(!f.fast && Near(f.x, 0.5f, 1e-3f), "snap: settles and leaves the fast phase");

        // The fast phase always ends by its time limit.
        var g = new M.Follower();
        g.Reset(0f);
        g.Measure(1f, 0f);
        for (int i = 0; i < 30; i++) g.Step(dt, tau); // 0.5 s
        Check(!g.fast, "fast phase ends within its time limit");

        // A stalled app (huge dt) is stable.
        var h = new M.Follower();
        h.Reset(0f);
        h.Measure(1f, 0f);
        float hx = h.Step(10f, tau);
        Check(hx >= 0f && hx <= 1f + 1e-4f && !float.IsNaN(hx), "a 10 s frame neither overshoots nor blows up");

        // Asymmetric (stereo strength): falls faster than it rises.
        var s = new M.Follower();
        s.Reset(1f);
        s.Measure(1f, 0f);
        for (int i = 0; i < 60; i++) s.StepAsymmetric(dt, tau, tau * 0.35f);
        s.Measure(0.5f, 0f); s.Measure(0.5f, 0f);
        for (int i = 0; i < 30; i++) s.StepAsymmetric(dt, tau, tau * 0.35f);
        float down = 1f - s.x;
        s.Measure(1f, 0f); s.Measure(1f, 0f);
        float from = s.x;
        float lo = from;
        for (int i = 0; i < 30; i++) { float x = s.StepAsymmetric(dt, tau, tau * 0.35f); if (x < lo) lo = x; }
        float up = s.x - from;
        Check(down > 2f * up, $"strength backs off faster than it returns (down {down:F3}, up {up:F3} in 0.5 s)");
        Check(lo >= from - 1e-5f, "direction change drops the velocity (no continued fall)");
    }

    static void TestDistanceText()
    {
        Check(float.IsPositiveInfinity(M.DistanceFromInverse(0f)), "inverse 0 = infinity");
        Check(Near(M.DistanceFromInverse(0.4f), 2.5f), "inverse 0.4 = 2.5 m");
        Check(M.InverseFromDistance(float.PositiveInfinity) == 0f && M.InverseFromDistance(0f) == 0f && M.InverseFromDistance(-1f) == 0f,
              "infinity / zero / negative distance = parallel");
        Check(Near(M.InverseFromDistance(2.5f), 0.4f), "2.5 m = inverse 0.4");
        Check(Near(M.InverseFromDistance(0.01f), 10f), "distances below the slider's range clamp to its top");
        Check(M.FormatDistance(0f) == "infinity" && M.FormatDistance(0.4f) == "2.5", "format");
        Check(M.TryParseDistance("2.5", out float inv) && Near(inv, 0.4f), "parse \"2.5\"");
        Check(M.TryParseDistance(" 2,5 m ", out inv) && Near(inv, 0.4f), "parse \" 2,5 m \"");
        Check(M.TryParseDistance("infinity", out inv) && inv == 0f, "parse \"infinity\"");
        Check(M.TryParseDistance("Inf", out inv) && inv == 0f, "parse \"Inf\"");
        Check(M.TryParseDistance("∞", out inv) && inv == 0f, "parse the infinity sign");
        Check(M.TryParseDistance("", out inv) && inv == 0f, "parse empty = infinity");
        Check(!M.TryParseDistance("far", out inv), "parse rejects words");
        Check(M.TryParseDistance(M.FormatDistance(0.37f), out inv) && Near(inv, 0.37f, 1e-3f), "format / parse round trip");
    }

    // Linear depth for the full-frame grid: the same projection the cursor unprojects with.
    static void TestLinearEyeDepth()
    {
        float q = (float)(Math.PI / 4);
        // Off-axis frustum: depth along the axis must not depend on where in the view.
        var p = DisplayXRCursorDepthMath.ProjectionFromFov(-0.9f * q, 0.4f * q, 0.7f * q, -0.3f * q, 0.1f, 100f);
        foreach (bool rev in new[] { true, false })
        {
            foreach (float d in new[] { 0.1f, 0.5f, 2f, 17f, 99f })
            {
                var g = DisplayXRCursorDepthMath.ToGpuProjection(p, rev);
                float x = 0.3f * d, y = -0.2f * d, z = -d;
                float cz = g[2] * x + g[6] * y + g[10] * z + g[14], cw = g[3] * x + g[7] * y + g[11] * z + g[15];
                float raw = rev ? cz / cw : (cz / cw + 1f) * 0.5f;
                if (DisplayXRCursorDepthMath.IsBackground(raw, rev)) continue; // the far plane itself
                float got = DisplayXRCursorDepthMath.LinearEyeDepth(p, raw, rev);
                Check(Near(got, d, d * 0.02f + 1e-4f), $"linear eye depth {d} m, reversedZ={rev} (got {got})");
            }
            Check(float.IsPositiveInfinity(DisplayXRCursorDepthMath.LinearEyeDepth(p, rev ? 0f : 1f, rev)), $"background is +infinity, reversedZ={rev}");
        }
        Check(float.IsNaN(DisplayXRCursorDepthMath.LinearEyeDepth(null, 0.5f, true)), "null projection is NaN");
    }

    public static int Run()
    {
        TestFarPlateauAndNearSubject();
        TestFlatPlane();
        TestEmptyAndInvalid();
        TestCentreWeightAndHysteresis();
        TestDisparityAndCaps();
        TestFollower();
        TestDistanceText();
        TestLinearEyeDepth();
        Console.WriteLine($"auto_convergence_math (C#): {s_pass} passed, {s_fail} failed");
        return s_fail;
    }
}
