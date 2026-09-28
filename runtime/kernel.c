/* Threads, request semaphores, timers, handles, traps.
 * One global lock (GIL) is held while guest code runs; threads release it only when they block
 * (WaitForAnyRequest, User::After, mutex waits). */
#include "hle.h"
#include <stdlib.h>

#ifdef _MSC_VER
#define THREAD_LOCAL __declspec(thread)
#else
#define THREAD_LOCAL _Thread_local
#endif

#define MAX_TRAPS 64
#define MAX_THREADS 16
#define MAX_TIMERS 256
#define MAX_HANDLES 1024

struct Thread {
    int id;
    CPU *cpu;
    uint32_t tls;
    TrapFrame traps[MAX_TRAPS];
    int trap_depth;
    TrapFrame resumed;
    int req;
    PlatCond *cv;
    uint32_t entry, arg, handle;
    int started, exited;
    Thread *resumer; /* waiting in RThread::Resume until this thread first blocks */
};

static PlatMutex *gil;
static Thread threads[MAX_THREADS];
static int nthreads;
static THREAD_LOCAL Thread *tcur;

Thread *cur_thread(void) { return tcur; }
int kernel_thread_id(void) { return tcur ? tcur->id : 0; }
CPU *cur_cpu(void) { return tcur->cpu; }
uint32_t *thread_tls(void) { return &tcur->tls; }

/* ---- timers ---- */
typedef struct { uint32_t owner, status; uint64_t deadline; Thread *t; int active; } Timer;
static Timer timers[MAX_TIMERS];

static void timers_poll(void) {
    uint64_t now = plat_now_us();
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (timers[i].active && timers[i].deadline <= now) {
            timers[i].active = 0;
            request_complete(timers[i].t, timers[i].status, KErrNone);
        }
    }
}

static uint64_t next_deadline(void) {
    uint64_t d = 0;
    for (int i = 0; i < MAX_TIMERS; i++)
        if (timers[i].active && (!d || timers[i].deadline < d)) d = timers[i].deadline;
    return d;
}

/* EKA1 timers only expire on the system tick (1/64 s on the N-Gage): a request completes at the
 * first tick boundary at or after the requested time. Games pace themselves on this. */
#define TICK_US 15625u
static uint64_t tick_epoch;
static uint64_t tick_deadline(uint64_t us) {
    uint64_t now = plat_now_us(), t = now + us - tick_epoch;
    return tick_epoch + (t + TICK_US - 1) / TICK_US * TICK_US;
}

void timer_after(uint32_t owner, uint32_t status, uint64_t us) {
    wr32(status, KRequestPending);
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (!timers[i].active) {
            timers[i] = (Timer){ owner, status, tick_deadline(us), tcur, 1 };
            return;
        }
    }
    LOG("kernel: out of timers");
    request_complete(tcur, status, KErrNoMemory);
}

void timer_cancel(uint32_t owner) {
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (timers[i].active && timers[i].owner == owner) {
            timers[i].active = 0;
            request_complete(timers[i].t, timers[i].status, KErrCancel);
        }
    }
}

/* ---- blocking ---- */
static void release_resumer(Thread *t) {
    if (t->resumer) {
        plat_cond_signal(t->resumer->cv);
        t->resumer = NULL;
    }
}

static void kwait(uint64_t until) {
    release_resumer(tcur);
    uint64_t d = next_deadline();
    if (until && (!d || until < d)) d = until;
    plat_cond_wait(tcur->cv, gil, d);
    timers_poll();
}

void request_complete(Thread *t, uint32_t status, int32_t value) {
    wr32(status, (uint32_t)value);
    t->req++;
    plat_cond_signal(t->cv);
}

void wait_any(void) {
    timers_poll();
    while (tcur->req == 0) kwait(0);
    tcur->req--;
}

void wait_for_request(uint32_t status) {
    int extra = 0;
    for (;;) {
        wait_any();
        if (rd32(status) != KRequestPending) break;
        extra++;
    }
    tcur->req += extra;
}

