// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
using System.IO;
using UnityEditor;
using UnityEditor.Build;
using UnityEditor.XR.Management;
using UnityEngine;
using UnityEngine.Rendering;
using UnityEngine.XR.Management;

namespace DisplayXR.Editor
{
    /// <summary>
    /// Project validation, the Unity side (#462): builds the context a rule is checked
    /// against, evaluates the registry, owns the project's ignore list, and is what the
    /// window, the settings page and the build hook all call. It also holds the small
    /// project queries several rules share, so the URP sub-assembly's rules ask the same
    /// questions the same way.
    /// <para>
    /// Nothing here changes a project setting. Only a rule's <c>Fix</c> does, and only the
    /// window calls one, from a click.
    /// </para>
    /// </summary>
    public static class DisplayXRProjectValidation
    {
        /// <summary>Where every rule is documented; a rule's anchor is its id.</summary>
        public const string DocUrl =
            "https://github.com/DisplayXR/displayxr-unity/blob/main/docs~/project-validation.md";

        /// <summary>The documentation link of a rule the package ships.</summary>
        public static string DocUrlFor(string ruleId) { return DocUrl + "#" + ruleId; }

        // ---- Context -----------------------------------------------------------

        /// <summary>A context for the editor's active build target.</summary>
        public static DisplayXRValidationContext ActiveContext()
        {
            return CreateContext(EditorUserBuildSettings.activeBuildTarget, isBuild: false);
        }

        /// <summary>A context for any build target (the build hook passes the build's own).</summary>
        public static DisplayXRValidationContext CreateContext(BuildTarget target, bool isBuild)
        {
            return new DisplayXRValidationContext
            {
                Platform = PlatformOf(target),
                BuildTargetId = (int)target,
                BuildTargetGroupId = (int)BuildPipeline.GetBuildTargetGroup(target),
                IsBuild = isBuild,
            };
        }

        public static DisplayXRValidationPlatforms PlatformOf(BuildTarget target)
        {
            switch (target)
            {
                case BuildTarget.StandaloneWindows:
                case BuildTarget.StandaloneWindows64: return DisplayXRValidationPlatforms.Windows;
                case BuildTarget.StandaloneOSX:       return DisplayXRValidationPlatforms.MacOS;
                case BuildTarget.StandaloneLinux64:   return DisplayXRValidationPlatforms.Linux;
                case BuildTarget.Android:             return DisplayXRValidationPlatforms.Android;
                default:                                          return DisplayXRValidationPlatforms.Other;
            }
        }

        /// <summary>The context's build target as Unity's enum.</summary>
        public static BuildTarget Target(this DisplayXRValidationContext context)
        {
            return (BuildTarget)context.BuildTargetId;
        }

        /// <summary>The context's build target group as Unity's enum.</summary>
        public static BuildTargetGroup Group(this DisplayXRValidationContext context)
        {
            return (BuildTargetGroup)context.BuildTargetGroupId;
        }

        /// <summary>The context's build target as a <see cref="NamedBuildTarget"/>.</summary>
        public static NamedBuildTarget NamedTarget(this DisplayXRValidationContext context)
        {
            return NamedBuildTarget.FromBuildTargetGroup(context.Group());
        }

        // ---- Evaluation --------------------------------------------------------

        /// <summary>Check every registered rule for the active build target.</summary>
        public static List<DisplayXRValidationEntry> Evaluate()
        {
            return Evaluate(ActiveContext());
        }

        /// <summary>Check every registered rule against <paramref name="context"/>.</summary>
        public static List<DisplayXRValidationEntry> Evaluate(DisplayXRValidationContext context)
        {
            return DisplayXRValidationEngine.Evaluate(
                DisplayXRValidationRegistry.Rules, context, IgnoredIds);
        }

        static List<DisplayXRValidationEntry> s_Cached;
        static double s_CachedAt;
        static int s_CachedTarget;

        /// <summary>
        /// A recent evaluation for the active build target, re-run when older than
        /// <paramref name="maxAgeSeconds"/>. For UI that repaints often (the settings
        /// page): some checks search the asset database, which is too much per repaint.
        /// </summary>
        public static List<DisplayXRValidationEntry> EvaluateCached(double maxAgeSeconds = 2.0)
        {
            int target = (int)EditorUserBuildSettings.activeBuildTarget;
            double now = EditorApplication.timeSinceStartup;
            if (s_Cached == null || s_CachedTarget != target || now - s_CachedAt > maxAgeSeconds)
            {
                s_Cached = Evaluate();
                s_CachedAt = now;
                s_CachedTarget = target;
            }
            return s_Cached;
        }

