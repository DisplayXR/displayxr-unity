# Transparent overlay (issue #57) — handoff after second session

## Status

**Visual transparency: working end-to-end.** On Leia SR + Unity 6 + D3D12, a `displayxr-unity-test-transparent` build renders the rotating cube above the desktop with proper per-pixel alpha. No chroma fringe inside the cube body. Faint magenta fringe at silhouette / de-occluded edges (binary chroma-key limitation; only fixable with alpha-aware Leia weaver in `displayxr-runtime-pvt#190`).

**Right-click + drag the cube to move the window: working.** Confirmed on hardware in session 2. WM_RBUTTONDOWN on overlay anchors cursor + window position; WM_MOUSEMOVE while dragging calls SetWindowPos on Unity; existing parent_subclass_proc WM_MOVE handler repositions the overlay to follow.

**Mirror-render perf hoist: working.** `XRSettings.gameViewRenderMode = GameViewRenderMode.None` now lives in `DisplayXRFeature.OnSessionCreate`, applied to every DisplayXR Unity app on every session.

**Left-click handling: BROKEN, partial implementation in tree.** Session 2 dropped `WS_EX_TRANSPARENT` and added `WS_EX_NOACTIVATE` + WM_NCHITTEST hit-rect gating + WM_LBUTTONDOWN/UP/DBLCLK + WM_MBUTTONDOWN/UP forwarding via PostMessage to Unity. Two failure modes observed on hardware:

