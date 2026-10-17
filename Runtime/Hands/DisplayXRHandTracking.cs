// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Compiled only when com.unity.xr.hands >= 1.3.0 is installed (DISPLAYXR_XR_HANDS).

using System;
using System.Collections.Generic;
using UnityEngine;
using UnityEngine.XR.Hands;
using UnityEngine.XR.Hands.ProviderImplementation;

namespace DisplayXR.Hands
{
    /// <summary>
    /// Owns the <see cref="DisplayXRHandSubsystem"/>'s lifecycle and exposes its status.
    ///
    /// The subsystem rides the DisplayXR display loader through
    /// <see cref="DisplayXRCompanionSubsystems"/>: created when the display subsystem is,
    /// started/stopped with it (including the editor's dock/undock restart), destroyed
    /// with it. It is driven by XR Hands' own <c>SubsystemUpdater</c>, which updates it at
    /// the start of <c>EarlyUpdate.XRUpdate</c> and again before render — the same cadence
    /// Unity's OpenXR hand-tracking feature uses — so <c>XRHandTrackingEvents</c>,
    /// <c>XRHandSkeletonDriver</c> and <c>XRHandMeshController</c> work unchanged.
    ///
    /// <b>Frame.</b> Joint poses are expressed relative to the <b>rig camera's transform</b>
    /// (the display plane — what the provider sends the runtime as the rig pose), in the
    /// rig's world units. Parent your hand visuals under the rig camera (or transform the
    /// poses by it) and they land where the real hands are in front of the panel; see the
    /// Hand Tracking doc page for why that is the right origin.
    ///
    /// <b>Scale.</b> The runtime reports joints in metres relative to the physical panel.
    /// A rig whose virtual display is not the physical size (display-centric
    /// <c>virtualDisplayHeight</c>, or camera-centric FOV × convergence distance) scales
    /// the eye positions by that ratio, and the hands must follow or a fingertip on the
    /// glass would not touch the virtual display plane. <see cref="ResolvePoseScale"/>
    /// derives the ratio from the active rig every update; <see cref="PoseScaleOverride"/>
    /// pins it instead. Scale-as-zoom (a scaled rig transform) needs nothing here — it is
    /// folded out of the tunables before they reach the runtime, and parenting under the
    /// (scaled) rig camera applies it to the hands.
    /// </summary>
    public static class DisplayXRHandTracking
    {
        /// <summary>
        /// Master switch, read when the display loader initializes (set it before XR
        /// init, e.g. from a <c>[RuntimeInitializeOnLoadMethod(BeforeSplashScreen)]</c>).
        /// Default true. <c>DISPLAYXR_HANDS=0</c> in the environment also disables it.
        /// </summary>
        public static bool Enabled { get; set; } = true;

        /// <summary>The created subsystem, or null (not created, package-gated out, or disabled).</summary>
        public static DisplayXRHandSubsystem Subsystem { get; private set; }

        /// <summary>True while the subsystem exists and is running (between loader Start and Stop).</summary>
        public static bool IsRunning => Subsystem != null && Subsystem.running;

        /// <summary>
        /// True once the native session is up with <c>XR_EXT_hand_tracking</c> enabled and
        /// the system reporting <c>supportsHandTracking</c> — i.e. the runtime has a hand
        /// source right now. False until the session starts (a frame or two into Play) and
        /// on a runtime with no input provider serving hands; <see cref="IsRunning"/> can be
        /// true while this is false, in which case both <c>XRHand.isTracked</c> stay false.
        /// </summary>
        public static bool IsSupported
        {
            get
            {
                if (s_NativeMissing) return false;
                try { return DisplayXRHandNative.DisplayXR_HandTracking_IsSupported() != 0; }
                catch (DllNotFoundException) { s_NativeMissing = true; return false; }
                catch (EntryPointNotFoundException) { s_NativeMissing = true; return false; }
            }
        }

        /// <summary>
        /// Pin the metres→rig-units factor applied to joint positions and radii instead of
        /// deriving it from the active rig (null = derive, the default). 1 = raw metres.
        /// </summary>
        public static float? PoseScaleOverride { get; set; }

        /// <summary>The factor the last update applied (diagnostics / HUDs).</summary>
        public static float CurrentPoseScale { get; private set; } = 1f;

        static XRHandProviderUtility.SubsystemUpdater s_Updater;
        static bool s_Hooked;
        static bool s_NativeMissing;

