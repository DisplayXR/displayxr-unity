// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

using System;
using System.IO;
using System.Text;
using UnityEditor;
using UnityEditor.Build;
using UnityEditor.Build.Reporting;
using UnityEngine;

namespace DisplayXR.Editor
{
    /// <summary>
    /// Post-build processor that generates a .displayxr.json sidecar file
    /// next to the built executable. Any DisplayXR-compatible workspace
    /// controller (the DisplayXR Shell is the reference) scans for these
    /// files to discover installed apps. When the user opts in via
    /// DisplayXRManifestSettings.registerWithDisplayXR, also writes a
    /// registered-mode manifest to %LOCALAPPDATA%\DisplayXR\apps\ so any
    /// compliant workspace controller can find this build without it
    /// living under Program Files.
    /// </summary>
    public class DisplayXRBuildProcessor : IPostprocessBuildWithReport
    {
        public int callbackOrder => 100;

        public void OnPostprocessBuild(BuildReport report)
        {
            // Bundle the OpenXR loader into macOS standalone builds. Unity's own
            // OpenXRBuildProcessor only handles Windows + Android; on macOS the
            // .app ships without a loader and OpenXR session init fails with
            // "Failed to load openxr runtime loader". See issue #71.
            if (report.summary.platform == BuildTarget.StandaloneOSX)
            {
                CopyMacOSOpenXRLoader(report.summary.outputPath);
            }

            string exePath = report.summary.outputPath;
            string exeDir = Path.GetDirectoryName(exePath);
            string exeName = Path.GetFileNameWithoutExtension(exePath);
            string sidecarPath = Path.Combine(exeDir, exeName + ".displayxr.json");

            var settings = DisplayXRManifestSettings.Find();

            // Resolve values — use settings if available, otherwise defaults
            string appName = settings != null ? settings.EffectiveName : Application.productName;
            string category = settings != null ? settings.category.ToString() : "app";
            string displayMode = settings != null ? settings.displayMode : "auto";
            string description = settings != null ? settings.description : "";
            string layout = settings != null
                ? DisplayXRManifestSettings.LayoutToString(settings.icon3DLayout)
                : "sbs-lr";

            WarnIfIconsDiverge(settings);

            // Export icons next to the exe (sidecar mode); record relative names
            // for the JSON. Spec §2.3: paths in the manifest are resolved relative
            // to the manifest file.
            string sidecarIconRel = null;
            string sidecarIcon3DRel = null;
            if (settings != null && settings.icon != null)
            {
                if (ExportTexture(settings.icon, Path.Combine(exeDir, "icon.png")))
                    sidecarIconRel = "icon.png";
            }
            if (settings != null && settings.icon3D != null)
            {
                if (ExportTexture(settings.icon3D, Path.Combine(exeDir, "icon_sbs.png")))
                    sidecarIcon3DRel = "icon_sbs.png";
            }

            string sidecarJson = BuildManifestJson(
                appName, category, displayMode, description,
                exePath: null, sidecarIconRel, sidecarIcon3DRel, layout);
            File.WriteAllText(sidecarPath, sidecarJson, Encoding.UTF8);
            Debug.Log($"DisplayXR: Generated app manifest at {sidecarPath}");

            if (settings != null && settings.registerWithDisplayXR)
            {
                WriteRegisteredManifest(exePath, settings,
                    appName, category, displayMode, description, layout);
            }
        }

        /// <summary>
        /// Copy the bundled OpenXR loader dylib into the macOS .app's PlugIns
        /// directory so OpenXR session init can dlopen it. The loader lives in
        /// the package at RuntimeLoaders~/macos/openxr_loader.dylib (the ~ suffix
        /// keeps Unity's asset pipeline from importing it). Unity's own
        /// OpenXRBuildProcessor only auto-deploys loaders on Windows + Android,
        /// not macOS, so we ship our own.
        /// </summary>
        private static void CopyMacOSOpenXRLoader(string outputPath)
        {
            string srcPath = Path.GetFullPath("Packages/com.displayxr.unity/RuntimeLoaders~/macos/openxr_loader.dylib");
            if (!File.Exists(srcPath))
            {
                Debug.LogWarning($"DisplayXR: OpenXR loader not found at {srcPath} — built .app will fail to init OpenXR.");
                return;
            }

            string dstPath = Path.Combine(outputPath, "Contents/PlugIns/openxr_loader.dylib");
            try
            {
                Directory.CreateDirectory(Path.GetDirectoryName(dstPath));
                File.Copy(srcPath, dstPath, overwrite: true);
                Debug.Log($"DisplayXR: Bundled OpenXR loader at {dstPath}");
            }
            catch (Exception e)
            {
                Debug.LogWarning($"DisplayXR: Failed to copy OpenXR loader to {dstPath}: {e.Message}");
            }
        }

