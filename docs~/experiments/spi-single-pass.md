# Experiment: Single Pass Instanced (SPI) on the URP path

**Branch:** `experiment/spi-single-pass` · **Status:** ✅ ROOT-CAUSED — one runtime fix away

## Result (2026-06-28) — plugin/Unity side correct; blocker is one runtime change

Tested on hardware (displayxr-unity-test-2d-ui, RTX 3080 SR display, D3D12, URP 17.0.4).
SPI renders FLAT, but **not** because of anything in the plugin or Unity. With an
eye-tracking lock the entire chain is per-eye correct; the woven image stays flat solely
because the **runtime compositor ignores `XrSwapchainSubImage::imageArrayIndex`** and
samples array layer 0 for both eyes. Fix that one thing (we own the runtime) and SPI works.

### The investigation had two red herrings — record them so we don't repeat them

1. **`tracking=0` fallback looks like "SPI is mono."** When no face is locked, the runtime
   correctly collapses both eyes to the nominal viewer (flat 2D is the safe no-track
   output). Several early captures (and the first native `xrLocateViews` A/B that
   "proved the runtime returns identical views under SPI") were taken with `tracking=0`.
   That conclusion was an artifact — **invalid; do not trust it.** WITH a lock
   (`tracking=1`), `hooked_xrLocateViews` returns fully distinct eyes under SPI
   (`rawEyes` separated, `dPosX≈0.01–0.08`, fovs differ) exactly like MultiPass.
2. **The boot splash** (transient 2nd rig, on by default) truncated `displayxr.log` and
   muddied early captures. Run diagnostics with `DISPLAYXR_NO_SPLASH=1`.

### What is actually true (tracking=1, splash off)

- Per-eye separation is present at EVERY stage: `GetStereoMatrices` `projL!=projR`;
  Unity's own `xr.GetProjMatrix(0)!=(1)` and `GetViewMatrix(0)!=(1)` at RecordRenderGraph;
  our pushed `sVP0!=sVP1`. The `SetGlobalMatrixArray` override is distinct and reaches the
  instanced draws. **Nothing on the plugin or Unity side is mono.**
- `xrCreateSwapchain: ... arrays=2` — Unity creates a **2-layer texture-array** swapchain
  and renders the two distinct eyes into layers 0 and 1.
- `xrEndFrame` submits ONE `XrCompositionLayerProjection` with `viewCount=2`, two views
  into that one swapchain: `view[0].subImage.imageArrayIndex=0` (left),
  `view[1].subImage.imageArrayIndex=1` (right), distinct poses. **Textbook SPI.**
- Yet the woven output is FLAT. The only explanation consistent with "MultiPass 3D, SPI
  flat" is that the compositor reads array **layer 0 for both views**. MultiPass works
  because each eye is its own swapchain (always layer 0); SPI puts the right eye in layer 1.

### The fix (both sides — we control the runtime)

- **Runtime (the blocker):** in the Display Processor / weave, when reading each projection
  view's source image, sample swapchain array layer = `view.subImage.imageArrayIndex`
  instead of a hardcoded 0. Equivalently, create the per-view image view / SRV for the
  texture-array layer the view references. This is the single change that unblocks SPI.
- **Plugin (already done on this branch):** `DISPLAYXR_SPI_EXPERIMENTAL` skips the forced
  MultiPass; `KooimaProjectionFixFeature` gates on `xr.viewCount==2` and re-pushes the
  runtime per-eye projection into both `unity_StereoMatrix*` slots (off-axis #127 fix,
  proven distinct + reaching the draws). No further plugin work needed for disparity.
- **Payoff:** ~half the CPU draw/cull cost vs MultiPass, URP-on-Windows. BiRP and
  macOS/Metal stay MultiPass (no command-buffer hook / Unity SPI-Metal broken).

### Verify after the runtime fix

Build the test repo SPI (`DISPLAYXR_SPI_EXPERIMENTAL` define + OpenXR Render Mode =
Single Pass Instanced), run with `DISPLAYXR_NO_SPLASH=1`, sit in front (tracking=1), and
confirm 3D matches the MultiPass build. The native `[rig-diag]` and C# `[KooimaProjFix][SPI]`
logs already print everything needed to confirm per-eye distinctness end-to-end.

---

(Original prototype plan, kept for reference:)

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
4. ~~Confirm `KooimaProjectionFixFeature` is on the URP renderer (it auto-wires; or
   `DisplayXR > Setup URP Projection Fix`).~~ **No longer applicable.** This step, and
   every mention of that feature below, describes the code as it stood in June 2026.
   The feature and both of its menu items were removed in **v2.2.0**: the provider now
   hands Unity a full per-eye projection matrix, so URP consumes the off-center frustum
   natively. Skip this step; there is nothing to wire.
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
