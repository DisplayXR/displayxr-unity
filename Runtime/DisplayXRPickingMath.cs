// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

using System;

namespace DisplayXR
{
    /// <summary>
    /// Pure math for stereo-correct picking (<see cref="DisplayXRPicking"/>): a screen point
    /// to a ray through the view the viewer actually sees, and a world point back to the
    /// screen. It consumes the per-eye view + projection matrices the provider already
    /// publishes - it builds no projection of its own (the runtime owns that math). No
    /// UnityEngine types on purpose: it is unit-tested outside Unity
    /// (<c>Tests~/PickingMath</c>, run in CI), and nothing here allocates.
    ///
    /// <para>Conventions.</para>
    /// <list type="bullet">
    /// <item>Matrices are float[16], COLUMN-major (element (row r, col c) at [c*4 + r]), the
    /// layout of OpenXR, OpenGL and Unity's Matrix4x4 indexer.</item>
    /// <item>The VIEW matrix is the provider's: it maps the OpenXR locate space (right-handed,
    /// -Z forward) to eye space. Points and rays that go in and out of this class are in the
    /// Unity-convention twin of that space (left-handed, +Z forward: the same space with Z
    /// negated), which IS Unity world space while a DisplayXR rig pushes its pose. The Z
    /// negation is applied here, so callers pass and get plain Unity coordinates.</item>
    /// <item>The PROJECTION is the GL-style one the provider hands Unity
    /// (<c>dxr_prov_build_projection</c>): eye space -Z forward, clip z in [-w, w]. Only its
    /// x and y rows decide where a point lands on the screen, and those are the same in every
    /// graphics API (the D3D / Vulkan / Metal remap that <c>GL.GetGPUProjectionMatrix</c>
    /// applies touches the z row only), so picking has no reversed-Z case.</item>
    /// <item>NDC: x, y in [-1, 1], +Y up, over the VIEWPORT the projection frames (the window,
    /// or a 3D zone rect). Viewport UV: [0, 1], origin TOP-left, v down. Unity screen pixels:
    /// origin BOTTOM-left (<c>Input.mousePosition</c>). Client pixels: origin TOP-left.</item>
    /// </list>
    /// </summary>
    public static class DisplayXRPickingMath
    {
        /// <summary>A point this close to the eye plane (or behind it) cannot be projected.
        /// Clip-space w, i.e. distance along the view axis in world units.</summary>
        public const float MinProjectDepth = 0.0001f;

        // ---- Pixel spaces ---------------------------------------------------------------

        /// <summary>
        /// Unity screen pixels (origin bottom-left) -> viewport UV (origin top-left, v down).
        /// The same rule the depth-aware cursor uses (<c>dxr_cursor::screen_to_canvas_uv</c>).
        /// </summary>
        /// <param name="clientW">Size of the window's client area in the pixels the zone rect
        /// is authored in. Pass 0 (or the screen size) when they are the same unit as the
        /// screen; they differ only where Unity's screen is in logical pixels (a scaled
        /// native-Wayland output).</param>
        /// <param name="zoneValid">True when the views frame an app-authored 3D zone: the
        /// viewport is then that zone's rect (client pixels, top-left origin) and not the
        /// whole screen.</param>
        /// <returns>False when a size is degenerate. The UV may lie outside [0, 1]: the point
        /// is then outside the viewport, and still maps to a valid ray.</returns>
        public static bool ScreenToViewportUV(float x, float yUp, float screenW, float screenH,
                                              float clientW, float clientH,
                                              bool zoneValid, float zoneX, float zoneY, float zoneW, float zoneH,
                                              out float u, out float vDown)
        {
            u = vDown = 0f;
            if (!(screenW > 0f) || !(screenH > 0f)) return false;
            float px = x, py = screenH - yUp; // -> top-left origin
            if (zoneValid)
            {
                if (!(zoneW > 0f) || !(zoneH > 0f)) return false;
                if (clientW > 0f && clientH > 0f)
                {
                    px *= clientW / screenW;
                    py *= clientH / screenH;
                }
                u = (px - zoneX) / zoneW;
                vDown = (py - zoneY) / zoneH;
            }
            else
            {
                u = px / screenW;
                vDown = py / screenH;
            }
            return IsFinite(u) && IsFinite(vDown);
        }

