# Contributing to displayxr-unity

The DisplayXR Unity plugin — Kooima eye-tracked stereo rendering and a
2D UI overlay for OpenXR-compatible 3D displays. Native plugin (C/C++)
plus Unity-side runtime and editor code.

See the [org-wide CONTRIBUTING](https://github.com/DisplayXR/.github/blob/main/CONTRIBUTING.md)
for issue routing, branch/PR flow, and licensing. The notes below cover
build, test, and conventions specific to this repo.

## Repo layout

- **`native~/`** — C/C++ source for the native OpenXR-hooking plugin
  (D3D11/D3D12 backends, GPU preference helpers, eye-tracking glue).
  The trailing `~` is intentional: Unity ignores tilde-suffixed
  directories so the C/C++ sources don't appear in the Unity asset
  database.
- **`Runtime/`** — Unity-side managed code that the editor / built
  app loads at runtime.
- **`Editor/`** — Unity editor extensions (preview window, camera
  selector, project-settings UI).
- **`docs~/`** — Quick-start and architecture docs (also tilde-hidden).

## Building the native plugin

The native plugin must be rebuilt whenever you touch anything under
`native~/`.

### Windows
```bat
cd native~
build-win.bat
```
Outputs the .dll into `Runtime/Plugins/x86_64/` so the Unity package
picks it up automatically. Requires Visual Studio 2022 with the C++
workload + CMake + Ninja.

### macOS
```bash
cd native~
./build-mac.sh
```
Outputs into `Runtime/Plugins/macOS/`.

### Cross-compiling Windows from a Unix shell
```bash
cd native~
./build-win.sh
```
Wraps `build-win.bat` via `cmd.exe`. See the existing helper for the
exact path conventions.

## Testing

There are no automated unit tests in this repo. The verification
loop is:

1. Open [`displayxr-unity-test`](https://github.com/DisplayXR/displayxr-unity-test)
   — a minimal Unity 6 project that depends on this plugin via UPM.
2. Hit Play. The DisplayXR runtime + native plugin should compose stereo
   output on a registered 3D display.
3. For headless / batchmode verification, use `Unity.exe -batchmode
   -quit -nographics -projectPath … -executeMethod
   BuildScript.BuildWindows64 -logFile …` against the test project.

Build CI (`.github/workflows/build-native.yml`) verifies the native
plugin compiles on every PR. It does not run Unity itself.

## Conventions

- **C/C++ style** — match the existing files; no enforced clang-format
  config in this repo (yet). Match brace style + spacing of the
  surrounding code.
- **Unity managed code** — match the existing patterns in `Runtime/`
  and `Editor/`. Don't introduce additional package dependencies
  without discussion (the plugin is intentionally lean).
- **Plugin GUIDs** — every Unity asset has a `.meta` file with a stable
  GUID. Don't regenerate them; that breaks downstream references in the
  test project.

## Architecture

Read [`README.md`](README.md) for the user-facing pitch. The
[architecture overview](docs~/quick-start-guide.md) covers the native
hook flow + Kooima projection details.

For OpenXR extension semantics consumed by this plugin (display info,
window binding, eye tracking modes), see the
[runtime extension specs](https://github.com/DisplayXR/displayxr-runtime/tree/main/docs/specs).

## Licensing

Boost Software License 1.0. By contributing you agree your work is
licensed under BSL-1.0.
