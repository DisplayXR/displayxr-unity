// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0

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

        /// <summary>Currently active camera for rig gating and input.</summary>
        public static Camera ActiveCamera
        {
            get => s_ActiveCamera;
            set
            {
                s_ActiveCamera = value;
                s_ActiveCameraName = value != null ? value.gameObject.name : null;
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

            // Auto-elect first registered camera
            if (s_ActiveCamera == null)
                ActiveCamera = cam;
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
