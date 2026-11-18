// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

using UnityEngine;

namespace DisplayXR
{
    /// <summary>
    /// Auto-convergence for the camera-centric rig (#461): keeps the zero-parallax "screen"
    /// plane on the subject as a free camera moves, and (optionally) backs the stereo
    /// strength off so the scene stays inside the comfort caps. Add it next to a
    /// <see cref="DisplayXRCamera"/>; it does nothing on a display-centric
    /// (<see cref="DisplayXRDisplay"/>) rig, whose screen plane is the authored display.
    ///
    /// <para><b>What it writes.</b> Only the rig's own tunables,
    /// <see cref="DisplayXRCamera.invConvergenceDistance"/> and (when
    /// <see cref="driveStereoStrength"/> is on) <see cref="DisplayXRCamera.ipdFactor"/>, every
    /// frame, so they reach the runtime through the rig's normal per-frame push (and so
    /// survive a session restart). The authored values are remembered and put back when the
    /// component is disabled. The runtime still owns the projection; the rear depth budget
    /// (<c>XR_DXR_depth_budget</c>) is not read, smoothed or written here.</para>
    ///
    /// <para><b>Where the screen plane goes.</b> Not on the mean depth - that lands on the big
    /// far plateau and pushes most of the frame out of the screen. It is the blend of the
    /// centre-weighted 80th percentile of nearness (1/depth) and the dominant depth clump
    /// outside the far background, so roughly a fifth of the frame sits in front of the
    /// screen. See <see cref="DisplayXRAutoConvergenceMath"/> and
    /// <c>docs~/architecture/auto-convergence.md</c>.</para>
    ///
    /// <para><b>Cuts.</b> Nothing is auto-detected. Call <see cref="ResetConvergence"/> after
    /// a camera teleport, a cut or a scene load: the next sample is followed over a few
    /// frames instead of over the slow time constant.</para>
    /// </summary>
    [AddComponentMenu("DisplayXR/Auto Convergence")]
    [DisallowMultipleComponent]
    public class DisplayXRAutoConvergence : MonoBehaviour
    {
        /// <summary>Where the depth samples come from.</summary>
        public enum Mode
        {
            /// <summary>Scene depth where the render pipeline provides a depth texture, else
            /// physics rays. This component never forces a depth pre-pass; on BiRP forward the
            /// DisplayXR rig's own foreground-clip pass already keeps the camera depth texture
            /// on, so Auto uses depth there too. Rays are the fallback for a custom pipeline
            /// or when no depth grid arrives.</summary>
            Auto = 0,
            /// <summary>A low-resolution grid of the camera depth texture (asynchronous
            /// readback; never waits on the GPU).</summary>
            Depth = 1,
            /// <summary>Physics rays through a grid over the view.</summary>
            Raycast = 2,
            /// <summary>A Transform, its Renderer bounds, or its DisplayXRContentBounds.</summary>
            Target = 3,
        }

        /// <summary>What actually supplied the last sample.</summary>
        public enum ActiveSource { None, Depth, Raycast, Target }

        [Tooltip("Auto: scene depth where the render pipeline provides a depth texture, else " +
                 "physics rays. Depth: scene depth only (URP: the camera Depth Texture is switched " +
                 "on while this is enabled; BiRP forward needs the camera to already render depth). " +
                 "Raycast: physics rays through a grid over the view. Target: converge on a " +
                 "Transform / its Renderer bounds / its DisplayXRContentBounds.")]
        public Mode mode = Mode.Auto;

        [Tooltip("Also reduce the rig's IPD Factor when the scene's nearest / farthest content " +
                 "would exceed the comfort caps: at once when a cap is reached, back up only " +
                 "gradually. Never raises it above the authored value.")]
        public bool driveStereoStrength = true;

        [Header("Convergence")]

        [Tooltip("Nearest the screen plane may come to the camera (rig units, metres).")]
        [Min(0.1f)]
        public float minConvergenceDistance = 0.3f;

        [Tooltip("Farthest the screen plane may go (rig units, metres).")]
        [Min(0.1f)]
        public float maxConvergenceDistance = 100f;

