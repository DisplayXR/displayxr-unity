// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

using System;
using System.Runtime.InteropServices;
using UnityEngine;

namespace DisplayXR
{
    /// <summary>What <see cref="DisplayXRSideStep"/> is doing.</summary>
    public enum SideStepState
    {
        /// <summary><see cref="DisplayXRSideStep.Enabled"/> is false.</summary>
        Off = 0,
        /// <summary>Not available here (see <see cref="DisplayXRSideStep.Reason"/>): not a
        /// native-Wayland Linux player, the editor, Windows or macOS.</summary>
        Unsupported = 1,
        /// <summary>On, but it cannot act yet: no XR session, no DisplayXR GNOME extension,
        /// the window not shown, or the window has just moved and is settling.</summary>
        Waiting = 2,
        /// <summary>On, holding still: the app's desktop control is not granted
        /// (<see cref="SideStepReason.NotGranted"/>), the screen is locked, the user is moving the
        /// window, it is minimized / maximized / fullscreen, or it stepped aside too often lately.</summary>
        Paused = 3,
        /// <summary>At home, watching.</summary>
        Home = 4,
        /// <summary>Gliding aside, or back home.</summary>
        Moving = 5,
        /// <summary>Stepped aside; goes back home once home is free (if
        /// <see cref="DisplayXRSideStep.ReturnHome"/>).</summary>
        Aside = 6,
    }

    /// <summary>Why <see cref="DisplayXRSideStep.State"/> is what it is.</summary>
    public enum SideStepReason
    {
        /// <summary>Nothing to report.</summary>
        None = 0,
        /// <summary>Not a Linux player (Windows, macOS, the editor).</summary>
        Platform = 1,
        /// <summary>A Linux player running under X11 (XWayland), not native Wayland.</summary>
        NotNativeWayland = 2,
        /// <summary>No native-Wayland XR session yet.</summary>
        NoSession = 3,
        /// <summary>No window list from the DisplayXR GNOME extension (not installed, disabled,
        /// or not a GNOME session).</summary>
        NoExtension = 4,
        /// <summary>The app's window is not shown.</summary>
        NoWindow = 5,
        /// <summary>The window moved (start-up placement, the user, the app) and is settling;
        /// where it stays becomes home.</summary>
        Settling = 6,
        /// <summary>The screen is locked: desktop control is suspended
        /// (<see cref="DesktopControlState.Suspended"/>). Side-step reads nothing and holds still, but
        /// keeps the app's own home and whether it is aside (<see cref="DisplayXRSideStep.IsAside"/>,
        /// <see cref="DisplayXRSideStep.TryGetHomePosition"/>), and carries on from there after the
        /// unlock. The user's kill switch also reads as this while it is on (the runtime reports
        /// both as Suspended); it ends the grant, so after it is turned off the reason becomes
        /// <see cref="NotGranted"/> and home is forgotten.</summary>
        ScreenLocked = 7,
        /// <summary>The user is moving or resizing the window.</summary>
        UserMoving = 8,
        /// <summary>The window is minimized.</summary>
        Minimized = 9,
        /// <summary>The window is maximized.</summary>
        Maximized = 10,
        /// <summary>The window is fullscreen.</summary>
        Fullscreen = 11,
        /// <summary>Covered, but it already stepped aside 6 times in the last minute; it stays for
        /// now. (Going back home is never held by this.)</summary>
        TooManyMoves = 12,
        /// <summary>Covered, but no place on this monitor is free; it stays.</summary>
        NoFreeSpot = 13,
        /// <summary>The native plug-in is older than this feature.</summary>
        NativeMissing = 14,
        /// <summary>The app's desktop control is not <see cref="DesktopControlState.Granted"/>
        /// (<see cref="DisplayXRDesktop.State"/> says why: never requested, pending, denied, turned
        /// off by the user, ended by the kill switch, or a runtime without desktop control; a
        /// screen lock after a grant is <see cref="ScreenLocked"/> instead). Side-step reads nothing
        /// then, the window stays where it is, and home is forgotten.</summary>
        NotGranted = 15,
    }

