// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading.Tasks;
using UnityEngine;

namespace DisplayXR
{
    /// <summary>Where the app stands with desktop control. Only <see cref="Granted"/> allows operations.</summary>
    public enum DesktopControlState
    {
        /// <summary>No backend on this OS / desktop, a runtime without <c>XR_DXR_desktop_control</c>,
        /// or no XR session right now.</summary>
        Unsupported = 0,
        /// <summary>Never requested in this session; call <see cref="DisplayXRDesktop.Request"/>.</summary>
        NotRequested = 1,
        /// <summary>A consent prompt (the OS's or DisplayXR's) is up; wait for <see cref="DisplayXRDesktop.StateChanged"/>.</summary>
        Pending = 2,
        Granted = 3,
        /// <summary>The user (or the OS) said no. Ask again only on a new user gesture.</summary>
        Denied = 4,
        /// <summary>The user turned desktop control off for this app (<c>displayxr-cli desktop deny</c>).</summary>
        DisabledByUser = 5,
        /// <summary>Temporarily unavailable: the user's kill switch is on, or the screen is locked.</summary>
        Suspended = 6,
    }

    /// <summary>What this OS backend can do; may be a subset.</summary>
    [Flags]
    public enum DesktopControlCapabilities : ulong
    {
        None = 0,
        ListWindows = 0x1,
        PointerPosition = 0x2,
        Pick = 0x4,
        ReadText = 0x8,
        WindowActions = 0x10,
        Input = 0x20,
        /// <summary>The backend can capture screen pixels (spec v2). Without it
        /// <see cref="DisplayXRDesktop.CaptureState"/> is <see cref="DesktopControlState.Unsupported"/>.</summary>
        Capture = 0x40,
    }

    /// <summary>
    /// The consent scopes of <see cref="DisplayXRDesktop.RequestAccess"/> (spec v2). Each has its own
    /// state: <see cref="DisplayXRDesktop.State"/> is CONTROL's, <see cref="DisplayXRDesktop.CaptureState"/>
    /// is CAPTURE's.
    /// </summary>
    [Flags]
    public enum DesktopControlScopes : ulong
    {
        None = 0,
        /// <summary>Everything of spec v1: observation, window actions, input.</summary>
        Control = 0x1,
        /// <summary>Screen pixels (<see cref="DisplayXRDesktop.CaptureAsync(RectInt, int, int)"/>).</summary>
        Capture = 0x2,
    }

    /// <summary>State bits of a desktop window.</summary>
    [Flags]
    public enum DesktopWindowState : ulong
    {
        None = 0,
        Focused = 0x1,
        Minimized = 0x2,
        Maximized = 0x4,
        Fullscreen = 0x8,
        /// <summary>The window belongs to this app's own process.</summary>
        Own = 0x10,
    }

    /// <summary>What a click at a point would meet.</summary>
    public enum DesktopPickKind
    {
        /// <summary>A top-level window (see <see cref="DesktopPick.Window"/>).</summary>
        Window = 0,
        /// <summary>The desktop background / icons.</summary>
        Desktop = 1,
        /// <summary>Taskbar, dock, panel, system UI.</summary>
        Shell = 2,
        /// <summary>Not on any monitor.</summary>
        Outside = 3,
    }

    public enum DesktopPointerButton
    {
        Left = 0,
        Right = 1,
        Middle = 2,
    }

    /// <summary>How a desktop action ended.</summary>
    public enum DesktopActionStatus
    {
        Success,
        /// <summary>The safety layer refused it: screen locked, kill switch on, the user kept
        /// typing, or the target window was not the one under the point / with focus.
        /// <see cref="DesktopActionResult.Message"/> says which.</summary>
        Refused,
        /// <summary>Desktop control is not granted for this app.</summary>
        NotGranted,
        /// <summary>The target window no longer exists.</summary>
        WindowLost,
        /// <summary>This OS backend cannot do it, or the runtime / plug-in lacks the extension.</summary>
        Unsupported,
        /// <summary>Anything else (invalid action, too many queued, the session ended ...).</summary>
        Failed,
    }

    /// <summary>The outcome of one desktop action.</summary>
    public readonly struct DesktopActionResult
    {
        public DesktopActionStatus Status { get; }
        /// <summary>Human-readable detail from the runtime (may be empty).</summary>
        public string Message { get; }
        /// <summary>The raw <c>XrResult</c> (or a binding code below -2000000000).</summary>
        public int XrResult { get; }
        public bool Succeeded => Status == DesktopActionStatus.Success;

        public DesktopActionResult(DesktopActionStatus status, string message, int xrResult)
        {
            Status = status;
            Message = message ?? "";
            XrResult = xrResult;
        }

        public override string ToString() =>
            Message.Length > 0 ? $"{Status} ({XrResult}): {Message}" : $"{Status} ({XrResult})";
    }

    /// <summary>
    /// The outcome of one screen capture. On success <see cref="Pixels"/> holds
    /// <see cref="Width"/> x <see cref="Height"/> RGBA8 pixels exactly as the runtime delivered them:
    /// sRGB-encoded, straight alpha, <b>top row first</b>, stride Width * 4. Withheld pixels (off every
    /// monitor, on a monitor the user did not share, inside an excluded window or one of this app's
    /// own windows) are (0,0,0,0); every other pixel has alpha 255. <see cref="ToTexture2D"/> flips the
    /// rows for Unity.
    /// </summary>
    public readonly struct DesktopCaptureResult
    {
        public DesktopActionStatus Status { get; }
        /// <summary>Human-readable detail from the runtime (may be empty).</summary>
        public string Message { get; }
        /// <summary>The raw <c>XrResult</c> (or a binding code below -2000000000).</summary>
        public int XrResult { get; }
        public int Width { get; }
        public int Height { get; }
        /// <summary>What the image covers, desktop coordinates (top-left origin, y down).</summary>
        public RectInt SourceRect { get; }
        /// <summary>The <c>XrTime</c> (ns) at which these pixels were current.</summary>
        public long FrameTime { get; }
        /// <summary>RGBA8, top row first; null unless <see cref="Succeeded"/>.</summary>
        public byte[] Pixels { get; }
        public bool Succeeded => Status == DesktopActionStatus.Success && Pixels != null;

        public DesktopCaptureResult(DesktopActionStatus status, string message, int xrResult, int width, int height,
                                    RectInt sourceRect, long frameTime, byte[] pixels)
        {
            Status = status;
            Message = message ?? "";
            XrResult = xrResult;
            Width = width;
            Height = height;
            SourceRect = sourceRect;
            FrameTime = frameTime;
            Pixels = pixels;
        }

        /// <summary>
        /// The desktop point at the centre of image pixel (<paramref name="px"/>, <paramref name="py"/>),
        /// with <paramref name="py"/> counted from the TOP row as in <see cref="Pixels"/>. Feed it straight
        /// to <see cref="DisplayXRDesktop.TryPick"/> or a pointer action.
        /// </summary>
        public Vector2Int ImageToDesktop(int px, int py) =>
            Width <= 0 || Height <= 0
                ? SourceRect.position
                : new Vector2Int(
                      SourceRect.x + Mathf.FloorToInt((px + 0.5f) * SourceRect.width / Width),
                      SourceRect.y + Mathf.FloorToInt((py + 0.5f) * SourceRect.height / Height));

        /// <summary>
        /// A new <c>RGBA32</c> sRGB texture of the image, rows flipped so it displays upright in Unity
        /// (Unity textures are bottom row first). Null unless <see cref="Succeeded"/>. Call it on the
        /// main thread; the caller owns (and destroys) the texture.
        /// </summary>
        public Texture2D ToTexture2D()
        {
            if (!Succeeded || Width <= 0 || Height <= 0) return null;
            int stride = Width * 4;
            var flipped = new byte[Pixels.Length];
            for (int row = 0; row < Height; row++)
                Buffer.BlockCopy(Pixels, row * stride, flipped, (Height - 1 - row) * stride, stride);
            var tex = new Texture2D(Width, Height, TextureFormat.RGBA32, false, false);
            tex.LoadRawTextureData(flipped);
            tex.Apply(false, false);
            return tex;
        }

        public override string ToString() =>
            Succeeded ? $"{Width}x{Height} of {SourceRect}"
            : Message.Length > 0 ? $"{Status} ({XrResult}): {Message}" : $"{Status} ({XrResult})";
    }

