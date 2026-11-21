// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Project validation, the rules the package ships (#462). Every rule restates something
// the package's own code or docs already require; the comment above each one names that
// source, and docs~/project-validation.md lists them all. The URP-only rules are in
// Editor/URP/DisplayXRUrpValidationRules.cs, so a project without URP never compiles them.
//
// A rule's Check only reads. A rule's Fix writes one ordinary Unity setting - something
// the user can see and undo on the usual settings page - and runs only from a click.

using System;
using System.Collections.Generic;
using System.IO;
using UnityEditor;
using UnityEditor.Build;
using UnityEditor.XR.Management;
using UnityEditor.XR.Management.Metadata;
using UnityEngine;
using UnityEngine.Rendering;
using UnityEngine.XR.Management;

using Platforms = DisplayXR.Editor.DisplayXRValidationPlatforms;
using Result = DisplayXR.Editor.DisplayXRValidationResult;
using Severity = DisplayXR.Editor.DisplayXRValidationSeverity;

namespace DisplayXR.Editor
{
    static class DisplayXRBuiltInRules
    {
        const string RuntimeReleasesUrl = "https://github.com/DisplayXR/displayxr-runtime/releases/latest";

        static string LoaderType { get { return typeof(DisplayXRDisplayLoader).FullName; } }

