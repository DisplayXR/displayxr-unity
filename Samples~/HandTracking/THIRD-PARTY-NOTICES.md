# Third-party notices — Hand Tracking sample

## Ultraleap Unity Plugin — Generic Hand ("Ghost Hand") model

| File in this sample | Origin |
|---|---|
| `Resources/DisplayXRGhostHand/GenericHand.fbx` | `Hands/Runtime/Models/GenericHand/GenericHand.fbx` — copied verbatim (import settings kept, material slot remapped to the sample's material) |
| `Resources/DisplayXRGhostHand/GenericHandTransparencyMap.png` | `Hands/Runtime/Models/GenericHand/GenericHandTransparencyMap.png` — copied verbatim |
| `Resources/DisplayXRGhostHand/GhostHand.shader` | derived from `GenericHandShader.shader` (renamed `DisplayXR/Samples/GhostHand`, defaults changed, `Fallback Off`) |
| `Resources/DisplayXRGhostHand/Ghost Hand.mat` | values transcribed from `Ghost Hand.mat` / `Outline.mat` (fresnel 22.4× white @ 0.098, power 0.629; lighting off; white hairline outline) |
| *(the URP shader + material ship in the separate "Hand Tracking (URP Ghost Hand)" sample, `Samples~/HandTrackingURP/`, with its own notice)* | |
| `DisplayXRGhostHand.cs` (bone table + rotation offsets) | the per-hand `HandBinder` settings transcribed from `Hands/Runtime/Prefabs/GhostHands.prefab` (`GlobalFingerRotationOffset` / `WristRotationOffset`, right hand = the left model mirrored in X) — code is DisplayXR's own |

Source: **Ultraleap Unity Plugin ("Ultraleap Tracking") 5.4.0**, package root `Assets/ThirdParty/Ultraleap/Tracking/`,
https://github.com/ultraleap/UnityPlugin.

Copyright (C) Ultraleap, Inc. 2011-2021.

Licensed under the **Apache License, Version 2.0** — the full text is in
[`LICENSE-Ultraleap-Apache-2.0.md`](LICENSE-Ultraleap-Apache-2.0.md) next to this file
(a copy of the plugin's `LICENSE.md`). You may not use these files except in compliance
with the License. Distributed on an "AS IS" basis, without warranties or conditions of any
kind. The files above were modified as noted in the table; everything else in this sample
is DisplayXR's own work under the package license.
