# Agent Prompt — Full Graphics Backends Refactor

> Paste everything below the line into a fresh Claude Code session running on a machine with the repo checked out. **Phase 1 requires both a Windows machine (for D3D11/D3D12 verification) and a macOS machine (for Metal verification)** — coordinate with the user about where to run each step. Phase 2 can largely be done anywhere that has a Vulkan SDK and can cross-compile; runtime validation is out of scope.

---

You are continuing work on the DisplayXR Unity plugin. Your job is to port the per-graphics-API backend refactor from a parallel branch (`origin/full_gfx_apis`) onto current `main`, in preparation for Android and Linux support. This is a two-phase effort delivered as two PRs.

**You are NOT cherry-picking or rebasing `full_gfx_apis`.** A literal rebase will fail catastrophically — both sides rewrote the same files. You are using the branch as a *template* and redoing the split by hand on current `main`. Read the plan before touching anything.

## Background you need before touching anything

1. **Read the full plan first** — it has all the context, strategy, file layout, failure modes, and verification steps:
   - `docs~/roadmap/full-gfx-backends-refactor-plan.md`
2. **Read the prior D3D11 fix plan** for related background — the D3D11 typed-swapchain substitution that `main` has today was cherry-picked from the same `full_gfx_apis` branch, and it must survive your refactor untouched:
   - `docs~/roadmap/d3d11-typeless-fix-plan.md`
3. **Skim `CLAUDE.md`**, especially:
   - "Building the Native Plugin" — the local build scripts for each platform.
   - "Key Architecture → Three Layers" and "OpenXR Hook Chain" — how the native plugin intercepts Unity's OpenXR calls.
   - "Known Issues" — the parked Windows preview drag/phase-snap behavior. **Do not try to fix it.**
4. **The branch `full-gfx-backends-refactor` already exists locally** (created from `main`). If it has not been pushed yet, push it with `-u` on your first commit. Do not create it again.

## What you are porting

Two commits from `origin/full_gfx_apis`:

- **`a089149` "refactor"** — splits `displayxr_hooks.cpp` and `displayxr_standalone.cpp` into per-backend translation units. Adds internal headers. ~2100 lines of pure code motion at the time it was written.
- **`98a6403` "more impls"** — real Vulkan / OpenGL / OpenGL ES backend bodies. ~2600 lines, gated by CMake `ENABLE_VULKAN` / `ENABLE_OPENGL` options.

**You are NOT porting** any other commit on `full_gfx_apis`. Specifically, do not bring `008f9e1..5fbfedb` — those are Windows windowed-mode and Game View overlay commits that were already superseded on `main` by `caf6de8`, `2a84927`, `a7c91dc`, `b7e2d22`. Bringing them will regress main.

Authoritative references (read-only while working):

```bash
git show a089149                                    # the template layout
git show 98a6403                                    # the Vulkan/GL/GLES bodies
git show a089149:native~/displayxr_hooks_internal.h
git show a089149:native~/displayxr_standalone_internal.h
git show a089149:native~/CMakeLists.txt
```

And these on `main`, which your refactor must preserve intact:

```bash
git show 2a9a933    # D3D11 typed-swapchain substitution (issue #91 fix)
git show b7e2d22    # N-view tile atlas composite
git show a7c91dc    # D3D12 atlas bridge
git show 2a84927    # Windows SA preview HWND + atlas bridge + input
git show caf6de8    # Windows preview Play-mode startup, input, weaving, Y-flip
```

## Hard rules

- **Do NOT** `git rebase` or `git merge` `full_gfx_apis`. Use `git show <sha>:<path>` to read branch files, then write the new files by hand on current `main`.
- **Do NOT** bring any commit from `full_gfx_apis` other than `a089149` and `98a6403`, and even those are references, not patches.
- **Do NOT** change any behavior in Phase 1. It is pure code motion. If Windows D3D11 or D3D12 or macOS Metal shows any regression, the split is wrong — fix the split, do not "patch" the result.
- **Do NOT** land Phase 1 and Phase 2 in the same PR. They have very different review burdens.
- **Do NOT** touch the Metal `.m` files (`displayxr_metal.m`, `displayxr_standalone_metal.m`). The new Metal backend `.cpp` files are thin wrappers that call into them.
- **Do NOT** try to validate Vulkan/GL/GLES backends against a real runtime. No such runtime exists yet on Android/Linux. Phase 2 delivers a compiling, guarded scaffold only.
- **Do NOT** force-push or rewrite history on a shared branch.
- **Do NOT** "improve" or refactor anything outside the scope of the split.
- If you cannot confidently decide where a piece of code from the monolith belongs in the split, **stop and ask the user**. `displayxr_hooks.cpp` and `displayxr_standalone.cpp` are sensitive — a wrong split will silently corrupt rendering rather than crash.

