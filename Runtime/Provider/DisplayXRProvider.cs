// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
using UnityEngine;

namespace DisplayXR
{
    /// <summary>
    /// App-facing facade for the custom IUnityXRDisplay Display Provider (epic
    /// #166): enumerated rendering modes, the active mode, mode/2D-3D/eye-tracking
    /// requests, and the runtime's mode/hardware/eye-tracking events.
    ///
    /// Mode *keybinding* is app policy — the plugin only exposes the API (cf. the
    /// windowing primitives). Events are pumped each frame by
    /// <see cref="DisplayXRProviderDriver"/>; they only fire while the provider
    /// session is running. All members are inert (no-ops / empty) when the provider
    /// isn't the active display subsystem.
    /// </summary>
    public static class DisplayXRProvider
    {
        /// <summary>True while the provider's runtime session is running.</summary>
        public static bool IsRunning => DisplayXRProviderNative.dxr_prov_session_is_running() != 0;

        /// <summary>Fired after the active rendering mode changes (previous, current modeIndex).</summary>
        public static event Action<uint, uint> RenderingModeChanged;

        /// <summary>Fired when the physical hardware 2D/3D state flips.</summary>
        public static event Action<bool> HardwareDisplayStateChanged;

        /// <summary>Fired on every edge of the derived eye-tracking state (isTracking, activeMode 0=MANAGED/1=MANUAL).</summary>
        public static event Action<bool, int> EyeTrackingStateChanged;

        /// <summary>
        /// Latest eye-tracking state, cached from <see cref="EyeTrackingStateChanged"/>
        /// (the runtime emits an edge on session start). The provider exposes no eye
        /// positions — only whether a face is currently tracked — so editor runtime
        /// status can surface tracked/not-tracked in provider mode. False until the
        /// first edge / when the provider isn't running.
        /// </summary>
        public static bool IsEyeTracked { get; private set; }

        /// <summary>Unity-world display-plane pose the driver last sent to the runtime
        /// via dxr_prov_set_display_pose, captured pre-XR-reset in LateUpdate. The URP
        /// foreground clip reads this so its plane tracks the moving rig (#166).</summary>
        public static Vector3 RigPlanePos     { get; internal set; }
        public static Vector3 RigPlaneForward { get; internal set; } = Vector3.forward;
        public static bool    RigPlaneValid   { get; internal set; }

        static DisplayXRProviderNative.ModeInfo[] s_modes = Array.Empty<DisplayXRProviderNative.ModeInfo>();

        /// <summary>The enumerated rendering modes (refreshed at session start + on mode change).</summary>
        public static IReadOnlyList<DisplayXRProviderNative.ModeInfo> Modes => s_modes;

        /// <summary>modeIndex of the currently active rendering mode (0 if not running).</summary>
        public static uint ActiveModeIndex =>
            IsRunning ? DisplayXRProviderNative.dxr_prov_get_active_mode_index() : 0;

        /// <summary>Request a vendor rendering mode by modeIndex. Returns true on success.</summary>
        public static bool RequestRenderingMode(uint modeIndex) =>
            IsRunning && DisplayXRProviderNative.dxr_prov_request_rendering_mode(modeIndex) != 0;

        /// <summary>Request the hardware 2D/3D display state. Returns true on success.</summary>
        public static bool RequestDisplayMode(bool mode3d) =>
            IsRunning && DisplayXRProviderNative.dxr_prov_request_display_mode(mode3d ? 1 : 0) != 0;

        /// <summary>Request the eye-tracking mode (true = MANUAL, false = MANAGED). Returns true on success.</summary>
        public static bool SetEyeTrackingMode(bool manual) =>
            IsRunning && DisplayXRProviderNative.dxr_prov_set_eye_tracking_mode(manual ? 1 : 0) != 0;

        /// <summary>
        /// App-facing atlas screenshot via XR_EXT_atlas_capture (#140). The runtime
        /// reads back its own compositor atlas and writes
        /// "&lt;pathPrefix&gt;_atlas_&lt;viewCount&gt;_&lt;cols&gt;x&lt;rows&gt;.png" on the next composed
        /// frame (non-blocking). Only captures while weaving (3D). Returns true if the
        /// request was accepted (runtime advertises the extension and a session is live).
        /// </summary>
        public static bool CaptureAtlas(string pathPrefix, bool projectionOnly = true) =>
            IsRunning && DisplayXRProviderNative.dxr_prov_capture_atlas(pathPrefix, projectionOnly ? 1 : 0) != 0;

