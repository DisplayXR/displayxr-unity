// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Unit test of the SDL fixups (displayxr_linux_sdl_fixups.{h,c}). Built and run by
// CTest on Linux.
//
// A fake UnityPlayer.so image in memory: a "text" page with the real 6000.4.6f1
// entry's code signatures, a read-only "RELRO" page with SDL's wl_registry listener,
// a "data" page with SDL's _this. Behind it a model of SDL's structures at the real
// offsets and of this SDL's two handlers: remove deletes the display and frees its
// output data WITHOUT unlinking it from the output list (the bug); add appends to the
// list by walking it (where the player crashes on a freed node). A freed node is
// poisoned and remembered, so a walk into one is seen instead of crashing.
//  - the fix installed: removals at the head, middle and tail and of the only node,
//    then adds, never walk a freed node;
//  - the kill switch (DXR_SDLFIX=off): nothing swapped, and the model walks the freed
//    node (the crash), so the model reproduces the bug;
//  - every verification refuses a mismatch (either table entry, each code signature,
//    a table outside RELRO) and leaves the table as it was;
//  - the real build entry agrees with its own signatures (addresses and offsets).

#include "displayxr_linux_sdl_fixups.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static int s_failed;

#define CHECK(cond)                                                                                                \
	do {                                                                                                       \
		if (!(cond)) {                                                                                     \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                            \
			s_failed++;                                                                                \
		}                                                                                                  \
	} while (0)

#define AT DXR_SDL_AT
#define NODE_SIZE 0xf0 // sizeof(SDL_WaylandOutputData) in this SDL

static const struct dxr_sdl_build *R; // the real 6000.4.6f1 entry: the model uses its offsets

/*
 * A model of this SDL.
 */

static struct {
	char *dev;        // SDL_VideoDevice
	char *video_data; // SDL_VideoData (the registry listener's data)
	void *freed[64];
	int n_freed;
	int walked_freed; // Wayland_add_display walked into a freed node: the player's crash
	int remove_calls;
} m;

static uintptr_t s_base;      // the fake image
static char **s_this_slot;    // its SDL_VideoDevice *_this

static int
is_freed(void *p)
{
	for (int i = 0; i < m.n_freed; i++)
		if (m.freed[i] == p)
			return 1;
	return 0;
}

static void
model_reset(void)
{
	memset(&m, 0, sizeof(m));
	m.dev = calloc(1, 0x400);
	m.video_data = calloc(1, 0x200);
	AT(m.dev, R->off_displays, char *) = calloc(64, R->sizeof_display);
}

//! Wayland_add_display, then the output's first 'done' (SDL_AddVideoDisplay).
static void
model_add(uint32_t name)
{
	void *node = calloc(1, NODE_SIZE);
	AT(node, R->off_output_registry_id, uint32_t) = name;
	void **head = &AT(m.video_data, R->off_output_list, void *);
	if (!*head) {
		*head = node;
	} else {
		void *n = *head;
		for (;;) { // while (node->next) node = node->next;
			if (is_freed(n)) {
				m.walked_freed++;
				return; // the player: SIGSEGV here
			}
			void *next = AT(n, R->off_output_next, void *);
			if (!next)
				break;
			n = next;
		}
		AT(n, R->off_output_next, void *) = node;
	}
	int *num = &AT(m.dev, R->off_num_displays, int);
	char *displays = AT(m.dev, R->off_displays, char *);
	AT(displays + (size_t)*num * R->sizeof_display, R->off_display_driverdata, void *) = node;
	(*num)++;
}

//! display_remove_global with Wayland_free_display inlined, as this SDL has it.
static void
sdl_remove(void *data, void *registry, uint32_t name)
{
	(void)data;
	(void)registry;
	m.remove_calls++;
	char *dev = *s_this_slot;
	int *num = &AT(dev, R->off_num_displays, int);
	char *displays = AT(dev, R->off_displays, char *);
	for (int i = 0; i < *num; i++) {
		void *dd = AT(displays + (size_t)i * R->sizeof_display, R->off_display_driverdata, void *);
		if (AT(dd, R->off_output_registry_id, uint32_t) != name)
			continue;
		memmove(displays + (size_t)i * R->sizeof_display, displays + (size_t)(i + 1) * R->sizeof_display,
		        (size_t)(*num - i - 1) * R->sizeof_display); // SDL_DelVideoDisplay
		(*num)--;
		memset(dd, 0xa5, NODE_SIZE); // free(), and the allocator reuses the chunk
		m.freed[m.n_freed++] = dd;
		return;
	}
}

