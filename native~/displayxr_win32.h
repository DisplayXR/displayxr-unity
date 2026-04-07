// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
//
// Win32 window management for the DisplayXR Unity plugin.
// Standalone mode: overlay child window. Shell mode: IAT hooks + subclass.

#pragma once

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

#ifdef __cplusplus
}
#endif