    /// <summary>One top-level desktop window.</summary>
    public readonly struct DesktopWindow
    {
        /// <summary>Stable for the window's lifetime, never 0 (HWND / CGWindowID / mutter id).</summary>
        public ulong Id { get; }
        /// <summary>Owner of a dialog / transient; 0 if none.</summary>
        public ulong ParentId { get; }
        public uint ProcessId { get; }
        /// <summary>Executable base name / bundle id / app id.</summary>
        public string AppId { get; }
        public string Title { get; }
        /// <summary>Desktop coordinates (see <see cref="DisplayXRDesktop"/>): x/y is the top-left, y down.</summary>
        public RectInt Frame { get; }
        public DesktopWindowState State { get; }
        /// <summary>0 = top-most of the listed windows.</summary>
        public uint ZOrder { get; }

        public bool IsFocused => (State & DesktopWindowState.Focused) != 0;
        public bool IsMinimized => (State & DesktopWindowState.Minimized) != 0;
        /// <summary>True for this app's own windows.</summary>
        public bool IsOwn => (State & DesktopWindowState.Own) != 0;

        public DesktopWindow(ulong id, ulong parentId, uint processId, string appId, string title, RectInt frame,
                             DesktopWindowState state, uint zOrder)
        {
            Id = id;
            ParentId = parentId;
            ProcessId = processId;
            AppId = appId ?? "";
            Title = title ?? "";
            Frame = frame;
            State = state;
            ZOrder = zOrder;
        }

        public override string ToString() => $"[{Id}] {AppId}: {Title} {Frame}";
    }

    /// <summary>What a click at <see cref="Point"/> would meet (the OS's own hit test).</summary>
    public readonly struct DesktopPick
    {
        /// <summary>Desktop coordinates.</summary>
        public Vector2Int Point { get; }
        public DesktopPickKind Kind { get; }
        /// <summary>Valid when <see cref="Kind"/> is <see cref="DesktopPickKind.Window"/>.</summary>
        public DesktopWindow Window { get; }

        public DesktopPick(Vector2Int point, DesktopPickKind kind, DesktopWindow window)
        {
            Point = point;
            Kind = kind;
            Window = window;
        }
    }

