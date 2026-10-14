// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for DisplayXRCursorDepthMath - the Depth cursor source's unprojection.
// Hand-rolled (no test framework) so it also runs from a bare `csc` + the .NET runtime
// that ships inside the Unity editor. Exit code 0 = all passed.

using System;
using DisplayXR;

static class Program
{
    static int s_pass, s_fail;

    static void Check(bool ok, string what)
    {
        if (ok) s_pass++;
        else { s_fail++; Console.WriteLine("FAIL " + what); }
    }

    static bool Near(float a, float b, float eps = 1e-4f) => Math.Abs(a - b) <= eps;

    // Column-major 4x4 * (x, y, z, 1).
    static void Mul(float[] m, float x, float y, float z, out float cx, out float cy, out float cz, out float cw)
    {
        cx = m[0] * x + m[4] * y + m[8] * z + m[12];
        cy = m[1] * x + m[5] * y + m[9] * z + m[13];
        cz = m[2] * x + m[6] * y + m[10] * z + m[14];
        cw = m[3] * x + m[7] * y + m[11] * z + m[15];
    }

    // What the GPU writes: project a view-space point through GL.GetGPUProjectionMatrix's
    // matrix (our ToGpuProjection) and store depth the way each API does.
    static void Render(float[] proj, bool reversedZ, float x, float y, float z, out float u, out float vUp, out float raw)
    {
        var g = DisplayXRCursorDepthMath.ToGpuProjection(proj, reversedZ);
        Mul(g, x, y, z, out float cx, out float cy, out float cz, out float cw);
        u = (cx / cw + 1f) * 0.5f;
        vUp = (cy / cw + 1f) * 0.5f;
        raw = reversedZ ? cz / cw : (cz / cw + 1f) * 0.5f; // GL stores (z_ndc + 1) / 2
    }

    const float N = 0.1f, F = 100f;
    static readonly float Q = (float)(Math.PI / 4);

    static void TestKnownVectors()
    {
        // 90 deg symmetric frustum, point straight ahead at 2 m.
        var p = DisplayXRCursorDepthMath.ProjectionFromFov(-Q, Q, Q, -Q, N, F);
        Render(p, true, 0f, 0f, -2f, out float u, out float v, out float rawRev);
        Check(Near(u, 0.5f) && Near(v, 0.5f), "centre point lands at uv (0.5, 0.5)");
        // reversed-Z: raw = n (f - d) / (d (f - n)) = 0.1 * 98 / (2 * 99.9)
        Check(Near(rawRev, 0.0490490f, 1e-6f), $"reversed-Z raw at 2 m = 0.049049 (got {rawRev:F7})");
        Render(p, false, 0f, 0f, -2f, out _, out _, out float rawGl);
        Check(Near(rawGl, 0.9509510f, 1e-6f), $"OpenGL raw at 2 m = 0.950951 (got {rawGl:F7})");
        Check(Near(rawRev, 1f - rawGl, 1e-6f), "reversed raw = 1 - GL raw");

        // Near / far planes map to the documented ends.
        Render(p, true, 0f, 0f, -N, out _, out _, out float rn);
        Render(p, true, 0f, 0f, -F, out _, out _, out float rf);
        Check(Near(rn, 1f, 1e-5f) && Near(rf, 0f, 1e-5f), "reversed-Z: near = 1, far = 0");
        Render(p, false, 0f, 0f, -N, out _, out _, out float gn);
        Render(p, false, 0f, 0f, -F, out _, out _, out float gf);
        Check(Near(gn, 0f, 1e-5f) && Near(gf, 1f, 1e-5f), "OpenGL: near = 0, far = 1");

        // Unproject the known vectors back.
        Check(DisplayXRCursorDepthMath.UnprojectToView(p, 0.5f, 0.5f, 0.0490490f, true, out float x, out float y, out float z)
              && Near(x, 0f) && Near(y, 0f) && Near(z, -2f, 1e-3f), $"reversed-Z unproject -> (0,0,-2) (got z={z})");
        Check(DisplayXRCursorDepthMath.UnprojectToView(p, 0.5f, 0.5f, 0.9509510f, false, out x, out y, out z)
              && Near(z, -2f, 1e-3f), $"OpenGL unproject -> (0,0,-2) (got z={z})");

        // Top-right corner of the viewport at 2 m: (tan45 * 2, tan45 * 2) = (2, 2).
        Check(DisplayXRCursorDepthMath.UnprojectToView(p, 1f, 1f, 0.0490490f, true, out x, out y, out z)
              && Near(x, 2f, 1e-3f) && Near(y, 2f, 1e-3f), "uv (1,1) = top-right (bottom-left origin)");
    }

