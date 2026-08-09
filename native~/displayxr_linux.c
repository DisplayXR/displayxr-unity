// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Desktop-Linux platform glue for the IUnityXRDisplay provider (#249).
//
// Two jobs, both mirroring what displayxr_macos.mm does for Cocoa:
//
//  1. Platform stubs the shared provider code references unconditionally
//     (displayxr_is_shell_mode). macOS ships the same stub for the same reason.
//
//  2. Find the PLAYER'S OWN top-level X11 window, so the session can be a
//     HANDLE app (XR_DXR_xlib_window_binding) exactly like Windows (HWND) and
//     macOS (NSView) — the runtime weaves into Unity's window instead of
//     creating one of its own.
//
// WHY WE DLOPEN libX11 RATHER THAN LINK IT
// ----------------------------------------
// Same policy as displayxr_vk_loader.c: the shipped .so must keep NO hard
// DT_NEEDED on a library that might be absent (a headless or Wayland-only box),
// and CI must not need libx11-dev. So every Xlib entry point is resolved at
// runtime and the whole feature degrades to "no window binding" if libX11 is
// missing — the runtime then self-hosts, which still renders.
//
// WHICH WINDOW THE RUNTIME WEAVES INTO — A CHILD OF UNITY'S
// ---------------------------------------------------------
// We create our own X11 window and hand the runtime that, never Unity's own.
// XR_DXR_xlib_window_binding gives the RUNTIME presentation of the supplied
// window, so it must not already carry a swapchain the app presents to — and
// Unity's main window does. Measured on the Odyssey: binding Unity's window gave
// a perfectly healthy session (weaver holding the XID, frames flowing, no errors)
// and nothing on the panel, because two presenters fought over one surface.
//
// The window is a CHILD of Unity's, not a top-level overlay. That is what makes
// this behave like displayxr-demo-modelviewer — weave INSIDE the app's window on
// X11 — and it is strictly better than the top-level overlay it replaces:
//
//   - It renders above its parent by definition, so there is no stacking fight.
//     A top-level overlay needed override-redirect to stop the WM restacking
//     Unity's focused window over it.
//   - It is clipped to and moves with the parent, so dragging or restacking
//     Unity's window needs no tracking at all — X does it.
//   - It selects NO events, so X delivers pointer/keyboard to the deepest window
//     that DID select them — Unity's. Input pass-through comes for free, where a
//     top-level overlay needed an empty XShape input region (the analogue of
//     Windows' WS_EX_NOACTIVATE) just to stop stealing clicks.
//
// The demo creates ONE decorated top-level and centres it on the 3D panel via
// XR_DXR_display_info. It can, because it owns the only window. Unity owns its
// window and its placement, so we follow it rather than fight it — same visible
// result: the weave appears in the app's window.
//
// We still LOCATE Unity's window by walking the tree for _NET_WM_PID ==
// getpid() — the X11 analogue of macOS walking NSApplication, since Unity does
// not expose its window through IUnityGraphics.

#if defined(__linux__) && !defined(__ANDROID__)

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "displayxr_exports.h"

extern void dxr_prov_file_log(const char *s);

static void
lin_log(const char *msg)
{
	fputs(msg, stderr);
	dxr_prov_file_log(msg);
}

// ---------------------------------------------------------------------------
// Shell-mode stub
// ---------------------------------------------------------------------------

// There is no DisplayXR Shell on Linux — the workspace/IPC tile session is a
// Windows product feature. The shared provider code queries this predicate
// unconditionally, so (exactly like the macOS stub) we answer a constant 0
// rather than sprinkle #ifdefs through every call site.
DISPLAYXR_EXPORT int
displayxr_is_shell_mode(void)
{
	return 0;
}

// ---------------------------------------------------------------------------
// Xlib, resolved at runtime
// ---------------------------------------------------------------------------

typedef void *XDpy;
typedef unsigned long XWin;
typedef unsigned long XAtom;

