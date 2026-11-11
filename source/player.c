#include "player.h"

#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "decoder.h"
#include "plat.h"
#include "util.h"

#define CHANNEL 0
#define NBUF 6
#define BUF_FRAMES 4096

// One Session per player_play() call. Starting a new one never blocks the UI: the new thread
// first waits for the previous session's thread to wind down (closing sockets can take a moment).
typedef struct Session {
	PlatThread thread;
	struct Session* prev;
	volatile bool quit;
	volatile bool paused;
	double seek_req; // < 0: none (guarded by lock)
	char source[768];
	bool stream;
	double start;
	int hint_dur;
} Session;

static bool dsp_ok;
static LightLock lock;
static Session* cur;
static PlInfo info;
static ndspWaveBuf wb[NBUF];
static s16* pcm;

static void bufs_reset(void)
{
	ndspChnWaveBufClear(CHANNEL);
	for (int i = 0; i < NBUF; i++) wb[i].status = NDSP_WBUF_FREE;
}

static int queued_frames(void)
{
	int q = 0;
	bool playing = false;
	for (int i = 0; i < NBUF; i++) {
		u8 st = wb[i].status;
		if (st == NDSP_WBUF_QUEUED || st == NDSP_WBUF_PLAYING) q += (int)wb[i].nsamples;
		if (st == NDSP_WBUF_PLAYING) playing = true;
	}
	if (playing) q -= (int)ndspChnGetSamplePos(CHANNEL);
	return q > 0 ? q : 0;
}

// Publish status for the UI, unless this session has been superseded.
static void publish(Session* s, PlState st, bool buffering, double pos, double dur, const char* err)
{
	LightLock_Lock(&lock);
	if (cur == s && !s->quit) {
		info.state = st;
		info.buffering = buffering;
		if (pos >= 0) info.pos = pos;
		if (dur > 0) info.dur = dur;
		if (err) str_copy(info.err, sizeof(info.err), err);
	}
	LightLock_Unlock(&lock);
}

static void session_thread(void* arg)
{
	Session* s = arg;
	if (s->prev) {
		plat_thread_join(&s->prev->thread);
		free(s->prev);
		s->prev = NULL;
	}
	if (s->quit) return;

	Decoder* d = s->stream ? dec_open_url(s->source) : dec_open_file(s->source);
	if (!d) {
		publish(s, PL_ERROR, false, -1, 0, s->stream ? "Couldn't start the stream (out of memory?)." : "Couldn't open the downloaded file.");
		return;
	}

	ndspChnReset(CHANNEL);
	ndspChnSetInterp(CHANNEL, NDSP_INTERP_LINEAR);
	for (int i = 0; i < NBUF; i++) {
		memset(&wb[i], 0, sizeof(wb[i]));
		wb[i].data_pcm16 = pcm + i * BUF_FRAMES * 2;
	}

	int applied_gen = 0, rate = 0, channels = 2;
	double seek_pending = s->start > 1.0 ? s->start : -1.0;
	bool started = false, eof = false, failed = false;
	char err[128] = "";

	while (!s->quit) {
		LightLock_Lock(&lock);
		if (s->seek_req >= 0) {
			seek_pending = s->seek_req;
			s->seek_req = -1;
		}
		LightLock_Unlock(&lock);

		if (seek_pending >= 0 && dec_rate(d) > 0) {
			if (dec_seek(d, seek_pending)) {
				bufs_reset();
				eof = false;
			}
			seek_pending = -1;
		}

		bool paused = s->paused;
		if (ndspChnIsPaused(CHANNEL) != paused) ndspChnSetPaused(CHANNEL, paused);

		int q = queued_frames();
		double pos = rate ? dec_tell(d) - (double)q / rate : -1;
		if (pos < 0 && rate) pos = 0;
		double dur = dec_duration(d);
		if (dur <= 0) dur = s->hint_dur;

		if (paused) {
			publish(s, PL_PAUSED, false, seek_pending >= 0 ? seek_pending : pos, dur, NULL);
			svcSleepThread(30 * 1000000LL);
			continue;
		}
		if (eof) {
			if (q == 0) break; // everything has been played
			publish(s, PL_PLAYING, false, pos, dur, NULL);
			svcSleepThread(20 * 1000000LL);
			continue;
		}

		int bi = -1;
		for (int i = 0; i < NBUF; i++)
			if (wb[i].status == NDSP_WBUF_FREE || wb[i].status == NDSP_WBUF_DONE) { bi = i; break; }
		if (bi < 0) {
			publish(s, PL_PLAYING, false, pos, dur, NULL);
			svcSleepThread(10 * 1000000LL);
			continue;
		}

		int n = dec_read(d, wb[bi].data_pcm16, BUF_FRAMES);
		if (n > 0) {
			// Resuming mid-episode: don't let the first moments of the file be heard before the
			// seek (which needs the format, known only now) happens on the next pass.
			if (seek_pending >= 0 && !started) continue;
			if (dec_format_gen(d) != applied_gen) {
				// Sample rate / channel count changed (first frame, or a spliced-in ad).
				// Let audio already queued in the old format finish first.
				for (int t = 0; t < 150 && !s->quit && !s->paused && queued_frames() > 0; t++) svcSleepThread(10 * 1000000LL);
				bufs_reset();
				rate = dec_rate(d);
				channels = dec_channels(d);
				ndspChnSetFormat(CHANNEL, channels == 2 ? NDSP_FORMAT_STEREO_PCM16 : NDSP_FORMAT_MONO_PCM16);
				ndspChnSetRate(CHANNEL, (float)rate);
				applied_gen = dec_format_gen(d);
			}
			wb[bi].nsamples = (u32)n;
			DSP_FlushDataCache(wb[bi].data_pcm16, (u32)(n * channels * 2));
			ndspChnWaveBufAdd(CHANNEL, &wb[bi]);
			started = true;
			publish(s, PL_PLAYING, false, pos, dur, NULL);
		} else if (n == 0) {
			eof = true;
		} else if (n == -2) {
			// Waiting on the network.
			publish(s, started && q > 0 ? PL_PLAYING : PL_LOADING, started ? dec_buffering(d) && q == 0 : true, pos, dur, NULL);
			svcSleepThread(20 * 1000000LL);
		} else {
			dec_error(d, err, sizeof(err));
			failed = true;
			break;
		}
	}

	bool finished = !s->quit && !failed;
	if (finished) publish(s, PL_ENDED, false, -1, 0, NULL);
	else if (failed) publish(s, PL_ERROR, false, -1, 0, err[0] ? err : "Playback failed.");
	bufs_reset();
	dec_close(d);
}

