# Window Mode

An **F11 toggle between full screen on the 3D panel and a window**, an optional **fade to
black** across the change, and a **windowed size and position that are remembered**: put back
on leaving full screen and at the next launch.

One component, `DisplayXRWindowModeSample`, plus `DisplayXRWindowModeRules` (the decisions,
pure C#, unit-tested).

## Why it is a sample

Which key toggles, whether to fade, what to remember and where the window goes are app
policy. The plug-in exposes the primitives and nothing more:

| Primitive | Used for |
|---|---|
| `DisplayXRProvider.MoveWindowToDisplay()` | Putting the window on the 3D panel before it goes full screen |
| `DisplayXRWindowSpaceUI.TryGetWindowPixelSize()` | Knowing when the window the provider renders for has stopped changing size |
| `DisplayXRModeSwitch` | The eased ramp of the fade |
| `DisplayXRDisplay.ipdFactor` / `DisplayXRCamera.ipdFactor` | Flattening the stereo during the fade |

Everything else is Unity's own `Screen` API. Fork it freely.

It replaces the bare `Screen.fullScreen = !Screen.fullScreen` that the Default Input
Controller sample used to run on F11. That line had two problems. `Screen.fullScreen = false`
keeps the full-screen resolution, so the "window" it returns to is as large as the monitor
and looks as if nothing happened. And it goes full screen on whichever monitor the window is
on, which is the 3D panel only by luck.

## Usage

1. Import this sample (Package Manager ▸ DisplayXR ▸ Samples ▸ Window Mode).
2. Add `DisplayXRWindowModeSample` to an **empty GameObject of its own** in your first scene.
   With **Persist Across Scenes** the whole GameObject is kept across scene loads.
3. Build a player and press **F11**.

From script: `Toggle()`, `SetFullscreen(bool)`, `IsSwitching`, the static `IsFullscreen` and
`ForgetSavedWindow()`.

In **Player Settings ▸ Resolution and Presentation**:

- **Fullscreen Mode** must not be *Exclusive Fullscreen* (the `fullscreen-mode` rule in
  **DisplayXR ▸ Project Validation** checks this).
- Untick **Allow Fullscreen Switch** if you do not want Unity's own Alt+Enter toggle. It
  bypasses this component: no fade, no move to the panel, and nothing is remembered.

## Fields

| Field | Default | |
|---|---|---|
| `toggleKey` | F11 | Read through the Input System when it is enabled, else the legacy Input Manager. `None` = no key. With the Input System the `KeyCode` is matched to the `Key` of the same name (F11, Space, A ...); one without a namesake logs a warning: call `Toggle()` from your own binding then. |
| `fullscreenOnPanel` | on | Move the window onto the DisplayXR panel before going full screen. Needs a running session and a runtime that reports the panel's place (`XR_DXR_display_info` v16+); otherwise full screen on the current monitor, and the log says so. |
| `rememberWindow` | on | Remember the windowed size and position in `PlayerPrefs` (key `DisplayXR.Samples.WindowMode`, one string). |
| `restoreOnLaunch` | on | At launch, put the window back the way the last run left it. Do not combine with a `DisplayXRTargetDisplay` that moves the window at start: both would place it. |
| `defaultWindowedSize` | 1280 x 720 | Used when nothing is remembered yet. |
| `fade`, `fadeSeconds`, `fadeEasing` | on, 0.25 s, SmoothStep | See below. |
| `settleFrames`, `settleTimeout` | 10 frames, 3 s | How long the window must keep one size before the fade comes back, and the longest wait. |
| `persistAcrossScenes` | on | `DontDestroyOnLoad`. A second instance in a later scene removes itself. |
| `logResult` | on | One `[DisplayXR] WindowMode:` line per switch. |

## What a switch does

**To full screen:** fade out → remember the windowed size and position → move the window
onto the panel (still windowed) → `Screen.SetResolution(panel width, panel height,
FullScreenWindow)` → wait for the window to settle → fade in.

**To windowed:** fade out → `Screen.SetResolution(remembered width, height, Windowed)` → wait
→ `Screen.MoveMainWindowTo(remembered display, remembered position)` → put the size back if
the move changed it → fade in.

**The fade** eases the active rig's `ipdFactor` to 0 while a black quad fades in, and back
afterwards. The quad is a world object on the rig's zero-parallax plane, drawn over
everything (`UI/Default` with its depth test set to Always). It has to be in the scene:
`OnGUI` and screen-space canvases draw into Unity's own window, which the provider's window
covers. The fade stays black until the window has kept one size for `settleFrames` frames,
because the provider reallocates its swapchain only once the size has held for 6 frames.
If a `DisplayXRAutoConvergence` drives the rig's stereo strength, the fade leaves
`ipdFactor` to it and only fades the quad.

## Pixels: one space, never two

The plug-in works in two pixel spaces on Windows. Its native side is per-monitor DPI aware
and answers in true physical pixels. Managed code gets whatever Unity's process awareness
gives. They are identical on a single monitor and differ by the ratio of the scale factors
on a mixed-DPI multi-monitor machine, so a mistake is invisible on a development box and
wrong on exactly the machines that have a separate 3D panel.

This sample therefore keeps to one rule: **every remembered number is read from Unity's
`Screen` API and handed back to Unity's `Screen` API, and to nothing else.**

- Size: `Screen.width` / `Screen.height` → `Screen.SetResolution`.
- Position: `Screen.mainWindowPosition`, relative to `Screen.mainWindowDisplayInfo` →
  `Screen.MoveMainWindowTo` on the display with the same name and size.
- The move onto the panel is `MoveWindowToDisplay()`: it takes no coordinates from managed
  code and does the geometry natively, in a per-monitor-aware scope.
- The size from `TryGetWindowPixelSize` is only compared with itself, frame to frame.

Two more rules from the same place:

- **(0, 0) is a position**, not "nothing saved": it is where a window in the corner of a
  display sits. Whether a position is saved is a separate flag.
- A value that fails a check is not saved and not applied: a minimized window reports a
  position far outside every display, and a remembered display may be unplugged or running
  at another resolution. The window is then left where the OS put it.

## Where it does nothing

Each case logs one line and leaves the window alone.

| Case | Why |
|---|---|
| The editor | The Game view is not a window the player's fullscreen mode applies to. |
| Transparent overlay apps (`DisplayXRTransparentOverlay`) | What is on screen is the plug-in's overlay window; Unity's own window is hidden and parked off screen, so `Screen.fullScreenMode` would change a window nobody sees. Size that app with `DisplayXRNative.displayxr_resize_overlay` / `displayxr_set_overlay_fullscreen`. |
| A workspace tile (the app runs under the DisplayXR shell) | The shell owns the window. |
| Android | No window modes. |
| Linux native Wayland | Size and mode work. The **position** is neither saved nor restored: a Wayland client cannot place its own window. |
| macOS | `MoveWindowToDisplay()` is not implemented there: full screen is on the current display. |

## What is verified, and what is not

- The decisions in `DisplayXRWindowModeRules` are unit-tested (`Tests~/WindowModeRules`, run by
  CI): the saved string and its round trip, which sizes and positions are kept, which display
  a position belongs to, the settle count.
- Both files were compile-checked outside Unity against Unity 6000.4's assemblies, with the
  Input System, the legacy Input Manager and both, and with the Windows, Linux and macOS
  player defines. They have not been imported into a Unity project yet, and not compiled
  against Unity 2022.3.
- **Not run on hardware yet** (#477): the switch itself, the fade (in particular that
  `UI/Default` draws over everything in URP and HDRP single-pass), and the round trip of size
  and position.
- **Not testable on a single-monitor machine:** the move onto a panel that is not the
  primary display, and the round trip across two displays with different scale factors. On
  that kind of machine a move makes the OS resize the window; the sample puts the remembered
  size back afterwards, and that step has not been exercised.
- **Minimizing** a DisplayXR app on Windows is an open issue of its own (#270). This sample
  does not minimize or restore the window, and never saves a minimized window's position.