        [InitializeOnLoadMethod]
        static void Register()
        {
            // ---------------------------------------------------------------- XR setup
            Add(new DisplayXRValidationRule
            {
                Id = "loader-enabled",
                Title = "DisplayXR Display loader enabled",
                Explanation =
                    "The provider is an XR Plug-in Management loader. Unless it is ticked for the " +
                    "build target, Unity never creates the display subsystem and the app renders " +
                    "one flat view. Leaving a target without it is a legitimate choice (a 3D app on " +
                    "Windows that also has an Android target), so this is a warning at most and is " +
                    "never written to the build log; every other rule is not applicable for such a " +
                    "target.",
                // The result sets the real severity (DisplayXRLoaderRules.NotEnabledSeverity).
                Severity = Severity.Warning,
                Order = 10,
                Check = CheckLoaderEnabled,
                Fix = FixLoaderEnabled,
                FixDescription = "Enable the DisplayXR Display loader in XR Plug-in Management for the " +
                                 "active build target. Like ticking the box, this can create assets: " +
                                 "the loader asset under Assets/XR/Loaders/, and that target's XR " +
                                 "settings inside the project's XR settings asset if it has none yet",
            });

            Add(new DisplayXRValidationRule
            {
                Id = "init-on-startup",
                Title = "Initialize XR on Startup",
                Explanation =
                    "With it off, XR Plug-in Management does not start the provider in Play Mode or " +
                    "in a built player. That is only right for an app that starts XR itself " +
                    "(XRGeneralSettings.Instance.Manager.InitializeLoaderSync) - ignore the rule then.",
                Severity = Severity.Warning,
                Order = 20,
                Check = CheckInitOnStartup,
                Fix = FixInitOnStartup,
                ExcludeFromFixAll = true, // a choice for apps that start XR themselves
                FixDescription = "Turn on Initialize XR on Startup in XR Plug-in Management for the " +
                                 "active build target",
            });

            Add(new DisplayXRValidationRule
            {
                Id = "other-xr-loader",
                Title = "No other XR loader beside DisplayXR",
                Explanation =
                    "XR Plug-in Management tries the enabled loaders in order and keeps the FIRST " +
                    "that initializes; the others never run. The provider drives the DisplayXR " +
                    "runtime itself and does not need Unity's OpenXR plug-in. A loader ahead of " +
                    "DisplayXR takes the session away from it; one behind it takes over whenever " +
                    "DisplayXR declines (no runtime installed, an editor graphics API it does not " +
                    "support).",
                Severity = Severity.Warning,
                Order = 30,
                Check = CheckOtherLoaders,
                Fix = FixOtherLoaders,
                ExcludeFromFixAll = true, // a project may keep a fallback loader on purpose
                FixDescription = "Untick every XR loader other than DisplayXR Display for the active " +
                                 "build target (XR Plug-in Management)",
            });

            // ---------------------------------------------------------------- Player
            Add(new DisplayXRValidationRule
            {
                Id = "graphics-api",
                Title = "Graphics API supported by the provider",
                Explanation =
                    "The provider has a backend for Direct3D 11, Direct3D 12 and Vulkan on Windows, " +
                    "Metal on macOS, and Vulkan on Linux and Android. On any other API the native " +
                    "session does not start. Unity starts on the first API in the list that the " +
                    "machine supports.",
                Severity = Severity.Error,
                Order = 40,
                Check = CheckGraphicsApi,
                Fix = FixGraphicsApi,
                FixDescription = "Change the build target's Graphics APIs (Player Settings > Other " +
                                 "Settings) so a supported API leads - Vulkan only on Android; turns " +
                                 "Auto Graphics API off for that target",
            });

            Add(new DisplayXRValidationRule
            {
                Id = "native-plugin",
                Title = "Native plug-in binary present",
                Explanation =
                    "The provider is a native library. If the package folder has none for the build " +
                    "target, the player is built without it and the loader fails to create the " +
                    "display subsystem.",
                Severity = Severity.Error,
                Order = 50,
                Check = CheckNativePlugin,
            });

            Add(new DisplayXRValidationRule
            {
                Id = "fullscreen-mode",
                Title = "Windows: fullscreen mode is not Exclusive Fullscreen",
                Explanation =
                    "The provider's window handling has no path for exclusive full screen. " +
                    "DisplayXRTargetDisplay refuses to move such a window onto the 3D panel, and " +
                    "the native move behind it only knows a windowed window and a borderless one " +
                    "that covers its monitor (Fullscreen Window). The 3D picture is also not drawn " +
                    "into Unity's window: the provider presents it in its own borderless window " +
                    "above Unity's, shown by the desktop compositor and following Unity's window. " +
                    "Exclusive full screen is the one mode where Unity's own swap chain owns the " +
                    "output instead, and it holds the display it was created on. The package is " +
                    "not tested in that mode. This is the launch default only: a script can still " +
                    "switch mode at run time.",
                Severity = Severity.Warning,
                Platforms = Platforms.Windows,
                Order = 55,
                Check = CheckFullscreenMode,
                Fix = FixFullscreenMode,
                FixDescription = "Set Fullscreen Mode to Fullscreen Window (Player Settings > " +
                                 "Resolution and Presentation)",
            });

            Add(new DisplayXRValidationRule
            {
                Id = "android-arm64",
                Title = "Android: ARM64 (IL2CPP)",
                Explanation =
                    "The package ships Android libraries for arm64-v8a only. Unity builds ARM64 " +
                    "with the IL2CPP scripting backend only.",
                Severity = Severity.Error,
                Platforms = Platforms.Android,
                Order = 60,
                Check = CheckAndroidArm64,
                Fix = FixAndroidArm64,
                FixDescription = "Android Player Settings: Scripting Backend = IL2CPP, Target " +
                                 "Architectures = ARM64 only",
            });

            Add(new DisplayXRValidationRule
            {
                Id = "android-min-sdk",
                Title = "Android: minimum API level 29",
                Explanation =
                    "The Android libraries are built against API level 29 (Android 10). On an older " +
                    "device the app installs but the provider cannot load.",
                Severity = Severity.Warning,
                Platforms = Platforms.Android,
                Order = 70,
                Check = CheckAndroidMinSdk,
                Fix = c => PlayerSettings.Android.minSdkVersion = (AndroidSdkVersions)DisplayXRAndroidRules.MinApiLevel,
                FixDescription = "Android Player Settings: Minimum API Level = 29",
            });

            Add(new DisplayXRValidationRule
            {
                Id = "linux-transparent-overlay",
                Title = "Linux: transparent overlay declared at build time",
                Explanation =
                    "On native Wayland the player's window has to be created with alpha for the " +
                    "desktop to show through, and Unity creates it before any script runs. The " +
                    "build writes that fact from the DisplayXR Manifest Settings; without the tick " +
                    "a transparent overlay app gets an opaque window there.",
                Severity = Severity.Warning,
                Platforms = Platforms.Linux,
                Order = 80,
                Check = CheckLinuxTransparentOverlay,
                Fix = FixLinuxTransparentOverlay,
                FixDescription = "Tick Transparent Overlay App in the DisplayXR Manifest Settings asset " +
                                 "(creates Assets/DisplayXRManifestSettings.asset if the project has none)",
            });

            // ---------------------------------------------------------------- Notes
            Add(new DisplayXRValidationRule
            {
                Id = "runtime-installed",
                Title = "DisplayXR runtime on this machine",
                Explanation =
                    "Without an OpenXR runtime the loader declines and the app runs as an ordinary " +
                    "2D application - in Play Mode here, and in a built player on any machine " +
                    "without the runtime. The runtime is installed separately on every target " +
                    "machine; with no 3D panel attached it falls back to a simulated display.",
                Severity = Severity.Info,
                Order = 100,
                Check = CheckRuntimeInstalled,
                Fix = c => Application.OpenURL(RuntimeReleasesUrl),
                FixLabel = "Get the runtime",
                FixOnlyNavigates = true,
            });

            Add(new DisplayXRValidationRule
            {
                Id = "editor-play-mode",
                Title = "Play Mode can run the provider in this editor",
                Explanation =
                    "Play Mode runs the provider itself, but it declines in two editors: the macOS " +
                    "editor (a Unity Metal editor defect) and any editor running on Vulkan (Unity's " +
                    "Vulkan XR path needs a hook that only built players have). Built players are " +
                    "not affected.",
                Severity = Severity.Info,
                Order = 110,
                Check = CheckEditorPlayMode,
            });

            Add(new DisplayXRValidationRule
            {
                Id = "input-handling",
                Title = "Input System backend active",
                Explanation =
                    "With Active Input Handling set to Input Manager (Old), the Input System does " +
                    "not run: DisplayXR controllers and styluses are not reachable through Input " +
                    "System bindings (UnityEngine.XR.InputDevices still works), and a transparent " +
                    "overlay cannot forward the pointer to Mouse.current.",
                Severity = Severity.Info,
                Order = 120,
                Check = CheckInputHandling,
                Fix = c => SettingsService.OpenProjectSettings("Project/Player"),
                FixLabel = "Open Player Settings",
                FixOnlyNavigates = true,
            });

            Add(new DisplayXRValidationRule
            {
                Id = "xr-hands-version",
                Title = "XR Hands 1.3.0 or newer (optional)",
                Explanation =
                    "Hand tracking is compiled in only when com.unity.xr.hands 1.3.0 or newer is in " +
                    "the project. With an older version installed it is silently left out.",
                Severity = Severity.Info,
                Order = 130,
                Check = CheckXrHandsVersion,
            });

            Add(new DisplayXRValidationRule
            {
                Id = "srp-version",
                Title = "URP / HDRP 17 or newer for the pipeline add-ons (optional)",
                Explanation =
                    "Stereo rendering works on any URP / HDRP version. The pipeline-specific " +
                    "add-ons are compiled in from version 17 (Unity 6): on URP the foreground clip " +
                    "shader, its setup menu and the depth-aware cursor's depth pass; on HDRP the " +
                    "depth-aware cursor's depth pass.",
                Severity = Severity.Info,
                Order = 140,
                Check = CheckSrpVersion,
            });
        }

