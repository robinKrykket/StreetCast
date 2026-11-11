// Thin threading/time shim so the network + decode code also builds on a PC for testing.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __3DS__
#include <3ds.h>
typedef LightLock PlatMutex;
typedef Thread PlatThread;
#define APP_DIR "sdmc:/3ds/StreetCast"
#define CA_BUNDLE "romfs:/cacert.pem"
#else
#include <pthread.h>
typedef pthread_mutex_t PlatMutex;
typedef pthread_t PlatThread;
#ifndef APP_DIR
#define APP_DIR "./sc_data"
#endif
#endif

typedef void (*PlatThreadFn)(void* arg);

void plat_mutex_init(PlatMutex* m);
void plat_lock(PlatMutex* m);
void plat_unlock(PlatMutex* m);
// prio_delta: negative = more urgent than the main thread, positive = less.
bool plat_thread_start(PlatThread* t, PlatThreadFn fn, void* arg, size_t stack, int prio_delta);
void plat_thread_join(PlatThread* t);
// Wait at most ms for the thread to finish. Returns false (thread left running) on timeout.
bool plat_thread_join_timeout(PlatThread* t, int ms);
void plat_sleep_ms(int ms);
uint64_t plat_time_ms(void);
