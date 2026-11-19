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
    /// <see cref="DisplayXRProviderDriver"/>; the mode/hardware/eye-tracking events only
    /// fire while the provider session is running, and <see cref="SessionStarted"/> /
    /// <see cref="SessionStopped"/> bracket that window. All members are inert (no-ops /
    /// empty) when the provider isn't the active display subsystem.
    ///
    /// Requests (<see cref="RequestDisplayMode"/> etc.) made before the session is running
    /// return false and are DROPPED — see <see cref="SessionStarted"/> and
    /// <see cref="WhenRunning"/> for the supported way to make one at scene start.
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
        /// Fired once each time the provider's runtime session comes up — after
        /// <see cref="Modes"/> has been refreshed for the new session, and again after a
        /// restart (the editor's dock/undock auto-switch restarts the session mid-Play).
        /// <para>
        /// Why you want this: the session starts on the render thread a frame or two into
        /// Play, so the first scene's <c>Start()</c> normally runs while <see cref="IsRunning"/>
        /// is still false — and every request made then (<see cref="RequestDisplayMode"/>,
        /// <see cref="RequestRenderingMode"/>, …) returns false and is dropped, with nothing
        /// else to tell you it never landed. The panel boots in 3D, so a 2D scene that fires
        /// its request once at Start silently stays 3D. Subscribe here, or use
        /// <see cref="WhenRunning"/> for a one-shot.
        /// </para>
        /// <para>
        /// Edge-triggered: if the session is already running when you subscribe, this has
        /// already fired — check <see cref="IsRunning"/> too (or use <see cref="WhenRunning"/>,
        /// which does exactly that).
        /// </para>
        /// </summary>
        public static event Action SessionStarted;

        /// <summary>
        /// Fired once when a running session goes down — session loss, the editor's
        /// dock/undock restart, or the provider stopping. Anything the app pushed into the
        /// session (display mode, rendering mode, eye-tracking mode) is gone with it; expect a
        /// <see cref="SessionStarted"/> to follow on a restart and re-apply there.
        /// </summary>
        public static event Action SessionStopped;

        /// <summary>
        /// True between <see cref="SessionStarted"/> and <see cref="SessionStopped"/> — the
        /// managed view of the session. Distinct from <see cref="IsRunning"/>, which is the
        /// native flag and flips true on the render thread up to a frame before the driver has
        /// noticed and refreshed <see cref="Modes"/> (measured in the editor: IsRunning already
        /// true at frame 0, SessionStarted at frame 1). While this is true, <see cref="Modes"/>
        /// has been refreshed at least once for the current session.
        /// </summary>
        public static bool IsSessionReady { get; private set; }

        /// <summary>
        /// Run <paramref name="action"/> as soon as the session is up: immediately if
        /// <see cref="IsSessionReady"/>, otherwise once, on the next <see cref="SessionStarted"/>.
        /// This is the supported way to make an initial request from a scene's <c>Start()</c> —
        /// the plain <see cref="RequestDisplayMode"/> / <see cref="RequestRenderingMode"/> are
        /// dropped (return false) when the session isn't up yet, and at Start it usually isn't.
        /// <para>
        /// Gated on <see cref="IsSessionReady"/> rather than <see cref="IsRunning"/> on purpose:
        /// the native flag can be true before the driver's first <see cref="RefreshModes"/>, and
        /// an action that looks its mode up in <see cref="Modes"/> would then see an empty table.
        /// The cost is at most one frame of latency.
        /// </para>
        /// <para>
        /// One-shot by design: it does not re-run after a session restart. If your request
        /// must survive a restart (it does not — the session state is gone), subscribe to
        /// <see cref="SessionStarted"/> instead, or use <see cref="DisplayXRSceneMode"/>,
        /// which holds and re-arms its target for you.
        /// </para>
        /// </summary>
        public static void WhenRunning(Action action)
        {
            if (action == null) return;
            if (IsSessionReady) { action(); return; }
            Action handler = null;
            handler = () =>
            {
                SessionStarted -= handler;
                action();
            };
            SessionStarted += handler;
        }

        /// <summary>
        /// Latest eye-tracking state, cached from <see cref="EyeTrackingStateChanged"/>
        /// (the runtime emits an edge on session start) — whether a face is currently
        /// tracked, so editor runtime status can surface tracked/not-tracked in provider
        /// mode. For the tracked eye *positions* see <see cref="TryGetViewerEyes"/>.
        /// False until the first edge / when the provider isn't running.
        /// </summary>
        public static bool IsEyeTracked { get; private set; }

        /// <summary>Unity-world display-plane pose the driver last sent to the runtime
        /// via dxr_prov_set_display_pose, captured pre-XR-reset in LateUpdate. The URP
        /// foreground clip reads this so its plane tracks the moving rig (#166).</summary>
        public static Vector3 RigPlanePos     { get; internal set; }
        public static Vector3 RigPlaneForward { get; internal set; } = Vector3.forward;
        public static bool    RigPlaneValid   { get; internal set; }

        /// <summary>The rotation sent with <see cref="RigPlanePos"/>.
        /// <see cref="DisplayXRPicking"/> needs the whole pose to place the located views.</summary>
        internal static Quaternion RigPlaneRotation { get; set; } = Quaternion.identity;

        // The rig pose describes what one session was last sent. Forget it when that session
        // stops (the next one starts with no display pose until a rig pushes again) and at
        // the start of every Play: with Enter Play Mode Options (no domain reload) the
        // statics would otherwise carry the previous Play's pose into a scene that may have
        // no rig at all.
        [RuntimeInitializeOnLoadMethod(RuntimeInitializeLoadType.SubsystemRegistration)]
        static void ResetRigPlane()
        {
            RigPlaneValid = false;
            RigPlanePos = Vector3.zero;
            RigPlaneForward = Vector3.forward;
            RigPlaneRotation = Quaternion.identity;
        }

        static DisplayXRProviderNative.ModeInfo[] s_modes = Array.Empty<DisplayXRProviderNative.ModeInfo>();

        /// <summary>The enumerated rendering modes (refreshed at session start + on mode change).</summary>
        public static IReadOnlyList<DisplayXRProviderNative.ModeInfo> Modes => s_modes;

        /// <summary>modeIndex of the currently active rendering mode (0 if not running).</summary>
        public static uint ActiveModeIndex =>
            IsRunning ? DisplayXRProviderNative.dxr_prov_get_active_mode_index() : 0;

        /// <summary>Request a vendor rendering mode by modeIndex. Returns true on success.</summary>
        public static bool RequestRenderingMode(uint modeIndex) =>
            IsRunning && DisplayXRProviderNative.dxr_prov_request_rendering_mode(modeIndex) != 0;

        /// <summary>
        /// (#266) Move the app's window onto the DisplayXR panel. Returns true on success,
        /// including the no-op case where it is already there.
        /// <para>
        /// Needs a runtime advertising <c>XR_DXR_display_info</c> v16+; returns false
        /// against anything older, and false on macOS (Windows and Linux are supported).
        /// Call it once the session is running — the panel's desktop position is read at
        /// session start.
        /// </para>
        /// <para>
        /// The move is performed in native inside a per-monitor-DPI-aware context on
        /// purpose. Doing the equivalent from C# with <c>Screen.MoveMainWindowTo</c> reads
        /// Unity's virtualized geometry and mis-places the window on mixed-DPI
        /// multi-monitor setups — the same coordinate-space split diagnosed in #263 —
        /// while looking correct on every single-monitor machine.
        /// </para>
        /// </summary>
        public static bool MoveWindowToDisplay() =>
            IsRunning && DisplayXRProviderNative.dxr_prov_move_window_to_display() != 0;

        // The same move as part of the app's start (DisplayXRTargetDisplay's moveOnStart):
        // on Linux native Wayland it keeps the size the window opened with while nobody
        // has placed the window, as the start-up placement does, however late the
        // session's first frames come. Elsewhere it is MoveWindowToDisplay.
        internal static bool MoveWindowToDisplayAtStart()
        {
            if (!IsRunning) return false;
            try { return DisplayXRProviderNative.dxr_prov_move_window_to_display_at_start() != 0; }
            catch (System.EntryPointNotFoundException) { return MoveWindowToDisplay(); } // older native
        }

        /// <summary>
        /// (#266) The panel's origin on the Windows virtual desktop, in signed physical
        /// pixels. False when the runtime predates <c>XR_DXR_display_info</c> v16.
        /// <para>
        /// Informational only — for placing a window prefer
        /// <see cref="MoveWindowToDisplay"/>, which acts in the right DPI context.
        /// (0,0) is a legitimate origin, so never infer "unknown" from the value.
        /// </para>
        /// </summary>
        public static bool TryGetDisplayDesktopOrigin(out int x, out int y)
        {
            x = 0; y = 0;
            return IsRunning &&
                   DisplayXRProviderNative.dxr_prov_get_display_desktop_origin(out x, out y) != 0;
        }

        /// <summary>Request the hardware 2D/3D display state. Returns true on success.</summary>
        public static bool RequestDisplayMode(bool mode3d) =>
            IsRunning && DisplayXRProviderNative.dxr_prov_request_display_mode(mode3d ? 1 : 0) != 0;

        /// <summary>Request the eye-tracking mode (true = MANUAL, false = MANAGED). Returns true on success.</summary>
        public static bool SetEyeTrackingMode(bool manual) =>
            IsRunning && DisplayXRProviderNative.dxr_prov_set_eye_tracking_mode(manual ? 1 : 0) != 0;

        /// <summary>
        /// App-facing atlas screenshot via XR_DXR_atlas_capture (#140). The runtime
        /// reads back its own compositor atlas and writes
        /// "&lt;pathPrefix&gt;_atlas_&lt;viewCount&gt;_&lt;cols&gt;x&lt;rows&gt;.png" on the next composed
        /// frame (non-blocking). Only captures while weaving (3D). Returns true if the
        /// request was accepted (runtime advertises the extension and a session is live).
        /// </summary>
        public static bool CaptureAtlas(string pathPrefix, bool projectionOnly = true) =>
            IsRunning && DisplayXRProviderNative.dxr_prov_capture_atlas(pathPrefix, projectionOnly ? 1 : 0) != 0;

        /// <summary>
        /// Provider display geometry (XR_DXR_display_info).
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
        /// World-space positions of the two eyes the provider is rendering with this
        /// frame (Unity coords) — the render-ready per-eye poses from
        /// <c>xrLocateViews</c> after the runtime's Kooima math (<c>XR_DXR_view_rig</c>),
        /// so they are always consistent with what the viewer actually sees.
        ///
        /// Prefer this over <c>Camera.GetStereoViewMatrix()</c> for head-coupled
        /// effects. That API has been reported returning the same matrix for both
        /// eyes under the provider (#236), which silently freezes any effect built
        /// on it. The plugin never writes that cache — <c>SetStereoViewMatrix</c>
        /// went away with the #166 provider migration, and per-eye poses now reach
        /// Unity through the native frame desc (<c>deviceAnchorToEyePose</c>),
        /// consumed inside Unity's render loop. Whether Unity then populates the C#
        /// cache is Unity's behaviour and varies by configuration (it IS populated
        /// in a built player on Windows/D3D12). This API doesn't depend on it.
        ///
        /// Returns false when the provider isn't running, or the two eyes are still
        /// coincident (no located views yet) — fall back to the camera transform.
        /// </summary>
        public static bool TryGetViewerEyes(out Vector3 left, out Vector3 right)
        {
            left = default;
            right = default;
            if (s_NativeMissing) return false;
            Vector3 l = default, r = default;
            try
            {
                if (!IsRunning) return false;
                DisplayXRProviderNative.dxr_prov_get_eye_clip(0, out _, out float lx, out float ly, out float lz);
                DisplayXRProviderNative.dxr_prov_get_eye_clip(1, out _, out float rx, out float ry, out float rz);
                l = new Vector3(lx, ly, lz);
                r = new Vector3(rx, ry, rz);
            }
            // Head-coupled effects call this every frame, so a project without the
            // native plugin (unsupported platform) must not throw per-frame — latch
            // and go quiet, mirroring DisplayXRGizmoHelpers.TryGetLiveRawEyes.
            catch (DllNotFoundException) { s_NativeMissing = true; return false; }
            catch (EntryPointNotFoundException) { s_NativeMissing = true; return false; }

            // Coincident eyes = the provider hasn't delivered real per-eye views yet
            // (pre-first-frame zeros, or a head-center-only session).
            if ((l - r).sqrMagnitude < 1e-10f) return false;
            left = l;
            right = r;
            return true;
        }

        /// <summary>Latched once the native plugin turns out to be absent, so
        /// <see cref="TryGetViewerEyes"/> stops throwing on every frame.</summary>
        static bool s_NativeMissing;

        /// <summary>
        /// World-space viewer head position: the midpoint of
        /// <see cref="TryGetViewerEyes"/>. Returns false on the same conditions.
        /// </summary>
        public static bool TryGetViewerHead(out Vector3 head)
        {
            head = default;
            if (!TryGetViewerEyes(out Vector3 l, out Vector3 r)) return false;
            head = (l + r) * 0.5f;
            return true;
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

        // ---- Render scale (#475) ----------------------------------------------

        // What the app set from code (0 = nothing) and what the settings asset says
        // (0 = nothing). The native provider is pushed the first one that is set; it
        // stores the number itself, so a session restart (editor dock / undock) needs
        // no re-push.
        static float s_appRenderScale;
        static float s_settingsRenderScale;

        /// <summary>
        /// The resolution the 3D views are rendered at, as a multiple of the default
        /// (window pixels x the display mode's view scale): 0.5 renders a quarter of the
        /// pixels for frame rate, 1.5 supersamples for edge quality. The woven picture
        /// keeps its size - the runtime scales each view into place - so this trades
        /// sharpness for speed and nothing else. Range 0.25 to 2; a value the runtime's
        /// maximum image size cannot hold is reduced, and the log says so.
        ///
        /// <para><b>Reading</b> gives the scale in effect: this property's value when it
        /// was set, else the DisplayXR settings asset's Render Scale, else Unity's own
        /// knob (<c>XRSettings.eyeTextureResolutionScale</c>, or the URP asset's Render
        /// Scale, which URP writes over the former every frame), else 1. The
        /// <c>DISPLAYXR_RENDER_SCALE</c> environment variable overrides all of them.</para>
        ///
        /// <para><b>Setting</b> takes precedence over the settings asset and over Unity's
        /// knob, on every render pipeline. Set 0 (or call <see cref="ClearRenderScale"/>)
        /// to hand the decision back. It may be set at any time, before the session
        /// exists included. A change reallocates the eye textures, so it is applied once
        /// the value has held still for a quarter of a second: animate it freely, but do
        /// not expect each intermediate value to be rendered.</para>
        ///
        /// <para>Window-space UI, Local2D layers, zone rectangles, picking and the
        /// click-through mask are in window pixels and do not change.</para>
        /// </summary>
        public static float RenderScale
        {
            get
            {
                if (TryGetRenderScaleState(out var s) && s.requested > 0f) return s.requested;
                float o = DisplayXRRenderScaleMath.ResolveOverride(s_appRenderScale, s_settingsRenderScale);
                return o > 0f ? o : 1f;
            }
            set
            {
                s_appRenderScale = DisplayXRRenderScaleMath.Sanitize(value);
                PushRenderScale();
            }
        }

        /// <summary>
        /// Forget the value set through <see cref="RenderScale"/>: the settings asset's
        /// Render Scale applies again, or Unity's own knob when that is unset too.
        /// </summary>
        public static void ClearRenderScale()
        {
            s_appRenderScale = DisplayXRRenderScaleMath.Unset;
            PushRenderScale();
        }

        /// <summary>
        /// The render scale in detail: what was asked and by whom, what the eye textures
        /// were actually allocated at (<c>applied</c>, <c>width</c> x <c>height</c> from
        /// <c>baseWidth</c> x <c>baseHeight</c>), and whether a limit reduced it. False
        /// only when the native plug-in is not loaded. Sizes are 0 until a session has
        /// allocated its eye textures.
        /// </summary>
        public static bool TryGetRenderScaleState(out DisplayXRProviderNative.DxrRenderScaleState state)
        {
            state = default;
            try { return DisplayXRProviderNative.dxr_prov_get_render_scale_state(out state) != 0; }
            catch (DllNotFoundException) { return false; }
            catch (EntryPointNotFoundException) { return false; }
        }

        /// <summary>
        /// The settings asset's Render Scale (0 = not set). Called by the display loader
        /// with the value a build stored (players) and by the editor with the asset's
        /// current value (Play Mode); a value set from code keeps winning.
        /// </summary>
        internal static void SetSettingsRenderScale(float value)
        {
            s_settingsRenderScale = DisplayXRRenderScaleMath.Sanitize(value);
            PushRenderScale();
        }

        /// <summary>
        /// Display loader, before the subsystem starts: establish the native value for
        /// this run. It is pushed even when nothing is set - the native library outlives a
        /// Play session in the editor, and a value left by the previous run must not leak
        /// into this one.
        /// </summary>
        internal static void InitRenderScale()
        {
            if (!Application.isEditor)
            {
                // The editor pushes the asset's value itself (it can read the asset). A
                // player reads what the build wrote next to its data.
                if (TryReadPlayerSettingsRenderScale(out float v)) s_settingsRenderScale = v;
            }
            PushRenderScale();
            float o = DisplayXRRenderScaleMath.ResolveOverride(s_appRenderScale, s_settingsRenderScale);
            if (o > 0f)
                Debug.Log("[DisplayXR] Render scale: " + o.ToString("0.##", System.Globalization.CultureInfo.InvariantCulture)
                          + (s_appRenderScale > 0f ? " (DisplayXRProvider.RenderScale)" : " (DisplayXR settings)"));
        }

        static void PushRenderScale()
        {
            float o = DisplayXRRenderScaleMath.ResolveOverride(s_appRenderScale, s_settingsRenderScale);
            try { DisplayXRProviderNative.dxr_prov_set_render_scale(o); }
            catch (DllNotFoundException) { }
            catch (EntryPointNotFoundException) { }
        }

        static bool TryReadPlayerSettingsRenderScale(out float value)
        {
            value = DisplayXRRenderScaleMath.Unset;
            try
            {
                // Application.dataPath is <name>_Data on Windows / Linux and
                // <name>.app/Contents on macOS, where the engine data sits one level down.
                string root = Application.dataPath;
                if (Application.platform == RuntimePlatform.OSXPlayer)
                    root = System.IO.Path.Combine(root, "Resources", "Data");
                string path = System.IO.Path.Combine(root, DisplayXRRenderScaleMath.PlayerSettingsFile);
                if (!System.IO.File.Exists(path)) return false;
                return DisplayXRRenderScaleMath.TryParsePlayerSettingsJson(System.IO.File.ReadAllText(path), out value);
            }
            catch (Exception)
            {
                return false; // no file system access here (e.g. inside an APK): nothing stored
            }
        }

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

        /// <summary>
        /// Set 3D zone <paramref name="index"/>'s cosmetic edge feather radius in
        /// client-window pixels (0 = hard edge, the default). Zone edges are hard by
        /// default since runtime spec v3; a zone wanting a soft composite edge (e.g.
        /// the avatar's zone-vs-band transition) opts in with an explicit radius.
        /// The runtime clamps the radius to half the zone's shorter side and never
        /// feathers the hardware wish; runtimes predating spec v3 ignore the request.
        /// May be called any time (applies from the next submitted frame).
        /// </summary>
        public static void SetZoneFeather(int index, float featherPx) =>
            DisplayXRProviderNative.dxr_prov_set_zone_feather((uint)index, featherPx);

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
            IsSessionReady = true;
            // After RefreshModes on purpose: a subscriber that wants a mode (e.g. the mono
            // one for a 2D scene) can look it up in Modes right here. Note the table can
            // still be empty/stale for a moment in a built player (see DisplayXRSceneMode's
            // retry) — this event says "requests are no longer dropped", not "the mode table
            // is final".
            SessionStarted?.Invoke();
        }

        internal static void OnSessionStopped()
        {
            IsSessionReady = false;
            IsEyeTracked = false;
            ResetRigPlane();
            SessionStopped?.Invoke();
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
