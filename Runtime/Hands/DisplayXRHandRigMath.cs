// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Pure C# (no UnityEngine) so Tests~/HandRigMath can run it under plain .NET.
// Compiled into the Unity package only when com.unity.xr.hands >= 1.3.0 is installed
// (DISPLAYXR_XR_HANDS, the assembly's define constraint).

namespace DisplayXR.Hands
{
    /// <summary>
    /// Where a hand joint lands in the rig camera's frame: <c>p_rig = scale · (p − origin)</c>,
    /// with <c>p</c> the joint in the provider's Unity-frame display-plane metres (origin at
    /// the panel centre, +x right, +y up, +z INTO the screen) and <c>p_rig</c> relative to the
    /// rig camera's transform. Radii scale by <see cref="scale"/>; orientations are unchanged.
    /// </summary>
    public struct DisplayXRHandRigMapping
    {
        /// <summary>The display-plane point that maps to the rig camera's origin (metres, Unity frame).</summary>
        public float originX, originY, originZ;

        /// <summary>Metres → rig units.</summary>
        public float scale;

        public static DisplayXRHandRigMapping Identity => new DisplayXRHandRigMapping { scale = 1f };

        public void Apply(ref float x, ref float y, ref float z)
        {
            x = scale * (x - originX);
            y = scale * (y - originY);
            z = scale * (z - originZ);
        }
    }

    /// <summary>
    /// Which rig a locate's joints are in (native <c>DisplayXR_HandTracking_RigKind</c>, the
    /// rig the views were located with), and which rig the scene renders with.
    /// </summary>
    public enum DisplayXRHandRigKind
    {
        /// <summary>Display-plane metres: no rig chained (older runtime) — C# maps them.</summary>
        None = 0,
        /// <summary>The app's display-centric rig (<c>DisplayXRDisplay</c>).</summary>
        Display = 1,
        /// <summary>The app's camera-centric rig (<c>DisplayXRCamera</c>).</summary>
        Camera = 2,
        /// <summary>The provider's default display rig: the app's rig has not reached the
        /// provider yet (session start, the boot splash) or the scene has none.</summary>
        DefaultDisplay = 3,
    }

    /// <summary>
    /// The joint → rig mapping of the runtime's <c>XR_DXR_view_rig</c> rig-scoped hand joints
    /// (§2 of <c>docs/specs/extensions/XR_DXR_view_rig.md</c>; camera rigs per spec 5), done
    /// app-side when the provider locates joints in LOCAL with no rig chained. Both rigs first
    /// rebase onto the canvas (the window on the panel): minus the canvas centre. Then:
    /// <list type="bullet">
    /// <item>display rig: × <c>perspectiveFactor · virtualDisplayHeight / canvas height</c>; the
    /// glass sits at the rig camera (the display plane);</item>
    /// <item>camera rig: the glass sits on the CONVERGENCE plane, <c>1 / invConvergenceDistance</c>
    /// in front of the camera (zero parallax), scaled by the camera's virtual screen height
    /// there, <c>V = 2 · tan(vFOV/2) / invConvergenceDistance</c>, over the canvas height — so a
    /// hand on the glass lands on the pixel it covers, exactly as under a display rig. Only at
    /// infinite convergence (<c>invConvergenceDistance</c> 0) does it fall back to
    /// × <c>metersToVirtual</c> about the nominal viewer plane (<c>z − nominal z</c>).</item>
    /// </list>
    /// Keep this in step with the runtime's <c>m_view_rig_map_relation</c>: chaining the rig on
    /// <c>xrLocateHandJointsEXT</c> (runtime ≥ spec 5 for a camera rig) must give the same numbers.
    /// A spec-4 runtime still maps chained camera-rig joints about the nominal viewer.
    /// </summary>
    public static class DisplayXRHandRigMath
    {
        /// <summary>
        /// Whether rig-frame joints can be placed relative to the scene's rig camera: only when
        /// the rig they were located with (<paramref name="jointRig"/>) is the one the scene
        /// renders with (<paramref name="sceneRig"/>: <see cref="DisplayXRHandRigKind.Display"/>,
        /// <see cref="DisplayXRHandRigKind.Camera"/>, or <see cref="DisplayXRHandRigKind.None"/>
        /// for a camera with no DisplayXR rig). Display-frame joints read as camera-relative
        /// land behind the camera, and camera-frame joints under a display rig sit a nominal
        /// viewer distance too deep, so a mismatch is held back (hand reported untracked) — it
        /// happens while the boot splash owns the views, before the app's rig is pushed.
        /// <see cref="DisplayXRHandRigKind.None"/> joints are display-plane metres that C#
        /// maps itself, so they always fit.
        /// </summary>
        public static bool JointRigFitsScene(DisplayXRHandRigKind jointRig, DisplayXRHandRigKind sceneRig)
        {
            switch (jointRig)
            {
                case DisplayXRHandRigKind.None: return true;
                case DisplayXRHandRigKind.Display: return sceneRig == DisplayXRHandRigKind.Display;
                case DisplayXRHandRigKind.Camera: return sceneRig == DisplayXRHandRigKind.Camera;
                case DisplayXRHandRigKind.DefaultDisplay: return sceneRig == DisplayXRHandRigKind.None;
                default: return false;
            }
        }

        /// <summary>Nominal viewer distance when display info has none (matches the gizmos).</summary>
        public const float NominalViewerZFallback = 0.5f;