    /// <summary>Which side <see cref="DisplayXRSideStep"/> prefers among equally free places.</summary>
    public enum SideStepSide
    {
        /// <summary>The shortest move.</summary>
        Auto = 0,
        /// <summary>Left of the covering window, or a left corner of the monitor.</summary>
        Left = 1,
        /// <summary>Right of it, or a right corner.</summary>
        Right = 2,
        /// <summary>Above it, or a top corner.</summary>
        Above = 3,
        /// <summary>Below it, or a bottom corner.</summary>
        Below = 4,
    }

    /// <summary>
    /// <b>Side-step</b>: the app's own window steps aside when the window the user is working
    /// in covers the app's content, and goes back home once that place is free again. Made for
    /// a desktop companion (an avatar in a transparent window) that should never sit in the way
    /// of the document the user is typing in.
    ///
    /// <para>
    /// <b>Where.</b> A Linux player running natively on Wayland, on GNOME with the DisplayXR
    /// GNOME extension. Everywhere else (X11 / XWayland players, Windows, macOS, the editor)
    /// <see cref="State"/> is <see cref="SideStepState.Unsupported"/> and nothing happens.
    /// </para>
    /// <para>
    /// <b>Only while desktop control is granted.</b> Side-step works from the other windows'
    /// frames and focus, which is what <c>DisplayXRNative.LinuxWindows</c> tells about other
    /// apps, so it is behind the same consent gate (#389, runtime ADR-056): it runs only while
    /// <see cref="DisplayXRDesktop.State"/> is <see cref="DesktopControlState.Granted"/>. Ask for
    /// it with <see cref="DisplayXRDesktop.Request"/> on a user gesture; side-step never asks.
    /// Before every look at the desktop (4 times a second) the plug-in asks the runtime, the same
    /// query <see cref="DisplayXRDesktop.State"/> makes. When the answer is no it reads nothing
    /// (no window list, no lock state) and asks again a second later; <see cref="State"/> is
    /// <see cref="SideStepState.Paused"/> / <see cref="SideStepReason.NotGranted"/>,
    /// <see cref="Covered"/> is 0, no rect or home is reported, and no event fires. A revoke or the
    /// kill switch stops it within one look, mid-move too: the window stays where it is and
    /// side-step forgets its home. When the grant comes back it starts afresh, as after turning it
    /// off and on: the place the window stays put at becomes home.
    /// </para>
    /// <para>
    /// <b>A screen lock</b> suspends desktop control: side-step stops within one look the same way
    /// and reads nothing (<see cref="SideStepState.Paused"/> /
    /// <see cref="SideStepReason.ScreenLocked"/>), but the home is the app's own window's data, so
    /// it is kept, and so is whether the window is aside. Only the memory of the other window it
    /// stepped aside for is dropped. After the unlock it carries on: aside, it goes back once home
    /// is free of the window that has the focus.
    /// </para>
    /// <para>
    /// <b>What the app learns: only its own window.</b> The plug-in reads the other windows'
    /// frames and the keyboard focus from the GNOME extension, decides inside the plug-in, and
    /// moves only the app's own window. No other window's id, process, app id, title or rect
    /// is handed to the app, and none is logged. The app sets the switch and the settings, and
    /// reads its own window's state: <see cref="State"/>, <see cref="Covered"/>,
    /// <see cref="IsAside"/>, its rect and its home, and the <see cref="SteppedAside"/> /
    /// <see cref="ReturnedHome"/> events (docs~/architecture/side-step.md).
    /// </para>
    /// <para>
    /// <b>Rules.</b> "Covered": the focused window of another app (a normal window, dialog or
    /// utility window; never the desktop-icons window, a menu, a panel or the app's own windows)
    /// covers at least <see cref="Trigger"/> of the app's content for <see cref="Dwell"/>
    /// seconds. Then the window glides (<see cref="GlideSeconds"/>) beside that window or to a
    /// corner of its monitor (least covered first, then <see cref="PreferredSide"/>, then the
    /// shortest move; it never leaves its monitor). Once home is free of the focused window and
    /// of the window it stepped aside for for <see cref="ReturnDelay"/> seconds, it glides back.
    /// The app's content is the transparent overlay's click-through silhouette, or the rect from
    /// <see cref="SetContentRect"/>, or the whole window.
    /// </para>
    /// <para>
    /// <b>Safety.</b> It never moves another window. It holds still while desktop control is not
    /// granted, while the user moves the window (where they leave it is the new home), while the
    /// screen is locked, and while the window is minimized, maximized or fullscreen. Its own moves
    /// never become home: a place the window stays at after anyone else moved it (the user, the
    /// app's own <c>displayxr_set_overlay_position</c>) does. At most 6 steps aside a minute (going
    /// back home is never held by that; a revoke and a re-grant do not reset the count). Turning it
    /// off stops at once; the window stays where it is.
    /// </para>
    /// <para>
    /// <b>Saving the window's place.</b> While <see cref="IsAside"/>, the window is not where
    /// the user put it: save <see cref="TryGetHomePosition"/> instead of
    /// <c>displayxr_get_overlay_position</c>.
    /// </para>
    /// Main thread only. The provider driver pumps the state and the events once a frame.
    /// </summary>
    public static class DisplayXRSideStep
    {
        // --- the switch and the settings ------------------------------------------------