        /// <summary>Drop the cached evaluation (after a fix or an ignore change).</summary>
        public static void InvalidateCache() { s_Cached = null; }

        // ---- Ignore list -------------------------------------------------------

        static HashSet<string> s_Ignored;

        /// <summary>
        /// The ids this project ignores. Stored in
        /// <c>ProjectSettings/DisplayXRProjectValidation.json</c> - project state that
        /// belongs in version control, so never EditorPrefs.
        /// </summary>
        public static ICollection<string> IgnoredIds
        {
            get
            {
                if (s_Ignored == null)
                    s_Ignored = DisplayXRValidationIgnoreList.Load(DisplayXRValidationIgnoreList.DefaultPath);
                return s_Ignored;
            }
        }

        public static bool IsIgnored(string ruleId) { return IgnoredIds.Contains(ruleId); }

        /// <summary>Ignore a rule for this project, or stop ignoring it. Saved at once.</summary>
        public static void SetIgnored(string ruleId, bool ignored)
        {
            var ids = (HashSet<string>)IgnoredIds;
            bool changed = ignored ? ids.Add(ruleId) : ids.Remove(ruleId);
            if (!changed) return;
            try
            {
                DisplayXRValidationIgnoreList.Save(DisplayXRValidationIgnoreList.DefaultPath, ids);
            }
            catch (Exception e)
            {
                Debug.LogWarning("[DisplayXR] Project validation: could not write " +
                    DisplayXRValidationIgnoreList.DefaultPath + ": " + e.Message);
            }
            InvalidateCache();
        }

        // ---- Build hook --------------------------------------------------------

        /// <summary>
        /// Called by <see cref="DisplayXRBuildProcessor"/> before a build: logs each open
        /// Error-severity finding for the build's target as a WARNING. It never fails the
        /// build and opens no window or dialog, so it is safe in batch mode. A rule the
        /// project ignores is not logged. Returns the number of lines logged.
        /// </summary>
        public static int LogBuildIssues(BuildTarget target)
        {
            List<string> lines;
            try
            {
                var context = CreateContext(target, isBuild: true);
                if (context.Platform == DisplayXRValidationPlatforms.Other)
                    return 0; // a target the provider does not exist for: nothing to say
                lines = DisplayXRValidationEngine.BuildMessages(Evaluate(context));
            }
            catch (Exception e)
            {
                // A validator must never be the reason a build log fills with exceptions.
                Debug.LogWarning("[DisplayXR] Project validation could not run: " + e.Message);
                return 0;
            }
            foreach (var line in lines)
                Debug.LogWarning("[DisplayXR] Project validation: " + line +
                    " (DisplayXR > Project Validation has the fix; ignore the rule there to silence this.)");
            return lines.Count;
        }

        // ---- Project queries shared by rules -----------------------------------

        /// <summary>
        /// The XR Plug-in Management settings of a build target group, when the project
        /// has any. False means XR Plug-in Management has never been set up for it.
        /// </summary>
        public static bool TryGetXRSettings(BuildTargetGroup group,
            out XRGeneralSettings general, out XRManagerSettings manager)
        {
            general = XRGeneralSettingsPerBuildTarget.XRGeneralSettingsForBuildTarget(group);
            manager = general != null ? general.Manager : null;
            return manager != null;
        }

        /// <summary>The full type names of a build target group's active loaders, in order.</summary>
        public static List<string> LoaderTypeNames(BuildTargetGroup group)
        {
            var names = new List<string>();
            XRGeneralSettings general; XRManagerSettings manager;
            if (!TryGetXRSettings(group, out general, out manager)) return names;
            foreach (var loader in manager.activeLoaders)
                if (loader != null)
                    names.Add(loader.GetType().FullName);
            return names;
        }

        /// <summary>The DisplayXR Display loader is enabled for this build target group.</summary>
        public static bool IsDisplayXRLoaderEnabled(BuildTargetGroup group)
        {
            return LoaderTypeNames(group).Contains(typeof(DisplayXRDisplayLoader).FullName);
        }

