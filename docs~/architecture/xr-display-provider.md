# Custom IUnityXRDisplay Display Provider (epic #166, M1)

> **Status: M1 spike (GO/NO-GO).** Native side builds and is registered; on-panel /
> sim_display weave validation is the user-run step (see the runbook at the end).
> This document describes the architecture, the design decisions, and the open
> questions that the Editor-side validation must resolve.

## Why this exists

Today the plugin reaches the runtime via an **OpenXR API-layer hook**
(`displayxr_hooks.cpp`) riding Unity's own OpenXR plugin. The runtime therefore
treats Unity as a **fixed-resolution, always-2-view "legacy compromise" app**
(see `displayxr-runtime/docs/architecture/unity-d3d12-app-path.md`):
`XR_EXT_display_info` off, mode keys disabled, per-view resolution fixed at session
start, and the plugin coupled to Unity's OpenXR-stack internals.

Epic #166 replaces the hook with a **custom `IUnityXRDisplay` Display Provider**
(the route Oculus / Varjo / Google Cardboard use). We drive the session; Unity
still renders (keeping **SPI + URP/HDRP**); we enable `XR_EXT_display_info` so the
runtime treats us as a **first-class extension app** — the Unity analog of the UE
plugin (`unreal-d3d12-app-path.md`). M1 proves the architecture end-to-end for
**stereo (SPI)** and reports GO/NO-GO; M2–M6 build out parity.

## Component map

| File | Role |
|---|---|
| `Runtime/UnitySubsystemsManifest.json` | Registers the display subsystem (`name: "DisplayXR"`, display `id: "DisplayXR Display"`, `libraryName: "displayxr_unity"`). Unity scans package manifests at load. |
| `native~/displayxr_xrprovider/displayxr_display_provider.cpp` | Unity-facing half: `RegisterLifecycleProvider`, main-thread + graphics-thread provider callbacks, `CreateTexture` handoff, per-frame `PopulateNextFrameDesc` / `SubmitCurrentFrame`. |
| `native~/displayxr_xrprovider/displayxr_provider_session.{h,cpp}` | Runtime-facing half: loads the DisplayXR runtime via `xrNegotiateLoaderRuntimeInterface`, creates instance/system/session on **Unity's D3D12 device**, enables EXT extensions, creates the `arraySize=2` (SPI) swapchain, consumes `XR_EXT_view_rig` render-ready views, submits the 2-view projection layer. |
| `native~/displayxr_unity_plugin.cpp` | `UnityPluginLoad` calls `displayxr_register_xr_display_provider(ifaces)`. Single DLL — no second plugin. |
| `native~/unity_pluginapi/` | Vendored Unity XR SDK headers (`IUnityXRDisplay.h`, `UnitySubsystemTypes.h`, `UnityXRTypes.h`, …) — Unity ships only graphics headers in the editor; the XR provider headers are sourced from the Unity XR SDK (Unity Companion License). |

The provider compiles into the **existing** `displayxr_unity` target (Windows
block in `CMakeLists.txt`) and registers from the **existing** `UnityPluginLoad`.
The OpenXR hook path and the editor standalone path are untouched (they stay until
M5).

## How the provider maps to the runtime session

```
Unity engine                          DisplayXR runtime (in-process, Unity's D3D12 device)
────────────                          ────────────────────────────────────────────────────
UnityPluginLoad
  └─ RegisterLifecycleProvider("DisplayXR","DisplayXR Display")
Lifecycle.Initialize (main thread)
  ├─ RegisterProviderForGraphicsThread
  └─ RegisterProvider (main)
GfxStart (render thread)
  ├─ Get Unity ID3D12Device + queue (IUnityGraphicsD3D12v8::GetDevice/GetCommandQueue)
  └─ dxr_prov_session_start ───────►  xrNegotiateLoaderRuntimeInterface (load runtime DLL)
                                       xrCreateInstance(+display_info,+D3D12,+win32_binding,+view_rig)
                                       xrGetSystem / xrGetSystemProperties(XrDisplayInfoEXT)
                                       xrGetD3D12GraphicsRequirementsKHR (LUID; Unity's device honored)
                                       xrCreateSession(GraphicsBindingD3D12{Unity device,queue} → win_binding)
                                       xrCreateReferenceSpace(LOCAL)
                                       (deferred) xrCreateSwapchain(arraySize=2) on session-ready
PopulateNextFrameDesc (per frame)
  ├─ dxr_prov_poll_events ─────────►  xrPollEvent → on READY: xrBeginSession + create swapchain
  ├─ create_textures_if_ready ─────►  CreateTexture(nativePtr = runtime swapchain image[i])  ← zero-copy
  ├─ dxr_prov_begin_frame ─────────►  xrWaitFrame + xrBeginFrame + xrLocateViews(XrDisplayRigEXT) + Acquire/Wait
  └─ fill 1 RenderPass × 2 RenderParams (slices 0/1, projection from view fov)
[Unity renders both eyes into the acquired swapchain image's 2 array slices]
SubmitCurrentFrame
  └─ dxr_prov_submit_frame ────────►  xrReleaseSwapchainImage + xrEndFrame(Projection, 2 views, imageArrayIndex 0/1)
                                       → runtime DP weaves to its window (or panel)
```

