#include "decoder.h"

#include <mpg123.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "stream.h"
#include "util.h"

#define IN_CHUNK (16 * 1024)
// If this much input goes by without a single decodable MPEG frame, it isn't an MP3.
#define MAX_JUNK (3 * 1024 * 1024)

struct Decoder {
	mpg123_handle* mh;
	FILE* file;
	Stream* stream;
	long long size;      // total input size, -1 unknown
	bool size_told;      // mpg123 has been told the size
	bool eof;            // input exhausted
	bool buffering;
	int rate, channels, gen;
	bool pending;        // a format change takes effect on the next dec_read
	int next_rate, next_channels;
	long long fed_since_output;
	long long total_frames;
	int err_run;
	char err[128];
	uint8_t in[IN_CHUNK];
};

static bool lib_ready;

static Decoder* dec_new(void)
{
	if (!lib_ready) {
		if (mpg123_init() != MPG123_OK) return NULL;
		lib_ready = true;
	}
	Decoder* d = calloc(1, sizeof(*d));
	if (!d) return NULL;
	int e = 0;
	d->mh = mpg123_new(NULL, &e);
	if (!d->mh) {
		free(d);
		return NULL;
	}
	// Don't keep ID3v2 tags (cover art can be megabytes), allow approximate seeking, and keep
	// hunting for the next frame through junk instead of giving up.
	mpg123_param(d->mh, MPG123_ADD_FLAGS, MPG123_SKIP_ID3V2 | MPG123_FUZZY | MPG123_QUIET, 0);
	mpg123_param(d->mh, MPG123_RESYNC_LIMIT, -1, 0);
	static const long rates[] = {8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000};
	mpg123_format_none(d->mh);
	for (size_t i = 0; i < sizeof(rates) / sizeof(rates[0]); i++)
		mpg123_format(d->mh, rates[i], MPG123_MONO | MPG123_STEREO, MPG123_ENC_SIGNED_16);
	if (mpg123_open_feed(d->mh) != MPG123_OK) {
		mpg123_delete(d->mh);
		free(d);
		return NULL;
	}
	d->size = -1;
	return d;
}

Decoder* dec_open_file(const char* path)
{
	FILE* f = fopen(path, "rb");
	if (!f) return NULL;
	Decoder* d = dec_new();
	if (!d) {
		fclose(f);
		return NULL;
	}
	d->file = f;
	setvbuf(f, NULL, _IOFBF, 32 * 1024);
	if (fseek(f, 0, SEEK_END) == 0) {
		d->size = ftell(f);
		fseek(f, 0, SEEK_SET);
	}
	return d;
}

Decoder* dec_open_url(const char* url)
{
	Decoder* d = dec_new();
	if (!d) return NULL;
	d->stream = stream_open(url);
	if (!d->stream) {
		dec_close(d);
		return NULL;
	}
	d->buffering = true;
	return d;
}

void dec_close(Decoder* d)
{
	if (!d) return;
	if (d->stream) stream_close(d->stream);
	if (d->file) fclose(d->file);
	if (d->mh) {
		mpg123_close(d->mh);
		mpg123_delete(d->mh);
	}
	free(d);
}

// Pull more input into mpg123. Returns 1 if fed, 0 at EOF, -1 error, -2 no data yet.
static int feed_more(Decoder* d)
{
	int n;
	if (d->file) {
		n = (int)fread(d->in, 1, IN_CHUNK, d->file);
		if (n == 0 && ferror(d->file)) {
			snprintf(d->err, sizeof(d->err), "Couldn't read the file from the SD card.");
			return -1;
		}
	} else {
		n = stream_read(d->stream, d->in, IN_CHUNK, 30);
		d->buffering = (n == -2);
		if (n == -1) {
			stream_error(d->stream, d->err, sizeof(d->err));
			if (!d->err[0]) snprintf(d->err, sizeof(d->err), "The stream stopped unexpectedly.");
			return -1;
		}
		if (n == -2) return -2;
		if (d->size < 0) d->size = stream_size(d->stream);
	}
	if (!d->size_told && d->size > 0) {
		mpg123_set_filesize(d->mh, (off_t)d->size); // lets mpg123 estimate length and seek offsets
		d->size_told = true;
	}
	if (n == 0) {
		d->eof = true;
		return 0;
	}
	if (mpg123_feed(d->mh, d->in, (size_t)n) != MPG123_OK) {
		snprintf(d->err, sizeof(d->err), "Out of memory while decoding.");
		return -1;
	}
	d->fed_since_output += n;
	if (d->fed_since_output > MAX_JUNK) {
		snprintf(d->err, sizeof(d->err), "This doesn't look like MP3 audio (only MP3 is supported).");
		return -1;
	}
	return 1;
}