        /// <summary>The canvas in display-plane metres (OpenXR / Unity share X and Y).</summary>
        public struct Canvas
        {
            public float centerX, centerY; // canvas centre relative to the panel centre, +y up
            public float heightM;          // physical canvas height
        }

        /// <summary>The whole panel as the canvas (fullscreen, or no live canvas yet).</summary>
        public static Canvas FullPanel(float panelHeightM) =>
            new Canvas { centerX = 0f, centerY = 0f, heightM = panelHeightM };

        /// <summary>
        /// The canvas from the runtime's raw channel (<c>XrViewDisplayRawDXR.canvasRectPx</c>,
        /// panel pixels with a TOP-LEFT origin on every platform, and <c>canvasSizeMeters</c>)
        /// plus the panel's pixel and metre size. False when an input is degenerate.
        /// </summary>
        public static bool TryCanvasFromPanelRect(int rectX, int rectY, int rectW, int rectH,
                                                  float canvasHeightM,
                                                  uint panelPixelWidth, uint panelPixelHeight,
                                                  float panelWidthM, float panelHeightM,
                                                  out Canvas canvas)
        {
            canvas = default;
            if (rectW <= 0 || rectH <= 0 || !(canvasHeightM > 0f)) return false;
            if (panelPixelWidth == 0 || panelPixelHeight == 0) return false;
            if (!(panelWidthM > 0f) || !(panelHeightM > 0f)) return false;
            float pxX = panelWidthM / panelPixelWidth;
            float pxY = panelHeightM / panelPixelHeight;
            canvas.centerX = (rectX + rectW * 0.5f - panelPixelWidth * 0.5f) * pxX;
            canvas.centerY = -(rectY + rectH * 0.5f - panelPixelHeight * 0.5f) * pxY; // pixels run down
            canvas.heightM = canvasHeightM;
            return true;
        }

        /// <summary>
        /// Display-centric rig (<c>XrDisplayRigDXR</c>). <paramref name="virtualDisplayHeight"/>
        /// is the value the provider SENDS (the rig's field, or the physical panel height when
        /// that is 0).
        /// </summary>
        public static DisplayXRHandRigMapping DisplayRig(Canvas canvas, float virtualDisplayHeight,
                                                        float perspectiveFactor)
        {
            float persp = perspectiveFactor > 0f ? perspectiveFactor : 1f;
            float m2v = (canvas.heightM > 0f && virtualDisplayHeight > 0f)
                ? virtualDisplayHeight / canvas.heightM : 1f;
            return new DisplayXRHandRigMapping
            {
                originX = canvas.centerX,
                originY = canvas.centerY,
                originZ = 0f,
                scale = persp * m2v,
            };
        }

        /// <summary>
        /// Camera-centric rig (<c>XrCameraRigDXR</c>) at a finite convergence: the glass lands on
        /// the convergence plane, <c>1 / invConvergenceDistance</c> in front of the camera, scaled
        /// by <c>V / canvas height</c> with <c>V = 2 · halfTanVFov / invConvergenceDistance</c>
        /// (the camera's virtual screen there). <paramref name="halfTanVFov"/> is
        /// <c>tan(vFOV/2)</c> — the tunables' <c>fovOverride</c>. In Unity's frame (+z in front
        /// of the camera) the glass is at <c>z = +1/invConvergenceDistance</c>.
        /// <c>invConvergenceDistance</c> ≤ 0 (parallel cameras), a non-positive FOV or an unknown
        /// canvas height fall back to <see cref="CameraRig(Canvas, float, float)"/>.
        /// </summary>
        public static DisplayXRHandRigMapping CameraRig(Canvas canvas, float nominalZ, float metersToVirtual,
                                                       float invConvergenceDistance, float halfTanVFov)
        {
            if (!(invConvergenceDistance > 0f) || !(halfTanVFov > 0f) || !(canvas.heightM > 0f))
                return CameraRig(canvas, nominalZ, metersToVirtual);
            float convergence = 1f / invConvergenceDistance;
            float virtualHeight = 2f * halfTanVFov * convergence;
            float s = virtualHeight / canvas.heightM;
            return new DisplayXRHandRigMapping
            {
                originX = canvas.centerX,
                originY = canvas.centerY,
                // p_rig.z = s · (0 − originZ) = convergence for a point on the glass.
                originZ = -convergence / s,
                scale = s,
            };
        }

        /// <summary>
        /// Camera-centric rig (<c>XrCameraRigDXR</c>) at INFINITE convergence (no glass plane):
        /// × <paramref name="metersToVirtual"/> about the nominal viewer plane. The provider
        /// sends <c>metersToVirtual = 1</c>; <paramref name="nominalZ"/> is the nominal viewer's
        /// distance from the panel (display info), ≤ 0 = <see cref="NominalViewerZFallback"/>.
        /// OpenXR <c>z − nominal z</c> (+z toward the viewer) is Unity <c>z + nominal z</c>.
        /// (Before runtime spec 5 this was the mapping at every convergence.)
        /// </summary>
        public static DisplayXRHandRigMapping CameraRig(Canvas canvas, float nominalZ, float metersToVirtual)
        {
            float n = nominalZ > 0f ? nominalZ : NominalViewerZFallback;
            return new DisplayXRHandRigMapping
            {
                originX = canvas.centerX,
                originY = canvas.centerY,
                originZ = -n,
                scale = metersToVirtual > 0f ? metersToVirtual : 1f,
            };
        }
    }
}
