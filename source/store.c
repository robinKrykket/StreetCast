#include "store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "plat.h"
#include "util.h"

#define MAX_STATE 2048

typedef struct {
	char id[17];
	int pos;
	bool played;
} EpState;

Sub subs[MAX_SUBS];
int nsubs;
DlEntry dls[MAX_DLS];
int ndls;
Settings settings = {true, 15, 30, THEME_LIGHT};

static EpState states[MAX_STATE];
static int nstates;
static bool subs_dirty, state_dirty, dls_dirty, settings_dirty;

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

static void put_field(FILE* f, const char* s, char sep)
{
	for (; *s; s++) fputc((*s == '\t' || *s == '\n' || *s == '\r') ? ' ' : *s, f);
	fputc(sep, f);
}

// Write to a temp file and rename, so a power cut can't leave a half-written list.
static FILE* open_tmp(const char* name, char* tmp, char* final, int cap)
{
	snprintf(final, (size_t)cap, APP_DIR "/%s", name);
	snprintf(tmp, (size_t)cap, APP_DIR "/%s.tmp", name);
	return fopen(tmp, "wb");
}

static void commit_tmp(FILE* f, const char* tmp, const char* final)
{
	bool ok = !ferror(f);
	if (fclose(f) != 0) ok = false;
	if (ok) {
		remove(final);
		rename(tmp, final);
	} else {
		remove(tmp);
	}
}

void path_feed_cache(const char* id, char* out, int cap) { snprintf(out, (size_t)cap, APP_DIR "/cache/%s.tsv", id); }
void path_feed_tmp(char* out, int cap) { snprintf(out, (size_t)cap, APP_DIR "/cache/feed.tmp"); }
void path_episode(const char* id, char* out, int cap) { snprintf(out, (size_t)cap, APP_DIR "/episodes/%s.mp3", id); }
void path_episode_part(const char* id, char* out, int cap) { snprintf(out, (size_t)cap, APP_DIR "/episodes/%s.part", id); }

// --- subscriptions -------------------------------------------------------------------------------

static void normalize_url(const char* in, char* out, size_t cap)
{
	str_copy(out, cap, in);
	str_sanitize(out);
	// "feed://" and scheme-less URLs are common when pasted from podcast directories.
	char tmp[512];
	if (str_starts_with_ci(out, "feed://")) {
		snprintf(tmp, sizeof(tmp), "http://%s", out + 7);
		str_copy(out, cap, tmp);
	} else if (out[0] && !strstr(out, "://")) {
		snprintf(tmp, sizeof(tmp), "https://%s", out);
		str_copy(out, cap, tmp);
	}
}

int subs_find(const char* url)
{
	char u[512];
	normalize_url(url, u, sizeof(u));
	for (int i = 0; i < nsubs; i++)
		if (!strcmp(subs[i].url, u)) return i;
	return -1;
}

int subs_add(const char* url, const char* title)
{
	char u[512];
	normalize_url(url, u, sizeof(u));
	if (!str_starts_with_ci(u, "http://") && !str_starts_with_ci(u, "https://")) return -1;
	if (strlen(u) < 12 || strchr(u, ' ')) return -1;
	int i = subs_find(u);
	if (i < 0) {
		if (nsubs >= MAX_SUBS) return -1;
		i = nsubs++;
		memset(&subs[i], 0, sizeof(Sub));
		str_copy(subs[i].url, sizeof(subs[i].url), u);
		hash_hex(u, subs[i].id);
		subs[i].count = -1;
	}
	if (title && title[0]) str_copy(subs[i].title, sizeof(subs[i].title), title);
	subs_dirty = true;
	return i;
}

void subs_remove(int i)
{
	if (i < 0 || i >= nsubs) return;
	char path[128];
	path_feed_cache(subs[i].id, path, sizeof(path));
	remove(path);
	memmove(&subs[i], &subs[i + 1], (size_t)(nsubs - i - 1) * sizeof(Sub));
	nsubs--;
	subs_dirty = true;
}

void subs_touch(void) { subs_dirty = true; }

static void subs_load(void)
{
	FILE* f = fopen(APP_DIR "/subs.tsv", "rb");
	if (!f) return;
	char line[1024];
	while (fgets(line, sizeof(line), f) && nsubs < MAX_SUBS) {
		line[strcspn(line, "\r\n")] = 0;
		char* fl[3];
		if (split_tabs(line, fl, 3) != 3 || !fl[0][0]) continue;
		int i = subs_add(fl[0], fl[2]);
		if (i >= 0) subs[i].count = atoi(fl[1]);
	}
	fclose(f);
	subs_dirty = false;
}

