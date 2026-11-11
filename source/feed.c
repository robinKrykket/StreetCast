#include "feed.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

// ---------------------------------------------------------------------------------------------
// RSS parsing. Feeds can be many megabytes, so the file is scanned through a sliding window that
// only ever has to hold one <item> at a time.
// ---------------------------------------------------------------------------------------------

#define WIN_CHUNK (32 * 1024)
#define WIN_MAX (3 * 1024 * 1024) // largest single <item> we are willing to buffer

typedef struct {
	FILE* f;
	char* buf;
	size_t len, cap;
	bool eof;
} Win;

static bool win_more(Win* w)
{
	if (w->eof) return false;
	if (w->len + WIN_CHUNK + 1 > w->cap) {
		if (w->cap >= WIN_MAX) return false;
		size_t ncap = w->cap ? w->cap * 2 : WIN_CHUNK * 2;
		char* nb = realloc(w->buf, ncap);
		if (!nb) return false;
		w->buf = nb;
		w->cap = ncap;
	}
	size_t n = fread(w->buf + w->len, 1, WIN_CHUNK, w->f);
	if (n == 0) {
		w->eof = true;
		return false;
	}
	for (size_t i = 0; i < n; i++)
		if (w->buf[w->len + i] == 0) w->buf[w->len + i] = ' '; // keep it a C string
	w->len += n;
	w->buf[w->len] = 0;
	return true;
}

static void win_drop(Win* w, size_t n)
{
	if (n > w->len) n = w->len;
	memmove(w->buf, w->buf + n, w->len - n + 1);
	w->len -= n;
}

// Find "<name" followed by whitespace, '>' or '/'.
static char* find_open(const char* s, const char* name)
{
	size_t n = strlen(name);
	for (const char* p = s; (p = strchr(p, '<')) != NULL; p++) {
		if (!strncmp(p + 1, name, n)) {
			char c = p[1 + n];
			if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '>' || c == '/') return (char*)p;
		}
	}
	return NULL;
}

static void remove_all(char* s, const char* needle)
{
	size_t n = strlen(needle);
	char* w = s;
	for (char* r = s; *r;) {
		if (!strncmp(r, needle, n)) r += n;
		else *w++ = *r++;
	}
	*w = 0;
}

// Copy the raw inner text of <name>...</name> (first occurrence) into out.
static bool get_tag(const char* s, const char* name, char* out, size_t cap, bool* cdata)
{
	out[0] = 0;
	if (cdata) *cdata = false;
	const char* p = find_open(s, name);
	if (!p) return false;
	const char* gt = strchr(p, '>');
	if (!gt) return false;
	if (gt[-1] == '/') return true; // <tag/>
	const char* content = gt + 1;
	char pat[48];
	snprintf(pat, sizeof(pat), "</%s", name);
	const char* e = strstr(content, pat);
	size_t n = e ? (size_t)(e - content) : strlen(content);
	if (n > cap - 1) n = cap - 1;
	memcpy(out, content, n);
	out[n] = 0;
	if (strstr(out, "<![CDATA[")) {
		if (cdata) *cdata = true;
		remove_all(out, "<![CDATA[");
		remove_all(out, "]]>");
	}
	return true;
}

static bool get_attr(const char* tag, const char* name, char* out, size_t cap)
{
	out[0] = 0;
	const char* end = strchr(tag, '>');
	if (!end) return false;
	size_t n = strlen(name);
	for (const char* p = tag + 1; p + n < end; p++) {
		if (!isspace((unsigned char)p[-1]) || strncmp(p, name, n)) continue;
		const char* q = p + n;
		while (q < end && isspace((unsigned char)*q)) q++;
		if (*q != '=') continue;
		q++;
		while (q < end && isspace((unsigned char)*q)) q++;
		char quote = *q;
		if (quote != '"' && quote != '\'') continue;
		q++;
		const char* e = memchr(q, quote, (size_t)(end - q));
		if (!e) return false;
		size_t len = (size_t)(e - q);
		if (len > cap - 1) return false; // a truncated URL is worse than none
		memcpy(out, q, len);
		out[len] = 0;
		decode_entities(out);
		str_sanitize(out);
		return true;
	}
	return false;
}

static void clean_text(char* s, bool cdata, bool html)
{
	if (!cdata) decode_entities(s);
	if (html) {
		strip_tags(s);
		decode_entities(s);
	} else if (cdata) {
		decode_entities(s);
	}
	str_sanitize(s);
}

