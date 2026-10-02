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
#include <time.h>
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
	// Transparent (ARGB top-level) overlay
	int (*XMatchVisualInfo)(XDpy, int, int, int, void *);
	unsigned long (*XCreateColormap)(XDpy, XWin, void *, int);
	int (*XFreeColormap)(XDpy, unsigned long);
	XWin (*XCreateWindow)(XDpy, XWin, int, int, unsigned int, unsigned int, unsigned int,
	                      int, unsigned int, void *, unsigned long, void *);
	int (*XTranslateCoordinates)(XDpy, XWin, XWin, int, int, int *, int *, XWin *);
	int (*XChangeProperty)(XDpy, XWin, XAtom, XAtom, int, int, const unsigned char *, int);
	int (*XDeleteProperty)(XDpy, XWin, XAtom);
	XWin (*XGetSelectionOwner)(XDpy, XAtom);
	// Click-through + right-drag move (#332)
	int (*XQueryPointer)(XDpy, XWin, XWin *, XWin *, int *, int *, int *, int *, unsigned int *);
	int (*XSendEvent)(XDpy, XWin, int, long, void *);
};

// Xlib structs the transparent overlay needs, mirrored so this TU keeps no X11
// build dependency. Field ORDER and widths must match <X11/Xutil.h> / <X11/Xlib.h>
// exactly (LP64: long/unsigned long/XID are 8 bytes, int/Bool 4).
typedef struct {
	void *visual;
	unsigned long visualid;
	int screen;
	int depth;
	int c_class;
	unsigned long red_mask, green_mask, blue_mask;
	int colormap_size;
	int bits_per_rgb;
} LinVisualInfo;

typedef struct {
	unsigned long background_pixmap;
	unsigned long background_pixel;
	unsigned long border_pixmap;
	unsigned long border_pixel;
	int bit_gravity;
	int win_gravity;
	int backing_store;
	unsigned long backing_planes;
	unsigned long backing_pixel;
	int save_under;
	long event_mask;
	long do_not_propagate_mask;
	int override_redirect;
	unsigned long colormap;
	unsigned long cursor;
} LinSetWindowAttributes;

#define LIN_TRUE_COLOR          4
#define LIN_INPUT_OUTPUT        1
#define LIN_CW_BACK_PIXEL       (1L << 1)
#define LIN_CW_BORDER_PIXEL     (1L << 3)
#define LIN_CW_OVERRIDE_REDIRECT (1L << 9)
#define LIN_CW_COLORMAP         (1L << 13)
#define LIN_PROP_MODE_REPLACE   0
#define LIN_SHAPE_INPUT         2
#define LIN_SHAPE_SET           0

// libXext's XShapeCombineRectangles, resolved separately: it is the only libXext
// entry point we need, and the transparent overlay is unusable without it (an
// override-redirect window with the default input region swallows every click).
typedef int (*PFN_XShapeCombineRectangles)(XDpy, XWin, int, int, int, void *, int, int, int);
static PFN_XShapeCombineRectangles s_shape_combine_rects;
// XShapeCombineMask with a None mask resets a shape to the default (unshaped) — how
// Unity's window gets its full input region back when the overlay goes away (#332).
typedef int (*PFN_XShapeCombineMask)(XDpy, XWin, int, int, int, unsigned long, int);
static PFN_XShapeCombineMask s_shape_combine_mask;

static struct XlibApi s_x;
static XDpy s_dpy;       // our own connection; must outlive the session
static XWin s_win;       // the player's top-level window, 0 if not found
static int  s_attempts;  // bounded retry — see the note in the getter
static int  s_gave_up;   // latched only after we stop trying
static XWin s_overlay;   // OUR window — the one the runtime weaves into
static unsigned int s_ow, s_oh; // last child size, to skip no-op resizes
static int  s_ox, s_oy;  // last top-level overlay origin (transparent mode only)
static int  s_transparent_requested; // set by displayxr_set_transparent_background
static int  s_overlay_is_toplevel;   // 1 = ARGB top-level overlay, 0 = opaque child
static unsigned long s_overlay_cmap; // colormap of the ARGB overlay

static void lin_reset_input_region(void); // click-through (#332), below
static long long lin_now_ns(void);

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
	XL_SYM(XMatchVisualInfo);
	XL_SYM(XCreateColormap);
	XL_SYM(XFreeColormap);
	XL_SYM(XCreateWindow);
	XL_SYM(XTranslateCoordinates);
	XL_SYM(XChangeProperty);
	XL_SYM(XDeleteProperty);
	XL_SYM(XGetSelectionOwner);
	XL_SYM(XQueryPointer);
	XL_SYM(XSendEvent);
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


