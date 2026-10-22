# Side-step: the app's window steps aside (Linux, native Wayland)

A desktop companion (an avatar in a transparent window) should not sit in the way of the
document the user is typing in. With side-step on, the app's own window moves aside when the
window the user is working in covers the app's content, and goes back once its place is free.

Linux only: a player running natively on Wayland (`-force-wayland`, what the generated launcher
picks), on GNOME with the DisplayXR GNOME extension. Everywhere else it reports `Unsupported`
and does nothing.

## Using it

```csharp
DisplayXRSideStep.Enabled = true;                   // opt in (default off)
DisplayXRSideStep.SteppedAside += rect => { ... };  // its own window, after the move
DisplayXRSideStep.ReturnedHome += rect => { ... };
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
`Paused`, `Home`, `Moving`, `Aside`), `Reason` (why: `NoSession`, `NoExtension`, `Settling`,
`ScreenLocked`, `UserMoving`, `Minimized`, `TooManyMoves`, `NoFreeSpot`, ...), `IsAside`,
`Covered` (the share of its content covered now), `TryGetWindowRect`, `TryGetHomeRect`,
`TryGetHomePosition`, and the `SteppedAside`, `ReturnedHome` and `StateChanged` events.

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
- **Holds still** while the screen is locked (`org.gnome.ScreenSaver.GetActive`), while the user
  moves or resizes the window (the extension's move grab flag, or the plug-in's own right-drag),
  and while the window is minimized, maximized or fullscreen. A drag that starts during a glide
  stops the glide: the user wins.
- **Rate**: one move at a time; 1 s between a move landing and the next; at most 6 steps aside
  per minute (then `TooManyMoves`, and it stays). The way back home is never held by that cap,
  so the cap cannot strand the window away from home. A move the compositor does not carry out
  (it keeps the window elsewhere) is taken as is after 2 s, never retried.
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

## Privacy: what is read, what the app gets

The plug-in reads, inside its native code, 4 times a second:

| Read | From | Used for |
|---|---|---|
| Every listed window's frame, focus flag, minimized flag, window type, process id, mutter id | The DisplayXR GNOME extension's read-only `GetAllWindows` | Which window is focused, how much of the app's content it covers, where is free, whether the window it stepped aside for is still there |
| Each window's app id | The same reply | Only to recognise the desktop-icons window; not kept |
| The app's own window: frame, monitor, move grab, minimized / maximized / fullscreen | The same reply | Home, landing, holding still |
| Whether the screen is locked | `org.gnome.ScreenSaver.GetActive` | Holding still |

No titles are read: the parser never looks at them, and extension version 18 does not send them
to this connection at all. Nothing about another window is stored past one poll, except the
mutter id of the window it stepped aside for (to tell when that window is gone). Nothing about
another window is logged: the log lines speak of the app's own window and its covered share only.

The app gets only facts about **its own window**: its state, its frame, its home, the share of its
content covered, and the events. No other window's id, process id, app id, title, focus or rect
reaches managed code; the native exports (`displayxr_linux_side_step_set`,
`_set_body_rect`, `_status`) carry nothing else either. What the app can infer is coarse and about
itself: that something covers part of it (`Covered`), and that the place it moved to was free.

### Why it needs no desktop-control grant

This follows the consent policy decided for #389 (runtime ADR-056): what tells the app about
OTHER apps (the window list with process ids, the focused window, the window at a point, the live
pointer) is served only while the app's desktop control is `Granted`, and the monitors and the X
root scale stay ungated. Side-step does not tell the app about other apps: it uses other windows'
frames and focus inside the plug-in to move the app's own window, which the extension lets any
app do (`MoveWindow` acts on the caller's own window only). So, like the plug-in's own placement
of the window onto the 3D panel, it works without the grant, and it never makes `LinuxWindows`,
`LinuxWindowAt` or `LinuxPointer` answer.

It never moves, focuses, raises or closes another window, and it sends no input anywhere.

## Where it lives

| Piece | File |
|---|---|
| Decision, parsing, content grid, where-to (pure, unit-tested) | `native~/displayxr_linux_side_step.h`, `native~/tests/test_linux_side_step.c` |
| Worker thread, exports, glide | `native~/displayxr_linux_side_step.c` |
| Window list and lock state over the plug-in's own session-bus connection | `native~/displayxr_linux_desktop_info.c` (`dxr_linux_desktop_info_window_list_json`, `_screen_locked`; not exported) |
| The silhouette as content | `native~/displayxr_linux.c` (`displayxr_set_overlay_hit_mask`) |
| The move (stage px, the GNOME extension's `MoveWindow`) | `native~/displayxr_linux_wayland.c` (`dxr_wl_set_player_position_stage`, Wayland library ABI 9) |
| C# API, component | `Runtime/DisplayXRSideStep.cs`, `Runtime/DisplayXRWindowSideStep.cs`; pumped by `DisplayXRProviderDriver` |

Everything is in mutter's stage coordinates (GNOME desktop coordinates, the space of
`DisplayXRNative.LinuxMonitor.Rect`). The moves go through the Wayland library's own placement,
so a new place gets a fresh correction budget like any app move (#382), and the plug-in never
fights the compositor over one.

## Limits

- Native Wayland on GNOME only. An X11 (XWayland) player reports `Unsupported` /
  `NotNativeWayland`.
- The monitor rect is the whole monitor, not its work area: a corner place can end up under
  GNOME's top bar.
- Windows on other workspaces are not told apart (the extension does not report the workspace);
  only the focused window triggers a step aside, and that one is always on the current workspace.
