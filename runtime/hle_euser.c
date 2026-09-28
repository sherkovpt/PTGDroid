/* EUSER: heap, descriptors, active objects, timers, threads, mutexes, TLS, TInt64/TTime, traps. */
#include "hle.h"
#include <stdlib.h>
#include <time.h>

/* kernel.c internals used here */
typedef struct Thread Thread;
Thread *thread_create(uint32_t entry, uint32_t arg);
void thread_resume(Thread *t);
int thread_id(Thread *t);
uint32_t thread_handle(Thread *t);
Thread *thread_by_id(int id);
int thread_exited(Thread *t);
void kernel_wake_all(void);
void kernel_block(void);

/* ---------- descriptors ---------- */
uint32_t des_ptr(uint32_t d) {
    switch (des_type(d)) {
    case EBufC: return d + 4;
    case EPtrC: return rd32(d + 4);
    case EPtr: return rd32(d + 8);
    case EBuf: return d + 8;
    case EBufCPtr: return rd32(d + 8) + 4;
    }
    LOG("des_ptr: bad descriptor type %u at %08x", des_type(d), d);
    return 0;
}

void des_setlen(uint32_t d, uint32_t len) {
    wr32(d, (rd32(d) & 0xF0000000u) | len);
    if (des_type(d) == EBufCPtr) {
        uint32_t b = rd32(d + 8);
        wr32(b, (rd32(b) & 0xF0000000u) | len);
    }
}

char *des_to_cstr(uint32_t d, int wide, char *buf, size_t n) {
    uint32_t len = des_len(d), p = des_ptr(d);
    size_t i = 0;
    for (; i < len && i + 1 < n; i++) {
        uint32_t ch = wide ? rd16(p + 2 * i) : rd8(p + i);
        buf[i] = ch < 128 ? (char)ch : '?';
    }
    buf[i] = 0;
    return buf;
}

static void des_copy(uint32_t dst, uint32_t src_ptr, uint32_t len, int wide) {
    uint32_t max = des_max(dst);
    if (len > max) {
        LOG("euser: descriptor overflow (%u > %u), truncating", len, max);
        len = max;
    }
    memmove(MEM + des_ptr(dst), MEM + src_ptr, len << wide);
    des_setlen(dst, len);
}

static int32_t mem_compare(uint32_t l, uint32_t ll, uint32_t r, uint32_t rl, int wide) {
    uint32_t n = ll < rl ? ll : rl;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t a = wide ? rd16(l + 2 * i) : rd8(l + i), b = wide ? rd16(r + 2 * i) : rd8(r + i);
        if (a != b) return (int32_t)a - (int32_t)b;
    }
    return (int32_t)ll - (int32_t)rl;
}

static void TDes8_Copy_ptr(CPU *c) { des_copy(ARG(c, 0), ARG(c, 1), ARG(c, 2), 0); }
static void TDes8_Copy_des(CPU *c) { des_copy(ARG(c, 0), des_ptr(ARG(c, 1)), des_len(ARG(c, 1)), 0); }
static void TDes16_Copy_ptr(CPU *c) { des_copy(ARG(c, 0), ARG(c, 1), ARG(c, 2), 1); }
static void TDes16_Copy_des(CPU *c) { des_copy(ARG(c, 0), des_ptr(ARG(c, 1)), des_len(ARG(c, 1)), 1); }
static void TDesC8_Compare(CPU *c) {
    uint32_t a = ARG(c, 0), b = ARG(c, 1);
    RET(mem_compare(des_ptr(a), des_len(a), des_ptr(b), des_len(b), 0));
}
static void TDesC16_Compare(CPU *c) {
    uint32_t a = ARG(c, 0), b = ARG(c, 1);
    RET(mem_compare(des_ptr(a), des_len(a), des_ptr(b), des_len(b), 1));
}
static void Mem_Compare(CPU *c) { RET(mem_compare(ARG(c, 0), ARG(c, 1), ARG(c, 2), ARG(c, 3), 0)); }
static void TDesC8_AtC(CPU *c) { RET(des_ptr(ARG(c, 0)) + ARG(c, 1)); }
static void TDesC16_AtC(CPU *c) { RET(des_ptr(ARG(c, 0)) + 2 * ARG(c, 1)); }
static void TDesC8_Ptr(CPU *c) { RET(des_ptr(ARG(c, 0))); }
static void TDesC16_Ptr(CPU *c) { RET(des_ptr(ARG(c, 0))); }
static void TDes8_SetLength(CPU *c) { des_setlen(ARG(c, 0), ARG(c, 1)); }
static void TDes16_Zero(CPU *c) { des_setlen(ARG(c, 0), 0); }
static void TDes16_FillZ(CPU *c) {
    uint32_t d = ARG(c, 0), len = ARG(c, 1);
    memset(MEM + des_ptr(d), 0, 2 * len);
    des_setlen(d, len);
}
static void TDes8_Fill(CPU *c) {
    uint32_t d = ARG(c, 0);
    memset(MEM + des_ptr(d), (int)ARG(c, 1), des_len(d));
}
static void TDes16_AppendNumFixedWidth(CPU *c) {
    uint32_t d = ARG(c, 0), val = ARG(c, 1), radix = ARG(c, 2), width = ARG(c, 3);
    uint32_t len = des_len(d), p = des_ptr(d);
    for (uint32_t i = 0; i < width; i++) {
        uint32_t digit = val % radix;
        val /= radix;
        wr16(p + 2 * (len + width - 1 - i), digit < 10 ? '0' + digit : 'a' + digit - 10);
    }
    des_setlen(d, len + width);
}