        [Tooltip("Time constant of the follower, seconds. 1.0-1.5 reads as a slow drift; " +
                 "0.6 visibly pumped on a panel.")]
        [Range(0.2f, 5f)]
        public float timeConstant = 1.2f;

        [Header("Comfort")]

        [Tooltip("User depth preference: scales both comfort caps. 1 = the full caps.")]
        [Range(0.05f, 1f)]
        public float depthScale = 1f;

        [Tooltip("Largest disparity allowed IN FRONT of the screen, in degrees seen from the " +
                 "display's nominal viewing distance.")]
        [Range(0.05f, 3f)]
        public float frontCapDegrees = DisplayXRAutoConvergenceMath.DefaultFrontCapDegrees;

        [Tooltip("Largest disparity allowed BEHIND the screen, in degrees seen from the " +
                 "display's nominal viewing distance. More is tolerated behind than in front.")]
        [Range(0.05f, 3f)]
        public float behindCapDegrees = DisplayXRAutoConvergenceMath.DefaultBehindCapDegrees;

        [Header("Sampling")]

        [Tooltip("Raycast source: layers the rays hit. Leave UI and a first-person weapon out. " +
                 "(The Depth source reads whatever wrote depth and cannot filter by layer - use " +
                 "Ignore Nearer Than for a first-person weapon there.)")]
        public LayerMask layerMask = ~((1 << 2) | (1 << 5));

        [Tooltip("Samples nearer than this (rig units, metres) are left out of the statistics: " +
                 "a first-person weapon or hands. 0 = off.")]
        [Min(0f)]
        public float ignoreNearerThan = 0f;

        [Tooltip("Raycast source: maximum ray length (world units).")]
        [Min(0.1f)]
        public float raycastMaxDistance = 1000f;

        [Tooltip("Raycast source: rays across the view.")]
        [Range(3, 24)]
        public int rayColumns = 9;

        [Tooltip("Raycast source: rays down the view.")]
        [Range(3, 24)]
        public int rayRows = 7;

        [Tooltip("Target source: converge on this Transform. If it (or a child) has a Renderer, " +
                 "or it carries a DisplayXRContentBounds, the bounds are used: the centre goes on " +
                 "the screen and the near / far faces size the stereo strength.")]
        public Transform target;

        [Tooltip("Seconds between depth samples. 0.125-0.2 (5-8 Hz) is plenty: the follower is " +
                 "far slower than that.")]
        [Range(0.05f, 1f)]
        public float samplePeriod = 0.15f;

        [Tooltip("Centre-weighted percentile of nearness the screen plane tracks: 80 puts about " +
                 "a fifth of the frame in front of the screen.")]
        [Range(50f, 95f)]
        public float percentile = 80f;

        [Tooltip("Screen plane = blend x percentile + (1 - blend) x dominant depth clump.")]
        [Range(0f, 1f)]
        public float blend = 0.5f;

        [Tooltip("Changes of the target smaller than this fraction of the comfort budget are ignored.")]
        [Range(0f, 0.2f)]
        public float deadBand = 0.02f;

        // ---- Read-only state (debug readout / HUDs) ---------------------------------------

        /// <summary>The source of the last sample (shows which branch Auto took).</summary>
        public ActiveSource Source { get; private set; }

        /// <summary>True while the component is driving the rig (active rig, session running,
        /// at least one confident sample).</summary>
        public bool IsDriving { get; private set; }

        /// <summary>The last sample was rejected (too little depth, low confidence, no result)
        /// and the previous target is being held.</summary>
        public bool IsHolding { get; private set; }

        /// <summary>Convergence distance being delivered, rig metres (+infinity = parallel).</summary>
        public float ConvergenceDistance => DisplayXRAutoConvergenceMath.DistanceFromInverse(m_Conv.x);

        /// <summary>Convergence distance the follower is heading for, rig metres.</summary>
        public float TargetConvergenceDistance => DisplayXRAutoConvergenceMath.DistanceFromInverse(m_Conv.target);

        /// <summary>IPD factor being delivered (== the authored one unless a cap bites).
        /// While the component runs, <c>rig.ipdFactor</c> holds THIS value, not the
        /// authored one: an app that saves its stereo settings must read
        /// <see cref="AuthoredIpdFactor"/> (and <see cref="AuthoredInvConvergenceDistance"/>).</summary>
        public float StereoStrength { get; private set; } = 1f;

