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
//  - the outputs fix installed: removals at the head, middle and tail and of the only
//    node, then adds, never walk a freed node;
//  - the clipboard fix installed: a cancelled source that is the device's selection is
//    cleared before SDL frees it, another source is left alone, and a model of the next
//    set_selection never uses the freed source; installed alone ("clipboard"), it still
//    cross-checks SDL's _this (a change in that signature alone refuses it);
//  - the IME capture: not owned, every text-input event goes on to SDL unchanged and
//    nothing is queued; owned, the state events are kept from SDL and come out once per
//    'done' with the full commit, the composition and its cursor, delete-surrounding;
//    taking the IME queues its current composition; giving it back resets the IME
//    (disable, commit, enable, set_content_type, commit) and drops the IME's own commit
//    of the composition once; "ime" alone, and the kill switch;
//  - the kill switch (DISPLAYXR_SDL_FIXUPS=off): nothing swapped, and the model walks the freed
//    node (the crash), so the model reproduces the bug;
//  - every verification refuses a mismatch (either table entry, each code signature,
//    a table outside RELRO) and leaves the table as it was, and so does a failed pin;
//  - the swap gives the page back the protection it had (read-only, or writable);
//  - DISPLAYXR_SDL_FIXUPS is parsed as a list of exact names;
//  - the real build entry agrees with its own signatures (addresses and offsets);
//  - a process without UnityPlayer.so (this test) gets nothing installed.

#include "displayxr_linux_sdl_fixups.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

typedef void (*fnptr_t)(void);

static int s_failed;

#define CHECK(cond)                                                                                                \
	do {                                                                                                       \
		if (!(cond)) {                                                                                     \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                            \
			s_failed++;                                                                                \
		}                                                                                                  \
	} while (0)

#define AT(p, off, T) (*(T *)((char *)(p) + (off)))
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
	m.dev = calloc(1, 0x800); // up to ->driverdata (+0x5c8)
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
 * A model of this SDL's clipboard (SDL_VideoData / SDL_WaylandInput / SDL_WaylandDataDevice,
 * at the real offsets) and its data_source_handle_cancelled.
 */

static struct {
	char *video_data;      // _this->driverdata (+0x5c8)
	char *input;           // ->input (+0xc8)
	char *data_device;     // ->data_device (+0x28), its selection_source at +0x30
	int cancelled_calls;
	int used_freed_source; // the next set_selection destroyed a freed source: the player's crash
} cm;

static void
clip_model_reset(void)
{
	memset(&cm, 0, sizeof(cm));
	cm.video_data = calloc(1, 0x600);
	cm.input = calloc(1, 0x100);
	cm.data_device = calloc(1, 0x40);
	AT(m.dev, R->off_driverdata, char *) = cm.video_data;
	AT(cm.video_data, R->off_input, char *) = cm.input;
	AT(cm.input, R->off_data_device, char *) = cm.data_device;
}

static void *
clip_selection(void)
{
	return AT(cm.data_device, R->off_selection_source, void *);
}

static void
clip_set_selection(void *source)
{
	AT(cm.data_device, R->off_selection_source, void *) = source;
}

//! data_source_handle_cancelled -> Wayland_data_source_destroy: SDL frees the source. It
//! does NOT clear selection_source (the bug); the chunk is poisoned and remembered.
static void
sdl_source_cancelled(void *data, void *source)
{
	(void)source;
	cm.cancelled_calls++;
	memset(data, 0xc1, 0x18); // free(); the allocator writes its own pointer at word 0
	m.freed[m.n_freed++] = data;
}

//! A later Wayland_data_device_set_selection destroying the old selection_source: if that
//! source was freed, this is the use-after-free (wl_proxy_marshal on freed memory).
static void
clip_model_set_selection(void *new_source)
{
	void *old = clip_selection();
	if (old && is_freed(old))
		cm.used_freed_source++;
	clip_set_selection(new_source);
}

/*
 * A model of this SDL's zwp_text_input_v3 handlers: they log what reaches them.
 */

static char s_ti_log[1024];
#define TI_LOG(...) snprintf(s_ti_log + strlen(s_ti_log), sizeof(s_ti_log) - strlen(s_ti_log), __VA_ARGS__)
static void sdl_ti_enter(void *d, void *t, void *sf) { (void)d, (void)t, (void)sf; TI_LOG("E"); }
static void sdl_ti_leave(void *d, void *t, void *sf) { (void)d, (void)t, (void)sf; TI_LOG("L"); }
static void sdl_ti_preedit(void *d, void *t, const char *x, int32_t b, int32_t e) { (void)d, (void)t; TI_LOG("P(%s,%d,%d)", x ? x : "nil", b, e); }
static void sdl_ti_commit(void *d, void *t, const char *x) { (void)d, (void)t; TI_LOG("C(%s)", x ? x : "nil"); }
static void sdl_ti_delete(void *d, void *t, uint32_t b, uint32_t a) { (void)d, (void)t; TI_LOG("X(%u,%u)", b, a); }
static void sdl_ti_done(void *d, void *t, uint32_t sr) { (void)d, (void)t; TI_LOG("D(%u)", sr); }

