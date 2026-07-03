// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

using UnityEngine;

namespace DisplayXR
{
    /// <summary>
    /// Boot-splash spawner. The DisplayXR splash is <b>on by default</b>: every
    /// DisplayXR app shows it at startup with no scene wiring. It spawns before
    /// the app's first scene loads, occludes the loading scene, plays the logo
    /// on the zero-disparity plane, then destroys itself to reveal the app.
    ///
    /// The splash rig does NOT join DisplayXRRigManager (it owns the projection
    /// via DisplayXRRigManager.SplashActive instead), so app scripts that bind
    /// to the active rig latch onto the app's real rig from frame one.
    ///
    /// Opt out by defining <c>DISPLAYXR_NO_SPLASH</c> (DisplayXR Manifest
    /// Settings ▸ Boot Splash ▸ Disable). When opted out the whole class
    /// compiles away and the companion build processor leaves Unity's stock
    /// splash alone.
    /// </summary>
#if !DISPLAYXR_NO_SPLASH
    internal static class DisplayXRSplashBootstrap
    {
        [RuntimeInitializeOnLoadMethod(RuntimeInitializeLoadType.BeforeSceneLoad)]
        static void Boot()
        {
            // Skip for transparent-overlay apps. They run in a cloaked, shifted-
            // offscreen overlay window (set up at SubsystemRegistration, before
            // this runs); the splash rig's window/viewport push pulls that window
            // back on-screen, and an opaque branded splash over a click-through
            // desktop overlay is wrong anyway. The flag is set by
            // DisplayXRTransparentOverlay.RequestTransparentSession() →
            // DisplayXRProvider.RequestTransparentBackground() at
            // SubsystemRegistration, which always precedes BeforeSceneLoad.
            if (DisplayXRProvider.s_TransparentBackgroundRequested)
                return;

            // Built inactive so DisplayXRDisplay.bootSplashOverlay is set before
            // OnEnable runs (which decides whether to register with the rig
            // manager). Activated last.
            var go = new GameObject("DisplayXR Boot Splash");
            go.SetActive(false);
            Object.DontDestroyOnLoad(go);
            go.AddComponent<Camera>();
            var display = go.AddComponent<DisplayXRDisplay>();
            display.bootSplashOverlay = true;   // don't register; own the projection
            var splash = go.AddComponent<DisplayXRSplash>();
            splash.overlayMode = true;
            go.SetActive(true);
        }
    }
#endif
}
