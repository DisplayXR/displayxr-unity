// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// SDL fixups: fixes for the SDL2 snapshot that Unity's Linux player links
// statically (a 2022-04/05 main-branch snapshot between 2.0.22 and 2.24.0), for a
// native-Wayland player. One module in the main plug-in, installed once from its
// earliest entry: XRSDKPreInit (the boot.config pre-init load, before Unity's
// graphics device), else UnityPluginLoad. It needs no libwayland: it only edits
// tables inside UnityPlayer.so.
//
// How: SDL keeps a static const listener table per Wayland interface in
// UnityPlayer.so's RELRO, and libwayland reads the table at every event it
// dispatches. A fix swaps single entries of such a table for a wrapper here
// (mprotect the page RW, an atomic pointer store, mprotect back) that does what
// the upstream SDL fix does and then calls SDL's own handler. libwayland is
// dlopened by UnityPlayer.so, so interposing wl_* symbols could not reach SDL;
// the tables can be reached whenever this library is loaded. In an X11 / XWayland
// player SDL never registers these listeners, so a swapped entry is never called.
//
// Only a UnityPlayer.so whose GNU build-id is in the build table below is
// touched, and a fix is installed only after every table entry it swaps and the
// code it relies on were byte-verified against that build (and the table lies in
// UnityPlayer.so's RELRO); anything else is a log line and the player runs exactly
// as without this module. The struct offsets a fix uses are verified the same way,
// in the instructions that read them. Before any swap this library pins itself
// (RTLD_NODELETE): SDL may call a wrapper until the process ends, so without the
// pin nothing is installed. A swap puts the page's previous protection back (read
// from /proc/self/maps; nothing is swapped if it cannot be read).
//
// Threads: libwayland reads a listener entry once per event and every entry is
// written with one atomic pointer store, so an event sees either SDL's handler or
// the wrapper. The wrappers run on the thread that dispatches SDL's events (Unity's
// main thread), as SDL's handlers do, and touch only what those handlers touch.
//
// Fixes:
// - outputs (DXR_SDLFIX_OUTPUTS, #410): a monitor removed and added again (GNOME
//   Settings off then Revert, a cable blink) crashes the player. SDL's
//   Wayland_free_display frees a removed wl_output's SDL_WaylandOutputData but
//   leaves it linked in SDL_VideoData::output_list; the next wl_output global makes
//   Wayland_add_display walk the list into the freed node: SIGSEGV in UnityPlayer.so
//   at vaddr 0x26d98c2, `mov 0xe8(%rax),%rax` (node->next of the freed node).
//   Upstream SDL 5f682e77c (2.24.0) unlinks the node first; the wrapper of SDL's
//   wl_registry global_remove does exactly that, then lets SDL free it.
// - clipboard (DXR_SDLFIX_CLIPBOARD, #412; SDL #6007): another app copies after a
//   GUIUtility.systemCopyBuffer write, and the next write crashes the player (a read
//   can too). wl_data_source.cancelled makes SDL destroy and free its source
//   (data_source_handle_cancelled -> Wayland_data_source_destroy) while
//   SDL_WaylandDataDevice::selection_source still points to it; the next
//   Wayland_data_device_set_selection destroys the freed source again
//   (wl_proxy_marshal on freed memory: SIGSEGV at vaddr 0x26df02c, Unity's
//   "UnityPlayer.so+0x26de032"), and Wayland_GetClipboardText with no offer text
//   walks its freed MIME list (Unity's "+0x26de152"). Upstream SDL a6179e85c (2.24.0)
//   clears selection_source when it frees that source; the wrapper of SDL's
//   wl_data_source cancelled does exactly that, then lets SDL destroy and free it.
// - ime (DXR_SDLFIX_IME, #415): SDL turns zwp_text_input_v3 preedit_string into
//   SDL_TEXTEDITING and commit_string into one SDL_TEXTINPUT with the full text, but
//   Unity's player loop drops SDL_TEXTEDITING and hands an SDL_TEXTINPUT that is not
//   paired with a key press to the Input System as its first code point only; UGUI
//   and TextMeshPro fields (IMGUI events, made only from key presses) get nothing. Not
//   an SDL bug, so nothing upstream to copy: the wrappers of SDL's six text-input
//   handlers record the protocol's double-buffered state at every 'done' for the
//   DisplayXR input fields (dxr_sdl_ime_*). While a field owns the IME the events do
//   not go on to SDL (the text reaches the app once); otherwise they go on unchanged.
//
// Adding a fix: its fields and code signatures in struct dxr_sdl_build (one table
// entry per UnityPlayer.so), a DXR_SDLFIX_* bit (and DXR_SDLFIX_ALL) and its name in
// k_fix_names, and an install_<fix>() that verifies first, then swaps, called from
// install_with().

#define _GNU_SOURCE // dladdr, RTLD_NODELETE
#include "displayxr_linux_sdl_fixups.h"

#include <dlfcn.h>
#include <elf.h>
#include <limits.h>
#include <link.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

typedef void (*fnptr)(void);

/*
 * The build table.
 */

// Unity 6000.4.6f1, Linux x86_64 player (UnityPlayer.so sha256 c24dc300246a...).

