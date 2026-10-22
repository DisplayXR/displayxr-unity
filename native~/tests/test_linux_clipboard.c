// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Unit test for the native-Wayland player's clipboard helpers
// (displayxr_linux_clipboard.h): the text types a read asks for, what a write may
// carry, when a read / write may go to the plug-in's data device, and what making
// that device does to SDL's (mutter's two-list rule). Built and run by CTest on Linux.

#include "displayxr_linux_clipboard.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int s_failed;

#define CHECK(cond)                                                                                                \
	do {                                                                                                       \
		if (!(cond)) {                                                                                     \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                            \
			s_failed++;                                                                                \
		}                                                                                                  \
	} while (0)

static void
test_mimes(void)
{
	CHECK(dxr_clip_mime_bit("text/plain;charset=utf-8") == 1u);
	CHECK(dxr_clip_mime_bit("UTF8_STRING") == 2u);
	CHECK(dxr_clip_mime_bit("text/plain") == 4u);
	CHECK(dxr_clip_mime_bit("TEXT") == 8u);
	CHECK(dxr_clip_mime_bit("STRING") == 16u);
	CHECK(dxr_clip_mime_bit("text/uri-list") == 0);
	CHECK(dxr_clip_mime_bit("text/html") == 0);
	CHECK(dxr_clip_mime_bit("text/plain;charset=UTF-8") == 0); // the exact spelling only, as GTK and SDL send it
	CHECK(dxr_clip_mime_bit(DXR_CLIP_OWN_MIME) == 0);           // never read as text
	CHECK(dxr_clip_mime_bit("") == 0);
	CHECK(dxr_clip_mime_bit(NULL) == 0);

	// a read asks for the best type the clipboard has
	CHECK(dxr_clip_pick(0) == -1);
	CHECK(dxr_clip_pick(dxr_clip_mime_bit("STRING")) == 4);
	CHECK(dxr_clip_pick(dxr_clip_mime_bit("text/plain") | dxr_clip_mime_bit("STRING")) == 2);
	CHECK(dxr_clip_pick(dxr_clip_mime_bit("UTF8_STRING") | dxr_clip_mime_bit("text/plain")) == 1);
	CHECK(dxr_clip_pick(0x1fu) == 0);
	CHECK(strcmp(dxr_clip_text_mimes[dxr_clip_pick(0x1fu)], "text/plain;charset=utf-8") == 0);
	// every type round-trips through its bit
	for (int i = 0; i < DXR_CLIP_TEXT_MIME_COUNT; i++)
		CHECK(dxr_clip_pick(dxr_clip_mime_bit(dxr_clip_text_mimes[i])) == i);
}

static void
test_write_checks(void)
{
	const char *cjk = "한글 文字 😀🚀"; // multi-byte UTF-8, no NUL
	CHECK(dxr_clip_check_write(cjk, (long)strlen(cjk)) == 0);
	CHECK(dxr_clip_check_write("", 0) == 0);   // empties the clipboard
	CHECK(dxr_clip_check_write(NULL, 0) == 0); // the same
	CHECK(dxr_clip_check_write(NULL, 3) == DXR_CLIP_INVALID);
	CHECK(dxr_clip_check_write("abc", -1) == DXR_CLIP_INVALID);
	CHECK(dxr_clip_check_write("a\0b", 3) == DXR_CLIP_INVALID);
	char *big = malloc(DXR_CLIP_MAX_BYTES + 1);
	memset(big, 'x', DXR_CLIP_MAX_BYTES + 1);
	CHECK(dxr_clip_check_write(big, DXR_CLIP_MAX_BYTES) == 0);
	CHECK(dxr_clip_check_write(big, DXR_CLIP_MAX_BYTES + 1) == DXR_CLIP_TOO_LARGE);
	free(big);
}

static void
test_gate(void)
{
	// SDL's device intact: its clipboard is the app's, whatever else holds
	for (int dev = 0; dev <= 1; dev++)
		for (int f = -1; f <= 1; f++)
			CHECK(dxr_clip_gate(dev, 0, f) == DXR_CLIP_USE_SDL);
	// cut, but no session / device
	CHECK(dxr_clip_gate(0, 1, 1) == DXR_CLIP_UNAVAILABLE);
	// cut, device: the focused app only
	CHECK(dxr_clip_gate(1, 1, 1) == 0);
	CHECK(dxr_clip_gate(1, 1, 0) == DXR_CLIP_NOT_FOCUSED);
	CHECK(dxr_clip_gate(1, 1, -1) == DXR_CLIP_NOT_FOCUSED);
	// the codes are distinct and negative (C# maps them one to one)
	int codes[] = {DXR_CLIP_UNAVAILABLE, DXR_CLIP_USE_SDL, DXR_CLIP_NOT_FOCUSED, DXR_CLIP_PENDING,
	               DXR_CLIP_SPACE,       DXR_CLIP_TOO_LARGE, DXR_CLIP_FAILED,    DXR_CLIP_INVALID};
	for (int i = 0; i < 8; i++) {
		CHECK(codes[i] == -1 - i);
	}
}

static void
test_make_effect(void)
{
	int cut = -9, remake = -9;
	dxr_dd_make_effect(1, &cut, &remake); // focused: SDL's survives; made again once unfocused
	CHECK(cut == 0 && remake == 1);
	dxr_dd_make_effect(0, &cut, &remake); // not focused: SDL's unlinked now, ours first already
	CHECK(cut == 1 && remake == 0);
	dxr_dd_make_effect(-1, &cut, &remake); // not known: assume cut, and make it again once unfocused
	CHECK(cut == 1 && remake == 1);
}

static void
test_drop_before_set(void)
{
	CHECK(!dxr_clip_drop_before_set(0, 7, 7));          // no text of ours on the clipboard
	CHECK(dxr_clip_drop_before_set(1, 7, 7));           // no input since: mutter would ignore the new one
	CHECK(!dxr_clip_drop_before_set(1, 7, 8));          // a newer serial: set first, then drop ours
	CHECK(dxr_clip_drop_before_set(1, 8, 7));           // an older serial (cannot happen): mutter ignores it too
	CHECK(!dxr_clip_drop_before_set(1, 0xffffffffu, 0)); // the serial wrapped: newer
	CHECK(dxr_clip_drop_before_set(1, 0, 0xffffffffu));  // ... and back: older
	CHECK(!dxr_clip_drop_before_set(1, 100, 100 + 0x7fffffffu)); // the far edge of mutter's window
}

int
main(void)
{
	test_mimes();
	test_write_checks();
	test_gate();
	test_make_effect();
	test_drop_before_set();
	if (s_failed) {
		fprintf(stderr, "%d check(s) failed\n", s_failed);
		return 1;
	}
	printf("test_linux_clipboard: all checks passed\n");
	return 0;
}
