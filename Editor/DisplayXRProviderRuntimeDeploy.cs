// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

using System;
using System.IO;
using UnityEditor;
using UnityEditor.Build;
using UnityEditor.Build.Reporting;
using UnityEngine;

namespace DisplayXR.Editor
{
    /// <summary>
    /// Post-build step that deploys the DisplayXR native plugin and its XR
    /// subsystem manifest into the built player.
    ///
    /// Unity's player build does NOT reliably auto-include this package's native
    /// library (<c>displayxr_unity</c>) or its <c>UnitySubsystemsManifest.json</c>:
    /// unlike a first-party XR package (e.g. com.unity.xr.openxr, whose manifest +
    /// native loader the OpenXR build processor copies), our custom
    /// <c>IUnityXRDisplay</c> provider package is not recognized by Unity's XR
    /// build pipeline, so a clean consumer build ships without the native provider
    /// and the app fails to weave (#166). Throughout development this was masked by
    /// hand-copying the DLL + manifest after every build.
    ///
    /// This processor closes that gap the same way <see cref="DisplayXRBuildProcessor"/>
    /// deploys the macOS OpenXR loader: copy the package's shipped native binary and
    /// subsystem manifest into the player's data folder if Unity didn't. It runs for
    /// both the provider and the legacy hook path (both need the native library); the
    /// manifest is inert when no DisplayXR display loader is active, so deploying it
    /// unconditionally is safe.
    /// </summary>
    public class DisplayXRProviderRuntimeDeploy : IPostprocessBuildWithReport
    {
        // After DisplayXRBuildProcessor (100) is not required; run early so the
        // native bits are in place regardless of other processors.
        public int callbackOrder => 60;

        const string PackageRoot = "Packages/com.displayxr.unity";
        // Subsystem-manifest folder name in the player = the manifest "name" field.
        const string SubsystemName = "DisplayXR";

        public void OnPostprocessBuild(BuildReport report)
        {
            var platform = report.summary.platform;
            string pkgRoot;
            try { pkgRoot = Path.GetFullPath(PackageRoot); }
            catch { pkgRoot = null; }

            if (string.IsNullOrEmpty(pkgRoot) || !Directory.Exists(pkgRoot))
            {
                // Dev checkout where the package isn't under Packages/ (rare). The
                // file:/embedded dev flows already manage the native bits manually.
                Debug.LogWarning("[DisplayXR] Provider deploy: package not found under " +
                                 PackageRoot + " — skipping native/manifest deploy.");
                return;
            }

            string manifestSrc = Path.Combine(pkgRoot, "Runtime", "UnitySubsystemsManifest.json");
            string outputPath = report.summary.outputPath;

            if (platform == BuildTarget.StandaloneWindows64 || platform == BuildTarget.StandaloneWindows)
            {
                string exeDir = Path.GetDirectoryName(outputPath);
                string exeName = Path.GetFileNameWithoutExtension(outputPath);
                string dataDir = Path.Combine(exeDir, exeName + "_Data");
                string dllSrc = Path.Combine(pkgRoot, "Runtime", "Plugins", "Windows", "x64", "displayxr_unity.dll");

                CopyFileInto(dllSrc, Path.Combine(dataDir, "Plugins", "x86_64", "displayxr_unity.dll"));
                CopyFileInto(manifestSrc, Path.Combine(dataDir, "UnitySubsystems", SubsystemName, "UnitySubsystemsManifest.json"));
            }
            else if (platform == BuildTarget.StandaloneOSX)
            {
                // .app: native plugin lives in Contents/PlugIns, engine data in
                // Contents/Resources/Data (mirrors CopyMacOSOpenXRLoader).
                string bundleSrc = Path.Combine(pkgRoot, "Runtime", "Plugins", "macOS", "displayxr_unity.bundle");
                CopyBundleInto(bundleSrc, Path.Combine(outputPath, "Contents", "PlugIns", "displayxr_unity.bundle"));
                CopyFileInto(manifestSrc, Path.Combine(outputPath, "Contents", "Resources", "Data",
                    "UnitySubsystems", SubsystemName, "UnitySubsystemsManifest.json"));
            }
            else if (platform == BuildTarget.StandaloneLinux64)
            {
                // Linux player layout mirrors Windows (#249): <name>_Data/Plugins/x86_64/
                // for the native .so, <name>_Data/UnitySubsystems/<id>/ for the manifest.
                // Without the manifest the display subsystem is never discovered and the
                // loader fails with "Failed to create the 'DisplayXR Display' subsystem".
                string exeDir = Path.GetDirectoryName(outputPath);
                string exeName = Path.GetFileNameWithoutExtension(outputPath);
                string dataDir = Path.Combine(exeDir, exeName + "_Data");
                string soSrc = Path.Combine(pkgRoot, "Runtime", "Plugins", "Linux", "x86_64", "libdisplayxr_unity.so");

                CopyFileInto(soSrc, Path.Combine(dataDir, "Plugins", "x86_64", "libdisplayxr_unity.so"));
                CopyFileInto(manifestSrc, Path.Combine(dataDir, "UnitySubsystems", SubsystemName, "UnitySubsystemsManifest.json"));
            }
            // Other standalone targets are not shipped for the provider today.
        }

        static void CopyFileInto(string src, string dst)
        {
            if (!File.Exists(src))
            {
                Debug.LogWarning($"[DisplayXR] Provider deploy: source missing, not copied: {src}");
                return;
            }
            try
            {
                Directory.CreateDirectory(Path.GetDirectoryName(dst));
                File.Copy(src, dst, overwrite: true);
                Debug.Log($"[DisplayXR] Provider deploy: {Path.GetFileName(dst)} -> {dst}");
            }
            catch (Exception e)
            {
                Debug.LogWarning($"[DisplayXR] Provider deploy: failed to copy {src} -> {dst}: {e.Message}");
            }
        }

        static void CopyBundleInto(string srcDir, string dstDir)
        {
            if (!Directory.Exists(srcDir))
            {
                Debug.LogWarning($"[DisplayXR] Provider deploy: macOS bundle missing, not copied: {srcDir}");
                return;
            }
            try
            {
                CopyDirRecursive(srcDir, dstDir);
                Debug.Log($"[DisplayXR] Provider deploy: displayxr_unity.bundle -> {dstDir}");
            }
            catch (Exception e)
            {
                Debug.LogWarning($"[DisplayXR] Provider deploy: failed to copy bundle {srcDir} -> {dstDir}: {e.Message}");
            }
        }

        static void CopyDirRecursive(string src, string dst)
        {
            Directory.CreateDirectory(dst);
            foreach (string file in Directory.GetFiles(src))
                File.Copy(file, Path.Combine(dst, Path.GetFileName(file)), overwrite: true);
            foreach (string dir in Directory.GetDirectories(src))
                CopyDirRecursive(dir, Path.Combine(dst, Path.GetFileName(dir)));
        }
    }
}
