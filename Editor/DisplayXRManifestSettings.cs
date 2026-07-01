// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

using UnityEngine;

namespace DisplayXR.Editor
{
    /// <summary>
    /// Project-level settings for the .displayxr.json app manifest sidecar
    /// generated alongside built executables. Any DisplayXR-compatible
    /// workspace controller (the DisplayXR Shell is the reference) uses
    /// this file to discover and display installed apps.
    /// </summary>
    [CreateAssetMenu(fileName = "DisplayXRManifestSettings", menuName = "DisplayXR/Manifest Settings", order = 0)]
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

        [Tooltip("Also write a registered manifest to %LOCALAPPDATA%\\DisplayXR\\apps\\ " +
                 "so DisplayXR-compatible workspace controllers (including the DisplayXR Shell) " +
                 "discover this build without needing it under Program Files.")]
        public bool registerWithDisplayXR = false;

        [Tooltip("DisplayXR shows a branded boot splash (logo on the zero-disparity " +
                 "plane) by default, replacing Unity's stock splash in standalone " +
                 "builds. Tick to OPT OUT — keep Unity's splash and skip the DisplayXR " +
                 "one (sets the DISPLAYXR_NO_SPLASH define).")]
        public bool disableBootSplash = false;

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
        /// Does not create a new asset. Searches the project for any asset
        /// of this type so the user can place it anywhere under Assets/.
        /// </summary>
        public static DisplayXRManifestSettings Find()
        {
            // Fast path: the canonical location.
            var settings = UnityEditor.AssetDatabase.LoadAssetAtPath<DisplayXRManifestSettings>(AssetPath);
            if (settings != null)
                return settings;

            // Fallback: anywhere under Assets/. Pick the first match.
            string[] guids = UnityEditor.AssetDatabase.FindAssets("t:DisplayXRManifestSettings");
            if (guids != null && guids.Length > 0)
            {
                string path = UnityEditor.AssetDatabase.GUIDToAssetPath(guids[0]);
                return UnityEditor.AssetDatabase.LoadAssetAtPath<DisplayXRManifestSettings>(path);
            }
            return null;
        }
#endif
    }
}