//! display_remove_global, from its entry to the registry-id compare: _this
//! (rip-relative), ->num_displays (+0x2d8), SDL_GetDisplay(i) (call 0x26d0340),
//! ->driverdata (+0x70), ->registry_id (+0x18).
static const unsigned char k_6046_remove[] = {
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
static const unsigned char k_6046_free[] = {
    0x4c, 0x89, 0xf7,                    // mov %r14,%rdi
    0xff, 0x15, 0x21, 0xd9, 0x10, 0x00}; // call *SDL_free
//! SDL_GetDisplay: _this, ->num_displays (+0x2d8), &->displays[i] (+0x2e0, stride 0x78).
static const unsigned char k_6046_getdisplay[] = {
    0x53, 0x48, 0x8b, 0x05, 0xe8, 0x34, 0x25, 0x00, // push rbx; mov _this(%rip),%rax
    0x48, 0x85, 0xc0, 0x74, 0x1d,                   // test; je
    0x8b, 0xb0, 0xd8, 0x02, 0x00, 0x00,             // mov 0x2d8(%rax),%esi
    0x85, 0xff, 0x78, 0x25, 0x39, 0xfe, 0x7e, 0x21, // test; js; cmp; jle
    0x89, 0xf9, 0x48, 0x6b, 0xd9, 0x78,             // mov %edi,%ecx; imul $0x78,%rcx,%rbx
    0x48, 0x03, 0x98, 0xe0, 0x02, 0x00, 0x00};      // add 0x2e0(%rax),%rbx
//! Wayland_add_display's append: d->output_list (+0xd8), node->next (+0xe8); the
//! crash is its `mov 0xe8(%rax),%rax` (0x26d98c2) on a freed node.
static const unsigned char k_6046_walk[] = {
    0x48, 0x8b, 0x83, 0xd8, 0x00, 0x00, 0x00,       // mov 0xd8(%rbx),%rax
    0x48, 0x85, 0xc0, 0x0f, 0x84, 0x88, 0x01, 0x00, // test; je
    0x00, 0x48, 0x89, 0xc3,                         // mov %rax,%rbx
    0x48, 0x8b, 0x80, 0xe8, 0x00, 0x00, 0x00,       // mov 0xe8(%rax),%rax
    0x48, 0x85, 0xc0, 0x75, 0xf1};                  // test; jne

//! data_source_handle_cancelled: `jmp Wayland_data_source_destroy` (0x26dd1e6).
static const unsigned char k_6046_cancelled[] = {0xe9, 0x19, 0x25, 0x00, 0x00};
//! Wayland_data_source_destroy: wl_data_source.destroy (wl_proxy_marshal opcode 1),
//! wl_proxy_destroy, the MIME list's free, SDL_free(source).
static const unsigned char k_6046_source_destroy[] = {
    0x48, 0x85, 0xff, 0x74, 0x43,                   // test %rdi; je
    0x41, 0x56, 0x53, 0x50, 0x48, 0x89, 0xfb,       // push r14, rbx, rax; mov %rdi,%rbx
    0x4c, 0x8b, 0x37, 0x4c, 0x89, 0xf7,             // mov (%rdi),%r14; mov %r14,%rdi
    0xbe, 0x01, 0x00, 0x00, 0x00, 0x31, 0xc0,       // mov $1,%esi; xor %eax,%eax
    0xff, 0x15, 0x5b, 0x6d, 0x24, 0x00,             // call *wl_proxy_marshal
    0x4c, 0x89, 0xf7, 0xff, 0x15, 0x8a, 0x6c, 0x24, // mov %r14,%rdi; call *wl_proxy_destroy
    0x00, 0x48, 0x8d, 0x7b, 0x08,                   // lea 0x8(%rbx),%rdi (the MIME list)
    0xe8, 0x18, 0x00, 0x00, 0x00,                   // call mime_data_list_free
    0x48, 0x89, 0xdf, 0xff, 0x15, 0xc8, 0xa4, 0x10, 0x00}; // mov %rbx,%rdi; call *SDL_free
//! Wayland_GetClipboardText (_this in %rdi): ->driverdata (+0x5c8), ->input (+0xc8),
//! ->data_device (+0x28).
static const unsigned char k_6046_get_device[] = {
    0x48, 0x8b, 0x87, 0xc8, 0x05, 0x00, 0x00, 0x48, 0x85, 0xc0, 0x74, 0x4a, // mov 0x5c8(%rdi),%rax; test; je
    0x48, 0x8b, 0x80, 0xc8, 0x00, 0x00, 0x00, 0x48, 0x85, 0xc0, 0x74, 0x4c, // mov 0xc8(%rax),%rax; test; je
    0x48, 0x8b, 0x58, 0x28};                                                // mov 0x28(%rax),%rbx
//! ... its fallback to the device's ->selection_source (+0x30) when no offer gave text.
static const unsigned char k_6046_get_source[] = {0x4c, 0x8b, 0x63, 0x30, 0x4d, 0x85, 0xe4, 0x74, 0x65};
//! Wayland_data_device_set_selection: the old ->selection_source (+0x30) destroyed as
//! Wayland_data_source_destroy does (the crash: its wl_proxy_marshal on a freed source).
static const unsigned char k_6046_set_old[] = {
    0x4c, 0x8b, 0x7d, 0x30, 0x4d, 0x85, 0xff, 0x74, 0x35, // mov 0x30(%rbp),%r15; test; je
    0x4d, 0x8b, 0x27, 0x4c, 0x89, 0xe7,                   // mov (%r15),%r12; mov %r12,%rdi
    0xbe, 0x01, 0x00, 0x00, 0x00, 0x31, 0xc0,             // mov $1,%esi; xor %eax,%eax
    0xff, 0x15, 0x2e, 0x4f, 0x24, 0x00,                   // call *wl_proxy_marshal (0x26df02c)
    0x4c, 0x89, 0xe7, 0xff, 0x15, 0x5d, 0x4e, 0x24, 0x00, // mov %r12,%rdi; call *wl_proxy_destroy
    0x49, 0x8d, 0x7f, 0x08, 0xe8, 0xeb, 0xe1, 0xff, 0xff, // lea 0x8(%r15),%rdi; call mime_data_list_free
    0x4c, 0x89, 0xff, 0xff, 0x15, 0x9b, 0x86, 0x10, 0x00}; // mov %r15,%rdi; call *SDL_free
//! SDL_SetClipboardText (inlined into Unity's systemCopyBuffer setter): _this
//! (rip-relative, = video_device) and its SetClipboardText(_this, text) (+0x288).
static const unsigned char k_6046_set_text[] = {
    0x4c, 0x8b, 0x35, 0x23, 0xed, 0x0d, 0x01, 0x4d, 0x85, 0xf6, 0x74, 0x41, // mov _this(%rip),%r14; test; je
    0x80, 0x7c, 0x24, 0x1f, 0x40, 0x72, 0x04, 0x48, 0x8b, 0x1c, 0x24,       // (the text's std::string)
    0x48, 0x85, 0xdb, 0x4c, 0x8d, 0x3d, 0x61, 0x36, 0xbf, 0xfe, 0x4c, 0x0f, 0x45, 0xfb,
    0x49, 0x8b, 0x86, 0x88, 0x02, 0x00, 0x00, 0x48, 0x85, 0xc0, 0x74, 0x3a, // mov 0x288(%r14),%rax; test; je
    0x4c, 0x89, 0xf7, 0x4c, 0x89, 0xfe, 0xff, 0xd0};                        // mov %r14,%rdi; mov %r15,%rsi; call *%rax

//! SDL's zwp_text_input_v3 handlers: enter, leave (a bare `ret` each), then
//! text_input_preedit_string's prologue (`movl $1,0x18(%rdi)`: has_preedit).
static const unsigned char k_6046_ti_preedit[] = {
    0xc3, 0xc3,                                     // enter: ret; leave: ret
    0x55, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, // push rbp, r15, r14, r13,
    0x54, 0x53, 0x48, 0x83, 0xec, 0x38,             // push r12, rbx; sub $0x38,%rsp
    0xc7, 0x47, 0x18, 0x01};                        // movl $1,0x18(%rdi)
//! text_input_commit_string's prologue (text in %rdx) ...
static const unsigned char k_6046_ti_commit[] = {
    0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, // push r15, r14, r13, r12
    0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x85, 0xd2}; // push rbx; sub $0x20,%rsp; test %rdx,%rdx
//! ... and its loop: SDL_utf8strlcpy into 32 bytes, then SDL_SendKeyboardText (0x269a82c).
static const unsigned char k_6046_ti_commit_send[] = {
    0x49, 0x8d, 0x34, 0x1f, 0x4c, 0x89, 0xe7,       // lea (%r15,%rbx),%rsi; mov %r12,%rdi
    0xe8, 0xc9, 0x76, 0xfc, 0xff,                   // call SDL_utf8strlcpy
    0x49, 0x89, 0xc5, 0x4c, 0x89, 0xe7,             // mov %rax,%r13; mov %r12,%rdi
    0xe8, 0x7a, 0xf4, 0xfb, 0xff};                  // call SDL_SendKeyboardText
//! delete_surrounding_text (a bare `ret`), then text_input_done's prologue.
static const unsigned char k_6046_ti_done[] = {
    0xc3,                                           // delete_surrounding_text: ret
    0x53, 0x48, 0x89, 0xfb, 0x83, 0x7f, 0x18, 0x00, // push rbx; mov %rdi,%rbx; cmpl $0,0x18(%rdi)
    0x75, 0x10};                                    // jne

static const struct dxr_sdl_build k_builds[] = {
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
        .outputs_sigs = {{0x26d9d30, k_6046_remove, sizeof(k_6046_remove)},
                         {0x26d9dbe, k_6046_free, sizeof(k_6046_free)},
                         {0x26d0340, k_6046_getdisplay, sizeof(k_6046_getdisplay)},
                         {0x26d98af, k_6046_walk, sizeof(k_6046_walk)}},
        .data_source_listener = 0x27c5180,
        .data_source_cancelled = 0x26dacc8,
        .off_driverdata = 0x5c8,
        .off_input = 0xc8,
        .off_data_device = 0x28,
        .off_selection_source = 0x30,
        .clipboard_sigs = {{0x26dacc8, k_6046_cancelled, sizeof(k_6046_cancelled)},
                           {0x26dd1e6, k_6046_source_destroy, sizeof(k_6046_source_destroy)},
                           {0x26df0a0, k_6046_get_device, sizeof(k_6046_get_device)},
                           {0x26df11e, k_6046_get_source, sizeof(k_6046_get_source)},
                           {0x26df016, k_6046_set_old, sizeof(k_6046_set_old)},
                           {0x1844b06, k_6046_set_text, sizeof(k_6046_set_text)}},
        .text_input_listener = 0x27c5238,
        .ti_fns = {0x26db2bc, 0x26db2bd, 0x26db2be, 0x26db36c, 0x26db3c8, 0x26db3c9},
        .ime_sigs = {{0x26db2bc, k_6046_ti_preedit, sizeof(k_6046_ti_preedit)},
                     {0x26db36c, k_6046_ti_commit, sizeof(k_6046_ti_commit)},
                     {0x26db39b, k_6046_ti_commit_send, sizeof(k_6046_ti_commit_send)},
                     {0x26db3c8, k_6046_ti_done, sizeof(k_6046_ti_done)}},
    },
};