void kernel_sleep_us(uint64_t us) {
    uint64_t until = tick_deadline(us);
    while (plat_now_us() < until) kwait(until);
}

/* Wakes every blocked thread; used by mutexes and host event injection. */
void kernel_wake_all(void) {
    for (int i = 0; i < nthreads; i++) plat_cond_signal(threads[i].cv);
}
void kernel_block(void) { kwait(0); }
/* Host threads (UI, input) may deliver events before the game thread has initialised the kernel. */
int kernel_ready(void) { return gil != NULL; }
void kernel_lock(void) { plat_mutex_lock(gil); }
void kernel_unlock(void) { plat_mutex_unlock(gil); }

/* ---- handles ---- */
typedef struct { int type; void *obj; } Handle;
static Handle handles[MAX_HANDLES];
#define HANDLE_BASE 0x100u

uint32_t handle_new(int type, void *obj) {
    for (uint32_t i = 0; i < MAX_HANDLES; i++) {
        if (handles[i].type == H_FREE) {
            handles[i].type = type;
            handles[i].obj = obj;
            return HANDLE_BASE + i;
        }
    }
    LOG("kernel: out of handles");
    return 0;
}
static Handle *hget(uint32_t h) {
    return h >= HANDLE_BASE && h < HANDLE_BASE + MAX_HANDLES ? &handles[h - HANDLE_BASE] : NULL;
}
int handle_type(uint32_t h) { Handle *x = hget(h); return x ? x->type : H_FREE; }
void *handle_obj(uint32_t h) { Handle *x = hget(h); return x ? x->obj : NULL; }
void handle_close(uint32_t h) {
    Handle *x = hget(h);
    if (!x) return;
    if (x->type == H_TIMER) timer_cancel(h);
    if (x->type == H_MUTEX) free(x->obj);
    x->type = H_FREE;
    x->obj = NULL;
}

/* ---- host functions ---- */
static GuestFn hostfns[HOSTFN_MAX];
static uint32_t nhostfns;

uint32_t hostfn(GuestFn fn) {
    for (uint32_t i = 0; i < nhostfns; i++)
        if (hostfns[i] == fn) return HOSTFN_BASE + 4 * i;
    if (nhostfns == HOSTFN_MAX) { LOG("kernel: out of host function slots"); abort(); }
    hostfns[nhostfns] = fn;
    return HOSTFN_BASE + 4 * nhostfns++;
}

GuestFn hostfn_lookup(uint32_t addr) {
    uint32_t i = (addr - HOSTFN_BASE) / 4;
    return addr >= HOSTFN_BASE && i < nhostfns ? hostfns[i] : NULL;
}

uint32_t make_vtable(const GuestFn *slots, int n) {
    uint32_t p = gallocz(8 + 4 * n);
    for (int i = 0; i < n; i++) wr32(p + 8 + 4 * i, slots[i] ? hostfn(slots[i]) : 0);
    return p; /* vptr points at the two leading zero words */
}

/* ---- traps ---- */
jmp_buf *trap_enter(CPU *c) {
    if (tcur->trap_depth == MAX_TRAPS) guest_fault(c, c->r[14], "trap stack overflow");
    TrapFrame *t = &tcur->traps[tcur->trap_depth];
    memcpy(t->saved, c->r, sizeof t->saved);
    t->result_ptr = c->r[1];
    t->host = 0;
    return &t->jb;
}

void trap_hle_trap(CPU *c) {
    tcur->trap_depth++;
    wr32(c->r[1], 0);
    c->r[0] = 0;
}

void trap_hle_untrap(CPU *c) {
    (void)c;
    if (tcur->trap_depth > 0) tcur->trap_depth--;
}

void trap_leave(CPU *c, int32_t code) {
    if (tcur->trap_depth == 0) {
        LOG("Leave %d with no trap harness", code);
        guest_fault(c, c->r[14], "unhandled leave");
    }
    TrapFrame *t = &tcur->traps[--tcur->trap_depth];
    t->code = code;
    tcur->resumed = *t;
    if (!t->host) wr32(t->result_ptr, (uint32_t)code);
    longjmp(t->jb, 1);
}

