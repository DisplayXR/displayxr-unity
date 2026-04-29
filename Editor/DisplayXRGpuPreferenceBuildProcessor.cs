// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0

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
    /// Windows Standalone Player so it launches on the discrete GPU.
    ///
    /// On hybrid Optimus laptops, third-party Unity builds default to the
    /// integrated GPU. The DisplayXR runtime requires the client app and the
    /// service compositor to share an adapter; otherwise xrCreateSession fails
    /// with XR_ERROR_GRAPHICS_DEVICE_INVALID. Pinning the built exe to the
    /// high-performance GPU via UserGpuPreferences avoids that.
    ///
    /// Mirrors the runtime-side fix in displayxr-runtime-pvt commit 5d2eee70b
    /// "Shell: pin launched apps to dGPU". This applies the same pin at build
    /// time so Unity apps work correctly even when launched outside the shell.
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

            const string subKey = @"Software\Microsoft\DirectX\UserGpuPreferences";
            const string value = "GpuPreference=2;";

            try
            {
                using (var key = Registry.CurrentUser.CreateSubKey(subKey))
                {
                    if (key == null)
                    {
                        Debug.LogWarning($"DisplayXR: Could not open {subKey} to pin {exePath} to dGPU.");
                        return;
                    }
                    key.SetValue(exePath, value, RegistryValueKind.String);
                }
                Debug.Log($"DisplayXR: Pinned {exePath} to high-performance GPU (UserGpuPreferences).");
            }
            catch (System.Exception e)
            {
                Debug.LogWarning($"DisplayXR: Failed to pin {exePath} to dGPU: {e.Message}");
            }
#endif
        }
    }
}
