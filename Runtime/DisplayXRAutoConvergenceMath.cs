// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

using System;
using System.Globalization;

namespace DisplayXR
{
    /// <summary>
    /// Pure math for <c>DisplayXRAutoConvergence</c> (#461): which depth of the scene goes on
    /// the screen plane, how much stereo strength fits the comfort caps, and how both move
    /// over time. No UnityEngine types on purpose - it is unit-tested outside Unity
    /// (<c>Tests~/CursorDepthMath</c>, run in CI).
    ///
    /// <para>Units and conventions.</para>
    /// <list type="bullet">
    /// <item><b>Nearness</b> <c>q = 1 / depth</c> in 1/m, METRIC, in the rig's own units (the
    /// unit of <c>DisplayXRCamera.invConvergenceDistance</c>). Depth is measured along the
    /// camera's forward axis. <c>q = 0</c> is the far plane / sky; <c>NaN</c> is "not a
    /// sample" (masked out). Nothing here re-normalises per frame: on-screen disparity is
    /// linear in q, so a fixed metric histogram has uniform resolution in exactly the
    /// quantity the eye sees, and two depths it cannot tell apart are stereoscopically
    /// the same depth.</item>
    /// <item><b>Disparity</b> is a fraction of the view width, positive BEHIND the screen
    /// plane, negative in front: <c>d = k * ipdFactor * (screenQ - q)</c> with
    /// <c>k = baseline / (2 tan(hfov/2))</c> (<see cref="DisparityScale"/>). This is the
    /// similar-triangles relation of any parallel-axis, shifted-frustum stereo pair; it is
    /// an estimate for sizing the comfort caps, NOT the runtime's projection (the runtime
    /// owns that, through <c>XR_DXR_view_rig</c>).</item>
    /// <item>Sample UV: origin bottom-left, both in [0, 1] over the view.</item>
    /// </list>
    /// </summary>
    public static class DisplayXRAutoConvergenceMath
    {
        /// <summary>Histogram resolution of the clump search.</summary>
        public const int Bins = 64;

        /// <summary>Largest sample set <see cref="Workspace"/> holds by default (a 32x32 grid).</summary>
        public const int DefaultCapacity = 1024;

        /// <summary>Eye separation the disparity estimate assumes, metres.</summary>
        public const float NominalIpdMeters = 0.063f;

        /// <summary>Default comfort caps, degrees of on-glass disparity seen from the nominal
        /// viewing distance. Asymmetric on purpose: about one third of the depth budget in
        /// front of the screen, two thirds behind.</summary>
        public const float DefaultFrontCapDegrees = 0.6f, DefaultBehindCapDegrees = 1.2f;

        /// <summary>Used when the display reports no geometry: a 15.6 inch panel at arm's length.</summary>
        public const float FallbackViewingDistanceMeters = 0.6f, FallbackViewWidthMeters = 0.344f;

        // ------------------------------------------------------------------------------
        // Statistic
        // ------------------------------------------------------------------------------

        /// <summary>Estimator tunables. <see cref="Default"/> are the panel-tuned values.</summary>
        public struct Params
        {
            /// <summary>Centre-prior Gaussian sigma, fraction of the view width / height.</summary>
            public float centerSigma;
            /// <summary>Weight at the bottom edge (1 at the top, linear in between).</summary>
            public float bottomDamp;
            /// <summary>Gaussian sigma of the histogram smoothing, in bins.</summary>
            public float histSmoothBins;
            /// <summary>Clumps in the far fraction of the frame's 2nd..98th percentile range
            /// are never the subject (the far plateau: scenery, a back wall).</summary>
            public float farExclude;
            /// <summary>Centre-weighted percentile of nearness the screen plane tracks (0..100).</summary>
            public float percentile;
            /// <summary>screen q = blend * percentile + (1 - blend) * dominant clump.</summary>
            public float blend;
            /// <summary>A peak must hold this fraction of the highest smoothed bin.</summary>
            public float peakFloor;
            /// <summary>Weighted mass within +-2 bins of the clump, fraction of the total.</summary>
            public float minConfidence;
            /// <summary>Samples with real depth (not sky, not masked) needed, fraction of all samples.</summary>
            public float minValidFraction;
            /// <summary>Samples with real depth needed, absolute.</summary>
            public int minValidSamples;
            /// <summary>Subject hysteresis: the clump q currently followed (&lt; 0 = none). A peak
            /// within <see cref="preferWindow"/> (fraction of the histogram range) of it is kept
            /// unless another scores <see cref="switchRatio"/> times higher.</summary>
            public float preferQ, preferWindow, switchRatio;

