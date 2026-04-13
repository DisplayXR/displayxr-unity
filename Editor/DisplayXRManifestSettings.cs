// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0

using UnityEngine;

namespace DisplayXR.Editor
{
    /// <summary>
    /// Project-level settings for the .displayxr.json app manifest sidecar
    /// generated alongside built executables. The DisplayXR shell's spatial
    /// launcher uses this file to discover and display installed apps.
    /// </summary>
    public class DisplayXRManifestSettings : ScriptableObject
    {
        private const string AssetPath = "Assets/DisplayXRManifestSettings.asset";

        public enum AppCategory { app, demo, test, tool }
        public enum StereoLayout { sbs_lr, sbs_rl, tb, bt }

        [Tooltip("Display name shown on the launcher tile. Leave empty to use Application.productName.")]
        public string appName = "";

        [Tooltip("Free-form category tag for the launcher.")]
        public AppCategory category = AppCategory.app;

        [Tooltip("Preferred display rendering mode at launch.")]
        public string displayMode = "auto";

        [Tooltip("One-line description for tooltips (max 256 chars).")]
        [TextArea(1, 3)]
        public string description = "";

        [Tooltip("2D tile icon (PNG, 512x512 recommended). Leave empty for text-only tile.")]
        public Texture2D icon;

        [Tooltip("Stereoscopic side-by-side tile icon (1024x512 recommended). Enables 3D tile in launcher.")]
        public Texture2D icon3D;

        [Tooltip("How the stereo pair is packed in the 3D icon.")]
        public StereoLayout icon3DLayout = StereoLayout.sbs_lr;

        /// <summary>Effective app name: explicit setting or fallback to product name.</summary>
        public string EffectiveName =>
            string.IsNullOrWhiteSpace(appName) ? Application.productName : appName;

        /// <summary>Convert StereoLayout enum to the JSON string value.</summary>
        public static string LayoutToString(StereoLayout layout)
        {
            switch (layout)
            {
                case StereoLayout.sbs_lr: return "sbs-lr";
                case StereoLayout.sbs_rl: return "sbs-rl";
                case StereoLayout.tb:     return "tb";
                case StereoLayout.bt:     return "bt";
                default:                  return "sbs-lr";
            }
        }

#if UNITY_EDITOR
        /// <summary>
        /// Load the settings asset, or create one if it doesn't exist.
        /// </summary>
        public static DisplayXRManifestSettings GetOrCreate()
        {
            var settings = UnityEditor.AssetDatabase.LoadAssetAtPath<DisplayXRManifestSettings>(AssetPath);
            if (settings != null)
                return settings;

            settings = CreateInstance<DisplayXRManifestSettings>();
            UnityEditor.AssetDatabase.CreateAsset(settings, AssetPath);
            UnityEditor.AssetDatabase.SaveAssets();
            return settings;
        }

        /// <summary>
        /// Try to load existing settings. Returns null if no asset exists.
        /// Does not create a new asset.
        /// </summary>
        public static DisplayXRManifestSettings Find()
        {
            return UnityEditor.AssetDatabase.LoadAssetAtPath<DisplayXRManifestSettings>(AssetPath);
        }
#endif
    }
}
