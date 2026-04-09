# DisplayXR Unity Plugin — Documentation

## For App Developers
- [README](../README.md) — Features, installation, scene setup, building
- [Quick Start Guide](quick-start-guide.md) — Hands-on walkthrough
- [Troubleshooting](../README.md#troubleshooting) — Common issues and fixes

## For Contributors

### Architecture — How the system works
- [Preview Session](architecture/preview-session.md) — SA session lifecycle, frame loop, Play Mode integration
- [Hook Chain](architecture/hook-chain.md) — OpenXR interception, thread-safe state, deferred destruction
- [Kooima Pipeline](architecture/kooima-pipeline.md) — Stereo projection math, transform chain, tunables

### Architecture Decision Records — Why things are the way they are
- [ADR-001: Deferred Destruction](adr/ADR-001-deferred-destruction.md) — Why xrDestroySession/Instance is deferred
- [ADR-002: Dual-Session Architecture](adr/ADR-002-dual-session.md) — SA session vs Unity XR loader
- [ADR-003: Native Preview Window](adr/ADR-003-native-preview-window.md) — Plugin-owned HWND/NSWindow instead of IOSurface
- [ADR-004: Camera vs Display Mode](adr/ADR-004-camera-vs-display-mode.md) — Two stereo rig types
- [ADR-005: Forced MultiPass](adr/ADR-005-multipass-forced.md) — Why Single-Pass Instanced is disabled
- [ADR-006: Window-Relative Kooima](adr/ADR-006-window-relative-kooima.md) — Off-center window projection correction

## Reference
- [CLAUDE.md](../CLAUDE.md) — AI developer briefing, build commands, code style
- [CHANGELOG.md](../CHANGELOG.md) — Version history
