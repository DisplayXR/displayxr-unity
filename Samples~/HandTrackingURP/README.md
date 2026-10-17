# Hand Tracking (URP Ghost Hand)

URP add-on for the **Hand Tracking (XR Hands)** sample. It holds only the Ghost Hand's
Universal Render Pipeline shader and material:

| File | What it is |
|---|---|
| `Resources/DisplayXRGhostHand/GhostHandURP.shader` | `DisplayXR/Samples/GhostHandURP`, a hand-written HLSL port (not Shader Graph) of the three Built-in passes: depth prime, front-culled outline, HDR fresnel body. SRP-batcher compatible, single-pass-instanced safe. |
| `Resources/DisplayXRGhostHand/Ghost Hand URP.mat` | The Ghost Hand look (fresnel 22.4× white @ α 0.098, power 0.629, lighting off, white hairline outline). |

## Use

In a **URP** project, import this sample **next to** *Hand Tracking (XR Hands)*
(Package Manager > DisplayXR > Samples). Nothing to wire: `DisplayXRGhostHand` loads
`Resources/DisplayXRGhostHand/Ghost Hand URP` when `GraphicsSettings.currentRenderPipeline`
is URP, and `Resources.Load` searches every `Resources` folder, so it finds this one
wherever the import lands. The material samples the main sample's
`GenericHandTransparencyMap.png` (by GUID), so import the main sample as well.
Without this add-on a URP project logs a warning naming it and shows the joint-sphere
visual instead (the Built-in material would render pink under URP).

## Why it is a separate sample

Importing a sample compiles every shader in it. This shader includes
`Packages/com.unity.render-pipelines.universal/ShaderLibrary/Core.hlsl`, which does not
exist in a Built-in or HDRP project, so shipping it inside the main sample put a red
`Couldn't open include file` error in the Console of every non-URP user on import
(field report on PR #379, Unity 6000.4). The sample still fell back correctly at runtime,
but an import that errors looks broken.

**Do not import it into a non-URP project.** If you do, the SubShader's
`PackageRequirements { "com.unity.render-pipelines.universal" }` block makes Unity skip it
before compilation, so you get one "no SubShader meets the package requirements" warning
rather than an include error. Delete the folder to clear it.

## License

The shader is derived from Ultraleap's `GenericHandShader.shader` (Apache License 2.0). See
[`THIRD-PARTY-NOTICES.md`](THIRD-PARTY-NOTICES.md) and
[`LICENSE-Ultraleap-Apache-2.0.md`](LICENSE-Ultraleap-Apache-2.0.md).