// ---------------------------------------------------------------------------
// Transparent mode: an ARGB TOP-LEVEL overlay instead of the child
// ---------------------------------------------------------------------------
//
// The child window above cannot show the desktop: it composites into Unity's
// window, which is an opaque 24-bit top-level. X11 per-pixel transparency is a
// property of a TOP-LEVEL window with a 32-bit TrueColor (ARGB) visual, so a
// transparent session weaves into one of those instead, parked over Unity's
// client area.
//
// Why the ARGB visual is the whole fix, on every vendor (measured, #249):
//   - Mesa (Intel/AMD/llvmpipe) reports compositeAlpha OPAQUE|INHERIT for a
//     default-visual window and PRE_MULTIPLIED|INHERIT for an ARGB one, so the
//     runtime's transparent swapchain path simply engages.
//   - NVIDIA reports OPAQUE only, for both. The runtime then falls back to an
//     OPAQUE swapchain, but on an ARGB window the presented alpha still reaches
//     the compositor — verified by eye on an RTX 4090 under GNOME/XWayland.
//     On a default-visual window there is no alpha channel to carry it.
//
// The earlier top-level overlay lost a stacking fight: the WM raised Unity's
// focused window over it (see the header). In transparent mode that fight is
// moot. Unity's own window is made practically invisible, so whichever of the
// two is on top, what you see is the overlay — and input should reach Unity
// anyway. The overlay therefore takes an EMPTY input region (XShape) and every
// click falls through to Unity's window beneath it.
//
// "Practically invisible" is _NET_WM_WINDOW_OPACITY = ~1/255, not 0: mutter
// drops a fully transparent actor from picking, which would route the clicks
// past Unity to the desktop.
//
// Any missing piece — no compositing manager, no 32-bit visual, no libXext —
// falls back to the opaque child path rather than presenting a black box.
//
// THE CLOAK CANNOT OUTLIVE THE SESSION (#332; the Windows failure class is
// #295/#296). Unity's window is cloaked in exactly one place — overlay creation in
// LifecycleStart — and un-cloaked in exactly one — displayxr_linux_destroy_weave_
// window, from LifecycleStop. Every way a session can fail after the cloak returns
// kUnitySubsystemErrorCodeFailure from GfxStart (unsupported graphics API, every
// dxr_prov_session_start failure — the session-stop calls all live inside it), and
// Unity answers a failed GfxStart with LifecycleStop. Measured with a broken
// runtime manifest: cloak, "dxr_prov_session_start failed", un-cloak, visible.
// So unlike Windows, which pre-cloaks BEFORE LifecycleStart and needs a backstop
// timer for the "session never attempted" case, there is no window here in which
// the cloak exists without a LifecycleStop to undo it. Keep it that way: do not
// move the cloak earlier (e.g. into displayxr_linux_set_transparent) without
// adding a revert for that earlier point.

#define LIN_UNITY_CLOAK_OPACITY 0x01010101UL

// A compositing manager owns _NET_WM_CM_S<screen>. Without one, ARGB windows are
// drawn with their alpha ignored, i.e. black where the content is clear.
static int
lin_have_compositor(void)
{
	char name[32];
	snprintf(name, sizeof(name), "_NET_WM_CM_S%d", s_x.XDefaultScreen(s_dpy));
	XAtom cm = s_x.XInternAtom(s_dpy, name, 0);
	return cm && s_x.XGetSelectionOwner(s_dpy, cm) != 0;
}

static int
lin_load_xshape(void)
{
	if (s_shape_combine_rects) return 1;
	void *lib = dlopen("libXext.so.6", RTLD_NOW | RTLD_LOCAL);
	if (!lib) lib = dlopen("libXext.so", RTLD_NOW | RTLD_LOCAL);
	if (!lib) return 0;
	*(void **)(&s_shape_combine_rects) = dlsym(lib, "XShapeCombineRectangles");
	*(void **)(&s_shape_combine_mask) = dlsym(lib, "XShapeCombineMask");
	return s_shape_combine_rects != NULL;
}

// Unity's client-area origin in ROOT coordinates. Under a reparenting WM the
// client's XGetGeometry x/y is relative to the frame, so translate instead.
static int
lin_app_origin(int *out_x, int *out_y)
{
	XWin child = 0;
	return s_x.XTranslateCoordinates(s_dpy, s_win, s_x.XDefaultRootWindow(s_dpy), 0, 0,
	                                 out_x, out_y, &child) != 0;
}

static void
lin_set_unity_opacity(int cloak)
{
	XAtom op = s_x.XInternAtom(s_dpy, "_NET_WM_WINDOW_OPACITY", 0);
	if (!op) return;
	if (cloak) {
		unsigned long v = LIN_UNITY_CLOAK_OPACITY; // format 32 props are longs in Xlib
		s_x.XChangeProperty(s_dpy, s_win, op, (XAtom)XA_CARDINAL_, 32, LIN_PROP_MODE_REPLACE,
		                    (const unsigned char *)&v, 1);
	} else {
		s_x.XDeleteProperty(s_dpy, s_win, op);
	}
}

// EWMH _NET_MOVERESIZE_WINDOW for Unity's window, as a client message to the root.
// StaticGravity (10): x/y are the CLIENT origin, whatever frame the WM adds. Bits
// 8/9: x and y present. Bits 12-13 = 2: from a pager/tool, so the WM honours it
// rather than treating it as an application placement request.
typedef struct {
	int type;
	unsigned long serial;
	int send_event;
	void *display;
	XWin window;
	XAtom message_type;
	int format;
	long l[5];
	long pad[12]; // XEvent is 24 longs
} LinClientMessage;

#define LIN_CLIENT_MESSAGE 33
#define LIN_SUBSTRUCTURE_MASK ((1L << 19) | (1L << 20)) // Notify | Redirect

