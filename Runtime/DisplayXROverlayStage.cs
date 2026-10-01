// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

using System.Collections.Generic;
using UnityEngine;

namespace DisplayXR
{
    /// <summary>
    /// Private world-space "stage" positions for the offscreen canvases of
    /// <see cref="DisplayXRLocal2D"/> and <see cref="DisplayXRWindowSpaceUI"/>.
    ///
    /// Each component parks its canvas far from the scene on a private layer and
    /// renders it with its own ortho camera that culls to that layer. They all used
    /// one shared spot, so with two or more overlays every camera also rendered the
    /// other canvases: a HUD showed up inside the Local2D bubble and vice versa (the
    /// layer can't separate them — they share it). Giving each live overlay its own
    /// stage, spaced far beyond any canvas's extent and the cameras' 10-unit far
    /// plane, keeps each camera on its own canvas.
    /// </summary>
    internal static class DisplayXROverlayStage
    {
        static readonly Vector3 kOrigin = new Vector3(0f, 100000f, 0f);
        // A 4096-unit canvas at the 0.01 canvas scale is ~41 world units wide.
        const float kSpacing = 1000f;

        static readonly HashSet<int> s_Used = new HashSet<int>();

        /// <summary>Take the lowest free stage index.</summary>
        public static int Acquire()
        {
            int i = 0;
            while (s_Used.Contains(i)) i++;
            s_Used.Add(i);
            return i;
        }

        /// <summary>Give a stage back. Negative indices are ignored.</summary>
        public static void Release(int index)
        {
            if (index >= 0) s_Used.Remove(index);
        }

        /// <summary>World position of the canvas on stage <paramref name="index"/>.</summary>
        public static Vector3 Position(int index) => kOrigin + new Vector3(index * kSpacing, 0f, 0f);
    }
}
