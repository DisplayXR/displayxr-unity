// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// SDL fixups: work-arounds for bugs in the SDL2 snapshot that Unity's Linux
// player links statically (UnityPlayer.so), for a native-Wayland player. Part of
// the Wayland support library (libdisplayxr_unity_wayland.so); installed once from
// its init, dxr_wl_lib_init (displayxr_linux_sdl_fixups.c).
//
// How a fix reaches SDL: SDL keeps one static const listener table per Wayland
// interface in UnityPlayer.so's RELRO, and libwayland reads the table at every
// event it dispatches. A fix swaps one entry of such a table for a wrapper here
// (mprotect the page RW, an atomic pointer store, mprotect it back) that does what
// the upstream SDL fix does and then calls SDL's own handler. Interposing wl_*
// symbols could not do it: UnityPlayer.so dlopens libwayland and calls it through
// pointers it looked up itself.
//
// Gating: only a UnityPlayer.so whose GNU build-id is in the build table below is
// touched, and a fix goes in only after the table entries it swaps and the SDL code
// that reads every struct offset it uses are byte-verified for that build (the
// table must also lie in UnityPlayer.so's RELRO). Anything else is one log line and
// the player runs exactly as without this module. DXR_SDLFIX=off is the kill switch.
//
// Fixes:
//  - outputs (DXR_SDLFIX_OUTPUTS, #410): a monitor removed and added again crashes the
//    player. SDL's Wayland_free_display frees a removed wl_output's
//    SDL_WaylandOutputData but leaves it linked in SDL_VideoData::output_list; the
//    next wl_output global (the monitor back) makes Wayland_add_display walk the
//    list into the freed node: SIGSEGV in UnityPlayer.so at vaddr 0x26d98c2,
//    `mov 0xe8(%rax),%rax`, node->next of the freed node. Upstream SDL
//    fixed it in 5f682e77c (SDL 2.24.0) by unlinking the node first; the wrapper of
//    SDL's wl_registry global_remove does exactly that, then lets SDL free it.
//
// Adding a fix: its fields and signatures go in struct dxr_sdl_build (one entry per
// UnityPlayer.so), a DXR_SDLFIX_* bit and its name in dxr_sdl_fixups_wanted, and an
// install_<fix>() in the .c that verifies, then swaps.
//
// This header holds the build table and the logic the unit test runs
// (tests/test_sdl_fixups.c); the .c the part that touches the process.

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

//! The fixes (dxr_sdl_fixups_install's result, dxr_sdl_fixups_wanted's).
#define DXR_SDLFIX_OUTPUTS 1u //!< a monitor re-add: SDL's output list use-after-free (upstream 5f682e77c)
#define DXR_SDLFIX_ALL DXR_SDLFIX_OUTPUTS

//! Installs every fix this process's UnityPlayer.so is known and verified for, once
//! (later calls return the first result). Returns the DXR_SDLFIX_* bits installed;
//! 0 without a UnityPlayer.so (the editor, a test), for an unknown build, on any
//! mismatch, or with DXR_SDLFIX=off.
unsigned dxr_sdl_fixups_install(void);

//! Bytes expected at a vaddr of UnityPlayer.so.
struct dxr_sdl_sig {
	uintptr_t at; //!< vaddr
	const unsigned char *bytes;
	unsigned len;
};

#define DXR_SDL_OUTPUTS_SIGS 4

//! One known UnityPlayer.so. Addresses are vaddrs (objdump's), struct offsets bytes.
struct dxr_sdl_build {
	const char *build_id; //!< hex GNU build-id
	const char *what;

	// SDL's video device and its displays.
	uintptr_t video_device;          //!< static SDL_VideoDevice *_this
	unsigned off_num_displays;       //!< SDL_VideoDevice::num_displays (int)
	unsigned off_displays;           //!< SDL_VideoDevice::displays (SDL_VideoDisplay *)
	unsigned sizeof_display;         //!< sizeof(SDL_VideoDisplay)
	unsigned off_display_driverdata; //!< SDL_VideoDisplay::driverdata (SDL_WaylandOutputData *)

	// outputs
	uintptr_t registry_listener;     //!< static const struct wl_registry_listener {global, global_remove}
	uintptr_t registry_global;       //!< its [0]: display_handle_global (Wayland_add_display inlined)
	uintptr_t registry_remove;       //!< its [1]: display_remove_global (Wayland_free_display inlined)
	unsigned off_output_list;        //!< SDL_VideoData::output_list
	unsigned off_output_registry_id; //!< SDL_WaylandOutputData::registry_id (uint32)
	unsigned off_output_next;        //!< SDL_WaylandOutputData::next
	//! The code that reads every address and offset above.
	struct dxr_sdl_sig outputs_sigs[DXR_SDL_OUTPUTS_SIGS];
};

/*
 * The build table.
 */

