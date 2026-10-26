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
    /// once and does nothing. It works on Windows and Linux (X11/XWayland); macOS is not
    /// implemented yet.
    /// </para>
    ///
    /// <para>
    /// The move itself happens in native code inside a per-monitor-DPI-aware context —
    /// see <see cref="DisplayXRProvider.MoveWindowToDisplay"/> for why doing it from C#
    /// would mis-place the window on exactly the mixed-DPI multi-monitor setups this is
    /// for.
    /// </para>
    ///
    /// <para>
    /// <b>Linux native Wayland: on by default, without this component</b>
    /// (<see cref="PlaceOnPanelAtStart"/>). There mutter opens a new window on the monitor
    /// under the pointer, which can be a screen-casting tool's virtual monitor nobody
    /// sees. So the plug-in moves the window onto the panel once, a couple of frames into
    /// the session, unless the app placed it first (its own
    /// <c>displayxr_set_overlay_position</c>, e.g. a saved position it restores, or this
    /// component), the user dragged it, or it opened on the panel. Turn it off with
    /// <see cref="PlaceOnPanelAtStart"/> = false, this component with
    /// <see cref="moveOnStart"/> off, or <c>DISPLAYXR_PANEL_PLACEMENT=0</c>.
    /// X11 / XWayland, Windows and macOS are unchanged: there this component opts in.
    /// </para>
    /// </summary>
    [AddComponentMenu("DisplayXR/Target Display (move window to the 3D panel)")]
    [DisallowMultipleComponent]
    public class DisplayXRTargetDisplay : MonoBehaviour
    {
        /// <summary>
        /// Move the window onto the DisplayXR panel when the session starts. Off: the
        /// window stays where it opened, and the default start-up placement (Linux
        /// native Wayland, <see cref="PlaceOnPanelAtStart"/>) is turned off too, from
        /// this component's <c>OnEnable</c>. That has to run before the session's
        /// second frame: on a GameObject that is inactive then, or in a scene loaded
        /// later, it is too late; set <see cref="PlaceOnPanelAtStart"/> = false from a
        /// <c>[RuntimeInitializeOnLoadMethod]</c> instead (or
        /// <c>DISPLAYXR_PANEL_PLACEMENT=0</c> for one run).
        /// </summary>
        [Tooltip("Move the window onto the DisplayXR panel when the session starts. Off: " +
                 "the window stays where it opened, and the default start-up placement " +
                 "(Linux native Wayland) is turned off too, if this component is enabled " +
                 "before the session's second frame. On an inactive object or in a scene " +
                 "loaded later that is too late: set PlaceOnPanelAtStart = false from a " +
                 "[RuntimeInitializeOnLoadMethod] instead.")]
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
            else PlaceOnPanelAtStart = false; // the app takes placement on itself
        }

        // ---- The start-up placement (on by default) ------------------------------

        /// <summary>
        /// Move the window onto the 3D panel once when the session starts, with no
        /// component in the scene. On by default; Linux native-Wayland players only for
        /// now (elsewhere nothing changes). It stands down when the app placed the window
        /// first (<c>displayxr_set_overlay_position</c>, <see cref="MoveNow"/>), the user
        /// dragged it, or it opened on the panel; it never acts onto a panel the runtime
        /// did not confirm (sim display, no 3D display) or for a session that comes up
        /// more than <see cref="StartupWindowSeconds"/> after launch.
        /// <para>
        /// To turn it off, set this to false before the session's second frame — from a
        /// <c>[RuntimeInitializeOnLoadMethod]</c> or the first scene's <c>Awake</c> /
        /// <c>OnEnable</c> — or add this component with <see cref="moveOnStart"/> off. For
        /// one run without code: <c>DISPLAYXR_PANEL_PLACEMENT=0</c>.
        /// </para>
        /// <para>
        /// The component turns it off from its <c>OnEnable</c>, so only when that runs
        /// before the session's second frame: not on a GameObject that is inactive then,
        /// nor in a scene loaded later. Use a <c>[RuntimeInitializeOnLoadMethod]</c> for
        /// those.
        /// </para>
        /// </summary>
        public static bool PlaceOnPanelAtStart { get; set; } = true;

        /// <summary>
        /// The start-up placement applies only to a session that comes up this soon after
        /// launch: an app that starts XR later, after the user has had the window for a
        /// while, keeps it where it is.
        /// </summary>
        public const float StartupWindowSeconds = 20f;

        static bool s_StartupDone;
        // The native side answered "deciding once the window is shown" (3): asked again
        // every frame until it decides (it gives up by itself when the window is not
        // shown within 5 s; the time limit here only guards against asking forever).
        static bool s_StartupWaiting;
        static float s_StartupAskUntil;

        /// <summary>
        /// Called by the provider driver every frame of a running session; acts once per
        /// run, at the session's second frame, so the app's own placement in its first
        /// frames (a saved position it restores) comes first. A window not shown by then
        /// is decided on the first frame after it is: the checks run again right before,
        /// so an app that went fullscreen, or turned the placement off, in the meantime
        /// keeps its window where it is.
        /// </summary>
        internal static void TickStartupPlacement(int sessionFrame)
        {
            if (s_StartupWaiting)
            {
                // A session that started anew (its frame count restarts) is not asked:
                // once per run.
                if (sessionFrame < 2 || Time.realtimeSinceStartup > s_StartupAskUntil) s_StartupWaiting = false;
                else AskStartupPlacement(true);
                return;
            }
            if (s_StartupDone || sessionFrame < 2) return;
            s_StartupDone = true;
            // Linux players only for now (the native call is 0 everywhere else): no
            // start-up placement lines in a Windows / macOS player's log, and none in
            // the editor.
            if (Application.platform != RuntimePlatform.LinuxPlayer) return;
            if (PlaceOnPanelAtStart && Time.realtimeSinceStartup > StartupWindowSeconds)
            {
                Debug.Log($"[DisplayXR] start-up placement: the session came up {Time.realtimeSinceStartup:F0}s after " +
                          "launch — leaving the window where it is.");
                return;
            }
            AskStartupPlacement(false);
        }

        // @p again: the window was not shown at the first ask; asking again now.
        static void AskStartupPlacement(bool again)
        {
            string meanwhile = again ? " before the window was shown" : "";
            s_StartupWaiting = false;
            if (!PlaceOnPanelAtStart)
            {
                Debug.Log($"[DisplayXR] start-up placement: turned off by the app{meanwhile} — the window stays " +
                          "where it opened.");
                return;
            }
            // A fullscreen window is not ours to move: Unity and the app choose its output.
            if (Screen.fullScreenMode != FullScreenMode.Windowed)
            {
                Debug.Log($"[DisplayXR] start-up placement: {Screen.fullScreenMode}{meanwhile} — not moved.");
                return;
            }
            int r;
            try { r = DisplayXRProviderNative.dxr_prov_place_window_at_start(); }
            catch (System.EntryPointNotFoundException) { return; }
            if (r == 3)
            {
                s_StartupWaiting = true;
                if (again) return; // still not shown: said so at the first ask
                s_StartupAskUntil = Time.realtimeSinceStartup + 60f;
            }
            if (r != 0)
                Debug.Log("[DisplayXR] start-up placement: " + (
                    r == 1 ? "moving the window onto the 3D panel." :
                    r == 2 ? "the window opened on the 3D panel." :
                    r == 3 ? "deciding once the window is shown." :
                    r == -1 ? "the app or the user placed the window — left there." :
                              "the window was not shown in time — left where it opened."));
        }

        /// <summary>
        /// Move now, if the session is running. Returns false when it is not, when the
        /// runtime does not report the panel position, or on macOS.
        /// <para>
        /// This is a move the app asks for: on Linux native Wayland, after the first 3 s
        /// the window has been on screen it keeps the window's device size (as on X11).
        /// The component's own move at start (<see cref="moveOnStart"/>) keeps the size the
        /// window opened with instead, as the default start-up placement does.
        /// </para>
        /// </summary>
        public bool MoveNow() => Move(false);

        // @p atStart: the component's own move once the session runs (moveOnStart), as
        // opposed to one the app asks for later through MoveNow.
        bool Move(bool atStart)
        {
            Attempted = true;
            Succeeded = atStart ? DisplayXRProvider.MoveWindowToDisplayAtStart() : DisplayXRProvider.MoveWindowToDisplay();
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
                        "left where the OS opened it. Making the panel the main " +
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
            Move(true);
        }
    }
}