static int classify(const char* url, const char* type)
{
	char path[768];
	str_copy(path, sizeof(path), url);
	char* q = strpbrk(path, "?#");
	if (q) *q = 0;
	if (str_ends_with_ci(path, ".mp3")) return EP_MP3;
	if (strstr(type, "mpeg") || strstr(type, "mp3")) return EP_MP3;
	static const char* bad_ext[] = {".m4a", ".aac", ".mp4", ".m4v", ".mov", ".ogg", ".opus", ".wav", ".flac", ".m4b"};
	for (size_t i = 0; i < sizeof(bad_ext) / sizeof(bad_ext[0]); i++)
		if (str_ends_with_ci(path, bad_ext[i])) return EP_UNSUPPORTED;
	if (type[0]) return EP_UNSUPPORTED; // some other declared type (audio/x-m4a, video/..., ...)
	return EP_MP3;                      // undeclared: assume MP3 and let the decoder decide
}

static bool parse_item(char* item, Episode* ep, char* scratch, size_t scratch_cap)
{
	memset(ep, 0, sizeof(*ep));
	char type[64] = "", lenbuf[32] = "";

	const char* enc = find_open(item, "enclosure");
	if (enc && get_attr(enc, "url", ep->url, sizeof(ep->url))) {
		get_attr(enc, "type", type, sizeof(type));
		get_attr(enc, "length", lenbuf, sizeof(lenbuf));
	} else {
		// Some feeds only carry Media RSS.
		const char* mc = find_open(item, "media:content");
		if (!mc || !get_attr(mc, "url", ep->url, sizeof(ep->url))) return false;
		get_attr(mc, "type", type, sizeof(type));
		get_attr(mc, "fileSize", lenbuf, sizeof(lenbuf));
	}
	if (!str_starts_with_ci(ep->url, "http://") && !str_starts_with_ci(ep->url, "https://")) return false;
	ep->kind = classify(ep->url, type);
	ep->size = atoll(lenbuf);
	if (ep->size < 0) ep->size = 0;

	bool cdata;
	if (!get_tag(item, "title", scratch, scratch_cap, &cdata) || !scratch[0]) get_tag(item, "itunes:title", scratch, scratch_cap, &cdata);
	clean_text(scratch, cdata, false);
	str_copy(ep->title, sizeof(ep->title), scratch[0] ? scratch : "(untitled episode)");

	if (get_tag(item, "guid", scratch, 512, &cdata) && scratch[0]) {
		str_sanitize(scratch);
		hash_hex(scratch, ep->id);
	} else {
		hash_hex(ep->url, ep->id);
	}

	if (get_tag(item, "pubDate", scratch, 128, NULL) || get_tag(item, "dc:date", scratch, 128, NULL)) {
		str_sanitize(scratch);
		fmt_pubdate(scratch, ep->date, sizeof(ep->date));
	}
	if (get_tag(item, "itunes:duration", scratch, 64, &cdata)) {
		str_sanitize(scratch);
		ep->duration = parse_duration(scratch);
	}

	if ((!get_tag(item, "description", scratch, scratch_cap, &cdata) || !scratch[0]) &&
	    (!get_tag(item, "itunes:summary", scratch, scratch_cap, &cdata) || !scratch[0]))
		get_tag(item, "content:encoded", scratch, scratch_cap, &cdata);
	clean_text(scratch, cdata, true);
	str_copy(ep->desc, sizeof(ep->desc), scratch);
	return true;
}

static bool feed_push(Feed* f, const Episode* ep)
{
	if (f->count == f->cap) {
		int ncap = f->cap ? f->cap * 2 : 32;
		Episode* n = realloc(f->eps, (size_t)ncap * sizeof(Episode));
		if (!n) return false;
		f->eps = n;
		f->cap = ncap;
	}
	f->eps[f->count++] = *ep;
	return true;
}

void feed_free(Feed* f)
{
	free(f->eps);
	memset(f, 0, sizeof(*f));
}

