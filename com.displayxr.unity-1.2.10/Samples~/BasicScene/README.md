# Basic Scene Sample

Minimal camera-centric stereo setup for testing the DisplayXR plugin.

## What's Included

- `BasicScene.unity` — A scene with a Main Camera using `DisplayXRCamera` component, plus a few colored cubes at varying depths to verify stereo rendering
- `BasicSceneSetup.cs` — Script that creates the scene objects programmatically (useful if the .unity scene file doesn't load cleanly across Unity versions)

## Quick Start

1. Import this sample via Package Manager > DisplayXR > Samples > Basic Scene
2. Open `BasicScene.unity`
3. Verify the Main Camera has a `DisplayXRCamera` component
4. Set `XR_RUNTIME_JSON` environment variable to your DisplayXR runtime
5. Enter Play Mode

## Without Hardware

Set these environment variables before launching Unity:
```
SIM_DISPLAY_ENABLE=1
SIM_DISPLAY_OUTPUT=sbs
```