static void
lin_moveresize_client(XDpy dpy, int x, int y, unsigned int w, unsigned int h)
{
	XAtom atom = s_x.XInternAtom(dpy, "_NET_MOVERESIZE_WINDOW", 0);
	if (!atom) return;
	LinClientMessage e;
	memset(&e, 0, sizeof(e));
	e.type = LIN_CLIENT_MESSAGE;
	e.window = s_win;
	e.message_type = atom;
	e.format = 32;
	e.l[0] = 10 | (1L << 8) | (1L << 9) | (2L << 12);
	e.l[1] = x;
	e.l[2] = y;
	if (w > 0 && h > 0) { // bits 10/11: width and height present
		e.l[0] |= (1L << 10) | (1L << 11);
		e.l[3] = (long)w;
		e.l[4] = (long)h;
	}
	s_x.XSendEvent(dpy, s_x.XDefaultRootWindow(dpy), 0, LIN_SUBSTRUCTURE_MASK, &e);
	s_x.XFlush(dpy);
}

static void
lin_move_client(XDpy dpy, int x, int y)
{
	lin_moveresize_client(dpy, x, y, 0, 0);
}

// Undecorate Unity's window while it is cloaked (#332), restore it after.
//
// A decorated X11 window sits inside the WM's frame window, and mutter takes input
// over the WHOLE frame: the input region we give Unity's window (click-through,
// below) was ignored — measured on GNOME/XWayland, a click beside the avatar never
// reached the desktop — and the invisible title bar above the client caught clicks
// as well. Without decorations mutter unframes the window and honours its input
// shape. Unframing hands the client the frame's geometry — up by the title bar and
// taller by as much (measured: +74 px, and Unity saves the size, so it grew on
// every launch) — so the client is put back at its own origin AND size; the
// overlay follows it as always.
//
// Same lifetime as the cloak, for the same reason (see the invariant above): set
// with it at overlay creation, restored with it in displayxr_linux_destroy_weave_window.
static int s_motif_saved;      // 1 = s_motif holds the window's own hints
static int s_motif_had;        // the window had _MOTIF_WM_HINTS before we touched it
static long s_motif[5];
static int s_undecorated;

static void
lin_set_unity_undecorated(int undecorate, int keep_x, int keep_y, unsigned int keep_w,
                          unsigned int keep_h)
{
	XAtom hints = s_x.XInternAtom(s_dpy, "_MOTIF_WM_HINTS", 0);
	if (!hints) return;
	if (undecorate) {
		if (s_undecorated) return;
		XAtom type = 0;
		int format = 0;
		unsigned long n = 0, after = 0;
		unsigned char *prop = NULL;
		s_motif_had = 0;
		if (s_x.XGetWindowProperty(s_dpy, s_win, hints, 0, 5, 0, 0 /* AnyPropertyType */, &type,
		                           &format, &n, &after, &prop) == 0 /* Success */ &&
		    prop && format == 32 && n == 5) {
			memcpy(s_motif, prop, sizeof(s_motif));
			s_motif_had = 1;
		}
		if (prop) s_x.XFree(prop);
		s_motif_saved = 1;
		long v[5] = {2 /* MWM_HINTS_DECORATIONS */, 0, 0 /* none */, 0, 0};
		s_x.XChangeProperty(s_dpy, s_win, hints, hints, 32, LIN_PROP_MODE_REPLACE,
		                    (const unsigned char *)v, 5);
		lin_moveresize_client(s_dpy, keep_x, keep_y, keep_w, keep_h);
		s_undecorated = 1;
		lin_log("[DisplayXR-LNX] Unity's window undecorated while cloaked (click-through)\n");
		return;
	}
	if (!s_undecorated) return;
	if (s_motif_saved && s_motif_had)
		s_x.XChangeProperty(s_dpy, s_win, hints, hints, 32, LIN_PROP_MODE_REPLACE,
		                    (const unsigned char *)s_motif, 5);
	else
		s_x.XDeleteProperty(s_dpy, s_win, hints);
	lin_moveresize_client(s_dpy, keep_x, keep_y, keep_w, keep_h);
	s_undecorated = 0;
	s_motif_saved = 0;
	lin_log("[DisplayXR-LNX] Unity's window decorations restored\n");
}