## Critical decision you must make before starting Phase 1

`b7e2d22` added an N-view tile atlas composite to `hooked_xrEndFrame` that is D3D11-specific today but architecturally shared. Read "Phase 1 step 5" in the plan. **Recommend Option A** (keep it inline in `displayxr_d3d11_backend.cpp`). Confirm with the user before starting if you want to deviate.

## Concrete task list — Phase 1 (refactor redo)

Work through these in order. Every step is elaborated in the plan file — read the plan, don't just skim the bullet.

1. **Sync and check out the branch.**
   ```bash
   git fetch origin
   git checkout full-gfx-backends-refactor
   git pull --ff-only     # no-op if not yet pushed
   ```

2. **Create the two new internal headers** (`displayxr_hooks_internal.h`, `displayxr_standalone_internal.h`). Start from `a089149`'s versions, diff against current main's monoliths, and add any `static` state introduced post-`7aa2c28` (D3D11 sub state, N-view tile atlas state, Windows preview HWND/atlas-bridge state).

3. **Create the per-backend TUs as skeletons first** — empty-but-compilable files with correct `#if` guards and correct `#include` lines. See the plan's Phase 1 step 3 table for the full file list.

4. **Move code from the monoliths into the backend TUs.** Mechanical: each function guarded by `#if defined(_WIN32)` + `s_d3d11_device` goes to `displayxr_d3d11_backend.cpp`; each `#if defined(_WIN32)` + `s_d3d12_device` section goes to `displayxr_d3d12_backend.cpp`; each `#if defined(__APPLE__)` section goes to `displayxr_metal_backend.cpp`; and so on. `displayxr_hooks.cpp` and `displayxr_standalone.cpp` keep only dispatch glue.

5. **Update `CMakeLists.txt`**: merge the branch's additions (`ENABLE_VULKAN`, `ENABLE_OPENGL` options; `find_package(Vulkan)` and OpenGL plumbing; new source files in `SOURCES`) onto the current main CMake. Do not regress main's Windows link libs or Metal source-file properties.

6. **Build all three targets.** All must succeed before you commit:
   ```bash
   native~/build-mac.sh                # macOS arm64+x86_64 Universal
   native~\build-win.bat               # Windows x64 (from a Dev Command Prompt)
   native~/build-win.sh                # Cross-compile check only, from macOS
   ```

7. **Run the end-to-end verification sweep** on `DisplayXR-test` (the test project — on macOS at `/Users/david.fattal/Documents/Unity/DisplayXR-test`, on Windows ask the user). Every test in the plan's "Phase 1" verification block must pass:
   - Windows D3D11: edit-mode preview and Play Mode, no X-pattern, no regression.
   - Windows D3D12: edit-mode preview and Play Mode, atlas bridge works, phase-snap on drag release.
   - macOS Metal: edit-mode preview and Play Mode unchanged.
   - Editor log should be line-for-line identical (modulo timestamps and DLL-load lines) to a pre-refactor run on `main`.

8. **Commit and push Phase 1 as one commit** (or a small series of logically clean commits — do not interleave with Phase 2):
   ```bash
   git add native~/ Runtime/Plugins/
   git commit -m "Refactor native plugin into per-backend translation units"
   git push -u origin full-gfx-backends-refactor
   ```

9. **Open PR #1** labeled `refactor`. Description must state: "pure code motion, no behavior change, validated on Windows D3D11/D3D12 and macOS Metal." Include the `Editor.log` diff result as evidence.

**STOP HERE and wait for the user to review and merge Phase 1 before starting Phase 2.** The two phases must land independently.

## Concrete task list — Phase 2 (backend bodies)

Only start this after Phase 1 is merged (or on an explicit go-ahead from the user to stack Phase 2 on the unreviewed Phase 1 branch).

1. **Sync.**
   ```bash
   git fetch origin
   git checkout full-gfx-backends-refactor   # or a follow-up branch
   git pull --ff-only
   ```