int dec_read(Decoder* d, int16_t* out, int max_frames)
{
	if (d->pending) {
		d->rate = d->next_rate;
		d->channels = d->next_channels;
		d->gen++;
		d->pending = false;
	}
	int frames = 0;
	while (frames < max_frames) {
		int ch = d->channels ? d->channels : 2;
		size_t done = 0;
		int r = mpg123_read(d->mh, (unsigned char*)(out + frames * ch), (size_t)(max_frames - frames) * (size_t)ch * 2, &done);
		if (done) {
			frames += (int)(done / ((size_t)ch * 2));
			d->fed_since_output = 0;
			d->err_run = 0;
		}
		if (r == MPG123_OK) continue;
		if (r == MPG123_NEW_FORMAT) {
			long rate = 0;
			int chans = 0, enc = 0;
			mpg123_getformat(d->mh, &rate, &chans, &enc);
			if (chans < 1 || chans > 2 || rate <= 0) {
				snprintf(d->err, sizeof(d->err), "Unsupported audio format.");
				return -1;
			}
			if ((int)rate == d->rate && chans == d->channels) continue;
			if (frames > 0) {
				// Hand back what we have in the old format; switch on the next call.
				d->next_rate = (int)rate;
				d->next_channels = chans;
				d->pending = true;
				break;
			}
			d->rate = (int)rate;
			d->channels = chans;
			d->gen++;
			continue;
		}
		if (r == MPG123_NEED_MORE) {
			if (d->eof) break;
			int f = feed_more(d);
			if (f == 1) continue;
			if (f == -1) return frames > 0 ? frames : -1;
			if (f == -2) return frames > 0 ? frames : -2;
			break; // EOF
		}
		if (r == MPG123_DONE) {
			d->eof = true;
			break;
		}
		// Decoder hiccup: mpg123 usually resynchronises on the next call.
		if (++d->err_run > 64) {
			snprintf(d->err, sizeof(d->err), "Couldn't decode this audio (%s).", mpg123_strerror(d->mh));
			return frames > 0 ? frames : -1;
		}
	}
	d->total_frames += frames;
	if (frames > 0) return frames;
	if (d->eof && d->total_frames == 0 && d->rate == 0) {
		snprintf(d->err, sizeof(d->err), "This doesn't look like MP3 audio (only MP3 is supported).");
		return -1;
	}
	return 0;
}

int dec_rate(Decoder* d) { return d->rate; }
int dec_channels(Decoder* d) { return d->channels; }
int dec_format_gen(Decoder* d) { return d->gen; }
bool dec_buffering(Decoder* d) { return d->stream ? (d->buffering || stream_buffering(d->stream)) : false; }

bool dec_seek(Decoder* d, double sec)
{
	if (d->rate == 0) return false;
	if (sec < 0) sec = 0;
	off_t in_off = 0;
	off_t pos = mpg123_feedseek(d->mh, (off_t)(sec * d->rate), SEEK_SET, &in_off);
	if (pos < 0) return false;
	if (in_off < 0) in_off = 0;
	// mpg123 dropped its buffered input; resume feeding from the byte offset it asked for.
	if (d->file) {
		if (fseek(d->file, (long)in_off, SEEK_SET) != 0) return false;
	} else {
		stream_seek(d->stream, (long long)in_off);
		d->buffering = true;
	}
	d->eof = false;
	d->pending = false;
	d->fed_since_output = 0;
	return true;
}

double dec_tell(Decoder* d)
{
	if (d->rate == 0) return 0;
	off_t t = mpg123_tell(d->mh);
	return t < 0 ? 0 : (double)t / d->rate;
}

double dec_duration(Decoder* d)
{
	if (d->rate == 0) return 0;
	off_t len = mpg123_length(d->mh);
	return len > 0 ? (double)len / d->rate : 0;
}

void dec_error(Decoder* d, char* out, int cap)
{
	str_copy(out, (size_t)cap, d->err);
}