int feed_parse_file(const char* path, Feed* out, int max_items)
{
	memset(out, 0, sizeof(*out));
	Win w = {0};
	w.f = fopen(path, "rb");
	if (!w.f) return -1;
	char* scratch = malloc(4096);
	int rc = 0;
	bool looks_like_rss = false;

	// Channel header: everything before the first <item>.
	char* first = NULL;
	while (win_more(&w)) {
		if ((first = find_open(w.buf, "item")) != NULL) break;
		if (w.len > 1024 * 1024) break;
	}
	if (!w.buf || !scratch) {
		rc = -1;
		goto done;
	}
	if (!first) first = find_open(w.buf, "item");
	looks_like_rss = strstr(w.buf, "<rss") || strstr(w.buf, "<channel");
	{
		char saved = 0;
		if (first) { saved = *first; *first = 0; }
		bool cdata;
		if (get_tag(w.buf, "title", scratch, 512, &cdata)) {
			clean_text(scratch, cdata, false);
			str_copy(out->title, sizeof(out->title), scratch);
		}
		if (first) *first = saved;
	}
	if (!first) {
		rc = looks_like_rss ? 0 : -2;
		goto done;
	}
	win_drop(&w, (size_t)(first - w.buf));

	Episode ep;
	while (out->count < max_items) {
		char* start = find_open(w.buf, "item");
		if (!start) {
			// Keep a small tail in case "<item" straddles the chunk boundary.
			if (w.len > 16) win_drop(&w, w.len - 16);
			if (!win_more(&w)) break;
			continue;
		}
		if (start != w.buf) win_drop(&w, (size_t)(start - w.buf));
		char* end;
		size_t searched = 0;
		while (!(end = strstr(w.buf + searched, "</item>"))) {
			searched = w.len > 8 ? w.len - 8 : 0;
			if (!win_more(&w)) break;
		}
		if (!end) break; // truncated feed, or an absurdly large item
		*end = 0;
		if (parse_item(w.buf, &ep, scratch, 4096) && !feed_push(out, &ep)) break;
		win_drop(&w, (size_t)(end - w.buf) + 7);
	}

done:
	fclose(w.f);
	free(w.buf);
	free(scratch);
	if (rc != 0) feed_free(out);
	return rc;
}

// ---------------------------------------------------------------------------------------------
// Episode cache: one tab-separated line per episode.
// ---------------------------------------------------------------------------------------------

static void put_field(FILE* f, const char* s, char sep)
{
	for (; *s; s++) fputc((*s == '\t' || *s == '\n' || *s == '\r') ? ' ' : *s, f);
	fputc(sep, f);
}

int feed_save_cache(const Feed* f, const char* path)
{
	FILE* fp = fopen(path, "wb");
	if (!fp) return -1;
	fputs("SC1\t", fp);
	put_field(fp, f->title, '\n');
	for (int i = 0; i < f->count; i++) {
		const Episode* e = &f->eps[i];
		fprintf(fp, "%s\t%d\t%d\t%lld\t", e->id, e->kind, e->duration, e->size);
		put_field(fp, e->date, '\t');
		put_field(fp, e->url, '\t');
		put_field(fp, e->title, '\t');
		put_field(fp, e->desc, '\n');
	}
	int err = ferror(fp);
	if (fclose(fp) != 0 || err) return -1;
	return 0;
}

// Split a line on tabs, in place. Empty fields are preserved.
static int split_tabs(char* line, char** fields, int max)
{
	int n = 0;
	char* p = line;
	while (n < max) {
		fields[n++] = p;
		char* t = strchr(p, '\t');
		if (!t) break;
		*t = 0;
		p = t + 1;
	}
	return n;
}

int feed_load_cache(Feed* f, const char* path)
{
	memset(f, 0, sizeof(*f));
	FILE* fp = fopen(path, "rb");
	if (!fp) return -1;
	char* line = malloc(4096);
	if (!line) {
		fclose(fp);
		return -1;
	}
	int rc = -1;
	if (fgets(line, 4096, fp) && !strncmp(line, "SC1\t", 4)) {
		line[strcspn(line, "\r\n")] = 0;
		str_copy(f->title, sizeof(f->title), line + 4);
		rc = 0;
		while (fgets(line, 4096, fp)) {
			line[strcspn(line, "\r\n")] = 0;
			char* fl[8];
			if (split_tabs(line, fl, 8) != 8) continue;
			Episode e;
			memset(&e, 0, sizeof(e));
			str_copy(e.id, sizeof(e.id), fl[0]);
			e.kind = atoi(fl[1]);
			e.duration = atoi(fl[2]);
			e.size = atoll(fl[3]);
			str_copy(e.date, sizeof(e.date), fl[4]);
			str_copy(e.url, sizeof(e.url), fl[5]);
			str_copy(e.title, sizeof(e.title), fl[6]);
			str_copy(e.desc, sizeof(e.desc), fl[7]);
			if (!is_hex_id(fl[0]) || !e.url[0]) continue; // ids become file names: never trust the file
			if (!feed_push(f, &e)) break;
		}
	}
	free(line);
	fclose(fp);
	return rc;
}

// ---------------------------------------------------------------------------------------------
// Minimal JSON reader for iTunes search results.
// ---------------------------------------------------------------------------------------------

static void j_ws(const char** p)
{
	while (**p == ' ' || **p == '\t' || **p == '\n' || **p == '\r') (*p)++;
}

static unsigned j_hex4(const char* p)
{
	unsigned v = 0;
	for (int i = 0; i < 4; i++) {
		char c = p[i];
		v <<= 4;
		if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
		else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
		else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
		else return 0xFFFD;
	}
	return v;
}