//! What the IME's requests sent (instead of wl_proxy_marshal): opcodes, set_cursor_rectangle as 6(x,y,w,h).
static char s_reset_log[256];
static void *s_reset_proxy;
static void
reset_send(void *text_input, unsigned opcode, const int32_t *args)
{
	s_reset_proxy = text_input;
	size_t n = strlen(s_reset_log);
	if (opcode == 6 && args)
		snprintf(s_reset_log + n, sizeof(s_reset_log) - n, "6(%d,%d,%d,%d)", args[0], args[1], args[2], args[3]);
	else
		snprintf(s_reset_log + n, sizeof(s_reset_log) - n, "%u", opcode);
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
	unsigned char clip_sig_bytes[DXR_SDL_CLIPBOARD_SIGS][64];
	void **table;    // the wl_registry listener (outputs)
	void **cs_table; // the wl_data_source listener (clipboard)
	unsigned char ime_sig_bytes[DXR_SDL_IME_SIGS][64];
	void **ti_table; // the zwp_text_input_v3 listener (ime)
} img;

enum {
	MUT_NONE,
	MUT_GLOBAL, MUT_REMOVE,                 // outputs: a wrong table entry
	MUT_SIG0, MUT_SIG1, MUT_SIG2, MUT_SIG3, // outputs: a byte signature off
	MUT_RW_PAGE,                            // the "RELRO" page left writable
	MUT_CS_CANCELLED,                       // clipboard: a wrong cancelled entry
	MUT_CS0, MUT_CS1, MUT_CS2, MUT_CS3, MUT_CS4, MUT_CS5, // clipboard: a byte signature off (CS5: SetClipboardText / _this)
	MUT_TI_ENTRY,                           // ime: a wrong table entry
	MUT_TI0, MUT_TI1, MUT_TI2, MUT_TI3      // ime: a byte signature off
};

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
	// The clipboard: SDL's wl_data_source listener (another table in the "RELRO" page), its
	// cancelled handler, its code signatures (past the outputs ones) at the real bytes.
	img.b.data_source_listener = (uintptr_t)img.pg + 0x200;
	img.b.data_source_cancelled = (uintptr_t)&sdl_source_cancelled - s_base;
	for (int i = 0; i < DXR_SDL_CLIPBOARD_SIGS; i++) {
		const struct dxr_sdl_sig *cs = &R->clipboard_sigs[i];
		unsigned off = 0x400 + 0x100 * (unsigned)i;
		memcpy(img.clip_sig_bytes[i], cs->bytes, cs->len);
		memcpy(img.mem + off, cs->bytes, cs->len);
		if (mutation == MUT_CS0 + i)
			img.mem[off + cs->len / 2] ^= 0x01;
		img.b.clipboard_sigs[i] = (struct dxr_sdl_sig){(uintptr_t)off, img.clip_sig_bytes[i], cs->len};
	}
	// The IME: SDL's zwp_text_input_v3 listener (a third table in the "RELRO" page), its six
	// handlers (the model above), and its code signatures at the real bytes.
	img.b.text_input_listener = (uintptr_t)img.pg + 0x300;
	const fnptr_t ti_fns[6] = {(fnptr_t)sdl_ti_enter,  (fnptr_t)sdl_ti_leave,  (fnptr_t)sdl_ti_preedit,
	                           (fnptr_t)sdl_ti_commit, (fnptr_t)sdl_ti_delete, (fnptr_t)sdl_ti_done};
	for (int i = 0; i < 6; i++)
		img.b.ti_fns[i] = (uintptr_t)ti_fns[i] - s_base;
	for (int i = 0; i < DXR_SDL_IME_SIGS; i++) {
		const struct dxr_sdl_sig *is = &R->ime_sigs[i];
		unsigned off = 0xa00 + 0x100 * (unsigned)i; // past the clipboard sigs (0x400..0x900)
		memcpy(img.ime_sig_bytes[i], is->bytes, is->len);
		memcpy(img.mem + off, is->bytes, is->len);
		if (mutation == MUT_TI0 + i)
			img.mem[off + is->len / 2] ^= 0x01;
		img.b.ime_sigs[i] = (struct dxr_sdl_sig){(uintptr_t)off, img.ime_sig_bytes[i], is->len};
	}
	img.ti_table = (void **)(s_base + img.b.text_input_listener);
	for (int i = 0; i < 6; i++)
		img.ti_table[i] = (void *)ti_fns[i];
	if (mutation == MUT_TI_ENTRY)
		img.ti_table[3] = (void *)&model_add;
	img.table = (void **)(s_base + img.b.registry_listener);
	img.table[0] = mutation == MUT_GLOBAL ? (void *)&model_add : (void *)&sdl_global;
	img.table[1] = mutation == MUT_REMOVE ? (void *)&model_add : (void *)&sdl_remove;
	img.cs_table = (void **)(s_base + img.b.data_source_listener);
	for (int i = 0; i < 6; i++)
		img.cs_table[i] = (void *)&sdl_global; // the entries that are not swapped
	img.cs_table[2] = mutation == MUT_CS_CANCELLED ? (void *)&model_add : (void *)&sdl_source_cancelled;
	if (mutation != MUT_RW_PAGE)
		CHECK(mprotect(img.mem + img.pg, (size_t)img.pg, PROT_READ) == 0); // RELRO
	model_reset();
	s_this_slot = (char **)(s_base + img.b.video_device);
	*s_this_slot = m.dev;
	clip_model_reset();
	s_ti_log[0] = 0;
	s_reset_log[0] = 0;
	s_reset_proxy = NULL;
	dxr_sdl_fixups_test_ime_reset(reset_send);
}

static unsigned
image_install(void)
{
	dxr_sdl_fixups_test_relro(s_base + (uintptr_t)img.pg, s_base + 2 * (uintptr_t)img.pg);
	return dxr_sdl_fixups_install_build(&img.b, s_base);
}

//! libwayland dispatching wl_registry.global_remove to SDL's listener.
static void
dispatch_remove(uint32_t name)
{
	typedef void (*fn)(void *, void *, uint32_t);
	fn f = (fn)__atomic_load_n(&img.table[1], __ATOMIC_SEQ_CST);
	f(m.video_data, NULL, name);
}