/* constructors */
static void TPtr8_ctor2(CPU *c) { /* TPtr8(TUint8*, TInt aMaxLength) */
    uint32_t t = ARG(c, 0);
    wr32(t, EPtr << 28); wr32(t + 4, ARG(c, 2)); wr32(t + 8, ARG(c, 1));
}
static void TPtr8_ctor3(CPU *c) { /* TPtr8(TUint8*, TInt aLength, TInt aMaxLength) */
    uint32_t t = ARG(c, 0);
    wr32(t, (EPtr << 28) | ARG(c, 2)); wr32(t + 4, ARG(c, 3)); wr32(t + 8, ARG(c, 1));
}
static void TPtrC8_ctor_z(CPU *c) {
    uint32_t t = ARG(c, 0), s = ARG(c, 1), n = 0;
    while (rd8(s + n)) n++;
    wr32(t, (EPtrC << 28) | n); wr32(t + 4, s);
}
static void TPtrC16_ctor_z(CPU *c) {
    uint32_t t = ARG(c, 0), s = ARG(c, 1), n = 0;
    while (rd16(s + 2 * n)) n++;
    wr32(t, (EPtrC << 28) | n); wr32(t + 4, s);
}
static void TBufBase8_ctor_max(CPU *c) { uint32_t t = ARG(c, 0); wr32(t, EBuf << 28); wr32(t + 4, ARG(c, 1)); }
static void TBufBase8_ctor_len(CPU *c) {
    uint32_t t = ARG(c, 0);
    wr32(t, (EBuf << 28) | ARG(c, 1)); wr32(t + 4, ARG(c, 2));
}
static void TBufBase16_ctor_max(CPU *c) { uint32_t t = ARG(c, 0); wr32(t, EBuf << 28); wr32(t + 4, ARG(c, 1)); }
static void TBufBase16_ctor_des(CPU *c) {
    uint32_t t = ARG(c, 0), src = ARG(c, 1);
    wr32(t, EBuf << 28); wr32(t + 4, ARG(c, 2));
    des_copy(t, des_ptr(src), des_len(src), 1);
}

/* ---------- memory ---------- */
static void e_memset(CPU *c) { memset(MEM + ARG(c, 0), (int)ARG(c, 1), ARG(c, 2)); }
static void e_memcpy(CPU *c) { memmove(MEM + ARG(c, 0), MEM + ARG(c, 1), ARG(c, 2)); }
static void Mem_Copy(CPU *c) { memmove(MEM + ARG(c, 0), MEM + ARG(c, 1), ARG(c, 2)); RET(ARG(c, 0) + ARG(c, 2)); }
static void Mem_FillZ(CPU *c) { memset(MEM + ARG(c, 0), 0, ARG(c, 1)); }

static void alloc_or_leave(CPU *c, uint32_t p) {
    if (!p) trap_leave(c, KErrNoMemory);
    RET(p);
}
static void CBase_newL(CPU *c) { alloc_or_leave(c, gallocz(ARG(c, 0))); }
static void CBase_new(CPU *c) { RET(gallocz(ARG(c, 0))); }
static void builtin_new(CPU *c) { RET(galloc(ARG(c, 0))); }
static void builtin_delete(CPU *c) { gfree(ARG(c, 0)); }

static uint32_t g_heap_obj;
static uint32_t heap_obj(void) {
    if (!g_heap_obj) g_heap_obj = gallocz(64);
    return g_heap_obj;
}
static void RHeap_Alloc(CPU *c) { RET(galloc(ARG(c, 1))); }
static void RHeap_Free(CPU *c) { gfree(ARG(c, 1)); }
static void RHeap_AllocLen(CPU *c) { RET(galloc_len(ARG(c, 1))); }
static void RHeap_AllocSize(CPU *c) { wr32(ARG(c, 1), heap_used()); RET(1); }
static void RHeap_Size(CPU *c) { RET(heap_used()); }
static void RHeap_Compress(CPU *c) { RET(0); }
static void RHeap_nop(CPU *c) { (void)c; }
static void RHeap_Open(CPU *c) { RET(KErrNone); }
static void UserHeap_ChunkHeap(CPU *c) { RET(gallocz(64)); }
static void User_Heap(CPU *c) { RET(heap_obj()); }
static void User_nop(CPU *c) { (void)c; }

