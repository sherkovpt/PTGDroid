/* Runs recompiled guest functions against expectations produced by tools/difftest.py (Unicorn). */
#include "hle.h"
#include <stdlib.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#define DATA_BASE 0x20000000u
#define DATA_SIZE 0x2000u
#define STACK_BASE 0x30000000u
#define STACK_SIZE 0x1000u
#define SENTINEL 0x0FFFF000u
#define PAGE 0x1000u
#define MAX_DEMAND_PAGES 256

static uint32_t rng;
static uint32_t next(void) {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}
static uint32_t val(void) {
    switch (next() & 3) {
    case 0: return DATA_BASE + ((next() % DATA_SIZE) & ~3u);
    case 1: return next() & 0xFF;
    case 2: return next();
    default: return 0;
    }
}

static uint8_t init_data[DATA_SIZE], init_stack[STACK_SIZE];
static uint32_t case_seed, import_calls;

/* Must match fake_import_result() in tools/difftest.py. */
static void fake_import(CPU *c, uint32_t idx) {
    uint32_t saved = rng;
    rng = case_seed ^ (idx << 16) ^ (import_calls * 0x9E3779B1u);
    if (!rng) rng = 1;
    c->r[0] = val();
    rng = saved;
    import_calls++;
}

/* ---- on-demand pages, must match demand_page() in tools/difftest.py ---- */
static uint32_t demand_pages[MAX_DEMAND_PAGES];
static unsigned demand_count;

static void demand_fill(uint32_t page, uint8_t *out) {
    uint32_t saved = rng;
    rng = case_seed ^ page ^ 0x5BD1E995u;
    if (!rng) rng = 1;
    for (uint32_t i = 0; i < PAGE; i += 4) { uint32_t v = val(); memcpy(out + i, &v, 4); }
    rng = saved;
}

static int demand_allowed(uint32_t page) {
    return page >= PAGE && !(page >= CODE_BASE && page < CODE_BASE + 0x200000u) && page != SENTINEL
        && !(page >= DATA_BASE && page < DATA_BASE + DATA_SIZE)
        && !(page >= STACK_BASE && page < STACK_BASE + STACK_SIZE);
}

static void demand_reset(void) {
#ifdef _WIN32
    for (unsigned i = 0; i < demand_count; i++) VirtualFree(MEM + demand_pages[i], PAGE, MEM_DECOMMIT);
#endif
    demand_count = 0;
}

#ifdef _MSC_VER
static int fault_filter(EXCEPTION_POINTERS *ep) {
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || er->ExceptionInformation[0] == 8)
        return EXCEPTION_EXECUTE_HANDLER;
    uint8_t *p = (uint8_t *)er->ExceptionInformation[1];
    if (p < MEM || p >= MEM + (1ull << 32)) return EXCEPTION_EXECUTE_HANDLER;
    uint32_t page = (uint32_t)(p - MEM) & ~(PAGE - 1);
    if (!demand_allowed(page) || demand_count == MAX_DEMAND_PAGES) return EXCEPTION_EXECUTE_HANDLER;
    for (unsigned i = 0; i < demand_count; i++)
        if (demand_pages[i] == page) return EXCEPTION_EXECUTE_HANDLER;
    VirtualAlloc(MEM + page, PAGE, MEM_COMMIT, PAGE_READWRITE);
    demand_fill(page, MEM + page);
    demand_pages[demand_count++] = page;
    return EXCEPTION_CONTINUE_EXECUTION;
}
#endif

static void setup(CPU *c, uint32_t seed) {
    rng = seed ? seed : 1;
    memset(c, 0, sizeof *c);
    for (int i = 0; i < 13; i++) c->r[i] = val();
    c->r[13] = STACK_BASE + STACK_SIZE - 512;
    c->r[14] = SENTINEL;
    uint32_t f = next() & 0xF0000000u;
    c->n = f >> 31; c->z = (f >> 30) & 1; c->c = (f >> 29) & 1; c->v = (f >> 28) & 1;
    for (uint32_t i = 0; i < DATA_SIZE; i += 4) { uint32_t v = val(); memcpy(init_data + i, &v, 4); }
    for (uint32_t i = 0; i < STACK_SIZE; i += 4) { uint32_t v = val(); memcpy(init_stack + i, &v, 4); }
    memcpy(MEM + DATA_BASE, init_data, DATA_SIZE);
    memcpy(MEM + STACK_BASE, init_stack, STACK_SIZE);
}

/* 0 = ran, 1 = guest_fault, 2 = host exception (wild memory access) */
/* Runs the function through dispatch: recompiled code when linked, the interpreter otherwise. */
static uint32_t g_target;
static void via_dispatch(CPU *c) { dispatch(c, g_target); }

static int run_guarded(GuestFn fn, CPU *c) {
    jmp_buf jb;
    g_fault_jmp = &jb;
    if (setjmp(jb)) { g_fault_jmp = NULL; return 1; }
#ifdef _MSC_VER
    __try { fn(c); } __except (fault_filter(GetExceptionInformation())) { g_fault_jmp = NULL; return 2; }
#else
    fn(c);
#endif
    g_fault_jmp = NULL;
    return 0;
}

static uint32_t rd_u32(FILE *f) { uint32_t v = 0; fread(&v, 4, 1, f); return v; }

/* Expected post-state: sparse list of (addr, byte) changes relative to the initial state. */
typedef struct { uint32_t addr, val; } Diff;