    /// <summary>
    /// Let the app's AI agent see and operate the user's <b>desktop</b>
    /// (<c>XR_DXR_desktop_control</c>, runtime ADR-056): list windows, find what is under the
    /// pointer, read UI text, activate / move / minimize / close windows, and type, press keys,
    /// click, drag and scroll in other apps. One API on Windows, macOS and GNOME Wayland.
    ///
    /// <code>
    /// async void OnUserSaidYes()   // a user gesture
    /// {
    ///     DisplayXRDesktop.StateChanged += s => Debug.Log($"desktop control: {s}");
    ///     if (DisplayXRDesktop.State != DesktopControlState.Granted)
    ///     {
    ///         DisplayXRDesktop.Request("so Lumo can open apps and type for you");
    ///         return;                         // wait for StateChanged(Granted)
    ///     }
    ///     foreach (DesktopWindow w in DisplayXRDesktop.ListWindows())
    ///     {
    ///         if (w.AppId != "notepad.exe") continue;
    ///         var r = await DisplayXRDesktop.Activate(w.Id);     // focus FIRST ...
    ///         if (r.Succeeded) r = await DisplayXRDesktop.TypeText(w.Id, "你好 👋");
    ///         if (r.Succeeded) r = await DisplayXRDesktop.PressKeys(w.Id, "ctrl+s");
    ///         if (!r.Succeeded) Debug.LogWarning($"desktop: {r}");
    ///         break;
    ///     }
    /// }
    /// </code>
    ///
    /// <para><b>The permission is the app's.</b> The runtime runs every operation inside this
    /// app's process, through the OS's own permission path, so the OS asks the user about
    /// <i>this app</i>, by name: macOS shows Accessibility (and Screen Recording) for the app's
    /// bundle; GNOME shows the RemoteDesktop portal dialog for the app id; Windows, which has
    /// no such prompt, shows a DisplayXR dialog naming the app plus your <c>reason</c>. Nothing
    /// works before the user said yes (<see cref="State"/> = <see cref="DesktopControlState.Granted"/>).
    /// Call <see cref="Request"/> in response to a user gesture, never at startup, and do not
    /// re-ask after <see cref="DesktopControlState.Denied"/> without a new gesture. The user can
    /// suspend everything at any time (<c>displayxr-cli desktop stop</c>) or turn this app off
    /// (<c>displayxr-cli desktop deny</c>); every mutating action lands in their local audit log
    /// (typed text never does, only its length).</para>
    ///
    /// <para><b>Coordinates</b> are DESKTOP coordinates, the OS's global input space — Windows:
    /// physical virtual-desktop pixels; macOS: global points, top-left of the primary display,
    /// y down; GNOME: logical (stage) pixels. Window frames and action points use the same space,
    /// so a frame from <see cref="ListWindows"/> or a point from <see cref="TryGetPointer"/> can be
    /// fed to an action unchanged. They are NOT Unity screen coordinates (y up, window-relative)
    /// and NOT Windows-virtualized managed-code pixels — never mix them with
    /// <c>Screen</c>/<c>Input.mousePosition</c> values.</para>
    ///
    /// <para><b>The safety layer</b> (the same on every OS) runs before each action: the state must
    /// still be Granted (a lock screen or the kill switch refuses it); input — and
    /// <see cref="Activate"/> — waits until the user has been idle for a moment and is refused
    /// after 10 s of continuous activity, so it never fights live typing; and immediately before
    /// a keystroke or click the runtime verifies the target: pointer actions must land on the
    /// target window (or its dialog), text and keys need the target to have keyboard focus.
    /// A failed check fails the action with <see cref="DesktopActionStatus.Refused"/> — input is
    /// never delivered to a different window. So: <b><see cref="Activate"/> the window before
    /// <see cref="TypeText"/> / <see cref="PressKeys"/></b>, and re-pick before clicking if the
    /// desktop may have changed.</para>
    ///
    /// <para><b>Actions are asynchronous</b> and run in submission order; each returned task
    /// completes on the main thread (drained from the provider driver's <c>LateUpdate</c>) with
    /// exactly one <see cref="DesktopActionResult"/>. A task never throws: a bad action, a
    /// missing extension or an ended session complete it with a failed status. Observation
    /// calls (<see cref="ListWindows"/>, <see cref="TryGetPointer"/>, <see cref="TryPick"/>,
    /// <see cref="GetTextAt"/>) are synchronous, never wait on a queued action, and are cheap
    /// enough for a frame loop (text-at may take tens of ms). Use this class from the main
    /// thread.</para>
    ///
    /// <para><b>Never expose these operations as MCP tools</b> (<see cref="DisplayXRMcp"/>), nor
    /// over any other channel other processes can reach (ADR-056 D4). Any local process of the
    /// same user can call an app's MCP tools; a "type_text" tool would hand the user's grant —
    /// given to <i>this</i> app — to every one of them. Desktop control is for the agent that
    /// lives inside the app.</para>
    ///
    /// <para><b>Screen capture</b> (spec v2) is a second consent scope next to CONTROL: ask for both in
    /// ONE request (<c>RequestAccess(DesktopControlScopes.Control | DesktopControlScopes.Capture, reason)</c>
    /// — on GNOME a later request for CAPTURE alone shows a second dialog that asks about input again),
    /// watch <see cref="CaptureStateChanged"/>, then <see cref="CaptureAsync(RectInt, int, int)"/>. A
    /// capture never prompts; one is in flight at a time and starts are at least 250 ms apart. Frames are
    /// for the agent inside this app: never write them anywhere other processes can reach. On Linux the
    /// consent dialog is parented to the app's window automatically
    /// (<see cref="DisplayXRNative"/><c>.LinuxWindowHandle()</c>, read right before each request). On a
    /// runtime whose extension predates spec v2 <see cref="CaptureState"/> is Unsupported and every
    /// capture completes with <see cref="DesktopActionStatus.Unsupported"/>; the rest is unchanged.</para>
    ///
    /// <para><b>Graceful absence.</b> On a runtime without the extension, an older native
    /// plug-in, or with no XR session, <see cref="State"/> is
    /// <see cref="DesktopControlState.Unsupported"/>, observation returns nothing and actions
    /// complete with <see cref="DesktopActionStatus.Unsupported"/> / <see cref="DesktopActionStatus.Failed"/>.
    /// When the session ends, every pending action completes with Failed ("session ended").</para>
    /// </summary>
    public static class DisplayXRDesktop
    {
        // Binding codes (displayxr_provider_desktop.h); outside every XrResult range.
        private const int E_Unavailable = -2000000001;
        private const int E_NoSession = -2000000002;
        private const int E_BadArgument = -2000000003;
        private const int E_NoCapture = -2000000004;

        private const int XR_ERROR_VALIDATION_FAILURE = -1;
        private const int XR_ERROR_FUNCTION_UNSUPPORTED = -7;
        private const int XR_ERROR_FEATURE_UNSUPPORTED = -8;
        private const int XR_ERROR_LIMIT_REACHED = -10;
        private const int XR_ERROR_SIZE_INSUFFICIENT = -11;
        private const int XR_ERROR_DESKTOP_CONTROL_REFUSED_DXR = -1004999400;
        private const int XR_ERROR_DESKTOP_CONTROL_NOT_GRANTED_DXR = -1004999401;
        private const int XR_ERROR_DESKTOP_WINDOW_LOST_DXR = -1004999402;

        private const int EventActionCompleted = 1;
        private const int EventStateChanged = 2;
        private const int EventCaptureCompleted = 3;
        private const int EventCaptureStateChanged = 4;

        private const ulong ScopeAskAgain = 0x100;

        /// <summary>The largest <c>maxWidth</c> / <c>maxHeight</c> a capture accepts (no downscale below it).</summary>
        public const int MaxCaptureSize = 8192;

        private struct Pending
        {
            public TaskCompletionSource<DesktopActionResult> Tcs;
            public uint Generation;
        }

        private static readonly object s_Lock = new object();
        private static readonly Dictionary<ulong, Pending> s_Pending = new Dictionary<ulong, Pending>();
        private static bool s_Missing; // native binary predates the binding (or layout mismatch)
        private static bool s_CaptureMissing; // native binary predates the spec-v2 exports
        private static uint s_Generation;
        private static DesktopControlState s_LastState = DesktopControlState.Unsupported;
        private static DesktopControlState s_LastCaptureState = DesktopControlState.Unsupported;

        private struct PendingCapture
        {
            public TaskCompletionSource<DesktopCaptureResult> Tcs;
            public uint Generation;
        }

        private static readonly Dictionary<ulong, PendingCapture> s_PendingCaptures =
            new Dictionary<ulong, PendingCapture>();

        static unsafe DisplayXRDesktop()
        {
            // A layout drift would corrupt every call; refuse the binding instead. Runs before
            // any member of this class is touched, so no call ever sees a wrong layout.
            if (sizeof(NativeWindow) != 432 || sizeof(NativePick) != 448 || sizeof(NativeAction) != 64 ||
                sizeof(NativeEvent) != 312 || sizeof(NativeCaptureInfo) != 40)
            {
                Debug.LogError("[DisplayXR] desktop control: native struct layout mismatch; disabled");
                s_Missing = true;
            }
        }

        /// <summary>
        /// Raised on the main thread when the consent state changes — the answer to
        /// <see cref="Request"/>, the kill switch, a lock screen, a session ending.
        /// </summary>
        public static event Action<DesktopControlState> StateChanged;