/* ---------- misc user ---------- */
static void User_Panic(CPU *c) {
    char cat[64];
    LOG("User::Panic(%s, %d)", des_to_cstr(ARG(c, 0), 1, cat, sizeof cat), (int32_t)ARG(c, 1));
    guest_fault(c, c->r[14], "panic");
}
static void User_Exit(CPU *c) { LOG("User::Exit(%d)", (int32_t)ARG(c, 0)); exit((int)ARG(c, 0)); }
static void User_Language(CPU *c) { RET(1); /* ELangEnglish */ }
static void User_LeaveIfError(CPU *c) { if ((int32_t)ARG(c, 0) < 0) trap_leave(c, (int32_t)ARG(c, 0)); }
static void User_LeaveNoMemory(CPU *c) { trap_leave(c, KErrNoMemory); }
static void UserHal_TickPeriod(CPU *c) { wr32(ARG(c, 0), 15625); RET(KErrNone); }
static void pure_virtual(CPU *c) { guest_fault(c, c->r[14], "pure virtual called"); }
static void CTrapCleanup_New(CPU *c) { RET(gallocz(16)); }
static void TVersion_ctor(CPU *c) { wr32(ARG(c, 0), 0); }

static void Dll_Tls(CPU *c) { RET(*thread_tls()); }
static void Dll_SetTls(CPU *c) { *thread_tls() = ARG(c, 1); RET(KErrNone); }
static void Dll_FreeTls(CPU *c) { *thread_tls() = 0; }

/* ---------- TInt64 / TTime ---------- */
static int64_t rd64(uint32_t a) { return (int64_t)((uint64_t)rd32(a) | ((uint64_t)rd32(a + 4) << 32)); }
static void wr64(uint32_t a, int64_t v) { wr32(a, (uint32_t)v); wr32(a + 4, (uint32_t)((uint64_t)v >> 32)); }
static void TInt64_ctor_int(CPU *c) { wr64(ARG(c, 0), (int32_t)ARG(c, 1)); }
static void TInt64_GetTInt(CPU *c) { RET(rd32(ARG(c, 0))); }
static void TInt64_div(CPU *c) {
    int64_t b = rd64(ARG(c, 2));
    wr64(ARG(c, 0), b ? rd64(ARG(c, 1)) / b : 0);
    RET(ARG(c, 0));
}
static void TInt64_sub(CPU *c) { wr64(ARG(c, 0), rd64(ARG(c, 1)) - rd64(ARG(c, 2))); RET(ARG(c, 0)); }

/* TTime = microseconds since 00:00 1 Jan 0001 (proleptic Gregorian), local time. */
static int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468; /* days since 1970-01-01 */
}
static void civil_from_days(int64_t z, int *y, unsigned *m, unsigned *d) {
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = (int)(yoe + era * 400 + (*m <= 2));
}
#define DAYS_0001_TO_1970 719162

/* Local wall-clock time sampled once, advanced with the monotonic clock so that HomeTime never
 * jumps (the game derives its 30 fps frame limiter from HomeTime differences). */
static void TTime_HomeTime(CPU *c) {
    static int64_t base_us;
    static uint64_t base_mono;
    if (!base_mono) {
        time_t now = time(NULL);
        struct tm lt;
#ifdef _WIN32
        localtime_s(&lt, &now);
#else
        localtime_r(&now, &lt);
#endif
        int64_t days = days_from_civil(lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday) + DAYS_0001_TO_1970;
        base_us = (days * 86400 + lt.tm_hour * 3600 + lt.tm_min * 60 + lt.tm_sec) * 1000000;
        base_mono = plat_now_us();
    }
    wr64(ARG(c, 0), base_us + (int64_t)(plat_now_us() - base_mono));
}

/* TDateTime TTime::DateTime() const: r0 = return slot, r1 = this */
static void TTime_DateTime(CPU *c) {
    int64_t us = rd64(ARG(c, 1));
    int64_t days = us / 86400000000LL, rem = us % 86400000000LL;
    int y; unsigned m, d;
    civil_from_days(days - DAYS_0001_TO_1970, &y, &m, &d);
    uint32_t r = ARG(c, 0);
    wr32(r, (uint32_t)y);
    wr32(r + 4, m - 1);            /* TMonth, 0-based */
    wr32(r + 8, d - 1);            /* day, 0-based */
    wr32(r + 12, (uint32_t)(rem / 3600000000LL));
    wr32(r + 16, (uint32_t)(rem / 60000000LL % 60));
    wr32(r + 20, (uint32_t)(rem / 1000000 % 60));
    wr32(r + 24, (uint32_t)(rem % 1000000));
    RET(r);
}

/* ---------- active objects ----------
 * CActive layout: vptr, iStatus@4, iActive@8, iLink{iNext@12, iPrev@16, iPriority@20}; size 24.
 * Virtual slots: 0 dtor, 1 DoCancel, 2 RunL, 3 RunError. */
#define AO_STATUS 4
#define AO_ACTIVE 8
#define AO_PRIORITY 20
#define MAX_AOS 256
#define MAX_LEVELS 16

typedef struct {
    uint32_t sched;              /* installed CActiveScheduler */
    uint32_t aos[MAX_AOS];       /* sorted by priority, highest first */
    int naos;
    int level;
    int stop[MAX_LEVELS];
} SchedState;
static SchedState sched_state[17];
static SchedState *ss(void) { return &sched_state[thread_id(cur_thread())]; }

