# Changelog

All notable changes to the DisplayXR Unity plugin will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [2.19.0] - 2026-09-06

### Added
- **Background-aware foreground clipping — `XR_DXR_depth_budget` (#318).** A transparent overlay used to throw away *every* pixel behind the display plane: content behind the plane carries positive disparity while being drawn *over* desktop pixels at zero disparity, and the eyes can't reconcile "it covers the icon" with "it is behind the icon". But that contradiction needs a background with a **horizontal** depth cue right there — text, icons, window edges. Over a plain wall of colour, a vertical gradient or horizontal stripes there is nothing to disagree with, and the rear of the model looks fine. The runtime now measures the desktop behind the app (off the capture the display's plug-in already takes for transparency) and publishes a ramped, advisory **rear depth budget**; the provider chains it on `XrViewState` at every `xrLocateViews` and adds it to the per-eye foreground far. So with an empty desktop behind it the avatar's back **grows in** after ~0.4 s, and when a text window moves behind it the clip **slides shut** in ~0.15 s. `foregroundOnlyClip` is unchanged as the app's opt-in — the budget only decides how much rear volume that clip currently allows.
  - **Honours both clip paths.** BiRP takes it through the native per-eye far (`dxr_prov_get_eye_clip`); URP's `DisplayXR/ForegroundClipURP` gets it as the new `_DXRRearOffset` global, because its shader derives each eye's display-plane distance itself (per eye *and* per zone) rather than taking ours. HDRP has no foreground-clip pass and is unaffected.
  - **`DisplayXRContentBounds`** — a new component for the content root (the avatar, the model). It unions the world bounds of its renderers each frame and hands the box to the provider, which projects it through every eye and reports the union as `XrContentBoundsDXR` on `xrEndFrame`, so the runtime measures only the desktop **behind the content**. Without it the runtime judges the whole window — an empty Notepad's own menu and status bars measure a cue of 0.93 and keep the budget shut even when the avatar is in the far corner. Strongly recommended for any transparent overlay; purely a hint, and every failure path (a corner behind an eye, a box that clamps off-canvas) falls back to the whole canvas rather than to "neutral".
  - **`DisplayXRDepthBudget`** — read-only `State` / `FarOffsetVH` / `CueEnergy` / `RearOffsetWorld` for HUDs, debugging and tests, plus one log line per state change (never per frame). Nothing to wire.
  - **Apply as delivered; do not smooth.** The runtime already time-ramps the value (~300 ms opening, ~150 ms closing), so the plane glides rather than pops; an app-side filter on top fights that ramp for a slower, less predictable plane.
  - **Nothing changes without it.** The extension is enabled only when the runtime enumerates it, and the offset is 0 — clip exactly on the plane, byte-for-byte today's behaviour — on an older runtime, on a runtime whose display processor supplies no background source, or for an **opaque** session. That last case is deliberate: the runtime's transparent flag is fixed at `xrCreateSession`, so a busy-background budget of 0 can arrive for a session that never composites over the desktop, and the app's own state has to win.
  - **Report only what occupies the rear volume.** The region is a *rectangle*, so every extra renderer widens the patch of desktop the runtime judges — and a ground plane or skybox is worse than merely wide: its AABB has corners behind the eye, which cannot be projected, so the provider conservatively falls back to the whole canvas. Measured on the panel, adding a floor quad to the list took a narrowed region straight to `roi=0,0,202,320 (whole preview)`. A rect is the granularity `XrContentBoundsDXR` offers today; it cannot express a silhouette, and a finer channel is a future-spec conversation.
  - **Skinned meshes report their real per-frame bounds** (`tightSkinnedBounds`, on by default). Unity's `SkinnedMeshRenderer.bounds` is the box baked at import to cover the *whole animation set* — for a humanoid that is arms-out, and it stays arms-out however the avatar is posed — so the reported region was a T-pose-wide rectangle around a character standing with its arms down, and busy desktop nowhere near the silhouette closed the clip. The component now sets `updateWhenOffscreen` on the skinned meshes it reports (restoring it on disable). Measured on the panel: `roi` width 163 → 146 of a 202-wide preview, ~68 window px of empty background dropped. Costs a per-frame bounds skin and disables frustum culling on those renderers, which is the right trade for the handful of avatars a transparent overlay shows; turn it off for a scene full of usually-off-screen skinned meshes.
  - **Zoned apps: the bounds are normalised to the app WINDOW, not to the zone** — the runtime's background preview is a downsample of the window's client rect. The provider projects in the zone's view, clamps to `[0,1]` and *then* rebases through the 3D zone rect into window space. The clamp has to come first: a skinned mesh's animation bounds routinely project outside their own frustum, and a negative `v` scaled by the zone rect reaches *above* the zone — into the 2D band of an avatar layout, so the runtime would judge the desktop behind the speech bubble instead of behind the model. Caught on the panel (`roi` top 96 against a zone starting at 119) and fixed before release.

## [2.18.4] - 2026-09-04

### Added
- **"Create Scene Content" button on the Basic Scene and URP Basic Scene samples** (#314). "Scene Setup" set up the Play session, not the scene: content was built in `Start()` and discarded on Stop, so nobody could move a cube or use the layout as a base. The inspector button authors the same cubes, floor and light as real scene objects (selectable, editable, saveable, one undo step), writes a material *asset* per colour into a `Materials/` folder beside the scene (an in-memory material would come back magenta on reload), then removes the setup component. Nothing authored is committed, so the shipped scenes stay pipeline-agnostic; Play without the button behaves exactly as before. Contributed by Byungju Lee.

### Fixed
- **Both basic-scene samples converged at infinity** (#314). `invConvergenceDistance` shipped as `0` — parallel projection — so cubes at 0.3/0.5/1.0 m all read as pure pop-out despite the script's own "at screen plane" / "behind screen" comments. Now `2` (plane at 0.5 m, the mid cube) in the shipped scenes and in both the button and runtime paths, applied only when the value is still 0 so a tuned scene keeps its value. Contributed by Byungju Lee.
- **Four sample scripts had no committed `.meta`** (#313) — `DisplayScene`, `FaceViewer`, `MinimalTransparent`, `TransparentAvatar` regenerated their GUIDs on every import, the same defect that kept the URP sample from having a scene (#308). Pre-emptive; nothing referenced them yet. Contributed by Byungju Lee.

## [2.18.3] - 2026-09-04

### Fixed
- **The URP Basic Scene sample now ships a scene to open** (#308, #310). `Samples~/URPBasicScene/` had no `.unity` file while `package.json` called it "same content as Basic Scene"; following the README instead (empty scene + `URPBasicSceneSetup`) showed cubes in Play and an empty scene on Stop, which reads as a broken import. The root cause was that the sample's scripts had **no committed `.meta` files**, so their GUIDs were regenerated on every import and no committed scene could reference them — the same gap #261 closed for the BiRP sample. `URPBasicScene.unity` is `BasicScene.unity` with one line changed (the setup script's GUID), so camera settings and structure match by construction; both `.meta` files are added; the README's Quick Start opens the scene and says plainly that the content is runtime-built and Play-mode only. Contributed by Byungju Lee.
- **The URP sample README no longer documents a projection fix that was removed in v2.2.0** (#309, #311). The "Off-axis projection fix" section described `KooimaProjectionFixFeature` and two menu items, none of which exist: the provider hands Unity a full per-eye projection matrix, so URP consumes the off-centre frustum natively and there is nothing to wire (and the feature must not come back — it would double-apply the projection). The section is deleted rather than rewritten, the HDRP paragraph loses its back-reference, and the June 2026 SPI experiment record is annotated. Contributed by Byungju Lee.

## [2.18.2] - 2026-09-03

### Fixed
- **The GPU-preference post-build processor no longer clobbers a deliberate per-machine `UserGpuPreferences` entry** (#306, #307). It re-stamped `GpuPreference=2` on every build, which silently broke a hybrid dev box pinned to `=1` to emulate an iGPU-only customer machine (Unity on the iGPU + `DXR_D3D_FORCE_GPU=scanout` on the runtime). An **existing entry is now kept** unless `DisplayXRGpuPreference.Target` is *explicitly* `Discrete`/`Integrated` (the no-settings-asset fallback counts as a default, not a declaration, and defers); an absent entry is written as before (fallback → Discrete for cross-adapter safety on a fresh box; `Auto` → nothing, Windows decides). **A deliberate per-machine pin therefore survives a rebuild by default** — no switch needed while `Target` is `Auto`/unset. The opt-out (`DisplayXRGpuPreference.StampRegistryOnBuild = false` or `DISPLAYXR_GPU_PREF_NO_STAMP=1`) is only for a project whose `Target` is set explicitly and still wants a per-machine override to win. One `[DisplayXR] GpuPreference:` line per build says which branch was taken. Behaviour change to know: `Auto` used to *delete* a leftover entry; it now keeps it (it cannot tell a stale pin from a human's) — a box stamped by an older build may need a one-time manual clear, visible in that log line.

## [2.18.1] - 2026-09-03

### Fixed
- **Transparent overlay: keyboard no longer dies after clicking away and back** (#304). The overlay is `WS_EX_NOACTIVATE`; on a press it foregrounded *itself*, which keeps the process foreground but never activates Unity's cloaked window. That was fine while Unity was still active from launch (the #270 subclass rewrites the deactivation our own overlay causes), but after a real switch to another app — deliberately let through since #270 so the window can be minimised — nothing re-activated Unity: a click back on the avatar gave Unity no `WM_ACTIVATE`, `Application.isFocused` stayed false and every key (WASD, Space, Esc) was dead for the rest of the session, while Alt+Tab, which activates Unity's window directly, restored them. A press now activates **Unity's HWND** (`overlay_claim_keyboard`), falling back to the overlay only if that HWND is unknown or refused; at launch, with Unity already active, it is a no-op. Verified on desktop-avatar with a control: after click-away/click-back the foreground window is Unity's with the fix and the overlay's without it. Field-found on a shipping avatar app; no app-side workaround is needed (one would race the plugin's focus management).

## [2.18.0] - 2026-09-03

### Added
- **`XR_DXR_display_info` v18 — the window move is gated on `isPanelConfirmed`** (#266, #287). The provider now chains the runtime's v18 `XrDisplayDesktopInfoDXR` (desktop rect, device name, `isPrimary`, `isPanelConfirmed`) alongside the v16 desktop-position struct. `DisplayXRTargetDisplay` moves the app window onto the panel only when the runtime *confirms* it resolved a real panel — so `sim_display` and non-DisplayXR runtimes (which report a fallback-to-primary rect) no longer drag the window somewhere it doesn't belong. Against a pre-v18 runtime the v16 path is used unchanged and the move keeps its previous behaviour. **Minimum runtime is unchanged** (the declared `XR_DXR_display_info` spec version is a floor and every path degrades); the v18 positive path was hardware-verified against runtime v2.16.2-class bits (`confirmed=1`, device name and rect deserialised correctly, so the wire layout matches the runtime's writer).
- **Cloak / un-cloak / revert log lines carry `(t+NNNN ms)` since process start, and the backstop timeout is overridable** (#296, #300). The 20 s early-cloak backstop is only safe when the overlay comes up faster than that on the *customer's* hardware, and Player.log has no timestamps — so `[DisplayXR] Cloaked Unity main window via DWMWA_CLOAK … (t+5231 ms)` now answers "how long did the overlay take on that machine" from any post-hoc log, no harness needed. `DISPLAYXR_EARLY_CLOAK_BACKSTOP_MS` (milliseconds, ≥ 1000; set in the process environment before launch) overrides the default, and the pre-cloak line prints the timeout in force. The `Created overlay HWND` line is stamped too — on the pre-cloak path (every transparent app) that is the time-to-overlay number, since the later "Cloaked Unity main window" line does not fire there.

### Fixed
- **`tools~/visibility_check.ps1` reported a correct small window as INVISIBLE on a high-DPI panel** (#295). The gate never pinned DPI awareness, so a visible 320×240 physical window read 128×96 on a 250 % panel and tripped the size conjunct; (this trap fails in the *safe* direction — a false FAIL, unlike the false-PASS traps the gate exists for — but a gate that cries wolf gets ignored, which ends the same way); it now pins `PER_MONITOR_AWARE_V2`, dumps every window it saw on FAIL (so the failing conjunct is visible), parses under PowerShell 5.1 (ASCII + BOM), and documents that `-GuardLog` must point at `Player.log` (the #295 guard is C#).

## [2.17.0] - 2026-09-03

Every fix here was reproduced and verified on real hardware (a Leia panel, mixed-DPI rig, and the desktop-avatar / birp-multipass samples), not on a maintainer's machine. Two are follow-through on the transparent-app invisibility regression that v2.16.2 first guarded.

### Fixed
- **A transparent app can no longer be left invisible when a session never fully comes up** (#296, completing #295). v2.16.2 added a guard that skips the pre-cloak when no runtime is *provably* resolvable; this adds the durable cure for the cases the guard can't see (a runtime that resolves but whose session then fails). The pre-cloak's revert now **un-cloaks AND un-parks** Unity's window — the old revert un-cloaked only, leaving it parked off-screen and still invisible — and a 20 s timeout backstop reverts if no overlay ever materialises, the #256 design applied to the pre-cloak's earlier call site. The backstop abstains while a live overlay exists, so a healthy transparent session (where Unity's window stays cloaked on purpose) is untouched. Hardware-verified: session-fails reverts in ~422 ms, a healthy session's timer abstains, and the no-runtime guard path is unaffected.
  - **Slow-start note:** if the overlay materialises *after* the 20 s revert (a very slow session start), the normal overlay-up path re-cloaks and re-parks Unity's window unconditionally, so the final state is still correct — the only cost is a brief flash of Unity's opaque window between the revert and the overlay. The timer does not re-arm. The relevant clock is time-to-overlay-window-created (early in the overlay setup), not time-to-steady-state; 20 s exceeds the heaviest measured startup (12–16 s for a heavy avatar).
- **The camera-centric rig keeps its authored field of view across scene loads** (#274). `DisplayXRCamera` cached the authored FOV by reading `Camera.fieldOfView` in `OnEnable`, but on a scene loaded while a session is live, Unity's XR has already stamped the tracking-derived FOV onto the camera — so the cache seeded from a polluted value and the FOV walked every visit (a `Screen Space - Camera` canvas then rendered progressively wrong). The authored value is now serialized on the component and seeded from there. Scenes authored before this fix capture it on the next editor save; the rig logs once if it has to fall back. Hardware-verified: authored FOV held constant across six scene reloads while the raw camera FOV walked.

### Changed
- **`DisplayXRWindowSpaceUI` renders at the live panel aspect by default** (#291, `matchPanelAspect`). The overlay RT is created at the panel's own aspect (authored height, derived width) so the runtime's stretch into the panel rect is the identity — circles stay round on every compositor path, including the editor weave window / Game view pane where the old pre-distortion did not cancel. The RT (and its overlay swapchain + cross-device bridge) is re-created, debounced, when the window aspect changes; input routers map into the new `OverlayResolution`. The router also disables an `InputSystemUIInputModule` that would act as a mirrored second pointer. Contributed by Byungju Lee.
  - **Upgrade note — this is default-on and changes the appearance of every existing wsui app on upgrade.** The previous behaviour is one inspector toggle away, no code change: set `matchPanelAspect` to **false** on the component and the RT stays at its authored fixed resolution with the old camera pre-distortion. An app that authored its RT for a specific aspect keeps it that way.
- **The Local2D overlay can render only when its content changes** (#244, `renderOnlyWhenContentChanges`, opt-in). Registers Unity's Graphic dirty callbacks on the canvas so a static speech bubble / HUD renders zero times per frame instead of every frame; runtime-spawned children are re-tracked so they are not silently left stale. Hardware-verified: static content 0 renders/sec, a text change triggers exactly one. Off by default.

### Added
- **A pre-release visibility check** (`tools~/visibility_check.ps1`) and a required hardware step in the release process (#295). Asserts a transparent app on a no-runtime machine is visible, not cloaked, on-screen and >200×200 — and that the plugin's own guard produced it — because the invisibility regression passed CI and a 3D-path panel check both times it shipped.

## [2.16.2] - 2026-09-03

**Fixes a shipped regression: a transparent app was permanently invisible on any machine with no DisplayXR runtime.** Customer-reported (Lenovo). Present since v2.15.0.

### Fixed
- **A transparent app no longer starts invisible when no OpenXR runtime is resolvable** (#295). `DisplayXRTransparentOverlay.RequestTransparentSession()` — which apps call from `[RuntimeInitializeOnLoadMethod(SubsystemRegistration)]`, before anything knows whether a runtime exists — requested a transparent session unconditionally, and that pre-cloaks Unity's window and parks it off-screen at (-32000,-32000) as the earliest native touchpoint (#277). Every revert of that state is gated on a session/overlay that a no-runtime machine never produces, so the window stayed cloaked and off-screen forever — nothing on screen, only a faint taskbar thumbnail (DWM composites cloaked windows).
  - This **regressed the #256/#258 fix**, which had already made a no-runtime machine "run as a normal, visible 2D application" by session-gating the OnEnable cloak with a 5 s skip-entirely backstop. #277's pre-cloak is deliberately *earlier than the session* (to beat the startup white flash), so it bypassed that gate and shipped without the backstop.
  - The request is now skipped when `DisplayXRRuntime.ProbeSupported && !DisplayXRRuntime.IsInstalled` — no runtime, no pre-cloak, an ordinary visible window. `IsInstalled` mirrors the native runtime resolution, so it matches exactly what session start would resolve; gated on `ProbeSupported` so a platform with no managed probe does not wrongly skip. **Hardware-verified** on the no-runtime path (window visible, not cloaked, on-screen) and the runtime-present path (session comes up unchanged).
  - This is defense-in-depth, not the whole story: a #256-style timeout backstop on the pre-cloak itself (covering a runtime that resolves but whose session then fails, #296) is a tracked follow-up.

## [2.16.1] - 2026-09-02

**The first external contribution to this plugin.** The shipped window-space-UI mouse router did not survive contact with an app that brings its own canvases — which is most apps — and the failure was silent in every case.

### Fixed
- **The wsui mouse router now works with nested canvases, claims input only over real UI, and routes hover and scroll** (#268, contributed by Byungju Lee). Found and fixed while porting a real app; four distinct defects, all of which a file browser or a modal dialog triggers immediately:
  - **Nested-canvas graphics were never hit-tested.** The router raycast only the wsui root's `GraphicRaycaster`, and anything arriving with its own `Canvas` — a file browser, a modal, a dropdown's blocker — brings its own raycaster that the root never consults. Those dialogs were *entirely dead to clicks*, with no error and no warning. Every raycaster under the wsui is now collected each frame (so runtime-spawned canvases need no wiring) and hits are ordered by `sortingOrder`, then graphic depth — the same order `GraphicRaycaster` uses within a single canvas, so an override-sorted dialog correctly beats the app UI beneath it.
  - **A nested WorldSpace canvas does not inherit the root's `worldCamera`.** `GraphicRaycaster` then falls back to `Camera.main` and projects with the wrong camera. Every canvas under the wsui is now pointed at the overlay camera.
  - **`ignoreReversedGraphics` must be off on *every* raycaster, not just the root.** The wsui's overlay camera has `up = Vector3.down` and looks toward `-Z` to Y-flip the rendered RT, which makes `Dot(camera.forward, canvas.forward) == -1`; `GraphicRaycaster` reads that as "the back of the graphic faces the camera" and skips every hit. The root was already handled — a nested canvas arrives with Unity's default of `true` and silently matches nothing.
  - **`IsCursorOverInteractive` was set from the layer rect rather than from an actual hit.** The recommended 2D-scene recipe is a *full-rect* wsui (position 0,0 / size 1,1), so "anywhere inside the layer rect" meant the entire window, and scene input — orbit, pan, zoom — was blocked everywhere, permanently. It is now true only over an actual graphic or while a press is held, which is what the flag's own doc comment always claimed. **This is a behaviour change for anything reading that flag**, and it is the behaviour the documentation described.
  - **Pointer enter/exit and scroll were not routed at all**, so hover highlights, tooltips and `ScrollRect` lists did nothing. Both now dispatch; scroll divides by 120 (Windows reports one notch as 120, UGUI's `ScrollRect` expects roughly 1 and applies its own `scrollSensitivity`).
  - Also: `OnDisable` now clears the hover, releases a held press and drops the input claim, so disabling the router cannot leave a `Selectable` stuck highlighted or scene controllers permanently blocked.

### Documentation
- **The `LifecycleStop` teardown comment names all three hops instead of one** (#284). It claimed "`xrDestroySession` unhooked the SR weaver subclass", which compresses `xrDestroySession → destroys the DP` (runtime, verified unconditional) / `DP destroy → destroys the weaver` (plug-in contract) / `weaver destroy → restores GWLP_WNDPROC` (SDK contract) into one, and names the wrong owner for the hop that actually fails. Field-measured: the SR wndproc was still installed on Unity's container window 25+ minutes after `Lifecycle Shutdown` with no session running. Comment only — no behaviour change, so the shipping binary is byte-identical to v2.16.0's apart from the rebuild.

## [2.16.0] - 2026-09-02

Everything here was reported from real integrations and verified on real hardware — a partner's mixed-DPI two-monitor rig and a shipping desktop-avatar app — rather than on any maintainer's machine. Two of the three are defects that shipped in v2.13.x through v2.15.0.

### Fixed
- **The Game view weave window no longer misaligns on a second monitor at a different scale** (#263). Diagnosed across five instrumented runs on the reporting rig, in both primary orderings, Free and fixed aspect, at six distinct pane geometries. Three defects produced one symptom:
  - **The pane is matched on SIZE, never position.** The old scorer summed position and size against a single tolerance, so a pane whose size matched *exactly* was rejected on a position term of 2560 px against a 150 px threshold. Position is precisely what cannot be computed across a scale boundary — it is what we are trying to learn — so it must not gate the match. `UnityGUIViewWndClass` candidates are now preferred over other same-sized windows of ours: in every capture across five rounds a `UnityContainerWndClass` runner-up scored 139, *inside* tolerance, and is wrong by 18 px in x and 103 in y.
  - **Position and size come from the matched pane's physical rect**, read under `PER_MONITOR_AWARE_V2` — the space the native weave window actually lives in. Managed code here runs in Unity's process DPI awareness, where Windows virtualizes geometry on any monitor whose scale differs from the primary's; the two agree only on the primary, which is why this survived until a 300%/150% pair. Measured: a true `x=2560` computed as `x=5120`.
  - **The toolbar comes from Unity's own zoom-area draw rect, and the letterbox is derived.** The previously derived toolbar (pane height − `mainSize` height) silently absorbed letterbox padding under a fixed aspect ratio — 141 px versus 349 px for the same pane. The draw rect is authoritative for the toolbar (`host.height − draw.height`) and misleading for everything else: `draw.y` is a 21 pt constant that is *not* the toolbar, and the rect carries no letterbox information at all. The letterbox fit is therefore computed, and clamped so the render target is never upscaled past 1:1 — Unity draws it at native size and centres it, leaving grey bars.
  - Also: the monitor clamp is resolved from the pane HWND rather than from a point derived from the very rect being validated; `Mathf.Max(0, …)` is replaced with a virtual-desktop bounds clamp, since the Windows virtual desktop is signed and a monitor left of or above the primary has negative coordinates; and the candidate diagnostic, which gated on a lazily-resolved field still holding its `-1` sentinel and so never printed at all, now works.
- **Transparent apps no longer stall the SYSTEM mouse cursor at startup** (#279, contributed by Suki Samra). A global `WH_MOUSE_LL` hook was installed on Unity's main thread **unconditionally**, in every transparent-overlay app. Windows delivers every system-wide mouse event to the installing thread; during scene load that thread is blocked, so the OS waits out `LowLevelHooksTimeout` (300 ms default) before skipping — for every event, for every process on the desktop. This is the "the system and mouse go slow when the app starts" reported independently by two people.
  - It is **diagnostic instrumentation that shipped enabled** — its own header calls it "click-through diagnostic instrumentation (issue #57)". Now gated behind `DXR_AVATAR_LLMOUSE_HOOK=1`, default off; the hook and its proc remain for click-routing debugging, where its cost is the point.
  - The callback already skipped `WM_MOUSEMOVE` and carried a comment about the timeout, so hot-path cost had been considered. That mitigation cannot help, and the reason generalises: the cost is not the callback's runtime, it is that the callback cannot be **reached** on a blocked thread. "Observation only, never block delivery" is true of a hook's return value and false of its existence.
  - **Panel-verified**, both arms back to back with binaries hash-verified: p95 **29.3 ms → 8.0 ms**, indistinguishable from an idle machine. The control arm captured five gaps of 305–311 ms clustered within 6 ms of the 300 ms timeout bound — the mechanism itself rather than a correlation with it. No such cluster with the fix. (Raw gap *counts* in both arms are contaminated by pauses in hand movement and should not be quoted; p95 and the timeout cluster are the robust signals.)
- **The present cap's monotonic clock is portable** (#279). The non-Windows branch of `prov_monotonic_ns` called `clock_gettime(CLOCK_MONOTONIC, …)` with no `<time.h>`, so it had never compiled anywhere — CI's Linux and macOS jobs failed on the first build of the branch that introduced it. Uses `std::chrono::steady_clock`; Windows keeps QPC deliberately, since the pacing gate opens a few ms before a vblank and a coarse clock would smear that phase.
- **An app can be deactivated again** (#270, partial — see below). The focus hook suppressed deactivation **unconditionally**: it swallowed `WM_ACTIVATEAPP`, rewrote `WM_ACTIVATE WA_INACTIVE` to `WA_ACTIVE`, and reclaimed focus on `WM_KILLFOCUS`. Measured on hardware: `Application.isFocused` never went false across a 28 s session with several Alt+Tabs away, which is wrong for any app that pauses on focus loss.
  - The hook cannot simply be removed — it is installed deliberately on the **opaque** provider path for keyboard input, because the overlay is `WS_EX_NOACTIVATE` and click-through, so Unity never takes OS foreground and Windows delivers `WM_INPUT` only to the foreground window. It now suppresses only when the window or thread taking foreground belongs to **this process** — our own overlay, the case it exists for. Conservative by construction: anything not positively attributable to this process counts as not-ours, so a wrong check fails as "deactivates when it needn't have" rather than "cannot be dismissed". `DXR_AVATAR_FOCUS_HOOK_LEGACY=1` restores the old behaviour.
  - **Verified in both directions**: `isFocused` now flips 9 times where it flipped 0, and keyboard delivery is unaffected — 77 keypresses while focused, zero during any unfocused stretch.
  - **This does NOT close #270.** Minimising still fails; a second cause remains and the issue stays open. The intermittent missing taskbar button did move with this change, confirming the two-signature split identified at triage.

### Changed
- **The startup curtain is opt-in** (`DXR_AVATAR_CURTAIN=1`) rather than on by default. Hardware measurement on the box it was written for: with the curtain, the overlay uncloaked at 28699 ms; without it, 5409 ms — the avatar appeared ~23 seconds later. It lifted on the 20 s backstop rather than its fast path, because that machine takes ~18 s to reach steady state and cannot accumulate 20 consecutively well-paced frames inside the window. A subsequent fix to its steadiness test (a 1.5x ratio bound that a rate-capped app at 31/47 ms exceeds at 1.516, resetting the run on essentially every frame) reduces that to ~14 s, but the remaining trade — hidden warm-up versus visible warm-up — is a product decision per app rather than a default, and the curtain is not load-bearing for the white-window fix, which the unconditional cloak handles.
- **`DXR_AVATAR_PRESENT_HZ`** (contributed by Suki Samra) — a present cap for the provider, porting the native avatar twin's pacing knob so one perf-ladder config drives both trees. Unity's own `Application.targetFrameRate` is inert under an XR display provider, and an empty `xrEndFrame` is not a cap — the runtime presents whether or not the app handed it layers, so only skipping `xrEndFrame` moves the rate.

## [2.15.0] - 2026-09-02

### Added
- **`DisplayXRTargetDisplay` — put the app window on the 3D panel** (#266). Unity creates its window before any app script runs and opens it wherever Windows decides, normally the OS primary. So on a machine where the 3D panel is a *secondary* display, a DisplayXR app opened on the wrong monitor and the user dragged it across; the standing workaround was to make the panel the Windows primary. The plugin could not do better because nothing told it where the panel *is* — `XrDisplayInfoDXR` carries physical size and pixel dimensions, not desktop geometry. The runtime has in fact shipped the origin since `XR_DXR_display_info` **v16** (`XrDisplayDesktopPositionDXR`), so this consumes what already exists rather than waiting on anything.
  - **The move is performed in native, inside a `PER_MONITOR_AWARE_V2` thread context, deliberately.** The coordinates are signed virtual-desktop *physical* pixels; managed code in this plugin runs in Unity's process DPI awareness, where Windows virtualizes geometry on any monitor whose scale differs from the primary's. Calling `Screen.MoveMainWindowTo` or `SetWindowPos` from C# would therefore mis-place the window on precisely the mixed-DPI multi-monitor rigs the feature exists to serve, while looking perfect on every single-monitor dev box — the same coordinate-space split diagnosed in #263, where a true x=2560 read as x=5120 on a 300%/150% pair. Keeping the coordinates entirely inside a pinned context removes the failure mode by construction rather than by remembering to convert at each call site.
  - **Detection is on the return value, never the coordinates**: an older runtime ignores the chained struct and leaves it untouched, but `(0,0)` is a *legitimate* origin for a panel that is the primary display — which is the most common configuration, because it is what everyone's workaround produces. Already-on-that-panel is a successful no-op rather than a move, so those users do not get a window jump they never had.
  - Opt-in: window placement is app policy, and an app that manages its own window should not have one moved out from under it. Windows-only; needs a runtime advertising v16+, and logs once and does nothing otherwise.
- **`DisplayXRCamera.AuthoredFieldOfView`** (#274) — the FOV the rig actually projects with. While a session runs the provider hands Unity a full per-eye projection and Unity's XR then *writes* `Camera.fieldOfView` from it each frame, a tracking-derived value measured in the field at 76.5°–124.5° against an authored 60°. A `Screen Space - Camera` canvas sizes itself from that, so it renders 1.4–3.3x too large and rescales as the viewer's head moves; anything else doing FOV maths drifts the same way, silently. The rig has always kept the authored value — it must, or reading the camera back would feed the Kooima FOV into itself and collapse it — it was simply private.
  - **The setter closes a gap that was not obvious**: an app could not change FOV at runtime *at all*. Assigning `Camera.fieldOfView` while XR is active is overwritten next frame **and** ignored by the rig, which projects from its cache. Setting the cache is the only thing that works, so it is now the supported path.
  - Does **not** address the other half of #274: a rig-less camera still receives head-tracked poses and has no authored-FOV record, since nothing captured one before XR started. Documented rather than half-solved — use `DisplayXRWindowSpaceUI` for screen-fixed UI.

### Fixed
- **Transparent apps no longer show a white "(Not responding)" window for ~8.5 s at startup** (#277, contributed by Suki Samra). The cloak that hides Unity's empty backbuffer already existed, but its only caller was `DisplayXRTransparentOverlay.ApplyWindowing` — a **coroutine**, so it could not run while the main thread was loading the scene. Measured: Unity shows its window at ~1.65 s, the main thread does not yield until ~10 s, and the cloak landed there; Windows re-titles the window "(Not responding)" in the meantime because the pump is blocked. Cloaking moves to `displayxr_set_transparent_background()`, the app's first native call, with overlay birth as a backstop.
  - **Hardware-verified** on the app it was found on: at 50 ms sampling the cloak lands at 7978 ms while the window is still *invisible*, visibility follows at 8099 ms, and across 30 s of startup no window is ever visible-and-uncloaked. The cloak *precedes* visibility rather than merely coinciding with it.
  - Worth recording so it is not re-proposed: a cross-process `ShowWindow(SW_HIDE)` does **not** work — a window's visibility can only change when its owning thread pumps, so the call blocks on the same stall (measured: issued at 1.66 s, returned at 10.93 s). `DWMWA_CLOAK` works because it is a DWM-side attribute needing no pump.
  - Two details that needed care: `find_unity_hwnd()` cannot serve the pre-cloak (it requires `IsWindowVisible` and client width > 100, exactly what the window is not yet), so the pre-cloak matches on window **class** with an editor guard, since the Unity editor's main window shares that class; and the early cloak needs its own flag, because every existing un-cloak is gated on `s_overlay_active` and a session-loss revert would otherwise leave the app permanently invisible.
- **The present cap's monotonic clock is now portable** (#277). The non-Windows branch of `prov_monotonic_ns` called `clock_gettime(CLOCK_MONOTONIC, …)` with no `<time.h>`, so it had never compiled — CI's Linux and macOS jobs failed on the first build. Uses `std::chrono::steady_clock`; Windows keeps QPC deliberately, since the pacing gate opens a few ms before a vblank and a coarse clock would smear that phase.

### Changed
- **The startup curtain is now opt-in** (`DXR_AVATAR_CURTAIN=1`) rather than on by default (#277). Hardware measurement on the box it was written for: with the curtain the overlay uncloaked at 28699 ms, without it at 5409 ms — the avatar appeared **~23 seconds later**. It lifted on the 20 s *backstop*, not the "20 consecutively well-paced frames" fast path, because that machine takes ~18 s to reach steady state and so can never accumulate 20 good frames inside the window. That is the wrong shape for a default: the curtain degrades most on exactly the slow-starting machines it exists to help, and its benefit is cosmetic — it hides warm-up, it does not shorten it (peak GPU 72.5% → 69.1%, mean flat). It is not load-bearing for the white-window fix, which the unconditional cloak handles; kept available because it does what it claims on a faster box.
- **`DXR_AVATAR_PRESENT_HZ`** (contributed by Suki Samra) — a present cap for the provider, porting the native avatar twin's pacing knob so one perf-ladder config drives both trees. Unity's own `Application.targetFrameRate` is inert under an XR display provider (the provider owns pacing), and an empty `xrEndFrame` is not a cap — the runtime presents whether or not the app handed it layers, so only skipping `xrEndFrame` moves the rate.

### Documentation
- **The DPI-awareness invariant is now written down** (`CLAUDE.md`) for whoever adds the next Win32 geometry call. This plugin spans two DPI spaces — `native~/` windows pin `PER_MONITOR_AWARE_V2`, managed P/Invoke inherits Unity's and never pins — and they agree only on the primary monitor. Includes the rules: prefer doing the work in native inside a pinned scope so there is no conversion left to forget; never mix numbers from the two spaces in one expression, *including sanity checks*; and `(0,0)` is a legitimate coordinate, not a sentinel. Cross-links the runtime's own write-up of the same invariant one layer lower.

## [2.14.0] - 2026-09-01

Everything here came out of a partner integrator's port of an existing app onto the plugin — the first time anyone has driven it from a two-display machine (a 2D laptop panel plus an external 3D display). All three features are hardware-verified on that rig.

### Added
- **`DisplayXRSceneMode` — author a scene as 2D or 3D** (#267). While a session is running, XR is active process-wide and Unity renders *every* camera in stereo — including a plain camera with **no rig component on it at all**. The rig components only *tune* stereo, they do not *gate* it, so deleting them does not give you a 2D scene; it gives you a stereo scene with default tuning, and a flat menu comes out weaved. The lever that works is the runtime's rendering mode. Drop a `DisplayXRSceneMode` in a scene, pick 2D or 3D, and it performs the transition.
  - **It works with no rig present**, which is the point — a 2D menu usually has none. When a rig *is* present its `ipdFactor` is ramped in step, via `DisplayXRModeSwitch`, because the transition is asymmetric and hand-rolled ramps get it backwards: 3D→2D must flatten disparity **before** the mode request lands; 2D→3D must fire the request **first**.
  - **It waits for a late mode table.** In a **built player** the runtime's mode table is not populated at scene-load time — it reads empty or stale for a while *after* the session reports running, and only later advertises the mono mode. Deciding once therefore worked in the editor and silently took the degrade path in players, which presents as `RequestDisplayMode` returning `true` while the active mode never changes: a 2D scene that renders weaved and looks black. It now refreshes and retries for up to 5 s before degrading, then re-asserts the request until `ActiveModeIndex` actually reads back as the one it asked for — an accepted request can still fail to take inside that same window.
  - Held requests survive a not-yet-running session *and* a subsystem restart (the editor's dock/undock auto-switch stops and restarts the session mid-Play, and a once-only push into a dead session succeeds *silently*). The 3D mode returned to is **captured, not guessed**. If no mono mode appears it flattens disparity, asks for the panel's hardware 2D state, and warns once — noting that the degrade path keeps the head-tracked pose, so world-anchored UI will visibly follow the viewer's face.
  - The three underlying levers (`RequestRenderingMode`, `RequestDisplayMode`, `DisplayXRModeSwitch`) all already existed with **zero mentions anywhere in `docs~/`**. The capability was there; the surface was not.
- **`Samples~/WindowSpaceUI` — a shipped mouse input router for window-space UI** (#268). `DisplayXRWindowSpaceUI` renders correctly but its buttons are dead: the composition layer carries **pixels, not input**, and the component moves your Canvas to a private WorldSpace location rendered by a hidden offscreen camera, so `GraphicRaycaster` never hits anything. The plugin already shipped the *hooks* (the overlay camera wired as the canvas's event camera, `IsCursorOverInteractive`); the router itself lived only in the samples monorepo, behind a link to a repo that has since been archived. It now ships with the package, decoupled from the sample-only `DisplayXRTuningUI` it used to `[RequireComponent]`.
  - **Fixed a stale assumption while promoting it:** the editor path read the cursor from the native overlay window, on the premise that the woven output is a separate window. That stopped being true in **v2.8.0**, when weave-to-texture inside the Game view became the editor default. It now branches on `DisplayXRProviderDriver.GameViewTextureModeEnabled()` — the same flag the provider uses. The old failure was silent: the panel simply never responded.
- **Edit-mode Game-view framing preview for the display-centric rig** (#265). `DisplayXRDisplay` treats its transform as the virtual **display plane**, not as a viewpoint. Outside Play there is no provider and no Kooima, so Unity renders a plain perspective camera sitting *on* that plane — the Game view shows the scene from the wrong place, usually near-clipping through the content, and the framing only snaps to what was authored once you press Play.
  - It **previews rather than mutates**, and does not move the transform even temporarily. Baking a pull-back into the transform would move the virtual display itself and change what the scene means in Play; displacing it around the render would mutate a serialized object mid-render, which can dirty the scene, fight a drag in progress, and strand the rig if a begin fires without its end. Instead the camera's `worldToCameraMatrix` is overridden for the render and reset after — the view moves, the scene data never does. `cullingMatrix` is overridden with it, or Unity keeps culling from the transform's real position and geometry the pulled-back frustum newly includes pops in only on Play.
  - The offset math is extracted into a public, side-effect-free `DisplayXRDisplay.TryGetVirtualDisplayPullback`, so the runtime 2D fallback (#256) and the preview are one implementation and cannot drift apart. Editor-only, BiRP and SRP both hooked, Play Mode untouched, and only the rig's own Game camera is affected — the Scene view keeps showing the true authored layout with its display-plane gizmos. On by default; toggle in the rig inspector or **DisplayXR > Edit-Mode Framing Preview** (EditorPrefs, never scene data). It is a **framing** preview, not a stereo preview: Play Mode runs the provider and remains the real preview.

### Documentation
- **`docs~/architecture/two-dimensional-scenes.md`** — why a rig-less camera still renders stereo, the three levers, the asymmetric transition and why it matters, and the configuration hardware-verified for a flat clickable home screen in a built player: **no rig + `DisplayXRSceneMode(TwoD)` + a full-rect `DisplayXRWindowSpaceUI` + the router**. Also warns off `Screen Space - Camera` canvases, which size themselves from the XR-written `Camera.fieldOfView` and so render too large and rescale as the viewer moves (#274).
- **`docs~/architecture/window-space-ui.md`** — the rendering model (WorldSpace at `(0, 100000, 0)` on layer 30, offscreen camera, 1 RT pixel == 1 UI unit) and the input-routing contract, including the traps: `ignoreReversedGraphics` must be `false` or *every* hit is silently skipped; do not add a second Y flip; read the cursor from the right place; `pressEventCamera` is read-only in Unity 6's UGUI.
- The display-centric section of the quick start now explains why the Game view looks wrong outside Play, and warns against "fixing" it by moving the camera by hand.

## [2.13.2] - 2026-08-31

### Fixed
- **The Basic Scene sample now ships the scene the docs tell you to open** (#261). Quick Start Step 4 and the sample's own README both said to open `Basic Scene/BasicScene.unity`; that file was never committed (the original #7 task never closed), so the imported sample contained only a script and a README and the very first thing a new user does after installing the package failed. `Samples~/BasicScene/BasicScene.unity` is now a real committed scene — a **Main Camera** carrying `DisplayXRCamera` and a **Scene Setup** object carrying `BasicSceneSetup` — with a committed `.meta` for the script so the scene's script reference is stable across imports. The test geometry is still built at Play time rather than authored, so the sample picks a material shader matching the host project's render pipeline (URP `Lit`, falling back to Built-in `Standard`) instead of shipping a material that renders magenta on the other one; the README now says so, since it means the Scene view looks empty until you press Play. The quick start's hardcoded `Assets/Samples/DisplayXR/0.1.0/...` path — stale by thirteen minor versions — is now version-agnostic.
- **The setup scripts in five samples no longer no-op when the boot splash is enabled** (#262). Each guarded on `FindAnyObjectByType<MeshRenderer>() != null` meaning "this scene already has content, don't rebuild it". But `FindAnyObjectByType` also scans the **DontDestroyOnLoad** scene, and the boot splash — on by default — spawns there at `BeforeSceneLoad` (`DisplayXRSplashBootstrap`) and builds its logo and subtitle quads with `MeshRenderer` (`DisplayXRSplash.MakeQuadGO`). When display info resolves on frame 1, `BuildArtwork` runs synchronously inside the splash's `Start()`, ahead of the scene's `Start()` — so the guard saw the splash's quads, concluded the scene was already populated, and created nothing. The symptom was an empty scene with no error, and the only workaround was to turn the splash off. All five guards are now scoped to the setup object's own scene (`gameObject.scene`, or the active scene in `MinimalTransparent`, which is a static class with no GameObject of its own): `BasicSceneSetup`, `DisplaySceneSetup`, `URPBasicSceneSetup`, `TransparentAvatarSetup`, `MinimalTransparent`. The two transparent samples were latently rather than actually affected — the splash bootstrap skips transparent-overlay apps — but the guard was wrong in the same way and is fixed in the same way.
  - Worth knowing beyond the samples: **any** app calling `FindAnyObjectByType` / `FindObjectsByType` on frame 1 sees the splash rig, because it is a `DontDestroyOnLoad` object created before the first scene loads.
- **The `DisplayXRWsuiMouseRouter` link in this changelog pointed at an archived repo** (#268). It now resolves to `displayxr-unity-samples/samples/urp-singlepass-ui`, where the router actually lives. (Shipping the router with the package, and documenting `DisplayXRWindowSpaceUI` at all, is tracked in #268.)

All five findings above came from a partner integrator's first-touch onboarding pass; the remaining reports from that pass are tracked in #263 (docked Game view weave glue on a second monitor), #264 (editor crash), #265 (display-centric framing outside Play), #266 (player opens on the OS main display) and #267 (no way to author a 2D scene).

## [2.13.1] - 2026-08-28

### Fixed
- **The transparent overlay's hit mask no longer clips the picture** (#259, #260). The `SetWindowRgn` region the mask drives shapes the *window* — a pixel the region misses is a pixel of the picture that is not drawn — but every trade-off in the mask path was justified against hit-testing and never re-checked against what the user sees (the same defect class displayxr-common fixed in its #27/#31). Field report from Lenovo's 3DLuma avatar on a 4K 3D display: the hat brim clipped when the avatar turned its head, and the region edge flickered near the taskbar while dragging the window. Three changes:
  - **The mask raster scales with the overlay** (~4 px per texel, floor 256x144, cap 640x512). The fixed 256x144 raster was ~15 px per texel at 4K, so a feature thinner than a texel — a hat brim edge-on — failed to rasterize and was cut from the picture. Mask dimensions are captured at request time and flow through the readback callback and the native call, so the raster can resize between frames without misinterpreting bytes; it is never resized under an in-flight readback.
  - **Dilation is sized in window pixels** (24 px target): the 5x5 max kernel ping-pongs as many passes as the texel size requires, absorbing AA edges *and* the readback's 1-2 frames of latency while the silhouette moves.
  - **Identical regions are skipped natively**: `displayxr_set_overlay_hit_mask` hashes the final rect list + destination size (FNV-1a) and skips `SetWindowRgn` when nothing changed. It previously called `SetWindowRgn(bRedraw=TRUE)` every frame even for identical regions — a forced repaint invalidation 60x/s, visible as flicker at the region edge during drags. The hash resets on every path that clears the region and on apply failure.

## [2.13.0] - 2026-08-19

### Added
- **The display-centric rig frames itself correctly in the 2D fallback** (#256). `DisplayXRDisplay` treats its transform as the virtual **display plane** — that exact pose is what the driver sends the runtime, and the Kooima projection puts the eyes in front of it. Without a runtime there is no Kooima and no eye offset, so a plain perspective camera sat *on* the display plane and near-clipped through the content: the graceful 2D window was visible but showed the inside of the avatar mesh. The rig owns both numbers needed to fix that, so at `Start()` it now backs the camera out along its own −forward by `D = (H / 2) / tan(fov / 2)` — the distance at which the frustum intercepts the virtual display's height exactly at the former display plane. (vHeight 2 at 60° → D = 1.732.) `H` is the **world** height, `virtualDisplayHeight × lossyScale.y` (not the driver's `vdh / scale`, which folds scale into the runtime-owned metric to get scale-as-zoom; a transform move needs plain world units), and the FOV is the camera's own `fieldOfView` — ignored by the 3D rig, honored by Unity in 2D, which is exactly why it is the right number here. Runtime-only and one-shot: it moves the live transform in Play Mode, touches nothing serialized, and never re-evaluates, so it cannot fight a script that repositions the camera afterwards. A `virtualDisplayHeight` of 0 ("use the physical display height") is unknowable with no session, so it falls back to the same 0.2 m nominal the gizmos use for an unknown display.
  - **The gate is "XR is not going to run", not "is not running right now."** `DisplayXRDisplayLoader.SubsystemRunning == false` alone would be wrong — the editor's dock/undock auto-switch restarts the subsystem mid-Play and would drag the camera back another `D` each time — and a "the loader declined" flag alone would also miss the canonical #257 pattern, where the app sets `XRGeneralSettings.InitManagerOnStart = false` so `Initialize()` never runs at all. `DisplayXRRuntime.IsInstalled` covers both and is restart-immune: it is a cached machine-level fact, so no subsystem restart can flip it. `SubsystemRunning` is kept as belt-and-braces and `ProbeSupported` keeps platforms with no managed probe on the existing behavior.
  - **`DisplayXRCamera` (camera-centric) needs nothing** — its transform is already a viewer position and its FOV is the camera's own, so the authored viewpoint renders unchanged; and the convergence drops itself. `invConvergenceDistance` leaves managed code only via `GetProviderTunables()` → the provider driver → `dxr_prov_set_tunables`, a native tunable consumed at `xrLocateViews`; with no session there is no driver, nothing is pushed and nothing consumes it. Verified that no managed path applies convergence camera-side (no projection override, lens shift, or eye/anchor offset — the only other reader is the editor gizmo), and that the BiRP foreground-clip pass is attached disabled and enabled only inside a `DisplayXRProviderDriver.IsActive` branch. So no dead code was added for it.
- **`DisplayXRRuntime` — a pre-session availability API** (#257). `DisplayXRRuntime.IsInstalled` / `DisplayXRRuntime.ResolvedManifestPath` answer "can this machine run DisplayXR at all?" with no OpenXR instance, no session and no native subsystem, so they are safe from a `[RuntimeInitializeOnLoadMethod(RuntimeInitializeLoadType.SubsystemRegistration)]` bootstrap — the deadline for an app that wants ONE build to run 3D where a runtime is installed and plain 2D everywhere else. Until now the only signal was `DisplayXRProvider.IsRunning`, which is post-session and far too late to choose content, so apps were re-implementing the probe themselves. The probe is deliberately the same resolution the native session start performs (`ps_resolve_runtime_json`): `XR_RUNTIME_JSON` if set — and if set, it wins outright, matching OpenXR loader semantics, so an env var pointing at a missing file means "no runtime" rather than "look elsewhere" — else `HKCU\SOFTWARE\Khronos\OpenXR\1\ActiveRuntime`, else the `HKLM` equivalent, else on macOS/Linux the fixed `/usr/local/share/openxr/1/active_runtime.json`; the manifest file must exist.

### Fixed
- **An app on a machine with no DisplayXR runtime now runs as a normal, visible 2D application** (#256, #257) instead of an invisible one. This is the "app doesn't start / correct taskbar thumbnail, nothing on screen" field report, and it was an invisibility, not a crash. Four changes, each of which is independently sufficient for part of the failure:
  - **The XR loader declines before creating anything.** `DisplayXRDisplayLoader.Initialize()` runs the `DisplayXRRuntime` probe first and, when nothing resolves, logs one clear warning and returns `false` — XR-Management then proceeds without the subsystem and Unity renders normally into its own window. Previously the loader started the subsystem regardless: `LifecycleStart` (the only site that creates the "DisplayXR Overlay" HWND) ran *before* the session was ever attempted, so the clean "No runtime JSON" refusal arrived too late to undo anything.
  - **A failed session start no longer leaks the overlay.** `GfxStart` now destroys the app-owned overlay when `dxr_prov_session_start` fails, and `LifecycleStop` destroys it on Windows too (it previously destroyed only the editor's dedicated weave window). The orphan was `WS_EX_TOPMOST` and — in transparent mode — created *without* `WS_EX_TRANSPARENT` (that bit is toggled per frame by the live hit test), so with nothing driving it, it also swallowed every mouse click over the app. Teardown unwinds installation in reverse: un-cloak Unity, remove the focus hook's subclass (chained on top of the overlay's), restore the overlay's own subclass — each guarded on still being the outermost WndProc — then clear the statics and `DestroyWindow`. Safe from the render thread: the destroy marshals itself to the window's creating thread.
  - **Transparent-overlay windowing is session-gated.** `DisplayXRTransparentOverlay.OnEnable` used to cloak Unity's HWND (`DWMWA_CLOAK`) and park it at (-32000,-32000) guarded only by platform and `Application.isEditor`, which is what turned a refused session into a permanently invisible app. It now defers that until the display subsystem *and* the provider session are both up, applies it then, and if neither arrives within 5 s logs one warning and skips the windowing entirely for that enable-cycle. In a healthy run the cloak lands a frame or two later than before (the session starts on the render thread); nothing depends on cloak-before-first-frame.
  - **A reverse watchdog un-cloaks if the session dies mid-run.** Nothing used to: the per-frame driver simply goes quiet on `!is_running`, and only a runtime-requested exit reaches `Application.Quit()` (and thus `OnDisable`). A session lost for more than 5 s now restores the app's own window with one warning.

## [2.12.1] - 2026-08-19

### Fixed
- **The transparent-overlay hit test no longer CPU-skins and ray-scans renderers that aren't being drawn** (#254). `DisplayXRTransparentOverlay.clickableRenderers` drove an unconditional `SkinnedMeshRenderer.BakeMesh` plus a managed per-triangle Möller–Trumbore scan for **every** entry, active or not — an app that wires four ~50k-triangle characters into the list and then deactivates three of them was skinning and scanning ~167k triangles per frame, almost all of it for invisible objects. The bake path and both hit-test loops (Win32 and macOS) now apply the same `enabled && activeInHierarchy` criteria the silhouette-mask and union-rect paths already used, via one shared `IsHitTestable()` helper so the two can't drift.
  - **Also a behaviour fix:** an invisible renderer used to stay clickable while being absent from the silhouette / `SetWindowRgn` click-through mask, so clicking and visibility disagreed. They now agree. Apps that deliberately relied on a `renderer.enabled = false` object still being clickable should deactivate a *collider* instead, or keep the renderer enabled with a transparent material.
  - A renderer skipped while inactive has its cached bake invalidated on the way past, so reactivating it can never hit-test against the pose it held before deactivation — the first bake after reactivation re-arms it.
- **Two ~600 KB-per-frame allocations in the bake path** (#254). The topology change-check read `entry.mesh.triangles.Length`, which materialises the entire `int[]` every frame purely to read a length off it and discard it; it now sums `Mesh.GetIndexCount()` over the submeshes (the same total `triangles[]` concatenates, and alloc-free). The vertex fetch moved from `Mesh.vertices` (a fresh `Vector3[]` per frame) to `Mesh.GetVertices(List<Vector3>)` into a persistent per-entry list. The bake path is now allocation-free in steady state.
- **The per-triangle scan is bounds-gated** (#254). `TryRayHitBakedSkinnedMesh` now rejects on `SkinnedMeshRenderer.bounds.IntersectRay()` before walking triangles, so the linear scan only runs when the cursor ray is actually over the renderer. `updateWhenOffscreen` keeps those bounds tracking the skinned pose, and the file already documents that the bake-local→world transform reproduces `renderer.bounds` exactly, so the box and the triangles live in the same space. A bounds reject is an ordinary miss — the `STICKY_FRAMES` hysteresis is untouched.

### Changed
- **`package.json` now declares `com.unity.inputsystem` (>= 1.4.4)** (#254). The `Runtime` asmdef has always hard-referenced `Unity.InputSystem`, and `DisplayXRTransparentOverlay` calls `InputSystem.QueueStateEvent` / `Mouse.current` to keep standard input alive behind the cloaked HWND, but the manifest declared only `com.unity.xr.management` — so a project without the Input System installed hit an unresolved assembly reference. 1.4.4 is the version Unity 2022.3 LTS (the package's minimum editor) verifies, and the APIs in use all date to 1.0, so the floor is as low as it can usefully go and no project is forced past its verified version.

## [2.12.0] - 2026-08-09

### Added
- **Linux (x86_64) support — the first Linux release of the plugin.** *Preview*: hardware-verified rendering through the real Leia SR weaver on an Odyssey G90XF, with one gap called out under "Known limitations" below. Mirrors how the runtime labels its own Linux support.
- **Linux (x86_64) support for the provider Vulkan backend** (#249) — the plugin now builds and ships `Runtime/Plugins/Linux/x86_64/libdisplayxr_unity.so`, with `DXR_GFX_VULKAN` compiled in. The DisplayXR runtime's Linux support is code-complete and hardware-validated (native Vulkan/XCB compositor, srSDK Vulkan weave on an Acer SpatialLabs DS1, `.deb` + tarball), so the runtime side of this was already there.
  - The Vulkan backend is now **platform-parametric** rather than Windows-only: the enable2 session flow, image create-info mirroring, dedicated allocations, parked-in-`GENERAL` layout scheme and per-frame copy are all shared, and the only per-OS part is the external-memory handle flavour — `VK_KHR_external_memory_win32` / `OPAQUE_WIN32` `HANDLE`s on Windows vs `VK_KHR_external_memory_fd` / `OPAQUE_FD` file descriptors on Linux. **Note the ownership rule inverts**: a successful fd import transfers ownership to Vulkan (never close it), whereas an imported Win32 `HANDLE` stays the application's to close.
  - The **cross-adapter guard now compares `deviceUUID`, not `deviceLUID`.** `deviceLUID` is only valid when `deviceLUIDValid` is set (in practice, Windows), so a LUID comparison would silently degrade to "guard skipped" on Linux. `deviceUUID` is always valid *and* is the identity the runtime itself matches on (`client_vk_deviceUUID` → `oxr_vk_get_physical_device`), so both sides now compare the same thing.
  - **The XR pre-init adapter policy is per-platform, deliberately.** Windows keeps the #248 non-matching-sentinel that steers a Vulkan project onto D3D12; **Linux declines the query instead** — correct on a single-GPU box, and there is no other graphics API to steer to (the provider has no GL backend), so the Windows sentinel would just fail engine init there. Linux serves the fd-flavoured device extensions.
  - Phase 1 scope: the **primary stereo path only**. The runtime self-hosts its weave window (no window-binding extension chained — `XR_DXR_xlib_window_binding` needs Unity's X11 window handle, which `IUnityGraphics` does not expose). The win32 overlay, wsui composition layer, Local2D, extra 3D display zones, weave-to-texture GameView and transparent-overlay click-through are **inert on Linux**, each with a clear log line — the same treatment the Windows Vulkan backend already gives them.
  - CI gains a **`build-linux` job** running in an `ubuntu:22.04` container (oldest supported glibc), gating on the `.so` exporting `UnityPluginLoad`/`UnityPluginUnload`/`XRSDKPreInit` under `-fvisibility=hidden` and on it having **no hard link dependency on libvulkan** (every entry point is `dlopen`ed, so the plugin still loads on a box with no Vulkan ICD). The release and package jobs consume the artifact.
  - `DisplayXRProviderRuntimeDeploy` now deploys the `.so` + `UnitySubsystemsManifest.json` into a Linux player's `<name>_Data/`, and `IsSinglePassEligible()` gains an explicit **Linux × Vulkan** row (URP/HDRP → SPI, BiRP → MultiPass, matching every other platform).
  - **Not verified on hardware.** This lands the build and the shipping path; whether Unity's `vk::Image::CreateImageViews` crash (#248 defect 2) reproduces on a Linux player is the open question, and answering it either way strengthens the upstream Unity bug report. Linux hybrid-GPU boxes also have no adapter-alignment lever yet (`DisplayXRGpuPreference` is DXGI-based and no-ops off Windows) — the session-side UUID guard turns that into a loud refusal rather than a black screen.

### Fixed
- **The Linux `.so` could not load at all** (#249) — two undefined symbols (`displayxr_is_shell_mode`, `displayxr_metal_view_backing_size`) from provider `#ifdef` blocks widened to Linux without auditing what they called. The systemic half matters more than the fix: **unlike MSVC and macOS's two-level namespace, a Linux shared library links successfully with unresolved references and only fails when the loader binds them** — so CI reported green on a binary that died in `GfxStart` with `undefined symbol`. Fixed by adding the `displayxr_linux.c` platform TU (macOS already ships the equivalent stub), guarding the Metal call, and passing `-Wl,--no-undefined` so the whole class becomes a link error. CI gained an `ldd -r` gate as belt-and-braces.
- **Nothing polled XR events on Linux** (#249) — the native pump was `#ifdef _WIN32` and the C# fallback `#if UNITY_*_OSX`, so Linux inherited neither: the `READY` event was never consumed, `xrBeginSession` never called, the session sat at state 2 forever and `shouldRender` stayed false. It presented as a completely healthy run — session created, weaver holding the window, ~1800 pump ticks — with a blank panel and zero bridge copies. Linux has no AppKit constraint, so it now polls on the graphics thread like Windows.
- **Red and blue were swapped on Linux** (#249) — the Vulkan swapchain-format table was `#ifdef _WIN32`, so the selection loop fell through to the Metal arm, matched nothing, and kept `formats[0] = 44` (`VK_FORMAT_B8G8R8A8_UNORM`) while the provider declares `kUnityXRRenderTextureFormatRGBA32` to Unity. Unity wrote RGBA into a BGRA image, so a yellow background rendered cyan. The VkFormat table is identical on both OSes and now lives outside the per-OS branch; the picked format is `37` (`R8G8B8A8_UNORM`).

### Changed
- **The Linux weave surface is a child of Unity's window** (#249), matching what the Windows backend already does. It renders above its parent by definition (no stacking fight), is clipped to and moves with it (no tracking), and — because it selects no X events — **input passes through to Unity for free**. This replaced a top-level override-redirect overlay that needed re-raising every frame and would otherwise have needed an `XShape` input region just to stop stealing clicks.

### Known limitations (Linux)
- **Stereo separation is not yet visually confirmed.** Every run so far was headless over SSH, so the panel's camera saw no face and the eye tracker returned its documented untracked fallback — both eyes collapsed to the nominal viewer position with ~0 IPD, which makes the two rendered views identical and leaves the weaver nothing to interlace. The native `displayxr-demo-modelviewer` behaves identically on the same box, so this is neither Unity-specific nor a plugin defect; the path is wired correctly end to end (`enable2` → runtime-owned device → Leia SR DP + real srSDK weaver → `got_eyes=1`). Confirming true 3D needs a person in front of the display — there is no forced-IPD test knob, because the tracker is stereo-triangulation rather than a settable IPD.
- Input pass-through is by construction (the child window selects no events) but has not been click-tested.
- **Inert on Linux, by design**: the win32 overlay layer, the wsui composition layer, Local2D, extra 3D display zones, weave-to-texture GameView and transparent-overlay click-through. Each logs one clear line rather than failing.
- **No adapter-alignment lever on hybrid-GPU Linux boxes** — `DisplayXRGpuPreference` is DXGI-based and no-ops off Windows. Single-GPU boxes are unaffected; on a hybrid box the session-side `deviceUUID` guard turns a mismatch into a loud refusal rather than a black screen.
- **Wayland is not supported** (`XR_DXR_wayland_surface_binding` untouched); X11 only.
- Editor Play Mode on Linux declines cleanly — Unity's VK XR path needs the `boot.config` pre-init hook, which only exists in built players. Build and run a Linux player.

## [2.11.0] - 2026-08-08

### Added
- **Vulkan groundwork + deliberate D3D12 fallback for Vulkan projects** (#248). Unity 6000.4's Vulkan XR path is broken for third-party display providers in two ways, both hardware-evidenced (see PR #248 and `docs~/unity-bug-report-vk-xr-preinit.md`): the XR pre-init adapter query is unanswerable (called once before any `VkInstance` exists, matched later by raw per-instance handle equality — disassembly-verified), and with Vulkan retained the first XR texture create hard-crashes in `vk::Image::CreateImageViews` regardless of every provider-controllable input. Rather than let a Vulkan-configured project crash, **the new XR pre-init provider deliberately answers the adapter query with a non-matching sentinel, steering the engine to the next graphics API in the project list — so a Vulkan project now runs full stereo on D3D12**, with a loud log explaining why. Requires D3D12 (or D3D11) after Vulkan in the project's graphics-API list; a Vulkan-only list fails engine init. `DISPLAYXR_VK_EXPERIMENTAL=1` opts back into real Vulkan (crash expected until Unity fixes the above). Ships with it: the complete `DXR_GFX_VULKAN` backend (enable2 session on a runtime-created `VkDevice` — verifies runtime#886 item 2, runtime-owned queue for the #868 repaint — plus a `VK_KHR_external_memory_win32` eye bridge), inert unless the engine actually comes up on Vulkan; the XR pre-init plumbing (`IXRLoaderPreInit` → boot.config `xrsdk-pre-init-library` → `XRSDKPreInit`); and an editor gate that declines Vulkan editor Play Mode cleanly (pre-init cannot run in-editor — no boot.config). Vulkan editor/player behaviour on D3D/Metal projects is unchanged (the pre-init provider no-ops for non-Vulkan renderers; regression-verified on the D3D12 path).
- **`DisplayXRGpuPreference` speaks Vulkan** (#247) — the adapter steer now sets `DXR_VK_FORCE_GPU` when Unity is on Vulkan (previously it always set `DXR_D3D_FORCE_GPU`, a silent no-op there). The runtime honours it transitively: it steers the compositor's `VkPhysicalDevice`, and `xrGetVulkanGraphicsDevice2KHR` suggests the device matching the compositor's UUID.
- **App-selectable target GPU** (#242) — an app can now choose which GPU it runs on instead of being unconditionally pinned to the discrete one.
  - **`DisplayXRGpuPreference.Target`** (`Auto` | `Discrete` | `Integrated`), settable from app code before XR initialization (e.g. a `[RuntimeInitializeOnLoadMethod]`), plus a **Target GPU** field on the manifest settings asset (shown in Project Settings > XR Plug-in Management > OpenXR > DisplayXR).
  - **`Auto` (the new default behaviour at runtime) points the runtime at whichever adapter Unity actually landed on**, using the same dedicated-VRAM classification the runtime uses for its `igpu`/`dgpu` keywords. On the ordinary discrete path this resolves to the runtime's existing default, so nothing changes there; it takes effect only in the configuration that is broken without it. On a single-GPU box it is a no-op — adapters cannot diverge, so the runtime's default is left alone.
  - This closes the mismatch class behind #240 at the source: previously the plugin pinned Unity to the dGPU while the runtime independently suggested the dGPU, and any deviation (a panel driven by the iGPU, a manually-set GpuPreference, Unity's D3D12 device filter falling back) produced a cross-adapter eye bridge that presents black.

- **`DisplayXRLocal2D.maxRefreshHz`** (#244, PR #245) — optional cap on how often the Local2D overlay canvas re-renders. The component leaves its offscreen orthographic camera enabled, so the canvas re-rendered **every frame regardless of content**, and in provider mode every frame also paid a full-RT `Graphics.CopyTexture` into the bridge — wasteful for the typical static speech bubble or HUD. `maxRefreshHz` gates the camera's `enabled` flag from `LateUpdate`, with `SetDirty()` to force an immediate refresh and an automatic force when the panel rect moves; the bridge copy now follows the camera via `OnEndOverlayCamera`. **Defaults to 0 (every frame) — no behaviour change unless opted in.** Deliberately not the manual `Camera.Render()` path, which races the canvas rebuild (`willRenderCanvases` runs after `LateUpdate`) and shipped the intermittent-blank bubble in v2.8.2.

### Changed
- The post-build GPU pin honours the Target GPU setting instead of hardcoding `GpuPreference=2;`. `Integrated` writes `GpuPreference=1;`, `Auto` removes any per-exe entry so Windows decides. **Default remains `Discrete`, so existing projects rebuild identically.**

### Fixed
- **The post-build GPU pin never actually worked** — a pre-existing bug, present since the pin shipped. Unity's `outputPath` is forward-slash separated, but Windows matches `UserGpuPreferences` entries against the backslash form of the exe path, so every entry the plugin has ever written was silently ignored: visibly present in the registry, with no effect. Masked until now because Windows/Optimus defaulted these apps to the discrete GPU anyway — the pin was agreeing with a decision it wasn't making. Hardware-verified both ways: a forward-slash `GpuPreference=1;` left Unity on the dGPU; the identical entry with backslashes moved it to the iGPU. The processor now normalises the path and removes the stale forward-slash entry.
- The runtime's adapter steer is now set through a native `_putenv_s` + `SetEnvironmentVariableW` export rather than managed code. The runtime reads `DXR_D3D_FORCE_GPU` with `getenv()`, which reads the CRT's cached environment table — `SetEnvironmentVariableW` (what C#'s `Environment.SetEnvironmentVariable` calls) does **not** update it, so a managed-only set is silently ignored. Verified on hardware that a late-loaded DLL observes the value via `getenv()` under both `/MD` and `/MT` CRT linkage.

## [2.10.1] - 2026-08-02

### Fixed
- **GPU adapter mismatches now fail loudly instead of silently presenting black** (#240, #241). Two configurations let a healthy-looking Unity app measure as rendering while nothing reached the panel:
  - **D3D12 own-device path**: no adapter-mismatch check existed at all — a session whose own/session device diverged from Unity's device started successfully and presented black through the cross-adapter eye bridge (the ADR-032/#223 failure mode, reproduced on hardware). The provider now refuses to start, with actionable remediation, rather than presenting black.
  - **D3D11 zero-copy path**: the mismatch WARN was a single quiet line. It now names both adapters (LUID → DXGI description) and prints the exact knobs that align them (per-exe GpuPreference, the runtime's `DXR_D3D_FORCE_GPU` from v2.2.4, `-force-d3d12`).
  - **D3D11 fallback**: added an info line explaining that Unity's D3D12 device filter commonly denies integrated Intel GPUs, and that the D3D11 zero-copy backend is the supported path there (bypass: `-force-d3d12`).

## [2.10.0] - 2026-07-31

### Added
- `DisplayXRProvider.SetZoneFeather(index, featherPx)` (#238) — per-zone cosmetic edge-feather radius in client-window pixels, chained as `XrDisplayZoneFeatherDXR` on the zone at submit (display-zones spec v3, runtime#800/#803). Zone edges are **hard by default** on post-#804 runtimes; a zone wanting a soft composite edge (e.g. the desktop-avatar's zone-vs-band transition) opts in with an explicit radius. Cosmetic only — the published hardware wish stays binary; pre-v3 runtimes ignore the request (hard edges, no error). Survives session restarts; applies from the next submitted frame.

## [2.9.1] - 2026-07-30

### Added
- `DisplayXRProvider.TryGetViewerEyes(out left, out right)` / `TryGetViewerHead(out head)` — the world-space (Unity coords) positions of the two eyes the provider is rendering with this frame, from the render-ready `xrLocateViews` poses. This is the supported way for an app to read the tracked viewer position for head-coupled effects (billboards, lean-to-zoom, parallax UI). A missing native plugin is latched once rather than throwing per frame.

### Fixed
- **Face Viewer (Billboard) sample never tracked the viewer's head** (#236). It derived the head from `Camera.GetStereoViewMatrix(Left/Right)`, which returns the **same (mono) matrix for both eyes** in provider mode: Unity's C#-side stereo matrix cache is only written by `Camera.SetStereoViewMatrix()`, which the plugin no longer calls — per-eye poses reach Unity through the native frame desc (`deviceAnchorToEyePose`) and are consumed inside Unity's render loop, never round-tripped back into the C# camera. The sample's coincident-eyes guard therefore tripped every frame and the billboard silently fell back to the camera transform, on every pipeline and graphics API. `FaceViewer.TryGetViewerHead(cam, out head)` now forwards to `DisplayXRProvider.TryGetViewerHead` (signature preserved for v2.8.3 source compatibility; `cam` is unused). The sample README documents the correct APIs and why `GetStereoViewMatrix` is the wrong tool here.

## [2.9.0] - 2026-07-28

### Fixed
- **App-authored 3D zones were corrupted in the editor's docked weave-to-texture Play Mode** (#233, #234). An app that authors its own 3D zone (a 3D band with a 2D/Local2D band beside it — the `desktop-avatar` pattern) rendered with the content truncated and a matching black band, a visibly magnified/broken weave, and its 2D band missing. Two bugs, one stale assumption — *"the 3D zone is the full pane"*, true only while the provider was forcing it to be:
  - `dxr_prov_converge_gameview_zone()` paired the app's zone **offset** with the Game view pane's **extent**, producing a zone that overran the pane (e.g. `(0,284) 1728x576` → `(0,284) 1728x860` in an 860-tall pane) and a compositor viewport that overflowed its tile. Converge now *repositions* an app zone (pane origin + app offset, extent preserved, clamped) and never redefines its extent; the app-authored rect is recorded separately so converge is idempotent.
  - `dxr_prov_get_woven_canvas()` returned the **zone** as the GameView mirror's source rect. Since the mirror stretches that to fill the Game view, a sub-window zone was magnified — destroying the lenticular interlace and never sampling the 2D bands. It now returns the **pane**, which is what the runtime actually composites into the shared texture.
- **State pushed once before the session started was silently lost on every session restart** (#233, #234, #235). The editor's docked↔undocked switch restarts the display subsystem mid-play, and session stop clears the provider's session state; because each C#→native push is change-detected or once-only, nothing re-pushed. Fixed for all six affected paths — the 3D zone (both render paths, not just the docked one), the transparent-background opt-in (sessions came back **opaque**), extra/multi-zone rects, the Local2D bridge and rect, and the `DisplayXRWindowSpaceUI` bridge. Bridges are now re-acquired by detecting the native pointer change rather than assuming a non-null wrapper is still live — a wrapper around a destroyed resource accepted copies silently, so the failure had no error signature.

### Changed
- `dxr_prov_get_woven_canvas()` now reports the Game view **pane** rect rather than the active 3D-zone rect. Identical for apps that author no zone (the provider's forced zone equals the pane); different only where an app authors a sub-window zone, which is the case this fixes.
- `dxr_prov_set_transparent_background()`, `dxr_prov_set_zone_count()` and `dxr_prov_set_zone()` now persist their request across session restarts instead of applying only to the session that was live when they were called.

## [2.8.3] - 2026-07-28

### Added
- **Face Viewer (Billboard) sample** (#232): new UPM sample with a `FaceViewer` component that rotates an object to always face the tracked viewer's head. The head position is derived from the rendered stereo eye poses (midpoint of the per-eye view-matrix positions), with a camera-transform fallback when stereo poses aren't available. Exposes a public static `TryGetViewerHead` helper, and the sample README documents the raw physical-space eye/window APIs.

## [2.8.2] - 2026-07-24

### Fixed
- URP docked editor Game view was black in weave-to-texture Play Mode (#231): the boot splash's `WaitForEndOfFrame` stalled the docked GameView texture path, and the window-space UI (wsui) layer wasn't rendered on the texture path. Fixed by driving the wsui camera via an explicit `Camera.Render` and adding a native shader-blit GameView mirror so the woven stereo lands in the docked Game view. (BiRP/HDRP and the undocked present path are unchanged.)

## [2.8.1] - 2026-07-23

### Fixed
- Present-path output was too dark for Linear color-space projects (HDRP, URP default): the runtime's window present (undocked editor window + built player) applied no linear→sRGB encode. The provider now requests an sRGB swapchain for Linear projects on the present path so Unity encodes linear→sRGB on store (docked texture path and Gamma projects unchanged). (#229/#230)

## [2.8.0] - 2026-07-23

### Changed
- **Weave-to-texture GameView is now the editor Play Mode default** (#227, #228): pressing Play weaves the runtime's stereo **inside** the Unity Game view instead of a separate external window — dockable/maximizable, identical to a built player. Opt back into the previous external-window Play Mode with `DISPLAYXR_PROV_EXTERNAL_WINDOW=1`. (Promotes the v2.6.0 texture path, formerly probe-gated behind `DISPLAYXR_PROV_TEXTURE_PROBE`, to the default.)

### Added
- Workspace-tile shell weave now consumes `xrGetWorkspaceTileSizeDXR` so the provider renders at the shell-assigned tile size rather than a fixed window size (#225, #226).

### Fixed
- README/docs vendor-neutrality pass: neutralized the vendor name in the `sim_display` note, corrected `SIM_DISPLAY_ENABLE` guidance (sim_display is the automatic fallback), and de-staled the MinimalTransparent README to the provider architecture.

## [2.7.0] - 2026-07-18

### Added
- Workspace-tile weave mode: the DisplayXR Shell / IPC service now composites the runtime's woven stereo into a workspace tile. The provider renders with a null window handle (`windowHandle=NULL`), bridges frames on the D3D12 same-device path, and pairs its begin/end frame loop with the service. (#223, #224)

### Fixed
- Provider now quits on the shell's close/exit request: an `EXITING` session state drives `Application.Quit`, so a shell-initiated close cleanly tears down the player. (#223, #224)
- Begin/end frame pairing when `shouldRender=false`: the provider no longer leaves an unbalanced frame open on skipped-render frames. (#223, #224)

## [2.6.0] - 2026-07-16

### Added
- Weave-to-texture Play Mode: the runtime's woven stereo now renders **inside** Unity's editor Game view (dockable/maximizable, identical to a built player) instead of a separate window. Auto-switching docked/undocked hybrid — docked binds texture mode (shared-texture weave → Game-tab mirror-blit, DP `phase_off` correction) and undocked binds present mode (self-anchored). Includes live POV during host drag, layout-reset re-target, and D3D12 resize stability (atlas crop barrier + full `XR_KHR_D3D12_enable` swapchain-state contract). Windows/D3D-only; env-gated on the probe, additive (the external-window path is unchanged when off). The prior external-window Play Mode approach is archived at branch/tag `*/external-window-playmode`. (#740, #747)

### Fixed
- Docked Game-view interlace phase: corrected two anchor-vs-content RT-centring offsets (Unity centres the render target in the pane, +3px X, and draws it with a bottom margin, −4px Y → X-phase through the slanted lens), latched to stay stable through interactive resizes. (#740)
- Refreshed the committed Windows/macOS native binaries to the v2.5.0 post-rename CI builds so shipped and CI-built plugins match. (#219, #221)

## [2.5.0] - 2026-07-12
- 6acc285 feat!: rename DisplayXR extensions XR_EXT_* -> XR_DXR_* (DisplayXR/displayxr-runtime#734)
- 85d3afb chore: bump package.json version to 2.4.0 to match released v2.4.0 tag
- 5287e39 Provider: Metal Local2D + extra 3D display-zones composition layers (#206)
- f9c4b88 Release v2.4.0
- a5138d4 docs: point samples at the displayxr-unity-samples monorepo
- 1f50a08 ci: docs_only short-circuit for empty/marker pushes

## [2.3.2] - 2026-07-07

### Added
- D3D11 provider now supports MultiPass (BiRP), mirroring the D3D12 own-device-bridge MultiPass path in both D3D11 sub-modes: built-player zero-copy (two plain Unity-device per-eye textures + same-device CopySubresourceRegion + Flush) and editor own-device bridge (two shared single-slice per-eye textures + fence-ordered own-context CopySubresourceRegion). BiRP + D3D11 is now fully supported (editor + player) — the previous no-start gate is removed. HW-verified on RTX 3080. (#195)

### Changed
- Render-path policy: BiRP → MultiPass on both D3D11 and D3D12 (was D3D12-only). URP/HDRP unchanged (SPI both APIs). Docs (CLAUDE.md render-path table + IsSinglePassEligible) updated.

## [2.3.1] - 2026-07-07

### Changed
- HDRP now defaults to Single-Pass-Instanced (SPI) on both D3D11 and D3D12 (was MultiPass on D3D12). URP + HDRP both default to SPI on both APIs; BiRP stays MultiPass (D3D12-only). The earlier HDRP+D3D12 SPI gate (#191, "washed-out splash") was dropped — that washout is a pipeline-wide HDRP lighting issue, not an SPI regression. Render-path policy documented in CLAUDE.md + IsSinglePassEligible. C#-only; native DLL unchanged.

## [2.3.0] - 2026-07-06

### Added
- D3D11 graphics backend for the provider: zero-copy in built players; editor own-device bridge (separate ID3D11Device + NT-handle shared 2-slice bridge + shared ID3D11Fence) that resolves the Optimus editor Play-Mode deadlock. SPI-only (URP/HDRP); BiRP+D3D11 warns and no-starts.
- wsui HUD / Local2D / extra-zone secondary layers render on D3D11 (both zero-copy and bridge sub-modes).

### Changed
- Provider backend selected from IUnityGraphics renderer (D3D11 or D3D12); D3D12 path unchanged (byte-identical).

## [2.2.2] - 2026-07-05

### Fixed
- Scene-view display-rig eye gizmo now tracks the window: the provider publishes display info + the runtime's Kooima canvas (window rect on the panel + physical size) to shared state, so the gizmo's eyes and convergence-plane aspect follow window move/resize instead of showing panel-relative fallbacks (follow-up to #189).

## [2.2.1] - 2026-07-05

### Fixed
- Scene-view eye-position gizmos were frozen at nominal under the provider; the provider now
  publishes live raw eye positions to shared state so the gizmo tracks head movement (#189).

## [2.2.0] - 2026-07-05

**URP off-axis simplification + HDRP support, plus the two already-merged cleanups (#185 meta hygiene, #186 preview-close-stops-play).** The provider now hands Unity a **full per-eye projection matrix** instead of half-angle FOVs, so URP and HDRP both consume the off-center Kooima frustum correctly with no per-pipeline fix. The URP `KooimaProjectionFixFeature` is removed. No app-facing API change; the projection change is internal to the native provider. Hardware-verified on RTX 3080 across BiRP, URP (2D-UI + transparent w/ foreground clip + click-through), and HDRP (#22, #166 M3).

### Changed
- **Provider hands Unity a full projection matrix** (`kUnityXRProjectionTypeMatrix`) — all four frame-desc projection sites (SPI + Multi-Pass, primary + extra-zone) now build a column-major GL-clip matrix from `XrView.fov` via the new `dxr_prov_build_projection` helper (same matrix as the stereo-readback path), instead of `kUnityXRProjectionTypeHalfAngles` + `tanf`. This carries the off-center frustum shear in the matrix itself, which URP and HDRP consume correctly (previously URP re-derived a projection from the half-angles and mangled strongly off-center frustums — Unity #1328435).

### Removed
- **URP `KooimaProjectionFixFeature`** (#22/#127) — the `ScriptableRendererFeature` that re-pushed the correct per-eye projection on URP is deleted, along with `DisplayXRUrpAutoWire` and the `DisplayXR > Setup URP Projection Fix` / `Auto-Wire URP Projection Fix` menu items. It is no longer needed now that the provider delivers a full projection matrix. **Migration:** URP projects that previously had the "Kooima Projection Fix" renderer feature auto-wired will show a harmless *missing script* entry on their URP renderer after upgrading — remove that renderer-feature entry (the projection is correct without it). The opt-in `DisplayXR > Setup URP Foreground Clip` and the `DisplayXR/ForegroundClipURP` shader are unchanged.

### Added
- **HDRP off-axis support** (#22, #166 M3) — HDRP consumes the provider's projection matrix natively (no fix code, no URP package required). A new `displayxr-unity-test-hdrp` regression repo covers it.

## [2.1.0] - 2026-07-04

Internal cleanup only — **no public API change**. Every C# P/Invoke export is preserved and the shipped Windows DLL is byte-identical in size. This release finishes Task 3 of the hook-removal epic (#166): the dead OpenXR-hook graphics-backend plumbing that survived the v2.0.0 hard-removal is now deleted.

### Removed
- **Dead hook graphics-backend plumbing** (#166, PR #183) — the six `GraphicsBackend` translation units and the readback subsystem (only ever driven by the removed OpenXR API-layer hook / SA preview) are deleted.
- **Hook function-pointer trampolines** — `s_real_*` / `s_next_gipa` hooked `xrGetInstanceProcAddr` fn-pointers and `win32_inject_window_binding` removed.
- **Dead hooked/standalone submission paths** — the hooked and standalone wsui + Local2D composition-layer submission code paths (unreachable under the provider) removed.

### Changed
- No behavioral change to the shipping provider path. The removal is confined to code that was already unreachable after the v2.0.0 hook hard-removal; provider rendering, transparency, zones, Local2D, and wsui are unaffected.

## [2.0.0] - 2026-07-03

**Provider-only (breaking).** The custom **`IUnityXRDisplay` display provider** is now the *sole* rendering path. The legacy OpenXR API-layer hook and the standalone (SA) editor-preview session/window — soft-deprecated in v1.24.0 — are **hard-removed** (#166, PR #177). Play Mode runs the provider directly and *is* the preview; there is no separate preview window. Apps that referenced the removed hook/SA/preview symbols must migrate to the provider surface before upgrading.

### Removed (breaking)
- **OpenXR API-layer hook** — `DisplayXRFeature` and `displayxr_hooks.cpp` deleted. Re-homed glue moved to `displayxr_native_shared.cpp`; native headers renamed (`displayxr_hooks.h`→`displayxr_exports.h`, `displayxr_hooks_internal.h`→`displayxr_backend.h`).
- **Standalone (SA) editor-preview** session + preview window — `DisplayXRPreview*` / `GameViewOverlay` C# and the SA preview code paths removed.
- **Removed public native exports** — `displayxr_request_display_mode`, `displayxr_standalone_*`, and the `DisplayXRPreviewInput` surface are gone. The SA render-to-atlas core (`displayxr_standalone*`) is kept **on disk but not compiled** (dormant), reserved as the seed for a future many-view "quilt" render path.

### Added
- **ADR-007 render-path-by-view-count guard** — documents the provider (≤8 views) vs future quilt (>8 views) split; the provider emits a one-shot WARN when a display advertises more than 8 views (Unity's `IUnityXRDisplay` caps at 8 views/frame).

### Fixed
- **Click-mask** — `displayxr_set_canvas_rect` re-homed (commit 0c3ee9b) after the hook translation unit was removed, restoring the canvas-rect / hit-mask path under the provider.

### Changed
- **Docs + `CLAUDE.md` are provider-only** — hook/SA/preview-window references retired; superseded ADRs marked historical. `docs~/architecture/xr-display-provider.md` is the current reference.

## [1.24.1] - 2026-07-02

### Fixed
- **Native provider now deploys into player builds (#166).** Unity's player build did not
  auto-include this package's native library (`displayxr_unity`) or its
  `UnitySubsystemsManifest.json` — unlike a first-party XR package, the custom
  `IUnityXRDisplay` provider package isn't recognized by Unity's XR build pipeline, so a clean
  consumer build shipped without the native provider and the app failed to weave. A new post-build
  step (`DisplayXRProviderRuntimeDeploy`) copies the shipped native binary + subsystem manifest into
  the player's data folder (Windows `_Data/Plugins/x86_64` + `_Data/UnitySubsystems/DisplayXR`; the
  macOS `.app` equivalents), mirroring how the existing build processor deploys the macOS OpenXR
  loader. Fixes clean `#upm` consumer builds (previously the bits had to be hand-copied).

## [1.24.0] - 2026-07-02

The custom **`IUnityXRDisplay` display provider becomes the shipping rendering path** (epic #166). The provider drives the DisplayXR runtime directly — Single-Pass-Instanced on URP + Windows + D3D12, MultiPass elsewhere — replacing the legacy native OpenXR-hook path for the primary workflow. The old `DisplayXRFeature` hook is soft-deprecated (`[Obsolete]`) but still functional. The repository is relicensed to **Apache-2.0**.

### Added
- **Custom `IUnityXRDisplay` display provider** (#166) — a Unity display provider that drives the DisplayXR runtime directly instead of intercepting Unity's OpenXR pipeline. Delivered across milestones M1 (provider skeleton), M1b (bridge — full stereo SPI weave on the Leia panel), and M2 (native-app parity control plane + cross-device handoff, in-app one-window weave).
- **SPI-vs-MultiPass gating by render pipeline** (#166) — Single-Pass-Instanced on URP + Windows + D3D12, MultiPass elsewhere.
- **Transparency + arbitrary-N 3D display zones + Local2D** under the provider (#166) — per-zone swapchain/bridge/locate/pass, multi-zone transparent mask, and LMB cyclopean hit-test.
- **Per-eye foreground clip on both pipelines under the provider** (#166) — BiRP foreground clip shader + MultiPass post-process AA, and per-zone/per-eye URP foreground clip.
- **Live tile reallocation** (#172) — zone-rect tile realloc for primary + extra zones and realloc on window resize.
- **App-owned / dedicated / self-host weave targets** (#173) — provider defaults to an app-owned window; self-host behind `DISPLAYXR_PROV_SELFHOST`; a dedicated movable weave window for in-editor Play Mode.
- **Provider runs in Play Mode** (#171) — provider-aware editor status; `xreditorsubsystem` keyword so the display subsystem is discoverable in-editor.
- **App-facing atlas screenshot** (`I` key) via `XR_DXR_atlas_capture` — screenshot parity with the hook path (#140).
- **Shared smooth 2D↔3D mode-switch sequencer** (`DisplayXRModeSwitch`) (#172).
- **Window-space UI (HUD) composition layer** under the provider (#166).
- **DLL code-signing** in the `.tgz` and `upm` branch (#167).

### Fixed
- **Provider hardware fixes** (#166): dedicated-window drag/input/teardown; keyboard input under the overlay via focus/raw-input hooks; Unity window-drag bracketing for SR phase-snap; single-application rig pose so the URP foreground clip tracks the moving display plane; URP `eye_world` view+proj for silhouette match; publish URP foreground-clip globals for the camera-centric rig; route hardware 2D/3D mode requests to the provider session; keep zones active in 2D so zoned content stays in its band; honor hardware 2D mode by submitting one full-res view; provider-aware boot splash.
- **Multi-rig switch** (#166): render only the active rig camera so multi-rig switching is correct.
- **Cross-platform link** (#166): guard provider mode-routing behind `_WIN32` so macOS/Linux link.
- Doc correction: built apps are 2-view only (no view synthesis) (#165).

### Changed
- **Legacy OpenXR-hook `DisplayXRFeature` soft-deprecated** (`[Obsolete]`) (#166) — the provider is now the shipping path; the hook remains functional for compatibility.
- **Repository relicensed to Apache-2.0** — vendor-neutral `NOTICE`; vendored OpenXR headers stay BSL-1.0.

## [1.23.0] - 2026-06-28

### Added
- **Single-Pass-Instanced (SPI) stereo rendering** on URP + Windows + D3D12, auto-gated by platform and runtime version (requires DisplayXR runtime >= v1.26.1); falls back to MultiPass otherwise (#162).

### Changed
- Rewrote `docs~/architecture/hook-chain.md`: two rendering paths, `PRIMARY_STEREO` clarification, SPI corollary, and an overlay/runtime-integration note (#163).

## [1.21.0] - 2026-06-19

### Added
- **`XR_DXR_display_zones` port — 3D display zones**: the plugin renders into runtime-advertised display zones, sizing the 3D eye render to `xrGetDisplayZoneRecommendedViewSize` (via `renderViewportScale`) so the zone-sized stereo render no longer leaks a band under zoom.
- **Local2D composition layer (#439/#491)** — modern 2D-over-3D path: a `DisplayXRLocal2D` component composites a 2D layer over the 3D scene.
- **Avatar-style windowing primitives** on the transparent overlay: native toggle-decoration (B), keyboard window resize, drag-move, get/set overlay window position (for app-side persistence), and consume-overlay-close-request (close-to-quit). Window-chrome UI policy moved out of the plugin into the app.

### Fixed
- **URP per-eye view (#127)**: each eye now renders with the runtime's `eye_world` VIEW matrix instead of URP's view, fixing the off-axis URP path.
- Local2D bubble vanish under foreground clip.

### Changed
- **Window-chrome UI policy moved out of the plugin into the app** — the plugin exposes native windowing primitives (decoration toggle, resize, position get/set, close-request) and the app owns the UX policy.

### Fixed
- **`XR_DXR_view_rig` SPEC_VERSION 3 compatibility** (native): the runtime advanced the extension to SPEC 3, which adds a trailing `metersToVirtual` float to `XrCameraRigDXR`. The plugin shipped the SPEC 2 struct, so against a SPEC 3 runtime the runtime read that field past the end of the plugin's struct for **camera-centric** rigs (`DisplayXRCamera`) — undefined value (best case 0 → runtime's pre-v3 default of 1.0, worst case garbage → wrong world scale). The plugin now declares SPEC 3 and writes `metersToVirtual = 1.0f` (scene scale is already folded into `convergenceDiopters`, so this exactly preserves pre-v3 behavior). **Display-centric rigs (`DisplayXRDisplay` → `XrDisplayRigDXR`) were unaffected** — that struct is byte-identical across SPEC 2/3 — so the transparent/2D-UI display-centric demos already worked on a SPEC 3 runtime; this fixes the camera-centric path. Detection remains name-based (no version gate).

## [1.20.0] - 2026-06-15

### Added
- **URP off-axis projection fix** (#127/#129): URP ignores `Camera.SetStereoProjectionMatrix` (Unity #1328435) and builds each eye's projection from `views[i].fov`, which it mishandles for strongly off-center window-relative Kooima frustums (head x<0 shifts/deforms — the prior "URP off-center" known limitation). A new URP-guarded sub-assembly (`Runtime/URP/`, `Editor/URP/`) ships **`KooimaProjectionFixFeature`**, a `ScriptableRendererFeature` that re-pushes the runtime's correct per-eye `leftProj`/`rightProj` via `cmd.SetViewProjectionMatrices` at `BeforeRenderingOpaques` (URP pushes the projection once per eye-pass at camera setup, so it sticks). Has a NaN/identity startup guard. **Auto-wired** into the URP renderer when a DisplayXR rig is in an open scene (`DisplayXRUrpAutoWire`; toggle `DisplayXR > Auto-Wire URP Projection Fix`, or run `DisplayXR > Setup URP Projection Fix`).
  - The sub-assembly is gated by `defineConstraints: ["DISPLAYXR_URP"]` + a `versionDefines` that only defines `DISPLAYXR_URP` for `com.unity.render-pipelines.universal >= 17.0.0` (RenderGraph), so BiRP-only and older-URP projects never compile it. Requires **URP 17 / Unity 6**.
- **Per-eye foreground clip on URP** (opt-in, #57/#129): a `DisplayXR/ForegroundClipURP` shader (in `Runtime/URP/Shaders/`, deliberately **not** in `Resources/` to keep BiRP builds safe — the #130 revert) does a per-eye depth-based foreground cut. The rig publishes both per-eye fars + eye positions via the `_DXRForegroundFar`/`_DXREyePosL`/`_DXREyePosR` globals; Unity's built-in `FullScreenPassRendererFeature` + a shipped material do the clip. When the clip is active, `KooimaProjectionFixFeature` re-pushes the off-axis projection with the **camera's full far** (rebuilding only the depth terms, off-axis shear untouched) so the scene renders fully and the shader's `eyeZ` reconstruction stays consistent with `_ZBufferParams` — the shader, not the projection, makes the per-eye cut. Wire with `DisplayXR > Setup URP Foreground Clip`.

### Changed
- The rig's URP branch (`DisplayXRDisplay`/`DisplayXRCamera.OnCameraPreRender`) no longer clamps `Camera.farClipPlane` to a single shared foreground far. Projection is now owned by `KooimaProjectionFixFeature`; the rig only publishes the per-eye `_DXRForegroundFar` globals for the opt-in clip pass (inert if that pass isn't wired). BiRP is unchanged (still `SetStereoProjectionMatrix`).

### Notes
- URP transparent overlays also require the per-project Player Setting **Preserve Framebuffer Alpha** (`preserveFramebufferAlpha = 1`); the plugin cannot set it at runtime. HDRP gets no off-axis fix (the RendererFeature is URP-only).

## [1.19.0] - 2026-06-09

### Changed
- **Kooima projection is now owned by the DisplayXR runtime via `XR_DXR_view_rig`** (`DisplayXR/displayxr-runtime#396` W7, ADR-024). The plugin no longer computes Kooima: it chains an `XrDisplayRigDXR`/`XrCameraRigDXR` descriptor (scene transform + the handful of tunables) onto `xrLocateViews` and consumes render-ready `XrView{pose, fov}` — on **both** the built-app hook path and the standalone editor-preview session. The former vendored/`displayxr::math` display3d/camera3d Kooima math is removed (~500 lines, plus `displayxr_kooima.{cpp,h}`; the `displayxr::math` link is dropped). The P/Invoke surface and C# API are unchanged.
  - **⚠️ Requires a DisplayXR runtime that advertises `XR_DXR_view_rig` (SPEC_VERSION 2) _and_ the #396 W7 window-metrics fixes — both ship in runtime `v1.16.0` (bundle `0.17.0`).** Against an older runtime the plugin emits a one-shot WARN and passes the raw views through, which renders **no stereo**. Update the runtime to `0.17.0`+ before installing this plugin version.
- The BiRP per-eye projection override (`SetStereoProjectionMatrix`) is **retained and always applied**. A probe showed BiRP consumes `views[i].fov` directly when the window is centered, but for **window-relative off-center** windows the rig fov is a sheared off-axis frustum that Unity's XR fov→projection path mishandles (over-separates the eyes) — so the plugin keeps building the projection itself. BiRP also keeps the `SetStereoViewMatrix` + `FlipViewZ` handedness shim.
- Foreground-only clip (#57 family) is preserved under the rig path: the rig fov is clip-independent, so the per-view far plane is rebuilt app-side (display rig = eye→display-plane distance; camera rig = convergence distance) — BiRP via `SetStereoProjectionMatrix`, **URP via `Camera.farClipPlane`** (URP ignores `SetStereoProjectionMatrix`).

### Fixed
- macOS (Metal): Gamma-space projects now downgrade the sRGB eye swapchain to UNORM on the Metal backend too (the branch was missing), fixing washed-out / overexposed output on macOS.
- macOS C# compile: a Windows-only fullscreen-overlay P/Invoke is now correctly guarded for Windows (#147) — it was latent because CI only builds the native plugin.
- Sample `DisplayXRInputController`: mouse-look drag is smooth again (uses `Mouse.current.delta` instead of a pointer-position diff).

### Known limitations
- **URP off-center window-relative Kooima**: URP ignores `SetStereoProjectionMatrix`, so the off-center frustum shear cannot be injected; centered / fullscreen URP is correct. (BiRP handles off-center.)

## [1.18.0] - 2026-06-09

### Added
- **DisplayXR boot splash on the zero-disparity plane** (#147): a DisplayXR logo + "for Unity" splash now renders on the zero-disparity plane as an on-by-default overlay. Opt out via the `DISPLAYXR_NO_SPLASH` environment variable. Ships with an accompanying UPM sample.

### Fixed
- Splash teardown now flushes the eye buffers on overlay teardown, avoiding a frozen atlas tile after the splash dismisses (#147).

### Changed
- CI: drift-guard fails the build if `displayxr::math` is re-vendored (`DisplayXR/displayxr-runtime#396` W5).

## [1.17.0] - 2026-06-05

### Changed
- **Kooima math now comes from the shared `displayxr::math` library** ([DisplayXR/displayxr-common](https://github.com/DisplayXR/displayxr-common) @ v0.2.0, pinned via CMake FetchContent) instead of vendored copies of `display3d_view.{c,h}` / `camera3d_view.{c,h}` (`DisplayXR/displayxr-runtime#396` W3 — last consumer; net −994 lines). Behavior-preserving: the projection convention is unchanged (GL `[-1,1]`, Unity converts via `GL.GetGPUProjectionMatrix`), `Camera.nearClipPlane`/`farClipPlane` are still honored as absolute view-space distances, and the foreground-only mode keeps its exact per-view semantics. The window-relative / canvas sub-rect Kooima input-prep (rect → screen meters + eye shift + Y-flip) in both the hooks path and the standalone editor-preview path is now the library's `display3d_resolve_window_rect()` (Layer 1). P/Invoke surface and C# API unchanged.

### Fixed
- The standalone (editor-preview) camera-centric path read an uninitialized foreground-only flag; the field is gone with the shared-library migration.

## [1.16.0] - 2026-06-05

### Changed
- Atlas screenshot filenames adopt the runtime-owned suffix `<stem>-<N>_atlas_<viewCount>_<cols>x<rows>.png` (XR_DXR_atlas_capture spec v2, `DisplayXR/displayxr-runtime#425`). The live `xrCaptureAtlasDXR` path now passes a bare `<stem>-<N>` prefix (no pre-baked `_<cols>x<rows>`) and lets the runtime own the `_atlas_…` tokens, so the final name no longer duplicates the layout (`..._2x1_atlas_2_2x1.png`). The editor-preview (app-side) path writes the same name so both share one sequence counter.

## [1.15.0] - 2026-06-04

### Changed
- **The 'I'-key atlas screenshot is now runtime-owned via `xrCaptureAtlasDXR`** (XR_DXR_atlas_capture, spec v1) for live OpenXR sessions (#140, #396 W6). A live session hands the runtime a path prefix; the runtime reads back its own composited atlas and writes `<prefix>_atlas.png`. The plugin no longer does an app-side `AsyncGPUReadback` or a hidden-camera Kooima re-render on the live path. New public API `DisplayXRFeature.CaptureAtlas(pathPrefix, projectionOnly)`. **Requires a DisplayXR runtime that advertises `XR_DXR_atlas_capture`** — against older runtimes the capture logs `…unavailable` and is a no-op (no crash). The editor-preview (standalone-session) path is unchanged: it still encodes the atlas RT app-side, since there is no runtime OpenXR session in pure-editor preview.

### Fixed
- Live-path screenshot no longer captures the white feedback flash. The flash draws into the same eye buffers the runtime composites for the capture, so arming it immediately whited out the saved atlas; the flash is now deferred a few frames so the runtime grabs the clean atlas first (#140).

## [1.14.0] - 2026-06-01

### Added
- Fullscreen 2D-surround region-editor support for the transparent overlay (#131): opt-in `displayxr_set_fullscreen_overlay_pref` births the overlay covering the monitor minus 1px (DWM-composited, covers the taskbar); new `displayxr_set_overlay_cursor` for app-driven overlay cursor shapes.
- `displayxr_get_canvas_rect_px` binding + sub-rect-aware cyclopean raycast in `DisplayXRTransparentOverlay` so LMB-rotate works with an active canvas sub-rect (#131).

### Fixed
- Startup white flashes + hang with a fullscreen transparent overlay: an exact-monitor window tripped Windows fullscreen-optimization / independent-flip (DWM-alpha bypass); the overlay is now born 1px short of the monitor to stay DWM-composited (#131).

### Changed
- Coarse `[+ms]` relative timestamps on every native log line.

## [1.13.0] - 2026-05-31

### Added
- Per-pixel surround click-through mask: `displayxr_set_overlay_surround_mask(mask, mask_w, mask_h, dst_x, dst_y, dst_w, dst_h)` registers the exact shape of a 2D surround element (e.g. a comic bubble with a triangular tail) as an alpha mask, RLE-unioned into the transparent-overlay `SetWindowRgn` region so the empty area beside the shape keeps routing clicks to the desktop — which the single bounding rect (`displayxr_set_overlay_surround_rect`) could not express. Windows hooked path; macOS resolves via the C# EntryPointNotFound fallback. (#131)

## [1.12.0] - 2026-05-31

### Fixed
- Sub-rect-aware Kooima — physically-correct 3D inside the 2D-surround canvas sub-rect (the frustum/FOV + convergence are now computed for the sub-rect the content actually weaves into, not the full window; no change when no canvas rect is set). (#131)

## [1.11.0] - 2026-05-30

### Added
- **2D surround on the hooked path (#131): high-res 2D content composited post-weave over the woven 3D.** A D3D12 fence-synced surround manager + `DisplayXRSurround` component render a full-resolution 2D layer (e.g. a text bubble) over the woven stereo image, confined to a canvas sub-rect. (#131)
- 2D surround on the standalone session too — works in both Editor Preview and Play Mode. (#131)
- Surround foundation: v7 extension typedefs + hooked canvas-rect re-apply. (#131)

### Fixed
- macOS build: guard Win32-only `sa_push_canvas_rect_to_runtime` calls so the standalone build compiles on macOS. (#131)
- Sub-rect-aware transparent-overlay click-through and a solid silhouette mask for the surround region. (#131)
- Silhouette mask: pin clip-space z so the near/far planes don't carve wedges out of the mask. (#131)
- Surround no-bubble: size the layer to the HWND weave target instead of the panel dimensions. (#131)
- `DisplayXRSurround` retries setup until the display dimensions are valid. (#131)
- Standalone runtime load failing with `ERROR_MOD_NOT_FOUND` (126) via an altered-search-path fix. (#131)

### Changed
- Documentation (CLAUDE.md): streamlined build steps, pruned runtime cruft, and documented independence from the runtime `versions.json` matrix.

## [1.10.0] - 2026-05-26

### Added
- Vulkan rendering backend for Windows, including a Vulkan editor standalone-preview backend. (#122, #124)

### Fixed
- Vulkan: correct dark-image/gamma via VkFormat sRGB→UNORM downgrade. (#122)
- Vulkan: standalone-session binding + CreateExternalTexture handle on the Vulkan editor path. (#124)
- Build post-process tolerates a locked app-icon file (DisplayXR Shell holds icons open) instead of failing.

## [1.9.0] - 2026-05-25

### Added
- Rig-level post-process FXAA anti-aliasing pass (`DisplayXRPostAA`) with a `postProcessAntiAliasing` toggle on DisplayXRCamera/DisplayXRDisplay. (#121)

### Fixed
- Preview-path silhouette aliasing via MSAA intermediate atlas RT. (#120)

### Changed
- **Post-process anti-aliasing now defaults to ON on DisplayXR rigs.** Unity drops MSAA on the XR eye render target (submits sampleCount=1 on D3D12 and Vulkan), so the plugin applies an FXAA pass to restore soft edges. This is a behavior change for existing projects (an extra per-eye blit, negligible cost). Built-in Render Pipeline only — under URP/HDRP `OnRenderImage` does not fire and the pass is a no-op; disable per-rig via the inspector toggle if not wanted. (#121)

## [1.8.1] - 2026-05-23

### Fixed
- Edit Mode preview: gizmo now updates real-time during window drag. (#119)

## [1.8.0] - 2026-05-23

### Added
- Window-relative gizmo + real-time resize intercept: per-eye Kooima frustum gizmos now track the standalone preview window during resize/move, with the resize intercept pushing canvas updates so visualization stays in sync with the live render canvas. (#111, #118)

### Changed
- Documentation: updated deployment notes for the post-#263 plug-in architecture. (#117)

## [1.7.3] - 2026-05-21

### Added
- Scene-view per-eye Kooima frustum gizmos for visualizing stereo projection in the editor. Extended to N views via standalone preview state, with display-centric m2v + scale-as-zoom applied and gating reworked so selection drives Edit Mode while the active rig drives preview. Includes filled-sphere eye glyphs (bumped to 3 cm) for visibility and accepts untracked-but-valid sim_display poses. (#111)

### Fixed
- macOS URP centering: N-view Kooima path now includes URP head-pose compensation so URP scenes render centered. (#115)
- Play Mode default-to-2D + macOS window-relative Kooima in built apps.
- Kooima frustum: near + far planes are now parallel to the display. (#111)
- Stale native bundle: catch `EntryPointNotFoundException` so older binaries fail gracefully. (#111)
- Guard hit-mask push for macOS compile. (#111)

### Changed
- Ship CI-built macOS bundle with N-view accessor. (#111)

## [1.7.2] - 2026-05-18

### Fixed
- Editor compile on Unity 6.0.x: reverted three Unity 6.1+ APIs in `Editor/DisplayXRPreviewSession.cs` back to cross-version-safe equivalents — `Camera.GetEntityId` → `GetInstanceID`, `EditorUtility.EntityIdToObject` → `EditorUtility.InstanceIDToObject`, and single-arg `FindObjectsByType<T>(FindObjectsInactive)` → two-arg `FindObjectsByType<T>(FindObjectsInactive, FindObjectsSortMode)`. Partners on Unity 6000.0.x were hitting 5 compile errors when importing the package; this restores Unity 6.0+ compatibility without affecting Unity 6.1 behavior. #109 (#110)

## [1.7.0] - 2026-05-14

### Added
- Transparent overlay: per-pixel silhouette click-through region (#57). Plugin computes a per-eye silhouette mask, unions both eyes, and applies it cross-process via `SetWindowRgn` so the OS-level hit-testing matches the rendered avatar shape — clicks land on the avatar; non-silhouette pixels pass through to the desktop. Replaces the prior coarse rect-based click-through. #105

### Fixed
- 2D UI window-space composition layer: set `XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT` on the layer flags so the runtime compositor blends UI textures with the documented unpremultiplied-alpha convention. Eliminates dark fringing around anti-aliased UI edges on transparent backgrounds. #105

## [1.6.0] - 2026-05-13

### Changed (breaking)
- Windows transparent overlay is now **alpha-native end-to-end** — same path as macOS. Unity emits per-pixel alpha to the swapchain (`SetEnvironmentBlendMode(AlphaBlend)` is no longer gated to macOS) and the runtime DP composes the captured desktop content under each tile pre-weave + alpha-gates post-weave. Anti-aliased silhouettes get true soft alpha; the v1.3.0 "hard-mask alpha (0 or 1) on Leia hardware" known limitation is gone.
- **Removed** the chroma-color workaround:
  - `DisplayXRTransparentOverlay.RequestChromaKey(Color)` static method
  - `DisplayXRTransparentOverlay.chromaKeyColor` field/property
  - Native `displayxr_set_transparent_chroma_key()` export
  - Internal `transparent_chroma_key_color` shared state
  - `colorKey` argument on `displayxr_set_transparent_overlay()` (signature is now `(enabled, topmost)`)
  - Comments / docs / sample text referring to the chroma color
  - The Camera clear in `OnEnable` is now unconditionally `(0,0,0,0)` on both Windows and macOS.
  - Apps that called `RequestChromaKey` will get a compile error; just delete the call — the alpha-native path is automatic. Resolves `DisplayXR/displayxr-unity#103`.

### Compatibility
- Requires a DisplayXR runtime that (a) advertises `XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND` on the Windows D3D11/D3D12 service compositor and (b) implements the compose-under-bg + alpha-gate DP path (formerly tracked as runtime#190). Older runtimes will fail `xrEndFrame` validation because they don't enumerate `ALPHA_BLEND` — same failure signature as v1.5.6 → v1.5.12. Update both plugin and runtime in lockstep.
- macOS path unchanged.

## [1.5.13] - 2026-05-13

### Fixed
- Windows transparent overlay regression from #85: `SetEnvironmentBlendMode(AlphaBlend)` is now gated to macOS. The Windows DisplayXR runtime (<= v1.3.0-6) does not enumerate `ALPHA_BLEND`, so Unity rejected the call and every `xrEndFrame` failed validation — content never reached the swapchain. Windows transparency continues to use the chroma-key path.

## [1.5.12] - 2026-05-14

### Fixed
- macOS: `displayxr_macos_set_window_borderless` switched to true `NSWindowStyleMaskBorderless` (= 0). v1.5.11's "titled but visually empty" approach left a 1-2 px top-edge contour visible. The earlier concern that mask=0 would break keyboard input (per Cocoa's default `canBecomeKeyWindow=NO`) turned out to be a separate bug (sample's `HAS_INPUT_SYSTEM` gate, fixed in v1.5.11). Empirically Unity's `PlayerWindow` overrides `canBecomeKeyWindow` to return YES regardless of mask, so true borderless works for keyboard. Retains the defensive `makeKeyAndOrderFront:` + `activateIgnoringOtherApps:` after the styleMask change. #101

## [1.5.11] - 2026-05-14

### Fixed
- `Samples~/DefaultInputController/DisplayXRInputController.cs` (default input sample): switched the input-system gate from `HAS_INPUT_SYSTEM` to `ENABLE_INPUT_SYSTEM`. `HAS_INPUT_SYSTEM` is the plugin's internal `versionDefines` symbol — only visible inside the plugin's asmdef. After the v1.5.9 refactor moved the sample into user `Assets/`, the symbol wasn't defined there, so the keyboard helpers fell through to legacy `Input.GetKey` (returns false in projects using New Input System only). WASD / V / Space / I / F11 silently broken in the sample since v1.5.9. `ENABLE_INPUT_SYSTEM` is Unity's official symbol set by Player Settings → Active Input Handling, visible to all assemblies. #100
- macOS: `displayxr_macos_set_window_borderless` now defensively re-keys the window after the styleMask change (`makeKeyAndOrderFront:` + `NSApp activateIgnoringOtherApps:`). In observed runs the Cocoa default kept the window key on its own, but this guards against future quirks. Also slims diagnostic logs from three lines to one. #100

## [1.5.10] - 2026-05-14

### Added
- macOS: `displayxr_macos_set_window_borderless(int enabled)` primitive. Toggles Unity's configured NSWindow between borderless (no title bar / close / minimize / resize chrome — avatar/floating-window look) and the saved original style mask. App-controlled; default behavior unchanged for existing apps. Save/restore symmetric. Drag stays via the cursor-anchored API (begin/update/end_window_drag) — Cocoa's default title-bar drag is gone with the title bar. #98

## [1.5.9] - 2026-05-14

### Changed (breaking for consumers using DisplayXRInputController)
- `DisplayXRInputController` moved out of plugin `Runtime/` into `Samples~/DefaultInputController/`. Plugin Runtime now contains only mechanisms (cursor polling, rig manager, mode setter); input policy lives in app code. Consumers import via Package Manager → DisplayXR → Samples → "Default Input Controller". Same class name, same `DisplayXR` namespace, same fields — just sourced from the project's `Assets/` folder. Scenes referencing the old Runtime type will fail to deserialize until the sample is imported. No deprecation shim — a shim sharing the namespace would collide with the imported sample. #97
- `DisplayXRNative` promoted from `internal` to `public` (class + all P/Invoke methods + `LogCallback` delegate). Enables the sample (now in user Assets/) to call the same bindings the plugin uses. Stability contract: method signatures track underlying native exports; prefer high-level wrappers (`DisplayXRFeature`, `DisplayXRTransparentOverlay`, `DisplayXRRigManager`) where they exist.

### Added
- `DisplayXRInputController.scrollZoomEnabled` field (sample) — parallel to `mouseLookEnabled`. Apps that drive their own scroll-based zoom can set false to opt out of the controller's built-in scroll → camera transform / FOV change.
- macOS `ConsumeWheelDelta` now returns real values (was Win32-only / 0 on Mac). `DisplayXRTransparentOverlay`'s Mac LateUpdate branch accumulates `Mouse.current.scroll.y × 120` per frame; same Win32-unit semantics as the Win32 path. Unblocks `WheelZoomVHeight`-style app wheel handlers on Mac.

### Migration
- Existing apps with `DisplayXRInputController` in scenes need to import the sample. After import, the project-owned copy in `Assets/Samples/com.displayxr.unity/.../Default Input Controller/` resolves scene references via the preserved meta GUID — no scene edits required.

## [1.5.8] - 2026-05-14

### Added
- `DisplayXRInputController.mouseLookEnabled` field (default true) to opt out of the controller's built-in left-mouse-drag → camera rotation. Apps that drive their own hit-tested left-drag interactions (e.g. `DragRotateCube` on a scene target) can set it to false to reserve left-drag for the app's hit-tested target. WASD movement, scroll zoom, and keyboard controls are unaffected. #96

## [1.5.7] - 2026-05-14

### Added
- macOS: cyclopean per-triangle hit-test + onPointer events ported from the Win32 path to the Mac overlay's LateUpdate. `DisplayXRTransparentOverlay.onPointerEnter/Exit/Down/Up/Click` now fire on Mac, allowing app code to know when the cursor is over a clickable renderer. Side effects: `DragRotateCube` left-click-drag-rotate-tiger now works on Mac (was silently no-op), and the test repo's `MacRightDragMoveWindow` can gate right-drag-to-move on the cursor being on the tiger. Hit-test logic mirrors Win32 (UpdateBakedHitColliders / TryGetStereoMatrices / BuildCyclopean / TryBuildEyeRay) with the Win32-only click-through plumbing (`displayxr_set_overlay_hit_*`) skipped — Mac click-through is future Phase 2 of #85. #95

## [1.5.6] - 2026-05-14

### Added
- macOS: cursor-anchored window-drag API (`displayxr_macos_begin_window_drag` / `_update_window_drag` / `_end_window_drag`). Recommended over the existing `offset_window` primitive for mouse-drag use cases — does all the cursor↔window math in pure Cocoa coords inside the plugin, avoiding the scale/feedback issues that occur when feeding `Mouse.current`-derived deltas to `offset_window` on Retina + HiDPI displays. Cursor stays glued to the same window-relative spot for the full drag. #94

## [1.5.5] - 2026-05-14

### Fixed
- **Windows editor preview wsui composition layer rendering** — both `StandaloneD3D12Backend::wsui_copy_to_swapchain_image` and the D3D11 equivalent were stubs that returned false (cross-device copy from Unity's D3D device to the standalone SA D3D device was unimplemented). Filled in by mirroring the existing atlas-bridge pattern: SHARED NT-handle `ID3D12Resource` on the SA device, opened on Unity's D3D12/D3D11 device. C# `Graphics.CopyTexture`s the wsui RT into the bridge each frame; SA backend then copies bridge → swapchain image with a fence wait. Also plumbed `app_pref` through `enumerate_and_pick_format_standalone` so the standalone swapchain matches the bridge's `B8G8R8A8_UNORM` (a cross-format CopyTextureRegion was crashing the runtime in the NVIDIA driver). Mac unaffected (Metal unified MTLDevice).
- **Built-app cube Y shift under URP** — Unity 6 OpenXR defaults to Floor tracking origin mode (eye Y ≈ user height ≈ 1.5–1.7m). The plugin's `xrLocateViews` hook returns LOCAL-space eye coords for Kooima. Under BiRP, Unity reads these directly — fine. Under URP, the RenderGraph picked up `XRInputSubsystem`'s Floor offset and added it on top → cube rendered shifted by ~head height in built apps using URP (`displayxr-unity-test-2d-ui`), while BiRP test repos (`displayxr-unity-test`, `displayxr-unity-test-transparent`) rendered correctly. Now forced to Device mode at `RuntimeInitializeOnLoadMethod(AfterSceneLoad)`.
- **Built-app Render Mode button dead on Windows D3D12** — `XR_EXT_display_rendering_mode` wiring was D3D11-only and `#if defined(_WIN32)`-gated, so the D3D12 hooked backend never enumerated modes and the standalone C ABI shims returned 0 in built apps (no `s_sa.session`). Rendering-mode bookkeeping promoted to `GraphicsBackend` base — D3D11, D3D12, Metal, Vulkan, GL all inherit it for free. C ABI shims fall back to the hooked backend's `rendering_modes[]` when no standalone session is running. Render Mode button now cycles in built apps on all platforms.

### Changed
- Refactored rendering-mode state + methods from `D3D11Backend` to `GraphicsBackend` base class. `D3D11Backend`'s atlas-swapchain logic still reads `current_rendering_mode_index` via the inherited field. No behavior change on D3D11 hooked path.

## [1.5.4] - 2026-05-13

### Added
- macOS: `displayxr_macos_offset_window(dx, dy)` primitive for app-driven borderless-window drag (e.g. right-click-drag-to-move). App owns the input policy, plugin owns the `[NSWindow setFrameOrigin:]` mechanism. Win32 keeps its built-in WndProc-based drag for now (coupled to SR weaver phase-snap on the overlay HWND); architectural unification deferred. #93

### Changed
- `native~/build-mac.sh` no longer `rm -rf build` on every invocation. Re-runs reuse the FetchContent'd OpenXR-SDK clone — local rebuilds drop from 30-60s to ~5s. Pass `--clean` to force a full rebuild.

## [1.5.3] - 2026-05-13

### Fixed
- macOS: push initial rendering mode to runtime in DisplayXRInputController.Start so 3D mode is active at first frame (previously the C# default `m_CurrentRenderingMode = 1` was never pushed; macOS sim_display defaults to 2D / passthrough, requiring two V keypresses to reach 3D). #92
- macOS: `displayxr_is_our_process_foreground` now returns 1 unconditionally on Mac. The Win32 reason for the gate (RIDEV_INPUTSINK delivers keystrokes system-wide) doesn't apply to Cocoa, and `[NSApp isActive]` had transient false-negative windows during app-activation handoff making Shift+Tab feel unreliable. #92

## [1.5.2] - 2026-05-13

### Fixed
- macOS: DisplayXRTransparentOverlay now updates PointerPosition / PointerDelta / IsLeftPressed via Unity's Mouse.current. Was previously gated entirely on UNITY_STANDALONE_WIN, leaving HUD slider drag and other app code that reads these properties non-functional on Mac. Active-rig gate matches Win32 (#91).

## [1.5.1] - 2026-05-13

### Fixed
- macOS: implement `displayxr_is_our_process_foreground` (was Win32-only). Unblocks Shift+Tab HUD toggle and any other C# caller that gates input/UI on app-active state. Uses `NSApplication.isActive` — same semantics as the Win32 foreground-window-PID check.

## [1.5.0] - 2026-05-12

### Added
- **macOS transparent overlay — Phase 1 visual transparency (`#85`)** —
  `XR_DXR_cocoa_window_binding` is wired through with the
  `transparentBackgroundEnabled` flag, and Unity's `NSWindow` is configured
  for per-pixel alpha so the runtime can render into a transparent surface
  on macOS. Mirrors the Windows transparent overlay capability (#57) at the
  plumbing layer; visual transparency now functions end-to-end on macOS.

### Fixed
- **macOS transparent overlay — clear contentView's CAMetalLayer.contents
  (`#86`)** — Unity's contentView retains a stale `CAMetalLayer.contents`
  image that was occluding the runtime's transparent surface, so
  `alpha = 0` regions appeared opaque even with all other plumbing in
  place. The plugin now clears `contentView.layer.contents` after the
  window is reconfigured for transparency, allowing the desktop to show
  through. Completes the macOS transparent overlay end-to-end visual
  verification.

### Changed
- Documentation updates around test-repo workflows and `#82` known-issue
  cleanup carried in for this release.

## [1.4.1] - 2026-05-12

### Fixed
- **wsui + transparent overlay crash (`#82`)** — combining
  `DisplayXRTransparentOverlay` with `DisplayXRWindowSpaceUI` no longer
  crashes the runtime in `xrEndFrame` the first frame the wsui swapchain
  image is copied into. Root cause was a format mismatch: the plugin's
  wsui swapchain was created in `DXGI_FORMAT_R8G8B8A8_UNORM` (the picker's
  hard-coded first preference) while Unity's wsui Canvas `RenderTexture`
  lands in `DXGI_FORMAT_B8G8R8A8_UNORM` on Windows D3D12.
  `CopyTextureRegion` across formats is invalid per the D3D12 spec; the
  release driver silently tolerated the byte permutation on opaque
  flip-model swapchains (so `-test-2d-ui` always worked), but DComp-backed
  transparent swapchains hand the cmd list to a stricter compositor-surface
  validation path that flags it as `DXGI_ERROR_INVALID_CALL` at GPU
  execution time and removes the device. Fix: the plugin now queries
  Unity's RT format via a new `GraphicsBackend::wsui_get_native_texture_format()`
  (implemented for D3D11 and D3D12) and asks the runtime to create the
  wsui swapchain in that exact format — `pick_overlay_format()` takes an
  optional app preference that is tried first.
- **D3D12 resource barriers around the wsui copy** — explicit
  `ResourceBarrier(COMMON → COPY_DEST/COPY_SOURCE → RENDER_TARGET/COMMON)`
  bracket `wsui_copy_to_swapchain_image`. Doesn't resolve `#82` on its own
  but hardens the copy against state-tracking drift regardless of the
  format-match fix.
- **`displayxr.log` path resolution** — `displayxr_log` now resolves an
  absolute path (preferred: `<ExeDir>\displayxr.log`, fallback:
  `%TEMP%\displayxr.log`, last resort: CWD-relative). Previously
  `fopen("displayxr.log", "w")` used Unity's CWD, which the built player
  doesn't guarantee matches the `.exe` directory — so the log file was
  effectively never created in built apps. The chosen path is announced
  via `OutputDebugStringA` and as the first line of the log itself.

## [1.4.0] - 2026-05-11

### Added
- **Per-view foreground-only clip tunable** — new `clip_at_display_plane`
  boolean in the native tunables struct (`Display3DTunables` /
  `Camera3DTunables`) that, when set, overrides each view's projection
  `far_z` with that view's distance to the display plane. Per-view and
  N-view safe — the Kooima per-view loop in `xrLocateViews` already runs
  once per output, so the override scales to 2-view stereo, 4-view quad,
  and N-view lenticular without further changes. Exposed in C# as
  `DisplayXRDisplay.foregroundOnlyClip` and `DisplayXRCamera.foregroundOnlyClip`
  (both inspector-visible with tooltips, pushed in `LateUpdate` alongside
  the other tunables). Resolves `displayxr-unity-test-transparent#2`.
  - Why per-view in native: Unity's XR pipeline reads per-eye projection
    from `xrLocateViews` output, NOT from `Camera.SetStereoProjectionMatrix`.
    A C# override updates Unity's matrix cache (visible to scene-view,
    culling, shadows) but never reaches the GPU draw. Doing the per-view
    `far_z` override inside the native Kooima hook is the only chain that
    affects the rendered image.
  - In display-centric rigs the clip distance is `|eye_scaled.z|`; in
    camera-centric rigs it is `1 / inv_convergence_distance`.
  - The `displayxr_set_tunables` P/Invoke signature gained one trailing
    `int` parameter — additive, but recompile required.
- `CLAUDE.md` Test repos section listing the three sibling Unity test
  projects (`-test`, `-test-transparent`, `-test-2d-ui`) so future
  contributors know the regression surface.

### Fixed
- **`DisplayXRTransparentOverlay` per-triangle SMR hit-test** — clickables
  with a `SkinnedMeshRenderer` are now ray-tested per-triangle
  (Möller-Trumbore against the current `BakeMesh` output) instead of via
  their attached collider. The old `BoxCollider` / `Physics.Raycast` path
  was always coarse — clicks inside the AABB but outside the visible
  silhouette were captured, which surfaced once the cube was swapped for
  a tiger with lots of transparent gaps (between legs, around the hat
  tip). Each `LateUpdate` (after the Animator step — moved from `Update`
  to fix head-drift during animation), the plugin calls
  `smr.BakeMesh(entry.mesh)` and caches `verts[]` + `tris[]`. The
  cyclopean ray walks every triangle, transforming each vertex via
  `Matrix4x4.TRS(smr.position, smr.rotation, Vector3.one)` —
  position + rotation only, NO scale (BakeMesh already applies the rig's
  scale chain). Forces `SkinnedMeshRenderer.updateWhenOffscreen = true`
  and `Animator.cullingMode = AlwaysAnimate` on first bake. Active-rig
  gate via `DisplayXRRigManager.ActiveCamera` so two rigs don't disagree
  on silhouette-edge pixels. 8-frame hysteresis smooths sub-pixel jitter
  on the silhouette edge. Non-SMR clickables (e.g. the cube) keep the
  existing `Physics.Raycast` path — no regression.
- **Win32 stuck-drag fix for forwarded button events** —
  `s_vkey_state` is now updated at the top of `overlay_wnd_proc` for
  EVERY button event (`WM_*BUTTONDOWN/UP/DBLCLK`), regardless of whether
  the event is captured by the overlay or forwarded to the underlying
  Unity HWND via `forward_click_to_underlying_window`. Previously,
  `s_vkey_state` was only updated by Unity's HWND subclass — so when a
  click on the silhouette dragged across the edge and released over a
  transparent area, the `WM_LBUTTONUP` was forwarded and Unity's subclass
  never saw it, leaving C#'s polled left-button state stuck at "pressed"
  forever (sample `DragRotateCube` kept rotating with cursor motion).

### Changed
- `CLAUDE.md` drops the now-shipped tiger-session "Unreleased changes"
  appendix.

### Known limitations
- Transparent overlay (`#57`) and window-space UI (`#65`) do not compose
  yet — see issue `#82`. Apps need to pick one or the other for now.
- SR weaver phase-snap still requires the SDK to own the modal drag loop;
  the transparent overlay's capture-based drag stutters in 3D during
  motion (tracked in `DisplayXR/displayxr-runtime#193`). No change since
  v1.3.0.

## [1.3.0] - 2026-05-09

### Changed
- Plugin now relies on the displayxr-runtime `chromaKeyColor = 0` -> default
  magenta convention shipped in runtime PR #213 / #3a / #3b / #3c. Apps that
  call `RequestChromaKey(Color.magenta)` see no behavior change. Apps that
  pass `0` (or never call the API) now get the runtime DP's default magenta
  instead of "no chroma-key conversion" — equivalent to the previous behavior
  on D3D11/D3D12 where the runtime already used magenta.
- Transparent backgrounds now also work on Vulkan and OpenGL Win32 standalone
  builds, not just D3D11/D3D12. Same `RequestTransparentSession()` API. The
  runtime's GL native compositor falls back to opaque presentation on GPUs
  without `WGL_NV_DX_interop2` (mainly Intel iGPUs) — the cube still renders
  but the desktop doesn't show through.

### Compatibility
- Requires displayxr-runtime ≥ v25.7.0 for Vulkan / OpenGL transparency.
  D3D11/D3D12 transparency keeps working with older runtimes.
- `chromaKeyColor` semantics are unchanged; the only difference is that
  passing `0` is now a useful (and recommended) value, not a no-op.

### Known limitations (no change since v1.2.x)
- Anti-aliased edges become hard-mask alpha on Leia hardware (alpha=0 or 1,
  no in-between). This is fundamental to the chroma-key trick used by the
  SR weaver — fully transparent regions punch through cleanly, but partial-
  transparency pixels on antialiased edges either snap to opaque (with
  possible fringing toward the chroma key) or to fully transparent. Apps
  that need soft alpha should choose a content-safe `chromaKeyColor` to
  minimize fringing.

## [1.2.13] - 2026-05-07

### Added
- Window-Space UI: app-side input routing primitives. `DisplayXRPreviewInput`
  exposes preview-window mouse position / button state / cursor-key polling
  via OS-level reads, so wsui input routers work while the standalone
  preview NSWindow has keyboard/mouse focus (Unity's Input System only
  fires when its own windows are focused).
- `DisplayXRWindowSpaceUI.IsCursorOverInteractive` — static gate that
  scene input controllers (`DisplayXRInputController`) consult to pause
  cube/camera rotation while the user is driving wsui controls.
- Native: `displayxr_standalone_get_preview_window_size` (cross-platform),
  `displayxr_standalone_get_rendering_mode_name(slot)` for runtime-supplied
  mode-name strings, `displayxr_standalone_is_key_pressed` for app-side
  hotkey polling.

### Fixed
- `enumerate_rendering_modes` no longer treats a NULL `mode_names` buffer
  as a count-only query — was the silent root cause of `m_ModeIndices`
  arriving as all-zeros for callers passing `IntPtr.Zero` for the names
  buffer.
- Wsui `Canvas.worldCamera` is now wired to the OverlayCamera so
  `GraphicRaycaster.Raycast` against the layer actually returns hits.
- Wsui content stays aspect-correct under window resize via per-frame
  `OverlayCamera.aspect` + canvas RectTransform updates — no RT
  recreation needed.
- Mac: preview-window click state via `[NSEvent pressedMouseButtons]`
  (wsui sliders/buttons couldn't detect presses before).
- Mac: `noResponderFor:` override on the preview NSWindow silences the
  system beep on hotkey poll-only key events.

### Changed
- Plain `Tab` (camera cycle) is now gated on `!Shift` so apps can bind
  Shift+Tab to their own actions without the rig manager firing.

## [1.2.12] - 2026-05-07

### Fixed
- v1.2.11 attempted to fix the nested-package upm bug but the same fix
  also deleted the .tgz before the GitHub Release step could attach it,
  leaving v1.2.11's release without a downloadable asset. v1.2.12
  reverts to a cleaner workflow that doesn't use `git add -A` at all in
  the upm-publish step (selective `git add -f` for binaries +
  `git rm --cached` for dev-file removals already covers everything).

### Notes
- No source-side changes from v1.2.10 or v1.2.11. Packaging-only release.
- Pin to `#upm/v1.2.12` going forward — both v1.2.9 and v1.2.10 have the
  nested-package bug; v1.2.11 has a clean upm tag but missing GH Release
  asset; v1.2.12 is the first fully-clean release.

## [1.2.11] - 2026-05-07

### Fixed
- CI: stop shipping a duplicate copy of the package nested inside the upm
  branch under `com.displayxr.unity-X.Y.Z/`. The "Create UPM tarball"
  step mkdir'd a staging directory in the working tree; the next step's
  `git add -A` swept it into the upm tag. Consumers saw the package
  imported twice and Unity raised hundreds of "Asset has no meta file,
  in immutable folder" errors on first install (forcing safe-mode editor
  load). v1.2.9 and v1.2.10 upm tags are affected — re-pin to v1.2.11.

### Notes
- No source-side changes from v1.2.10. This is a packaging hotfix.

## [1.2.10] - 2026-05-07

### Added
- New native API `displayxr_standalone_get_preview_mouse_position(out fx, out fy)`
  exposing the runtime preview window's cursor position as fractional
  (0..1, top-left) content-area coords. Mac (`NSWindow`/`NSEvent.mouseLocation`)
  and Windows (`WM_MOUSEMOVE` tracked in `sa_wndproc`) covered. Public C#
  helper: `DisplayXR.DisplayXRPreviewInput.TryGetPreviewMousePosition()`.
- This is the *primitive* an app-side input router needs to make
  `DisplayXRWindowSpaceUI` interactive. The plugin doesn't ship a router
  — different consumer apps want different input models (mouse, hand-
  tracking, touch). See the sample
  [`DisplayXRWsuiMouseRouter.cs`](https://github.com/DisplayXR/displayxr-unity-samples/blob/main/samples/urp-singlepass-ui/Assets/Scripts/DisplayXRWsuiMouseRouter.cs)
  in `displayxr-unity-test-2d-ui` for the canonical mouse → fractional →
  canvas-local → `EventSystem.RaycastAll` flow.

## [1.2.9] - 2026-05-07

### Fixed
- Stop shipping dev-only files in the published UPM package. Unity Package
  Manager was extracting CLAUDE.md, CONTRIBUTING.md, .claude/, .github/,
  launch-*.sh, .gitignore + .gitattributes from the upm branch — Unity
  treats unrecognized .md files at the package root as importable assets
  and warns once per file that the .meta is missing (UPM also strips
  *.md.meta files at extraction). Each install logged 4+ "Asset has no
  meta file, in immutable folder" warnings. The CI now strips these
  files in the upm-branch publish step. Files retained on main; only
  excluded from the published package.

## [1.2.8] - 2026-05-07

### Fixed
- DisplayXRWindowSpaceUI now works under URP. The previous design (Canvas in
  ScreenSpaceCamera mode + dedicated camera with a depth-less RenderTexture)
  was silently failing under URP's RenderGraph: empty RT, transparent layer,
  no UI shown. Rewrote the component to use a private WorldSpace canvas + a
  dedicated overlay camera with the camera's "up" vector inverted to handle
  the bottom-left ↔ top-left RT origin convention. The RT is created with
  explicit GraphicsFormat color + D24_UNorm_S8_UInt depth-stencil to satisfy
  RenderGraph's render-target requirements. Camera is auto-render-disabled
  and manually Render()-ed each LateUpdate. [ExecuteAlways] so this works in
  edit-mode preview as well as Play Mode. (#78)
- Canvas state (renderMode, transform, layer) is now saved + restored in
  OnDisable so the host app's Canvas is left as we found it.

### Known limitations
- WorldSpace-canvas approach means UI elements aren't directly clickable —
  Unity's GraphicRaycaster expects screen-space mouse coordinates against a
  canvas in screen-space or a worldCamera-projected canvas. wsui-rendered
  UI is read-only for now. An input router (mouse → window-fractional →
  canvas-local → synthetic events) is tracked as a v1.2.9+ follow-up.

## [1.2.7] - 2026-05-06

### Fixed
- Export `displayxr_window_space_ui_set_texture/_set_layer/_clear` with `DISPLAYXR_EXPORT` so Unity's P/Invoke can find them. v1.2.6 was missing these symbols (visibility=hidden on Mac, no `__declspec(dllexport)` on Windows), causing `EntryPointNotFoundException` on first DisplayXRWindowSpaceUI.LateUpdate call. (#67)

## [1.2.6] - 2026-05-06

### Added
- Submit `DisplayXRWindowSpaceUI` as `XrCompositionLayerWindowSpaceDXR` so
  2D UI canvases composite as a stereo overlay layer with proper disparity
  on the DisplayXR runtime. (#67)

## [1.2.5] - 2026-05-06

### Added
- macOS standalone builds now auto-bundle the OpenXR loader (`openxr_loader.dylib`)
  into `<App>.app/Contents/PlugIns/`. Unity's own `OpenXRBuildProcessor` only
  handles Windows + Android; without this, every macOS build failed at session
  init with "Failed to load openxr runtime loader". Loader ships at
  `RuntimeLoaders~/macos/` (ignored by Unity's asset pipeline) and is copied
  by `DisplayXRBuildProcessor.OnPostprocessBuild`. (#71)
- `URPBasicScene` sample now ships an editor-only build hook that registers
  `Universal Render Pipeline/Lit` in **Project Settings > Graphics > Always
  Included Shaders** before any standalone build, so the shader isn't dropped
  by Unity's stripper. Also exposed as **Tools > DisplayXR > Register URP/Lit
  in Always Included Shaders**. (#72)

### Documentation
- README "macOS Deployment" section now covers the unsigned-`.app` symlink
  issue (`XR_ERROR_RUNTIME_UNAVAILABLE` despite a working
  `~/Library/Application Support/openxr/1/active_runtime.json` symlink) and
  the two workarounds: explicit `XR_RUNTIME_JSON`, or ad-hoc `codesign
  --deep --force --sign - MyApp.app`. Also covers Developer ID + notarization
  for distribution. New troubleshooting row mirrors this. (#72)

## [1.2.4] - 2026-05-06

### Added
- URP and HDRP support for the stereo rig camera callbacks. `DisplayXRDisplay`
  and `DisplayXRCamera` now route through `RenderPipelineManager.beginCameraRendering`
  when a Scriptable Render Pipeline is active, and continue to use
  `Camera.onPreRender` on the Built-in Render Pipeline. Adds a `URPBasicScene`
  sample mirroring `BasicScene`. (#68)

### Fixed
- macOS native bundle (`Runtime/Plugins/macOS/displayxr_unity.bundle`) was
  missing the `displayxr_set_use_srgb_swapchain` export — a regression that
  caused `EntryPointNotFoundException` on macOS standalone builds. Rebuilt
  from current `native~/` source. (#68)

## [1.2.3] - 2026-05-05

### Fixed
- Unity 6 compile error in `DiscoverCameras`: pass explicit `FindObjectsSortMode.None` so the call binds to the two-arg overload that exists on both Unity 2022.3 and Unity 6 (#3).

## [1.2.2] - 2026-05-05

### Added
- `Samples~/MinimalTransparent/` — minimal teaching sample for the chroma-key transparent overlay technique. ~70-line bootstrap script + long-form README that dissects the four-mechanism pipeline (camera clear, OpenXR extension fields, runtime post-weave shader, OS LWA_COLORKEY) and the layer-ownership map. Companion to the polished `TransparentAvatar` sample.
- `DisplayXRTransparentOverlay.ConsumeWheelDelta()` — public method on the component returning the accumulated mouse-wheel delta (Win32 raw units, 120 per notch). Apps poll this and decide what to do with the wheel.
- Native export `displayxr_consume_overlay_wheel_delta()` — atomic read + zero of the overlay's accumulated wheel delta (`InterlockedExchange` on a `volatile LONG`). Declared in `displayxr_hooks.h` and `displayxr_win32.h`.

### Removed
- The experimental WM_MOUSEWHEEL → resize-overlay-HWND behavior from v1.2.0. Plugin no longer self-resizes the overlay when the user scrolls; apps now drive what the wheel does (e.g. `DisplayXRDisplay.virtualDisplayHeight` for zoom-in-window). The plugin still consumes the wheel message when its overlay is foreground so it doesn't bubble to underlying apps.

## [1.2.1] - 2026-05-04

### Added
- `DisplayXRTransparentOverlay.chromaKeyColor` is now a settable property — assigning at runtime re-pushes camera clear + native overlay state. New `ApplyChromaKey()` private helper + `OnValidate` for live Inspector edits during Play.

### Fixed
- 3D stutter during right-drag of the transparent overlay and during the standalone preview's SC_MOVE intercept — synchronous WM_ENTERSIZEMOVE/EXITSIZEMOVE bracketing now drives the SR SDK weaver's phase-snap state machine without needing a runtime-side API change (#61).

### Changed
- `Samples~/TransparentAvatar` default chroma key switched from magenta (1,0,1) to near-mid-gray (128,127,129) so silhouette-edge halos blend invisibly into typical desktop/photo backgrounds. README updated with the rationale and the new palette-clamp trade-off.
- CI is now PR-driven: `build-native.yml` fires on push-to-main, all PRs (drafts included), v* tags, workflow_dispatch; concurrency cancels in-progress runs on rapid pushes. The `/ci-monitor` skill is retired in favor of opening PRs and letting CI report on the head ref.

## [1.2.0] - 2026-05-04

### Added
- **Cross-process click-through finally works for the transparent avatar** (#57). Click anywhere through the avatar's transparent halo and the click reaches the actual deepest control under the cursor — verified end-to-end with Notepad (`RichEditD2DPT`) and Explorer (`DirectUIHWND`). Activation transfers cleanly: the underlying app gains foreground / focus, caret appears, keystrokes land. Implementation: the overlay catches every click via `WM_NCHITTEST`/HTCLIENT (Approach C), `forward_click_to_underlying_window` does iterate-top-level + `ChildWindowFromPointEx`-recursive-descend to find the deepest non-transparent leaf, then `SetForegroundWindow` on the top-level frame and `PostMessage` to the leaf with the leaf's client coordinates.
- **Mouse-wheel scroll resizes the overlay window when in focus.** Uniform scaling around the current center, 10% per WHEEL_DELTA notch, floor at 400×400. Win32's "wheel goes to focused window" routing means scroll naturally goes to whichever app you've click-through'd to (Notepad scrolls its document, etc.) — no explicit foreground gate needed.
- **Foreground-aware input gating** for `DisplayXRInputController`. Exposed `displayxr_is_our_process_foreground()` (calls real OS `GetForegroundWindow` from the plugin DLL whose IAT isn't patched). `Update()` early-returns when not foreground so WASD doesn't move the cube while the user is typing in Notepad. Cube reclaims foreground via `SetForegroundWindow(overlay)` from the wndproc on cube-press. Custom input scripts should call `DisplayXRNative.displayxr_is_our_process_foreground()` for the same gate.
- `displayxr_get_overlay_size()` getter so C# raycast / hit-rect math uses the overlay's actual client size (which scroll-resize can change) rather than `Screen.width/height` (Unity's frozen off-screen HWND).
- Diagnostic instrumentation kept in main: `WH_MOUSE_LL` global mouse hook (button-only) logs `WindowFromPoint` resolution per click to `displayxr.log`, and `overlay_wnd_proc` logs every button-event entry with the live `WS_EX_TRANSPARENT` bit. Cheap, lifetime-of-process; was indispensable for diagnosing the foreground-transfer bug.

### Fixed
- Cyclopean Kooima raycast now uses the overlay's actual client size (via `displayxr_get_overlay_size`) for cursor → NDC conversion. Previously `Screen.width`/`Screen.height` returned Unity's off-screen HWND dimensions, which don't track scroll-resize — every click inside a resized overlay registered as `hit_active=0` and "passed through" the cube.

## [1.1.1] - 2026-04-30

### Fixed
- **Package import warnings**: Renamed `docs/` to `docs~/` so Unity's package importer skips the folder entirely (UPM convention for "ignored" folders). Previously, every markdown file under `docs/` produced a `"has no meta file, but it's in an immutable folder"` warning when the package was installed via Package Manager. Drops the orphan `docs.meta` and `docs/quick-start-guide.md.meta` that a previous editor session created. Internal cross-references in the architecture / ADR / roadmap docs updated to the new path.

## [1.1.0] - 2026-04-30

### Added
- **Atlas screenshot capture** — press `I` (or call `DisplayXRScreenshot.Capture()`) to save the multi-view atlas the app wrote to the swapchain as a PNG to `Pictures/DisplayXR/<app>-N_NxM.png`. Mirrors the C++ test app and Unreal plugin convention. Brief white flash on capture for visual feedback. Two paths: editor SA preview reads the existing atlas RT before submit; built standalone re-renders the active rig camera's L/R Kooima views via a hidden capture camera, with a CommandBuffer-driven flash on every registered rig camera so it lands in the OpenXR swapchain.
- **`.displayxr.json` app manifest sidecar** generated next to the built executable on build (#51). Optional `Register with DisplayXR` mode (#54) also writes to `%LOCALAPPDATA%\DisplayXR\apps\` so the DisplayXR Shell discovers the build without it living under Program Files.
- **`Window > DisplayXR > Manifest Settings`** menu shortcut.
- **`Hidden/DisplayXRFlash` shader** (Runtime/Resources/) used by the on-demand flash overlay.
- Pin built Unity Player to the dGPU on hybrid laptops via `NvOptimusEnablement` / `AmdPowerXpressRequestHighPerformance` exports.

### Fixed
- **Gamma color space double-darkening** in built apps on D3D11 and D3D12. The `xrCreateSwapchain` hook now downgrades sRGB color formats (29 → 28, 91 → 87) for Unity Gamma projects so already-gamma-encoded shader output lands without re-encoding. Linear projects keep sRGB. C# tells native via a new `displayxr_set_use_srgb_swapchain` setter at `OnInstanceCreate`.
- `KeyCode.I` was missing from the new Input System mapping in `DisplayXRInputController`, throwing `ArgumentOutOfRangeException` and aborting Update before any later handlers ran.
- Asmdef `.meta` importer type set to `AssemblyDefinitionImporter` (was `DefaultImporter`).
- Built-app capture PNG was Y-flipped because the SA path's projection-Y flip isn't applied in the on-demand path; the Y-flip blit is now opt-in per path.

### Changed
- **UGUI is now an optional dependency** of the runtime assembly — no hard compile-time UI module requirement.
- `FindObjectsByType` call updated to drop the deprecated `FindObjectsSortMode` argument (Unity 2023+ deprecation).

## [1.0.0] - 2026-04-11

First stable release of the DisplayXR Unity plugin. Headline changes: standalone
editor preview with native HWND + input forwarding, D3D11 hooked path
generalized to N-view tile atlas, GitHub org transfer to DisplayXR, and full
documentation structure with ADRs.

### Added
- **Standalone editor preview window** — native HWND on Windows with D3D11 atlas
  bridge, D3D12 blit, and input suppression (replaces the earlier IOSurface
  approach). Works in both Play Mode and Edit Mode.
- **Edit Mode preview** — live composited 3D output without entering Play Mode
- **Game View eye tile atlas** displayed during Play Mode for debugging
- **D3D11 hooked path**: generalized SBS composite to N-view tile atlas (#91)
- **D3D11 typed swapchain substitution** for the hooked path (#91)
- **Input forwarding** from preview window to Unity — mouse events, focus-aware
  handling, camera rotation support
- **Documentation structure**: ADRs, architecture docs, navigation
- **`/release` skill** for tagged release orchestration
- **Shell mode**: full input forwarding from main HWND (#43, #44, #45)

### Changed
- Repo references updated from `dfattal/openxr-3d-display` to
  `DisplayXR/displayxr-runtime` following GitHub org transfer
- CI triggers restricted to PR validation and tag pushes — no more triggers on
  main branch (devs use local builds for daily iteration)
- Shell mode: Kooima viewport updates on window resize/move (#46)
- Game View camera rendering suppressed during editor Play Mode (preview
  window takes over)

### Fixed
- macOS build: gate `xrDestroySwapchain` dispatch on `_WIN32` (#91)
- Preview window: Play Mode startup, input handling, weaving, Y-flip
- Play Mode auto-start and camera selection reliability
- Camera selection after Play Mode domain reload
- Camera rotation during preview window move/resize/drag (multiple fixes)
- Closing preview window now correctly exits Play Mode
- Preview tab: crop atlas to content region and fix Y-flip
- Crash on second Play Mode entry: execute deferred session/instance destroy
- Crash on Play Mode exit: defer preview window destruction, destroy preview
  before XR teardown
- `EntryPointNotFoundException` for `window_was_closed` handled gracefully
- Preview window content frozen during live resize

## [0.7.0] - 2026-03-31

### Fixed
- Fix window-relative Kooima projection not responding to window drag (#41)
  - Added `WM_MOVE` handler so viewport position updates when the window is
    dragged, not just on resize (`WM_SIZE`)
  - Set initial viewport position on overlay creation so projection is correct
    from frame 1
  - Re-added viewport-change diagnostic logging for verification

## [0.6.5] - 2026-03-30

### Fixed
- Restore child window overlay for built apps (dfattal/openxr-3d-display#107)
  - Reverts top-level HWND pass-through which caused D3D12 swapchain conflict
    (`E_ACCESSDENIED`) because Unity already owns the swapchain on that window
  - Child window gives the runtime its own HWND for presentation

### Added
- Local Windows MSVC build script `native~/build-win.bat` (#42)

## [0.6.3] - 2026-03-27

### Changed
- Remove Kooima diagnostic logs — window-relative projection verified (#41)
  - All diagnostic logging for Kooima projection parameters has been removed
    now that window-relative projection is confirmed working correctly

## [0.6.2] - 2026-03-27

### Changed
- Log Kooima params only on viewport resize instead of every 60 frames (#41)
  - Reduces log noise by triggering diagnostic output only when the viewport
    dimensions actually change

## [0.6.1] - 2026-03-27

### Added
- Throttled Kooima diagnostic logs for window-relative projection (#41)
  - Native hooks and standalone code now log key projection parameters
    at reduced frequency for easier debugging without log spam

## [0.6.0] - 2026-03-27

### Changed
- Bump version to 0.6.0 — milestone cleanup for Game View overlay and
  window-relative Kooima projection (#41)

### Fixed
- Move diagnostic label back to top-left of Game View overlay (#41)

## [0.5.9] - 2026-03-27

### Changed
- Window-relative Kooima projection: replace viewport-scale factor with actual
  window physical dimensions and window-center eye offset (ADR-012) (#41)
- Native WM_SIZE handler now captures HWND screen position via ClientToScreen
  for correct off-center window perspective on Windows

## [0.5.8] - 2026-03-27

### Fixed
- Center diagnostic text in Game View for visibility at all DPI (#41)

## [0.5.7] - 2026-03-27

### Fixed
- Revert UV to canvas/surface crop — weaver respects viewport (#41)
  - UV=1.0 test confirmed: weaver writes to dp_target viewport (2203x1147),
    NOT the full 3840x2160 surface. Content is at bottom-left in UV space.
  - Restoring canvas/surface UV crop which samples that exact region.

## [0.5.6] - 2026-03-27

### Changed
- Test: use full UV range (1.0) for shared texture sampling (#41)
  - If the weaver renders to the full 3840x2160 shared texture (ignoring viewport),
    the previous UV crop clipped to only the canvas portion, showing a zoomed/cropped view
  - Testing with UV=1.0 to confirm whether full-texture sampling resolves this

## [0.5.5] - 2026-03-27

### Fixed
- Revert canvas to physical pixels — weaver needs physical px precision (#41)
  - Screen.width/height gives logical pixels; multiplying by backingScale gives physical
  - The weaver must output at physical resolution for correct lenticular interlacing

## [0.5.4] - 2026-03-27

### Fixed
- Use Screen.width/height directly for canvas size in Play Mode (#41)
  - Unity Game View has pixelsPerPoint=1.0, so the backbuffer is at logical resolution
  - Multiplying by backingScale (2.5) sent oversized dimensions to the weaver
  - Now sends Screen.width/height directly, matching actual Game View size

## [0.5.3] - 2026-03-27

### Added
- Comprehensive texture size diagnostics for weaving debug (#41)
  - Native: log GetDpiForSystem, atlas size, display size in set_canvas_rect
  - C#: show canvas, shared tex, UV, Screen, backingScale, pixelsPerPoint, drawRect logical+physical, mode, camera in Game View overlay

## [0.5.2] - 2026-03-26

### Fixed
- Skip `GL.invertCulling` on Windows D3D12 (#41)
  - Without the projection Y-flip on D3D12, only the view Z-flip affects winding
  - Normal culling (no inversion) is correct on Windows, fixing inside-out faces

## [0.5.1] - 2026-03-26

### Fixed
- Make projection Y-flip macOS-only — D3D12 native memory doesn't need it (#41)
  - The Y-flip was for Metal RenderTexture convention; on D3D12, removing it produces right-side-up content matching the reference test app
  - The weaver now receives correctly oriented atlas content on Windows

## [0.5.0] - 2026-03-25

### Fixed
- Set `FilterMode.Point` on shared texture to preserve interlacing (#41)
  - Bilinear filtering interpolates between rows, destroying the per-row interlacing pattern from the Leia SR weaver
  - Point filtering preserves exact pixel values for correct lenticular 3D output

## [0.4.9] - 2026-03-25

### Fixed
- Set Per-Monitor DPI Awareness V2 before runtime init for correct weaving (#41)
  - `SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)` ensures `GetClientRect` returns physical pixels
  - Fixes Leia SR weaver interlacing mismatch on DPI-scaled Windows displays

## [0.4.8] - 2026-03-25

### Fixed
- Revert display Y-flip (separate issue), add canvas/texture size diagnostics (#41)
  - Native `set_canvas_rect` now logs exact pixel values sent to runtime
  - Game View overlay shows canvas/surface/UV/screen/scale/draw sizes for debugging

## [0.4.7] - 2026-03-25

### Fixed
- Revert native D3D12 blit to simple copy, flip at display via UV coords (#41)
  - Atlas content is Y-flipped (Unity D3D12 convention), weaver interlaces it Y-flipped, and the display flips it back via Rect(0, vMax, uMax, -vMax)
  - All three flips are consistent — no native row-by-row flip needed

## [0.4.6] - 2026-03-25

### Fixed
- D3D12 atlas blit: row-by-row Y-flip copy for correct weaver orientation (#41)
  - Unity RenderTextures on D3D12 store content Y-flipped in native memory
  - Copy each row to the reversed Y position in the swapchain image so the weaver receives correctly oriented content for lenticular interlacing

## [0.4.5] - 2026-03-25

### Fixed
- Revert to Graphics.CopyTexture — Graphics.Blit Y-flip broke rendering (#41)
  - Reverts to the working v0.4.2 atlas copy path
  - Y-flip will be handled separately (native D3D12 blit or projection matrix)

## [0.4.4] - 2026-03-25

### Fixed
- Simplify Y-flip blit: remove GL matrix manipulation that may have broken D3D12 (#41)

## [0.4.3] - 2026-03-25

### Fixed
- Fix D3D12 Y-flip: blit atlas with vertical flip before copying to bridge texture (#41)
  - Unity RenderTextures on D3D12 are Y-flipped in native memory
  - Use `Graphics.Blit` with `scale(1,-1)` to flip before bridge copy

## [0.4.2] - 2026-03-25

### Fixed
- Revert canvas rect to physical pixels — runtime uses them as GPU viewport dims (#41)

## [0.4.1] - 2026-03-25

### Fixed
- Fix canvas rect DPI: send logical pixels on Windows, backing pixels on macOS (#41)
  - `xrSetSharedTextureOutputRectDXR` takes HWND client-area pixels per spec
  - On DPI-aware Windows (Unity 6), `Screen.width` is already logical pixels
  - On macOS, `Screen.width` is in points — multiply by backing scale factor

## [0.4.0] - 2026-03-25

### Fixed
- Fix shared texture format: use R8G8B8A8_UNORM / RGBA32 to match runtime weaver PSO format (#38)
  - Runtime hardcodes DXGI_FORMAT_R8G8B8A8_UNORM (28) for the weaver; our shared texture was B8G8R8A8_UNORM (87)
  - Format mismatch caused weaver to silently no-op
  - Updated native standalone, preview session, preview window, and game view overlay

## [0.3.9] - 2026-03-25

### Changed
- Pass Unity's HWND to Win32 window binding for standalone preview — required by Leia weaver for correct window targeting (#38)

## [0.3.8] - 2026-03-25

### Fixed
- Fix Windows DPI scaling for canvas rect: `get_backing_scale_factor` now returns system DPI / 96 instead of hardcoded 1.0 (#38)

## [0.3.7] - 2026-03-25

### Fixed
- Fix atlas RT format: use BGRA32 to match bridge texture format for `Graphics.CopyTexture` compatibility (#38)

## [0.3.6] - 2026-03-25

### Fixed
- Fix preview window not opening from menu (#40): null-guard custom editors to prevent `SerializedObjectNotCreatableException` during domain reload
- D3D11 TYPELESS swapchain textures: replace proxy texture copy with thin COM wrapper that overrides `GetDesc()` to report concrete format — zero-copy, no extra textures (#36)

## [0.3.5] - 2026-03-25

### Changed
- Cross-device atlas blit via DXGI shared bridge texture (#38)
  - Unity renders atlas on its D3D12 device, then `Graphics.CopyTexture` to a bridge texture shared on both devices
  - `CopyTextureRegion` from bridge to swapchain on the runtime's device
  - Completes the cross-device rendering pipeline started in 0.3.4

## [0.3.4] - 2026-03-25

### Changed
- D3D12: use separate device for runtime session, shared texture via DXGI handle for Unity (#35)
  - Sharing Unity's D3D12 device with the runtime caused device removal
  - Create dedicated D3D12 device for the runtime OpenXR session
  - Use `OpenSharedHandle` on Unity's device for `CreateExternalTexture`
  - Atlas blit skipped (cross-device TODO)

## [0.3.3] - 2026-03-25

### Fixed
- D3D12 atlas blit: remove explicit resource barriers, rely on implicit COMMON state promotion for cross-queue copy operations (#35)

## [0.3.2] - 2026-03-25

### Fixed
- D3D12 OpenXR struct type IDs: corrected from `1000027xxx` to `1000028xxx` (`XR_TYPE_GRAPHICS_BINDING_D3D12_KHR`, `XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR`, `XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR`) (#35)

## [0.3.1] - 2026-03-25

### Fixed
- Crash in `set_unity_device` on Windows D3D12: validate `ID3D12Resource` via `QueryInterface` before calling `GetDevice`, preventing access violation when Unity passes a non-D3D12 resource (#35)

## [0.3.0] - 2026-03-25

### Changed
- Migrate Windows standalone preview from D3D11 to D3D12 (#35)
  - Replace D3D11 device/context with D3D12 device/queue/command list/fence
  - D3D12 shared texture via `CreateCommittedResource` + `D3D12_HEAP_FLAG_SHARED`
  - Atlas blit with D3D12 command list, resource barriers, and fence sync
  - `XrGraphicsBindingD3D12KHR` for session creation
  - Platform-conditional Y-flip (Metal vs D3D12)
  - Supports both D3D11 and D3D12 Unity graphics backends

## [0.2.2] - 2026-03-24

### Fixed
- CS0104 ambiguous `Object` reference in `DisplayXRPreviewSession.cs` — qualify as `UnityEngine.Object` to resolve conflict with `System.Object` (#35)

## [0.2.1] - 2026-03-24

### Fixed
- Null texture in `set_unity_device`: force `RenderTexture.Create()` before `GetNativeTexturePtr()` to ensure GPU resource is allocated (#35)

## [0.2.0] - 2026-03-25

### Fixed
- Windows standalone preview: use Unity's own D3D11 device instead of creating a separate one, fixing cross-device TDR crashes when sharing textures between devices (#35)

## [0.1.9] - 2026-03-25

### Fixed
- Revert shared texture to B8G8R8A8_UNORM — runtime rejects TYPELESS format (`xrCreateSession` fails with -6). The C# `linear=true` flag is sufficient for correct gamma handling.

## [0.1.8] - 2026-03-24

### Fixed
- D3D11 shared texture compatibility: use TYPELESS format with linear SRV to avoid gamma/format mismatch issues in the standalone preview rendering pipeline

## [0.1.7] - 2026-03-24

### Added
- Windows standalone preview: D3D11 swapchain image acquisition, atlas blit from shared texture, and `xrEndFrame` submission — completes the Windows standalone preview rendering pipeline

## [0.1.6] - 2026-03-24

### Fixed
- Windows crash: `displayxr_standalone_get_shared_texture` now returns `ID3D11Texture2D*` (what Unity's `CreateExternalTexture` expects) instead of the DXGI shared `HANDLE` (which is for cross-device sharing with the runtime)

## [0.1.5] - 2026-03-24

### Added
- Windows standalone preview: D3D11 shared texture creation and DXGI handle passing to runtime via Win32 window binding — enables zero-copy GPU texture sharing for preview output

## [0.1.4] - 2026-03-23

### Fixed
- Windows standalone preview: create D3D11 device with correct adapter LUID and pass `XrGraphicsBindingD3D11KHR` to session creation (fixes `xrCreateSession` error -38)

## [0.1.3] - 2026-03-23

### Added
- Windows standalone preview: implement `LoadLibrary`/`GetProcAddress` runtime loading and Win32 window binding for session creation — standalone preview now starts on Windows

## [0.1.2] - 2026-03-23

### Fixed
- Windows DLL plugin settings: enable Editor platform so standalone preview and Play Mode can load `displayxr_unity.dll` in the Windows editor

## [0.1.1] - 2026-03-23

### Fixed
- Standalone preview now discovers the runtime via Windows registry (`Khronos\OpenXR\1\ActiveRuntime`) when `XR_RUNTIME_JSON` is not set
- Settings page shows runtime discovery source (env var vs registry)
- UPM git URL install: added `.gitattributes` to prevent binary corruption, documented Git prerequisites for Windows and macOS
- Quick-start guide updated with git URL as primary install method

## [0.1.0] - 2026-03-23

### Added
- Initial release as standalone UPM package (moved from `openxr-3d-display` runtime repo)
- OpenXR Feature lifecycle (`DisplayXRFeature`) with native hook chain
- Camera-centric stereo rig (`DisplayXRCamera`) for retrofitting existing scenes
- Display-centric stereo rig (`DisplayXRDisplay`) for virtual display placement
- Kooima asymmetric frustum projection via native plugin (display3d + camera3d libraries)
- Eye tracking integration through OpenXR extensions
- Stereo tunables: IPD factor, parallax factor, perspective factor, inverse convergence distance
- 2D UI overlay component (`DisplayXRWindowSpaceUI`) for HUDs and menus
- Standalone editor preview window with camera selector, rendering mode controls, and zero-copy GPU texture sharing (IOSurface/DXGI)
- Game View overlay (`DisplayXRGameViewOverlay`) for Play Mode shared texture output
- Canvas-aware shared texture cropping via `xrSetSharedTextureOutputRectDXR`
- Custom inspectors for camera-centric and display-centric modes
- Project Settings page showing runtime status and display info
- Native plugin source (`native~/`) with independent CMake build
- CI workflow for Windows (MSVC) and macOS (Universal) native builds
- Cross-platform support: Windows x64 and macOS
- Cross-compilation support: build Windows target from macOS editor
- `.gitattributes` for binary file protection
- Quick start guide and comprehensive README with troubleshooting
