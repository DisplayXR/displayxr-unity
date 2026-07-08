// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

using System.Collections.Generic;
using UnityEngine;
using UnityEngine.Rendering;
using UnityEngine.XR;
using UnityEngine.XR.Management;

namespace DisplayXR
{
    /// <summary>
    /// XRLoader for the custom DisplayXR IUnityXRDisplay Display Provider (epic
    /// #166). Registering it under XR Plug-in Management is what makes the
    /// "DisplayXR Display" subsystem a real toggle and ships
    /// Runtime/UnitySubsystemsManifest.json in builds automatically — replacing the
    /// M1 [RuntimeInitializeOnLoadMethod] bootstrap + manifest hand-copy.
    ///
    /// The native provider (registered from UnityPluginLoad) implements the
    /// subsystem; this loader creates/starts/stops it and owns the lifecycle of the
    /// per-frame <see cref="DisplayXRProviderDriver"/>. Display-only (no input
    /// subsystem) — by design (#166): apps render stereo 3D while using normal
    /// Unity input.
    /// </summary>
    public sealed class DisplayXRDisplayLoader : XRLoaderHelper
    {
        // Must match Runtime/UnitySubsystemsManifest.json's display "id".
        const string k_DisplayId = "DisplayXR Display";

        static readonly List<XRDisplaySubsystemDescriptor> s_DisplayDescriptors = new();

        /// <summary>The created display subsystem (null until Initialize succeeds).</summary>
        public XRDisplaySubsystem DisplaySubsystem => GetLoadedSubsystem<XRDisplaySubsystem>();

        public override bool Initialize()
        {
            // macOS EDITOR gate (#204): built players run the full zero-copy Metal
            // pipeline, but the Unity EDITOR SEGVs in its Metal GfxDeviceWorker
            // (CreateColorRenderSurface NULL deref) whenever it allocates a new
            // color RenderSurface while XR passes are active — reproduced on
            // 6000.3 + 6000.4 with zero provider command buffers in flight (a
            // Unity editor defect; full bisection on #204). Until a Unity fix or
            // workaround lands, macOS editor Play Mode declines cleanly instead
            // of crashing the editor. DISPLAYXR_METAL_EDITOR=1 opts back in for
            // the workaround hunt. Built players (OSXPlayer) are unaffected.
            if (Application.platform == RuntimePlatform.OSXEditor &&
                System.Environment.GetEnvironmentVariable("DISPLAYXR_METAL_EDITOR") != "1")
            {
                Debug.LogWarning("[DisplayXR] macOS editor Play Mode is disabled pending a Unity " +
                    "editor Metal fix (see displayxr-unity#204) — build & run a macOS player to " +
                    "test stereo, or set DISPLAYXR_METAL_EDITOR=1 to opt in anyway.");
                return false;
            }

            // Pick the stereo render mode BEFORE creating/starting the subsystem so
            // the native GfxStart (which reads it) sees the right value (#166 task #8).
            // SPI is correct only on URP+Windows+D3D12; on BiRP it renders opaque
            // geometry wrong (skybox-only), so gate to MultiPass there. Mirrors the
            // hook path's DisplayXRFeature.IsUrpWindowsD3D12 (the provider can't use
            // OpenXRRuntime.name — Unity's OpenXR loader isn't active in provider mode).
            bool spi = IsSinglePassEligible();
            DisplayXRProviderNative.dxr_prov_set_single_pass(spi ? 1 : 0);
            Debug.Log("[DisplayXR] Provider render mode: " + (spi ? "Single-Pass-Instanced" : "MultiPass")
                      + " (pipeline=" + (GraphicsSettings.currentRenderPipeline != null
                          ? GraphicsSettings.currentRenderPipeline.GetType().Name : "Built-in")
                      + ", gfx=" + SystemInfo.graphicsDeviceType + ")");

            // In editor Play Mode the runtime must weave into a window that COEXISTS
            // with the editor, not the app-owned overlay (which tracks Unity's window =
            // the whole editor) and not self-host (no bound HWND → window-relative Kooima
            // dead + focus-switch crash). Opt into the dedicated standalone window (#173)
            // before the subsystem starts (LifecycleStart reads the flag). Built players
            // keep the overlay default. Windows-only behavior: on macOS the runtime
            // self-hosts its NSWindow (the Metal bring-up shape, #204) — the dedicated
            // flag also selects the D3D11 editor bridge, which doesn't exist there.
            // macOS parity (#205): the flag also tells the Metal provider to weave
            // into a dedicated preview window in the editor, vs. Unity's own game
            // window (the input-transparent in-app overlay) in a built player — so
            // built-player mouse/keyboard reach Unity's window, not a detached one.
            if (Application.platform == RuntimePlatform.WindowsEditor
             || Application.platform == RuntimePlatform.OSXEditor)
            {
                DisplayXRProviderNative.dxr_prov_set_dedicated_window(1);
                Debug.Log("[DisplayXR] Provider: editor Play Mode → dedicated weave window (#173)");
            }

            CreateSubsystem<XRDisplaySubsystemDescriptor, XRDisplaySubsystem>(
                s_DisplayDescriptors, k_DisplayId);
            if (DisplaySubsystem == null)
            {
                Debug.LogError("[DisplayXR] Failed to create the '" + k_DisplayId +
                    "' display subsystem. Is the native plugin present and " +
                    "UnitySubsystemsManifest.json shipped?");
                return false;
            }
            return true;
        }

