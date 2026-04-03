// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0

using UnityEngine;

namespace DisplayXR
{
    /// <summary>
    /// Deprecated: Previously rendered the DisplayXR composited preview in the Game View
    /// during Play Mode using the standalone session's shared texture.
    ///
    /// With the new architecture, the runtime composites directly into a plugin-created
    /// native window. Unity's XR loader runs natively during Play Mode. This component
    /// is no longer needed and will be removed in a future version.
    /// </summary>
    [AddComponentMenu("DisplayXR/Game View Overlay (Deprecated)")]
    public class DisplayXRGameViewOverlay : MonoBehaviour
    {
    }
}
