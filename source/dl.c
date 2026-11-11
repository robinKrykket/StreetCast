#include "dl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "net.h"
#include "plat.h"
#include "store.h"
#include "util.h"

static DlJob jobs[DL_MAX_JOBS];
static int njobs;
static PlatMutex lock;
static PlatThread thread;
static bool thread_started;
static volatile bool quit;
static NetProgress prog;   // progress/cancel for the active transfer
static char active_id[17]; // episode being transferred ("" if none)

static int find_locked(const char* id)
{
	for (int i = 0; i < njobs; i++)
		if (!strcmp(jobs[i].ep.id, id)) return i;
	return -1;
}

static void remove_locked(int i)
{
	memmove(&jobs[i], &jobs[i + 1], (size_t)(njobs - i - 1) * sizeof(DlJob));
	njobs--;
}

static void dl_thread(void* arg)
{
	(void)arg;
	while (!quit) {
		Episode* ep = malloc(sizeof(Episode));
		bool have = false;
		plat_lock(&lock);
		for (int i = 0; i < njobs && ep; i++) {
			if (jobs[i].state == DJ_QUEUED) {
				jobs[i].state = DJ_ACTIVE;
				*ep = jobs[i].ep;
				str_copy(active_id, sizeof(active_id), ep->id);
				prog.cancel = false;
				prog.now = prog.total = 0;
				have = true;
				break;
			}
		}
		plat_unlock(&lock);
		if (!have) {
			free(ep);
			plat_sleep_ms(150);
			continue;
		}

		char part[128], final[128], err[128];
		path_episode_part(ep->id, part, sizeof(part));
		path_episode(ep->id, final, sizeof(final));
		int rc = net_download(ep->url, part, true, false, &prog, err, sizeof(err));
		if (rc == 0) {
			remove(final);
			if (rename(part, final) != 0) {
				rc = -1;
				snprintf(err, sizeof(err), "Couldn't save the file to the SD card.");
			}
		}

		plat_lock(&lock);
		int i = find_locked(ep->id);
		active_id[0] = 0;
		if (i >= 0 && jobs[i].state == DJ_ACTIVE) {
			if (rc == 0) {
				jobs[i].state = DJ_DONE;
				jobs[i].now = jobs[i].total = prog.now;
			} else {
				// Keep the partial file: a retry continues where this stopped.
				jobs[i].state = DJ_FAILED;
				str_copy(jobs[i].err, sizeof(jobs[i].err), err);
			}
		} else {
			// The job was cancelled while it ran: leave nothing behind.
			remove(rc == 0 ? final : part);
		}
		plat_unlock(&lock);
		free(ep);
	}
}

void dl_init(void)
{
	plat_mutex_init(&lock);
}

bool dl_exit(void)
{
	quit = true;
	prog.cancel = true;
	if (thread_started && !plat_thread_join_timeout(&thread, 4000)) return false;
	thread_started = false;
	return true;
}

bool dl_enqueue(const Episode* ep, const char* podcast)
{
	if (!thread_started) {
		// Lower priority than the UI and audio threads.
		if (!plat_thread_start(&thread, dl_thread, NULL, 96 * 1024, 2)) return false;
		thread_started = true;
	}
	bool ok = false;
	plat_lock(&lock);
	int i = find_locked(ep->id);
	if (i >= 0) {
		if (jobs[i].state == DJ_FAILED) jobs[i].state = DJ_QUEUED;
		ok = true;
	} else if (njobs < DL_MAX_JOBS) {
		DlJob* j = &jobs[njobs++];
		memset(j, 0, sizeof(*j));
		j->ep = *ep;
		str_copy(j->podcast, sizeof(j->podcast), podcast);
		j->state = DJ_QUEUED;
		j->total = ep->size;
		ok = true;
	}
	plat_unlock(&lock);
	return ok;
}

void dl_cancel(const char* id)
{
	plat_lock(&lock);
	int i = find_locked(id);
	if (i >= 0) {
		bool active = jobs[i].state == DJ_ACTIVE;
		remove_locked(i);
		if (active) {
			prog.cancel = true; // the thread deletes the partial file when the transfer stops
		} else {
			char part[128];
			path_episode_part(id, part, sizeof(part));
			remove(part);
		}
	}
	plat_unlock(&lock);
}

void dl_retry(const char* id)
{
	plat_lock(&lock);
	int i = find_locked(id);
	if (i >= 0 && jobs[i].state == DJ_FAILED) {
		jobs[i].state = DJ_QUEUED;
		jobs[i].err[0] = 0;
	}
	plat_unlock(&lock);
}

static void refresh_locked(int i)
{
	if (jobs[i].state == DJ_ACTIVE && !strcmp(active_id, jobs[i].ep.id)) {
		jobs[i].now = prog.now;
		if (prog.total > 0) jobs[i].total = prog.total;
	}
}

int dl_count(void)
{
	plat_lock(&lock);
	int n = njobs;
	plat_unlock(&lock);
	return n;
}

bool dl_get(int i, DlJob* out)
{
	bool ok = false;
	plat_lock(&lock);
	if (i >= 0 && i < njobs) {
		refresh_locked(i);
		*out = jobs[i];
		ok = true;
	}
	plat_unlock(&lock);
	return ok;
}

int dl_find(const char* id, DlJob* out)
{
	plat_lock(&lock);
	int i = find_locked(id);
	if (i >= 0 && out) {
		refresh_locked(i);
		*out = jobs[i];
	}
	plat_unlock(&lock);
	return i;
}

bool dl_take_finished(DlJob* out)
{
	bool ok = false;
	plat_lock(&lock);
	for (int i = 0; i < njobs; i++) {
		if (jobs[i].state == DJ_DONE) {
			*out = jobs[i];
			remove_locked(i);
			ok = true;
			break;
		}
	}
	plat_unlock(&lock);
	return ok;
}

bool dl_busy(void)
{
	bool busy = false;
	plat_lock(&lock);
	for (int i = 0; i < njobs; i++)
		if (jobs[i].state == DJ_QUEUED || jobs[i].state == DJ_ACTIVE) busy = true;
	plat_unlock(&lock);
	return busy;
}
