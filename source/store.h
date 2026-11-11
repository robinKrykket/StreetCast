// Everything persisted on the SD card under /3ds/StreetCast: subscriptions, per-episode
// listening state, the downloaded-episode registry and settings. Main thread only.
#pragma once
#include <stdbool.h>

#include "feed.h"

#define MAX_SUBS 64
#define MAX_DLS 256
#define MAX_FEED_ITEMS 300

typedef struct {
	char id[17];
	char url[512];
	char title[128];
	int count; // cached episode count, -1 if never refreshed
} Sub;

typedef struct {
	char id[17];
	char title[160];
	char podcast[128];
	int duration;
	long long size;
} DlEntry;

typedef struct {
	bool verify_tls;
	int skip_back, skip_fwd; // seconds
} Settings;

extern Sub subs[MAX_SUBS];
extern int nsubs;
extern DlEntry dls[MAX_DLS];
extern int ndls;
extern Settings settings;

void store_init(void);
void store_flush(void); // write anything dirty

// Subscriptions. subs_add returns the index (existing or new), or -1 if full/invalid.
int subs_add(const char* url, const char* title);
int subs_find(const char* url);
void subs_remove(int i);
void subs_touch(void); // mark dirty after editing subs[] in place
// Subscribe to every URL listed in feeds.txt; returns how many were new.
int subs_import_txt(void);

void path_feed_cache(const char* sub_id, char* out, int cap);
void path_feed_tmp(char* out, int cap);
void path_episode(const char* ep_id, char* out, int cap);
void path_episode_part(const char* ep_id, char* out, int cap);

// Listening state.
int st_pos(const char* ep_id);      // resume position in seconds (0 = start)
bool st_played(const char* ep_id);
void st_set_pos(const char* ep_id, int sec);
void st_set_played(const char* ep_id, bool played);

// Downloaded episodes.
int dls_find(const char* ep_id);
void dls_add(const Episode* ep, const char* podcast, long long size);
void dls_remove(int i); // also deletes the file

void settings_touch(void);
