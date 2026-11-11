// Audio playback: a decode thread feeding NDSP. All functions are called from the main thread
// and never block on the network.
#pragma once
#include <stdbool.h>

typedef enum { PL_STOPPED, PL_LOADING, PL_PLAYING, PL_PAUSED, PL_ENDED, PL_ERROR } PlState;

typedef struct {
	PlState state;
	bool buffering;   // waiting for stream data
	bool is_stream;
	double pos, dur;  // seconds (dur 0 if unknown)
	char id[17];
	char title[160];
	char podcast[128];
	char err[128];
} PlInfo;

bool player_init(void); // false if the DSP could not be started (missing dspfirm.cdc)
// Returns false if the audio thread did not stop in time (sound is then left initialised).
bool player_exit(void);

// source is a file path, or a URL when stream is true. hint_dur is the feed's duration (may be 0).
void player_play(const char* source, bool stream, const char* id, const char* title, const char* podcast, double start, int hint_dur);
void player_stop(void);
void player_set_paused(bool paused);
void player_seek(double sec);
void player_get(PlInfo* out);
// True while audio is (or is about to be) audible: used for the sleep/lid policy.
bool player_active(void);