        /// <summary>
        /// Raised on the main thread when the CAPTURE scope's state changes (spec v2) — the answer to a
        /// <see cref="RequestAccess"/> that listed <see cref="DesktopControlScopes.Capture"/>, a
        /// revocation, the kill switch, a lock screen, a session ending.
        /// </summary>
        public static event Action<DesktopControlState> CaptureStateChanged;

        /// <summary>The raw result of the last observation / request call (0 = success).</summary>
        public static int LastResult { get; private set; }

        /// <summary>True while a session is up on a runtime with <c>XR_DXR_desktop_control</c>.
        /// Says nothing about consent — see <see cref="State"/>.</summary>
        public static bool IsAvailable => Call(() => Native.dxr_prov_desktop_available()) == 1;

        /// <summary>The current consent state (a synchronous runtime query).</summary>
        public static DesktopControlState State
        {
            get
            {
                QueryState(out DesktopControlState s, out _);
                return s;
            }
        }

        /// <summary>What this OS backend can do (None unless a session is up).</summary>
        public static DesktopControlCapabilities Capabilities
        {
            get
            {
                QueryState(out _, out DesktopControlCapabilities c);
                return c;
            }
        }

        /// <summary>True while a session is up on a runtime with spec-v2 screen capture. Says nothing
        /// about consent — see <see cref="CaptureState"/>.</summary>
        public static bool IsCaptureAvailable => CallCapture(() => Native.dxr_prov_desktop_capture_available()) == 1;

        /// <summary>The CAPTURE scope's consent state (a synchronous runtime query);
        /// <see cref="DesktopControlState.Unsupported"/> on a runtime without spec-v2 capture.</summary>
        public static DesktopControlState CaptureState
        {
            get
            {
                QueryCaptureState(out DesktopControlState s);
                return s;
            }
        }

        /// <summary>
        /// Ask the user for desktop control. Returns at once; the state goes
        /// <see cref="DesktopControlState.Pending"/> and the answer arrives through
        /// <see cref="StateChanged"/> (a remembered grant resolves without a prompt). Call it from
        /// a user gesture.
        /// </summary>
        /// <param name="reason">Shown in DisplayXR's prompt where DisplayXR draws one (Windows) and
        /// written to the audit log, e.g. "so Lumo can open apps and type for you". May be null.</param>
        /// <returns>False when the request could not be made (no session, no extension).</returns>
        public static bool Request(string reason) => RequestAccess(DesktopControlScopes.Control, reason);

        /// <summary>
        /// Ask the user for one or more consent scopes in ONE prompt (spec v2). Returns at once; each
        /// listed scope goes <see cref="DesktopControlState.Pending"/> and its answer arrives through
        /// <see cref="StateChanged"/> (CONTROL) / <see cref="CaptureStateChanged"/> (CAPTURE). A scope
        /// already Granted is not asked again unless <paramref name="askAgain"/>, which shows the prompt
        /// again so the user can change what is shared (GNOME: which screens) or decline — on macOS it
        /// opens System Settings instead. Use <paramref name="askAgain"/> only on an explicit user
        /// gesture ("change what the assistant can see"). On Linux the dialog is parented to the app's
        /// window. Against a runtime without spec v2, Control is requested exactly as
        /// <see cref="Request"/> always did, and Capture alone fails (<see cref="LastResult"/>).
        /// </summary>
        /// <param name="scopes">What to ask for; <see cref="DesktopControlScopes.None"/> means Control.</param>
        /// <param name="reason">Shown where DisplayXR draws the prompt (Windows) and written to the audit
        /// log. May be null.</param>
        /// <param name="askAgain">Re-ask even for scopes already granted.</param>
        /// <returns>False when the request could not be made (no session, no extension, no capture).</returns>
        public static bool RequestAccess(DesktopControlScopes scopes, string reason = null, bool askAgain = false)
        {
            byte[] r = string.IsNullOrEmpty(reason) ? null : Utf8(reason);
            ulong bits = (ulong)scopes | (askAgain ? ScopeAskAgain : 0);
            byte[] parent = ParentWindow();
            int res = CallCapture(() => Native.dxr_prov_desktop_request_scopes(r, bits, parent));
            if (s_CaptureMissing)
            {
                // A native binary without the v2 export: the v1 request, CONTROL only.
                bool control = scopes == DesktopControlScopes.None || (scopes & DesktopControlScopes.Control) != 0;
                res = control ? Call(() => Native.dxr_prov_desktop_request(r)) : E_NoCapture;
            }
            LastResult = res;
            return res >= 0;
        }

        // The app's window as the consent dialog's parent: "wayland:<handle>" / "x11:<hex>" on a Linux
        // player, read right before each request (a window shown anew gets a new handle), passed
        // verbatim (a Wayland handle may contain spaces). Null elsewhere / when not known.
        private static byte[] ParentWindow()
        {
#if UNITY_STANDALONE_LINUX && !UNITY_EDITOR
            try
            {
                string h = DisplayXRNative.LinuxWindowHandle();
                if (!string.IsNullOrEmpty(h)) return Encoding.ASCII.GetBytes(h + "\0");
            }
            catch (EntryPointNotFoundException) { }
            catch (DllNotFoundException) { }
#endif
            return null;
        }

        // ---------------------------------------------------------------- observation

        /// <summary>Top-level visible windows, top-most first. Empty when not granted (see
        /// <see cref="LastResult"/>), never null.</summary>
        public static unsafe DesktopWindow[] ListWindows()
        {
            if (s_Missing) { LastResult = E_Unavailable; return Array.Empty<DesktopWindow>(); }
            try
            {
                for (int attempt = 0; attempt < 4; attempt++)
                {
                    int r = Native.dxr_prov_desktop_enumerate_windows(0, out uint count, null);
                    LastResult = r;
                    if (r < 0 || count == 0) return Array.Empty<DesktopWindow>();
                    // Slack absorbs a window opening between the two calls.
                    var raw = new NativeWindow[count + 4];
                    fixed (NativeWindow* p = raw)
                        r = Native.dxr_prov_desktop_enumerate_windows((uint)raw.Length, out count, p);
                    LastResult = r;
                    if (r == XR_ERROR_SIZE_INSUFFICIENT) continue;
                    if (r < 0) return Array.Empty<DesktopWindow>();
                    int n = (int)Math.Min(count, (uint)raw.Length);
                    var result = new DesktopWindow[n];
                    for (int i = 0; i < n; i++) result[i] = ToWindow(raw[i]);
                    return result;
                }
            }
            catch (EntryPointNotFoundException) { s_Missing = true; }
            catch (DllNotFoundException) { s_Missing = true; }
            return Array.Empty<DesktopWindow>();
        }