2. **Port the Vulkan, OpenGL, and OpenGL ES backend bodies** from `98a6403`:
   ```bash
   git show 98a6403:native~/displayxr_vulkan_backend.cpp       > native~/displayxr_vulkan_backend.cpp
   git show 98a6403:native~/displayxr_standalone_vulkan.cpp    > native~/displayxr_standalone_vulkan.cpp
   git show 98a6403:native~/displayxr_opengl_backend.cpp       > native~/displayxr_opengl_backend.cpp
   git show 98a6403:native~/displayxr_opengles_backend.cpp     > native~/displayxr_opengles_backend.cpp
   git show 98a6403:native~/displayxr_standalone_opengl.cpp    > native~/displayxr_standalone_opengl.cpp
   git show 98a6403:native~/displayxr_standalone_opengles.cpp  > native~/displayxr_standalone_opengles.cpp
   ```
   Each file must be guarded at the top level by `#if defined(ENABLE_VULKAN)` or `#if defined(ENABLE_OPENGL)` as appropriate — Phase 1 skeletons already have these guards, and the `98a6403` bodies assume them.

3. **Verify the extern declarations** the new bodies depend on all exist in `displayxr_hooks_internal.h` / `displayxr_standalone_internal.h` (should, since Phase 1 copied them from `a089149`). Resolve any missing symbols by adding declarations, not by changing the bodies.

4. **Wire the CMake link libraries** for Vulkan and OpenGL under their respective `ENABLE_*` gates. Cross-reference `git show 98a6403:native~/CMakeLists.txt`.

5. **Compile-check with the new options enabled** on every host you can:
   ```bash
   # Default build (must still match Phase 1)
   native~/build-mac.sh
   native~\build-win.bat

   # Opt-in Vulkan build
   cmake -S native~ -B native~/build-vk -DENABLE_VULKAN=ON -DCMAKE_BUILD_TYPE=Release
   cmake --build native~/build-vk --config Release

   # Opt-in OpenGL build
   cmake -S native~ -B native~/build-gl -DENABLE_OPENGL=ON -DCMAKE_BUILD_TYPE=Release
   cmake --build native~/build-gl --config Release
   ```
   On Windows you will need the Vulkan SDK installed (sets `VULKAN_SDK` env var). On macOS, Vulkan is available via MoltenVK if installed. OpenGL is system-provided on both.

6. **Re-run the default-build regression sweep.** Default builds (`ENABLE_VULKAN=OFF ENABLE_OPENGL=OFF`) must be bit-identical in behavior to Phase 1. Rerun the Windows D3D11/D3D12 and macOS Metal checks. If anything changed, an `#if` guard leaked.

7. **Commit and push Phase 2** as one or two commits (mirroring the branch's "add bodies" + "CMake wire-up" split is fine):
   ```bash
   git add native~/ Runtime/Plugins/
   git commit -m "Add Vulkan / OpenGL / GLES backend bodies (scaffold, opt-in)"
   git push
   ```

8. **Open PR #2.** Description must state: "Vulkan / OpenGL / GLES scaffolding, opt-in via `ENABLE_VULKAN` / `ENABLE_OPENGL` CMake options, default builds unchanged. Runtime validation is a separate deliverable — this PR is compile-validated only."

9. **Report back to the user** with what built, on which hosts, and any warnings. Mention that runtime validation is pending — do not claim Vulkan/GL/GLES work end-to-end, because we don't yet have a DisplayXR runtime to test against on Android/Linux.

## Definition of done

### Phase 1
1. `full-gfx-backends-refactor` has a clean set of commits doing nothing but code motion.
2. `displayxr_hooks.cpp` and `displayxr_standalone.cpp` are substantially shrunk; the per-backend TUs exist and contain the platform-specific code.
3. `displayxr_hooks_internal.h` and `displayxr_standalone_internal.h` exist and are consumed by the backend TUs.
4. Local builds succeed on Windows (`build-win.bat`), macOS (`build-mac.sh`), and macOS→Windows cross (`build-win.sh`).
5. `DisplayXR-test` passes on Windows D3D11, Windows D3D12, and macOS Metal with **zero behavior change** vs. `main`.
6. CI green on `full-gfx-backends-refactor` for both Windows x64 and macOS Universal.
7. PR #1 opened and the user has the info needed to review.

### Phase 2
8. Vulkan / OpenGL / GLES backend TUs contain the real bodies from `98a6403`, guarded by `ENABLE_VULKAN` / `ENABLE_OPENGL`.
9. CMake wires the new link libraries under those gates.
10. Default builds (`ENABLE_VULKAN=OFF ENABLE_OPENGL=OFF`) are a bit-identical regression-free match to Phase 1.
11. `-DENABLE_VULKAN=ON` and `-DENABLE_OPENGL=ON` compile-check builds succeed on at least one desktop host with the SDKs available.
12. PR #2 opened, explicitly labeled as scaffold/opt-in with runtime validation still pending.

Good luck. Read the plan file first — really, read it, don't skim. The plan's "Failure modes to watch for" section is the shortcut to not getting stuck.