            public static Params Default => new Params
            {
                centerSigma = 0.35f,
                bottomDamp = 0.5f,
                histSmoothBins = 1.5f,
                farExclude = 0.15f,
                percentile = 80f,
                blend = 0.5f,
                peakFloor = 0.05f,
                minConfidence = 0.06f,
                minValidFraction = 0.03f,
                minValidSamples = 4,
                preferQ = -1f,
                preferWindow = 0.08f,
                switchRatio = 1.5f,
            };
        }

        /// <summary>One measurement.</summary>
        public struct Estimate
        {
            /// <summary>A confident screen plane was found. False = hold the last value.</summary>
            public bool valid;
            /// <summary>Nearness to put on the screen plane (1/m).</summary>
            public float screenQ;
            /// <summary>The centre-weighted percentile of nearness.</summary>
            public float percentileQ;
            /// <summary>The dominant clump outside the far plateau (&lt; 0 = none).</summary>
            public float clumpQ;
            /// <summary>98th / 2nd UNWEIGHTED percentile of nearness: the content the front /
            /// behind caps are sized for. Sky counts (farQ = 0 when 2 % of the frame is sky).</summary>
            public float nearQ, farQ;
            /// <summary>Weighted mass near the clump / total weighted mass.</summary>
            public float confidence;
            /// <summary>Samples used (not masked), and of those the ones with real depth.</summary>
            public int samples, validSamples;
        }

        /// <summary>Scratch buffers, allocated once and reused: the estimator itself never allocates.</summary>
        public sealed class Workspace
        {
            internal readonly float[] q, w;
            internal readonly double[] counts = new double[Bins], sumQ = new double[Bins], smooth = new double[Bins];

            public Workspace(int capacity = DefaultCapacity)
            {
                if (capacity < 1) capacity = 1;
                q = new float[capacity];
                w = new float[capacity];
            }

            public int Capacity => q.Length;
        }