        static void Add(DisplayXRValidationRule rule)
        {
            rule.DocUrl = DisplayXRProjectValidation.DocUrlFor(rule.Id);
            DisplayXRValidationRegistry.Register(rule);
        }

        // ==== loader-enabled ====================================================
        // Source: Editor/Provider/DisplayXRDisplayPackage.cs (the loader is registered for
        // Standalone and Android); README "Enabling the Feature".

        static Result CheckLoaderEnabled(DisplayXRValidationContext c)
        {
            var group = c.Group();
            if (DisplayXRProjectValidation.IsDisplayXRLoaderEnabled(group))
                return Result.Pass("Enabled for " + group + ".");

            // Enabled for the other target group = this one was left out on purpose: a
            // note. Enabled nowhere = the package is installed and does nothing: a warning.
            bool elsewhere = DisplayXRProjectValidation.IsDisplayXRLoaderEnabledElsewhere(group);
            var severity = DisplayXRLoaderRules.NotEnabledSeverity(elsewhere);
            string tail = elsewhere
                ? " It is enabled for another build target, so this may be intended: builds for " +
                  group + " then render without DisplayXR, and the other rules do not apply to them."
                : " It is not enabled for any build target, so the plug-in does nothing yet.";

            XRGeneralSettings general; XRManagerSettings manager;
            if (!DisplayXRProjectValidation.TryGetXRSettings(group, out general, out manager))
                return Result.Fail("XR Plug-in Management has no settings for " + group +
                                   " yet, so no XR loader is enabled for it." + tail, severity);
            return Result.Fail("DisplayXR Display is not ticked under XR Plug-in Management > " + group +
                               "." + tail, severity);
        }

