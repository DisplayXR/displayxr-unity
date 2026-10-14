// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

using System;
using UnityEngine;
#if HAS_INPUT_SYSTEM && ENABLE_INPUT_SYSTEM
using UnityEngine.InputSystem;
#endif

namespace DisplayXR
{
    /// <summary>
    /// The DisplayXR <b>depth-aware cursor</b> (<c>XR_DXR_cursor_depth</c>, runtime ADR-046).
    ///
    /// <para>
    /// <b>The problem.</b> The OS cursor is flat: it sits on the glass, at zero disparity.
    /// Hover it over content that pops OUT of the display and the cursor is drawn on top of
    /// pixels whose stereo depth says they are in front of it - the eyes get two contradicting
    /// depth cues. The fix is a cursor drawn in stereo, just in front of the nearest content
    /// under it.
    /// </para>
    /// <para>
    /// <b>Who does what.</b> The DisplayXR <i>runtime</i> draws that cursor (it looks like the
    /// user's own OS cursor, at its usual size) and owns all of its behaviour: never behind
    /// the glass, a small margin in front of the content, smooth rise/sink, and back to the
    /// plain OS cursor whenever the content under it is on or behind the glass. The plug-in
    /// sends the runtime the pointer every frame, tells it where the depth comes from, and
    /// hides the OS cursor exactly while the runtime reports it is drawing its own - so there
    /// is never a second cursor and never no cursor.
    /// </para>
    /// <para>
    /// <b>Opt-in, zero cost otherwise.</b> Nothing happens until <see cref="Enabled"/> is set
    /// (or a <see cref="DisplayXRDepthCursor"/> component is enabled in the scene). An older
    /// runtime simply keeps the plain OS cursor; see <see cref="Supported"/>.
    /// </para>
    /// <para>
    /// <b>Depth sources.</b>
    /// <list type="bullet">
    /// <item><see cref="DepthSource.Measured"/> (default): the runtime measures the depth
    /// under the cursor itself, by matching the two views Unity renders. Works for any content
    /// with no colliders and no code. Low-texture areas (sky, a flat wall) read as "on the
    /// glass", i.e. the plain OS cursor. Needs runtime spec v7.</item>
    /// <item><see cref="DepthSource.AppPoint"/>: the app says what is under the cursor. By
    /// default the plug-in raycasts the cursor footprint against 3D physics colliders
    /// (<see cref="UsePhysicsRaycast"/>); or set <see cref="CustomHitTest"/>, or call
    /// <see cref="SetHitPoint"/> every frame from <c>Update</c>. Exact, works on any texture.
    /// Needs runtime spec v5 and an active DisplayXR rig (<see cref="DisplayXRDisplay"/> or
    /// <see cref="DisplayXRCamera"/>).</item>
    /// </list>
    /// </para>
    /// </summary>
    public static class DisplayXRCursorDepth
    {
        /// <summary>Where the depth under the cursor comes from.</summary>
        public enum DepthSource
        {
            /// <summary>The runtime block-matches the rendered views (runtime spec v7+).</summary>
            Measured = 0,
            /// <summary>The app supplies the content point under the cursor (runtime spec v5+).</summary>
            AppPoint = 1,
        }

        /// <summary>What the cursor is doing right now.</summary>
        public enum CursorState
        {
            /// <summary><see cref="Enabled"/> is false: the plain OS cursor, nothing requested.</summary>
            Off,
            /// <summary>Enabled, but the runtime is too old for the chosen source (or the
            /// session is not running): the plain OS cursor.</summary>
            Unsupported,
            /// <summary>Enabled and supported, but nothing was requested this frame (pointer off
            /// the 3D canvas, cursor locked, no mouse).</summary>
            Idle,
            /// <summary>Requested; the content under the cursor is on or behind the glass, so the
            /// OS cursor IS the cursor.</summary>
            OnGlass,
            /// <summary>The runtime draws the stereo cursor in front of the glass; the OS cursor
            /// is hidden.</summary>
            Lifted,
        }

        /// <summary>
        /// Custom hit test for <see cref="DepthSource.AppPoint"/>: return true and the
        /// WORLD-space hit for one footprint ray, false for a miss. Called up to 20 times a
        /// frame (the cursor footprint seen by both eyes).
        /// </summary>
        public delegate bool HitTest(Ray ray, out Vector3 hitPoint);

