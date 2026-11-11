#include "plat.h"

#ifdef __3DS__

void plat_mutex_init(PlatMutex* m) { LightLock_Init(m); }
void plat_lock(PlatMutex* m) { LightLock_Lock(m); }
void plat_unlock(PlatMutex* m) { LightLock_Unlock(m); }

static s32 main_prio = 0x30;
static bool have_prio;

bool plat_thread_start(PlatThread* t, PlatThreadFn fn, void* arg, size_t stack, int prio_delta)
{
	if (!have_prio) {
		// Must first be called from the main thread (it is: during init).
		svcGetThreadPriority(&main_prio, CUR_THREAD_HANDLE);
		have_prio = true;
	}
	s32 prio = main_prio + prio_delta;
	if (prio < 0x18) prio = 0x18;
	if (prio > 0x3F) prio = 0x3F;
	*t = threadCreate(fn, arg, stack, prio, -2, false);
	return *t != NULL;
}

void plat_thread_join(PlatThread* t)
{
	if (!*t) return;
	threadJoin(*t, U64_MAX);
	threadFree(*t);
	*t = NULL;
}

bool plat_thread_join_timeout(PlatThread* t, int ms)
{
	if (!*t) return true;
	// A timed-out wait returns a non-zero (but not "failed") result code.
	if (threadJoin(*t, (u64)ms * 1000000ULL) != 0) return false;
	threadFree(*t);
	*t = NULL;
	return true;
}

void plat_sleep_ms(int ms) { svcSleepThread((s64)ms * 1000000LL); }
uint64_t plat_time_ms(void) { return osGetTime(); }

#else

#include <stdlib.h>
#include <time.h>
#include <unistd.h>

void plat_mutex_init(PlatMutex* m) { pthread_mutex_init(m, NULL); }
void plat_lock(PlatMutex* m) { pthread_mutex_lock(m); }
void plat_unlock(PlatMutex* m) { pthread_mutex_unlock(m); }

struct tramp { PlatThreadFn fn; void* arg; };
static void* tramp_fn(void* p)
{
	struct tramp t = *(struct tramp*)p;
	free(p);
	t.fn(t.arg);
	return NULL;
}

bool plat_thread_start(PlatThread* t, PlatThreadFn fn, void* arg, size_t stack, int prio_delta)
{
	(void)stack; (void)prio_delta;
	struct tramp* tr = malloc(sizeof(*tr));
	tr->fn = fn; tr->arg = arg;
	return pthread_create(t, NULL, tramp_fn, tr) == 0;
}

void plat_thread_join(PlatThread* t) { pthread_join(*t, NULL); }
bool plat_thread_join_timeout(PlatThread* t, int ms) { (void)ms; pthread_join(*t, NULL); return true; }
void plat_sleep_ms(int ms) { usleep((useconds_t)ms * 1000); }
uint64_t plat_time_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

#endif
