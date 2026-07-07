# macOS/Metal provider recon — gap audit for the parity epic (#202)

*2026-07-07. Verified from source: this repo at v2.3.2 and `displayxr-runtime` at v1.28.0
(`c23ae821a`). This is the Phase 0 deliverable of the macOS parity epic; the Unity-side
Metal XR probe is deferred to Phase 2 first light (it needs a compiling provider first).*

## TL;DR

The runtime's macOS build is **already provider-ready** — every extension the provider
needs is advertised on macOS, including `XR_EXT_view_rig` at SPEC_VERSION 3 and the
Metal graphics binding. The gap is entirely plugin-side: the provider is compiled only
under `if(WIN32)` and the shipped macOS bundle exports zero `dxr_prov_*` symbols, so
the C# loader throws `EntryPointNotFoundException` before subsystem creation. The
Windows coupling in the provider TUs is via unconditional includes and direct Win32/D3D
calls (not `#ifdef`s), concentrated in well-mapped chunks; the OpenXR/view-rig/projection
core is backend-agnostic and ports unchanged.

## Extension × platform table (runtime `oxr_extension_support.h`)

| Extension | SPEC_VERSION | Windows | macOS |
|---|---|---|---|
| `XR_KHR_metal_enable` (+ `XR_KHRX2_metal_enable` alias) | 2 | n/a | **yes** |
| `XR_EXT_view_rig` | **3** (provider needs ≥2) | yes | **yes** (no platform gate) |
| `XR_EXT_display_info` | 15 | yes | yes |
| `XR_EXT_atlas_capture` | 3 | yes | yes |
| `XR_EXT_display_zones` | 1 | yes | yes |
| `XR_EXT_local_3d_zone` | 4 | yes | yes |
| `XR_EXT_cocoa_window_binding` | 6 (incl. `transparentBackgroundEnabled`) | n/a | yes |
| `XR_EXT_win32_window_binding` | — | yes | n/a |
| `XR_EXT_weave` | — | yes | **no — Win32-gated** |
| `XR_EXT_workspace_file_dialog` | — | yes | no — Win32-gated |
| `XR_EXT_mcp_tools` | — | yes | yes |

Local2D and the window-space-UI HUD are **compositor-layer features, not separate
extensions** — they ride the standard layer path, and the runtime's Metal compositor
already has the Local2D flatten pipeline (`oxr_session_gfx_metal_native.c`).

## Runtime compositor / window model on macOS

- **In-process, client-owned queue** — the exact analog of the Windows model. The app's
  `id<MTLCommandQueue>` from `XrGraphicsBindingMetalKHR` (non-NULL required, verified by
  `oxr_verify.c`) is passed straight into `comp_metal_compositor_create`. The Metal
  native compositor is disabled in IPC/service mode, so the shipping macOS path is
  non-IPC.
- **Window model** (`comp_metal_compositor.m`): cocoa window binding with
  `window_handle == NULL` → runtime creates and owns an NSWindow + CAMetalLayer
  (titled/closable/resizable); a non-NULL `NSView*` → runtime renders into the app's
  view. Transparent background (non-opaque NSWindow + layer) supported via the binding.
  This maps 1:1 onto the provider's existing three weave-target modes (dedicated /
  app-owned / self-host) — self-host is the zero-code bring-up mode.
- `xrGetMetalGraphicsRequirementsKHR` returns the runtime's preferred `MTLDevice`
  (system default). On Apple Silicon this always matches Unity's device.

## Plugin-side gaps (all verified)

1. **Provider not compiled on macOS**: `displayxr_provider_session.cpp`,
   `displayxr_display_provider.cpp`, and `displayxr_unity_plugin.cpp` (UnityPluginLoad +
   provider registration) are inside `if(WIN32)` in `native~/CMakeLists.txt`. The macOS
   bundle today contains only shared state, wsui/local2d setters, and hook-era
   Metal/NSWindow helpers — no session, no frame loop.
2. **`unity_pluginapi/` include dir is WIN32-only** in CMake, and there is **no
   `IUnityGraphicsMetal.h`** vendored (`IUnityGraphics.h` does define
   `kUnityGfxRendererMetal = 16`).
3. **C# throws before subsystem creation**: `DisplayXRDisplayLoader.Initialize()` calls
   `dxr_prov_set_single_pass` unguarded; all 34 P/Invokes in
   `DisplayXRProviderNative.cs` are absent from the macOS bundle. The XR-Management
   toggle *is* offered on macOS (`BuildTargetGroup.Standalone`).
