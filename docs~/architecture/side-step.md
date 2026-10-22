# Side-step: the app's window steps aside (Linux, native Wayland)

A desktop companion (an avatar in a transparent window) should not sit in the way of the
document the user is typing in. With side-step on, the app's own window moves aside when the
window the user is working in covers the app's content, and goes back once its place is free.

Linux only: a player running natively on Wayland (`-force-wayland`, what the generated launcher
picks), on GNOME with the DisplayXR GNOME extension. Everywhere else it reports `Unsupported`
and does nothing.

It acts only while the app's **desktop control is granted** (`DisplayXRDesktop.State ==
Granted`), the same consent gate `DisplayXRNative.LinuxWindows` answers behind (#389). See
[Consent: only while granted](#consent-only-while-granted).

## Using it

```csharp
DisplayXRSideStep.Enabled = true;                   // opt in (default off)
DisplayXRSideStep.SteppedAside += rect => { ... };  // its own window, after the move
DisplayXRSideStep.ReturnedHome += rect => { ... };

// It does nothing until the user grants desktop control (on a user gesture; side-step never asks):
if (DisplayXRDesktop.State != DesktopControlState.Granted)
    DisplayXRDesktop.Request("so the avatar can step out of the way of the window you work in");
```

Or add a **DisplayXR Window Side-Step** component (`DisplayXRWindowSideStep`) to any GameObject.
It turns side-step on while the component is enabled and exposes the settings.

| Setting | Default | Meaning |
|---|---|---|
| `Trigger` | 0.15 | Share of the app's content the window in front must cover |
| `Dwell` | 0.6 s | Covered this long before it steps aside |
| `ReturnHome` | true | Go back once home is free. False: the new place becomes home |
| `ReturnDelay` | 2 s | Home free this long before it goes back |
| `GlideSeconds` | 0.3 s | A move glides (ease-out); 0 jumps |
| `PreferredSide` | `Auto` | Among equally free places: `Left`, `Right`, `Above`, `Below`, or the shortest move |
| `Margin` | 24 px | Desktop px between the app's content and the other window or the monitor's edge |

What the app reads, all about its own window: `State` (`Off`, `Unsupported`, `Waiting`,
`Paused`, `Home`, `Moving`, `Aside`), `Reason` (why: `NotGranted`, `NoSession`, `NoExtension`,
`Settling`, `ScreenLocked`, `UserMoving`, `Minimized`, `TooManyMoves`, `NoFreeSpot`, ...),
`IsAside`, `Covered` (the share of its content covered now), `TryGetWindowRect`,
`TryGetHomeRect`, `TryGetHomePosition`, and the `SteppedAside`, `ReturnedHome` and
`StateChanged` events. While desktop control is not granted: `Paused` / `NotGranted`,
`Covered` 0, `IsAside` false, no rect, no home, no events. While the screen is locked:
`Paused` / `ScreenLocked`, `Covered` 0, no events, but home, `IsAside` and the last seen rect
are kept.

### The app's content

Only the app's content counts as covered, and the content (not the whole window) keeps the
margin. In order:

1. `SetContentRect(RectInt)`: a rect the app gives, in window device px (the space of
   `displayxr_get_overlay_size`), e.g. the avatar's 3D zone. `ClearContentRect()` undoes it.
2. The transparent overlay's click-through silhouette: the mask `DisplayXRTransparentOverlay`
   already pushes (`displayxr_set_overlay_hit_mask`), taken at most every 100 ms.
3. The whole window (an opaque app).

The content is kept as a 32 x 32 grid of fractions of the window, so it does not depend on the
window's scale. An empty band above an avatar is not content: a window over it does not count.

### Saving the window's place

While `IsAside`, the window is not where the user put it. An app that saves its position on quit
or periodically saves `TryGetHomePosition` instead (the same X root px as
`displayxr_get_overlay_position` / `displayxr_set_overlay_position`):

```csharp
Vector2Int pos;
if (!(DisplayXRSideStep.IsAside && DisplayXRSideStep.TryGetHomePosition(out pos)))
{
    DisplayXRNative.displayxr_get_overlay_position(out int x, out int y);
    pos = new Vector2Int(x, y);
}
```

## The rules

- **Covered**: the focused window of another app covers at least `Trigger` of the app's content
  for `Dwell`. Only the focused window triggers it: that is the window the user is working in.
- **Where to**: beside that window (left, right, above, below) or in a corner of the monitor,
  `Margin` from both; least covered first, then `PreferredSide`, then the shortest move. The
  whole window stays on its monitor (a window whose centre crosses onto another monitor changes
  scale, and the plug-in's own placement check would move it back). A place counts only if it is
  covered less than `Trigger` and less than half of the share covered now; otherwise it stays
  (`NoFreeSpot`).
- **Back home**: once home is free (covered less than half of `Trigger`) of the focused window
  AND of the window it stepped aside for, for `ReturnDelay`. Clicking the avatar moves the focus
  away from the document, but the document still covers home, so the avatar stays aside.
- **Home** is where the window stays put (1 s) after anyone but side-step moved it: the start-up
  placement onto the 3D panel, the user's drag, the app's own `displayxr_set_overlay_position`.
  Side-step's own moves never become home.
- **Only while granted**: nothing at all while desktop control is not granted, and a revoke
  stops it where it is and forgets home (below).
- **Holds still** while the screen is locked (a lock suspends the grant: nothing is read, home is
  kept, below; `org.gnome.ScreenSaver.GetActive` is asked too, after the gate, in case the
  runtime has not seen the lock yet), while the user moves or resizes the window (the
  extension's move grab flag, or the plug-in's own right-drag), and while the window is
  minimized, maximized or fullscreen. A drag that starts during a glide stops the glide: the
  user wins.
- **Rate**: one move at a time; 1 s between a move landing and the next; at most 6 steps aside
  per minute (then `TooManyMoves`, and it stays; a revoke and a re-grant do not reset the
  count). The way back home is never held by that cap, so the cap cannot strand the window away
  from home. A move the compositor does not carry out (it keeps the window elsewhere) is taken
  as is after 2 s, never retried.
- **Off** stops at once: no further moves, the window stays where it is.

### Which windows count

The extension's `GetAllWindows` (version 16+; `GetWindows` before) lists normal windows,
dialogs, modal dialogs and utility windows; menus, popups, tooltips, docks, panels and the
desktop are not in it. Of those, side-step ignores:

- the app's own windows (its process id),
- minimized windows,
- the desktop-icons window (`com.rastersoft.ding`, Ubuntu's default; a full-monitor NORMAL
  window before mutter 49 that takes the focus when the user clicks the desktop),
- windows without a frame.

A maximized or fullscreen document covers the whole monitor: there is no free place, so the
avatar stays.

## Consent: only while granted

Side-step decides from the other windows' frames and focus. That is exactly what
`DisplayXRNative.LinuxWindows` tells an app about other apps, and the consent policy decided for
#389 (runtime ADR-056) serves that only while the app's desktop control is `Granted`. So
side-step sits behind the same gate, even though nothing it reads is handed to the app: the
app's own window state (`Covered`, the rect it stepped to, `NoFreeSpot`) still lets an app infer
roughly where the window the user works in is, and polling the window list without consent is
not something a feature should do quietly.

**The gate.** Before every poll (4 times a second) the worker asks the runtime whether the app's
desktop control is `Granted`: `dxr_prov_desktop_get_state`, the very query
`DisplayXRDesktop.State` makes and `LinuxWindows` / `LinuxWindowAt` / `LinuxPointer` answer
behind (a synchronous runtime call of about 1.4 ms, evaluated afresh each time, so the user's
deny, the kill switch and a screen lock count at once). Only a yes lets that poll read the window
list and the lock state. A glide asks again before each of its steps. While the answer is no,
side-step asks once a second and reads nothing else. One gate, one answer: side-step and
`LinuxWindows` open and close together.

The runtime answers each of these queries with its own lock probe (`ScreenSaver.GetActive` on
its desktop-control connection), as it does for any `DisplayXRDesktop.State` read; that is the
gate's cost, not a read by side-step.

| Desktop control | Side-step |
|---|---|
| `Granted` | Runs as described above |
| `Suspended` after a grant (the screen is locked; the kill switch `displayxr-cli desktop stop` reads the same) | `Paused` / `ScreenLocked`. Reads nothing: no window list, no lock state. `Covered` 0, no events. Keeps the app's own home, whether it is aside and its last seen rect; drops the id of the window it stepped aside for. The window stays where it is |
| `NotRequested`, `Pending`, `Denied`, `DisabledByUser` (`displayxr-cli desktop deny`), `Unsupported` (no session, a runtime without `XR_DXR_desktop_control`); `Suspended` without an earlier grant | `Paused` / `NotGranted`. Reads nothing: no window list, no lock state. `Covered` 0, `IsAside` false, no rect, no home, no events. The window stays where it is |

**A revoke mid-session** (the user's deny, or the kill switch once it is turned off again): the
next poll, at most 250 ms after the revoke (a second while it was suspended), finds no grant and
stops; a glide stops at its next step (40 ms). The window stays where it is, wherever that is
(aside, or part way through a glide); no `SteppedAside` / `ReturnedHome` fires for the move that
was cut short. Everything the decision remembered is forgotten: home, where it was going, the
window it stepped aside for (only the times of its own recent steps aside are kept, for the
6-a-minute cap).

**A screen lock** suspends desktop control (the runtime reports `Suspended`). Side-step stops the
same way, within one poll or at the next glide step, and reads nothing while the screen is locked.
But home is the app's own window's data, so it survives the lock: side-step keeps home, whether the
window is aside (a glide cut short by the lock counts as aside, where its last step put the
window; with `ReturnHome` off, that place becomes home after the unlock, as a landed step would),
and the window's last seen rect. It drops only what it knew about the other window: the id of the
window it stepped aside for. After the unlock (`Granted` again) it carries on from the kept home,
with no start-up delay and no settling; aside, the normal way back applies, and since the id is
gone, home only has to be free of the window that has the focus.

The runtime reports the kill switch as `Suspended` too, and side-step cannot tell the two apart
without reading the lock state, which it must not do without the grant. So while the kill switch
is on, side-step holds still and keeps home exactly as for a lock: that keeps only the app's own
window's data and reads nothing. The kill switch ends the grant, so once it is turned off the
runtime reports `Denied`, and side-step forgets home then.

**When the grant comes back** it starts afresh, as after turning side-step off and on: after the
start-up delay, the place the window stays put at becomes home. So an avatar that was aside when
the grant ended stays at that place, and that place is its new home.

Side-step never asks for the grant: the app asks (`DisplayXRDesktop.Request` /
`RequestAccess(DesktopControlScopes.Control, ...)`) on a user gesture, and the GNOME
RemoteDesktop portal names the app. Turning side-step on does not make `LinuxWindows`,
`LinuxWindowAt` or `LinuxPointer` answer; they follow the grant on their own.

## Privacy: what is read, what the app gets

While the grant holds, the plug-in reads, inside its native code, 4 times a second:

| Read | From | Used for |
|---|---|---|
| Every listed window's frame, focus flag, minimized flag, window type, process id, mutter id | The DisplayXR GNOME extension's read-only `GetAllWindows` | Which window is focused, how much of the app's content it covers, where is free, whether the window it stepped aside for is still there |
| Each window's app id | The same reply | Only to recognise the desktop-icons window; not kept |
| The app's own window: frame, monitor, move grab, minimized / maximized / fullscreen | The same reply | Home, landing, holding still |
| Whether the screen is locked | `org.gnome.ScreenSaver.GetActive` | Holding still |

No titles are read: the parser never looks at them, and extension version 18 does not send them
to this connection at all. Nothing about another window is stored past one poll, except the
mutter id of the window it stepped aside for (to tell when that window is gone; forgotten when
the grant ends or the screen locks). Nothing about another window is logged: the log lines speak of the app's own
window and its covered share only. Without the grant nothing in this table is read.

The app gets only facts about **its own window**: its state, its frame, its home, the share of its
content covered, and the events. No other window's id, process id, app id, title, focus or rect
reaches managed code; the native exports (`displayxr_linux_side_step_set`,
`_set_body_rect`, `_status`) carry nothing else either. What the app can infer is coarse (that
something covers part of it, and that the place it moved to was free), and it can infer it only
while it may read the whole window list anyway (`LinuxWindows`, behind the same grant).

It never moves, focuses, raises or closes another window, and it sends no input anywhere.

## Where it lives

| Piece | File |
|---|---|
| The gate in front of every poll and glide step, decision, parsing, content grid, where-to, glide (pure, unit-tested; the reads are calls the test counts) | `native~/displayxr_linux_side_step.h` (`dxr_ss_poll`, `dxr_ss_glide`), `native~/tests/test_linux_side_step.c` |
| Worker thread, exports, the real reads and the gate query | `native~/displayxr_linux_side_step.c` (`op_granted`: `dxr_prov_desktop_get_state`) |
| The gate's runtime query (shared with `DisplayXRDesktop.State`) | `native~/displayxr_xrprovider/displayxr_provider_desktop.cpp` |
| Window list and lock state over the plug-in's own session-bus connection | `native~/displayxr_linux_desktop_info.c` (`dxr_linux_desktop_info_window_list_json`, `_screen_locked`; not exported) |
| The silhouette as content | `native~/displayxr_linux.c` (`displayxr_set_overlay_hit_mask`) |
| The move (stage px, the GNOME extension's `MoveWindow`) | `native~/displayxr_linux_wayland.c` (`dxr_wl_set_player_position_stage`, Wayland library ABI 10) |
| C# API, component | `Runtime/DisplayXRSideStep.cs`, `Runtime/DisplayXRWindowSideStep.cs`; pumped by `DisplayXRProviderDriver` |

Everything is in mutter's stage coordinates (GNOME desktop coordinates, the space of
`DisplayXRNative.LinuxMonitor.Rect`). The moves go through the Wayland library's own placement,
so a new place gets a fresh correction budget like any app move (#382), and the plug-in never
fights the compositor over one.

## Limits

- Needs the app's desktop control granted (the GNOME RemoteDesktop portal's consent): an app that
  wants side-step asks for desktop control, even if it uses nothing else of it.
- After a screen lock, only the focused window keeps the window aside (the id of the window it
  stepped aside for is dropped at the lock): a document that covers home but no longer has the
  focus does not hold it aside any more, as it would without the lock.
- Native Wayland on GNOME only. An X11 (XWayland) player reports `Unsupported` /
  `NotNativeWayland`.
- The monitor rect is the whole monitor, not its work area: a corner place can end up under
  GNOME's top bar.
- Windows on other workspaces are not told apart (the extension does not report the workspace);
  only the focused window triggers a step aside, and that one is always on the current workspace.