        /// <summary>The rig's authored tunables: what is restored on disable, and the ceiling
        /// of <see cref="StereoStrength"/>.</summary>
        public float AuthoredIpdFactor => m_AuthoredIpd;
        /// <inheritdoc cref="AuthoredIpdFactor"/>
        public float AuthoredInvConvergenceDistance => m_AuthoredInv;

        /// <summary>Estimated disparity of the nearest / farthest content (2nd / 98th
        /// percentile) at the delivered settings, percent of the view width. Negative = in
        /// front of the screen.</summary>
        public float NearDisparityPercent { get; private set; }
        /// <inheritdoc cref="NearDisparityPercent"/>
        public float FarDisparityPercent { get; private set; }

        /// <summary>The caps in the same unit (percent of the view width), after
        /// <see cref="depthScale"/>. They are physical angles converted through the nominal
        /// viewing distance and the physical width of the app's canvas (its window).</summary>
        public float FrontCapPercent { get; private set; }
        /// <inheritdoc cref="FrontCapPercent"/>
        public float BehindCapPercent { get; private set; }

        /// <summary>Confidence of the last estimate (0..1), and the samples behind it.</summary>
        public float Confidence { get; private set; }
        /// <inheritdoc cref="Confidence"/>
        public int ValidSamples { get; private set; }
        /// <inheritdoc cref="Confidence"/>
        public int TotalSamples { get; private set; }

        /// <summary>Depth source only: mean depth (rig metres) of the TOP and BOTTOM third of
        /// the last grid; NaN when that third had no depth. A diagnostic for the grid's
        /// vertical orientation, which the bottom-row damping depends on: look at a floor
        /// from standing height - the bottom third must be the NEARER one. NaN whenever the
        /// current source is not Depth or the last grid is older than three sample periods. If it is the top
        /// third, the grid is upside down on this graphics API / pipeline (set
        /// <c>DISPLAYXR_CURSOR_DEPTH_FLIPY=1</c> and report it).</summary>
        public float GridTopThirdDepth { get; private set; } = float.NaN;
        /// <inheritdoc cref="GridTopThirdDepth"/>
        public float GridBottomThirdDepth { get; private set; } = float.NaN;

        /// <summary>
        /// Follow the next sample fast (a ramp of a few frames, never a one-frame snap) and
        /// take it as soon as possible. Call after a camera teleport, a cut, or a scene load.
        /// Depth samples drawn before this call are discarded, including one already on its
        /// way back from the GPU.
        /// </summary>
        public void ResetConvergence()
        {
            m_Conv.SnapNext(); // (strength: a cap still bites at once, and still recovers slowly)
            m_NextSampleTime = 0f;
            m_LastClumpQ = -1f;
            m_GridNotBeforeFrame = Time.frameCount;
        }

        /// <summary>Log the grid-orientation diagnostic once (see <see cref="GridTopThirdDepth"/>).</summary>
        [ContextMenu("Log Grid Orientation")]
        public void LogGridOrientation()
        {
            Debug.Log($"[DisplayXR] Auto-convergence grid orientation: top third {GridTopThirdDepth:F2} m, " +
                      $"bottom third {GridBottomThirdDepth:F2} m (source {Source}, {SystemInfo.graphicsDeviceType}). " +
                      "Looking at a floor, the bottom third must be the nearer one.", this);
        }

        // -----------------------------------------------------------------------------------

        const int kMaxSamples = DisplayXRCursorDepthPatch.Size * DisplayXRCursorDepthPatch.Size;
        const float kDepthFallbackSeconds = 1.0f;   // Auto: no grid for this long -> rays

        DisplayXRCamera m_Rig;
        Camera m_Camera;
        bool m_WarnedNoRig;
        float m_NextBindTime;

        // The authored tunables, and what we last wrote (to tell our writes from anyone else's).
        float m_AuthoredInv, m_AuthoredIpd;
        float m_WrittenInv = float.NaN, m_WrittenIpd = float.NaN;