//! libwayland dispatching wl_data_source.cancelled to SDL's listener.
static void
dispatch_cancelled(void *source_struct)
{
	typedef void (*fn)(void *, void *);
	fn f = (fn)__atomic_load_n(&img.cs_table[2], __ATOMIC_SEQ_CST);
	f(source_struct, (void *)0x5ec0 /* the wl_data_source proxy: unused by the model */);
}

//! libwayland dispatching zwp_text_input_v3 events to SDL's listener (data: SDL's struct).
#define TI_PROXY ((void *)0x7170)
static void
ti_preedit(const char *text, int32_t b, int32_t e)
{
	((void (*)(void *, void *, const char *, int32_t, int32_t))__atomic_load_n(&img.ti_table[2], __ATOMIC_SEQ_CST))(
	    NULL, TI_PROXY, text, b, e);
}
static void
ti_commit(const char *text)
{
	((void (*)(void *, void *, const char *))__atomic_load_n(&img.ti_table[3], __ATOMIC_SEQ_CST))(NULL, TI_PROXY, text);
}
static void
ti_delete(uint32_t b, uint32_t a)
{
	((void (*)(void *, void *, uint32_t, uint32_t))__atomic_load_n(&img.ti_table[4], __ATOMIC_SEQ_CST))(NULL, TI_PROXY, b, a);
}
static void
ti_done(uint32_t serial)
{
	((void (*)(void *, void *, uint32_t))__atomic_load_n(&img.ti_table[5], __ATOMIC_SEQ_CST))(NULL, TI_PROXY, serial);
}
static void
ti_enter(void)
{
	((void (*)(void *, void *, void *))__atomic_load_n(&img.ti_table[0], __ATOMIC_SEQ_CST))(NULL, TI_PROXY, NULL);
}
static void
ti_leave(void)
{
	((void (*)(void *, void *, void *))__atomic_load_n(&img.ti_table[1], __ATOMIC_SEQ_CST))(NULL, TI_PROXY, NULL);
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
	CHECK(dxr_sdl_fixups_find_build("4ae6b3f45943aa2cf6ca0d0ac4fdec503e60e55c") == R);
	CHECK(dxr_sdl_fixups_find_build("4AE6B3F45943AA2CF6CA0D0AC4FDEC503E60E55C") == NULL); // we print lower case
	CHECK(dxr_sdl_fixups_find_build("4ae6b3f45943aa2cf6ca0d0ac4fdec503e60e55") == NULL);
	CHECK(dxr_sdl_fixups_find_build("0000000000000000000000000000000000000000") == NULL);
	CHECK(dxr_sdl_fixups_find_build("") == NULL);
	CHECK(dxr_sdl_fixups_find_build(NULL) == NULL);
}

//! The DISPLAYXR_SDL_FIXUPS values: what an install on a fresh image puts in.
static unsigned
install_with_env(const char *value)
{
	if (value)
		setenv("DISPLAYXR_SDL_FIXUPS", value, 1);
	else
		unsetenv("DISPLAYXR_SDL_FIXUPS");
	image_new(MUT_NONE);
	unsigned got = image_install();
	CHECK((img.table[1] != (void *)&sdl_remove) == ((got & DXR_SDLFIX_OUTPUTS) != 0));
	CHECK((img.cs_table[2] != (void *)&sdl_source_cancelled) == ((got & DXR_SDLFIX_CLIPBOARD) != 0));
	CHECK((img.ti_table[3] != (void *)&sdl_ti_commit) == ((got & DXR_SDLFIX_IME) != 0));
	unsetenv("DISPLAYXR_SDL_FIXUPS");
	return got;
}

//! dxr_sdl_fixups_parse(@p value) gives @p bits and the unknown names @p unknown.
static int
parse_is(const char *value, unsigned bits, const char *unknown)
{
	char u[64];
	unsigned got = dxr_sdl_fixups_parse(value, u, sizeof(u));
	int ok = got == bits && strcmp(u, unknown) == 0;
	if (!ok)
		fprintf(stderr, "  parse(%s%s%s) = %u, unknown '%s'\n", value ? "\"" : "", value ? value : "NULL",
		        value ? "\"" : "", got, u);
	return ok;
}

static void
test_parse(void)
{
	const unsigned A = DXR_SDLFIX_ALL, O = DXR_SDLFIX_OUTPUTS, C = DXR_SDLFIX_CLIPBOARD, I = DXR_SDLFIX_IME;
	CHECK(A == (C | I | O));
	CHECK(parse_is("ime", I, ""));
	CHECK(parse_is(" ime , outputs", I | O, ""));
	CHECK(parse_is("imeX", 0, "imeX"));
	CHECK(parse_is(NULL, A, ""));
	CHECK(parse_is("", A, ""));
	CHECK(parse_is("   ", A, ""));
	CHECK(parse_is("1", A, ""));
	CHECK(parse_is("on", A, ""));
	CHECK(parse_is("all", A, ""));
	CHECK(parse_is(" on ", A, ""));
	CHECK(parse_is("0", 0, ""));
	CHECK(parse_is("off", 0, ""));
	CHECK(parse_is(" off ", 0, ""));
	CHECK(parse_is("outputs", O, ""));
	CHECK(parse_is(" outputs ", O, ""));
	CHECK(parse_is("clipboard", C, ""));
	CHECK(parse_is(" clipboard ", C, ""));
	CHECK(parse_is("outputs,clipboard", C | O, ""));
	CHECK(parse_is("clipboard , outputs", C | O, ""));
	CHECK(parse_is("clipboard,ime,outputs", A, ""));
	CHECK(parse_is("clipboardX", 0, "clipboardX"));
	CHECK(parse_is("clip", 0, "clip"));
	CHECK(parse_is(",outputs,", O, ""));
	CHECK(parse_is("outputsX", 0, "outputsX"));
	CHECK(parse_is("output", 0, "output"));
	CHECK(parse_is("OUTPUTS", 0, "OUTPUTS"));
	CHECK(parse_is("clipboard,ime", C | I, ""));
	CHECK(parse_is("on,outputs", O, "on")); // the specials are whole values only
	CHECK(dxr_sdl_fixups_parse("outputsX", NULL, 0) == 0);
	char tiny[4];
	CHECK(dxr_sdl_fixups_parse("imeXYZ,outputs", tiny, sizeof(tiny)) == O && strlen(tiny) < sizeof(tiny));
}

