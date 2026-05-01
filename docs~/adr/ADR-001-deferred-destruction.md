---
status: Accepted
date: 2025-06-15
source: "DisplayXR/displayxr-unity#36"
---

# ADR-001: Deferred OpenXR Session/Instance Destruction

## Context

Unity's OpenXR loader calls `xrPollEvent` *after* `xrDestroyInstance` returns, through JIT-generated dispatch trampolines that reference runtime memory (code pages, dispatch tables, session/compositor objects). If we actually destroy the instance, those trampolines read freed pages and crash (SIGSEGV in `RenderPlayModeViewCamerasInternal`).

The Game View's `RenderToHMDOnly` repaint cycle continuously calls `ProcessOpenXRMessageLoop` -> `xrPollEvent` during the teardown sequence. Even closing the Game View window doesn't immediately stop the repaint cycle.

## Decision

Defer `xrDestroySession` and `xrDestroyInstance`:

1. **Mark as dead** (`s_session_alive = 0`, `s_instance_alive = 0`) so our hook guards reject further API calls
2. **Store the handles + function pointers** for later cleanup
3. **Return `XR_SUCCESS`** immediately without calling the real destroy
4. **Pin the runtime library** via `dlopen(RTLD_NODELETE)` on macOS to keep code pages mapped
5. **Execute the real destroys** at the start of the *next* instance lifecycle in `displayxr_install_hooks()`

Additionally, `displayxr_stop_polling()` nulls the `xrPollEvent` function pointer so the hook returns `XR_EVENT_UNAVAILABLE` immediately, breaking the repaint cycle's event loop.

## Consequences

- The runtime stays in memory between Play sessions (RTLD_NODELETE prevents unmap)
- The real destroy functions *must* be called at the next `install_hooks()` to prevent stale runtime state (session still alive on second Play → xrCreateSession fails)
- The `CloseAllGameViews()` call in `ExitingPlayMode` is a defense-in-depth measure — it stops the repaint cycle before `Deinitialize()` begins
- Function pointers are nulled in `hooked_xrDestroyInstance` as a safety net
