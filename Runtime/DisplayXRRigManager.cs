// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

using System.Collections.Generic;
using UnityEngine;

namespace DisplayXR
{
    /// <summary>
    /// Static registry of DisplayXR rig cameras. Rigs self-register in OnEnable
    /// and self-deregister in OnDisable. The first registered camera is auto-elected
    /// as active. No MonoBehaviour or scene object required.
    /// </summary>
    public static class DisplayXRRigManager
    {
        private static Camera s_ActiveCamera;
        private static string s_ActiveCameraName;
        private static readonly List<Camera> s_RegisteredCameras = new List<Camera>();

        /// <summary>
        /// True while the boot splash owns the projection. The splash rig is NOT
        /// registered (so app scripts that read <see cref="ActiveCamera"/> bind to
        /// the app's real rig, not the transient splash), so to keep the splash
        /// the sole tunables-pusher the registered rigs suspend their push while
        /// this is set. Cleared when the splash hands off.
        /// </summary>
        public static bool SplashActive { get; set; }

        /// <summary>Currently active camera for rig gating and input.</summary>
        public static Camera ActiveCamera
        {
            get => s_ActiveCamera;
            set
            {
                s_ActiveCamera = value;
                s_ActiveCameraName = value != null ? value.gameObject.name : null;
                ApplyActiveCameraGate();
            }
        }

        /// <summary>
        /// Enforce single-active rendering: only the active rig camera stays enabled,
        /// so only it renders into the XR eye textures. In provider mode (and the real
        /// Unity-OpenXR hook path) Unity renders EVERY enabled camera with
        /// stereoTargetEye=Both into the shared per-eye targets — but the provider is
        /// handed ONE rig pose per frame (DisplayXRProviderDriver pushes the active
        /// rig's pose, and the native eye pose is rig-RELATIVE). An inactive rig camera
        /// at a different transform composes that rig-relative eye pose against its own
        /// transform → a mismatched world eye, and at equal camera depth it overwrites
        /// the correct render (e.g. switching from the display rig at z=0 to a camera
        /// rig at z=-2 makes the still-enabled display camera render past the cube and
        /// clear it away). Disabling the inactive rig cameras keeps the rendered set
        /// matched to the single pushed pose. No-op outside Play Mode (rigs only
        /// register via OnEnable in Play) — the edit-mode preview renders its own
        /// dedicated eye-cams, not these source cameras, so it is unaffected.
        /// </summary>
        private static void ApplyActiveCameraGate()
        {
            if (!Application.isPlaying) return;
            for (int i = 0; i < s_RegisteredCameras.Count; i++)
            {
                var cam = s_RegisteredCameras[i];
                if (cam != null)
                    cam.enabled = (cam == s_ActiveCamera);
            }
        }

        /// <summary>Cached name of the active camera (for UI labels).</summary>
        public static string ActiveCameraName => s_ActiveCameraName;

        /// <summary>Read-only list of registered rig cameras.</summary>
        public static IReadOnlyList<Camera> RegisteredCameras => s_RegisteredCameras;

        /// <summary>
        /// Register a rig camera. Called from DisplayXRDisplay/DisplayXRCamera OnEnable.
        /// Auto-elects as active if no active camera is set.
        /// </summary>
        public static void Register(Camera cam)
        {
            if (cam == null || s_RegisteredCameras.Contains(cam)) return;
            s_RegisteredCameras.Add(cam);

            // Auto-elect first registered camera (the setter applies the gate). For a
            // later registration the active camera is unchanged, so apply the gate
            // explicitly to disable the newly-registered (non-active) rig camera —
            // otherwise it renders alongside the active one and corrupts the eye
            // textures (see ApplyActiveCameraGate).
            if (s_ActiveCamera == null)
                ActiveCamera = cam;
            else
                ApplyActiveCameraGate();
        }

        /// <summary>
        /// Unregister a rig camera. Called from DisplayXRDisplay/DisplayXRCamera OnDisable.
        /// Elects next available if the unregistered camera was active.
        /// </summary>
        public static void Unregister(Camera cam)
        {
            s_RegisteredCameras.Remove(cam);

            if (s_ActiveCamera == cam)
                ActiveCamera = s_RegisteredCameras.Count > 0 ? s_RegisteredCameras[0] : null;
        }

        /// <summary>Cycle to the next registered camera (Tab key).</summary>
        public static void CycleNext()
        {
            if (s_RegisteredCameras.Count < 2) return;

            int cur = s_RegisteredCameras.IndexOf(s_ActiveCamera);
            int next = (cur + 1) % s_RegisteredCameras.Count;
            ActiveCamera = s_RegisteredCameras[next];
        }
    }
}
