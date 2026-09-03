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
    ///
    /// <para>An entry that is <i>already there</i> is left alone (#306) — someone set
    /// it deliberately, e.g. pinning a build to the iGPU to emulate an iGPU-only
    /// customer box. It is only overwritten when the project declares a non-Auto
    /// Target GPU on the manifest settings asset, which is an intentional statement
    /// about the build. <see cref="DisplayXRGpuPreference.StampRegistryOnBuild"/> (or
    /// <c>DISPLAYXR_GPU_PREF_NO_STAMP=1</c>) skips the key entirely. Every branch logs
    /// one line, so the build log always says what happened to the entry.</para>
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

            // Escape hatch for harnesses that own the key themselves (#306) — a static
            // toggle for build scripts, plus an env var for CI or a one-off build.
            const string noStampEnvVar = "DISPLAYXR_GPU_PREF_NO_STAMP";
            if (!DisplayXRGpuPreference.StampRegistryOnBuild ||
                System.Environment.GetEnvironmentVariable(noStampEnvVar) == "1")
            {
                Debug.Log($"[DisplayXR] GpuPreference: stamping disabled — left UserGpuPreferences untouched for {exePath}.");
                return;
            }

            // Default to Discrete when no settings asset exists — the pre-#242
            // behaviour, so existing projects rebuild identically.
            var settings = DisplayXRManifestSettings.Find();
            var choice = settings != null
                ? settings.targetGpu
                : DisplayXRGpuPreference.TargetGpu.Discrete;

            // An intentional project declaration wins over whatever this machine holds:
            // Target GPU set to Discrete/Integrated on the settings asset is the
            // build-time face of DisplayXRGpuPreference.Target, so it overwrites. Auto —
            // and the no-asset fallback above, which is a default rather than a
            // declaration — defer to an entry that is already there (#306).
            bool explicitTarget = settings != null &&
                                  settings.targetGpu != DisplayXRGpuPreference.TargetGpu.Auto;

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
                        Debug.LogWarning($"[DisplayXR] GpuPreference: could not open {subKey} to set the GPU preference for {exePath}.");
                        return;
                    }

                    // Drop the ineffective forward-slash entry that builds before this
                    // fix wrote, so the registry doesn't keep a misleading no-op around.
                    // (Always ours — Windows' own Graphics settings page never writes one.)
                    if (forwardSlashPath != exePath && key.GetValue(forwardSlashPath) != null)
                        key.DeleteValue(forwardSlashPath, false);

                    object raw = key.GetValue(exePath);
                    string existing = raw == null ? null : raw.ToString();

                    if (existing != null && !explicitTarget)
                    {
                        // Someone set this deliberately — a developer emulating an
                        // iGPU-only box, a harness, or Windows' Graphics settings page.
                        // Re-stamping it silently breaks that setup, and the cross-adapter
                        // safety this processor exists for (#240/#242) only bites on hybrid
                        // boxes, where a human pin aligns the adapters just as ours does.
                        Debug.Log($"[DisplayXR] GpuPreference: kept existing entry {Pretty(existing)} for {exePath} (Target={choice}).");
                        return;
                    }

                    if (value == null)
                    {
                        // Auto with no entry: "Windows decides" is exactly what an absent
                        // entry means, so there is nothing to write. (An entry that IS
                        // there was honoured above, rather than cleared as before #306.)
                        Debug.Log($"[DisplayXR] GpuPreference: wrote nothing for {exePath} (entry absent, Target=Auto — Windows decides).");
                        return;
                    }

                    key.SetValue(exePath, value, RegistryValueKind.String);

                    if (existing == null)
                        Debug.Log($"[DisplayXR] GpuPreference: wrote {Pretty(value)} for {exePath} (entry absent, Target={choice}).");
                    else if (existing != value)
                        Debug.Log($"[DisplayXR] GpuPreference: overwrote existing {Pretty(existing)} -> {Pretty(value)} for {exePath} (Target={choice} explicit).");
                    else
                        Debug.Log($"[DisplayXR] GpuPreference: existing entry already {Pretty(value)} for {exePath} (Target={choice} explicit).");
                }
            }
            catch (System.Exception e)
            {
                Debug.LogWarning($"[DisplayXR] GpuPreference: failed to set the GPU preference for {exePath}: {e.Message}");
            }
#endif
        }

#if UNITY_EDITOR_WIN
        /// <summary>Registry entries carry a trailing ';' — drop it so logs read cleanly.</summary>
        static string Pretty(string entry)
        {
            return entry.TrimEnd(';');
        }
#endif
    }
}