    static void TestRoundTripOffAxis()
    {
        // A Kooima-style off-axis frustum (what a tracked off-centre eye gets).
        var p = DisplayXRCursorDepthMath.ProjectionFromFov(-0.40f, 0.25f, 0.30f, -0.20f, 0.05f, 50f);
        float[] depths = { 0.08f, 0.3f, 0.6f, 1.2f, 7f, 40f };
        foreach (bool rev in new[] { true, false })
        foreach (float d in depths)
        for (int i = 0; i <= 4; i++)
        for (int j = 0; j <= 4; j++)
        {
            // A point on the ray through a grid of NDC positions.
            float sx = -0.9f + 0.45f * i, sy = -0.9f + 0.45f * j;
            float x = (sx - p[8]) / p[0] * d, y = (sy - p[9]) / p[5] * d; // inverse of the x/y rows at w = d
            Render(p, rev, x, y, -d, out float u, out float v, out float raw);
            bool ok = DisplayXRCursorDepthMath.UnprojectToView(p, u, v, raw, rev, out float ux, out float uy, out float uz);
            // float32 depth precision: relative tolerance grows with distance on OpenGL.
            float tol = rev ? 2e-3f * d : 2e-2f * d * d;
            Check(ok && Near(ux, x, tol) && Near(uy, y, tol) && Near(uz, -d, tol),
                  $"off-axis round trip rev={rev} d={d} ({i},{j}) -> ({ux},{uy},{uz}) want ({x},{y},{-d})");
        }
    }

    static void TestGpuRemap()
    {
        var p = DisplayXRCursorDepthMath.ProjectionFromFov(-0.5f, 0.5f, 0.4f, -0.4f, N, F);
        var g = DisplayXRCursorDepthMath.ToGpuProjection(p, false);
        bool same = true;
        for (int i = 0; i < 16; i++) same &= g[i] == p[i];
        Check(same, "OpenGL: GPU projection == GL projection");
        var r = DisplayXRCursorDepthMath.ToGpuProjection(p, true);
        Check(r[0] == p[0] && r[5] == p[5] && r[8] == p[8], "reversed: x/y rows untouched");
        Check(Near(r[10], 0.5f * p[11] - 0.5f * p[10], 1e-6f) && Near(r[14], -0.5f * p[14], 1e-6f), "reversed: z row = (row3 - row2) / 2");
    }

    static void TestBackgroundAndNearest()
    {
        Check(DisplayXRCursorDepthMath.IsBackground(0f, true), "reversed: 0 is background");
        Check(!DisplayXRCursorDepthMath.IsBackground(0.01f, true), "reversed: 0.01 is geometry");
        Check(DisplayXRCursorDepthMath.IsBackground(1f, false), "GL: 1 is background");
        Check(DisplayXRCursorDepthMath.IsBackground(float.NaN, true), "NaN is background");
        Check(!DisplayXRCursorDepthMath.UnprojectToView(DisplayXRCursorDepthMath.ProjectionFromFov(-Q, Q, Q, -Q, N, F),
              0.5f, 0.5f, 0f, true, out _, out _, out _), "background does not unproject");

        float[] rev = { 0f, 0.02f, 0.05f, float.NaN, 0.03f, 0f };
        Check(DisplayXRCursorDepthMath.NearestTexel(rev, rev.Length, true) == 2, "reversed: nearest = largest raw");
        float[] gl = { 1f, 0.98f, 0.95f, 0.97f, 1f };
        Check(DisplayXRCursorDepthMath.NearestTexel(gl, gl.Length, false) == 2, "GL: nearest = smallest raw");
        Check(DisplayXRCursorDepthMath.NearestTexel(new float[] { 0f, 0f }, 2, true) == -1, "all background -> -1");
        Check(DisplayXRCursorDepthMath.NearestTexel(null, 4, true) == -1, "null -> -1");
        Check(DisplayXRCursorDepthMath.NearestTexel(rev, 99, true) == 2, "count clamps to array length");
    }

