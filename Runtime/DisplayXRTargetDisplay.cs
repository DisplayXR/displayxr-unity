// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

using System.Collections;
using UnityEngine;

namespace DisplayXR
{
    /// <summary>
    /// (#266) Moves the app's window onto the DisplayXR panel at startup.
    ///
    /// <para>
    /// <b>The problem.</b> Unity creates its window before any app script runs, and it
    /// opens whereever Windows decides — normally the OS primary display. Nothing in the
    /// app can influence that. So on a machine where the 3D panel is a secondary display,
    /// a DisplayXR app opens on the wrong monitor and the user has to drag it across; the
    /// long-standing workaround has been to make the panel the Windows primary.
    /// </para>
    ///
    /// <para>
    /// This component asks the runtime where the panel actually is and moves the window
    /// there once the session is up. It is <b>opt-in</b>: window placement is app policy,
    /// and an app that manages its own window (or is deliberately fullscreen on another
    /// display) should not have one silently moved out from under it.
    /// </para>
    ///
    /// <para>
    /// <b>Requires a runtime advertising <c>XR_DXR_display_info</c> v16+.</b> Against an
    /// older one the panel's desktop position is simply not reported; the component logs
    /// once and does nothing. It is also Windows-only today.
    /// </para>
    ///
    /// <para>
    /// The move itself happens in native code inside a per-monitor-DPI-aware context —
    /// see <see cref="DisplayXRProvider.MoveWindowToDisplay"/> for why doing it from C#
    /// would mis-place the window on exactly the mixed-DPI multi-monitor setups this is
    /// for.
    /// </para>
    /// </summary>
    [AddComponentMenu("DisplayXR/Target Display (move window to the 3D panel)")]
    [DisallowMultipleComponent]
    public class DisplayXRTargetDisplay : MonoBehaviour
    {
        [Tooltip("Move the window onto the DisplayXR panel when the session starts.")]
        public bool moveOnStart = true;

        [Tooltip("How long to wait for the session before giving up, in seconds. The " +
                 "session comes up on the render thread a frame or two into Play.")]
        [Range(1f, 30f)]
        public float sessionTimeout = 10f;

        [Tooltip("Log the outcome. Worth leaving on — a silent no-op and an unsupported " +
                 "runtime look identical from the app's side otherwise.")]
        public bool logResult = true;

        /// <summary>True once a move has been attempted this session (successful or not).</summary>
        public bool Attempted { get; private set; }

        /// <summary>Result of the last attempt. False until one succeeds.</summary>
        public bool Succeeded { get; private set; }

        void OnEnable()
        {
            if (moveOnStart) StartCoroutine(MoveWhenReady());
        }

        /// <summary>
        /// Move now, if the session is running. Returns false when it is not, when the
        /// runtime does not report the panel position, or off Windows.
        /// </summary>
        public bool MoveNow()
        {
            Attempted = true;
            Succeeded = DisplayXRProvider.MoveWindowToDisplay();
            if (logResult)
            {
                if (Succeeded)
                {
                    Debug.Log("[DisplayXR] TargetDisplay: window is on the DisplayXR panel.");
                }
                else if (!DisplayXRProvider.IsRunning)
                {
                    Debug.LogWarning("[DisplayXR] TargetDisplay: no session — cannot place the window.");
                }
                else
                {
                    Debug.LogWarning(
                        "[DisplayXR] TargetDisplay: the runtime did not report the panel's " +
                        "desktop position (needs XR_DXR_display_info v16+), so the window was " +
                        "left where Windows opened it. Making the panel the Windows main " +
                        "display remains the workaround.");
                }
            }
            return Succeeded;
        }

        IEnumerator MoveWhenReady()
        {
            // The session starts on the render thread a frame or two into Play, and the
            // panel position is read at session start — so there is nothing to ask for
            // until then. Poll rather than hook: there is no "session ready" event that
            // fires reliably before the first frame, and this costs a branch per frame
            // for at most a few seconds.
            float t = 0f;
            while (!DisplayXRProvider.IsRunning && t < sessionTimeout)
            {
                t += Time.unscaledDeltaTime;
                yield return null;
            }
            if (!DisplayXRProvider.IsRunning)
            {
                if (logResult)
                    Debug.LogWarning($"[DisplayXR] TargetDisplay: no session within {sessionTimeout:F0}s — " +
                                     "leaving the window where it is.");
                Attempted = true;
                yield break;
            }
            MoveNow();
        }
    }
}