        /// <summary>
        /// The screen plane for one set of depth samples.
        /// </summary>
        /// <param name="q">Nearness per sample (1/m): 0 = sky / far plane, NaN = masked out.</param>
        /// <param name="u">Sample position, 0 = left, 1 = right.</param>
        /// <param name="vUp">Sample position, 0 = bottom, 1 = top.</param>
        /// <param name="count">Samples to read (clamped to the arrays and the workspace).</param>
        /// <param name="histogramMaxQ">Top of the fixed histogram range (1/m), normally
        /// 1 / the minimum convergence distance. Nearer samples land in the last bin for the
        /// clump search; the percentiles use their true value.</param>
        /// <returns><c>est.valid</c>.</returns>
        public static bool EstimateScreenPlane(float[] q, float[] u, float[] vUp, int count, float histogramMaxQ,
                                    in Params p, Workspace ws, out Estimate est)
        {
            est = default;
            est.clumpQ = -1f;
            if (q == null || u == null || vUp == null || ws == null) return false;
            if (count > q.Length) count = q.Length;
            if (count > u.Length) count = u.Length;
            if (count > vUp.Length) count = vUp.Length;
            if (count > ws.Capacity) count = ws.Capacity;
            if (count <= 0 || !(histogramMaxQ > 0f) || float.IsInfinity(histogramMaxQ)) return false;

            float sigma = p.centerSigma > 0f ? p.centerSigma : 0.35f;
            float damp = Clamp(p.bottomDamp, 0f, 1f);
            double[] counts = ws.counts, sumQ = ws.sumQ, sm = ws.smooth;
            for (int b = 0; b < Bins; b++) { counts[b] = 0.0; sumQ[b] = 0.0; }

            int n = 0, nValid = 0;
            double wsum = 0.0;
            float binScale = Bins / histogramMaxQ;
            for (int i = 0; i < count; i++)
            {
                float qi = q[i];
                if (float.IsNaN(qi)) continue;
                if (!(qi > 0f)) qi = 0f;                       // sky / far plane
                else if (float.IsInfinity(qi)) qi = float.MaxValue;
                float dx = (u[i] - 0.5f) / sigma, dy = (vUp[i] - 0.5f) / sigma;
                float vdamp = 1f - (1f - damp) * (1f - Clamp(vUp[i], 0f, 1f));
                float wi = (float)Math.Exp(-0.5 * (dx * dx + dy * dy)) * vdamp;
                if (!(wi > 0f)) wi = 1e-12f;
                ws.q[n] = qi;
                ws.w[n] = wi;
                n++;
                wsum += wi;
                if (qi > 0f)
                {
                    // Sky is kept out of the clump search (it is never the subject) but stays
                    // in the percentiles, so "20 % of the frame in front" and the behind cap
                    // both still see it.
                    nValid++;
                    float fb = qi * binScale;
                    int b = fb >= Bins ? Bins - 1 : (int)fb;
                    counts[b] += wi;
                    sumQ[b] += (double)wi * Math.Min(qi, histogramMaxQ);
                }
            }
            est.samples = n;
            est.validSamples = nValid;
            if (n == 0 || !(wsum > 0.0)) return false;

            SortPairs(ws.q, ws.w, n);
            est.percentileQ = WeightedQuantile(ws.q, ws.w, n, wsum, Clamp(p.percentile, 0f, 100f) / 100.0);
            est.farQ = ws.q[(int)Math.Floor(0.02 * (n - 1))];
            est.nearQ = ws.q[(int)Math.Ceiling(0.98 * (n - 1))];

            int need = Math.Max(p.minValidSamples, (int)Math.Ceiling(p.minValidFraction * n));
            if (need < 1) need = 1;
            if (nValid < need) return false;

            // Gaussian-smoothed histogram.
            double sig = p.histSmoothBins > 0f ? p.histSmoothBins : 1.5;
            int rad = (int)Math.Ceiling(3.0 * sig);
            double smax = 0.0, norm = 0.0;
            for (int j = -rad; j <= rad; j++) norm += Math.Exp(-(double)(j * j) / (2.0 * sig * sig));
            for (int b = 0; b < Bins; b++)
            {
                // Normalised by the FULL kernel, also at the ends of the range: in metric
                // nearness the far plateau always sits in the first bins, and renormalising
                // a truncated kernel there would inflate it against every other peak.
                double acc = 0.0;
                for (int j = -rad; j <= rad; j++)
                {
                    int bb = b + j;
                    if (bb < 0 || bb >= Bins) continue;
                    acc += counts[bb] * Math.Exp(-(double)(j * j) / (2.0 * sig * sig));
                }
                sm[b] = acc / norm;
                if (sm[b] > smax) smax = sm[b];
            }

            // Peaks outside the far plateau. The plateau is the far farExclude of what this
            // frame actually spans (2nd..98th percentile, sky included); a bin is excluded
            // when it lies wholly inside it, so a single flat plane is still its own subject.
            double farLimit = est.farQ + Clamp(p.farExclude, 0f, 1f) * (double)(est.nearQ - est.farQ);
            double binWidth = histogramMaxQ / (double)Bins;
            int best = -1, kept = -1;
            double bestScore = -1.0, keptScore = -1.0;
            bool prefer = p.preferQ >= 0f;
            double preferWindow = p.preferWindow * (double)histogramMaxQ;
            for (int b = 0; b < Bins; b++)
            {
                double l = b > 0 ? sm[b - 1] : -1.0;
                double r = b < Bins - 1 ? sm[b + 1] : -1.0;
                if (!(sm[b] >= l && sm[b] >= r && sm[b] > p.peakFloor * smax && sm[b] > 0.0)) continue;
                if ((b + 1) * binWidth <= farLimit) continue;
                double c = (b + 0.5) * binWidth;
                double score = sm[b];
                if (score > bestScore) { bestScore = score; best = b; }
                if (prefer && Math.Abs(c - p.preferQ) <= preferWindow && score > keptScore)
                {
                    keptScore = score;
                    kept = b;
                }
            }

            float blend = Clamp(p.blend, 0f, 1f);
            if (best < 0)
            {
                // Only the far plateau has a peak: the percentile alone still places the
                // screen plane sensibly, with the weakest confidence that counts.
                if (blend <= 0f) return false;
                est.screenQ = est.percentileQ;
                est.confidence = p.minConfidence > 0f ? p.minConfidence : 0.06f;
                est.valid = true;
                return true;
            }
            // Hysteresis: two comparable clumps (a face and the wall behind it) must not
            // trade places from one sample to the next.
            if (kept >= 0 && kept != best && bestScore < p.switchRatio * keptScore) best = kept;

            // The clump's own depth: weighted mean of the samples around the peak (exact
            // for a flat surface, where a bin centre would be off by up to half a bin).
            double cw = 0.0, cq = 0.0, nearMass = 0.0;
            for (int j = -2; j <= 2; j++)
            {
                int bb = best + j;
                if (bb < 0 || bb >= Bins) continue;
                nearMass += counts[bb];
                if (j >= -1 && j <= 1) { cw += counts[bb]; cq += sumQ[bb]; }
            }
            est.clumpQ = cw > 0.0 ? (float)(cq / cw) : (float)((best + 0.5) * binWidth);
            est.screenQ = blend * est.percentileQ + (1f - blend) * est.clumpQ;
            est.confidence = (float)(nearMass / wsum);
            est.valid = est.confidence >= p.minConfidence;
            return est.valid;
        }