        DisplayXRAutoConvergenceMath.Follower m_Conv;
        // The delivered strength: caps push it down at once, it only recovers gradually.
        DisplayXRAutoConvergenceMath.StrengthGovernor m_Strength;
        DisplayXRAutoConvergenceMath.Workspace m_Work;
        float[] m_Q, m_U, m_V;
        float m_NextSampleTime;
        float m_LastClumpQ = -1f;
        // The disparity model, refreshed every frame: content extremes of the last sample,
        // caps (fraction of the view width, after the depth scale), and k in rig units.
        float m_NearQ, m_FarQ;
        bool m_HaveExtremes; // a sample has reported the content extremes
        float m_K, m_CapFront, m_CapBehind;
        bool m_NoCanvasApi;

        // Depth grid bookkeeping: which grids are ours to consume.
        int m_GridSerial;
        int m_GridFrameConsumed = -1, m_GridNotBeforeFrame;
        float m_DepthWantedSince = -1f, m_LastGridTime = -1f;
        bool m_WasGated = true;

        Transform m_TargetCached;
        Renderer[] m_TargetRenderers;
        DisplayXRContentBounds m_TargetBounds;

        void OnEnable()
        {
            m_Camera = GetComponent<Camera>();
            m_Rig = null;
            m_NextBindTime = 0f;
            if (!TryBind() && !m_WarnedNoRig)
            {
                m_WarnedNoRig = true;
                Debug.LogWarning("[DisplayXR] DisplayXRAutoConvergence on '" + name + "' has no " +
                    "DisplayXRCamera next to it and does nothing until one is added. It drives the " +
                    "camera-centric rig only; a display-centric rig's screen plane is the authored display.", this);
            }
        }

        // Find the rig and start from its authored values. Also called (once a second) from
        // LateUpdate while there is no rig, so a rig added later is picked up.
        bool TryBind()
        {
            m_Rig = GetComponent<DisplayXRCamera>();
            if (m_Rig == null) return false;
            if (m_Camera == null) m_Camera = GetComponent<Camera>();
            m_Work ??= new DisplayXRAutoConvergenceMath.Workspace(kMaxSamples);
            m_Q ??= new float[kMaxSamples];
            m_U ??= new float[kMaxSamples];
            m_V ??= new float[kMaxSamples];

            m_AuthoredInv = m_Rig.invConvergenceDistance;
            m_AuthoredIpd = m_Rig.ipdFactor;
            m_WrittenInv = m_WrittenIpd = float.NaN;
            m_Conv.Reset(m_AuthoredInv);
            m_Strength.Reset(m_AuthoredIpd);
            StereoStrength = m_AuthoredIpd;
            m_NextSampleTime = 0f;
            m_LastClumpQ = -1f;
            m_NearQ = m_FarQ = 0f;
            m_HaveExtremes = false;
            GridTopThirdDepth = GridBottomThirdDepth = float.NaN;
            m_GridSerial = DisplayXRCursorDepthPatch.GridSerial;
            m_GridFrameConsumed = -1;
            m_GridNotBeforeFrame = Time.frameCount;
            m_DepthWantedSince = m_LastGridTime = -1f;
            m_WasGated = true;
            m_TargetCached = null;
            Source = ActiveSource.None;
            IsDriving = IsHolding = false;
            return true;
        }

        void OnDisable()
        {
            // Put the authored values back - unless someone else has written the field since
            // our last write, in which case theirs stands.
            if (m_Rig != null)
            {
                if (m_Rig.invConvergenceDistance == m_WrittenInv) m_Rig.invConvergenceDistance = m_AuthoredInv;
                if (m_Rig.ipdFactor == m_WrittenIpd) m_Rig.ipdFactor = m_AuthoredIpd;
            }
            m_WrittenInv = m_WrittenIpd = float.NaN;
            IsDriving = false;
            Source = ActiveSource.None;
        }

#if UNITY_EDITOR
        void OnValidate()
        {
            if (maxConvergenceDistance < minConvergenceDistance) maxConvergenceDistance = minConvergenceDistance;
        }
#endif

