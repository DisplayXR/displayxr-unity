# Full Graphics Backends Refactor — Plan

**Branch:** `full-gfx-backends-refactor` (created from `main`, not yet pushed)
**Source material:** `origin/full_gfx_apis`, commits `a089149` ("refactor") and `98a6403` ("more impls")
**Target platforms:** unlock Android / Linux (Vulkan), broaden desktop reach (OpenGL/GLES), keep Windows (D3D11/D3D12) and macOS (Metal) behavior unchanged
**Related prior work:** `d3d11-typeless-fix-plan.md` (cherry-picked as `2a9a933` on `main`), issue #91

---

## Context

DisplayXR has an active roadmap item to support **Android and Linux** deployments of the Unity plugin. Unity's default graphics backend on Android is Vulkan; Linux desktop commonly uses Vulkan or OpenGL. Today `main` supports only:

- **Windows:** D3D11 (via the typed-swapchain substitution from `2a9a933`) and D3D12 (atlas bridge from `a7c91dc`, `b7e2d22`).
- **macOS:** Metal.

There is **no Vulkan, OpenGL, or OpenGL ES path on `main`**, neither in the OpenXR hook chain (`displayxr_hooks.cpp`) nor in the standalone preview session (`displayxr_standalone.cpp`). Both files are monolithic and platform-guarded with `#if defined(_WIN32) / __APPLE__`.

A parallel branch, `origin/full_gfx_apis`, contains two commits we now want to bring home:

1. **`a089149` "refactor"** — Splits the two monolithic files into per-backend translation units. Adds internal headers shared between the core dispatch and the backend TUs. Net: `+2094 / -191`.
   - Hook-chain side: `displayxr_{d3d11,d3d12,metal,vulkan,opengl,opengles}_backend.cpp` + `displayxr_hooks_internal.h`.
   - Standalone side: `displayxr_standalone_{d3d11,d3d12,metal_backend,vulkan,opengl,opengles}.cpp` + `displayxr_standalone_internal.h`.
   - CMake additions: `ENABLE_VULKAN`, `ENABLE_OPENGL` options; Vulkan SDK / OpenGL find_package plumbing.
2. **`98a6403` "more impls"** — Real bodies for the non-D3D/Metal backends. Net: `+2578 / -125`.
   - Vulkan hook-chain backend: ~529 lines.
   - Vulkan standalone backend: ~883 lines.
   - OpenGL hook-chain / standalone: ~457 / ~365 lines.
   - OpenGL ES hook-chain / standalone: ~392 / ~55 lines.

Everything else on `full_gfx_apis` (Windows windowed-mode fixes, Game View overlay rework, `008f9e1..5fbfedb`) is **already superseded** on `main` by different commits (`caf6de8`, `2a84927`, `a7c91dc`, `b7e2d22`, etc.). Do not bring those.

## Why a literal `git rebase` of `full_gfx_apis` is the wrong tool

Both sides heavily rewrote the same two files since the fork point (`7aa2c28`):

| File                            | `main` churn   | `full_gfx_apis` churn              |
|---------------------------------|----------------|-------------------------------------|
| `native~/displayxr_hooks.cpp`   | +783 / rewrite | +471 (then split away by `a089149`) |
| `native~/displayxr_standalone.cpp` | +908 / rewrite | +1290 (then split away by `a089149`) |

Current sizes on `main`: `displayxr_hooks.cpp` ≈ 1996 lines, `displayxr_standalone.cpp` ≈ 2307 lines. Main's additions are the D3D11 typed-swapchain substitution, the N-view tile atlas generalization, and the full Windows preview window / atlas bridge / edit-mode path. `a089149` was written against a much smaller base and then deletes most of it during the split. Replaying all 14 branch commits in order against today's `main` will conflict on nearly every commit with almost no semantic guidance from Git — and the conflict resolution has to re-understand the split *while* preserving the D3D11+atlas work that was never in the branch.

**Strategy instead:** treat `a089149` as a *template* for the file layout, not a patch to apply. Redo the split by hand on current `main`. Then cherry-pick / re-apply `98a6403` on top — most of it is new files and should go in cleanly.

## Strategy — two phases, two PRs

### Phase 1 — Refactor redo (behavior-preserving split)

Goal: `main` compiles and runs identically on Windows D3D11, Windows D3D12, and macOS Metal, but the native source is now organized as per-backend TUs with shared `_internal.h` headers. No new backends enabled. Pure code motion + minor extern/static hygiene.