4. **Deploy plumbing is already macOS-complete**: `DisplayXRProviderRuntimeDeploy`
   copies the bundle → `Contents/PlugIns/` and the subsystems manifest →
   `Contents/Resources/Data/UnitySubsystems/DisplayXR/`; `DisplayXRBuildProcessor`
   ships `openxr_loader.dylib`. Needs an end-to-end test in Phase 2, not new code.

## Corrections to prior assumptions

- **No IOSurface helpers exist.** The `displayxr_metal.h` "IOSurface helper" comment is
  stale — there is no IOSurface-backed texture creation and no `MTLSharedEvent` code
  anywhere in the repo. Any own-device bridge would be new code. (The IOSurface
  framework is linked, unused.)
- **`XR_EXT_view_rig` is at SPEC_VERSION 3** (the epic notes said "needs 2") and is
  advertised unconditionally — no runtime work needed for macOS Kooima.
- The SA-era Metal backend (`displayxr_standalone_metal_backend.cpp`) never needed a
  bridge: all its cross-device methods are stubs, because Metal's unified device model
  made same-device sharing sufficient. This informs the Phase 2 design (own *queue*,
  not own *device*).

## Reusable dormant assets

| Asset | State | Reuse |
|---|---|---|
| `displayxr_standalone_metal_backend.cpp` | on disk, uncompiled | `xrGetMetalGraphicsRequirementsKHR` check + `XrSwapchainImageMetalKHR` enumeration patterns |
| `displayxr_standalone_internal.h:120-148` | on disk, uncompiled | Metal KHR hand-defs (structtypes 1000029000-2) — to be promoted into `displayxr_extensions.h` (OpenXR SDK 1.0.34 headers lack them even with `XR_USE_GRAPHICS_API_METAL`) |
| `displayxr_standalone_metal.m` | on disk, uncompiled | format-converting blit (`displayxr_sa_metal_blit`), device/queue creation |
| `displayxr_metal.m` | **compiled today** | same-format blit taking an explicit queue; `displayxr_get_app_main_view()` passthrough CAMetalLayer NSView (the Phase 2 in-app weave target) |
| `displayxr_macos.mm` | **compiled today** | transparent NSWindow config + drag helpers (Phase 4 overlay) |
| `displayxr_standalone.cpp:1014-1060` | on disk, uncompiled | dlopen + `xrNegotiateLoaderRuntimeInterface` runtime load — lifted into the provider's macOS arm |

## Decided policy target (acceptance baseline for the epic)

- **Render path**: MultiPass for all pipelines on macOS initially. First light uses a
  blit (two per-eye `MTLTexture`s → `copyFromTexture:...destinationSlice:` into the
  arraySize=2 swapchain), then zero-copy per-eye slice views
  (`newTextureViewWithPixelFormat:...slices:`) as a fast-follow within Phase 2. SPI
  (whole-array wrap, `textureArrayLength=2`) is a Phase 3 experiment — if current
  Unity 6 Metal XR accepts it, macOS URP/HDRP flip to SPI to match Windows; the old
  "SPI broken on Metal" finding predates the provider and must be re-probed, not
  assumed.
- **Session queue**: provider-created `MTLCommandQueue` on Unity's `MTLDevice`
  (IUnityGraphicsMetal exposes no queue accessor; an own queue is deterministic at
  GfxStart and is itself the editor-contention mitigation — Metal queues are
  independent, there is no Optimus/DXGI shared-context analog). Contingency if a real
  contention hazard appears: own `MTLDevice` + IOSurface textures + `MTLSharedEvent`
  (documented, not built; the sync event is `MTLSharedEvent` from day one to keep the
  door open).
- **Sync**: coarse `waitUntilCompleted` for first light → `MTLSharedEvent`
  (signal on Unity's `CurrentCommandBuffer()`, wait on the session queue), mirroring
  the proven D3D11 shared-fence dance.
- **Window**: runtime self-hosted NSWindow first (`viewHandle=NULL`, the existing
  SELFHOST shape), then the in-app `displayxr_get_app_main_view()` NSView.
- **Windows stays byte-identical**: wrap-only `#ifdef` edits in the shared TUs, all
  Metal code in a new APPLE-only ObjC++ TU, `build-win.sh` (MinGW) gate on every step.