static void
test_wanted(void)
{
	CHECK(DXR_SDLFIX_CLIPBOARD == 1u);
	CHECK(DXR_SDLFIX_IME == 2u);
	CHECK(DXR_SDLFIX_OUTPUTS == 4u);
	const unsigned A = DXR_SDLFIX_ALL;
	CHECK(install_with_env(NULL) == A);
	CHECK(install_with_env("") == A);
	CHECK(install_with_env("1") == A);
	CHECK(install_with_env("on") == A);
	CHECK(install_with_env("all") == A);
	CHECK(install_with_env("outputs") == DXR_SDLFIX_OUTPUTS);
	CHECK(install_with_env(" outputs ") == DXR_SDLFIX_OUTPUTS);
	CHECK(install_with_env("clipboard") == DXR_SDLFIX_CLIPBOARD);
	CHECK(install_with_env("outputsX") == 0);
	CHECK(install_with_env("clipboard,outputs") == (DXR_SDLFIX_CLIPBOARD | DXR_SDLFIX_OUTPUTS));
	CHECK(install_with_env("clipboard,ime") == (DXR_SDLFIX_CLIPBOARD | DXR_SDLFIX_IME));
	CHECK(install_with_env("ime") == DXR_SDLFIX_IME);
	CHECK(install_with_env("clipboard,ime,outputs") == A);
	CHECK(install_with_env("off") == 0);
	CHECK(install_with_env("0") == 0);
	CHECK(install_with_env("imeX") == 0);
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

	// Clipboard: the addresses and offsets its own signatures read.
	const struct dxr_sdl_sig *ca = &R->clipboard_sigs[0], *sd = &R->clipboard_sigs[1], *cgd = &R->clipboard_sigs[2],
	                         *gs = &R->clipboard_sigs[3], *so = &R->clipboard_sigs[4], *st = &R->clipboard_sigs[5];
	// cancelled: a jmp (0xe9) to Wayland_data_source_destroy, which frees through SDL_free (slot 0x27e76e8).
	CHECK(ca->at == R->data_source_cancelled);
	CHECK(ca->bytes[0] == 0xe9 && ca->at + 5 + (uintptr_t)(intptr_t)rel32(ca->bytes + 1) == sd->at);
	CHECK(sd->at + sd->len + (uintptr_t)(intptr_t)rel32(sd->bytes + sd->len - 4) == 0x27e76e8);
	// Wayland_GetClipboardText: _this->driverdata (+0x5c8), ->input (+0xc8), ->data_device (+0x28) ...
	CHECK(u32(cgd->bytes + 3) == R->off_driverdata);
	CHECK(u32(cgd->bytes + 15) == R->off_input);
	CHECK(cgd->bytes[27] == R->off_data_device);
	// ... its fallback to ->selection_source (+0x30); set_selection destroys the old ->selection_source (+0x30).
	CHECK(gs->bytes[3] == R->off_selection_source);
	CHECK(so->bytes[3] == R->off_selection_source);
	// SDL_SetClipboardText: _this (rip-relative, 7-byte mov) is the video_device the wrapper reads, and its
	// SetClipboardText(_this, ...) is _this's +0x288. This is the _this check when only "clipboard" is on.
	CHECK(st->bytes[0] == 0x4c && st->bytes[1] == 0x8b && st->bytes[2] == 0x35);
	CHECK(st->at + 7 + (uintptr_t)(intptr_t)rel32(st->bytes + 3) == R->video_device);
	CHECK(u32(st->bytes + 40) == 0x288 && st->bytes[49] == 0x4c && st->bytes[50] == 0x89 && st->bytes[51] == 0xf7);
	// The wl_data_source listener sits in the same RELRO; its cancelled entry is index 2.
	CHECK(R->data_source_listener >= 0x2734260 && R->data_source_listener + 48 <= 0x2734260 + 0x96da0);

	// IME: the six handlers are the ones the signatures cover. enter / leave / delete_surrounding_text
	// are a bare ret (0xc3); preedit follows leave, done follows delete_surrounding_text.
	const struct dxr_sdl_sig *ip = &R->ime_sigs[0], *ic = &R->ime_sigs[1], *is = &R->ime_sigs[2],
	                         *id = &R->ime_sigs[3];
	CHECK(ip->at == R->ti_fns[0] && R->ti_fns[1] == ip->at + 1 && R->ti_fns[2] == ip->at + 2);
	CHECK(ip->bytes[0] == 0xc3 && ip->bytes[1] == 0xc3);
	CHECK(ic->at == R->ti_fns[3]);
	CHECK(id->at == R->ti_fns[4] && R->ti_fns[5] == id->at + 1 && id->bytes[0] == 0xc3);
	// commit_string's loop calls SDL_SendKeyboardText (0x269a82c) with the whole chunk.
	CHECK(is->at > ic->at && is->at < R->ti_fns[4]);
	CHECK(is->bytes[18] == 0xe8 && is->at + 23 + (uintptr_t)(intptr_t)rel32(is->bytes + 19) == 0x269a82c);
	// The listener (6 entries) sits in 6000.4.6f1's RELRO.
	CHECK(R->text_input_listener >= 0x2734260 && R->text_input_listener + 48 <= 0x2734260 + 0x96da0);
}

