// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

using UnityEngine;

namespace DisplayXR.Samples
{
    /// <summary>
    /// Minimal starter for the alpha-native transparent overlay technique.
    /// Boots a DisplayXR rig + transparent overlay on Camera.main, drops a
    /// placeholder cube, and runs above the Windows desktop with click-
    /// through outside the cube's silhouette.
    ///
    /// Drop this into an empty scene, build Windows standalone, run on a
    /// Leia SR display. See README.md for the full walk-through of which
    /// piece (app / plugin / runtime / OS) owns what.
    /// </summary>
    public static class MinimalTransparent
    {
        // PHASE 1: BEFORE xrCreateSession.
        // SubsystemRegistration runs earlier than the OpenXR loader's session
        // creation, so the runtime sees the transparent-background flag when
        // constructing XrWin32WindowBindingCreateInfoDXR. OnEnable on a scene
        // component would be too late — the session is already up.
        [RuntimeInitializeOnLoadMethod(RuntimeInitializeLoadType.SubsystemRegistration)]
        static void Bootstrap()
        {
            DisplayXR.DisplayXRTransparentOverlay.RequestTransparentSession();
        }

        // PHASE 2: AFTER scene load.
        // Attach the rig + overlay to Camera.main and ensure there's
        // something to render.
        [RuntimeInitializeOnLoadMethod(RuntimeInitializeLoadType.AfterSceneLoad)]
        static void Install()
        {
            var cam = Camera.main;
            if (cam == null)
            {
                Debug.LogWarning("[MinimalTransparent] No Camera.main — skipping setup.");
                return;
            }

            // Stereo rig — display-centric (scale-as-zoom). For a camera-
            // centric / FOV-driven rig instead, swap DisplayXRDisplay for
            // DisplayXRCamera. Either works; both push Kooima projection
            // tunables to the native hook chain via the static rig manager.
            if (cam.GetComponent<DisplayXR.DisplayXRDisplay>() == null
             && cam.GetComponent<DisplayXR.DisplayXRCamera>() == null)
            {
                cam.gameObject.AddComponent<DisplayXR.DisplayXRDisplay>();
            }

            // Transparent overlay component. The camera clear flips to
            // SolidColor (0,0,0,0) in OnEnable; the runtime composes the
            // desktop under each tile pre-weave so the alpha=0 regions show
            // through to whatever's behind the window.
            var overlay = cam.gameObject.GetComponent<DisplayXR.DisplayXRTransparentOverlay>();
            if (overlay == null)
                overlay = cam.gameObject.AddComponent<DisplayXR.DisplayXRTransparentOverlay>();

            // Placeholder content if the scene is empty. Replace with your
            // own avatar / props / etc.
            if (Object.FindAnyObjectByType<MeshRenderer>() == null)
            {
                var cube = GameObject.CreatePrimitive(PrimitiveType.Cube);
                cube.name = "Avatar";
                cube.transform.position = new Vector3(0f, 0f, 0.4f);
                cube.transform.localScale = Vector3.one * 0.1f;
                overlay.clickableRenderers = new Renderer[] { cube.GetComponent<Renderer>() };

                var lightGo = new GameObject("DirectionalLight");
                var light = lightGo.AddComponent<Light>();
                light.type = LightType.Directional;
                lightGo.transform.rotation = Quaternion.Euler(50f, -30f, 0f);
            }

            // Subscribe via overlay.onPointerDown / .onPointerClick / etc.
            // Do NOT use OnMouseDown — Unity's input system can't observe a
            // cloaked HWND, so it stays frozen. The overlay polls the cursor
            // natively (Win32 GetCursorPos) and dispatches per-pixel hit
            // events through these UnityEvents.
        }
    }
}
