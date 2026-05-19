// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0

#if UNITY_EDITOR
using UnityEngine;

namespace DisplayXR
{
    /// <summary>
    /// Shared Scene-view gizmo primitives for DisplayXRDisplay / DisplayXRCamera.
    /// Re-implements the canonical Kooima eye-factor math
    /// (native~/display3d_view.c, display3d_apply_eye_factors_n) in C# so the
    /// drawn frustum origins match what the runtime actually consumes in
    /// xrLocateViews. Supports N views (Standard 3D = 2, quad = 4,
    /// lenticular = 8, …) by reading the standalone preview's per-view
    /// state through displayxr_standalone_get_n_eye_positions and the
    /// active mode's viewCount through displayxr_standalone_get_current_mode_info.
    /// </summary>
    internal static class DisplayXRGizmoHelpers
    {
        public const int MAX_VIEWS = 16;
        public const float NOMINAL_IPD_METERS = 0.063f;
        public const float NOMINAL_VIEWER_Z_FALLBACK = 0.5f;

        // -----------------------------------------------------------------
        // Session detection (selection-driven in Edit Mode; active-rig-only
        // when a DisplayXR session is alive)
        // -----------------------------------------------------------------

        static System.Reflection.PropertyInfo s_PreviewIsRunningProp;
        static bool s_PreviewLookupDone;

        public static bool IsSessionActive()
        {
            if (Application.isPlaying) return true;
            return IsPreviewSessionRunning();
        }

        static bool IsPreviewSessionRunning()
        {
            if (!s_PreviewLookupDone)
            {
                s_PreviewLookupDone = true;
                // Editor asmdef references Runtime, not the other way around,
                // so DisplayXRPreviewSession (DisplayXR.Editor namespace) is
                // not visible at compile time. Reflect through loaded
                // assemblies — cached after first hit.
                try
                {
                    foreach (var asm in System.AppDomain.CurrentDomain.GetAssemblies())
                    {
                        var t = asm.GetType("DisplayXR.Editor.DisplayXRPreviewSession", false);
                        if (t == null) continue;
                        s_PreviewIsRunningProp = t.GetProperty("IsRunning",
                            System.Reflection.BindingFlags.Public |
                            System.Reflection.BindingFlags.Static);
                        break;
                    }
                }
                catch { /* swallow — fall back to "no session" */ }
            }
            if (s_PreviewIsRunningProp == null) return false;
            try { return (bool)s_PreviewIsRunningProp.GetValue(null); }
            catch { return false; }
        }

        // -----------------------------------------------------------------
        // Data source
        // -----------------------------------------------------------------

        public static DisplayXRDisplayInfo ReadDisplayInfoFromNative()
        {
            var info = new DisplayXRDisplayInfo();
            try
            {
                DisplayXRNative.displayxr_get_display_info(
                    out info.displayWidthMeters,
                    out info.displayHeightMeters,
                    out info.displayPixelWidth,
                    out info.displayPixelHeight,
                    out info.nominalViewerX,
                    out info.nominalViewerY,
                    out info.nominalViewerZ,
                    out info.recommendedViewScaleX,
                    out info.recommendedViewScaleY,
                    out int isValid);
                info.isValid = isValid != 0;
            }
            catch (System.DllNotFoundException)
            {
                info.isValid = false;
            }
            return info;
        }

        /// <summary>
        /// View count for the currently active rendering mode (Standard 3D = 2,
        /// quad = 4, lenticular = 8, …). Falls back to 2 when no preview
        /// session is running or the mode info is unavailable. Clamped to
        /// [1, MAX_VIEWS].
        /// </summary>
        public static int QueryActiveViewCount()
        {
            uint vc = 0;
            try
            {
                DisplayXRNative.displayxr_standalone_get_current_mode_info(
                    out vc, out _, out _, out _, out _, out _, out _, out _);
            }
            catch (System.DllNotFoundException) { }
            int n = vc > 0 ? (int)vc : 2;
            if (n < 1) n = 1;
            if (n > MAX_VIEWS) n = MAX_VIEWS;
            return n;
        }