static void
test_fix(void)
{
	setenv("DISPLAYXR_SDL_FIXUPS", "outputs", 1);
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
	unsetenv("DISPLAYXR_SDL_FIXUPS");
}

static void
test_clipboard(void)
{
	unsetenv("DISPLAYXR_SDL_FIXUPS");
	image_new(MUT_NONE);
	CHECK(image_install() == DXR_SDLFIX_ALL);
	CHECK(img.cs_table[2] != (void *)&sdl_source_cancelled); // cancelled: the wrapper
	CHECK(img.cs_table[0] == (void *)&sdl_global);           // the others untouched
	CHECK(strncmp(prot_of(img.cs_table), "r--", 3) == 0);    // RELRO read-only again

	// SDL's selection source cancelled (another app copied): cleared before SDL frees it.
	void *src = calloc(1, 0x18);
	clip_set_selection(src);
	dispatch_cancelled(src);
	CHECK(cm.cancelled_calls == 1); // SDL's handler ran
	CHECK(clip_selection() == NULL);
	clip_model_set_selection(calloc(1, 0x18)); // the next copy
	CHECK(cm.used_freed_source == 0);
	// Many copy / other-app-copy cycles.
	for (int k = 0; k < 20; k++) {
		void *s2 = calloc(1, 0x18);
		clip_model_set_selection(s2);
		dispatch_cancelled(s2);
		CHECK(clip_selection() == NULL);
	}
	clip_model_set_selection(calloc(1, 0x18));
	CHECK(cm.used_freed_source == 0);
	// A cancelled source that is not the selection (SDL replaced it): the device is left alone.
	void *keep = calloc(1, 0x18);
	clip_set_selection(keep);
	dispatch_cancelled(calloc(1, 0x18));
	CHECK(clip_selection() == keep);
	CHECK(cm.cancelled_calls == 22);
}

//! DISPLAYXR_SDL_FIXUPS=clipboard: only the clipboard fix, and its own signatures still check
//! SDL's _this (with the outputs fix off, nothing else would).
static void
test_clipboard_only(void)
{
	setenv("DISPLAYXR_SDL_FIXUPS", "clipboard", 1);
	image_new(MUT_NONE);
	CHECK(image_install() == DXR_SDLFIX_CLIPBOARD);
	CHECK(img.table[1] == (void *)&sdl_remove);              // outputs: untouched
	CHECK(img.cs_table[2] != (void *)&sdl_source_cancelled); // clipboard: the wrapper
	void *src = calloc(1, 0x18);
	clip_set_selection(src);
	dispatch_cancelled(src);
	CHECK(clip_selection() == NULL);
	// A change in the SetClipboardText / _this signature alone refuses it.
	image_new(MUT_CS5);
	CHECK(image_install() == 0);
	CHECK(img.cs_table[2] == (void *)&sdl_source_cancelled);
	// The fake image's _this is the one the wrapper follows: a source cancelled through it is found.
	image_new(MUT_NONE);
	CHECK(image_install() == DXR_SDLFIX_CLIPBOARD);
	*s_this_slot = NULL; // no SDL video device: the wrapper must not touch anything
	void *src2 = calloc(1, 0x18);
	clip_set_selection(src2);
	dispatch_cancelled(src2);
	CHECK(clip_selection() == src2 && cm.cancelled_calls == 1);
	unsetenv("DISPLAYXR_SDL_FIXUPS");
}

static void
test_clipboard_kill_switch_reproduces_bug(void)
{
	const char *offs[] = {"off", "0", "outputs"}; // none of them installs the clipboard fix
	for (int i = 0; i < 3; i++) {
		setenv("DISPLAYXR_SDL_FIXUPS", offs[i], 1);
		image_new(MUT_NONE);
		CHECK((image_install() & DXR_SDLFIX_CLIPBOARD) == 0);
		CHECK(img.cs_table[2] == (void *)&sdl_source_cancelled);
		void *src = calloc(1, 0x18);
		clip_set_selection(src);
		dispatch_cancelled(src); // SDL frees it and leaves selection_source set
		CHECK(clip_selection() == src);
		clip_model_set_selection(calloc(1, 0x18));
		CHECK(cm.used_freed_source == 1); // the use-after-free: the player's crash
	}
	unsetenv("DISPLAYXR_SDL_FIXUPS");
}

