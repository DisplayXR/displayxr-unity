# Phase 1 Mac Finish — Metal Backend Alignment + Verify + Merge

## Context

Branch `full-gfx-backends-refactor` has PR #50 open against `main`. The Windows
side (D3D11 + D3D12, hooks + standalone dispatch) is complete and verified.
The macOS side needs one small fix before it compiles, then build + test + merge.

## What to do

### 1. Fix `native~/displayxr_standalone_metal_backend.cpp`

The file was scaffolded from the template commit `a089149` which assumed function
names that don't exist in main's `displayxr_standalone_metal.h`. Four methods
need trivial fixes — replace calls to nonexistent functions with no-ops:

```cpp
// create_shared_texture: replace body with:
bool create_shared_texture(uint32_t width, uint32_t height) override
{
    (void)width; (void)height;
    return true;
}

// destroy_shared_texture: replace body with:
void destroy_shared_texture() override {}

// get_shared_texture_native_ptr: replace body with:
void *get_shared_texture_native_ptr() override { return nullptr; }

// destroy: replace body with:
void destroy() override {}
```

**Why no-ops:** On the Metal SA path, there is no plugin-managed "shared texture" —
the runtime uses IOSurface via the session binding. These methods exist in the
`StandaloneGraphicsBackend` interface for the Windows D3D12 path but are dead
code on Metal. They just need to compile.

Also remove the `=== SCAFFOLD STATE ===` comment block at the top of the file
(lines 7-16) since the file is now aligned.

### 2. Build

```bash
native~/build-mac.sh
```

Must produce `Runtime/Plugins/macOS/displayxr_unity.bundle` with no errors.

### 3. Test

- Open DisplayXR-test project in Unity
- Metal → edit-mode SA preview Start → clean stereo
- Metal → Play Mode → clean stereo
- No regressions vs pre-refactor main

### 4. Commit + push

```bash
git add native~/displayxr_standalone_metal_backend.cpp Runtime/Plugins/macOS/
git commit -m "Align Metal SA backend with main's .m exports

Replace template-era function calls (displayxr_sa_metal_{create,destroy,
get_texture}) with no-ops — these methods are dead code on the Metal SA
path where IOSurface is managed by the runtime, not the plugin.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>"
git push origin full-gfx-backends-refactor
```

### 5. Merge PR #50

Rebase onto main:
```bash
gh pr merge 50 --rebase
```

## Hard rules

- Do NOT edit `displayxr_metal.m` or `displayxr_standalone_metal.m`
- Do NOT change any behavior — this is pure compile-fix
- If the build fails for any other reason, investigate — don't patch around it

## Files to touch

- `native~/displayxr_standalone_metal_backend.cpp` — the only edit
- `Runtime/Plugins/macOS/displayxr_unity.bundle` — rebuilt binary