static void subs_save(void)
{
	char tmp[96], final[96];
	FILE* f = open_tmp("subs.tsv", tmp, final, sizeof(tmp));
	if (!f) return;
	for (int i = 0; i < nsubs; i++) {
		put_field(f, subs[i].url, '\t');
		fprintf(f, "%d\t", subs[i].count);
		put_field(f, subs[i].title, '\n');
	}
	commit_tmp(f, tmp, final);
}

int subs_import_txt(void)
{
	FILE* f = fopen(APP_DIR "/feeds.txt", "rb");
	if (!f) return -1;
	char line[600];
	int added = 0;
	while (fgets(line, sizeof(line), f)) {
		str_sanitize(line);
		if (!line[0] || line[0] == '#') continue;
		if (subs_find(line) >= 0) continue;
		if (subs_add(line, NULL) >= 0) added++;
	}
	fclose(f);
	return added;
}

// --- listening state -----------------------------------------------------------------------------

static EpState* st_get(const char* id, bool create)
{
	for (int i = 0; i < nstates; i++)
		if (!strcmp(states[i].id, id)) return &states[i];
	if (!create) return NULL;
	if (nstates == MAX_STATE) {
		// Full: forget the oldest entry.
		memmove(&states[0], &states[1], (MAX_STATE - 1) * sizeof(EpState));
		nstates--;
	}
	EpState* s = &states[nstates++];
	memset(s, 0, sizeof(*s));
	str_copy(s->id, sizeof(s->id), id);
	return s;
}

int st_pos(const char* id)
{
	EpState* s = st_get(id, false);
	return s ? s->pos : 0;
}

bool st_played(const char* id)
{
	EpState* s = st_get(id, false);
	return s && s->played;
}

void st_set_pos(const char* id, int sec)
{
	EpState* s = st_get(id, sec > 0);
	if (!s || s->pos == sec) return;
	s->pos = sec;
	state_dirty = true;
}

void st_set_played(const char* id, bool played)
{
	EpState* s = st_get(id, played);
	if (!s || s->played == played) return;
	s->played = played;
	state_dirty = true;
}

static void state_load(void)
{
	FILE* f = fopen(APP_DIR "/state.tsv", "rb");
	if (!f) return;
	char line[128];
	while (fgets(line, sizeof(line), f) && nstates < MAX_STATE) {
		char* fl[3];
		line[strcspn(line, "\r\n")] = 0;
		if (split_tabs(line, fl, 3) != 3 || !is_hex_id(fl[0])) continue;
		EpState* s = &states[nstates++];
		str_copy(s->id, sizeof(s->id), fl[0]);
		s->pos = atoi(fl[1]);
		s->played = atoi(fl[2]) != 0;
	}
	fclose(f);
}

static void state_save(void)
{
	char tmp[96], final[96];
	FILE* f = open_tmp("state.tsv", tmp, final, sizeof(tmp));
	if (!f) return;
	for (int i = 0; i < nstates; i++)
		if (states[i].pos > 0 || states[i].played) fprintf(f, "%s\t%d\t%d\n", states[i].id, states[i].pos, states[i].played ? 1 : 0);
	commit_tmp(f, tmp, final);
}

// --- downloads -----------------------------------------------------------------------------------

int dls_find(const char* id)
{
	for (int i = 0; i < ndls; i++)
		if (!strcmp(dls[i].id, id)) return i;
	return -1;
}

void dls_add(const Episode* ep, const char* podcast, long long size)
{
	int i = dls_find(ep->id);
	if (i < 0) {
		if (ndls == MAX_DLS) return;
		// Newest first.
		memmove(&dls[1], &dls[0], (size_t)ndls * sizeof(DlEntry));
		ndls++;
		i = 0;
	}
	memset(&dls[i], 0, sizeof(DlEntry));
	str_copy(dls[i].id, sizeof(dls[i].id), ep->id);
	str_copy(dls[i].title, sizeof(dls[i].title), ep->title);
	str_copy(dls[i].podcast, sizeof(dls[i].podcast), podcast);
	dls[i].duration = ep->duration;
	dls[i].size = size;
	dls_dirty = true;
}

