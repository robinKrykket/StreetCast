// Background episode download queue (one transfer at a time).
#pragma once
#include <stdbool.h>

#include "feed.h"

#define DL_MAX_JOBS 24

typedef enum { DJ_QUEUED, DJ_ACTIVE, DJ_DONE, DJ_FAILED } DlState;

typedef struct {
	Episode ep;
	char podcast[128];
	DlState state;
	long long now, total;
	char err[128];
} DlJob;

void dl_init(void);
// Stops the worker. Returns false if it did not stop in time (it is then left running).
bool dl_exit(void);

bool dl_enqueue(const Episode* ep, const char* podcast);
// Cancel/remove the job for this episode (deletes the partial file).
void dl_cancel(const char* ep_id);
void dl_retry(const char* ep_id);

int dl_count(void);
bool dl_get(int i, DlJob* out);
// Index of the job for an episode, or -1.
int dl_find(const char* ep_id, DlJob* out);
// Pop one finished job (main thread registers it in the store). Returns false if none.
bool dl_take_finished(DlJob* out);
// True while something is downloading or waiting to.
bool dl_busy(void);