        static void FixLoaderEnabled(DisplayXRValidationContext c)
        {
            var group = c.Group();
            XRGeneralSettings general; XRManagerSettings manager;
            if (!DisplayXRProjectValidation.TryGetXRSettings(group, out general, out manager))
            {
                // The project has an XR settings asset but nothing for this target yet:
                // create that target's (empty) settings, as its tab would on first open.
                XRGeneralSettingsPerBuildTarget perTarget;
                if (EditorBuildSettings.TryGetConfigObject(XRGeneralSettings.k_SettingsKey, out perTarget)
                    && perTarget != null)
                {
                    if (!perTarget.HasManagerSettingsForBuildTarget(group))
                        perTarget.CreateDefaultManagerSettingsForBuildTarget(group);
                    DisplayXRProjectValidation.TryGetXRSettings(group, out general, out manager);
                }
            }
            if (manager == null)
            {
                // No XR settings asset at all. Only the settings page creates one, so send
                // the user there instead of writing into a project folder they did not pick.
                Debug.Log("[DisplayXR] XR Plug-in Management is not set up in this project yet - opening " +
                          "its page; tick DisplayXR Display there.");
                DisplayXRProjectValidation.OpenXRPluginManagement();
                return;
            }
            if (!XRPackageMetadataStore.AssignLoader(manager, LoaderType, group))
                Debug.LogWarning("[DisplayXR] Could not enable the DisplayXR Display loader for " + group +
                                 " - tick it under Project Settings > XR Plug-in Management.");
        }

        // ==== init-on-startup ===================================================
        // Source: README "Enabling the Feature" step 4; XRGeneralSettings.InitManagerOnStart.

        static Result CheckInitOnStartup(DisplayXRValidationContext c)
        {
            var group = c.Group();
            Result off;
            if (DisplayXRProjectValidation.LoaderNotEnabled(c, out off)) return off;
            XRGeneralSettings general; XRManagerSettings manager;
            DisplayXRProjectValidation.TryGetXRSettings(group, out general, out manager);
            return general.InitManagerOnStart
                ? Result.Pass()
                : Result.Fail("Initialize XR on Startup is off for " + group +
                              ": the provider does not start unless the app starts XR itself.");
        }

        static void FixInitOnStartup(DisplayXRValidationContext c)
        {
            XRGeneralSettings general; XRManagerSettings manager;
            if (!DisplayXRProjectValidation.TryGetXRSettings(c.Group(), out general, out manager)) return;
            general.InitManagerOnStart = true;
            EditorUtility.SetDirty(general);
            AssetDatabase.SaveAssetIfDirty(general);
        }

        // ==== other-xr-loader ===================================================
        // Source: XRManagerSettings.InitializeLoaderSync (first loader to initialize wins);
        // README "Enabling the Feature" step 3; docs~/quick-start-guide.md step 2.

        static Result CheckOtherLoaders(DisplayXRValidationContext c)
        {
            var names = DisplayXRProjectValidation.LoaderTypeNames(c.Group());
            var verdict = DisplayXRLoaderRules.Classify(names);
            if (verdict == DisplayXRLoaderRules.Verdict.NotEnabled)
                return Result.NotApplicable("The DisplayXR Display loader is not enabled.");
            if (verdict == DisplayXRLoaderRules.Verdict.Sole)
                return Result.Pass();

            var friendly = new List<string>();
            foreach (var other in DisplayXRLoaderRules.Others(names))
                friendly.Add(DisplayXRLoaderRules.FriendlyName(other));
            string list = string.Join(", ", friendly.ToArray());

            if (verdict == DisplayXRLoaderRules.Verdict.OtherLoaderFirst)
                return Result.Fail(
                    "Also enabled, and tried BEFORE DisplayXR: " + list + ". If it initializes, the " +
                    "DisplayXR provider never starts.", Severity.Error);
            return Result.Fail(
                "Also enabled, tried after DisplayXR: " + list + ". It takes over whenever DisplayXR " +
                "declines (for example on a machine without the DisplayXR runtime).");
        }

