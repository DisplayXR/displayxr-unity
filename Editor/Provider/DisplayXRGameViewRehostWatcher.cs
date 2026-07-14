// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

using UnityEditor;
using UnityEngine;
using UnityEngine.XR.Management;
using DisplayXR;

namespace DisplayXR.Editor
{
    /// <summary>
    /// (#740) Mid-Play GameView RE-HOST watcher for the weave-to-texture hybrid.
    /// A layout reset (Window &gt; Layouts &gt; Default / Reset All Layouts) destroys and
    /// recreates every EditorWindow — including the GameView the glue is bound to.
    /// The per-frame glue cannot be trusted THROUGH that transition: the transient
    /// GameView instances yield reflection readings that are internally consistent but
    /// wrong (observed: the logical-vs-physical units comparison flipped against a
    /// transient host → a ×2.5 rect glued the weave window huge), and the session's
    /// bind-time state (born rect, zone) is stale regardless. A fresh
    /// <c>loader.Start()</c> on the SETTLED layout is already proven correct (Play
    /// stop/start always recovers), so the solid fix is: detect the re-host, wait for
    /// the layout to settle, and restart the display subsystem.
    ///
    /// Detection = the set of live <c>UnityEditor.GameView</c> instance IDs changed
    /// since the current session bound. Maximize/restore, dock/undock moves and tab
    /// resizes keep the same instances (the lockstep glue handles those, verified) —
    /// only a genuine window recreation trips this watcher.
    ///
    /// It deliberately lives OUTSIDE <see cref="DisplayXRProviderDriver"/>:
    /// <c>loader.Stop()</c> destroys the driver GameObject, so a driver-hosted watcher
    /// would destroy itself mid-restart and never issue the Start(). Stop and Start
    /// run on SEPARATE editor ticks so the render thread finishes the native session
    /// teardown before the new session's start is queued. (Same architecture as the
    /// auto-switch dock watcher; this is its re-host sibling and the base it will
    /// merge into.)
    ///
    /// Editor+Windows only; inert unless the texture probe
    /// (DISPLAYXR_PROV_TEXTURE_PROBE=1) is active. Unlike the dock watcher this stays
    /// ACTIVE when the bind mode is env-pinned (DISPLAYXR_PROV_PRESENT_MODE) — a
    /// re-host restart re-binds the SAME mode; it just needs the fresh settled rect.
    /// </summary>
#if UNITY_EDITOR_WIN
    [InitializeOnLoad]
#endif
    internal static class DisplayXRGameViewRehostWatcher
    {
#if UNITY_EDITOR_WIN
        const double kStableSeconds = 0.75; // new instance set must hold this long
        const double kMinRestartGap = 2.0;  // never cycle the subsystem faster
        const double kDetectGap     = 0.25; // detection cadence

        static int         s_gate = -1;     // DISPLAYXR_PROV_TEXTURE_PROBE cache
        static System.Type s_gvType;
        static string      s_boundSig;      // GameView instance-set signature at session bind
        static bool        s_haveBoundSig;
        static string      s_pendingSig;
        static double      s_pendingSince = -1.0;
        static double      s_lastRestart  = -1e9;
        static double      s_nextDetect;
        // Recovery state machine: 0 idle → 1 heal-maximize issued → 2 heal-restore issued →
        // 3 Stop issued (Start next tick). The heal comes FIRST: Unity's recreated GameView
        // after an in-Play layout reset is DPI-corrupted (its view rect reports physical px
        // as points → Unity allocates a ×ppp-doubled target); a maximize/restore cycle
        // rebuilds the pane GUIView with correct DPI state (proven live), and only then is a
        // fresh subsystem bind worth doing.
        static int         s_restartPhase;
        static double      s_phaseAt;       // when the current phase may advance
        static bool        s_wasActive;
        static bool        s_warnedNoLoader;

        static DisplayXRGameViewRehostWatcher()
        {
            EditorApplication.update += Poll;
        }

        static bool Enabled()
        {
            if (s_gate < 0)
                s_gate = System.Environment.GetEnvironmentVariable(
                             "DISPLAYXR_PROV_TEXTURE_PROBE") == "1" ? 1 : 0;
            return s_gate == 1;
        }

        static DisplayXRDisplayLoader ActiveLoader =>
            XRGeneralSettings.Instance != null && XRGeneralSettings.Instance.Manager != null
                ? XRGeneralSettings.Instance.Manager.activeLoader as DisplayXRDisplayLoader
                : null;