        // Reusable scratch buffer for the N-eye native marshalling. Editor-
        // only, single-threaded gizmo callbacks, so a static instance is fine.
        static float[] s_NEyeBuf;

        /// <summary>
        /// Try to fill <paramref name="eyesOut"/> with the runtime's last
        /// per-view eye positions (OpenXR display-relative, right-hand,
        /// −Z forward). Standalone preview path returns up to N; Unity
        /// hook chain (Play Mode) returns 2. Returns the number written;
        /// 0 if no live data is available or the data is degenerate
        /// (all-zero, or L==R with N==2 — head-center-only sessions).
        /// </summary>
        public static int TryGetLiveRawEyes(Vector3[] eyesOut)
        {
            if (eyesOut == null || eyesOut.Length == 0) return 0;
            int cap = eyesOut.Length;

            // 1) Standalone preview (N-view): preferred when running.
            if (s_NEyeBuf == null || s_NEyeBuf.Length < cap * 3)
                s_NEyeBuf = new float[cap * 3];
            try
            {
                DisplayXRNative.displayxr_standalone_get_n_eye_positions(
                    s_NEyeBuf, (uint)cap, out uint outCount, out int outTracked);
                if (outTracked != 0 && outCount > 0)
                {
                    int n = (int)outCount;
                    for (int i = 0; i < n; i++)
                    {
                        eyesOut[i] = new Vector3(
                            s_NEyeBuf[i * 3 + 0],
                            s_NEyeBuf[i * 3 + 1],
                            s_NEyeBuf[i * 3 + 2]);
                    }
                    if (!IsDegenerateEyeSet(eyesOut, n)) return n;
                }
            }
            catch (System.DllNotFoundException) { }

            // 2) Unity hook chain (Play Mode XR loader): 2 eyes only.
            if (cap >= 2)
            {
                int tracked = 0;
                try
                {
                    DisplayXRNative.displayxr_get_eye_positions(
                        out float lx, out float ly, out float lz,
                        out float rx, out float ry, out float rz,
                        out tracked);
                    if (tracked != 0)
                    {
                        eyesOut[0] = new Vector3(lx, ly, lz);
                        eyesOut[1] = new Vector3(rx, ry, rz);
                        if (!IsDegenerateEyeSet(eyesOut, 2)) return 2;
                    }
                }
                catch (System.DllNotFoundException) { }
            }
            return 0;
        }

        // All-zero or all-equal positions mean the runtime hasn't actually
        // tracked the viewer yet — fall back to nominal so the user still
        // sees a sensible IPD separation.
        static bool IsDegenerateEyeSet(Vector3[] eyes, int count)
        {
            if (count == 0) return true;
            if (eyes[0] != Vector3.zero) return AllSame(eyes, count);
            // First eye is zero — check the rest.
            for (int i = 1; i < count; i++)
                if (eyes[i] != Vector3.zero) return AllSame(eyes, count);
            return true; // all zero
        }

        static bool AllSame(Vector3[] eyes, int count)
        {
            for (int i = 1; i < count; i++)
                if ((eyes[i] - eyes[0]).sqrMagnitude > 1e-8f) return false;
            return true;
        }

        /// <summary>
        /// Two-eye nominal pair around the runtime's nominal viewer (or a
        /// constant fallback when display info is invalid). Used when no
        /// live data is available.
        /// </summary>
        public static int FillNominalEyes(DisplayXRDisplayInfo info, Vector3[] eyesOut)
        {
            if (eyesOut == null || eyesOut.Length < 2) return 0;
            float nx = info.isValid ? info.nominalViewerX : 0f;
            float ny = info.isValid ? info.nominalViewerY : 0f;
            float nz = info.isValid ? info.nominalViewerZ : NOMINAL_VIEWER_Z_FALLBACK;
            float halfIpd = NOMINAL_IPD_METERS * 0.5f;
            eyesOut[0] = new Vector3(nx - halfIpd, ny, nz);
            eyesOut[1] = new Vector3(nx + halfIpd, ny, nz);
            return 2;
        }