        /// <summary>Weighted quantile of ASCENDING-sorted values (interpolated inside the
        /// sample that crosses the target mass).</summary>
        static float WeightedQuantile(float[] q, float[] w, int n, double wsum, double fraction)
        {
            double target = fraction * wsum, acc = 0.0;
            for (int i = 0; i < n; i++)
            {
                double next = acc + w[i];
                if (next >= target)
                {
                    if (i == 0 || !(w[i] > 0f)) return q[i];
                    double t = (target - acc) / w[i];
                    return (float)(q[i - 1] + (q[i] - q[i - 1]) * t);
                }
                acc = next;
            }
            return q[n - 1];
        }

        /// <summary>In-place ascending shell sort of (key, value) pairs: no allocation, no
        /// recursion, a few thousand elements at a handful of Hz.</summary>
        static void SortPairs(float[] key, float[] val, int n)
        {
            int gap = 1;
            while (gap < n / 3) gap = 3 * gap + 1;
            for (; gap >= 1; gap /= 3)
            {
                for (int i = gap; i < n; i++)
                {
                    float k = key[i], v = val[i];
                    int j = i;
                    while (j >= gap && key[j - gap] > k)
                    {
                        key[j] = key[j - gap];
                        val[j] = val[j - gap];
                        j -= gap;
                    }
                    key[j] = k;
                    val[j] = v;
                }
            }
        }

        // ------------------------------------------------------------------------------
        // Disparity and stereo strength
        // ------------------------------------------------------------------------------

        /// <summary>
        /// <c>k</c>: on-screen disparity (fraction of the view width) per unit of nearness
        /// difference (1/m) at stereo strength 1.
        /// </summary>
        public static float DisparityScale(float baselineMeters, float tanHalfHorizontalFov)
        {
            if (!(tanHalfHorizontalFov > 1e-6f) || !(baselineMeters > 0f)) return 0f;
            return baselineMeters / (2f * tanHalfHorizontalFov);
        }

        /// <summary>Disparity of content at nearness <paramref name="q"/>, fraction of the view
        /// width: positive behind the screen plane, negative in front.</summary>
        public static float Disparity(float k, float ipdFactor, float screenQ, float q)
        {
            return k * ipdFactor * (screenQ - q);
        }

        /// <summary>
        /// A comfort cap given as an angle (on-glass disparity seen from the viewing distance)
        /// -> fraction of the view width. Use the display's NOMINAL viewing distance, not the
        /// live head position: a cap that follows the head makes the picture breathe.
        /// </summary>
        public static float CapFractionFromDegrees(float degrees, float viewingDistanceMeters, float viewWidthMeters)
        {
            if (!(viewingDistanceMeters > 0f) || float.IsInfinity(viewingDistanceMeters))
                viewingDistanceMeters = FallbackViewingDistanceMeters;
            if (!(viewWidthMeters > 0f) || float.IsInfinity(viewWidthMeters))
                viewWidthMeters = FallbackViewWidthMeters;
            if (!(degrees > 0f)) return 0f;
            if (degrees > 89f) degrees = 89f;
            return (float)(Math.Tan(degrees * Math.PI / 180.0) * viewingDistanceMeters / viewWidthMeters);
        }

