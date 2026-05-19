// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0

#if UNITY_EDITOR
using UnityEngine;

namespace DisplayXR
{
    /// <summary>
    /// Shared Scene-view gizmo primitives for DisplayXRDisplay / DisplayXRCamera.
    /// Re-implements the canonical <c>display3d_apply_eye_factors</c> math
    /// (native~/display3d_view.c:119-160) in C# so the drawn frustum origins
    /// match what the runtime actually consumes in xrLocateViews.
    /// </summary>
    internal static class DisplayXRGizmoHelpers
    {
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
        /// Read the last per-eye pose pushed through xrLocateViews. Positions
        /// are in OpenXR display-relative space (right-hand, -Z forward).
        /// Returns true only if the runtime reported tracked AND non-zero
        /// positions (untracked sessions can return zeros).
        /// </summary>
        public static bool TryGetLiveEyesOpenXR(out Vector3 rawLeft, out Vector3 rawRight)
        {
            rawLeft = rawRight = Vector3.zero;
            int tracked = 0;
            try
            {
                DisplayXRNative.displayxr_get_eye_positions(
                    out float lx, out float ly, out float lz,
                    out float rx, out float ry, out float rz,
                    out tracked);
                rawLeft = new Vector3(lx, ly, lz);
                rawRight = new Vector3(rx, ry, rz);
            }
            catch (System.DllNotFoundException)
            {
                return false;
            }
            if (tracked == 0) return false;
            // Some runtimes return tracked=1 with zero positions before the
            // first frame — guard against that to avoid drawing eyes at the
            // display origin.
            if (rawLeft == Vector3.zero && rawRight == Vector3.zero) return false;
            // Some runtimes report a single head-center pose for both eyes
            // before per-eye tracking kicks in. Treat that as "no live IPD
            // separation" and fall back to the nominal pair so the user can
            // still see two distinct ray origins.
            if ((rawLeft - rawRight).sqrMagnitude < 1e-8f) return false;
            return true;
        }

        /// <summary>
        /// Nominal eye pair around the runtime's nominal viewer (or constant
        /// fallback when display info is invalid). Same conventions as
        /// TryGetLiveEyesOpenXR (OpenXR display-relative space).
        /// </summary>
        public static void NominalEyesOpenXR(
            DisplayXRDisplayInfo info,
            out Vector3 nominalLeft, out Vector3 nominalRight)
        {
            float nx = info.isValid ? info.nominalViewerX : 0f;
            float ny = info.isValid ? info.nominalViewerY : 0f;
            float nz = info.isValid ? info.nominalViewerZ : NOMINAL_VIEWER_Z_FALLBACK;
            float halfIpd = NOMINAL_IPD_METERS * 0.5f;
            nominalLeft = new Vector3(nx - halfIpd, ny, nz);
            nominalRight = new Vector3(nx + halfIpd, ny, nz);
        }

        // -----------------------------------------------------------------
        // Kooima eye-factor math (mirror display3d_apply_eye_factors)
        // -----------------------------------------------------------------

        public static void ApplyKooimaEyeFactors(
            Vector3 rawLeft, Vector3 rawRight,
            float nominalZ,
            float ipdFactor, float parallaxFactor, float perspectiveFactor,
            out Vector3 outLeft, out Vector3 outRight)
        {
            // Step 1: IPD factor scales eye-to-center vector, center fixed.
            Vector3 center = (rawLeft + rawRight) * 0.5f;
            Vector3 leftVec = (rawLeft - center) * ipdFactor;
            Vector3 rightVec = (rawRight - center) * ipdFactor;

            // Step 2: parallax factor lerps center toward (0, 0, nominal_z).
            Vector3 centerNew = new Vector3(
                parallaxFactor * center.x,
                parallaxFactor * center.y,
                nominalZ + parallaxFactor * (center.z - nominalZ));

            outLeft = (centerNew + leftVec) * perspectiveFactor;
            outRight = (centerNew + rightVec) * perspectiveFactor;
        }

        // -----------------------------------------------------------------
        // World-space eye placement per rig type
        // -----------------------------------------------------------------