        // -----------------------------------------------------------------
        // Kooima eye-factor math, N-view variant
        // (mirror display3d_apply_eye_factors_n at display3d_view.c:220-260)
        // -----------------------------------------------------------------

        public static void ApplyKooimaEyeFactorsN(
            Vector3[] rawEyes, int count,
            float nominalZ,
            float ipdFactor, float parallaxFactor, float perspectiveFactor,
            Vector3[] outEyes)
        {
            if (count <= 0) return;

            // Centroid of all eyes
            Vector3 center = Vector3.zero;
            for (int i = 0; i < count; i++) center += rawEyes[i];
            center /= count;

            // Parallax: lerp centroid toward (0, 0, nominalZ)
            Vector3 centerNew = new Vector3(
                parallaxFactor * center.x,
                parallaxFactor * center.y,
                nominalZ + parallaxFactor * (center.z - nominalZ));

            // IPD: scale each eye's offset from centroid
            for (int i = 0; i < count; i++)
            {
                Vector3 e = centerNew + (rawEyes[i] - center) * ipdFactor;
                outEyes[i] = e * perspectiveFactor;
            }
        }

        // -----------------------------------------------------------------
        // World-space placement per rig type
        // -----------------------------------------------------------------

        /// <summary>
        /// Display-centric: mirrors native display3d_compute_view
        /// (display3d_view.c:280-298). Applies anisotropic eye corrections
        /// (ax = sy/sx, az = sy/sz) and m2v_effective = vdh/(sy*display_h),
        /// then rig.position + rig.rotation only (no TransformPoint —
        /// lossyScale is already folded into m2v via /sy).
        /// </summary>
        public static int GetDisplayCentricEyesWorld(
            Transform rig,
            DisplayXRDisplayInfo info,
            float ipdFactor, float parallaxFactor, float perspectiveFactor,
            float virtualDisplayHeight,
            Vector3[] worldEyesOut, out bool isLive)
        {
            int count = TryGetLiveRawEyes(worldEyesOut);
            isLive = count > 0;
            if (!isLive) count = FillNominalEyes(info, worldEyesOut);

            float nominalZ = info.isValid ? info.nominalViewerZ : NOMINAL_VIEWER_Z_FALLBACK;
            ApplyKooimaEyeFactorsN(
                worldEyesOut, count, nominalZ,
                ipdFactor, parallaxFactor, perspectiveFactor,
                worldEyesOut);

            float sx = SafeAbs(rig.lossyScale.x);
            float sy = SafeAbs(rig.lossyScale.y);
            float sz = SafeAbs(rig.lossyScale.z);
            float vdh = virtualDisplayHeight > 0f
                ? virtualDisplayHeight
                : (info.isValid ? info.displayHeightMeters : 0.2f);
            float ph = info.isValid ? info.displayHeightMeters : 0.2f;
            float ax = sy / sx;
            float az = sy / sz;
            float m2v = (vdh / sy) / ph;

            for (int i = 0; i < count; i++)
            {
                Vector3 e = worldEyesOut[i];
                Vector3 local = new Vector3(e.x * ax * m2v, e.y * m2v, e.z * az * m2v);
                worldEyesOut[i] = rig.position + rig.rotation * new Vector3(local.x, local.y, -local.z);
            }
            return count;
        }

