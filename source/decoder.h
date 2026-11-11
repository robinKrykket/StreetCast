// MP3 decoding (mpg123 in feed mode) from either a local file or an HTTP stream.
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct Decoder Decoder;

Decoder* dec_open_file(const char* path);
Decoder* dec_open_url(const char* url);
void dec_close(Decoder* d);

// Decode up to max_frames interleaved 16-bit frames in the current format.
// Returns frames decoded (>0), 0 at end of audio, -1 on error, -2 if waiting for data.
int dec_read(Decoder* d, int16_t* out, int max_frames);

// Output format of the frames dec_read returns next. rate is 0 until the first frame is seen.
// dec_format_gen changes whenever the format does (ads spliced in at another sample rate).
int dec_rate(Decoder* d);
int dec_channels(Decoder* d);
int dec_format_gen(Decoder* d);

// Seek to a time in seconds. Returns false if not possible yet (format unknown) or on failure.
bool dec_seek(Decoder* d, double sec);
double dec_tell(Decoder* d);     // position of the next frame dec_read will return, seconds
double dec_duration(Decoder* d); // estimated total length in seconds, 0 if unknown
bool dec_buffering(Decoder* d);
void dec_error(Decoder* d, char* out, int cap);
