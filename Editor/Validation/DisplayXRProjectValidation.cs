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

        /// <summary>
        /// Read the ignore list from disk again. The window does this on every refresh, so
        /// a change that arrived from version control (or a hand edit) is picked up
        /// without a domain reload.
        /// </summary>
        public static void ReloadIgnoreList()
        {
            s_Ignored = null;
            InvalidateCache();
        }

        /// <summary>
        /// Ignore a rule for this project, or stop ignoring it. Saved at once. If the file
        /// cannot be written (read-only under version control, for instance) nothing
        /// changes - the in-memory list is put back - a warning is logged, and the method
        /// returns false with the reason in <paramref name="error"/>.
        /// </summary>
        public static bool SetIgnored(string ruleId, bool ignored, out string error)
        {
            bool ok = DisplayXRValidationIgnoreList.TrySet(
                DisplayXRValidationIgnoreList.DefaultPath, IgnoredIds, ruleId, ignored, out error);
            if (!ok)
                Debug.LogWarning("[DisplayXR] Project validation: the ignore list was NOT changed - could not " +
                    "write " + DisplayXRValidationIgnoreList.DefaultPath + ": " + error +
                    " (if the file is read-only under version control, check it out first).");
            InvalidateCache();
            return ok;
        }

        /// <summary><see cref="SetIgnored(string,bool,out string)"/> without the reason.</summary>
        public static bool SetIgnored(string ruleId, bool ignored)
        {
            string error;
            return SetIgnored(ruleId, ignored, out error);
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
            // The window checks the ACTIVE build target; a build can be for another one
            // (BuildPipeline.BuildPlayer from a script). Name the target, so the line is
            // not read against the wrong set of settings.
            string hint = DisplayXRValidationEngine.BuildHint(
                target.ToString(), EditorUserBuildSettings.activeBuildTarget.ToString());
            foreach (var line in lines)
                Debug.LogWarning("[DisplayXR] Project validation: " + line + " " + hint);
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

        /// <summary>
        /// The DisplayXR Display loader is enabled for a build target group OTHER than
        /// <paramref name="group"/> (of the two the loader is registered for).
        /// </summary>
        public static bool IsDisplayXRLoaderEnabledElsewhere(BuildTargetGroup group)
        {
            foreach (var other in new[] { BuildTargetGroup.Standalone, BuildTargetGroup.Android })
                if (other != group && IsDisplayXRLoaderEnabled(other))
                    return true;
            return false;
        }

        /// <summary>
        /// The gate every rule that only matters to a DisplayXR build opens with: when the
        /// loader is not enabled for the context's build target, the rule has nothing to
        /// say (<paramref name="notApplicable"/>) and <c>loader-enabled</c> is the single
        /// finding. Without it, a project that uses DisplayXR on Windows only would be
        /// told to change its Android scripting backend, architectures and graphics APIs.
        /// <code>
        /// DisplayXRValidationResult off;
        /// if (DisplayXRProjectValidation.LoaderNotEnabled(c, out off)) return off;
        /// </code>
        /// </summary>
        public static bool LoaderNotEnabled(DisplayXRValidationContext context,
                                            out DisplayXRValidationResult notApplicable)
        {
            var group = context.Group();
            if (IsDisplayXRLoaderEnabled(group))
            {
                notApplicable = default(DisplayXRValidationResult);
                return false;
            }
            notApplicable = DisplayXRValidationResult.NotApplicable(
                "The DisplayXR Display loader is not enabled for " + group + ".");
            return true;
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
            var cameras = FindRigCamerasWithForegroundClip();
            return cameras.Count > 0 ? cameras[0].name : null;
        }

        /// <summary>The cameras of every rig in a loaded scene that has Foreground Only Clip ticked.</summary>
        public static List<Camera> FindRigCamerasWithForegroundClip()
        {
            var cameras = new List<Camera>();
            foreach (var rig in FindInLoadedScenes<DisplayXRCamera>())
                if (rig.foregroundOnlyClip) AddCamera(cameras, rig);
            foreach (var rig in FindInLoadedScenes<DisplayXRDisplay>())
                if (rig.foregroundOnlyClip) AddCamera(cameras, rig);
            return cameras;
        }

        static void AddCamera(List<Camera> cameras, Component rig)
        {
            var camera = rig.GetComponent<Camera>();
            if (camera != null && !cameras.Contains(camera)) cameras.Add(camera);
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
        /// Returns null when the folder cannot be found - callers must then say nothing
        /// rather than report every file in it as missing.
        /// </summary>
        public static string PackageRoot(out bool isSourceCheckout)
        {
            var info = UnityEditor.PackageManager.PackageInfo.FindForAssembly(
                typeof(DisplayXRProjectValidation).Assembly);
            if (info != null && !string.IsNullOrEmpty(info.resolvedPath))
            {
                isSourceCheckout =
                    info.source == UnityEditor.PackageManager.PackageSource.Local ||
                    info.source == UnityEditor.PackageManager.PackageSource.Embedded;
                return info.resolvedPath;
            }

            // Not a package: the folder was copied into Assets/. Find it from this
            // assembly's own .asmdef, which sits in <root>/Editor/. Such a copy is a
            // source tree as far as its binaries go.
            isSourceCheckout = true;
            string asmdef = UnityEditor.Compilation.CompilationPipeline
                .GetAssemblyDefinitionFilePathFromAssemblyName(
                    typeof(DisplayXRProjectValidation).Assembly.GetName().Name);
            if (!string.IsNullOrEmpty(asmdef))
            {
                string editorDir = Path.GetDirectoryName(Path.GetFullPath(asmdef));
                string root = editorDir != null ? Path.GetDirectoryName(editorDir) : null;
                if (root != null && File.Exists(Path.Combine(root, "package.json")))
                    return root;
            }
            return null;
        }

        /// <summary>The installed version of a package, or null when it is not in the project.</summary>
        public static string InstalledPackageVersion(string packageName)
        {
            // FindForAssetPath has been in UnityEditor.PackageManager since 2019 and takes
            // the package's virtual root; it returns null for a package that is not there.
            var info = UnityEditor.PackageManager.PackageInfo.FindForAssetPath("Packages/" + packageName);
            return info != null ? info.version : null;
        }
    }
}
