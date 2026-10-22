// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

using UnityEngine;

namespace DisplayXR
{
    /// <summary>
    /// Drop-in switch for DisplayXR <b>side-step</b>: while this component is enabled, the app's
    /// window steps aside when the window the user is working in covers the app's content, and
    /// goes back once its place is free (Linux, native-Wayland player on GNOME with the DisplayXR
    /// GNOME extension; elsewhere nothing happens). It acts only while the app's desktop control
    /// is granted (<see cref="DisplayXRDesktop.State"/> == <see cref="DesktopControlState.Granted"/>;
    /// the app asks with <see cref="DisplayXRDesktop.Request"/>, this component never does).
    /// Put it on any GameObject (one per scene). The settings mirror
    /// <see cref="DisplayXRSideStep"/>, which code can drive directly instead, and which also has
    /// the state and the events.
    /// </summary>
    [AddComponentMenu("DisplayXR/DisplayXR Window Side-Step")]
    [DisallowMultipleComponent]
    public class DisplayXRWindowSideStep : MonoBehaviour
    {
        [Tooltip("Share of the app's content (0..1) the window in front must cover before the window steps aside.")]
        [Range(0.01f, 1f)]
        public float trigger = 0.15f;

        [Tooltip("Covered this long (seconds) before it steps aside.")]
        [Min(0f)]
        public float dwell = 0.6f;

        [Tooltip("Go back home once its place is free. Off: the place it stepped to becomes home.")]
        public bool returnHome = true;

        [Tooltip("Home free this long (seconds) before it goes back.")]
        [Min(0f)]
        public float returnDelay = 2f;

        [Tooltip("A move glides this long (seconds, ease-out); 0 jumps.")]
        [Range(0f, 2f)]
        public float glideSeconds = 0.3f;

        [Tooltip("The preferred side among equally free places.")]
        public SideStepSide preferredSide = SideStepSide.Auto;

        [Tooltip("Desktop px kept between the app's content and the other window or the monitor's edge.")]
        [Min(0)]
        public int margin = 24;

        void OnEnable()
        {
            Apply();
            DisplayXRSideStep.Enabled = true;
        }

        void OnDisable()
        {
            DisplayXRSideStep.Enabled = false;
        }

        // Live-apply inspector edits (cheap: the native side is told only when a value changed).
        void Update() => Apply();

        void Apply()
        {
            DisplayXRSideStep.Trigger = trigger;
            DisplayXRSideStep.Dwell = dwell;
            DisplayXRSideStep.ReturnHome = returnHome;
            DisplayXRSideStep.ReturnDelay = returnDelay;
            DisplayXRSideStep.GlideSeconds = glideSeconds;
            DisplayXRSideStep.PreferredSide = preferredSide;
            DisplayXRSideStep.Margin = margin;
        }
    }
}
