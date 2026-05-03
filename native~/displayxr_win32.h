// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
//
// Win32 window management for the DisplayXR Unity plugin.
// Standalone mode: overlay child window. Shell mode: IAT hooks + subclass.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Get or create the overlay child HWND on top of Unity's main window.
/// On first call: finds Unity's window, creates a transparent child window.
/// On subsequent calls: returns the existing overlay HWND.
/// @return HWND cast to void*, or NULL if no window found.
void *displayxr_get_app_main_view(void);

/// Get Unity's top-level HWND without creating an overlay child window.
/// For shell/IPC mode where the compositor uses IPC swapchain textures.
/// @return HWND cast to void*, or NULL if no window found.
void *displayxr_get_unity_main_hwnd(void);

/// Check whether the plugin is running in shell/IPC mode.
/// Detected via DISPLAYXR_SHELL_SESSION=1 environment variable.
int displayxr_is_shell_mode(void);

/// Install IAT hooks and window subclass for shell mode input.
/// Hooks GetForegroundWindow, GetFocus, RegisterRawInputDevices.
/// Subclasses Unity's HWND to suppress deactivation and track button state.
/// @param unity_hwnd The Unity main HWND.
/// @return 1 on success, 0 on failure.
int displayxr_install_focus_hook(void *unity_hwnd);

/// (issue #57) Toggle chroma-key transparent overlay mode on the parent
/// (Unity top-level) HWND. When enabled, the window is flipped to
/// WS_POPUP | WS_EX_LAYERED with LWA_COLORKEY so DWM punches the chroma color
/// through to the desktop, and WM_NCHITTEST is gated by the rect set via
/// displayxr_set_overlay_hit_rect(). Mutually exclusive with shell mode.
/// @param enabled   Non-zero to enable, zero to restore the original styles.
/// @param color_key Windows COLORREF (0x00BBGGRR). Typical: 0x00FF00FF (magenta).
/// @param topmost   Non-zero to add WS_EX_TOPMOST while enabled.
void displayxr_set_transparent_overlay(int enabled, uint32_t color_key, int topmost);

/// (issue #57) Update the rectangular hit-test region used while transparent
/// overlay mode is enabled. Inside the rect, WM_NCHITTEST returns HTCLIENT;
/// outside, HTTRANSPARENT (clicks fall through to whatever's behind).
/// Coordinates are client-space pixels (top-left origin).
void displayxr_set_overlay_hit_rect(int x, int y, int w, int h);

/// (issue #57) Per-pixel hit-test override for transparent overlay mode.
/// AND-ed with the rect check above in WM_NCHITTEST. C# updates this each
/// frame from a Physics.Raycast at the current cursor — lets clicks fall
/// through inside the AABB but outside the cube silhouette.
void displayxr_set_overlay_hit_active(int active);

/// (issue #57) Read the cursor position in overlay-client coords plus the
/// current mouse-button state. Designed for transparent overlay mode where
/// Unity's New Input System Mouse.current.position is frozen because the
/// cloaked Unity HWND isn't OS-foreground (documented Unity limitation).
/// Position comes from GetCursorPos + ScreenToClient on the overlay HWND;
/// buttons come from the s_vkey_state table populated by raw-input + shell
/// subclass. @param clientX Set to client-space X (-1 if no overlay).
/// @param clientY Set to client-space Y (-1 if no overlay).
/// @param buttons Bit 0 = left, 1 = right, 2 = middle.
void displayxr_get_overlay_pointer(int *clientX, int *clientY, int *buttons);

#ifdef __cplusplus
}
#endif
