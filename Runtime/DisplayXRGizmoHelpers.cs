// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

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
    /// lenticular = 8, …) by reading the runtime's per-view state through
    /// the native display-info / eye-position getters.
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

        public static bool IsSessionActive()
        {
            // The provider (and thus a live session) only runs in Play Mode /
            // built apps. In Edit Mode there is no session, so gizmos are
            // selection-driven.
            return Application.isPlaying;
        }

        // -----------------------------------------------------------------
        // Data source
        // -----------------------------------------------------------------

        public struct CanvasRect
        {
            public int x, y;
            public uint w, h;
            public bool isValid;
        }

        /// <summary>
        /// Active 3D canvas sub-rect (window-client pixels), used to mirror the
        /// window-relative Kooima shift the runtime applies. isValid=false when no
        /// sub-rect is set (full-window canvas). Returns invalid in Edit Mode (no
        /// live session).
        /// </summary>
        public static CanvasRect TryGetCanvasRect()
        {
            var r = new CanvasRect();
            try
            {
                int valid = DisplayXRNative.displayxr_get_canvas_rect_px(
                    out r.x, out r.y, out r.w, out r.h);
                r.isValid = valid != 0;
            }
            catch (System.DllNotFoundException) { r.isValid = false; }
            catch (System.EntryPointNotFoundException) { r.isValid = false; }
            return r;
        }

        /// <summary>
        /// Live Kooima canvas (window on the panel) from the runtime's
        /// XR_DXR_view_rig raw channel, published each frame by the provider
        /// (#189). rect is in panel pixels (top-left origin) — used for the
        /// window-relative eye rebase; size (meters) is the physical canvas —
        /// used for the convergence-plane aspect. Invalid in Edit Mode / on
        /// runtimes that don't fill the raw canvas fields.
        /// </summary>
        public struct KooimaCanvas
        {
            public int rectX, rectY, rectW, rectH; // panel pixels, top-left origin
            public float widthMeters, heightMeters; // physical canvas size
            public bool isValid;
        }

        public static KooimaCanvas TryGetKooimaCanvas()
        {
            var c = new KooimaCanvas();
            try
            {
                int valid = DisplayXRNative.displayxr_get_kooima_canvas(
                    out c.rectX, out c.rectY, out c.rectW, out c.rectH,
                    out c.widthMeters, out c.heightMeters);
                c.isValid = valid != 0 && c.rectW > 0 && c.rectH > 0
                            && c.widthMeters > 0f && c.heightMeters > 0f;
            }
            catch (System.DllNotFoundException) { c.isValid = false; }
            catch (System.EntryPointNotFoundException) { c.isValid = false; }
            return c;
        }

        /// <summary>
        /// Mirrors the window-relative Kooima shift the runtime applies in
        /// xrLocateViews. Returns the virtual-display dims (meters) for sizing
        /// the gizmo rect, and the eye-offset (meters) to subtract from raw eye
        /// positions.
        ///
        /// Preferred source: the runtime's live Kooima canvas (#189) — its
        /// physical size gives the convergence-plane dims directly and its
        /// panel-pixel rect gives the exact window-relative recentering, so the
        /// gizmo tracks window move/resize. Falls back to the app-set canvas
        /// sub-rect (zone/2D-surround apps), then to pass-through (physical
        /// panel dims, no shift) when neither is available.
        /// </summary>
        public static void ComputeWindowRelativeShift(
            DisplayXRDisplayInfo info, CanvasRect canvas,
            out float windowWidthMeters, out float windowHeightMeters,
            out float eyeOffsetXMeters, out float eyeOffsetYMeters)
        {
            // Default = pass-through (physical-display dims, no shift).
            windowWidthMeters = info.isValid ? info.displayWidthMeters : 0f;
            windowHeightMeters = info.isValid ? info.displayHeightMeters : 0f;
            eyeOffsetXMeters = 0f;
            eyeOffsetYMeters = 0f;

            // Preferred: the runtime's live Kooima canvas. Its physical size is
            // the convergence-plane dims; its panel-pixel rect recenters the
            // eyes from panel-center to window-center. Needs display-info pixel
            // + physical dims to convert panel pixels → meters.
            var kc = TryGetKooimaCanvas();
            if (kc.isValid && info.isValid
                && info.displayPixelWidth > 0 && info.displayPixelHeight > 0
                && info.displayWidthMeters > 0f && info.displayHeightMeters > 0f)
            {
                windowWidthMeters = kc.widthMeters;
                windowHeightMeters = kc.heightMeters;

                float kcPxX = info.displayWidthMeters / info.displayPixelWidth;
                float kcPxY = info.displayHeightMeters / info.displayPixelHeight;
                float kcWinCX = kc.rectX + kc.rectW * 0.5f;
                float kcWinCY = kc.rectY + kc.rectH * 0.5f;
                float kcDispCX = info.displayPixelWidth * 0.5f;
                float kcDispCY = info.displayPixelHeight * 0.5f;
                eyeOffsetXMeters = (kcWinCX - kcDispCX) * kcPxX;
                eyeOffsetYMeters = (kcWinCY - kcDispCY) * kcPxY;
#if UNITY_EDITOR_WIN
                // Panel pixels are top-down; OpenXR eye coords are Y-up.
                eyeOffsetYMeters = -eyeOffsetYMeters;
#endif
                return;
            }

            if (!canvas.isValid || !info.isValid) return;
            if (info.displayPixelWidth == 0 || info.displayPixelHeight == 0) return;
            if (canvas.w == 0 || canvas.h == 0) return;

            float pxSizeX = info.displayWidthMeters / (float)info.displayPixelWidth;
            float pxSizeY = info.displayHeightMeters / (float)info.displayPixelHeight;
            windowWidthMeters = canvas.w * pxSizeX;
            windowHeightMeters = canvas.h * pxSizeY;

            float winCenterX = canvas.x + canvas.w * 0.5f;
            float winCenterY = canvas.y + canvas.h * 0.5f;
            float dispCenterX = info.displayPixelWidth * 0.5f;
            float dispCenterY = info.displayPixelHeight * 0.5f;
            eyeOffsetXMeters = (winCenterX - dispCenterX) * pxSizeX;
            eyeOffsetYMeters = (winCenterY - dispCenterY) * pxSizeY;
#if UNITY_EDITOR_WIN
            // Win32 canvas Y is top-down; OpenXR eye coords are Y-up.
            // Matches the #ifdef _WIN32 in hooks.cpp:317 / standalone.cpp:1558.
            eyeOffsetYMeters = -eyeOffsetYMeters;
#endif
        }

        public static DisplayXRDisplayInfo ReadDisplayInfoFromNative()
        {
            var info = new DisplayXRDisplayInfo();
            // Read the runtime's display info from native shared state (populated
            // by the provider in Play Mode / built apps).
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
            catch (System.DllNotFoundException) { info.isValid = false; }
            return info;
        }

        /// <summary>
        /// Try to fill <paramref name="eyesOut"/> with the runtime's last
        /// per-view eye positions (OpenXR display-relative, right-hand,
        /// −Z forward). Returns the number written (2 eyes); 0 if no live data
        /// is available or the data is degenerate (all-zero, or L==R / all-same —
        /// head-center-only sessions).
        /// </summary>
        public static int TryGetLiveRawEyes(Vector3[] eyesOut)
        {
            if (eyesOut == null || eyesOut.Length == 0) return 0;
            int cap = eyesOut.Length;

            // Runtime eye positions from native shared state (Play Mode): 2 eyes.
            // Keep the tracked gate here — XR sessions can legitimately be in a
            // pre-tracked state and we don't want to draw stale data.
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
                catch (System.EntryPointNotFoundException) { }
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

            // Mirror native: shift raw eyes from display-relative to
            // window-relative coords before Kooima factors, and use the
            // WINDOW height as the Kooima "screen" height for m2v scaling
            // (display3d_view.c:280 — m2v = vdh / screen->height_m).
            // When canvas isn't valid, winH_m falls back to physical
            // display height (pass-through), matching old behavior.
            ComputeWindowRelativeShift(info, TryGetCanvasRect(),
                out float _winW_m, out float winH_m,
                out float offX_m, out float offY_m);
            if (offX_m != 0f || offY_m != 0f)
            {
                Vector3 shift = new Vector3(offX_m, offY_m, 0f);
                for (int i = 0; i < count; i++) worldEyesOut[i] -= shift;
            }

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
            float screenH = winH_m > 0f ? winH_m
                : (info.isValid ? info.displayHeightMeters : 0.2f);
            float ax = sy / sx;
            float az = sy / sz;
            float m2v = (vdh / sy) / screenH;

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
        /// Asymmetric (Kooima) frustum: per-eye truncated pyramid bounded by
        /// the near, display, and far planes. The near and far planes are
        /// parallel to the display plane (i.e. perpendicular to the display
        /// normal) — NOT perpendicular to each eye-corner ray. Drawing them
        /// along the rays would tilt them, which is incorrect for off-axis
        /// projection.
        ///
        /// nearDist and farDist are perpendicular distances from the eye to
        /// the near/far planes, in display-normal direction (i.e. world
        /// meters along the display's forward axis). farDist is clamped to
        /// keep the gizmo legible — the actual rendered far plane may sit
        /// further out.
        /// </summary>
        public static void DrawAsymmetricFrustum(
            Vector3 eye,
            Vector3 cBL, Vector3 cBR, Vector3 cTR, Vector3 cTL,
            float nearDist, float farDist,
            Color color)
        {
            // Display plane center + normal. Normal flipped to point toward
            // the eye if needed, so signedDist is positive.
            Vector3 planeCenter = (cBL + cBR + cTR + cTL) * 0.25f;
            Vector3 planeNormal = Vector3.Cross(cBR - cBL, cTL - cBL).normalized;
            float signedDist = Vector3.Dot(eye - planeCenter, planeNormal);
            if (signedDist < 0f)
            {
                planeNormal = -planeNormal;
                signedDist = -signedDist;
            }
            float dDisp = signedDist;
            if (dDisp < 1e-4f) return; // eye coplanar with display, degenerate

            // Clamp so the frustum stays inside a reasonable visual envelope.
            // Near stays in front of the display (between eye and display);
            // far sits 1.5-3× display distance past the display.
            float nSafe = Mathf.Clamp(nearDist, 0.001f, dDisp * 0.5f);
            float fSafe = Mathf.Clamp(farDist, dDisp * 1.5f, dDisp * 3f);

            float tN = nSafe / dDisp;
            float tF = fSafe / dDisp;

            Vector3 nBL = eye + tN * (cBL - eye);
            Vector3 nBR = eye + tN * (cBR - eye);
            Vector3 nTR = eye + tN * (cTR - eye);
            Vector3 nTL = eye + tN * (cTL - eye);
            Vector3 fBL = eye + tF * (cBL - eye);
            Vector3 fBR = eye + tF * (cBR - eye);
            Vector3 fTR = eye + tF * (cTR - eye);
            Vector3 fTL = eye + tF * (cTL - eye);

            Gizmos.color = color;

            // Apex edges: eye → 4 near corners.
            Gizmos.DrawLine(eye, nBL);
            Gizmos.DrawLine(eye, nBR);
            Gizmos.DrawLine(eye, nTR);
            Gizmos.DrawLine(eye, nTL);

            // Near plane rectangle.
            Gizmos.DrawLine(nBL, nBR);
            Gizmos.DrawLine(nBR, nTR);
            Gizmos.DrawLine(nTR, nTL);
            Gizmos.DrawLine(nTL, nBL);

            // Connecting edges near → far (pass through the display plane).
            Gizmos.DrawLine(nBL, fBL);
            Gizmos.DrawLine(nBR, fBR);
            Gizmos.DrawLine(nTR, fTR);
            Gizmos.DrawLine(nTL, fTL);

            // Far plane rectangle.
            Gizmos.DrawLine(fBL, fBR);
            Gizmos.DrawLine(fBR, fTR);
            Gizmos.DrawLine(fTR, fTL);
            Gizmos.DrawLine(fTL, fBL);
        }

        public static void DrawEyeGlyph(
            Vector3 pos, Quaternion orient, float size, Color color)
        {
            Gizmos.color = color;
            var saved = Gizmos.matrix;
            Gizmos.matrix = Matrix4x4.TRS(pos, orient, Vector3.one);
            // Filled sphere — the most readable marker at typical Scene-view
            // zoom levels. The earlier wire-cube + forward-line combo at 1.5 cm
            // was effectively invisible inside the converging frustum lines.
            Gizmos.DrawSphere(Vector3.zero, size * 0.5f);
            // Forward stub keeps orientation visible.
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
