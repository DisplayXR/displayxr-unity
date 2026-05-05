# Transparent overlay (issue #57) — RESOLVED in v1.2.0

> **Status: closed.** Session 5 landed cross-process click-through end-to-end
> (Notepad caret, Explorer item selection, keyboard focus all work) plus
> scroll-to-resize for the overlay window and foreground-aware input gating
> so the cube doesn't keep responding to WASD while the user is typing in
> another app. Released as v1.2.0 (2026-05-04).
>
> The historical session notes below are kept for context — what was tried
> and ruled out across sessions 1–4 — and to document the full architecture
> for future contributors. **Skip to the "Resolution (session 5)" section
> for the final shipping design.**

## Resolution (session 5)

The remaining bug after session 4 — clicks routed past the overlay via
`WS_EX_TRANSPARENT` reach the underlying HWND but the OS never transfers
foreground to it, so Notepad's caret never appears — turned out to be a
fundamental Win32 limitation, not a routing problem. Verified empirically
with `WH_MOUSE_LL` instrumentation (`WindowFromPoint` correctly resolved to
`RichEditD2DPT pid=Notepad` in the LL hook log, but the click was visibly
inert). Same hover-passes-through-but-clicks-don't asymmetry the user
observed in early tests: hover doesn't need focus transfer, clicks do.

**The shipping design — Approach C ("catch and forward"):**

1. `WM_NCHITTEST` returns `HTCLIENT` for the entire overlay rect in
   transparent mode — the OS dispatches every click to us. The
   `WS_EX_TRANSPARENT` toggle in `displayxr_set_overlay_hit_active` is
   gone; that function now only updates `s_hit_active` for the wndproc.
2. `forward_click_to_underlying_window` does a two-stage resolution:
   - Stage 1: iterate top-level windows in z-order
     (`FindWindowExW(NULL, prev, ...)`), skip our overlay, our cloaked
     Unity HWND, anything cloaked / `WS_EX_TRANSPARENT`, **and any
     window in our process** (the runtime's DComp visual host
     `class=DisplayXRD3D11` is in our process — forwarding to it is a
     dead-end). Pick the first whose rect contains the cursor — that's
     the underlying app's top-level frame.
   - Stage 2: recursively descend with
     `ChildWindowFromPointEx(CWP_SKIPINVISIBLE | CWP_SKIPDISABLED |
     CWP_SKIPTRANSPARENT)` until no more children. That's the actual
     leaf control (e.g. Notepad's `RichEditD2DPT`, Explorer's
     `DirectUIHWND`).
3. `SetForegroundWindow(GetAncestor(target, GA_ROOT))` — SFW applies to
   top-level windows, calling it on a child HWND is undefined. We have
   permission via "process received the last input event" since the
   click came to our wndproc.
4. `PostMessage(deepest_leaf, msg, wParam, deepest_leaf_client_pt)` —
   converted to the leaf's own client coords, not the frame's.