        /// <summary>
        /// The largest stereo strength (ipdFactor) that keeps <paramref name="nearQ"/> inside
        /// the front cap and <paramref name="farQ"/> inside the behind cap, never above
        /// <paramref name="authored"/>: this only ever REDUCES stereo, it never amplifies.
        /// </summary>
        public static float StrengthForCaps(float k, float authored, float screenQ, float nearQ, float farQ,
                                            float capFront, float capBehind)
        {
            float f = authored > 0f ? authored : 0f;
            if (!(k > 0f)) return f;
            float front = k * (nearQ - screenQ);   // disparity in front at strength 1
            float behind = k * (screenQ - farQ);   // disparity behind at strength 1
            if (front > 0f) f = Math.Min(f, Math.Max(capFront, 0f) / front);
            if (behind > 0f) f = Math.Min(f, Math.Max(capBehind, 0f) / behind);
            return f;
        }

        /// <summary>
        /// The nearness at which content reaches the front / behind cap (for the comfort-volume
        /// gizmo). <paramref name="behindQ"/> &lt;= 0 means the behind limit is at or beyond
        /// infinity: everything behind the screen is inside the cap.
        /// </summary>
        public static void ComfortRangeQ(float k, float ipdFactor, float screenQ, float capFront, float capBehind,
                                         out float frontQ, out float behindQ)
        {
            float kf = k * ipdFactor;
            if (!(kf > 1e-9f))
            {
                frontQ = float.PositiveInfinity;
                behindQ = 0f;
                return;
            }
            frontQ = screenQ + Math.Max(capFront, 0f) / kf;
            behindQ = screenQ - Math.Max(capBehind, 0f) / kf;
        }

        // ------------------------------------------------------------------------------
        // Temporal policy
        // ------------------------------------------------------------------------------

        /// <summary>
        /// Median of the last three measurements, a dead band, and a critically damped spring
        /// (exact integration: stable for any dt, no overshoot from rest). The first
        /// measurement and the one after <see cref="SnapNext"/> are followed with a short time
        /// constant - a ramp of a few frames, never a one-frame snap - and the velocity is
        /// dropped when that fast phase hands over to the slow spring (carried over, it
        /// overshoots the new plane and drifts back for seconds).
        /// </summary>
        public struct Follower
        {
            /// <summary>A first measurement was taken.</summary>
            public bool have;
            /// <summary>Delivered value, and its rate (1/s).</summary>
            public float x, v;
            /// <summary>What x follows.</summary>
            public float target;
            /// <summary>In the fast phase.</summary>
            public bool fast;

            float m0, m1, m2;
            int nMed, medNext;
            bool snapPending, lastDown;
            float fastTime, fastSpan;

            /// <summary>Start (or restart) from <paramref name="start"/> with no measurement.</summary>
            public void Reset(float start)
            {
                this = default;
                x = target = start;
            }

            /// <summary>Follow the next measurement fast (camera teleport, cut, scene load).</summary>
            public void SnapNext()
            {
                snapPending = true;
            }

            /// <summary>Feed one VALID measurement. (No call = hold.)</summary>
            public void Measure(float value, float deadband)
            {
                if (float.IsNaN(value) || float.IsInfinity(value)) return;
                if (!have || snapPending)
                {
                    have = true;
                    snapPending = false;
                    target = value;
                    m0 = m1 = m2 = value;
                    nMed = 3;
                    medNext = 0;
                    fast = true;
                    fastTime = 0f;
                    fastSpan = Math.Abs(x - target);
                    v = 0f;
                    return;
                }
                if (medNext == 0) m0 = value; else if (medNext == 1) m1 = value; else m2 = value;
                medNext = (medNext + 1) % 3;
                if (nMed < 3) nMed++;
                float m = nMed < 3 ? value : Median3(m0, m1, m2);
                if (Math.Abs(m - target) > deadband) target = m;
            }

