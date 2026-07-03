// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// C# port of displayxr-common/common/mode_switch.{h,cpp} — the smooth 2D<->3D
// rendering-mode switch sequencer the native runtime test apps share. Provided as
// a plugin helper so Unity apps get the identical transition without copying it
// per project. It is a pure, dependency-free state machine (no UnityEngine, no
// plugin state): the app owns the policy (which key, which target mode, the steady
// IPD) and drives this per frame; the sequencer owns the tricky part — the
// sequencing asymmetry that hand-rolled ramps get wrong:
//
//   - 3D -> 2D : ramp the disparity (ipdFactor) to 0 FIRST, then issue the mode
//                request, so 2D lands on already-flat content.
//   - 2D -> 3D : issue the mode request FIRST (the first 3D frame is flat), then
//                ease the disparity up to the app's steady value.
//
// Wall-clock dt driven (frame-rate independent), interruptible (retargets cleanly
// mid-ramp — a not-yet-fired ->2D reverses without ever switching). Keep in sync
// with the C++ original (displayxr-common).
//
// Usage (per frame, in the app's Update):
//   // On the toggle key, hand the target to the sequencer:
//   modeSwitch.Request(nextMode, viewCountOf(nextMode),
//                      DisplayXRProvider.ActiveModeIndex, currentViewCount,
//                      currentIpd, steadyIpd);
//   // Every frame, advance and act on the outputs:
//   float ipd = modeSwitch.Update(Time.deltaTime, out bool fire, out uint mode);
//   rig.ipdFactor = ipd;                                  // submit on the rig
//   if (fire && mode != DisplayXRProvider.ActiveModeIndex) // issue the runtime switch
//       DisplayXRProvider.RequestRenderingMode(mode);

namespace DisplayXR
{
    public enum ModeSwitchEasing { Linear, SmoothStep, EaseOutCubic }

    /// <summary>Smooth 2D&lt;-&gt;3D rendering-mode switch sequencer. One instance per session.</summary>
    public sealed class DisplayXRModeSwitch
    {
        private enum Phase { Idle, RampDownThenFire, FireThenRampUp }

        private Phase m_Phase = Phase.Idle;
        private uint m_TargetMode;
        private bool m_FirePending;   // FireThenRampUp: emit fire on the next update
        private bool m_FireAtEnd;      // RampDownThenFire: emit fire when the ramp lands

        private float m_From, m_To, m_Cur;
        private float m_T = 1f;        // normalized progress; 1 = landed/idle
        private float m_Dur = 0.18f;
        private ModeSwitchEasing m_Easing = ModeSwitchEasing.SmoothStep;

        /// <summary>Ramp duration (s) and easing. duration &lt;= 0 → instant (fire, no ramp).</summary>
        public void Configure(float durationSeconds, ModeSwitchEasing easing = ModeSwitchEasing.SmoothStep)
        {
            m_Dur = durationSeconds > 0f ? durationSeconds : 0f;
            m_Easing = easing;
        }

        /// <summary>True while a ramp is in flight or a mode request is still pending.</summary>
        public bool Active => m_Phase != Phase.Idle;

        /// <summary>Current ipdFactor without advancing the clock.</summary>
        public float Ipd => m_Cur;

        private float Ease(float t)
        {
            if (t <= 0f) return 0f;
            if (t >= 1f) return 1f;
            switch (m_Easing)
            {
                case ModeSwitchEasing.Linear: return t;
                case ModeSwitchEasing.EaseOutCubic: { float u = 1f - t; return 1f - u * u * u; }
                default: return t * t * (3f - 2f * t); // SmoothStep (Hermite)
            }
        }

        /// <summary>
        /// Begin — or, mid-flight, seamlessly retarget — a switch to <paramref name="targetMode"/>.
        /// <paramref name="currentIpd"/> = the ipdFactor in effect now (the last Update output, or
        /// the steady value when idle); <paramref name="steadyIpd"/> = the app's full-3D ipdFactor
        /// to restore to (NOT assumed 1.0 — the user may have tuned it).
        /// </summary>
        public void Request(uint targetMode, uint targetViewCount,
                            uint currentMode, uint currentViewCount,
                            float currentIpd, float steadyIpd)
        {
            bool toMono = targetViewCount <= 1;
            bool fromMono = currentViewCount <= 1;

            m_TargetMode = targetMode;
            m_From = currentIpd;

            if (toMono && !fromMono)
            {
                // 3D -> 2D: flatten the disparity first, fire on landing so 2D engages
                // on already-mono content (the request is held until the ramp completes).
                m_Phase = Phase.RampDownThenFire;
                m_To = 0f;
                m_FireAtEnd = true;
                m_FirePending = false;
            }
            else if (!toMono && fromMono)
            {
                // 2D -> 3D: fire now (first 3D frame flat via from=0), then ease disparity
                // up to steady. The runtime's #615 coherence guard absorbs the first frame.
                m_Phase = Phase.FireThenRampUp;
                m_From = 0f;
                m_To = steadyIpd;
                m_FirePending = true;
                m_FireAtEnd = false;
            }
            else
            {
                // Same dimensionality (or a reversal of a not-yet-fired ->2D). Switch now;
                // restore steady disparity for 3D, leave it for 2D (mono, irrelevant).
                m_Phase = Phase.FireThenRampUp;
                m_To = toMono ? currentIpd : steadyIpd;
                m_FirePending = (targetMode != currentMode);
                m_FireAtEnd = false;
            }

            m_T = m_Dur > 0f ? 0f : 1f;
            m_Cur = m_From;
        }

        /// <summary>
        /// Advance the ramp by <paramref name="dtSeconds"/> and return the ipdFactor to submit
        /// this frame. <paramref name="fire"/> is true on exactly one frame — issue the runtime
        /// mode request for <paramref name="mode"/> then (skip if it equals the current mode).
        /// </summary>
        public float Update(float dtSeconds, out bool fire, out uint mode)
        {
            fire = false;
            mode = m_TargetMode;

            if (m_Phase != Phase.Idle)
            {
                if (m_T < 1f && m_Dur > 0f)
                {
                    m_T += dtSeconds / m_Dur;
                    if (m_T > 1f) m_T = 1f;
                }
                else m_T = 1f;
                m_Cur = m_From + (m_To - m_From) * Ease(m_T);

                if (m_Phase == Phase.FireThenRampUp)
                {
                    if (m_FirePending) { m_FirePending = false; fire = true; }
                    if (m_T >= 1f) m_Phase = Phase.Idle;
                }
                else // RampDownThenFire
                {
                    if (m_T >= 1f)
                    {
                        if (m_FireAtEnd) { m_FireAtEnd = false; fire = true; }
                        m_Phase = Phase.Idle;
                    }
                }
            }
            return m_Cur;
        }
    }
}
