# Custom IUnityXRDisplay Display Provider (epic #166)

> **Status: Shipping (v1.24.x → v2.0.0).** The custom IUnityXRDisplay provider is the sole rendering backend — the legacy OpenXR hook and the standalone editor-preview session were removed (#166). See [ADR-007](../adr/ADR-007-render-path-by-view-count.md) for the >8-view (light-field/quilt) roadmap.

## Why this exists

The plugin **used to** reach the runtime via an **OpenXR API-layer hook**
(`displayxr_hooks.cpp`) riding Unity's own OpenXR plugin. In that model the runtime
treated Unity as a **fixed-resolution, always-2-view "legacy compromise" app**
(see `displayxr-runtime/docs/architecture/unity-d3d12-app-path.md`):
`XR_EXT_display_info` off, mode keys disabled, per-view resolution fixed at session
start, and the plugin coupled to Unity's OpenXR-stack internals.

Epic #166 replaced that hook with a **custom `IUnityXRDisplay` Display Provider**
(the route Oculus / Varjo / Google Cardboard use), and this provider is now the
**sole shipping backend** — the hook and the standalone editor-preview session were
removed. We drive the session; Unity still renders (keeping **SPI + URP/HDRP**); we
enable `XR_EXT_display_info` so the runtime treats us as a **first-class extension
app** — the Unity analog of the UE plugin (`unreal-d3d12-app-path.md`). The
historical M1–M2 bring-up sections below document how the architecture was proven
and hardened on hardware. For the >8-view (light-field/quilt) roadmap that layers on
top of this provider, see [ADR-007](../adr/ADR-007-render-path-by-view-count.md).

## Component map

| File | Role |
|---|---|
| `Runtime/UnitySubsystemsManifest.json` | Registers the display subsystem (`name: "DisplayXR"`, display `id: "DisplayXR Display"`, `libraryName: "displayxr_unity"`). Unity scans package manifests at load. |
| `native~/displayxr_xrprovider/displayxr_display_provider.cpp` | Unity-facing half: `RegisterLifecycleProvider`, main-thread + graphics-thread provider callbacks, `CreateTexture` handoff, per-frame `PopulateNextFrameDesc` / `SubmitCurrentFrame`. |
| `native~/displayxr_xrprovider/displayxr_provider_session.{h,cpp}` | Runtime-facing half: loads the DisplayXR runtime via `xrNegotiateLoaderRuntimeInterface`, creates instance/system/session on **Unity's D3D12 device**, enables EXT extensions, creates the `arraySize=2` (SPI) swapchain, consumes `XR_EXT_view_rig` render-ready views, submits the 2-view projection layer. |
| `native~/displayxr_unity_plugin.cpp` | `UnityPluginLoad` calls `displayxr_register_xr_display_provider(ifaces)`. Single DLL — no second plugin. |
| `native~/unity_pluginapi/` | Vendored Unity XR SDK headers (`IUnityXRDisplay.h`, `UnitySubsystemTypes.h`, `UnityXRTypes.h`, …) — Unity ships only graphics headers in the editor; the XR provider headers are sourced from the Unity XR SDK (Unity Companion License). |

The provider compiles into the `displayxr_unity` target (Windows block in
`CMakeLists.txt`) and registers from `UnityPluginLoad`. The OpenXR hook path and the
editor standalone (SA) editor-preview session/window have since been **removed** —
the provider is the only backend. The dormant `displayxr_standalone*`
render-to-atlas core is **kept in-tree but out of the CMake build**, preserved as the
seed for the future >8-view quilt/atlas render path (see
[ADR-007](../adr/ADR-007-render-path-by-view-count.md)); it is not compiled.

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
> `DisplayXRDisplay` (M2); (4) in-app-window weave — now the default via a top-level
> `WS_POPUP` overlay (see "Weave target" below); M1 used `windowHandle=NULL` self-host.

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

## M2 — native-app parity (implemented)

M2 turns the M1 spike into a first-class extension app matching the native
handle app (`cube_handle_d3d12_win`) / the UE plugin. Three buckets + a proper
loader.

### A. Rig-EXT driven by real per-frame tunables

The provider now drives the runtime's Kooima math from the **active DisplayXR
rig**, not fixed defaults — so a tracked face gets correct depth/parallax.

- New C# layer (`Runtime/Provider/`): `DisplayXRProviderNative` (the `dxr_prov_*`
  P/Invokes), `DisplayXRProvider` (app-facing facade), `DisplayXRProviderDriver`
  (the per-frame runner created/destroyed by the loader).