        /// <summary>The pointer position and what is under it.</summary>
        public static bool TryGetPointer(out DesktopPick pick)
        {
            NativePick raw = default;
            int r = Call(() => Native.dxr_prov_desktop_get_pointer(out raw));
            LastResult = r;
            pick = r >= 0 ? ToPick(raw) : default;
            return r >= 0;
        }

        /// <summary>What a click at (<paramref name="x"/>, <paramref name="y"/>) — desktop
        /// coordinates — would meet: the OS's own hit test, so stacking, click-through regions
        /// and system UI on top count as they would for a real click.</summary>
        public static bool TryPick(int x, int y, out DesktopPick pick)
        {
            NativePick raw = default;
            int r = Call(() => Native.dxr_prov_desktop_pick(x, y, out raw));
            LastResult = r;
            pick = r >= 0 ? ToPick(raw) : default;
            return r >= 0;
        }

        /// <summary>
        /// The accessible text of the UI element at (<paramref name="x"/>, <paramref name="y"/>).
        /// "" when the element exposes none (apps with accessibility off — some browsers,
        /// Electron — expose nothing; that is not an error); null on failure (not granted,
        /// unsupported: see <see cref="LastResult"/>).
        /// </summary>
        public static string GetTextAt(int x, int y)
        {
            if (s_Missing) { LastResult = E_Unavailable; return null; }
            try
            {
                for (int attempt = 0; attempt < 4; attempt++)
                {
                    int r = Native.dxr_prov_desktop_text_at(x, y, 0, out uint count, null);
                    LastResult = r;
                    if (r < 0) return null;
                    if (count <= 1) return "";
                    var buf = new byte[count + 64];
                    r = Native.dxr_prov_desktop_text_at(x, y, (uint)buf.Length, out count, buf);
                    LastResult = r;
                    if (r == XR_ERROR_SIZE_INSUFFICIENT) continue; // the text grew in between
                    if (r < 0) return null;
                    return FromUtf8(buf);
                }
            }
            catch (EntryPointNotFoundException) { s_Missing = true; }
            catch (DllNotFoundException) { s_Missing = true; }
            return null;
        }

        // ---------------------------------------------------------------- window actions

        /// <summary>Raise and focus the window (restores it if minimized, where the OS allows).
        /// Waits for the user to be idle. Do this before typing into it.</summary>
        public static Task<DesktopActionResult> Activate(ulong windowId) =>
            Submit(new NativeAction { actionType = 0, targetWindowId = windowId });

        /// <summary>Move/resize to <paramref name="rect"/> (desktop coordinates, top-left origin);
        /// a width or height of 0 keeps the current size.</summary>
        public static Task<DesktopActionResult> MoveResize(ulong windowId, RectInt rect) =>
            Submit(new NativeAction
            {
                actionType = 1, targetWindowId = windowId,
                rectX = rect.x, rectY = rect.y, rectWidth = rect.width, rectHeight = rect.height,
            });

        public static Task<DesktopActionResult> Minimize(ulong windowId) =>
            Submit(new NativeAction { actionType = 2, targetWindowId = windowId });

        /// <summary>Un-minimize / un-maximize.</summary>
        public static Task<DesktopActionResult> Restore(ulong windowId) =>
            Submit(new NativeAction { actionType = 3, targetWindowId = windowId });

        public static Task<DesktopActionResult> Maximize(ulong windowId) =>
            Submit(new NativeAction { actionType = 4, targetWindowId = windowId });

        /// <summary>A polite close request — the app may ask to save.</summary>
        public static Task<DesktopActionResult> Close(ulong windowId) =>
            Submit(new NativeAction { actionType = 5, targetWindowId = windowId });

        // ---------------------------------------------------------------- input actions

        /// <summary>Type <paramref name="text"/> into the window, committed as text (IME-safe, any
        /// language). The window must have keyboard focus when it is delivered —
        /// <see cref="Activate"/> it first, or the action is Refused.</summary>
        public static Task<DesktopActionResult> TypeText(ulong windowId, string text) =>
            Submit(new NativeAction { actionType = 10, targetWindowId = windowId }, text: text);

        /// <summary>Press a key chord: <c>"ctrl+s"</c>, <c>"alt+tab"</c>, <c>"enter"</c>,
        /// <c>"shift+f10"</c> (+-separated, case-insensitive; modifiers ctrl, shift, alt/option,
        /// super/win/cmd/meta). Needs keyboard focus, like <see cref="TypeText"/>.</summary>
        public static Task<DesktopActionResult> PressKeys(ulong windowId, string chord) =>
            Submit(new NativeAction { actionType = 11, targetWindowId = windowId }, keys: chord);

        /// <summary>Move the pointer to <paramref name="to"/> (desktop coordinates), which must be
        /// over the window.</summary>
        public static Task<DesktopActionResult> MovePointer(ulong windowId, Vector2Int to) =>
            Submit(new NativeAction { actionType = 12, targetWindowId = windowId, x = to.x, y = to.y });

        /// <summary>Click <paramref name="count"/> times at <paramref name="at"/>; refused unless
        /// the window (or its dialog) is what is under the point at delivery time.</summary>
        public static Task<DesktopActionResult> Click(ulong windowId, Vector2Int at,
                                                      DesktopPointerButton button = DesktopPointerButton.Left,
                                                      int count = 1) =>
            Submit(new NativeAction
            {
                actionType = 13, targetWindowId = windowId, x = at.x, y = at.y,
                button = (int)button, clickCount = (uint)Math.Max(1, count),
            });

        /// <summary>Hold <paramref name="button"/> from <paramref name="from"/> to <paramref name="to"/>;
        /// the start point is verified against the window.</summary>
        public static Task<DesktopActionResult> Drag(ulong windowId, Vector2Int from, Vector2Int to,
                                                     DesktopPointerButton button = DesktopPointerButton.Left) =>
            Submit(new NativeAction
            {
                actionType = 14, targetWindowId = windowId, x = from.x, y = from.y, x2 = to.x, y2 = to.y,
                button = (int)button,
            });

        /// <summary>Scroll by notches at <paramref name="at"/>: <paramref name="dx"/> +right,
        /// <paramref name="dy"/> +down.</summary>
        public static Task<DesktopActionResult> Scroll(ulong windowId, Vector2Int at, int dx, int dy) =>
            Submit(new NativeAction
            {
                actionType = 15, targetWindowId = windowId, x = at.x, y = at.y, scrollX = dx, scrollY = dy,
            });

        // ---------------------------------------------------------------- screen capture (spec v2)

