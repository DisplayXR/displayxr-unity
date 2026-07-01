// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0

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
        /// Request a transparent background for the provider session (#166 Phase A).
        /// Must be called BEFORE the session starts (e.g. from a transparent app's
        /// bootstrap, mirroring <see cref="DisplayXRTransparentOverlay.RequestTransparentSession"/>
        /// on the hook path). Opts the session into ALPHA_BLEND +
        /// transparentBackgroundEnabled so the runtime's DComp overlay composites the
        /// woven 3D over the desktop; only takes effect if the runtime advertises
        /// ALPHA_BLEND. Safe to call whether or not the provider ends up active.
        /// </summary>
        public static void RequestTransparentBackground(bool enabled = true) =>
            DisplayXRProviderNative.dxr_prov_set_transparent_background(enabled ? 1 : 0);

        /// <summary>
        /// Set the single 3D-zone rect (client-window pixels) the runtime frames the
        /// Kooima 3D into (#166 Phase B) — the provider analog of
        /// <see cref="DisplayXRNative.displayxr_set_3d_zone_rect"/> on the hook path.
        /// Seed early (SubsystemRegistration) so the swapchain is born zone-sized.
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

        internal static void OnSessionStarted() => RefreshModes();

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
                EyeTrackingStateChanged?.Invoke(tracking != 0, mode);
        }
    }
}