static XWin
lin_create_argb_overlay(unsigned int w, unsigned int h, int *out_x, int *out_y)
{
	if (!lin_have_compositor()) {
		lin_log("[DisplayXR-LNX] transparent: no compositing manager (_NET_WM_CM_Sn unowned) — "
		        "using the opaque child window\n");
		return 0;
	}
	if (!lin_load_xshape()) {
		lin_log("[DisplayXR-LNX] transparent: libXext (XShape) unavailable — using the opaque "
		        "child window\n");
		return 0;
	}
	LinVisualInfo vi;
	memset(&vi, 0, sizeof(vi));
	if (!s_x.XMatchVisualInfo(s_dpy, s_x.XDefaultScreen(s_dpy), 32, LIN_TRUE_COLOR, &vi) ||
	    !vi.visual) {
		lin_log("[DisplayXR-LNX] transparent: no 32-bit TrueColor visual — using the opaque "
		        "child window\n");
		return 0;
	}

	int x = 0, y = 0;
	lin_app_origin(&x, &y);

	XWin root = s_x.XDefaultRootWindow(s_dpy);
	LinSetWindowAttributes a;
	memset(&a, 0, sizeof(a));
	a.colormap = s_x.XCreateColormap(s_dpy, root, vi.visual, 0 /* AllocNone */);
	a.background_pixel = 0; // fully clear until the first present
	a.border_pixel = 0;     // required with a non-default visual, or BadMatch
	a.override_redirect = 1;
	XWin win = s_x.XCreateWindow(s_dpy, root, x, y, w, h, 0, 32, LIN_INPUT_OUTPUT, vi.visual,
	                             LIN_CW_BACK_PIXEL | LIN_CW_BORDER_PIXEL |
	                                 LIN_CW_OVERRIDE_REDIRECT | LIN_CW_COLORMAP,
	                             &a);
	if (!win) {
		s_x.XFreeColormap(s_dpy, a.colormap);
		lin_log("[DisplayXR-LNX] transparent: XCreateWindow (ARGB) failed — using the opaque "
		        "child window\n");
		return 0;
	}
	// Empty input region: clicks fall through to Unity's window beneath.
	s_shape_combine_rects(s_dpy, win, LIN_SHAPE_INPUT, 0, 0, NULL, 0, LIN_SHAPE_SET, 0);
	s_overlay_cmap = a.colormap;
	*out_x = x;
	*out_y = y;
	return win;
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
		int measured = lin_child_size(&w, &h) && w != 0 && h != 0;
		if (!measured) { w = 1920; h = 1080; }

		if (s_transparent_requested) {
			int x = 0, y = 0;
			s_overlay = lin_create_argb_overlay(w, h, &x, &y);
			if (s_overlay) {
				s_overlay_is_toplevel = 1;
				s_ox = x; s_oy = y;
				lin_set_unity_opacity(1);
				// Size only when measured: never "restore" Unity to the 1920x1080 guess.
				lin_set_unity_undecorated(1, x, y, measured ? w : 0, measured ? h : 0);
				s_x.XMapWindow(s_dpy, s_overlay);
				s_x.XFlush(s_dpy);
				s_x.XSync(s_dpy, 0);
				s_ow = w; s_oh = h;

				char m[256];
				snprintf(m, sizeof(m),
				         "[DisplayXR-LNX] weave window 0x%lx created as an ARGB TOP-LEVEL over "
				         "Unity's window 0x%lx at (%d,%d) %ux%u — transparent, input passes "
				         "through, Unity's window cloaked\n",
				         (unsigned long)s_overlay, unity_win, x, y, w, h);
				lin_log(m);
			}
		}

		if (!s_overlay) {
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
			s_overlay_is_toplevel = 0;
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
	}
	if (!s_dpy || !s_overlay) return 0;
	if (out_display) *out_display = s_dpy;
	if (out_window) *out_window = (unsigned long)s_overlay;
	return 1;
}

// Keep the overlay matched to Unity's client area. For the child, POSITION needs
// no work — a child is clipped to and moves with its parent — so it only ever
// resizes. The transparent top-level has to follow Unity's origin as well.
DISPLAYXR_EXPORT void
displayxr_linux_track_window(void)
{
	if (!s_dpy || !s_overlay || !s_win) return;
	unsigned int w = 0, h = 0;
	if (!lin_child_size(&w, &h) || w == 0 || h == 0) return;
	if (s_overlay_is_toplevel) {
		int x = 0, y = 0;
		if (!lin_app_origin(&x, &y)) return;
		if (w == s_ow && h == s_oh && x == s_ox && y == s_oy) return;
		s_ow = w; s_oh = h; s_ox = x; s_oy = y;
		s_x.XMoveResizeWindow(s_dpy, s_overlay, x, y, w, h);
		s_x.XFlush(s_dpy);
		return;
	}
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
		if (s_overlay_is_toplevel && s_win) {
			int cx = s_ox, cy = s_oy;
			unsigned int cw = 0, ch = 0;
			lin_app_origin(&cx, &cy); // keep the client where the user left it
			lin_child_size(&cw, &ch);
			lin_set_unity_undecorated(0, cx, cy, cw, ch);
			lin_set_unity_opacity(0);
			// The one un-cloak (see the invariant above). Logged on its own line so a
			// customer log shows the revert happened, not just that the overlay died.
			char m[128];
			snprintf(m, sizeof(m), "[DisplayXR-LNX] Unity's window 0x%lx un-cloaked (visible again)\n",
			         (unsigned long)s_win);
			lin_log(m);
		}
		lin_reset_input_region(); // with the cloak: Unity takes clicks everywhere again
		s_x.XDestroyWindow(s_dpy, s_overlay);
		if (s_overlay_cmap) s_x.XFreeColormap(s_dpy, s_overlay_cmap);
		s_x.XFlush(s_dpy);
		lin_log("[DisplayXR-LNX] weave overlay window destroyed\n");
	}
	s_overlay = 0;
	s_overlay_cmap = 0;
	s_overlay_is_toplevel = 0;
	s_ow = s_oh = 0;
	s_ox = s_oy = 0;
}

// Transparent-background request, forwarded from displayxr_set_transparent_background
// (the app's earliest native call, before LifecycleStart creates the overlay). Only
// the NEXT overlay creation reads it; a live overlay keeps its kind.
DISPLAYXR_EXPORT void
displayxr_linux_set_transparent(int enabled)
{
	s_transparent_requested = enabled != 0;
}

