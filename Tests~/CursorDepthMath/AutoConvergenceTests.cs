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

    // ---- Review findings (#461): one regression test each --------------------------------

    // Finding 1: the caps must hold for what is DELIVERED while the plane is still moving.
    static void TestDeliveredDisparityWhilePlaneMoves()
    {
        const float dt = 1f / 60f, tau = 1.2f;
        float k = M.DisparityScale(0.063f, (float)Math.Tan(30 * Math.PI / 180) * 16f / 9f); // vfov 60, 16:9
        float capF = M.CapFractionFromDegrees(0.6f, 0.6f, 0.344f), capB = M.CapFractionFromDegrees(1.2f, 0.6f, 0.344f);

        foreach (bool towardNear in new[] { true, false })
        {
            // Settled on one flat surface, then the camera turns to another: 50 m -> 0.5 m
            // (front cap at risk) and 0.5 m -> 50 m (behind cap at risk).
            float q0 = towardNear ? 1f / 50f : 2f, q1 = towardNear ? 2f : 1f / 50f;
            var conv = new M.Follower(); conv.Reset(q0); conv.Measure(q0, 0f);
            var str = new M.Follower(); str.Reset(1f); str.Measure(1f, 0f);
            for (int i = 0; i < 120; i++) { conv.Step(dt, tau); str.StepAsymmetric(dt, tau, tau * 0.35f); }

            float worstBounded = 0f, worstUnbounded = 0f, maxStrength = 0f, t = 0f, next = 0f;
            for (int i = 0; i < 600; i++, t += dt)
            {
                if (t >= next)
                {
                    next = t + 0.15f;
                    conv.Measure(q1, 0.001f);
                    str.Measure(M.StrengthForCaps(k, 1f, conv.target, q1, q1, capF, capB), 0.02f);
                }
                float inv = conv.Step(dt, tau);
                float followed = str.StepAsymmetric(dt, tau, tau * 0.35f);
                float delivered = M.BoundStrength(followed, k, 1f, inv, q1, q1, capF, capB);
                float cap = towardNear ? capF : capB;
                float dB = Math.Abs(M.Disparity(k, delivered, inv, q1)) / cap;
                float dU = Math.Abs(M.Disparity(k, Math.Min(followed, 1f), inv, q1)) / cap;
                if (dB > worstBounded) worstBounded = dB;
                if (dU > worstUnbounded) worstUnbounded = dU;
                if (delivered > maxStrength) maxStrength = delivered;
            }
            string dir = towardNear ? "50 m -> 0.5 m" : "0.5 m -> 50 m";
            Check(worstUnbounded > 1.5f, $"{dir}: without the per-frame bound the cap is exceeded ({worstUnbounded:F2}x) - the test bites");
            Check(worstBounded <= 1.0005f, $"{dir}: delivered disparity never exceeds the cap ({worstBounded:F4}x)");
            Check(maxStrength <= 1f + 1e-6f, $"{dir}: delivered strength never above the authored one");
            float end = M.BoundStrength(str.x, k, 1f, conv.x, q1, q1, capF, capB);
            Check(Near(end, 1f, 1e-3f), $"{dir}: full strength again once the plane has arrived ({end:F4})");
        }
        Check(M.BoundStrength(float.NaN, k, 1f, 0.5f, 0.5f, 0.5f, capF, capB) == 1f, "bound: a NaN followed value falls back to the bound");
        Check(M.StrengthForCaps(k, 1f, 0.5f, 5f, 0f, float.NaN, float.NaN) == 0f, "a NaN cap is no stereo, not NaN");
    }

    // Finding 4: content nearer than the minimum distance must not blind the clump search.
    static void TestNearIntruderDoesNotBlindTheClump()
    {
        // A room at 6 m, a centred character at 1.5 m (20 %), and something 15 cm from the
        // lens in a corner (9 %) - nearer than the 0.3 m minimum convergence distance.
        Func<float, float, float> scene = (u, v) =>
            (u > 0.7f && v < 0.3f) ? 0.15f
            : ((Math.Abs(u - 0.5f) < 0.2f && Math.Abs(v - 0.5f) < 0.25f) ? 1.5f : 6f);
        bool ok = Run(scene, out var e);
        Check(ok && e.valid, "near intruder: still a valid estimate (was: hold forever)");
        Check(Near(e.clumpQ, 1f / 1.5f, 0.02f), $"near intruder: the clump is the character (q={e.clumpQ:F3})");
        Check(1f / e.screenQ > 0.9f && 1f / e.screenQ < 3f, $"near intruder: screen near the character ({1f / e.screenQ:F2} m)");
        Check(Near(e.nearQ, 1f / 0.15f, 1e-3f), "near intruder: its true nearness still reaches the caps");
    }

    // Finding 5g: the content extremes are reported even when the screen plane is not confident.
    static void TestExtremesReportedWhenHolding()
    {
        // 25 texels of something at 0.4 m against sky: below the 3 % floor -> hold...
        bool ok = Run((u, v) => (u < 0.16f && v > 0.84f) ? 0.4f : float.PositiveInfinity, out var e);
        Check(!ok && !e.valid, "small intruder: the screen plane holds");
        // ...but it is more than 2 % of the frame, so the front cap must see it.
        Check(e.samples == N && Near(e.nearQ, 2.5f, 1e-3f), $"small intruder: nearQ still reported for the caps ({e.nearQ:F2})");
    }

    // Findings 2 and 3: which depth grids a consumer may use.
    static void TestGridAcceptance()
    {
        Check(M.AcceptGrid(100, -1, 0, true), "a first grid from this camera is taken");
        Check(!M.AcceptGrid(100, 100, 0, true), "the same grid published twice is taken once (median of three stays three samples)");
        Check(!M.AcceptGrid(99, 100, 0, true), "an older grid arriving late is ignored");
        Check(!M.AcceptGrid(100, 90, 101, true), "a grid drawn before ResetConvergence() is ignored");
        Check(M.AcceptGrid(101, 90, 101, true), "a grid drawn in the reset frame (rendered after it) is taken");
        Check(!M.AcceptGrid(100, 90, 0, false), "another rig's grid is ignored");

        // The sequence the reset exists for: a pre-cut grid in flight must not take the snap.
        var f = new M.Follower();
        f.Reset(0.2f); f.Measure(0.2f, 0f);
        for (int i = 0; i < 120; i++) f.Step(1f / 60f, 1.2f);
        int notBefore = 500, consumed = 480;
        f.SnapNext();
        if (M.AcceptGrid(498, consumed, notBefore, true)) f.Measure(0.2f, 0f);   // pre-cut grid arrives late
        Check(!f.fast, "pre-cut grid after a reset does not use up the fast ramp");
        if (M.AcceptGrid(501, consumed, notBefore, true)) f.Measure(1.5f, 0f);   // first post-cut grid
        Check(f.fast && Near(f.target, 1.5f, 1e-6f), "the first post-cut grid gets the fast ramp");
    }

    // Finding 6: rig scale. The runtime keeps the eye baseline a physical distance in WORLD
    // units (camera rig sent with metersToVirtual = 1) while the convergence is authored in
    // rig units (the driver sends inv / scale). Everything must agree with the world answer.
    static void TestRigScale()
    {
        float tanH = 1.0264f;
        float capF = M.CapFractionFromDegrees(0.6f, 0.6f, 0.344f), capB = M.CapFractionFromDegrees(1.2f, 0.6f, 0.344f);
        float kWorld = M.DisparityScale(0.063f, tanH);
        // World-space scene: screen at 4 units, content from 1 to 400 units.
        float qsW = 1f / 4f, qnW = 1f, qfW = 1f / 400f;
        float dWorld = M.Disparity(kWorld, 1f, qsW, qnW);
        float fWorld = M.StrengthForCaps(kWorld, 1f, qsW, qnW, qfW, capF, capB);
        Check(fWorld < 1f, "rig scale: the reference scene is cap-limited");
        foreach (float scale in new[] { 0.1f, 1f, 10f })
        {
            // Rig units: q_rig = scale * q_world (what the component computes from depth).
            float k = M.DisparityScale(M.BaselineInRigUnits(0.063f, scale), tanH);
            float d = M.Disparity(k, 1f, scale * qsW, scale * qnW);
            float f = M.StrengthForCaps(k, 1f, scale * qsW, scale * qnW, scale * qfW, capF, capB);
            Check(Near(d, dWorld, Math.Abs(dWorld) * 1e-4f), $"rig scale {scale}: disparity equals the world-space value ({d:F5} vs {dWorld:F5})");
            Check(Near(f, fWorld, 1e-4f), $"rig scale {scale}: strength equals the world-space value ({f:F4} vs {fWorld:F4})");
            M.ComfortRangeQ(k, 1f, scale * qsW, capF, capB, out float fq, out _);
            M.ComfortRangeQ(kWorld, 1f, qsW, capF, capB, out float fqW, out _);
            Check(Near(fq / scale, fqW, fqW * 1e-4f), $"rig scale {scale}: the gizmo's front limit is the same world distance");
        }
        // The old model (63 mm in RIG units) was wrong by the scale factor.
        float kOld = M.DisparityScale(0.063f, tanH);
        float fOld = M.StrengthForCaps(kOld, 1f, 10f * qsW, 10f * qnW, 10f * qfW, capF, capB);
        Check(fOld < 0.2f * fWorld, "rig scale 10: the unscaled baseline cut the strength ~10x too hard (regression guard)");
        Check(M.BaselineInRigUnits(0.063f, 0f) == 0.063f && M.BaselineInRigUnits(0.063f, float.NaN) == 0.063f, "degenerate scale reads as 1");
    }

    // Finding 5d: a ceiling that moves between samples re-targets at once.
    static void TestRetargetAndFollowerHardening()
    {
        var f = new M.Follower();
        f.Reset(1f);
        f.Retarget(2f);
        Check(!f.have && f.target == 1f, "retarget before the first measurement is a no-op");
        f.Measure(0.8f, 0f);
        for (int i = 0; i < 120; i++) f.StepAsymmetric(1f / 60f, 1.2f, 0.42f);
        f.Retarget(1.5f);
        Check(Near(f.target, 1.5f, 1e-6f), "retarget moves the target without waiting for the median");
        f.Measure(1.5f, 0f);
        Check(Near(f.target, 1.5f, 1e-6f), "retarget also refills the median");
        float x0 = f.x;
        float x1 = f.StepAsymmetric(1f / 60f, 1.2f, 0.42f);
        Check(x1 > x0 && x1 < x0 + 0.05f, "retarget is still followed smoothly");

        // A NaN time constant must never turn into a one-frame snap.
        var g = new M.Follower();
        g.Reset(0.5f); g.Measure(0.5f, 0f);
        for (int i = 0; i < 60; i++) g.Step(1f / 60f, 1.2f);
        g.Measure(1f, 0f); g.Measure(1f, 0f);
        float gx = g.Step(1f / 60f, float.NaN);
        Check(gx < 0.6f && !float.IsNaN(gx), $"NaN time constant: no snap ({gx:F3})");
        g.Step(float.NaN, 1.2f);
        Check(!float.IsNaN(g.x), "NaN dt: ignored");
        g.Measure(float.NaN, 0f); g.Measure(float.PositiveInfinity, 0f);
        Check(Near(g.target, 1f, 1e-6f), "NaN / infinite measurements are ignored");
    }

    // Design choice, pinned: with exactly two surfaces - a SMALL centred object in front of a
    // large wall - the percentile sits on the wall and the clump on the object, so the
    // screen lands half-way between them IN NEARNESS (i.e. the disparity between the two is
    // split evenly: the object comes out by half of it, the wall recedes by half). Once the
    // object fills about a fifth of the weighted frame the percentile joins the clump and the
    // screen sits on the object.
    static void TestTwoSurfacePolicy()
    {
        const float wall = 8f, obj = 2f;
        Run((u, v) => Centre(u, v, 0.11f) ? obj : wall, out var small);
        float mid = 0.5f * (1f / wall + 1f / obj);
        Check(Near(small.percentileQ, 1f / wall, 1e-3f) && Near(small.clumpQ, 1f / obj, 0.01f), "two surfaces, small object: percentile on the wall, clump on the object");
        Check(Near(small.screenQ, mid, 0.01f), $"two surfaces, small object: screen half-way in nearness ({1f / small.screenQ:F2} m)");
        Run((u, v) => Centre(u, v, 0.22f) ? obj : wall, out var large);
        Check(Near(large.screenQ, 1f / obj, 0.01f), $"two surfaces, large object: screen on the object ({1f / large.screenQ:F2} m)");
    }

    // ---- Integration findings (#461) ------------------------------------------------------

    // The delivered strength is one state: a cap pushes it down at once, it comes back only
    // gradually - also when the cap lifts while the convergence is HOLDING (the player run
    // popped 0.745 -> 1.0 in one frame when a low-confidence near intruder left).
    static void TestStrengthNeverPopsBack()
    {
        const float dt = 1f / 60f, tau = 1.2f, period = 0.15f;
        float k = M.DisparityScale(0.063f, (float)Math.Tan(30 * Math.PI / 180) * 16f / 9f);
        float capF = M.CapFractionFromDegrees(0.6f, 0.6f, 0.344f), capB = M.CapFractionFromDegrees(1.2f, 0.6f, 0.344f);

        // The scene: a wall at 4 m. The intruder: 25 texels at 0.25 m in a corner against it
        // - more than 2 % of the frame (the caps see it), too little to move the plane.
        Func<float, float, float> settled = (u, v) => 4f;
        Func<float, float, float> intruder = (u, v) => (u < 0.16f && v > 0.84f) ? 0.25f : 4f;

        var conv = new M.Follower(); conv.Reset(0f);
        var gov = new M.StrengthGovernor(); gov.Reset(1f);
        float nearQ = 0f, farQ = 0f, prefer = -1f;
        bool holdingSeen = false;
        float t = 0f, next = 0f, prev = 1f, minStrength = 1f, largestRise = 0f, dropFrames = 0f, worstCap = 0f;
        float strengthWhenLeft = 0f, tHalf = -1f;
        // 0-3 s settle, 3-6 s intruder present (20 samples), 6-16 s gone.
        for (int i = 0; i < 16 * 60; i++, t += dt)
        {
            bool present = t >= 3f && t < 6f;
            if (t >= next)
            {
                next = t + period;
                bool ok = Run(present ? intruder : settled, out var e, prefer);
                if (e.samples > 0) { nearQ = e.nearQ; farQ = e.farQ; }   // extremes: every sample
                if (ok) { prefer = e.clumpQ; conv.Measure(e.screenQ, 0.01f); }
                if (present) holdingSeen |= Near(conv.target, 0.25f, 0.02f); // plane did not chase it
            }
            float inv = conv.Step(dt, tau);
            float bound = M.StrengthForCaps(k, 1f, inv, nearQ, farQ, capF, capB);
            float f = gov.Step(dt, tau, 1f, bound);
            if (f > bound + 1e-6f || f > 1f + 1e-6f) worstCap = Math.Max(worstCap, f - Math.Min(bound, 1f));
            if (f - prev > largestRise) largestRise = f - prev;
            if (present && f < prev - 0.05f) dropFrames++;
            if (f < minStrength) minStrength = f;
            if (Math.Abs(t - 6f) < dt * 0.5f) strengthWhenLeft = prev;
            if (t > 6f && tHalf < 0f && f >= 0.5f * (1f + minStrength)) tHalf = t - 6f;
            prev = f;
        }
        float gap = 1f - minStrength;
        Check(holdingSeen, "intruder: the plane stays on the wall (it does not chase the intruder)");
        Check(minStrength < 0.9f, $"intruder: the front cap bites while the convergence holds (min strength {minStrength:F3})");
        Check(dropFrames == 1f, "intruder: the reduction is applied in ONE frame");
        Check(worstCap == 0f, "intruder: delivered strength never above the bound or the authored value");
        Check(largestRise <= gap * dt / tau, $"intruder left: largest one-frame rise {largestRise:F5} <= follower rate {gap * dt / tau:F5} (was {gap:F3} in one frame)");
        Check(tHalf > 0.5f, $"intruder left: half-way back only after {tHalf:F2} s");
        Check(Near(prev, 1f, 2e-3f), $"intruder left: full strength again in the end ({prev:F4})");

        // The same rule at the governor alone, with a bound that toggles every frame.
        var g = new M.StrengthGovernor(); g.Reset(1f);
        float gp = 1f, rise = 0f;
        bool under = true;
        for (int i = 0; i < 600; i++)
        {
            float b = (i % 2 == 0) ? 0.6f : 5f;
            float x = g.Step(dt, tau, 1f, b);
            if (x - gp > rise) rise = x - gp;
            if (x > Math.Min(1f, b) + 1e-6f) under = false;
            gp = x;
        }
        Check(under, "a bound toggling every frame: never above the bound");
        Check(rise <= 0.4f * dt / tau, $"a bound toggling every frame never lets the strength pop ({rise:F5})");

        // The authored value itself: followed directly while no cap bites, never while one does.
        var a = new M.StrengthGovernor(); a.Reset(1f);
        Check(a.Step(dt, tau, 1.5f, float.NaN) == 1.5f, "authored raised with no cap biting: followed at once (an app slider)");
        Check(a.Step(dt, tau, 0.8f, 9f) == 0.8f, "authored lowered: at once");
        a.Step(dt, tau, 1f, 0.5f);
        float up = a.Step(dt, tau, 2f, 9f);
        Check(up < 0.51f, $"authored raised while recovering from a cap: still gradual ({up:F4})");
        Check(a.Step(float.NaN, tau, 2f, 9f) == up && !float.IsNaN(a.Step(dt, float.NaN, 2f, 9f)), "NaN dt / tau are harmless");
        Check(a.Step(dt, tau, float.NaN, 9f) == 0f, "NaN authored: no stereo");
    }

    // A centred subject covering about a fifth of the frame gets the screen at ANY distance.
    // (Player run: subject 6 m / wall 40 m settled at 10.4 m - with a fixed metric histogram
    // both fell into neighbouring bins and the far plateau was not excluded.)
    static void TestSubjectAtAnyDistance()
    {
        float[][] pairs = { new[] { 1.5f, 6f }, new[] { 3f, 25f }, new[] { 6f, 40f }, new[] { 20f, 200f }, new[] { 60f, 900f } };
        foreach (var pr in pairs)
        {
            float subj = pr[0], wall = pr[1];
            bool ok = Run((u, v) => Centre(u, v, 0.24f) ? subj : wall, out var e);   // 23 % of the samples
            float dist = 1f / e.screenQ;
            Check(ok && Near(dist, subj, subj * 0.03f), $"subject {subj} m / wall {wall} m: screen on the subject ({dist:F2} m)");
            Check(Near(e.clumpQ, 1f / subj, 0.02f / subj), $"subject {subj} m / wall {wall} m: the clump is the subject, not the wall");
            // With relief on the subject (+-4 %) and a receding wall.
            ok = Run((u, v) => Centre(u, v, 0.24f) ? subj * (1f + 0.04f * (float)Math.Sin(40 * u + 23 * v))
                                                    : wall * (1f + 0.5f * u), out e);
            dist = 1f / e.screenQ;
            Check(ok && dist > subj * 0.9f && dist < subj * 1.15f, $"subject {subj} m / wall {wall} m, with relief: screen on the subject ({dist:F2} m)");
            // And with sky above the wall.
            ok = Run((u, v) => Centre(u, v, 0.24f) ? subj : (v > 0.8f ? float.PositiveInfinity : wall), out e);
            dist = 1f / e.screenQ;
            Check(ok && Near(dist, subj, subj * 0.03f), $"subject {subj} m / wall {wall} m / sky: screen on the subject ({dist:F2} m)");
        }
        // A continuous depth ramp (a floor to the horizon) is still a confident estimate.
        bool okRamp = Run((u, v) => 1f + 30f * v, out var ramp);
        Check(okRamp && ramp.valid, $"depth ramp: valid (confidence {ramp.confidence:F2})");
        Check(1f / ramp.screenQ > 1f && 1f / ramp.screenQ < 12f, $"depth ramp: screen in the near part ({1f / ramp.screenQ:F2} m)");
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
        TestDeliveredDisparityWhilePlaneMoves();
        TestNearIntruderDoesNotBlindTheClump();
        TestExtremesReportedWhenHolding();
        TestGridAcceptance();
        TestRigScale();
        TestRetargetAndFollowerHardening();
        TestTwoSurfacePolicy();
        TestStrengthNeverPopsBack();
        TestSubjectAtAnyDistance();
        Console.WriteLine($"auto_convergence_math (C#): {s_pass} passed, {s_fail} failed");
        return s_fail;
    }
}