bool player_init(void)
{
	LightLock_Init(&lock);
	if (R_FAILED(ndspInit())) return false;
	ndspSetOutputMode(NDSP_OUTPUT_STEREO);
	pcm = linearAlloc(NBUF * BUF_FRAMES * 2 * sizeof(s16));
	if (!pcm) {
		ndspExit();
		return false;
	}
	dsp_ok = true;
	return true;
}

bool player_exit(void)
{
	LightLock_Lock(&lock);
	Session* s = cur;
	cur = NULL;
	if (s) s->quit = true;
	LightLock_Unlock(&lock);
	if (s) {
		// Each thread joins its predecessor first. A thread stuck in a network call must not
		// keep the app from closing: give up on it and leave the DSP alone.
		if (!plat_thread_join_timeout(&s->thread, 4000)) return false;
		free(s);
	}
	if (dsp_ok) {
		ndspExit();
		linearFree(pcm);
		dsp_ok = false;
	}
	return true;
}

void player_play(const char* source, bool stream, const char* id, const char* title, const char* podcast, double start, int hint_dur)
{
	PlInfo ni;
	memset(&ni, 0, sizeof(ni));
	ni.is_stream = stream;
	ni.pos = start > 1.0 ? start : 0;
	ni.dur = hint_dur;
	str_copy(ni.id, sizeof(ni.id), id);
	str_copy(ni.title, sizeof(ni.title), title);
	str_copy(ni.podcast, sizeof(ni.podcast), podcast);

	Session* s = dsp_ok ? calloc(1, sizeof(*s)) : NULL;
	if (!s) {
		ni.state = PL_ERROR;
		snprintf(ni.err, sizeof(ni.err), dsp_ok ? "Out of memory." : "Sound isn't available: DSP firmware (dspfirm.cdc) is missing.");
		LightLock_Lock(&lock);
		if (cur) cur->quit = true;
		info = ni;
		LightLock_Unlock(&lock);
		return;
	}
	str_copy(s->source, sizeof(s->source), source);
	s->stream = stream;
	s->start = start;
	s->hint_dur = hint_dur;
	s->seek_req = -1;
	ni.state = PL_LOADING;
	ni.buffering = stream;

	LightLock_Lock(&lock);
	s->prev = cur;
	if (cur) cur->quit = true;
	cur = s;
	info = ni;
	LightLock_Unlock(&lock);

	// Above the UI thread so decoding isn't starved by drawing; it sleeps whenever buffers are full.
	if (!plat_thread_start(&s->thread, session_thread, s, 64 * 1024, -2)) {
		LightLock_Lock(&lock);
		cur = s->prev; // already told to quit; the next session will reap it
		info.state = PL_ERROR;
		snprintf(info.err, sizeof(info.err), "Couldn't start the audio thread (out of memory).");
		LightLock_Unlock(&lock);
		free(s);
	}
}

void player_stop(void)
{
	LightLock_Lock(&lock);
	if (cur) cur->quit = true;
	info.state = PL_STOPPED;
	info.buffering = false;
	LightLock_Unlock(&lock);
}

void player_set_paused(bool paused)
{
	LightLock_Lock(&lock);
	if (cur && !cur->quit && (info.state == PL_PLAYING || info.state == PL_PAUSED || info.state == PL_LOADING)) {
		cur->paused = paused;
		// Reflect it right away so the UI doesn't flicker for a frame.
		if (paused) info.state = PL_PAUSED;
		else if (info.state == PL_PAUSED) info.state = PL_PLAYING;
	}
	LightLock_Unlock(&lock);
}

void player_seek(double sec)
{
	LightLock_Lock(&lock);
	if (cur && !cur->quit && (info.state == PL_PLAYING || info.state == PL_PAUSED || info.state == PL_LOADING)) {
		if (info.dur > 0 && sec > info.dur - 2) sec = info.dur - 2;
		if (sec < 0) sec = 0;
		cur->seek_req = sec;
		info.pos = sec;
	}
	LightLock_Unlock(&lock);
}

void player_get(PlInfo* out)
{
	LightLock_Lock(&lock);
	*out = info;
	LightLock_Unlock(&lock);
}

bool player_active(void)
{
	LightLock_Lock(&lock);
	bool a = info.state == PL_PLAYING || info.state == PL_LOADING;
	LightLock_Unlock(&lock);
	return a;
}