        // Signature of the live GameView EditorWindow instances (sorted instance IDs).
        // Recreation (layout reset) changes it; re-parenting (dock/maximize) does not.
        static string CurrentSignature()
        {
            if (s_gvType == null)
            {
                s_gvType = System.Type.GetType("UnityEditor.GameView,UnityEditor");
                if (s_gvType == null) return null;
            }
            var views = Resources.FindObjectsOfTypeAll(s_gvType);
            var ids = new System.Collections.Generic.List<int>(views.Length);
            foreach (var v in views) ids.Add(v.GetInstanceID());
            ids.Sort();
            return string.Join(",", ids);
        }

        // Set EditorWindow.maximized on the main GameView (heal cycle). Guarded — a
        // floating GameView can't maximize; treat failure as "skip the heal".
        static bool SetGameViewMaximized(bool maximized)
        {
            if (s_gvType == null) return false;
            try
            {
                var views = Resources.FindObjectsOfTypeAll(s_gvType);
                if (views.Length == 0) return false;
                var win = views[0] as EditorWindow;
                if (win == null) return false;
                win.maximized = maximized;
                return true;
            }
            catch { return false; }
        }

        static void Poll()
        {
            if (!Enabled()) return;

            // Recovery state machine (see s_restartPhase). Each step waits for s_phaseAt so
            // the editor gets a few ticks to process the previous window operation.
            if (s_restartPhase != 0)
            {
                double tnow = EditorApplication.timeSinceStartup;
                if (tnow < s_phaseAt) return;
                switch (s_restartPhase)
                {
                case 1: // heal step 1: maximize (rebuilds the pane GUIView)
                    if (!SetGameViewMaximized(true))
                    {
                        s_restartPhase = 3; // can't maximize (floating) — restart only
                        s_phaseAt = tnow + 0.25;
                        return;
                    }
                    s_restartPhase = 2;
                    s_phaseAt = tnow + 0.5;
                    return;
                case 2: // heal step 2: restore
                    SetGameViewMaximized(false);
                    s_restartPhase = 3;
                    s_phaseAt = tnow + 0.5;
                    return;
                case 3: // stop the subsystem (driver destroyed, native session teardown)
                    {
                        var loader = ActiveLoader;
                        if (loader == null || !EditorApplication.isPlaying) { s_restartPhase = 0; return; }
                        loader.Stop();
                        s_restartPhase = 4;
                        s_phaseAt = 0.0; // next tick
                    }
                    return;
                case 4: // start it fresh: reruns TryPushInitialGameViewRect on the healed layout
                    s_restartPhase = 0;
                    var l = ActiveLoader;
                    if (l != null && EditorApplication.isPlaying)
                    {
                        l.Start();
                        Debug.Log("[DisplayXR] GameView re-host recovery: healed (max/restore) + subsystem re-started");
                    }
                    return;
                }
            }

            bool active = EditorApplication.isPlaying && DisplayXRProviderDriver.IsActive;
            if (!active)
            {
                s_wasActive = false;
                s_haveBoundSig = false;
                s_pendingSince = -1.0;
                return;
            }

            double now = EditorApplication.timeSinceStartup;
            if (now < s_nextDetect) return;
            s_nextDetect = now + kDetectGap;

            // Capture the baseline on the driver's rising edge (per session bind — a
            // restart re-captures against the NEW instances automatically).
            if (!s_wasActive || !s_haveBoundSig)
            {
                s_wasActive = true;
                string sig0 = CurrentSignature();
                if (string.IsNullOrEmpty(sig0)) return; // no GameView yet — wait
                s_boundSig = sig0;
                s_haveBoundSig = true;
                s_pendingSince = -1.0;
                return;
            }

            string sig = CurrentSignature();
            if (string.IsNullOrEmpty(sig)) { s_pendingSince = -1.0; return; } // mid-transition
            if (sig == s_boundSig) { s_pendingSince = -1.0; return; }

            if (s_pendingSince < 0.0 || s_pendingSig != sig)
            {
                s_pendingSig = sig;
                s_pendingSince = now;
                return;
            }
            if (now - s_pendingSince < kStableSeconds) return;
            if (now - s_lastRestart < kMinRestartGap) return;

            Debug.Log("[DisplayXR] GameView re-hosted mid-Play (layout change: instances " +
                      s_boundSig + " -> " + sig + ") — healing view + restarting display subsystem");
            Restart(now);
        }

        static void Restart(double now)
        {
            if (ActiveLoader == null)
            {
                if (!s_warnedNoLoader)
                {
                    s_warnedNoLoader = true;
                    Debug.LogWarning("[DisplayXR] GameView re-host detected but no active " +
                        "DisplayXRDisplayLoader — cannot restart the subsystem.");
                }
                return;
            }
            s_lastRestart  = now;
            s_pendingSince = -1.0;
            s_haveBoundSig = false; // re-baseline after the recovery
            s_restartPhase = 1;     // heal (maximize → restore) then Stop/Start — see Poll()
            s_phaseAt = now + 0.25;
        }
#endif // UNITY_EDITOR_WIN
    }
}
