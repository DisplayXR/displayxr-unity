// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Unit test for the file-drop helpers of the native-Wayland player: the
// text/uri-list parser (displayxr_linux_uri_list.h) and the drop point's
// coordinates (dxr_wl_surface_point, displayxr_linux_wayland_place.h). Built and
// run by CTest on Linux.

#include "displayxr_linux_uri_list.h"
#include "displayxr_linux_wayland_place.h"

#include <stdio.h>
#include <string.h>

static int s_failed;

#define CHECK(cond)                                                                                                \
	do {                                                                                                       \
		if (!(cond)) {                                                                                     \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                            \
			s_failed++;                                                                                \
		}                                                                                                  \
	} while (0)

//! The paths of a list, joined with '|' (each path NUL-terminated in the parser's output).
static int
parse(const char *list, const char *host, char *joined, size_t joined_size, size_t *bytes)
{
	char out[1024];
	size_t n = strlen(list);
	memset(out, 0x7f, sizeof(out)); // no accidental NULs
	int count = dxr_uri_list_to_paths(list, n, host, out, bytes);
	joined[0] = 0;
	const char *p = out;
	for (int i = 0; i < count; i++) {
		if (i)
			strncat(joined, "|", joined_size - strlen(joined) - 1);
		strncat(joined, p, joined_size - strlen(joined) - 1);
		p += strlen(p) + 1;
	}
	CHECK(*p == 0);                    // the list ends with an empty string
	CHECK((size_t)(p + 1 - out) == *bytes); // ... which is the last byte written
	CHECK(*bytes <= n + 1);            // the documented bound
	return count;
}

int
main(void)
{
	char j[1024];
	size_t bytes = 0;

	// What GTK / Nautilus send: CRLF lines, spaces and non-ASCII escaped as UTF-8.
	CHECK(parse("file:///home/me/My%20Report.pdf\r\nfile:///home/me/%ED%95%9C%EA%B8%80%20%E6%96%87%E4%BB%B6.txt\r\n",
	            NULL, j, sizeof(j), &bytes) == 2);
	CHECK(!strcmp(j, "/home/me/My Report.pdf|/home/me/\xED\x95\x9C\xEA\xB8\x80 \xE6\x96\x87\xE4\xBB\xB6.txt"));
	// Raw UTF-8 (unescaped) passes through; a bare LF ends a line too; no final line end.
	CHECK(parse("file:///tmp/\xC3\xA9t\xC3\xA9\nfile:///tmp/b", NULL, j, sizeof(j), &bytes) == 2);
	CHECK(!strcmp(j, "/tmp/\xC3\xA9t\xC3\xA9|/tmp/b"));
	// localhost, this host (case-insensitive), the short form; another host is skipped.
	CHECK(parse("file://localhost/a\r\nfile://DS1/b\r\nfile:/c\r\nfile://other/d\r\n", "ds1", j, sizeof(j), &bytes) == 3);
	CHECK(!strcmp(j, "/a|/b|/c"));
	CHECK(parse("file://ds1/b\r\n", NULL, j, sizeof(j), &bytes) == 0); // host unknown to the caller: not local
	// Scheme is case-insensitive.
	CHECK(parse("FILE:///x\r\n", NULL, j, sizeof(j), &bytes) == 1 && !strcmp(j, "/x"));
	// Comments, blank lines, other schemes, relative paths: skipped.
	CHECK(parse("# a comment\r\n\r\nhttps://example.com/a.pdf\r\nsftp://h/x\r\ntrash:///y\r\nfile:rel\r\nfile:///ok\r\n",
	            NULL, j, sizeof(j), &bytes) == 1);
	CHECK(!strcmp(j, "/ok"));
	// Bad escapes, an escaped NUL and a raw '#' make the URI unusable; the others stay.
	CHECK(parse("file:///a%2\r\nfile:///b%zz\r\nfile:///c%00d\r\nfile:///e#frag\r\nfile:///f%23g\r\n", NULL, j, sizeof(j),
	            &bytes) == 1);
	CHECK(!strcmp(j, "/f#g")); // an ESCAPED '#' is a file name character
	// "%" at the very end, and escapes of '/', '%' and CR/LF inside a name.
	CHECK(parse("file:///a%\r\nfile:///p%25q%2Fr\r\nfile:///line%0Abreak\r\n", NULL, j, sizeof(j), &bytes) == 2);
	CHECK(!strcmp(j, "/p%q/r|/line\nbreak"));
	// Empty and whitespace-only lists: no paths, a single NUL.
	CHECK(parse("", NULL, j, sizeof(j), &bytes) == 0 && bytes == 1);
	CHECK(parse("\r\n \r\n", NULL, j, sizeof(j), &bytes) == 0 && bytes == 1);
	// Surrounding blanks are trimmed (some sources pad lines).
	CHECK(parse("  file:///sp  \r\n", NULL, j, sizeof(j), &bytes) == 1 && !strcmp(j, "/sp"));
	// "file://" with no path, "file:" alone.
	CHECK(parse("file://\r\nfile:\r\nfile://host\r\n", NULL, j, sizeof(j), &bytes) == 0);

	// The drop point. Logical layout (k = 1): a window whose main surface is at
	// stage (2941,191) on a desktop with X root = stage x 2.
	int stage = 0, root = -1;
	CHECK(dxr_wl_surface_point(2941, 100.25, 1, 2, &stage, &root) && stage == 3041 && root == 6083);
	// No X root conversion: stage only, root untouched.
	root = -1;
	CHECK(!dxr_wl_surface_point(191, 10.0, 1, 0, &stage, &root) && stage == 201 && root == -1);
	// Physical layout at 200 %: a surface px is 2 stage px; X root = stage.
	CHECK(dxr_wl_surface_point(3840, 50.5, 2, 1, &stage, &root) && stage == 3941 && root == 3941);
	// Negative origins (a monitor left of the primary).
	CHECK(dxr_wl_surface_point(-1280, 0.0, 1, 1, &stage, &root) && stage == -1280 && root == -1280);

	if (s_failed)
		fprintf(stderr, "%d check(s) failed\n", s_failed);
	else
		printf("test_uri_list: all checks passed\n");
	return s_failed ? 1 : 0;
}