        /// <summary>Turn the depth-aware cursor on. Default off.</summary>
        public static bool Enabled
        {
            get => s_Enabled;
            set => s_Enabled = value;
        }

        /// <summary>Where the depth comes from. Default <see cref="DepthSource.Measured"/>.</summary>
        public static DepthSource Source = DepthSource.Measured;

        /// <summary>
        /// How far in front of the content the cursor floats, in eye baselines (about 65 mm
        /// on the glass per 1.0). 0 = the runtime default (0.015, about 1 mm). Leave it at 0
        /// unless you have a reason: every DisplayXR app shares the default.
        /// </summary>
        public static float Margin = 0f;

        /// <summary>
        /// Hide the OS cursor (<c>Cursor.visible</c>) while the runtime draws its own. Default
        /// true. Set false to manage it yourself from <see cref="DepthCursorShownChanged"/>.
        /// (Over the plug-in's own presentation window the native side always hides it - that
        /// window sets its cursor itself, so <c>Cursor.visible</c> cannot reach it.)
        /// </summary>
        public static bool ManageOsCursor = true;

        /// <summary>
        /// <see cref="DepthSource.AppPoint"/> without <see cref="SetHitPoint"/> or
        /// <see cref="CustomHitTest"/>: raycast the footprint against 3D physics colliders.
        /// Default true.
        /// </summary>
        public static bool UsePhysicsRaycast = true;

        /// <summary>Layers the physics raycast considers. Default: every layer except Ignore
        /// Raycast (= <c>Physics.DefaultRaycastLayers</c>, spelled out so this file compiles in
        /// a project that disabled the built-in Physics module).</summary>
        public static int RaycastLayerMask = ~(1 << 2);

        /// <summary>Maximum physics raycast distance, world units.</summary>
        public static float RaycastMaxDistance = 1000f;

        /// <summary>Custom hit test for <see cref="DepthSource.AppPoint"/> (takes precedence
        /// over the physics raycast).</summary>
        public static HitTest CustomHitTest;

        /// <summary>Fired when the runtime starts (true) or stops (false) drawing the cursor.</summary>
        public static event Action<bool> DepthCursorShownChanged;

        /// <summary>The <c>XR_DXR_cursor_depth</c> version the runtime advertised; 0 = none
        /// (or below v5, which this plug-in cannot use).</summary>
        public static uint RuntimeSpecVersion { get; private set; }

        /// <summary>True when the running runtime supports the selected <see cref="Source"/>.</summary>
        public static bool Supported =>
            RuntimeSpecVersion >= (Source == DepthSource.Measured ? 7u : 5u);

        /// <summary>True while the runtime draws the stereo cursor (OS cursor hidden).</summary>
        public static bool WillDraw { get; private set; }

        /// <summary>Filtered cursor disparity in eye baselines (0 = on the glass, &lt; 0 = in
        /// front). Diagnostic.</summary>
        public static float Disparity { get; private set; }

        /// <summary>See <see cref="CursorState"/>.</summary>
        public static CursorState State { get; private set; }

        /// <summary>Log one line per <see cref="State"/> change. Default true.</summary>
        public static bool LogStateChanges = true;

        /// <summary>
        /// <see cref="DepthSource.AppPoint"/>: the nearest content point under the cursor this
        /// frame, in WORLD space. Call every frame from <c>Update</c>; it overrides the built-in
        /// raycast for this frame only. Use the rays from <see cref="GetFootprintRays"/> so a
        /// nearer edge next to the hotspot still lifts the cursor.
        /// </summary>
        public static void SetHitPoint(Vector3 worldPoint)
        {
            s_ExplicitFrame = Time.frameCount;
            s_ExplicitHas = true;
            s_ExplicitPoint = worldPoint;
        }

        /// <summary><see cref="DepthSource.AppPoint"/>: nothing of the app's content is under the
        /// cursor this frame (overrides the built-in raycast for this frame only).</summary>
        public static void ClearHitPoint()
        {
            s_ExplicitFrame = Time.frameCount;
            s_ExplicitHas = false;
        }

