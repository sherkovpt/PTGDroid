#include "plat.h"
#include <stdlib.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

struct PlatMutex { SRWLOCK l; };
struct PlatCond { CONDITION_VARIABLE cv; };

PlatMutex *plat_mutex_new(void) { PlatMutex *m = calloc(1, sizeof *m); InitializeSRWLock(&m->l); return m; }
void plat_mutex_lock(PlatMutex *m) { AcquireSRWLockExclusive(&m->l); }
void plat_mutex_unlock(PlatMutex *m) { ReleaseSRWLockExclusive(&m->l); }
PlatCond *plat_cond_new(void) { PlatCond *c = calloc(1, sizeof *c); InitializeConditionVariable(&c->cv); return c; }
void plat_cond_signal(PlatCond *c) { WakeAllConditionVariable(&c->cv); }

void plat_cond_wait(PlatCond *c, PlatMutex *m, uint64_t deadline_us) {
    DWORD ms = INFINITE;
    if (deadline_us) {
        uint64_t now = plat_now_us();
        ms = deadline_us > now ? (DWORD)((deadline_us - now + 999) / 1000) : 0;
    }
    SleepConditionVariableSRW(&c->cv, &m->l, ms, 0);
}

typedef struct { PlatThreadFn fn; void *arg; } Start;
static DWORD WINAPI trampoline(LPVOID p) {
    Start s = *(Start *)p;
    free(p);
    s.fn(s.arg);
    return 0;
}
int plat_thread_start(PlatThreadFn fn, void *arg) {
    Start *s = malloc(sizeof *s);
    s->fn = fn;
    s->arg = arg;
    HANDLE h = CreateThread(NULL, 8 << 20, trampoline, s, 0, NULL);
    if (!h) { free(s); return -1; }
    CloseHandle(h);
    return 0;
}

uint64_t plat_now_us(void) {
    static LARGE_INTEGER freq;
    LARGE_INTEGER t;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t);
    return (uint64_t)(t.QuadPart / freq.QuadPart) * 1000000ull +
           (uint64_t)(t.QuadPart % freq.QuadPart) * 1000000ull / (uint64_t)freq.QuadPart;
}

#else
#include <errno.h>
#include <pthread.h>
#include <time.h>

struct PlatMutex { pthread_mutex_t m; };
struct PlatCond { pthread_cond_t c; };

PlatMutex *plat_mutex_new(void) { PlatMutex *m = calloc(1, sizeof *m); pthread_mutex_init(&m->m, NULL); return m; }
void plat_mutex_lock(PlatMutex *m) { pthread_mutex_lock(&m->m); }
void plat_mutex_unlock(PlatMutex *m) { pthread_mutex_unlock(&m->m); }

PlatCond *plat_cond_new(void) {
    PlatCond *c = calloc(1, sizeof *c);
    pthread_condattr_t a;
    pthread_condattr_init(&a);
    pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
    pthread_cond_init(&c->c, &a);
    return c;
}
void plat_cond_signal(PlatCond *c) { pthread_cond_broadcast(&c->c); }

void plat_cond_wait(PlatCond *c, PlatMutex *m, uint64_t deadline_us) {
    if (!deadline_us) { pthread_cond_wait(&c->c, &m->m); return; }
    struct timespec ts = { (time_t)(deadline_us / 1000000), (long)(deadline_us % 1000000) * 1000 };
    pthread_cond_timedwait(&c->c, &m->m, &ts);
}

typedef struct { PlatThreadFn fn; void *arg; } Start;
static void *trampoline(void *p) {
    Start s = *(Start *)p;
    free(p);
    s.fn(s.arg);
    return NULL;
}
int plat_thread_start(PlatThreadFn fn, void *arg) {
    Start *s = malloc(sizeof *s);
    s->fn = fn;
    s->arg = arg;
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 8 << 20);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &a, trampoline, s)) { free(s); return -1; }
    return 0;
}

uint64_t plat_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000;
}
#endif
