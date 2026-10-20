// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Native-Wayland player: the client of the DisplayXR GNOME Shell extension
// (window-geometry@displayxr.org), for the window controls a Wayland client
// cannot do for itself. Part of libdisplayxr_unity_wayland.so; libdbus is
// dlopen'd.
//
// All D-Bus traffic runs on ONE worker thread (no caller ever blocks on a round
// trip to gnome-shell): requests are queued, and what the extension reports
// comes back through callbacks on that thread. It follows:
//  - the extension's presence on the bus (it goes away on the lock screen, and
//    with the extension disabled or on another desktop);
//  - every change of our window, from its WindowsChanged signal: its frame in
//    mutter's stage coordinates, and whether a move grab is in progress.
// The extension only ever acts on this process's own window: it takes the
// caller's PID from the bus connection.

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

//! Placement capability bits (GetPlacementCapabilities, extension version 6+).
#define DXR_WL_EXT_CAP_POINTER_DRAG 4u // BeginPointerDrag / EndPointerDrag (version 9)
#define DXR_WL_EXT_CAP_KEEP_ABOVE 32u  // SetKeepAbove (version 15)
#define DXR_WL_EXT_CAP_ACTIVATE 64u    // ActivateWindow (version 15)
#define DXR_WL_EXT_CAP_MINIMIZE 128u   // SetMinimized (version 15)

//! Requests on our own window that a Wayland client cannot make itself
//! (extension version 15), for dxr_wl_ext_own_window().
enum dxr_wl_ext_own_op {
	DXR_WL_EXT_OWN_KEEP_ABOVE = 0, //!< value: keep it above other windows (1) or not (0)
	DXR_WL_EXT_OWN_ACTIVATE = 1,   //!< bring it to the front with focus (restores it too); value unused
	DXR_WL_EXT_OWN_MINIMIZE = 2,   //!< value: minimize (1) or restore without focus (0)
};

//! Our window as the extension last reported it.
typedef struct DxrWlExtWindow {
	int present;         //!< our window is in the extension's list
	int x, y, w, h;      //!< its frame, stage px; w = h = 0 until mutter maps it
	int bx, by, bw, bh;  //!< its main surface ("buffer"), stage px; bw = 0 = not reported
	int moving;          //!< an interactive move/resize grab is in progress
	double device_scale; //!< device px per stage px on its monitor; 0 = not reported
	int mon_x, mon_y, mon_w, mon_h; //!< its monitor's rect, stage px; mon_w = 0 = not reported
	int minimized;       //!< minimized (version 15+); -1 = not reported
	int above;           //!< kept above other windows (version 15+); -1 = not reported
	unsigned long long id; //!< mutter's window id (version 16+): a new one = a new window; 0 = not reported
	int focus;           //!< it has the focus (mutter's focus window); -1 = not reported
} DxrWlExtWindow;

//! Called on the worker thread, never while it holds a lock of its own.
typedef struct DxrWlExtCallbacks {
	//! The extension appeared or went away. caps: its placement capabilities (0
	//! when it cannot say). layout: mutter's layout mode, "logical" or
	//! "physical", or "" when the extension does not report it.
	void (*availability)(int available, unsigned caps, const char *layout);
	//! Our window changed. after_move: this is the state read right after a
	//! MoveWindow we asked for, which `moved` answered.
	void (*window)(const DxrWlExtWindow *w, int after_move, int moved);
	//! A BeginPointerDrag we asked for was answered.
	void (*drag)(int started);
	//! Our window, read afresh (GetWindows) for a dxr_wl_ext_verify_after().
	void (*verify)(const DxrWlExtWindow *w);
	//! A dxr_wl_ext_own_window() request was answered: ok = the extension did
	//! it (0 also when it cannot: not there, too old, refused). seq: the one the
	//! request was made with.
	void (*own_window)(int op, int value, unsigned seq, int ok);
} DxrWlExtCallbacks;

//! Start the worker (once; later calls are ignored).
void dxr_wl_ext_start(const DxrWlExtCallbacks *callbacks);
//! Move our window's frame to (x, y), stage px. Asynchronous; only the latest
//! request is kept. The window callback follows with after_move = 1.
void dxr_wl_ext_move_window(int x, int y);
//! Start a compositor-driven move that follows the pointer while `button` is
//! held. Asynchronous; the drag callback answers.
void dxr_wl_ext_begin_pointer_drag(unsigned button);
void dxr_wl_ext_end_pointer_drag(void);
//! Read our window afresh in delay_ms and report it to the verify callback
//! (placement verification, #361). Only the latest request is kept.
void dxr_wl_ext_verify_after(int delay_ms);
//! An own-window request (enum dxr_wl_ext_own_op). Asynchronous; requests are
//! made in the order asked and each is answered through the own_window callback,
//! with @p seq (the caller's number for it). A queued keep-above takes the latest
//! value and seq (one at most, never dropped, so the latest is always answered);
//! past a handful queued, the oldest activate / minimize is dropped unanswered.
void dxr_wl_ext_own_window(int op, int value, unsigned seq);

#ifdef __cplusplus
}
#endif
