// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

using System;

namespace DisplayXR
{
    /// <summary>
    /// Pure math for the depth-aware cursor's <b>Depth</b> source: where to sample Unity's
    /// depth texture around the cursor, which sample is the nearest content, and how to turn
    /// that raw depth back into a 3D point. No UnityEngine types on purpose - it is unit-tested
    /// outside Unity (<c>Tests~/CursorDepthMath</c>, run in CI).
    ///
    /// <para>Conventions.</para>
    /// <list type="bullet">
    /// <item>Matrices are float[16], COLUMN-major (element (row r, col c) at [c*4 + r]), the
    /// layout of OpenXR, OpenGL and Unity's Matrix4x4 indexer.</item>
    /// <item>The projection is the GL-style one the provider hands Unity
    /// (<c>dxr_prov_build_projection</c>): right-handed view space, -Z forward, clip z in
    /// [-w, w]. Unity's view space has the same handedness, so a view-space point is the same
    /// in OpenXR and in Unity.</item>
    /// <item>Raw depth is what the depth texture stores. On reversed-Z platforms (D3D11/12,
    /// Metal, Vulkan, consoles: <c>SystemInfo.usesReversedZBuffer</c>) near = 1 and far = 0;
    /// on OpenGL near = 0 and far = 1. This is exactly the z remap
    /// <c>GL.GetGPUProjectionMatrix</c> applies (reversed: z' = (w - z)/2; OpenGL: unchanged,
    /// with raw = (z_ndc + 1)/2), so unprojecting through it and through the GL projection with
    /// the inverse remap is the same thing - <see cref="ToGpuProjection"/> exists to prove that
    /// in the tests.</item>
    /// <item>Texture UV: origin BOTTOM-left (Unity's sampling convention for render textures:
    /// Unity flips the projection when rendering into a texture on top-left-origin APIs so that
    /// sampling with bottom-left UVs is upright everywhere). Canvas UV: origin TOP-left, v down
    /// (what <c>XrCursorDepthHintDXR::cursorUV</c> uses).</item>
    /// </list>
    /// </summary>
    public static class DisplayXRCursorDepthMath
    {
        /// <summary>Runtime default cursor height, fraction of the canvas height.</summary>
        public const float CursorHeight = 0.03f;

        /// <summary>The arrow body sits down-right of the tip: centre = tip + (0.3 h / aspect, 0.5 h).</summary>
        public const float BodyOffsetX = 0.3f, BodyOffsetY = 0.5f;

        /// <summary>Side of the square patch, in cursor heights. The footprint is a ring of
        /// radius 0.75 h around the body; the tip is 0.58 h from it. 1.6 h covers both.</summary>
        public const float PatchSideInHeights = 1.6f;

        /// <summary>Raw depths this close to the far value count as "nothing drawn" (cleared).</summary>
        public const float BackgroundEpsilon = 1e-6f;

        /// <summary>
        /// The patch to sample, in BOTTOM-left texture UV of the eye's VIEWPORT ([0,1] over the
        /// rendered area): x, y = lower-left corner, w, h = size. The patch is centred on the
        /// cursor body (canvas UV <paramref name="u"/>, <paramref name="vDown"/> = the hotspot)
        /// and is square on the screen.
        /// </summary>
        public static void PatchRect(float u, float vDown, float canvasAspect,
                                     out float x, out float y, out float w, out float h)
        {
            if (!(canvasAspect > 0f) || float.IsInfinity(canvasAspect)) canvasAspect = 1f;
            float cu = u + BodyOffsetX * CursorHeight / canvasAspect;
            float cv = vDown + BodyOffsetY * CursorHeight;
            h = PatchSideInHeights * CursorHeight;
            w = h / canvasAspect;
            x = cu - 0.5f * w;
            y = (1f - cv) - 0.5f * h; // top-left v down -> bottom-left v up
        }

        /// <summary>True when the raw depth is the cleared far value (no geometry there).</summary>
        public static bool IsBackground(float raw, bool reversedZ)
        {
            if (float.IsNaN(raw)) return true;
            return reversedZ ? raw <= BackgroundEpsilon : raw >= 1f - BackgroundEpsilon;
        }