static void
test_ime(void)
{
	char cm[64], pe[64];
	int32_t info[6];
	unsetenv("DISPLAYXR_SDL_FIXUPS");
	image_new(MUT_NONE);
	CHECK(image_install() == DXR_SDLFIX_ALL);
	for (int i = 0; i < 6; i++)
		CHECK(img.ti_table[i] != (void *)NULL && img.ti_table[i] != img.b.ti_fns[i] + (char *)s_base); // all wrapped
	CHECK(strncmp(prot_of(img.ti_table), "r--", 3) == 0);

	// Before any text-input event a field cannot take the IME (an X11 player never gets one).
	CHECK(!(dxr_sdl_ime_state() & DXR_SDL_IME_AVAILABLE));
	CHECK(!(dxr_sdl_ime_own(1) & DXR_SDL_IME_OWNED));
	ti_enter();
	CHECK(strcmp(s_ti_log, "E") == 0); // enter goes on to SDL
	CHECK(dxr_sdl_ime_state() & DXR_SDL_IME_AVAILABLE);
	CHECK(dxr_sdl_ime_state() & DXR_SDL_IME_FOCUS);

	// Not owned: SDL gets everything unchanged, nothing is queued.
	s_ti_log[0] = 0;
	ti_preedit("ㅎ", 3, 3);
	ti_done(6);
	CHECK(strcmp(s_ti_log, "P(ㅎ,3,3)D(6)") == 0);
	CHECK(dxr_sdl_ime_next(cm, sizeof cm, pe, sizeof pe, info) == 0);

	// Owned: the current composition first; then the state events are kept from SDL and come out per 'done'.
	s_ti_log[0] = 0;
	CHECK(dxr_sdl_ime_own(1) & DXR_SDL_IME_OWNED);
	CHECK(dxr_sdl_ime_next(cm, sizeof cm, pe, sizeof pe, info) == 1 && strcmp(pe, "ㅎ") == 0 && info[4] == -1);
	ti_preedit(NULL, 0, 0);
	ti_commit("你好");
	ti_preedit(NULL, 0, 0);
	ti_done(6);
	ti_preedit("ni'hao", 6, 6);
	ti_done(6);
	ti_delete(3, 0);
	ti_done(6);
	CHECK(s_ti_log[0] == 0); // SDL got none of it
	CHECK(dxr_sdl_ime_next(cm, sizeof cm, pe, sizeof pe, info) == 1 && strcmp(cm, "你好") == 0 && info[5] == -1);
	CHECK(dxr_sdl_ime_next(cm, sizeof cm, pe, sizeof pe, info) == 1 && info[4] == -1 && strcmp(pe, "ni'hao") == 0 &&
	      info[0] == 6 && info[1] == 6);
	CHECK(dxr_sdl_ime_next(cm, sizeof cm, pe, sizeof pe, info) == 1 && info[2] == 3 && info[3] == 0 && info[5] == -1);
	CHECK(dxr_sdl_ime_next(cm, sizeof cm, pe, sizeof pe, info) == 0);
	// Two commits in one group come out as one.
	ti_commit("한");
	ti_commit("글");
	ti_done(6);
	CHECK(dxr_sdl_ime_next(cm, sizeof cm, pe, sizeof pe, info) == 1 && strcmp(cm, "한글") == 0);
	// A buffer too small: the change stays, its sizes are told.
	ti_commit("abcdefghij");
	ti_done(6);
	char tiny[4];
	CHECK(dxr_sdl_ime_next(tiny, sizeof tiny, pe, sizeof pe, info) == -1 && info[4] == 10);
	CHECK(dxr_sdl_ime_next(cm, sizeof cm, pe, sizeof pe, info) == 1 && strcmp(cm, "abcdefghij") == 0);

	// Given back with a composition showing: the IME is reset on SDL's text input, and its own commit of
	// that composition is dropped once; the rest goes on to SDL.
	ti_preedit("한", 3, 3);
	ti_done(6);
	while (dxr_sdl_ime_next(cm, sizeof cm, pe, sizeof pe, info) == 1)
		;
	s_ti_log[0] = 0;
	CHECK(!(dxr_sdl_ime_release(1) & DXR_SDL_IME_OWNED));
	CHECK(strcmp(s_reset_log, "27157") == 0 && s_reset_proxy == TI_PROXY); // disable, commit, enable, content type, commit
	ti_preedit(NULL, 0, 0);
	ti_commit("한"); // the IME's flush on the reset
	ti_done(8);
	ti_commit("한"); // a real one afterwards
	ti_done(8);
	CHECK(strcmp(s_ti_log, "P(nil,0,0)D(8)C(한)D(8)") == 0);
	CHECK(dxr_sdl_fixups_test_ime_echoes() == 1);
	// Given back without a composition: no reset needed.
	s_reset_log[0] = 0;
	dxr_sdl_ime_own(1);
	dxr_sdl_ime_release(1);
	CHECK(s_reset_log[0] == 0);
}

//! A fresh install with a field owning the IME and composing @p comp, then given back (the reset).
static void
ime_owned_then_released(const char *comp, int drop_echo)
{
	char cm[64], pe[64];
	int32_t info[6];
	unsetenv("DISPLAYXR_SDL_FIXUPS");
	image_new(MUT_NONE);
	CHECK(image_install() == DXR_SDLFIX_ALL);
	ti_enter();
	CHECK(dxr_sdl_ime_own(1) & DXR_SDL_IME_OWNED);
	ti_preedit(comp, (int32_t)strlen(comp), (int32_t)strlen(comp));
	ti_done(6);
	while (dxr_sdl_ime_next(cm, sizeof cm, pe, sizeof pe, info) == 1)
		;
	s_ti_log[0] = 0;
	s_reset_log[0] = 0;
	dxr_sdl_ime_release(drop_echo);
	CHECK(strncmp(s_reset_log, "2715", 4) == 0);
}