// Parse a string value; out may be NULL to skip it.
static bool j_str(const char** pp, char* out, size_t cap)
{
	const char* p = *pp;
	if (*p != '"') return false;
	p++;
	size_t o = 0;
	while (*p && *p != '"') {
		unsigned cp;
		if (*p == '\\') {
			p++;
			switch (*p) {
			case 'n': case 'r': case 't': cp = ' '; p++; break;
			case 'b': case 'f': cp = ' '; p++; break;
			case 'u': {
				for (int i = 1; i <= 4; i++)
					if (!p[i]) return false;
				cp = j_hex4(p + 1);
				p += 5;
				if (cp >= 0xD800 && cp < 0xDC00 && p[0] == '\\' && p[1] == 'u') {
					bool full = p[2] && p[3] && p[4] && p[5];
					if (!full) return false;
					unsigned lo = j_hex4(p + 2);
					if (lo >= 0xDC00 && lo < 0xE000) {
						cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
						p += 6;
					}
				}
				break;
			}
			case 0: return false;
			default: cp = (unsigned char)*p; p++; break;
			}
		} else {
			cp = (unsigned char)*p++;
			if (out && o + 1 < cap) out[o++] = (char)cp; // raw byte (already UTF-8)
			continue;
		}
		if (out && o + 5 < cap) {
			if (cp < 0x80) out[o++] = (char)cp;
			else if (cp < 0x800) { out[o++] = (char)(0xC0 | (cp >> 6)); out[o++] = (char)(0x80 | (cp & 0x3F)); }
			else if (cp < 0x10000) {
				out[o++] = (char)(0xE0 | (cp >> 12)); out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[o++] = (char)(0x80 | (cp & 0x3F));
			} else {
				out[o++] = (char)(0xF0 | (cp >> 18)); out[o++] = (char)(0x80 | ((cp >> 12) & 0x3F));
				out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[o++] = (char)(0x80 | (cp & 0x3F));
			}
		}
	}
	if (*p != '"') return false;
	if (out) {
		out[o] = 0;
		out[utf8_trunc(out, cap - 1)] = 0;
	}
	*pp = p + 1;
	return true;
}

static bool j_skip(const char** pp, int depth)
{
	if (depth > 32) return false;
	j_ws(pp);
	const char* p = *pp;
	if (*p == '"') return j_str(pp, NULL, 0);
	if (*p == '{' || *p == '[') {
		char close = (*p == '{') ? '}' : ']';
		bool obj = (*p == '{');
		p++;
		for (;;) {
			j_ws(&p);
			if (*p == close) { p++; break; }
			if (obj) {
				if (!j_str(&p, NULL, 0)) return false;
				j_ws(&p);
				if (*p != ':') return false;
				p++;
			}
			if (!j_skip(&p, depth + 1)) return false;
			j_ws(&p);
			if (*p == ',') p++;
			else if (*p != close) return false;
		}
		*pp = p;
		return true;
	}
	if (!*p) return false;
	while (*p && !strchr(",}] \t\r\n", *p)) p++; // number / true / false / null
	*pp = p;
	return true;
}

int search_parse(const char* json, SearchResult* out, int max)
{
	const char* p = json;
	int count = 0;
	char key[64];
	j_ws(&p);
	if (*p != '{') return 0;
	p++;
	for (;;) {
		j_ws(&p);
		if (*p != '"' || !j_str(&p, key, sizeof(key))) return count;
		j_ws(&p);
		if (*p != ':') return count;
		p++;
		j_ws(&p);
		if (strcmp(key, "results") || *p != '[') {
			if (!j_skip(&p, 0)) return count;
		} else {
			p++;
			for (;;) {
				j_ws(&p);
				if (*p == ']') { p++; break; }
				if (*p != '{') return count;
				p++;
				SearchResult r;
				memset(&r, 0, sizeof(r));
				for (;;) {
					j_ws(&p);
					if (*p == '}') { p++; break; }
					if (!j_str(&p, key, sizeof(key))) return count;
					j_ws(&p);
					if (*p != ':') return count;
					p++;
					j_ws(&p);
					bool ok;
					if (*p == '"' && !strcmp(key, "collectionName")) ok = j_str(&p, r.name, sizeof(r.name));
					else if (*p == '"' && !strcmp(key, "artistName")) ok = j_str(&p, r.author, sizeof(r.author));
					else if (*p == '"' && !strcmp(key, "feedUrl")) ok = j_str(&p, r.url, sizeof(r.url));
					else ok = j_skip(&p, 0);
					if (!ok) return count;
					j_ws(&p);
					if (*p == ',') p++;
				}
				if (r.url[0] && count < max) {
					str_sanitize(r.name);
					str_sanitize(r.author);
					out[count++] = r;
				}
				j_ws(&p);
				if (*p == ',') p++;
			}
		}
		j_ws(&p);
		if (*p == ',') p++;
		else return count;
	}
}