// ---------------------------------------------------------------------------
// Click-through + right-drag move (#332)
// ---------------------------------------------------------------------------
//
// CLICK-THROUGH. In transparent mode the overlay takes no input (empty XShape
// input region, above) and Unity's cloaked window beneath it catches every click
// over its whole client area, so nothing outside the avatar reaches the desktop.
// Windows solves this with SetWindowRgn on its overlay; the X11 analogue is an
// XShape INPUT region on Unity's window — only the input shape: what is VISIBLE
// is already decided by the overlay's alpha, and Unity's window is cloaked.
//
// The region is built from the same inputs as the Windows one, with the same
// exports and semantics, so DisplayXRTransparentOverlay and the apps drive both
// the same way:
//   - displayxr_set_overlay_hit_mask: the per-pixel silhouette (C# renders it each
//     frame), RLE'd into rects and stamped into each 3D zone's rect, or the canvas
//     rect, like displayxr_win32.c — except that a single zone with a sub-rect uses
//     that rect where Windows uses the full client (see the mask function).
//   - displayxr_set_overlay_hit_rect: the AABB, used only until the first mask.
//   - displayxr_set_overlay_surround_rect / _mask: 2D surround elements (the
//     speech bubble) unioned in.
// Plus one Linux addition: every live window-space UI layer (HUD) is unioned in.
// A HUD is drawn wherever its rect is, inside the avatar or not, and must take
// its own clicks; on Linux the HUD's own rect is known natively, so no app call
// is needed for it.
//
// Only in transparent top-level mode. The opaque child path keeps Unity's
// default input region: there is nothing to click through to.
//
// RIGHT-DRAG MOVE. Windows moves the overlay on a right-button drag inside the
// region (custom capture drag, #57/#61). Here Unity's window gets the press — the
// region decides that, so a press can only start on the avatar, a HUD or the
// bubble — and DisplayXRTransparentOverlay calls displayxr_linux_drag_window each
// frame while the right button is down. We follow the ROOT pointer (XQueryPointer)
// and move Unity's window with _NET_MOVERESIZE_WINDOW (lin_move_client); the
// overlay follows it in
// displayxr_linux_track_window, and the runtime snaps the weave phase to the
// reachable placement lattice (its x11_placement rule).
//
// Main-thread calls (C# P/Invoke), so a connection of their own: s_dpy is shared
// with the runtime's Vulkan surface and the provider's tracking thread.

typedef struct {
	short x, y;
	unsigned short width, height;
} LinXRect;

#define LIN_SHAPE_UNSORTED 0
#define LIN_HIT_MAX_RECTS  32768 // X request size cap; a real silhouette is far below

static XDpy s_hit_dpy;
static int s_hit_unavailable;
static int s_hit_mask_active;         // a mask has been applied; the AABB stops driving
static unsigned long long s_hit_hash; // last applied region, 0 = none/unknown
static int s_hit_shaped;              // Unity's input region is ours (needs a reset)
static int s_surround_valid;
static LinXRect s_surround_rect;
static uint8_t *s_surround_mask;
static int s_surround_mask_w, s_surround_mask_h;
static int s_surround_dst_x, s_surround_dst_y, s_surround_dst_w, s_surround_dst_h;

extern uint32_t dxr_prov_get_zone_count(void);
extern int dxr_prov_get_zone_rect_px(uint32_t zone, int *x, int *y, int *w, int *h);
extern int displayxr_window_space_ui_get_pending_slot(int slot, void **out_tex, int *out_tex_w,
                                                      int *out_tex_h, float *out_x, float *out_y,
                                                      float *out_lw, float *out_lh, float *out_disp);
#ifndef DXR_WSUI_MAX_SLOTS
#define DXR_WSUI_MAX_SLOTS 4 // displayxr_window_space_ui.h
#endif

static int
lin_hit_ready(void)
{
	if (s_hit_unavailable || !s_overlay_is_toplevel || !s_overlay || !s_win) return 0;
	if (!s_shape_combine_rects) return 0;
	if (!s_hit_dpy) {
		s_hit_dpy = s_x.XOpenDisplay(NULL);
		if (!s_hit_dpy) {
			lin_log("[DisplayXR-LNX] click-through: XOpenDisplay failed — the whole window "
			        "keeps catching clicks\n");
			s_hit_unavailable = 1;
			return 0;
		}
	}
	return 1;
}

typedef struct {
	LinXRect *r;
	int n, cap, oom;
} LinRects;

static void
lin_rects_push(LinRects *v, long x0, long y0, long x1, long y1)
{
	if (v->oom || x1 <= x0 || y1 <= y0) return;
	if (v->n >= LIN_HIT_MAX_RECTS) { v->oom = 1; return; }
	if (v->n >= v->cap) {
		int nc = v->cap ? v->cap * 2 : 1024;
		LinXRect *nr = (LinXRect *)realloc(v->r, (size_t)nc * sizeof(LinXRect));
		if (!nr) { v->oom = 1; return; }
		v->r = nr;
		v->cap = nc;
	}
	if (x0 < -32768) x0 = -32768;
	if (y0 < -32768) y0 = -32768;
	if (x1 > 32767) x1 = 32767;
	if (y1 > 32767) y1 = 32767;
	v->r[v->n].x = (short)x0;
	v->r[v->n].y = (short)y0;
	v->r[v->n].width = (unsigned short)(x1 - x0);
	v->r[v->n].height = (unsigned short)(y1 - y0);
	v->n++;
}

