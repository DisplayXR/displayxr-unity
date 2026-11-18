// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

using UnityEngine;

namespace DisplayXR.Samples
{
    /// <summary>
    /// A generated stereo test picture, so the sample ships no media file.
    ///
    /// The LEFT eye picture has a red background and a big "L", the RIGHT eye picture a
    /// blue background and a big "R" - if each eye sees the wrong letter, the layout is
    /// swapped. Three shapes sit at different depths; the only difference between the two
    /// pictures (besides the tint and the letter) is where each shape is horizontally:
    /// <list type="bullet">
    /// <item>yellow circle - behind the screen (further right in the right picture);</item>
    /// <item>white square, the letter and the grid - on the screen (same place in both);</item>
    /// <item>green triangle - in front of the screen (further left in the right picture).</item>
    /// </list>
    /// </summary>
    public static class StereoMediaTestPattern
    {
        /// <summary>Horizontal offset between the two pictures of the far / near shape, as a
        /// fraction of the picture width.</summary>
        public const float Parallax = 0.02f;

        static readonly Color32 k_LeftDark = new Color32(70, 14, 18, 255);
        static readonly Color32 k_LeftLight = new Color32(150, 36, 40, 255);
        static readonly Color32 k_RightDark = new Color32(14, 26, 78, 255);
        static readonly Color32 k_RightLight = new Color32(36, 70, 160, 255);
        static readonly Color32 k_Grid = new Color32(255, 255, 255, 255);
        static readonly Color32 k_Letter = new Color32(245, 245, 245, 255);
        static readonly Color32 k_Circle = new Color32(250, 205, 40, 255);
        static readonly Color32 k_Square = new Color32(240, 240, 240, 255);
        static readonly Color32 k_Triangle = new Color32(60, 215, 110, 255);

        /// <summary>
        /// Build the picture. <paramref name="layout"/> decides the packing: side-by-side
        /// gives a (2 * eyeWidth) x eyeHeight texture, top-bottom eyeWidth x (2 * eyeHeight),
        /// Mono the left picture alone. The swapped layouts put the right picture first.
        /// The texture holds sRGB-encoded colour and is created as such.
        /// </summary>
        public static Texture2D Create(int eyeWidth = 960, int eyeHeight = 540,
                                       DisplayXRStereoLayout layout = DisplayXRStereoLayout.SideBySide)
        {
            eyeWidth = Mathf.Clamp(eyeWidth, 64, 4096);
            eyeHeight = Mathf.Clamp(eyeHeight, 64, 4096);
            Color32[] left = DrawEye(eyeWidth, eyeHeight, false);
            Color32[] right = DrawEye(eyeWidth, eyeHeight, true);

            int w = eyeWidth, h = eyeHeight;
            Color32[] packed;
            switch (layout)
            {
                case DisplayXRStereoLayout.SideBySide:
                case DisplayXRStereoLayout.SideBySideSwapped:
                {
                    bool swapped = layout == DisplayXRStereoLayout.SideBySideSwapped;
                    w = eyeWidth * 2;
                    packed = new Color32[w * h];
                    for (int y = 0; y < h; y++)
                    {
                        System.Array.Copy(swapped ? right : left, y * eyeWidth, packed, y * w, eyeWidth);
                        System.Array.Copy(swapped ? left : right, y * eyeWidth, packed, y * w + eyeWidth, eyeWidth);
                    }
                    break;
                }
                case DisplayXRStereoLayout.TopBottom:
                case DisplayXRStereoLayout.TopBottomSwapped:
                {
                    bool swapped = layout == DisplayXRStereoLayout.TopBottomSwapped;
                    h = eyeHeight * 2;
                    packed = new Color32[w * h];
                    // Row 0 is the BOTTOM of a Unity texture, so the top picture goes second.
                    System.Array.Copy(swapped ? left : right, 0, packed, 0, left.Length);
                    System.Array.Copy(swapped ? right : left, 0, packed, left.Length, left.Length);
                    break;
                }
                default:
                    packed = left;
                    break;
            }

            // linear: false -> an sRGB texture, which is what these 8-bit values are.
            var tex = new Texture2D(w, h, TextureFormat.RGBA32, false, false)
            {
                name = "StereoMediaTestPattern_" + layout,
                wrapMode = TextureWrapMode.Clamp,
                filterMode = FilterMode.Bilinear,
            };
            tex.SetPixels32(packed);
            tex.Apply(false, false);
            return tex;
        }

