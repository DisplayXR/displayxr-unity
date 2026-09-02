# 2D scenes and menus

**The problem in one sentence:** while a DisplayXR session is running, XR is active
process-wide and Unity renders *every* camera in stereo — including a plain camera with no
DisplayXR rig component on it at all — so a flat menu comes out weaved.

This trips up almost every app, because the intuition is exactly backwards. The rig
components (`DisplayXRCamera`, `DisplayXRDisplay`) only **tune** stereo; they do not
**gate** it. Deleting them does not give you a 2D scene. It gives you a stereo scene with
default tuning.

## The short answer

Put a **`DisplayXRSceneMode`** component anywhere in the scene and set its mode to **2D**.

```csharp
// or from script:
sceneMode.Apply(DisplayXRSceneDimensionality.TwoD);
```

It works with **no rig present**, which is the point — a 2D menu usually has none.

## What's actually happening underneath

Three levers exist. `DisplayXRSceneMode` drives all three in the right order; this section
is for when you need to drive them yourself.

| Lever | API | What it changes |
|---|---|---|
| **Rendering mode** | `DisplayXRProvider.RequestRenderingMode(modeIndex)` | How many views the runtime renders. The mode whose `viewCount <= 1` is the mono one. **This is the real 2D switch.** |
| **Hardware display state** | `DisplayXRProvider.RequestDisplayMode(bool mode3d)` | Whether the panel itself is in its 3D or 2D state |
| **Disparity** | `ipdFactor` on the rig | Flattens the stereo separation. Cosmetic on its own — it does not change the render path |

Enumerate the available modes with `DisplayXRProvider.Modes`; each entry carries
`modeIndex`, `viewCount`, `name` and `isRequestable`.

### The transition is asymmetric — this is the part people get wrong

You cannot just flip the mode. The two directions need opposite ordering:

- **3D → 2D:** ramp disparity to zero **first**, *then* issue the mode request, so the
  switch lands on content that is already flat.
- **2D → 3D:** issue the mode request **first** (the first 3D frame is flat anyway), *then*
  ease disparity up to its steady value.

Get it backwards and you get a visible snap at the switch. `DisplayXRModeSwitch`
(`Runtime/DisplayXRModeSwitch.cs`) is a dependency-free sequencer that encodes this — it is
a C# port of the same state machine the native runtime test apps use, it is wall-clock
driven so it is frame-rate independent, and it retargets cleanly mid-ramp (a not-yet-fired
3D→2D reverses without ever having switched). `DisplayXRSceneMode` is a thin scene-level
wrapper around it.

Driving the sequencer by hand:

```csharp
var seq = new DisplayXRModeSwitch();
seq.Configure(0.18f);
seq.Request(targetMode, targetViewCount,
            DisplayXRProvider.ActiveModeIndex, currentViewCount,
            currentIpd, steadyIpd);

// every frame:
float ipd = seq.Update(Time.unscaledDeltaTime, out bool fire, out uint mode);
rig.ipdFactor = ipd;
if (fire && mode != DisplayXRProvider.ActiveModeIndex)
    DisplayXRProvider.RequestRenderingMode(mode);
```

## `DisplayXRSceneMode` reference

| Field | Default | Meaning |
|---|---|---|
| `mode` | `ThreeD` | How this scene should be presented |
| `applyOnEnable` | `true` | Apply on enable. Turn off to drive transitions yourself via `Apply()` |
| `transitionSeconds` | `0.18` | Disparity ramp length. `0` switches instantly |
| `driveHardwareDisplayMode` | `true` | Also request the panel's hardware 2D/3D state |
| `steadyIpdFactor` | `0` (auto) | The `ipdFactor` to restore on return to 3D. Captured from the active rig on first apply when left at 0 |

Behavioral notes worth knowing:

- **`Apply()` is safe before the session exists.** The request is held and applied on the
  first frame the provider reports a live session.
- **It survives a subsystem restart.** The editor's dock/undock auto-switch stops and
  restarts the session mid-Play; a once-only push into a dead session succeeds *silently*,
  so the request is re-armed rather than dropped.
- **The 3D mode it returns to is captured, not guessed** — whatever multi-view mode was
  active the first time it looked, so a 2D→3D transition restores your app's real mode
  rather than "the first stereo mode in the list".