        /// <summary>Turn side-step on. Default off. It acts only while desktop control is granted
        /// (<see cref="DisplayXRDesktop.State"/>); until then <see cref="State"/> is
        /// <see cref="SideStepState.Paused"/> / <see cref="SideStepReason.NotGranted"/>.</summary>
        public static bool Enabled
        {
            get => s_Enabled;
            set { if (s_Enabled != value) { s_Enabled = value; Push(); } }
        }

        /// <summary>The share of the app's content (0..1) the window in front must cover. Default 0.15.</summary>
        public static float Trigger
        {
            get => s_Settings.trigger;
            set { if (s_Settings.trigger != value) { s_Settings.trigger = value; Push(); } }
        }

        /// <summary>Covered this long (seconds) before it steps aside. Default 0.6.</summary>
        public static float Dwell
        {
            get => s_Settings.dwellS;
            set { if (s_Settings.dwellS != value) { s_Settings.dwellS = value; Push(); } }
        }

        /// <summary>Home free this long (seconds) before it goes back. Default 2.</summary>
        public static float ReturnDelay
        {
            get => s_Settings.returnDelayS;
            set { if (s_Settings.returnDelayS != value) { s_Settings.returnDelayS = value; Push(); } }
        }

        /// <summary>A move glides this long (seconds, ease-out); 0 jumps. Default 0.3 (at most 2).</summary>
        public static float GlideSeconds
        {
            get => s_Settings.glideS;
            set { if (s_Settings.glideS != value) { s_Settings.glideS = value; Push(); } }
        }

        /// <summary>Go back home once it is free. Default true. False: the place it stepped to
        /// becomes home.</summary>
        public static bool ReturnHome
        {
            get => s_Settings.returnHome != 0;
            set { int v = value ? 1 : 0; if (s_Settings.returnHome != v) { s_Settings.returnHome = v; Push(); } }
        }

        /// <summary>The preferred side among equally free places. Default <see cref="SideStepSide.Auto"/>.</summary>
        public static SideStepSide PreferredSide
        {
            get => (SideStepSide)s_Settings.side;
            set { if (s_Settings.side != (int)value) { s_Settings.side = (int)value; Push(); } }
        }

        /// <summary>Desktop px kept between the app's content and the other window or the
        /// monitor's edge. Default 24.</summary>
        public static int Margin
        {
            get => s_Settings.margin;
            set { if (s_Settings.margin != value) { s_Settings.margin = value; Push(); } }
        }

        /// <summary>
        /// The app's content inside its window, in window device px (the space of
        /// <c>DisplayXRNative.displayxr_get_overlay_size</c>; origin top-left), e.g. the avatar's
        /// 3D zone. Only this part counts as covered, and this part keeps the margin. Without it:
        /// the transparent overlay's click-through silhouette, else the whole window.
        /// </summary>
        public static void SetContentRect(RectInt windowPx)
        {
            s_ContentRect = windowPx;
            s_ContentRectSet = windowPx.width > 0 && windowPx.height > 0;
            PushContentRect();
        }

        /// <summary>Back to the automatic content (silhouette, else the whole window).</summary>
        public static void ClearContentRect()
        {
            s_ContentRectSet = false;
            PushContentRect();
        }

        // --- what the app learns: its own window ----------------------------------------

