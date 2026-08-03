// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

#if UNITY_EDITOR_WIN
using Microsoft.Win32;
#endif
using UnityEditor;
using UnityEditor.Build;
using UnityEditor.Build.Reporting;
using UnityEngine;

namespace DisplayXR.Editor
{
    /// <summary>
    /// Post-build processor that writes a per-app GPU preference for the built
    /// Windows Standalone Player, honouring the Target GPU setting (#242).
    ///
    /// <para>This is the only lever that can move <i>Unity's</i> side: Unity's
    /// graphics device is created during process startup, before any app script
    /// runs, so the choice has to be in place before launch. The matching
    /// runtime-side steer happens at load time in
    /// <see cref="DisplayXRGpuPreference"/> — both must agree or the eye bridge
    /// goes cross-adapter and presents black (#240).</para>
    ///
    /// <para>On hybrid Optimus laptops, third-party Unity builds default to the
    /// integrated GPU. The DisplayXR runtime requires the client app and the
    /// service compositor to share an adapter; otherwise xrCreateSession fails
    /// with XR_ERROR_GRAPHICS_DEVICE_INVALID. Pinning the built exe via
    /// UserGpuPreferences avoids that. Mirrors the runtime-side fix in
    /// displayxr-runtime commit 5d2eee70b "Shell: pin launched apps to dGPU", so
    /// Unity apps work even when launched outside a workspace controller.</para>
    /// </summary>
    public class DisplayXRGpuPreferenceBuildProcessor : IPostprocessBuildWithReport
    {
        public int callbackOrder => 200;

        public void OnPostprocessBuild(BuildReport report)
        {
#if UNITY_EDITOR_WIN
            var target = report.summary.platform;
            if (target != BuildTarget.StandaloneWindows64 && target != BuildTarget.StandaloneWindows)
                return;

            string exePath = report.summary.outputPath;
            if (string.IsNullOrEmpty(exePath))
                return;

            // Windows matches these entries against the exe path in its OWN form —
            // BACKSLASH-separated. Unity hands us outputPath with forward slashes, and
            // an entry written that way is silently ignored: the value is visibly
            // present in the registry, and has no effect whatsoever. (Verified on
            // hardware — a forward-slash "GpuPreference=1;" left Unity on the dGPU;
            // the identical entry with backslashes moved it to the iGPU.) Normalise.
            string forwardSlashPath = exePath.Replace('\\', '/');
            exePath = exePath.Replace('/', '\\');

            // Default to Discrete when no settings asset exists — the pre-#242
            // behaviour, so existing projects rebuild identically.
            var settings = DisplayXRManifestSettings.Find();
            var choice = settings != null
                ? settings.targetGpu
                : DisplayXRGpuPreference.TargetGpu.Discrete;

            const string subKey = @"Software\Microsoft\DirectX\UserGpuPreferences";
            // 1 = power saving (integrated), 2 = high performance (discrete).
            string value = choice == DisplayXRGpuPreference.TargetGpu.Integrated ? "GpuPreference=1;"
                         : choice == DisplayXRGpuPreference.TargetGpu.Discrete   ? "GpuPreference=2;"
                         : null; // Auto → no per-exe entry; Windows decides

            try
            {
                using (var key = Registry.CurrentUser.CreateSubKey(subKey))
                {
                    if (key == null)
                    {
                        Debug.LogWarning($"DisplayXR: Could not open {subKey} to set the GPU preference for {exePath}.");
                        return;
                    }

                    // Drop the ineffective forward-slash entry that builds before this
                    // fix wrote, so the registry doesn't keep a misleading no-op around.
                    if (forwardSlashPath != exePath && key.GetValue(forwardSlashPath) != null)
                        key.DeleteValue(forwardSlashPath, false);

                    if (value == null)
                    {
                        // Auto must actually mean "Windows decides" — clear a preference
                        // an earlier build of this exe left behind, or the stale pin wins.
                        if (key.GetValue(exePath) != null)
                        {
                            key.DeleteValue(exePath, false);
                            Debug.Log($"DisplayXR: Target GPU = Auto — removed the per-app GPU preference for {exePath}.");
                        }
                        return;
                    }

                    key.SetValue(exePath, value, RegistryValueKind.String);
                }
                Debug.Log($"DisplayXR: Target GPU = {choice} — pinned {exePath} via UserGpuPreferences ({value}).");
            }
            catch (System.Exception e)
            {
                Debug.LogWarning($"DisplayXR: Failed to set the GPU preference for {exePath}: {e.Message}");
            }
#endif
        }
    }
}
