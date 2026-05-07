# Display Scene Sample — Crate Test

Reproduces the native `cube_shared_metal_macos` test app in Unity. A wooden crate on a grid floor with two cameras demonstrating both stereo rig modes for direct A/B comparison.

## Quick Start

1. Import via **Package Manager > DisplayXR > Samples > Display Scene**
2. Create a new scene (**File > New Scene > Basic Built-in**)
3. Add **DisplaySceneSetup** to any GameObject (e.g. the Main Camera)
4. Set `XR_RUNTIME_JSON` environment variable and enter Play Mode

The script creates everything: crate, grid floor, lighting, and both cameras. The default Main Camera is replaced.

## Scene (created by script)

```
DisplayCam              (0, 0, 0) — display surface pose
├── Camera + DisplayXRDisplay (active by default)

CameraCam               (0, 0.05, 0.3) — viewer behind crate, looking at it
├── Camera + DisplayXRCamera (convergence=0.3m, FOV=40)

Crate                   (0, 0.03, 0) — 0.06m textured cube, slowly rotating
GroundGrid              (0, 0, 0) — checker-textured quad
KeyLight                — warm directional light
```

## Controls

| Input | Action |
|-------|--------|
| **C** | **Toggle active camera** (display-centric / camera-centric) |
| Mouse drag | Rotate active camera (yaw/pitch) |
| WASD | Move active camera (relative to view direction) |
| Q/E | Move down/up |
| Scroll | Display: scale factor. Camera: zoom (FOV) |
| Shift+Scroll | Adjust IPD factor [0-1] |
| Ctrl+Scroll | Adjust parallax factor [0-1] |
| Option+Scroll | Display: perspective factor. Camera: convergence distance |
| Space | Reset active camera to defaults |
| Double-click | Reset active camera to defaults |

## Without Hardware

```
SIM_DISPLAY_ENABLE=1
SIM_DISPLAY_OUTPUT=sbs
```
