# D3D11 TYPELESS Fix — Plan

**Branch:** `d3d11-typeless-fix` (pushed, currently identical to `main`)
**Source of the fix:** `origin/full_gfx_apis`, commit `7fef4f5` ("working dx11")
**Target platform:** Windows, D3D11 graphics API
**Related issue:** #91 (D3D11 compositor X-pattern)

---

## Context

Recent `main` work (merged from `preview-own-window`) brought the Windows standalone preview window to parity with macOS:

- Plugin-owned HWND with `WS_EX_NOACTIVATE | WS_EX_TOPMOST`, `xrSetSharedTextureOutputRectEXT` on `WM_MOVE/WM_SIZE`.
- Atlas-bridge cross-device D3D12 blit (Unity device → SA device) for preview and play mode.
- Custom WndProc, native mouse tracker polled by `DisplayXRInputController`, Y-flip on eye-camera projection for both platforms.
- Play-mode auto-start, XR-loader swap, camera selection survives domain reload.

However, this Windows path is currently validated with **D3D12 only**. **D3D11 on `main` still has the X-pattern artifact (issue #91):** the runtime creates `R8G8B8A8_TYPELESS` color swapchains per the OpenXR D3D11 spec, Unity renders into them fine, but the SR compositor can't build valid SRVs from TYPELESS textures, so the weaver samples garbage → X-pattern corruption. `native~/displayxr_hooks.cpp` on `main` still has the old "known-broken" comment block in its `_WIN32` D3D11 section.

A parallel branch, `origin/full_gfx_apis`, contains a **working D3D11 fix** as commit `7fef4f5`. The rest of `full_gfx_apis` is a large per-backend refactor (`a089149` and later) that we **do not want** — it predates all of the preview-own-window work and would conflict massively. We want only the D3D11 fix grafted on top of today's `main`.

## The Fix (commit `7fef4f5`)

Touches only `native~/displayxr_hooks.cpp` (and rebuilds the Windows DLL). Strategy:

1. **Parallel typed swapchain.** In `hooked_xrCreateSwapchain`, for every Unity color swapchain, silently call the real `xrCreateSwapchain` again with `format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB (29)`. Track the pair in a small static array (`s_sc_subs[DISPLAYXR_MAX_SC_SUBS]`).
2. **Route Unity to the typed textures.** `hooked_xrEnumerateSwapchainImages` returns the typed swapchain's images so Unity builds RTVs from `UNORM_SRGB`, not TYPELESS. No proxy/COM wrapping (prior attempts caused TDR).
3. **Mirror acquire/wait/release** on both swapchains so the runtime state machine stays valid. `Release` on the typed swapchain is **deferred** (`release_pending = true`) because we still need to write into it during `xrEndFrame`.
4. **SBS composite in `hooked_xrEndFrame`.** The SR compositor always splits the submitted image at `width/2`, ignoring `imageRect`. Create a single `2 × eye_w` SBS output swapchain on first use, `CopySubresourceRegion` the left-eye typed texture to `[0, eye_w)` and the right-eye to `[eye_w, 2·eye_w)`, then patch both projection views to reference `s_sbs_sc` with the correct half-width rects. After `xrEndFrame` returns, restore the original view fields so Unity's bookkeeping stays intact.
5. **Cleanup.** `hooked_xrDestroySession` and `hooked_xrDestroyInstance` call `d3d11_sub_cleanup_all()`, which destroys all typed/SBS swapchains via a captured `s_real_destroy_swapchain` (hooked only for pointer capture, not behavior).

All additions are inside `#if defined(_WIN32)` and only activate when `s_d3d11_device != nullptr`, so D3D12 and macOS are untouched.

## Why cherry-pick, not merge

Merging/rebasing `full_gfx_apis` is **not** viable: its later refactor splits the native code into per-backend files (`displayxr_d3d11_backend.cpp`, `displayxr_hooks_internal.h`, etc.) that don't exist on `main`, and the pre-refactor state also predates every recent preview-window commit. Cherry-picking `7fef4f5` alone is clean and self-contained.

## Steps

1. **Start on the branch** (already created and pushed):
   ```bash
   git fetch origin
   git checkout d3d11-typeless-fix
   git pull --ff-only
   ```

2. **Cherry-pick the single fix commit.**
   ```bash
   git cherry-pick 7fef4f5
   ```
   Expect conflicts only in `native~/displayxr_hooks.cpp`:

   - The old "TYPELESS workaround attempts" comment block around the `_WIN32` static state is replaced by the new struct (`D3D11ScSub`, `s_sc_subs[DISPLAYXR_MAX_SC_SUBS]`) and helper declarations (`s_real_destroy_swapchain`, `d3d11_sub_cleanup_all`, `d3d11_sub_find`, plus `s_sbs_sc` / `s_sbs_textures` / `s_sbs_img_count`).
   - Additions inside `hooked_xrCreateSwapchain`, `hooked_xrEnumerateSwapchainImages`, `hooked_xrAcquireSwapchainImage`, `hooked_xrWaitSwapchainImage`, `hooked_xrReleaseSwapchainImage`, `hooked_xrEndFrame`, `hooked_xrDestroySession`, `hooked_xrDestroyInstance`, and the `xrGetInstanceProcAddr` dispatch (new `xrDestroySwapchain` case).
   - In `hooked_xrReleaseSwapchainImage`, the fix replaces the existing "flush before compositor reads" block with a typed-vs-unmapped path. **Keep the flush in the non-substituted branch** so the original render-sync fix is preserved.
   - In `hooked_xrEndFrame`, the fix wraps the existing overlay-layer logic in an `if (active_layers == 0) { pass-through } else { build extended layers }` structure, and adds the D3D11 SBS composite block + the post-`s_real_end_frame` "restore patched views" loop. When resolving: the `main`-branch overlay path (`layers[]` copy, `modified.layers`, delete) must live untouched inside the `else` branch.
   - **Drop the binary change** — do not let the cherry-pick overwrite `Runtime/Plugins/Windows/x64/displayxr_unity.dll`. Rebuild from source.

3. **Reset the stale binary** pulled in by the cherry-pick:
   ```bash
   git checkout main -- Runtime/Plugins/Windows/x64/displayxr_unity.dll
   git add native~/displayxr_hooks.cpp Runtime/Plugins/Windows/x64/displayxr_unity.dll
   ```
   (Second `git add` is only to mark the binary as resolved if Git flagged it during cherry-pick. If not flagged, skip.)

4. **Rebuild the Windows DLL locally on the Windows machine.**
   ```bat
   native~\build-win.bat
   ```
   Requires Visual Studio 2022 (or Build Tools) with "Desktop development with C++". Output: `Runtime/Plugins/Windows/x64/displayxr_unity.dll`.

5. **Finish the cherry-pick commit** (the build script updates the DLL — include it in the commit):
   ```bash
   git add Runtime/Plugins/Windows/x64/displayxr_unity.dll
   git cherry-pick --continue
   ```
   Or if `--continue` is fussy:
   ```bash
   git commit -m "Cherry-pick 7fef4f5: D3D11 typed swapchain substitution (#91)"
   ```

6. **Push for CI cross-check** (optional but recommended):
   ```bash
   git push
   ```
   This kicks off `.github/workflows/build-native.yml`, which builds both Windows x64 and macOS Universal and uploads artifacts — catches any macOS regression even though we expect the fix to be a no-op there.

## Critical Files

- `native~/displayxr_hooks.cpp` — the only source file the cherry-pick touches; expect all merge conflicts here.
- `Runtime/Plugins/Windows/x64/displayxr_unity.dll` — shipped binary, must be rebuilt by `native~\build-win.bat`.
- Reference diff (read-only while resolving conflicts):
  ```bash
  git show 7fef4f5 -- native~/displayxr_hooks.cpp
  ```
  This is the authoritative view of every line the fix changes.
- `CLAUDE.md` — "Known Issues" section describes the Windows preview drag/phase-snap parking, not directly related but useful context.

## Verification (end-to-end)

Test project: `DisplayXR-test` (typically cloned alongside this repo). Unity 2022.3+, `com.unity.xr.openxr` ≥ 1.16.1.

1. **Set Graphics API to D3D11.** `Edit ▸ Project Settings ▸ Player ▸ Other Settings ▸ Graphics APIs for Windows` → D3D11 first (remove D3D12 if present).
2. **Install the rebuilt plugin.** Local path package reference, or copy the new `displayxr_unity.dll` into `Runtime/Plugins/Windows/x64/` if the test project uses a git-URL install.
3. **Edit-mode preview.** `Window ▸ DisplayXR ▸ Preview Window ▸ Start`. Expect a clean stereo image on the lenticular display — **no X-pattern**, no geometric garbage.
4. **Play Mode.** Enter play. Expect the same clean image, working WASD, mouse-look via the native tracker, Tab cycling cameras.
5. **Move/resize the preview window.** Weaving should phase-snap on drag release (parked behavior from `caf6de8`), no new corruption.
6. **Exit and re-enter Play Mode.** No crash (existing deferred-destroy path from `08f2da7` owns lifecycle), no swapchain leaks.
7. **Editor log check** (`%LOCALAPPDATA%\Unity\Editor\Editor.log`):
   - `[DisplayXR] Typed swapchain paired: unity=… typed=…` for each color swapchain.
   - `[DisplayXR] SBS swapchain created: sc=… 3840x… imgs=…` on first frame.
   - `[DisplayXR] xrEndFrame: SBS composite OK` for the first ~4 frames.
   - No `FAILED` lines around swapchain creation.
8. **D3D12 regression check.** Switch the test project to D3D12, relaunch, verify the D3D12 path (which already works on `main`) is unchanged — the fix must be a no-op because `s_d3d11_device == nullptr` on that path.
9. **macOS regression check** (can defer to CI or a separate run on the Mac dev machine): run `native~/build-mac.sh`, start preview in the test project, confirm Metal behavior identical to `main`.

## Failure modes to watch for

- **Subsequent-frame garbage** → the `xrEndFrame` conflict resolution probably skipped the "restore patched views" loop, so Unity keeps writing to a stale `s_sbs_sc` handle.
- **Compositor layer stack empty** → the overlay-layer (`active_layers > 0`) branch got moved outside the `else`, so `s_real_end_frame` is called twice or never.
- **TDR / GPU hang on session teardown** → `d3d11_sub_cleanup_all()` not wired into both `hooked_xrDestroySession` and `hooked_xrDestroyInstance`, or `s_real_destroy_swapchain` not captured because the `xrDestroySwapchain` case in `xrGetInstanceProcAddr` was dropped.
- **Still X-pattern after fix** → Unity is likely still rendering into the TYPELESS swapchain. Check that `hooked_xrEnumerateSwapchainImages` takes the `sub != nullptr` branch (log `unity_sc=… → typed_sc=…`). If not, `xrCreateSwapchain` didn't capture the pair — look for `s_d3d11_device == nullptr` at swapchain creation time.

## Out of scope

- Do NOT bring in any of the per-backend refactor from `full_gfx_apis` (`displayxr_d3d11_backend.cpp`, `displayxr_hooks_internal.h`, `displayxr_standalone_d3d11.cpp`, etc.). Those exist only as a future direction; they are not on `main` and would collide with every recent commit.
- Do NOT rewrite any of the main-branch preview-window or play-mode code. The fix is purely additive inside `_WIN32` D3D11 paths.
- Do NOT change the macOS/Metal or D3D12 code paths.