        /// <summary>The inverse of <see cref="ScreenToViewportUV"/>.</summary>
        public static bool ViewportUVToScreen(float u, float vDown, float screenW, float screenH,
                                              float clientW, float clientH,
                                              bool zoneValid, float zoneX, float zoneY, float zoneW, float zoneH,
                                              out float x, out float yUp)
        {
            x = yUp = 0f;
            if (!(screenW > 0f) || !(screenH > 0f)) return false;
            float px, py;
            if (zoneValid)
            {
                if (!(zoneW > 0f) || !(zoneH > 0f)) return false;
                px = zoneX + u * zoneW;
                py = zoneY + vDown * zoneH;
                if (clientW > 0f && clientH > 0f)
                {
                    px *= screenW / clientW;
                    py *= screenH / clientH;
                }
            }
            else
            {
                px = u * screenW;
                py = vDown * screenH;
            }
            x = px;
            yUp = screenH - py;
            return IsFinite(x) && IsFinite(yUp);
        }

        /// <summary>Viewport UV (top-left origin, v down) -> NDC (+Y up).</summary>
        public static void ViewportUVToNdc(float u, float vDown, out float ndcX, out float ndcY)
        {
            ndcX = 2f * u - 1f;
            ndcY = 1f - 2f * vDown;
        }

        /// <summary>NDC (+Y up) -> viewport UV (top-left origin, v down).</summary>
        public static void NdcToViewportUV(float ndcX, float ndcY, out float u, out float vDown)
        {
            u = (ndcX + 1f) * 0.5f;
            vDown = (1f - ndcY) * 0.5f;
        }

        /// <summary>
        /// Client pixels (top-left origin) -> NDC over a viewport rect given in the same
        /// pixels. A viewport side below one pixel counts as one.
        /// </summary>
        public static void ClientPixelToNdc(float px, float pyDown,
                                            float vpX, float vpY, float vpW, float vpH,
                                            out float ndcX, out float ndcY)
        {
            float sw = vpW > 1f ? vpW : 1f;
            float sh = vpH > 1f ? vpH : 1f;
            ndcX = 2f * (px - vpX) / sw - 1f;
            ndcY = 1f - 2f * (pyDown - vpY) / sh;
        }

        // ---- The cyclopean view -----------------------------------------------------------

        /// <summary>
        /// The view between the two eyes: where the viewer perceives a fused point, which is
        /// what a pointer is aimed with. The left view with its translation averaged with the
        /// right one's (both eyes of a DisplayXR view pair share an orientation), and the
        /// element-wise mean of the two projections. That is exactly the midpoint eye's own
        /// frustum, for any two eye positions (a turned or tilted head included): every term
        /// of an off-axis projection that frames one fixed rectangle is linear in the eye
        /// position, so the mean of two projections is the projection of the mean position.
        /// Writes into the caller's arrays (which may not alias the inputs).
        /// </summary>
        public static void Cyclopean(float[] leftView, float[] leftProj, float[] rightView, float[] rightProj,
                                     float[] outView, float[] outProj)
        {
            for (int i = 0; i < 16; i++)
            {
                outView[i] = leftView[i];
                outProj[i] = (leftProj[i] + rightProj[i]) * 0.5f;
            }
            outView[12] = (leftView[12] + rightView[12]) * 0.5f;
            outView[13] = (leftView[13] + rightView[13]) * 0.5f;
            outView[14] = (leftView[14] + rightView[14]) * 0.5f;
        }

        // ---- Where the views sit --------------------------------------------------------------