// RLE a mask (non-zero = catch) into rects over [dx,dy,dw,dh], rounding OUTWARD
// (leading edges floor, trailing edges ceil) so the region covers the silhouette —
// the same mapping displayxr_win32.c uses for SetWindowRgn.
static void
lin_rects_from_mask(LinRects *v, const uint8_t *mask, int mw, int mh, long dx, long dy, long dw,
                    long dh)
{
	for (int my = 0; my < mh; my++) {
		const uint8_t *row = mask + (size_t)my * (size_t)mw;
		long top = dy + (long long)my * dh / mh;
		long bottom = dy + ((long long)(my + 1) * dh + mh - 1) / mh;
		int mx = 0;
		while (mx < mw) {
			while (mx < mw && row[mx] == 0) mx++;
			if (mx >= mw) break;
			int x0 = mx;
			while (mx < mw && row[mx] != 0) mx++;
			lin_rects_push(v, dx + (long long)x0 * dw / mw, top,
			               dx + ((long long)mx * dw + mw - 1) / mw, bottom);
		}
	}
}

// Live HUD rects. A window-space layer is placed in fractions of the 3D content
// region (zone 0, or the whole window without one) and shifted by +-disparity/2
// per eye, so widen by that much on each side.
static void
lin_rects_add_wsui(LinRects *v)
{
	int zx = 0, zy = 0, zw = 0, zh = 0;
	if (!dxr_prov_get_zone_rect_px(0, &zx, &zy, &zw, &zh) || zw <= 0 || zh <= 0) return;
	for (int slot = 0; slot < DXR_WSUI_MAX_SLOTS; slot++) {
		float fx = 0, fy = 0, fw = 0, fh = 0, disp = 0;
		if (!displayxr_window_space_ui_get_pending_slot(slot, NULL, NULL, NULL, &fx, &fy, &fw, &fh,
		                                                &disp))
			continue;
		if (fw <= 0.0f || fh <= 0.0f) continue;
		float half = (disp < 0 ? -disp : disp) * 0.5f;
		long x0 = zx + (long)((fx - half) * (float)zw);
		long y0 = zy + (long)(fy * (float)zh);
		long x1 = zx + (long)((fx + fw + half) * (float)zw + 0.999f);
		long y1 = zy + (long)((fy + fh) * (float)zh + 0.999f);
		lin_rects_push(v, x0, y0, x1, y1);
	}
}

static void
lin_rects_add_surround(LinRects *v)
{
	if (s_surround_valid)
		lin_rects_push(v, s_surround_rect.x, s_surround_rect.y,
		               (long)s_surround_rect.x + s_surround_rect.width,
		               (long)s_surround_rect.y + s_surround_rect.height);
	if (s_surround_mask)
		lin_rects_from_mask(v, s_surround_mask, s_surround_mask_w, s_surround_mask_h,
		                    s_surround_dst_x, s_surround_dst_y, s_surround_dst_w,
		                    s_surround_dst_h);
}

// Install `v` as Unity's input region, unless it is the region already there.
static void
lin_apply_input_region(LinRects *v, const char *what)
{
	unsigned long long h = 1469598103934665603ULL; // FNV-1a 64
	const unsigned char *b = (const unsigned char *)v->r;
	size_t nb = (size_t)v->n * sizeof(LinXRect);
	for (size_t i = 0; i < nb; i++) { h ^= b[i]; h *= 1099511628211ULL; }
	h ^= (unsigned long long)(unsigned)v->n; h *= 1099511628211ULL;
	if (h == 0) h = 1;
	if (s_hit_shaped && h == s_hit_hash) return;
	s_hit_hash = h;

	// n == 0 is a valid region: nothing catches (an empty frame of the avatar).
	s_shape_combine_rects(s_hit_dpy, s_win, LIN_SHAPE_INPUT, 0, 0, v->r, v->n, LIN_SHAPE_SET,
	                      LIN_SHAPE_UNSORTED);
	s_x.XFlush(s_hit_dpy);
	if (!s_hit_shaped) {
		char m[160];
		snprintf(m, sizeof(m), "[DisplayXR-LNX] click-through: %s input region on 0x%lx (%d rects)\n",
		         what, (unsigned long)s_win, v->n);
		lin_log(m);
	}
	s_hit_shaped = 1;
}

