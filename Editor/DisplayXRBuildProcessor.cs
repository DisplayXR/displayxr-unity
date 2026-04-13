// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0

using System.IO;
using System.Text;
using UnityEditor.Build;
using UnityEditor.Build.Reporting;
using UnityEngine;

namespace DisplayXR.Editor
{
    /// <summary>
    /// Post-build processor that generates a .displayxr.json sidecar file
    /// next to the built executable. The DisplayXR shell's spatial launcher
    /// scans for these files to discover installed apps.
    /// </summary>
    public class DisplayXRBuildProcessor : IPostprocessBuildWithReport
    {
        public int callbackOrder => 100;

        public void OnPostprocessBuild(BuildReport report)
        {
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

            // Build JSON
            var sb = new StringBuilder();
            sb.AppendLine("{");
            sb.AppendLine("  \"schema_version\": 1,");
            sb.AppendLine($"  \"name\": {JsonEscape(appName)},");
            sb.Append($"  \"type\": \"3d\"");

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

            // Export icon textures if assigned
            if (settings != null && settings.icon != null)
            {
                string iconFile = "icon.png";
                if (ExportTexture(settings.icon, Path.Combine(exeDir, iconFile)))
                {
                    sb.AppendLine(",");
                    sb.Append($"  \"icon\": {JsonEscape(iconFile)}");
                }
            }
            if (settings != null && settings.icon3D != null)
            {
                string icon3DFile = "icon_sbs.png";
                if (ExportTexture(settings.icon3D, Path.Combine(exeDir, icon3DFile)))
                {
                    sb.AppendLine(",");
                    sb.Append($"  \"icon_3d\": {JsonEscape(icon3DFile)}");
                    string layout = DisplayXRManifestSettings.LayoutToString(settings.icon3DLayout);
                    if (layout != "sbs-lr")
                    {
                        sb.AppendLine(",");
                        sb.Append($"  \"icon_3d_layout\": {JsonEscape(layout)}");
                    }
                }
            }

            sb.AppendLine();
            sb.AppendLine("}");

            File.WriteAllText(sidecarPath, sb.ToString(), Encoding.UTF8);
            Debug.Log($"DisplayXR: Generated app manifest at {sidecarPath}");
        }

        /// <summary>
        /// Export a texture to PNG, handling non-readable textures via GPU readback.
        /// </summary>
        private static bool ExportTexture(Texture2D source, string destPath)
        {
            try
            {
                byte[] png;
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
                    Object.DestroyImmediate(readable);
                }

                if (png == null || png.Length == 0)
                    return false;

                File.WriteAllBytes(destPath, png);
                return true;
            }
            catch (System.Exception e)
            {
                Debug.LogWarning($"DisplayXR: Failed to export icon to {destPath}: {e.Message}");
                return false;
            }
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