void trap_resume(CPU *c) {
    for (int i = 4; i <= 11; i++) c->r[i] = tcur->resumed.saved[i];
    c->r[13] = tcur->resumed.saved[13];
    c->r[14] = tcur->resumed.saved[14]; /* return address of the TTrap::Trap call (interpreter) */
    c->r[0] = 1;
}

int32_t host_trap(CPU *c, void (*fn)(CPU *c, void *arg), void *arg) {
    if (tcur->trap_depth == MAX_TRAPS) guest_fault(c, 0, "trap stack overflow");
    TrapFrame *t = &tcur->traps[tcur->trap_depth++];
    memcpy(t->saved, c->r, sizeof t->saved);
    t->host = 1;
    t->code = 0;
    int depth = tcur->trap_depth;
    if (setjmp(t->jb) == 0) {
        fn(c, arg);
        if (tcur->trap_depth != depth) LOG("kernel: unbalanced trap depth after host_trap");
        tcur->trap_depth = depth - 1;
        return 0;
    }
    for (int i = 4; i <= 11; i++) c->r[i] = tcur->resumed.saved[i];
    c->r[13] = tcur->resumed.saved[13];
    return tcur->resumed.code;
}

/* ---- threads ---- */
static Thread *thread_alloc(CPU *cpu) {
    if (nthreads == MAX_THREADS) return NULL;
    Thread *t = &threads[nthreads];
    memset(t, 0, sizeof *t);
    t->id = nthreads + 1;
    t->cv = plat_cond_new();
    t->cpu = cpu;
    uint32_t stack = STACK_AREA + (uint32_t)nthreads * 0x100000u;
    mem_commit(stack, STACK_SIZE_GUEST);
    cpu->r[13] = stack + STACK_SIZE_GUEST - 16;
    nthreads++;
    return t;
}

void kernel_init(CPU *main_cpu) {
    PlatMutex *m = plat_mutex_new();
    tick_epoch = plat_now_us();
    plat_mutex_lock(m);
    gil = m; /* published only once held, so early host events block until init completes */
    tcur = thread_alloc(main_cpu);
    tcur->started = 1;
    tcur->handle = handle_new(H_THREAD, tcur);
}

Thread *thread_create(uint32_t entry, uint32_t arg) {
    CPU *cpu = calloc(1, sizeof *cpu);
    Thread *t = thread_alloc(cpu);
    if (!t) { free(cpu); return NULL; }
    t->entry = entry;
    t->arg = arg;
    t->handle = handle_new(H_THREAD, t);
    return t;
}

static void thread_main(void *p) {
    Thread *t = p;
    plat_mutex_lock(gil);
    tcur = t;
    t->cpu->r[0] = t->arg;
    t->cpu->r[14] = 0;
    LOG("thread %d started at %08x", t->id, t->entry);
    dispatch(t->cpu, t->entry);
    LOG("thread %d exited with %d", t->id, (int32_t)t->cpu->r[0]);
    t->exited = 1;
    release_resumer(t);
    plat_mutex_unlock(gil);
}

/* A resumed thread runs until it first blocks before the resumer continues: on the device the
 * resumer soon blocks in synchronous server calls (file I/O), so new threads initialise early and
 * the game relies on that ordering. */
void thread_resume(Thread *t) {
    if (t->started) return;
    t->started = 1;
    t->resumer = tcur;
    plat_thread_start(thread_main, t);
    while (t->resumer && !t->exited) plat_cond_wait(tcur->cv, gil, 0);
}

int thread_id(Thread *t) { return t->id; }
uint32_t thread_handle(Thread *t) { return t->handle; }
Thread *thread_by_id(int id) { return id >= 1 && id <= nthreads ? &threads[id - 1] : NULL; }
int thread_exited(Thread *t) { return t->exited; }