        /// <summary>
        /// The texel nearest the viewer among <paramref name="count"/> raw depths (one per texel,
        /// any order): the LARGEST raw value on reversed-Z, the SMALLEST on OpenGL. Background
        /// texels are skipped. Returns -1 when every texel is background (nothing under the
        /// cursor: the cursor belongs on the glass).
        /// </summary>
        public static int NearestTexel(float[] raw, int count, bool reversedZ)
        {
            if (raw == null) return -1;
            if (count > raw.Length) count = raw.Length;
            int best = -1;
            float bestRaw = 0f;
            for (int i = 0; i < count; i++)
            {
                float d = raw[i];
                if (IsBackground(d, reversedZ) || float.IsInfinity(d)) continue;
                if (best < 0 || (reversedZ ? d > bestRaw : d < bestRaw))
                {
                    best = i;
                    bestRaw = d;
                }
            }
            return best;
        }

        /// <summary>Raw depth -> GL NDC z in [-1, 1] (-1 = near).</summary>
        public static float NdcZFromRaw(float raw, bool reversedZ)
        {
            return reversedZ ? 1f - 2f * raw : 2f * raw - 1f;
        }

        /// <summary>
        /// The z remap of <c>GL.GetGPUProjectionMatrix(proj, false)</c>: on reversed-Z, row 2
        /// becomes (row3 - row2) / 2, so z/w lands in [0, 1] with near = 1. Unchanged on OpenGL.
        /// </summary>
        public static float[] ToGpuProjection(float[] proj, bool reversedZ)
        {
            var g = (float[])proj.Clone();
            if (!reversedZ) return g;
            for (int c = 0; c < 4; c++)
                g[c * 4 + 2] = 0.5f * proj[c * 4 + 3] - 0.5f * proj[c * 4 + 2];
            return g;
        }

        /// <summary>
        /// Unproject a depth sample to a VIEW-space point (right-handed, -Z forward).
        /// <paramref name="u"/>, <paramref name="vUp"/>: bottom-left UV over the eye's viewport.
        /// Returns false for background or a degenerate projection.
        /// </summary>
        public static bool UnprojectToView(float[] proj, float u, float vUp, float raw, bool reversedZ,
                                           out float x, out float y, out float z)
        {
            x = y = z = 0f;
            if (proj == null || proj.Length < 16 || IsBackground(raw, reversedZ)) return false;
            if (!Invert(proj, out float[] inv)) return false;
            float nx = 2f * u - 1f, ny = 2f * vUp - 1f, nz = NdcZFromRaw(raw, reversedZ);
            float px = inv[0] * nx + inv[4] * ny + inv[8] * nz + inv[12];
            float py = inv[1] * nx + inv[5] * ny + inv[9] * nz + inv[13];
            float pz = inv[2] * nx + inv[6] * ny + inv[10] * nz + inv[14];
            float pw = inv[3] * nx + inv[7] * ny + inv[11] * nz + inv[15];
            if (!(Math.Abs(pw) > 1e-12f)) return false;
            x = px / pw; y = py / pw; z = pz / pw;
            return !(float.IsNaN(x) || float.IsNaN(y) || float.IsNaN(z));
        }

        /// <summary>View-space point -> locate space through the eye pose (rotate, then translate).</summary>
        public static void ViewToSpace(float[] eyePos, float[] eyeQuat, float x, float y, float z,
                                       out float wx, out float wy, out float wz)
        {
            Rotate(eyeQuat, x, y, z, out wx, out wy, out wz);
            wx += eyePos[0]; wy += eyePos[1]; wz += eyePos[2];
        }

        /// <summary>
        /// Texel index (row-major, row 0 = BOTTOM, the layout Unity's readback returns) of a
        /// <paramref name="size"/> x <paramref name="size"/> patch -> bottom-left viewport UV of
        /// the texel centre.
        /// </summary>
        public static void TexelToViewportUV(int index, int size, float px, float py, float pw, float ph,
                                             out float u, out float vUp)
        {
            int col = index % size, row = index / size;
            u = px + (col + 0.5f) / size * pw;
            vUp = py + (row + 0.5f) / size * ph;
        }