WS_EX_TOPMOST is back (avatar stays visually on top); WS_EX_NOACTIVATE is
back (clicking the cube doesn't passively activate us — we activate
programmatically via `SetForegroundWindow(overlay)` from the wndproc on
cube-press only, so click-through to other apps doesn't bounce focus).

**Foreground-aware input gating** (so WASD doesn't keep moving the cube
while typing in Notepad):
- New native getter `displayxr_is_our_process_foreground()` — calls real
  OS `GetForegroundWindow` from the plugin DLL (its IAT isn't patched, so
  it bypasses the IAT-hooked-for-Unity version that always returns Unity's
  HWND).
- `DisplayXRInputController.Update()` early-returns when not foreground.
- Cube reclaims foreground when clicked: in
  `overlay_wnd_proc`'s cube-area press paths (`hit_active=1`), we call
  `SetForegroundWindow(s_overlay_hwnd)`. WS_EX_NOACTIVATE blocks
  click-driven activation, but programmatic SFW is allowed.

**Mouse-wheel scroll-to-resize**: `WM_MOUSEWHEEL` in `overlay_wnd_proc`,
transparent mode only, uniform scaling around current center, 10% per
WHEEL_DELTA notch, floor 400×400. Win32 routes `WM_MOUSEWHEEL` to the
focused window — so when you've click-through'd to Notepad and Notepad has
focus, scroll naturally goes to Notepad's document. No explicit foreground
gate needed in the wndproc.

**Raycast fix**: when the overlay is scroll-resized, its client size
diverges from `Screen.width/height` (which still reflects Unity's frozen
off-screen HWND). `TryBuildEyeRay` and `TryProjectBoundsToScreen` were
using `Screen.*` for cursor → NDC, which made every click after a resize
register as `hit_active=0`. New native getter `displayxr_get_overlay_size()`
exposes the overlay's actual client rect; both functions take it as a
parameter now.

**Diagnostic instrumentation kept in main** (cheap, lifetime-of-process):
- `WH_MOUSE_LL` global mouse hook, button events only, logs the OS's
  `WindowFromPoint(cursor)` resolution per click → `[LLMouse]` lines.
- `overlay_wnd_proc` entry log for button events showing the live
  `WS_EX_TRANSPARENT` bit + `hit_active` + `ll_seq_at_entry` for
  cross-correlation → `[OvlWnd]` lines.

Both were indispensable for diagnosing the foreground-transfer bug and
will save time on future input issues.

---

## Kickoff prompt for next session (historical)

> Continue the transparent-overlay work for displayxr-unity#57. The authoritative
> starting point is this handoff doc — read it cover-to-cover before touching code.
> Sessions 1–4 burned a lot of cycles on dead-end approaches; the "What NOT to try"
> section lists them.
>
> **Current state (session-4 HEAD on `main`):** Approach A (Unity off-screen) is
> implemented and verified by log readback. Cube hit-detection is correct in both
> single-rig and multi-rig scenes — clicks register on the cube body where the
> user perceives it, thanks to a cyclopean Kooima ray (`m_Feature.GetStereoMatrices`
> + averaged left/right view+projection → `Physics.Raycast`). The
> session-3 raycast bug and the multi-rig phantom-click-zone bug are both gone.
>
> **The remaining problem:** cross-process click-through still does not work
> reliably. With `WS_EX_TRANSPARENT` correctly toggled on the overlay
> (verified by exstyle readback log) and Unity confirmed off-screen at
> `(-32000, -32000)`, OS-level click delivery to underlying apps (e.g.
> Notepad placed behind the avatar) does not happen — caret never appears,
> typing doesn't reach the target. Hover routing through to desktop items
> works *partially / unreliably* — sometimes desktop hover-highlights, sometimes
> not. The `forward_click_to_underlying_window` Approach-C fallback is wired up
> in the wndproc but the previous log shows it never fires (overlay isn't
> receiving the click events to forward), suggesting `WS_EX_TRANSPARENT`
> is at least taking the overlay out of the hit-test path — yet the click
> doesn't appear in the underlying app either. Where the click goes is the
> central mystery to investigate.
>
> Your job: figure out where the click ends up after `WS_EX_TRANSPARENT`
> skips the overlay, and route it to the underlying app reliably.
> Recommended starting points are in the **"Click-through investigation
> playbook"** section below. Don't add code defensively — instrument first.

## Status — end of session 4

### What's working (verified on hardware, Leia SR + Unity 6 + D3D12)

- **Visual transparency**: cube renders above desktop with per-pixel alpha. Faint magenta silhouette fringe (only fixable by `displayxr-runtime-pvt#190`).
- **Cube click detection**: cyclopean Kooima ray (built by averaging left + right view+projection matrices from `m_Feature.GetStereoMatrices`) inverse-projects cursor → `Physics.Raycast` against `clickableRenderers`. Hit region matches the cube silhouette at the user's stereo-fused perceived position. No more disparity-driven offset.
- **Right-click drag the cube to move the window**: `overlay_wnd_proc` `WM_RBUTTONDOWN` captures, `WM_MOUSEMOVE` calls `SetWindowPos` on the overlay HWND directly (not Unity — Unity stays off-screen), `WM_RBUTTONUP` releases.
- **Approach A (Unity off-screen)**: in `displayxr_set_transparent_overlay` enable path we save Unity's pre-cloak rect, snap the overlay to that rect, then `SetWindowPos(unity_hwnd, NULL, -32000, -32000, w, h, SWP_NOZORDER | SWP_NOACTIVATE)`. `parent_subclass_proc` and `shell_subclass_proc` skip viewport pushes from Unity's WM_MOVE/WM_SIZE in transparent mode (the off-screen position is meaningless for the runtime); the overlay's own WM_MOVE/WM_SIZE handlers push viewport coords using its actual screen rect. On disable we restore Unity to the overlay's current rect (so the windowed app appears where the avatar last was). All verified in `displayxr.log` via readback (`Moved Unity main window off-screen: requested (-32000,-32000 ...) readback (-32000,-32000 ...)`).
- **Multi-rig works**: both `DisplayXRDisplay` and `DisplayXRCamera` rigs read the same Kooima matrices from shared state (single source of truth populated by the runtime), so they compute the same hit test and write the same `hit_active` value. No more last-writer-wins phantom click zones — multi-rig and single-rig behave identically.
- **Standard Unity input** (`Mouse.current.leftButton.wasPressedThisFrame`, `OnMouseDown`, EventSystem): works via per-frame `InputSystem.QueueStateEvent(Mouse.current, MouseState{...})` injection of polled cursor + button state. RawInput delivery to off-screen Unity is preserved (RawInput is by HWND target, not by cursor position).
- **Mirror-render perf hoist**: `XRSettings.gameViewRenderMode = GameViewRenderMode.None` in `DisplayXRFeature.OnSessionCreate` (covers all DisplayXR Unity apps, not just transparent).

### What's broken — the only remaining issue

**Cross-process click-through.** Clicks in magenta transparent zones don't reach apps behind the overlay. Hover sometimes routes through (desktop items hover-highlight) but not consistently. The mystery: `WS_EX_TRANSPARENT` toggle is verified to flip on/off correctly via exstyle readback, Unity is verified off-screen, the `forward_click_to_underlying_window` fallback (in `overlay_wnd_proc`) doesn't fire (no `forward_click:` log lines from previous tests) — meaning the overlay isn't receiving the click events to forward, but the click also doesn't appear at the intended underlying app. Where is the click going?

## Click-through investigation playbook

The next session should start by **instrumenting**, not adding code. Concrete steps:

1. **Confirm the click delivery destination empirically.** Open Notepad, position it precisely under the cursor location where the click will land (avatar at `(393, 0, 3234x2248)` approximately, per latest log — Notepad needs to be at that screen rect). Click in a magenta zone. If Notepad's caret moves and typing lands → `WS_EX_TRANSPARENT` does deliver to underlying apps cross-process; the previous "doesn't work" reports were testing on desktop wallpaper which doesn't show single-click feedback. Done — close the issue.

2. **If Notepad doesn't get the click**, install a global low-level mouse hook (`SetWindowsHookEx(WH_MOUSE_LL, ...)`) inside the plugin (load-time) and log every mouse-down event with `WindowFromPoint(cursor_pos)` resolved at the time of the click. The hook fires before any window's wndproc, so it logs the OS's hit-test result directly. Cross-reference with the overlay's wndproc log — if the LL hook says target = Notepad's HWND but Notepad doesn't respond, the issue is downstream of OS hit-test (UIPI? broken Notepad? topmost-overlay z-order interfering with foreground transition?). If the LL hook says target = our overlay despite `WS_EX_TRANSPARENT` being set, then `WS_EX_TRANSPARENT` isn't actually being honored and we need to investigate why (combination with other ex-styles? specific Windows version behavior?).

3. **Test with `WS_EX_NOACTIVATE` removed.** The handoff doc cited NOACTIVATE for keeping Unity foreground when the overlay was clicked, but with Unity off-screen and IAT-hooked GetForegroundWindow that may be redundant. NOACTIVATE may interfere with cross-process click delivery — without it, clicks delivered past us via `WS_EX_TRANSPARENT` should activate the underlying app cleanly.

4. **Test removing `WS_EX_TOPMOST`** as a probe. Topmost has special z-order semantics that might affect click routing. If dropping it fixes click-through, find a less-aggressive way to keep the overlay visually on top (e.g., just SetWindowPos to top of z-order on a regular cadence).

5. **Force the Approach C fallback by removing `WS_EX_TRANSPARENT` toggle entirely.** Comment out the toggle in `displayxr_set_overlay_hit_active`. The overlay always catches all clicks. `forward_click_to_underlying_window` then runs for every transparent-zone click and unconditionally synthesizes a `PostMessage` to the underlying window. This is the most deterministic path — if it works, ship it as the production approach. The downside is hover behavior: with no `WS_EX_TRANSPARENT` toggling, hover messages always hit the overlay too, blocking desktop hover-feedback. Could be fine for the avatar use case (hover over avatar = hover the avatar; avatar shouldn't pass hover through arbitrarily).

6. **Check whether `SetForegroundWindow` from `forward_click_to_underlying_window` works.** The function calls it on press messages but might silently fail due to Win32 foreground rules. Try the alternative: `BringWindowToTop` + `SetActiveWindow`, or use `AttachThreadInput` to get permission to call SetForegroundWindow.

### What NOT to try (already burned cycles in sessions 1–4):

- `WS_EX_TRANSPARENT` on Unity HWND — kills RawInput button delivery to Unity. (Session 3.)
- Strict active-rig gate alone with the OLD symmetric `Camera.ScreenPointToRay` — broke click detection because the active rig's projection misses the cube silhouette. (Session 3.) Fixed in session 4 by switching to cyclopean Kooima ray; gate could now be re-introduced safely if event-doubling becomes an issue.
- `WS_EX_LAYERED + LWA_COLORKEY` on Unity or overlay — flip-model swapchains bypass it (Win10+).
- `WS_EX_LAYERED` on the overlay alongside DComp — incompatible with `WS_EX_NOREDIRECTIONBITMAP`; would lose per-pixel alpha. Per MSDN, full hit-test transparency for `WS_EX_TRANSPARENT` requires `WS_EX_LAYERED`, but that's blocked here.
- `SWP_FRAMECHANGED` on the WS_EX_TRANSPARENT toggle — added as a "flush the OS hit-test cache" defensive measure, then reverted. The exstyle readback confirms `SetWindowLongPtr` alone is sufficient; SWP_FRAMECHANGED was suspected of regressing hover and removed.
- `forward_click_to_underlying_window` for `WM_MOUSEMOVE` — caused message floods on the underlying window when hover dipped into the wndproc. Restricted to button events only in session 4. Don't expand back to MOUSEMOVE without a strong reason.
- Adding `WS_CAPTION` to overlay so DefWindowProc modal drag works — drag-stutter problem, separate issue, also blocked on `displayxr-runtime-pvt#193` (external drag API).

## Test environment

- Test project: `C:\Users\Sparks i7 3080\Documents\GitHub\displayxr-unity-test-transparent\` (sibling of unity-3d-display, NOT under git).
  - `Assets/CubeTest.unity` — 2 rigs + Cube + animation.
  - `Assets/TransparentAutoSetup.cs` — auto-attaches DisplayXRTransparentOverlay to rig cameras, wires inline pointer-event log handlers, ensures Cube has BoxCollider.
  - `Assets/CubeClickLog.cs` — orphaned (used to be auto-attached via TransparentAutoSetup). Safe to delete.
  - `Assets/InputDiagnostic.cs` — orphaned. Safe to delete.
  - `Assets/DragRotateCube.cs` — auto-attached to Cube. Click → red, drag → rotate. Wired via `TransparentAutoSetup`.
- Build folder: `C:\Users\Sparks i7 3080\Documents\Unity\DisplayXR-Test-Transparent-Build\`.
- Plugin log: `displayxr.log` next to the exe.
- Unity log: `C:\Users\Sparks i7 3080\AppData\LocalLow\DisplayXR\DisplayXR-test\Player.log` (where `Debug.Log` lands).
- Display: 4K @ 250% DPI scaling. Camera.pixel = 1920x1080, Screen = 2498x2248 (window pixels).

## How the transparency stack works (read this before changing anything)

Three layers, each with a specific job:

```
Unity rendering ───► OpenXR eye swapchains ───► Runtime weaver ───► DComp swapchain ───► DWM ───► Screen
   (Camera.main         (xrCreateSwapchain,         (Leia SR D3D12       (FLIP_DISCARD +    (uses
    clears to magenta,   format=R8G8B8A8_UNORM,      display processor    PREMULTIPLIED      per-pixel
    renders cube on it)  alpha preserved)            interlaces L+R)      alpha mode)        alpha)
```

### Plugin side (`displayxr-unity` `main` HEAD)

- `Runtime/DisplayXRTransparentOverlay.cs` — MonoBehaviour. Defaults `chromaKeyColor = (1, 0, 1, 0)` (magenta). On `OnEnable`: flips camera clear flags to `SolidColor + chromaKeyColor`, calls `displayxr_set_transparent_overlay()` (window-style mutation), pushes a screen-space hit rect each `LateUpdate` for click-through.
- Static `RequestTransparentSession()` and `RequestChromaKey(Color)` — must be called from `RuntimeInitializeOnLoadMethod(SubsystemRegistration)` so the flags are set in shared state **before** `xrCreateSession` runs. The plugin's own `displayxr_state_init()` memset preserves these flags across the install-hooks reset.
- `Samples~/TransparentAvatar/` — in-package sample with programmatic setup script.
- `displayxr-unity-test-transparent` (sibling repo, NOT in git yet) — `Assets/TransparentAutoSetup.cs` wires the `SubsystemRegistration` calls AND sets `XRSettings.gameViewRenderMode = GameViewRenderMode.None` to suppress Unity's mirror blit (saves a flat re-render of Camera.main per frame; should be hoisted into `DisplayXRFeature` so every DisplayXR Unity app benefits, not just this test project).

### Wire format (`native~/displayxr_extensions.h`)

`XR_EXT_win32_window_binding` `SPEC_VERSION = 5`. Field-at-end struct:

```c
typedef struct XrWin32WindowBindingCreateInfoEXT {
    XrStructureType        type;
    const void            *next;
    void                  *windowHandle;          // HWND
    PFN_xrReadbackCallback readbackCallback;
    void                  *readbackUserdata;
    void                  *sharedTextureHandle;
    XrBool32               transparentBackgroundEnabled;  // v4: opt-in BitBlt (D3D11) / DComp (D3D12)
    uint32_t               chromaKeyColor;                // v5: COLORREF, post-weave alpha conversion
} XrWin32WindowBindingCreateInfoEXT;
```

Plugin populates both fields in two construction sites in `displayxr_hooks.cpp` (the `win32_inject_window_binding` helper used by D3D11/D3D12 backends, and the `xrCreateSession` chain-walking fallback).

### Runtime side (`displayxr-runtime-pvt` branch `feature/workspace-extensions-2C`, local-only)

When `transparentBackgroundEnabled = XR_TRUE`:
- D3D12: `comp_d3d12_target_create` switches to `IDXGIFactory2::CreateSwapChainForComposition` (HWND-less swapchain) + `FLIP_DISCARD` + `DXGI_ALPHA_MODE_PREMULTIPLIED`. Then `IDCompositionDevice → CreateTargetForHwnd(hwnd, TRUE) → CreateVisual → SetContent(swapchain) → SetRoot(visual) → Commit()`. Logs `Transparent HWND opt-in: DComp + flip-model swapchain (FLIP_DISCARD + PREMULTIPLIED, bc=3)`.
- D3D11: `comp_d3d11_target_create` switches to BitBlt swap effect (`DXGI_SWAP_EFFECT_DISCARD + ALPHA_MODE_UNSPECIFIED + bc=1`). Logs `Transparent HWND opt-in: BitBlt swapchain (DISCARD + UNSPECIFIED, bc=1)`. (Unity D3D11 is currently broken by an independent TYPELESS engine bug — see `docs~/roadmap/d3d11-typeless-fix-plan.md` — so this path is only useful for non-Unity D3D11 apps right now.)

When `chromaKeyColor != 0`: a post-weave shader pass runs between the weaver's output and the swapchain present. For matching pixels, outputs `(0, 0, 0, 0)` premultiplied; otherwise `(rgb, 1.0)`. Logs `Post-weave chroma-key conversion enabled: 0x00FF00FF`.

### HWND topology (the part that took the longest to get right)

In transparent mode, the plugin creates the overlay HWND as a **top-level, unowned `WS_POPUP` with `WS_EX_NOREDIRECTIONBITMAP`** (in `displayxr_get_app_main_view`):

| HWND | Style | What happens |
|------|-------|--------------|
| Unity main HWND | `WS_POPUP` (decorations stripped), no `WS_EX_LAYERED`, **cloaked** via `DwmSetWindowAttribute(DWMWA_CLOAK)` | Invisible to DWM, but receives input + Unity keeps rendering eye textures into OpenXR swapchains |
| **Overlay HWND** | `WS_POPUP + WS_EX_NOREDIRECTIONBITMAP + WS_EX_TRANSPARENT + WS_EX_TOPMOST + WS_EX_TOOLWINDOW`, owner = `NULL` (not Unity) | No DWM redirection surface — content comes purely from the runtime's DComp visuals. Per-pixel alpha works because nothing else touches it. Independent z-order. |

Three subtleties that bit us during development:
1. **Owner relationship matters.** If the overlay is owned by Unity (3rd arg of `CreateWindowExW`), cloaking Unity also hides the overlay. We pass `NULL` for owner in transparent mode so they're independent. (Opaque path keeps Unity as parent because `WS_CHILD`.)
2. **`find_unity_hwnd()` must skip our overlay class.** After the visible top-level overlay is created, it can win the foreground/visible-window race in the search, and we end up styling our own overlay instead of Unity's. The fix lives in `is_displayxr_overlay_class()`.
3. **`parent_subclass_proc` branches on `s_overlay_is_toplevel`.** `WM_SIZE` and `WM_MOVE` reposition the overlay in **screen** coords (transparent path) vs **client** coords (opaque path).

### Why we can't use simpler approaches

We tried each of these first and they all failed for documented reasons — don't repeat them:

| Approach | Why it failed |
|----------|---------------|
| `WS_EX_LAYERED + LWA_COLORKEY` on Unity's main HWND | Flip-model DXGI swapchains bypass `LWA_COLORKEY` on Win10+. DWM blits flip-model swapchain content directly, ignoring the layered-window key. |
| `WS_EX_LAYERED + LWA_COLORKEY` on the overlay (when overlay was a `WS_CHILD`) | Same flip-model bypass — runtime's DComp swapchain is also flip-model. Plus the parent's opaque content showed through anyway. |
| Layering both parent and child | Same as above + DComp got confused by the layered child surface, produced opaque-white background. |
| `XRSettings.gameViewRenderMode = None` alone (no other changes) | Suppresses the mirror render but leaves Unity's swapchain attached and the parent HWND opaque-black. Need the cloak too. |
| Cloaking Unity while overlay was **owned** by Unity | Owned popup follows owner's visibility. Both went invisible. |
| `WS_EX_NOREDIRECTIONBITMAP` on the child overlay (still `WS_CHILD`) | Even with no redirection on the child, the parent's opaque surface is still composited. Only useful when overlay is top-level. |

## Files to know

| File | Purpose |
|------|---------|
| `Runtime/DisplayXRTransparentOverlay.cs` | The user-facing component |
| `Runtime/DisplayXRNative.cs` | DllImport bindings — search for `transparent` |
| `native~/displayxr_extensions.h` | Embedded `XR_EXT_win32_window_binding` v5 header |
| `native~/displayxr_hooks.cpp` | Window-binding struct construction (two sites — both populate v5 fields). Also `displayxr_set_transparent_background` and `displayxr_set_transparent_chroma_key` C-side setters. |
| `native~/displayxr_shared_state.{h,cpp}` | `transparent_background_requested` + `transparent_chroma_key_color` flags; preserved across `displayxr_state_init` memset |
| `native~/displayxr_win32.c` | All the Windows-specific window magic. `displayxr_get_app_main_view` (top-level vs child branch), `displayxr_set_transparent_overlay` (style flip + cloak), `parent_subclass_proc` (WM_NCHITTEST hit-rect, WM_SIZE/MOVE tracking), `find_unity_hwnd` (skip overlay class) |

## Test reproducer

1. Open `displayxr-unity-test-transparent` (sibling dir of this repo) in Unity 6.
2. Scene: `Assets/CubeTest.unity`. Project Settings: D3D12 + Auto (default).
3. *File → Build Settings → Build*. Existing build folder: `C:\Users\Sparks i7 3080\Documents\Unity\DisplayXR-Test-Transparent-Build\`.
4. Make sure `Program Files\DisplayXR\Runtime\` has the v5 runtime (`displayxr-service.exe` and `DisplayXRClient.dll` timestamped 01:13 May 2 2026 or later).
5. Run the exe. Cube should float above the desktop with no rectangular background.

Logs:
- Plugin: `C:\Users\Sparks i7 3080\Documents\Unity\DisplayXR-Test-Transparent-Build\displayxr.log` — look for `set_transparent_background: requested=1`, `set_transparent_chroma_key: color=0x00FF00FF`, `Injecting win32 window binding: ... transparentBackgroundEnabled=1, chromaKeyColor=0x00FF00FF`, `Created overlay HWND ... TOP-LEVEL WS_POPUP + NOREDIRECTIONBITMAP (transparent)`, `Cloaked Unity main window via DWMWA_CLOAK`, and the `dx parent` / `dx overlay` diagnostic dump.
- Service: `%LOCALAPPDATA%\DisplayXR\DisplayXR_DisplayXR-test.exe.*.log` (most recent) — look for `Transparent HWND opt-in: DComp + flip-model swapchain` and `Post-weave chroma-key conversion enabled: 0x00FF00FF`.

`gh` access works — issue lives at `DisplayXR/displayxr-runtime-pvt#191`. Read the full thread for design decisions; the latest comment from May 2 is the runtime author confirming the post-weave shader fix landed.

## Next session — concrete tasks

> Tasks 1–3 from sessions 1–3 (left-click, right-drag, gameViewRenderMode hoist)
> are all DONE and shipped on `main`. The single remaining task is cross-process
> click-through — see the **"Click-through investigation playbook"** section
> near the top.

## Open issues elsewhere

- `displayxr-runtime-pvt#190` — vendor request to Leia for alpha-respecting weaver. When that lands, plugin can drop magenta clear and use `chromaKeyColor = 0`; runtime relies on per-pixel alpha straight through, eliminating the silhouette fringe entirely.
- `displayxr-runtime-pvt#193` — app-driven "external drag" API for window movement with phase-snap. Required to eliminate the right-drag 3D stutter in transparent overlay mode (and to re-enable real-time Kooima during the standalone preview's parked SC_MOVE intercept). See CLAUDE.md "Known Issues" for the failure modes that block plugin-only solutions.
- `displayxr-unity#57` — open, will be closed when click-through and drag-by-cube land. Cross-referenced from runtime#191.
- `docs~/roadmap/d3d11-typeless-fix-plan.md` — Unity D3D11 TYPELESS engine bug fix plan. Not strictly needed for transparent overlay (since Unity 6 defaults to D3D12) but would unblock the D3D11 BitBlt path for the rare D3D11-only Unity build.

## Local commits

The session 1–4 work landed as a sequence of commits on `main`:
- `8c9b912` — initial visual transparency (session 1)
- `0893a01` — handoff doc (session 1)
- `a72ca37` — right-drag + gameViewRenderMode hoist + partial click-through scaffolding (session 2)
- `40d9edc` — input architecture (session 3)
- session-4 commit (Approach A Unity-off-screen + cyclopean Kooima ray + click-forward fallback + this handoff update) — see HEAD

Test project (`displayxr-unity-test-transparent`) is not under git; relevant files there are `Assets/TransparentAutoSetup.cs`, `Assets/DragRotateCube.cs`, `Packages/manifest.json` (points at `file:../../unity-3d-display`), and `ProjectSettings/ProjectSettings.asset` (D3D12 + Auto).
