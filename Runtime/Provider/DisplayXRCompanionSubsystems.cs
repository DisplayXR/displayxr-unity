// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

using System;
using UnityEngine;

namespace DisplayXR
{
    /// <summary>
    /// Lifecycle hooks for <b>companion XR subsystems</b> that ride the DisplayXR loader
    /// but live in a separate, package-gated assembly — today the XR Hands subsystem
    /// (<c>Runtime/Hands/</c>, compiled only when <c>com.unity.xr.hands</c> is installed).
    ///
    /// The main assembly cannot reference those assemblies (they reference it, and an
    /// optional package must never be a hard dependency), so <see cref="DisplayXRDisplayLoader"/>
    /// raises these events at each XR Plug-in Management stage and a companion subscribes
    /// from a <c>[RuntimeInitializeOnLoadMethod(SubsystemRegistration)]</c> — which runs
    /// before XR Management initializes any loader, so no stage is ever missed.
    ///
    /// Order is the loader's: <see cref="Initialized"/> once the display subsystem exists
    /// (not raised when the loader declines, e.g. no runtime installed),
    /// <see cref="Started"/> after the display subsystem and provider driver are up,
    /// <see cref="Stopped"/> before they are torn down, <see cref="Deinitialized"/> before
    /// the display subsystem is destroyed. Start/Stop may cycle within one Play (the
    /// editor's dock/undock restart), so a companion must be restartable.
    /// </summary>
    public static class DisplayXRCompanionSubsystems
    {
        /// <summary>The DisplayXR display subsystem was created (loader <c>Initialize</c>).</summary>
        public static event Action Initialized;
        /// <summary>The display subsystem started (loader <c>Start</c>).</summary>
        public static event Action Started;
        /// <summary>The display subsystem is about to stop (loader <c>Stop</c>).</summary>
        public static event Action Stopped;
        /// <summary>The display subsystem is about to be destroyed (loader <c>Deinitialize</c>).</summary>
        public static event Action Deinitialized;

        internal static void RaiseInitialized() => Invoke(Initialized, nameof(Initialized));
        internal static void RaiseStarted() => Invoke(Started, nameof(Started));
        internal static void RaiseStopped() => Invoke(Stopped, nameof(Stopped));
        internal static void RaiseDeinitialized() => Invoke(Deinitialized, nameof(Deinitialized));

        // One failing companion must not take the display provider down with it, nor
        // starve the companions after it in the invocation list.
        static void Invoke(Action handlers, string stage)
        {
            if (handlers == null) return;
            foreach (Delegate d in handlers.GetInvocationList())
            {
                try { ((Action)d)(); }
                catch (Exception e)
                {
                    Debug.LogError("[DisplayXR] companion subsystem threw in " + stage + ": " + e.Message);
                    Debug.LogException(e);
                }
            }
        }
    }
}