static void
test_ime_flush_exact(void)
{
	// 1. The IME flushes the composition on the reset (ibus-hangul): that commit is dropped, once.
	ime_owned_then_released("한", 1);
	ti_preedit(NULL, 0, 0);
	ti_commit("한");
	ti_done(8);
	CHECK(strcmp(s_ti_log, "P(nil,0,0)D(8)") == 0);
	CHECK(dxr_sdl_fixups_test_ime_echoes() == 1);
	ti_commit("한"); // typed again: genuine
	ti_done(9);
	CHECK(strcmp(s_ti_log, "P(nil,0,0)D(8)C(한)D(9)") == 0);
	CHECK(dxr_sdl_fixups_test_ime_echoes() == 1);

	// 2. The IME does not flush; the user then types the same syllable again: it is kept.
	ime_owned_then_released("한", 1);
	ti_preedit("ㅎ", 3, 3);
	ti_done(8);
	ti_preedit("한", 3, 3);
	ti_done(8);
	ti_preedit(NULL, 0, 0);
	ti_commit("한");
	ti_done(8);
	CHECK(strcmp(s_ti_log, "P(ㅎ,3,3)D(8)P(한,3,3)D(8)P(nil,0,0)C(한)D(8)") == 0);
	CHECK(dxr_sdl_fixups_test_ime_echoes() == 0);

	// 3. The IME's first commit after the reset is a different text: kept, and the drop is disarmed
	//    (the same text afterwards is kept too).
	ime_owned_then_released("한", 1);
	ti_commit("글");
	ti_done(8);
	ti_commit("한");
	ti_done(9);
	CHECK(strcmp(s_ti_log, "C(글)D(8)C(한)D(9)") == 0);
	CHECK(dxr_sdl_fixups_test_ime_echoes() == 0);

	// 4. A new composition equal to the old one does not disarm (the IME may restate it before
	//    flushing); a new focus does: no flush of the old focus comes after it.
	ime_owned_then_released("한", 1);
	ti_preedit("한", 3, 3);
	ti_commit("한");
	ti_done(8);
	CHECK(strcmp(s_ti_log, "P(한,3,3)D(8)") == 0 && dxr_sdl_fixups_test_ime_echoes() == 1);
	ime_owned_then_released("한", 1);
	ti_leave();
	ti_enter();
	ti_commit("한");
	ti_done(8);
	CHECK(strcmp(s_ti_log, "LEC(한)D(8)") == 0 && dxr_sdl_fixups_test_ime_echoes() == 0);

	// 5. A delete around the caret first: not a flush, disarmed.
	ime_owned_then_released("한", 1);
	ti_delete(3, 0);
	ti_commit("한");
	ti_done(8);
	CHECK(strcmp(s_ti_log, "X(3,0)C(한)D(8)") == 0 && dxr_sdl_fixups_test_ime_echoes() == 0);

	// 6. Not asked to drop (drop_echo 0): the reset is sent, the flush goes on to SDL.
	ime_owned_then_released("한", 0);
	ti_commit("한");
	ti_done(8);
	CHECK(strcmp(s_ti_log, "C(한)D(8)") == 0 && dxr_sdl_fixups_test_ime_echoes() == 0);
}

static void
test_ime_cursor_rect(void)
{
	unsetenv("DISPLAYXR_SDL_FIXUPS");
	image_new(MUT_NONE);
	CHECK(image_install() == DXR_SDLFIX_ALL);
	ti_enter();
	// No field owns the IME: ignored.
	dxr_sdl_ime_cursor_rect(10, 20, 1, 16);
	CHECK(s_reset_log[0] == 0);
	// Owned: sent as SDL_SetTextInputRect would (set_cursor_rectangle + commit), only when it changes.
	dxr_sdl_ime_own(1);
	dxr_sdl_ime_cursor_rect(10, 20, 1, 16);
	CHECK(strcmp(s_reset_log, "6(10,20,1,16)7") == 0 && s_reset_proxy == TI_PROXY);
	dxr_sdl_ime_cursor_rect(10, 20, 1, 16);
	CHECK(strcmp(s_reset_log, "6(10,20,1,16)7") == 0);
	dxr_sdl_ime_cursor_rect(0, 0, 0, 16); // empty: ignored, the last one stays
	CHECK(strcmp(s_reset_log, "6(10,20,1,16)7") == 0);
	dxr_sdl_ime_cursor_rect(42, 20, 1, 16);
	CHECK(strcmp(s_reset_log, "6(10,20,1,16)76(42,20,1,16)7") == 0);
	// The reset re-sends the last one before its final commit.
	ti_preedit("한", 3, 3);
	ti_done(6);
	s_reset_log[0] = 0;
	dxr_sdl_ime_release(1);
	CHECK(strcmp(s_reset_log, "27156(42,20,1,16)7") == 0);
	// The next owner has not reported one yet: a reset without it.
	dxr_sdl_ime_own(1);
	ti_preedit("글", 3, 3);
	ti_done(7);
	s_reset_log[0] = 0;
	dxr_sdl_ime_release(1);
	CHECK(strcmp(s_reset_log, "27157") == 0);
}

static void
test_ime_only_and_kill_switch(void)
{
	// "ime" alone: only the IME capture.
	setenv("DISPLAYXR_SDL_FIXUPS", "ime", 1);
	image_new(MUT_NONE);
	CHECK(image_install() == DXR_SDLFIX_IME);
	CHECK(img.table[1] == (void *)&sdl_remove);
	CHECK(img.cs_table[2] == (void *)&sdl_source_cancelled);
	CHECK(img.ti_table[3] != (void *)&sdl_ti_commit);
	// Off (or not named): SDL's handlers stay, a field cannot take the IME.
	const char *offs[] = {"off", "0", "clipboard,outputs"};
	for (int i = 0; i < 3; i++) {
		setenv("DISPLAYXR_SDL_FIXUPS", offs[i], 1);
		image_new(MUT_NONE);
		CHECK((image_install() & DXR_SDLFIX_IME) == 0);
		CHECK(img.ti_table[3] == (void *)&sdl_ti_commit);
		ti_enter();
		ti_commit("你好");
		ti_done(6);
		CHECK(strcmp(s_ti_log, "EC(你好)D(6)") == 0); // straight to SDL
		CHECK(!(dxr_sdl_ime_own(1) & DXR_SDL_IME_OWNED));
	}
	unsetenv("DISPLAYXR_SDL_FIXUPS");
}

