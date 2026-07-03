// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

using UnityEngine;
using DisplayXR;

// hook-path bridge: DisplayXRFeature is soft-deprecated (#166) but remains the active backend on the
// legacy hook path. Suppress CS0618 for these intentional internal uses.
#pragma warning disable 618

namespace DisplayXR.Editor
{
    /// <summary>
    /// Unifies "connected display" runtime status across the three DisplayXR session
    /// backends so editor inspectors and the settings page show live info no matter
    /// which one is driving (#166 gap #6):
    ///
    ///   1. Display Provider (<see cref="DisplayXRProvider"/>) — the built-app path,
    ///      and Play Mode once it runs the provider (#171). <see cref="DisplayXRFeature"/>
    ///      is NOT instantiated in provider mode, so the old
    ///      <c>DisplayXRFeature.Instance.DisplayInfo</c> reads returned null there.
    ///   2. Standalone editor preview (<see cref="DisplayXRPreviewSession"/>) — the
    ///      edit-mode Preview window's own OpenXR session.
    ///   3. OpenXR hook (<see cref="DisplayXRFeature"/>) — the legacy built-app / Play
    ///      Mode path when the provider isn't the active loader.
    ///
    /// Provider is preferred, then the standalone preview, then the hook.
    /// </summary>
    public static class DisplayXREditorStatus
    {
        /// <summary>Which backend currently supplies runtime status, if any.</summary>
        public enum Source { None, Provider, StandalonePreview, OpenXRHook }

        /// <summary>
        /// Fetch display geometry from whichever backend is live. Returns false (and
        /// <paramref name="source"/> = None) when nothing is connected.
        /// </summary>
        public static bool TryGetDisplayInfo(out DisplayXRDisplayInfo info, out Source source)
        {
            // 1. Provider (built app + Play Mode after #171 unification). The
            // dxr_prov_* exports exist only in the Windows DLL (the provider is
            // Windows/D3D12-only), so this branch is Windows-editor-only — on other
            // editors the P/Invoke would throw EntryPointNotFoundException.
#if UNITY_EDITOR_WIN
            if (DisplayXRProvider.TryGetDisplayInfo(out var p))
            {
                info = new DisplayXRDisplayInfo
                {
                    displayWidthMeters = p.widthM,
                    displayHeightMeters = p.heightM,
                    displayPixelWidth = p.pixelWidth,
                    displayPixelHeight = p.pixelHeight,
                    nominalViewerX = p.nominalX,
                    nominalViewerY = p.nominalY,
                    nominalViewerZ = p.nominalZ,
                    recommendedViewScaleX = p.scaleX,
                    recommendedViewScaleY = p.scaleY,
                    isValid = p.isValid != 0,
                };
                source = Source.Provider;
                return info.isValid;
            }
#endif

            // 2. Standalone editor preview session.
            if (DisplayXRPreviewSession.IsRunning && DisplayXRPreviewSession.DisplayInfo.isValid)
            {
                info = DisplayXRPreviewSession.DisplayInfo;
                source = Source.StandalonePreview;
                return true;
            }

            // 3. OpenXR hook (legacy path).
            var feature = DisplayXRFeature.Instance;
            if (feature != null && feature.DisplayInfo.isValid)
            {
                info = feature.DisplayInfo;
                source = Source.OpenXRHook;
                return true;
            }

            info = default;
            source = Source.None;
            return false;
        }

        /// <summary>
        /// Eye-tracking status from the live backend. <paramref name="hasPositions"/>
        /// is true only for the standalone preview and the hook (the provider reports
        /// tracked/not-tracked but no eye positions); when false, callers should show
        /// only the tracked flag.
        /// </summary>
        public static bool TryGetEyeTracking(out bool tracked, out Vector3 left,
                                             out Vector3 right, out bool hasPositions)
        {
            tracked = false;
            left = right = Vector3.zero;
            hasPositions = false;

#if UNITY_EDITOR_WIN
            if (DisplayXRProvider.IsRunning)
            {
                tracked = DisplayXRProvider.IsEyeTracked;
                hasPositions = false; // provider exposes no eye positions
                return true;
            }
#endif

            if (DisplayXRPreviewSession.IsRunning)
            {
                tracked = DisplayXRPreviewSession.IsEyeTracked;
                left = DisplayXRPreviewSession.LeftEyePosition;
                right = DisplayXRPreviewSession.RightEyePosition;
                hasPositions = true;
                return true;
            }

            var feature = DisplayXRFeature.Instance;
            if (feature != null)
            {
                tracked = feature.IsEyeTracked;
                left = feature.LeftEyePosition;
                right = feature.RightEyePosition;
                hasPositions = true;
                return true;
            }

            return false;
        }

        /// <summary>Human-readable label for a status source (for the inspector header).</summary>
        public static string SourceLabel(Source source)
        {
            switch (source)
            {
                case Source.Provider:          return "Display Provider";
                case Source.StandalonePreview: return "Standalone Preview";
                case Source.OpenXRHook:        return "OpenXR Hook";
                default:                       return "None";
            }
        }
    }
}
#pragma warning restore 618
