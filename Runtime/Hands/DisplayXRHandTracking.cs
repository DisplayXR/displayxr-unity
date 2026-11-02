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
    /// (what the provider sends the runtime as the rig pose: the display plane for a
    /// display-centric rig, the nominal viewer for a camera-centric one), in the rig's world
    /// units. Parent your hand visuals under the rig camera (or transform the
    /// poses by it) and they land where the real hands are in front of the panel; see the
    /// Hand Tracking doc page for why that is the right origin.
    ///
    /// <b>Placement.</b> The runtime reports joints in metres relative to the physical panel.
    /// <see cref="ResolveMapping"/> maps them the way the rig maps the eyes (the runtime's
    /// <c>XR_DXR_view_rig</c> rule for hand joints): rebased onto the canvas (the window on
    /// the panel), then for a display-centric rig scaled by the virtual-to-canvas height
    /// ratio, and for a camera-centric rig offset so the rig camera is the nominal viewer
    /// (the panel, and a hand in front of it, land in front of the camera). Without it a
    /// camera-centric rig put every joint behind its camera.
    /// <see cref="PoseScaleOverride"/> pins the scale instead. Scale-as-zoom (a scaled rig
    /// transform) needs nothing here — it is folded out of the tunables before they reach
    /// the runtime, and parenting under the (scaled) rig camera applies it to the hands.
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
        /// deriving it from the active rig (null = derive, the default). 1 = raw metres. The
        /// rig's origin (canvas centre, camera-centric nominal viewer) still applies.
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
        /// The joint → rig mapping for this update (see <see cref="DisplayXRHandRigMath"/>):
        /// the canvas centre is subtracted, a camera-centric rig moves the origin to the
        /// nominal viewer (the rig camera is the viewer there, so the panel sits
        /// <c>nominalViewerZ</c> in front of it), and the position is scaled by the rig's
        /// metres→rig-units factor (display-centric: <c>perspectiveFactor ·
        /// virtualDisplayHeight / canvas height</c>; camera-centric: 1, the
        /// <c>metersToVirtual</c> the provider sends). <see cref="PoseScaleOverride"/>, if
        /// set, replaces the scale only. Identity when there is no rig or no display info.
        /// Called by the provider every update; public for HUDs.
        /// </summary>
        public static DisplayXRHandRigMapping ResolveMapping()
        {
            var m = ComputeAutoMapping();
            s_AutoScale = m.scale > 0f && !float.IsInfinity(m.scale) ? m.scale : 1f;
            if (PoseScaleOverride.HasValue) m.scale = PoseScaleOverride.Value;
            if (!(m.scale > 0f) || float.IsInfinity(m.scale)) m.scale = 1f;
            CurrentPoseScale = m.scale;
            CurrentMapping = m;
            return m;
        }

        /// <summary>The scale part of <see cref="ResolveMapping"/> (kept for HUDs and older callers).</summary>
        public static float ResolvePoseScale() => ResolveMapping().scale;

        /// <summary>The mapping the last update applied (diagnostics / HUDs).</summary>
        public static DisplayXRHandRigMapping CurrentMapping { get; private set; } = DisplayXRHandRigMapping.Identity;

        static DisplayXRHandRigMapping ComputeAutoMapping()
        {
            var cam = DisplayXRRigManager.ActiveCamera;
            if (cam == null) return DisplayXRHandRigMapping.Identity;

            DisplayXRTunables t;
            var disp = cam.GetComponent<DisplayXRDisplay>();
            if (disp != null && disp.enabled) t = disp.GetProviderTunables();
            else
            {
                var camRig = cam.GetComponent<DisplayXRCamera>();
                if (camRig == null || !camRig.enabled) return DisplayXRHandRigMapping.Identity; // rig-less camera: metres
                t = camRig.GetProviderTunables();
            }

            if (!DisplayXRProvider.TryGetDisplayInfo(out var di) || di.heightM <= 0f)
                return DisplayXRHandRigMapping.Identity;

            // The canvas the runtime framed this frame's views into (the window on the
            // panel, published by the provider from the XR_DXR_view_rig raw channel); the
            // whole panel until the first located frame or on a runtime without it.
            var canvas = DisplayXRHandRigMath.FullPanel(di.heightM);
            if (TryGetLiveCanvas(out int rx, out int ry, out int rw, out int rh, out float ch) &&
                DisplayXRHandRigMath.TryCanvasFromPanelRect(rx, ry, rw, rh, ch,
                    di.pixelWidth, di.pixelHeight, di.widthM, di.heightM, out var live))
                canvas = live;

            if (t.cameraCentricMode)
                return DisplayXRHandRigMath.CameraRig(canvas, di.nominalZ, 1f);

            // The height the provider sends: the rig's field, or the physical panel when 0
            // (ps_display_rig_vh in native).
            float vdh = t.virtualDisplayHeight > 0f ? t.virtualDisplayHeight : di.heightM;
            return DisplayXRHandRigMath.DisplayRig(canvas, vdh, t.perspectiveFactor);
        }

        static bool s_RigFrameMissing;

        /// <summary>
        /// True when the last locate came back in the RIG frame (runtime <c>XR_DXR_view_rig</c>
        /// ≥ 4 with the rig chained on the joint locate): the runtime already applied the
        /// canvas rebase, the rig scale and the camera rig's viewer offset, so the C# mapping
        /// must not. False on older runtimes and older native binaries.
        /// </summary>
        public static bool JointsInRigFrame { get; private set; }

        internal static bool QueryRigFrame()
        {
            bool rig = false;
            if (!s_RigFrameMissing)
            {
                try { rig = DisplayXRHandNative.DisplayXR_HandTracking_IsRigFrame() != 0; }
                catch (DllNotFoundException) { s_RigFrameMissing = true; }
                catch (EntryPointNotFoundException) { s_RigFrameMissing = true; }
            }
            JointsInRigFrame = rig;
            return rig;
        }

        static float s_AutoScale = 1f;

        /// <summary>
        /// The mapping for rig-frame joints: identity, or — when
        /// <see cref="PoseScaleOverride"/> is set — the ratio that turns the runtime's rig
        /// scale (the same number <see cref="ResolveMapping"/> derives) into the override,
        /// about the rig camera, so the override means the same on both paths.
        /// </summary>
        internal static DisplayXRHandRigMapping RigFrameMapping()
        {
            var m = DisplayXRHandRigMapping.Identity;
            if (PoseScaleOverride.HasValue && PoseScaleOverride.Value > 0f && !float.IsInfinity(PoseScaleOverride.Value))
                m.scale = PoseScaleOverride.Value / s_AutoScale;
            return m;
        }

        static bool s_CanvasMissing;

        static bool TryGetLiveCanvas(out int x, out int y, out int w, out int h, out float heightM)
        {
            x = y = w = h = 0; heightM = 0f;
            if (s_CanvasMissing) return false;
            try
            {
                return DisplayXRNative.displayxr_get_kooima_canvas(out x, out y, out w, out h, out _, out heightM) != 0;
            }
            catch (DllNotFoundException) { s_CanvasMissing = true; return false; }
            catch (EntryPointNotFoundException) { s_CanvasMissing = true; return false; }
        }
    }
}