DISPLAYXR_EXPORT void
displayxr_set_overlay_hit_mask(const uint8_t *mask, int mask_w, int mask_h, int dst_w, int dst_h)
{
	if (!lin_hit_ready()) return;
	if (mask == NULL || mask_w <= 0 || mask_h <= 0 || dst_w <= 0 || dst_h <= 0) {
		// Back to the AABB path: the next hit_rect push drives the region again.
		s_hit_mask_active = 0;
		s_hit_hash = 0;
		return;
	}

	LinRects v = {0};
	int zone_count = 1, use_zones = 0;
	uint32_t pzc = dxr_prov_get_zone_count();
	if (pzc > 1) { zone_count = (int)pzc; use_zones = 1; }
	for (int zi = 0; zi < zone_count; zi++) {
		int32_t cvx = 0, cvy = 0;
		uint32_t cvw = (uint32_t)dst_w, cvh = (uint32_t)dst_h;
		if (use_zones) {
			int rx = 0, ry = 0, rw = 0, rh = 0;
			if (!dxr_prov_get_zone_rect_px((uint32_t)zi, &rx, &ry, &rw, &rh) || rw <= 0 || rh <= 0)
				continue;
			cvx = rx; cvy = ry; cvw = (uint32_t)rw; cvh = (uint32_t)rh;
		} else if (!displayxr_get_canvas_rect_px(&cvx, &cvy, &cvw, &cvh)) {
			// No canvas sub-rect: the 3D zone's own rect (the whole window when no zone
			// is set). The mask is rendered through zone 0's frustum, so it belongs in
			// that rect — an app that keeps a 2D band beside a single zone (the
			// lenovo-avatar bubble) otherwise gets a silhouette stretched over the band.
			// displayxr_win32.c maps this case to the full client instead.
			int rx = 0, ry = 0, rw = 0, rh = 0;
			if (dxr_prov_get_zone_rect_px(0, &rx, &ry, &rw, &rh) && rw > 0 && rh > 0) {
				cvx = rx; cvy = ry; cvw = (uint32_t)rw; cvh = (uint32_t)rh;
			}
		}
		lin_rects_from_mask(&v, mask, mask_w, mask_h, cvx, cvy, cvw, cvh);
	}
	lin_rects_add_surround(&v);
	lin_rects_add_wsui(&v);
	if (v.oom) {
		// Better too much than too little: keep the last region rather than cut the avatar.
		free(v.r);
		return;
	}
	lin_apply_input_region(&v, "silhouette");
	s_hit_mask_active = 1;

	// ~1 Hz, like the Windows hit_mask log: it runs every frame.
	static long long s_last_log_ns;
	long long now = lin_now_ns();
	if (now - s_last_log_ns >= 1000000000LL) {
		s_last_log_ns = now;
		char m[160];
		snprintf(m, sizeof(m), "[DisplayXR-LNX] hit_mask: mask=%dx%d rects=%d dst=%dx%d\n", mask_w,
		         mask_h, v.n, dst_w, dst_h);
		lin_log(m);
	}
	free(v.r);
}

DISPLAYXR_EXPORT void
displayxr_set_overlay_hit_rect(int x, int y, int w, int h)
{
	if (!lin_hit_ready() || s_hit_mask_active) return; // the mask owns the region once it runs
	if (w <= 0 || h <= 0) return;
	LinRects v = {0};
	lin_rects_push(&v, x, y, (long)x + w, (long)y + h);
	lin_rects_add_surround(&v);
	lin_rects_add_wsui(&v);
	if (!v.oom) lin_apply_input_region(&v, "AABB");
	free(v.r);
}

// Windows answers WM_NCHITTEST from this; the X input region needs no per-cursor
// flag. Exported so the shared C# call site binds on Linux too.
DISPLAYXR_EXPORT void
displayxr_set_overlay_hit_active(int active)
{
	(void)active;
}

DISPLAYXR_EXPORT void
displayxr_set_overlay_surround_rect(int x, int y, int w, int h)
{
	if (w <= 0 || h <= 0) {
		s_surround_valid = 0;
		return;
	}
	s_surround_rect.x = (short)x;
	s_surround_rect.y = (short)y;
	s_surround_rect.width = (unsigned short)w;
	s_surround_rect.height = (unsigned short)h;
	s_surround_valid = 1;
}

DISPLAYXR_EXPORT void
displayxr_set_overlay_surround_mask(const uint8_t *mask, int mask_w, int mask_h, int dst_x,
                                    int dst_y, int dst_w, int dst_h)
{
	free(s_surround_mask);
	s_surround_mask = NULL;
	if (mask == NULL || mask_w <= 0 || mask_h <= 0 || dst_w <= 0 || dst_h <= 0) return;
	size_t n = (size_t)mask_w * (size_t)mask_h;
	s_surround_mask = (uint8_t *)malloc(n);
	if (!s_surround_mask) return;
	memcpy(s_surround_mask, mask, n);
	s_surround_mask_w = mask_w;
	s_surround_mask_h = mask_h;
	s_surround_dst_x = dst_x;
	s_surround_dst_y = dst_y;
	s_surround_dst_w = dst_w;
	s_surround_dst_h = dst_h;
}

// Give Unity's window its default input region back. Overlay teardown (s_dpy, the
// provider's thread) — the region must not outlive the cloak it was made for.
static void
lin_reset_input_region(void)
{
	if (s_hit_shaped && s_win && s_shape_combine_mask) {
		s_shape_combine_mask(s_dpy, s_win, LIN_SHAPE_INPUT, 0, 0, 0 /* None */, LIN_SHAPE_SET);
		lin_log("[DisplayXR-LNX] click-through: Unity's input region reset\n");
	}
	s_hit_shaped = 0;
	s_hit_hash = 0;
	s_hit_mask_active = 0;
}

// --- right-drag move --------------------------------------------------------

static int s_drag_active;
static int s_drag_px, s_drag_py;     // root pointer at the press
static int s_drag_wx, s_drag_wy;     // Unity's client origin at the press
static int s_drag_last_dx, s_drag_last_dy; // last offset sent, to skip repeats