        // SubsystemRegistration runs before XR Plug-in Management touches any loader, so
        // every stage the loader raises is observed. Guarded for domain-reload-off editors,
        // where statics (and these subscriptions) survive across Play sessions.
        [RuntimeInitializeOnLoadMethod(RuntimeInitializeLoadType.SubsystemRegistration)]
        static void Hook()
        {
            if (s_Hooked) return;
            s_Hooked = true;
            DisplayXRCompanionSubsystems.Initialized += OnLoaderInitialized;
            DisplayXRCompanionSubsystems.Started += OnLoaderStarted;
            DisplayXRCompanionSubsystems.Stopped += OnLoaderStopped;
            DisplayXRCompanionSubsystems.Deinitialized += OnLoaderDeinitialized;
        }

        static void OnLoaderInitialized()
        {
            if (Subsystem != null) return;
            if (!Enabled || Environment.GetEnvironmentVariable("DISPLAYXR_HANDS") == "0")
            {
                Debug.Log("[DisplayXR] Hands: disabled (DisplayXRHandTracking.Enabled / DISPLAYXR_HANDS=0)");
                return;
            }
            var descriptors = new List<XRHandSubsystemDescriptor>();
            SubsystemManager.GetSubsystemDescriptors(descriptors);
            foreach (var d in descriptors)
            {
                if (d.id != DisplayXRHandSubsystem.SubsystemId) continue;
                Subsystem = d.Create() as DisplayXRHandSubsystem;
                break;
            }
            if (Subsystem == null)
            {
                Debug.LogWarning("[DisplayXR] Hands: descriptor '" + DisplayXRHandSubsystem.SubsystemId +
                    "' not registered — XR Hands subsystem not created");
                return;
            }
            s_Updater = new XRHandProviderUtility.SubsystemUpdater(Subsystem);
            Debug.Log("[DisplayXR] Hands: XR Hands subsystem created (XR_EXT_hand_tracking via the " +
                "DisplayXR runtime; support is decided when the session starts)");
        }

        static void OnLoaderStarted()
        {
            if (Subsystem == null) return;
            Subsystem.Start();
            s_Updater?.Start();
        }

        static void OnLoaderStopped()
        {
            if (Subsystem == null) return;
            s_Updater?.Stop();
            Subsystem.Stop();
        }

        static void OnLoaderDeinitialized()
        {
            if (Subsystem == null) return;
            s_Updater?.Destroy();
            s_Updater = null;
            Subsystem.Destroy();
            Subsystem = null;
        }

        /// <summary>
        /// The metres→rig-units factor for this update: <see cref="PoseScaleOverride"/> if
        /// set, else the active rig's virtual display height over the physical panel
        /// height (display-centric: <c>virtualDisplayHeight</c>, 0 = physical;
        /// camera-centric: the vertical FOV's span at the convergence distance,
        /// 2·tan(vFov/2)/invConvergenceDistance). 1 when there is no rig, no display info,
        /// or the rig converges at infinity. Called by the provider; public for HUDs.
        /// </summary>
        public static float ResolvePoseScale()
        {
            float scale = PoseScaleOverride ?? ComputeAutoScale();
            if (!(scale > 0f) || float.IsInfinity(scale)) scale = 1f;
            CurrentPoseScale = scale;
            return scale;
        }

        static float ComputeAutoScale()
        {
            var cam = DisplayXRRigManager.ActiveCamera;
            if (cam == null) return 1f;

            DisplayXRTunables t;
            var disp = cam.GetComponent<DisplayXRDisplay>();
            if (disp != null && disp.enabled) t = disp.GetProviderTunables();
            else
            {
                var camRig = cam.GetComponent<DisplayXRCamera>();
                if (camRig == null || !camRig.enabled) return 1f; // rig-less camera: metres
                t = camRig.GetProviderTunables();
            }

            if (!DisplayXRProvider.TryGetDisplayInfo(out var di) || di.heightM <= 0f) return 1f;

            float virtualHeight;
            if (t.cameraCentricMode)
            {
                // fovOverride is tan(vFov/2); the display plane sits at the convergence
                // distance and the FOV spans it, so its height is 2·tan(vFov/2)·distance.
                if (t.invConvergenceDistance <= 0f || t.fovOverride <= 0f) return 1f;
                virtualHeight = 2f * t.fovOverride / t.invConvergenceDistance;
            }
            else
            {
                virtualHeight = t.virtualDisplayHeight; // raw field; 0 = physical height
                if (virtualHeight <= 0f) return 1f;
            }
            return virtualHeight / di.heightM;
        }
    }
}
