#include "net.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "plat.h"

#ifdef __3DS__
#include <malloc.h>
#define SOC_BUFFERSIZE 0x100000
static u32* soc_buf;
static bool have_sslc;
#endif

volatile bool net_verify_tls = true;

int net_init(void)
{
#ifdef __3DS__
	soc_buf = memalign(0x1000, SOC_BUFFERSIZE);
	if (!soc_buf) return -1;
	if (R_FAILED(socInit(soc_buf, SOC_BUFFERSIZE))) {
		free(soc_buf);
		soc_buf = NULL;
		return -1;
	}
	// mbedTLS takes its entropy from the system's ssl:C service. The portlib ignores failures
	// there (leaving "random" bytes uninitialised), so make sure the service is actually open.
	have_sslc = R_SUCCEEDED(sslcInit(0));
#endif
	return curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK ? 0 : -1;
}

void net_exit(void)
{
	curl_global_cleanup();
#ifdef __3DS__
	if (have_sslc) {
		sslcExit();
		have_sslc = false;
	}
	if (soc_buf) {
		socExit();
		free(soc_buf);
		soc_buf = NULL;
	}
#endif
}

void net_setup(void* h, const char* url)
{
	CURL* c = h;
	curl_easy_setopt(c, CURLOPT_URL, url);
	curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(c, CURLOPT_MAXREDIRS, 12L);
	curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 25L);
	// Give up on a transfer that has been completely stalled for 40 seconds.
	curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);
	curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 40L);
	curl_easy_setopt(c, CURLOPT_USERAGENT, "StreetCast/1.0 (Nintendo 3DS; podcast player)");
	curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
	curl_easy_setopt(c, CURLOPT_BUFFERSIZE, 32L * 1024);
	curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "http,https");
	curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
	if (net_verify_tls) {
		curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
		curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);
#ifdef CA_BUNDLE
		curl_easy_setopt(c, CURLOPT_CAINFO, CA_BUNDLE);
#endif
	} else {
		curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
		curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
	}
}

void net_describe(int code, long status, char* err, size_t cap)
{
	if (!err || !cap) return;
	switch (code) {
	case CURLE_OK: err[0] = 0; break;
	case CURLE_COULDNT_RESOLVE_HOST: snprintf(err, cap, "Couldn't find the server. Is Wi-Fi connected?"); break;
	case CURLE_COULDNT_CONNECT: snprintf(err, cap, "Couldn't connect to the server."); break;
	case CURLE_OPERATION_TIMEDOUT: snprintf(err, cap, "The connection timed out."); break;
	case CURLE_HTTP_RETURNED_ERROR: snprintf(err, cap, "The server returned an error (HTTP %ld).", status); break;
	case CURLE_PEER_FAILED_VERIFICATION:
	case CURLE_SSL_CACERT_BADFILE:
	case CURLE_SSL_CERTPROBLEM:
		snprintf(err, cap, "Secure connection check failed. Check the system clock, or turn off \"Verify HTTPS\" in Settings.");
		break;
	case CURLE_SSL_CONNECT_ERROR: snprintf(err, cap, "Couldn't set up a secure connection to the server."); break;
	case CURLE_WRITE_ERROR: snprintf(err, cap, "Couldn't write to the SD card (is it full?)."); break;
	case CURLE_ABORTED_BY_CALLBACK: snprintf(err, cap, "Cancelled."); break;
	case CURLE_FILESIZE_EXCEEDED: snprintf(err, cap, "The response was too large."); break;
	case CURLE_OUT_OF_MEMORY: snprintf(err, cap, "Out of memory."); break;
	default: snprintf(err, cap, "Network error %d (%s).", code, curl_easy_strerror((CURLcode)code)); break;
	}
	// Tag every message with the raw code: invaluable when diagnosing from a photo of the screen.
	if (code != CURLE_OK && code != CURLE_ABORTED_BY_CALLBACK) {
		size_t n = strlen(err);
		if (n + 12 < cap) snprintf(err + n, cap - n, " [E%d]", code);
	}
}

typedef struct {
	NetProgress* p;
	long long base; // bytes already on disk when resuming
} ProgCtx;

static int on_progress(void* ud, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow)
{
	(void)ultotal; (void)ulnow;
	ProgCtx* ctx = ud;
	if (!ctx->p) return 0;
	ctx->p->now = ctx->base + dlnow;
	ctx->p->total = dltotal > 0 ? ctx->base + dltotal : 0;
	return ctx->p->cancel ? 1 : 0;
}