static void ao_add(uint32_t ao) {
    SchedState *s = ss();
    if (s->naos == MAX_AOS) { LOG("euser: too many active objects"); return; }
    int32_t pr = (int32_t)rd32(ao + AO_PRIORITY);
    int i = s->naos;
    while (i > 0 && (int32_t)rd32(s->aos[i - 1] + AO_PRIORITY) < pr) { s->aos[i] = s->aos[i - 1]; i--; }
    s->aos[i] = ao;
    s->naos++;
    wr32(ao + 12, 1); /* mark iLink as queued (IsAdded) */
}

static void ao_remove(uint32_t ao) {
    for (int t = 0; t < 17; t++) {
        SchedState *s = &sched_state[t];
        for (int i = 0; i < s->naos; i++) {
            if (s->aos[i] == ao) {
                memmove(&s->aos[i], &s->aos[i + 1], (s->naos - i - 1) * 4);
                s->naos--;
                wr32(ao + 12, 0);
                return;
            }
        }
    }
}

static void CActive_ctor(CPU *c) {
    uint32_t t = ARG(c, 0);
    wr32(t + AO_STATUS, 0);
    wr32(t + AO_ACTIVE, 0);
    wr32(t + AO_PRIORITY, ARG(c, 1));
}
static void CActive_Cancel(CPU *c) {
    uint32_t t = ARG(c, 0);
    if (!rd32(t + AO_ACTIVE)) return;
    vcall(c, t, 1); /* DoCancel */
    wait_for_request(t + AO_STATUS);
    wr32(t + AO_ACTIVE, 0);
}
static void CActive_dtor(CPU *c) {
    uint32_t t = ARG(c, 0), flags = ARG(c, 1);
    if (rd32(t + AO_ACTIVE)) LOG("euser: deleting active CActive %08x", t);
    ao_remove(t);
    if (flags & 1) gfree(t);
}
static void CActive_SetActive(CPU *c) { wr32(ARG(c, 0) + AO_ACTIVE, 1); }
static void CActive_RunError(CPU *c) { RET(ARG(c, 1)); }
static void CActiveScheduler_Add(CPU *c) { ao_add(ARG(c, 0)); }

/* Default CActiveScheduler virtuals: 0 dtor, 1 WaitForAnyRequest, 2 Error */
static void sched_dtor(CPU *c) { if (ARG(c, 1) & 1) gfree(ARG(c, 0)); }
static void sched_wait(CPU *c) { (void)c; wait_any(); }
static void sched_error(CPU *c) { LOG("CActiveScheduler::Error(%d)", (int32_t)ARG(c, 1)); }
static uint32_t sched_vtable;

static void CActiveScheduler_ctor(CPU *c) {
    if (!sched_vtable) {
        GuestFn v[] = { sched_dtor, sched_wait, sched_error };
        sched_vtable = make_vtable(v, 3);
    }
    wr32(ARG(c, 0), sched_vtable);
}
static void CActiveScheduler_Install(CPU *c) { ss()->sched = ARG(c, 0); }

uint32_t scheduler_create_default(CPU *c) {
    uint32_t s = gallocz(32);
    c->r[0] = s;
    CActiveScheduler_ctor(c);
    sched_state[thread_id(cur_thread())].sched = s;
    return s;
}

static void run_ao(CPU *c, void *arg) { vcall(c, (uint32_t)(uintptr_t)arg, 2); }

static void run_one(CPU *c) {
    SchedState *s = ss();
    if (s->sched) vcall(c, s->sched, 1); else wait_any();
    for (int i = 0; i < s->naos; i++) {
        uint32_t ao = s->aos[i];
        if (rd32(ao + AO_ACTIVE) && rd32(ao + AO_STATUS) != KRequestPending) {
            wr32(ao + AO_ACTIVE, 0);
            int32_t err = host_trap(c, run_ao, (void *)(uintptr_t)ao);
            if (err) {
                c->r[1] = (uint32_t)err;
                vcall(c, ao, 3); /* RunError */
                if (c->r[0]) {
                    c->r[1] = c->r[0];
                    if (s->sched) vcall(c, s->sched, 2); else LOG("unhandled AO error %d", err);
                }
            }
            return;
        }
    }
    LOG("euser: stray signal (no ready active object)");
}

static void CActiveScheduler_Start(CPU *c) {
    SchedState *s = ss();
    int lvl = ++s->level;
    s->stop[lvl] = 0;
    uint32_t saved[16];
    memcpy(saved, c->r, sizeof saved);
    while (!ss()->stop[lvl]) run_one(c);
    memcpy(c->r, saved, sizeof saved);
    s->level--;
}
void CActiveScheduler_Start_entry(CPU *c) { CActiveScheduler_Start(c); }

static void CActiveScheduler_Stop(CPU *c) {
    (void)c;
    SchedState *s = ss();
    if (s->level > 0) s->stop[s->level] = 1;
}