        /// <summary>
        /// Provider display geometry (XR_EXT_display_info).
        /// Returns false if the provider isn't running or the runtime hasn't reported
        /// valid geometry yet.
        /// </summary>
        public static bool TryGetDisplayInfo(out DisplayXRProviderNative.DisplayInfo info)
        {
            info = default;
            if (!IsRunning) return false;
            DisplayXRProviderNative.dxr_prov_get_display_info(out info);
            return info.isValid != 0;
        }

        /// <summary>
        /// Request a transparent background for the provider session (#166 Phase A).
        /// Must be called BEFORE the session starts (e.g. from a transparent app's
        /// bootstrap, mirroring <see cref="DisplayXRTransparentOverlay.RequestTransparentSession"/>
        /// on the hook path). Opts the session into ALPHA_BLEND +
        /// transparentBackgroundEnabled so the runtime's DComp overlay composites the
        /// woven 3D over the desktop; only takes effect if the runtime advertises
        /// ALPHA_BLEND. Safe to call whether or not the provider ends up active.
        /// </summary>
        public static void RequestTransparentBackground(bool enabled = true)
        {
            s_TransparentBackgroundRequested = enabled;
            DisplayXRProviderNative.dxr_prov_set_transparent_background(enabled ? 1 : 0);
        }

        /// <summary>Latched by <see cref="RequestTransparentBackground"/>; read by the
        /// splash bootstrap to pick a transparent-friendly clear. Set via
        /// <see cref="DisplayXRTransparentOverlay.RequestTransparentSession"/>.</summary>
        internal static bool s_TransparentBackgroundRequested;

        /// <summary>
        /// Set the single 3D-zone rect (client-window pixels) the runtime frames the
        /// Kooima 3D into (#166 Phase B). Seed early (SubsystemRegistration) so the
        /// swapchain is born zone-sized.
        /// width&lt;=0||height&lt;=0 clears. Safe to call whether or not the provider is active.
        /// </summary>
        public static void SetZoneRect(int x, int y, int width, int height) =>
            DisplayXRProviderNative.dxr_prov_set_3d_zone_rect(x, y, width, height);

        /// <summary>Clear the 3D-zone rect (revert to full-window framing).</summary>
        public static void ClearZone() => DisplayXRProviderNative.dxr_prov_clear_3d_zone();

        /// <summary>
        /// Set the total number of 3D zones (#166 Phase B2): 1 primary + extras (max 4,
        /// Unity's render-pass cap). Call before setting the zone rects. Seed early
        /// (SubsystemRegistration) so the swapchains are born zone-sized.
        /// </summary>
        public static void SetZoneCount(int totalZones) =>
            DisplayXRProviderNative.dxr_prov_set_zone_count((uint)System.Math.Max(0, totalZones));

        /// <summary>
        /// Set 3D zone <paramref name="index"/>'s rect (client-window pixels). index 0 is
        /// the primary zone (equivalent to <see cref="SetZoneRect"/>); index &gt;= 1 are the
        /// extra zones. Each 3D zone weaves into its own window-pixel rect.
        /// </summary>
        public static void SetZone(int index, int zoneId, int x, int y, int width, int height) =>
            DisplayXRProviderNative.dxr_prov_set_zone((uint)index, (uint)zoneId, x, y, width, height);

        /// <summary>Re-read the enumerated modes from native. Called at session start + on mode change.</summary>
        public static void RefreshModes()
        {
            uint count = DisplayXRProviderNative.dxr_prov_get_mode_count();
            if (count == 0) { s_modes = Array.Empty<DisplayXRProviderNative.ModeInfo>(); return; }
            var modes = new DisplayXRProviderNative.ModeInfo[count];
            for (uint i = 0; i < count; i++)
                DisplayXRProviderNative.dxr_prov_get_mode_info(i, out modes[i]);
            s_modes = modes;
        }

        // ---- Called by DisplayXRProviderDriver -------------------------------

        internal static void OnSessionStarted()
        {
            IsEyeTracked = false;
            RefreshModes();
        }

        internal static void PumpEvents()
        {
            if (DisplayXRProviderNative.dxr_prov_consume_mode_changed(out uint prev, out uint cur) != 0)
            {
                RefreshModes();
                RenderingModeChanged?.Invoke(prev, cur);
            }
            if (DisplayXRProviderNative.dxr_prov_consume_hw_state_changed(out int hw3d) != 0)
                HardwareDisplayStateChanged?.Invoke(hw3d != 0);
            if (DisplayXRProviderNative.dxr_prov_consume_eye_tracking_changed(out int tracking, out int mode) != 0)
            {
                IsEyeTracked = tracking != 0;
                EyeTrackingStateChanged?.Invoke(tracking != 0, mode);
            }
        }
    }
}