        static Color32[] DrawEye(int w, int h, bool rightEye)
        {
            var px = new Color32[w * h];
            Color32 dark = rightEye ? k_RightDark : k_LeftDark;
            Color32 light = rightEye ? k_RightLight : k_LeftLight;

            // Background: a vertical gradient in the eye's colour.
            for (int y = 0; y < h; y++)
            {
                Color32 c = Color32.Lerp(dark, light, (float)y / (h - 1));
                int row = y * w;
                for (int x = 0; x < w; x++) px[row + x] = c;
            }

            // Grid and frame, identical in both pictures: they sit on the screen plane.
            int line = Mathf.Max(1, h / 270);
            for (int i = 1; i < 8; i++) FillRect(px, w, h, i * w / 8 - line / 2, 0, line, h, Dim(k_Grid, dark, 0.18f));
            for (int i = 1; i < 4; i++) FillRect(px, w, h, 0, i * h / 4 - line / 2, w, line, Dim(k_Grid, dark, 0.18f));
            int frame = line * 3;
            FillRect(px, w, h, 0, 0, w, frame, k_Grid);
            FillRect(px, w, h, 0, h - frame, w, frame, k_Grid);
            FillRect(px, w, h, 0, 0, frame, h, k_Grid);
            FillRect(px, w, h, w - frame, 0, frame, h, k_Grid);

            // The letter, top-left, on the screen plane.
            int lh = h * 30 / 100, lw = lh * 62 / 100, stroke = Mathf.Max(2, lh / 6);
            int lx = w * 7 / 100, ly = h - h * 8 / 100 - lh;
            if (rightEye) DrawR(px, w, h, lx, ly, lw, lh, stroke);
            else DrawL(px, w, h, lx, ly, lw, lh, stroke);

            // Shapes. Screen parallax p = x_right - x_left: each picture moves its shape by
            // half of it, in opposite directions.
            float half = Parallax * w * 0.5f * (rightEye ? 1f : -1f);
            float r = h * 0.13f;
            float cy = h * 0.34f;
            FillCircle(px, w, h, w * 0.26f + half, cy, r, k_Circle);                 // p > 0: behind
            FillRect(px, w, h, Mathf.RoundToInt(w * 0.52f - r), Mathf.RoundToInt(cy - r),
                     Mathf.RoundToInt(2f * r), Mathf.RoundToInt(2f * r), k_Square);  // p = 0: on the screen
            float tx = w * 0.80f - half;                                             // p < 0: in front
            FillTriangle(px, w, h, tx - r * 1.15f, cy - r, tx + r * 1.15f, cy - r, tx, cy + r, k_Triangle);
            return px;
        }

        static Color32 Dim(Color32 c, Color32 background, float amount) => Color32.Lerp(background, c, amount);

        static void DrawL(Color32[] px, int w, int h, int x, int y, int lw, int lh, int s)
        {
            FillRect(px, w, h, x, y, s, lh, k_Letter);
            FillRect(px, w, h, x, y, lw, s, k_Letter);
        }

        static void DrawR(Color32[] px, int w, int h, int x, int y, int lw, int lh, int s)
        {
            int mid = y + lh / 2 - s / 2;
            FillRect(px, w, h, x, y, s, lh, k_Letter);                         // stem
            FillRect(px, w, h, x, y + lh - s, lw, s, k_Letter);                // top of the bowl
            FillRect(px, w, h, x, mid, lw, s, k_Letter);                       // bottom of the bowl
            FillRect(px, w, h, x + lw - s, mid, s, y + lh - mid, k_Letter);    // side of the bowl
            // The leg, from the bowl down to the baseline.
            float x0 = x + s, x1 = x + lw - s;
            FillQuad(px, w, h, x0, mid, x0 + s * 1.3f, mid, x1 + s, y, x1 - s * 0.3f, y, k_Letter);
        }

        static void FillRect(Color32[] px, int w, int h, int x, int y, int rw, int rh, Color32 c)
        {
            int x0 = Mathf.Max(0, x), x1 = Mathf.Min(w, x + rw);
            int y0 = Mathf.Max(0, y), y1 = Mathf.Min(h, y + rh);
            for (int yy = y0; yy < y1; yy++)
            {
                int row = yy * w;
                for (int xx = x0; xx < x1; xx++) px[row + xx] = c;
            }
        }

        static void FillCircle(Color32[] px, int w, int h, float cx, float cy, float r, Color32 c)
        {
            int x0 = Mathf.Max(0, Mathf.FloorToInt(cx - r)), x1 = Mathf.Min(w - 1, Mathf.CeilToInt(cx + r));
            int y0 = Mathf.Max(0, Mathf.FloorToInt(cy - r)), y1 = Mathf.Min(h - 1, Mathf.CeilToInt(cy + r));
            float r2 = r * r;
            for (int y = y0; y <= y1; y++)
                for (int x = x0; x <= x1; x++)
                {
                    float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
                    if (dx * dx + dy * dy <= r2) px[y * w + x] = c;
                }
        }

        static void FillTriangle(Color32[] px, int w, int h, float ax, float ay, float bx, float by,
                                 float cx, float cy, Color32 c)
        {
            int x0 = Mathf.Max(0, Mathf.FloorToInt(Mathf.Min(ax, Mathf.Min(bx, cx))));
            int x1 = Mathf.Min(w - 1, Mathf.CeilToInt(Mathf.Max(ax, Mathf.Max(bx, cx))));
            int y0 = Mathf.Max(0, Mathf.FloorToInt(Mathf.Min(ay, Mathf.Min(by, cy))));
            int y1 = Mathf.Min(h - 1, Mathf.CeilToInt(Mathf.Max(ay, Mathf.Max(by, cy))));
            for (int y = y0; y <= y1; y++)
                for (int x = x0; x <= x1; x++)
                {
                    float px0 = x + 0.5f, py0 = y + 0.5f;
                    float d0 = Edge(ax, ay, bx, by, px0, py0);
                    float d1 = Edge(bx, by, cx, cy, px0, py0);
                    float d2 = Edge(cx, cy, ax, ay, px0, py0);
                    bool neg = d0 < 0f || d1 < 0f || d2 < 0f;
                    bool pos = d0 > 0f || d1 > 0f || d2 > 0f;
                    if (!(neg && pos)) px[y * w + x] = c;
                }
        }

        static void FillQuad(Color32[] px, int w, int h, float ax, float ay, float bx, float by,
                             float cx, float cy, float dx, float dy, Color32 c)
        {
            FillTriangle(px, w, h, ax, ay, bx, by, cx, cy, c);
            FillTriangle(px, w, h, ax, ay, cx, cy, dx, dy, c);
        }

        static float Edge(float ax, float ay, float bx, float by, float x, float y) =>
            (bx - ax) * (y - ay) - (by - ay) * (x - ax);
    }
}
