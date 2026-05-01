# Agent Prompt — D3D11 TYPELESS Fix (Windows machine)

> Paste everything below the line into a fresh Claude Code session running **on the Windows test machine** (the i7 + RTX 3080 Laptop with Visual Studio 2022 installed). Make sure the working directory is the local clone of `displayxr-unity`.

---

You are continuing work on the DisplayXR Unity plugin. Your job is to graft a known-working D3D11 rendering fix from a parallel branch onto a freshly created test branch, build the Windows native plugin, and verify it end-to-end on this Windows machine. **You are running on the only Windows machine with the test display attached, so you are the one who can actually validate this.**

## Background you need before touching anything

1. **Read the full plan first** — it has all the context, conflict-resolution hints, verification steps, and failure modes:
   - `docs~/roadmap/d3d11-typeless-fix-plan.md`
2. **Skim the project README/CLAUDE.md** for the build commands and the "Known Issues" entry on Windows preview drag/phase-snap (that behavior is parked and you should not touch it).
3. **The branch `d3d11-typeless-fix` already exists on `origin`** and is currently identical to `main`. Do not create it again — just check it out.

## What "the fix" is

A single commit on `origin/full_gfx_apis`: **`7fef4f5` ("working dx11")**. It only modifies `native~/displayxr_hooks.cpp` (plus the prebuilt DLL, which you will rebuild yourself).

Authoritative diff:
```bash
git show 7fef4f5 -- native~/displayxr_hooks.cpp
```

The fix solves issue #91 (D3D11 compositor X-pattern) by creating a parallel `R8G8B8A8_UNORM_SRGB` swapchain alongside Unity's TYPELESS one, routing Unity to render into the typed textures, and compositing both eyes into a single 2×-wide SBS swapchain inside `xrEndFrame`. Read the plan for the full mechanism — you will need to understand it to resolve merge conflicts intelligently.

**Do not pull in any other commits from `full_gfx_apis`.** The rest of that branch is a per-backend refactor that predates all of `main`'s preview-window work and would conflict massively. Cherry-pick `7fef4f5` and only `7fef4f5`.

## Concrete task list

Work through these in order. If something fails, diagnose with the failure-modes section in the plan before retrying.

1. **Sync and check out the branch.**
   ```bash
   git fetch origin
   git checkout d3d11-typeless-fix
   git pull --ff-only
   ```

2. **Cherry-pick the fix.**
   ```bash
   git cherry-pick 7fef4f5
   ```
   - You will get conflicts in `native~/displayxr_hooks.cpp`. Resolve them by following the "Steps" section of the plan (it lists exactly which functions are touched and what to watch for, especially in `hooked_xrReleaseSwapchainImage` and `hooked_xrEndFrame`).
   - Open `git show 7fef4f5 -- native~/displayxr_hooks.cpp` in another buffer as the source of truth.
   - **Drop the binary change** — restore the existing DLL from main:
     ```bash
     git checkout main -- Runtime/Plugins/Windows/x64/displayxr_unity.dll
     ```

3. **Build the Windows native plugin** from a Developer Command Prompt for VS 2022 (or any shell where MSVC is on PATH):
   ```bat
   native~\build-win.bat
   ```
   This produces the new `Runtime/Plugins/Windows/x64/displayxr_unity.dll`. If the build fails, fix the conflict resolution — do not work around build errors with shims.

4. **Finish the cherry-pick commit.**
   ```bash
   git add native~/displayxr_hooks.cpp Runtime/Plugins/Windows/x64/displayxr_unity.dll
   git cherry-pick --continue
   ```
   If `--continue` complains, fall back to:
   ```bash
   git commit -m "Cherry-pick 7fef4f5: D3D11 typed swapchain substitution (#91)"
   ```
   Use a NEW commit, do not amend.

5. **Push and let CI cross-check both platforms** (this catches any macOS regression — the fix should be a no-op there because it lives inside `_WIN32 && s_d3d11_device != nullptr`, but verify):
   ```bash
   git push
   ```
   Optionally run `/ci-monitor --watch-only` to watch the build, but **do not block on CI** — proceed to local testing in parallel.

6. **Run the end-to-end verification on this machine.** Follow steps 1–8 in the "Verification" section of `docs~/roadmap/d3d11-typeless-fix-plan.md`. Specifically:
   - Test project: `DisplayXR-test` (ask the user where it lives if you can't find it; on Mac it's at `/Users/david.fattal/Documents/Unity/DisplayXR-test`, on Windows it's likely under the user's home).
   - Force the project to **D3D11** in Player Settings.
   - Edit-mode preview should render cleanly with no X-pattern.
   - Play Mode should render cleanly, with input working (WASD, mouse, Tab cycle).
   - Move/resize the preview window — should phase-snap on release without new corruption.
   - Exit/re-enter Play Mode — no crash.
   - Check `%LOCALAPPDATA%\Unity\Editor\Editor.log` for the expected `[DisplayXR]` log lines listed in the plan.
   - Then flip the project to D3D12 and confirm no regression there either.

7. **Report results back to the user.** A short writeup is fine — what you saw, screenshots/photos if possible, any log excerpts that confirm the fix is taking the typed-swapchain code path. Mention any warnings or anomalies even if they didn't break the test.

## Hard rules

- **Do NOT** bring in any other commits from `full_gfx_apis`. Only `7fef4f5`.
- **Do NOT** create new files, refactor unrelated code, or "improve" anything outside the cherry-pick scope.
- **Do NOT** change macOS, D3D12, or non-Windows code paths.
- **Do NOT** push directly to `main`. All work stays on `d3d11-typeless-fix` until the user reviews.
- **Do NOT** force-push or rewrite history on a shared branch.
- If you can't resolve a conflict confidently, stop and ask the user — `displayxr_hooks.cpp` is sensitive code and a wrong merge here will silently corrupt rendering rather than crash.
- The "Known Issue" about Windows preview drag stutter (`CLAUDE.md`, `sa_wndproc`) is **parked**. Do not try to fix it as part of this task.

## Definition of done

1. `d3d11-typeless-fix` has exactly one new commit on top of `main`: a clean cherry-pick of `7fef4f5` with conflicts resolved.
2. The Windows DLL is rebuilt from source and committed.
3. Edit-mode preview and Play Mode both render cleanly under **D3D11** on this Windows machine — no X-pattern, no garbage geometry, no new crashes.
4. **D3D12** still works on this machine (no regression).
5. CI build on `d3d11-typeless-fix` passes for both Windows and macOS.
6. You have reported back to the user with what you observed and the relevant `[DisplayXR]` log lines confirming the typed-swapchain path was active.

Good luck. Read the plan file first.