        void LateUpdate()
        {
            float now = Time.unscaledTime;
            if (m_Rig == null)
            {
                if (now < m_NextBindTime) return;
                m_NextBindTime = now + 1f;
                if (!TryBind()) return;
            }

            // A value in the rig's field that is not the one we wrote was set by someone else
            // (the inspector, an app slider): it is the new authored value.
            // (NaN = we have not written: the comparison is then true by definition.)
            if (m_Rig.invConvergenceDistance != m_WrittenInv) m_AuthoredInv = m_Rig.invConvergenceDistance;
            if (m_Rig.ipdFactor != m_WrittenIpd) m_AuthoredIpd = m_Rig.ipdFactor;

            // Same gates as the rig's own push: provider running, not the boot splash, and
            // this camera is the active rig. Otherwise hold - write nothing, sample nothing.
            if (!m_Rig.isActiveAndEnabled || !DisplayXRProviderDriver.IsActive ||
                DisplayXRRigManager.SplashActive || DisplayXRRigManager.ActiveCamera != m_Camera ||
                !DisplayXRProvider.IsRunning)
            {
                IsDriving = false;
                // Nothing that arrives while gated is ours: forget the depth clock (or Auto
                // would take one raycast sample on return, mixed in with depth) and stay in
                // step with the grid counter.
                m_DepthWantedSince = m_LastGridTime = -1f;
                m_GridSerial = DisplayXRCursorDepthPatch.GridSerial;
                m_WasGated = true;
                GridTopThirdDepth = GridBottomThirdDepth = float.NaN;
                return;
            }
            if (m_WasGated)
            {
                // (Re)activated: a grid drawn before now shows another rig's view, or this
                // one's before the gap.
                m_WasGated = false;
                m_GridNotBeforeFrame = Time.frameCount;
                m_NextSampleTime = 0f;
            }

            float scale = transform.lossyScale.y;
            if (scale < 1e-4f) scale = 1e-4f;
            RefreshModel(scale);

            // ---- Source ----
            Mode m = mode;
            bool wantDepth = m == Mode.Depth || m == Mode.Auto;
            bool depthWorks = false;
            if (wantDepth)
            {
                DisplayXRCursorDepthPatch.KeepGridAlive();
                if (m_DepthWantedSince < 0f) m_DepthWantedSince = now;
                float since = m_LastGridTime >= 0f ? now - m_LastGridTime : now - m_DepthWantedSince;
                depthWorks = !DisplayXRCursorDepthPatch.DepthUnavailable && since <= kDepthFallbackSeconds;
            }
            else
            {
                m_DepthWantedSince = m_LastGridTime = -1f;
            }

            // ---- Sample ----
            bool measured = false, tried = false;
            float screenQ = 0f;
            if (wantDepth && DisplayXRCursorDepthPatch.GridSerial != m_GridSerial)
            {
                // A grid arrived (asynchronously, a frame or two after it was asked for).
                // Take it only if it is new, drawn since the last reset / activation, and
                // drawn from this camera.
                m_GridSerial = DisplayXRCursorDepthPatch.GridSerial;
                int gridFrame = DisplayXRCursorDepthPatch.GridFrame;
                if (DisplayXRAutoConvergenceMath.AcceptGrid(gridFrame, m_GridFrameConsumed, m_GridNotBeforeFrame,
                        ReferenceEquals(DisplayXRCursorDepthPatch.GridCamera, m_Camera)))
                {
                    m_GridFrameConsumed = gridFrame;
                    m_LastGridTime = now;
                    depthWorks = !DisplayXRCursorDepthPatch.DepthUnavailable;
                    tried = true;
                    measured = MeasureGrid(scale, out screenQ);
                    Source = ActiveSource.Depth;
                }
            }
            if (now >= m_NextSampleTime)
            {
                m_NextSampleTime = now + samplePeriod;
                if (wantDepth) DisplayXRCursorDepthPatch.RequestGrid(); // also keeps probing in Auto
                if (m == Mode.Target)
                {
                    tried = true;
                    measured = MeasureTarget(scale, out screenQ);
                    Source = ActiveSource.Target;
                }
                else if (m == Mode.Raycast || (m == Mode.Auto && !depthWorks))
                {
                    tried = true;
                    measured = MeasureRays(scale, out screenQ);
                    Source = ActiveSource.Raycast;
                }
            }

            if (tried)
            {
                IsHolding = !measured;
                if (measured) Feed(screenQ);
            }

            // The grid-orientation readout describes the grid the Depth source is using NOW:
            // not one from an earlier place, kept while rays or a target supply the samples.
            if (Source != ActiveSource.Depth || m_LastGridTime < 0f || now - m_LastGridTime > 3f * samplePeriod)
                GridTopThirdDepth = GridBottomThirdDepth = float.NaN;

            // ---- Follow and write ----
            float dt = Time.unscaledDeltaTime;
            float inv;
            if (m_Conv.have)
            {
                IsDriving = true;
                inv = Mathf.Clamp(m_Conv.Step(dt, timeConstant), 0f, 10f);
                m_Rig.invConvergenceDistance = inv;
                m_WrittenInv = inv;
            }
            else
            {
                // Nothing confident yet: the authored convergence stands. The caps below
                // still apply to it once a sample has reported what is in the frame.
                IsDriving = false;
                inv = Mathf.Clamp(m_AuthoredInv, 0f, 10f);
            }

            if (driveStereoStrength)
            {
                // ONE state for the delivered strength. The caps, evaluated at the convergence
                // actually delivered this frame, push it down at once; it comes back only
                // gradually, at the follower's time constant - whatever lifted the cap (the
                // plane arriving, an intruder leaving while the convergence holds, a
                // low-confidence sample).
                float bound = m_HaveExtremes
                    ? DisplayXRAutoConvergenceMath.StrengthForCaps(
                        m_K, m_AuthoredIpd, inv, m_NearQ, m_FarQ, m_CapFront, m_CapBehind)
                    : m_AuthoredIpd;
                float f = m_Strength.Step(dt, timeConstant, m_AuthoredIpd, bound);
                if (m_HaveExtremes || !float.IsNaN(m_WrittenIpd))
                {
                    m_Rig.ipdFactor = f;
                    m_WrittenIpd = f;
                }
                StereoStrength = f;
            }
            else if (!float.IsNaN(m_WrittenIpd))
            {
                // Strength driving was switched off: hand the authored value back once.
                if (m_Rig.ipdFactor == m_WrittenIpd) m_Rig.ipdFactor = m_AuthoredIpd;
                m_WrittenIpd = float.NaN;
                m_Strength.Reset(m_AuthoredIpd);
                StereoStrength = m_AuthoredIpd;
            }
            else
            {
                m_Strength.Reset(m_AuthoredIpd);
                StereoStrength = m_AuthoredIpd;
            }

            NearDisparityPercent = 100f * DisplayXRAutoConvergenceMath.Disparity(m_K, StereoStrength, inv, m_NearQ);
            FarDisparityPercent = 100f * DisplayXRAutoConvergenceMath.Disparity(m_K, StereoStrength, inv, m_FarQ);
        }

