// RSS podcast feed parsing, the on-SD episode cache, and iTunes search results.
#pragma once
#include <stdbool.h>
#include <stddef.h>

enum { EP_MP3 = 0, EP_UNSUPPORTED = 1 };

typedef struct {
	char id[17];      // hash of guid (or enclosure URL)
	char title[160];
	char url[768];
	char date[16];
	char desc[400];
	int duration;     // seconds, 0 if unknown
	long long size;   // bytes, 0 if unknown
	int kind;         // EP_MP3 / EP_UNSUPPORTED (AAC, M4A, ...)
} Episode;

typedef struct {
	char title[128];
	Episode* eps;
	int count, cap;
} Feed;

// Parse an RSS file from disk, keeping at most max_items (feeds list newest first).
// Returns 0 on success, -1 if it can't be read, -2 if it isn't a podcast feed.
int feed_parse_file(const char* path, Feed* out, int max_items);
void feed_free(Feed* f);

int feed_save_cache(const Feed* f, const char* path);
int feed_load_cache(Feed* f, const char* path);

typedef struct {
	char name[128];
	char author[96];
	char url[512];
} SearchResult;

// Parse the JSON returned by https://itunes.apple.com/search?media=podcast
int search_parse(const char* json, SearchResult* out, int max);
