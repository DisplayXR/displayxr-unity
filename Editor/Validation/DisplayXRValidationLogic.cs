// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Project validation, the decisions (#462): given plain facts about a project, what is
// wrong with it. Each function here mirrors a rule the package's own code enforces at run
// time, and names that code, so the two can be kept in step.
//
// PURE C# - no UnityEngine / UnityEditor (see DisplayXRValidationCore.cs). The rules in
// DisplayXRBuiltInRules.cs gather the facts from Unity and call in here; the tests in
// Tests~/ProjectValidation call in with the facts written out.

using System;
using System.Collections.Generic;

namespace DisplayXR.Editor
{
    /// <summary>
    /// Graphics API list of a build target vs what the provider has a backend for.
    /// Source of truth: the platform x API table in
    /// <c>DisplayXRDisplayLoader.IsSinglePassEligible</c> ("anything else: the native
    /// GfxStart no-starts"). APIs are named as <c>GraphicsDeviceType.ToString()</c>.
    /// </summary>
    public static class DisplayXRGraphicsApiRules
    {
        public const string D3D11 = "Direct3D11";
        public const string D3D12 = "Direct3D12";
        public const string Vulkan = "Vulkan";
        public const string Metal = "Metal";

        public enum Verdict
        {
            /// <summary>The API Unity will pick is one the provider supports.</summary>
            Ok,
            /// <summary>The first API has no provider backend: no 3D.</summary>
            FirstUnsupported,
            /// <summary>
            /// Windows with Vulkan first: the shipping policy is a deliberate fall back to
            /// D3D12, with stereo (#248). Worth knowing, not a fault.
            /// </summary>
            WindowsVulkanFallsBackToD3D12,
            /// <summary>
            /// Android with Vulkan first but another API behind it: a device that refuses
            /// Vulkan would start on an API the provider has no backend for.
            /// </summary>
            AndroidHasNonVulkanFallback,
            /// <summary>Nothing to judge (unknown platform, or no list).</summary>
            Unknown,
        }

        /// <summary>The APIs the provider supports on a platform, in the order it prefers them.</summary>
        public static string[] Supported(DisplayXRValidationPlatforms platform)
        {
            switch (platform)
            {
                case DisplayXRValidationPlatforms.Windows: return new[] { D3D11, D3D12, Vulkan };
                case DisplayXRValidationPlatforms.MacOS:   return new[] { Metal };
                case DisplayXRValidationPlatforms.Linux:   return new[] { Vulkan };
                case DisplayXRValidationPlatforms.Android: return new[] { Vulkan };
                default:                                   return new string[0];
            }
        }

        public static bool IsSupported(DisplayXRValidationPlatforms platform, string api)
        {
            return Array.IndexOf(Supported(platform), api) >= 0;
        }

        /// <summary>
        /// Judge a build target's API list. Unity starts on the first entry the machine
        /// supports, so the first entry is the one that matters; the rest only matter
        /// where the docs say a fallback must not exist (Android).
        /// </summary>
        public static Verdict Classify(DisplayXRValidationPlatforms platform, IList<string> apis)
        {
            if (Supported(platform).Length == 0 || apis == null || apis.Count == 0)
                return Verdict.Unknown;
            if (!IsSupported(platform, apis[0]))
                return Verdict.FirstUnsupported;
            if (platform == DisplayXRValidationPlatforms.Windows && apis[0] == Vulkan)
                return Verdict.WindowsVulkanFallsBackToD3D12;
            if (platform == DisplayXRValidationPlatforms.Android)
                for (int i = 1; i < apis.Count; i++)
                    if (apis[i] != Vulkan)
                        return Verdict.AndroidHasNonVulkanFallback;
            return Verdict.Ok;
        }

        /// <summary>
        /// The list the Fix writes. Android: Vulkan only. Elsewhere the project's own
        /// list is kept and re-ordered so a supported API leads - the first supported one
        /// already in the list, or the platform's preferred one when there is none.
        /// </summary>
        public static List<string> Recommend(DisplayXRValidationPlatforms platform, IList<string> apis)
        {
            var supported = Supported(platform);
            var result = new List<string>();
            if (supported.Length == 0)
            {
                if (apis != null) result.AddRange(apis);
                return result;
            }
            if (platform == DisplayXRValidationPlatforms.Android)
            {
                result.Add(Vulkan);
                return result;
            }

            string lead = null;
            if (apis != null)
                foreach (var api in apis)
                    if (IsSupported(platform, api)) { lead = api; break; }
            if (lead == null) lead = supported[0];

            result.Add(lead);
            if (apis != null)
                foreach (var api in apis)
                    if (api != lead && !result.Contains(api))
                        result.Add(api);
            return result;
        }
    }