static void
sdl_global(void *data, void *registry, uint32_t name, const char *iface, uint32_t version)
{
	(void)data, (void)registry, (void)name, (void)iface, (void)version;
}

//! The list's registry ids, e.g. "3,5,6" (stops at a freed node: "!").
static const char *
model_list(void)
{
	static char buf[128];
	size_t len = 0;
	buf[0] = 0;
	void *n = AT(m.video_data, R->off_output_list, void *);
	for (int k = 0; n && k < 16; k++) {
		if (is_freed(n)) {
			snprintf(buf + len, sizeof(buf) - len, "%s!", len ? "," : "");
			break;
		}
		len += (size_t)snprintf(buf + len, sizeof(buf) - len, "%s%u", len ? "," : "",
		                        AT(n, R->off_output_registry_id, uint32_t));
		n = AT(n, R->off_output_next, void *);
	}
	return buf;
}

/*
 * The fake image.
 */

static struct {
	long pg;
	unsigned char *mem; // text | RELRO | data
	struct dxr_sdl_build b;
	struct dxr_sdl_sig sigs[DXR_SDL_OUTPUTS_SIGS];
	unsigned char sig_bytes[DXR_SDL_OUTPUTS_SIGS][64];
	void **table;
} img;

enum { MUT_NONE, MUT_GLOBAL, MUT_REMOVE, MUT_SIG0, MUT_SIG1, MUT_SIG2, MUT_SIG3 };