        /// <summary>
        /// Capture <paramref name="desktopRect"/> (desktop coordinates, top-left origin, y down; clipped
        /// to the monitors) as one still. Needs <see cref="CaptureState"/> Granted (CONTROL is not
        /// required) and never prompts. The image is scaled down to fit
        /// <paramref name="maxWidth"/> x <paramref name="maxHeight"/> (1..<see cref="MaxCaptureSize"/>)
        /// keeping the aspect ratio, never up. The task completes on the main thread and never throws.
        /// </summary>
        public static Task<DesktopCaptureResult> CaptureAsync(RectInt desktopRect, int maxWidth = MaxCaptureSize,
                                                              int maxHeight = MaxCaptureSize) =>
            StartCapture(new NativeCaptureInfo
            {
                target = 1, x = desktopRect.x, y = desktopRect.y, width = desktopRect.width,
                height = desktopRect.height, maxWidth = (uint)Math.Max(0, maxWidth),
                maxHeight = (uint)Math.Max(0, maxHeight),
            });

        /// <summary>Capture the bounding box of every monitor (gaps come back transparent). See
        /// <see cref="CaptureAsync(RectInt, int, int)"/>.</summary>
        public static Task<DesktopCaptureResult> CaptureDesktopAsync(int maxWidth = MaxCaptureSize,
                                                                     int maxHeight = MaxCaptureSize) =>
            StartCapture(new NativeCaptureInfo
            {
                target = 0, maxWidth = (uint)Math.Max(0, maxWidth), maxHeight = (uint)Math.Max(0, maxHeight),
            });

        /// <summary>Capture <paramref name="windowId"/>'s frame as it is on screen (windows overlapping it
        /// are in the image; a minimized window is Refused). See
        /// <see cref="CaptureAsync(RectInt, int, int)"/>.</summary>
        public static Task<DesktopCaptureResult> CaptureWindowAsync(ulong windowId, int maxWidth = MaxCaptureSize,
                                                                    int maxHeight = MaxCaptureSize) =>
            StartCapture(new NativeCaptureInfo
            {
                target = 2, windowId = windowId, maxWidth = (uint)Math.Max(0, maxWidth),
                maxHeight = (uint)Math.Max(0, maxHeight),
            });

        /// <summary>
        /// <see cref="CaptureAsync(RectInt, int, int)"/> straight to a texture (see
        /// <see cref="DesktopCaptureResult.ToTexture2D"/>): null on failure, with the reason logged as a
        /// warning. Use <see cref="CaptureAsync(RectInt, int, int)"/> when you need the status.
        /// </summary>
        public static async Task<Texture2D> Capture(RectInt desktopRect, int maxWidth = MaxCaptureSize,
                                                    int maxHeight = MaxCaptureSize)
        {
            DesktopCaptureResult r = await CaptureAsync(desktopRect, maxWidth, maxHeight);
            if (!r.Succeeded) Debug.LogWarning($"[DisplayXR] desktop capture: {r}");
            return r.ToTexture2D();
        }

        private static Task<DesktopCaptureResult> StartCapture(NativeCaptureInfo info)
        {
            if (s_Missing || s_CaptureMissing) return CaptureDone(s_Missing ? E_Unavailable : E_NoCapture, null);
            try
            {
                int r = Native.dxr_prov_desktop_capture(ref info, out ulong id);
                if (r < 0 || id == 0) return CaptureDone(r < 0 ? r : XR_ERROR_VALIDATION_FAILURE, null);
                var tcs = new TaskCompletionSource<DesktopCaptureResult>();
                uint gen = Native.dxr_prov_desktop_session_generation();
                lock (s_Lock) s_PendingCaptures[id] = new PendingCapture { Tcs = tcs, Generation = gen };
                return tcs.Task;
            }
            catch (EntryPointNotFoundException) { s_CaptureMissing = true; }
            catch (DllNotFoundException) { s_Missing = true; }
            return CaptureDone(E_NoCapture, null);
        }

        // A completed capture: fetch the image now (the runtime keeps it 10 s; we are on the main
        // thread, draining the event the frame it arrived).
        private static unsafe DesktopCaptureResult FinishCapture(in NativeEvent ev, string message)
        {
            var src = new RectInt(ev.srcX, ev.srcY, ev.srcWidth, ev.srcHeight);
            if (ev.result < 0) return MakeCapture(ev.result, message, src, ev.frameTime);
            long bytes = (long)ev.width * ev.height * 4;
            if (ev.width == 0 || ev.height == 0 || bytes > int.MaxValue)
                return MakeCapture(XR_ERROR_VALIDATION_FAILURE, "the runtime returned no image", src, ev.frameTime);
            var px = new byte[bytes];
            int r;
            uint count;
            try { r = Native.dxr_prov_desktop_capture_image(ev.actionId, (uint)px.Length, out count, px); }
            catch (EntryPointNotFoundException) { s_CaptureMissing = true; r = E_NoCapture; count = 0; }
            if (r < 0) return MakeCapture(r, "could not fetch the captured image", src, ev.frameTime);
            if (count != (uint)px.Length)
                return MakeCapture(XR_ERROR_VALIDATION_FAILURE, "captured image size mismatch", src, ev.frameTime);
            return new DesktopCaptureResult(DesktopActionStatus.Success, message, r, (int)ev.width, (int)ev.height, src,
                                            ev.frameTime, px);
        }

        private static Task<DesktopCaptureResult> CaptureDone(int r, string message) =>
            Task.FromResult(MakeCapture(r, message, default, 0));

        private static DesktopCaptureResult MakeCapture(int r, string message, RectInt src, long frameTime)
        {
            string fallback = null;
            switch (r)
            {
                case XR_ERROR_DESKTOP_CONTROL_NOT_GRANTED_DXR: fallback = "screen capture is not granted for this app"; break;
                case XR_ERROR_LIMIT_REACHED: fallback = "a capture is already in flight"; break;
                case XR_ERROR_VALIDATION_FAILURE: fallback = "invalid capture (empty region, window id 0, size 0 or > 8192?)"; break;
                case E_NoCapture: fallback = "the runtime has no screen capture (XR_DXR_desktop_control spec v2)"; break;
            }
            DesktopActionResult a = Make(r, string.IsNullOrEmpty(message) ? fallback : message);
            DesktopActionStatus status = r == E_NoCapture ? DesktopActionStatus.Unsupported : a.Status;
            if (status == DesktopActionStatus.Success) status = DesktopActionStatus.Failed; // no image = not a success
            return new DesktopCaptureResult(status, a.Message, r, 0, 0, src, frameTime, null);
        }

        // ---------------------------------------------------------------- dispatch

