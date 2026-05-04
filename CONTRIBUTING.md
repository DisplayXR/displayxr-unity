# Contributing to displayxr-unity

Thanks for your interest in the DisplayXR Unity plugin.

## Quick Reference

1. Fork the repository (external) or create a feature branch off `main` (collaborators)
2. Make your changes; if you touch C# code, run the standard Unity formatter / `dotnet format` before committing
3. Submit a pull request targeting `main`
4. CI must pass (build + UPM package validation)
5. A maintainer will review your PR

## What Lives Where

This repo contains the Unity plugin only — the OpenXR runtime, native compositors, and extension headers are in [`displayxr-runtime`](https://github.com/DisplayXR/displayxr-runtime). The plugin talks to the runtime over standard OpenXR plus the `XR_EXT_display_info` / window-binding extensions published in [`displayxr-extensions`](https://github.com/DisplayXR/displayxr-extensions).

A ready-to-open sample project lives in [`displayxr-unity-test`](https://github.com/DisplayXR/displayxr-unity-test) — use it to verify plugin changes against a working scene before opening a PR.

## Issues

- **Plugin bugs and feature requests** → open an issue here.
- **Runtime / native-compositor / extension issues** → file at [`displayxr-runtime`](https://github.com/DisplayXR/displayxr-runtime/issues).
- **Spatial Shell issues** → [`displayxr-shell-releases`](https://github.com/DisplayXR/displayxr-shell-releases/issues).

## License

By contributing, you agree your code is licensed under the same terms as the rest of the repository (see [LICENSE](LICENSE)).
