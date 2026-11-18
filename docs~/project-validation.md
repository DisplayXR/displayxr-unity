# Project Validation

**DisplayXR > Project Validation** checks a Unity project against what the plug-in needs
for the active build target, and offers a fix where a safe one exists. The same summary is
on the DisplayXR page of Project Settings, with a button that opens the window.

- [What it does and does not do](#what-it-does-and-does-not-do)
- [The window](#the-window)
- [At build time](#at-build-time)
- [The rules](#the-rules)
- [Considered and left out](#considered-and-left-out)
- [Adding a rule](#adding-a-rule)
- [Code map and tests](#code-map-and-tests)

## What it does and does not do

- It **never changes a setting by itself.** A fix runs when you press its button. **Fix All
  Errors** lists every change it is about to make and asks first.
- It only judges build targets that **use** DisplayXR. For a target where the DisplayXR
  Display loader is not enabled, `loader-enabled` is the only finding, and it is a warning or
  a note, never an error. A project that uses DisplayXR on Windows only is not told to change
  its Android settings.
- It **never opens by itself.** There is no first-run pop-up and nothing runs on project load.
- Every fix writes an ordinary Unity setting (XR Plug-in Management, Player Settings, a URP
  renderer asset, the DisplayXR Manifest Settings asset). You can see it and change it back
  on its usual page.
- Fixes are disabled in Play Mode: Unity does not allow changes to the XR loader list while
  playing.
- Checks that look at scenes look at the scenes that are **open**. A component in a scene
  that is not loaded is not seen. This can miss an issue; it cannot invent one.

## The window

Each rule is one row: a status (Error, Warning, or a note), what was found, why the rule
exists, and up to three buttons.

| Button | What it does |
|---|---|
| **Fix** | Applies the rule's fix. Shown only when the rule has one for this finding. Some rules have a button with another label (**Get the runtime**, **Open Player Settings**): those only open a page and change nothing. |
| **Ignore** | Stops reporting the rule for this project. Ignored rules move to their own group and can be un-ignored there. |
| **Docs** | Opens the rule's section on this page. |

**Fix All Errors** runs the fixes of the open **Errors**, after a confirmation that lists
exactly those changes. It leaves out:

- warnings and notes;
- ignored rules;
- buttons that only open a page;
- rules whose fix is a project's choice, whatever their severity: `other-xr-loader` (a
  project may keep a fallback loader on purpose) and `init-on-startup` (an app may start XR
  itself).

Everything it leaves out keeps its own **Fix** button. **Show passed** adds the rules that
passed or do not apply to the build target.

The ignore list is project state. It is stored in
`ProjectSettings/DisplayXRProjectValidation.json`, which belongs in version control with the
rest of `ProjectSettings/`:

```json
{
  "version": 1,
  "ignored": [
    "runtime-installed"
  ]
}
```

The file exists only while at least one rule is ignored. Ids are sorted, one per line. A
file that cannot be read counts as "nothing ignored".

- The window reads the file again on every refresh (and it refreshes when it gets focus), so
  a change that arrives from version control shows up without a restart.
- If the file cannot be written (read-only until checked out, for example), **Ignore** changes
  nothing: the row stays as it was, the window says "Not saved", and the Console has the
  reason.

## At build time

Before a build, `DisplayXRBuildProcessor.OnPreprocessBuild` evaluates the rules for the
target being built and logs each open **Error** as a **warning**:

```
[DisplayXR] Project validation: Graphics API supported by the provider: The first Graphics API for StandaloneLinux64 is OpenGLCore, which the provider has no backend for (supported here: Vulkan). Current list: OpenGLCore, Vulkan. [graphics-api] (Build target StandaloneLinux64. DisplayXR > Project Validation checks the active build target, which is StandaloneWindows64: switch the active build target to StandaloneLinux64 to see this finding and its fix there, or ignore the rule to silence this.)
```

- It never fails the build and opens no window or dialog, so a batch-mode build behaves the
  same as an interactive one.
- Warnings and notes are not logged. Ignored rules are not logged.
- Each line names the target that was built. The window checks the **active** build target,
  so when a script builds another one the line says to switch to it.
- Nothing is logged for a build target where the DisplayXR Display loader is not enabled:
  that is a choice, `loader-enabled` is never an error, and no other rule applies there.
- Nothing is logged for a build target the provider does not exist for (WebGL, iOS, ...).

## The rules

Severity is the default; some findings of a rule are less (or more) serious and say so.
"Applies to" is the build target. Every rule down to `linux-transparent-overlay`, and the
two URP rules, apply only when the DisplayXR Display loader is enabled for that target;
otherwise they are "not applicable" and `loader-enabled` is the single finding. The notes
are about the editor machine or the installed packages and do not depend on it.

| Id | Severity | Applies to | Fix |
|---|---|---|---|
| [`loader-enabled`](#loader-enabled) | Warning / note | all supported | yes (not in Fix All) |
| [`init-on-startup`](#init-on-startup) | Warning | all supported | yes (not in Fix All) |
| [`other-xr-loader`](#other-xr-loader) | Warning / Error | all supported | yes (not in Fix All) |
| [`graphics-api`](#graphics-api) | Error / Warning / note | all supported | yes |
| [`native-plugin`](#native-plugin) | Error / note | all supported | no |
| [`android-arm64`](#android-arm64) | Error / Warning | Android | yes |
| [`android-min-sdk`](#android-min-sdk) | Warning | Android | yes |
| [`linux-transparent-overlay`](#linux-transparent-overlay) | Warning | Linux | yes |
| [`runtime-installed`](#runtime-installed) | note | all supported | link |
| [`editor-play-mode`](#editor-play-mode) | note | all supported | no |
| [`input-handling`](#input-handling) | note | all supported | opens Player Settings |
| [`xr-hands-version`](#xr-hands-version) | note | all supported | no |
| [`srp-version`](#srp-version) | note | all supported | no |
| [`urp-preserve-framebuffer-alpha`](#urp-preserve-framebuffer-alpha) | Error | Windows, macOS, Linux (URP 17+) | yes |
| [`urp-foreground-clip`](#urp-foreground-clip) | Warning | all supported (URP 17+) | yes |

"All supported" is Windows x64, macOS, Linux x86_64 and Android.

### loader-enabled

**DisplayXR Display is ticked in XR Plug-in Management for the build target.**

- **Why:** the provider is an XR Plug-in Management loader. If it is not enabled, Unity never
  creates the display subsystem and the app renders one flat view.
- **Severity:** never an error, because leaving a target without the loader is a legitimate
  choice.
  - Enabled for the other target group (Standalone / Android): a **note**. This target was
    most likely left out on purpose.
  - Enabled for no target: a **warning**. The package is installed and does nothing yet.
- **Consequences of that choice:** it is never written to the build log, Fix All never
  enables it, and every other project rule is "not applicable" for that target.
- **Source of truth:** `Editor/Provider/DisplayXRDisplayPackage.cs` registers the loader for
  the Standalone and Android build target groups; `DisplayXRLoaderRules.NotEnabledSeverity`.
- **Fix:** enables the loader with `XRPackageMetadataStore.AssignLoader`, the call the
  XR Plug-in Management checkbox makes. Like ticking the box, it can **create assets**: the
  loader asset under `Assets/XR/Loaders/`, and, if the project has XR settings but none for
  this target yet, that target's settings inside the project's XR settings asset. If the
  project has no XR settings asset at all, it creates nothing and opens the XR Plug-in
  Management page instead, which creates one.

### init-on-startup

**Initialize XR on Startup is on.**

- **Why:** with it off, XR Plug-in Management does not start the provider in Play Mode or in
  a built player.
- **Source of truth:** `XRGeneralSettings.InitManagerOnStart`; the README's "Enabling the
  Feature".
- **When to ignore:** an app that starts XR itself. The `DisplayXRRuntime` class comment
  shows one that turns it off at run time on machines without a runtime.
- **Fix:** turns the setting on for the build target. Not part of Fix All.

### other-xr-loader

**DisplayXR Display is the only XR loader enabled for the build target.**

- **Why:** `XRManagerSettings.InitializeLoaderSync` tries the enabled loaders in order and
  keeps the first whose `Initialize()` succeeds. The others never run. The provider drives
  the DisplayXR runtime itself and does not use Unity's OpenXR plug-in.
  - A loader tried **before** DisplayXR takes the session if it initializes, and the provider
    never starts: **Error**.
  - A loader tried **after** DisplayXR takes over whenever DisplayXR declines (no runtime on
    the machine, an editor graphics API it does not support): **Warning**.
- **Source of truth:** XR Plug-in Management's `XRManagerSettings.InitializeLoaderSync`; the
  loader's decline paths in `DisplayXRDisplayLoader.Initialize`.
- **Note on order:** the order is not a user setting. `XRPackageMetadataStore.AssignLoader`
  re-sorts the list each time a loader is ticked.
- **When to ignore:** a project that ships a second loader on purpose as the fallback.
- **Fix:** unticks every other loader for the build target (`XRPackageMetadataStore.RemoveLoader`).
  Not part of Fix All, even when the finding is an error.

### graphics-api

**The build target's first Graphics API is one the provider has a backend for.**

| Build target | Supported |
|---|---|
| Windows | Direct3D 11, Direct3D 12, Vulkan (see below) |
| macOS | Metal |
| Linux | Vulkan |
| Android | Vulkan |

- **Why:** on any other API the native session does not start. Unity starts on the first API
  in the list that the machine supports, so the first entry is the one that is judged.
- **Findings:**
  - First API not supported: **Error**.
  - Windows with Vulkan first: a **note**, with no fix. By design the player then runs on
    Direct3D 12, with stereo (#248, `docs~/unity-bug-report-vk-xr-preinit.md`).
  - Android with Vulkan first and another API behind it: **Warning**. There is no fallback
    API on Android.
- **Source of truth:** the platform x API table in
  `DisplayXRDisplayLoader.IsSinglePassEligible` (`Runtime/Provider/DisplayXRDisplayLoader.cs`).
- **Fix:** turns Auto Graphics API off for the target and writes the list: Vulkan only on
  Android; elsewhere the project's own list with a supported API moved to the front (or
  added, when the list has none). After a change to the editor's own platform, restart the
  editor for Play Mode to use it.

### native-plugin

**The package folder has the native library for the build target.**

| Build target | Files under `Runtime/Plugins/` |
|---|---|
| Windows x64 | `Windows/x64/displayxr_unity.dll` |
| macOS | `macOS/displayxr_unity.bundle` |
| Linux x86_64 | `Linux/x86_64/libdisplayxr_unity.so`, `libdisplayxr_unity_wayland.so` |
| Android arm64 | `Android/arm64-v8a/libdisplayxr_unity.so`, `libopenxr_loader.so`, `libVkLayer_DXR_queue_lock.so` |

- **Why:** without the library the player is built without the provider.
- **Findings:**
  - A file is missing: **Error**. For Linux and Android the usual cause is an install from a
    plain `vX.Y.Z` tag. Those are source tags; only `#upm` and `#upm/vX.Y.Z` carry the
    Linux and Android libraries.
  - A Linux or Android file is missing and the package is a source checkout (a `file:` or
    embedded install): a **note**. Those libraries are built by CI and are not committed, so
    this is expected until you build them (`native~/build-linux.sh`, `native~/build-android.sh`).
  - The build target is 32-bit Windows: **Error**. The package ships x64 only.
  - The package folder cannot be located: not applicable. A package copied into `Assets/`
    is found through its `Editor` assembly definition and treated as a source checkout.
- **Source of truth:** `Runtime/Plugins/`, `.gitignore`, the install note in the README.
- **Fix:** none.

### android-arm64

**Android: Scripting Backend IL2CPP, Target Architectures ARM64 only.**

- **Why:** the package ships Android libraries for `arm64-v8a` only, and Unity builds ARM64
  with IL2CPP only. Without ARM64: **Error**. ARM64 plus another ABI: **Warning** (on the
  other ABI the app has no provider library).
- **Source of truth:** `Runtime/Plugins/Android/arm64-v8a/` is the only ABI folder.
- **Fix:** sets both Player Settings.

### android-min-sdk

**Android: Minimum API Level 29 or higher.**

- **Why:** the Android libraries are built against API level 29 (Android 10).
- **Source of truth:** `ANDROID_PLATFORM` in `native~/build-android.sh`.
- **Fix:** sets Minimum API Level to 29.

### linux-transparent-overlay

**Linux: a transparent overlay app has Transparent Overlay App ticked in the Manifest Settings.**

- **Applies when:** an open scene has a `DisplayXRTransparentOverlay` component.
- **Why:** on native Wayland the window must be created with alpha for the desktop to show
  through, and Unity creates it before any script runs. The build writes that fact into
  `<Data>/DisplayXR/linux_player.json` from the settings asset.
- **Source of truth:** `Editor/DisplayXRProviderRuntimeDeploy.cs`;
  `DisplayXRManifestSettings.transparentOverlay`.
- **Fix:** ticks the box. If the project has no Manifest Settings asset it creates one at
  `Assets/DisplayXRManifestSettings.asset`.

### runtime-installed

**An OpenXR runtime resolves on this machine.**

- **Why it is only a note:** the project is fine. Without a runtime the loader declines and
  the app runs as an ordinary 2D application: in Play Mode on this machine, and in a built
  player on any machine without the runtime. With a runtime but no 3D panel, the runtime
  falls back to a simulated display.
- **Limits:** it is about the editor's machine, not the target device. It does not check
  that the resolved runtime is a DisplayXR one. The answer is read once per editor session,
  so restart the editor after installing. There is no probe on Android.
- **Source of truth:** `DisplayXRRuntime.IsInstalled`; `DisplayXRDisplayLoader.Initialize`
  declines on the same probe.
- **Button:** opens the runtime's releases page.

### editor-play-mode

**Play Mode can run the provider in this editor.**

- **Applies when:** the loader is enabled for Standalone (Play Mode uses those settings).
- **Why:** the loader declines in the macOS editor (a Unity Metal editor defect, #204;
  `DISPLAYXR_METAL_EDITOR=1` opts back in) and in any editor running on Vulkan (#247, #249).
  Built players are not affected.
- **Source of truth:** the two editor gates in `DisplayXRDisplayLoader.Initialize`.
- **Fix:** none. Build a player, or on Windows switch the editor to Direct3D.

### input-handling

**Active Input Handling includes the Input System.**

- **Why:** with Input Manager (Old) only, the Input System does not run. DisplayXR
  controllers and styluses are then not reachable through Input System bindings
  (`UnityEngine.XR.InputDevices` still works), and a transparent overlay cannot forward the
  pointer to `Mouse.current`. The samples' input controller works with either setting.
- **Source of truth:** `Runtime/Provider/DisplayXRInputLayouts.cs`; the `Mouse.current`
  forwarding in `Runtime/DisplayXRTransparentOverlay.cs`.
- **Button:** opens Player Settings. Changing the setting restarts the editor, so the
  validator does not do it.

### xr-hands-version

**If `com.unity.xr.hands` is installed, it is 1.3.0 or newer.**

- **Why:** the hand-tracking assembly is compiled only from 1.3.0. With an older version
  installed it is left out without a message. Not having the package at all passes: hand
  tracking is optional.
- **Source of truth:** `versionDefines` in `Runtime/Hands/com.displayxr.unity.hands.asmdef`.

### srp-version

**If URP or HDRP is installed, it is 17 or newer for the pipeline add-ons.**

- **Why:** stereo rendering works on any URP or HDRP version. The add-ons are compiled from
  version 17 (Unity 6): on URP the foreground clip shader, **DisplayXR > Setup URP Foreground
  Clip**, the depth-aware cursor's depth pass and the two URP validation rules below; on HDRP
  the depth-aware cursor's depth pass.
- **Source of truth:** `versionDefines` in `Runtime/URP`, `Editor/URP` and `Runtime/HDRP`.

### urp-preserve-framebuffer-alpha

**URP transparent overlay: Preserve Framebuffer Alpha is on.**

- **Applies when:** URP is the active pipeline and the project looks like a transparent
  overlay app: Transparent Overlay App is ticked in the Manifest Settings, or an open scene
  has a `DisplayXRTransparentOverlay`. An ordinary opaque app never sees this rule fail.
- **Why:** without the Player Setting, URP may choose a colour format with no alpha channel
  (HDR with 32-bit colour uses `B10G11R11`) and the overlay renders opaque black. The plug-in
  cannot set it at run time.
- **Source of truth:** `Samples~/URPBasicScene/README.md`, "Transparent overlays on URP" (#127).
- **Fix:** turns on Preserve Framebuffer Alpha.

### urp-foreground-clip

**A rig with Foreground Only Clip has the URP clip pass wired.**

- **Applies when:** URP is the active pipeline and a rig in an open scene has
  `foregroundOnlyClip` ticked.
- **Why:** on URP the rig only publishes the clip planes as shader globals. The clip is done
  by a Full Screen Pass renderer feature running `DisplayXR/ForegroundClipURP`. Without the
  pass nothing is clipped and nothing warns.
- **Which renderer assets:** the ones in use, not the first one a project search returns.
  `DisplayXRUrpInstaller.ResolveClipRenderers` takes every URP pipeline asset the project can
  run with (the active one, the Graphics default, and each quality level's override) and, in
  each, the renderer the rig's camera selects (its Renderer setting; the pipeline's default
  renderer when the camera says "default"). The finding names the renderer assets that lack
  the pass. A quality level you never ship still counts; ignore the rule if that is the only
  one named.
- **Last resort:** if no URP pipeline asset is assigned anywhere, the first URP renderer
  asset in the project is used, and the finding (and the installer) say that it is a guess.
- **A pass wired by hand counts.** A Full Screen Pass whose material runs
  `DisplayXR/ForegroundClipURP` is recognised under any name, and the installer leaves it
  exactly as it is (material, injection point, requirements). It only reconfigures the
  feature it created itself, named "DisplayXR Foreground Clip".
- **Source of truth:** `ResolveClipRenderers` and `FindClipFeature` in
  `Editor/URP/DisplayXRUrpInstaller.cs`. The installer and this rule use the same functions.
- **Fix:** the same as **DisplayXR > Setup URP Foreground Clip**: creates
  `Assets/DisplayXR/DXRForegroundClip.mat` and adds the renderer feature to each of those
  renderer assets that lacks it.

## Considered and left out

| Candidate | Why there is no rule |
|---|---|
| Render path (Built-in = multi-pass, URP / HDRP = single-pass instanced) | No project setting can contradict it. The loader decides at start-up from the active pipeline (`IsSinglePassEligible`). |
| Colour space | Both work. The provider picks the swapchain format from the project's colour space. |
| Scripting backend on desktop | Nothing in the package depends on it. |
| `com.unity.xr.openxr` installed | Having the package is harmless. Its loader being enabled is what matters: `other-xr-loader`. |
| A separate "32-bit Windows" rule | Part of `native-plugin`. |

## Adding a rule

Any editor assembly can add rules. Reference `com.displayxr.unity.editor` and register from
an `[InitializeOnLoadMethod]`:

```csharp
using DisplayXR.Editor;
using UnityEditor;

static class MyRules
{
    [InitializeOnLoadMethod]
    static void Register()
    {
        DisplayXRValidationRegistry.Register(new DisplayXRValidationRule
        {
            Id = "com.acme.color-space",          // lower-case, digits, '-' and '.'
            Title = "Linear colour space",
            Explanation = "Our shaders assume linear.",
            Severity = DisplayXRValidationSeverity.Warning,
            Platforms = DisplayXRValidationPlatforms.Supported,
            Order = 1000,                          // the package's rules use 10..210
            Check = c => PlayerSettings.colorSpace == UnityEngine.ColorSpace.Linear
                ? DisplayXRValidationResult.Pass()
                : DisplayXRValidationResult.Fail("The project is in Gamma space."),
            Fix = c => PlayerSettings.colorSpace = UnityEngine.ColorSpace.Linear,
            FixDescription = "Set Color Space to Linear (Player Settings)",
        });
    }
}
```

- `Check` must only read. It receives the build target (`c.Platform`, `c.Target()`,
  `c.Group()`, `c.NamedTarget()`) and whether it runs from the build hook (`c.IsBuild`).
- `Fix` is optional. Give one only when it writes an ordinary setting the user can change
  back. Set `FixOnlyNavigates` when it only opens a page; such a fix is not part of Fix All.
- Fix All runs the fixes of Errors only. Set `ExcludeFromFixAll` on a rule whose fix is a
  matter of project policy, so that it is left out even when its finding is an error.
- A rule that only matters to a DisplayXR build should open with the loader gate, so that it
  is silent for a target that does not use DisplayXR:
  `DisplayXRValidationResult off; if (DisplayXRProjectValidation.LoaderNotEnabled(c, out off)) return off;`
- A rule id is what the ignore list stores. Do not rename one after it has shipped.
- A check that throws is shown as a warning naming the exception. It does not stop the
  other rules.

The package's URP rules are registered this way from `Editor/URP/`, an assembly that is
compiled only when URP 17 or newer is installed.

## Code map and tests

| File | Role |
|---|---|
| `Editor/Validation/DisplayXRValidationCore.cs` | Rule, result, registry, evaluation, summary, ignore-list file. Pure C#. |
| `Editor/Validation/DisplayXRValidationLogic.cs` | The decisions behind the built-in rules (graphics APIs, loader order, native files, Android, versions). Pure C#. |
| `Editor/Validation/DisplayXRProjectValidation.cs` | Unity side: context, evaluation, ignore list, build hook, shared project queries. |
| `Editor/Validation/DisplayXRBuiltInRules.cs` | The package's rules. |
| `Editor/URP/DisplayXRUrpValidationRules.cs` | The URP-only rules. |
| `Editor/Validation/DisplayXRProjectValidationWindow.cs` | The window. |
| `Editor/DisplayXRBuildProcessor.cs` | `OnPreprocessBuild` calls the build hook. |
| `Editor/DisplayXRSettingsProvider.cs` | The summary on the settings page. |

The two pure C# files are compiled and tested without the editor by
`Tests~/ProjectValidation` (`dotnet run --project "Tests~/ProjectValidation" -c Release`),
which CI runs with the other C# tests. They cover the registry, evaluation, what Fix All
takes, what the build hook logs and its hint, the ignore-list file (including a failed
write being rolled back), and each decision above. The tests also check the rules against
the tree: every library a rule names has a committed `.meta`, the loader's class name, and
the API level in `native~/build-android.sh`. Reading the facts out of a project (including
which URP renderer assets are in use), the fixes and the window need the editor and are not
covered by those tests.

## Rig menu items

Not validation, but added with it (#462): **GameObject > DisplayXR > Camera-Centric Rig** and
**Display-Centric Rig** (`Editor/DisplayXRRigMenu.cs`).

- The item acts once over the whole selection, from the menu bar and from the Hierarchy's
  right-click menu alike.
- If the selection has cameras, it adds the rig component to each of them and creates
  nothing, whatever else is selected.
- If the selection has no camera, it creates one GameObject with a Camera and the rig, as a
  child of the selected object if there is one. It is tagged `MainCamera` only when there is
  no main camera yet, and given an Audio Listener only when the open scenes have none.
- In Prefab Mode the new camera gets neither the tag nor an Audio Listener: the prefab's
  preview scene has none, but the scenes the prefab goes into usually do.
- It is one Undo step. Every rig value is the component's own default.
- A camera takes one rig. The item skips a camera that already has the other kind and logs
  a warning.

The same components are under **Component > DisplayXR**.