        /// <summary>
        /// Build the .displayxr.json body. Pass exePath != null for registered mode
        /// (per spec §2.2 / §3.2). Icon names are written verbatim — callers are
        /// responsible for placing the actual files where the manifest expects them.
        /// </summary>
        private static string BuildManifestJson(
            string appName, string category, string displayMode, string description,
            string exePath, string iconRelName, string icon3DRelName, string icon3DLayout)
        {
            var sb = new StringBuilder();
            sb.AppendLine("{");
            sb.AppendLine("  \"schema_version\": 1,");
            sb.AppendLine($"  \"name\": {JsonEscape(appName)},");
            sb.Append($"  \"type\": \"3d\"");

            if (!string.IsNullOrEmpty(exePath))
            {
                sb.AppendLine(",");
                sb.Append($"  \"exe_path\": {JsonEscape(exePath)}");
            }

            // Optional fields — only include non-default values
            if (category != "app")
            {
                sb.AppendLine(",");
                sb.Append($"  \"category\": {JsonEscape(category)}");
            }
            if (displayMode != "auto")
            {
                sb.AppendLine(",");
                sb.Append($"  \"display_mode\": {JsonEscape(displayMode)}");
            }
            if (!string.IsNullOrEmpty(description))
            {
                string truncated = description.Length > 256 ? description.Substring(0, 256) : description;
                sb.AppendLine(",");
                sb.Append($"  \"description\": {JsonEscape(truncated)}");
            }
            if (!string.IsNullOrEmpty(iconRelName))
            {
                sb.AppendLine(",");
                sb.Append($"  \"icon\": {JsonEscape(iconRelName)}");
            }
            if (!string.IsNullOrEmpty(icon3DRelName))
            {
                sb.AppendLine(",");
                sb.Append($"  \"icon_3d\": {JsonEscape(icon3DRelName)}");
                if (icon3DLayout != "sbs-lr")
                {
                    sb.AppendLine(",");
                    sb.Append($"  \"icon_3d_layout\": {JsonEscape(icon3DLayout)}");
                }
            }

            sb.AppendLine();
            sb.AppendLine("}");
            return sb.ToString();
        }

        /// <summary>
        /// Write a registered-mode manifest to %LOCALAPPDATA%\DisplayXR\apps\ so
        /// workspace controllers discover this build via exe_path. Per-app prefixed
        /// icon filenames avoid collision in the shared discovery dir (mirrors the
        /// DisplayXR Shell's Browse-for-app convention).
        /// </summary>
        private static void WriteRegisteredManifest(
            string exePath, DisplayXRManifestSettings settings,
            string appName, string category, string displayMode, string description,
            string icon3DLayout)
        {
            string localAppData = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
            if (string.IsNullOrEmpty(localAppData))
            {
                Debug.LogWarning("DisplayXR: Cannot resolve %LOCALAPPDATA% — skipping registered manifest.");
                return;
            }

            string registeredDir = Path.Combine(localAppData, "DisplayXR", "apps");
            try
            {
                Directory.CreateDirectory(registeredDir);
            }
            catch (Exception e)
            {
                Debug.LogWarning($"DisplayXR: Cannot create {registeredDir}: {e.Message} — skipping registered manifest.");
                return;
            }

            string sanitized = SanitizeBasename(appName);
            if (string.IsNullOrEmpty(sanitized))
            {
                Debug.LogWarning("DisplayXR: App name sanitized to empty — skipping registered manifest.");
                return;
            }

            string manifestPath = Path.Combine(registeredDir, sanitized + ".displayxr.json");

            string iconRel = null;
            string icon3DRel = null;
            if (settings.icon != null)
            {
                string name = sanitized + ".png";
                if (ExportTexture(settings.icon, Path.Combine(registeredDir, name)))
                    iconRel = name;
            }
            if (settings.icon3D != null)
            {
                string name = sanitized + "_sbs.png";
                if (ExportTexture(settings.icon3D, Path.Combine(registeredDir, name)))
                    icon3DRel = name;
            }

            string json = BuildManifestJson(
                appName, category, displayMode, description,
                exePath: exePath, iconRel, icon3DRel, icon3DLayout);
            File.WriteAllText(manifestPath, json, Encoding.UTF8);
            Debug.Log($"DisplayXR: Registered with DisplayXR — manifest at {manifestPath}");
        }

        /// <summary>
        /// Replace every char outside [A-Za-z0-9._-] with '_'. Matches the
        /// DisplayXR Shell's sanitize_basename so manifest filenames are
        /// interchangeable across the ecosystem.
        /// </summary>
        private static string SanitizeBasename(string src)
        {
            if (string.IsNullOrEmpty(src)) return "";
            var sb = new StringBuilder(src.Length);
            foreach (char c in src)
            {
                bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
                        || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
                sb.Append(ok ? c : '_');
            }
            return sb.ToString();
        }

