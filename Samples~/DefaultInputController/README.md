# Default Input Controller

Drop-in keyboard + mouse handler for navigating around a DisplayXR rig camera. Bundles the gestures we ship as a "first-five-minutes" sample so the plugin Runtime can stay focused on the rendering pipeline.

## What it does

| Input | Action |
|---|---|
| W / S | Move along view forward axis |
| A / D | Strafe |
| Q / E | Up / Down |
| Left-mouse drag | Rotate camera (yaw + pitch) |
| Scroll wheel | Zoom (camera scale on display-centric rigs, FOV on camera-centric) |
| Tab | Cycle active rig (via `DisplayXRRigManager.CycleNext`) |
| V | Toggle 2D / 3D display mode |
| Space | Reset pose to scene-load values |
| F11 | Toggle fullscreen |
| I | Screenshot |
| ESC | Quit |

## How to use

1. Package Manager → DisplayXR → Samples → "Default Input Controller" → **Import**.
2. The script lands at `Assets/Samples/DisplayXR/<version>/Default Input Controller/DisplayXRInputController.cs`.
3. Attach `DisplayXRInputController` to each rig camera GameObject (`DisplayXRDisplay` or `DisplayXRCamera` host).

You're now editing the input handler as project code, free to reshape or fork it.

## Inspector fields

- `moveSpeed` — meters per second for WASD/QE
- `rotationSensitivity` — radians per pixel for left-mouse-drag yaw/pitch
- `zoomSpeed` — scroll wheel zoom factor per tick
- **`mouseLookEnabled`** — set to `false` to disable left-drag-rotates-camera. Useful for apps that drive their own hit-tested left-drag (e.g. drag-to-rotate-target). WASD/scroll/keyboard still work.

## Why this is a sample, not Runtime

The plugin's Runtime should expose mechanisms (cursor polling on cloaked HWND, rig manager, mode setter); the policy (which key → which action, gesture priorities, target gating) belongs to the app. As of v1.5.9 this controller is shipped as opt-in sample code that apps can take ownership of, fork, or replace with their own.

Previous releases (≤ v1.5.8) bundled it under `Runtime/`. v1.5.9 left a deprecated shim there that logs a one-shot warning; the next minor release removes the shim entirely.

## Cross-references

- `DisplayXRRigManager` (plugin Runtime) — registry of active rigs, `CycleNext()`
- `DisplayXRFeature.Instance.RequestDisplayMode(bool)` (plugin Runtime) — 2D/3D mode
- `DisplayXRTransparentOverlay.PointerPosition / PointerDelta / IsLeftPressed` (plugin Runtime) — cursor state on transparent-overlay builds (Win32 native poll + Mac Mouse.current poll)