        /// <summary>
        /// Camera-centric: each eye is placed as the displacement from
        /// nominal head-center (0, 0, nominalZ), z-flipped, then
        /// TransformPoint via the rig (which is the camera). Mirrors
        /// camera3d_view.c which multiplies raw eye by scene scale before
        /// rotation — TransformPoint reproduces that on top of the IPD /
        /// parallax adjustments already applied by ApplyKooimaEyeFactorsN.
        /// </summary>
        public static int GetCameraCentricEyesWorld(
            Transform rig,
            DisplayXRDisplayInfo info,
            float ipdFactor, float parallaxFactor,
            Vector3[] worldEyesOut, out bool isLive)
        {
            int count = TryGetLiveRawEyes(worldEyesOut);
            isLive = count > 0;
            if (!isLive) count = FillNominalEyes(info, worldEyesOut);

            float nominalZ = info.isValid ? info.nominalViewerZ : NOMINAL_VIEWER_Z_FALLBACK;
            ApplyKooimaEyeFactorsN(
                worldEyesOut, count, nominalZ,
                ipdFactor, parallaxFactor, perspectiveFactor: 1f,
                worldEyesOut);

            Vector3 headCenter = new Vector3(
                info.isValid ? info.nominalViewerX : 0f,
                info.isValid ? info.nominalViewerY : 0f,
                nominalZ);

            for (int i = 0; i < count; i++)
            {
                Vector3 d = worldEyesOut[i] - headCenter;
                worldEyesOut[i] = rig.TransformPoint(new Vector3(d.x, d.y, -d.z));
            }
            return count;
        }

        static float SafeAbs(float v)
        {
            float a = Mathf.Abs(v);
            return a < 0.001f ? 1f : a;
        }

        // -----------------------------------------------------------------
        // Drawing primitives
        // -----------------------------------------------------------------

        /// <summary>
        /// Asymmetric (Kooima) frustum: eye → 4 display corners, plus a far
        /// quad along each eye→corner ray (extrapolated past the display
        /// plane). farDistanceFromEye is measured from the eye.
        /// </summary>
        public static void DrawAsymmetricFrustum(
            Vector3 eye,
            Vector3 cBL, Vector3 cBR, Vector3 cTR, Vector3 cTL,
            float farDistanceFromEye, Color color)
        {
            Gizmos.color = color;
            Gizmos.DrawLine(eye, cBL);
            Gizmos.DrawLine(eye, cBR);
            Gizmos.DrawLine(eye, cTR);
            Gizmos.DrawLine(eye, cTL);

            Vector3 fBL = ExtrapolateRay(eye, cBL, farDistanceFromEye);
            Vector3 fBR = ExtrapolateRay(eye, cBR, farDistanceFromEye);
            Vector3 fTR = ExtrapolateRay(eye, cTR, farDistanceFromEye);
            Vector3 fTL = ExtrapolateRay(eye, cTL, farDistanceFromEye);
            Gizmos.DrawLine(fBL, fBR);
            Gizmos.DrawLine(fBR, fTR);
            Gizmos.DrawLine(fTR, fTL);
            Gizmos.DrawLine(fTL, fBL);
        }

        static Vector3 ExtrapolateRay(Vector3 origin, Vector3 through, float distance)
        {
            Vector3 dir = through - origin;
            float len = dir.magnitude;
            if (len < 1e-5f) return through;
            return origin + dir * (distance / len);
        }

        public static void DrawEyeGlyph(
            Vector3 pos, Quaternion orient, float size, Color color)
        {
            Gizmos.color = color;
            var saved = Gizmos.matrix;
            Gizmos.matrix = Matrix4x4.TRS(pos, orient, Vector3.one);
            Gizmos.DrawWireCube(Vector3.zero, Vector3.one * size);
            Gizmos.DrawLine(Vector3.zero, new Vector3(0, 0, size * 2.0f));
            Gizmos.matrix = saved;
        }

        // -----------------------------------------------------------------
        // Eye-glyph colors
        // -----------------------------------------------------------------

        public static readonly Color EyeGlyphLive = new Color(1f, 1f, 0.2f, 1f);
        public static readonly Color EyeGlyphNominal = new Color(0.9f, 0.85f, 0.3f, 0.7f);
    }
}
#endif
