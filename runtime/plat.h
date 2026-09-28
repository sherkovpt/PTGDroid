#pragma once
#include <stdint.h>

/* Minimal host platform layer: threads, locks, time. Win32 and POSIX implementations. */
typedef struct PlatMutex PlatMutex;
typedef struct PlatCond PlatCond;
typedef void (*PlatThreadFn)(void *arg);

PlatMutex *plat_mutex_new(void);
void plat_mutex_lock(PlatMutex *m);
void plat_mutex_unlock(PlatMutex *m);
PlatCond *plat_cond_new(void);
void plat_cond_signal(PlatCond *c);
/* Waits until signalled or until the absolute deadline (plat_now_us() clock); 0 = no deadline. */
void plat_cond_wait(PlatCond *c, PlatMutex *m, uint64_t deadline_us);
int plat_thread_start(PlatThreadFn fn, void *arg);
uint64_t plat_now_us(void);