static void
test_kill_switch_reproduces_bug(void)
{
	const char *offs[] = {"off", "0", "ime", "clipboard"}; // none of them installs the outputs fix
	for (int i = 0; i < 4; i++) {
		setenv("DISPLAYXR_SDL_FIXUPS", offs[i], 1);
		image_new(MUT_NONE);
		CHECK((image_install() & DXR_SDLFIX_OUTPUTS) == 0);
		CHECK(img.table[1] == (void *)&sdl_remove);
		model_add(3);
		model_add(4);
		model_add(5);
		dispatch_remove(4);
		CHECK(strcmp(model_list(), "3,!") == 0); // SDL left the freed node linked
		model_add(6);
		CHECK(m.walked_freed == 1); // the player's crash
	}
	unsetenv("DISPLAYXR_SDL_FIXUPS");
}

static void
test_refusals(void)
{
	unsetenv("DISPLAYXR_SDL_FIXUPS");
	for (int mut = MUT_GLOBAL; mut <= MUT_SIG3; mut++) {
		image_new(mut);
		void *before = img.table[1];
		CHECK(image_install() == (DXR_SDLFIX_CLIPBOARD | DXR_SDLFIX_IME)); // the outputs fix refused, the others not affected
		CHECK(img.table[1] == before);
	}
	for (int mut = MUT_CS_CANCELLED; mut <= MUT_CS5; mut++) {
		image_new(mut);
		void *before = img.cs_table[2];
		CHECK(image_install() == (DXR_SDLFIX_IME | DXR_SDLFIX_OUTPUTS)); // the clipboard fix refused
		CHECK(img.cs_table[2] == before);
	}
	for (int mut = MUT_TI_ENTRY; mut <= MUT_TI3; mut++) {
		image_new(mut);
		void *before[6];
		memcpy(before, img.ti_table, sizeof(before));
		CHECK(image_install() == (DXR_SDLFIX_CLIPBOARD | DXR_SDLFIX_OUTPUTS)); // the IME fix refused
		CHECK(memcmp(before, img.ti_table, sizeof(before)) == 0);           // no entry swapped
	}
	// The plug-in cannot pin itself: nothing is swapped.
	image_new(MUT_NONE);
	dxr_sdl_fixups_test_pin(0);
	CHECK(image_install() == 0);
	CHECK(img.table[1] == (void *)&sdl_remove);
	CHECK(img.cs_table[2] == (void *)&sdl_source_cancelled);
	CHECK(img.ti_table[3] == (void *)&sdl_ti_commit);
	dxr_sdl_fixups_test_pin(1);
	CHECK(image_install() == DXR_SDLFIX_ALL);
	// A table outside RELRO (here: RELRO said to be the data page).
	image_new(MUT_NONE);
	dxr_sdl_fixups_test_relro(s_base + 2 * (uintptr_t)img.pg, s_base + 3 * (uintptr_t)img.pg);
	CHECK(dxr_sdl_fixups_install_build(&img.b, s_base) == 0);
	CHECK(img.table[1] == (void *)&sdl_remove);
	CHECK(img.cs_table[2] == (void *)&sdl_source_cancelled);
	CHECK(img.ti_table[3] == (void *)&sdl_ti_commit);
	CHECK(image_install() == DXR_SDLFIX_ALL); // the same image, RELRO right
}

//! The swap puts back whatever protection the page had.
static void
test_previous_protection(void)
{
	unsetenv("DISPLAYXR_SDL_FIXUPS");
	image_new(MUT_RW_PAGE);
	CHECK(strncmp(prot_of(img.table), "rw-", 3) == 0);
	CHECK(image_install() == DXR_SDLFIX_ALL);
	CHECK(img.table[1] != (void *)&sdl_remove);
	CHECK(img.cs_table[2] != (void *)&sdl_source_cancelled);
	CHECK(strncmp(prot_of(img.table), "rw-", 3) == 0); // not made read-only
	image_new(MUT_NONE);
	CHECK(strncmp(prot_of(img.table), "r--", 3) == 0);
	CHECK(image_install() == DXR_SDLFIX_ALL);
	CHECK(strncmp(prot_of(img.table), "r--", 3) == 0);
	CHECK(strncmp(prot_of(img.cs_table), "r--", 3) == 0);
	CHECK(strncmp(prot_of(img.ti_table), "r--", 3) == 0);
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
	unsetenv("DISPLAYXR_SDL_FIXUPS");
	unsetenv("DISPLAYXR_SDL_FIXUPS_LOG");
	R = dxr_sdl_fixups_find_build("4ae6b3f45943aa2cf6ca0d0ac4fdec503e60e55c");
	if (!R) {
		fprintf(stderr, "FAIL: no 6000.4.6f1 entry\n");
		return 1;
	}
	test_find_build();
	test_parse();
	test_wanted();
	test_unlink();
	test_real_entry_consistent();
	test_fix();
	test_clipboard();
	test_clipboard_only();
	test_ime();
	test_ime_flush_exact();
	test_ime_cursor_rect();
	test_ime_only_and_kill_switch();
	test_kill_switch_reproduces_bug();
	test_clipboard_kill_switch_reproduces_bug();
	test_refusals();
	test_previous_protection();
	test_process_without_unityplayer();
	if (s_failed) {
		fprintf(stderr, "test_sdl_fixups: %d FAILED\n", s_failed);
		return 1;
	}
	printf("test_sdl_fixups: all passed\n");
	return 0;
}
