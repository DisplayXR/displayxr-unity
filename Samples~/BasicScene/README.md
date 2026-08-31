# Basic Scene Sample

Minimal camera-centric stereo setup for testing the DisplayXR plugin.

## What's Included

- `BasicScene.unity` — the scene to open: a **Main Camera** carrying the `DisplayXRCamera` component, plus a **Scene Setup** object carrying `BasicSceneSetup`
- `BasicSceneSetup.cs` — builds the test content (colored cubes at varying depths, a floor, a light) when you press Play

> The cubes are created at runtime rather than authored into the scene, so the sample
> picks a material shader that matches your project's render pipeline (URP `Lit`,
> falling back to Built-in `Standard`). That means the scene looks empty in the Scene
> view and fills in the moment you enter Play Mode — that is expected.

## Quick Start

1. Import this sample via Package Manager > DisplayXR > Samples > Basic Scene
2. Open `BasicScene.unity` (under `Assets/Samples/DisplayXR/<version>/Basic Scene/`)
3. Verify the Main Camera has a `DisplayXRCamera` component
4. Set `XR_RUNTIME_JSON` environment variable to your DisplayXR runtime
5. Enter Play Mode

## Without Hardware

With no 3D panel present, the runtime uses **sim_display** automatically — no env var needed. Optionally pick the sim output format before launching Unity:
```
SIM_DISPLAY_OUTPUT=sbs
```