        /// <summary>
        /// The rigid frame the located views sit in for a camera, from that camera's pose and
        /// the rig pose the provider last sent the runtime (identity when none was sent):
        /// camera * rig^-1, i.e. rotation = Rc * Rr^-1 and position = pc - rotation * pr. The
        /// views come back with the rig pose baked in and Unity re-applies the camera's own
        /// transform to them rig-relative, so this is what turns a point of the located space
        /// into the world that camera draws. It is the identity when the two poses are equal.
        /// Quaternions are (x, y, z, w), unit; any consistent coordinate convention.
        /// </summary>
        public static void FrameFromPoses(
            float cpx, float cpy, float cpz, float cqx, float cqy, float cqz, float cqw,
            float rpx, float rpy, float rpz, float rqx, float rqy, float rqz, float rqw,
            out float px, out float py, out float pz,
            out float qx, out float qy, out float qz, out float qw)
        {
            // q = qc * conj(qr)
            double ax = cqx, ay = cqy, az = cqz, aw = cqw;
            double bx = -rqx, by = -rqy, bz = -rqz, bw = rqw;
            double x = aw * bx + ax * bw + ay * bz - az * by;
            double y = aw * by - ax * bz + ay * bw + az * bx;
            double z = aw * bz + ax * by - ay * bx + az * bw;
            double w = aw * bw - ax * bx - ay * by - az * bz;
            double n = Math.Sqrt(x * x + y * y + z * z + w * w);
            if (n > 1e-12) { x /= n; y /= n; z /= n; w /= n; }
            else { x = y = z = 0.0; w = 1.0; }
            // p = pc - q * pr
            double tx = 2.0 * (y * rpz - z * rpy);
            double ty = 2.0 * (z * rpx - x * rpz);
            double tz = 2.0 * (x * rpy - y * rpx);
            px = (float)(cpx - (rpx + w * tx + (y * tz - z * ty)));
            py = (float)(cpy - (rpy + w * ty + (z * tx - x * tz)));
            pz = (float)(cpz - (rpz + w * tz + (x * ty - y * tx)));
            qx = (float)x; qy = (float)y; qz = (float)z; qw = (float)w;
        }

        // ---- Screen -> world --------------------------------------------------------------

        /// <summary>
        /// The ray through NDC point (<paramref name="ndcX"/>, <paramref name="ndcY"/>) of one
        /// view. Origin: the point on the near plane; direction: unit, towards the far plane.
        /// Returns false for a degenerate view or projection.
        /// </summary>
        public static bool TryUnprojectRay(float[] view, float[] proj, float ndcX, float ndcY,
                                           out float ox, out float oy, out float oz,
                                           out float dx, out float dy, out float dz)
        {
            ox = oy = oz = 0f;
            dx = dy = 0f; dz = 1f;
            if (!TryNearFarEye(proj, ndcX, ndcY, out V3 n, out V3 f)) return false;
            if (!TryInverseView(view, out M4 vInv)) return false;
            if (!MulPoint(ref vInv, n, out V3 nw) || !MulPoint(ref vInv, f, out V3 fw)) return false;
            double ex = fw.x - nw.x, ey = fw.y - nw.y, ez = fw.z - nw.z;
            double len = Math.Sqrt(ex * ex + ey * ey + ez * ez);
            if (!(len >= 1e-4)) return false;
            ox = (float)nw.x; oy = (float)nw.y; oz = (float)nw.z;
            dx = (float)(ex / len); dy = (float)(ey / len); dz = (float)(ez / len);
            return IsFinite(ox) && IsFinite(oy) && IsFinite(oz) && IsFinite(dx) && IsFinite(dy) && IsFinite(dz);
        }

        /// <summary>
        /// The point seen at NDC (<paramref name="ndcX"/>, <paramref name="ndcY"/>) at
        /// <paramref name="depth"/> world units in front of the eye, measured along the view
        /// axis (the z <see cref="TryProject"/> reports), not along the ray.
        /// </summary>
        public static bool TryUnprojectPoint(float[] view, float[] proj, float ndcX, float ndcY, float depth,
                                             out float x, out float y, out float z)
        {
            x = y = z = 0f;
            if (!TryNearFarEye(proj, ndcX, ndcY, out V3 n, out V3 f)) return false;
            if (!TryInverseView(view, out M4 vInv)) return false;
            double dzEye = f.z - n.z;
            if (!(Math.Abs(dzEye) > 1e-12)) return false;
            double t = (-(double)depth - n.z) / dzEye; // eye space looks down -Z
            V3 p;
            p.x = n.x + t * (f.x - n.x);
            p.y = n.y + t * (f.y - n.y);
            p.z = n.z + t * dzEye;
            if (!MulPoint(ref vInv, p, out V3 w)) return false;
            x = (float)w.x; y = (float)w.y; z = (float)w.z;
            return IsFinite(x) && IsFinite(y) && IsFinite(z);
        }