    /// <summary>
    /// The XR loader list of a build target. Source of truth:
    /// <c>XRManagerSettings.InitializeLoaderSync</c> - loaders are tried in order and the
    /// first whose <c>Initialize()</c> returns true becomes THE active loader; the rest
    /// never run.
    /// </summary>
    public static class DisplayXRLoaderRules
    {
        public const string DisplayXRLoaderType = "DisplayXR.DisplayXRDisplayLoader";
        public const string UnityOpenXRLoaderType = "UnityEngine.XR.OpenXR.OpenXRLoader";

        public enum Verdict
        {
            /// <summary>DisplayXR is not in the list.</summary>
            NotEnabled,
            /// <summary>DisplayXR is the only loader.</summary>
            Sole,
            /// <summary>Another loader is tried first; if it initializes, DisplayXR never runs.</summary>
            OtherLoaderFirst,
            /// <summary>
            /// DisplayXR is tried first, another loader follows: it takes over whenever
            /// DisplayXR declines (no runtime on the machine, an unsupported editor API).
            /// </summary>
            OtherLoaderAfter,
        }

        public static Verdict Classify(IList<string> loaderTypeNames)
        {
            int index = loaderTypeNames == null ? -1 : loaderTypeNames.IndexOf(DisplayXRLoaderType);
            if (index < 0) return Verdict.NotEnabled;
            if (loaderTypeNames.Count == 1) return Verdict.Sole;
            return index == 0 ? Verdict.OtherLoaderAfter : Verdict.OtherLoaderFirst;
        }

        /// <summary>
        /// How serious "the DisplayXR loader is not enabled for this build target" is.
        /// Never an Error: leaving a target without the loader is a legitimate choice (a
        /// Windows-only 3D app that also has an Android target), so it must not reach the
        /// build log, which reports Errors only. A note when the loader is enabled for
        /// another target - then it is clearly a choice; a Warning when it is enabled
        /// nowhere - then the package is installed and does nothing.
        /// </summary>
        public static DisplayXRValidationSeverity NotEnabledSeverity(bool enabledForAnotherTarget)
        {
            return enabledForAnotherTarget
                ? DisplayXRValidationSeverity.Info
                : DisplayXRValidationSeverity.Warning;
        }

        /// <summary>Every loader in the list that is not DisplayXR's, in order.</summary>
        public static List<string> Others(IList<string> loaderTypeNames)
        {
            var others = new List<string>();
            if (loaderTypeNames != null)
                foreach (var name in loaderTypeNames)
                    if (name != DisplayXRLoaderType)
                        others.Add(name);
            return others;
        }

        /// <summary>"UnityEngine.XR.OpenXR.OpenXRLoader" reads better as "OpenXR Loader".</summary>
        public static string FriendlyName(string loaderTypeName)
        {
            if (string.IsNullOrEmpty(loaderTypeName)) return "(missing loader)";
            if (loaderTypeName == UnityOpenXRLoaderType) return "OpenXR (Unity's OpenXR plug-in)";
            int dot = loaderTypeName.LastIndexOf('.');
            return dot >= 0 ? loaderTypeName.Substring(dot + 1) : loaderTypeName;
        }
    }

    /// <summary>
    /// The native plug-in binaries a build target needs, and how bad it is when one is
    /// missing. Source of truth: <c>Runtime/Plugins/</c>, <c>.gitignore</c> (the Linux
    /// and Android libraries are built by CI and not committed) and the install note in
    /// the README ("Pin #upm/vX.Y.Z, not #vX.Y.Z").
    /// </summary>
    public static class DisplayXRNativePluginRules
    {
        /// <summary>
        /// Paths relative to the package root. A path ending in ".bundle" is a directory.
        /// Empty for a platform the package ships nothing for.
        /// </summary>
        public static string[] RequiredFiles(DisplayXRValidationPlatforms platform)
        {
            switch (platform)
            {
                case DisplayXRValidationPlatforms.Windows:
                    return new[] { "Runtime/Plugins/Windows/x64/displayxr_unity.dll" };
                case DisplayXRValidationPlatforms.MacOS:
                    return new[] { "Runtime/Plugins/macOS/displayxr_unity.bundle" };
                case DisplayXRValidationPlatforms.Linux:
                    return new[]
                    {
                        "Runtime/Plugins/Linux/x86_64/libdisplayxr_unity.so",
                        "Runtime/Plugins/Linux/x86_64/libdisplayxr_unity_wayland.so",
                    };
                case DisplayXRValidationPlatforms.Android:
                    return new[]
                    {
                        "Runtime/Plugins/Android/arm64-v8a/libdisplayxr_unity.so",
                        "Runtime/Plugins/Android/arm64-v8a/libopenxr_loader.so",
                        "Runtime/Plugins/Android/arm64-v8a/libVkLayer_DXR_queue_lock.so",
                    };
                default:
                    return new string[0];
            }
        }