static int initial_byte(uint32_t a, uint8_t *out) {
    if (a >= DATA_BASE && a < DATA_BASE + DATA_SIZE) { *out = init_data[a - DATA_BASE]; return 1; }
    if (a >= STACK_BASE && a < STACK_BASE + STACK_SIZE) { *out = init_stack[a - STACK_BASE]; return 1; }
    return 0;
}

static int check_memory(Diff *d, uint32_t nd, char *why, size_t whylen) {
    /* 1) every expected change is present */
    for (uint32_t i = 0; i < nd; i++) {
        uint32_t a = d[i].addr;
        uint32_t page = a & ~(PAGE - 1);
        int mapped = (a >= DATA_BASE && a < DATA_BASE + DATA_SIZE) || (a >= STACK_BASE && a < STACK_BASE + STACK_SIZE);
        for (unsigned k = 0; k < demand_count && !mapped; k++) mapped = demand_pages[k] == page;
        if (!mapped) { snprintf(why, whylen, "mem %08x never touched, want %02x", a, d[i].val); return 0; }
        if (MEM[a] != (uint8_t)d[i].val) { snprintf(why, whylen, "mem %08x got %02x want %02x", a, MEM[a], d[i].val); return 0; }
    }
    /* 2) no unexpected changes: build expected image of each region and compare */
    static uint8_t buf[PAGE];
    struct { uint32_t base, size; } regions[2 + MAX_DEMAND_PAGES];
    unsigned nr = 0;
    regions[nr].base = DATA_BASE; regions[nr++].size = DATA_SIZE;
    regions[nr].base = STACK_BASE; regions[nr++].size = STACK_SIZE;
    for (unsigned k = 0; k < demand_count; k++) { regions[nr].base = demand_pages[k]; regions[nr++].size = PAGE; }
    for (unsigned r = 0; r < nr; r++) {
        for (uint32_t off = 0; off < regions[r].size; off += PAGE) {
            uint32_t base = regions[r].base + off;
            if (r < 2) { for (uint32_t i = 0; i < PAGE; i++) initial_byte(base + i, &buf[i]); }
            else demand_fill(base, buf);
            for (uint32_t i = 0; i < nd; i++)
                if (d[i].addr >= base && d[i].addr < base + PAGE) buf[d[i].addr - base] = (uint8_t)d[i].val;
            for (uint32_t i = 0; i < PAGE; i++)
                if (MEM[base + i] != buf[i]) {
                    snprintf(why, whylen, "mem %08x got %02x want %02x", base + i, MEM[base + i], buf[i]);
                    return 0;
                }
        }
    }
    return 1;
}

int main(int argc, char **argv) {
    if (argc < 3) { LOG("usage: %s <6r72.app> <cases.bin>", argv[0]); return 2; }
    set_card_root_from_app(argv[1]);
    mem_init();
    load_image(argv[1]);
    hle_bind();
    mem_commit(DATA_BASE, DATA_SIZE);
    mem_commit(STACK_BASE, STACK_SIZE);
    g_hle_fake = fake_import;
    static CPU kernel_cpu; /* traps are per thread: the harness runs as the kernel's main thread */
    kernel_init(&kernel_cpu);
    FILE *f = fopen(argv[2], "rb");
    if (!f) { LOG("cannot open %s", argv[2]); return 2; }
    char magic[4];
    fread(magic, 1, 4, f);
    uint32_t n = rd_u32(f);
    unsigned pass = 0, fail = 0;
    uint32_t last_failed_fn = 0;
    Diff *diffs = NULL;
    uint32_t cap = 0;
    for (uint32_t k = 0; k < n; k++) {
        uint32_t fn_addr = rd_u32(f), seed = rd_u32(f), exp[15];
        for (int i = 0; i < 15; i++) exp[i] = rd_u32(f);
        uint32_t exp_flags = rd_u32(f), ndiff = rd_u32(f);
        if (ndiff > cap) { cap = ndiff * 2; diffs = realloc(diffs, cap * sizeof *diffs); }
        for (uint32_t d = 0; d < ndiff; d++) { diffs[d].addr = rd_u32(f); diffs[d].val = rd_u32(f); }
        CPU c;
        demand_reset();
        setup(&c, seed);
        case_seed = seed;
        import_calls = 0;
        g_target = fn_addr;
        GuestFn fn = via_dispatch;
        int st = run_guarded(fn, &c);
        char why[256] = "";
        if (st == 1) snprintf(why, sizeof why, "guest_fault %08x %s", g_fault_addr, fn ? g_fault_what : "no fn");
        else if (st == 2) snprintf(why, sizeof why, "host exception");
        else {
            for (int i = 0; i < 15 && !why[0]; i++)
                if (c.r[i] != exp[i]) snprintf(why, sizeof why, "r%d got %08x want %08x", i, c.r[i], exp[i]);
            uint32_t flags = ((uint32_t)c.n << 31) | ((uint32_t)c.z << 30) | ((uint32_t)c.c << 29) | ((uint32_t)c.v << 28);
            if (!why[0] && flags != exp_flags) snprintf(why, sizeof why, "flags got %x want %x", flags >> 28, exp_flags >> 28);
            if (!why[0]) check_memory(diffs, ndiff, why, sizeof why);
        }
        if (why[0]) {
            fail++;
            if (fn_addr != last_failed_fn) printf("FAIL %08x seed %08x: %s\n", fn_addr, seed, why);
            last_failed_fn = fn_addr;
        } else {
            pass++;
        }
    }
    fclose(f);
    printf("difftest: %u passed, %u failed\n", pass, fail);
    return fail ? 1 : 0;
}
