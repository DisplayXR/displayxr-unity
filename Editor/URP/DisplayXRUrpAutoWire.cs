// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
//
// Zero-click wiring of the UNIVERSAL off-axis projection fix on URP.
//
// KooimaProjectionFixFeature must be on the URP renderer for off-center Kooima to
// render correctly — every URP DisplayXR app needs it (unlike the opt-in foreground
// clip). Rather than make every user run a menu item, this bootstrap auto-adds it
// once when it sees a DisplayXR rig in an open scene under URP.
//
// Scope is deliberately narrow:
//   - Only the projection fix is ever auto-added — NEVER the foreground clip
//     (that one is opt-in for transparent-overlay apps).
//   - Only fires when a DisplayXRDisplay / DisplayXRCamera exists in a loaded scene,
//     so a URP project that merely has the package installed isn't touched.
//   - Idempotent (skips if already wired) and suppressible via
//     DisplayXR > Auto-Wire URP Projection Fix.

using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;
using UnityEngine.Rendering;
using UnityEngine.SceneManagement;
using DisplayXR.URP;

namespace DisplayXR.Editor.URP
{
    [InitializeOnLoad]
    internal static class DisplayXRUrpAutoWire
    {
        const string kPrefKey = "DisplayXR.URP.AutoWireProjectionFix";
        const string kMenu = "DisplayXR/Auto-Wire URP Projection Fix";

        static DisplayXRUrpAutoWire()
        {
            EditorApplication.delayCall += TryAutoWire;
            EditorSceneManager.sceneOpened += (_, __) => TryAutoWire();
        }

        static bool Enabled
        {
            get => EditorPrefs.GetBool(kPrefKey, true);
            set => EditorPrefs.SetBool(kPrefKey, value);
        }

        [MenuItem(kMenu)]
        static void ToggleEnabled()
        {
            Enabled = !Enabled;
            if (Enabled) TryAutoWire();
        }

        [MenuItem(kMenu, validate = true)]
        static bool ToggleValidate()
        {
            Menu.SetChecked(kMenu, Enabled);
            return true;
        }

        static void TryAutoWire()
        {
            if (!Enabled) return;
            // URP only — the fix is a URP RendererFeature.
            if (GraphicsSettings.currentRenderPipeline == null) return;
            if (!SceneHasDisplayXRRig()) return;

            var rendererData = DisplayXRUrpInstaller.FindRendererData();
            if (rendererData == null) return;
            if (DisplayXRUrpInstaller.HasFeature<KooimaProjectionFixFeature>(rendererData)) return;

            if (DisplayXRUrpInstaller.InstallProjectionFix(interactive: false))
                Debug.Log("[DisplayXRUrp] Auto-wired the Kooima Projection Fix into " +
                          rendererData.name + " (URP DisplayXR rig detected). " +
                          "Disable via DisplayXR > Auto-Wire URP Projection Fix.");
        }

        static bool SceneHasDisplayXRRig()
        {
            for (int i = 0; i < SceneManager.sceneCount; i++)
            {
                var scene = SceneManager.GetSceneAt(i);
                if (!scene.isLoaded) continue;
                foreach (var root in scene.GetRootGameObjects())
                {
                    if (root.GetComponentInChildren<DisplayXRDisplay>(true) != null) return true;
                    if (root.GetComponentInChildren<DisplayXRCamera>(true) != null) return true;
                }
            }
            return false;
        }
    }
}