        /// <summary>
        /// Display-centric world placement. Mirrors the native
        /// display3d_compute_view pipeline (display3d_view.c:280-298):
        /// applies anisotropic eye corrections (ax = sy/sx, az = sy/sz),
        /// then m2v_effective = vdh / (sy * display_height_m) so eyes
        /// scale to virtual-display space. Uses rig.position + rig.rotation
        /// (NOT TransformPoint) because the runtime folds lossyScale into
        /// m2v via /sy — using TransformPoint would multiply by sy on top
        /// of that, producing sy^2 scaling.
        /// </summary>
        public static void GetDisplayCentricEyes(
            Transform rig,
            DisplayXRDisplayInfo info,
            float ipdFactor, float parallaxFactor, float perspectiveFactor,
            float virtualDisplayHeight,
            out Vector3 leftWorld, out Vector3 rightWorld, out bool isLive)
        {
            isLive = TryGetLiveEyesOpenXR(out Vector3 rawL, out Vector3 rawR);
            if (!isLive)
                NominalEyesOpenXR(info, out rawL, out rawR);

            float nominalZ = info.isValid ? info.nominalViewerZ : NOMINAL_VIEWER_Z_FALLBACK;
            ApplyKooimaEyeFactors(
                rawL, rawR, nominalZ,
                ipdFactor, parallaxFactor, perspectiveFactor,
                out Vector3 kooimaL, out Vector3 kooimaR);

            // Scale-as-zoom: vdh /= sy (matches displayxr_hooks.cpp:447).
            // Anisotropic per-axis corrections so non-uniform rig scale still
            // produces consistent geometry (matches lines 451-470).
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

            Vector3 placeL = ApplyAnisoAndM2v(kooimaL, ax, az, m2v);
            Vector3 placeR = ApplyAnisoAndM2v(kooimaR, ax, az, m2v);

            // OpenXR (right-hand, -Z forward) → Unity local (left-hand, +Z fwd).
            // Apply rotation and position ONLY — scale is already folded in
            // via m2v above. TransformPoint would re-multiply by lossyScale.
            leftWorld = rig.position + rig.rotation * new Vector3(placeL.x, placeL.y, -placeL.z);
            rightWorld = rig.position + rig.rotation * new Vector3(placeR.x, placeR.y, -placeR.z);
        }

        /// <summary>
        /// Camera-centric world placement. Eyes are at the rig (camera)
        /// position plus the Kooima-vs-nominal displacement, mapped through
        /// rig pose. TransformPoint here matches camera3d_view.c which
        /// multiplies eye_raw by scene scale before the rotation.
        /// </summary>
        public static void GetCameraCentricEyes(
            Transform rig,
            DisplayXRDisplayInfo info,
            float ipdFactor, float parallaxFactor,
            out Vector3 leftWorld, out Vector3 rightWorld, out bool isLive)
        {
            isLive = TryGetLiveEyesOpenXR(out Vector3 rawL, out Vector3 rawR);
            if (!isLive)
                NominalEyesOpenXR(info, out rawL, out rawR);

            float nominalZ = info.isValid ? info.nominalViewerZ : NOMINAL_VIEWER_Z_FALLBACK;
            ApplyKooimaEyeFactors(
                rawL, rawR, nominalZ,
                ipdFactor, parallaxFactor, perspectiveFactor: 1f,
                out Vector3 kooimaL, out Vector3 kooimaR);

            NominalEyesOpenXR(info, out Vector3 nomL, out Vector3 nomR);
            Vector3 dL = kooimaL - nomL;
            Vector3 dR = kooimaR - nomR;

            leftWorld = rig.TransformPoint(new Vector3(dL.x, dL.y, -dL.z));
            rightWorld = rig.TransformPoint(new Vector3(dR.x, dR.y, -dR.z));
        }

        static float SafeAbs(float v)
        {
            float a = Mathf.Abs(v);
            return a < 0.001f ? 1f : a;
        }

        static Vector3 ApplyAnisoAndM2v(Vector3 eye, float ax, float az, float m2v)
        {
            return new Vector3(
                eye.x * ax * m2v,
                eye.y * m2v,
                eye.z * az * m2v);
        }

        // -----------------------------------------------------------------
        // Drawing primitives
        // -----------------------------------------------------------------

        /// <summary>
        /// Draw an asymmetric (Kooima) frustum: eye → 4 display corners,
        /// plus a far-plane quad along each eye→corner ray. Distance is
        /// measured from the eye to the corresponding corner; the far
        /// corner sits at <c>farDistanceFromEye</c> meters along the same
        /// ray (extrapolated past the display plane).
        /// </summary>
        public static void DrawAsymmetricFrustum(
            Vector3 eye,
            Vector3 cBL, Vector3 cBR, Vector3 cTR, Vector3 cTL,
            float farDistanceFromEye, Color color)
        {
            Gizmos.color = color;
            // 4 edges eye → corner
            Gizmos.DrawLine(eye, cBL);
            Gizmos.DrawLine(eye, cBR);
            Gizmos.DrawLine(eye, cTR);
            Gizmos.DrawLine(eye, cTL);

            // Far quad along each ray (extrapolated past the display plane).
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
            Vector3 dir = (through - origin);
            float len = dir.magnitude;
            if (len < 1e-5f) return through;
            return origin + dir * (distance / len);
        }

        /// <summary>
        /// Small camera glyph at the eye: a wire cube plus a short forward
        /// stub indicating orientation. <paramref name="orient"/> follows
        /// Unity's +Z-forward convention.
        /// </summary>
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
