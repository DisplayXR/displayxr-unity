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
