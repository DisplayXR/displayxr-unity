// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

using UnityEngine;

namespace DisplayXR
{
    /// <summary>2D or 3D — the dimensionality a scene wants to be presented in.</summary>
    public enum DisplayXRSceneDimensionality
    {
        /// <summary>Stereo. The runtime weaves; the rig's disparity is at its steady value.</summary>
        ThreeD = 0,
        /// <summary>Flat. The runtime renders a single view and the panel shows plain 2D.</summary>
        TwoD = 1,
    }

    /// <summary>
    /// Declares whether this scene should be presented in 2D or in 3D, and performs the
    /// transition. Drop one on any GameObject in the scene (a menu, a loading screen, a
    /// 2D home screen) and pick a mode.
    /// <para>
    /// <b>Why this exists (#267).</b> While a DisplayXR session is running, XR is active
    /// process-wide and Unity renders <i>every</i> camera in stereo — including a plain
    /// camera with no DisplayXR rig component on it at all. The rig components only
    /// <i>tune</i> stereo, they do not gate it, so "just don't add a rig" does not give you
    /// a 2D scene: a flat menu still comes out weaved. The lever that actually works is the
    /// runtime's rendering mode, and this component drives it.
    /// </para>
    /// <para>
    /// <b>It works with no rig present</b>, which is the point — a 2D scene typically has
    /// none. When a rig <i>is</i> present its <c>ipdFactor</c> is ramped in step with the
    /// switch, because the transition is asymmetric and hand-rolled ramps get it backwards:
    /// going 3D→2D the disparity must reach zero <i>before</i> the mode request lands, and
    /// going 2D→3D the request must land <i>first</i>. <see cref="DisplayXRModeSwitch"/>
    /// owns that sequencing; this component is the scene-level wrapper around it.
    /// </para>
    /// <para>
    /// <b>Graceful degrade.</b> If the runtime advertises no mono (viewCount &lt;= 1)
    /// rendering mode, the component still flattens disparity and — if
    /// <see cref="driveHardwareDisplayMode"/> is set — asks the panel for its 2D state, then
    /// logs once that a true mono mode was unavailable. It never throws and never blocks
    /// scene load.
    /// </para>
    /// </summary>
    [AddComponentMenu("DisplayXR/Scene Mode (2D / 3D)")]
    [DisallowMultipleComponent]
    public class DisplayXRSceneMode : MonoBehaviour
    {
        [Tooltip("How this scene should be presented. Applied when the session is running.")]
        public DisplayXRSceneDimensionality mode = DisplayXRSceneDimensionality.ThreeD;

        [Tooltip("Apply on enable. Turn off to drive the transition yourself via Apply().")]
        public bool applyOnEnable = true;

        [Tooltip("Disparity ramp length, seconds. 0 = switch instantly (no ramp).")]
        [Range(0f, 2f)]
        public float transitionSeconds = 0.18f;

        [Tooltip("Also request the panel's hardware 2D/3D state, not just the rendering mode. " +
                 "Leave on unless something else in the app owns that.")]
        public bool driveHardwareDisplayMode = true;

        [Tooltip("The ipdFactor to restore when returning to 3D. Captured from the active rig " +
                 "on first apply if left at 0.")]
        public float steadyIpdFactor = 0f;

        readonly DisplayXRModeSwitch m_Switch = new DisplayXRModeSwitch();

        bool  m_Pending;                 // a target is set but the session wasn't up yet
        bool  m_Applied;
        uint  m_ThreeDModeIndex;         // the 3D mode to come back to
        bool  m_HaveThreeDMode;
        bool  m_WarnedNoMonoMode;
        DisplayXRSceneDimensionality m_Target;

        /// <summary>The dimensionality most recently requested through this component.</summary>
        public DisplayXRSceneDimensionality CurrentTarget => m_Target;

        /// <summary>True while a transition is still in flight.</summary>
        public bool Transitioning => m_Switch.Active;

        void OnEnable()
        {
            m_Target = mode;
            if (applyOnEnable) Apply(mode);
        }

        /// <summary>
        /// Request a dimensionality. Safe to call before the session is running — the request
        /// is held and applied on the first frame the provider reports a live session. Safe to
        /// call mid-transition: <see cref="DisplayXRModeSwitch"/> retargets cleanly, and a
        /// not-yet-fired 3D→2D reverses without ever having switched.
        /// </summary>
        public void Apply(DisplayXRSceneDimensionality target)
        {
            m_Target = target;
            mode = target;
            m_Pending = true;
            m_Applied = false;
            if (DisplayXRProvider.IsRunning) BeginTransition();
        }

        void Update()
        {
            // The session comes up on the render thread a frame or two into Play, and a
            // dock/undock auto-switch restarts it mid-run — so a held request is re-armed
            // rather than dropped (a once-only push into a dead session succeeds silently).
            if (m_Pending && !m_Applied && DisplayXRProvider.IsRunning)
                BeginTransition();

            if (!m_Switch.Active) return;

            float ipd = m_Switch.Update(Time.unscaledDeltaTime, out bool fire, out uint modeIndex);
            PushIpdFactor(ipd);
            if (fire && DisplayXRProvider.IsRunning && modeIndex != DisplayXRProvider.ActiveModeIndex)
                DisplayXRProvider.RequestRenderingMode(modeIndex);
        }

