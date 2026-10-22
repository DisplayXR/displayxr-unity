// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// The plug-in's own clipboard on a native-Wayland player (displayxr_linux_clipboard_*),
// and what making the plug-in's Wayland data device does to SDL's: the parts that
// need no Wayland connection, so they are unit-tested (tests/test_linux_clipboard.c).
// The Wayland side is in displayxr_linux_wayland.c ("File drops" and "Clipboard").
//
// Why the plug-in has a clipboard at all: mutter keeps one data device per client
// on its list of unfocused clients, and the file-drop data device the plug-in makes
// while the player is NOT focused unlinks SDL's for good. SDL then gets no clipboard
// offers, so GUIUtility.systemCopyBuffer cannot read what other apps copy. The
// plug-in's device gets them instead, and these calls read and write the clipboard
// through it, by the Wayland rules (the focused app only).

#pragma once

#include <stddef.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

//! displayxr_linux_clipboard_read / _write: what went wrong (>= 0 is success).
#define DXR_CLIP_UNAVAILABLE -1 //!< no plug-in clipboard here: not a native-Wayland player, no 3D session
#define DXR_CLIP_USE_SDL -2     //!< SDL's clipboard still works (drops never cut it): use GUIUtility.systemCopyBuffer
#define DXR_CLIP_NOT_FOCUSED -3 //!< Wayland gives the clipboard to the focused app only
#define DXR_CLIP_PENDING -4     //!< read: not in yet; call again (the same read goes on)
#define DXR_CLIP_SPACE -5       //!< read: the buffer is too small; *needed says how big (the text is kept)
#define DXR_CLIP_TOO_LARGE -6   //!< more than DXR_CLIP_MAX_BYTES
#define DXR_CLIP_FAILED -7      //!< the clipboard's owner sent nothing in time, or an error
#define DXR_CLIP_INVALID -8     //!< write: a negative length, or a NUL in the text

//! displayxr_linux_clipboard_state: bits.
#define DXR_CLIP_STATE_AVAILABLE 1u //!< the plug-in's clipboard serves reads and writes (SDL's is cut, a session runs)
#define DXR_CLIP_STATE_FOCUSED 2u   //!< the player has the keyboard focus (reads and writes work now)
#define DXR_CLIP_STATE_SDL_CUT 4u   //!< SDL's data device was replaced: systemCopyBuffer cannot read other apps' copies
#define DXR_CLIP_STATE_OWNED 8u     //!< the clipboard holds the plug-in's last write (confirmed by the compositor)

//! The most a read or a write takes (UTF-8 bytes): the runtime's desktop-control
//! clipboard (XR_DXR_desktop_control spec v3) has the same limit.
#define DXR_CLIP_MAX_BYTES (1024 * 1024)

//! A private type next to the text ones on the plug-in's own writes: an offer that
//! lists it is the plug-in's text (read back without a pipe; confirms a write).
#define DXR_CLIP_OWN_MIME "application/x-displayxr-unity-clipboard"

//! The text types, best first. A write offers all of them with the same UTF-8 bytes
//! (as SDL does); a read takes the first one the clipboard has. Plain "text/plain"
//! and the X11 names are read as UTF-8 too (what GNOME apps and XWayland send).
static const char *const dxr_clip_text_mimes[] = {
    "text/plain;charset=utf-8", "UTF8_STRING", "text/plain", "TEXT", "STRING",
};
#define DXR_CLIP_TEXT_MIME_COUNT 5

//! The bit of a text type in an offer's mask (1u << index), or 0 for any other type.
static inline unsigned
dxr_clip_mime_bit(const char *mime)
{
	if (!mime)
		return 0;
	for (int i = 0; i < DXR_CLIP_TEXT_MIME_COUNT; i++)
		if (strcmp(mime, dxr_clip_text_mimes[i]) == 0)
			return 1u << i;
	return 0;
}

//! The text type a read asks for, from an offer's mask: the index into
//! dxr_clip_text_mimes, or -1 when the clipboard holds no text.
static inline int
dxr_clip_pick(unsigned mask)
{
	for (int i = 0; i < DXR_CLIP_TEXT_MIME_COUNT; i++)
		if (mask & (1u << i))
			return i;
	return -1;
}

//! Whether a write of @p len bytes at @p text may go out: 0, DXR_CLIP_INVALID or
//! DXR_CLIP_TOO_LARGE. An empty text (len 0) is valid: it empties the clipboard.
static inline int
dxr_clip_check_write(const char *text, long len)
{
	if (len < 0 || (len > 0 && !text))
		return DXR_CLIP_INVALID;
	if (len > DXR_CLIP_MAX_BYTES)
		return DXR_CLIP_TOO_LARGE;
	if (len > 0 && memchr(text, 0, (size_t)len))
		return DXR_CLIP_INVALID;
	return 0;
}

//! Whether a read or a write may go to the plug-in's data device now: 0 when it may,
//! else the error. @p device: a 3D session runs and the plug-in's data device exists;
//! @p sdl_cut: SDL's was replaced; @p focused: 1, 0, or -1 when not known.
static inline int
dxr_clip_gate(int device, int sdl_cut, int focused)
{
	if (!sdl_cut)
		return DXR_CLIP_USE_SDL; // SDL still gets offers: its clipboard is the app's
	if (!device)
		return DXR_CLIP_UNAVAILABLE;
	if (focused != 1)
		return DXR_CLIP_NOT_FOCUSED;
	return 0;
}

//! What making the plug-in's data device does, by the player's keyboard focus at that
//! moment (@p focused: 1, 0, -1 = not known). Mutter (50) keeps a client's devices on
//! one of two lists: the focused client's, and everybody else's. Making a device
//! unlinks the client's first device on the UNFOCUSED list, for good, and a drag goes
//! to the client's first device on that list:
//!  - focused: SDL's sits on the focused list, so it survives (*sdl_cut = 0); ours
//!    takes drags only while the player keeps the focus, so it is made again once the
//!    focus is lost (*remake = 1), which is when SDL's is unlinked;
//!  - not focused: SDL's is the one unlinked now (*sdl_cut = 1, *remake = 0);
//!  - not known: assume it was (*sdl_cut = 1) and make it again once unfocused.
static inline void
dxr_dd_make_effect(int focused, int *sdl_cut, int *remake)
{
	*sdl_cut = focused != 1;
	*remake = focused != 0;
}

#ifdef __cplusplus
}
#endif
