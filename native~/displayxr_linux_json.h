// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Just enough of a JSON reader to walk what the DisplayXR GNOME extension sends
// (GetWindows / GetAllWindows / WindowsChanged / GetPointer) without being fooled by
// a window title: strings are skipped with their escapes, so a title holding
// quotes, braces or a key name never ends an object early. Pure (no D-Bus), so it
// is unit-tested on its own (native~/tests/test_linux_desktop_info.c).
//
// Every function takes the text as [p, end) and never reads past `end`.

#pragma once

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline const char *
dxr_json_ws(const char *p, const char *end)
{
	while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
		p++;
	return p;
}

//! One past the JSON value at p, or NULL if it is cut short.
static inline const char *
dxr_json_skip(const char *p, const char *end)
{
	if (p >= end)
		return NULL;
	if (*p == '"') {
		for (p++; p < end; p++) {
			if (*p == '\\')
				p++;
			else if (*p == '"')
				return p + 1;
		}
		return NULL;
	}
	if (*p == '{' || *p == '[') {
		int depth = 0;
		for (; p < end; p++) {
			if (*p == '"') {
				p = dxr_json_skip(p, end);
				if (!p)
					return NULL;
				p--;
			} else if (*p == '{' || *p == '[') {
				depth++;
			} else if (*p == '}' || *p == ']') {
				if (--depth == 0)
					return p + 1;
			}
		}
		return NULL;
	}
	while (p < end && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\n')
		p++;
	return p;
}

//! The value of `key` at the top level of the object [obj, end), or NULL.
static inline const char *
dxr_json_field(const char *obj, const char *end, const char *key)
{
	const char *p = dxr_json_ws(obj, end);
	if (p >= end || *p != '{')
		return NULL;
	size_t key_len = strlen(key);
	for (p++;;) {
		p = dxr_json_ws(p, end);
		if (p >= end || *p != '"')
			return NULL;
		const char *k = p + 1;
		const char *after = dxr_json_skip(p, end);
		if (!after)
			return NULL;
		int match = (size_t)(after - 1 - k) == key_len && !memcmp(k, key, key_len);
		p = dxr_json_ws(after, end);
		if (p >= end || *p != ':')
			return NULL;
		const char *value = dxr_json_ws(p + 1, end);
		if (match)
			return value;
		p = dxr_json_skip(value, end);
		if (!p)
			return NULL;
		p = dxr_json_ws(p, end);
		if (p < end && *p == ',')
			p++;
		else
			return NULL;
	}
}

//! A JSON string value into out, as it is on the wire (no unescaping; for values
//! that have none, such as "logical").
static inline void
dxr_json_string(const char *v, const char *end, char *out, size_t size)
{
	out[0] = 0;
	if (!v || v >= end || *v != '"')
		return;
	const char *close = dxr_json_skip(v, end);
	if (!close)
		return;
	size_t n = (size_t)(close - 1 - (v + 1));
	if (n >= size)
		n = size - 1;
	memcpy(out, v + 1, n);
	out[n] = 0;
}

static inline int
dxr_json_hex4(const char *p, const char *end, unsigned *out)
{
	if (end - p < 4)
		return 0;
	unsigned v = 0;
	for (int i = 0; i < 4; i++) {
		char c = p[i];
		v <<= 4;
		if (c >= '0' && c <= '9')
			v |= (unsigned)(c - '0');
		else if (c >= 'a' && c <= 'f')
			v |= (unsigned)(c - 'a' + 10);
		else if (c >= 'A' && c <= 'F')
			v |= (unsigned)(c - 'A' + 10);
		else
			return 0;
	}
	*out = v;
	return 1;
}

//! A JSON string value into out as NUL-terminated UTF-8, unescaped (\" \\ \/ \b \f
//! \n \r \t \uXXXX, surrogate pairs included). Too long a value is cut at a whole
//! character. A value that is not a string, or is malformed, gives "". Returns 1
//! when the whole value fitted.
static inline int
dxr_json_string_utf8(const char *v, const char *end, char *out, size_t size)
{
	if (size == 0)
		return 0;
	out[0] = 0;
	if (!v || v >= end || *v != '"')
		return 0;
	size_t n = 0;
	for (const char *p = v + 1; p < end;) {
		unsigned cp;
		char c = *p;
		if (c == '"') {
			out[n] = 0;
			return 1;
		}
		if (c != '\\') {
			// Raw bytes (UTF-8 passes through): copy one whole sequence or none.
			unsigned char b = (unsigned char)c;
			size_t len = b < 0x80 ? 1 : (b >> 5) == 6 ? 2 : (b >> 4) == 14 ? 3 : (b >> 3) == 30 ? 4 : 1;
			if ((size_t)(end - p) < len)
				break;
			if (n + len >= size) {
				out[n] = 0;
				return 0;
			}
			memcpy(out + n, p, len);
			n += len;
			p += len;
			continue;
		}
		if (p + 1 >= end)
			break;
		char e = p[1];
		p += 2;
		switch (e) {
		case '"': cp = '"'; break;
		case '\\': cp = '\\'; break;
		case '/': cp = '/'; break;
		case 'b': cp = '\b'; break;
		case 'f': cp = '\f'; break;
		case 'n': cp = '\n'; break;
		case 'r': cp = '\r'; break;
		case 't': cp = '\t'; break;
		case 'u':
			if (!dxr_json_hex4(p, end, &cp))
				goto bad;
			p += 4;
			if (cp >= 0xD800 && cp <= 0xDBFF) {
				unsigned lo;
				if (end - p >= 6 && p[0] == '\\' && p[1] == 'u' && dxr_json_hex4(p + 2, end, &lo) &&
				    lo >= 0xDC00 && lo <= 0xDFFF) {
					cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
					p += 6;
				} else {
					cp = 0xFFFD; // a lone high surrogate
				}
			} else if (cp >= 0xDC00 && cp <= 0xDFFF) {
				cp = 0xFFFD; // a lone low surrogate
			}
			break;
		default: goto bad;
		}
		{
			char u[4];
			size_t len;
			if (cp < 0x80) {
				u[0] = (char)cp;
				len = 1;
			} else if (cp < 0x800) {
				u[0] = (char)(0xC0 | (cp >> 6));
				u[1] = (char)(0x80 | (cp & 0x3F));
				len = 2;
			} else if (cp < 0x10000) {
				u[0] = (char)(0xE0 | (cp >> 12));
				u[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
				u[2] = (char)(0x80 | (cp & 0x3F));
				len = 3;
			} else {
				u[0] = (char)(0xF0 | (cp >> 18));
				u[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
				u[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
				u[3] = (char)(0x80 | (cp & 0x3F));
				len = 4;
			}
			if (n + len >= size) {
				out[n] = 0;
				return 0;
			}
			memcpy(out + n, u, len);
			n += len;
		}
	}
bad:
	out[0] = 0; // cut short or malformed
	return 0;
}

//! A JSON number value as a double; `fallback` when the value is not a number.
static inline double
dxr_json_number(const char *v, const char *end, double fallback)
{
	if (!v || v >= end || !(*v == '-' || (*v >= '0' && *v <= '9')))
		return fallback;
	char buf[64];
	size_t n = 0;
	while (v + n < end && n < sizeof(buf) - 1 &&
	       ((v[n] >= '0' && v[n] <= '9') || v[n] == '-' || v[n] == '+' || v[n] == '.' || v[n] == 'e' || v[n] == 'E'))
		n++;
	memcpy(buf, v, n);
	buf[n] = 0;
	char *stop = NULL;
	double d = strtod(buf, &stop);
	return stop && stop != buf ? d : fallback;
}

//! A JSON true / false; `fallback` for anything else (absent, null).
static inline int
dxr_json_bool(const char *v, const char *end, int fallback)
{
	if (v && end - v >= 4 && !memcmp(v, "true", 4))
		return 1;
	if (v && end - v >= 5 && !memcmp(v, "false", 5))
		return 0;
	return fallback;
}

#ifdef __cplusplus
}
#endif