        static void FixOtherLoaders(DisplayXRValidationContext c)
        {
            var group = c.Group();
            XRGeneralSettings general; XRManagerSettings manager;
            if (!DisplayXRProjectValidation.TryGetXRSettings(group, out general, out manager)) return;
            foreach (var other in DisplayXRLoaderRules.Others(DisplayXRProjectValidation.LoaderTypeNames(group)))
                if (!XRPackageMetadataStore.RemoveLoader(manager, other, group))
                    Debug.LogWarning("[DisplayXR] Could not disable the XR loader " + other + " for " + group +
                                     " - untick it under Project Settings > XR Plug-in Management.");
        }

        // ==== graphics-api ======================================================
        // Source: the platform x API table in DisplayXRDisplayLoader.IsSinglePassEligible;
        // README "Linux Standalone" / "Android"; issue #248 for Windows + Vulkan.

        static List<string> GraphicsApiNames(BuildTarget target)
        {
            var names = new List<string>();
            var apis = PlayerSettings.GetGraphicsAPIs(target);
            if (apis != null)
                foreach (var api in apis)
                    names.Add(api.ToString());
            return names;
        }

        static Result CheckGraphicsApi(DisplayXRValidationContext c)
        {
            Result off;
            if (DisplayXRProjectValidation.LoaderNotEnabled(c, out off)) return off;
            var apis = GraphicsApiNames(c.Target());
            string list = string.Join(", ", apis.ToArray());
            switch (DisplayXRGraphicsApiRules.Classify(c.Platform, apis))
            {
                case DisplayXRGraphicsApiRules.Verdict.Ok:
                    return Result.Pass(list);
                case DisplayXRGraphicsApiRules.Verdict.FirstUnsupported:
                    return Result.Fail(
                        "The first Graphics API for " + c.Target() + " is " + apis[0] + ", which the " +
                        "provider has no backend for (supported here: " +
                        string.Join(", ", DisplayXRGraphicsApiRules.Supported(c.Platform)) +
                        "). Current list: " + list + ".");
                case DisplayXRGraphicsApiRules.Verdict.WindowsVulkanFallsBackToD3D12:
                    return Result.Fail(
                        "Vulkan is the first Graphics API on Windows. By design the player then runs " +
                        "on Direct3D 12, with stereo: Unity's Vulkan XR start-up cannot be satisfied " +
                        "on Windows (#248). Nothing to fix - put Direct3D 12 or 11 first to make the " +
                        "list say what actually runs.", Severity.Info, noFix: true);
                case DisplayXRGraphicsApiRules.Verdict.AndroidHasNonVulkanFallback:
                    return Result.Fail(
                        "Vulkan leads, but the list also has another API (" + list + "). A device that " +
                        "does not take Vulkan would start on an API the provider has no backend for. " +
                        "There is no fallback API on Android: keep Vulkan only.", Severity.Warning);
                default:
                    return Result.NotApplicable();
            }
        }

        static void FixGraphicsApi(DisplayXRValidationContext c)
        {
            var target = c.Target();
            var recommended = DisplayXRGraphicsApiRules.Recommend(c.Platform, GraphicsApiNames(target));
            var apis = new List<GraphicsDeviceType>();
            foreach (var name in recommended)
            {
                GraphicsDeviceType api;
                if (Enum.TryParse(name, out api)) apis.Add(api);
            }
            if (apis.Count == 0) return;
            PlayerSettings.SetUseDefaultGraphicsAPIs(target, false);
            PlayerSettings.SetGraphicsAPIs(target, apis.ToArray());
            Debug.Log("[DisplayXR] Graphics APIs for " + target + " set to: " +
                      string.Join(", ", recommended.ToArray()) +
                      ". If this is the editor's own platform, restart the editor for Play Mode to use it.");
        }

        // ==== native-plugin =====================================================
        // Source: Runtime/Plugins/ (Windows x64 only), .gitignore (the Linux and Android
        // libraries are CI-built, not committed), README install note on #upm/vX.Y.Z.