        /// <summary>
        /// The cursor FOOTPRINT rays in world space: from both eyes through the hotspot and a
        /// ring around the cursor body, built from the previous frame's views (the runtime's
        /// smoothing absorbs the one-frame lag). Cast them all and pass the nearest hit to
        /// <see cref="SetHitPoint"/>. Returns the ray count written (0..20); 0 when the pointer
        /// is off the 3D canvas, the display is in 2D, or no DisplayXR rig is active.
        /// </summary>
        public static int GetFootprintRays(Ray[] buffer)
        {
            if (buffer == null || buffer.Length == 0) return 0;
            int n = FetchRays();
            if (n > buffer.Length) n = buffer.Length;
            for (int i = 0; i < n; i++) buffer[i] = s_Rays[i];
            return n;
        }

        // ---------------------------------------------------------------------

        const int kMaxRays = 20;
        static bool s_Enabled;
        static readonly float[] s_RayBuf = new float[kMaxRays * 6];
        static readonly Ray[] s_Rays = new Ray[kMaxRays];
        static readonly float[] s_HitBuf = new float[kMaxRays * 3];
        static int s_ExplicitFrame = -1;
        static bool s_ExplicitHas;
        static Vector3 s_ExplicitPoint;
        static bool s_PushedEnabled;
        static DepthSource s_PushedSource;
        static float s_PushedMargin = float.NaN;
        static bool s_WeHidCursor;
        static bool s_LastShown;
        static bool s_StateKnown;
        static CursorState s_LoggedState;
        static bool s_NativeMissing;

        static int FetchRays()
        {
            uint n;
            try { n = DisplayXRProviderNative.dxr_prov_cursor_get_footprint_rays(s_RayBuf, kMaxRays); }
            catch (Exception) { return 0; }
            if (n > kMaxRays) n = kMaxRays;
            for (int i = 0; i < n; i++)
            {
                int o = i * 6;
                s_Rays[i] = new Ray(new Vector3(s_RayBuf[o], s_RayBuf[o + 1], s_RayBuf[o + 2]),
                                    new Vector3(s_RayBuf[o + 3], s_RayBuf[o + 4], s_RayBuf[o + 5]));
            }
            return (int)n;
        }

        static bool TryGetPointer(out Vector2 pos)
        {
            pos = default;
            if (Cursor.lockState == CursorLockMode.Locked) return false; // FPS-style: no pointer
#if HAS_INPUT_SYSTEM && ENABLE_INPUT_SYSTEM
            var m = Mouse.current;
            if (m != null)
            {
                pos = m.position.ReadValue(); // bottom-left origin, screen px
                return true;
            }
#endif
#if ENABLE_LEGACY_INPUT_MANAGER
            if (Input.mousePresent)
            {
                Vector3 mp = Input.mousePosition;
                pos = new Vector2(mp.x, mp.y);
                return true;
            }
#endif
            return false;
        }

        /// <summary>AppPoint: collect this frame's hits (Unity world) and hand them to native.</summary>
        static void PushAppPointHits()
        {
            uint count = 0;
            if (s_ExplicitFrame == Time.frameCount)
            {
                if (s_ExplicitHas)
                {
                    s_HitBuf[0] = s_ExplicitPoint.x;
                    s_HitBuf[1] = s_ExplicitPoint.y;
                    s_HitBuf[2] = s_ExplicitPoint.z;
                    count = 1;
                }
            }
            else if (CustomHitTest != null || UsePhysicsRaycast)
            {
                int n = FetchRays();
                for (int i = 0; i < n; i++)
                {
                    Vector3 p;
                    bool hit;
                    if (CustomHitTest != null)
                    {
                        hit = CustomHitTest(s_Rays[i], out p);
                    }
                    else
                    {
                        hit = PhysicsRaycast(s_Rays[i], out p);
                    }
                    if (!hit) continue;
                    s_HitBuf[count * 3 + 0] = p.x;
                    s_HitBuf[count * 3 + 1] = p.y;
                    s_HitBuf[count * 3 + 2] = p.z;
                    count++;
                }
            }
            DisplayXRProviderNative.dxr_prov_cursor_set_hits(s_HitBuf, count);
        }

        static bool PhysicsRaycast(Ray ray, out Vector3 point)
        {
#if DISPLAYXR_HAS_PHYSICS
            if (Physics.Raycast(ray, out RaycastHit h, RaycastMaxDistance, RaycastLayerMask,
                                QueryTriggerInteraction.Ignore))
            {
                point = h.point;
                return true;
            }
#endif
            point = default;
            return false;
        }