        /// <summary>What it is doing (pumped once a frame).</summary>
        public static SideStepState State { get; private set; }

        /// <summary>Why (pumped once a frame).</summary>
        public static SideStepReason Reason { get; private set; }

        /// <summary>True while the window is away from home because of side-step (stepped
        /// aside, or gliding either way). Save <see cref="TryGetHomePosition"/> then. Kept while the
        /// screen is locked; false while desktop control is not granted (side-step has forgotten
        /// home then).</summary>
        public static bool IsAside { get; private set; }

        /// <summary>The share (0..1) of the app's content the focused window of another app
        /// covers now. 0 when nothing does or side-step cannot tell (always 0 while desktop
        /// control is not granted).</summary>
        public static float Covered { get; private set; }

        /// <summary>The app's window frame in GNOME desktop coordinates (the space of
        /// <c>DisplayXRNative.LinuxMonitor.Rect</c>), as side-step last saw it (kept while the
        /// screen is locked). False while desktop control is not granted.</summary>
        public static bool TryGetWindowRect(out RectInt rect)
        {
            rect = new RectInt(s_Status.winX, s_Status.winY, s_Status.winW, s_Status.winH);
            return State > SideStepState.Unsupported && s_Status.windowValid != 0;
        }

        /// <summary>Home: the window frame's place the user (or the app) last left it at, in
        /// GNOME desktop coordinates, with the window's current size. Kept while the screen is
        /// locked; false while desktop control is not granted.</summary>
        public static bool TryGetHomeRect(out RectInt rect)
        {
            rect = new RectInt(s_Status.homeX, s_Status.homeY, s_Status.winW, s_Status.winH);
            return State > SideStepState.Unsupported && s_Status.homeValid != 0;
        }

        /// <summary>
        /// Home in the window-position API's space (X root px: what
        /// <c>DisplayXRNative.displayxr_get_overlay_position</c> returns and
        /// <c>displayxr_set_overlay_position</c> takes). Save this instead of the current
        /// position while <see cref="IsAside"/>. False when not known.
        /// </summary>
        public static bool TryGetHomePosition(out Vector2Int position)
        {
            position = new Vector2Int(s_Status.homeXrootX, s_Status.homeXrootY);
            return State > SideStepState.Unsupported && s_Status.homeXrootValid != 0;
        }

        /// <summary>The window stepped aside and has stopped; its new frame (GNOME desktop
        /// coordinates). Main thread.</summary>
        public static event Action<RectInt> SteppedAside;

        /// <summary>The window is back home and has stopped; its frame. Main thread.</summary>
        public static event Action<RectInt> ReturnedHome;

        /// <summary><see cref="State"/> or <see cref="Reason"/> changed. Main thread.</summary>
        public static event Action<SideStepState, SideStepReason> StateChanged;

        // --- internals --------------------------------------------------------------------

        // Byte-for-byte mirrors of DisplayXRSideStepSettings / DisplayXRSideStepStatus in
        // native~/displayxr_linux_side_step.h (its unit test pins 28 / 68 bytes).
        [StructLayout(LayoutKind.Sequential)]
        private struct NativeSettings
        {
            public float trigger, dwellS, returnDelayS, glideS;
            public int returnHome, side, margin;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct NativeStatus
        {
            public int state, reason;
            public float covered;
            public int aside, windowValid, winX, winY, winW, winH;
            public int homeValid, homeX, homeY;
            public int homeXrootValid, homeXrootX, homeXrootY;
            public uint asideCount, homeCount;
        }

        private static bool s_Enabled;
        private static NativeSettings s_Settings = new NativeSettings
        {
            trigger = 0.15f, dwellS = 0.6f, returnDelayS = 2f, glideS = 0.3f, returnHome = 1, side = 0, margin = 24,
        };
        private static RectInt s_ContentRect;
        private static bool s_ContentRectSet;
        private static NativeStatus s_Status;
        private static bool s_Primed;
        private static uint s_AsideSeen, s_HomeSeen;
        private static bool s_NativeMissing;

#if UNITY_STANDALONE_LINUX && !UNITY_EDITOR
        private const string LibName = "displayxr_unity";

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        private static extern int displayxr_linux_side_step_set(int enabled, ref NativeSettings settings);

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        private static extern void displayxr_linux_side_step_set_body_rect(int x, int y, int w, int h);

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        private static extern void displayxr_linux_side_step_status(out NativeStatus status);
#endif

