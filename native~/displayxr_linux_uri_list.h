// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Native-Wayland player, file drops: a text/uri-list (RFC 2483) to local file
// paths. Pure string work (no Wayland), unit-tested on its own
// (native~/tests/test_uri_list.c).
//
// What a file manager sends: one URI per line, lines ended by CRLF (a bare LF is
// taken too), '#' lines are comments. Only local file URIs become paths:
//   file:///home/me/a%20b.txt      -> /home/me/a b.txt
//   file://localhost/home/me/x     -> /home/me/x
//   file://<this host>/home/me/x   -> /home/me/x   (when the caller names the host)
//   file:/home/me/x                -> /home/me/x   (RFC 8089's short form)
// Anything else is skipped: other schemes (http:, sftp:, trash:, recent:),
// another host, a relative path, a bad or NUL (%00) escape, and a raw '#' (a
// fragment: GLib's g_filename_from_uri refuses those too). Escapes are decoded
// byte by byte, so UTF-8 names (escaped or not) come through unchanged.

#pragma once

#include <stddef.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline int
dxr_uri_hex(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

//! Percent-decode [s, s + n) into out (room for at least n bytes, no NUL is
//! written). The decoded length, or -1 for a bad escape, an escaped NUL or a raw '#'.
static inline int
dxr_uri_unescape(const char *s, size_t n, char *out)
{
	size_t o = 0;
	for (size_t i = 0; i < n; i++) {
		char c = s[i];
		if (c == '#' || c == 0)
			return -1;
		if (c == '%') {
			if (i + 2 >= n)
				return -1; // cut short
			int hi = dxr_uri_hex(s[i + 1]), lo = dxr_uri_hex(s[i + 2]);
			if (hi < 0 || lo < 0 || (hi == 0 && lo == 0))
				return -1;
			out[o++] = (char)(hi * 16 + lo);
			i += 2;
			continue;
		}
		out[o++] = c;
	}
	return (int)o;
}

static inline int
dxr_uri_ieq(const char *a, const char *b, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		char x = a[i], y = b[i];
		if (x >= 'A' && x <= 'Z')
			x = (char)(x - 'A' + 'a');
		if (y >= 'A' && y <= 'Z')
			y = (char)(y - 'A' + 'a');
		if (x != y)
			return 0;
	}
	return 1;
}

//! One URI (no line end) to a local path in out (room for at least n + 1 bytes),
//! NUL-terminated. The path's length, or -1 when it is not a local file URI.
//! @p self_host: this machine's host name, also taken as local (NULL: only
//! "" and "localhost" are).
static inline int
dxr_uri_to_path(const char *uri, size_t n, const char *self_host, char *out)
{
	if (n < 6 || !dxr_uri_ieq(uri, "file:", 5))
		return -1;
	const char *p = uri + 5, *end = uri + n;
	if (end - p >= 2 && p[0] == '/' && p[1] == '/') {
		// file://host/path: the authority runs to the next '/'.
		const char *host = p + 2, *slash = host;
		while (slash < end && *slash != '/')
			slash++;
		if (slash == end)
			return -1; // no path at all
		size_t hn = (size_t)(slash - host);
		int local = hn == 0 || (hn == 9 && dxr_uri_ieq(host, "localhost", 9)) ||
		            (self_host && strlen(self_host) == hn && dxr_uri_ieq(host, self_host, hn));
		if (!local)
			return -1;
		p = slash;
	}
	if (p >= end || *p != '/')
		return -1; // relative, or file:host
	int len = dxr_uri_unescape(p, (size_t)(end - p), out);
	if (len <= 0 || out[0] != '/')
		return -1;
	out[len] = 0;
	return len;
}

//! A whole text/uri-list (not NUL-terminated: [text, text + n)) to local paths,
//! written to out one after another, each NUL-terminated, with one more NUL at
//! the end (an empty list is a single NUL). out needs room for n + 1 bytes (a
//! path is never longer than its URI, which also needs its line end or the
//! extra NUL). Returns the number of paths; *out_bytes = the bytes written.
static inline int
dxr_uri_list_to_paths(const char *text, size_t n, const char *self_host, char *out, size_t *out_bytes)
{
	size_t o = 0;
	int count = 0;
	const char *p = text, *end = text + n;
	while (p < end) {
		const char *eol = p;
		while (eol < end && *eol != '\n')
			eol++;
		const char *line_end = eol;
		if (line_end > p && line_end[-1] == '\r')
			line_end--;
		// Surrounding blanks are not part of a URI (RFC 2483 lines have none).
		while (p < line_end && (*p == ' ' || *p == '\t'))
			p++;
		while (line_end > p && (line_end[-1] == ' ' || line_end[-1] == '\t'))
			line_end--;
		if (line_end > p && *p != '#') {
			int len = dxr_uri_to_path(p, (size_t)(line_end - p), self_host, out + o);
			if (len > 0) {
				o += (size_t)len + 1;
				count++;
			}
		}
		p = eol < end ? eol + 1 : end;
	}
	out[o++] = 0;
	if (out_bytes)
		*out_bytes = o;
	return count;
}

#ifdef __cplusplus
}
#endif