// Unity 6000.4.6f1, Linux x86_64 player: UnityPlayer.so sha256 c24dc300246a1a53...,
// an SDL2 main-branch snapshot from 2022-04/05 (after 2.0.22, before 2.24.0).

//! display_remove_global, from its entry to the registry-id compare: _this
//! (rip-relative), ->num_displays (+0x2d8), SDL_GetDisplay(i) (call 0x26d0340),
//! ->driverdata (+0x70), ->registry_id (+0x18).
static const unsigned char k_dxr_sdl_6046_remove[] = {
    0x55, 0x41, 0x57, 0x41, 0x56, 0x53, 0x50,       // push rbp, r15, r14, rbx, rax
    0x48, 0x8b, 0x05, 0xf2, 0x9a, 0x24, 0x00,       // mov _this(%rip),%rax
    0x48, 0x85, 0xc0, 0x74, 0x2e,                   // test; je
    0x44, 0x8b, 0xb8, 0xd8, 0x02, 0x00, 0x00,       // mov 0x2d8(%rax),%r15d
    0x45, 0x85, 0xff, 0x0f, 0x8e, 0x98, 0x00, 0x00, // test; jle
    0x00, 0x89, 0xd5, 0x31, 0xdb, 0x89, 0xdf,       // mov %edx,%ebp; xor; mov %ebx,%edi
    0xe8, 0xe2, 0x65, 0xff, 0xff,                   // call SDL_GetDisplay
    0x4c, 0x8b, 0x70, 0x70,                         // mov 0x70(%rax),%r14
    0x41, 0x39, 0x6e, 0x18};                        // cmp %ebp,0x18(%r14)
//! ... and the free of that display's driverdata (no unlink before it: the bug).
static const unsigned char k_dxr_sdl_6046_free[] = {
    0x4c, 0x89, 0xf7,                    // mov %r14,%rdi
    0xff, 0x15, 0x21, 0xd9, 0x10, 0x00}; // call *SDL_free
//! SDL_GetDisplay: _this, ->num_displays (+0x2d8), &->displays[i] (+0x2e0, stride 0x78).
static const unsigned char k_dxr_sdl_6046_getdisplay[] = {
    0x53, 0x48, 0x8b, 0x05, 0xe8, 0x34, 0x25, 0x00, // push rbx; mov _this(%rip),%rax
    0x48, 0x85, 0xc0, 0x74, 0x1d,                   // test; je
    0x8b, 0xb0, 0xd8, 0x02, 0x00, 0x00,             // mov 0x2d8(%rax),%esi
    0x85, 0xff, 0x78, 0x25, 0x39, 0xfe, 0x7e, 0x21, // test; js; cmp; jle
    0x89, 0xf9, 0x48, 0x6b, 0xd9, 0x78,             // mov %edi,%ecx; imul $0x78,%rcx,%rbx
    0x48, 0x03, 0x98, 0xe0, 0x02, 0x00, 0x00};      // add 0x2e0(%rax),%rbx
//! Wayland_add_display's append: d->output_list (+0xd8), node->next (+0xe8). The
//! crash is its `mov 0xe8(%rax),%rax` at 0x26d98c2 on a freed node.
static const unsigned char k_dxr_sdl_6046_walk[] = {
    0x48, 0x8b, 0x83, 0xd8, 0x00, 0x00, 0x00,       // mov 0xd8(%rbx),%rax
    0x48, 0x85, 0xc0, 0x0f, 0x84, 0x88, 0x01, 0x00, // test; je
    0x00, 0x48, 0x89, 0xc3,                         // mov %rax,%rbx
    0x48, 0x8b, 0x80, 0xe8, 0x00, 0x00, 0x00,       // mov 0xe8(%rax),%rax
    0x48, 0x85, 0xc0, 0x75, 0xf1};                  // test; jne

static const struct dxr_sdl_build dxr_sdl_builds[] = {
    {
        .build_id = "4ae6b3f45943aa2cf6ca0d0ac4fdec503e60e55c",
        .what = "Unity 6000.4.6f1 Linux x86_64 player",
        .video_device = 0x2923830,
        .off_num_displays = 0x2d8,
        .off_displays = 0x2e0,
        .sizeof_display = 0x78,
        .off_display_driverdata = 0x70,
        .registry_listener = 0x27c5110,
        .registry_global = 0x26d95c2,
        .registry_remove = 0x26d9d30,
        .off_output_list = 0xd8,
        .off_output_registry_id = 0x18,
        .off_output_next = 0xe8,
        .outputs_sigs = {{0x26d9d30, k_dxr_sdl_6046_remove, sizeof(k_dxr_sdl_6046_remove)},
                         {0x26d9dbe, k_dxr_sdl_6046_free, sizeof(k_dxr_sdl_6046_free)},
                         {0x26d0340, k_dxr_sdl_6046_getdisplay, sizeof(k_dxr_sdl_6046_getdisplay)},
                         {0x26d98af, k_dxr_sdl_6046_walk, sizeof(k_dxr_sdl_6046_walk)}},
    },
};

