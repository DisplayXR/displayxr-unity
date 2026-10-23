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

//! dxr_sdl_fixups_install(): which fixes are in place.
#define DXR_SDLFIX_CLIPBOARD 1u //!< SDL's clipboard-source use-after-free after another app copies (SDL #6007, upstream a6179e85c, #412)
#define DXR_SDLFIX_IME 2u       //!< SDL's zwp_text_input_v3 events recorded for the DisplayXR input fields (#415)
#define DXR_SDLFIX_OUTPUTS 4u   //!< SDL's output-list use-after-free on a monitor re-add (upstream 5f682e77c, #410)
#define DXR_SDLFIX_ALL (DXR_SDLFIX_CLIPBOARD | DXR_SDLFIX_IME | DXR_SDLFIX_OUTPUTS)

//! Installs every fix this UnityPlayer.so is known and verified for, once (later
//! calls return the same). Env DISPLAYXR_SDL_FIXUPS (dxr_sdl_fixups_parse): unset,
//! 1, on or all = all; 0 or off = none (the kill switch); else a comma-separated
//! list of fix names (clipboard, ime, outputs) = only those. DISPLAYXR_SDL_FIXUPS_LOG=1 also logs
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
#define DXR_SDL_IME_SIGS 4

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
	// ime
	uintptr_t text_input_listener; //!< static const struct zwp_text_input_v3_listener (6 entries)
	uintptr_t ti_fns[6];           //!< its entries: enter, leave, preedit_string, commit_string, delete_surrounding_text, done
	//! The code of those six handlers (enter / leave / delete_surrounding_text are a bare `ret`).
	struct dxr_sdl_sig ime_sigs[DXR_SDL_IME_SIGS];
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

/*
 * The IME (DXR_SDLFIX_IME) for the DisplayXR input fields (Runtime/DisplayXRTextIme.cs),
 * through the main plug-in's displayxr_linux_ime_* exports. Unity's main thread only
 * (the thread that dispatches SDL's events). Typed text never reaches a log.
 */

//! dxr_sdl_ime_state() bits.
#define DXR_SDL_IME_AVAILABLE 16u //!< the IME fix is in and SDL's text input has had an event: a field may take it
#define DXR_SDL_IME_OWNED 32u     //!< a field owns the IME's events now
#define DXR_SDL_IME_FOCUS 64u     //!< the text input has the keyboard focus (enter without leave)

unsigned dxr_sdl_ime_state(void);
//! A field takes (1) or gives back (0, no reset) the IME. Taking it queues the IME's
//! current composition first. Returns the state (DXR_SDL_IME_OWNED when taken).
unsigned dxr_sdl_ime_own(int own);
//! The field gives the IME back; with a composition showing, the IME is reset (the
//! field's last cursor rectangle re-sent with it). @p drop_echo: the field took that
//! composition into its text or dropped it, so the IME's flush of it (its first commit
//! after the reset, equal to it, before any other text-input event; within a second)
//! is dropped; anything else first disarms that. Returns the state.
unsigned dxr_sdl_ime_release(int drop_echo);
//! The owning field's caret rectangle (surface-local logical px; w, h > 0), for the IME's
//! candidate window: sent (set_cursor_rectangle + commit) when it changes, and with a
//! reset. Ignored while no field owns the IME. Returns the state.
unsigned dxr_sdl_ime_cursor_rect(int32_t x, int32_t y, int32_t w, int32_t h);
//! The oldest change while a field owns the IME: 1, 0 (none), -1 (a buffer too small:
//! info[4] / info[5] say the bytes; the change stays). @p info (6): cursor_begin,
//! cursor_end, delete_before, delete_after (UTF-8 bytes), commit bytes, composition
//! bytes (-1 = none).
int dxr_sdl_ime_next(char *commit, int commit_cap, char *preedit, int preedit_cap, int32_t *info);

#ifdef DXR_SDLFIX_TEST
//! Unit tests: install for one build entry, at @p base (a fake image).
unsigned dxr_sdl_fixups_install_build(const struct dxr_sdl_build *b, uintptr_t base);
//! Unit tests: the RELRO range the next install checks tables against (default: all).
void dxr_sdl_fixups_test_relro(uintptr_t lo, uintptr_t hi);
//! Unit tests: what the IME's requests go to instead of wl_proxy_marshal (opcodes in order;
//! @p args: x, y, w, h for set_cursor_rectangle (6), else NULL).
void dxr_sdl_fixups_test_ime_reset(void (*send)(void *text_input, unsigned opcode, const int32_t *args));
//! Unit tests: the IME's commits dropped as echoes so far.
unsigned dxr_sdl_fixups_test_ime_echoes(void);
//! Unit tests: what pinning the library answers (1 pinned, 0 failed; -1 = really try).
void dxr_sdl_fixups_test_pin(int result);
#endif

#ifdef __cplusplus
}
#endif
