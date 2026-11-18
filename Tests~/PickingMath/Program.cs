// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for DisplayXRPickingMath - the math behind DisplayXRPicking (#460).
// Hand-rolled (no test framework) so it also runs from a bare `csc` + the .NET runtime
// that ships inside the Unity editor. Exit code 0 = all passed.
//
// The fixture is a physical one: a display rectangle, two eyes in front of it, and the
// off-axis frustum each eye needs to frame that rectangle. The frusta are built by the
// provider's own builder (DisplayXRCursorDepthMath.ProjectionFromFov) from the fov the
// geometry dictates, so the tests check the picking math against the geometry and not
// against itself.

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

    const float N = 0.05f, F = 100f;

    // ---- Fixture ------------------------------------------------------------------------

    // A rig: where the display rectangle sits in the locate space (OpenXR: right-handed,
    // -Z forward, the viewer on the +Z side of an unrotated display).
    struct Rig
    {
        public float[] pos;   // display centre
        public float[] quat;  // display orientation (x, y, z, w)
        public float w, h;    // display size
    }

    static Rig MakeRig(float px, float py, float pz, float yawDeg, float pitchDeg, float w, float h)
    {
        double y = yawDeg * Math.PI / 360.0, p = pitchDeg * Math.PI / 360.0;
        // q = yaw(Y) * pitch(X)
        float[] qy = { 0f, (float)Math.Sin(y), 0f, (float)Math.Cos(y) };
        float[] qp = { (float)Math.Sin(p), 0f, 0f, (float)Math.Cos(p) };
        return new Rig { pos = new[] { px, py, pz }, quat = QMul(qy, qp), w = w, h = h };
    }

    static float[] QMul(float[] a, float[] b)
    {
        return new[]
        {
            a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
            a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
            a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
            a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2],
        };
    }

    // Rig-local point -> locate space.
    static void RigToSpace(Rig r, float x, float y, float z, out float ox, out float oy, out float oz)
    {
        DisplayXRCursorDepthMath.Rotate(r.quat, x, y, z, out ox, out oy, out oz);
        ox += r.pos[0]; oy += r.pos[1]; oz += r.pos[2];
    }

    // Rig-local point -> UNITY coordinates (locate space with Z negated): what the picking
    // math takes and returns.
    static void RigToUnity(Rig r, float x, float y, float z, out float ox, out float oy, out float oz)
    {
        RigToSpace(r, x, y, z, out ox, out oy, out oz);
        oz = -oz;
    }

    // The view + projection of an eye at rig-local (ex, ey, ez), ez > 0, facing the display:
    // the eye keeps the display's orientation, and its frustum passes through the display's
    // four edges. Column-major.
    static void EyeMatrices(Rig r, float ex, float ey, float ez, out float[] view, out float[] proj)
    {
        proj = DisplayXRCursorDepthMath.ProjectionFromFov(
            (float)Math.Atan2(-r.w / 2 - ex, ez), (float)Math.Atan2(r.w / 2 - ex, ez),
            (float)Math.Atan2(r.h / 2 - ey, ez), (float)Math.Atan2(-r.h / 2 - ey, ez), N, F);
        RigToSpace(r, ex, ey, ez, out float px, out float py, out float pz);
        // view = inverse of the eye pose: rotate by the conjugate, after subtracting the position.
        float[] qc = { -r.quat[0], -r.quat[1], -r.quat[2], r.quat[3] };
        view = new float[16];
        for (int c = 0; c < 3; c++)
        {
            DisplayXRCursorDepthMath.Rotate(qc, c == 0 ? 1f : 0f, c == 1 ? 1f : 0f, c == 2 ? 1f : 0f,
                                            out view[c * 4], out view[c * 4 + 1], out view[c * 4 + 2]);
        }
        DisplayXRCursorDepthMath.Rotate(qc, -px, -py, -pz, out view[12], out view[13], out view[14]);
        view[15] = 1f;
    }

    // Distance from a point to a ray (0 = the ray passes through it), and the ray parameter.
    static float DistToRay(float ox, float oy, float oz, float dx, float dy, float dz,
                           float x, float y, float z, out float t)
    {
        float vx = x - ox, vy = y - oy, vz = z - oz;
        t = vx * dx + vy * dy + vz * dz;
        float cx = vx - t * dx, cy = vy - t * dy, cz = vz - t * dz;
        return (float)Math.Sqrt(cx * cx + cy * cy + cz * cz);
    }

    // ---- Tests --------------------------------------------------------------------------

    static void TestKnownVectors()
    {
        // Unrotated 0.4 x 0.3 display at the origin, eye centred 0.6 in front of it.
        var rig = MakeRig(0, 0, 0, 0, 0, 0.4f, 0.3f);
        EyeMatrices(rig, 0f, 0f, 0.6f, out float[] v, out float[] p);

        // Centre of the screen: a ray straight at the display. In Unity coordinates the eye
        // is at z = -0.6 and looks down +Z, so the near-plane origin is at z = -0.6 + N.
        Check(DisplayXRPickingMath.TryUnprojectRay(v, p, 0f, 0f, out float ox, out float oy, out float oz,
                                                   out float dx, out float dy, out float dz), "centre ray builds");
        Check(Near(ox, 0f) && Near(oy, 0f) && Near(oz, -0.6f + N), $"centre ray starts on the near plane (z={oz})");
        Check(Near(dx, 0f) && Near(dy, 0f) && Near(dz, 1f), "centre ray points down +Z (Unity forward)");

        // Top-right NDC corner: the ray passes through the display's top-right corner.
        Check(DisplayXRPickingMath.TryUnprojectRay(v, p, 1f, 1f, out ox, out oy, out oz, out dx, out dy, out dz),
              "corner ray builds");
        Check(DistToRay(ox, oy, oz, dx, dy, dz, 0.2f, 0.15f, 0f, out float t) < 1e-5f && t > 0f,
              "NDC (1, 1) passes through the display's top-right corner");

        // The display centre projects to the middle of the screen at depth 0.6.
        Check(DisplayXRPickingMath.TryProject(v, p, 0f, 0f, 0f, out float nx, out float ny, out float nz, out float depth),
              "display centre projects");
        Check(Near(nx, 0f) && Near(ny, 0f) && Near(depth, 0.6f), $"display centre -> NDC (0, 0), depth 0.6 (got {depth})");
        Check(nz > -1f && nz < 1f, "display centre is between the clip planes");

        // +X is right and +Y is up on the screen.
        DisplayXRPickingMath.TryProject(v, p, 0.1f, 0.075f, 0f, out nx, out ny, out _, out _);
        Check(Near(nx, 0.5f) && Near(ny, 0.5f), $"a quarter of the way to the top-right corner -> NDC (0.5, 0.5) (got {nx}, {ny})");
    }

    static void TestRoundTrip()
    {
        // world -> screen -> ray passes through the point, for an off-axis eye pair behind a
        // moved and turned display, per eye and through the cyclopean view.
        var rigs = new[]
        {
            MakeRig(0, 0, 0, 0, 0, 0.4f, 0.3f),
            MakeRig(1.5f, -0.4f, 2.0f, 30f, -12f, 0.6f, 0.34f),
            MakeRig(-3f, 1f, -0.5f, -75f, 20f, 0.31f, 0.2f),
        };
        float[][] heads = { new[] { 0f, 0f, 0.6f }, new[] { 0.18f, 0.09f, 0.5f }, new[] { -0.25f, -0.12f, 0.9f } };
        float[][] points = // rig-local: on the plane, behind it (z < 0), in front of it (z > 0)
        {
            new[] { 0f, 0f, 0f }, new[] { 0.15f, -0.1f, 0f }, new[] { -0.1f, 0.05f, -0.3f },
            new[] { 0.3f, 0.2f, -2f }, new[] { 0.02f, -0.03f, 0.2f }, new[] { -0.4f, 0.3f, -10f },
        };
        const float ipd = 0.063f;
        var cv = new float[16]; var cp = new float[16];
        foreach (var rig in rigs)
        foreach (var h in heads)
        {
            EyeMatrices(rig, h[0] - ipd / 2, h[1], h[2], out float[] lv, out float[] lp);
            EyeMatrices(rig, h[0] + ipd / 2, h[1], h[2], out float[] rv, out float[] rp);
            DisplayXRPickingMath.Cyclopean(lv, lp, rv, rp, cv, cp);
            float[][] views = { lv, rv, cv }, projs = { lp, rp, cp };
            foreach (var pt in points)
            for (int e = 0; e < 3; e++)
            {
                RigToUnity(rig, pt[0], pt[1], pt[2], out float wx, out float wy, out float wz);
                bool ok = DisplayXRPickingMath.TryProject(views[e], projs[e], wx, wy, wz,
                                                          out float nx, out float ny, out _, out float depth);
                ok &= DisplayXRPickingMath.TryUnprojectRay(views[e], projs[e], nx, ny, out float ox, out float oy, out float oz,
                                                           out float dx, out float dy, out float dz);
                float miss = ok ? DistToRay(ox, oy, oz, dx, dy, dz, wx, wy, wz, out float t) : float.NaN;
                float tol = 2e-4f * Math.Max(1f, depth);
                Check(ok && miss < tol, $"round trip eye {e} point ({pt[0]}, {pt[1]}, {pt[2]}) head ({h[0]}, {h[1]}, {h[2]}): ray misses by {miss}");
                Check(ok && Near(depth, h[2] - pt[2], 1e-3f), $"depth is the distance along the view axis (got {depth}, want {h[2] - pt[2]})");

                // ... and the same screen point at that depth is the point itself.
                ok = DisplayXRPickingMath.TryUnprojectPoint(views[e], projs[e], nx, ny, depth, out float ux, out float uy, out float uz);
                Check(ok && Near(ux, wx, tol) && Near(uy, wy, tol) && Near(uz, wz, tol),
                      $"screen point at depth {depth} -> ({ux}, {uy}, {uz}), want ({wx}, {wy}, {wz})");
            }
        }
    }

    static void TestDisplayPlaneAndDisparity()
    {
        // The property the whole display rests on: a point ON the display plane lands on the
        // same pixel for both eyes and for any head position; a point off the plane does not.
        var rig = MakeRig(0.2f, 0.1f, -1f, 20f, 5f, 0.4f, 0.3f);
        const float ipd = 0.06f, d = 0.6f;
        EyeMatrices(rig, -ipd / 2, 0f, d, out float[] lv, out float[] lp);
        EyeMatrices(rig, +ipd / 2, 0f, d, out float[] rv, out float[] rp);
        var cv = new float[16]; var cp = new float[16];
        DisplayXRPickingMath.Cyclopean(lv, lp, rv, rp, cv, cp);

        RigToUnity(rig, 0.1f, -0.05f, 0f, out float wx, out float wy, out float wz);
        DisplayXRPickingMath.TryProject(lv, lp, wx, wy, wz, out float lx, out float ly, out _, out _);
        DisplayXRPickingMath.TryProject(rv, rp, wx, wy, wz, out float rx, out float ry, out _, out _);
        DisplayXRPickingMath.TryProject(cv, cp, wx, wy, wz, out float cx, out float cy, out _, out _);
        Check(Near(lx, rx, 1e-5f) && Near(ly, ry, 1e-5f) && Near(lx, cx, 1e-5f) && Near(ly, cy, 1e-5f),
              "a point on the display plane has zero disparity");
        Check(Near(lx, 0.5f, 1e-5f) && Near(ly, -1f / 3f, 1e-5f), $"... and sits where it is on the panel (got {lx}, {ly})");

        // A point z behind the plane: similar triangles put the two images ipd * z / (d + z)
        // apart on the panel, the left eye's image on the left. In NDC that is * 2 / width.
        float z = 0.3f;
        RigToUnity(rig, 0.1f, -0.05f, -z, out wx, out wy, out wz);
        DisplayXRPickingMath.TryProject(lv, lp, wx, wy, wz, out lx, out ly, out _, out _);
        DisplayXRPickingMath.TryProject(rv, rp, wx, wy, wz, out rx, out ry, out _, out _);
        DisplayXRPickingMath.TryProject(cv, cp, wx, wy, wz, out cx, out cy, out _, out _);
        float want = ipd * z / (d + z) * 2f / rig.w;
        Check(Near(rx - lx, want, 1e-5f), $"behind the plane: uncrossed disparity {rx - lx}, want {want}");
        Check(Near(ly, ry, 1e-5f), "disparity is horizontal only");
        Check(Near(cx, 0.5f * (lx + rx), 1e-5f) && Near(cy, ly, 1e-5f), "the cyclopean image is midway between the two eyes'");

        // In front of the plane the disparity is crossed (the left eye's image on the right).
        RigToUnity(rig, 0.1f, -0.05f, 0.2f, out wx, out wy, out wz);
        DisplayXRPickingMath.TryProject(lv, lp, wx, wy, wz, out lx, out _, out _, out _);
        DisplayXRPickingMath.TryProject(rv, rp, wx, wy, wz, out rx, out _, out _, out _);
        Check(Near(rx - lx, -ipd * 0.2f / (d - 0.2f) * 2f / rig.w, 1e-5f), $"in front of the plane: crossed disparity {rx - lx}");

        // Why a fixed projection misses: move the head and the off-plane point's pixel moves
        // by the motion parallax (shift * z / (d + z)), while an on-plane point stays put. A
        // ray built from a projection that does not know the head position cannot follow.
        const float shift = 0.15f;
        EyeMatrices(rig, shift, 0f, d, out float[] mv, out float[] mp);
        EyeMatrices(rig, 0f, 0f, d, out float[] zv, out float[] zp);
        RigToUnity(rig, 0.1f, -0.05f, -z, out wx, out wy, out wz);
        DisplayXRPickingMath.TryProject(zv, zp, wx, wy, wz, out float x0, out _, out _, out _);
        DisplayXRPickingMath.TryProject(mv, mp, wx, wy, wz, out float x1, out _, out _, out _);
        Check(Near(x1 - x0, shift * z / (d + z) * 2f / rig.w, 1e-5f), $"head 15 cm right: the point 30 cm behind moves {x1 - x0} NDC");
        // The same ray, read as if the head had not moved, misses the point by that parallax.
        DisplayXRPickingMath.TryUnprojectRay(zv, zp, x1, 0f, out float ox, out float oy, out float oz,
                                             out float dx, out float dy, out float dz);
        RigToUnity(rig, 0.1f, 0f, -z, out float qx, out float qy, out float qz);
        float miss = DistToRay(ox, oy, oz, dx, dy, dz, qx, qy, qz, out _);
        Check(miss > 0.04f, $"a ray that ignores the head position misses the point by {miss} m (> 4 cm)");
        DisplayXRPickingMath.TryUnprojectRay(mv, mp, x1, 0f, out ox, out oy, out oz, out dx, out dy, out dz);
        Check(DistToRay(ox, oy, oz, dx, dy, dz, qx, qy, qz, out _) < 1e-4f, "... and the ray through the tracked view hits it");
    }

    static void TestCyclopeanIsTheMidpointEye()
    {
        // For two eyes at the same distance from the display, the averaged matrices are the
        // matrices the midpoint eye would get.
        var rig = MakeRig(-0.3f, 0.2f, 0.7f, -40f, 8f, 0.52f, 0.29f);
        EyeMatrices(rig, 0.07f - 0.032f, -0.04f, 0.55f, out float[] lv, out float[] lp);
        EyeMatrices(rig, 0.07f + 0.032f, -0.04f, 0.55f, out float[] rv, out float[] rp);
        EyeMatrices(rig, 0.07f, -0.04f, 0.55f, out float[] mv, out float[] mp);
        var cv = new float[16]; var cp = new float[16];
        DisplayXRPickingMath.Cyclopean(lv, lp, rv, rp, cv, cp);
        bool same = true;
        for (int i = 0; i < 16; i++) same &= Near(cv[i], mv[i], 1e-5f) && Near(cp[i], mp[i], 1e-5f);
        Check(same, "cyclopean view + projection == the midpoint eye's");
    }

    static void TestCyclopeanWithATurnedHead()
    {
        // Eyes at DIFFERENT distances from the display (a head turned and tilted): the
        // averaged matrices are still exactly the midpoint eye's, because every term of an
        // off-axis projection that frames one fixed rectangle is linear in the eye position.
        var rig = MakeRig(0.4f, -0.1f, 1.2f, 25f, -10f, 0.52f, 0.29f);
        float[] l = { 0.04f, -0.05f, 0.48f }, r = { 0.09f, -0.02f, 0.63f };
        float[] m = { (l[0] + r[0]) / 2, (l[1] + r[1]) / 2, (l[2] + r[2]) / 2 };
        EyeMatrices(rig, l[0], l[1], l[2], out float[] lv, out float[] lp);
        EyeMatrices(rig, r[0], r[1], r[2], out float[] rv, out float[] rp);
        EyeMatrices(rig, m[0], m[1], m[2], out float[] mv, out float[] mp);
        var cv = new float[16]; var cp = new float[16];
        DisplayXRPickingMath.Cyclopean(lv, lp, rv, rp, cv, cp);
        bool same = true;
        for (int i = 0; i < 16; i++) same &= Near(cv[i], mv[i], 1e-5f) && Near(cp[i], mp[i], 1e-5f);
        Check(same, "eyes at unequal depth: cyclopean view + projection == the midpoint eye's");

        // ... so the cyclopean ray through a screen point starts at the midpoint of the eyes
        // (extended back from the near plane) and passes through that point of the panel.
        DisplayXRPickingMath.TryUnprojectRay(cv, cp, 0.3f, -0.4f, out float ox, out float oy, out float oz,
                                             out float dx, out float dy, out float dz);
        RigToUnity(rig, m[0], m[1], m[2], out float ex, out float ey, out float ez);
        RigToUnity(rig, 0.3f * rig.w / 2, -0.4f * rig.h / 2, 0f, out float sx, out float sy, out float sz);
        Check(DistToRay(ox, oy, oz, dx, dy, dz, ex, ey, ez, out float te) < 1e-4f && te < 0f,
              "the cyclopean ray comes from the midpoint between the eyes");
        Check(DistToRay(ox, oy, oz, dx, dy, dz, sx, sy, sz, out float ts) < 1e-4f && ts > 0f,
              "... through the panel point under the cursor");
    }

    // A rigid pose applied to a point: rotate, then translate.
    static void Apply(float[] pos, float[] quat, float x, float y, float z, out float ox, out float oy, out float oz)
    {
        DisplayXRCursorDepthMath.Rotate(quat, x, y, z, out ox, out oy, out oz);
        ox += pos[0]; oy += pos[1]; oz += pos[2];
    }

    static void TestFrameFromPoses()
    {
        // The frame the located views sit in for a camera is camera * rig^-1. Checked the
        // way it is used: a point the RIG pose puts somewhere must land where the CAMERA
        // pose puts it, i.e. frame(rig(x)) == camera(x) for every x.
        var cams = new[] { MakeRig(1.5f, -0.4f, 2.0f, 30f, -12f, 0, 0), MakeRig(-300f, 12f, 80f, -140f, 35f, 0, 0),
                           MakeRig(0, 0, 0, 0, 0, 0, 0) };
        var rigs = new[] { MakeRig(0.2f, 0.1f, -1f, 20f, 5f, 0, 0), MakeRig(40f, -7f, 15f, 95f, -60f, 0, 0),
                           MakeRig(0, 0, 0, 0, 0, 0, 0) };
        float[][] pts = { new[] { 0f, 0f, 0f }, new[] { 0.3f, -0.2f, 1.5f }, new[] { -4f, 2f, -9f } };
        foreach (var c in cams)
        foreach (var r in rigs)
        {
            DisplayXRPickingMath.FrameFromPoses(
                c.pos[0], c.pos[1], c.pos[2], c.quat[0], c.quat[1], c.quat[2], c.quat[3],
                r.pos[0], r.pos[1], r.pos[2], r.quat[0], r.quat[1], r.quat[2], r.quat[3],
                out float px, out float py, out float pz, out float qx, out float qy, out float qz, out float qw);
            float[] fp = { px, py, pz }, fq = { qx, qy, qz, qw };
            Check(Near(qx * qx + qy * qy + qz * qz + qw * qw, 1f, 1e-5f), "the frame rotation is a unit quaternion");
            foreach (var x in pts)
            {
                Apply(r.pos, r.quat, x[0], x[1], x[2], out float rx, out float ry, out float rz);
                Apply(fp, fq, rx, ry, rz, out float gx, out float gy, out float gz);
                Apply(c.pos, c.quat, x[0], x[1], x[2], out float wx, out float wy, out float wz);
                float tol = 1e-4f * Math.Max(1f, Math.Abs(wx) + Math.Abs(wy) + Math.Abs(wz));
                Check(Near(gx, wx, tol) && Near(gy, wy, tol) && Near(gz, wz, tol),
                      $"frame(rig(x)) == camera(x): got ({gx}, {gy}, {gz}), want ({wx}, {wy}, {wz})");
            }
        }

        // The camera standing where its rig pushed: the identity, also far from the origin.
        var far = MakeRig(-300f, 12f, 80f, -140f, 35f, 0, 0);
        DisplayXRPickingMath.FrameFromPoses(
            far.pos[0], far.pos[1], far.pos[2], far.quat[0], far.quat[1], far.quat[2], far.quat[3],
            far.pos[0], far.pos[1], far.pos[2], far.quat[0], far.quat[1], far.quat[2], far.quat[3],
            out float ix, out float iy, out float iz, out float jx, out float jy, out float jz, out float jw);
        Check(Near(ix, 0f, 1e-4f) && Near(iy, 0f, 1e-4f) && Near(iz, 0f, 1e-4f) &&
              Near(jx, 0f, 1e-6f) && Near(jy, 0f, 1e-6f) && Near(jz, 0f, 1e-6f) && Near(Math.Abs(jw), 1f, 1e-6f),
              $"camera pose == rig pose -> the identity frame (got pos ({ix}, {iy}, {iz}))");

        // No rig pose sent (identity): the frame is the camera's own pose.
        var cam = MakeRig(1.5f, -0.4f, 2.0f, 30f, -12f, 0, 0);
        DisplayXRPickingMath.FrameFromPoses(
            cam.pos[0], cam.pos[1], cam.pos[2], cam.quat[0], cam.quat[1], cam.quat[2], cam.quat[3],
            0f, 0f, 0f, 0f, 0f, 0f, 1f,
            out ix, out iy, out iz, out jx, out jy, out jz, out jw);
        Check(Near(ix, cam.pos[0]) && Near(iy, cam.pos[1]) && Near(iz, cam.pos[2]) && Near(jx, cam.quat[0], 1e-6f) &&
              Near(jy, cam.quat[1], 1e-6f) && Near(jz, cam.quat[2], 1e-6f) && Near(jw, cam.quat[3], 1e-6f),
              "no rig pose -> the frame is the camera pose");

        // The rig camera moved 0.5 along x AFTER its rig stopped pushing: the views move with it.
        DisplayXRPickingMath.FrameFromPoses(
            cam.pos[0] + 0.5f, cam.pos[1], cam.pos[2], cam.quat[0], cam.quat[1], cam.quat[2], cam.quat[3],
            cam.pos[0], cam.pos[1], cam.pos[2], cam.quat[0], cam.quat[1], cam.quat[2], cam.quat[3],
            out ix, out iy, out iz, out jx, out jy, out jz, out jw);
        Check(Near(ix, 0.5f, 1e-5f) && Near(iy, 0f, 1e-5f) && Near(iz, 0f, 1e-5f) && Near(Math.Abs(jw), 1f, 1e-6f),
              $"a camera that moved after the last push shifts the views by the same amount (got {ix}, {iy}, {iz})");
    }

    static void TestBehindTheEye()
    {
        var rig = MakeRig(0, 0, 0, 0, 0, 0.4f, 0.3f);
        EyeMatrices(rig, 0.05f, 0f, 0.6f, out float[] v, out float[] p);
        // The eye is at Unity z = -0.6. A point at z = -1 is 0.4 behind it.
        bool ok = DisplayXRPickingMath.TryProject(v, p, 0.1f, 0.1f, -1f, out float nx, out float ny, out _, out float depth);
        Check(!ok, "a point behind the eye is rejected");
        Check(Near(depth, -0.4f), $"... with its (negative) depth still reported (got {depth})");
        Check(nx == 0f && ny == 0f, "... and no screen position");
        // On the eye plane itself: rejected too (it would divide by zero).
        ok = DisplayXRPickingMath.TryProject(v, p, 0.3f, 0.1f, -0.6f, out _, out _, out _, out depth);
        Check(!ok && Near(depth, 0f), "a point on the eye plane is rejected");
        // Just in front of the eye but before the near plane: projects (NDC z < -1 says so).
        ok = DisplayXRPickingMath.TryProject(v, p, 0.05f, 0f, -0.6f + 0.01f, out _, out _, out float nz, out depth);
        Check(ok && nz < -1f && Near(depth, 0.01f), "a point before the near plane projects, with NDC z < -1");
        // Beyond the far plane: NDC z > 1.
        ok = DisplayXRPickingMath.TryProject(v, p, 0f, 0f, 500f, out _, out _, out nz, out _);
        Check(ok && nz > 1f, "a point beyond the far plane projects, with NDC z > 1");
    }

    static void TestDegenerate()
    {
        var rig = MakeRig(0, 0, 0, 0, 0, 0.4f, 0.3f);
        EyeMatrices(rig, 0f, 0f, 0.6f, out float[] v, out float[] p);
        var zero = new float[16];
        Check(!DisplayXRPickingMath.TryUnprojectRay(v, zero, 0f, 0f, out _, out _, out _, out _, out _, out _),
              "a zero projection gives no ray");
        Check(!DisplayXRPickingMath.TryUnprojectRay(zero, p, 0f, 0f, out _, out _, out _, out _, out _, out _),
              "a zero view gives no ray");
        Check(!DisplayXRPickingMath.TryUnprojectRay(null, p, 0f, 0f, out _, out _, out _, out _, out _, out _) &&
              !DisplayXRPickingMath.TryUnprojectRay(v, new float[4], 0f, 0f, out _, out _, out _, out _, out _, out _),
              "null / short arrays give no ray");
        Check(!DisplayXRPickingMath.TryUnprojectPoint(v, zero, 0f, 0f, 1f, out _, out _, out _), "a zero projection gives no point");
        Check(!DisplayXRPickingMath.TryProject(v, zero, 0f, 0f, 0f, out _, out _, out _, out _), "a zero projection projects nothing");
        Check(!DisplayXRPickingMath.TryUnprojectRay(v, p, float.NaN, 0f, out _, out _, out _, out _, out _, out _),
              "a NaN screen point gives no ray");
    }

    static void TestGraphicsApiIndependent()
    {
        // The projection handed to the GPU differs per graphics API in its z row only
        // (reversed-Z on D3D / Vulkan / Metal, unchanged on OpenGL). Where a point lands on
        // the screen is the same through all of them, so picking needs no per-API case.
        var rig = MakeRig(0.4f, 0f, 1f, 15f, 0f, 0.4f, 0.3f);
        EyeMatrices(rig, 0.12f, 0.05f, 0.5f, out float[] v, out float[] p);
        RigToUnity(rig, -0.1f, 0.08f, -0.7f, out float wx, out float wy, out float wz);
        DisplayXRPickingMath.TryProject(v, p, wx, wy, wz, out float gx, out float gy, out _, out float gd);
        foreach (bool rev in new[] { true, false })
        {
            var g = DisplayXRCursorDepthMath.ToGpuProjection(p, rev);
            bool ok = DisplayXRPickingMath.TryProject(v, g, wx, wy, wz, out float x, out float y, out float z, out float d);
            Check(ok && x == gx && y == gy && d == gd, $"reversedZ={rev}: same screen position and depth as the GL projection");
            if (rev) Check(z > 0f && z < 1f, "reversed-Z: depth lands in [0, 1]");
        }
    }

    static void TestPixelSpaces()
    {
        // Unity screen (bottom-left) -> viewport UV (top-left), whole screen.
        Check(DisplayXRPickingMath.ScreenToViewportUV(0f, 1080f, 1920f, 1080f, 0f, 0f, false, 0, 0, 0, 0, out float u, out float v)
              && Near(u, 0f) && Near(v, 0f), "top-left of the screen -> UV (0, 0)");
        DisplayXRPickingMath.ScreenToViewportUV(1920f, 0f, 1920f, 1080f, 0f, 0f, false, 0, 0, 0, 0, out u, out v);
        Check(Near(u, 1f) && Near(v, 1f), "bottom-right of the screen -> UV (1, 1)");
        DisplayXRPickingMath.ScreenToViewportUV(480f, 810f, 1920f, 1080f, 0f, 0f, false, 0, 0, 0, 0, out u, out v);
        Check(Near(u, 0.25f) && Near(v, 0.25f), "screen (480, 810) of 1920x1080 -> UV (0.25, 0.25)");

        // A 3D zone: the rect is client pixels from the TOP-left; the screen point is from the bottom.
        // Zone (100, 200) 800x400 in a 1920x1080 window; its centre is client (500, 400) = screen (500, 680).
        DisplayXRPickingMath.ScreenToViewportUV(500f, 680f, 1920f, 1080f, 0f, 0f, true, 100f, 200f, 800f, 400f, out u, out v);
        Check(Near(u, 0.5f) && Near(v, 0.5f), $"zone centre -> UV (0.5, 0.5) (got {u}, {v})");
        DisplayXRPickingMath.ScreenToViewportUV(100f, 880f, 1920f, 1080f, 0f, 0f, true, 100f, 200f, 800f, 400f, out u, out v);
        Check(Near(u, 0f) && Near(v, 0f), "zone top-left -> UV (0, 0)");
        // A point outside the zone still maps (outside [0, 1]); only degenerate sizes fail.
        Check(DisplayXRPickingMath.ScreenToViewportUV(0f, 0f, 1920f, 1080f, 0f, 0f, true, 100f, 200f, 800f, 400f, out u, out v)
              && u < 0f && v > 1f, "a point outside the zone maps outside [0, 1]");
        Check(!DisplayXRPickingMath.ScreenToViewportUV(0f, 0f, 0f, 1080f, 0f, 0f, false, 0, 0, 0, 0, out _, out _), "zero screen width fails");
        Check(!DisplayXRPickingMath.ScreenToViewportUV(0f, 0f, 1920f, 1080f, 0f, 0f, true, 0f, 0f, 0f, 400f, out _, out _), "zero zone width fails");

        // Logical screen pixels (a 200 % output): the screen is 960x540 logical, the window
        // 1920x1080 device pixels, and the zone rect is in device pixels.
        DisplayXRPickingMath.ScreenToViewportUV(250f, 340f, 960f, 540f, 1920f, 1080f, true, 100f, 200f, 800f, 400f, out u, out v);
        Check(Near(u, 0.5f) && Near(v, 0.5f), $"logical screen pixels scale to the zone's client pixels (got {u}, {v})");
        // Without a zone the scale cancels, so the client size is not needed.
        DisplayXRPickingMath.ScreenToViewportUV(240f, 405f, 960f, 540f, 1920f, 1080f, false, 0, 0, 0, 0, out u, out v);
        Check(Near(u, 0.25f) && Near(v, 0.25f), "whole screen: logical pixels need no scale");

        // Inverse, all three shapes.
        float[][] cases =
        {
            new[] { 1920f, 1080f, 0f, 0f, 0f, 0f, 0f, 0f, 0f },
            new[] { 1920f, 1080f, 0f, 0f, 1f, 100f, 200f, 800f, 400f },
            new[] { 960f, 540f, 1920f, 1080f, 1f, 100f, 200f, 800f, 400f },
        };
        foreach (var c in cases)
        {
            bool zone = c[4] != 0f;
            DisplayXRPickingMath.ScreenToViewportUV(333f, 222f, c[0], c[1], c[2], c[3], zone, c[5], c[6], c[7], c[8], out u, out v);
            bool ok = DisplayXRPickingMath.ViewportUVToScreen(u, v, c[0], c[1], c[2], c[3], zone, c[5], c[6], c[7], c[8], out float x, out float y);
            Check(ok && Near(x, 333f, 1e-2f) && Near(y, 222f, 1e-2f), $"screen -> UV -> screen (zone={zone}, client {c[2]}) (got {x}, {y})");
        }

        // UV <-> NDC, and client pixels -> NDC over a viewport rect.
        DisplayXRPickingMath.ViewportUVToNdc(0f, 0f, out float nx, out float ny);
        Check(Near(nx, -1f) && Near(ny, 1f), "UV (0, 0) = NDC top-left (-1, 1)");
        DisplayXRPickingMath.NdcToViewportUV(1f, -1f, out u, out v);
        Check(Near(u, 1f) && Near(v, 1f), "NDC (1, -1) = UV bottom-right (1, 1)");
        DisplayXRPickingMath.ClientPixelToNdc(500f, 400f, 100f, 200f, 800f, 400f, out nx, out ny);
        Check(Near(nx, 0f) && Near(ny, 0f), "client pixel at the viewport centre -> NDC (0, 0)");
        DisplayXRPickingMath.ClientPixelToNdc(100f, 200f, 100f, 200f, 800f, 400f, out nx, out ny);
        Check(Near(nx, -1f) && Near(ny, 1f), "client pixel at the viewport's top-left -> NDC (-1, 1)");
        DisplayXRPickingMath.ClientPixelToNdc(0f, 0f, 0f, 0f, 0f, 0f, out nx, out ny);
        Check(Near(nx, -1f) && Near(ny, 1f), "a zero-size viewport counts as one pixel (no division by zero)");
    }

    static void TestScreenPixelEndToEnd()
    {
        // A mouse position in a zone, all the way to a ray and back: the 3D zone is the
        // right half of a 1920x1080 window, the views frame that zone.
        var rig = MakeRig(0, 0, 0, 0, 0, 0.3f, 0.3375f);
        EyeMatrices(rig, -0.03f, 0.02f, 0.6f, out float[] lv, out float[] lp);
        EyeMatrices(rig, +0.03f, 0.02f, 0.6f, out float[] rv, out float[] rp);
        var cv = new float[16]; var cp = new float[16];
        DisplayXRPickingMath.Cyclopean(lv, lp, rv, rp, cv, cp);
        const float zx = 960f, zy = 0f, zw = 960f, zh = 1080f;

        // A point 0.25 behind the display, a little right and up.
        RigToUnity(rig, 0.05f, 0.06f, -0.25f, out float wx, out float wy, out float wz);
        DisplayXRPickingMath.TryProject(cv, cp, wx, wy, wz, out float nx, out float ny, out _, out float depth);
        DisplayXRPickingMath.NdcToViewportUV(nx, ny, out float u, out float v);
        DisplayXRPickingMath.ViewportUVToScreen(u, v, 1920f, 1080f, 0f, 0f, true, zx, zy, zw, zh, out float sx, out float sy);
        Check(sx > 960f && sx < 1920f && sy > 540f && sy < 1080f, $"the point shows in the zone's upper half (screen {sx}, {sy})");

        DisplayXRPickingMath.ScreenToViewportUV(sx, sy, 1920f, 1080f, 0f, 0f, true, zx, zy, zw, zh, out u, out v);
        DisplayXRPickingMath.ViewportUVToNdc(u, v, out nx, out ny);
        bool ok = DisplayXRPickingMath.TryUnprojectRay(cv, cp, nx, ny, out float ox, out float oy, out float oz,
                                                       out float dx, out float dy, out float dz);
        Check(ok && DistToRay(ox, oy, oz, dx, dy, dz, wx, wy, wz, out _) < 2e-4f, "the ray from that mouse position passes through the point");

        // Mapped against the whole window instead of the zone, the same pixel points somewhere else.
        DisplayXRPickingMath.ScreenToViewportUV(sx, sy, 1920f, 1080f, 0f, 0f, false, 0, 0, 0, 0, out u, out v);
        DisplayXRPickingMath.ViewportUVToNdc(u, v, out nx, out ny);
        DisplayXRPickingMath.TryUnprojectRay(cv, cp, nx, ny, out ox, out oy, out oz, out dx, out dy, out dz);
        Check(DistToRay(ox, oy, oz, dx, dy, dz, wx, wy, wz, out _) > 0.05f, "... and ignoring the zone rect misses it");
        Check(depth > 0f, "depth in front of the eye");
    }

    static int Main()
    {
        TestKnownVectors();
        TestRoundTrip();
        TestDisplayPlaneAndDisparity();
        TestCyclopeanIsTheMidpointEye();
        TestCyclopeanWithATurnedHead();
        TestFrameFromPoses();
        TestBehindTheEye();
        TestDegenerate();
        TestGraphicsApiIndependent();
        TestPixelSpaces();
        TestScreenPixelEndToEnd();
        Console.WriteLine($"picking_math (C#): {s_pass} passed, {s_fail} failed");
        return s_fail == 0 ? 0 : 1;
    }
}
