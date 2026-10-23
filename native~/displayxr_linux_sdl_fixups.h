// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Fixes for the SDL2 snapshot that Unity's Linux player links statically
// (displayxr_linux_sdl_fixups.c). Part of the main plug-in (libdisplayxr_unity.so);
// installed once, at its earliest entry (XRSDKPreInit, else UnityPluginLoad).

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! dxr_sdl_fixups_install(): which fixes are in place. (2u is kept for the IME fix.)
#define DXR_SDLFIX_CLIPBOARD 1u //!< SDL's clipboard-source use-after-free after another app copies (SDL #6007, upstream a6179e85c, #412)
#define DXR_SDLFIX_OUTPUTS 4u   //!< SDL's output-list use-after-free on a monitor re-add (upstream 5f682e77c, #410)
#define DXR_SDLFIX_ALL (DXR_SDLFIX_CLIPBOARD | DXR_SDLFIX_OUTPUTS)

//! Installs every fix this UnityPlayer.so is known and verified for, once (later
//! calls return the same). Env DISPLAYXR_SDL_FIXUPS (dxr_sdl_fixups_parse): unset,
//! 1, on or all = all; 0 or off = none (the kill switch); else a comma-separated
//! list of fix names (clipboard, outputs) = only those. DISPLAYXR_SDL_FIXUPS_LOG=1 also logs
//! SDL's output list after every monitor removal. Returns the DXR_SDLFIX_* bits
//! installed; 0 without a UnityPlayer.so (the editor, a test), for an unknown build,
//! on any mismatch, when the plug-in cannot pin itself, or when off.
unsigned dxr_sdl_fixups_install(void);

//! A DISPLAYXR_SDL_FIXUPS value -> DXR_SDLFIX_* bits. NULL, empty, "1", "on" or
//! "all" = DXR_SDLFIX_ALL; "0" or "off" = 0; else a comma-separated list of exact
//! fix names, blanks around each ignored. Names it does not know are ignored and
//! written to @p unknown, comma-separated (may be NULL).
unsigned dxr_sdl_fixups_parse(const char *value, char *unknown, size_t unknown_cap);

//! One known UnityPlayer.so (displayxr_linux_sdl_fixups.c, the build table). Vaddrs.
struct dxr_sdl_sig {
	uintptr_t at; //!< vaddr
	const unsigned char *bytes;
	unsigned len;
};

#define DXR_SDL_OUTPUTS_SIGS 4
#define DXR_SDL_CLIPBOARD_SIGS 6

struct dxr_sdl_build {
	const char *build_id; //!< hex GNU build-id of UnityPlayer.so
	const char *what;
	uintptr_t video_device; //!< SDL's static SDL_VideoDevice *_this
	// outputs
	unsigned off_num_displays;       //!< SDL_VideoDevice -> num_displays (int)
	unsigned off_displays;           //!< SDL_VideoDevice -> displays (SDL_VideoDisplay *)
	unsigned sizeof_display;         //!< sizeof(SDL_VideoDisplay)
	unsigned off_display_driverdata; //!< SDL_VideoDisplay -> driverdata (SDL_WaylandOutputData *)
	uintptr_t registry_listener;     //!< static const struct wl_registry_listener {global, global_remove}
	uintptr_t registry_global;       //!< its [0]: display_handle_global (Wayland_add_display inlined)
	uintptr_t registry_remove;       //!< its [1]: display_remove_global (Wayland_free_display inlined)
	unsigned off_output_list;        //!< SDL_VideoData -> output_list
	unsigned off_output_registry_id; //!< SDL_WaylandOutputData -> registry_id (uint32)
	unsigned off_output_next;        //!< SDL_WaylandOutputData -> next
	//! The code that reads every address and offset above (see the build table).
	struct dxr_sdl_sig outputs_sigs[DXR_SDL_OUTPUTS_SIGS];
	// clipboard
	uintptr_t data_source_listener;  //!< static const struct wl_data_source_listener (6 entries)
	uintptr_t data_source_cancelled; //!< its [2]: data_source_handle_cancelled (a jmp to Wayland_data_source_destroy)
	unsigned off_driverdata;         //!< SDL_VideoDevice -> driverdata (SDL_VideoData *)
	unsigned off_input;              //!< SDL_VideoData -> input (SDL_WaylandInput *)
	unsigned off_data_device;        //!< SDL_WaylandInput -> data_device (SDL_WaylandDataDevice *)
	unsigned off_selection_source;   //!< SDL_WaylandDataDevice -> selection_source (SDL_WaylandDataSource *)
	//! The code that reads every clipboard address and offset above, _this included (see the build table).
	struct dxr_sdl_sig clipboard_sigs[DXR_SDL_CLIPBOARD_SIGS];
};

//! The build table's entry for a GNU build-id (lower-case hex), or NULL.
const struct dxr_sdl_build *dxr_sdl_fixups_find_build(const char *build_id);

//! Unlinks @p target from the list at @p head (links at byte @p off_next of a node),
//! as upstream SDL 5f682e77c does in Wayland_free_display. 1 if it was in the list.
static inline int
dxr_sdl_output_unlink(void **head, void *target, unsigned off_next)
{
	void **pp = head;
	while (*pp && *pp != target)
		pp = (void **)((char *)*pp + off_next);
	if (!*pp)
		return 0;
	*pp = *(void **)((char *)target + off_next);
	return 1;
}

//! Before SDL frees a cancelled clipboard source: if the data device still holds it as
//! its selection_source (at byte @p off_selection_source of @p data_device), clear it,
//! as upstream SDL a6179e85c does in Wayland_data_source_destroy. 1 if it was cleared.
static inline int
dxr_sdl_clipboard_forget(void *data_device, void *source, unsigned off_selection_source)
{
	void **sel = data_device ? (void **)((char *)data_device + off_selection_source) : 0;
	if (!sel || !source || *sel != source)
		return 0;
	*sel = 0;
	return 1;
}

#ifdef DXR_SDLFIX_TEST
//! Unit tests: install for one build entry, at @p base (a fake image).
unsigned dxr_sdl_fixups_install_build(const struct dxr_sdl_build *b, uintptr_t base);
//! Unit tests: the RELRO range the next install checks tables against (default: all).
void dxr_sdl_fixups_test_relro(uintptr_t lo, uintptr_t hi);
//! Unit tests: what pinning the library answers (1 pinned, 0 failed; -1 = really try).
void dxr_sdl_fixups_test_pin(int result);
#endif

#ifdef __cplusplus
}
#endif