static size_t on_file_write(char* data, size_t sz, size_t n, void* ud)
{
	return fwrite(data, 1, sz * n, (FILE*)ud);
}

static long long file_size(const char* path)
{
	FILE* f = fopen(path, "rb");
	if (!f) return 0;
	fseek(f, 0, SEEK_END);
	long long s = ftell(f);
	fclose(f);
	return s > 0 ? s : 0;
}

static int download_once(const char* url, const char* path, long long from, bool compressed, NetProgress* p, long* status)
{
	FILE* f = fopen(path, from > 0 ? "ab" : "wb");
	if (!f) return CURLE_WRITE_ERROR;
	// A large stdio buffer keeps SD writes in big sequential chunks.
	char* iobuf = malloc(64 * 1024);
	if (iobuf) setvbuf(f, iobuf, _IOFBF, 64 * 1024);

	CURL* c = curl_easy_init();
	if (!c) {
		fclose(f);
		free(iobuf);
		return CURLE_OUT_OF_MEMORY;
	}
	ProgCtx ctx = {p, from};
	net_setup(c, url);
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_file_write);
	curl_easy_setopt(c, CURLOPT_WRITEDATA, f);
	curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, on_progress);
	curl_easy_setopt(c, CURLOPT_XFERINFODATA, &ctx);
	if (compressed) curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
	if (from > 0) curl_easy_setopt(c, CURLOPT_RESUME_FROM_LARGE, (curl_off_t)from);

	int rc = curl_easy_perform(c);
	*status = 0;
	curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, status);
	curl_easy_cleanup(c);
	if (fflush(f) != 0 && rc == CURLE_OK) rc = CURLE_WRITE_ERROR;
	if (fclose(f) != 0 && rc == CURLE_OK) rc = CURLE_WRITE_ERROR;
	free(iobuf);
	return rc;
}

int net_download(const char* url, const char* path, bool resume, bool compressed, NetProgress* p, char* err, size_t errcap)
{
	long status = 0;
	long long from = resume ? file_size(path) : 0;
	int rc = download_once(url, path, from, compressed, p, &status);
	if (rc != CURLE_OK && rc != CURLE_ABORTED_BY_CALLBACK && from > 0 &&
	    (rc == CURLE_RANGE_ERROR || rc == CURLE_HTTP_RETURNED_ERROR || rc == CURLE_BAD_DOWNLOAD_RESUME)) {
		// The server would not continue the partial file; start over.
		rc = download_once(url, path, 0, compressed, p, &status);
	}
	net_describe(rc, status, err, errcap);
	return rc;
}

typedef struct {
	char* buf;
	size_t len, cap, max;
} MemCtx;

static size_t on_mem_write(char* data, size_t sz, size_t n, void* ud)
{
	MemCtx* m = ud;
	size_t add = sz * n;
	if (m->len + add > m->max) return 0;
	if (m->len + add + 1 > m->cap) {
		size_t ncap = m->cap ? m->cap * 2 : 16 * 1024;
		while (ncap < m->len + add + 1) ncap *= 2;
		char* nb = realloc(m->buf, ncap);
		if (!nb) return 0;
		m->buf = nb;
		m->cap = ncap;
	}
	memcpy(m->buf + m->len, data, add);
	m->len += add;
	m->buf[m->len] = 0;
	return add;
}

int net_fetch_mem(const char* url, char** out, size_t maxlen, NetProgress* p, char* err, size_t errcap)
{
	*out = NULL;
	CURL* c = curl_easy_init();
	if (!c) {
		net_describe(CURLE_OUT_OF_MEMORY, 0, err, errcap);
		return CURLE_OUT_OF_MEMORY;
	}
	MemCtx m = {NULL, 0, 0, maxlen};
	ProgCtx ctx = {p, 0};
	net_setup(c, url);
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_mem_write);
	curl_easy_setopt(c, CURLOPT_WRITEDATA, &m);
	curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, on_progress);
	curl_easy_setopt(c, CURLOPT_XFERINFODATA, &ctx);
	curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
	int rc = curl_easy_perform(c);
	long status = 0;
	curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
	curl_easy_cleanup(c);
	if (rc == CURLE_WRITE_ERROR) rc = CURLE_FILESIZE_EXCEEDED; // our callback refused the data
	if (rc == CURLE_OK && !m.buf) {
		m.buf = calloc(1, 1);
		if (!m.buf) rc = CURLE_OUT_OF_MEMORY;
	}
	if (rc != CURLE_OK) {
		free(m.buf);
		m.buf = NULL;
	}
	*out = m.buf;
	net_describe(rc, status, err, errcap);
	return rc;
}