- **It waits for a late mode table.** In a **built player** the runtime's mode table is not
  populated at scene-load time — it reads empty or stale for a while *after* the session
  reports running, and only later advertises the mono mode. `DisplayXRSceneMode` refreshes
  and retries for up to 5 s rather than deciding once, then re-asserts the request until
  `ActiveModeIndex` actually reads back as the one it asked for. Hardware-verified: deciding
  once worked in the editor and silently degraded in players, which presents as
  `RequestDisplayMode` returning `true` while the active mode never changes — a 2D scene
  that renders weaved and looks black.
- **Graceful degrade.** If no mono mode appears within that window, it flattens disparity,
  asks for the panel's 2D state, and logs one warning. It never throws and never blocks
  scene load.

  > **The degrade path still head-tracks.** "Flat but still rendered through the stereo
  > path" means the mono view keeps the head-tracked pose, so **world-anchored UI visibly
  > follows the viewer's face**. A `Screen Space - Camera` canvas will drift and rescale as
  > the viewer moves. If you need screen-fixed UI on this path, use
  > [`DisplayXRWindowSpaceUI`](window-space-ui.md) — it is a composition layer at a fixed
  > window rect and is immune to the camera pose entirely.

## Patterns

**A 2D home screen that loads into a 3D scene.** Put a `DisplayXRSceneMode` set to `TwoD` in
the menu scene and one set to `ThreeD` in the content scene. Each applies on enable; the
sequencer handles the ramp in both directions.

**Screen-fixed 2D UI.** The configuration verified on hardware for a fully flat, clickable
home screen in a built player is:

> **no rig** + `DisplayXRSceneMode(TwoD)` + a full-rect `DisplayXRWindowSpaceUI` + the
> `Samples~/WindowSpaceUI` mouse router.

Do **not** reach for a `Screen Space - Camera` canvas here. While a session is running,
Unity's XR writes `Camera.fieldOfView` each frame from the runtime's projection, and such a
canvas sizes itself from that value — so it renders too large and rescales as the viewer's
head moves. `DisplayXRWindowSpaceUI` sidesteps this completely: it is a composition layer at
a fixed window rect, not something projected through the camera.

### Reading (and changing) the FOV you authored

`Camera.fieldOfView` is **not** your authored value in Play — XR overwrites it from the
projection, so it is tracking-derived and moves with the viewer (measured in the field at
76.5°–124.5° against an authored 60°). Anything doing FOV maths off it drifts silently.

`DisplayXRCamera.AuthoredFieldOfView` is the value the rig actually projects with:

```csharp
float fov = rig.AuthoredFieldOfView;   // what you set, stable
rig.AuthoredFieldOfView = 75f;         // change it at runtime
```

Assigning `Camera.fieldOfView` while a session is running does **not** work — XR overwrites
it each frame, and the rig projects from its own cache regardless. The setter above is the
supported way to change FOV at runtime.

**The authored FOV is serialized, so it survives a scene load (#274).** The rig captures it
from the Camera in edit mode only and stores it on the component, so a scene loaded while a
session is *already live* seeds from that stored value rather than from a
`Camera.fieldOfView` that XR has already stamped with a tracking-derived number. Without
this the cache seeded polluted, that value fed the next projection, and the FOV walked
roughly 2× per scene visit — the scene rendered smaller on every home → viewer round trip.
Scenes saved before this fix carry no captured value; open each one in the editor and save
it once, and the rig logs a one-shot warning whenever it has to fall back. Reflecting into
the private `m_CachedCameraFov` to work around the walk is no longer necessary.

A camera with **no rig** has no such record: nothing captured its authored FOV before XR
started, so the original is gone. Snapshot it yourself before the session comes up if you
need it — or use window-space UI, which doesn't care about the camera at all.

**A 2D overlay inside a 3D scene** is a *different* problem — you want
`DisplayXRWindowSpaceUI` (a composition layer at a fixed window rect) or `DisplayXRLocal2D`
(a flat 2D band inside the 3D scene), not a mode switch. Mode switching changes the whole
panel.

**Toggling at runtime** — call `Apply()` on the component. Don't call
`RequestRenderingMode` directly while a `DisplayXRSceneMode` is driving transitions, or the
two will fight over the same state.

## See also

- `Runtime/DisplayXRModeSwitch.cs` — the sequencer, including the reasoning for the
  asymmetry
- [`window-space-ui.md`](window-space-ui.md) — 2D UI *within* a 3D scene
- [`xr-display-provider.md`](xr-display-provider.md) — how the provider drives the runtime