/* ---------- calling guest code from the host ---------- */
uint32_t guest_call(CPU *c, uint32_t fn, int n, const uint32_t *args) {
    uint32_t saved[16];
    memcpy(saved, c->r, sizeof saved);
    int stack_words = n > 4 ? n - 4 : 0;
    c->r[13] -= (uint32_t)((stack_words * 4 + 7) & ~7);
    for (int i = 0; i < n; i++) {
        if (i < 4) c->r[i] = args[i];
        else wr32(c->r[13] + 4 * (i - 4), args[i]);
    }
    dispatch(c, fn);
    uint32_t r0 = c->r[0];
    memcpy(c->r, saved, sizeof saved);
    return r0;
}

uint32_t guest_vcall(CPU *c, int slot, int n, const uint32_t *args) {
    return guest_call(c, rd32(rd32(args[0]) + 8 + 4 * slot), n, args);
}

/* ---------- host callbacks delivered through an active object ----------
 * Each guest thread that receives callbacks owns a callback AO in its own scheduler, so callbacks
 * run on the thread that owns the target object, as Symbian delivers them. */
#define MAX_CALLBACKS 128
#define MAX_TID 17
typedef struct { HostCallback fn; uint32_t a[8]; } Callback;
typedef struct { Callback q[MAX_CALLBACKS]; int n; uint32_t ao; } CallbackQueue;
static CallbackQueue cbqs[MAX_TID];

static CallbackQueue *cbq_of_ao(uint32_t ao) {
    for (int t = 0; t < MAX_TID; t++) if (cbqs[t].ao == ao) return &cbqs[t];
    return NULL;
}
static void cb_runl(CPU *c) {
    CallbackQueue *cq = cbq_of_ao(ARG(c, 0));
    Callback q[MAX_CALLBACKS];
    int n = cq->n;
    memcpy(q, cq->q, sizeof(Callback) * (size_t)n);
    cq->n = 0;
    for (int i = 0; i < n; i++) q[i].fn(c, q[i].a);
}
static void cb_docancel(CPU *c) { (void)c; }

/* Creates the callback AO of the calling guest thread (idempotent). */
void callbacks_init(void) {
    int tid = thread_id(cur_thread());
    if (cbqs[tid].ao) return;
    GuestFn v[] = { sched_dtor, cb_docancel, cb_runl, CActive_RunError };
    cbqs[tid].ao = host_object("CallbackAO", 32, v, 4);
    ao_add(cbqs[tid].ao);
}

/* Caller holds the kernel lock (always true on guest threads). */
void post_callback_to(int tid, HostCallback fn, int n, const uint32_t *a) {
    CallbackQueue *cq = &cbqs[tid];
    if (!cq->ao) { LOG("euser: thread %d has no callback AO", tid); return; }
    if (cq->n == MAX_CALLBACKS) { LOG("euser: callback queue full"); return; }
    cq->q[cq->n].fn = fn;
    memset(cq->q[cq->n].a, 0, sizeof cq->q[cq->n].a);
    memcpy(cq->q[cq->n].a, a, sizeof(uint32_t) * (size_t)n);
    cq->n++;
    if (!rd32(cq->ao + AO_ACTIVE)) {
        wr32(cq->ao + AO_ACTIVE, 1);
        request_complete(thread_by_id(tid), cq->ao + AO_STATUS, KErrNone);
    }
}

void post_callback(HostCallback fn, int n, const uint32_t *a) { post_callback_to(1, fn, n, a); }

/* Host-owned active object with its own timer, added to the calling thread's scheduler.
 * Layout: CActive (24 bytes) + timer handle @24. */
static void host_ao_docancel(CPU *c) { timer_cancel(rd32(ARG(c, 0) + 24)); }
uint32_t host_ao_new(const char *name, GuestFn runl) {
    GuestFn v[] = { sched_dtor, host_ao_docancel, runl, CActive_RunError };
    uint32_t ao = host_object(name, 32, v, 4);
    wr32(ao + 24, handle_new(H_TIMER, NULL));
    ao_add(ao);
    return ao;
}
void host_ao_after(uint32_t ao, uint32_t us) {
    if (rd32(ao + AO_ACTIVE)) return;
    timer_after(rd32(ao + 24), ao + AO_STATUS, us);
    wr32(ao + AO_ACTIVE, 1);
}

/* ---------- timers ---------- */
/* RTimer is RHandleBase { TInt iHandle }. CTimer = CActive + RTimer iTimer@24. */
static void RTimer_CreateLocal(CPU *c) { wr32(ARG(c, 0), handle_new(H_TIMER, NULL)); RET(KErrNone); }
static void RTimer_After(CPU *c) { timer_after(rd32(ARG(c, 0)), ARG(c, 1), (int32_t)ARG(c, 2) < 0 ? 0 : ARG(c, 2)); }
static void RTimer_Cancel(CPU *c) { timer_cancel(rd32(ARG(c, 0))); }
static void RHandleBase_Close(CPU *c) { handle_close(rd32(ARG(c, 0))); wr32(ARG(c, 0), 0); }