        /// <summary>
        /// Per-frame pump, called by <c>DisplayXRProviderDriver.LateUpdate</c> while the
        /// session runs. Apps do not call this.
        /// </summary>
        internal static void Tick()
        {
            if (s_NativeMissing) return;
            try
            {
                TickInner();
            }
            catch (Exception e) when (e is DllNotFoundException || e is EntryPointNotFoundException)
            {
                // An older native binary alongside newer managed code: degrade to the plain
                // OS cursor once, never throw per frame.
                s_NativeMissing = true;
                Debug.LogWarning("[DisplayXR] Depth-aware cursor unavailable: native plug-in has no " +
                                 "dxr_prov_cursor_* exports (stale binary?). " + e.Message);
                SetShown(false);
            }
        }

        static void TickInner()
        {
            // Push the configuration only on change (the native side logs each change).
            if (s_Enabled != s_PushedEnabled || Source != s_PushedSource || Margin != s_PushedMargin)
            {
                DisplayXRProviderNative.dxr_prov_cursor_configure(s_Enabled ? 1 : 0, (int)Source,
                                                                  Margin);
                s_PushedEnabled = s_Enabled;
                s_PushedSource = Source;
                s_PushedMargin = Margin;
            }

            if (s_Enabled)
            {
                bool havePointer = TryGetPointer(out Vector2 p);
                DisplayXRProviderNative.dxr_prov_cursor_set_pointer(
                    havePointer ? 1 : 0, p.x, p.y, Screen.width, Screen.height);
                if (Source == DepthSource.AppPoint) PushAppPointHits();
            }

            DisplayXRProviderNative.dxr_prov_cursor_get_state(out var st);
            RuntimeSpecVersion = st.runtimeSpec;
            WillDraw = s_Enabled && st.willDraw != 0;
            Disparity = st.disparity;
            if (!s_Enabled) State = CursorState.Off;
            else if (!Supported) State = CursorState.Unsupported;
            else if (st.requested == 0) State = CursorState.Idle;
            else State = WillDraw ? CursorState.Lifted : CursorState.OnGlass;

            SetShown(WillDraw);
            LogState();
        }

        static void SetShown(bool shown)
        {
            if (shown == s_LastShown) return;
            s_LastShown = shown;
            if (ManageOsCursor || s_WeHidCursor)
            {
                if (shown && ManageOsCursor)
                {
                    // Hide only a cursor that is showing, and remember that WE hid it, so an
                    // app that hides its own cursor never gets it shown back by us.
                    if (Cursor.visible)
                    {
                        Cursor.visible = false;
                        s_WeHidCursor = true;
                    }
                }
                else if (!shown && s_WeHidCursor)
                {
                    Cursor.visible = true;
                    s_WeHidCursor = false;
                }
            }
            DepthCursorShownChanged?.Invoke(shown);
        }

        static void LogState()
        {
            if (!LogStateChanges) return;
            // Lifted <-> OnGlass flips with every hover; log only the coarse states.
            CursorState coarse = State == CursorState.Lifted ? CursorState.OnGlass : State;
            if (s_StateKnown && s_LoggedState == coarse) return;
            s_StateKnown = true;
            s_LoggedState = coarse;
            if (coarse == CursorState.Unsupported)
                Debug.Log($"[DisplayXR] Depth-aware cursor: runtime XR_DXR_cursor_depth spec v{RuntimeSpecVersion} " +
                          $"does not support source {Source} (needs v{(Source == DepthSource.Measured ? 7 : 5)}) " +
                          "- keeping the plain OS cursor.");
            else
                Debug.Log($"[DisplayXR] Depth-aware cursor: {coarse} (source {Source}, runtime spec v{RuntimeSpecVersion})");
        }

        /// <summary>Session stopped: show the OS cursor again and forget the runtime's answers.
        /// The app's opt-in (<see cref="Enabled"/>, <see cref="Source"/>) survives restarts.</summary>
        internal static void Reset()
        {
            SetShown(false);
            WillDraw = false;
            Disparity = 0f;
            State = s_Enabled ? CursorState.Unsupported : CursorState.Off;
            s_StateKnown = false;
            // Re-push the config to the next session (native keeps it, but be explicit).
            s_PushedMargin = float.NaN;
        }
    }
}
