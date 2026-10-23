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
// in the instructions that read them. Once a wrapper is in, this library pins
// itself (RTLD_NODELETE): SDL may call it until the process ends.
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
//
// Adding a fix: its fields and code signatures in struct dxr_sdl_build (one table
// entry per UnityPlayer.so), a DXR_SDLFIX_* bit and its name in wanted(), and an
// install_<fix>() that verifies first, then swaps, called from install_with().

#define _GNU_SOURCE // dladdr, RTLD_NODELETE
#include "displayxr_linux_sdl_fixups.h"

#include <dlfcn.h>
#include <elf.h>
#include <link.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
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
	fputs("[DisplayXR-WL] SDL fixups: ", stderr);
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

//! Swaps @p n entries of the table at vaddr @p table from @p first on: each must
//! still hold base + @p expect[i]. 0 when done, -1 (nothing changed) otherwise.
static int
swap_entries(uintptr_t table, int first, int n, const uintptr_t *expect, const fnptr *wrappers)
{
	void **t = (void **)(s_base + table);
	for (int i = 0; i < n; i++)
		if ((uintptr_t)t[first + i] != s_base + expect[i])
			return -1;
	long pg = sysconf(_SC_PAGESIZE);
	uintptr_t lo = (uintptr_t)&t[first] & ~(uintptr_t)(pg - 1);
	uintptr_t hi = ((uintptr_t)&t[first + n] + (uintptr_t)pg - 1) & ~(uintptr_t)(pg - 1);
	if (mprotect((void *)lo, hi - lo, PROT_READ | PROT_WRITE) != 0)
		return -1;
	for (int i = 0; i < n; i++)
		__atomic_store_n(&t[first + i], (void *)wrappers[i], __ATOMIC_SEQ_CST);
	if (mprotect((void *)lo, hi - lo, PROT_READ) != 0)
		logf_("could not make SDL's listener table read-only again");
	return 0;
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
	if (swap_entries(b->registry_listener, 1, 1, expect, wrap) != 0) {
		logf_("monitor re-add fix NOT installed: SDL's wl_registry listener is not as expected");
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

//! Which fixes the environment allows (DISPLAYXR_SDL_FIXUPS).
static unsigned
wanted(void)
{
	const char *e = getenv("DISPLAYXR_SDL_FIXUPS");
	if (!e || !*e || strcmp(e, "1") == 0)
		return DXR_SDLFIX_OUTPUTS;
	if (strcmp(e, "0") == 0 || strcmp(e, "off") == 0)
		return 0;
	unsigned w = 0;
	if (strstr(e, "outputs"))
		w |= DXR_SDLFIX_OUTPUTS;
	return w;
}

//! Once a wrapper is in one of SDL's tables this library must never be unloaded
//! (the engine may close the handle it took for XRSDKPreInit).
static void
pin_self(void)
{
	Dl_info info;
	if (dladdr((void *)&pin_self, &info) && info.dli_fname)
		(void)dlopen(info.dli_fname, RTLD_NOW | RTLD_NOLOAD | RTLD_NODELETE);
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
	s_b = b;
	s_base = base;
	unsigned got = 0;
	// DXR_SDLFIX_CLIPBOARD, DXR_SDLFIX_IME: their install_*() go here.
	if ((want & DXR_SDLFIX_OUTPUTS) && install_outputs())
		got |= DXR_SDLFIX_OUTPUTS;
	if (got)
		pin_self();
	logf_("%s (build-id %.8s..., base %p): monitor re-add %s", b->what, b->build_id, (void *)base,
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
dxr_sdl_fixups_test_relro(uintptr_t lo, uintptr_t hi)
{
	s_relro_lo = lo;
	s_relro_hi = hi;
	s_test_relro_set = 1;
}

unsigned
dxr_sdl_fixups_install_build(const struct dxr_sdl_build *b, uintptr_t base)
{
	if (!s_test_relro_set) {
		s_relro_lo = 0;
		s_relro_hi = UINTPTR_MAX;
	}
	s_test_relro_set = 0;
	s_installed = install_with(b, base);
	return s_installed;
}
#endif