        // The disparity model for this frame: the caps and k.
        //
        // Caps are PHYSICAL angles at the viewer, so they become a fraction of the view through
        // the physical width of what the view is shown on: the app's canvas (its window on the
        // panel), not the whole display - the same fraction of a small window is fewer
        // millimetres on the glass. The viewing distance is the display's NOMINAL one (a cap
        // that follows the live head makes the picture breathe).
        //
        // k is in rig units: the runtime keeps the eye baseline a physical distance in WORLD
        // units whatever the rig's scale, so in rig units it is ipd / scale.
        void RefreshModel(float scale)
        {
            float viewDist = 0f, viewWidth = 0f;
            if (DisplayXRProvider.TryGetDisplayInfo(out var di))
            {
                viewDist = di.nominalZ;
                viewWidth = di.widthM;
            }
            if (!m_NoCanvasApi)
            {
                try
                {
                    if (DisplayXRNative.displayxr_get_kooima_canvas(out _, out _, out int cw, out int ch,
                                                                    out float canvasW, out float canvasH) != 0 &&
                        cw > 0 && ch > 0 && canvasW > 0f && canvasH > 0f)
                        viewWidth = canvasW;
                }
                catch (System.Exception e) when (e is System.DllNotFoundException || e is System.EntryPointNotFoundException)
                {
                    m_NoCanvasApi = true; // older native binary: the display width stands in
                }
            }
            m_CapFront = depthScale * DisplayXRAutoConvergenceMath.CapFractionFromDegrees(frontCapDegrees, viewDist, viewWidth);
            m_CapBehind = depthScale * DisplayXRAutoConvergenceMath.CapFractionFromDegrees(behindCapDegrees, viewDist, viewWidth);
            FrontCapPercent = 100f * m_CapFront;
            BehindCapPercent = 100f * m_CapBehind;

            float aspect = m_Camera != null && m_Camera.aspect > 0.01f ? m_Camera.aspect : 16f / 9f;
            float tanHalfH = Mathf.Tan(m_Rig.AuthoredFieldOfView * 0.5f * Mathf.Deg2Rad) * aspect;
            m_K = DisplayXRAutoConvergenceMath.DisparityScale(
                DisplayXRAutoConvergenceMath.BaselineInRigUnits(DisplayXRAutoConvergenceMath.NominalIpdMeters, scale),
                tanHalfH);
        }

