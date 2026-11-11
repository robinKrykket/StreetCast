#include "stream.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "net.h"
#include "plat.h"
#include "util.h"

#define RING_SIZE (1024 * 1024)
#define REBUFFER_BYTES (48 * 1024) // wait for this much before (re)starting playback
#define MAX_FAILS 8

struct Stream {
	char url[768];
	uint8_t* ring;
	size_t rpos, fill;   // read index into ring, bytes buffered
	long long base;      // absolute stream offset of the byte at rpos
	long long total;     // total size, -1 until known
	long long skip;      // bytes still to discard (server ignored our Range request)
	bool no_range;       // server does not support Range
	bool restart;        // a seek moved the read position: abandon the current request
	bool complete;       // the request ran to the end of the file
	bool failed;         // gave up after repeated errors
	bool buffering;
	volatile bool quit;
	int fails;
	char err[128];
	PlatMutex lock;
	PlatThread thread;
	CURL* curl;
};

static size_t on_write(char* data, size_t sz, size_t n, void* ud)
{
	Stream* s = ud;
	size_t len = sz * n, done = 0;

	plat_lock(&s->lock);
	if (s->total < 0 && !s->restart) {
		curl_off_t cl = -1;
		curl_easy_getinfo(s->curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &cl);
		// With a Range request Content-Length is the remainder, so add where we started.
		if (cl > 0) s->total = (s->no_range ? 0 : s->base + (long long)s->fill) + cl;
	}
	if (s->skip > 0) {
		size_t k = (long long)len < s->skip ? len : (size_t)s->skip;
		s->skip -= (long long)k;
		done = k;
	}
	plat_unlock(&s->lock);

	while (done < len) {
		plat_lock(&s->lock);
		if (s->quit || s->restart) {
			plat_unlock(&s->lock);
			return 0; // abort the transfer
		}
		size_t space = RING_SIZE - s->fill;
		size_t k = len - done < space ? len - done : space;
		if (k) {
			size_t w = (s->rpos + s->fill) % RING_SIZE;
			size_t first = RING_SIZE - w < k ? RING_SIZE - w : k;
			memcpy(s->ring + w, data + done, first);
			memcpy(s->ring, data + done + first, k - first);
			s->fill += k;
			done += k;
			s->fails = 0;
		}
		plat_unlock(&s->lock);
		if (done < len) plat_sleep_ms(20); // ring is full: wait for the decoder to catch up
	}
	return len;
}

static int on_progress(void* ud, curl_off_t a, curl_off_t b, curl_off_t c, curl_off_t d)
{
	(void)a; (void)b; (void)c; (void)d;
	Stream* s = ud;
	return (s->quit || s->restart) ? 1 : 0;
}

// Sleep in small steps so quit/seek are noticed quickly. Returns false if interrupted.
static bool idle(Stream* s, int ms)
{
	for (int t = 0; t < ms; t += 20) {
		if (s->quit || s->restart) return false;
		plat_sleep_ms(20);
	}
	return !(s->quit || s->restart);
}

static void stream_thread(void* arg)
{
	Stream* s = arg;
	while (!s->quit) {
		plat_lock(&s->lock);
		s->restart = false;
		long long start = s->base + (long long)s->fill;
		bool at_end = s->total >= 0 && start >= s->total;
		if (at_end) s->complete = true;
		bool wait = at_end || s->failed;
		bool no_range = s->no_range;
		s->skip = (no_range && !wait) ? start : 0;
		plat_unlock(&s->lock);

		if (wait) {
			// Nothing to fetch until a seek moves us (or we are closed).
			while (idle(s, 200)) {}
			continue;
		}

		CURL* c = curl_easy_init();
		if (!c) {
			idle(s, 500);
			continue;
		}
		net_setup(c, s->url);
		curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_write);
		curl_easy_setopt(c, CURLOPT_WRITEDATA, s);
		curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
		curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, on_progress);
		curl_easy_setopt(c, CURLOPT_XFERINFODATA, s);
		if (start > 0 && !no_range) curl_easy_setopt(c, CURLOPT_RESUME_FROM_LARGE, (curl_off_t)start);
		s->curl = c;
		int rc = curl_easy_perform(c);
		long status = 0;
		curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
		s->curl = NULL;
		curl_easy_cleanup(c);

		if (s->quit) break;
		plat_lock(&s->lock);
		if (s->restart) {
			// Interrupted by a seek; loop around and start from the new offset.
		} else if (rc == CURLE_OK) {
			s->complete = true;
			if (s->total < 0) s->total = s->base + (long long)s->fill;
		} else if (rc == CURLE_RANGE_ERROR && !s->no_range) {
			s->no_range = true; // fall back to downloading from the start and discarding
		} else if (rc == CURLE_HTTP_RETURNED_ERROR && status == 416) {
			// Asked for bytes past the end: we already have everything.
			s->complete = true;
			if (s->total < 0) s->total = s->base + (long long)s->fill;
		} else {
			s->fails++;
			bool fatal = s->fails >= MAX_FAILS ||
			             (rc == CURLE_HTTP_RETURNED_ERROR && status >= 400 && status < 500 && status != 408 && status != 429) ||
			             rc == CURLE_PEER_FAILED_VERIFICATION || rc == CURLE_SSL_CACERT_BADFILE || rc == CURLE_UNSUPPORTED_PROTOCOL ||
			             rc == CURLE_URL_MALFORMAT;
			if (fatal) {
				s->failed = true;
				net_describe(rc, status, s->err, sizeof(s->err));
			}
			int backoff = 400 * s->fails;
			plat_unlock(&s->lock);
			idle(s, backoff);
			continue;
		}
		plat_unlock(&s->lock);
	}
}

