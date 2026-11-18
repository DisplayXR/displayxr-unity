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
            /// physics rays. (BiRP forward without a camera depth texture uses rays: a depth
            /// pre-pass is never forced on the app.)</summary>
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
                 "would exceed the comfort caps. Never raises it above the authored value.")]
        public bool driveStereoStrength = true;

        [Header("Convergence")]

        [Tooltip("Nearest the screen plane may come to the camera (rig units, metres).")]
        [Min(0.1f)]
        public float minConvergenceDistance = 0.3f;

        [Tooltip("Farthest the screen plane may go (rig units, metres).")]
        [Min(0.1f)]
        public float maxConvergenceDistance = 100f;

        [Tooltip("Time constant of the follower, seconds. 1.0-1.5 reads as a slow drift; " +
                 "below ~0.8 the depth visibly pumps.")]
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

        /// <summary>IPD factor being delivered (== the authored one unless a cap bites).</summary>
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
        /// <see cref="depthScale"/>.</summary>
        public float FrontCapPercent { get; private set; }
        /// <inheritdoc cref="FrontCapPercent"/>
        public float BehindCapPercent { get; private set; }

        /// <summary>Confidence of the last estimate (0..1), and the samples behind it.</summary>
        public float Confidence { get; private set; }
        /// <inheritdoc cref="Confidence"/>
        public int ValidSamples { get; private set; }
        /// <inheritdoc cref="Confidence"/>
        public int TotalSamples { get; private set; }

        /// <summary>
        /// Follow the next sample fast (a ramp of a few frames, never a one-frame snap) and
        /// take it as soon as possible. Call after a camera teleport, a cut, or a scene load.
        /// </summary>
        public void ResetConvergence()
        {
            m_Conv.SnapNext();
            m_Strength.SnapNext();
            m_NextSampleTime = 0f;
            m_LastClumpQ = -1f;
        }

        // -----------------------------------------------------------------------------------

        const int kMaxSamples = DisplayXRCursorDepthPatch.Size * DisplayXRCursorDepthPatch.Size;
        const float kDepthFallbackSeconds = 1.0f;   // Auto: no grid for this long -> rays
        const float kStrengthDownFactor = 0.35f;    // stereo backs off faster than it returns

        DisplayXRCamera m_Rig;
        Camera m_Camera;
        bool m_WarnedNoRig;

        // The authored tunables, and what we last wrote (to tell our writes from anyone else's).
        float m_AuthoredInv, m_AuthoredIpd;
        float m_WrittenInv = float.NaN, m_WrittenIpd = float.NaN;

        DisplayXRAutoConvergenceMath.Follower m_Conv, m_Strength;
        DisplayXRAutoConvergenceMath.Workspace m_Work;
        float[] m_Q, m_U, m_V;
        float m_NextSampleTime;
        float m_LastClumpQ = -1f;
        float m_NearQ, m_FarQ;
        float m_K;

        int m_GridSerial;
        float m_DepthWantedSince = -1f, m_LastGridTime = -1f;

        Transform m_TargetCached;
        Renderer m_TargetRenderer;
        DisplayXRContentBounds m_TargetBounds;

        void OnEnable()
        {
            m_Camera = GetComponent<Camera>();
            m_Rig = GetComponent<DisplayXRCamera>();
            if (m_Rig == null)
            {
                if (!m_WarnedNoRig)
                {
                    m_WarnedNoRig = true;
                    Debug.LogWarning("[DisplayXR] DisplayXRAutoConvergence on '" + name + "' has no " +
                        "DisplayXRCamera next to it and does nothing. It drives the camera-centric rig " +
                        "only; a display-centric rig's screen plane is the authored display.", this);
                }
                return;
            }
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
            m_GridSerial = DisplayXRCursorDepthPatch.GridSerial;
            m_DepthWantedSince = m_LastGridTime = -1f;
            m_TargetCached = null;
            Source = ActiveSource.None;
            IsDriving = IsHolding = false;
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
            if (m_Rig == null) return;

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
                m_DepthWantedSince = -1f;
                return;
            }

            float now = Time.unscaledTime;
            float scale = transform.lossyScale.y;
            if (scale < 1e-4f) scale = 1e-4f;

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
                m_GridSerial = DisplayXRCursorDepthPatch.GridSerial;
                m_LastGridTime = now;
                depthWorks = !DisplayXRCursorDepthPatch.DepthUnavailable;
                tried = true;
                measured = MeasureGrid(scale, out screenQ);
                Source = ActiveSource.Depth;
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

            // ---- Follow and write ----
            if (!m_Conv.have)
            {
                IsDriving = false;
                return; // nothing confident yet: the authored values stand
            }
            IsDriving = true;
            float dt = Time.unscaledDeltaTime;
            float inv = Mathf.Clamp(m_Conv.Step(dt, timeConstant), 0f, 10f);
            m_Rig.invConvergenceDistance = inv;
            m_WrittenInv = inv;

            if (driveStereoStrength && m_Strength.have)
            {
                float f = m_Strength.StepAsymmetric(dt, timeConstant, timeConstant * kStrengthDownFactor);
                f = Mathf.Clamp(f, 0f, m_AuthoredIpd); // never amplify
                m_Rig.ipdFactor = f;
                m_WrittenIpd = f;
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
                StereoStrength = m_AuthoredIpd;
            }

            NearDisparityPercent = 100f * DisplayXRAutoConvergenceMath.Disparity(m_K, StereoStrength, inv, m_NearQ);
            FarDisparityPercent = 100f * DisplayXRAutoConvergenceMath.Disparity(m_K, StereoStrength, inv, m_FarQ);
        }

        // A confident measurement: clamp, hand it to the followers, size the strength.
        void Feed(float screenQ)
        {
            float qMin = 1f / Mathf.Max(maxConvergenceDistance, minConvergenceDistance);
            float qMax = 1f / Mathf.Max(minConvergenceDistance, 0.1f);
            screenQ = Mathf.Clamp(screenQ, qMin, qMax);

            // Caps as a fraction of the view width, from the display's NOMINAL viewing
            // distance (a cap that follows the live head makes the picture breathe).
            float viewDist = 0f, viewWidth = 0f;
            if (DisplayXRProvider.TryGetDisplayInfo(out var di))
            {
                viewDist = di.nominalZ;
                viewWidth = di.widthM;
            }
            float capFront = depthScale * DisplayXRAutoConvergenceMath.CapFractionFromDegrees(frontCapDegrees, viewDist, viewWidth);
            float capBehind = depthScale * DisplayXRAutoConvergenceMath.CapFractionFromDegrees(behindCapDegrees, viewDist, viewWidth);
            FrontCapPercent = 100f * capFront;
            BehindCapPercent = 100f * capBehind;

            float aspect = m_Camera != null && m_Camera.aspect > 0.01f ? m_Camera.aspect : 16f / 9f;
            float tanHalfH = Mathf.Tan(m_Rig.AuthoredFieldOfView * 0.5f * Mathf.Deg2Rad) * aspect;
            m_K = DisplayXRAutoConvergenceMath.DisparityScale(DisplayXRAutoConvergenceMath.NominalIpdMeters, tanHalfH);

            float kf = m_K * Mathf.Max(m_AuthoredIpd, 0.05f);
            float deadbandQ = kf > 0f ? deadBand * (capFront + capBehind) / kf : 0f;
            m_Conv.Measure(screenQ, deadbandQ);

            float strength = DisplayXRAutoConvergenceMath.StrengthForCaps(
                m_K, m_AuthoredIpd, m_Conv.target, m_NearQ, m_FarQ, capFront, capBehind);
            m_Strength.Measure(strength, 0.02f * m_AuthoredIpd);
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
            for (int i = 0; i < n; i++)
            {
                float d = depth[i];
                float q;
                if (float.IsPositiveInfinity(d)) q = 0f;                       // sky
                else if (float.IsNaN(d) || d <= 1e-6f || d < nearLimit) q = float.NaN; // masked
                else q = scale / d;
                m_Q[i] = q;
                m_U[i] = gu[i];
                m_V[i] = gv[i];
            }
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
            if (!ok) return false;
            m_LastClumpQ = est.clumpQ;
            m_NearQ = est.nearQ;
            m_FarQ = est.farQ;
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
                m_TargetRenderer = target.GetComponentInChildren<Renderer>();
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
            else if (m_TargetRenderer != null)
            {
                b = m_TargetRenderer.bounds;
                haveBounds = true;
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
            Confidence = 1f;
            ValidSamples = TotalSamples = 1;
            return true;
        }
    }
}