        // A confident measurement: clamp and hand it to the convergence follower. (The
        // strength needs no feeding: it is bounded every frame in LateUpdate.)
        void Feed(float screenQ)
        {
            float qMin = 1f / Mathf.Max(maxConvergenceDistance, minConvergenceDistance);
            float qMax = 1f / Mathf.Max(minConvergenceDistance, 0.1f);
            screenQ = Mathf.Clamp(screenQ, qMin, qMax);

            float kf = m_K * Mathf.Max(m_AuthoredIpd, 0.05f);
            float deadbandQ = kf > 0f ? deadBand * (m_CapFront + m_CapBehind) / kf : 0f;
            m_Conv.Measure(screenQ, deadbandQ);
        }

        // ---- Depth grid ----------------------------------------------------------------

        bool MeasureGrid(float scale, out float screenQ)
        {
            screenQ = 0f;
            int n = DisplayXRCursorDepthPatch.GridCount;
            if (n > kMaxSamples) n = kMaxSamples;
            float[] depth = DisplayXRCursorDepthPatch.GridDepth;
            float[] gu = DisplayXRCursorDepthPatch.GridU, gv = DisplayXRCursorDepthPatch.GridV;
            float nearLimit = ignoreNearerThan * scale; // rig metres -> world units
            float topSum = 0f, bottomSum = 0f;
            int topN = 0, bottomN = 0;
            for (int i = 0; i < n; i++)
            {
                float d = depth[i];
                float q;
                if (float.IsPositiveInfinity(d)) q = 0f;                       // sky
                else if (float.IsNaN(d) || d <= 1e-6f || d < nearLimit) q = float.NaN; // masked
                else
                {
                    q = scale / d;
                    // Orientation diagnostic (GridTopThirdDepth / GridBottomThirdDepth).
                    if (gv[i] > 2f / 3f) { topSum += d; topN++; }
                    else if (gv[i] < 1f / 3f) { bottomSum += d; bottomN++; }
                }
                m_Q[i] = q;
                m_U[i] = gu[i];
                m_V[i] = gv[i];
            }
            GridTopThirdDepth = topN > 0 ? topSum / topN / scale : float.NaN;
            GridBottomThirdDepth = bottomN > 0 ? bottomSum / bottomN / scale : float.NaN;
            return Estimate(n, out screenQ);
        }

        // ---- Physics rays --------------------------------------------------------------

