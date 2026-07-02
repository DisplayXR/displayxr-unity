// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
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

/// (#173) Create a DEDICATED, standalone weave window for the provider's editor
/// Play Mode. Unlike displayxr_get_app_main_view (which tracks Unity's window),
/// this is a top-level, movable/resizable WS_OVERLAPPEDWINDOW that is NOT parented
/// to or tracking Unity — so in the editor it coexists with the whole editor window
/// instead of covering it, while the runtime still weaves into a bound HWND (so
/// window-relative Kooima + #172 live realloc work). WS_EX_NOACTIVATE keeps the
/// editor foreground (the Input System keeps receiving keyboard/mouse); an explicit
/// Per-Monitor-V2 DPI awareness context avoids the SR weaver DPI/activation crash
/// self-host hit on focus-switch. Idempotent: returns the existing HWND on re-call.
/// @return HWND cast to void*, or NULL on failure (caller falls back to self-host).
void *displayxr_create_provider_dedicated_window(void);

/// (#173) Destroy the dedicated provider window (editor Play Mode). Call from the
/// provider's LifecycleStop (MAIN thread, after the session is stopped) so the
/// window doesn't linger frozen after Play stops. Idempotent.
void displayxr_destroy_provider_dedicated_window(void);

/// Check whether the plugin is running in shell/IPC mode.
/// Detected via DISPLAYXR_WORKSPACE_SESSION=1 (legacy DISPLAYXR_SHELL_SESSION=1
/// is also honored) environment variable.
int displayxr_is_shell_mode(void);

/// Install IAT hooks and window subclass for shell mode input.
/// Hooks GetForegroundWindow, GetFocus, RegisterRawInputDevices.
/// Subclasses Unity's HWND to suppress deactivation and track button state.
/// @param unity_hwnd The Unity main HWND.
/// @return 1 on success, 0 on failure.
int displayxr_install_focus_hook(void *unity_hwnd);

/// Shell mode: ask the main UI thread to park the app window far OFF-SCREEN
/// while keeping it visible (WS_VISIBLE), so Unity keeps rendering into the XR
/// swapchain (it skips the scene render for a hidden window) without the user
/// ever seeing the bare window. Posts a message handled SYNCHRONOUSLY in the
/// window subclass (main thread) — safe to call from the render thread, where a
/// direct SetWindowPos/ShowWindow would deadlock against the main thread and an
/// async SetWindowPos does not reliably move the window. Idempotent: a no-op
/// once the window is already off-screen + visible. Requires the focus-hook
/// subclass to be installed (it is, in shell mode).
/// @param unity_hwnd The Unity main HWND (window_handle).
void displayxr_shell_park_offscreen(void *unity_hwnd);

/// (issue #57) Toggle transparent overlay mode on the parent (Unity top-
/// level) HWND. Strips decorations, cloaks Unity, moves it off-screen so
/// transparent-zone clicks route to whatever's behind, and snaps the top-
/// level NOREDIRECTIONBITMAP overlay HWND to Unity's former rect.
/// Transparency itself comes from the runtime's DComp visuals + ALPHA_BLEND
/// swapchain; no OS color key. WM_NCHITTEST is gated by the rect set via
/// displayxr_set_overlay_hit_rect(). Mutually exclusive with shell mode.
/// @param enabled  Non-zero to enable, zero to restore the original styles.
/// @param topmost  Non-zero to add WS_EX_TOPMOST while enabled.
void displayxr_set_transparent_overlay(int enabled, int topmost);

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

/// (issue #57) Returns 1 if the OS foreground window belongs to our process,
/// 0 otherwise. Use this to gate input handlers (e.g. WASD movement) that
/// should be inactive when the user has clicked through to another app.
/// The IAT-hooked GetForegroundWindow inside Unity always returns Unity's
/// HWND so Unity perceives itself as foreground for OpenXR purposes — this
/// function calls the real OS GetForegroundWindow from the plugin DLL
/// (whose IAT is not patched) and reflects the actual OS state.
int displayxr_is_our_process_foreground(void);

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

/// (v1.2.2) Atomically read + zero the overlay's accumulated mouse-wheel
/// delta. Returns Win32 raw units (120 per notch; positive = wheel forward).
/// Always 0 in opaque mode (s_overlay_is_toplevel == 0). The plugin no
/// longer self-resizes the overlay on wheel events — apps consume the
/// delta here and choose what to do (e.g. drive a DisplayXRDisplay rig's
/// virtualDisplayHeight to zoom-in-window).
int displayxr_consume_overlay_wheel_delta(void);

/// (display-zones port) Atomically read + zero the overlay's close-request
/// flag. Returns 1 once after the user pressed the decorated overlay's close
/// (X) button or Alt+F4 — the plugin swallows WM_CLOSE (so it does NOT destroy
/// the overlay) and raises this instead. C# polls it each frame and calls
/// Application.Quit() so the whole app shuts down cleanly, not just the overlay.
int displayxr_consume_overlay_close_request(void);

/// (display-zones port) Resize the managed window (overlay HWND, or the dormant
/// simple-window HWND) to width x height client/window px via SetWindowPos. The
/// reliable resize path on the SR display: the weaver subclass eats mouse edge
/// interactions on the non-activating overlay, but SetWindowPos is never
/// intercepted. #61-bracketed for lenticular phase-snap. Clamped to a minimum
/// size. App drives this from a keyboard shortcut.
void displayxr_resize_overlay(int width, int height);

/// (display-zones port) Get/set the managed window's screen-space top-left.
/// For app-side window-position persistence (remember/restore across launches).
/// set is #61-bracketed (size unchanged); get returns (0,0) if no window.
void displayxr_get_overlay_position(int *x, int *y);
void displayxr_set_overlay_position(int x, int y);

#ifdef __cplusplus
}
#endif