static void CTimer_ctor(CPU *c) { CActive_ctor(c); }
static void CTimer_ConstructL(CPU *c) { wr32(ARG(c, 0) + 24, handle_new(H_TIMER, NULL)); }
static void CTimer_After(CPU *c) {
    uint32_t t = ARG(c, 0);
    timer_after(rd32(t + 24), t + AO_STATUS, (int32_t)ARG(c, 1) < 0 ? 0 : ARG(c, 1));
    wr32(t + AO_ACTIVE, 1);
}
static void CTimer_DoCancel(CPU *c) { timer_cancel(rd32(ARG(c, 0) + 24)); }
static void CTimer_dtor(CPU *c) {
    uint32_t t = ARG(c, 0), flags = ARG(c, 1);
    if (rd32(t + AO_ACTIVE)) { timer_cancel(rd32(t + 24)); wait_for_request(t + AO_STATUS); wr32(t + AO_ACTIVE, 0); }
    handle_close(rd32(t + 24));
    c->r[1] = 0;
    CActive_dtor(c);
    if (flags & 1) gfree(t);
}

static void User_After(CPU *c) { kernel_sleep_us(ARG(c, 0)); }
static void User_WaitForRequest(CPU *c) { wait_for_request(ARG(c, 0)); }
static void User_RequestComplete(CPU *c) {
    uint32_t pp = ARG(c, 0), st = rd32(pp);
    if (st) request_complete(cur_thread(), st, (int32_t)ARG(c, 1));
    wr32(pp, 0);
}

/* CPeriodic: CTimer + TCallBack{fn@28, ptr@32} + iInterval@36 */
static void periodic_dtor(CPU *c) { CTimer_dtor(c); }
static void periodic_docancel(CPU *c) { CTimer_DoCancel(c); }
static void periodic_runl(CPU *c) {
    uint32_t t = ARG(c, 0);
    timer_after(rd32(t + 24), t + AO_STATUS, rd32(t + 36));
    wr32(t + AO_ACTIVE, 1);
    c->r[0] = rd32(t + 32);
    dispatch(c, rd32(t + 28));
}
static uint32_t periodic_vtable;

static uint32_t periodic_new(CPU *c, int32_t prio) {
    if (!periodic_vtable) {
        GuestFn v[] = { periodic_dtor, periodic_docancel, periodic_runl, CActive_RunError };
        periodic_vtable = make_vtable(v, 4);
    }
    uint32_t t = gallocz(40);
    if (!t) return 0;
    wr32(t, periodic_vtable);
    wr32(t + AO_PRIORITY, (uint32_t)prio);
    wr32(t + 24, handle_new(H_TIMER, NULL));
    ao_add(t);
    (void)c;
    return t;
}
static void CPeriodic_New(CPU *c) { RET(periodic_new(c, (int32_t)ARG(c, 0))); }
static void CPeriodic_NewL(CPU *c) { alloc_or_leave(c, periodic_new(c, (int32_t)ARG(c, 0))); }
static void CPeriodic_Start(CPU *c) {
    uint32_t t = ARG(c, 0);
    wr32(t + 36, ARG(c, 2));
    wr32(t + 28, ARG(c, 3));
    wr32(t + 32, ARG(c, 4));
    timer_after(rd32(t + 24), t + AO_STATUS, ARG(c, 1));
    wr32(t + AO_ACTIVE, 1);
}

/* ---------- threads ---------- */
/* RThread::Create(const TDesC&, TThreadFunction, TInt aStack, RHeap*, TAny*, TOwnerType) */
static void RThread_Create(CPU *c) {
    char name[64];
    Thread *t = thread_create(ARG(c, 2), ARG(c, 5));
    LOG("RThread::Create(%s, fn=%08x)", des_to_cstr(ARG(c, 1), 1, name, sizeof name), ARG(c, 2));
    if (!t) { RET(KErrNoMemory); return; }
    wr32(ARG(c, 0), thread_handle(t));
    RET(KErrNone);
}
static Thread *thread_of(uint32_t rthread) {
    uint32_t h = rd32(rthread);
    return handle_type(h) == H_THREAD ? handle_obj(h) : cur_thread();
}
static void RThread_Resume(CPU *c) { thread_resume(thread_of(ARG(c, 0))); }
static void RThread_Id(CPU *c) { RET(thread_id(thread_of(ARG(c, 0)))); }
static void RThread_Open(CPU *c) {
    Thread *t = thread_by_id((int)ARG(c, 1));
    if (!t) { RET(KErrNotFound); return; }
    wr32(ARG(c, 0), thread_handle(t));
    RET(KErrNone);
}
static void RThread_ExitType(CPU *c) { RET(thread_exited(thread_of(ARG(c, 0))) ? 0 : 3); }
static void RThread_Kill(CPU *c) { LOG("RThread::Kill ignored"); (void)c; }
static void RThread_RequestComplete(CPU *c) {
    uint32_t pp = ARG(c, 1), st = rd32(pp);
    if (st) request_complete(thread_of(ARG(c, 0)), st, (int32_t)ARG(c, 2));
    wr32(pp, 0);
}
static void RThread_Heap(CPU *c) { RET(heap_obj()); }
static void ret_none(CPU *c) { RET(KErrNone); }

