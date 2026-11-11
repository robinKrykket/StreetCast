// HTTP audio stream: a background thread downloads into a ring buffer; the decoder reads from it.
// Dropped connections are resumed with Range requests, and seeking restarts the request at the
// new byte offset.
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct Stream Stream;

Stream* stream_open(const char* url);
void stream_close(Stream* s);

// Returns bytes read (>0), 0 at end of stream, -1 on a fatal error, -2 if no data arrived
// within timeout_ms (still buffering).
int stream_read(Stream* s, uint8_t* buf, int n, int timeout_ms);
// Continue reading from absolute byte offset off.
void stream_seek(Stream* s, long long off);
// Total size in bytes, or -1 if not known (yet).
long long stream_size(Stream* s);
bool stream_buffering(Stream* s);
// Copy the last error message (empty if none).
void stream_error(Stream* s, char* out, int cap);