        // ---- World -> screen --------------------------------------------------------------

        /// <summary>
        /// Project a point through one view. <paramref name="depth"/> is its signed distance
        /// in front of the eye along the view axis (negative: behind the viewer) and is always
        /// written. Returns false, with the NDC outputs zeroed, when the point is at or behind
        /// the eye plane: it has no position on the screen. <paramref name="ndcZ"/> is the GL
        /// depth in [-1, 1] between the near and far planes; outside that range the point is
        /// beyond a clip plane, which this method reports but does not reject.
        /// </summary>
        public static bool TryProject(float[] view, float[] proj, float wx, float wy, float wz,
                                      out float ndcX, out float ndcY, out float ndcZ, out float depth)
        {
            ndcX = ndcY = ndcZ = 0f;
            // Unity point -> locate space is z -> -z, the same thing as negating the view's Z column.
            double x = wx, y = wy, z = -(double)wz;
            double ex = view[0] * x + view[4] * y + view[8] * z + view[12];
            double ey = view[1] * x + view[5] * y + view[9] * z + view[13];
            double ez = view[2] * x + view[6] * y + view[10] * z + view[14];
            double ew = view[3] * x + view[7] * y + view[11] * z + view[15];
            depth = (float)(-ez);
            double cx = proj[0] * ex + proj[4] * ey + proj[8] * ez + proj[12] * ew;
            double cy = proj[1] * ex + proj[5] * ey + proj[9] * ez + proj[13] * ew;
            double cz = proj[2] * ex + proj[6] * ey + proj[10] * ez + proj[14] * ew;
            double cw = proj[3] * ex + proj[7] * ey + proj[11] * ez + proj[15] * ew;
            if (!(cw > MinProjectDepth)) return false;
            ndcX = (float)(cx / cw);
            ndcY = (float)(cy / cw);
            ndcZ = (float)(cz / cw);
            return IsFinite(ndcX) && IsFinite(ndcY) && IsFinite(ndcZ);
        }

        // ---- Internals --------------------------------------------------------------------

        struct V3 { public double x, y, z; }

        // aRC = row R, column C.
        struct M4
        {
            public double a00, a01, a02, a03, a10, a11, a12, a13, a20, a21, a22, a23, a30, a31, a32, a33;
        }

        static bool IsFinite(float v) { return !float.IsNaN(v) && !float.IsInfinity(v); }

        // The two points a screen position unprojects to, on the near and far planes, in eye space.
        static bool TryNearFarEye(float[] proj, float ndcX, float ndcY, out V3 near, out V3 far)
        {
            near = default(V3); far = default(V3);
            if (proj == null || proj.Length < 16) return false;
            Load(proj, false, out M4 p);
            if (!Invert(ref p, out M4 pInv)) return false;
            return MulNdc(ref pInv, ndcX, ndcY, -1.0, out near) && MulNdc(ref pInv, ndcX, ndcY, 1.0, out far);
        }

        // Unity-convention eye-from-world is the provider's view with its Z column negated;
        // this is its inverse (Unity world from eye space).
        static bool TryInverseView(float[] view, out M4 inv)
        {
            inv = default(M4);
            if (view == null || view.Length < 16) return false;
            Load(view, true, out M4 v);
            return Invert(ref v, out inv);
        }

        static void Load(float[] m, bool negateZColumn, out M4 a)
        {
            double s = negateZColumn ? -1.0 : 1.0;
            a.a00 = m[0]; a.a10 = m[1]; a.a20 = m[2]; a.a30 = m[3];
            a.a01 = m[4]; a.a11 = m[5]; a.a21 = m[6]; a.a31 = m[7];
            a.a02 = s * m[8]; a.a12 = s * m[9]; a.a22 = s * m[10]; a.a32 = s * m[11];
            a.a03 = m[12]; a.a13 = m[13]; a.a23 = m[14]; a.a33 = m[15];
        }