        private static Task<DesktopActionResult> Submit(NativeAction a, string text = null, string keys = null)
        {
            if (s_Missing) return Done(E_Unavailable, null);
            byte[] t = text == null ? null : Utf8(text);
            byte[] k = keys == null ? null : Utf8(keys);
            try
            {
                int r = Native.dxr_prov_desktop_submit(ref a, t, k, out ulong id);
                if (r < 0 || id == 0) return Done(r < 0 ? r : XR_ERROR_VALIDATION_FAILURE, null);
                var tcs = new TaskCompletionSource<DesktopActionResult>();
                uint gen = Native.dxr_prov_desktop_session_generation();
                lock (s_Lock) s_Pending[id] = new Pending { Tcs = tcs, Generation = gen };
                return tcs.Task;
            }
            catch (EntryPointNotFoundException) { s_Missing = true; }
            catch (DllNotFoundException) { s_Missing = true; }
            return Done(E_Unavailable, null);
        }

        /// <summary>
        /// Drain queued completions and consent changes. Called by the provider driver every
        /// LateUpdate on the main thread; apps do not call this.
        /// </summary>
        internal static unsafe void Pump()
        {
            if (s_Missing) return;
            try
            {
                uint gen = Native.dxr_prov_desktop_session_generation();
                if (gen != s_Generation)
                {
                    s_Generation = gen;
                    FailPending(p => p.Generation != gen);
                    // A new / ended session: re-read, announce if it moved.
                    QueryState(out DesktopControlState now, out _);
                    SetState(now);
                    QueryCaptureState(out DesktopControlState cnow);
                    SetCaptureState(cnow);
                }

                // Bounded per frame: the native queue holds at most 128.
                for (int i = 0; i < 128; i++)
                {
                    NativeEvent ev;
                    if (Native.dxr_prov_desktop_next_event(out ev) != 1) break;
                    if (ev.kind == EventStateChanged)
                    {
                        SetState((DesktopControlState)ev.state);
                    }
                    else if (ev.kind == EventActionCompleted)
                    {
                        Pending p;
                        bool found;
                        lock (s_Lock)
                        {
                            found = s_Pending.TryGetValue(ev.actionId, out p);
                            if (found) s_Pending.Remove(ev.actionId);
                        }
                        if (found) p.Tcs.TrySetResult(Make(ev.result, Utf8Z(ev.message, 256)));
                    }
                    else if (ev.kind == EventCaptureStateChanged)
                    {
                        SetCaptureState((DesktopControlState)ev.state);
                    }
                    else if (ev.kind == EventCaptureCompleted)
                    {
                        PendingCapture p;
                        bool found;
                        lock (s_Lock)
                        {
                            found = s_PendingCaptures.TryGetValue(ev.actionId, out p);
                            if (found) s_PendingCaptures.Remove(ev.actionId);
                        }
                        // Fetch even when nobody waits: the copy frees the runtime's frame at once.
                        DesktopCaptureResult cr = FinishCapture(ev, Utf8Z(ev.message, 256));
                        if (found) p.Tcs.TrySetResult(cr);
                    }
                }
            }
            catch (EntryPointNotFoundException) { s_Missing = true; }
            catch (DllNotFoundException) { s_Missing = true; }
        }

        /// <summary>The provider stopped (driver destroyed): nothing will complete any more.</summary>
        internal static void OnProviderStopped()
        {
            FailPending(_ => true);
            SetState(DesktopControlState.Unsupported);
            SetCaptureState(DesktopControlState.Unsupported);
        }

        private static void FailPending(Func<Pending, bool> which)
        {
            List<Pending> failed = null;
            lock (s_Lock)
            {
                List<ulong> ids = null;
                foreach (var kv in s_Pending)
                {
                    if (!which(kv.Value)) continue;
                    (ids ??= new List<ulong>()).Add(kv.Key);
                    (failed ??= new List<Pending>()).Add(kv.Value);
                }
                if (ids != null) foreach (ulong id in ids) s_Pending.Remove(id);
            }
            List<PendingCapture> failedCaptures = null;
            lock (s_Lock)
            {
                List<ulong> ids = null;
                foreach (var kv in s_PendingCaptures)
                {
                    var asPending = new Pending { Generation = kv.Value.Generation };
                    if (!which(asPending)) continue;
                    (ids ??= new List<ulong>()).Add(kv.Key);
                    (failedCaptures ??= new List<PendingCapture>()).Add(kv.Value);
                }
                if (ids != null) foreach (ulong id in ids) s_PendingCaptures.Remove(id);
            }
            if (failedCaptures != null)
            {
                var cr = new DesktopCaptureResult(DesktopActionStatus.Failed, "session ended", E_NoSession, 0, 0,
                                                  default, 0, null);
                foreach (PendingCapture p in failedCaptures) p.Tcs.TrySetResult(cr);
            }
            if (failed == null) return;
            var r = new DesktopActionResult(DesktopActionStatus.Failed, "session ended", E_NoSession);
            foreach (Pending p in failed) p.Tcs.TrySetResult(r);
        }

        private static void SetState(DesktopControlState s)
        {
            if (s == s_LastState) return;
            s_LastState = s;
            try { StateChanged?.Invoke(s); }
            catch (Exception e) { Debug.LogException(e); }
        }

        private static void SetCaptureState(DesktopControlState s)
        {
            if (s == s_LastCaptureState) return;
            s_LastCaptureState = s;
            try { CaptureStateChanged?.Invoke(s); }
            catch (Exception e) { Debug.LogException(e); }
        }

        private static void QueryCaptureState(out DesktopControlState state)
        {
            int st = 0;
            int r = CallCapture(() => Native.dxr_prov_desktop_get_capture_state(out st));
            state = r >= 0 ? (DesktopControlState)st : DesktopControlState.Unsupported;
        }

        private static void QueryState(out DesktopControlState state, out DesktopControlCapabilities caps)
        {
            int st = 0;
            ulong c = 0;
            int r = Call(() => Native.dxr_prov_desktop_get_state(out st, out c));
            LastResult = r;
            state = r >= 0 ? (DesktopControlState)st : DesktopControlState.Unsupported;
            caps = r >= 0 ? (DesktopControlCapabilities)c : DesktopControlCapabilities.None;
        }

        private static Task<DesktopActionResult> Done(int r, string message) => Task.FromResult(Make(r, message));

