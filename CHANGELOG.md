# Changelog

All notable changes to the DisplayXR Unity plugin will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.2.0] - 2026-05-04

### Added
- **Cross-process click-through finally works for the transparent avatar** (#57). Click anywhere through the avatar's transparent halo and the click reaches the actual deepest control under the cursor — verified end-to-end with Notepad (`RichEditD2DPT`) and Explorer (`DirectUIHWND`). Activation transfers cleanly: the underlying app gains foreground / focus, caret appears, keystrokes land. Implementation: the overlay catches every click via `WM_NCHITTEST`/HTCLIENT (Approach C), `forward_click_to_underlying_window` does iterate-top-level + `ChildWindowFromPointEx`-recursive-descend to find the deepest non-transparent leaf, then `SetForegroundWindow` on the top-level frame and `PostMessage` to the leaf with the leaf's client coordinates.
- **Mouse-wheel scroll resizes the overlay window when in focus.** Uniform scaling around the current center, 10% per WHEEL_DELTA notch, floor at 400×400. Win32's "wheel goes to focused window" routing means scroll naturally goes to whichever app you've click-through'd to (Notepad scrolls its document, etc.) — no explicit foreground gate needed.
- **Foreground-aware input gating** for `DisplayXRInputController`. Exposed `displayxr_is_our_process_foreground()` (calls real OS `GetForegroundWindow` from the plugin DLL whose IAT isn't patched). `Update()` early-returns when not foreground so WASD doesn't move the cube while the user is typing in Notepad. Cube reclaims foreground via `SetForegroundWindow(overlay)` from the wndproc on cube-press. Custom input scripts should call `DisplayXRNative.displayxr_is_our_process_foreground()` for the same gate.
- `displayxr_get_overlay_size()` getter so C# raycast / hit-rect math uses the overlay's actual client size (which scroll-resize can change) rather than `Screen.width/height` (Unity's frozen off-screen HWND).
- Diagnostic instrumentation kept in main: `WH_MOUSE_LL` global mouse hook (button-only) logs `WindowFromPoint` resolution per click to `displayxr.log`, and `overlay_wnd_proc` logs every button-event entry with the live `WS_EX_TRANSPARENT` bit. Cheap, lifetime-of-process; was indispensable for diagnosing the foreground-transfer bug.

### Fixed
- Cyclopean Kooima raycast now uses the overlay's actual client size (via `displayxr_get_overlay_size`) for cursor → NDC conversion. Previously `Screen.width`/`Screen.height` returned Unity's off-screen HWND dimensions, which don't track scroll-resize — every click inside a resized overlay registered as `hit_active=0` and "passed through" the cube.

## [1.1.1] - 2026-04-30

### Fixed
- **Package import warnings**: Renamed `docs/` to `docs~/` so Unity's package importer skips the folder entirely (UPM convention for "ignored" folders). Previously, every markdown file under `docs/` produced a `"has no meta file, but it's in an immutable folder"` warning when the package was installed via Package Manager. Drops the orphan `docs.meta` and `docs/quick-start-guide.md.meta` that a previous editor session created. Internal cross-references in the architecture / ADR / roadmap docs updated to the new path.

## [1.1.0] - 2026-04-30

### Added
- **Atlas screenshot capture** — press `I` (or call `DisplayXRScreenshot.Capture()`) to save the multi-view atlas the app wrote to the swapchain as a PNG to `Pictures/DisplayXR/<app>-N_NxM.png`. Mirrors the C++ test app and Unreal plugin convention. Brief white flash on capture for visual feedback. Two paths: editor SA preview reads the existing atlas RT before submit; built standalone re-renders the active rig camera's L/R Kooima views via a hidden capture camera, with a CommandBuffer-driven flash on every registered rig camera so it lands in the OpenXR swapchain.
- **`.displayxr.json` app manifest sidecar** generated next to the built executable on build (#51). Optional `Register with DisplayXR` mode (#54) also writes to `%LOCALAPPDATA%\DisplayXR\apps\` so the DisplayXR Shell discovers the build without it living under Program Files.
- **`Window > DisplayXR > Manifest Settings`** menu shortcut.
- **`Hidden/DisplayXRFlash` shader** (Runtime/Resources/) used by the on-demand flash overlay.
- Pin built Unity Player to the dGPU on hybrid laptops via `NvOptimusEnablement` / `AmdPowerXpressRequestHighPerformance` exports.

### Fixed
- **Gamma color space double-darkening** in built apps on D3D11 and D3D12. The `xrCreateSwapchain` hook now downgrades sRGB color formats (29 → 28, 91 → 87) for Unity Gamma projects so already-gamma-encoded shader output lands without re-encoding. Linear projects keep sRGB. C# tells native via a new `displayxr_set_use_srgb_swapchain` setter at `OnInstanceCreate`.
- `KeyCode.I` was missing from the new Input System mapping in `DisplayXRInputController`, throwing `ArgumentOutOfRangeException` and aborting Update before any later handlers ran.
- Asmdef `.meta` importer type set to `AssemblyDefinitionImporter` (was `DefaultImporter`).
- Built-app capture PNG was Y-flipped because the SA path's projection-Y flip isn't applied in the on-demand path; the Y-flip blit is now opt-in per path.

### Changed
- **UGUI is now an optional dependency** of the runtime assembly — no hard compile-time UI module requirement.
- `FindObjectsByType` call updated to drop the deprecated `FindObjectsSortMode` argument (Unity 2023+ deprecation).

## [1.0.0] - 2026-04-11

First stable release of the DisplayXR Unity plugin. Headline changes: standalone
editor preview with native HWND + input forwarding, D3D11 hooked path
generalized to N-view tile atlas, GitHub org transfer to DisplayXR, and full
documentation structure with ADRs.

### Added
- **Standalone editor preview window** — native HWND on Windows with D3D11 atlas
  bridge, D3D12 blit, and input suppression (replaces the earlier IOSurface
  approach). Works in both Play Mode and Edit Mode.
- **Edit Mode preview** — live composited 3D output without entering Play Mode
- **Game View eye tile atlas** displayed during Play Mode for debugging
- **D3D11 hooked path**: generalized SBS composite to N-view tile atlas (#91)
- **D3D11 typed swapchain substitution** for the hooked path (#91)
- **Input forwarding** from preview window to Unity — mouse events, focus-aware
  handling, camera rotation support
- **Documentation structure**: ADRs, architecture docs, navigation
- **`/release` skill** for tagged release orchestration
- **Shell mode**: full input forwarding from main HWND (#43, #44, #45)

### Changed
- Repo references updated from `dfattal/openxr-3d-display` to
  `DisplayXR/displayxr-runtime-pvt` following GitHub org transfer
- CI triggers restricted to PR validation and tag pushes — no more triggers on
  main branch (devs use local builds for daily iteration)
- Shell mode: Kooima viewport updates on window resize/move (#46)
- Game View camera rendering suppressed during editor Play Mode (preview
  window takes over)

### Fixed
- macOS build: gate `xrDestroySwapchain` dispatch on `_WIN32` (#91)
- Preview window: Play Mode startup, input handling, weaving, Y-flip
- Play Mode auto-start and camera selection reliability
- Camera selection after Play Mode domain reload
- Camera rotation during preview window move/resize/drag (multiple fixes)
- Closing preview window now correctly exits Play Mode
- Preview tab: crop atlas to content region and fix Y-flip
- Crash on second Play Mode entry: execute deferred session/instance destroy
- Crash on Play Mode exit: defer preview window destruction, destroy preview
  before XR teardown
- `EntryPointNotFoundException` for `window_was_closed` handled gracefully
- Preview window content frozen during live resize

## [0.7.0] - 2026-03-31

### Fixed
- Fix window-relative Kooima projection not responding to window drag (#41)
  - Added `WM_MOVE` handler so viewport position updates when the window is
    dragged, not just on resize (`WM_SIZE`)
  - Set initial viewport position on overlay creation so projection is correct
    from frame 1
  - Re-added viewport-change diagnostic logging for verification

## [0.6.5] - 2026-03-30

### Fixed
- Restore child window overlay for built apps (dfattal/openxr-3d-display#107)
  - Reverts top-level HWND pass-through which caused D3D12 swapchain conflict
    (`E_ACCESSDENIED`) because Unity already owns the swapchain on that window
  - Child window gives the runtime its own HWND for presentation

### Added
- Local Windows MSVC build script `native~/build-win.bat` (#42)

## [0.6.3] - 2026-03-27

### Changed
- Remove Kooima diagnostic logs — window-relative projection verified (#41)
  - All diagnostic logging for Kooima projection parameters has been removed
    now that window-relative projection is confirmed working correctly

## [0.6.2] - 2026-03-27

### Changed
- Log Kooima params only on viewport resize instead of every 60 frames (#41)
  - Reduces log noise by triggering diagnostic output only when the viewport
    dimensions actually change

## [0.6.1] - 2026-03-27

### Added
- Throttled Kooima diagnostic logs for window-relative projection (#41)
  - Native hooks and standalone code now log key projection parameters
    at reduced frequency for easier debugging without log spam

## [0.6.0] - 2026-03-27

### Changed
- Bump version to 0.6.0 — milestone cleanup for Game View overlay and
  window-relative Kooima projection (#41)

### Fixed
- Move diagnostic label back to top-left of Game View overlay (#41)

## [0.5.9] - 2026-03-27

### Changed
- Window-relative Kooima projection: replace viewport-scale factor with actual
  window physical dimensions and window-center eye offset (ADR-012) (#41)
- Native WM_SIZE handler now captures HWND screen position via ClientToScreen
  for correct off-center window perspective on Windows

## [0.5.8] - 2026-03-27

### Fixed
- Center diagnostic text in Game View for visibility at all DPI (#41)

## [0.5.7] - 2026-03-27

### Fixed
- Revert UV to canvas/surface crop — weaver respects viewport (#41)
  - UV=1.0 test confirmed: weaver writes to dp_target viewport (2203x1147),
    NOT the full 3840x2160 surface. Content is at bottom-left in UV space.
  - Restoring canvas/surface UV crop which samples that exact region.

## [0.5.6] - 2026-03-27

### Changed
- Test: use full UV range (1.0) for shared texture sampling (#41)
  - If the weaver renders to the full 3840x2160 shared texture (ignoring viewport),
    the previous UV crop clipped to only the canvas portion, showing a zoomed/cropped view
  - Testing with UV=1.0 to confirm whether full-texture sampling resolves this

## [0.5.5] - 2026-03-27

### Fixed
- Revert canvas to physical pixels — weaver needs physical px precision (#41)
  - Screen.width/height gives logical pixels; multiplying by backingScale gives physical
  - The weaver must output at physical resolution for correct lenticular interlacing

## [0.5.4] - 2026-03-27

### Fixed
- Use Screen.width/height directly for canvas size in Play Mode (#41)
  - Unity Game View has pixelsPerPoint=1.0, so the backbuffer is at logical resolution
  - Multiplying by backingScale (2.5) sent oversized dimensions to the weaver
  - Now sends Screen.width/height directly, matching actual Game View size

## [0.5.3] - 2026-03-27

### Added
- Comprehensive texture size diagnostics for weaving debug (#41)
  - Native: log GetDpiForSystem, atlas size, display size in set_canvas_rect
  - C#: show canvas, shared tex, UV, Screen, backingScale, pixelsPerPoint, drawRect logical+physical, mode, camera in Game View overlay

## [0.5.2] - 2026-03-26

### Fixed
- Skip `GL.invertCulling` on Windows D3D12 (#41)
  - Without the projection Y-flip on D3D12, only the view Z-flip affects winding
  - Normal culling (no inversion) is correct on Windows, fixing inside-out faces

## [0.5.1] - 2026-03-26

### Fixed
- Make projection Y-flip macOS-only — D3D12 native memory doesn't need it (#41)
  - The Y-flip was for Metal RenderTexture convention; on D3D12, removing it produces right-side-up content matching the reference test app
  - The weaver now receives correctly oriented atlas content on Windows

## [0.5.0] - 2026-03-25

### Fixed
- Set `FilterMode.Point` on shared texture to preserve interlacing (#41)
  - Bilinear filtering interpolates between rows, destroying the per-row interlacing pattern from the Leia SR weaver
  - Point filtering preserves exact pixel values for correct lenticular 3D output

## [0.4.9] - 2026-03-25

### Fixed
- Set Per-Monitor DPI Awareness V2 before runtime init for correct weaving (#41)
  - `SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)` ensures `GetClientRect` returns physical pixels
  - Fixes Leia SR weaver interlacing mismatch on DPI-scaled Windows displays

## [0.4.8] - 2026-03-25

### Fixed
- Revert display Y-flip (separate issue), add canvas/texture size diagnostics (#41)
  - Native `set_canvas_rect` now logs exact pixel values sent to runtime
  - Game View overlay shows canvas/surface/UV/screen/scale/draw sizes for debugging

## [0.4.7] - 2026-03-25

### Fixed
- Revert native D3D12 blit to simple copy, flip at display via UV coords (#41)
  - Atlas content is Y-flipped (Unity D3D12 convention), weaver interlaces it Y-flipped, and the display flips it back via Rect(0, vMax, uMax, -vMax)
  - All three flips are consistent — no native row-by-row flip needed

## [0.4.6] - 2026-03-25

### Fixed
- D3D12 atlas blit: row-by-row Y-flip copy for correct weaver orientation (#41)
  - Unity RenderTextures on D3D12 store content Y-flipped in native memory
  - Copy each row to the reversed Y position in the swapchain image so the weaver receives correctly oriented content for lenticular interlacing

## [0.4.5] - 2026-03-25

### Fixed
- Revert to Graphics.CopyTexture — Graphics.Blit Y-flip broke rendering (#41)
  - Reverts to the working v0.4.2 atlas copy path
  - Y-flip will be handled separately (native D3D12 blit or projection matrix)

## [0.4.4] - 2026-03-25

### Fixed
- Simplify Y-flip blit: remove GL matrix manipulation that may have broken D3D12 (#41)

## [0.4.3] - 2026-03-25

### Fixed
- Fix D3D12 Y-flip: blit atlas with vertical flip before copying to bridge texture (#41)
  - Unity RenderTextures on D3D12 are Y-flipped in native memory
  - Use `Graphics.Blit` with `scale(1,-1)` to flip before bridge copy

## [0.4.2] - 2026-03-25

### Fixed
- Revert canvas rect to physical pixels — runtime uses them as GPU viewport dims (#41)

## [0.4.1] - 2026-03-25

### Fixed
- Fix canvas rect DPI: send logical pixels on Windows, backing pixels on macOS (#41)
  - `xrSetSharedTextureOutputRectEXT` takes HWND client-area pixels per spec
  - On DPI-aware Windows (Unity 6), `Screen.width` is already logical pixels
  - On macOS, `Screen.width` is in points — multiply by backing scale factor

## [0.4.0] - 2026-03-25

### Fixed
- Fix shared texture format: use R8G8B8A8_UNORM / RGBA32 to match runtime weaver PSO format (#38)
  - Runtime hardcodes DXGI_FORMAT_R8G8B8A8_UNORM (28) for the weaver; our shared texture was B8G8R8A8_UNORM (87)
  - Format mismatch caused weaver to silently no-op
  - Updated native standalone, preview session, preview window, and game view overlay

## [0.3.9] - 2026-03-25

### Changed
- Pass Unity's HWND to Win32 window binding for standalone preview — required by Leia weaver for correct window targeting (#38)

## [0.3.8] - 2026-03-25

### Fixed
- Fix Windows DPI scaling for canvas rect: `get_backing_scale_factor` now returns system DPI / 96 instead of hardcoded 1.0 (#38)

## [0.3.7] - 2026-03-25

### Fixed
- Fix atlas RT format: use BGRA32 to match bridge texture format for `Graphics.CopyTexture` compatibility (#38)

## [0.3.6] - 2026-03-25

### Fixed
- Fix preview window not opening from menu (#40): null-guard custom editors to prevent `SerializedObjectNotCreatableException` during domain reload
- D3D11 TYPELESS swapchain textures: replace proxy texture copy with thin COM wrapper that overrides `GetDesc()` to report concrete format — zero-copy, no extra textures (#36)

## [0.3.5] - 2026-03-25

### Changed
- Cross-device atlas blit via DXGI shared bridge texture (#38)
  - Unity renders atlas on its D3D12 device, then `Graphics.CopyTexture` to a bridge texture shared on both devices
  - `CopyTextureRegion` from bridge to swapchain on the runtime's device
  - Completes the cross-device rendering pipeline started in 0.3.4

## [0.3.4] - 2026-03-25

### Changed
- D3D12: use separate device for runtime session, shared texture via DXGI handle for Unity (#35)
  - Sharing Unity's D3D12 device with the runtime caused device removal
  - Create dedicated D3D12 device for the runtime OpenXR session
  - Use `OpenSharedHandle` on Unity's device for `CreateExternalTexture`
  - Atlas blit skipped (cross-device TODO)

## [0.3.3] - 2026-03-25

### Fixed
- D3D12 atlas blit: remove explicit resource barriers, rely on implicit COMMON state promotion for cross-queue copy operations (#35)

## [0.3.2] - 2026-03-25

### Fixed
- D3D12 OpenXR struct type IDs: corrected from `1000027xxx` to `1000028xxx` (`XR_TYPE_GRAPHICS_BINDING_D3D12_KHR`, `XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR`, `XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR`) (#35)

## [0.3.1] - 2026-03-25

### Fixed
- Crash in `set_unity_device` on Windows D3D12: validate `ID3D12Resource` via `QueryInterface` before calling `GetDevice`, preventing access violation when Unity passes a non-D3D12 resource (#35)

## [0.3.0] - 2026-03-25

### Changed
- Migrate Windows standalone preview from D3D11 to D3D12 (#35)
  - Replace D3D11 device/context with D3D12 device/queue/command list/fence
  - D3D12 shared texture via `CreateCommittedResource` + `D3D12_HEAP_FLAG_SHARED`
  - Atlas blit with D3D12 command list, resource barriers, and fence sync
  - `XrGraphicsBindingD3D12KHR` for session creation
  - Platform-conditional Y-flip (Metal vs D3D12)
  - Supports both D3D11 and D3D12 Unity graphics backends

## [0.2.2] - 2026-03-24

### Fixed
- CS0104 ambiguous `Object` reference in `DisplayXRPreviewSession.cs` — qualify as `UnityEngine.Object` to resolve conflict with `System.Object` (#35)

## [0.2.1] - 2026-03-24

### Fixed
- Null texture in `set_unity_device`: force `RenderTexture.Create()` before `GetNativeTexturePtr()` to ensure GPU resource is allocated (#35)

## [0.2.0] - 2026-03-25

### Fixed
- Windows standalone preview: use Unity's own D3D11 device instead of creating a separate one, fixing cross-device TDR crashes when sharing textures between devices (#35)

## [0.1.9] - 2026-03-25

### Fixed
- Revert shared texture to B8G8R8A8_UNORM — runtime rejects TYPELESS format (`xrCreateSession` fails with -6). The C# `linear=true` flag is sufficient for correct gamma handling.

## [0.1.8] - 2026-03-24

### Fixed
- D3D11 shared texture compatibility: use TYPELESS format with linear SRV to avoid gamma/format mismatch issues in the standalone preview rendering pipeline

## [0.1.7] - 2026-03-24

### Added
- Windows standalone preview: D3D11 swapchain image acquisition, atlas blit from shared texture, and `xrEndFrame` submission — completes the Windows standalone preview rendering pipeline

## [0.1.6] - 2026-03-24

### Fixed
- Windows crash: `displayxr_standalone_get_shared_texture` now returns `ID3D11Texture2D*` (what Unity's `CreateExternalTexture` expects) instead of the DXGI shared `HANDLE` (which is for cross-device sharing with the runtime)

## [0.1.5] - 2026-03-24

### Added
- Windows standalone preview: D3D11 shared texture creation and DXGI handle passing to runtime via Win32 window binding — enables zero-copy GPU texture sharing for preview output

## [0.1.4] - 2026-03-23

### Fixed
- Windows standalone preview: create D3D11 device with correct adapter LUID and pass `XrGraphicsBindingD3D11KHR` to session creation (fixes `xrCreateSession` error -38)

## [0.1.3] - 2026-03-23

### Added
- Windows standalone preview: implement `LoadLibrary`/`GetProcAddress` runtime loading and Win32 window binding for session creation — standalone preview now starts on Windows

## [0.1.2] - 2026-03-23

### Fixed
- Windows DLL plugin settings: enable Editor platform so standalone preview and Play Mode can load `displayxr_unity.dll` in the Windows editor

## [0.1.1] - 2026-03-23

### Fixed
- Standalone preview now discovers the runtime via Windows registry (`Khronos\OpenXR\1\ActiveRuntime`) when `XR_RUNTIME_JSON` is not set
- Settings page shows runtime discovery source (env var vs registry)
- UPM git URL install: added `.gitattributes` to prevent binary corruption, documented Git prerequisites for Windows and macOS
- Quick-start guide updated with git URL as primary install method

## [0.1.0] - 2026-03-23

### Added
- Initial release as standalone UPM package (moved from `openxr-3d-display` runtime repo)
- OpenXR Feature lifecycle (`DisplayXRFeature`) with native hook chain
- Camera-centric stereo rig (`DisplayXRCamera`) for retrofitting existing scenes
- Display-centric stereo rig (`DisplayXRDisplay`) for virtual display placement
- Kooima asymmetric frustum projection via native plugin (display3d + camera3d libraries)
- Eye tracking integration through OpenXR extensions
- Stereo tunables: IPD factor, parallax factor, perspective factor, inverse convergence distance
- 2D UI overlay component (`DisplayXRWindowSpaceUI`) for HUDs and menus
- Standalone editor preview window with camera selector, rendering mode controls, and zero-copy GPU texture sharing (IOSurface/DXGI)
- Game View overlay (`DisplayXRGameViewOverlay`) for Play Mode shared texture output
- Canvas-aware shared texture cropping via `xrSetSharedTextureOutputRectEXT`
- Custom inspectors for camera-centric and display-centric modes
- Project Settings page showing runtime status and display info
- Native plugin source (`native~/`) with independent CMake build
- CI workflow for Windows (MSVC) and macOS (Universal) native builds
- Cross-platform support: Windows x64 and macOS
- Cross-compilation support: build Windows target from macOS editor
- `.gitattributes` for binary file protection
- Quick start guide and comprehensive README with troubleshooting