        static Result CheckNativePlugin(DisplayXRValidationContext c)
        {
            Result off;
            if (DisplayXRProjectValidation.LoaderNotEnabled(c, out off)) return off;
            if (c.Target() == BuildTarget.StandaloneWindows)
                return Result.Fail("The build target is 32-bit Windows. The package ships a Windows " +
                                   "x64 library only - switch the architecture to x86_64.");

            bool sourceCheckout;
            string root = DisplayXRProjectValidation.PackageRoot(out sourceCheckout);
            if (root == null)
                return Result.NotApplicable("The package folder could not be located, so its " +
                                            "libraries were not checked.");
            var missing = new List<string>();
            foreach (var relative in DisplayXRNativePluginRules.RequiredFiles(c.Platform))
            {
                string path = Path.Combine(root, relative);
                if (!File.Exists(path) && !Directory.Exists(path))
                    missing.Add(Path.GetFileName(relative));
            }
            if (missing.Count == 0) return Result.Pass();

            string list = string.Join(", ", missing.ToArray());
            var severity = DisplayXRNativePluginRules.MissingSeverity(c.Platform, sourceCheckout);
            if (severity == Severity.Info)
                return Result.Fail(
                    "Not in the package folder: " + list + ". Expected here: this is a source " +
                    "checkout of the package, and the " + c.Platform + " libraries are built by CI, " +
                    "not committed. Build them with native~/build-" +
                    (c.Platform == Platforms.Android ? "android" : "linux") +
                    ".sh, or install the package from the upm branch, before building a player.",
                    severity);
            if (DisplayXRNativePluginRules.IsCiBuiltOnly(c.Platform))
                return Result.Fail(
                    "Not in the package folder: " + list + ". The plain vX.Y.Z tags are source tags " +
                    "without the " + c.Platform + " libraries - install from ...displayxr-unity.git#upm " +
                    "or #upm/vX.Y.Z (or the release tarball).", severity);
            return Result.Fail("Not in the package folder: " + list + ". Reinstall the package, or " +
                               "build it from native~/ in a source checkout.", severity);
        }

        // ==== fullscreen-mode ===================================================
        // Source: the ExclusiveFullScreen refusal in DisplayXRTargetDisplay.Move
        // (Runtime/DisplayXRTargetDisplay.cs); ps_window_is_full_screen / ps_cover_monitor /
        // ps_fit_window_to_monitor in native~/displayxr_xrprovider/displayxr_provider_session.cpp
        // (the two window shapes the move knows); displayxr_get_app_main_view and
        // parent_subclass_proc in native~/displayxr_win32.c (the provider's own window above
        // Unity's). PlayerSettings.fullScreenMode is one value for every desktop target; only
        // Windows has an exclusive mode.

        static Result CheckFullscreenMode(DisplayXRValidationContext c)
        {
            Result off;
            if (DisplayXRProjectValidation.LoaderNotEnabled(c, out off)) return off;
            string mode = PlayerSettings.fullScreenMode.ToString();
            switch (DisplayXRFullscreenRules.Classify(c.Platform, mode))
            {
                case DisplayXRFullscreenRules.Verdict.Ok:
                    return Result.Pass(mode + ".");
                case DisplayXRFullscreenRules.Verdict.ExclusiveNotSupported:
                    return Result.Fail(
                        "Fullscreen Mode is Exclusive Fullscreen. The player then starts in a mode " +
                        "the provider's window handling does not cover: start-up placement " +
                        "(DisplayXRTargetDisplay) leaves the window on the display it opened on, and " +
                        "the 3D picture is presented in a separate window above it. Use Fullscreen " +
                        "Window (borderless) to fill the 3D panel.");
                default:
                    return Result.NotApplicable();
            }
        }

        static void FixFullscreenMode(DisplayXRValidationContext c)
        {
            string recommended = DisplayXRFullscreenRules.Recommend(
                c.Platform, PlayerSettings.fullScreenMode.ToString());
            FullScreenMode mode;
            if (!Enum.TryParse(recommended, out mode)) return;
            PlayerSettings.fullScreenMode = mode;
            Debug.Log("[DisplayXR] Fullscreen Mode set to " + mode + " (Player Settings > Resolution " +
                      "and Presentation). A player that already ran on this machine may still open " +
                      "in the mode it last saved.");
        }

        // ==== android-arm64 =====================================================
        // Source: Runtime/Plugins/Android/arm64-v8a is the only ABI folder; README "Android".

