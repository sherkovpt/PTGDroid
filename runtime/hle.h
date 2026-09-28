#pragma once
#include "runtime.h"
#include "plat.h"

/* ---- Symbian constants ---- */
#define KRequestPending 0x80000001u
#define KErrNone 0
#define KErrNotFound (-1)
#define KErrGeneral (-2)
#define KErrCancel (-3)
#define KErrNoMemory (-4)
#define KErrNotSupported (-5)
#define KErrArgument (-6)
#define KErrAlreadyExists (-11)
#define KErrPathNotFound (-12)
#define KErrInUse (-14)
#define KErrNotReady (-18)
#define KErrAccessDenied (-21)
#define KErrEof (-25)
#define KErrBadName (-28)
#define KErrDisconnected (-36)
#define KErrOverflow (-9)

/* ---- guest memory map ---- */
#define HOSTFN_BASE 0x0E000000u   /* addresses dispatching to host C functions */
#define HOSTFN_MAX 4096u
#define HEAP_BASE 0x40000000u
#define HEAP_LIMIT 0x60000000u
#define DISPLAY_BASE 0x60000000u
#define STACK_AREA 0x70000000u
#define STACK_SIZE_GUEST 0x40000u

/* ---- argument access (APCS: r0-r3 then stack) ---- */
static FORCEINLINE uint32_t ARG(CPU *c, int i) { return i < 4 ? c->r[i] : rd32(c->r[13] + 4 * (i - 4)); }
#define RET(v) (c->r[0] = (uint32_t)(v))

/* ---- guest heap ---- */
void heap_init(void);
uint32_t galloc(uint32_t size);   /* 0 on failure */
uint32_t gallocz(uint32_t size);
void gfree(uint32_t p);
uint32_t grealloc(uint32_t p, uint32_t size);
uint32_t galloc_len(uint32_t p);
uint32_t heap_used(void);

/* ---- host functions callable from guest (synthetic vtables, callbacks) ---- */
uint32_t hostfn(GuestFn fn);               /* stable guest address for a host function */
GuestFn hostfn_lookup(uint32_t addr);
uint32_t make_vtable(const GuestFn *slots, int n); /* returns vptr value (points 8 bytes in) */
/* Host-owned object of `size` bytes whose vtable uses `impl` (NULL entries / slots >= n report
 * "unimplemented virtual <name> slot k" when called). */
uint32_t host_object(const char *name, uint32_t size, const GuestFn *impl, int n);

/* Call guest virtual `slot` of object `obj` (args r1..r3 must already be set). */
static FORCEINLINE void vcall(CPU *c, uint32_t obj, int slot) {
    uint32_t fn = rd32(rd32(obj) + 8 + 4 * slot);
    c->r[0] = obj;
    dispatch(c, fn);
}

/* ---- descriptors ---- */
enum { EBufC = 0, EPtrC = 1, EPtr = 2, EBuf = 3, EBufCPtr = 4 };
static FORCEINLINE uint32_t des_type(uint32_t d) { return rd32(d) >> 28; }
static FORCEINLINE uint32_t des_len(uint32_t d) { return rd32(d) & 0x0FFFFFFFu; }
static FORCEINLINE uint32_t des_max(uint32_t d) { return rd32(d + 4); }
uint32_t des_ptr(uint32_t d);
void des_setlen(uint32_t d, uint32_t len);
/* Decode a 16- or 8-bit descriptor into a host UTF-8/latin1 string (for paths, logging). */
char *des_to_cstr(uint32_t d, int wide, char *buf, size_t n);

/* ---- threads / requests / scheduler ---- */
typedef struct Thread Thread;
Thread *cur_thread(void);
CPU *cur_cpu(void);
void kernel_init(CPU *main_cpu);
void request_complete(Thread *t, uint32_t status, int32_t value);
void wait_any(void);                  /* User::WaitForAnyRequest */
void wait_for_request(uint32_t status);
void kernel_sleep_us(uint64_t us);
uint32_t *thread_tls(void);

/* Timers keyed by an owner id (RTimer handle). */
void timer_after(uint32_t owner, uint32_t status, uint64_t us);
void timer_cancel(uint32_t owner);

/* Handles (RHandleBase::iHandle values). */
enum { H_FREE, H_TIMER, H_MUTEX, H_THREAD, H_HEAP, H_OTHER };
uint32_t handle_new(int type, void *obj);
int handle_type(uint32_t h);
void *handle_obj(uint32_t h);
void handle_close(uint32_t h);

/* Host-side traps: run fn under a trap harness, returns leave code (0 = no leave). */
int32_t host_trap(CPU *c, void (*fn)(CPU *c, void *arg), void *arg);

/* Call guest code at `fn` with up to 8 word arguments (APCS: r0-r3, rest on the stack).
 * Callee-saved registers and sp are preserved; returns r0. */
uint32_t guest_call(CPU *c, uint32_t fn, int n, const uint32_t *args);
/* Guest virtual call with arguments (args[0] is `this`). */
uint32_t guest_vcall(CPU *c, int slot, int n, const uint32_t *args);

/* Run `fn(c, a)` later from the main thread's active scheduler, like a completed async request
 * delivering its callback. */
typedef void (*HostCallback)(CPU *c, const uint32_t *a);
void post_callback(HostCallback fn, int n, const uint32_t *a);
/* Same, delivered on guest thread `tid` (which must have called callbacks_init()). */
void post_callback_to(int tid, HostCallback fn, int n, const uint32_t *a);
void callbacks_init(void);
int kernel_thread_id(void);

/* Host active object (RunL = `runl`) in the calling thread's scheduler; host_ao_after arms its
 * timer so RunL runs after `us` microseconds. */
uint32_t host_ao_new(const char *name, GuestFn runl);
void host_ao_after(uint32_t ao, uint32_t us);

/* Audio output provided by the front end (SDL); NULL members = headless (simulated clock). */
typedef struct {
    int (*open)(int rate, int channels);          /* returns device id, <= 0 on failure */
    void (*queue)(int dev, const int16_t *pcm, uint32_t bytes);
    uint32_t (*queued_bytes)(int dev);
    void (*clear)(int dev);
    void (*close)(int dev);
} AudioBackend;
extern AudioBackend g_audio;

/* Registration tables; each module exports one, terminated by a {0} entry. */
extern const HleEntry HLE_EUSER[], HLE_LIBC[], HLE_APP[], HLE_MISC[];
