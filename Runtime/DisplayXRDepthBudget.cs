// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

using UnityEngine;

namespace DisplayXR
{
    /// <summary>
    /// Read-only view of the runtime's advisory <b>rear depth budget</b>
    /// (<c>XR_DXR_depth_budget</c>, issue #318) — how far <i>behind</i> the display
    /// plane this transparent overlay may render right now.
    ///
    /// <para>
    /// <b>Why this exists.</b> A transparent overlay composites over the live desktop.
    /// Content behind the display plane carries positive disparity while being drawn
    /// <i>over</i> desktop pixels at zero disparity: the model covers a desktop icon, so
    /// it must be in front, yet its stereo depth says it is behind. That contradiction
    /// is why <see cref="DisplayXRDisplay.foregroundOnlyClip"/> exists at all — it
    /// throws the whole rear half of the scene away, unconditionally.
    /// </para>
    /// <para>
    /// But the contradiction needs a background with a <i>horizontal</i> depth cue right
    /// there — text, icons, window edges. Over a plain wall of colour, a vertical
    /// gradient, or horizontal stripes there is nothing for the eyes to disagree with,
    /// and the rear of the model looks fine. The runtime watches the desktop behind the
    /// content (off the capture the display's plug-in already takes for transparency)
    /// and publishes this budget every frame. The plug-in simply pushes the foreground
    /// clip plane back by that much.
    /// </para>
    /// <para>
    /// <b>Nothing here needs wiring.</b> The values are published by the provider driver
    /// each frame and consumed inside the rig; this class is for HUDs, debug overlays
    /// and tests. What an app <i>should</i> add is a <see cref="DisplayXRContentBounds"/>
    /// on its content root, so the runtime measures the desktop behind the content
    /// rather than behind the whole window.
    /// </para>
    /// <para>
    /// <b>Do not smooth these values.</b> The runtime already time-ramps
    /// <see cref="FarOffsetVH"/> (roughly 300 ms opening, 150 ms closing) so the clip
    /// plane glides rather than pops. A second filter on top fights that ramp and
    /// produces a slower, less predictable plane.
    /// </para>
    /// </summary>
    public static class DisplayXRDepthBudget
    {
        /// <summary>Why the runtime is handing out the budget it is handing out.
        /// Mirrors <c>XrRearDepthBudgetStateDXR</c>.</summary>
        public enum BudgetState
        {
            /// <summary>Session is not transparent — nothing composites over the desktop.</summary>
            UnrestrictedOpaque = 0,
            /// <summary>Transparent, but running under a workspace controller.</summary>
            UnrestrictedWorkspace = 1,
            /// <summary>Transparent + standalone, and the background carries no horizontal cue.</summary>
            Open = 2,
            /// <summary>Transparent + standalone, background is busy — ramping shut.</summary>
            ClippedBusyBackground = 3,
            /// <summary>No background preview available (no capture source, or it declined).</summary>
            ClippedNoSource = 4,
            /// <summary>A <c>DXR_REAR_BUDGET=open|clip</c> override is pinning the budget.</summary>
            Forced = 5,
        }

        /// <summary>
        /// True when the runtime supports <c>XR_DXR_depth_budget</c> AND filled the
        /// chained struct on the last locate. False on an older runtime, or before the
        /// session comes up — in which case every value below reads as "clip at the
        /// display plane", which is exactly the pre-#318 behaviour.
        /// </summary>
        public static bool Available { get; private set; }

        /// <summary>Why the current budget (see <see cref="BudgetState"/>).</summary>
        public static BudgetState State { get; private set; }

        /// <summary>
        /// The budget in virtual display heights: 0 = clip at the display plane,
        /// &gt;= 1000 = unrestricted. Already ramped by the runtime — apply as-is.
        /// </summary>
        public static float FarOffsetVH { get; private set; }

        /// <summary>
        /// 0..1 diagnostic — how much horizontal structure the runtime measured in the
        /// background behind the content. 0 when there is no capture source.
        /// </summary>
        public static float CueEnergy { get; private set; }

        /// <summary>
        /// The budget resolved into app world units: the distance behind each eye's
        /// display-plane far that may currently be drawn. This is the number the rig
        /// hands the clip shaders. 0 = clip on the plane.
        /// </summary>
        public static float RearOffsetWorld { get; private set; }

        /// <summary>
        /// Log one line whenever <see cref="State"/> changes (never per frame). On by
        /// default: a rear-depth verdict that flips silently is very hard to attribute
        /// when eyeballing the panel. Set false to silence it.
        /// </summary>
        public static bool LogStateChanges = true;

        private static bool s_StateKnown;
        private static BudgetState s_LoggedState;

        /// <summary>
        /// Pull the provider's latest budget. Called once per frame by
        /// <c>DisplayXRProviderDriver</c> — apps read the properties, they do not call
        /// this.
        /// </summary>
        internal static void Poll()
        {
            int live;
            int state;
            float vh, cue, rear;
            try
            {
                live = DisplayXRProviderNative.dxr_prov_get_rear_budget(
                    out state, out vh, out cue, out rear);
            }
            catch (System.DllNotFoundException)
            {
                Reset();
                return;
            }
            catch (System.EntryPointNotFoundException)
            {
                // An older native plug-in binary alongside newer managed code (a stale
                // Library/ or a hand-copied DLL). Degrade to "no budget", never throw
                // once per frame.
                Reset();
                return;
            }

            Available = live != 0;
            State = (BudgetState)state;
            FarOffsetVH = vh;
            CueEnergy = cue;
            RearOffsetWorld = rear;

            if (!LogStateChanges || !Available) return;
            if (s_StateKnown && s_LoggedState == State) return;
            s_StateKnown = true;
            s_LoggedState = State;
            Debug.Log($"[DisplayXR] Rear depth budget: {State} " +
                      $"(farOffsetVH={FarOffsetVH:F0}, rear={RearOffsetWorld:F3} world units, " +
                      $"cue={CueEnergy:F2})");
        }

        /// <summary>Back to "no budget" = clip at the display plane. Called when the
        /// provider session stops, so a stale value cannot outlive its session.</summary>
        internal static void Reset()
        {
            Available = false;
            State = BudgetState.UnrestrictedOpaque;
            FarOffsetVH = 0f;
            CueEnergy = 0f;
            RearOffsetWorld = 0f;
            s_StateKnown = false;
        }
    }
}
