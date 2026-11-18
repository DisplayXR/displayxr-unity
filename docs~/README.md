# DisplayXR Unity Plugin — Documentation

## For app developers

Start here:
- [README](../README.md) — Features, requirements, installation, scene setup, building
- [Quick Start Guide](quick-start-guide.md) — Hands-on walkthrough
- [Render Path](../README.md#render-path) — which pipeline renders single-pass or multi-pass, per graphics API
- [Runtime Compatibility](../README.md#runtime-compatibility) — which plugin feature needs which runtime version or extension
- [What Happens When Things Are Missing](../README.md#what-happens-when-things-are-missing) — no runtime, no 3D panel, eye tracking lost, session stopped, and the API that detects each
- [Troubleshooting](../README.md#troubleshooting) — Common issues, fixes, and where the log files are

Feature guides. These live under `architecture/` and also explain how each feature is built, but each one holds facts you need to integrate the feature:
- [Window-Space 2D UI](architecture/window-space-ui.md) — `DisplayXRWindowSpaceUI` rendering model and the input-routing contract (the composition layer carries pixels, not input)
- [2D Scenes and Menus](architecture/two-dimensional-scenes.md) — why a rig-less camera still renders stereo, the three levers that actually switch to 2D, the asymmetric 3D↔2D transition, and `DisplayXRSceneMode`
- [Depth-Aware Cursor](architecture/cursor-depth.md) — `XR_DXR_cursor_depth` (runtime ADR-046): the runtime draws a stereo cursor in front of popped-out content; opt-in API, depth sources, wiring, and the Windows panel verification recipe
- [Stereo-Correct Picking](architecture/stereo-picking.md) — `DisplayXRPicking` (#460): why `Camera.ScreenPointToRay` misses everywhere but at the screen centre (the geometry, and the measured error), the screen point ↔ ray / world point API, cyclopean vs per-eye, and the pixel-space rules (the hidden Unity window on Windows, 3D zones, the editor Game view)
- [Click-Through Silhouette Mask](architecture/click-through-mask.md) — transparent-overlay per-pixel click-through: the mask→`SetWindowRgn` pipeline, the mask/weave rect-alignment invariant, and the debugging pitfalls (the #166 clip)
- [Hand Tracking](architecture/hand-tracking.md) — the XR Hands subsystem: `XR_EXT_hand_tracking` from the runtime's input provider (Ultraleap) through `com.unity.xr.hands`; package gating, the rig-camera origin + scale rule, threading, limits, validation
- [Controller and Stylus Input](architecture/controller-input.md) — the "DisplayXR Input" `IUnityXRInput` subsystem: OpenXR actions for `khr/simple_controller` and the stylus interaction profile (`XR_LOGITECH_mx_ink_stylus_interaction`, when advertised) as `UnityEngine.XR` InputDevices; feature table, Input System layouts, the rig-camera pose frame, haptics, validation
- [Side-Step](architecture/side-step.md) — Linux native Wayland: the app's own window steps aside for the window the user works in and goes back when its place is free; only while the app's desktop control is granted (the #389 gate, checked before every poll); other windows are read inside the plug-in only
- [File Drops and the Clipboard](architecture/linux-file-drop-clipboard.md) — Linux native Wayland: files dragged onto the window (#395), what that does to Unity's clipboard and the plug-in clipboard that replaces it (#398), and the input fields that copy and paste through it and get IME text (`DisplayXRClipboardInputField`, `DisplayXRClipboardLegacyInputField`, #415)

Sample READMEs (each sample in Package Manager > DisplayXR > Samples has one) cover the input controller, the window-space UI mouse router, transparent overlays, the splash screen, hand tracking and stylus input.

## For contributors

### Architecture — How the system works
- [XR Display Provider](architecture/xr-display-provider.md) — **the shipping path (#166):** the custom `IUnityXRDisplay` provider, its component map, and Play Mode parity
- [Kooima Pipeline](architecture/kooima-pipeline.md) — Stereo projection math, transform chain, tunables
- [SDL Fixups](architecture/linux-sdl-fixups.md) — Linux native Wayland: upstream SDL fixes the plug-in applies to Unity's embedded SDL2 from outside (a monitor removed and added again, #410; the clipboard use-after-free, #412), the IME text the DisplayXR input fields get through the same mechanism (#415), how they are gated on the exact `UnityPlayer.so`, and the `DISPLAYXR_SDL_FIXUPS=0` kill switch
- The feature guides listed under "For app developers" above are also the design notes for those features.

### Architecture Decision Records — Why things are the way they are
- [ADR-001: Deferred Destruction](adr/ADR-001-deferred-destruction.md) — Why xrDestroySession/Instance is deferred
- [ADR-002: Dual-Session Architecture](adr/ADR-002-dual-session.md) — SA session vs Unity XR loader (superseded)
- [ADR-003: Native Preview Window](adr/ADR-003-native-preview-window.md) — Plugin-owned HWND/NSWindow instead of IOSurface (superseded)
- [ADR-004: Camera vs Display Mode](adr/ADR-004-camera-vs-display-mode.md) — Two stereo rig types
- [ADR-005: Forced MultiPass](adr/ADR-005-multipass-forced.md) — Why Single-Pass Instanced is disabled (superseded)
- [ADR-006: Window-Relative Kooima](adr/ADR-006-window-relative-kooima.md) — Off-center window projection correction
- [ADR-007: Render Path by View Count](adr/ADR-007-render-path-by-view-count.md) — Provider (<=8 views) vs future quilt/atlas (>8 views) path selection

## Reference
- [CLAUDE.md](../CLAUDE.md) — AI developer briefing, build commands, code style (contributors)
- [CHANGELOG.md](../CHANGELOG.md) — Version history