/*
 * Common.
 */

static pthread_once_t s_once = PTHREAD_ONCE_INIT;
static const struct dxr_sdl_build *s_b;
static uintptr_t s_base;
static uintptr_t s_relro_lo, s_relro_hi; // UnityPlayer.so's PT_GNU_RELRO
static unsigned s_installed;
static int s_log;

static void
logf_(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fputs("[DisplayXR-SDLFIX] ", stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}

#define AT(p, off, T) (*(T *)((char *)(p) + (off)))

static int
sigs_ok(const struct dxr_sdl_sig *sigs, int n)
{
	for (int i = 0; i < n; i++)
		if (!sigs[i].bytes || memcmp((const void *)(s_base + sigs[i].at), sigs[i].bytes, sigs[i].len) != 0)
			return 0;
	return 1;
}

//! Whether @p n entries of the table at vaddr @p table, from @p first on, lie in
//! UnityPlayer.so's RELRO (so read-only is the protection to restore).
static int
in_relro(uintptr_t table, int first, int n)
{
	uintptr_t lo = s_base + table + (uintptr_t)first * sizeof(void *);
	return lo >= s_relro_lo && lo + (uintptr_t)n * sizeof(void *) <= s_relro_hi;
}

//! The protection (PROT_*) of the one mapping that holds [@p lo, @p hi), from
//! /proc/self/maps; -1 if it cannot be read or one mapping does not hold it all.
static int
mapping_prot(uintptr_t lo, uintptr_t hi)
{
	FILE *f = fopen("/proc/self/maps", "re");
	if (!f)
		return -1;
	char line[PATH_MAX + 256];
	int prot = -1;
	while (fgets(line, sizeof(line), f)) {
		unsigned long a, b;
		char perms[8];
		if (sscanf(line, "%lx-%lx %7s", &a, &b, perms) != 3 || strlen(perms) < 3)
			continue;
		if (lo >= a && lo < b) {
			if (hi <= b)
				prot = (perms[0] == 'r' ? PROT_READ : 0) | (perms[1] == 'w' ? PROT_WRITE : 0) |
				       (perms[2] == 'x' ? PROT_EXEC : 0);
			break;
		}
	}
	fclose(f);
	return prot;
}

enum { SWAP_OK = 0, SWAP_NOT_EXPECTED = -1, SWAP_PROT_UNKNOWN = -2, SWAP_MPROTECT = -3 };

//! Swaps @p n entries of the table at vaddr @p table from @p first on: each must
//! still hold base + @p expect[i]. The page gets its previous protection back.
//! SWAP_OK when done; otherwise nothing changed.
static int
swap_entries(uintptr_t table, int first, int n, const uintptr_t *expect, const fnptr *wrappers)
{
	void **t = (void **)(s_base + table);
	for (int i = 0; i < n; i++)
		if ((uintptr_t)t[first + i] != s_base + expect[i])
			return SWAP_NOT_EXPECTED;
	long pg = sysconf(_SC_PAGESIZE);
	uintptr_t lo = (uintptr_t)&t[first] & ~(uintptr_t)(pg - 1);
	uintptr_t hi = ((uintptr_t)&t[first + n] + (uintptr_t)pg - 1) & ~(uintptr_t)(pg - 1);
	int prev = mapping_prot(lo, hi);
	if (prev < 0)
		return SWAP_PROT_UNKNOWN;
	if (mprotect((void *)lo, hi - lo, prev | PROT_READ | PROT_WRITE) != 0)
		return SWAP_MPROTECT;
	for (int i = 0; i < n; i++)
		__atomic_store_n(&t[first + i], (void *)wrappers[i], __ATOMIC_SEQ_CST);
	if (mprotect((void *)lo, hi - lo, prev) != 0)
		logf_("could not give SDL's listener table its previous protection back (it stays writable)");
	return SWAP_OK;
}

//! Why a swap of SDL's @p table (e.g. "wl_registry listener") was refused. Called
//! during the one install (pthread_once), so one static buffer is enough.
static const char *
swap_why(int r, const char *table)
{
	static char why[160];
	snprintf(why, sizeof(why),
	         r == SWAP_PROT_UNKNOWN ? "the protection of SDL's %s's page could not be read"
	         : r == SWAP_MPROTECT   ? "SDL's %s's page could not be made writable"
	                                : "SDL's %s is not as expected",
	         table);
	return why;
}

/*
 * Outputs: the monitor re-add use-after-free.
 */

typedef void (*registry_remove_fn)(void *data, void *registry, uint32_t name);
static registry_remove_fn s_orig_registry_remove;

//! The SDL_WaylandOutputData SDL frees for wl_output global @p name: the driverdata
//! of the display with that registry id, found the way Wayland_free_display does.
static void *
output_for(uint32_t name)
{
	char *dev = *(char **)(s_base + s_b->video_device);
	if (!dev)
		return NULL;
	int n = AT(dev, s_b->off_num_displays, int);
	char *displays = AT(dev, s_b->off_displays, char *);
	for (int i = 0; displays && i < n; i++) {
		void *data = AT(displays + (size_t)i * s_b->sizeof_display, s_b->off_display_driverdata, void *);
		if (data && AT(data, s_b->off_output_registry_id, uint32_t) == name)
			return data;
	}
	return NULL;
}

static void
log_output_list(void *video_data)
{
	char buf[256];
	size_t len = 0;
	buf[0] = 0;
	void *node = AT(video_data, s_b->off_output_list, void *);
	for (int k = 0; node && k < 16 && len < sizeof(buf) - 16; k++) {
		len += (size_t)snprintf(buf + len, sizeof(buf) - len, " %u", AT(node, s_b->off_output_registry_id, uint32_t));
		node = AT(node, s_b->off_output_next, void *);
	}
	logf_("SDL's output list now:%s", buf[0] ? buf : " (empty)");
}

//! wl_registry.global_remove for SDL (its data: SDL_VideoData).
static void
fix_registry_remove(void *data, void *registry, uint32_t name)
{
	void *target = data ? output_for(name) : NULL;
	if (target && dxr_sdl_output_unlink(&AT(data, s_b->off_output_list, void *), target, s_b->off_output_next))
		logf_("monitor removed (wl_output global %u): unlinked SDL's output data before SDL frees it", name);
	s_orig_registry_remove(data, registry, name); // SDL: delete the display, free the data
	if (target && s_log)
		log_output_list(data);
}

static int
install_outputs(void)
{
	const struct dxr_sdl_build *b = s_b;
	if (!in_relro(b->registry_listener, 0, 2)) {
		logf_("monitor re-add fix NOT installed: SDL's wl_registry listener is not in UnityPlayer.so's RELRO");
		return 0;
	}
	void *const *t = (void *const *)(s_base + b->registry_listener);
	if ((uintptr_t)t[0] != s_base + b->registry_global) {
		logf_("monitor re-add fix NOT installed: SDL's wl_registry listener is not as expected");
		return 0;
	}
	if (!sigs_ok(b->outputs_sigs, DXR_SDL_OUTPUTS_SIGS)) {
		logf_("monitor re-add fix NOT installed: SDL's code differs from the known build's");
		return 0;
	}
	s_orig_registry_remove = (registry_remove_fn)(s_base + b->registry_remove);
	const uintptr_t expect[1] = {b->registry_remove};
	const fnptr wrap[1] = {(fnptr)fix_registry_remove};
	int r = swap_entries(b->registry_listener, 1, 1, expect, wrap);
	if (r != SWAP_OK) {
		logf_("monitor re-add fix NOT installed: %s", swap_why(r, "wl_registry listener"));
		return 0;
	}
	return 1;
}

/*
 * Install.
 */

struct find_ctx {
	uintptr_t base, relro_lo, relro_hi;
	char build_id[64];
	int found;
};

static int
find_unityplayer(struct dl_phdr_info *info, size_t size, void *arg)
{
	(void)size;
	struct find_ctx *c = arg;
	const char *nm = info->dlpi_name ? info->dlpi_name : "";
	const char *slash = strrchr(nm, '/');
	if (strcmp(slash ? slash + 1 : nm, "UnityPlayer.so") != 0)
		return 0;
	c->base = info->dlpi_addr;
	c->found = 1;
	for (int i = 0; i < info->dlpi_phnum; i++) {
		const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
		if (ph->p_type == PT_GNU_RELRO) {
			c->relro_lo = info->dlpi_addr + ph->p_vaddr;
			c->relro_hi = c->relro_lo + ph->p_memsz;
		}
		if (ph->p_type != PT_NOTE)
			continue;
		const char *p = (const char *)(info->dlpi_addr + ph->p_vaddr), *end = p + ph->p_memsz;
		while (p + sizeof(ElfW(Nhdr)) <= end) {
			const ElfW(Nhdr) *nh = (const ElfW(Nhdr) *)p;
			const char *nname = p + sizeof(*nh);
			const unsigned char *desc = (const unsigned char *)nname + ((nh->n_namesz + 3) & ~3u);
			if (nh->n_type == NT_GNU_BUILD_ID && nh->n_namesz == 4 && memcmp(nname, "GNU", 4) == 0)
				for (unsigned j = 0; j < nh->n_descsz && j < 31; j++)
					snprintf(c->build_id + 2 * j, 3, "%02x", desc[j]);
			p = (const char *)desc + ((nh->n_descsz + 3) & ~3u);
		}
	}
	return 1;
}

/*
 * Clipboard: the cancelled-source use-after-free (SDL #6007).
 */

typedef void (*source_cancelled_fn)(void *data, void *source);
static source_cancelled_fn s_orig_source_cancelled;

//! SDL's data device (SDL_WaylandDataDevice), as Wayland_GetClipboardText finds it.
static void *
sdl_data_device(void)
{
	char *dev = *(char **)(s_base + s_b->video_device);
	char *video_data = dev ? AT(dev, s_b->off_driverdata, char *) : NULL;
	char *input = video_data ? AT(video_data, s_b->off_input, char *) : NULL;
	return input ? AT(input, s_b->off_data_device, void *) : NULL;
}

//! wl_data_source.cancelled for SDL (its data: the SDL_WaylandDataSource).
static void
fix_source_cancelled(void *data, void *source)
{
	if (dxr_sdl_clipboard_forget(sdl_data_device(), data, s_b->off_selection_source))
		logf_("another app took the clipboard: cleared SDL's selection source before SDL frees it");
	s_orig_source_cancelled(data, source); // SDL: destroy the wl_data_source, free the source
}

static int
install_clipboard(void)
{
	const struct dxr_sdl_build *b = s_b;
	if (!in_relro(b->data_source_listener, 0, 6)) {
		logf_("clipboard fix NOT installed: SDL's wl_data_source listener is not in UnityPlayer.so's RELRO");
		return 0;
	}
	if (!sigs_ok(b->clipboard_sigs, DXR_SDL_CLIPBOARD_SIGS)) {
		logf_("clipboard fix NOT installed: SDL's code differs from the known build's");
		return 0;
	}
	s_orig_source_cancelled = (source_cancelled_fn)(s_base + b->data_source_cancelled);
	const uintptr_t expect[1] = {b->data_source_cancelled};
	const fnptr wrap[1] = {(fnptr)fix_source_cancelled};
	int r = swap_entries(b->data_source_listener, 2, 1, expect, wrap);
	if (r != SWAP_OK) {
		logf_("clipboard fix NOT installed: %s", swap_why(r, "wl_data_source listener"));
		return 0;
	}
	return 1;
}

/*
 * IME: SDL's zwp_text_input_v3 events, recorded for the DisplayXR input fields.
 *
 * The protocol's preedit_string, commit_string and delete_surrounding_text are
 * double-buffered and take effect at 'done' (the preedit is empty unless one came).
 * One record per 'done' while a field owns the IME; the field applies it in the
 * protocol's order (the old composition goes, the text around the caret is deleted,
 * the commit is inserted, the new composition shows). While owned, the four state
 * events do not go on to SDL; enter / leave always do. A field that gives the IME
 * back with a composition showing resets it (disable + commit, enable +
 * set_content_type [+ set_cursor_rectangle] + commit on SDL's own text input; SDL
 * keeps it enabled the whole time otherwise and flushes the requests with its next
 * event pump). An IME such as ibus-hangul answers the reset by committing that
 * composition (its "flush"); the field already took it, so that one commit is
 * dropped and reaches neither Unity nor the next field. Only the flush is: the IME's
 * first commit after the reset, equal to the composition the field took, with no
 * new composition, commit, delete or focus change before it (and within a second,
 * a backstop). An IME that does not flush never costs a genuine commit: whatever it
 * sends first disarms the drop. The cursor rectangle is the one the owning field
 * last reported (its caret, as Input.compositionCursorPos): Unity never calls
 * SDL_SetTextInputRect (6000.4.6f1 does not even link it; SDL's
 * Wayland_SetTextInputRect is reachable only through its unused device slot), so
 * SDL itself never sends one. Everything runs on the thread that dispatches SDL's
 * events (Unity's main thread), as do the fields' calls; the mutex only keeps a stray
 * caller safe.
 */

typedef void (*fnptr_ti_surface)(void *, void *, void *);
typedef void (*fnptr_ti_preedit)(void *, void *, const char *, int32_t, int32_t);
typedef void (*fnptr_ti_commit)(void *, void *, const char *);
typedef void (*fnptr_ti_delete)(void *, void *, uint32_t, uint32_t);
typedef void (*fnptr_ti_done)(void *, void *, uint32_t);

typedef struct ImeRec {
	char *commit;  // NULL: none
	char *preedit; // NULL: empty
	int32_t cursor_begin, cursor_end;
	uint32_t del_before, del_after;
} ImeRec;

#define IME_QUEUE 64
static pthread_mutex_t s_ime_mx = PTHREAD_MUTEX_INITIALIZER;
static ImeRec s_q[IME_QUEUE];
static int s_q_head, s_q_len;
static fnptr s_ti_orig[6];
static void *s_ti_proxy; // SDL's zwp_text_input_v3, from its events (for the reset)
static int s_owned, s_ti_focus;
static struct {
	char *commit;
	char *preedit;
	int has_preedit;
	int32_t cursor_begin, cursor_end;
	uint32_t del_before, del_after;
	int owned_any;
} s_pend;
static char *s_cur_preedit; // the IME's composition as of the last 'done'
static int32_t s_cur_begin, s_cur_end;
static char *s_echo; // armed at a reset: the composition the field took (the IME's flush of it is dropped)
static int64_t s_echo_until_ns;
static unsigned s_echoes;
static int32_t s_rect[4]; // the owning field's caret rectangle (surface-local logical px)
static int s_has_rect;
//! The requests (tests replace it): opcode, and for set_cursor_rectangle (6) x, y, w, h.
static void (*s_ime_send)(void *text_input, unsigned opcode, const int32_t *args);

//! Disarms the flush drop: the IME sent something else first (or the focus changed).
static void
echo_disarm_locked(void)
{
	free(s_echo);
	s_echo = NULL;
}

static int64_t
now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void
rec_free(ImeRec *r)
{
	free(r->commit);
	free(r->preedit);
	memset(r, 0, sizeof(*r));
}

static void
q_push(ImeRec *r) // takes the strings; drops the oldest when full
{
	if (s_q_len == IME_QUEUE) {
		rec_free(&s_q[s_q_head]);
		s_q_head = (s_q_head + 1) % IME_QUEUE;
		s_q_len--;
	}
	s_q[(s_q_head + s_q_len) % IME_QUEUE] = *r;
	s_q_len++;
}

static void
q_clear(void)
{
	while (s_q_len) {
		rec_free(&s_q[s_q_head]);
		s_q_head = (s_q_head + 1) % IME_QUEUE;
		s_q_len--;
	}
	s_q_head = 0;
}

//! Every event: remember SDL's proxy; whether a field owns the IME.
static int
ti_event_locked(void *ti)
{
	s_ti_proxy = ti;
	return s_owned;
}

static void
fix_ti_enter(void *data, void *ti, void *surface)
{
	pthread_mutex_lock(&s_ime_mx);
	ti_event_locked(ti);
	s_ti_focus = 1;
	echo_disarm_locked(); // a new focus: no flush of the old one comes any more
	pthread_mutex_unlock(&s_ime_mx);
	((fnptr_ti_surface)s_ti_orig[0])(data, ti, surface);
}

static void
fix_ti_leave(void *data, void *ti, void *surface)
{
	pthread_mutex_lock(&s_ime_mx);
	ti_event_locked(ti);
	s_ti_focus = 0;
	echo_disarm_locked();
	pthread_mutex_unlock(&s_ime_mx);
	((fnptr_ti_surface)s_ti_orig[1])(data, ti, surface);
}

static void
fix_ti_preedit(void *data, void *ti, const char *text, int32_t begin, int32_t end)
{
	pthread_mutex_lock(&s_ime_mx);
	int owned = ti_event_locked(ti);
	if (s_echo && text && *text && strcmp(text, s_echo) != 0)
		echo_disarm_locked(); // a new composition: the IME did not flush (an empty one may come with the flush)
	free(s_pend.preedit);
	s_pend.preedit = (text && *text) ? strdup(text) : NULL;
	s_pend.has_preedit = 1;
	s_pend.cursor_begin = begin;
	s_pend.cursor_end = end;
	s_pend.owned_any |= owned;
	pthread_mutex_unlock(&s_ime_mx);
	if (!owned)
		((fnptr_ti_preedit)s_ti_orig[2])(data, ti, text, begin, end);
}

static void
fix_ti_commit(void *data, void *ti, const char *text)
{
	pthread_mutex_lock(&s_ime_mx);
	int owned = ti_event_locked(ti);
	if (s_echo && now_ns() > s_echo_until_ns)
		echo_disarm_locked();
	int flush = 0;
	if (text && *text) { // the IME's first commit after the reset: the flush, or proof that there is none
		flush = s_echo && strcmp(text, s_echo) == 0;
		echo_disarm_locked();
	}
	if (flush) {
		s_echoes++;
		pthread_mutex_unlock(&s_ime_mx);
		if (s_log)
			logf_("IME: dropped the IME's own commit of a composition the field took itself");
		return;
	}
	if (text && *text) {
		size_t a = s_pend.commit ? strlen(s_pend.commit) : 0, b = strlen(text);
		char *n = realloc(s_pend.commit, a + b + 1);
		if (n) {
			memcpy(n + a, text, b + 1);
			s_pend.commit = n;
		}
	}
	s_pend.owned_any |= owned;
	pthread_mutex_unlock(&s_ime_mx);
	if (!owned)
		((fnptr_ti_commit)s_ti_orig[3])(data, ti, text);
}

static void
fix_ti_delete(void *data, void *ti, uint32_t before, uint32_t after)
{
	pthread_mutex_lock(&s_ime_mx);
	int owned = ti_event_locked(ti);
	echo_disarm_locked();
	s_pend.del_before = before;
	s_pend.del_after = after;
	s_pend.owned_any |= owned;
	pthread_mutex_unlock(&s_ime_mx);
	if (!owned)
		((fnptr_ti_delete)s_ti_orig[4])(data, ti, before, after);
}

static void
fix_ti_done(void *data, void *ti, uint32_t serial)
{
	pthread_mutex_lock(&s_ime_mx);
	int owned = ti_event_locked(ti);
	free(s_cur_preedit);
	s_cur_preedit = s_pend.has_preedit ? s_pend.preedit : NULL;
	s_cur_begin = s_pend.has_preedit ? s_pend.cursor_begin : 0;
	s_cur_end = s_pend.has_preedit ? s_pend.cursor_end : 0;
	if (owned || s_pend.owned_any) {
		ImeRec r = {
		    .commit = s_pend.commit,
		    .preedit = s_cur_preedit ? strdup(s_cur_preedit) : NULL,
		    .cursor_begin = s_cur_begin,
		    .cursor_end = s_cur_end,
		    .del_before = s_pend.del_before,
		    .del_after = s_pend.del_after,
		};
		q_push(&r);
	} else {
		free(s_pend.commit);
	}
	memset(&s_pend, 0, sizeof(s_pend));
	pthread_mutex_unlock(&s_ime_mx);
	if (!owned)
		((fnptr_ti_done)s_ti_orig[5])(data, ti, serial);
}

//! wl_proxy_marshal from the libwayland-client SDL loaded (never loads it: no NEEDED,
//! and none in an X11 player, where there is no SDL text input to reset anyway).
static void
send_marshal(void *text_input, unsigned opcode, const int32_t *args)
{
	static void (*marshal)(void *, uint32_t, ...);
	static int tried;
	if (!tried) {
		tried = 1;
		void *wl = dlopen("libwayland-client.so.0", RTLD_NOW | RTLD_NOLOAD);
		if (wl)
			marshal = (void (*)(void *, uint32_t, ...))dlsym(wl, "wl_proxy_marshal");
	}
	if (!marshal)
		return;
	if (opcode == 5) // set_content_type(hint 0, purpose 0): what SDL sends
		marshal(text_input, opcode, 0u, 0u);
	else if (opcode == 6 && args) // set_cursor_rectangle(x, y, width, height)
		marshal(text_input, opcode, args[0], args[1], args[2], args[3]);
	else
		marshal(text_input, opcode);
}

static int
install_ime(void)
{
	const struct dxr_sdl_build *b = s_b;
	if (!in_relro(b->text_input_listener, 0, 6)) {
		logf_("IME fix NOT installed: SDL's zwp_text_input_v3 listener is not in UnityPlayer.so's RELRO");
		return 0;
	}
	if (!sigs_ok(b->ime_sigs, DXR_SDL_IME_SIGS)) {
		logf_("IME fix NOT installed: SDL's code differs from the known build's");
		return 0;
	}
	for (int i = 0; i < 6; i++)
		s_ti_orig[i] = (fnptr)(s_base + b->ti_fns[i]);
	const fnptr wrap[6] = {(fnptr)fix_ti_enter,  (fnptr)fix_ti_leave,  (fnptr)fix_ti_preedit,
	                       (fnptr)fix_ti_commit, (fnptr)fix_ti_delete, (fnptr)fix_ti_done};
	int r = swap_entries(b->text_input_listener, 0, 6, b->ti_fns, wrap);
	if (r != SWAP_OK) {
		logf_("IME fix NOT installed: %s", swap_why(r, "zwp_text_input_v3 listener"));
		return 0;
	}
	if (!s_ime_send)
		s_ime_send = send_marshal;
	return 1;
}

unsigned
dxr_sdl_ime_state(void)
{
	pthread_mutex_lock(&s_ime_mx);
	unsigned st = (((s_installed & DXR_SDLFIX_IME) && s_ti_proxy) ? DXR_SDL_IME_AVAILABLE : 0) |
	              (s_owned ? DXR_SDL_IME_OWNED : 0) | (s_ti_focus ? DXR_SDL_IME_FOCUS : 0);
	pthread_mutex_unlock(&s_ime_mx);
	return st;
}

unsigned
dxr_sdl_ime_own(int own)
{
	pthread_mutex_lock(&s_ime_mx);
	if (!(s_installed & DXR_SDLFIX_IME) || !s_ti_proxy)
		own = 0;
	int was = s_owned;
	s_owned = own ? 1 : 0;
	if (s_owned && !was) {
		q_clear();
		if (s_cur_preedit) {
			ImeRec r = {.preedit = strdup(s_cur_preedit), .cursor_begin = s_cur_begin, .cursor_end = s_cur_end};
			q_push(&r);
		}
	} else if (!s_owned && was) {
		q_clear();
	}
	pthread_mutex_unlock(&s_ime_mx);
	return dxr_sdl_ime_state();
}

unsigned
dxr_sdl_ime_release(int drop_echo)
{
	pthread_mutex_lock(&s_ime_mx);
	s_owned = 0;
	q_clear();
	echo_disarm_locked();
	void *ti = s_ti_proxy;
	void (*send)(void *, unsigned, const int32_t *) = s_ime_send;
	int reset = ti && send && s_cur_preedit;
	if (reset && drop_echo) {
		s_echo = strdup(s_cur_preedit);
		s_echo_until_ns = now_ns() + 1000000000LL; // a backstop only: the first event after the reset decides
	}
	free(s_cur_preedit);
	s_cur_preedit = NULL;
	int32_t rect[4];
	int has_rect = s_has_rect;
	memcpy(rect, s_rect, sizeof(rect));
	s_has_rect = 0; // the next owner reports its own
	if (reset) {
		// zwp_text_input_v3 requests, as SDL's StopTextInput + StartTextInput send them: 2 disable,
		// 7 commit, 1 enable, 5 set_content_type, 6 set_cursor_rectangle (when known), 7 commit.
		send(ti, 2, NULL);
		send(ti, 7, NULL);
		send(ti, 1, NULL);
		send(ti, 5, NULL);
		if (has_rect)
			send(ti, 6, rect);
		send(ti, 7, NULL);
	}
	pthread_mutex_unlock(&s_ime_mx);
	if (reset && s_log)
		logf_("IME: reset (disable / enable)%s", has_rect ? ", cursor rectangle re-sent" : "");
	return dxr_sdl_ime_state();
}

unsigned
dxr_sdl_ime_cursor_rect(int32_t x, int32_t y, int32_t w, int32_t h)
{
	pthread_mutex_lock(&s_ime_mx);
	if (s_owned && w > 0 && h > 0) {
		int changed = !s_has_rect || s_rect[0] != x || s_rect[1] != y || s_rect[2] != w || s_rect[3] != h;
		s_rect[0] = x, s_rect[1] = y, s_rect[2] = w, s_rect[3] = h;
		s_has_rect = 1;
		if (changed && s_ti_proxy && s_ime_send) {
			// What SDL_SetTextInputRect would send: set_cursor_rectangle + commit.
			s_ime_send(s_ti_proxy, 6, s_rect);
			s_ime_send(s_ti_proxy, 7, NULL);
		}
	}
	pthread_mutex_unlock(&s_ime_mx);
	return dxr_sdl_ime_state();
}

int
dxr_sdl_ime_next(char *commit, int commit_cap, char *preedit, int preedit_cap, int32_t *info)
{
	pthread_mutex_lock(&s_ime_mx);
	if (!s_q_len) {
		pthread_mutex_unlock(&s_ime_mx);
		return 0;
	}
	ImeRec *r = &s_q[s_q_head];
	int cl = r->commit ? (int)strlen(r->commit) : -1, pl = r->preedit ? (int)strlen(r->preedit) : -1;
	if (info) {
		info[0] = r->cursor_begin;
		info[1] = r->cursor_end;
		info[2] = (int32_t)r->del_before;
		info[3] = (int32_t)r->del_after;
		info[4] = cl;
		info[5] = pl;
	}
	if ((cl >= 0 && (!commit || commit_cap <= cl)) || (pl >= 0 && (!preedit || preedit_cap <= pl))) {
		pthread_mutex_unlock(&s_ime_mx);
		return -1;
	}
	if (commit && commit_cap > 0) {
		if (cl >= 0)
			memcpy(commit, r->commit, (size_t)cl + 1);
		else
			commit[0] = 0;
	}
	if (preedit && preedit_cap > 0) {
		if (pl >= 0)
			memcpy(preedit, r->preedit, (size_t)pl + 1);
		else
			preedit[0] = 0;
	}
	rec_free(r);
	s_q_head = (s_q_head + 1) % IME_QUEUE;
	s_q_len--;
	pthread_mutex_unlock(&s_ime_mx);
	return 1;
}

//! The fixes by name (DISPLAYXR_SDL_FIXUPS).
static const struct {
	const char *name;
	unsigned bit;
} k_fix_names[] = {
    {"clipboard", DXR_SDLFIX_CLIPBOARD},
    {"ime", DXR_SDLFIX_IME},
    {"outputs", DXR_SDLFIX_OUTPUTS},
};

static int
is_blank(char c)
{
	return c == ' ' || c == '\t';
}

//! [@p s, @p s + @p n) is exactly @p word.
static int
token_is(const char *s, size_t n, const char *word)
{
	return strlen(word) == n && strncmp(s, word, n) == 0;
}

unsigned
dxr_sdl_fixups_parse(const char *value, char *unknown, size_t unknown_cap)
{
	size_t ulen = 0;
	if (unknown && unknown_cap)
		unknown[0] = 0;
	const char *s = value ? value : "";
	while (is_blank(*s))
		s++;
	size_t n = strlen(s);
	while (n && is_blank(s[n - 1]))
		n--;
	if (n == 0 || token_is(s, n, "1") || token_is(s, n, "on") || token_is(s, n, "all"))
		return DXR_SDLFIX_ALL;
	if (token_is(s, n, "0") || token_is(s, n, "off"))
		return 0;
	unsigned w = 0;
	for (const char *p = s, *end = s + n; p < end;) {
		const char *comma = memchr(p, ',', (size_t)(end - p));
		const char *a = p, *z = comma ? comma : end;
		p = comma ? comma + 1 : end;
		while (a < z && is_blank(*a))
			a++;
		while (z > a && is_blank(z[-1]))
			z--;
		if (a == z)
			continue;
		unsigned bit = 0;
		for (size_t i = 0; i < sizeof(k_fix_names) / sizeof(k_fix_names[0]); i++)
			if (token_is(a, (size_t)(z - a), k_fix_names[i].name))
				bit = k_fix_names[i].bit;
		if (bit) {
			w |= bit;
		} else if (unknown && ulen + 1 < unknown_cap) {
			int k = snprintf(unknown + ulen, unknown_cap - ulen, "%s%.*s", ulen ? "," : "", (int)(z - a), a);
			if (k > 0)
				ulen = ulen + (size_t)k < unknown_cap ? ulen + (size_t)k : unknown_cap - 1;
		}
	}
	return w;
}

//! Which fixes the environment allows; unknown names are logged (once: one install).
static unsigned
wanted(void)
{
	char unknown[160];
	unsigned w = dxr_sdl_fixups_parse(getenv("DISPLAYXR_SDL_FIXUPS"), unknown, sizeof(unknown));
	if (unknown[0])
		logf_("DISPLAYXR_SDL_FIXUPS: unknown fix name(s) ignored: %s", unknown);
	return w;
}

#ifdef DXR_SDLFIX_TEST
static int s_test_pin = 1; // the test executable cannot pin itself by name: 1 / 0 = pretend, -1 = real
#endif

//! A wrapper in one of SDL's tables may be called until the process ends, so this
//! library must never be unloaded (the engine may close the handle it took for
//! XRSDKPreInit). 1 when pinned.
static int
pin_self(void)
{
#ifdef DXR_SDLFIX_TEST
	if (s_test_pin >= 0)
		return s_test_pin;
#endif
	Dl_info info;
	if (!dladdr((void *)&pin_self, &info) || !info.dli_fname)
		return 0;
	return dlopen(info.dli_fname, RTLD_NOW | RTLD_NOLOAD | RTLD_NODELETE) != NULL; // never closed
}

static unsigned
install_with(const struct dxr_sdl_build *b, uintptr_t base)
{
	const char *l = getenv("DISPLAYXR_SDL_FIXUPS_LOG");
	s_log = l ? atoi(l) : 0;
	unsigned want = wanted();
	if (!want) {
		logf_("%s: off (DISPLAYXR_SDL_FIXUPS=%s)", b->what, getenv("DISPLAYXR_SDL_FIXUPS"));
		return 0;
	}
	if (!pin_self()) {
		logf_("%s: nothing installed: the plug-in could not pin itself in memory (RTLD_NODELETE), and SDL "
		      "would call into it after an unload",
		      b->what);
		return 0;
	}
	s_b = b;
	s_base = base;
	unsigned got = 0;
	if ((want & DXR_SDLFIX_CLIPBOARD) && install_clipboard())
		got |= DXR_SDLFIX_CLIPBOARD;
	if ((want & DXR_SDLFIX_IME) && install_ime())
		got |= DXR_SDLFIX_IME;
	if ((want & DXR_SDLFIX_OUTPUTS) && install_outputs())
		got |= DXR_SDLFIX_OUTPUTS;
	logf_("%s (build-id %.8s..., base %p): clipboard %s, IME %s, monitor re-add %s", b->what, b->build_id,
	      (void *)base, (got & DXR_SDLFIX_CLIPBOARD) ? "fixed" : (want & DXR_SDLFIX_CLIPBOARD) ? "NOT fixed" : "off",
	      (got & DXR_SDLFIX_IME) ? "for the DisplayXR fields" : (want & DXR_SDLFIX_IME) ? "NOT fixed" : "off",
	      (got & DXR_SDLFIX_OUTPUTS) ? "fixed" : (want & DXR_SDLFIX_OUTPUTS) ? "NOT fixed" : "off");
	return got;
}

static void
install_once(void)
{
	struct find_ctx c = {0};
	s_installed = 0;
	dl_iterate_phdr(find_unityplayer, &c);
	if (!c.found)
		return; // not a Unity player (the editor, a test, a tool)
	const struct dxr_sdl_build *b = dxr_sdl_fixups_find_build(c.build_id);
	if (!b) {
		logf_("UnityPlayer.so build-id %s is not a known build: nothing installed (Unity's own SDL behaviour stays)",
		      c.build_id[0] ? c.build_id : "(none)");
		return;
	}
	s_relro_lo = c.relro_lo;
	s_relro_hi = c.relro_hi;
	s_installed = install_with(b, c.base);
}

unsigned
dxr_sdl_fixups_install(void)
{
	pthread_once(&s_once, install_once);
	return s_installed;
}

const struct dxr_sdl_build *
dxr_sdl_fixups_find_build(const char *build_id)
{
	if (!build_id || !*build_id)
		return NULL;
	for (size_t i = 0; i < sizeof(k_builds) / sizeof(k_builds[0]); i++)
		if (strcmp(k_builds[i].build_id, build_id) == 0)
			return &k_builds[i];
	return NULL;
}

#ifdef DXR_SDLFIX_TEST
static int s_test_relro_set;

void
dxr_sdl_fixups_test_ime_reset(void (*send)(void *text_input, unsigned opcode, const int32_t *args))
{
	s_ime_send = send;
}

unsigned
dxr_sdl_fixups_test_ime_echoes(void)
{
	return s_echoes;
}

void
dxr_sdl_fixups_test_pin(int result)
{
	s_test_pin = result;
}

void
dxr_sdl_fixups_test_relro(uintptr_t lo, uintptr_t hi)
{
	s_relro_lo = lo;
	s_relro_hi = hi;
	s_test_relro_set = 1;
}

unsigned
dxr_sdl_fixups_install_build(const struct dxr_sdl_build *b, uintptr_t base)
{
	// A fresh IME for every test install.
	q_clear();
	s_owned = s_ti_focus = 0;
	s_ti_proxy = NULL;
	free(s_cur_preedit);
	s_cur_preedit = NULL;
	free(s_echo);
	s_echo = NULL;
	s_has_rect = 0;
	free(s_pend.commit);
	free(s_pend.preedit);
	memset(&s_pend, 0, sizeof(s_pend));
	s_echoes = 0;
	if (!s_test_relro_set) {
		s_relro_lo = 0;
		s_relro_hi = UINTPTR_MAX;
	}
	s_test_relro_set = 0;
	s_installed = install_with(b, base);
	return s_installed;
}
#endif