- The driver reads the active rig
  (`DisplayXRRigManager.ActiveCamera` → `DisplayXRDisplay` /`DisplayXRCamera`)
  via a new behaviour-preserving `GetProviderTunables()` on each rig, and pushes
  `dxr_prov_set_tunables` + `dxr_prov_set_display_pose` each frame (mirroring the
  hook-path LateUpdate push, which is inert in provider mode because
  `DisplayXRFeature` needs Unity's OpenXR loader). It respects the same gating
  (`SplashActive`, active-rig-only) as the rigs.
- Native `dxr_prov_begin_frame` chains `XrDisplayRigEXT` (display-centric) or
  `XrCameraRigEXT` (camera-centric) exactly like `displayxr_standalone.cpp`. The
  rig pose is converted Unity→OpenXR (negate posZ, negate quat X/Y) in
  `dxr_prov_set_display_pose`; scene scale (`lossyScale`, scale-as-zoom) is folded
  into `virtualDisplayHeight` / `convergenceDiopters` on the C# side, matching the
  standalone (native ignores scale).

### B. Adaptive N-tile rendering contract

- **In-app weave (WS_CHILD overlay):** `GfxStart` calls the hook path's
  `displayxr_get_app_main_view()` (reused from `displayxr_win32.c`) to create a
  `WS_CHILD` overlay sized to Unity's client area + subclass Unity's wndproc, and
  binds the runtime to that HWND (M1 used `windowHandle=NULL` → runtime
  self-hosted). In provider mode the OpenXR hook is not installed, so the provider
  is the sole caller. If the overlay can't be created it falls back to self-host.
- **Worst-case swapchain (ADR-010):** the SPI (`arraySize=2`) swapchain + bridge
  are sized once to `max(viewWidthPixels) × max(viewHeightPixels)` across **all**
  enumerated modes, and never reallocated.
- **Per-frame adaptive render rect:** `dxr_prov_begin_frame` derives
  `render = overlayClient × active-mode scaleXY`, clamped to the worst-case
  swapchain. The provider sets `renderParams[eye].viewportRect` to the normalized
  sub-rect (`render / swapchain`) so Unity renders only that sub-region, and
  submits each eye with `subImage.imageArrayIndex = eye`,
  `imageRect.extent = render` (SPI per-slice sub-region). Re-derived every frame,
  so a live window resize / mode switch needs no reallocation.

### C. Mode enumeration + control + events

- Native exports surface the enumerated modes (`dxr_prov_get_mode_count/info`),
  the active mode, and `xrRequestDisplayRenderingModeEXT` /
  `xrRequestDisplayModeEXT` / `xrRequestEyeTrackingModeEXT` (all soft-resolved —
  inert on an older runtime).
- `dxr_prov_poll_events` handles `XrEventDataRenderingModeChangedEXT` (re-enumerate
  modes + re-derive tiling/resolution live), `…HardwareDisplayStateChangedEXT`,
  and `…EyeTrackingStateChangedEXT`, latching each into atomic read-and-clear
  flags. The driver pumps them into `DisplayXRProvider`'s C# events. Mode
  *keybinding* stays app policy — the plugin only exposes the API. The three event
  structs were added to `displayxr_extensions.h` verbatim from the runtime's
  `XR_EXT_display_info.h`.

### Loader (XR Plug-in Management)

`DisplayXRDisplayLoader : XRLoaderHelper` creates/starts/stops the
`XRDisplaySubsystem` from the `"DisplayXR Display"` descriptor and owns the
driver's lifecycle; `DisplayXRDisplaySettings` (`[XRConfigurationData]`) +
`Editor/Provider/DisplayXRDisplayPackage` (`IXRPackage`) register it as a
**Standalone** XR Plug-in Management toggle so Unity ships the loader +
`UnitySubsystemsManifest.json` automatically — the M1 bootstrap + hand-copy hack
is gone. Display-only (no input subsystem) by design.

## Play Mode runs the provider (#171)

> **Historical (pre-#166 hook-removal).** This section describes the transition when
> three session paths still coexisted. The OpenXR hook and the standalone (SA)
> editor-preview session/window have since been removed — **the provider is the sole
> backend, and pressing Play *is* the preview** (there is no edit-mode preview
> window). The rationale below is retained for context.

At the time there were three DisplayXR session paths: the **OpenXR hook** (legacy
built app), the **provider** (new built app), and the **standalone (SA)** session (the
edit-mode Preview window, and — historically — Play Mode via `PlayModeIntegration`,
which stripped Unity's OpenXR loader and auto-started the SA session).

Because the provider is a real `IUnityXRDisplay` subsystem, **Play Mode can run it
directly** through Unity's standard XR Plug-in Management lifecycle — no loader
stripping, no SA. `DisplayXRPreviewSession.OnPlayModeStateChanged` now checks
`IsProviderLoaderActive()` (is `DisplayXRDisplayLoader` a configured active XR
loader?) and, when true, **stands aside completely** so XR-mgmt brings the provider
up. This makes Play Mode == built app: true parity, provider bugs surface in-editor,
and #172 live tile realloc is testable in Play Mode (resize the Game/Player window).

When the provider is *not* the active loader (OpenXR-hook or SA-only projects), the
legacy `PlayModeIntegration` SA path is unchanged.

**The edit-mode Preview window stays on the SA session and is effectively
deprecated for provider projects.** A Unity XR subsystem's lifecycle is tied to
Play/build, not to an arbitrary `EditorWindow`, so the Preview window *cannot* host
the provider — that is exactly why the SA session exists. The window shows an info
banner (when the provider is the active loader) pointing users to Play Mode for
provider-fidelity preview. The Preview window remains useful for hook/SA workflows
and quick edit-mode iteration; provider projects should prefer Play Mode.

Editor inspectors and the settings page read runtime status through
`DisplayXREditorStatus` (Editor), which unifies display info + eye-tracking across
all three backends (provider → SA preview → hook). This closes gap #6: they
previously read `DisplayXRFeature.Instance.DisplayInfo`, which is null in provider
mode.

## M2 hardware validation (RTX 3080, Leia DP, dev runtime v1.26.1)

Drove a built `displayxr-unity-test` (BiRP, Gamma) player through the provider via
the real `XRLoader` (loader swapped from OpenXR → `DisplayXRDisplayLoader` with
`XRPackageMetadataStore`; the manifest shipped automatically). **Confirmed
working end-to-end:** provider registers (`DisplayXR successfully registered
Provider for DisplayXR Display`), session → **FOCUSED**, mode enumeration (`2D`
1-view + `LeiaSR` 2-view `2×1`, active mode detected), eye-tracking event fires
(`isTracking=1`), `window×scaleXY` adaptive sizing (640×360 from a 1280×720
window), valid per-eye projections (`shouldRender=1`, real FOV), and the runtime
weaves every frame with **zero** device-removed / VIEW-SIZE / CALL_ORDER errors.

Three bugs found + fixed on hardware:

1. **Render-thread overlay deadlock.** Creating the `WS_CHILD` overlay in
   `GfxStart` (render thread) deadlocks — a `WS_CHILD` of Unity's main-thread
   window attaches the two threads' input queues while Unity's main thread is
   blocked waiting for `GfxStart`. **Fix:** create the overlay in `LifecycleStart`
   (main thread); `GfxStart` only binds the stored HWND. (Symptom: white window,
   `GfxStart` never logged.)
2. **Worst-case-swapchain sub-rect → black.** A render sub-rect inside a larger
   worst-case texture mismatches Unity's bottom-left viewport vs the runtime's
   D3D top-left `imageRect`, so the runtime sampled empty texels. **Fix (for now):**
   size the swapchain/bridge to the actual render rect (`window×scaleXY`) so
   render == swapchain (no sub-rect). The worst-case/no-realloc ADR-010
   optimization is deferred until that Y-origin is reconciled + a live resize
   realloc is added.
3. **Cross-device bridge handoff → black.** Unity renders into the shared bridge
   on *its* device and leaves it in `RENDER_TARGET`; the provider's separate
   device read it incoherently. **Fix:** a SHARED cross-device fence (Unity's
   queue signals after its render; the own-device copy GPU-waits before copying)
   **plus** `IUnityGraphicsD3D12v8::RequestResourceState(bridge, COMMON)` so Unity
   transitions the bridge to a cross-device-readable state. This is what carried
   Unity's pixels through (black → the bridge now reflects Unity's output).

**Decisive proof the architecture works:** Unity's eye color-texture pointer
(`XRTextureManager::SetupRenderTextureFromXRRequest col:`) == the provider's
bridge Unity-side pointer — Unity renders its scene *directly into our bridge*.

**Open item — uniform white instead of the scene.** With the handoff fixed the
woven output is uniform white rather than the skybox+cube. Color-space is ruled
out (the test project is **Gamma**, so the `R8G8B8A8_UNORM` bridge is correct;
toggling `kUnityXRRenderTextureFlagsSRGB` changed nothing). This is a
render-correctness detail — *what Unity draws into the eye texture* — to diagnose
with the Unity frame debugger. Likely-next probe: confirm Unity draws geometry
(not just a clear) into the bridge, and compare against the standalone/preview
path (which has Unity render to its own RT and `Graphics.CopyTexture` into the
bridge — a known-good content path the provider could adopt if the direct
external-RT render proves problematic).

**Weave target — the app owns its window (default).** A Unity DisplayXR app owns
its window like a native handle app: the provider creates a **top-level `WS_POPUP`
overlay** over Unity's client area (on the main thread in `LifecycleStart`) that
tracks the app window's move/resize, and binds the runtime to it, so the runtime
weaves into the app's own window (single-window UX). Keyboard/mouse route to Unity
via the focus hook (`displayxr_install_focus_hook`; the Input System reads RawInput,
delivered only to the foreground window). This is the **default and only supported
shipping model**. A top-level popup is used (not `WS_CHILD`) because a child window
doesn't composite the runtime's D3D12 DComp flip swapchain; the popup does.

**Self-host is a diagnostic fallback, not an app mode.** `DISPLAYXR_PROV_SELFHOST=1`
makes the runtime host its own window (`windowHandle=NULL`) — the M1b bring-up
baseline. It is **not** a deployment mode: it produces a two-window experience,
leaves Unity's window non-foreground (so keyboard doesn't reach the Input System),
and has no real window geometry (the tracking origin floats to standing height).
Use it only for bring-up/diagnostics. If the app-owned overlay HWND fails to
create, GfxStart also falls back to self-host as a safety net.

## Porting hook-path features to provider mode — the inert-`DisplayXRFeature` rule

> **Rule of thumb:** in provider mode there is **no OpenXR API-layer hook and no
> Unity OpenXR loader**, so **any C# that reaches the runtime through
> `DisplayXRFeature` is dead code.** `DisplayXRFeature.Instance` is `null` and its
> `GetStereoMatrices` / lifecycle callbacks never fire. Before assuming a hook-path
> feature "just works" under the provider, check what it reads — if it touches
> `DisplayXRFeature`, `OpenXRRuntime.IsExtensionEnabled`, or any Unity-OpenXR API,
> it needs a **provider branch** gated on `DisplayXRProviderDriver.IsActive` that
> reads the equivalent **native shared state** instead. This has bitten us
> repeatedly; treat it as the default failure mode when a ported feature silently
> no-ops.

The provider publishes the state the hook used to route through `DisplayXRFeature`
into the same native shared-state exports the hook populated, so the C# side only
needs to change *where it reads from*, not the downstream logic:

| Hook-path source (inert under provider) | Provider-mode source | Consumers that needed a branch |
|---|---|---|
| `DisplayXRFeature.Instance.GetStereoMatrices(...)` | `displayxr_get_stereo_matrices` (provider-populated via `ps_publish_stereo_matrices`) | `DisplayXRTransparentOverlay` silhouette/hit-test (URP eye_world view+proj — else the click-through silhouette truncates popped-out geometry, bc001ce). *(The former URP `KooimaProjectionFixFeature` also read this; it was removed in v2.2.0 once the provider began handing Unity a full projection matrix — see #22.)* |
| hook `LateUpdate` push of `dxr_set_tunables` / display pose | `DisplayXRProviderDriver` per-frame `dxr_prov_set_tunables` / `dxr_prov_set_display_pose` | display/camera rig tunables |
| `OpenXRRuntime.IsExtensionEnabled("XR_EXT_local_3d_zone")` gate | `DisplayXRProviderDriver.IsActive` | `DisplayXRLocal2D` bridge branch |
| `SetEnvironmentBlendMode(AlphaBlend)` on the OpenXR feature | `dxr_prov_set_transparent_background` | `DisplayXRTransparentOverlay` |
| foreground-clip globals from `GetStereoMatrices` (rig URP branch) | `dxr_prov_get_eye_clip` published in `DisplayXRDisplay.PublishProviderForegroundClip` | `DisplayXR/ForegroundClipURP` |

**When adding or auditing a hook-path C# component for provider mode:** grep it for
`DisplayXRFeature`, `OpenXRRuntime`, `OpenXRSettings`, and `GetStereoMatrices`. Each
hit is a spot that needs an `if (DisplayXRProviderDriver.IsActive)` branch reading
the native export. This is the substance of "Phase C" gating cleanup.

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
6. **Window binding.** ✅ **Done (M2).** The provider creates a `WS_CHILD` overlay
   over Unity's window (`displayxr_get_app_main_view`, reused from the hook path)
   and binds the runtime to it — in-app weave, with per-view resolution tracking
   Unity's live client size. Validate that the overlay creates correctly when
   `GfxStart` runs on the render thread (it falls back to runtime self-host if not).

## Validation runbook (user-run)

All in a **non-elevated** terminal (the OpenXR loader ignores `XR_RUNTIME_JSON`
when elevated).

1. **Runtime, hardware-free:** in `displayxr-runtime`, register the sim DP once
   (elevated, one-time): `scripts\register_dev_plugin.bat`; then
   `set SIM_DISPLAY_OUTPUT=sbs`; sanity-check `displayxr-cli selftest`. Point
   `XR_RUNTIME_JSON` at the dev runtime JSON.
2. **Import the plugin** into `displayxr-unity-test` (BiRP) — the freshly built
   `Runtime/Plugins/Windows/x64/displayxr_unity.dll` ships with the package. In
   **Project Settings > XR Plug-in Management (Standalone)**: **enable "DisplayXR
   Display"** and **disable Unity's OpenXR** (the provider replaces it). Remove the
   M1 `[RuntimeInitializeOnLoadMethod]` bootstrap + manifest hand-copy — the
   XRLoader ships the manifest now.
3. **Run / build a player.** Confirm in `Player.log` / `%TEMP%\displayxr_prov_native.log`:
   `RegisterLifecycleProvider OK` → `Lifecycle Initialize` → `GfxStart OK` →
   worst-case `SPI swapchain: …arraySize=2` → `session state: …FOCUSED`. With a
   tracked face, eye0≠eye1 separation tracks head motion; resize the window and
   confirm the per-view render rect adapts (no `VIEW SIZE MISMATCH`).
4. **SPI active:** confirm Unity renders **1 instanced pass** to a 2-slice array
   (frame debugger / `XRSettings`), not 2 passes.
5. **Weave:** capture the atlas via the runtime file trigger —
   `rm %LOCALAPPDATA%\Temp\workspace_screenshot.png` then
   `touch %LOCALAPPDATA%\Temp\workspace_screenshot_trigger` → read the PNG.
6. **URP:** repeat in `displayxr-unity-test-2d-ui`.
7. **Panel (optional):** register the Leia plugin and confirm weave on hardware.
