// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// SDL fixups (displayxr_linux_sdl_fixups.h): finding UnityPlayer.so in this
// process, the table swaps and the wrappers. Part of libdisplayxr_unity_wayland.so;
// dxr_wl_lib_init installs it once.
//
// Threads: the swap may run on any thread while SDL dispatches Wayland events on
// Unity's main thread; libwayland reads a listener entry once per event, and every
// entry is written with one atomic pointer store, so an event sees either SDL's own
// handler or the wrapper. The wrappers run on the thread that dispatches SDL's
// events, as SDL's handlers do, and touch only what those handlers touch.
//
// Env: DXR_SDLFIX (dxr_sdl_fixups_wanted: off = none); DXR_SDLFIX_VERBOSE=1 also
// logs SDL's output list after every monitor removal.

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

static pthread_once_t s_once = PTHREAD_ONCE_INIT;
static const struct dxr_sdl_build *s_b;
static uintptr_t s_base;
static unsigned s_installed;
static int s_verbose;

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

//! Swaps @p n entries of the table at vaddr @p table, from @p first on, for
//! @p wrappers; each must still hold base + @p expect[i]. 0 when done, -1 with
//! nothing changed otherwise. The page goes back to read-only (RELRO).
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
		logf_("could not make SDL's listener table read-only again (it stays writable)");
	return 0;
}

/*
 * outputs: the monitor re-add use-after-free.
 */

typedef void (*registry_remove_fn)(void *data, void *registry, uint32_t name);
static registry_remove_fn s_orig_registry_remove;

static void
log_output_list(void *video_data)
{
	char buf[256];
	size_t len = 0;
	buf[0] = 0;
	void *node = DXR_SDL_AT(video_data, s_b->off_output_list, void *);
	for (int k = 0; node && k < 16 && len < sizeof(buf) - 16; k++) {
		len += (size_t)snprintf(buf + len, sizeof(buf) - len, " %u",
		                        DXR_SDL_AT(node, s_b->off_output_registry_id, uint32_t));
		node = DXR_SDL_AT(node, s_b->off_output_next, void *);
	}
	logf_("SDL's output list now:%s", buf[0] ? buf : " (empty)");
}

//! wl_registry.global_remove for SDL (whose data is its SDL_VideoData).
static void
fix_registry_remove(void *data, void *registry, uint32_t name)
{
	void *target = data ? dxr_sdl_output_for(s_b, s_base, name) : NULL;
	if (target && dxr_sdl_output_unlink(&DXR_SDL_AT(data, s_b->off_output_list, void *), target,
	                                    s_b->off_output_next))
		logf_("monitor removed (wl_output global %u): unlinked SDL's output data before SDL frees it", name);
	s_orig_registry_remove(data, registry, name); // SDL: delete the display, free the data
	if (target && s_verbose)
		log_output_list(data);
}

static const char *
install_outputs(uintptr_t relro_lo, uintptr_t relro_hi)
{
	const char *why = dxr_sdl_outputs_check(s_b, s_base, relro_lo, relro_hi);
	if (why)
		return why;
	s_orig_registry_remove = (registry_remove_fn)(s_base + s_b->registry_remove);
	const uintptr_t expect[1] = {s_b->registry_remove};
	const fnptr wrap[1] = {(fnptr)fix_registry_remove};
	if (swap_entries(s_b->registry_listener, 1, 1, expect, wrap) != 0)
		return "SDL's wl_registry listener could not be changed";
	return NULL;
}

/*
 * Install.
 */

//! Once a wrapper is in one of SDL's tables this library must never be unloaded.
static void
pin_self(void)
{
	Dl_info info;
	if (dladdr((void *)&pin_self, &info) && info.dli_fname)
		(void)dlopen(info.dli_fname, RTLD_NOW | RTLD_NOLOAD | RTLD_NODELETE);
}

static unsigned
install_with(const struct dxr_sdl_build *b, uintptr_t base, uintptr_t relro_lo, uintptr_t relro_hi)
{
	const char *env = getenv("DXR_SDLFIX");
	unsigned want = dxr_sdl_fixups_wanted(env);
	const char *v = getenv("DXR_SDLFIX_VERBOSE");
	s_verbose = v && atoi(v) > 0;
	if (!want) {
		logf_("%s: off (DXR_SDLFIX=%s)", b->what, env ? env : "");
		return 0;
	}
	s_b = b;
	s_base = base;
	unsigned got = 0;
	const char *outputs_why = NULL;
	if (want & DXR_SDLFIX_OUTPUTS) {
		outputs_why = install_outputs(relro_lo, relro_hi);
		if (!outputs_why)
			got |= DXR_SDLFIX_OUTPUTS;
	}
	if (got)
		pin_self();
	char outputs[160];
	if (got & DXR_SDLFIX_OUTPUTS)
		snprintf(outputs, sizeof(outputs), "installed");
	else if (want & DXR_SDLFIX_OUTPUTS)
		snprintf(outputs, sizeof(outputs), "NOT installed (%s)", outputs_why);
	else
		snprintf(outputs, sizeof(outputs), "off");
	logf_("%s (UnityPlayer.so build-id %.8s..., base %p): monitor re-add fix %s", b->what, b->build_id,
	      (void *)base, outputs);
	return got;
}

struct find_ctx {
	uintptr_t base, relro_lo, relro_hi;
	char build_id[2 * 32 + 1];
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
				for (unsigned j = 0; j < nh->n_descsz && j < 32; j++)
					snprintf(c->build_id + 2 * j, 3, "%02x", desc[j]);
			p = (const char *)desc + ((nh->n_descsz + 3) & ~3u);
		}
	}
	return 1;
}

static void
install_once(void)
{
	struct find_ctx c = {0};
	dl_iterate_phdr(find_unityplayer, &c);
	if (!c.found) {
		logf_("no UnityPlayer.so in this process: nothing installed");
		return;
	}
	const struct dxr_sdl_build *b = dxr_sdl_find_build(c.build_id);
	if (!b) {
		logf_("UnityPlayer.so build-id %s is not a known build: nothing installed",
		      c.build_id[0] ? c.build_id : "(none)");
		return;
	}
	s_installed = install_with(b, c.base, c.relro_lo, c.relro_hi);
}

unsigned
dxr_sdl_fixups_install(void)
{
	pthread_once(&s_once, install_once);
	return s_installed;
}

#ifdef DXR_SDLFIX_TEST
unsigned
dxr_sdl_fixups_install_image(const struct dxr_sdl_build *b, uintptr_t base, uintptr_t relro_lo, uintptr_t relro_hi)
{
	s_installed = install_with(b, base, relro_lo, relro_hi);
	return s_installed;
}

void
dxr_sdl_fixups_test_reset(void)
{
	s_installed = 0;
	s_b = NULL;
	s_base = 0;
	s_orig_registry_remove = NULL;
}
#endif