        private static void Push()
        {
#if UNITY_STANDALONE_LINUX && !UNITY_EDITOR
            if (s_NativeMissing) return;
            try
            {
                displayxr_linux_side_step_set(s_Enabled ? 1 : 0, ref s_Settings);
                if (s_Enabled) PushContentRect();
            }
            catch (Exception e) when (e is DllNotFoundException || e is EntryPointNotFoundException)
            {
                NativeMissing(e);
            }
#endif
            Tick(); // the state follows at once, not on the next frame
        }

        private static void PushContentRect()
        {
#if UNITY_STANDALONE_LINUX && !UNITY_EDITOR
            if (s_NativeMissing) return;
            try
            {
                if (s_ContentRectSet)
                    displayxr_linux_side_step_set_body_rect(s_ContentRect.x, s_ContentRect.y, s_ContentRect.width,
                                                            s_ContentRect.height);
                else
                    displayxr_linux_side_step_set_body_rect(0, 0, 0, 0);
            }
            catch (Exception e) when (e is DllNotFoundException || e is EntryPointNotFoundException)
            {
                NativeMissing(e);
            }
#endif
        }

        private static void NativeMissing(Exception e)
        {
            if (s_NativeMissing) return;
            s_NativeMissing = true;
            Debug.LogWarning("[DisplayXR] Side-step unavailable: the native plug-in has no " +
                             "displayxr_linux_side_step_* exports (stale binary?). " + e.Message);
        }

        /// <summary>
        /// Per-frame pump, called by <c>DisplayXRProviderDriver.LateUpdate</c> (also when no
        /// session runs). Apps do not call this.
        /// </summary>
        internal static void Tick()
        {
            NativeStatus st = default;
            SideStepReason fallback = SideStepReason.Platform;
#if UNITY_STANDALONE_LINUX && !UNITY_EDITOR
            fallback = SideStepReason.NativeMissing;
            if (!s_NativeMissing)
            {
                try
                {
                    displayxr_linux_side_step_status(out st);
                }
                catch (Exception e) when (e is DllNotFoundException || e is EntryPointNotFoundException)
                {
                    NativeMissing(e);
                }
            }
            bool native = !s_NativeMissing;
#else
            bool native = false;
#endif
            if (!native)
            {
                st = default;
                st.state = s_Enabled ? (int)SideStepState.Unsupported : (int)SideStepState.Off;
                st.reason = s_Enabled ? (int)fallback : 0;
            }
            else if (!s_Enabled)
            {
                st.state = (int)SideStepState.Off;
                st.reason = 0;
            }
            Apply(st);
        }

        private static void Apply(NativeStatus st)
        {
            var oldState = State;
            var oldReason = Reason;
            s_Status = st;
            State = (SideStepState)st.state;
            Reason = (SideStepReason)st.reason;
            IsAside = st.aside != 0 && State > SideStepState.Unsupported;
            Covered = st.covered;
            var rect = new RectInt(st.winX, st.winY, st.winW, st.winH);
            if (!s_Primed)
            {
                // The counters run for the process; only steps after the first look count.
                s_Primed = true;
                s_AsideSeen = st.asideCount;
                s_HomeSeen = st.homeCount;
            }
            if (st.asideCount != s_AsideSeen)
            {
                s_AsideSeen = st.asideCount;
                Invoke(SteppedAside, rect, nameof(SteppedAside));
            }
            if (st.homeCount != s_HomeSeen)
            {
                s_HomeSeen = st.homeCount;
                Invoke(ReturnedHome, rect, nameof(ReturnedHome));
            }
            if (State != oldState || Reason != oldReason)
            {
                try { StateChanged?.Invoke(State, Reason); }
                catch (Exception e) { Debug.LogException(e); }
            }
        }

        private static void Invoke(Action<RectInt> handler, RectInt rect, string what)
        {
            try { handler?.Invoke(rect); }
            catch (Exception e)
            {
                Debug.LogError($"[DisplayXR] Side-step: a {what} handler threw");
                Debug.LogException(e);
            }
        }
    }
}