/* ---------- mutexes ---------- */
typedef struct { Thread *owner; int count; } Mutex;
static void RMutex_CreateLocal(CPU *c) {
    wr32(ARG(c, 0), handle_new(H_MUTEX, calloc(1, sizeof(Mutex))));
    RET(KErrNone);
}
static void RMutex_Wait(CPU *c) {
    Mutex *m = handle_obj(rd32(ARG(c, 0)));
    if (!m) return;
    while (m->owner && m->owner != cur_thread()) kernel_block();
    m->owner = cur_thread();
    m->count++;
}
static void RMutex_Signal(CPU *c) {
    Mutex *m = handle_obj(rd32(ARG(c, 0)));
    if (!m || m->owner != cur_thread()) return;
    if (--m->count == 0) { m->owner = NULL; kernel_wake_all(); }
}
static void RMutex_Count(CPU *c) {
    Mutex *m = handle_obj(rd32(ARG(c, 0)));
    RET(m && m->owner ? 1 - m->count : 1);
}

/* ---------- integer division (libgcc) ---------- */
static void divsi3(CPU *c) { int32_t b = (int32_t)ARG(c, 1); RET(b ? (int32_t)ARG(c, 0) / b : 0); }
static void modsi3(CPU *c) { int32_t b = (int32_t)ARG(c, 1); RET(b ? (int32_t)ARG(c, 0) % b : 0); }
static void udivsi3(CPU *c) { uint32_t b = ARG(c, 1); RET(b ? ARG(c, 0) / b : 0); }
static void umodsi3(CPU *c) { uint32_t b = ARG(c, 1); RET(b ? ARG(c, 0) % b : 0); }