/*
 * The logic.
 */

#define DXR_SDL_AT(p, off, T) (*(T *)((char *)(p) + (off)))

//! The table entry for a GNU build-id (lower-case hex), or NULL.
static inline const struct dxr_sdl_build *
dxr_sdl_find_build(const char *build_id)
{
	if (!build_id || !*build_id)
		return NULL;
	for (size_t i = 0; i < sizeof(dxr_sdl_builds) / sizeof(dxr_sdl_builds[0]); i++)
		if (strcmp(dxr_sdl_builds[i].build_id, build_id) == 0)
			return &dxr_sdl_builds[i];
	return NULL;
}

//! Which fixes DXR_SDLFIX allows: unset, empty, "1" or "on" = all; "0" or "off" =
//! none (the kill switch); else a comma list of fix names ("outputs") = those only.
static inline unsigned
dxr_sdl_fixups_wanted(const char *env)
{
	if (!env || !*env || strcmp(env, "1") == 0 || strcmp(env, "on") == 0)
		return DXR_SDLFIX_ALL;
	if (strcmp(env, "0") == 0 || strcmp(env, "off") == 0)
		return 0;
	unsigned w = 0;
	for (const char *p = env; *p;) {
		size_t n = strcspn(p, ",");
		if (n == strlen("outputs") && strncmp(p, "outputs", n) == 0)
			w |= DXR_SDLFIX_OUTPUTS;
		p += n;
		if (*p == ',')
			p++;
	}
	return w;
}

//! Every signature matches at @p base.
static inline int
dxr_sdl_sigs_ok(uintptr_t base, const struct dxr_sdl_sig *sigs, int n)
{
	for (int i = 0; i < n; i++)
		if (!sigs[i].bytes || !sigs[i].len ||
		    memcmp((const void *)(base + sigs[i].at), sigs[i].bytes, sigs[i].len) != 0)
			return 0;
	return 1;
}

//! Whether the outputs fix may go into the UnityPlayer.so at @p base whose RELRO is
//! [@p relro_lo, @p relro_hi): NULL if so, else why not (nothing is touched).
static inline const char *
dxr_sdl_outputs_check(const struct dxr_sdl_build *b, uintptr_t base, uintptr_t relro_lo, uintptr_t relro_hi)
{
	uintptr_t t = base + b->registry_listener;
	if (t < relro_lo || t + 2 * sizeof(void *) > relro_hi)
		return "SDL's wl_registry listener is not in UnityPlayer.so's RELRO";
	void *const *table = (void *const *)t;
	if ((uintptr_t)table[0] != base + b->registry_global || (uintptr_t)table[1] != base + b->registry_remove)
		return "SDL's wl_registry listener is not as expected";
	if (!dxr_sdl_sigs_ok(base, b->outputs_sigs, DXR_SDL_OUTPUTS_SIGS))
		return "SDL's code differs from the known build's";
	return NULL;
}

//! The SDL_WaylandOutputData SDL frees for wl_output global @p name: the driverdata
//! of the display with that registry id, found the way Wayland_free_display does.
static inline void *
dxr_sdl_output_for(const struct dxr_sdl_build *b, uintptr_t base, uint32_t name)
{
	char *dev = *(char **)(base + b->video_device);
	if (!dev)
		return NULL;
	int n = DXR_SDL_AT(dev, b->off_num_displays, int);
	char *displays = DXR_SDL_AT(dev, b->off_displays, char *);
	for (int i = 0; displays && i < n; i++) {
		void *data = DXR_SDL_AT(displays + (size_t)i * b->sizeof_display, b->off_display_driverdata, void *);
		if (data && DXR_SDL_AT(data, b->off_output_registry_id, uint32_t) == name)
			return data;
	}
	return NULL;
}

//! Unlinks @p target from the list at @p head (links at @p off_next), as upstream
//! SDL 5f682e77c does in Wayland_free_display. 1 if it was in the list, else 0.
static inline int
dxr_sdl_output_unlink(void **head, void *target, unsigned off_next)
{
	void **pp = head;
	while (*pp && *pp != target)
		pp = &DXR_SDL_AT(*pp, off_next, void *);
	if (!*pp)
		return 0;
	*pp = DXR_SDL_AT(target, off_next, void *);
	return 1;
}

#ifdef DXR_SDLFIX_TEST
//! Unit test: install for @p b at @p base (a fake image), RELRO [@p relro_lo, @p relro_hi).
unsigned dxr_sdl_fixups_install_image(const struct dxr_sdl_build *b, uintptr_t base, uintptr_t relro_lo,
                                      uintptr_t relro_hi);
//! Unit test: forget the last install (the swapped tables stay as they are).
void dxr_sdl_fixups_test_reset(void);
#endif

#ifdef __cplusplus
}
#endif