        /// <summary>Open Project Settings > XR Plug-in Management.</summary>
        public static void OpenXRPluginManagement()
        {
            SettingsService.OpenProjectSettings("Project/XR Plug-in Management");
        }

        /// <summary>
        /// The active render pipeline is URP. Decided the way
        /// <c>DisplayXRDisplayLoader.IsSinglePassEligible</c> decides it - by the pipeline
        /// asset's type name - so it needs no reference to URP.
        /// </summary>
        public static bool IsUrpActive()
        {
            var rp = GraphicsSettings.currentRenderPipeline;
            return rp != null && rp.GetType().FullName.Contains("Universal");
        }

        /// <summary>
        /// Whether the project is a transparent-overlay app, as far as the editor can
        /// tell without opening scenes: the manifest settings say so, or a loaded scene
        /// holds a <see cref="DisplayXRTransparentOverlay"/>. An app that only calls
        /// <c>RequestTransparentSession()</c> from code in a scene that is not open is not
        /// detected - a miss, never a false alarm.
        /// </summary>
        public static bool UsesTransparentOverlay(out string evidence)
        {
            var manifest = DisplayXRManifestSettings.Find();
            if (manifest != null && manifest.transparentOverlay)
            {
                evidence = "Transparent Overlay App is ticked in the DisplayXR Manifest Settings";
                return true;
            }
            var overlays = FindInLoadedScenes<DisplayXRTransparentOverlay>();
            if (overlays.Length > 0)
            {
                evidence = "'" + overlays[0].name + "' has a DisplayXR Transparent Overlay component";
                return true;
            }
            evidence = null;
            return false;
        }

        /// <summary>
        /// A rig in a loaded scene has Foreground Only Clip ticked. Null when none has;
        /// otherwise the GameObject's name.
        /// </summary>
        public static string FindRigWithForegroundClip()
        {
            foreach (var rig in FindInLoadedScenes<DisplayXRCamera>())
                if (rig.foregroundOnlyClip) return rig.name;
            foreach (var rig in FindInLoadedScenes<DisplayXRDisplay>())
                if (rig.foregroundOnlyClip) return rig.name;
            return null;
        }

        /// <summary>
        /// Components of a type in the scenes that are open, inactive objects included;
        /// assets (prefabs in the Project window) and hidden editor objects left out.
        /// </summary>
        public static T[] FindInLoadedScenes<T>() where T : Component
        {
            // Resources.FindObjectsOfTypeAll rather than Object.FindObjectsByType: the
            // latter's signature differs between Unity 2022.3 and Unity 6.4 (the sort-mode
            // overload is obsolete there), and this one is the same everywhere.
            var found = new List<T>();
            foreach (var component in Resources.FindObjectsOfTypeAll<T>())
            {
                if (component == null || EditorUtility.IsPersistent(component)) continue;
                if ((component.hideFlags & (HideFlags.HideAndDontSave | HideFlags.NotEditable)) != 0) continue;
                if (!component.gameObject.scene.IsValid()) continue;
                found.Add(component);
            }
            return found.ToArray();
        }

        /// <summary>
        /// The package's folder on disk and whether it is a source checkout (a "file:" or
        /// embedded install) as opposed to a published copy (git URL, tarball, registry).
        /// </summary>
        public static string PackageRoot(out bool isSourceCheckout)
        {
            var info = UnityEditor.PackageManager.PackageInfo.FindForAssembly(
                typeof(DisplayXRProjectValidation).Assembly);
            if (info != null)
            {
                isSourceCheckout =
                    info.source == UnityEditor.PackageManager.PackageSource.Local ||
                    info.source == UnityEditor.PackageManager.PackageSource.Embedded;
                return info.resolvedPath;
            }
            // Not resolved as a package (should not happen): same fallback the build
            // processors use.
            isSourceCheckout = false;
            return Path.GetFullPath("Packages/com.displayxr.unity");
        }

        /// <summary>The installed version of a package, or null when it is not in the project.</summary>
        public static string InstalledPackageVersion(string packageName)
        {
            foreach (var info in UnityEditor.PackageManager.PackageInfo.GetAllRegisteredPackages())
                if (info.name == packageName)
                    return info.version;
            return null;
        }
    }
}