struct XlibApi {
	void *lib;
	XDpy (*XOpenDisplay)(const char *);
	int (*XCloseDisplay)(XDpy);
	XWin (*XDefaultRootWindow)(XDpy);
	int (*XQueryTree)(XDpy, XWin, XWin *, XWin *, XWin **, unsigned int *);
	int (*XFree)(void *);
	XAtom (*XInternAtom)(XDpy, const char *, int);
	int (*XGetWindowProperty)(XDpy, XWin, XAtom, long, long, int, XAtom, XAtom *,
	                          int *, unsigned long *, unsigned long *, unsigned char **);
	int (*XGetGeometry)(XDpy, XWin, XWin *, int *, int *, unsigned int *,
	                    unsigned int *, unsigned int *, unsigned int *);
	int (*XSync)(XDpy, int);
	// Overlay-window creation
	XWin (*XCreateSimpleWindow)(XDpy, XWin, int, int, unsigned int, unsigned int,
	                            unsigned int, unsigned long, unsigned long);
	int (*XMapWindow)(XDpy, XWin);
	int (*XMoveResizeWindow)(XDpy, XWin, int, int, unsigned int, unsigned int);
	int (*XDestroyWindow)(XDpy, XWin);
	int (*XFlush)(XDpy);
	int (*XDefaultScreen)(XDpy);
};

static struct XlibApi s_x;
static XDpy s_dpy;       // our own connection; must outlive the session
static XWin s_win;       // the player's top-level window, 0 if not found
static int  s_attempts;  // bounded retry — see the note in the getter
static int  s_gave_up;   // latched only after we stop trying
static XWin s_overlay;   // OUR window — the one the runtime weaves into
static unsigned int s_ow, s_oh; // last child size, to skip no-op resizes

// Don't latch a FAILURE forever. The window may simply not be mapped yet on the
// first call (LifecycleStart can beat Unity's window creation depending on how
// early XR initializes), and caching "not found" from that one early look would
// permanently demote us to the self-hosted path for the whole process. Retry a
// bounded number of times, then stop walking the tree every frame.
#define LIN_MAX_WINDOW_SEARCHES 120