            /// <summary>Advance by <paramref name="dt"/> seconds; returns the value to deliver.</summary>
            public float Step(float dt, float tau, float tauFast = 0.06f, float fastMaxSeconds = 0.4f)
            {
                return StepInternal(dt, tau, tau, tauFast, fastMaxSeconds);
            }

            /// <summary>As <see cref="Step"/>, with a different time constant for a falling
            /// value (stereo strength backs off faster than it comes back). The velocity is
            /// dropped whenever the direction - and so the time constant - changes.</summary>
            public float StepAsymmetric(float dt, float tauUp, float tauDown, float tauFast = 0.06f,
                                        float fastMaxSeconds = 0.4f)
            {
                return StepInternal(dt, tauUp, tauDown, tauFast, fastMaxSeconds);
            }

            float StepInternal(float dt, float tauUp, float tauDown, float tauFast, float fastMax)
            {
                if (!have) return x;
                if (!(dt > 0f)) return x;
                if (dt > 0.5f) dt = 0.5f; // a stalled app resumes gently
                bool down = target < x;
                if (down != lastDown)
                {
                    lastDown = down;
                    if (!fast && tauUp != tauDown) v = 0f;
                }
                double tau = fast ? tauFast : (down ? tauDown : tauUp);
                if (!(tau > 1e-4)) tau = 1e-4;
                double w = 1.0 / tau;
                // y = x - target; y(t) = (y0 + (v0 + w y0) t) e^(-wt).
                double y0 = (double)x - target, v0 = v;
                double kk = v0 + w * y0;
                double e = Math.Exp(-w * dt);
                x = (float)(target + (y0 + kk * dt) * e);
                v = (float)((v0 - w * kk * dt) * e);
                if (fast)
                {
                    fastTime += dt;
                    if (Math.Abs(x - target) <= 0.02f * fastSpan || fastTime > fastMax)
                    {
                        fast = false;
                        v = 0f;
                    }
                }
                return x;
            }

            static float Median3(float a, float b, float c)
            {
                if (a > b) { float t = a; a = b; b = t; }
                if (b > c) { b = c; }
                return a > b ? a : b;
            }
        }

        // ------------------------------------------------------------------------------
        // Convergence as a distance (inspector)
        // ------------------------------------------------------------------------------

        /// <summary>Inverse convergence distance -> metres; +infinity when it is 0 (parallel).</summary>
        public static float DistanceFromInverse(float inverse)
        {
            return inverse > 1e-6f ? 1f / inverse : float.PositiveInfinity;
        }

        /// <summary>Metres -> inverse convergence distance, clamped to [0, <paramref name="maxInverse"/>];
        /// infinity, 0 and negative distances give 0 (parallel).</summary>
        public static float InverseFromDistance(float meters, float maxInverse = 10f)
        {
            if (float.IsNaN(meters) || float.IsInfinity(meters) || !(meters > 0f)) return 0f;
            float inv = 1f / meters;
            return inv > maxInverse ? maxInverse : inv;
        }

        /// <summary>"2.50" or "infinity".</summary>
        public static string FormatDistance(float inverse)
        {
            float d = DistanceFromInverse(inverse);
            return float.IsInfinity(d) ? "infinity" : d.ToString("0.###", CultureInfo.InvariantCulture);
        }

        /// <summary>Parse what a user types for a convergence distance: a number of metres
        /// (an optional trailing "m"), or "infinity" / "inf" / the infinity sign / an empty
        /// string for parallel. False when it is none of those.</summary>
        public static bool TryParseDistance(string text, out float inverse, float maxInverse = 10f)
        {
            inverse = 0f;
            string s = (text ?? "").Trim().ToLowerInvariant();
            if (s.EndsWith("m")) s = s.Substring(0, s.Length - 1).Trim();
            if (s.Length == 0 || s == "inf" || s == "infinity" || s == "∞") return true;
            s = s.Replace(',', '.');
            if (!float.TryParse(s, NumberStyles.Float, CultureInfo.InvariantCulture, out float meters)) return false;
            if (float.IsNaN(meters)) return false;
            inverse = InverseFromDistance(meters, maxInverse);
            return true;
        }

        static float Clamp(float v, float lo, float hi) => v < lo ? lo : (v > hi ? hi : v);
    }
}