1. **Outside the cube silhouette: clicks no longer fall through to the desktop.** Root cause: `HTTRANSPARENT` from WM_NCHITTEST only re-routes within the same thread (per MSDN), not across processes. To pass clicks to other apps you need `WS_EX_TRANSPARENT`, which we removed. Removing it was a regression for click-through.
2. **Inside the cube silhouette: PostMessage'd WM_LBUTTONDOWN doesn't fire `OnMouseDown` on the cube.** Most likely cause: modern Unity reads mouse buttons from RawInput (WM_INPUT with raw button flags) rather than legacy WM_LBUTTONDOWN. PostMessage'd legacy messages get queued but Unity's input layer ignores them for button state. (Cursor position via GetCursorPos still works, which is why hover-based things would work, but button-down events don't.)

**Next steps for session 3 (left-click):**
- For (1) outside-silhouette pass-through: either re-add `WS_EX_TRANSPARENT` and find another way to capture inside-cube clicks (e.g. a small WS_CHILD without WS_EX_TRANSPARENT covering only the hit rect), OR keep WS_EX_TRANSPARENT off and manually forward outside-cube clicks via `WindowFromPoint(pt, CWP_SKIPINVISIBLE | CWP_SKIPTRANSPARENT)` + SendMessage to whatever's beneath. The first option is cleaner.
- For (2) PostMessage not triggering OnMouseDown: try `SendInput(INPUT_MOUSE, ...)` to inject actual hardware mouse events. SendInput goes through the OS's input queue and is what Unity's RawInput layer expects. Caveat: SendInput hits the cursor position globally — needs careful sequencing (move cursor, click, restore?) and may have weird interactions with the user's actual cursor.
- Worth testing: does `Application.runInBackground = true` + the existing PostMessage actually fire `OnMouseUp` even if not `OnMouseDown`? If asymmetric, that narrows down the diagnosis.

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

> **Note:** Tasks 2 and 3 below were completed in session 2. Task 1 was partially implemented and exposed a deeper issue — see "Next steps for session 3 (left-click)" at the top of this doc for the current state. The sketch below is preserved because the architectural reasoning (HWND topology, hit-rect, drag math) is still correct; only the assumption that PostMessage'd left-clicks would trigger OnMouseDown turned out to be wrong.

### Task 1: Click-through to Unity (so cube `OnMouseDown` fires)

The overlay currently has `WS_EX_TRANSPARENT` (every click passes through) and Unity is cloaked (so clicks that pass through don't land anywhere meaningful). Two design choices:

- **Option A (preferred):** drop `WS_EX_TRANSPARENT` from the overlay. The existing `parent_subclass_proc` already handles `WM_NCHITTEST` — returns `HTCLIENT` inside the cube's screen-space bounding rect (set per-frame by `displayxr_set_overlay_hit_rect` from the C# `LateUpdate`), `HTTRANSPARENT` outside. Inside the rect, the overlay receives clicks. Then the overlay's wndproc forwards them to Unity via `PostMessage` (need to translate screen→client coords for Unity).
- **Option B:** Keep `WS_EX_TRANSPARENT`, un-cloak Unity, accept the grey background back. Unity gets clicks directly. (User explicitly didn't want this — they want the desktop visible.)

Option A is right. Implementation sketch:
1. In `displayxr_get_app_main_view` transparent path, drop `WS_EX_TRANSPARENT` from the ex-style. The hit-test logic now exclusively governs click-through.
2. Add `WM_LBUTTONDOWN/UP`, `WM_RBUTTONDOWN/UP`, `WM_MOUSEMOVE` cases to `overlay_wnd_proc`. For each: translate the lParam (overlay client coords) to Unity's client coords (`ClientToScreen` on overlay → `ScreenToClient` on Unity), then `PostMessage(unity_hwnd, msg, wParam, MAKELPARAM(x, y))`.
3. Verify: scene with a cube + a script that logs `OnMouseDown`. Should fire when you click the cube body, not fire when you click outside (those clicks pass through to the desktop).

### Task 2: Right-click + drag the cube to move the window

User wants to drag the avatar by right-click + drag on the cube. Implementation:

1. In `overlay_wnd_proc`'s `WM_RBUTTONDOWN`: capture the mouse position, set a flag `s_dragging = 1`, `SetCapture(overlay)`.
2. In `WM_MOUSEMOVE` while `s_dragging`: compute delta from initial position, call `SetWindowPos(unity_hwnd, ...)` to move Unity (the existing `parent_subclass_proc` `WM_MOVE` handler will then reposition the overlay automatically).
3. In `WM_RBUTTONUP`: clear `s_dragging`, `ReleaseCapture()`.

Alternative: `PostMessage(unity_hwnd, WM_NCLBUTTONDOWN, HTCAPTION, ...)` to trigger the OS's standard window-drag behavior. Cleaner — Windows handles the drag loop, including snap-to-edge. But Unity is `WS_POPUP` so the OS drag may behave oddly.

Test: right-click on cube → drag → cube and Unity HWND move together; release → both stay where dropped.

### Task 3 (smaller, while you're in there)

Hoist `XRSettings.gameViewRenderMode = GameViewRenderMode.None` from the test project's `TransparentAutoSetup.cs` into `DisplayXRFeature.OnSessionBegin` (or wherever XR loader has finished init). Saves a flat re-render of Camera.main per frame for *every* DisplayXR Unity app, transparent or not. Mirror render is wasted work in normal DisplayXR builds anyway (covered by the opaque overlay) — should always be off.

## Open issues elsewhere

- `displayxr-runtime-pvt#190` — vendor request to Leia for alpha-respecting weaver. When that lands, plugin can drop magenta clear and use `chromaKeyColor = 0`; runtime relies on per-pixel alpha straight through, eliminating the silhouette fringe entirely.
- `displayxr-runtime-pvt#193` — app-driven "external drag" API for window movement with phase-snap. Required to eliminate the right-drag 3D stutter in transparent overlay mode (and to re-enable real-time Kooima during the standalone preview's parked SC_MOVE intercept). See CLAUDE.md "Known Issues" for the failure modes that block plugin-only solutions.
- `displayxr-unity#57` — open, will be closed when click-through and drag-by-cube land. Cross-referenced from runtime#191.
- `docs~/roadmap/d3d11-typeless-fix-plan.md` — Unity D3D11 TYPELESS engine bug fix plan. Not strictly needed for transparent overlay (since Unity 6 defaults to D3D12) but would unblock the D3D11 BitBlt path for the rare D3D11-only Unity build.

## Local commits (not pushed)

Three commits on `main`:
- `8c9b912` — initial visual transparency (session 1)
- `0893a01` — handoff doc (session 1)
- session 2 commit (right-drag + gameViewRenderMode hoist + partial click-through scaffolding) — see HEAD

User hasn't pushed yet — can push when ready. Test project (`displayxr-unity-test-transparent`) is not under git; relevant files there are `Assets/TransparentAutoSetup.cs`, `Assets/CubeClickLog.cs` (added session 2 for click-through verification), `Packages/manifest.json` (points at `file:../../unity-3d-display`), and the modified `ProjectSettings/ProjectSettings.asset` (D3D12 + Auto, currently the right setting).