Stream* stream_open(const char* url)
{
	Stream* s = calloc(1, sizeof(*s));
	if (!s) return NULL;
	s->ring = malloc(RING_SIZE);
	if (!s->ring) {
		free(s);
		return NULL;
	}
	str_copy(s->url, sizeof(s->url), url);
	s->total = -1;
	s->buffering = true;
	plat_mutex_init(&s->lock);
	// libcurl + mbedTLS handshakes want a roomy stack.
	if (!plat_thread_start(&s->thread, stream_thread, s, 96 * 1024, 1)) {
		free(s->ring);
		free(s);
		return NULL;
	}
	return s;
}

void stream_close(Stream* s)
{
	if (!s) return;
	s->quit = true;
	plat_thread_join(&s->thread);
	free(s->ring);
	free(s);
}

int stream_read(Stream* s, uint8_t* buf, int n, int timeout_ms)
{
	uint64_t deadline = plat_time_ms() + (uint64_t)timeout_ms;
	for (;;) {
		plat_lock(&s->lock);
		bool ended = s->complete && !s->restart;
		if (s->failed && s->fill == 0) {
			plat_unlock(&s->lock);
			return -1;
		}
		if (s->buffering) {
			long long need = REBUFFER_BYTES;
			if (s->total >= 0 && s->total - s->base < need) need = s->total - s->base;
			if ((long long)s->fill >= need || ended || s->failed) s->buffering = false;
		}
		if (!s->buffering && s->fill > 0) {
			size_t k = (size_t)n < s->fill ? (size_t)n : s->fill;
			size_t first = RING_SIZE - s->rpos < k ? RING_SIZE - s->rpos : k;
			memcpy(buf, s->ring + s->rpos, first);
			memcpy(buf + first, s->ring, k - first);
			s->rpos = (s->rpos + k) % RING_SIZE;
			s->fill -= k;
			s->base += (long long)k;
			plat_unlock(&s->lock);
			return (int)k;
		}
		if (s->fill == 0) {
			if (ended) {
				plat_unlock(&s->lock);
				return 0;
			}
			s->buffering = true;
		}
		plat_unlock(&s->lock);
		if (plat_time_ms() >= deadline) return -2;
		plat_sleep_ms(10);
	}
}

void stream_seek(Stream* s, long long off)
{
	if (off < 0) off = 0;
	plat_lock(&s->lock);
	if (off >= s->base && off <= s->base + (long long)s->fill) {
		// Target is already buffered: just skip ahead.
		size_t k = (size_t)(off - s->base);
		s->rpos = (s->rpos + k) % RING_SIZE;
		s->fill -= k;
		s->base = off;
	} else {
		s->base = off;
		s->rpos = 0;
		s->fill = 0;
		s->restart = true;
		s->complete = false;
		s->failed = false;
		s->fails = 0;
		s->err[0] = 0;
		s->buffering = true;
	}
	plat_unlock(&s->lock);
}

long long stream_size(Stream* s)
{
	plat_lock(&s->lock);
	long long t = s->total;
	plat_unlock(&s->lock);
	return t;
}

bool stream_buffering(Stream* s)
{
	plat_lock(&s->lock);
	bool b = s->buffering;
	plat_unlock(&s->lock);
	return b;
}

void stream_error(Stream* s, char* out, int cap)
{
	plat_lock(&s->lock);
	str_copy(out, (size_t)cap, s->err);
	plat_unlock(&s->lock);
}