void dls_remove(int i)
{
	if (i < 0 || i >= ndls) return;
	char path[128];
	path_episode(dls[i].id, path, sizeof(path));
	remove(path);
	memmove(&dls[i], &dls[i + 1], (size_t)(ndls - i - 1) * sizeof(DlEntry));
	ndls--;
	dls_dirty = true;
}

static void dls_load(void)
{
	FILE* f = fopen(APP_DIR "/downloads.tsv", "rb");
	if (!f) return;
	char line[512];
	while (fgets(line, sizeof(line), f) && ndls < MAX_DLS) {
		char* fl[5];
		line[strcspn(line, "\r\n")] = 0;
		if (split_tabs(line, fl, 5) != 5 || !is_hex_id(fl[0])) continue;
		// Drop entries whose file has gone missing (deleted on a PC, say).
		char path[128];
		struct stat st;
		path_episode(fl[0], path, sizeof(path));
		if (stat(path, &st) != 0) {
			dls_dirty = true;
			continue;
		}
		DlEntry* d = &dls[ndls++];
		memset(d, 0, sizeof(*d));
		str_copy(d->id, sizeof(d->id), fl[0]);
		d->duration = atoi(fl[1]);
		d->size = (long long)st.st_size;
		str_copy(d->podcast, sizeof(d->podcast), fl[3]);
		str_copy(d->title, sizeof(d->title), fl[4]);
	}
	fclose(f);
}

static void dls_save(void)
{
	char tmp[96], final[96];
	FILE* f = open_tmp("downloads.tsv", tmp, final, sizeof(tmp));
	if (!f) return;
	for (int i = 0; i < ndls; i++) {
		fprintf(f, "%s\t%d\t%lld\t", dls[i].id, dls[i].duration, dls[i].size);
		put_field(f, dls[i].podcast, '\t');
		put_field(f, dls[i].title, '\n');
	}
	commit_tmp(f, tmp, final);
}

// --- settings ------------------------------------------------------------------------------------

void settings_touch(void) { settings_dirty = true; }

static void settings_load(void)
{
	FILE* f = fopen(APP_DIR "/settings.txt", "rb");
	if (!f) return;
	char line[64];
	while (fgets(line, sizeof(line), f)) {
		int v;
		if (sscanf(line, "verify_tls=%d", &v) == 1) settings.verify_tls = v != 0;
		else if (sscanf(line, "skip_back=%d", &v) == 1 && v >= 5 && v <= 120) settings.skip_back = v;
		else if (sscanf(line, "skip_fwd=%d", &v) == 1 && v >= 5 && v <= 120) settings.skip_fwd = v;
		else if (sscanf(line, "theme=%d", &v) == 1 && v >= THEME_LIGHT && v <= THEME_AUTO) settings.theme = v;
	}
	fclose(f);
}

static void settings_save(void)
{
	char tmp[96], final[96];
	FILE* f = open_tmp("settings.txt", tmp, final, sizeof(tmp));
	if (!f) return;
	fprintf(f, "verify_tls=%d\nskip_back=%d\nskip_fwd=%d\ntheme=%d\n", settings.verify_tls ? 1 : 0, settings.skip_back, settings.skip_fwd, settings.theme);
	commit_tmp(f, tmp, final);
}

// -------------------------------------------------------------------------------------------------

void store_init(void)
{
	mkdir("sdmc:/3ds", 0777);
	mkdir(APP_DIR, 0777);
	mkdir(APP_DIR "/cache", 0777);
	mkdir(APP_DIR "/episodes", 0777);
	settings_load();
	subs_load();
	state_load();
	dls_load();
	// Drop a starter feeds.txt so people can add feeds from a PC.
	FILE* f = fopen(APP_DIR "/feeds.txt", "rb");
	if (f) {
		fclose(f);
	} else if ((f = fopen(APP_DIR "/feeds.txt", "wb")) != NULL) {
		fputs("# StreetCast feed list. Put one podcast RSS URL per line, then choose\n"
		      "# Add Podcast > Import feeds.txt on the 3DS. Lines starting with # are ignored.\n",
		      f);
		fclose(f);
	}
}

void store_flush(void)
{
	if (subs_dirty) { subs_save(); subs_dirty = false; }
	if (state_dirty) { state_save(); state_dirty = false; }
	if (dls_dirty) { dls_save(); dls_dirty = false; }
	if (settings_dirty) { settings_save(); settings_dirty = false; }
}
