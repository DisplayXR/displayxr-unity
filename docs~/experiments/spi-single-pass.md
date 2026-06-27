# Experiment: Single Pass Instanced (SPI) on the URP path

**Branch:** `experiment/spi-single-pass` · **Status:** prototype, untested on hardware

## Goal

Decide, on hardware, whether DisplayXR can run **Single Pass Instanced** instead of
the forced **Multi-Pass** — halving the per-eye CPU/cull/draw cost — now that the URP
adapter injects the Kooima projection through the command buffer (`KooimaProjectionFix
Feature`, #127) rather than through `Camera.SetStereoProjectionMatrix` (the API that
forced Multi-Pass, see `ADR-005`).

This is **URP-only**. BiRP has no command-buffer injection hook and its only lever
(`SetStereoProjectionMatrix`) is Multi-Pass-only; macOS/Metal SPI is broken in Unity.
So a positive result means *SPI as a URP-on-Windows opt-in*, not a global switch.

## The open question being tested

Under SPI a single pass renders both eyes via GPU instancing; shaders read
`unity_StereoMatrix*[unity_StereoEyeIndex]`. The prototype writes **both** array slots
with the runtime's correct per-eye matrices via `cmd.SetGlobalMatrixArray`. **Unknown:**
whether that override sticks for the opaque draws, or whether the engine re-binds its
own fov-built `UnityStereoGlobals` cbuffer (which mishandles off-center frustums — the
original #127 bug). The hardware A/B answers exactly this.

## What the prototype changes (C# only — no native rebuild)

- `DisplayXRFeature.OnInstanceCreate` — with `DISPLAYXR_SPI_EXPERIMENTAL` defined, does
  **not** force Multi-Pass; logs a warning banner with the active render mode. Without
  the define: unchanged (Multi-Pass forced).
- `DisplayXRFeature` validation rule — the "requires Multi-Pass" error is suppressed
  under the define.
- `KooimaProjectionFixFeature` — adds an SPI branch gated on `xr.singlePassEnabled &&
  xr.viewCount == 2`. DisplayXR is N-view in general, but Unity's render path is always
  `PRIMARY_STEREO`; the `viewCount == 2` check is the precise form of "display max view
  count == 2" and bails safely if a pass ever carried more.

## How to test (use a URP DisplayXR test repo)

`displayxr-unity-test-2d-ui` or `displayxr-unity-test-transparent` (both URP). The SPI
path never runs on a BiRP project.

1. Pin the test repo's plugin to this branch (temporarily, in `Packages/manifest.json`):
   `"com.displayxr.unity": "https://github.com/DisplayXR/displayxr-unity.git#experiment/spi-single-pass"`
   then refresh `packages-lock.json` (delete its entry or bump the hash).
2. Player Settings → Other Settings → **Scripting Define Symbols** (Windows): add
   `DISPLAYXR_SPI_EXPERIMENTAL`.
3. Project Settings → XR Plug-in Management → OpenXR → **Render Mode = Single Pass
   Instanced**.
4. Confirm `KooimaProjectionFixFeature` is on the URP renderer (it auto-wires; or
   `DisplayXR > Setup URP Projection Fix`).
5. Build & run on the SR display (D3D12).

### Reading the result

- **PASS:** image matches the Multi-Pass build — correct off-axis parallax, the
  click-through silhouette still sits under the content, no eye over-separation.
- **FAIL:** off-axis deformation / over-separation / double image → the engine ignored
  the array override; SPI is not viable without engine-level matrix injection.
- Log lines `[KooimaProjFix][SPI] views=2 slot0.VP.m02=… slot1.VP.m02=…` confirm the
  branch ran and what it pushed. The banner `[DisplayXR][SPI-EXPERIMENT] … NOT forcing
  MultiPass` confirms the define took.

### Baseline A/B

Build once **without** the define (Multi-Pass, known-good) and once **with** it (SPI).
Compare the same camera pose. Identical → SPI works.

## If it fails

The fallback would be a custom matrix injection below URP (e.g. an `XRDisplaySubsystem`
/ provider-level override), which is a much larger lift. Record the verdict here and in
`ADR-005` before deleting the branch.