        bool MeasureRays(float scale, out float screenQ)
        {
            screenQ = 0f;
#if DISPLAYXR_HAS_PHYSICS
            int cols = Mathf.Clamp(rayColumns, 1, 24), rows = Mathf.Clamp(rayRows, 1, 24);
            while (cols * rows > kMaxSamples) rows--;
            float aspect = m_Camera != null && m_Camera.aspect > 0.01f ? m_Camera.aspect : 16f / 9f;
            float tanV = Mathf.Tan(m_Rig.AuthoredFieldOfView * 0.5f * Mathf.Deg2Rad);
            float tanH = tanV * aspect;
            Transform t = transform;
            Vector3 origin = t.position;
            float nearLimit = ignoreNearerThan * scale;
            int n = 0;
            for (int r = 0; r < rows; r++)
            {
                float v = (r + 0.5f) / rows;
                for (int c = 0; c < cols; c++)
                {
                    float u = (c + 0.5f) / cols;
                    // The rig's authored frustum (Camera.fieldOfView is XR-written in Play).
                    var local = new Vector3((2f * u - 1f) * tanH, (2f * v - 1f) * tanV, 1f);
                    float len = local.magnitude;
                    Vector3 dir = t.TransformDirection(local) / len;
                    float q = 0f; // a miss is sky
                    if (Physics.Raycast(origin, dir, out RaycastHit hit, raycastMaxDistance, layerMask,
                                        QueryTriggerInteraction.Ignore))
                    {
                        float d = hit.distance / len; // along the camera's forward axis
                        q = (d <= 1e-6f || d < nearLimit) ? float.NaN : scale / d;
                    }
                    m_Q[n] = q;
                    m_U[n] = u;
                    m_V[n] = v;
                    n++;
                }
            }
            return Estimate(n, out screenQ);
#else
            return false; // no physics module in this project
#endif
        }

        bool Estimate(int n, out float screenQ)
        {
            var p = DisplayXRAutoConvergenceMath.Params.Default;
            p.percentile = percentile;
            p.blend = blend;
            p.preferQ = m_LastClumpQ;
            float histMax = 1f / Mathf.Max(minConvergenceDistance, 0.1f);
            bool ok = DisplayXRAutoConvergenceMath.EstimateScreenPlane(m_Q, m_U, m_V, n, histMax, in p, m_Work, out var est);
            Confidence = est.confidence;
            ValidSamples = est.validSamples;
            TotalSamples = est.samples;
            screenQ = est.screenQ;
            // The content extremes are known whenever there were samples at all - also when
            // the screen plane is not confident enough to move. The caps must see a small
            // near intruder even while the convergence holds.
            if (est.samples > 0)
            {
                m_NearQ = est.nearQ;
                m_FarQ = est.farQ;
                m_HaveExtremes = true;
            }
            if (!ok) return false;
            m_LastClumpQ = est.clumpQ;
            return true;
        }

        // ---- Target --------------------------------------------------------------------

        bool MeasureTarget(float scale, out float screenQ)
        {
            screenQ = 0f;
            if (target == null) return false;
            if (!ReferenceEquals(target, m_TargetCached))
            {
                m_TargetCached = target;
                m_TargetBounds = target.GetComponent<DisplayXRContentBounds>();
                m_TargetRenderers = target.GetComponentsInChildren<Renderer>(); // once per target
            }
            Transform t = transform;
            Vector3 fwd = t.forward;
            Vector3 centre = target.position;
            float reach = 0f; // half-depth of the bounds along the view axis
            bool haveBounds = false;
            Bounds b = default;
            if (m_TargetBounds != null && m_TargetBounds.LastReportValid)
            {
                b = m_TargetBounds.LastReportedBounds;
                haveBounds = true;
            }
            else if (m_TargetRenderers != null)
            {
                // The union of every enabled renderer under the target (a character is many).
                for (int i = 0; i < m_TargetRenderers.Length; i++)
                {
                    Renderer r = m_TargetRenderers[i];
                    if (r == null || !r.enabled || !r.gameObject.activeInHierarchy) continue;
                    if (!haveBounds) { b = r.bounds; haveBounds = true; }
                    else b.Encapsulate(r.bounds);
                }
            }
            if (haveBounds)
            {
                centre = b.center;
                Vector3 e = b.extents;
                reach = Mathf.Abs(e.x * fwd.x) + Mathf.Abs(e.y * fwd.y) + Mathf.Abs(e.z * fwd.z);
            }
            float d = Vector3.Dot(centre - t.position, fwd);
            if (d <= 1e-4f) return false; // behind the camera: hold
            screenQ = scale / d;
            m_NearQ = scale / Mathf.Max(d - reach, 1e-4f);
            m_FarQ = scale / (d + reach);
            m_HaveExtremes = true;
            Confidence = 1f;
            ValidSamples = TotalSamples = 1;
            return true;
        }
    }
}
