// libcurl wrappers: whole-file downloads (feeds, episodes) and small in-memory fetches.
#pragma once
#include <stdbool.h>
#include <stddef.h>

typedef struct {
	volatile bool cancel;      // set by the UI to abort
	volatile long long now;    // bytes so far
	volatile long long total;  // 0 if unknown
} NetProgress;

extern volatile bool net_verify_tls; // user setting: verify HTTPS certificates

int net_init(void);
void net_exit(void);

// Apply the options every StreetCast request shares (redirects, TLS, timeouts, user agent).
void net_setup(void* curl_easy, const char* url);

// Download url to path. If resume is true an existing partial file is continued.
// Returns 0 on success; on failure writes a human-readable message to err.
int net_download(const char* url, const char* path, bool resume, bool compressed, NetProgress* p, char* err, size_t errcap);

// Fetch a small document into a malloc'd, NUL-terminated buffer.
int net_fetch_mem(const char* url, char** out, size_t maxlen, NetProgress* p, char* err, size_t errcap);

// Turn a curl error code into something a person can act on.
void net_describe(int curl_code, long http_status, char* err, size_t errcap);