        /// <summary>
        /// Warn if Player Settings → Icon and the manifest 2D icon point at
        /// different source textures. They should share a single source so the
        /// launcher tile and Windows taskbar/Start Menu icons stay in sync.
        /// </summary>
        private static void WarnIfIconsDiverge(DisplayXRManifestSettings settings)
        {
            if (settings == null || settings.icon == null) return;

            Texture2D[] playerIcons = PlayerSettings.GetIcons(
                NamedBuildTarget.Standalone, IconKind.Application);
            if (playerIcons == null) return;

            foreach (var t in playerIcons)
            {
                if (t != null && t != settings.icon)
                {
                    Debug.LogWarning(
                        "DisplayXR: Player Settings → Icon and DisplayXRManifestSettings → 2D Icon " +
                        "reference different textures. Use a single source so the launcher tile and " +
                        "Windows taskbar icon stay in sync.");
                    return;
                }
            }
        }

        /// <summary>
        /// Export a texture to PNG, handling non-readable textures via GPU readback.
        /// </summary>
        private static bool ExportTexture(Texture2D source, string destPath)
        {
            byte[] png;
            try
            {
                if (source.isReadable)
                {
                    png = source.EncodeToPNG();
                }
                else
                {
                    // GPU readback for non-readable textures
                    var rt = RenderTexture.GetTemporary(source.width, source.height, 0, RenderTextureFormat.ARGB32);
                    Graphics.Blit(source, rt);
                    var prev = RenderTexture.active;
                    RenderTexture.active = rt;
                    var readable = new Texture2D(source.width, source.height, TextureFormat.RGBA32, false);
                    readable.ReadPixels(new Rect(0, 0, source.width, source.height), 0, 0);
                    readable.Apply();
                    RenderTexture.active = prev;
                    RenderTexture.ReleaseTemporary(rt);
                    png = readable.EncodeToPNG();
                    UnityEngine.Object.DestroyImmediate(readable);
                }
            }
            catch (System.Exception e)
            {
                Debug.LogWarning($"DisplayXR: Failed to encode icon for {destPath}: {e.Message}");
                return false;
            }

            if (png == null || png.Length == 0)
                return false;

            return WriteBytesTolerant(destPath, png);
        }

        /// <summary>
        /// Write bytes to a path, tolerating the file being held open by another
        /// process. The DisplayXR Shell keeps registered app icons open to draw
        /// its launcher tiles, so rebuilding while the Shell is running hits a
        /// sharing violation on the icon. Skip the write when the content is
        /// unchanged (the common rebuild case) so we never touch the locked
        /// file; otherwise retry briefly to ride out a transient lock.
        /// </summary>
        private static bool WriteBytesTolerant(string destPath, byte[] bytes)
        {
            // Unchanged content → nothing to do. Read with shared access so a
            // viewer that has the file open for reading doesn't block us.
            try
            {
                if (File.Exists(destPath))
                {
                    byte[] existing;
                    using (var fs = new FileStream(destPath, FileMode.Open, FileAccess.Read, FileShare.ReadWrite))
                    {
                        existing = new byte[fs.Length];
                        int read = 0;
                        while (read < existing.Length)
                        {
                            int n = fs.Read(existing, read, existing.Length - read);
                            if (n <= 0) break;
                            read += n;
                        }
                    }
                    if (existing.Length == bytes.Length && ByteArraysEqual(existing, bytes))
                        return true;
                }
            }
            catch
            {
                // Couldn't compare (e.g. locked for read too) — fall through and
                // attempt the write/retry path below.
            }

            const int maxAttempts = 5;
            for (int attempt = 1; attempt <= maxAttempts; attempt++)
            {
                try
                {
                    File.WriteAllBytes(destPath, bytes);
                    return true;
                }
                catch (IOException e)
                {
                    if (attempt == maxAttempts)
                    {
                        Debug.LogWarning($"DisplayXR: Could not write icon to {destPath} after {maxAttempts} attempts " +
                                         $"(likely held open by the running DisplayXR Shell): {e.Message}. " +
                                         "Keeping the existing icon; close the Shell and rebuild to refresh it.");
                        return false;
                    }
                    System.Threading.Thread.Sleep(120);
                }
                catch (System.Exception e)
                {
                    Debug.LogWarning($"DisplayXR: Failed to write icon to {destPath}: {e.Message}");
                    return false;
                }
            }
            return false;
        }

        private static bool ByteArraysEqual(byte[] a, byte[] b)
        {
            for (int i = 0; i < a.Length; i++)
                if (a[i] != b[i]) return false;
            return true;
        }

        /// <summary>JSON-escape a string value with surrounding quotes.</summary>
        private static string JsonEscape(string value)
        {
            if (value == null) return "\"\"";
            return "\"" + value
                .Replace("\\", "\\\\")
                .Replace("\"", "\\\"")
                .Replace("\n", "\\n")
                .Replace("\r", "\\r")
                .Replace("\t", "\\t")
                + "\"";
        }
    }
}
