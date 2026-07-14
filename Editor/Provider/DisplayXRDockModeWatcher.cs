// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

using System.IO;
using UnityEditor;
using UnityEngine;
using UnityEngine.XR.Management;
using DisplayXR;

namespace DisplayXR.Editor
{
    /// <summary>
    /// (#740 auto-switch) Mid-Play dock-transition watcher for the GameView
    /// weave-to-texture hybrid. The bind mode (docked → texture+child-glue,
    /// undocked → present) is fixed at the OpenXR session bind, so when the user
    /// docks/undocks the Game view DURING Play the display subsystem must be
    /// restarted for <c>session_start</c> to re-bind the right mode. This watcher
    /// polls the dock state from <c>EditorApplication.update</c> and drives
    /// <c>loader.Stop()</c> → <c>loader.Start()</c> on a debounced transition.
    ///
    /// It deliberately lives OUTSIDE <see cref="DisplayXRProviderDriver"/>:
    /// <c>loader.Stop()</c> destroys the driver GameObject, so a driver-hosted
    /// watcher would destroy itself mid-restart and never issue the Start().
    /// The editor update loop survives the whole subsystem cycle.
    ///
    /// Stop and Start are issued on SEPARATE editor ticks (STOPPING → RESTARTING)
    /// so the render thread finishes the native session teardown (GfxStop →
    /// xrDestroySession) before the new session's GfxStart is queued.
    ///
    /// Test hook: touching <c>%TEMP%\dxr_dock_toggle</c> forces a mode-flip
    /// restart without a physical undock (scripted verification of the full
    /// teardown/re-bind cycle) — it routes the flipped state through
    /// <see cref="DisplayXRProviderDriver.ForceNextDockState"/> so the restart
    /// binds the OPPOSITE mode instead of re-detecting the real one. The forced
    /// mode is TRANSIENT by design: real detection still disagrees, so the
    /// watcher restarts back to the true mode ~2–3 s later (debounce + min gap).
    /// Use it to smoke the rebind machinery; use AutoFloatProbe-style real
    /// layout changes to test steady-state mode switches.
    ///
    /// Editor+Windows only, and inert unless the texture probe
    /// (DISPLAYXR_PROV_TEXTURE_PROBE=1) is active — same gate as the rest of the
    /// GameView feature.
    /// </summary>
#if UNITY_EDITOR_WIN
    [InitializeOnLoad]
#endif
    internal static class DisplayXRDockModeWatcher
    {
#if UNITY_EDITOR_WIN
        const double kStableSeconds  = 0.75; // new dock state must hold this long
        const double kMinRestartGap  = 2.0;  // never cycle the subsystem faster
        const double kTogglePollGap  = 0.25; // test-hook file poll cadence

        static int    s_gate = -1;           // DISPLAYXR_PROV_TEXTURE_PROBE cache
        static bool   s_pendingState;
        static double s_pendingSince = -1.0;
        static double s_lastRestart  = -1e9;
        static double s_nextTogglePoll;
        static double s_nextDetect;          // detection cadence (reflection isn't free)
        static int    s_restartPhase;        // 0 idle, 1 = Stop issued, Start next tick
        static bool   s_warnedNoLoader;

        static DisplayXRDockModeWatcher()
        {
            EditorApplication.update += Poll;
        }

        static bool Enabled()
        {
            if (s_gate < 0)
            {
                // Inert unless the GameView texture-probe feature is on — and also when
                // the bind mode is env-pinned (test launchers): a dock transition can't
                // change the mode then, so a restart would be pure churn.
                bool on = System.Environment.GetEnvironmentVariable(
                              "DISPLAYXR_PROV_TEXTURE_PROBE") == "1"
                       && System.Environment.GetEnvironmentVariable(
                              "DISPLAYXR_PROV_PRESENT_MODE") == null;
                s_gate = on ? 1 : 0;
            }
            return s_gate == 1;
        }

        static DisplayXRDisplayLoader ActiveLoader =>
            XRGeneralSettings.Instance != null && XRGeneralSettings.Instance.Manager != null
                ? XRGeneralSettings.Instance.Manager.activeLoader as DisplayXRDisplayLoader
                : null;

        static void Poll()
        {
            if (!Enabled()) return;

            // Second half of a restart: issue Start() one tick after Stop() so the
            // native teardown (render-thread GfxStop → session_stop) has drained.
            if (s_restartPhase == 1)
            {
                s_restartPhase = 0;
                var l = ActiveLoader;
                if (l != null && EditorApplication.isPlaying)
                {
                    l.Start(); // runs ApplyDockModeForSessionStart → fresh bind mode
                    Debug.Log("[DisplayXR] Dock-transition restart: subsystem re-started");
                }
                return;
            }

            if (!EditorApplication.isPlaying) { s_pendingSince = -1.0; return; }
            if (!DisplayXRProviderDriver.IsActive) { s_pendingSince = -1.0; return; }

            double now = EditorApplication.timeSinceStartup;

            // Manual test hook: %TEMP%\dxr_dock_toggle → forced mode-flip restart.
            if (now >= s_nextTogglePoll)
            {
                s_nextTogglePoll = now + kTogglePollGap;
                string tf = Path.Combine(Path.GetTempPath(), "dxr_dock_toggle");
                if (File.Exists(tf))
                {
                    try { File.Delete(tf); } catch { }
                    bool flipped = !DisplayXRProviderDriver.LastAppliedDocked;
                    DisplayXRProviderDriver.ForceNextDockState(flipped);
                    Debug.Log("[DisplayXR] dxr_dock_toggle: forcing dock state -> " +
                              (flipped ? "DOCKED" : "UNDOCKED") + " (test hook)");
                    Restart(now);
                    return;
                }
            }

            // Real detection: compare the live dock state to the one the current
            // session was bound for; require it stable before restarting (dock and
            // undock re-host the pane through transient intermediate states).
            // Throttled — the reflection walk needn't run every editor tick.
            if (now < s_nextDetect) return;
            s_nextDetect = now + kTogglePollGap;
            if (!DisplayXRProviderDriver.TryDetectDockStateFresh(out bool docked))
            { s_pendingSince = -1.0; return; }

            if (docked == DisplayXRProviderDriver.LastAppliedDocked)
            { s_pendingSince = -1.0; return; }

            if (s_pendingSince < 0.0 || s_pendingState != docked)
            {
                s_pendingState = docked;
                s_pendingSince = now;
                return;
            }
            if (now - s_pendingSince < kStableSeconds) return;
            if (now - s_lastRestart < kMinRestartGap) return;

            Debug.Log("[DisplayXR] Game view dock state changed mid-Play (" +
                      (docked ? "now DOCKED" : "now UNDOCKED") +
                      ") — restarting display subsystem to re-bind");
            Restart(now);
        }

        static void Restart(double now)
        {
            var loader = ActiveLoader;
            if (loader == null)
            {
                if (!s_warnedNoLoader)
                {
                    s_warnedNoLoader = true;
                    Debug.LogWarning("[DisplayXR] Dock transition detected but no active " +
                        "DisplayXRDisplayLoader — cannot restart the subsystem.");
                }
                return;
            }
            s_lastRestart  = now;
            s_pendingSince = -1.0;
            loader.Stop();       // driver destroyed + StopSubsystem → native session teardown
            s_restartPhase = 1;  // Start() next editor tick
        }
#endif // UNITY_EDITOR_WIN
    }
}
