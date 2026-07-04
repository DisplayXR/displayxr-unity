// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// OpenXR function interception for Unity's OpenXR Feature hook mechanism.

#pragma once

#include <stdint.h>
#include <openxr/openxr.h>

#ifdef __cplusplus
extern "C" {
#endif

// Platform export macros
#if defined(_WIN32)
#define DISPLAYXR_EXPORT __declspec(dllexport)
#elif defined(__GNUC__)
#define DISPLAYXR_EXPORT __attribute__((visibility("default")))
#else
#define DISPLAYXR_EXPORT
#endif

/// Logging helper — writes to displayxr.log and OutputDebugString on Windows.
void displayxr_log(const char *fmt, ...);

// --- P/Invoke exports for C# ---

DISPLAYXR_EXPORT void displayxr_set_tunables(float ipd_factor,
                                           float parallax_factor,
                                           float perspective_factor,
                                           float virtual_display_height,
                                           float inv_convergence_distance,
                                           float fov_override,
                                           float near_z,
                                           float far_z,
                                           int camera_centric,
                                           int clip_at_display_plane);

DISPLAYXR_EXPORT void displayxr_get_display_info(float *display_width_m,
                                               float *display_height_m,
                                               uint32_t *pixel_width,
                                               uint32_t *pixel_height,
                                               float *nominal_x,
                                               float *nominal_y,
                                               float *nominal_z,
                                               float *scale_x,
                                               float *scale_y,
                                               int *is_valid);

DISPLAYXR_EXPORT void displayxr_get_eye_positions(float *lx,
                                                float *ly,
                                                float *lz,
                                                float *rx,
                                                float *ry,
                                                float *rz,
                                                int *is_tracked);

DISPLAYXR_EXPORT void displayxr_set_scene_transform(float pos_x,
                                                   float pos_y,
                                                   float pos_z,
                                                   float ori_x,
                                                   float ori_y,
                                                   float ori_z,
                                                   float ori_w,
                                                   float scale_x,
                                                   float scale_y,
                                                   float scale_z,
                                                   int enabled);

DISPLAYXR_EXPORT void displayxr_set_window_handle(void *handle);

DISPLAYXR_EXPORT void displayxr_set_editor_mode(int enabled);

/// (issue runtime-pvt #191 / displayxr-unity #57) Request the runtime's
/// transparent-background mode for the next OpenXR session. Sets the
/// transparentBackgroundEnabled field on XrWin32WindowBindingCreateInfoEXT
/// (XR_EXT_win32_window_binding spec_version 4) when the next session is
/// created, opting the bound HWND's swapchain into BitBlt (D3D11) or DComp
/// (D3D12) presentation.
///
/// Must be called BEFORE xrCreateSession runs (typically from
/// [RuntimeInitializeOnLoadMethod(SubsystemRegistration)] in C#).
/// On older runtimes that don't implement spec_version 4, the field is at
/// the end of the struct and is harmlessly ignored.
DISPLAYXR_EXPORT void displayxr_set_transparent_background(int enabled);

/// Opt into avatar-style "simple window" mode (Windows). When enabled the
/// plugin binds the runtime to Unity's REAL main HWND directly — no off-screen
/// overlay, no DWM cloak, no off-screen move. Click-through is region-based
/// (SetWindowRgn on Unity's HWND) and decoration toggles via
/// displayxr_toggle_window_decoration. Call from C# at SubsystemRegistration
/// (before xrCreateSession), like displayxr_set_transparent_background.
DISPLAYXR_EXPORT void displayxr_request_simple_window(int enabled);

/// (#131) Register a Unity RenderTexture (R8G8B8A8_UNORM) as the 2D surround
/// source. The runtime fills the non-canvas region (outside the canvas sub-rect
/// set via displayxr_set_canvas_rect) from this texture each frame, post-weave,
/// at full native panel resolution. The plugin copies the RT into a SHARED
/// surround texture + signals a SHARED ID3D12Fence, then registers via
/// xrSetSharedTextureSurround2DFenceEXT. Pass NULL to clear. D3D12 hooked path.
DISPLAYXR_EXPORT void displayxr_surround_set_texture(void *unity_native_tex,
                                                     uint32_t width, uint32_t height);

/// (#131) Clear the registered 2D surround texture and unregister from the
/// runtime. Releases the SHARED surround texture + fence.
DISPLAYXR_EXPORT void displayxr_surround_clear(void);

/// (#131) Get the runtime's weave-target size = the bound HWND client area, in
/// physical pixels. On Leia SR this differs from the display panel dims (the SR
/// weaver oversizes/crops the window). The 2D surround texture + canvas sub-rect
/// must be sized in THESE pixels. Returns 0x0 if no window is bound yet.
DISPLAYXR_EXPORT void displayxr_get_render_target_size(uint32_t *out_w, uint32_t *out_h);

/// (issue displayxr-unity #85) Configure Unity's main render NSWindow for
/// transparent overlay mode on macOS. Pass enabled=1 to flip
/// `setOpaque:NO` + `backgroundColor=clearColor` so per-pixel alpha from
/// the camera reaches the desktop; pass 0 to restore the saved values.
///
/// The runtime configures the CAMetalLayer it presents into (via the
/// transparentBackgroundEnabled bit on the cocoa window binding), but
/// the NSWindow itself is the app's responsibility — Unity won't flip it
/// for us. Call from DisplayXRTransparentOverlay.OnEnable / OnDisable.
///
/// No-op on non-Apple platforms.
DISPLAYXR_EXPORT void displayxr_macos_configure_unity_nswindow(int enabled);

/// Move Unity's configured NSWindow by (dx, dy) screen points. Primitive
/// for app-driven borderless-window drag (right-mouse-drag, etc.). Cocoa
/// screen coords: +x right, +y up. No-op when no window is configured or
/// on non-Apple platforms.
DISPLAYXR_EXPORT void displayxr_macos_offset_window(int dx, int dy);

/// Recommended cursor-anchored window-drag API (v1.5.5+). Begin captures
/// the cursor's offset within the window; each Update snaps the window
/// origin so the cursor stays pinned to the same spot. Avoids the
/// scale/feedback issues of computing deltas in the app from
/// Mouse.current. App calls Begin on mouse-down, Update each frame while
/// held, End on mouse-up. All no-ops on non-Apple platforms / before
/// configure_unity_nswindow.
DISPLAYXR_EXPORT void displayxr_macos_begin_window_drag(void);
DISPLAYXR_EXPORT void displayxr_macos_update_window_drag(void);
DISPLAYXR_EXPORT void displayxr_macos_end_window_drag(void);

/// Toggle Unity's configured NSWindow between borderless (no title bar /
/// close / minimize / resize chrome) and the saved original style. Apps
/// that want an avatar / floating-window look call this with enabled=1
/// after configure_unity_nswindow. Cocoa's title-bar drag is gone with
/// the title bar — use the cursor-anchored drag API to move the window.
/// Save/restore is symmetric; set(0) restores. Idempotent.
DISPLAYXR_EXPORT void displayxr_macos_set_window_borderless(int enabled);

/// Hint the typed-swapchain substitution about Unity's project color space.
/// Must be called BEFORE Unity creates its OpenXR swapchains (i.e. from the
/// OpenXR feature's OnInstanceCreate).
///   1 = Linear color space → UNORM_SRGB typed siblings.
///   0 = Gamma color space  → UNORM typed siblings (no double gamma encoding).
/// Default is 1 (sRGB) for backward compatibility.
DISPLAYXR_EXPORT void displayxr_set_use_srgb_swapchain(int enabled);

/// Check whether the plugin is running in shell/IPC mode.
/// Detected via DISPLAYXR_WORKSPACE_SESSION=1 (legacy DISPLAYXR_SHELL_SESSION=1
/// is also honored) environment variable.
/// Callable from C# via P/Invoke.
DISPLAYXR_EXPORT int displayxr_is_shell_mode(void);

/// Get shell-mode mouse button state (from WM_INPUT usButtonFlags).
/// Returns current button mask: bit 0 = left, bit 1 = right, bit 2 = middle.
/// Also returns mouse position (client coords from shell-forwarded WM_MOUSEMOVE).
DISPLAYXR_EXPORT void displayxr_get_shell_mouse_state(int *buttons, int *mouseX, int *mouseY);

DISPLAYXR_EXPORT void displayxr_set_viewport_size(uint32_t width, uint32_t height,
                                                  int32_t screen_x, int32_t screen_y);

/// Same as displayxr_set_viewport_size but marks native (WM_SIZE) as the
/// authoritative source, causing subsequent C# displayxr_set_viewport_size
/// calls to become no-ops.  Prevents stale Screen.width/height from
/// overwriting correct values during resize/fullscreen transitions.
DISPLAYXR_EXPORT void displayxr_set_viewport_size_native(uint32_t width, uint32_t height,
                                                         int32_t screen_x, int32_t screen_y);

DISPLAYXR_EXPORT int displayxr_request_display_mode(int mode_3d);

/// (#140 / #396 W6) Capture the runtime's composed multi-view atlas to a PNG via
/// xrCaptureAtlasEXT (XR_EXT_atlas_capture). The runtime does the readback with
/// the compositor's own atlas image and writes
/// "<path_prefix>_atlas_<viewCount>_<cols>x<rows>.png" (it owns the suffix —
/// DisplayXR/displayxr-runtime#425) — the app passes a bare prefix and no longer
/// needs an AsyncGPUReadback or hidden-camera re-render. Non-blocking
/// (latches; PNG lands next composed frame). @param stage 1 = projection-only,
/// 0 = post-compose (includes chrome/quad layers). Returns 1 on XR_SUCCEEDED, 0
/// if the extension is unresolved, there is no live session, or the call failed.
DISPLAYXR_EXPORT int displayxr_capture_atlas(const char *path_prefix, int stage);

DISPLAYXR_EXPORT void displayxr_get_stereo_matrices(float *left_view,
                                                   float *left_proj,
                                                   float *right_view,
                                                   float *right_proj,
                                                   int *valid);

DISPLAYXR_EXPORT void displayxr_get_readback(uint8_t **pixels,
                                           uint32_t *width,
                                           uint32_t *height,
                                           int *ready);

DISPLAYXR_EXPORT void *displayxr_create_shared_texture(uint32_t width, uint32_t height);

DISPLAYXR_EXPORT void displayxr_destroy_shared_texture(void);

DISPLAYXR_EXPORT void displayxr_get_shared_texture(void **native_ptr,
                                                  uint32_t *width,
                                                  uint32_t *height,
                                                  int *ready);

/// Set the canvas output rect for shared texture compositing (play mode).
/// Calls xrSetSharedTextureOutputRectEXT on the runtime session.
DISPLAYXR_EXPORT void displayxr_set_canvas_rect(
    int32_t x, int32_t y, uint32_t w, uint32_t h);

/// (#131) Report the active canvas sub-rect in HWND-client pixels. Returns 1 and
/// fills the out params when a sub-rect is set (via displayxr_set_canvas_rect),
/// 0 otherwise. Used by the Win32 overlay to map the click-through silhouette
/// into the same sub-rect the runtime weaves the 3D into. Out params may be NULL.
DISPLAYXR_EXPORT int displayxr_get_canvas_rect_px(
    int32_t *x, int32_t *y, uint32_t *w, uint32_t *h);

/// XR_EXT_display_zones: define the 3D-zone rect (client-window pixels, top-left
/// origin) the runtime frames the Kooima 3D into. The locate hook chains an
/// XrDisplayZoneEXT in front of the view-rig (zone-scoped projection) and the
/// xrEndFrame hook chains the same zone on each projection layer (binding its
/// views into the rect). w<=0 || h<=0 clears. Inert unless the runtime advertises
/// XR_EXT_display_zones and reports caps.supported. Mutually exclusive with the
/// canvas-rect/surround path (a zones frame makes the output rect inert). Hooked
/// path (built apps) only.
DISPLAYXR_EXPORT void displayxr_set_3d_zone_rect(
    int32_t x, int32_t y, int32_t w, int32_t h);

/// XR_EXT_display_zones: clear the 3D-zone rect (revert to full-window framing).
DISPLAYXR_EXPORT void displayxr_clear_3d_zone(void);

/// Kill xrPollEvent forwarding immediately. Call from C# before session/instance
/// teardown to prevent use-after-free when the runtime is unloaded.
DISPLAYXR_EXPORT void displayxr_stop_polling(void);

#ifdef _WIN32
/// (issue #57) Toggle transparent overlay mode on the parent (Unity top-
/// level) HWND. Strips decorations, cloaks Unity, moves it off-screen so
/// click-through routes to desktop apps, and snaps the top-level
/// NOREDIRECTIONBITMAP overlay HWND to Unity's former rect. Transparency
/// itself comes from the runtime's DComp visuals + ALPHA_BLEND swapchain;
/// no OS color key. Mutually exclusive with shell mode.
/// @param enabled  Non-zero to enable, zero to restore the original styles.
/// @param topmost  Non-zero to add WS_EX_TOPMOST while enabled.
DISPLAYXR_EXPORT void displayxr_set_transparent_overlay(int enabled,
                                                        int topmost);

/// (avatar simple-window) Style Unity's REAL main HWND for avatar-style
/// windowing: strip to borderless WS_POPUP (no off-screen overlay, no DWM
/// cloak, no off-screen move — Unity stays the on-screen render target the
/// runtime composites into). Click-through is region-based (SetWindowRgn via
/// the existing hit-mask path, retargeted to this HWND); a borderless
/// right-drag moves the window (#61-bracketed). Decoration toggles via
/// displayxr_toggle_window_decoration. Pair with displayxr_request_simple_window
/// (which selects the real HWND at session create). Mutually exclusive with
/// shell mode and the transparent overlay. enabled=0 restores Unity's styles.
/// @param enabled  Non-zero to enable, zero to restore.
/// @param topmost  Non-zero to add WS_EX_TOPMOST while enabled.
DISPLAYXR_EXPORT void displayxr_set_simple_window(int enabled, int topmost);

/// (avatar simple-window) Toggle window decoration on the simple-window HWND:
/// WS_POPUP (borderless) <-> WS_OVERLAPPEDWINDOW (title bar + sizing frame for
/// OS move/resize). The client rect is preserved across the toggle so rendered
/// content doesn't jump/rescale. Decorating clears the silhouette region (whole
/// frame grabbable); going borderless lets the next hit-mask push re-shape it.
/// No-op unless displayxr_set_simple_window is active. Matches the avatar's B key.
DISPLAYXR_EXPORT void displayxr_toggle_window_decoration(void);

/// (avatar simple-window) Set decoration state explicitly (1=decorated,
/// 0=borderless). Same behavior as displayxr_toggle_window_decoration but
/// idempotent. No-op unless displayxr_set_simple_window is active.
DISPLAYXR_EXPORT void displayxr_set_window_decorated(int decorated);

/// (issue #57) Update the rectangular hit-test region used while transparent
/// overlay mode is enabled. Inside the rect, WM_NCHITTEST returns HTCLIENT;
/// outside, HTTRANSPARENT (clicks fall through to whatever's behind).
/// Coordinates are client-space pixels (top-left origin).
DISPLAYXR_EXPORT void displayxr_set_overlay_hit_rect(int x, int y, int w, int h);

/// (issue #57) Per-pixel hit-test override for transparent overlay mode.
/// AND-ed with the rect check above in WM_NCHITTEST. Set from C# each frame
/// based on a Physics.Raycast at the polled cursor — lets clicks fall
/// through inside the AABB but outside the cube silhouette.
DISPLAYXR_EXPORT void displayxr_set_overlay_hit_active(int active);

/// (issue #57 Approach B+) Per-pixel silhouette mask drives SetWindowRgn
/// for cross-process click-through. mask is mask_w*mask_h bytes (non-zero
/// = opaque/catch, zero = transparent/route-past), upsampled to overlay
/// client size dst_w*dst_h. NULL/empty mask reverts to AABB-region path.
/// Once a mask is applied, displayxr_set_overlay_hit_rect stops driving
/// SetWindowRgn for the lifetime of the overlay (mask wins).
DISPLAYXR_EXPORT void displayxr_set_overlay_hit_mask(const uint8_t *mask,
                                                     int mask_w, int mask_h,
                                                     int dst_w, int dst_h);

/// (#131) Register an opaque rect (overlay client pixels, top-left origin) that
/// must catch clicks even though it lives in the 2D surround region outside the
/// 3D silhouette — e.g. a high-res text bubble drawn into the surround. It is
/// UNION-ed into the SetWindowRgn region built by displayxr_set_overlay_hit_mask
/// each frame, so the bubble catches clicks while the empty surround still routes
/// past to the desktop. Pass w<=0 or h<=0 to clear. Takes effect on the next
/// hit-mask update.
DISPLAYXR_EXPORT void displayxr_set_overlay_surround_rect(int x, int y,
                                                          int w, int h);

/// (#131) Per-pixel variant of displayxr_set_overlay_surround_rect: register the
/// EXACT shape of a 2D surround element (e.g. a comic bubble with a triangular
/// tail) as an alpha mask (mask_w*mask_h bytes, non-zero = opaque/catch), mapped
/// over the dst rect (overlay client px, top-left). It is RLE'd and UNION-ed into
/// the SetWindowRgn region built by displayxr_set_overlay_hit_mask each frame, so
/// the element catches clicks while the empty area beside/around it (including the
/// corners next to a triangular tail) keeps routing past to the desktop — which a
/// single bounding rect cannot express. The surround is flat post-weave 2D, so
/// the caller rasterizes the mask directly (no disparity / per-view math). The
/// plugin copies the bytes. Pass mask=NULL or any dim <=0 to clear. Coexists with
/// the rect API (both are unioned in); callers using the mask should clear the
/// rect. Takes effect on the next hit-mask update.
DISPLAYXR_EXPORT void displayxr_set_overlay_surround_mask(const uint8_t *mask,
                                                          int mask_w, int mask_h,
                                                          int dst_x, int dst_y,
                                                          int dst_w, int dst_h);

/// (#131) Put the transparent overlay into "fixed full-screen, app-managed
/// window" mode. enabled=1: resize the overlay HWND to its monitor bounds at
/// the monitor origin (so the weaver interlaces the whole screen once, at the
/// lenticular-aligned origin) and set an app-managed flag that DISABLES the
/// native right-drag MOVE — the app now owns ALL window interaction by placing
/// its content in virtual rects (3D canvas sub-rect + 2D surround) inside the
/// fixed full-screen surface. enabled=0: clear the flag (restore native move).
/// Windows transparent overlay only; no-op when there is no overlay HWND.
DISPLAYXR_EXPORT void displayxr_set_overlay_fullscreen(int enabled);

/// (#131) Opt in to a born-fullscreen transparent overlay. Must be called BEFORE
/// the overlay is created (call as early as possible — e.g. a Unity
/// RuntimeInitializeOnLoadMethod(BeforeSplashScreen)). The overlay is then
/// created already covering its monitor, so a fullscreen 2D-surround app never
/// needs a post-creation resize (which recreates the swapchain = a startup
/// flash) and Unity itself can stay WINDOWED (avoiding fullscreen-optimization /
/// independent-flip, which bypasses DWM alpha compositing). Setting it after the
/// overlay exists has no effect on birth size. Windows transparent overlay only.
DISPLAYXR_EXPORT void displayxr_set_fullscreen_overlay_pref(int enabled);

/// (#131) Set the transparent overlay's mouse cursor shape, applied on every
/// mouse move within the overlay's client area (the region editor uses it to
/// show resize affordances on window edges/corners and region lines). Shapes:
/// 0=arrow, 1=size-WE (horizontal), 2=size-NS (vertical), 3=size-NWSE,
/// 4=size-NESW, 5=size-all (move). Out-of-range falls back to arrow. Windows
/// transparent overlay only; no-op elsewhere.
DISPLAYXR_EXPORT void displayxr_set_overlay_cursor(int shape);

/// (issue #57) Returns 1 if the OS foreground window belongs to our process,
/// 0 otherwise. Use to gate input handlers (WASD etc.) that should be
/// inactive when the user has clicked through the overlay to another app.
/// The IAT-hooked GetForegroundWindow inside Unity always returns Unity's
/// HWND for OpenXR purposes — this function calls the real OS GetForegroundWindow
/// from the plugin DLL (whose IAT is not patched) and reflects actual OS state.
DISPLAYXR_EXPORT int displayxr_is_our_process_foreground(void);

/// (issue #57) Read the overlay's current client size in pixels. The
/// overlay's screen rect can change at runtime (e.g. an app resizing it
/// itself) but Unity's Screen.width/height stay frozen at the off-screen
/// Unity HWND's dimensions — C# raycast code must use these values for
/// cursor → NDC conversion, not Screen.*. Returns (0, 0) if no overlay HWND.
DISPLAYXR_EXPORT void displayxr_get_overlay_size(int *width, int *height);

/// (issue #57) Read cursor position (overlay-client coords, top-left origin)
/// + mouse button state. Designed for transparent overlay mode where Unity's
/// New Input System Mouse.current.position is frozen because the cloaked
/// Unity HWND isn't OS-foreground (documented Unity limitation). Returns
/// (-1, -1) for clientX/Y if no overlay HWND. Buttons: bit 0 = left, 1 =
/// right, 2 = middle.
DISPLAYXR_EXPORT void displayxr_get_overlay_pointer(int *clientX, int *clientY,
                                                    int *buttons);

/// (v1.2.2) Atomically read + zero the overlay's accumulated mouse-wheel
/// delta. Returns Win32 raw units (120 per notch; positive = wheel forward).
/// Always 0 in opaque mode (no transparent overlay). The plugin no longer
/// self-resizes the overlay on wheel events — apps consume the delta here
/// and decide what to do (e.g. drive a DisplayXRDisplay rig's
/// virtualDisplayHeight for zoom-in-window).
DISPLAYXR_EXPORT int displayxr_consume_overlay_wheel_delta(void);

/// (display-zones port) Atomically read + zero the overlay's close-request flag.
/// Returns 1 once after the user pressed the decorated overlay's close (X)
/// button or Alt+F4. The plugin swallows the overlay's WM_CLOSE (so it does NOT
/// self-destruct the overlay HWND, which would leave cloaked Unity running
/// headless) and raises this flag instead. Apps poll it each frame and call
/// Application.Quit() to shut the whole process down cleanly. Windows
/// transparent overlay only; always 0 elsewhere.
DISPLAYXR_EXPORT int displayxr_consume_overlay_close_request(void);

/// (display-zones port) Resize the managed window (transparent overlay HWND, or
/// the dormant simple-window HWND) to width x height px via SetWindowPos,
/// #61-bracketed for lenticular phase-snap and clamped to a minimum size. This
/// is the RELIABLE resize path on the SR display: the Leia weaver subclass on a
/// non-activating overlay claims edge button-downs (so mouse/OS-border resize
/// can't start), but SetWindowPos is never intercepted. Apps drive this from a
/// keyboard shortcut. Windows transparent overlay only; no-op elsewhere.
DISPLAYXR_EXPORT void displayxr_resize_overlay(int width, int height);

/// (display-zones port) Get/set the managed overlay window's screen-space
/// top-left, for app-side window-position persistence (remember/restore across
/// launches). set is #61-bracketed (size unchanged). Windows overlay only.
DISPLAYXR_EXPORT void displayxr_get_overlay_position(int *x, int *y);
DISPLAYXR_EXPORT void displayxr_set_overlay_position(int x, int y);
#endif // _WIN32

#ifdef __cplusplus
}
#endif