        /// <summary>The platform's libraries are produced by CI only, never committed.</summary>
        public static bool IsCiBuiltOnly(DisplayXRValidationPlatforms platform)
        {
            return platform == DisplayXRValidationPlatforms.Linux
                || platform == DisplayXRValidationPlatforms.Android;
        }

        /// <summary>
        /// A missing binary is an Error - the build has no provider - except in the one
        /// case where it is expected: a source checkout of the package (a "file:" /
        /// embedded install) targeting Linux or Android, whose libraries only CI builds.
        /// </summary>
        public static DisplayXRValidationSeverity MissingSeverity(
            DisplayXRValidationPlatforms platform, bool packageIsSourceCheckout)
        {
            return packageIsSourceCheckout && IsCiBuiltOnly(platform)
                ? DisplayXRValidationSeverity.Info
                : DisplayXRValidationSeverity.Error;
        }
    }

    /// <summary>
    /// Android player settings. Source of truth: the package ships
    /// <c>Runtime/Plugins/Android/arm64-v8a/</c> and nothing else, and
    /// <c>native~/build-android.sh</c> builds against API level 29.
    /// </summary>
    public static class DisplayXRAndroidRules
    {
        // UnityEditor.AndroidArchitecture bit values.
        public const uint ArchARMv7 = 1u << 0;
        public const uint ArchARM64 = 1u << 1;

        public const int MinApiLevel = 29;

        public enum ArchVerdict
        {
            Arm64Only,
            /// <summary>ARM64 plus another ABI: on that ABI the app has no provider.</summary>
            Arm64AndOthers,
            /// <summary>No ARM64 at all: no device gets the provider.</summary>
            NoArm64,
        }

        public static ArchVerdict ClassifyArchitectures(uint androidArchitectureMask)
        {
            if ((androidArchitectureMask & ArchARM64) == 0) return ArchVerdict.NoArm64;
            return androidArchitectureMask == ArchARM64 ? ArchVerdict.Arm64Only : ArchVerdict.Arm64AndOthers;
        }
    }

    /// <summary>
    /// How a <c>GameObject/...</c> menu item treats a multi-selection. From the Hierarchy's
    /// right-click menu Unity invokes the item once PER selected object, each time with
    /// that object as the command's context; from the main menu bar it invokes it once,
    /// with no context. An item that must act once over the whole selection therefore
    /// has to drop all but one of those invocations.
    /// </summary>
    public static class DisplayXRMenuRules
    {
        /// <summary>
        /// Whether this invocation is the one that does the work.
        /// <paramref name="contextIndexInSelection"/> is the context object's index in
        /// the selection, or -1 when it is not part of it.
        /// </summary>
        public static bool ActsOnThisInvocation(bool hasContext, int contextIndexInSelection)
        {
            // No context: the single main-menu invocation. Context outside the selection:
            // an invocation for that one object alone. Otherwise only the first of the N.
            return !hasContext || contextIndexInSelection <= 0;
        }

        /// <summary>
        /// Whether the invocation covers the whole selection (true) or only the context
        /// object (false - the context is not part of the selection).
        /// </summary>
        public static bool CoversWholeSelection(bool hasContext, int contextIndexInSelection)
        {
            return !hasContext || contextIndexInSelection >= 0;
        }
    }

    /// <summary>Package versions, as much of semver as <c>versionDefines</c> compares.</summary>
    public static class DisplayXRVersionRules
    {
        /// <summary>
        /// True when <paramref name="version"/> is at least major.minor.patch. A
        /// pre-release tag ("17.0.0-pre.2") is ignored, the way Unity's versionDefines
        /// treat it when the expression is a bare version. Unparseable = false.
        /// </summary>
        public static bool AtLeast(string version, int major, int minor, int patch)
        {
            int a, b, c;
            if (!TryParse(version, out a, out b, out c)) return false;
            if (a != major) return a > major;
            if (b != minor) return b > minor;
            return c >= patch;
        }

        public static bool TryParse(string version, out int major, out int minor, out int patch)
        {
            major = minor = patch = 0;
            if (string.IsNullOrEmpty(version)) return false;
            int cut = version.IndexOfAny(new[] { '-', '+' });
            string core = cut >= 0 ? version.Substring(0, cut) : version;
            string[] parts = core.Split('.');
            if (parts.Length < 1 || parts.Length > 3) return false;
            if (!int.TryParse(parts[0], out major) || major < 0) return false;
            if (parts.Length > 1 && (!int.TryParse(parts[1], out minor) || minor < 0)) return false;
            if (parts.Length > 2 && (!int.TryParse(parts[2], out patch) || patch < 0)) return false;
            return true;
        }
    }
}