## CreateTexture ↔ runtime-swapchain handoff (zero-copy)

**Decision: bind the OpenXR session to Unity's own D3D12 device** (the runtime's
documented Unity-D3D12 contract — `unity-d3d12-app-path.md`: "swapchain images are
compositor-owned `ID3D12Resource`s on Unity's device"). Then:

1. The runtime's `arraySize=2` swapchain images are `ID3D12Resource*` **on Unity's
   device**, retrieved via `dxr_prov_get_swapchain_image(i)`.
2. The provider wraps each with `IUnityXRDisplayInterface::CreateTexture`, passing
   `desc.color.nativePtr = <that ID3D12Resource*>`, `textureArrayLength = 2`. Unity
   renders **directly into the runtime's swapchain image** — no copy, no bridge.
3. `PopulateNextFrameDesc` sets `renderPasses[0].textureId` to the Unity texture
   wrapping the **acquired** swapchain image (rotates by acquired index).

> **M1 EMPIRICAL RESULT — zero-copy fails; the bridge IS required.** M1 first
> tried zero-copy (deviating from the plan's "bridge-first"). On-panel validation
> (RTX 3080, Leia DP, dev runtime v1.26.1) showed the full path works up to and
> including a *successful* `CreateTexture` — and then Unity **crashes with D3D12
> device-removed** (`887a0005`) the moment it touches the XR textures
> (`XRTextureManager::RequestCreateTexture`). Root cause: although we pass Unity's
> `ID3D12Device` to `xrCreateSession`, the runtime's in-process D3D12 compositor
> allocates its swapchain `ID3D12Resource`s on its **own** device instance;
> handing those raw cross-device pointers to Unity is invalid D3D12 usage →
> device removal. **Cross-`ID3D12Device` sharing requires shared NT handles — i.e.
> the bridge.** This vindicates the plan's original "bridge-first" call.
>
> **Required fix (M1b):** run the runtime session on a **separate** D3D12 device
> (as `displayxr_standalone_d3d12.cpp` already does), allocate a **shared,
> 2-slice-array** bridge texture (NT handle, opened on Unity's device), hand the
> *bridge's* Unity-side pointer to `CreateTexture`, and per-frame copy
> bridge→runtime-swapchain with a cross-device fence (mirror `create_atlas_bridge`
> / `blit_atlas`, extended from `arraySize=1` to `arraySize=2`). Everything else in
> this provider is unchanged and already validated.
>
> **M1b RESULT — SOLVED (2026-06-29, on the Leia panel).** Implemented the bridge
> (own D3D12 device matched to the runtime adapter LUID → session bound to it →
> shared 2-slice-array bridge → per-frame copy bridge→swapchain both slices,
> fence-synced). The crash is gone; the session now runs to **FOCUSED** and the
> runtime's D3D12 native compositor **weaves to the panel** (`D3D12 weaving via
> display processor`, `atlas=3840x1080 tile 2x1`) with **zero** device-removed,
> `VIEW SIZE MISMATCH`, or `CALL_ORDER_INVALID` errors. The SPI swapchain is sized
> to the active 3D mode's **per-eye** pixels (1920×1080), and an acquire guard
> prevents a startup double-`xrWaitSwapchainImage`. Remaining refinements (not
> blockers): (1) coarse cross-device sync — copy fence-waits on the own queue but
> has no shared fence with Unity's writes (possible 1-frame latency/tearing);
> (2) stereo separation needs a tracked face *or* app tunables — the bare scene's
> view-rig defaults to `virtualDisplayHeight = display height`, and the raw eyes
> read nominal when untracked; (3) wire `dxr_prov_set_tunables` from
> `DisplayXRDisplay` (M2); (4) `WS_CHILD` overlay for in-app-window weave (M1 uses
> `windowHandle=NULL` → runtime self-hosts).

## SPI render-pass setup

`UnityXRNextFrameDesc` models single-pass-instanced as **1 `UnityXRRenderPass` ×
2 `UnityXRRenderParameter`** over a 2-slice texture array (`IUnityXRDisplay.h`,
"Single pass stereo rendering (1 RenderPass x 2 RenderParams)"). The provider:

- `renderPassesCount = 1`, `renderParamsCount = 2`.
- `renderParams[eye].textureArraySlice = eye` (0 = left, 1 = right).
- `renderParams[eye].projection` = `kUnityXRProjectionTypeHalfAngles` with
  `{left,right,top,bottom} = tan(fov.angleLeft/Right/Up/Down)` from the runtime's
  render-ready `XR_EXT_view_rig` view.
- `renderParams[eye].deviceAnchorToEyePose` = the view pose, converted OpenXR
  (right-handed, −Z fwd) → Unity (left-handed, +Z fwd).
- `GfxStart` sets `renderingCaps.noSinglePassRenderingSupport = false`.

On submit, the projection layer carries `subImage.imageArrayIndex = eye`, matching
the runtime's shipped SPI fix (per-view SRV `FirstArraySlice = imageArrayIndex`).

## Extensions enabled (extension-app, not legacy)

`XR_EXT_display_info` (geometry + becomes an extension app), `XR_KHR_D3D12_enable`
(graphics binding), `XR_EXT_win32_window_binding` (weave target window),
`XR_EXT_view_rig` (runtime-owned Kooima → render-ready views; probed before
enabling, since older runtimes reject unknown extensions). Without `XR_EXT_view_rig`
the provider logs a one-shot WARN and produces no stereo (same contract as the hook
/ standalone).

## Open questions (resolve at Editor validation)

1. **Does a third-party provider register + start?** Cardboard proves the API is
   not partner-gated; confirm `XRDisplaySubsystem` starts with our manifest
   (`Player.log`: "Lifecycle Initialize: providers registered" / "GfxStart OK").
2. **`GetCommandQueue` access.** `IUnityGraphicsD3D12::GetCommandQueue` may be gated
   by `ConfigureEvent(graphicsQueueAccess = Allow)`. The provider logs a WARNING if
   the queue is null; if so, call `ConfigureEvent` in `GfxStart` (the runtime needs
   a queue for the D3D12 graphics binding).
3. **Resource-state coordination.** Unity and the runtime share the swapchain
   `ID3D12Resource` on one device; the image must be in the right state for Unity to
   render (RTV) and for the runtime to sample (atlas blit/weave). Confirm no
   D3D12 debug-layer state errors; if so, use `RequestResourceState` /
   `NotifyResourceState` around the pass.
4. **OpenXR→Unity pose/quaternion convention.** Position `(x,y,−z)`, quaternion
   `(−x,−y,z,w)` is the M1 assumption — verify parallax sign + no left/right swap on
   the panel; adjust if mirrored.
5. **Frame cadence / thread.** M1 drives `xrWaitFrame/Begin/End` on Unity's render
   thread inside the gfx callbacks (the contract allows `PopulateNextFrameDesc` to
   block for cadence). UE uses a dedicated compositor thread so the render thread
   never blocks on vsync — adopt that if cadence stalls appear.
6. **Window binding.** M1 passes `windowHandle = NULL` (runtime self-hosts a window
   — fine for sim_display bring-up). In-app weave over Unity's own window needs the
   `WS_CHILD` overlay (`displayxr_get_app_main_view`, reused from the hook path) —
   M2.

## Validation runbook (user-run)

All in a **non-elevated** terminal (the OpenXR loader ignores `XR_RUNTIME_JSON`
when elevated).

1. **Runtime, hardware-free:** in `displayxr-runtime`, register the sim DP once
   (elevated, one-time): `scripts\register_dev_plugin.bat`; then
   `set SIM_DISPLAY_OUTPUT=sbs`; sanity-check `displayxr-cli selftest`. Point
   `XR_RUNTIME_JSON` at the dev runtime JSON.
2. **Import the plugin** into `displayxr-unity-test` (BiRP) — the freshly built
   `Runtime/Plugins/Windows/x64/displayxr_unity.dll` ships with the package — and in
   **XR Plug-in Management** enable the **DisplayXR** display subsystem.
3. **Run / build a player.** Confirm in `Player.log`:
   `[DisplayXR-PROV] RegisterLifecycleProvider OK` → `Lifecycle Initialize` →
   `GfxStart OK` → `SPI swapchain: …arraySize=2` → `session state: …`.
4. **SPI active:** confirm Unity renders **1 instanced pass** to a 2-slice array
   (frame debugger / `XRSettings`), not 2 passes.
5. **Weave:** capture the atlas via the runtime file trigger —
   `rm %LOCALAPPDATA%\Temp\workspace_screenshot.png` then
   `touch %LOCALAPPDATA%\Temp\workspace_screenshot_trigger` → read the PNG.
6. **URP:** repeat in `displayxr-unity-test-2d-ui`.
7. **Panel (optional):** register the Leia plugin and confirm weave on hardware.
