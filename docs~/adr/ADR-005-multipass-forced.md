---
status: Accepted
date: 2025-05-01
---

# ADR-005: Forced Multi-Pass Rendering

## Context

Unity's OpenXR integration supports three stereo render modes: Multi-Pass, Single-Pass Instanced, and Single-Pass. Kooima asymmetric frustum projection requires *different* projection matrices per eye (asymmetric, not symmetric). Single-Pass Instanced renders both eyes in one draw call with a single (symmetric) projection matrix, which is fundamentally incompatible.

Additionally, Single-Pass Instanced is broken on macOS/Metal (confirmed Unity bug, Won't Fix status).

## Decision

Force Multi-Pass in `DisplayXRFeature.OnInstanceCreate()`:
- Set `OpenXRSettings.renderMode = MultiPass`
- Also update the private backing field `m_renderMode` via reflection to survive `ApplySettings()` which runs after `OnInstanceCreate`
- Add a validation rule that warns if Multi-Pass is not selected

## Consequences

- Rendering cost is 2x draw calls (one per eye) — acceptable for 3D displays where per-eye asymmetric frustum is required
- No Single-Pass Instanced support, even on platforms where it works (Windows)
- The reflection hack (`m_renderMode` field access) may break if Unity changes the internal field name

## Revisited (2026-06-28): SPI is viable — one runtime change unblocks it

After the URP off-axis fix (#127) moved projection injection off the MultiPass-only
`SetStereoProjectionMatrix` onto a command-buffer re-push, SPI was re-tried on hardware
(branch `experiment/spi-single-pass`). Conclusion: **the plugin and Unity sides are
already correct for SPI**; the only blocker is in the **runtime compositor**, which we own.

With an eye-tracking lock the whole chain is per-eye distinct (`GetStereoMatrices`,
Unity's own `xr.GetProjMatrix(0)!=(1)`, and the pushed stereo arrays). Unity creates a
2-layer texture-array swapchain (`xrCreateSwapchain ... arrays=2`), renders the two eyes
into layers 0/1, and submits one projection layer with two views referencing
`subImage.imageArrayIndex` 0 (left) and 1 (right). The woven output is flat only because
the runtime samples array **layer 0 for both views**, ignoring `imageArrayIndex`.
MultiPass works because each eye is its own single-layer swapchain.

**Runtime fix:** sample swapchain array layer = `view.subImage.imageArrayIndex` per
projection view in the Display Processor / weave (not hardcoded 0). Then SPI renders 3D
and the plugin's off-axis push (#127) makes it pixel-correct — URP-on-Windows, ~half the
CPU cost. (BiRP/macOS-Metal stay MultiPass.) **Until that runtime change lands, MultiPass
remains forced.** Two earlier red herrings to avoid re-investigating: the `tracking=0`
nominal fallback looks mono (it's the correct no-track flat-2D output), and the boot
splash truncates `displayxr.log` — diagnose with `DISPLAYXR_NO_SPLASH=1` and a face lock.
Full evidence: `docs~/experiments/spi-single-pass.md`.