static void
image_new(int mutation)
{
	img.pg = sysconf(_SC_PAGESIZE);
	img.mem = mmap(NULL, (size_t)img.pg * 3, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	CHECK(img.mem != MAP_FAILED);
	s_base = (uintptr_t)img.mem;
	img.b = *R;
	img.b.registry_listener = (uintptr_t)img.pg + 0x110;
	img.b.registry_global = (uintptr_t)&sdl_global - s_base;
	img.b.registry_remove = (uintptr_t)&sdl_remove - s_base;
	img.b.video_device = (uintptr_t)img.pg * 2 + 0x30;
	for (int i = 0; i < DXR_SDL_OUTPUTS_SIGS; i++) {
		const struct dxr_sdl_sig *s = &R->outputs_sigs[i];
		memcpy(img.sig_bytes[i], s->bytes, s->len);
		img.sigs[i] = (struct dxr_sdl_sig){(uintptr_t)(0x100 * i), img.sig_bytes[i], s->len};
		memcpy(img.mem + 0x100 * i, s->bytes, s->len);
		if (mutation == MUT_SIG0 + i)
			img.mem[0x100 * i + s->len / 2] ^= 0x01;
		img.b.outputs_sigs[i] = img.sigs[i];
	}
	img.table = (void **)(s_base + img.b.registry_listener);
	img.table[0] = mutation == MUT_GLOBAL ? (void *)&model_add : (void *)&sdl_global;
	img.table[1] = mutation == MUT_REMOVE ? (void *)&model_add : (void *)&sdl_remove;
	CHECK(mprotect(img.mem + img.pg, (size_t)img.pg, PROT_READ) == 0); // RELRO
	model_reset();
	s_this_slot = (char **)(s_base + img.b.video_device);
	*s_this_slot = m.dev;
	dxr_sdl_fixups_test_reset();
}

static unsigned
image_install(void)
{
	return dxr_sdl_fixups_install_image(&img.b, s_base, s_base + (uintptr_t)img.pg, s_base + 2 * (uintptr_t)img.pg);
}

//! libwayland dispatching wl_registry.global_remove to SDL's listener.
static void
dispatch_remove(uint32_t name)
{
	typedef void (*fn)(void *, void *, uint32_t);
	fn f = (fn)__atomic_load_n(&img.table[1], __ATOMIC_SEQ_CST);
	f(m.video_data, NULL, name);
}

//! The page's protection from /proc/self/maps ("r--p", ...), or "".
static const char *
prot_of(const void *addr)
{
	static char perms[8];
	perms[0] = 0;
	FILE *f = fopen("/proc/self/maps", "r");
	if (!f)
		return perms;
	char line[512];
	while (fgets(line, sizeof(line), f)) {
		unsigned long lo, hi;
		char p[8];
		if (sscanf(line, "%lx-%lx %7s", &lo, &hi, p) == 3 && (uintptr_t)addr >= lo && (uintptr_t)addr < hi) {
			snprintf(perms, sizeof(perms), "%s", p);
			break;
		}
	}
	fclose(f);
	return perms;
}

/*
 * The tests.
 */

static void
test_find_build(void)
{
	CHECK(R != NULL);
	CHECK(dxr_sdl_find_build("4ae6b3f45943aa2cf6ca0d0ac4fdec503e60e55c") == R);
	CHECK(dxr_sdl_find_build("4AE6B3F45943AA2CF6CA0D0AC4FDEC503E60E55C") == NULL); // we print lower case
	CHECK(dxr_sdl_find_build("4ae6b3f45943aa2cf6ca0d0ac4fdec503e60e55") == NULL);
	CHECK(dxr_sdl_find_build("0000000000000000000000000000000000000000") == NULL);
	CHECK(dxr_sdl_find_build("") == NULL);
	CHECK(dxr_sdl_find_build(NULL) == NULL);
}

static void
test_wanted(void)
{
	CHECK(dxr_sdl_fixups_wanted(NULL) == DXR_SDLFIX_ALL);
	CHECK(dxr_sdl_fixups_wanted("") == DXR_SDLFIX_ALL);
	CHECK(dxr_sdl_fixups_wanted("1") == DXR_SDLFIX_ALL);
	CHECK(dxr_sdl_fixups_wanted("on") == DXR_SDLFIX_ALL);
	CHECK(dxr_sdl_fixups_wanted("off") == 0);
	CHECK(dxr_sdl_fixups_wanted("0") == 0);
	CHECK(dxr_sdl_fixups_wanted("outputs") == DXR_SDLFIX_OUTPUTS);
	CHECK(dxr_sdl_fixups_wanted("clipboard,outputs") == DXR_SDLFIX_OUTPUTS);
	CHECK(dxr_sdl_fixups_wanted(",outputs,") == DXR_SDLFIX_OUTPUTS);
	CHECK(dxr_sdl_fixups_wanted("clipboard") == 0);
	CHECK(dxr_sdl_fixups_wanted("outputsx") == 0);
	CHECK(dxr_sdl_fixups_wanted("output") == 0);
}

//! Builds the list @p ids, unlinks @p target_id, returns the ids left ("" for none).
static const char *
unlink_case(const uint32_t *ids, int n, uint32_t target_id, int *out_ret)
{
	static char buf[64];
	void *head = NULL, *nodes[8] = {0}, *target = NULL, **tail = &head;
	for (int i = 0; i < n; i++) {
		nodes[i] = calloc(1, NODE_SIZE);
		AT(nodes[i], 0x18, uint32_t) = ids[i];
		*tail = nodes[i];
		tail = &AT(nodes[i], 0xe8, void *);
		if (ids[i] == target_id)
			target = nodes[i];
	}
	void *absent = calloc(1, NODE_SIZE);
	*out_ret = dxr_sdl_output_unlink(&head, target ? target : absent, 0xe8);
	size_t len = 0;
	buf[0] = 0;
	for (void *p = head; p; p = AT(p, 0xe8, void *))
		len += (size_t)snprintf(buf + len, sizeof(buf) - len, "%s%u", len ? "," : "", AT(p, 0x18, uint32_t));
	for (int i = 0; i < n; i++)
		free(nodes[i]);
	free(absent);
	return buf;
}

static void
test_unlink(void)
{
	const uint32_t three[] = {1, 2, 3}, one[] = {7};
	int ret;
	CHECK(strcmp(unlink_case(three, 3, 1, &ret), "2,3") == 0 && ret == 1);  // head
	CHECK(strcmp(unlink_case(three, 3, 2, &ret), "1,3") == 0 && ret == 1);  // middle
	CHECK(strcmp(unlink_case(three, 3, 3, &ret), "1,2") == 0 && ret == 1);  // tail
	CHECK(strcmp(unlink_case(one, 1, 7, &ret), "") == 0 && ret == 1);       // the only one
	CHECK(strcmp(unlink_case(three, 3, 9, &ret), "1,2,3") == 0 && ret == 0); // not in the list
	CHECK(strcmp(unlink_case(NULL, 0, 9, &ret), "") == 0 && ret == 0);      // empty list
}

static int32_t
rel32(const unsigned char *p)
{
	int32_t v;
	memcpy(&v, p, 4);
	return v;
}

static uint32_t
u32(const unsigned char *p)
{
	uint32_t v;
	memcpy(&v, p, 4);
	return v;
}

//! The 6000.4.6f1 entry's addresses and offsets are the ones its signatures read.
static void
test_real_entry_consistent(void)
{
	const struct dxr_sdl_sig *rm = &R->outputs_sigs[0], *fr = &R->outputs_sigs[1], *gd = &R->outputs_sigs[2],
	                         *wk = &R->outputs_sigs[3];
	CHECK(rm->at == R->registry_remove);
	// display_remove_global: mov _this(%rip),%rax at +7 (7 bytes), mov 0x2d8(%rax),%r15d at +19,
	// call SDL_GetDisplay at +41 (5 bytes), mov 0x70(%rax),%r14 at +46, cmp %ebp,0x18(%r14) at +50.
	CHECK(rm->at + 14 + (uintptr_t)(intptr_t)rel32(rm->bytes + 10) == R->video_device);
	CHECK(u32(rm->bytes + 22) == R->off_num_displays);
	CHECK(rm->bytes[41] == 0xe8 && rm->at + 46 + (uintptr_t)(intptr_t)rel32(rm->bytes + 42) == gd->at);
	CHECK(rm->bytes[49] == R->off_display_driverdata);
	CHECK(rm->bytes[53] == R->off_output_registry_id);
	// ... the free of %r14 (the driverdata) further down: call *SDL_free (the slot at 0x27e76e8).
	CHECK(fr->at > rm->at && fr->at < rm->at + 0x100);
	CHECK(fr->at + 9 + (uintptr_t)(intptr_t)rel32(fr->bytes + 5) == 0x27e76e8);
	// SDL_GetDisplay: mov _this(%rip),%rax at +1, ->num_displays, imul $stride, add ->displays.
	CHECK(gd->at + 8 + (uintptr_t)(intptr_t)rel32(gd->bytes + 4) == R->video_device);
	CHECK(u32(gd->bytes + 15) == R->off_num_displays);
	CHECK(gd->bytes[32] == R->sizeof_display);
	CHECK(u32(gd->bytes + 36) == R->off_displays);
	// Wayland_add_display's walk: ->output_list, ->next.
	CHECK(u32(wk->bytes + 3) == R->off_output_list);
	CHECK(u32(wk->bytes + 22) == R->off_output_next);
	// The listener table sits in 6000.4.6f1's RELRO (vaddr 0x2734260 + 0x96da0).
	CHECK(R->registry_listener >= 0x2734260 && R->registry_listener + 16 <= 0x2734260 + 0x96da0);
}

static void
test_fix(void)
{
	unsetenv("DXR_SDLFIX");
	image_new(MUT_NONE);
	CHECK(image_install() == DXR_SDLFIX_OUTPUTS);
	CHECK(img.table[0] == (void *)&sdl_global);         // add: untouched
	CHECK(img.table[1] != (void *)&sdl_remove);         // remove: the wrapper
	CHECK(strncmp(prot_of(img.table), "r--", 3) == 0); // RELRO read-only again
	model_add(3);
	model_add(4);
	model_add(5);
	CHECK(strcmp(model_list(), "3,4,5") == 0);
	dispatch_remove(4); // middle (the DS1 case: the second monitor)
	CHECK(strcmp(model_list(), "3,5") == 0);
	model_add(6); // the monitor back
	CHECK(m.walked_freed == 0);
	CHECK(strcmp(model_list(), "3,5,6") == 0);
	dispatch_remove(3); // head
	dispatch_remove(6); // tail
	CHECK(strcmp(model_list(), "5") == 0);
	dispatch_remove(5); // the only one
	CHECK(strcmp(model_list(), "") == 0);
	model_add(7);
	model_add(8);
	dispatch_remove(99); // not an output (SDL assumes every removed global is one)
	CHECK(strcmp(model_list(), "7,8") == 0);
	CHECK(m.walked_freed == 0);
	CHECK(m.remove_calls == 5); // SDL's handler ran every time
	CHECK(AT(m.dev, R->off_num_displays, int) == 2);

	// Many off / on cycles of one monitor.
	for (uint32_t k = 0; k < 20; k++) {
		dispatch_remove(8 + k);
		model_add(9 + k);
	}
	CHECK(m.walked_freed == 0);
	CHECK(strcmp(model_list(), "7,28") == 0);
}

static void
test_kill_switch_reproduces_bug(void)
{
	const char *offs[] = {"off", "0", "clipboard"};
	for (int i = 0; i < 3; i++) {
		setenv("DXR_SDLFIX", offs[i], 1);
		image_new(MUT_NONE);
		CHECK(image_install() == 0);
		CHECK(img.table[1] == (void *)&sdl_remove);
		model_add(3);
		model_add(4);
		model_add(5);
		dispatch_remove(4);
		CHECK(strcmp(model_list(), "3,!") == 0); // SDL left the freed node linked
		model_add(6);
		CHECK(m.walked_freed == 1); // the player's crash
	}
	setenv("DXR_SDLFIX", "outputs", 1);
	image_new(MUT_NONE);
	CHECK(image_install() == DXR_SDLFIX_OUTPUTS);
	unsetenv("DXR_SDLFIX");
}

static void
test_refusals(void)
{
	unsetenv("DXR_SDLFIX");
	for (int mut = MUT_GLOBAL; mut <= MUT_SIG3; mut++) {
		image_new(mut);
		void *before = img.table[1];
		CHECK(image_install() == 0);
		CHECK(img.table[1] == before);
	}
	// A table outside RELRO (here: RELRO said to be the data page).
	image_new(MUT_NONE);
	CHECK(dxr_sdl_fixups_install_image(&img.b, s_base, s_base + 2 * (uintptr_t)img.pg,
	                                   s_base + 3 * (uintptr_t)img.pg) == 0);
	CHECK(img.table[1] == (void *)&sdl_remove);
	CHECK(dxr_sdl_outputs_check(&img.b, s_base, s_base + (uintptr_t)img.pg, s_base + 2 * (uintptr_t)img.pg) == NULL);
}

static void
test_process_without_unityplayer(void)
{
	CHECK(dxr_sdl_fixups_install() == 0);
	CHECK(dxr_sdl_fixups_install() == 0); // once
}

int
main(void)
{
	R = dxr_sdl_find_build("4ae6b3f45943aa2cf6ca0d0ac4fdec503e60e55c");
	if (!R) {
		fprintf(stderr, "FAIL: no 6000.4.6f1 entry\n");
		return 1;
	}
	test_find_build();
	test_wanted();
	test_unlink();
	test_real_entry_consistent();
	test_fix();
	test_kill_switch_reproduces_bug();
	test_refusals();
	test_process_without_unityplayer();
	if (s_failed) {
		fprintf(stderr, "test_sdl_fixups: %d FAILED\n", s_failed);
		return 1;
	}
	printf("test_sdl_fixups: all passed\n");
	return 0;
}
