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
    /// The joint → rig mapping of the runtime's <c>XR_DXR_view_rig</c> spec 4 ("rig-scoped hand
    /// joints", §2 of <c>docs/specs/extensions/XR_DXR_view_rig.md</c>), done app-side because the
    /// provider locates joints in LOCAL with no rig chained. A joint maps exactly as an eye at
    /// that point would, with the eye-pair factors (ipd, parallax) neutral:
    /// <list type="bullet">
    /// <item>both rigs first rebase onto the canvas (the window on the panel): minus the canvas
    /// centre;</item>
    /// <item>display rig: × <c>perspectiveFactor · virtualDisplayHeight / canvas height</c>; the
    /// plane sits at the rig camera;</item>
    /// <item>camera rig: × <c>metersToVirtual</c> about the nominal viewer plane
    /// (<c>z − nominal z</c>), so the rig camera IS the nominal viewer and the panel sits
    /// <c>nominal z</c> in front of it. The frustum (convergence, FOV) does not move a
    /// point.</item>
    /// </list>
    /// Keep this in step with the runtime's <c>m_view_rig_map_relation</c>; a later move to
    /// chaining the rig on <c>xrLocateHandJointsEXT</c> must give the same numbers.
    /// </summary>
    public static class DisplayXRHandRigMath
    {
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
        /// Camera-centric rig (<c>XrCameraRigDXR</c>). The provider sends
        /// <c>metersToVirtual = 1</c>; <paramref name="nominalZ"/> is the nominal viewer's
        /// distance from the panel (display info), ≤ 0 = <see cref="NominalViewerZFallback"/>.
        /// OpenXR <c>z − nominal z</c> (+z toward the viewer) is Unity <c>z + nominal z</c>.
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