        static Result CheckAndroidArm64(DisplayXRValidationContext c)
        {
            Result off;
            if (DisplayXRProjectValidation.LoaderNotEnabled(c, out off)) return off;
            bool il2cpp = PlayerSettings.GetScriptingBackend(NamedBuildTarget.Android)
                          == ScriptingImplementation.IL2CPP;
            var verdict = DisplayXRAndroidRules.ClassifyArchitectures(
                (uint)PlayerSettings.Android.targetArchitectures);
            if (!il2cpp)
                return Result.Fail("The Android Scripting Backend is not IL2CPP, so the build cannot " +
                                   "target ARM64 - the only ABI the package has libraries for.");
            switch (verdict)
            {
                case DisplayXRAndroidRules.ArchVerdict.NoArm64:
                    return Result.Fail("Target Architectures does not include ARM64 - the only ABI the " +
                                       "package has libraries for.");
                case DisplayXRAndroidRules.ArchVerdict.Arm64AndOthers:
                    return Result.Fail("Target Architectures includes more than ARM64 (" +
                                       PlayerSettings.Android.targetArchitectures + "). On the other " +
                                       "ABIs the app has no provider library.", Severity.Warning);
                default:
                    return Result.Pass();
            }
        }

        static void FixAndroidArm64(DisplayXRValidationContext c)
        {
            PlayerSettings.SetScriptingBackend(NamedBuildTarget.Android, ScriptingImplementation.IL2CPP);
            PlayerSettings.Android.targetArchitectures = AndroidArchitecture.ARM64;
        }

        // ==== android-min-sdk ===================================================
        // Source: native~/build-android.sh (API 29); README "Android".

        static Result CheckAndroidMinSdk(DisplayXRValidationContext c)
        {
            Result off;
            if (DisplayXRProjectValidation.LoaderNotEnabled(c, out off)) return off;
            int min = (int)PlayerSettings.Android.minSdkVersion;
            return min >= DisplayXRAndroidRules.MinApiLevel
                ? Result.Pass("Minimum API Level " + min + ".")
                : Result.Fail("Minimum API Level is " + min + "; the provider needs " +
                              DisplayXRAndroidRules.MinApiLevel + " (Android 10).");
        }

        // ==== linux-transparent-overlay =========================================
        // Source: Editor/DisplayXRProviderRuntimeDeploy.cs (writes linux_player.json from
        // DisplayXRManifestSettings.transparentOverlay); README "Linux Standalone" step 3.

        static Result CheckLinuxTransparentOverlay(DisplayXRValidationContext c)
        {
            Result off;
            if (DisplayXRProjectValidation.LoaderNotEnabled(c, out off)) return off;
            var overlays = DisplayXRProjectValidation.FindInLoadedScenes<DisplayXRTransparentOverlay>();
            if (overlays.Length == 0)
                return Result.NotApplicable("No DisplayXR Transparent Overlay in the open scenes.");
            var manifest = DisplayXRManifestSettings.Find();
            if (manifest != null && manifest.transparentOverlay) return Result.Pass();
            return Result.Fail(
                "'" + overlays[0].name + "' has a DisplayXR Transparent Overlay, but Transparent " +
                "Overlay App is not ticked in the DisplayXR Manifest Settings" +
                (manifest == null ? " (the project has no such asset yet)." : "."));
        }

        static void FixLinuxTransparentOverlay(DisplayXRValidationContext c)
        {
            var manifest = DisplayXRManifestSettings.GetOrCreate();
            Undo.RecordObject(manifest, "Tick Transparent Overlay App");
            manifest.transparentOverlay = true;
            EditorUtility.SetDirty(manifest);
            AssetDatabase.SaveAssetIfDirty(manifest);
        }

        // ==== runtime-installed =================================================
        // Source: DisplayXRRuntime.IsInstalled, and DisplayXRDisplayLoader.Initialize, which
        // declines on that same probe.

        static Result CheckRuntimeInstalled(DisplayXRValidationContext c)
        {
            if (!DisplayXRRuntime.ProbeSupported) return Result.NotApplicable();
            if (DisplayXRRuntime.IsInstalled)
                return Result.Pass(DisplayXRRuntime.ResolvedManifestPath);
            return Result.Fail(
                "No OpenXR runtime resolves on this machine (XR_RUNTIME_JSON, then the system's " +
                "active-runtime record), so Play Mode runs as a 2D app. The answer is read once per " +
                "editor session: restart the editor after installing.");
        }