    static void TestPatchAndTexels()
    {
        float a = 16f / 9f;
        DisplayXRCursorDepthMath.PatchRect(0.5f, 0.5f, a, out float x, out float y, out float w, out float h);
        float cu = 0.5f + 0.3f * 0.03f / a, cvDown = 0.5f + 0.5f * 0.03f;
        Check(Near(x + w / 2, cu, 1e-6f) && Near(y + h / 2, 1f - cvDown, 1e-6f), "patch centred on the arrow body");
        Check(Near(h, 0.048f, 1e-6f) && Near(w * a, h, 1e-6f), "patch is 1.6 cursor heights, square on screen");
        // The hotspot (tip) lies inside the patch.
        Check(0.5f >= x && 0.5f <= x + w && 0.5f >= y && 0.5f <= y + h, "tip inside the patch");

        DisplayXRCursorDepthMath.TexelToViewportUV(0, 32, x, y, w, h, out float u0, out float v0);
        Check(Near(u0, x + w / 64f, 1e-6f) && Near(v0, y + h / 64f, 1e-6f), "texel 0 = bottom-left texel centre");
        DisplayXRCursorDepthMath.TexelToViewportUV(32 * 32 - 1, 32, x, y, w, h, out float u1, out float v1);
        Check(Near(u1, x + w - w / 64f, 1e-6f) && Near(v1, y + h - h / 64f, 1e-6f), "last texel = top-right texel centre");
        DisplayXRCursorDepthMath.TexelToViewportUV(32 + 3, 32, x, y, w, h, out float u2, out float v2);
        Check(Near(u2, x + 3.5f / 32f * w, 1e-6f) && Near(v2, y + 1.5f / 32f * h, 1e-6f), "row-major, row 0 at the bottom");
    }

    static void TestPose()
    {
        // Yaw 90 deg about +Y: view -Z (forward) becomes -X.
        float s = (float)Math.Sin(Math.PI / 4);
        float[] q = { 0f, s, 0f, s };
        float[] pos = { 0.03f, 0.10f, 0.60f };
        DisplayXRCursorDepthMath.ViewToSpace(pos, q, 0f, 0f, -1f, out float x, out float y, out float z);
        Check(Near(x, 0.03f - 1f) && Near(y, 0.10f) && Near(z, 0.60f), "pose: rotate then translate");
        DisplayXRCursorDepthMath.ViewToSpace(pos, new float[] { 0, 0, 0, 1 }, 0.1f, 0.2f, -0.3f, out x, out y, out z);
        Check(Near(x, 0.13f) && Near(y, 0.30f) && Near(z, 0.30f), "identity pose = translate only");
    }

    static void TestEndToEnd()
    {
        // An eye 0.6 m in front of a 0.4 x 0.3 m canvas at z = 0, looking down -Z, with a
        // "cube face" 0.05 m in front of the canvas filling the lower half of the patch.
        float d = 0.6f;
        float[] eye = { 0.0f, 0.0f, d };
        float[] q = { 0, 0, 0, 1 };
        var p = DisplayXRCursorDepthMath.ProjectionFromFov((float)Math.Atan(-0.2 / d), (float)Math.Atan(0.2 / d),
                                                         (float)Math.Atan(0.15 / d), (float)Math.Atan(-0.15 / d), 0.05f, 100f);
        DisplayXRCursorDepthMath.PatchRect(0.5f, 0.5f, 0.4f / 0.3f, out float px, out float py, out float pw, out float ph);
        const int S = 32;
        var raw = new float[S * S];
        for (int i = 0; i < raw.Length; i++)
        {
            DisplayXRCursorDepthMath.TexelToViewportUV(i, S, px, py, pw, ph, out float u, out float v);
            bool face = (i / S) < S / 2; // bottom half of the patch
            if (!face) { raw[i] = 0f; continue; }
            float vd = d - 0.05f; // view distance to the face plane
            float x = (2f * u - 1f - p[8]) / p[0] * vd, y = (2f * v - 1f - p[9]) / p[5] * vd;
            Render(p, true, x, y, -vd, out _, out _, out raw[i]);
        }
        int best = DisplayXRCursorDepthMath.NearestTexel(raw, raw.Length, true);
        Check(best >= 0 && best / S < S / 2, "nearest texel is on the face");
        DisplayXRCursorDepthMath.TexelToViewportUV(best, S, px, py, pw, ph, out float bu, out float bv);
        Check(DisplayXRCursorDepthMath.UnprojectToView(p, bu, bv, raw[best], true, out float vx, out float vy, out float vz), "face unprojects");
        DisplayXRCursorDepthMath.ViewToSpace(eye, q, vx, vy, vz, out float wx, out float wy, out float wz);
        Check(Near(wz, 0.05f, 1e-3f), $"world point is 5 cm in front of the canvas (z={wz})");
    }

    static int Main()
    {
        TestKnownVectors();
        TestRoundTripOffAxis();
        TestGpuRemap();
        TestBackgroundAndNearest();
        TestPatchAndTexels();
        TestPose();
        TestEndToEnd();
        Console.WriteLine($"cursor_depth_math (C#): {s_pass} passed, {s_fail} failed");
        return s_fail == 0 ? 0 : 1;
    }
}