        public override bool Start()
        {
            StartSubsystem<XRDisplaySubsystem>();
            DisplayXRProviderDriver.EnsureInstance();
            return true;
        }

        public override bool Stop()
        {
            DisplayXRProviderDriver.DestroyInstance();
            StopSubsystem<XRDisplaySubsystem>();
            return true;
        }

        public override bool Deinitialize()
        {
            DestroySubsystem<XRDisplaySubsystem>();
            return base.Deinitialize();
        }

        /// <summary>
        /// Whether to drive the provider in Single-Pass-Instanced (SPI) mode.
        /// <para>
        /// <b>Default render path:</b> <b>URP and HDRP both default to SPI on D3D11, D3D12,
        /// and Metal.</b> They consume the provider's full per-eye projection matrix and render
        /// into the arraySize=2 SPI swapchain (eyes = array slices 0/1), a pipeline-agnostic
        /// path. <b>BiRP → MultiPass</b> (SPI renders BiRP's off-center opaque geometry wrong).
        /// </para>
        /// <para>
        /// <b>MultiPass now runs on both D3D11 and D3D12</b> (#195): each eye renders into its
        /// own single-slice texture, which the provider copies into the array swapchain's slice
        /// 0/1. On D3D12 the per-eye textures are the own-device bridge; on D3D11 they are plain
        /// Unity textures in a built player (zero-copy) or the own-device bridge in the editor.
        /// So <b>BiRP + D3D11</b> is fully supported (editor + player). D3D11 URP/HDRP use SPI
        /// the same way (zero-copy in players, own-device bridge in the editor).
        /// </para>
        /// Dev overrides: <c>DISPLAYXR_FORCE_SPI=1</c> / <c>DISPLAYXR_FORCE_MULTIPASS=1</c>.
        /// </summary>
        static bool IsSinglePassEligible()
        {
            if (System.Environment.GetEnvironmentVariable("DISPLAYXR_FORCE_MULTIPASS") == "1")
                return false;
            if (System.Environment.GetEnvironmentVariable("DISPLAYXR_FORCE_SPI") == "1")
                return true;

            // EXPLICIT PLATFORM × GRAPHICS-API × PIPELINE POLICY TABLE (#202):
            //
            //   platform × API      | URP | HDRP | BiRP
            //   --------------------|-----|------|------
            //   Windows × D3D11     | SPI | SPI  | MultiPass
            //   Windows × D3D12     | SPI | SPI  | MultiPass
            //   macOS   × Metal     | SPI | SPI  | MultiPass
            //   anything else       | MultiPass (moot — the native GfxStart no-starts)
            //
            // Rationale: URP and HDRP both consume the provider's full per-eye
            // projection matrix and render into the arraySize=2 SPI swapchain (eyes =
            // array slices 0/1) — pipeline-agnostic, so SPI regardless of API. On
            // Metal the whole arraySize=2 swapchain image is wrapped as one 2-slice
            // Unity texture and Unity renders both eyes into it (#205, hardware-
            // verified: correct off-center stereo, no regression). HDRP+D3D12 SPI was
            // once gated off for a "washed-out splash" (#191), but that washout is
            // pipeline-wide (repros on D3D12 MultiPass and D3D11 SPI too) — an HDRP
            // lighting/exposure issue, not an SPI regression. BiRP → MultiPass (SPI
            // renders BiRP's off-center opaque geometry wrong); MultiPass runs on all
            // three APIs (per-eye single-slice textures woven into slices 0/1: D3D
            // #195, Metal zero-copy slice views #204).
            var plat = Application.platform;
            bool isWindows = plat == RuntimePlatform.WindowsPlayer
                          || plat == RuntimePlatform.WindowsEditor;
            bool isMac = plat == RuntimePlatform.OSXPlayer
                      || plat == RuntimePlatform.OSXEditor;

            var gdt = SystemInfo.graphicsDeviceType;
            bool isD3D11 = gdt == GraphicsDeviceType.Direct3D11;
            bool isD3D12 = gdt == GraphicsDeviceType.Direct3D12;
            bool isMetal = gdt == GraphicsDeviceType.Metal;
            bool apiEligible = (isWindows && (isD3D11 || isD3D12))
                            || (isMac && isMetal);
            if (!apiEligible)
                return false; // unsupported platform/API: MultiPass (native no-starts)

            var rp = GraphicsSettings.currentRenderPipeline;
            var typeName = rp != null ? rp.GetType().FullName : null;
            bool isUrp  = typeName != null && typeName.Contains("Universal");
            bool isHdrp = typeName != null && typeName.Contains("HighDefinition");
            return isUrp || isHdrp; // BiRP (rp == null) → MultiPass
        }
    }
}
