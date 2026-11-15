# Controller and Stylus Input — the "DisplayXR Input" subsystem

> **Status: new in the next release (branch `feat/stylus-unity-input`).** Native and
> editor compile checks plus a fake-runtime unit test; not yet run against a live
> stylus provider (see *Validation*).

## What it is

A DisplayXR runtime serves controllers and styluses from **input-provider plug-ins**
(runtime ADR-034 / ADR-054; discovery in
[`input-provider-discovery.md`](https://github.com/DisplayXR/displayxr-runtime/blob/main/docs/specs/runtime/input-provider-discovery.md)).
An OpenXR app reaches them the standard way: create actions, suggest bindings for an
interaction profile, attach, `xrSyncActions`, `xrLocateSpace`.

This plugin owns the app's OpenXR session (the `IUnityXRDisplay` provider loads the
runtime itself; there is no `com.unity.xr.openxr`), so before this subsystem no Unity
app could see any provider's controllers. The plugin now creates the session's actions
itself and surfaces them through an **`IUnityXRInput` provider**, `"DisplayXR Input"`.
Every hand the runtime binds becomes an ordinary `UnityEngine.XR.InputDevice`, so
`InputDevices.GetDevicesWithCharacteristics`, `TryGetFeatureValue`, `SendHapticImpulse`,
the Input System (`<XRController>{RightHand}/trigger`) and `TrackedPoseDriver` work
unchanged.

## Devices

One device per hand (`/user/hand/left`, `/user/hand/right`). Its kind follows the
interaction profile the runtime bound to that hand (`xrGetCurrentInteractionProfile`),
re-queried on `XrEventDataInteractionProfileChanged` and every 30 updates. A profile
change disconnects the device and connects a new one, because a device's feature list
is fixed at connection.

| Device name (`InputDevice.name`) | Bound profile | Characteristics |
|---|---|---|
| `DisplayXR Stylus` | `/interaction_profiles/logitech/mx_ink_stylus_logitech` | `HeldInHand \| TrackedDevice \| Controller \| Left/Right` |
| `DisplayXR Controller` | `/interaction_profiles/khr/simple_controller` | same |

Manufacturer `DisplayXR` for both. No profile bound = no device.

The stylus profile is the public Khronos vendor extension
`XR_LOGITECH_mx_ink_stylus_interaction`. The plugin enables it, and suggests its
bindings, **only when the runtime advertises it** in
`xrEnumerateInstanceExtensionProperties`; otherwise only `khr/simple_controller` is
suggested and a stylus provider (if any) appears through that profile, if the runtime
binds one. `DisplayXRInput.IsStylusProfileAvailable` says which case you are in.

### Features

The feature name is the usage, so `CommonUsages.*` and `DisplayXRStylusUsages.*` find
them with `TryGetFeatureValue`.

| Feature | Type | Stylus source | Simple controller source |
|---|---|---|---|
| `IsTracked` | bool | grip pose active, position + orientation valid | same |
| `TrackingState` | `InputTrackingState` | grip pose validity bits | same |
| `DevicePosition` / `DeviceRotation` | Vector3 / Quaternion | `/input/grip/pose` | `/input/grip/pose` |
| `DeviceVelocity` / `DeviceAngularVelocity` | Vector3 | grip pose velocity | same |
| `PointerPosition` / `PointerRotation` | Vector3 / Quaternion | `/input/aim/pose` | `/input/aim/pose` |
| `TipPosition` / `TipRotation` | Vector3 / Quaternion | `/input/tip_logitech/pose` | — |
| `Trigger` | float | `/input/tip_logitech/force` | `/input/select/click` as 0 / 1 |
| `TipPressure` | float | `/input/tip_logitech/force` (same as `Trigger`) | — |
| `TriggerButton` | bool | tip force > 0 | `/input/select/click` |
| `Grip` | float | `/input/cluster_middle_logitech/force` | — |
| `MiddlePressure` | float | `/input/cluster_middle_logitech/force` (same as `Grip`) | — |
| `GripButton` | bool | middle force ≥ 0.5 | — |
| `PrimaryButton` | bool | `/input/cluster_front_logitech/click` | — |
| `PrimaryDoubleTap` | bool | `/input/cluster_front_logitech/double_tap_logitech` | — |
| `SecondaryButton` | bool | `/input/cluster_back_logitech/click` | — |
| `SecondaryDoubleTap` | bool | `/input/cluster_back_logitech/double_tap_logitech` | — |
| `Docked` | bool | `/input/dock_logitech/docked_logitech` | — |
| `MenuButton` | bool | — | `/input/menu/click` |

`/input/system/click` is a system component of the stylus profile and is not bound.

**Haptics.** `device.SendHapticImpulse(0, amplitude, durationSeconds)` →
`xrApplyHapticFeedback` on the hand's `/output/haptic` (amplitude clamped to 0..1,
duration 0 = the runtime's minimum pulse, frequency unspecified). `StopHaptics()` →
`xrStopHapticFeedback`. One channel, impulses only (no buffers).

### Reading it

```csharp
using DisplayXR;
using UnityEngine.XR;

if (DisplayXRInput.TryGetStylus(out InputDevice pen, InputDeviceCharacteristics.Right))
{
    pen.TryGetFeatureValue(CommonUsages.trigger, out float tip);               // tip force 0..1
    pen.TryGetFeatureValue(DisplayXRStylusUsages.middlePressure, out float mid);
    pen.TryGetFeatureValue(CommonUsages.primaryButton, out bool front);
    pen.TryGetFeatureValue(DisplayXRStylusUsages.docked, out bool docked);
    if (DisplayXRInput.TryGetWorldPose(pen, DisplayXRStylusUsages.tipPosition,
                                       DisplayXRStylusUsages.tipRotation, out Pose tipWorld))
        marker.SetPositionAndRotation(tipWorld.position, tipWorld.rotation);
    if (front) pen.SendHapticImpulse(0, 0.5f, 0.05f);
}
```

The *Stylus Input* sample (`Samples~/StylusInput/DisplayXRStylusPointer.cs`) draws
pressure-sensitive strokes with exactly this.

### Input System

The Input System builds a layout for every XR device from its feature list (one
control per feature, the lower-cased feature name). The package registers two typed
layouts that those generated layouts extend, so the device instances are typed:

| Layout | Matches | Typed controls (besides `XRController`'s `devicePosition`, `deviceRotation`, `isTracked`, `trackingState`) |
|---|---|---|
| `DisplayXRStylus` | manufacturer `DisplayXR`, product `DisplayXR Stylus` | `tipPressure`, `middlePressure`, `trigger`, `triggerButton`, `grip`, `gripButton`, `primaryButton`, `primaryDoubleTap`, `secondaryButton`, `secondaryDoubleTap`, `docked`, `pointerPosition`/`Rotation`, `tipPosition`/`Rotation` |
| `DisplayXRController` | product `DisplayXR Controller` | `trigger`, `triggerButton`, `menuButton`, `pointerPosition`/`Rotation` |

Both derive from `XRControllerWithRumble` (`SendImpulse` drives the haptics) and carry
the `LeftHand` / `RightHand` usage. Binding paths:
`<DisplayXRStylus>{RightHand}/tipPressure`, `<DisplayXRStylus>/primaryButton`,
`<XRController>{RightHand}/trigger` (generic). The classes declare no controls of their
own (they resolve the generated ones in `FinishSetup`), so their state offsets can
never disagree with the native feature list. Compiled only where the Input System's XR
support is (`com.unity.modules.xr`, `ENABLE_VR`).

## Where the poses land: the rig-camera frame

Poses are **relative to the rig camera's transform, in rig units** — the frame the XR
Hands joints use ([hand-tracking.md](hand-tracking.md)), so a pen tip and a fingertip on
the same pixel agree:

| Active rig | Where the glass is, relative to the rig camera | Metres → rig units |
|---|---|---|
| `DisplayXRDisplay` | the camera's plane (z = 0) | `perspectiveFactor · virtualDisplayHeight / canvas height` |
| `DisplayXRCamera` | the convergence plane, `1 / invConvergenceDistance` in front | `(2 · tan(vFOV/2) / invConvergenceDistance) / canvas height` |
| `DisplayXRCamera`, `invConvergenceDistance` 0 | `nominalViewerZ` in front | 1 |
| rig-less camera | the display plane, LOCAL | 1 |

Parent the pen visual under the rig camera (local pose = device pose), or compose it
yourself: `DisplayXRInput.TryGetWorldPose` = `rigCamera.TransformPoint(position)`,
`rigCamera.rotation * rotation` (the camera's scale included, so scale-as-zoom keeps the
pen on the content). `QueryTrackingOriginMode` reports `Device`; recentring is the app's
(move the rig).

**Why the plugin rescales.** For a display-relative (`RIG_LOCAL`) input source — the
frame the runtime asks stylus and hand providers to publish in — the runtime locates an
action space at `P ∘ (s·L)` in the session's LOCAL space: `P` is the display plane the
session's rig puts in LOCAL (the views' frame), `s·L` the provider's display-plane pose
read on the canvas, in **metres** (`input-provider-discovery.md` §4b.4). Action spaces
do not get the rig's length scale; the views do. So the provider takes
`inv(P) ∘ G` and applies the rig's scale about the plane — the same point mapping the
runtime applies to rig-scoped hand joints (`XR_DXR_view_rig` spec 4/5). The rig used is
a snapshot of the descriptor the latest primary `xrLocateViews` chained, never the live
tunables, so the poses are in the rig the views were located with.
`DisplayXRInput.RigKind` says which rig that was (`DefaultDisplay` while the boot
splash holds the app's rig back — the same transient the hands document).

**Physical registration.** In a full-screen session (`canvas = display`) a pen touching
the glass lands on the glass, on the pixel it touches. In a **window** the runtime
reads `L` as fractions of the physical display height and applies it to the canvas
(runtime ADR-034 Amendment 7): the pen keeps its place relative to the *scene*, at the
window's scale, not to the physical glass under it. That is a runtime rule; this plugin
re-expresses whatever the runtime returns and does not undo it.

## Component map

| Piece | Where | Role |
|---|---|---|
| `displayxr_provider_input.{h,cpp}` | `native~/displayxr_xrprovider/` | The OpenXR side: extension gate, action set + 13 actions, suggested bindings for both profiles, action spaces, attach, sync, per-hand state, rig-frame conversion, haptics. One mutex orders it against the session teardown (the hands TU's model). |
| `displayxr_input_provider.cpp` | `native~/displayxr_xrprovider/` | The `IUnityXRInput` provider: device connect / disconnect per hand, feature definitions, state writes, haptic callbacks. Main thread. |
| Session hooks | `displayxr_provider_session.cpp` | Probes `XR_LOGITECH_mx_ink_stylus_interaction`, enables it, calls the TU after `xrCreateInstance`, after the LOCAL space exists, on every `xrBeginFrame` (predicted display time), after the primary `xrLocateViews` (rig frame), on `XrEventDataInteractionProfileChanged`, and before `xrDestroySession`. |
| `IUnityXRInput.h` | `native~/unity_pluginapi/` | Vendored Unity XR SDK header (Unity Companion License), same SDK drop as the display headers. |
| `UnitySubsystemsManifest.json` | `Runtime/` | `"inputs": [{ "id": "DisplayXR Input" }]`. |
| `DisplayXRDisplayLoader` | `Runtime/Provider/` | Creates / starts / stops / destroys the `XRInputSubsystem` with the display subsystem; pushes the native switch before each session start. |
| `DisplayXRInput`, `DisplayXRStylusUsages` | `Runtime/Provider/DisplayXRInput.cs` | App-facing API: `Enabled`, `IsActive`, `IsStylusProfileAvailable`, `RigKind`, `TryGetStylus`, `TryGetController`, `TryGetWorldPose`, the custom usages. |
| `DisplayXRStylus`, `DisplayXRController` | `Runtime/Provider/DisplayXRInputLayouts.cs` | Input System layouts. |
| Sample *Stylus Input* | `Samples~/StylusInput/` | `DisplayXRStylusPointer`. |
| `input_binding_test` | `native~/tests/` | Fake-runtime unit test (CI: *Input binding tests*). |

## Threading and lifetime

The session lives on Unity's graphics thread; the input subsystem ticks on the main
thread (`Tick` → sync + read + locate on the dynamic update, relocate before render;
`UpdateDeviceState` reads the snapshot). `xrSyncActions`, `xrGetActionState*`,
`xrLocateSpace` and `xrApplyHapticFeedback` run on the main thread under the TU's lock;
the session teardown takes the same lock to destroy the spaces and the action set
before `xrDestroySession`. Nothing is synced before the first `xrBeginFrame`.

Everything is per instance: the provider creates a new instance on every session start
(the editor's dock/undock restart included), so the action set, bindings, spaces and
attach are redone each time and survive restarts the way the hands do.

**Opt-out:** `DisplayXRInput.Enabled = false` before XR init, or `DISPLAYXR_INPUT=0`.
Disabled = no input subsystem, no action set, no extension enabled: the session is
exactly what it was before this subsystem existed.

## Validation

Done so far:

- `native~\build-win.bat` (MSVC) builds the DLL with the new exports.
- `input_binding_test` (fake runtime) passes: stylus profile only with its extension
  and never `/input/system/click`; simple controller always; the opt-out creates no
  action set; per-hand kind from the bound profile; per-hand action state; display-rig
  and camera-rig pose conversion; haptic clamping and nanosecond durations; teardown.
- Editor compile check (Unity 6000.4, batchmode, package as a `file:` dependency): no
  compile errors; a device described the way the native provider describes a stylus
  gets the `DisplayXRStylus` type with every typed control resolved and distinct state
  offsets.

Still to do on a box with a stylus provider:

1. `displayxr-cli selftest` lists the input provider; the runtime advertises
   `XR_LOGITECH_mx_ink_stylus_interaction`.
2. Native log (`%TEMP%\displayxr_prov_native.log`): `XR_LOGITECH_mx_ink_stylus_interaction:
   advertised` → `input: suggested 22 bindings for logitech/mx_ink_stylus_logitech:
   accepted` → `input: action set ATTACHED` → `input: right hand -> stylus` →
   `input: right device N connected as "DisplayXR Stylus"`.
3. Import *Stylus Input* into a scene with a `DisplayXRDisplay` rig at full screen:
   the marker sits on the pen tip, a stroke drawn on the glass lands at zero parallax
   under the tip; tip/middle pressure, both buttons, double taps, dock and both haptic
   pulses behave as the table says. Repeat under a `DisplayXRCamera` rig and in a
   window (see *Physical registration* for the expected window behaviour).
4. Unplug / dock the pen: the device disconnects or goes untracked, no errors; replug:
   it reconnects. Dock/undock the Game view: input resumes without a Play restart.
5. A project with `DisplayXRInput.Enabled = false`: no `input:` lines after the
   disabled one, behaviour as before.

## Limitations

- Two profiles only. Other profiles (`khr/generic_controller`, gamepads on
  `/user/gamepad`, vendor controller profiles) are not suggested; a provider the
  runtime exposes only through them stays invisible.
- In a multi-zone app the runtime takes the canvas from the most recent
  `xrLocateViews` (the last zone's), like the hands.
- Poses are located at the latest frame's predicted display time (no
  `TryGetDeviceStateAtTime`).
- Camera rigs need a runtime with `XR_DXR_view_rig` ≥ 5 for the glass on the
  convergence plane (as for the hands).
