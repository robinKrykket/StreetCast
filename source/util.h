// Small portable string helpers (also compiled for host-side tests).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

uint64_t fnv1a64(const char* s);
void hash_hex(const char* s, char out[17]);

// Copy with truncation that never splits a UTF-8 sequence.
void str_copy(char* dst, size_t cap, const char* src);
// Largest byte count <= max that ends on a UTF-8 boundary.
size_t utf8_trunc(const char* s, size_t max);
// Tabs/newlines/control chars -> space, collapse runs, trim ends. In place.
void str_sanitize(char* s);
// Decode XML/HTML entities in place.
void decode_entities(char* s);
// Strip HTML tags in place (an unterminated tag drops the remainder).
void strip_tags(char* s);
void url_encode(const char* in, char* out, size_t cap);
// "M:SS" or "H:MM:SS"
void fmt_time(int sec, char* out, size_t cap);
void fmt_size(long long bytes, char* out, size_t cap);
// "3600", "62:03", "1:02:03" -> seconds (0 if unparseable)
int parse_duration(const char* s);
// RFC 822 date -> "Nov 3, 2026" (falls back to a truncated copy)
void fmt_pubdate(const char* rfc822, char* out, size_t cap);
bool str_ends_with_ci(const char* s, const char* suffix);
bool str_starts_with_ci(const char* s, const char* prefix);
// True for the 16-hex-digit ids StreetCast uses in file names (rejects anything path-like).
bool is_hex_id(const char* s);