const HleEntry HLE_EUSER[] = {
    {"euser", "memset", e_memset},
    {"euser", "memcpy", e_memcpy},
    {"euser", "newL__5CBaseUi", CBase_newL},
    {"euser", "__nw__5CBaseUi", CBase_new},
    {"euser", "__builtin_new", builtin_new},
    {"euser", "__builtin_vec_new", builtin_new},
    {"euser", "__builtin_delete", builtin_delete},
    {"euser", "__builtin_vec_delete", builtin_delete},
    {"euser", "Add__16CActiveSchedulerP7CActive", CActiveScheduler_Add},
    {"euser", "After__4UserG27TTimeIntervalMicroSeconds32", User_After},
    {"euser", "After__6CTimerG27TTimeIntervalMicroSeconds32", CTimer_After},
    {"euser", "After__6RTimerR14TRequestStatusG27TTimeIntervalMicroSeconds32", RTimer_After},
    {"euser", "AllocLen__C5RHeapPCv", RHeap_AllocLen},
    {"euser", "AllocSize__C5RHeapRi", RHeap_AllocSize},
    {"euser", "Alloc__5RHeapi", RHeap_Alloc},
    {"euser", "AppendNumFixedWidth__6TDes16Ui6TRadixi", TDes16_AppendNumFixedWidth},
    {"euser", "AtC__C6TDesC8i", TDesC8_AtC},
    {"euser", "AtC__C7TDesC16i", TDesC16_AtC},
    {"euser", "Cancel__6RTimer", RTimer_Cancel},
    {"euser", "Cancel__7CActive", CActive_Cancel},
    {"euser", "ChunkHeap__8UserHeapPC7TDesC16iii", UserHeap_ChunkHeap},
    {"euser", "Close__11RHandleBase", RHandleBase_Close},
    {"euser", "Close__5RHeap", RHeap_nop},
    {"euser", "Compare__3MemPCUciT1i", Mem_Compare},
    {"euser", "Compare__C6TDesC8RC6TDesC8", TDesC8_Compare},
    {"euser", "Compare__C7TDesC16RC7TDesC16", TDesC16_Compare},
    {"euser", "CompressAllHeaps__4User", User_nop},
    {"euser", "Compress__5RHeap", RHeap_Compress},
    {"euser", "ConstructL__6CTimer", CTimer_ConstructL},
    {"euser", "Copy__3MemPvPCvi", Mem_Copy},
    {"euser", "Copy__5TDes8PCUci", TDes8_Copy_ptr},
    {"euser", "Copy__5TDes8RC6TDesC8", TDes8_Copy_des},
    {"euser", "Copy__6TDes16PCUsi", TDes16_Copy_ptr},
    {"euser", "Copy__6TDes16RC7TDesC16", TDes16_Copy_des},
    {"euser", "Count__6RMutex", RMutex_Count},
    {"euser", "CreateLocal__6RMutex10TOwnerType", RMutex_CreateLocal},
    {"euser", "CreateLocal__6RTimer", RTimer_CreateLocal},
    {"euser", "Create__7RThreadRC7TDesC16PFPv_iiP5RHeapPv10TOwnerType", RThread_Create},
    {"euser", "DateTime__C5TTime", TTime_DateTime},
    {"euser", "DllFreeTls__7UserSvri", Dll_FreeTls},
    {"euser", "DllSetTls__7UserSvriPv", Dll_SetTls},
    {"euser", "DllTls__7UserSvri", Dll_Tls},
    {"euser", "ExitType__C7RThread", RThread_ExitType},
    {"euser", "Exit__4Useri", User_Exit},
    {"euser", "FillZ__3MemPvi", Mem_FillZ},
    {"euser", "FillZ__6TDes16i", TDes16_FillZ},
    {"euser", "Fill__5TDes8G5TChari", TDes8_Fill},
    {"euser", "FreeAll__5RHeap", RHeap_nop},
    {"euser", "Free__5RHeapPv", RHeap_Free},
    {"euser", "GetTInt__C6TInt64", TInt64_GetTInt},
    {"euser", "Heap__4User", User_Heap},
    {"euser", "Heap__7RThread", RThread_Heap},
    {"euser", "HomeTime__5TTime", TTime_HomeTime},
    {"euser", "Id__C7RThread", RThread_Id},
    {"euser", "Install__16CActiveSchedulerP16CActiveScheduler", CActiveScheduler_Install},
    {"euser", "Kill__7RThreadi", RThread_Kill},
    {"euser", "Language__4User", User_Language},
    {"euser", "LeaveIfError__4Useri", User_LeaveIfError},
    {"euser", "LeaveNoMemory__4User", User_LeaveNoMemory},
    {"euser", "NewL__9CPeriodici", CPeriodic_NewL},
    {"euser", "New__12CTrapCleanup", CTrapCleanup_New},
    {"euser", "New__9CPeriodici", CPeriodic_New},
    {"euser", "Open__5RHeap", RHeap_Open},
    {"euser", "Open__7RThreadG9TThreadId10TOwnerType", RThread_Open},
    {"euser", "Panic__4UserRC7TDesC16i", User_Panic},
    {"euser", "Ptr__C6TDesC8", TDesC8_Ptr},
    {"euser", "Ptr__C7TDesC16", TDesC16_Ptr},
    {"euser", "Rename__C7RThreadRC7TDesC16", ret_none},
    {"euser", "RequestComplete__4UserRP14TRequestStatusi", User_RequestComplete},
    {"euser", "RequestComplete__C7RThreadRP14TRequestStatusi", RThread_RequestComplete},
    {"euser", "ResetInactivityTime__4User", User_nop},
    {"euser", "Resume__C7RThread", RThread_Resume},
    {"euser", "RunError__7CActivei", CActive_RunError},
    {"euser", "SetActive__7CActive", CActive_SetActive},
    {"euser", "SetExceptionHandler__7RThreadPF8TExcType_vUl", ret_none},
    {"euser", "SetLength__5TDes8i", TDes8_SetLength},
    {"euser", "SetPriority__C7RThread15TThreadPriority", User_nop},
    {"euser", "Signal__6RMutex", RMutex_Signal},
    {"euser", "Size__C5RHeap", RHeap_Size},
    {"euser", "Start__16CActiveScheduler", CActiveScheduler_Start},
    {"euser", "Start__9CPeriodicG27TTimeIntervalMicroSeconds32T1G9TCallBack", CPeriodic_Start},
    {"euser", "Stop__16CActiveScheduler", CActiveScheduler_Stop},
    {"euser", "TickPeriod__7UserHalR27TTimeIntervalMicroSeconds32", UserHal_TickPeriod},
    {"euser", "Trap__5TTrapRi", trap_hle_trap},
    {"euser", "UnTrap__5TTrap", trap_hle_untrap},
    {"euser", "WaitForRequest__4UserR14TRequestStatus", User_WaitForRequest},
    {"euser", "Wait__6RMutex", RMutex_Wait},
    {"euser", "Zero__6TDes16", TDes16_Zero},
    {"euser", "_._6CTimer", CTimer_dtor},
    {"euser", "_._7CActive", CActive_dtor},
    {"euser", "__10TBufBase16RC7TDesC16i", TBufBase16_ctor_des},
    {"euser", "__10TBufBase16i", TBufBase16_ctor_max},
    {"euser", "__16CActiveScheduler", CActiveScheduler_ctor},
    {"euser", "__5TPtr8PUci", TPtr8_ctor2},
    {"euser", "__5TPtr8PUcii", TPtr8_ctor3},
    {"euser", "__6CTimeri", CTimer_ctor},
    {"euser", "__6TInt64i", TInt64_ctor_int},
    {"euser", "__6TPtrC8PCUc", TPtrC8_ctor_z},
    {"euser", "__7CActivei", CActive_ctor},
    {"euser", "__7TPtrC16PCUs", TPtrC16_ctor_z},
    {"euser", "__8TVersion", TVersion_ctor},
    {"euser", "__9TBufBase8i", TBufBase8_ctor_max},
    {"euser", "__9TBufBase8ii", TBufBase8_ctor_len},
    {"euser", "__dv__C6TInt64RC6TInt64", TInt64_div},
    {"euser", "__mi__C6TInt64RC6TInt64", TInt64_sub},
    {"euser", "__pure_virtual", pure_virtual},
    {"euser", "__divsi3", divsi3},
    {"euser", "__modsi3", modsi3},
    {"euser", "__udivsi3", udivsi3},
    {"euser", "__umodsi3", umodsi3},
    {0, 0, 0},
};
