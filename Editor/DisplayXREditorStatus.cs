// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

using UnityEngine;
using DisplayXR;

namespace DisplayXR.Editor
{
    /// <summary>
    /// Surfaces "connected display" runtime status from the Display Provider
    /// (<see cref="DisplayXRProvider"/>) so editor inspectors and the settings page
    /// show live info in the built-app path and in Play Mode once it runs the
    /// provider (#171).
    /// </summary>
    public static class DisplayXREditorStatus
    {
        /// <summary>Which backend currently supplies runtime status, if any.</summary>
        public enum Source { None, Provider }

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

            info = default;
            source = Source.None;
            return false;
        }

        /// <summary>
        /// Eye-tracking status from the provider. <paramref name="hasPositions"/> is
        /// always false (the provider reports tracked/not-tracked but no eye
        /// positions); callers should show only the tracked flag.
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

            return false;
        }

        /// <summary>Human-readable label for a status source (for the inspector header).</summary>
        public static string SourceLabel(Source source)
        {
            switch (source)
            {
                case Source.Provider: return "Display Provider";
                default:              return "None";
            }
        }
    }
}