        void BeginTransition()
        {
            m_Applied = true;

            // Remember the 3D mode to return to: whatever multi-view mode is active the first
            // time we look. Captured once so a later 2D->3D restores the app's real mode
            // rather than "the first stereo mode in the list".
            if (!m_HaveThreeDMode) CaptureThreeDMode();

            if (steadyIpdFactor <= 0f)
            {
                float rigIpd = ReadIpdFactor();
                steadyIpdFactor = rigIpd > 0f ? rigIpd : 1f;
            }

            uint currentMode  = DisplayXRProvider.ActiveModeIndex;
            uint currentViews = ViewCountOf(currentMode, fallback: 2u);

            uint targetMode, targetViews;
            if (m_Target == DisplayXRSceneDimensionality.TwoD)
            {
                if (TryFindMonoMode(out targetMode))
                {
                    targetViews = 1u;
                }
                else
                {
                    // No mono mode advertised. Flatten what we can and say so, once.
                    targetMode = currentMode;
                    targetViews = 1u;
                    if (!m_WarnedNoMonoMode)
                    {
                        m_WarnedNoMonoMode = true;
                        Debug.LogWarning(
                            "[DisplayXR] SceneMode: the runtime advertises no mono (viewCount<=1) " +
                            "rendering mode, so this scene cannot be switched to a true 2D render " +
                            "path. Falling back to zero disparity" +
                            (driveHardwareDisplayMode ? " + the panel's hardware 2D state." : ".") +
                            " Content will be flat but still rendered through the stereo path.");
                    }
                }
            }
            else
            {
                targetMode  = m_HaveThreeDMode ? m_ThreeDModeIndex : currentMode;
                targetViews = ViewCountOf(targetMode, fallback: 2u);
                if (targetViews <= 1u) targetViews = 2u;   // never sequence 3D as if it were mono
            }

            m_Switch.Configure(transitionSeconds);
            m_Switch.Request(targetMode, targetViews, currentMode, currentViews,
                             ReadIpdFactor(), steadyIpdFactor);

            if (driveHardwareDisplayMode)
                DisplayXRProvider.RequestDisplayMode(m_Target == DisplayXRSceneDimensionality.ThreeD);

            // transitionSeconds == 0 means Request() already landed; push the final disparity
            // now so a zero-length transition doesn't wait a frame.
            if (!m_Switch.Active) PushIpdFactor(m_Switch.Ipd);
        }

        void CaptureThreeDMode()
        {
            uint active = DisplayXRProvider.ActiveModeIndex;
            if (ViewCountOf(active, fallback: 0u) >= 2u)
            {
                m_ThreeDModeIndex = active; m_HaveThreeDMode = true; return;
            }
            var modes = DisplayXRProvider.Modes;
            for (int i = 0; i < modes.Count; i++)
                if (modes[i].viewCount >= 2u)
                {
                    m_ThreeDModeIndex = modes[i].modeIndex; m_HaveThreeDMode = true; return;
                }
        }

        static bool TryFindMonoMode(out uint modeIndex)
        {
            modeIndex = 0;
            var modes = DisplayXRProvider.Modes;
            for (int i = 0; i < modes.Count; i++)
                if (modes[i].viewCount <= 1u && modes[i].isRequestable != 0)
                {
                    modeIndex = modes[i].modeIndex; return true;
                }
            // Second pass ignoring isRequestable: older runtimes leave the flag zeroed.
            for (int i = 0; i < modes.Count; i++)
                if (modes[i].viewCount <= 1u)
                {
                    modeIndex = modes[i].modeIndex; return true;
                }
            return false;
        }

        static uint ViewCountOf(uint modeIndex, uint fallback)
        {
            var modes = DisplayXRProvider.Modes;
            for (int i = 0; i < modes.Count; i++)
                if (modes[i].modeIndex == modeIndex) return modes[i].viewCount;
            return fallback;
        }

        // --- Rig disparity ------------------------------------------------------
        // The two rig types don't share a base class or interface, and a 2D scene very
        // often has NEITHER — which is the whole reason this component exists. So both
        // accessors are best-effort and a missing rig is the normal case, not an error.

        float ReadIpdFactor()
        {
            var cam = DisplayXRRigManager.ActiveCamera;
            if (cam == null) return steadyIpdFactor > 0f ? steadyIpdFactor : 1f;
            var d = cam.GetComponent<DisplayXRDisplay>();
            if (d != null) return d.ipdFactor;
            var c = cam.GetComponent<DisplayXRCamera>();
            if (c != null) return c.ipdFactor;
            return steadyIpdFactor > 0f ? steadyIpdFactor : 1f;
        }

        void PushIpdFactor(float ipd)
        {
            var cam = DisplayXRRigManager.ActiveCamera;
            if (cam == null) return;                       // rig-less 2D scene: nothing to ramp
            var d = cam.GetComponent<DisplayXRDisplay>();
            if (d != null) { d.ipdFactor = ipd; return; }
            var c = cam.GetComponent<DisplayXRCamera>();
            if (c != null) c.ipdFactor = ipd;
        }
    }
}