static int
lin_root_pointer(int *x, int *y)
{
	XWin r = 0, c = 0;
	int wx = 0, wy = 0;
	unsigned int mask = 0;
	return s_x.XQueryPointer(s_hit_dpy, s_win, &r, &c, x, y, &wx, &wy, &mask) != 0;
}

/// (#332) Right-drag move for the transparent overlay. Call every frame with the
/// right button's state: the first pressed call starts the drag, later ones move
/// Unity's window by how far the pointer has moved, the first released call ends
/// it. Returns 1 while a drag is in progress. No-op outside transparent mode.
DISPLAYXR_EXPORT int
displayxr_linux_drag_window(int right_pressed)
{
	if (!right_pressed) {
		if (s_drag_active) lin_log("[DisplayXR-LNX] drag: end\n");
		s_drag_active = 0;
		return 0;
	}
	if (!lin_hit_ready()) return 0;
	int px = 0, py = 0;
	if (!lin_root_pointer(&px, &py)) return s_drag_active;
	if (!s_drag_active) {
		XWin child = 0;
		if (!s_x.XTranslateCoordinates(s_hit_dpy, s_win, s_x.XDefaultRootWindow(s_hit_dpy), 0, 0,
		                               &s_drag_wx, &s_drag_wy, &child))
			return 0;
		s_drag_px = px;
		s_drag_py = py;
		s_drag_last_dx = s_drag_last_dy = 0;
		s_drag_active = 1;
		lin_log("[DisplayXR-LNX] drag: start\n");
		return 1;
	}
	int dx = px - s_drag_px, dy = py - s_drag_py;
	if (dx == s_drag_last_dx && dy == s_drag_last_dy) return 1;
	s_drag_last_dx = dx;
	s_drag_last_dy = dy;
	lin_move_client(s_hit_dpy, s_drag_wx + dx, s_drag_wy + dy);
	return 1;
}

// ---------------------------------------------------------------------------
// Foreground query (#332)
// ---------------------------------------------------------------------------
//
// Apps gate keyboard shortcuts on displayxr_is_our_process_foreground(). It had
// no Linux export, so every call threw EntryPointNotFoundException — caught by
// the callers' fail-open, i.e. "always foreground", at the cost of an exception
// per caller per frame.
//
// The answer is the EWMH _NET_ACTIVE_WINDOW on the root, compared with Unity's
// window by XID. We deliberately never read a property off the ACTIVE window
// itself (e.g. its _NET_WM_PID): it belongs to another client and can be
// destroyed between our two requests, and the resulting BadWindow would go to
// Xlib's default error handler, which exits the process. Under XWayland, while a
// native Wayland window has focus, mutter points _NET_ACTIVE_WINDOW at a placeholder
// window of its own (measured: 0x400003, no WM_NAME / _NET_WM_PID) rather than None.
// Either way it is not Unity's XID, so that case correctly reads as "not us".
//
// A separate Display connection: s_dpy is borrowed by the runtime for its Vulkan
// surface, so a per-frame query on it would share a connection with runtime
// threads. Results are cached briefly because several scripts ask every frame.

#define XA_WINDOW_ 33L // <X11/Xatom.h>
#define LIN_FOREGROUND_CACHE_NS 50000000LL

static XDpy s_focus_dpy;
static XAtom s_active_atom;
static int s_focus_unavailable;
static int s_focus_cached = 1;
static long long s_focus_cached_at_ns = -1;

static long long
lin_now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static int
lin_query_foreground(void)
{
	if (s_focus_unavailable || !s_win) return 1; // cannot tell: fail open
	if (!s_focus_dpy) {
		s_focus_dpy = s_x.XOpenDisplay(NULL);
		if (s_focus_dpy)
			s_active_atom = s_x.XInternAtom(s_focus_dpy, "_NET_ACTIVE_WINDOW", 1);
		if (!s_focus_dpy || !s_active_atom) {
			lin_log("[DisplayXR-LNX] foreground query: no EWMH _NET_ACTIVE_WINDOW — "
			        "reporting foreground unconditionally\n");
			s_focus_unavailable = 1;
			return 1;
		}
	}

	XAtom actual_type = 0;
	int actual_format = 0;
	unsigned long nitems = 0, bytes_after = 0;
	unsigned char *prop = NULL;
	if (s_x.XGetWindowProperty(s_focus_dpy, s_x.XDefaultRootWindow(s_focus_dpy), s_active_atom,
	                           0, 1, 0, (XAtom)XA_WINDOW_, &actual_type, &actual_format,
	                           &nitems, &bytes_after, &prop) != 0 /* Success == 0 */)
		return 1;
	if (!prop) return 1; // property absent: the WM does not publish it
	XWin active = (nitems >= 1 && actual_format == 32) ? (XWin)(*(unsigned long *)prop) : 0;
	s_x.XFree(prop);
	return active == s_win;
}

DISPLAYXR_EXPORT int
displayxr_is_our_process_foreground(void)
{
	if (!lin_load_xlib()) return 1;
	long long now = lin_now_ns();
	if (s_focus_cached_at_ns >= 0 && now - s_focus_cached_at_ns < LIN_FOREGROUND_CACHE_NS)
		return s_focus_cached;
	s_focus_cached = lin_query_foreground();
	s_focus_cached_at_ns = now;
	return s_focus_cached;
}

#endif // __linux__ && !__ANDROID__