        static bool MulNdc(ref M4 m, double x, double y, double z, out V3 o)
        {
            o = default(V3);
            double w = m.a30 * x + m.a31 * y + m.a32 * z + m.a33;
            if (!(Math.Abs(w) >= 1e-6)) return false;
            o.x = (m.a00 * x + m.a01 * y + m.a02 * z + m.a03) / w;
            o.y = (m.a10 * x + m.a11 * y + m.a12 * z + m.a13) / w;
            o.z = (m.a20 * x + m.a21 * y + m.a22 * z + m.a23) / w;
            return true;
        }

        static bool MulPoint(ref M4 m, V3 p, out V3 o)
        {
            o = default(V3);
            double w = m.a30 * p.x + m.a31 * p.y + m.a32 * p.z + m.a33;
            if (!(Math.Abs(w) >= 1e-12)) return false;
            o.x = (m.a00 * p.x + m.a01 * p.y + m.a02 * p.z + m.a03) / w;
            o.y = (m.a10 * p.x + m.a11 * p.y + m.a12 * p.z + m.a13) / w;
            o.z = (m.a20 * p.x + m.a21 * p.y + m.a22 * p.z + m.a23) / w;
            return true;
        }

        // General 4x4 inverse by 2x2 sub-determinants, in doubles, on the stack.
        static bool Invert(ref M4 a, out M4 b)
        {
            b = default(M4);
            double s0 = a.a00 * a.a11 - a.a10 * a.a01;
            double s1 = a.a00 * a.a12 - a.a10 * a.a02;
            double s2 = a.a00 * a.a13 - a.a10 * a.a03;
            double s3 = a.a01 * a.a12 - a.a11 * a.a02;
            double s4 = a.a01 * a.a13 - a.a11 * a.a03;
            double s5 = a.a02 * a.a13 - a.a12 * a.a03;
            double c5 = a.a22 * a.a33 - a.a32 * a.a23;
            double c4 = a.a21 * a.a33 - a.a31 * a.a23;
            double c3 = a.a21 * a.a32 - a.a31 * a.a22;
            double c2 = a.a20 * a.a33 - a.a30 * a.a23;
            double c1 = a.a20 * a.a32 - a.a30 * a.a22;
            double c0 = a.a20 * a.a31 - a.a30 * a.a21;
            double det = s0 * c5 - s1 * c4 + s2 * c3 + s3 * c2 - s4 * c1 + s5 * c0;
            if (!(Math.Abs(det) >= 1e-30)) return false;
            double d = 1.0 / det;
            b.a00 = (a.a11 * c5 - a.a12 * c4 + a.a13 * c3) * d;
            b.a01 = (-a.a01 * c5 + a.a02 * c4 - a.a03 * c3) * d;
            b.a02 = (a.a31 * s5 - a.a32 * s4 + a.a33 * s3) * d;
            b.a03 = (-a.a21 * s5 + a.a22 * s4 - a.a23 * s3) * d;
            b.a10 = (-a.a10 * c5 + a.a12 * c2 - a.a13 * c1) * d;
            b.a11 = (a.a00 * c5 - a.a02 * c2 + a.a03 * c1) * d;
            b.a12 = (-a.a30 * s5 + a.a32 * s2 - a.a33 * s1) * d;
            b.a13 = (a.a20 * s5 - a.a22 * s2 + a.a23 * s1) * d;
            b.a20 = (a.a10 * c4 - a.a11 * c2 + a.a13 * c0) * d;
            b.a21 = (-a.a00 * c4 + a.a01 * c2 - a.a03 * c0) * d;
            b.a22 = (a.a30 * s4 - a.a31 * s2 + a.a33 * s0) * d;
            b.a23 = (-a.a20 * s4 + a.a21 * s2 - a.a23 * s0) * d;
            b.a30 = (-a.a10 * c3 + a.a11 * c1 - a.a12 * c0) * d;
            b.a31 = (a.a00 * c3 - a.a01 * c1 + a.a02 * c0) * d;
            b.a32 = (-a.a30 * s3 + a.a31 * s1 - a.a32 * s0) * d;
            b.a33 = (a.a20 * s3 - a.a21 * s1 + a.a22 * s0) * d;
            return true;
        }
    }
}
