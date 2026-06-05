# Changelog

All notable changes to the DisplayXR Unity plugin will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Changed
- Atlas screenshot filenames adopt the runtime-owned suffix `<stem>-<N>_atlas_<viewCount>_<cols>x<rows>.png` (XR_EXT_atlas_capture spec v2, `DisplayXR/displayxr-runtime#425`). The live `xrCaptureAtlasEXT` path now passes a bare `<stem>-<N>` prefix (no pre-baked `_<cols>x<rows>`) and lets the runtime own the `_atlas_…` tokens, so the final name no longer duplicates the layout (`..._2x1_atlas_2_2x1.png`). The editor-preview (app-side) path writes the same name so both share one sequence counter.

## [1.15.0] - 2026-06-04

### Changed
- **The 'I'-key atlas screenshot is now runtime-owned via `xrCaptureAtlasEXT`** (XR_EXT_atlas_capture, spec v1) for live OpenXR sessions (#140, #396 W6). A live session hands the runtime a path prefix; the runtime reads back its own composited atlas and writes `<prefix>_atlas.png`. The plugin no longer does an app-side `AsyncGPUReadback` or a hidden-camera Kooima re-render on the live path. New public API `DisplayXRFeature.CaptureAtlas(pathPrefix, projectionOnly)`. **Requires a DisplayXR runtime that advertises `XR_EXT_atlas_capture`** — against older runtimes the capture logs `…unavailable` and is a no-op (no crash). The editor-preview (standalone-session) path is unchanged: it still encodes the atlas RT app-side, since there is no runtime OpenXR session in pure-editor preview.

### Fixed
- Live-path screenshot no longer captures the white feedback flash. The flash draws into the same eye buffers the runtime composites for the capture, so arming it immediately whited out the saved atlas; the flash is now deferred a few frames so the runtime grabs the clean atlas first (#140).

## [1.14.0] - 2026-06-01

### Added
- Fullscreen 2D-surround region-editor support for the transparent overlay (#131): opt-in `displayxr_set_fullscreen_overlay_pref` births the overlay covering the monitor minus 1px (DWM-composited, covers the taskbar); new `displayxr_set_overlay_cursor` for app-driven overlay cursor shapes.
- `displayxr_get_canvas_rect_px` binding + sub-rect-aware cyclopean raycast in `DisplayXRTransparentOverlay` so LMB-rotate works with an active canvas sub-rect (#131).

### Fixed
- Startup white flashes + hang with a fullscreen transparent overlay: an exact-monitor window tripped Windows fullscreen-optimization / independent-flip (DWM-alpha bypass); the overlay is now born 1px short of the monitor to stay DWM-composited (#131).

### Changed
- Coarse `[+ms]` relative timestamps on every native log line.

## [1.13.0] - 2026-05-31

### Added
- Per-pixel surround click-through mask: `displayxr_set_overlay_surround_mask(mask, mask_w, mask_h, dst_x, dst_y, dst_w, dst_h)` registers the exact shape of a 2D surround element (e.g. a comic bubble with a triangular tail) as an alpha mask, RLE-unioned into the transparent-overlay `SetWindowRgn` region so the empty area beside the shape keeps routing clicks to the desktop — which the single bounding rect (`displayxr_set_overlay_surround_rect`) could not express. Windows hooked path; macOS resolves via the C# EntryPointNotFound fallback. (#131)

## [1.12.0] - 2026-05-31

### Fixed
- Sub-rect-aware Kooima — physically-correct 3D inside the 2D-surround canvas sub-rect (the frustum/FOV + convergence are now computed for the sub-rect the content actually weaves into, not the full window; no change when no canvas rect is set). (#131)

## [1.11.0] - 2026-05-30

### Added
- **2D surround on the hooked path (#131): high-res 2D content composited post-weave over the woven 3D.** A D3D12 fence-synced surround manager + `DisplayXRSurround` component render a full-resolution 2D layer (e.g. a text bubble) over the woven stereo image, confined to a canvas sub-rect. (#131)
- 2D surround on the standalone session too — works in both Editor Preview and Play Mode. (#131)
- Surround foundation: v7 extension typedefs + hooked canvas-rect re-apply. (#131)

### Fixed
- macOS build: guard Win32-only `sa_push_canvas_rect_to_runtime` calls so the standalone build compiles on macOS. (#131)
- Sub-rect-aware transparent-overlay click-through and a solid silhouette mask for the surround region. (#131)
- Silhouette mask: pin clip-space z so the near/far planes don't carve wedges out of the mask. (#131)
- Surround no-bubble: size the layer to the HWND weave target instead of the panel dimensions. (#131)
- `DisplayXRSurround` retries setup until the display dimensions are valid. (#131)
- Standalone runtime load failing with `ERROR_MOD_NOT_FOUND` (126) via an altered-search-path fix. (#131)

### Changed
- Documentation (CLAUDE.md): streamlined build steps, pruned runtime cruft, and documented independence from the runtime `versions.json` matrix.

## [1.10.0] - 2026-05-26

### Added
- Vulkan rendering backend for Windows, including a Vulkan editor standalone-preview backend. (#122, #124)

### Fixed
- Vulkan: correct dark-image/gamma via VkFormat sRGB→UNORM downgrade. (#122)
- Vulkan: standalone-session binding + CreateExternalTexture handle on the Vulkan editor path. (#124)
- Build post-process tolerates a locked app-icon file (DisplayXR Shell holds icons open) instead of failing.

## [1.9.0] - 2026-05-25

### Added
- Rig-level post-process FXAA anti-aliasing pass (`DisplayXRPostAA`) with a `postProcessAntiAliasing` toggle on DisplayXRCamera/DisplayXRDisplay. (#121)

### Fixed
- Preview-path silhouette aliasing via MSAA intermediate atlas RT. (#120)

### Changed
- **Post-process anti-aliasing now defaults to ON on DisplayXR rigs.** Unity drops MSAA on the XR eye render target (submits sampleCount=1 on D3D12 and Vulkan), so the plugin applies an FXAA pass to restore soft edges. This is a behavior change for existing projects (an extra per-eye blit, negligible cost). Built-in Render Pipeline only — under URP/HDRP `OnRenderImage` does not fire and the pass is a no-op; disable per-rig via the inspector toggle if not wanted. (#121)

## [1.8.1] - 2026-05-23

### Fixed
- Edit Mode preview: gizmo now updates real-time during window drag. (#119)

## [1.8.0] - 2026-05-23

### Added
- Window-relative gizmo + real-time resize intercept: per-eye Kooima frustum gizmos now track the standalone preview window during resize/move, with the resize intercept pushing canvas updates so visualization stays in sync with the live render canvas. (#111, #118)

### Changed
- Documentation: updated deployment notes for the post-#263 plug-in architecture. (#117)

## [1.7.3] - 2026-05-21

### Added
- Scene-view per-eye Kooima frustum gizmos for visualizing stereo projection in the editor. Extended to N views via standalone preview state, with display-centric m2v + scale-as-zoom applied and gating reworked so selection drives Edit Mode while the active rig drives preview. Includes filled-sphere eye glyphs (bumped to 3 cm) for visibility and accepts untracked-but-valid sim_display poses. (#111)

### Fixed
- macOS URP centering: N-view Kooima path now includes URP head-pose compensation so URP scenes render centered. (#115)
- Play Mode default-to-2D + macOS window-relative Kooima in built apps.
- Kooima frustum: near + far planes are now parallel to the display. (#111)
- Stale native bundle: catch `EntryPointNotFoundException` so older binaries fail gracefully. (#111)
- Guard hit-mask push for macOS compile. (#111)

### Changed
- Ship CI-built macOS bundle with N-view accessor. (#111)

## [1.7.2] - 2026-05-18

### Fixed
- Editor compile on Unity 6.0.x: reverted three Unity 6.1+ APIs in `Editor/DisplayXRPreviewSession.cs` back to cross-version-safe equivalents — `Camera.GetEntityId` → `GetInstanceID`, `EditorUtility.EntityIdToObject` → `EditorUtility.InstanceIDToObject`, and single-arg `FindObjectsByType<T>(FindObjectsInactive)` → two-arg `FindObjectsByType<T>(FindObjectsInactive, FindObjectsSortMode)`. Partners on Unity 6000.0.x were hitting 5 compile errors when importing the package; this restores Unity 6.0+ compatibility without affecting Unity 6.1 behavior. #109 (#110)

## [1.7.0] - 2026-05-14

### Added
- Transparent overlay: per-pixel silhouette click-through region (#57). Plugin computes a per-eye silhouette mask, unions both eyes, and applies it cross-process via `SetWindowRgn` so the OS-level hit-testing matches the rendered avatar shape — clicks land on the avatar; non-silhouette pixels pass through to the desktop. Replaces the prior coarse rect-based click-through. #105

### Fixed
- 2D UI window-space composition layer: set `XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT` on the layer flags so the runtime compositor blends UI textures with the documented unpremultiplied-alpha convention. Eliminates dark fringing around anti-aliased UI edges on transparent backgrounds. #105

## [1.6.0] - 2026-05-13

### Changed (breaking)
- Windows transparent overlay is now **alpha-native end-to-end** — same path as macOS. Unity emits per-pixel alpha to the swapchain (`SetEnvironmentBlendMode(AlphaBlend)` is no longer gated to macOS) and the runtime DP composes the captured desktop content under each tile pre-weave + alpha-gates post-weave. Anti-aliased silhouettes get true soft alpha; the v1.3.0 "hard-mask alpha (0 or 1) on Leia hardware" known limitation is gone.
- **Removed** the chroma-color workaround:
  - `DisplayXRTransparentOverlay.RequestChromaKey(Color)` static method
  - `DisplayXRTransparentOverlay.chromaKeyColor` field/property
  - Native `displayxr_set_transparent_chroma_key()` export
  - Internal `transparent_chroma_key_color` shared state
  - `colorKey` argument on `displayxr_set_transparent_overlay()` (signature is now `(enabled, topmost)`)
  - Comments / docs / sample text referring to the chroma color
  - The Camera clear in `OnEnable` is now unconditionally `(0,0,0,0)` on both Windows and macOS.
  - Apps that called `RequestChromaKey` will get a compile error; just delete the call — the alpha-native path is automatic. Resolves `DisplayXR/displayxr-unity#103`.

### Compatibility
- Requires a DisplayXR runtime that (a) advertises `XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND` on the Windows D3D11/D3D12 service compositor and (b) implements the compose-under-bg + alpha-gate DP path (formerly tracked as runtime#190). Older runtimes will fail `xrEndFrame` validation because they don't enumerate `ALPHA_BLEND` — same failure signature as v1.5.6 → v1.5.12. Update both plugin and runtime in lockstep.
- macOS path unchanged.

## [1.5.13] - 2026-05-13

### Fixed
- Windows transparent overlay regression from #85: `SetEnvironmentBlendMode(AlphaBlend)` is now gated to macOS. The Windows DisplayXR runtime (<= v1.3.0-6) does not enumerate `ALPHA_BLEND`, so Unity rejected the call and every `xrEndFrame` failed validation — content never reached the swapchain. Windows transparency continues to use the chroma-key path.

## [1.5.12] - 2026-05-14

### Fixed
- macOS: `displayxr_macos_set_window_borderless` switched to true `NSWindowStyleMaskBorderless` (= 0). v1.5.11's "titled but visually empty" approach left a 1-2 px top-edge contour visible. The earlier concern that mask=0 would break keyboard input (per Cocoa's default `canBecomeKeyWindow=NO`) turned out to be a separate bug (sample's `HAS_INPUT_SYSTEM` gate, fixed in v1.5.11). Empirically Unity's `PlayerWindow` overrides `canBecomeKeyWindow` to return YES regardless of mask, so true borderless works for keyboard. Retains the defensive `makeKeyAndOrderFront:` + `activateIgnoringOtherApps:` after the styleMask change. #101

## [1.5.11] - 2026-05-14

### Fixed
- `Samples~/DefaultInputController/DisplayXRInputController.cs` (default input sample): switched the input-system gate from `HAS_INPUT_SYSTEM` to `ENABLE_INPUT_SYSTEM`. `HAS_INPUT_SYSTEM` is the plugin's internal `versionDefines` symbol — only visible inside the plugin's asmdef. After the v1.5.9 refactor moved the sample into user `Assets/`, the symbol wasn't defined there, so the keyboard helpers fell through to legacy `Input.GetKey` (returns false in projects using New Input System only). WASD / V / Space / I / F11 silently broken in the sample since v1.5.9. `ENABLE_INPUT_SYSTEM` is Unity's official symbol set by Player Settings → Active Input Handling, visible to all assemblies. #100
- macOS: `displayxr_macos_set_window_borderless` now defensively re-keys the window after the styleMask change (`makeKeyAndOrderFront:` + `NSApp activateIgnoringOtherApps:`). In observed runs the Cocoa default kept the window key on its own, but this guards against future quirks. Also slims diagnostic logs from three lines to one. #100

## [1.5.10] - 2026-05-14

### Added
- macOS: `displayxr_macos_set_window_borderless(int enabled)` primitive. Toggles Unity's configured NSWindow between borderless (no title bar / close / minimize / resize chrome — avatar/floating-window look) and the saved original style mask. App-controlled; default behavior unchanged for existing apps. Save/restore symmetric. Drag stays via the cursor-anchored API (begin/update/end_window_drag) — Cocoa's default title-bar drag is gone with the title bar. #98

## [1.5.9] - 2026-05-14

### Changed (breaking for consumers using DisplayXRInputController)
- `DisplayXRInputController` moved out of plugin `Runtime/` into `Samples~/DefaultInputController/`. Plugin Runtime now contains only mechanisms (cursor polling, rig manager, mode setter); input policy lives in app code. Consumers import via Package Manager → DisplayXR → Samples → "Default Input Controller". Same class name, same `DisplayXR` namespace, same fields — just sourced from the project's `Assets/` folder. Scenes referencing the old Runtime type will fail to deserialize until the sample is imported. No deprecation shim — a shim sharing the namespace would collide with the imported sample. #97
- `DisplayXRNative` promoted from `internal` to `public` (class + all P/Invoke methods + `LogCallback` delegate). Enables the sample (now in user Assets/) to call the same bindings the plugin uses. Stability contract: method signatures track underlying native exports; prefer high-level wrappers (`DisplayXRFeature`, `DisplayXRTransparentOverlay`, `DisplayXRRigManager`) where they exist.

### Added
- `DisplayXRInputController.scrollZoomEnabled` field (sample) — parallel to `mouseLookEnabled`. Apps that drive their own scroll-based zoom can set false to opt out of the controller's built-in scroll → camera transform / FOV change.
- macOS `ConsumeWheelDelta` now returns real values (was Win32-only / 0 on Mac). `DisplayXRTransparentOverlay`'s Mac LateUpdate branch accumulates `Mouse.current.scroll.y × 120` per frame; same Win32-unit semantics as the Win32 path. Unblocks `WheelZoomVHeight`-style app wheel handlers on Mac.

### Migration
- Existing apps with `DisplayXRInputController` in scenes need to import the sample. After import, the project-owned copy in `Assets/Samples/com.displayxr.unity/.../Default Input Controller/` resolves scene references via the preserved meta GUID — no scene edits required.

## [1.5.8] - 2026-05-14

### Added
- `DisplayXRInputController.mouseLookEnabled` field (default true) to opt out of the controller's built-in left-mouse-drag → camera rotation. Apps that drive their own hit-tested left-drag interactions (e.g. `DragRotateCube` on a scene target) can set it to false to reserve left-drag for the app's hit-tested target. WASD movement, scroll zoom, and keyboard controls are unaffected. #96

## [1.5.7] - 2026-05-14

### Added
- macOS: cyclopean per-triangle hit-test + onPointer events ported from the Win32 path to the Mac overlay's LateUpdate. `DisplayXRTransparentOverlay.onPointerEnter/Exit/Down/Up/Click` now fire on Mac, allowing app code to know when the cursor is over a clickable renderer. Side effects: `DragRotateCube` left-click-drag-rotate-tiger now works on Mac (was silently no-op), and the test repo's `MacRightDragMoveWindow` can gate right-drag-to-move on the cursor being on the tiger. Hit-test logic mirrors Win32 (UpdateBakedHitColliders / TryGetStereoMatrices / BuildCyclopean / TryBuildEyeRay) with the Win32-only click-through plumbing (`displayxr_set_overlay_hit_*`) skipped — Mac click-through is future Phase 2 of #85. #95

## [1.5.6] - 2026-05-14

### Added
- macOS: cursor-anchored window-drag API (`displayxr_macos_begin_window_drag` / `_update_window_drag` / `_end_window_drag`). Recommended over the existing `offset_window` primitive for mouse-drag use cases — does all the cursor↔window math in pure Cocoa coords inside the plugin, avoiding the scale/feedback issues that occur when feeding `Mouse.current`-derived deltas to `offset_window` on Retina + HiDPI displays. Cursor stays glued to the same window-relative spot for the full drag. #94

## [1.5.5] - 2026-05-14

### Fixed
- **Windows editor preview wsui composition layer rendering** — both `StandaloneD3D12Backend::wsui_copy_to_swapchain_image` and the D3D11 equivalent were stubs that returned false (cross-device copy from Unity's D3D device to the standalone SA D3D device was unimplemented). Filled in by mirroring the existing atlas-bridge pattern: SHARED NT-handle `ID3D12Resource` on the SA device, opened on Unity's D3D12/D3D11 device. C# `Graphics.CopyTexture`s the wsui RT into the bridge each frame; SA backend then copies bridge → swapchain image with a fence wait. Also plumbed `app_pref` through `enumerate_and_pick_format_standalone` so the standalone swapchain matches the bridge's `B8G8R8A8_UNORM` (a cross-format CopyTextureRegion was crashing the runtime in the NVIDIA driver). Mac unaffected (Metal unified MTLDevice).
- **Built-app cube Y shift under URP** — Unity 6 OpenXR defaults to Floor tracking origin mode (eye Y ≈ user height ≈ 1.5–1.7m). The plugin's `xrLocateViews` hook returns LOCAL-space eye coords for Kooima. Under BiRP, Unity reads these directly — fine. Under URP, the RenderGraph picked up `XRInputSubsystem`'s Floor offset and added it on top → cube rendered shifted by ~head height in built apps using URP (`displayxr-unity-test-2d-ui`), while BiRP test repos (`displayxr-unity-test`, `displayxr-unity-test-transparent`) rendered correctly. Now forced to Device mode at `RuntimeInitializeOnLoadMethod(AfterSceneLoad)`.
- **Built-app Render Mode button dead on Windows D3D12** — `XR_EXT_display_rendering_mode` wiring was D3D11-only and `#if defined(_WIN32)`-gated, so the D3D12 hooked backend never enumerated modes and the standalone C ABI shims returned 0 in built apps (no `s_sa.session`). Rendering-mode bookkeeping promoted to `GraphicsBackend` base — D3D11, D3D12, Metal, Vulkan, GL all inherit it for free. C ABI shims fall back to the hooked backend's `rendering_modes[]` when no standalone session is running. Render Mode button now cycles in built apps on all platforms.

### Changed
- Refactored rendering-mode state + methods from `D3D11Backend` to `GraphicsBackend` base class. `D3D11Backend`'s atlas-swapchain logic still reads `current_rendering_mode_index` via the inherited field. No behavior change on D3D11 hooked path.

## [1.5.4] - 2026-05-13

### Added
- macOS: `displayxr_macos_offset_window(dx, dy)` primitive for app-driven borderless-window drag (e.g. right-click-drag-to-move). App owns the input policy, plugin owns the `[NSWindow setFrameOrigin:]` mechanism. Win32 keeps its built-in WndProc-based drag for now (coupled to SR weaver phase-snap on the overlay HWND); architectural unification deferred. #93

### Changed
- `native~/build-mac.sh` no longer `rm -rf build` on every invocation. Re-runs reuse the FetchContent'd OpenXR-SDK clone — local rebuilds drop from 30-60s to ~5s. Pass `--clean` to force a full rebuild.

## [1.5.3] - 2026-05-13

### Fixed
- macOS: push initial rendering mode to runtime in DisplayXRInputController.Start so 3D mode is active at first frame (previously the C# default `m_CurrentRenderingMode = 1` was never pushed; macOS sim_display defaults to 2D / passthrough, requiring two V keypresses to reach 3D). #92
- macOS: `displayxr_is_our_process_foreground` now returns 1 unconditionally on Mac. The Win32 reason for the gate (RIDEV_INPUTSINK delivers keystrokes system-wide) doesn't apply to Cocoa, and `[NSApp isActive]` had transient false-negative windows during app-activation handoff making Shift+Tab feel unreliable. #92

## [1.5.2] - 2026-05-13

### Fixed
- macOS: DisplayXRTransparentOverlay now updates PointerPosition / PointerDelta / IsLeftPressed via Unity's Mouse.current. Was previously gated entirely on UNITY_STANDALONE_WIN, leaving HUD slider drag and other app code that reads these properties non-functional on Mac. Active-rig gate matches Win32 (#91).

## [1.5.1] - 2026-05-13

### Fixed
- macOS: implement `displayxr_is_our_process_foreground` (was Win32-only). Unblocks Shift+Tab HUD toggle and any other C# caller that gates input/UI on app-active state. Uses `NSApplication.isActive` — same semantics as the Win32 foreground-window-PID check.

## [1.5.0] - 2026-05-12

### Added
- **macOS transparent overlay — Phase 1 visual transparency (`#85`)** —
  `XR_EXT_cocoa_window_binding` is wired through with the
  `transparentBackgroundEnabled` flag, and Unity's `NSWindow` is configured
  for per-pixel alpha so the runtime can render into a transparent surface
  on macOS. Mirrors the Windows transparent overlay capability (#57) at the
  plumbing layer; visual transparency now functions end-to-end on macOS.

### Fixed
- **macOS transparent overlay — clear contentView's CAMetalLayer.contents
  (`#86`)** — Unity's contentView retains a stale `CAMetalLayer.contents`
  image that was occluding the runtime's transparent surface, so
  `alpha = 0` regions appeared opaque even with all other plumbing in
  place. The plugin now clears `contentView.layer.contents` after the
  window is reconfigured for transparency, allowing the desktop to show
  through. Completes the macOS transparent overlay end-to-end visual
  verification.

### Changed
- Documentation updates around test-repo workflows and `#82` known-issue
  cleanup carried in for this release.

## [1.4.1] - 2026-05-12

### Fixed
- **wsui + transparent overlay crash (`#82`)** — combining
  `DisplayXRTransparentOverlay` with `DisplayXRWindowSpaceUI` no longer
  crashes the runtime in `xrEndFrame` the first frame the wsui swapchain
  image is copied into. Root cause was a format mismatch: the plugin's
  wsui swapchain was created in `DXGI_FORMAT_R8G8B8A8_UNORM` (the picker's
  hard-coded first preference) while Unity's wsui Canvas `RenderTexture`
  lands in `DXGI_FORMAT_B8G8R8A8_UNORM` on Windows D3D12.
  `CopyTextureRegion` across formats is invalid per the D3D12 spec; the
  release driver silently tolerated the byte permutation on opaque
  flip-model swapchains (so `-test-2d-ui` always worked), but DComp-backed
  transparent swapchains hand the cmd list to a stricter compositor-surface
  validation path that flags it as `DXGI_ERROR_INVALID_CALL` at GPU
  execution time and removes the device. Fix: the plugin now queries
  Unity's RT format via a new `GraphicsBackend::wsui_get_native_texture_format()`
  (implemented for D3D11 and D3D12) and asks the runtime to create the
  wsui swapchain in that exact format — `pick_overlay_format()` takes an
  optional app preference that is tried first.
- **D3D12 resource barriers around the wsui copy** — explicit
  `ResourceBarrier(COMMON → COPY_DEST/COPY_SOURCE → RENDER_TARGET/COMMON)`
  bracket `wsui_copy_to_swapchain_image`. Doesn't resolve `#82` on its own
  but hardens the copy against state-tracking drift regardless of the
  format-match fix.
- **`displayxr.log` path resolution** — `displayxr_log` now resolves an
  absolute path (preferred: `<ExeDir>\displayxr.log`, fallback:
  `%TEMP%\displayxr.log`, last resort: CWD-relative). Previously
  `fopen("displayxr.log", "w")` used Unity's CWD, which the built player
  doesn't guarantee matches the `.exe` directory — so the log file was
  effectively never created in built apps. The chosen path is announced
  via `OutputDebugStringA` and as the first line of the log itself.

## [1.4.0] - 2026-05-11

### Added
- **Per-view foreground-only clip tunable** — new `clip_at_display_plane`
  boolean in the native tunables struct (`Display3DTunables` /
  `Camera3DTunables`) that, when set, overrides each view's projection
  `far_z` with that view's distance to the display plane. Per-view and
  N-view safe — the Kooima per-view loop in `xrLocateViews` already runs
  once per output, so the override scales to 2-view stereo, 4-view quad,
  and N-view lenticular without further changes. Exposed in C# as
  `DisplayXRDisplay.foregroundOnlyClip` and `DisplayXRCamera.foregroundOnlyClip`
  (both inspector-visible with tooltips, pushed in `LateUpdate` alongside
  the other tunables). Resolves `displayxr-unity-test-transparent#2`.
  - Why per-view in native: Unity's XR pipeline reads per-eye projection
    from `xrLocateViews` output, NOT from `Camera.SetStereoProjectionMatrix`.
    A C# override updates Unity's matrix cache (visible to scene-view,
    culling, shadows) but never reaches the GPU draw. Doing the per-view
    `far_z` override inside the native Kooima hook is the only chain that
    affects the rendered image.
  - In display-centric rigs the clip distance is `|eye_scaled.z|`; in
    camera-centric rigs it is `1 / inv_convergence_distance`.
  - The `displayxr_set_tunables` P/Invoke signature gained one trailing
    `int` parameter — additive, but recompile required.
- `CLAUDE.md` Test repos section listing the three sibling Unity test
  projects (`-test`, `-test-transparent`, `-test-2d-ui`) so future
  contributors know the regression surface.

### Fixed
- **`DisplayXRTransparentOverlay` per-triangle SMR hit-test** — clickables
  with a `SkinnedMeshRenderer` are now ray-tested per-triangle
  (Möller-Trumbore against the current `BakeMesh` output) instead of via
  their attached collider. The old `BoxCollider` / `Physics.Raycast` path
  was always coarse — clicks inside the AABB but outside the visible
  silhouette were captured, which surfaced once the cube was swapped for
  a tiger with lots of transparent gaps (between legs, around the hat
  tip). Each `LateUpdate` (after the Animator step — moved from `Update`
  to fix head-drift during animation), the plugin calls
  `smr.BakeMesh(entry.mesh)` and caches `verts[]` + `tris[]`. The
  cyclopean ray walks every triangle, transforming each vertex via
  `Matrix4x4.TRS(smr.position, smr.rotation, Vector3.one)` —
  position + rotation only, NO scale (BakeMesh already applies the rig's
  scale chain). Forces `SkinnedMeshRenderer.updateWhenOffscreen = true`
  and `Animator.cullingMode = AlwaysAnimate` on first bake. Active-rig
  gate via `DisplayXRRigManager.ActiveCamera` so two rigs don't disagree
  on silhouette-edge pixels. 8-frame hysteresis smooths sub-pixel jitter
  on the silhouette edge. Non-SMR clickables (e.g. the cube) keep the
  existing `Physics.Raycast` path — no regression.
- **Win32 stuck-drag fix for forwarded button events** —
  `s_vkey_state` is now updated at the top of `overlay_wnd_proc` for
  EVERY button event (`WM_*BUTTONDOWN/UP/DBLCLK`), regardless of whether
  the event is captured by the overlay or forwarded to the underlying
  Unity HWND via `forward_click_to_underlying_window`. Previously,
  `s_vkey_state` was only updated by Unity's HWND subclass — so when a
  click on the silhouette dragged across the edge and released over a
  transparent area, the `WM_LBUTTONUP` was forwarded and Unity's subclass
  never saw it, leaving C#'s polled left-button state stuck at "pressed"
  forever (sample `DragRotateCube` kept rotating with cursor motion).

### Changed
- `CLAUDE.md` drops the now-shipped tiger-session "Unreleased changes"
  appendix.

### Known limitations
- Transparent overlay (`#57`) and window-space UI (`#65`) do not compose
  yet — see issue `#82`. Apps need to pick one or the other for now.
- SR weaver phase-snap still requires the SDK to own the modal drag loop;
  the transparent overlay's capture-based drag stutters in 3D during
  motion (tracked in `DisplayXR/displayxr-runtime#193`). No change since
  v1.3.0.

## [1.3.0] - 2026-05-09

### Changed
- Plugin now relies on the displayxr-runtime `chromaKeyColor = 0` -> default
  magenta convention shipped in runtime PR #213 / #3a / #3b / #3c. Apps that
  call `RequestChromaKey(Color.magenta)` see no behavior change. Apps that
  pass `0` (or never call the API) now get the runtime DP's default magenta
  instead of "no chroma-key conversion" — equivalent to the previous behavior
  on D3D11/D3D12 where the runtime already used magenta.
- Transparent backgrounds now also work on Vulkan and OpenGL Win32 standalone
  builds, not just D3D11/D3D12. Same `RequestTransparentSession()` API. The
  runtime's GL native compositor falls back to opaque presentation on GPUs
  without `WGL_NV_DX_interop2` (mainly Intel iGPUs) — the cube still renders
  but the desktop doesn't show through.

### Compatibility
- Requires displayxr-runtime ≥ v25.7.0 for Vulkan / OpenGL transparency.
  D3D11/D3D12 transparency keeps working with older runtimes.
- `chromaKeyColor` semantics are unchanged; the only difference is that
  passing `0` is now a useful (and recommended) value, not a no-op.

### Known limitations (no change since v1.2.x)
- Anti-aliased edges become hard-mask alpha on Leia hardware (alpha=0 or 1,
  no in-between). This is fundamental to the chroma-key trick used by the
  SR weaver — fully transparent regions punch through cleanly, but partial-
  transparency pixels on antialiased edges either snap to opaque (with
  possible fringing toward the chroma key) or to fully transparent. Apps
  that need soft alpha should choose a content-safe `chromaKeyColor` to
  minimize fringing.

## [1.2.13] - 2026-05-07

### Added
- Window-Space UI: app-side input routing primitives. `DisplayXRPreviewInput`
  exposes preview-window mouse position / button state / cursor-key polling
  via OS-level reads, so wsui input routers work while the standalone
  preview NSWindow has keyboard/mouse focus (Unity's Input System only
  fires when its own windows are focused).
- `DisplayXRWindowSpaceUI.IsCursorOverInteractive` — static gate that
  scene input controllers (`DisplayXRInputController`) consult to pause
  cube/camera rotation while the user is driving wsui controls.
- Native: `displayxr_standalone_get_preview_window_size` (cross-platform),
  `displayxr_standalone_get_rendering_mode_name(slot)` for runtime-supplied
  mode-name strings, `displayxr_standalone_is_key_pressed` for app-side
  hotkey polling.

### Fixed
- `enumerate_rendering_modes` no longer treats a NULL `mode_names` buffer
  as a count-only query — was the silent root cause of `m_ModeIndices`
  arriving as all-zeros for callers passing `IntPtr.Zero` for the names
  buffer.
- Wsui `Canvas.worldCamera` is now wired to the OverlayCamera so
  `GraphicRaycaster.Raycast` against the layer actually returns hits.
- Wsui content stays aspect-correct under window resize via per-frame
  `OverlayCamera.aspect` + canvas RectTransform updates — no RT
  recreation needed.
- Mac: preview-window click state via `[NSEvent pressedMouseButtons]`
  (wsui sliders/buttons couldn't detect presses before).
- Mac: `noResponderFor:` override on the preview NSWindow silences the
  system beep on hotkey poll-only key events.

### Changed
- Plain `Tab` (camera cycle) is now gated on `!Shift` so apps can bind
  Shift+Tab to their own actions without the rig manager firing.

## [1.2.12] - 2026-05-07

### Fixed
- v1.2.11 attempted to fix the nested-package upm bug but the same fix
  also deleted the .tgz before the GitHub Release step could attach it,
  leaving v1.2.11's release without a downloadable asset. v1.2.12
  reverts to a cleaner workflow that doesn't use `git add -A` at all in
  the upm-publish step (selective `git add -f` for binaries +
  `git rm --cached` for dev-file removals already covers everything).

### Notes
- No source-side changes from v1.2.10 or v1.2.11. Packaging-only release.
- Pin to `#upm/v1.2.12` going forward — both v1.2.9 and v1.2.10 have the
  nested-package bug; v1.2.11 has a clean upm tag but missing GH Release
  asset; v1.2.12 is the first fully-clean release.

## [1.2.11] - 2026-05-07

### Fixed
- CI: stop shipping a duplicate copy of the package nested inside the upm
  branch under `com.displayxr.unity-X.Y.Z/`. The "Create UPM tarball"
  step mkdir'd a staging directory in the working tree; the next step's
  `git add -A` swept it into the upm tag. Consumers saw the package
  imported twice and Unity raised hundreds of "Asset has no meta file,
  in immutable folder" errors on first install (forcing safe-mode editor
  load). v1.2.9 and v1.2.10 upm tags are affected — re-pin to v1.2.11.

### Notes
- No source-side changes from v1.2.10. This is a packaging hotfix.

## [1.2.10] - 2026-05-07

### Added
- New native API `displayxr_standalone_get_preview_mouse_position(out fx, out fy)`
  exposing the runtime preview window's cursor position as fractional
  (0..1, top-left) content-area coords. Mac (`NSWindow`/`NSEvent.mouseLocation`)
  and Windows (`WM_MOUSEMOVE` tracked in `sa_wndproc`) covered. Public C#
  helper: `DisplayXR.DisplayXRPreviewInput.TryGetPreviewMousePosition()`.
- This is the *primitive* an app-side input router needs to make
  `DisplayXRWindowSpaceUI` interactive. The plugin doesn't ship a router
  — different consumer apps want different input models (mouse, hand-
  tracking, touch). See the sample
  [`DisplayXRWsuiMouseRouter.cs`](https://github.com/DisplayXR/displayxr-unity-test-2d-ui/blob/main/Assets/Scripts/DisplayXRWsuiMouseRouter.cs)
  in `displayxr-unity-test-2d-ui` for the canonical mouse → fractional →
  canvas-local → `EventSystem.RaycastAll` flow.

## [1.2.9] - 2026-05-07

### Fixed
- Stop shipping dev-only files in the published UPM package. Unity Package
  Manager was extracting CLAUDE.md, CONTRIBUTING.md, .claude/, .github/,
  launch-*.sh, .gitignore + .gitattributes from the upm branch — Unity
  treats unrecognized .md files at the package root as importable assets
  and warns once per file that the .meta is missing (UPM also strips
  *.md.meta files at extraction). Each install logged 4+ "Asset has no
  meta file, in immutable folder" warnings. The CI now strips these
  files in the upm-branch publish step. Files retained on main; only
  excluded from the published package.

## [1.2.8] - 2026-05-07

### Fixed
- DisplayXRWindowSpaceUI now works under URP. The previous design (Canvas in
  ScreenSpaceCamera mode + dedicated camera with a depth-less RenderTexture)
  was silently failing under URP's RenderGraph: empty RT, transparent layer,
  no UI shown. Rewrote the component to use a private WorldSpace canvas + a
  dedicated overlay camera with the camera's "up" vector inverted to handle
  the bottom-left ↔ top-left RT origin convention. The RT is created with
  explicit GraphicsFormat color + D24_UNorm_S8_UInt depth-stencil to satisfy
  RenderGraph's render-target requirements. Camera is auto-render-disabled
  and manually Render()-ed each LateUpdate. [ExecuteAlways] so this works in
  edit-mode preview as well as Play Mode. (#78)
- Canvas state (renderMode, transform, layer) is now saved + restored in
  OnDisable so the host app's Canvas is left as we found it.

### Known limitations
- WorldSpace-canvas approach means UI elements aren't directly clickable —
  Unity's GraphicRaycaster expects screen-space mouse coordinates against a
  canvas in screen-space or a worldCamera-projected canvas. wsui-rendered
  UI is read-only for now. An input router (mouse → window-fractional →
  canvas-local → synthetic events) is tracked as a v1.2.9+ follow-up.

## [1.2.7] - 2026-05-06

### Fixed
- Export `displayxr_window_space_ui_set_texture/_set_layer/_clear` with `DISPLAYXR_EXPORT` so Unity's P/Invoke can find them. v1.2.6 was missing these symbols (visibility=hidden on Mac, no `__declspec(dllexport)` on Windows), causing `EntryPointNotFoundException` on first DisplayXRWindowSpaceUI.LateUpdate call. (#67)

## [1.2.6] - 2026-05-06

### Added
- Submit `DisplayXRWindowSpaceUI` as `XrCompositionLayerWindowSpaceEXT` so
  2D UI canvases composite as a stereo overlay layer with proper disparity
  on the DisplayXR runtime. (#67)

## [1.2.5] - 2026-05-06

### Added
- macOS standalone builds now auto-bundle the OpenXR loader (`openxr_loader.dylib`)
  into `<App>.app/Contents/PlugIns/`. Unity's own `OpenXRBuildProcessor` only
  handles Windows + Android; without this, every macOS build failed at session
  init with "Failed to load openxr runtime loader". Loader ships at
  `RuntimeLoaders~/macos/` (ignored by Unity's asset pipeline) and is copied
  by `DisplayXRBuildProcessor.OnPostprocessBuild`. (#71)
- `URPBasicScene` sample now ships an editor-only build hook that registers
  `Universal Render Pipeline/Lit` in **Project Settings > Graphics > Always
  Included Shaders** before any standalone build, so the shader isn't dropped
  by Unity's stripper. Also exposed as **Tools > DisplayXR > Register URP/Lit
  in Always Included Shaders**. (#72)

### Documentation
- README "macOS Deployment" section now covers the unsigned-`.app` symlink
  issue (`XR_ERROR_RUNTIME_UNAVAILABLE` despite a working
  `~/Library/Application Support/openxr/1/active_runtime.json` symlink) and
  the two workarounds: explicit `XR_RUNTIME_JSON`, or ad-hoc `codesign
  --deep --force --sign - MyApp.app`. Also covers Developer ID + notarization
  for distribution. New troubleshooting row mirrors this. (#72)

## [1.2.4] - 2026-05-06

### Added
- URP and HDRP support for the stereo rig camera callbacks. `DisplayXRDisplay`
  and `DisplayXRCamera` now route through `RenderPipelineManager.beginCameraRendering`
  when a Scriptable Render Pipeline is active, and continue to use
  `Camera.onPreRender` on the Built-in Render Pipeline. Adds a `URPBasicScene`
  sample mirroring `BasicScene`. (#68)

### Fixed
- macOS native bundle (`Runtime/Plugins/macOS/displayxr_unity.bundle`) was
  missing the `displayxr_set_use_srgb_swapchain` export — a regression that
  caused `EntryPointNotFoundException` on macOS standalone builds. Rebuilt
  from current `native~/` source. (#68)

## [1.2.3] - 2026-05-05

### Fixed
- Unity 6 compile error in `DiscoverCameras`: pass explicit `FindObjectsSortMode.None` so the call binds to the two-arg overload that exists on both Unity 2022.3 and Unity 6 (#3).

## [1.2.2] - 2026-05-05

### Added
- `Samples~/MinimalTransparent/` — minimal teaching sample for the chroma-key transparent overlay technique. ~70-line bootstrap script + long-form README that dissects the four-mechanism pipeline (camera clear, OpenXR extension fields, runtime post-weave shader, OS LWA_COLORKEY) and the layer-ownership map. Companion to the polished `TransparentAvatar` sample.
- `DisplayXRTransparentOverlay.ConsumeWheelDelta()` — public method on the component returning the accumulated mouse-wheel delta (Win32 raw units, 120 per notch). Apps poll this and decide what to do with the wheel.
- Native export `displayxr_consume_overlay_wheel_delta()` — atomic read + zero of the overlay's accumulated wheel delta (`InterlockedExchange` on a `volatile LONG`). Declared in `displayxr_hooks.h` and `displayxr_win32.h`.

### Removed
- The experimental WM_MOUSEWHEEL → resize-overlay-HWND behavior from v1.2.0. Plugin no longer self-resizes the overlay when the user scrolls; apps now drive what the wheel does (e.g. `DisplayXRDisplay.virtualDisplayHeight` for zoom-in-window). The plugin still consumes the wheel message when its overlay is foreground so it doesn't bubble to underlying apps.

## [1.2.1] - 2026-05-04

### Added
- `DisplayXRTransparentOverlay.chromaKeyColor` is now a settable property — assigning at runtime re-pushes camera clear + native overlay state. New `ApplyChromaKey()` private helper + `OnValidate` for live Inspector edits during Play.

### Fixed
- 3D stutter during right-drag of the transparent overlay and during the standalone preview's SC_MOVE intercept — synchronous WM_ENTERSIZEMOVE/EXITSIZEMOVE bracketing now drives the SR SDK weaver's phase-snap state machine without needing a runtime-side API change (#61).

### Changed
- `Samples~/TransparentAvatar` default chroma key switched from magenta (1,0,1) to near-mid-gray (128,127,129) so silhouette-edge halos blend invisibly into typical desktop/photo backgrounds. README updated with the rationale and the new palette-clamp trade-off.
- CI is now PR-driven: `build-native.yml` fires on push-to-main, all PRs (drafts included), v* tags, workflow_dispatch; concurrency cancels in-progress runs on rapid pushes. The `/ci-monitor` skill is retired in favor of opening PRs and letting CI report on the head ref.

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
  `DisplayXR/displayxr-runtime` following GitHub org transfer
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