        // ==== editor-play-mode ==================================================
        // Source: the two editor gates in DisplayXRDisplayLoader.Initialize (#204, #247/#249).

        static Result CheckEditorPlayMode(DisplayXRValidationContext c)
        {
            // Play Mode always runs the Standalone XR settings.
            if (!DisplayXRProjectValidation.IsDisplayXRLoaderEnabled(BuildTargetGroup.Standalone))
                return Result.NotApplicable("The DisplayXR Display loader is not enabled for Standalone.");

            if (Application.platform == RuntimePlatform.OSXEditor &&
                Environment.GetEnvironmentVariable("DISPLAYXR_METAL_EDITOR") != "1")
                return Result.Fail(
                    "In the macOS editor the provider declines in Play Mode (a Unity Metal editor " +
                    "defect, displayxr-unity#204). Build and run a macOS player to see stereo.");

            if (SystemInfo.graphicsDeviceType == GraphicsDeviceType.Vulkan)
                return Result.Fail(
                    "This editor runs on Vulkan, where the provider declines in Play Mode " +
                    "(displayxr-unity#247 / #249). " +
                    (Application.platform == RuntimePlatform.LinuxEditor
                        ? "Build and run a Linux player to see stereo."
                        : "Build and run a player, or switch the editor to Direct3D 12 / 11."));

            return Result.Pass("Editor graphics API: " + SystemInfo.graphicsDeviceType + ".");
        }

        // ==== input-handling ====================================================
        // Source: Runtime/Provider/DisplayXRInputLayouts.cs (Input System layouts) and the
        // Mouse.current forwarding in Runtime/DisplayXRTransparentOverlay.cs.

        static Result CheckInputHandling(DisplayXRValidationContext c)
        {
#if ENABLE_INPUT_SYSTEM
            return Result.Pass();
#else
            return Result.Fail(
                "Active Input Handling is Input Manager (Old). Choose Input System Package (New) or " +
                "Both under Player Settings > Other Settings (Unity restarts the editor) if the app " +
                "reads DisplayXR controllers or a stylus through the Input System, or expects " +
                "Mouse.current to work over a transparent overlay.");
#endif
        }

        // ==== xr-hands-version ==================================================
        // Source: Runtime/Hands/com.displayxr.unity.hands.asmdef (versionDefines 1.3.0).

        static Result CheckXrHandsVersion(DisplayXRValidationContext c)
        {
            string version = DisplayXRProjectValidation.InstalledPackageVersion("com.unity.xr.hands");
            if (version == null)
                return Result.Pass("com.unity.xr.hands is not installed - hand tracking is left out. " +
                                   "Install 1.3.0 or newer to get it.");
            return DisplayXRVersionRules.AtLeast(version, 1, 3, 0)
                ? Result.Pass("com.unity.xr.hands " + version + ".")
                : Result.Fail("com.unity.xr.hands " + version + " is installed, but hand tracking " +
                              "needs 1.3.0 or newer - it is not compiled in.");
        }

        // ==== srp-version =======================================================
        // Source: the versionDefines (17.0.0) of Runtime/URP, Editor/URP and Runtime/HDRP.

        static Result CheckSrpVersion(DisplayXRValidationContext c)
        {
            string urp = DisplayXRProjectValidation.InstalledPackageVersion("com.unity.render-pipelines.universal");
            string hdrp = DisplayXRProjectValidation.InstalledPackageVersion("com.unity.render-pipelines.high-definition");
            if (urp == null && hdrp == null)
                return Result.NotApplicable("Neither URP nor HDRP is installed.");

            var notes = new List<string>();
            if (urp != null && !DisplayXRVersionRules.AtLeast(urp, 17, 0, 0))
                notes.Add("URP " + urp + ": the foreground clip shader, DisplayXR > Setup URP " +
                          "Foreground Clip and the depth-aware cursor's URP depth pass are not compiled in");
            if (hdrp != null && !DisplayXRVersionRules.AtLeast(hdrp, 17, 0, 0))
                notes.Add("HDRP " + hdrp + ": the depth-aware cursor's HDRP depth pass is not compiled in");
            return notes.Count == 0
                ? Result.Pass()
                : Result.Fail(string.Join("; ", notes.ToArray()) + ". Stereo rendering itself is unaffected.");
        }
    }
}