### Phase 2 — Vulkan / OpenGL / GLES backend bodies

Goal: the GL/GLES/Vulkan backend TUs gain real implementations, gated behind CMake `ENABLE_VULKAN` and `ENABLE_OPENGL` options that default **OFF**. Default builds are still identical to main — the new code only activates when a downstream target opts in.

Each phase should land as its own PR so review and rollback are cheap.

---

## Phase 1 — Detailed steps

### 1. Prep

```bash
git fetch origin
git checkout full-gfx-backends-refactor
git pull --ff-only     # no-op if not yet pushed; otherwise fast-forward
```

Open these in parallel buffers as reference (read-only):

- `git show a089149` — the template layout you are reproducing against a newer base.
- `git show a089149:native~/displayxr_hooks_internal.h`
- `git show a089149:native~/displayxr_standalone_internal.h`
- `git show a089149:native~/CMakeLists.txt`

### 2. Create the two internal headers

New files, copied from the branch and adjusted for current `main`:

- `native~/displayxr_hooks_internal.h`
- `native~/displayxr_standalone_internal.h`

These publish, to the per-backend TUs, anything that used to be `static` inside the monolith:

- All `s_real_*` function pointers.
- All graphics-binding struct definitions inlined by main (`XrGraphicsBindingD3D11KHR`, `XrSwapchainImageD3D11KHR`, etc.).
- The `D3D11ScSub` / `s_sc_subs[]` / `s_sbs_sc` state from the `2a9a933` cherry-pick.
- The N-view tile atlas state from `b7e2d22` — see the critical decision below.
- Shared helpers: `displayxr_log`, any path/format utilities, the SBS/tile composite entry points.

Start from the branch version, diff against current main's monolith, and add the post-`7aa2c28` additions.

### 3. Create the per-backend TUs (skeletons first)

Hook chain side:

| New file | Content |
|---|---|
| `native~/displayxr_d3d11_backend.cpp` | Current `main`'s `#if defined(_WIN32) && d3d11` sections from `displayxr_hooks.cpp`: typed-swapchain sub, SBS/tile atlas composite, D3D11 device capture, `d3d11_sub_cleanup_all`. |
| `native~/displayxr_d3d12_backend.cpp` | D3D12 device capture + anything D3D12-specific in the hook chain. |
| `native~/displayxr_metal_backend.cpp` | Metal-specific hook-chain bits (currently minimal; most Metal work lives in `displayxr_metal.m`). |
| `native~/displayxr_vulkan_backend.cpp` | Empty skeleton guarded by `#if defined(ENABLE_VULKAN)`. Phase 2 fills it. |
| `native~/displayxr_opengl_backend.cpp` | Empty skeleton guarded by `#if defined(ENABLE_OPENGL)`. Phase 2 fills it. |
| `native~/displayxr_opengles_backend.cpp` | Empty skeleton guarded by `#if defined(ENABLE_OPENGL) && defined(ANDROID/__ANDROID__)`. |

Standalone side:

| New file | Content |
|---|---|
| `native~/displayxr_standalone_d3d11.cpp` | D3D11 portions of main's standalone (atlas bridge D3D11 source, D3D11 blit). |
| `native~/displayxr_standalone_d3d12.cpp` | D3D12 device/queue, atlas bridge, cross-device blit (the bulk of current `displayxr_standalone.cpp`'s `_WIN32` path, from `2a84927`, `a7c91dc`, `caf6de8`). |
| `native~/displayxr_standalone_metal_backend.cpp` | Metal standalone surface / IOSurface plumbing (thin wrapper — heavy lifting stays in `displayxr_standalone_metal.m`). |
| `native~/displayxr_standalone_vulkan.cpp` | Skeleton, Phase 2. |
| `native~/displayxr_standalone_opengl.cpp` | Skeleton, Phase 2. |
| `native~/displayxr_standalone_opengles.cpp` | Skeleton, Phase 2. |

After this step, both `displayxr_hooks.cpp` and `displayxr_standalone.cpp` should shrink dramatically — they should retain only the platform-agnostic dispatch glue (hook installation, shared state, the `xrGetInstanceProcAddr` router, top-level lifecycle) and delegate to backend entry points defined in `*_internal.h` and implemented in the backend TUs.

### 4. CMakeLists.txt

Merge the branch's CMake additions onto current main:

- Add `ENABLE_VULKAN` and `ENABLE_OPENGL` options (both default `OFF` on Win/macOS; `ON` for `ENABLE_VULKAN` on Android/Linux per the branch's default).
- Add `find_package(Vulkan)` gated by `ENABLE_VULKAN`.
- Add OpenGL find plumbing gated by `ENABLE_OPENGL`.
- Add every new `*.cpp` to `SOURCES` (backends are guarded internally by `#if`, so unconditionally listing them is fine and matches the branch).

Do **not** regress the main-side CMake: keep the existing Metal source-file properties block, the Windows link libraries (`d3d11 d3d12 dxgi`), and anything added by `2a84927` / `a7c91dc`.

### 5. Critical decision — where does the N-view tile atlas live?

`b7e2d22` generalized the SBS composite in `hooked_xrEndFrame` into an N-view tile atlas. That code is D3D11-specific today (uses `ID3D11DeviceContext::CopySubresourceRegion`) but is architecturally "the compositor for every hooked backend that ever needs to tile N eye textures into one SR-friendly output."

**Pick one** before starting the split:

- **Option A — Keep it D3D11-only, inline in `displayxr_d3d11_backend.cpp`.** Simpler, no abstraction. When Vulkan/GL need their own tiler in Phase 2, they duplicate the algorithm against their own APIs. Acceptable if we don't expect frequent changes to the tile layout.
- **Option B — Extract the *algorithm* (view-layout computation, rect patching, view-restoration loop) into a small backend-agnostic helper, and leave only the `Copy…` call per-backend.** Cleaner long term because each backend only owns its copy primitive. More refactor work in Phase 1.

**Recommendation: Option A** for Phase 1. It keeps the split mechanical and minimizes risk to the D3D11 path that was painful to get working. If Phase 2 shows the algorithm duplicated 3×, revisit in a Phase 2.5 cleanup.

### 6. Build and test the split

```bash
# macOS
native~/build-mac.sh

# Windows
native~\build-win.bat

# Windows cross-check from macOS
native~/build-win.sh
```

All three must succeed. Then run the standard `DisplayXR-test` verification sweep:

- Windows D3D11 edit-mode preview → no X-pattern, no regression vs. `2a9a933` + `b7e2d22`.
- Windows D3D12 edit-mode preview → atlas bridge path works.
- Windows Play Mode → same.
- macOS edit-mode preview → Metal path unchanged.
- macOS Play Mode → same.

**This is a pure refactor PR. If any of those show a behavior diff, the split is wrong — do not ship it with a "small fix."**

### 7. Commit & push Phase 1

```bash
git add native~/ Runtime/Plugins/
git commit -m "Refactor native plugin into per-backend translation units (Phase 1)"
git push -u origin full-gfx-backends-refactor
```

Open a PR labeled `refactor` with a description that makes reviewability explicit: "pure code motion, no behavior change, validated on Windows D3D11/D3D12 and macOS Metal."

---

## Phase 2 — Detailed steps

### 1. Start from Phase 1 merged (or rebase onto it if still in flight)

```bash
git fetch origin
git checkout full-gfx-backends-refactor   # or a follow-up branch
git pull --ff-only
```

### 2. Port Vulkan backend bodies from `98a6403`

The target files already exist as skeletons from Phase 1. Cherry-pick the file contents directly:

```bash
git show 98a6403:native~/displayxr_vulkan_backend.cpp       > native~/displayxr_vulkan_backend.cpp
git show 98a6403:native~/displayxr_standalone_vulkan.cpp    > native~/displayxr_standalone_vulkan.cpp
```

Then reconcile by hand:

- The branch version includes the same skeleton Phase 1 created, plus the implementation. Overwriting is safe if the Phase 1 skeleton is identical to `a089149`'s version; otherwise diff and merge manually.
- The Vulkan bodies reference extern declarations from `displayxr_hooks_internal.h` / `displayxr_standalone_internal.h`. Make sure those declarations exist (should, since Phase 1 copied them verbatim from the branch).
- Guard every implementation file with `#if defined(ENABLE_VULKAN)` top-level — Phase 1 skeletons already do this, and the `98a6403` bodies expect it.

### 3. Port OpenGL / GLES backend bodies

Same mechanic:

```bash
git show 98a6403:native~/displayxr_opengl_backend.cpp       > native~/displayxr_opengl_backend.cpp
git show 98a6403:native~/displayxr_opengles_backend.cpp     > native~/displayxr_opengles_backend.cpp
git show 98a6403:native~/displayxr_standalone_opengl.cpp    > native~/displayxr_standalone_opengl.cpp
git show 98a6403:native~/displayxr_standalone_opengles.cpp  > native~/displayxr_standalone_opengles.cpp
```

Guard with `#if defined(ENABLE_OPENGL)` (and `defined(__ANDROID__)` or similar for the GLES variant where the branch did so).

### 4. CMake — wire the Vulkan/OpenGL link libraries

Phase 1 added the `option()` and `find_package()` calls but skeletons didn't need link libs. Phase 2 bodies do:

- Under `if(ENABLE_VULKAN AND Vulkan_FOUND)`: `target_link_libraries(displayxr_unity PRIVATE ${Vulkan_LIBRARIES})`.
- Under `if(ENABLE_OPENGL)`: link `OpenGL::GL` (desktop), or GLES libs on Android/Linux per the branch.
- Cross-reference `git show 98a6403:native~/CMakeLists.txt` for the exact incantation — the branch was tested against Android-style GLES which has platform-specific find logic.

### 5. Build with the new options enabled

```bash
# On macOS, Vulkan on via MoltenVK (if installed) — mainly a compile check
cmake -S native~ -B native~/build-vk -DENABLE_VULKAN=ON
cmake --build native~/build-vk --config Release

# On Windows, Vulkan on via Vulkan SDK
cmake -S native~ -B native~/build-vk -DENABLE_VULKAN=ON -G "Visual Studio 17 2022"
cmake --build native~/build-vk --config Release
```

This is *compile validation only* — we do not have a runtime to validate GL/GLES/Vulkan backends against yet. That is a separate engineering effort (setting up a DisplayXR runtime on Android/Linux). The deliverable for Phase 2 is: **code compiles under `ENABLE_VULKAN=ON` and `ENABLE_OPENGL=ON` on all hosts that have the SDKs available, and default builds (both options OFF) are bit-identical in behavior to Phase 1.**

### 6. Default-build regression sweep

Re-run the full Windows (D3D11/D3D12) + macOS (Metal) verification from Phase 1 step 6. Since Phase 2 only adds guarded new files, the default build must remain unchanged. If it isn't, something leaked out of an `#if`.

### 7. Commit & push Phase 2

Two commits (mirroring the branch) is fine:

```bash
git commit -m "Add Vulkan / OpenGL / GLES backend bodies (Phase 2)"
git commit -m "CMake: wire ENABLE_VULKAN / ENABLE_OPENGL link libraries"
git push
```

Or one squashed commit — either works. Open the second PR.

---

## Critical Files

- `native~/displayxr_hooks.cpp` — current monolith; Phase 1 shrinks it to dispatch glue only.
- `native~/displayxr_standalone.cpp` — same, for standalone session.
- `native~/CMakeLists.txt` — gains `ENABLE_VULKAN`, `ENABLE_OPENGL`, and the full backend file list.
- `native~/displayxr_hooks_internal.h` — **new**, created in Phase 1.
- `native~/displayxr_standalone_internal.h` — **new**, created in Phase 1.
- All new `displayxr_*_backend.cpp` and `displayxr_standalone_*.cpp` — **new**, created empty in Phase 1, filled in Phase 2.
- Reference commits (read-only during work):
  - `git show a089149` — the refactor template.
  - `git show 98a6403` — the Vulkan/GL/GLES bodies.
  - `git show 2a9a933` — the D3D11 typed-swapchain fix that must survive the split.
  - `git show b7e2d22` — the N-view tile atlas that must survive the split.
  - `git show a7c91dc` — the D3D12 atlas bridge that must survive the split.
  - `git show caf6de8` / `git show 2a84927` — Windows preview window lifecycle, must survive the split.

## Verification (end-to-end, per phase)

Test project: `DisplayXR-test` (Unity 2022.3+, `com.unity.xr.openxr` ≥ 1.16.1).

**Phase 1 — pure refactor, no behavior change:**

1. **Windows, Graphics API = D3D11.** Edit-mode preview: clean stereo, no X-pattern. Play Mode: same, input works (WASD + native mouse + Tab). Log check: `[DisplayXR] Typed swapchain paired: …` and `[DisplayXR] SBS/tile composite OK` still fire.
2. **Windows, Graphics API = D3D12.** Edit-mode preview: atlas bridge path works, clean stereo. Play Mode: same. Move/resize preview window: phase-snaps on release.
3. **macOS, Metal.** Edit-mode preview: clean stereo. Play Mode: same. IOSurface path unchanged.
4. **Editor log diff vs. pre-refactor `main`.** Save `Editor.log` from a clean run on main, then again on Phase 1 — they should be line-for-line identical modulo timestamps and the "Loaded X.dll" lines.
5. **CI green on both Windows x64 and macOS Universal** (`.github/workflows/build-native.yml`).

**Phase 2 — default build regression + opt-in compile:**

6. All of 1–5 again, on a default Phase 2 build (`ENABLE_VULKAN=OFF`, `ENABLE_OPENGL=OFF`). Must be identical to Phase 1.
7. `cmake -DENABLE_VULKAN=ON` compile check on Windows and macOS.
8. `cmake -DENABLE_OPENGL=ON` compile check on Windows and macOS.
9. Optional, if an Android NDK + SDK is handy: `cmake` cross-compile for `arm64-v8a` with `ENABLE_VULKAN=ON -DCMAKE_SYSTEM_NAME=Android` — aspirational, the branch claimed this builds.

## Failure modes to watch for

- **Linker unresolved externals after Phase 1 split** → a `static` helper in the old monolith was moved into one backend TU but is called from another. Fix by publishing it via `displayxr_*_internal.h` and dropping `static`.
- **D3D11 X-pattern returns on Phase 1 D3D11 test** → the `D3D11ScSub`/`s_sc_subs` state was not moved intact into `displayxr_d3d11_backend.cpp`, or the `xrEndFrame` SBS/tile composite block was not moved with it. Compare the resulting backend file against `git show 2a9a933` and `git show b7e2d22`.
- **D3D12 atlas bridge broken after Phase 1** → the cross-device blit from `a7c91dc` + `2a84927` lives partly in `displayxr_standalone.cpp` and partly in device-capture hooks. Make sure both halves end up in the same TU (`displayxr_standalone_d3d12.cpp`) or that the standalone dispatcher knows to call into the new TU.
- **macOS builds but preview window is empty** → the Metal IOSurface bridge was split across `displayxr_metal_backend.cpp` / `displayxr_standalone_metal_backend.cpp` and one of them lost the `#if defined(__APPLE__)` guard or the Objective-C bridge call.
- **Phase 2 builds fine but default builds now pull in Vulkan headers** → a skeleton file is missing its top-level `#if defined(ENABLE_VULKAN)` guard, so it tries to include `<vulkan/vulkan.h>` unconditionally.
- **`ENABLE_VULKAN=ON` build on Windows fails with "Vulkan SDK not found"** → CMake `find_package(Vulkan)` didn't locate the SDK. Installer ships it to `C:\VulkanSDK\<ver>\`; setting `VULKAN_SDK` env var should fix it. Do not hard-code paths.
- **Circular include between `displayxr_hooks_internal.h` and `displayxr_standalone_internal.h`** → they should not include each other. Shared helpers (e.g. `displayxr_log`) belong in a third header or in `displayxr_shared_state.h`.

## Out of scope

- **Do NOT** bring any of `008f9e1..5fbfedb` from `full_gfx_apis`. Those are Windows windowed-mode / Game View overlay commits that are already superseded on `main` by `caf6de8`, `2a84927`, `a7c91dc`, `b7e2d22`.
- **Do NOT** try to validate the Vulkan / OpenGL / GLES backends against a real DisplayXR runtime as part of this work. No such runtime build exists yet on Android/Linux. Phase 2 delivers a compiling, guarded scaffold — runtime validation is a separate engineering effort.
- **Do NOT** rewrite any main-branch preview-window, D3D11 fix, N-view atlas, or play-mode code. The refactor is pure motion.
- **Do NOT** change the Metal `.m` files (`displayxr_metal.m`, `displayxr_standalone_metal.m`). They already contain the platform-specific Objective-C bridge; the new Metal backend `.cpp` files are thin wrappers that call into them.
- **Do NOT** touch the Windows preview drag / phase-snap "parked" behavior documented in `CLAUDE.md`'s Known Issues.
- **Do NOT** skip the N-view tile atlas decision (§ Phase 1 step 5). Picking wrong forces a second pass. Recommend Option A (D3D11-inlined) unless there is a strong reason otherwise.