        /// <summary>Rotate (x, y, z) by unit quaternion q = (x, y, z, w).</summary>
        public static void Rotate(float[] q, float x, float y, float z, out float ox, out float oy, out float oz)
        {
            float tx = 2f * (q[1] * z - q[2] * y);
            float ty = 2f * (q[2] * x - q[0] * z);
            float tz = 2f * (q[0] * y - q[1] * x);
            ox = x + q[3] * tx + (q[1] * tz - q[2] * ty);
            oy = y + q[3] * ty + (q[2] * tx - q[0] * tz);
            oz = z + q[3] * tz + (q[0] * ty - q[1] * tx);
        }

        /// <summary>General 4x4 inverse (column-major). False when singular.</summary>
        public static bool Invert(float[] m, out float[] inv)
        {
            inv = new float[16];
            double[] a = new double[16];
            for (int i = 0; i < 16; i++) a[i] = m[i];
            double[] r = new double[16];
            r[0] = a[5] * a[10] * a[15] - a[5] * a[11] * a[14] - a[9] * a[6] * a[15] + a[9] * a[7] * a[14] + a[13] * a[6] * a[11] - a[13] * a[7] * a[10];
            r[4] = -a[4] * a[10] * a[15] + a[4] * a[11] * a[14] + a[8] * a[6] * a[15] - a[8] * a[7] * a[14] - a[12] * a[6] * a[11] + a[12] * a[7] * a[10];
            r[8] = a[4] * a[9] * a[15] - a[4] * a[11] * a[13] - a[8] * a[5] * a[15] + a[8] * a[7] * a[13] + a[12] * a[5] * a[11] - a[12] * a[7] * a[9];
            r[12] = -a[4] * a[9] * a[14] + a[4] * a[10] * a[13] + a[8] * a[5] * a[14] - a[8] * a[6] * a[13] - a[12] * a[5] * a[10] + a[12] * a[6] * a[9];
            r[1] = -a[1] * a[10] * a[15] + a[1] * a[11] * a[14] + a[9] * a[2] * a[15] - a[9] * a[3] * a[14] - a[13] * a[2] * a[11] + a[13] * a[3] * a[10];
            r[5] = a[0] * a[10] * a[15] - a[0] * a[11] * a[14] - a[8] * a[2] * a[15] + a[8] * a[3] * a[14] + a[12] * a[2] * a[11] - a[12] * a[3] * a[10];
            r[9] = -a[0] * a[9] * a[15] + a[0] * a[11] * a[13] + a[8] * a[1] * a[15] - a[8] * a[3] * a[13] - a[12] * a[1] * a[11] + a[12] * a[3] * a[9];
            r[13] = a[0] * a[9] * a[14] - a[0] * a[10] * a[13] - a[8] * a[1] * a[14] + a[8] * a[2] * a[13] + a[12] * a[1] * a[10] - a[12] * a[2] * a[9];
            r[2] = a[1] * a[6] * a[15] - a[1] * a[7] * a[14] - a[5] * a[2] * a[15] + a[5] * a[3] * a[14] + a[13] * a[2] * a[7] - a[13] * a[3] * a[6];
            r[6] = -a[0] * a[6] * a[15] + a[0] * a[7] * a[14] + a[4] * a[2] * a[15] - a[4] * a[3] * a[14] - a[12] * a[2] * a[7] + a[12] * a[3] * a[6];
            r[10] = a[0] * a[5] * a[15] - a[0] * a[7] * a[13] - a[4] * a[1] * a[15] + a[4] * a[3] * a[13] + a[12] * a[1] * a[7] - a[12] * a[3] * a[5];
            r[14] = -a[0] * a[5] * a[14] + a[0] * a[6] * a[13] + a[4] * a[1] * a[14] - a[4] * a[2] * a[13] - a[12] * a[1] * a[6] + a[12] * a[2] * a[5];
            r[3] = -a[1] * a[6] * a[11] + a[1] * a[7] * a[10] + a[5] * a[2] * a[11] - a[5] * a[3] * a[10] - a[9] * a[2] * a[7] + a[9] * a[3] * a[6];
            r[7] = a[0] * a[6] * a[11] - a[0] * a[7] * a[10] - a[4] * a[2] * a[11] + a[4] * a[3] * a[10] + a[8] * a[2] * a[7] - a[8] * a[3] * a[6];
            r[11] = -a[0] * a[5] * a[11] + a[0] * a[7] * a[9] + a[4] * a[1] * a[11] - a[4] * a[3] * a[9] - a[8] * a[1] * a[7] + a[8] * a[3] * a[5];
            r[15] = a[0] * a[5] * a[10] - a[0] * a[6] * a[9] - a[4] * a[1] * a[10] + a[4] * a[2] * a[9] + a[8] * a[1] * a[6] - a[8] * a[2] * a[5];
            double det = a[0] * r[0] + a[1] * r[4] + a[2] * r[8] + a[3] * r[12];
            if (Math.Abs(det) < 1e-30) return false;
            for (int i = 0; i < 16; i++) inv[i] = (float)(r[i] / det);
            return true;
        }

