// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Readback buffer management for offscreen editor preview.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Readback callback invoked by DisplayXR runtime (macOS offscreen mode).
/// Copies pixel data into the shared state readback buffer.
void displayxr_readback_callback(const uint8_t *pixels, uint32_t width, uint32_t height, void *userdata);

/// Allocate the readback buffer for given dimensions.
void displayxr_readback_alloc(uint32_t width, uint32_t height);

/// Free the readback buffer.
void displayxr_readback_free(void);

#ifdef __cplusplus
}
#endif