        private static DesktopActionResult Make(int r, string message)
        {
            DesktopActionStatus status;
            string fallback;
            switch (r)
            {
                case XR_ERROR_DESKTOP_CONTROL_REFUSED_DXR:
                    status = DesktopActionStatus.Refused; fallback = "refused by the safety layer"; break;
                case XR_ERROR_DESKTOP_CONTROL_NOT_GRANTED_DXR:
                    status = DesktopActionStatus.NotGranted; fallback = "desktop control is not granted for this app"; break;
                case XR_ERROR_DESKTOP_WINDOW_LOST_DXR:
                    status = DesktopActionStatus.WindowLost; fallback = "the target window no longer exists"; break;
                case XR_ERROR_FEATURE_UNSUPPORTED:
                case XR_ERROR_FUNCTION_UNSUPPORTED:
                    status = DesktopActionStatus.Unsupported; fallback = "not supported on this desktop"; break;
                case E_Unavailable:
                    status = DesktopActionStatus.Unsupported;
                    fallback = "the runtime (or this plug-in build) has no XR_DXR_desktop_control"; break;
                case E_NoSession:
                    status = DesktopActionStatus.Failed; fallback = "no XR session"; break;
                case XR_ERROR_VALIDATION_FAILURE:
                    status = DesktopActionStatus.Failed; fallback = "invalid action (missing window id, text or keys?)"; break;
                case XR_ERROR_LIMIT_REACHED:
                    status = DesktopActionStatus.Failed; fallback = "too many queued actions"; break;
                default:
                    status = r >= 0 ? DesktopActionStatus.Success : DesktopActionStatus.Failed;
                    fallback = r >= 0 ? "" : "failed"; break;
            }
            return new DesktopActionResult(status, string.IsNullOrEmpty(message) ? fallback : message, r);
        }

        // ---------------------------------------------------------------- interop

        private static int Call(Func<int> f)
        {
            if (s_Missing) return E_Unavailable;
            try { return f(); }
            catch (EntryPointNotFoundException) { s_Missing = true; }
            catch (DllNotFoundException) { s_Missing = true; }
            return E_Unavailable;
        }

        // The spec-v2 exports: a native binary without them disables capture only, never the v1 surface.
        private static int CallCapture(Func<int> f)
        {
            if (s_Missing) return E_Unavailable;
            if (s_CaptureMissing) return E_NoCapture;
            try { return f(); }
            catch (EntryPointNotFoundException) { s_CaptureMissing = true; }
            catch (DllNotFoundException) { s_Missing = true; return E_Unavailable; }
            return E_NoCapture;
        }

        // NUL-terminated UTF-8; never a marshalled .NET string (Windows DllImport defaults
        // to the ANSI code page, which would mangle non-ASCII text).
        private static byte[] Utf8(string s)
        {
            int n = Encoding.UTF8.GetByteCount(s);
            var b = new byte[n + 1];
            Encoding.UTF8.GetBytes(s, 0, s.Length, b, 0);
            return b;
        }

        private static string FromUtf8(byte[] b)
        {
            int n = Array.IndexOf(b, (byte)0);
            return Encoding.UTF8.GetString(b, 0, n < 0 ? b.Length : n);
        }

        private static unsafe string Utf8Z(byte* p, int cap)
        {
            int n = 0;
            while (n < cap && p[n] != 0) n++;
            return n == 0 ? "" : Encoding.UTF8.GetString(p, n);
        }

        private static unsafe DesktopWindow ToWindow(NativeWindow w) =>
            new DesktopWindow(w.windowId, w.parentWindowId, w.pid, Utf8Z(w.appId, 128), Utf8Z(w.title, 256),
                              new RectInt(w.x, w.y, w.width, w.height), (DesktopWindowState)w.state, w.zOrder);

        private static DesktopPick ToPick(NativePick p) =>
            new DesktopPick(new Vector2Int(p.x, p.y), (DesktopPickKind)p.kind,
                            p.kind == (int)DesktopPickKind.Window ? ToWindow(p.window) : default);

        // Byte-for-byte mirrors of the Dxr* structs in displayxr_provider_desktop.h (pinned
        // there by static_asserts): 432 / 448 / 64 / 312 / 40 bytes.
        [StructLayout(LayoutKind.Sequential)]
        private unsafe struct NativeWindow
        {
            public ulong windowId, parentWindowId, state;
            public uint pid, zOrder;
            public int x, y, width, height;
            public fixed byte appId[128];
            public fixed byte title[256];
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct NativePick
        {
            public int x, y, kind, reserved;
            public NativeWindow window;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct NativeAction
        {
            public ulong targetWindowId;
            public int actionType;
            public int rectX, rectY, rectWidth, rectHeight;
            public int x, y, x2, y2;
            public int button;
            public uint clickCount;
            public int scrollX, scrollY;
            public int reserved;
        }

        [StructLayout(LayoutKind.Sequential)]
        private unsafe struct NativeEvent
        {
            public ulong actionId;
            public int kind, result, state, reserved;
            public fixed byte message[256];
            // Spec v2, CAPTURE_COMPLETED (actionId is then the capture id).
            public uint width, height;
            public int srcX, srcY, srcWidth, srcHeight;
            public long frameTime;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct NativeCaptureInfo
        {
            public ulong windowId;
            public int target; // XrDesktopCaptureTargetDXR: 0 desktop, 1 region, 2 window
            public int x, y, width, height;
            public uint maxWidth, maxHeight;
            public int reserved;
        }

        private static unsafe class Native
        {
            private const string LibName = "displayxr_unity";

            [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
            public static extern int dxr_prov_desktop_available();

            [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
            public static extern uint dxr_prov_desktop_session_generation();

            [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
            public static extern int dxr_prov_desktop_get_state(out int state, out ulong capabilities);

            [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
            public static extern int dxr_prov_desktop_request(byte[] reasonUtf8);

            [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
            public static extern int dxr_prov_desktop_enumerate_windows(uint capacity, out uint count, NativeWindow* windows);

            [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
            public static extern int dxr_prov_desktop_get_pointer(out NativePick pick);

            [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
            public static extern int dxr_prov_desktop_pick(int x, int y, out NativePick pick);

            [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
            public static extern int dxr_prov_desktop_text_at(int x, int y, uint capacity, out uint count, byte[] bufferUtf8);

            [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
            public static extern int dxr_prov_desktop_submit(ref NativeAction action, byte[] textUtf8, byte[] keysUtf8,
                                                             out ulong actionId);

            [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
            public static extern int dxr_prov_desktop_next_event(out NativeEvent ev);

            // Spec v2 (screen capture).
            [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
            public static extern int dxr_prov_desktop_request_scopes(byte[] reasonUtf8, ulong scopes,
                                                                     byte[] parentWindowUtf8);

            [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
            public static extern int dxr_prov_desktop_capture_available();

            [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
            public static extern int dxr_prov_desktop_get_capture_state(out int state);

            [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
            public static extern int dxr_prov_desktop_capture(ref NativeCaptureInfo info, out ulong captureId);

            [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
            public static extern int dxr_prov_desktop_capture_image(ulong captureId, uint capacity, out uint count,
                                                                    byte[] rgba);
        }
    }
}