#define XL_SYM(name)                                                       \
	do {                                                                   \
		*(void **)(&s_x.name) = dlsym(s_x.lib, #name);                     \
		if (!s_x.name) {                                                   \
			lin_log("[DisplayXR-LNX] libX11 missing symbol " #name "\n");  \
			return 0;                                                      \
		}                                                                  \
	} while (0)

static int
lin_load_xlib(void)
{
	if (s_x.lib) return 1;
	// SONAME, not the -dev symlink: a runtime box has libX11.so.6 but usually
	// not the unversioned libX11.so.
	s_x.lib = dlopen("libX11.so.6", RTLD_NOW | RTLD_LOCAL);
	if (!s_x.lib) s_x.lib = dlopen("libX11.so", RTLD_NOW | RTLD_LOCAL);
	if (!s_x.lib) {
		lin_log("[DisplayXR-LNX] libX11 not present — no window binding; the runtime "
		        "will self-host its weave window\n");
		return 0;
	}
	XL_SYM(XOpenDisplay);
	XL_SYM(XCloseDisplay);
	XL_SYM(XDefaultRootWindow);
	XL_SYM(XQueryTree);
	XL_SYM(XFree);
	XL_SYM(XInternAtom);
	XL_SYM(XGetWindowProperty);
	XL_SYM(XGetGeometry);
	XL_SYM(XSync);
	XL_SYM(XCreateSimpleWindow);
	XL_SYM(XMapWindow);
	XL_SYM(XMoveResizeWindow);
	XL_SYM(XDestroyWindow);
	XL_SYM(XFlush);
	XL_SYM(XDefaultScreen);
	return 1;
}

#define XA_CARDINAL_ 6L // <X11/Xatom.h>, inlined so we need no X11 headers
// Read _NET_WM_PID off `w`. 0 when the property is absent.
static pid_t
lin_window_pid(XWin w, XAtom pid_atom)
{
	XAtom actual_type = 0;
	int actual_format = 0;
	unsigned long nitems = 0, bytes_after = 0;
	unsigned char *prop = NULL;
	pid_t out = 0;

	if (s_x.XGetWindowProperty(s_dpy, w, pid_atom, 0, 1, 0, (XAtom)XA_CARDINAL_,
	                           &actual_type, &actual_format, &nitems, &bytes_after,
	                           &prop) != 0 /* Success == 0 */)
		return 0;
	if (prop) {
		if (nitems >= 1 && actual_format == 32) out = (pid_t)(*(unsigned long *)prop);
		s_x.XFree(prop);
	}
	return out;
}

// Bounded BFS from the root, keeping the largest PID-matching window.
static void
lin_scan(XWin root, XAtom pid_atom, pid_t self, int depth, XWin *best, unsigned long *best_area)
{
	if (depth > 3) return;

	XWin r = 0, parent = 0, *kids = NULL;
	unsigned int nkids = 0;
	if (!s_x.XQueryTree(s_dpy, root, &r, &parent, &kids, &nkids) || !kids) return;

	for (unsigned int i = 0; i < nkids; i++) {
		if (lin_window_pid(kids[i], pid_atom) == self) {
			XWin gr = 0;
			int gx = 0, gy = 0;
			unsigned int gw = 0, gh = 0, gb = 0, gd = 0;
			if (s_x.XGetGeometry(s_dpy, kids[i], &gr, &gx, &gy, &gw, &gh, &gb, &gd)) {
				unsigned long area = (unsigned long)gw * (unsigned long)gh;
				// Ignore 1x1 / icon-sized helper windows.
				if (gw > 16 && gh > 16 && area > *best_area) {
					*best_area = area;
					*best = kids[i];
				}
			}
		}
		lin_scan(kids[i], pid_atom, self, depth + 1, best, best_area);
	}
	s_x.XFree(kids);
}

// Find (once) the player's own top-level X11 window.
// Returns 1 and fills the out-params on success. The Display connection is
// owned by this TU and intentionally kept open for the process lifetime — the
// runtime borrows it for the Vulkan surface (see the extension header).
DISPLAYXR_EXPORT int
displayxr_linux_get_app_window(void **out_display, unsigned long *out_window)
{
	if (!s_win && !s_gave_up) {
		if (!lin_load_xlib()) {
			s_gave_up = 1; // no libX11 — retrying cannot help
		} else {
			if (!s_dpy) {
				s_dpy = s_x.XOpenDisplay(NULL);
				if (!s_dpy) {
					lin_log("[DisplayXR-LNX] XOpenDisplay failed (DISPLAY unset?) — no window "
					        "binding; the runtime will self-host its weave window\n");
					s_gave_up = 1; // no display — retrying cannot help either
				}
			}
			if (s_dpy) {
				// A sync makes sure the tree we walk reflects what the server has;
				// Unity's window may have been created moments ago.
				s_x.XSync(s_dpy, 0);
				XWin root = s_x.XDefaultRootWindow(s_dpy);
				XAtom pid_atom = s_x.XInternAtom(s_dpy, "_NET_WM_PID", 1 /*only_if_exists*/);
				unsigned long best_area = 0;
				if (pid_atom) lin_scan(root, pid_atom, getpid(), 0, &s_win, &best_area);

				if (s_win) {
					char m[192];
					snprintf(m, sizeof(m),
					         "[DisplayXR-LNX] bound app X11 window 0x%lx (%lu px area, pid %d, "
					         "after %d search(es))\n",
					         (unsigned long)s_win, best_area, (int)getpid(), s_attempts + 1);
					lin_log(m);
				} else if (++s_attempts >= LIN_MAX_WINDOW_SEARCHES) {
					char m[192];
					snprintf(m, sizeof(m),
					         "[DisplayXR-LNX] no _NET_WM_PID window matched pid %d after %d "
					         "searches — giving up; the runtime will self-host its weave "
					         "window\n", (int)getpid(), s_attempts);
					lin_log(m);
					s_gave_up = 1;
				}
			}
		}
	}
	if (!s_dpy || !s_win) return 0;
	if (out_display) *out_display = s_dpy;
	if (out_window) *out_window = (unsigned long)s_win;
	return 1;
}

// Size of Unity's window (its client area). A child is positioned in PARENT
// coordinates, so unlike a top-level overlay we never need the absolute screen
// position — which also removes the XTranslateCoordinates round-trip and the
// class of off-by-a-titlebar placement bugs that came with it.
static int
lin_child_size(unsigned int *out_w, unsigned int *out_h)
{
	if (!s_dpy || !s_win) return 0;
	XWin gr = 0;
	int gx = 0, gy = 0;
	unsigned int gw = 0, gh = 0, gb = 0, gd = 0;
	if (!s_x.XGetGeometry(s_dpy, s_win, &gr, &gx, &gy, &gw, &gh, &gb, &gd)) return 0;
	*out_w = gw; *out_h = gh;
	return 1;
}

// Create (once) the PLUGIN-OWNED overlay window the runtime weaves into, sized and
// positioned over Unity's window. Returns 1 and fills the out-params on success.
//
// The Display connection is intentionally kept open for the process lifetime —
// the runtime borrows it for the Vulkan surface (see the extension header).
DISPLAYXR_EXPORT int
displayxr_linux_get_weave_window(void **out_display, unsigned long *out_window)
{
	if (!s_overlay) {
		void *dpy = NULL;
		unsigned long unity_win = 0;
		if (!displayxr_linux_get_app_window(&dpy, &unity_win)) return 0;

		unsigned int w = 0, h = 0;
		if (!lin_child_size(&w, &h) || w == 0 || h == 0) { w = 1920; h = 1080; }

		// CHILD of Unity's window, at 0,0, full size. CopyFromParent for depth
		// and visual so it matches whatever Unity negotiated. We deliberately
		// select NO events: X then delivers pointer/keyboard to the deepest
		// window that DID select them — Unity's — so input passes straight
		// through and we never steal a click.
		s_overlay = s_x.XCreateSimpleWindow(s_dpy, (XWin)unity_win, 0, 0, w, h, 0, 0, 0);
		if (!s_overlay) {
			lin_log("[DisplayXR-LNX] XCreateSimpleWindow (child) failed — the runtime "
			        "will self-host its weave window\n");
			return 0;
		}
		s_x.XMapWindow(s_dpy, s_overlay);
		s_x.XFlush(s_dpy);
		s_x.XSync(s_dpy, 0);
		s_ow = w; s_oh = h;

		char m[224];
		snprintf(m, sizeof(m),
		         "[DisplayXR-LNX] weave window 0x%lx created as a CHILD of Unity's window "
		         "0x%lx (%ux%u) — in-window weave, input passes through\n",
		         (unsigned long)s_overlay, unity_win, w, h);
		lin_log(m);
	}
	if (!s_dpy || !s_overlay) return 0;
	if (out_display) *out_display = s_dpy;
	if (out_window) *out_window = (unsigned long)s_overlay;
	return 1;
}

// Keep the child matched to Unity's client area. POSITION needs no work — a child
// is clipped to and moves with its parent — so this only ever resizes, and only
// when the parent actually changed.
DISPLAYXR_EXPORT void
displayxr_linux_track_window(void)
{
	if (!s_dpy || !s_overlay || !s_win) return;
	unsigned int w = 0, h = 0;
	if (!lin_child_size(&w, &h) || w == 0 || h == 0) return;
	if (w == s_ow && h == s_oh) return;
	s_ow = w; s_oh = h;
	s_x.XMoveResizeWindow(s_dpy, s_overlay, 0, 0, w, h);
	s_x.XFlush(s_dpy);
}

// Live size of the WEAVE window, for the per-frame canvas reconcile. 0 on failure
// so callers fall back to the display-info default.
DISPLAYXR_EXPORT int
displayxr_linux_window_size(uint32_t *out_w, uint32_t *out_h)
{
	XWin target = s_overlay ? s_overlay : s_win;
	if (!s_dpy || !target || !s_x.XGetGeometry) return 0;
	XWin gr = 0;
	int gx = 0, gy = 0;
	unsigned int gw = 0, gh = 0, gb = 0, gd = 0;
	if (!s_x.XGetGeometry(s_dpy, target, &gr, &gx, &gy, &gw, &gh, &gb, &gd)) return 0;
	if (gw == 0 || gh == 0) return 0;
	if (out_w) *out_w = (uint32_t)gw;
	if (out_h) *out_h = (uint32_t)gh;
	return 1;
}

// Tear down the overlay. The Display connection stays open — the runtime may still
// be unwinding its surface, and re-opening costs nothing we need back.
DISPLAYXR_EXPORT void
displayxr_linux_destroy_weave_window(void)
{
	if (s_dpy && s_overlay && s_x.XDestroyWindow) {
		s_x.XDestroyWindow(s_dpy, s_overlay);
		s_x.XFlush(s_dpy);
		lin_log("[DisplayXR-LNX] weave overlay window destroyed\n");
	}
	s_overlay = 0;
	s_ow = s_oh = 0;
}

#endif // __linux__ && !__ANDROID__
