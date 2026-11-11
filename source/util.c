#include "util.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint64_t fnv1a64(const char* s)
{
	uint64_t h = 0xcbf29ce484222325ULL;
	for (; *s; s++) {
		h ^= (unsigned char)*s;
		h *= 0x100000001b3ULL;
	}
	return h;
}

void hash_hex(const char* s, char out[17])
{
	uint64_t h = fnv1a64(s);
	snprintf(out, 17, "%08lx%08lx", (unsigned long)(h >> 32), (unsigned long)(h & 0xffffffffUL));
}

size_t utf8_trunc(const char* s, size_t max)
{
	size_t len = strlen(s);
	if (len <= max) return len;
	size_t n = max;
	// Back up over continuation bytes so we never cut a sequence in half.
	while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
	return n;
}

void str_copy(char* dst, size_t cap, const char* src)
{
	if (!cap) return;
	size_t n = utf8_trunc(src, cap - 1);
	memmove(dst, src, n);
	dst[n] = 0;
}

void str_sanitize(char* s)
{
	char* w = s;
	bool space = true; // swallow leading whitespace
	for (char* r = s; *r; r++) {
		unsigned char c = (unsigned char)*r;
		if (c <= 0x20 || c == 0x7f) {
			if (!space) *w++ = ' ';
			space = true;
		} else {
			*w++ = (char)c;
			space = false;
		}
	}
	if (w > s && w[-1] == ' ') w--;
	*w = 0;
}

static int put_utf8(char* w, unsigned cp)
{
	if (cp < 0x80) { w[0] = (char)cp; return 1; }
	if (cp < 0x800) { w[0] = (char)(0xC0 | (cp >> 6)); w[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
	if (cp < 0x10000) {
		w[0] = (char)(0xE0 | (cp >> 12)); w[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); w[2] = (char)(0x80 | (cp & 0x3F));
		return 3;
	}
	if (cp < 0x110000) {
		w[0] = (char)(0xF0 | (cp >> 18)); w[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
		w[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); w[3] = (char)(0x80 | (cp & 0x3F));
		return 4;
	}
	return 0;
}

void decode_entities(char* s)
{
	static const struct { const char* name; unsigned cp; } named[] = {
		{"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''}, {"nbsp", ' '},
		{"ndash", 0x2013}, {"mdash", 0x2014}, {"hellip", 0x2026}, {"rsquo", 0x2019}, {"lsquo", 0x2018},
		{"rdquo", 0x201D}, {"ldquo", 0x201C}, {"copy", 0xA9}, {"reg", 0xAE}, {"trade", 0x2122},
	};
	char* w = s;
	for (char* r = s; *r;) {
		if (*r == '&') {
			char* semi = NULL;
			for (int i = 1; i <= 10 && r[i]; i++)
				if (r[i] == ';') { semi = r + i; break; }
			if (semi) {
				unsigned cp = 0;
				bool ok = false;
				if (r[1] == '#') {
					char* end;
					cp = (r[2] == 'x' || r[2] == 'X') ? strtoul(r + 3, &end, 16) : strtoul(r + 2, &end, 10);
					ok = (end == semi) && cp > 0;
				} else {
					size_t n = (size_t)(semi - r - 1);
					for (size_t i = 0; i < sizeof(named) / sizeof(named[0]); i++)
						if (strlen(named[i].name) == n && !strncmp(named[i].name, r + 1, n)) { cp = named[i].cp; ok = true; break; }
				}
				if (ok) {
					// The decoded form is never longer than the entity, so in-place is safe.
					w += put_utf8(w, cp);
					r = semi + 1;
					continue;
				}
			}
		}
		*w++ = *r++;
	}
	*w = 0;
}

void strip_tags(char* s)
{
	char* w = s;
	for (char* r = s; *r;) {
		if (*r == '<') {
			char* end = strchr(r, '>');
			if (!end) break;
			// Block-level tags become a space so words don't run together.
			*w++ = ' ';
			r = end + 1;
		} else {
			*w++ = *r++;
		}
	}
	*w = 0;
}

void url_encode(const char* in, char* out, size_t cap)
{
	static const char hex[] = "0123456789ABCDEF";
	size_t o = 0;
	for (; *in && o + 4 < cap; in++) {
		unsigned char c = (unsigned char)*in;
		if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out[o++] = (char)c;
		else if (c == ' ') out[o++] = '+';
		else { out[o++] = '%'; out[o++] = hex[c >> 4]; out[o++] = hex[c & 15]; }
	}
	out[o] = 0;
}

void fmt_time(int sec, char* out, size_t cap)
{
	if (sec < 0) sec = 0;
	int h = sec / 3600, m = (sec / 60) % 60, s = sec % 60;
	if (h) snprintf(out, cap, "%d:%02d:%02d", h, m, s);
	else snprintf(out, cap, "%d:%02d", m, s);
}

void fmt_size(long long bytes, char* out, size_t cap)
{
	if (bytes >= 1024LL * 1024 * 1024) snprintf(out, cap, "%.1f GB", (double)bytes / (1024.0 * 1024 * 1024));
	else if (bytes >= 1024 * 1024) snprintf(out, cap, "%.1f MB", (double)bytes / (1024.0 * 1024));
	else snprintf(out, cap, "%d KB", (int)(bytes / 1024));
}

int parse_duration(const char* s)
{
	int parts[3] = {0, 0, 0}, n = 0;
	while (*s == ' ') s++;
	if (!isdigit((unsigned char)*s)) return 0;
	while (*s && n < 3) {
		if (!isdigit((unsigned char)*s)) break;
		long v = strtol(s, (char**)&s, 10);
		if (v < 0 || v > 1000000) return 0;
		parts[n++] = (int)v;
		if (*s == ':') s++;
		else break;
	}
	if (n == 1) return parts[0];
	if (n == 2) return parts[0] * 60 + parts[1];
	return parts[0] * 3600 + parts[1] * 60 + parts[2];
}

void fmt_pubdate(const char* in, char* out, size_t cap)
{
	static const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
	const char* p = strchr(in, ',');
	p = p ? p + 1 : in;
	while (*p == ' ') p++;
	char* end;
	long day = strtol(p, &end, 10);
	if (end != p && day >= 1 && day <= 31) {
		p = end;
		while (*p == ' ') p++;
		for (int i = 0; i < 12; i++) {
			if (!strncasecmp(p, months[i], 3)) {
				p += 3;
				while (*p && !isdigit((unsigned char)*p)) p++;
				long year = strtol(p, &end, 10);
				if (end != p) {
					if (year < 100) year += 2000;
					snprintf(out, cap, "%s %ld, %ld", months[i], day, year);
					return;
				}
			}
		}
	}
	// ISO 8601 (2026-11-03T...) or anything else: keep the leading date-ish part.
	str_copy(out, cap < 11 ? cap : 11, in);
	str_sanitize(out);
}

bool str_starts_with_ci(const char* s, const char* prefix)
{
	return strncasecmp(s, prefix, strlen(prefix)) == 0;
}

bool str_ends_with_ci(const char* s, const char* suffix)
{
	size_t a = strlen(s), b = strlen(suffix);
	return a >= b && strcasecmp(s + a - b, suffix) == 0;
}

bool is_hex_id(const char* s)
{
	for (int i = 0; i < 16; i++)
		if (!isxdigit((unsigned char)s[i])) return false;
	return s[16] == 0;
}