        /// <summary>The GL-style off-axis projection the provider builds from an OpenXR fov
        /// (tangent extents at the near plane), column-major. Mirrors ps_projection_from_fov.</summary>
        public static float[] ProjectionFromFov(float angleLeft, float angleRight, float angleUp, float angleDown,
                                                float near, float far)
        {
            float l = (float)Math.Tan(angleLeft) * near, r = (float)Math.Tan(angleRight) * near;
            float t = (float)Math.Tan(angleUp) * near, b = (float)Math.Tan(angleDown) * near;
            var m = new float[16];
            m[0] = 2f * near / (r - l);
            m[5] = 2f * near / (t - b);
            m[8] = (r + l) / (r - l);
            m[9] = (t + b) / (t - b);
            m[10] = -(far + near) / (far - near);
            m[11] = -1f;
            m[14] = -2f * far * near / (far - near);
            return m;
        }

        // ---- Custom cursor sprite (#369) ------------------------------------------------

        /// <summary>
        /// A sprite hotspot in texture PIXELS from the TOP-left corner (Unity's
        /// <c>Cursor.SetCursor</c> convention) -> what <c>XrCursorDepthDrawInfoDXR::hotSpot</c>
        /// wants: normalised [0,1]^2 over the sprite, origin top-left, v down. Both conventions
        /// are top-left/y-down, so this is a divide with no flip; the result is clamped onto the
        /// sprite. Non-finite components map to 0; a degenerate size gives (0, 0).
        /// Mirrors <c>dxr_cursor::sprite_hotspot_uv</c> (native).
        /// </summary>
        public static void SpriteHotspotUV(float hotspotX, float hotspotY, float width, float height,
                                           out float u, out float vDown)
        {
            u = 0f;
            vDown = 0f;
            if (!(width > 0f) || !(height > 0f)) return;
            u = IsFinite(hotspotX) ? hotspotX / width : 0f;
            vDown = IsFinite(hotspotY) ? hotspotY / height : 0f;
            u = u < 0f ? 0f : (u > 1f ? 1f : u);
            vDown = vDown < 0f ? 0f : (vDown > 1f ? 1f : vDown);
        }

        /// <summary>The largest sprite edge the provider makes a swapchain for (native
        /// <c>dxr_cursor::kSpriteMaxEdge</c>).</summary>
        public const int SpriteMaxEdge = 1024;

        /// <summary>True when a sprite of this size is accepted (1..<see cref="SpriteMaxEdge"/> each way).</summary>
        public static bool SpriteSizeOk(int width, int height) =>
            width > 0 && height > 0 && width <= SpriteMaxEdge && height <= SpriteMaxEdge;

        static bool IsFinite(float f) => !float.IsNaN(f) && !float.IsInfinity(f);
    }
}
