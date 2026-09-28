#include "runtime.h"
#include "plat.h"
#include <stdlib.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/mman.h>
#endif

uint8_t *MEM;
uint32_t g_image_export1;

CPU *cur_cpu(void);

static void report_crash(uintptr_t fault) {
    CPU *c = cur_cpu();
    if (fault >= (uintptr_t)MEM && fault < (uintptr_t)MEM + (1ull << 32))
        LOG("CRASH: guest memory access to %08x (unmapped)", (uint32_t)(fault - (uintptr_t)MEM));
    else
        LOG("CRASH: host access violation at %p", (void *)fault);
    if (c) {
        LOG("  in guest function %08x (last entered), lr %08x", c->r[15], c->r[14]);
        for (int i = 0; i < 16; i += 4)
            LOG("  r%-2d %08x  r%-2d %08x  r%-2d %08x  r%-2d %08x", i, c->r[i], i + 1, c->r[i + 1],
                i + 2, c->r[i + 2], i + 3, c->r[i + 3]);
    }
    fflush(stderr);
}

#ifdef _WIN32
static LONG WINAPI crash_handler(EXCEPTION_POINTERS *ep) {
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        report_crash((uintptr_t)ep->ExceptionRecord->ExceptionInformation[1]);
        ExitProcess(3);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
void install_crash_handler(void) { AddVectoredExceptionHandler(0, crash_handler); }
#else
#include <signal.h>
#include <unistd.h>
static void crash_handler(int sig, siginfo_t *si, void *ctx) {
    (void)sig; (void)ctx;
    report_crash((uintptr_t)si->si_addr);
    _exit(3);
}
void install_crash_handler(void) {
#ifdef __ANDROID__
    return; /* ART relies on its own SIGSEGV handler (implicit null checks) */
#endif
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
}
#endif

void mem_init(void) {
#ifdef _WIN32
    MEM = VirtualAlloc(NULL, 1ull << 32, MEM_RESERVE, PAGE_NOACCESS);
#else
    MEM = mmap(NULL, 1ull << 32, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (MEM == MAP_FAILED) MEM = NULL;
#endif
    if (!MEM) { LOG("cannot reserve guest address space"); exit(1); }
}

void mem_commit(uint32_t addr, uint32_t size) {
    uint64_t lo = addr & ~0xFFFull, hi = ((uint64_t)addr + size + 0xFFF) & ~0xFFFull;
#ifdef _WIN32
    if (!VirtualAlloc(MEM + lo, (size_t)(hi - lo), MEM_COMMIT, PAGE_READWRITE)) {
#else
    if (mprotect(MEM + lo, (size_t)(hi - lo), PROT_READ | PROT_WRITE)) {
#endif
        LOG("commit failed %08x+%x", addr, size);
        exit(1);
    }
}

/* ---- modules: the game and the DLL it ships, loaded from the user's memory card ----
 * With PTG_RECOMPILED the build contains C translations of their code (generated from the user's
 * own copy by recomp/armrecomp.py); otherwise, and for anything not translated, the interpreter
 * runs the original ARM code. */
#ifdef PTG_RECOMPILED
extern const DispatchEntry DISPATCH_app[], DISPATCH_zlib[];
extern const unsigned DISPATCH_app_COUNT, DISPATCH_zlib_COUNT;
#define DISP(m) DISPATCH_##m, &DISPATCH_##m##_COUNT
#else
static const unsigned no_functions = 0;
#define DISP(m) NULL, &no_functions
#endif

typedef struct {
    const char *name;
    const char *relpath; /* relative to the card root; NULL = the image passed on the command line */
    uint32_t base;
    const DispatchEntry *disp;
    const unsigned *ndisp;
    uint32_t code_size, text_size;
    uint32_t import_base, nimports; /* slice of the global import table */
    uint32_t exports[256];
    unsigned nexports;
} Module;

/* Import indices are global: modules' import tables are concatenated in this order, which is
 * also the --import-base given to the recompiler for each module. */
static Module modules[] = {
    {"app", NULL, CODE_BASE, DISP(app)},
    {"zlib", "system/apps/6r72/zlib.dll", 0x18000000u, DISP(zlib)},
};
#define NMODULES (sizeof modules / sizeof modules[0])

static ImportInfo *import_info;
static unsigned import_count;

typedef struct { uint32_t addr, idx; } Stub;
static Stub *stubs;
static unsigned nstubs;

extern const SymbianOrdinal SYMBIAN_ORDINALS[];

static const char *symbian_name(const char *lib, uint32_t ordinal) {
    for (const SymbianOrdinal *o = SYMBIAN_ORDINALS; o->lib; o++)
        if (o->ordinal == ordinal && !strcmp(o->lib, lib)) return o->name;
    return NULL;
}

/* EKA1 import section: u32 size, then per DLL {u32 name offset, i32 count, u32 ordinals[count]}. */
static void parse_imports(Module *m, const uint8_t *buf, const uint32_t *h) {
    uint32_t io = h[27], ndll = h[21];
    m->import_base = import_count;
    if (!io) return;
    uint32_t p = io + 4;
    for (uint32_t d = 0; d < ndll; d++) {
        uint32_t name_off, n;
        memcpy(&name_off, buf + p, 4);
        memcpy(&n, buf + p + 4, 4);
        char lib[64];
        const char *dll = (const char *)buf + io + name_off;
        size_t k = 0;
        while (dll[k] && dll[k] != '[' && dll[k] != '.' && k < sizeof lib - 1) {
            lib[k] = (char)((dll[k] >= 'A' && dll[k] <= 'Z') ? dll[k] + 32 : dll[k]);
            k++;
        }
        lib[k] = 0;
        import_info = realloc(import_info, sizeof *import_info * (import_count + n));
        for (uint32_t i = 0; i < n; i++) {
            uint32_t ord;
            memcpy(&ord, buf + p + 8 + 4 * i, 4);
            ImportInfo *im = &import_info[import_count++];
            im->lib = strdup(lib);
            im->ordinal = ord;
            im->name = symbian_name(lib, ord);
        }
        p += 8 + 4 * n;
    }
    m->nimports = import_count - m->import_base;
}

/* Import stubs: ldr ip,[pc,#4]; ldr ip,[ip]; bx ip; .word <IAT slot> */
static void find_stubs(Module *m) {
    uint32_t iat = m->base + m->text_size;
    for (uint32_t off = 0; off + 16 <= m->text_size; off += 4) {
        uint32_t a = m->base + off;
        if (rd32(a) == 0xE59FC004u && rd32(a + 4) == 0xE59CC000u && rd32(a + 8) == 0xE12FFF1Cu) {
            uint32_t slot = (rd32(a + 12) - iat) / 4;
            if (slot >= m->nimports) continue;
            stubs = realloc(stubs, sizeof *stubs * (nstubs + 1));
            stubs[nstubs].addr = a;
            stubs[nstubs].idx = m->import_base + slot;
            nstubs++;
        }
    }
}

int stub_import_index(uint32_t addr) {
    unsigned lo = 0, hi = nstubs;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (stubs[mid].addr < addr) lo = mid + 1; else hi = mid;
    }
    return lo < nstubs && stubs[lo].addr == addr ? (int)stubs[lo].idx : -1;
}

int is_trap_import(uint32_t idx) {
    return import_info[idx].name && !strcmp(import_info[idx].name, "Trap__5TTrapRi");
}

static int load_module(Module *m, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { LOG("cannot open %s", path); return 0; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(n);
    fread(buf, 1, n, f);
    fclose(f);
    uint32_t h[31];
    memcpy(h, buf, sizeof h);
    if (h[4] != 0x434F5045u) { LOG("%s is not an E32 image", path); free(buf); return 0; }
    uint32_t code_size = h[12], link_base = h[19], exp_off = h[22], exp_count = h[23];
    uint32_t code_off = h[25], reloc_off = h[28];
    uint32_t delta = m->base - link_base;
    m->code_size = code_size;
    m->text_size = h[24];
    mem_commit(m->base, code_size);
    uint8_t *code = MEM + m->base;
    memcpy(code, buf + code_off, code_size);
    if (delta && reloc_off) {
        uint32_t size;
        memcpy(&size, buf + reloc_off, 4);
        for (uint32_t p = reloc_off + 8; p < reloc_off + 8 + size;) {
            uint32_t page, block;
            memcpy(&page, buf + p, 4);
            memcpy(&block, buf + p + 4, 4);
            for (uint32_t i = 0; i < (block - 8) / 2; i++) {
                uint16_t e;
                memcpy(&e, buf + p + 8 + 2 * i, 2);
                if (e >> 12) {
                    uint32_t v;
                    memcpy(&v, code + page + (e & 0xFFF), 4);
                    v += delta;
                    memcpy(code + page + (e & 0xFFF), &v, 4);
                }
            }
            p += block;
        }
    }
    m->nexports = exp_count < 256 ? exp_count : 256;
    for (unsigned i = 0; i < m->nexports; i++) {
        uint32_t e;
        memcpy(&e, buf + exp_off + 4 * i, 4);
        if (e < link_base) e += link_base; /* stored code-relative */
        m->exports[i] = e + delta;
    }
    parse_imports(m, buf, h);
    free(buf);
    find_stubs(m);
    return 1;
}

static DispatchEntry *disp_all;
static unsigned disp_n;

static int disp_cmp(const void *a, const void *b) {
    uint32_t x = ((const DispatchEntry *)a)->addr, y = ((const DispatchEntry *)b)->addr;
    return x < y ? -1 : x > y;
}

char g_card_root[512];

void set_card_root_from_app(const char *app_path) {
    snprintf(g_card_root, sizeof g_card_root, "%s", app_path);
    for (int up = 0; up < 4; up++) {
        char *s = strrchr(g_card_root, '/'), *b = strrchr(g_card_root, '\\');
        if (b > s) s = b;
        if (!s) { strcpy(g_card_root, "."); return; }
        *s = 0;
    }
}

int load_image(const char *path) {
    for (unsigned i = 0; i < NMODULES; i++) {
        char p[1024];
        if (modules[i].relpath) snprintf(p, sizeof p, "%s/%s", g_card_root, modules[i].relpath);
        if (!load_module(&modules[i], modules[i].relpath ? p : path)) return 0;
        disp_n += *modules[i].ndisp;
    }
    disp_all = malloc(sizeof *disp_all * (disp_n ? disp_n : 1));
    unsigned k = 0;
    for (unsigned i = 0; i < NMODULES; i++) {
        if (*modules[i].ndisp) memcpy(disp_all + k, modules[i].disp, sizeof *disp_all * *modules[i].ndisp);
        k += *modules[i].ndisp;
    }
    qsort(disp_all, disp_n, sizeof *disp_all, disp_cmp);
    g_image_export1 = modules[0].exports[0];
    LOG("loaded %u modules, %u imports, %u recompiled functions%s", (unsigned)NMODULES, import_count, disp_n,
        disp_n ? "" : " (interpreter only)");
    return 1;
}

GuestFn dispatch_lookup(uint32_t addr) {
    unsigned lo = 0, hi = disp_n;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (disp_all[mid].addr < addr) lo = mid + 1; else hi = mid;
    }
    return lo < disp_n && disp_all[lo].addr == addr ? disp_all[lo].fn : NULL;
}

GuestFn hostfn_lookup(uint32_t addr);
void interp_call(CPU *c, uint32_t addr);

static int in_module_code(uint32_t addr) {
    for (unsigned i = 0; i < NMODULES; i++)
        if (addr >= modules[i].base && addr < modules[i].base + modules[i].text_size) return 1;
    return 0;
}

int guest_code_addr(uint32_t addr) { return in_module_code(addr & ~1u); }

static uint32_t g_watch = 1; /* 1 = not yet read from PTG_WATCH */

void dispatch(CPU *c, uint32_t addr) {
    if (g_watch == 1) g_watch = getenv("PTG_WATCH") ? (uint32_t)strtoul(getenv("PTG_WATCH"), NULL, 16) : 0;
    if (addr == g_watch)
        LOG("[%llu ms] watch %08x: r0=%08x r1=%08x r2=%08x lr=%08x", (unsigned long long)(plat_now_us() / 1000 % 1000000),
            addr, c->r[0], c->r[1], c->r[2], c->r[14]);
    GuestFn fn = dispatch_lookup(addr);
    if (!fn) fn = hostfn_lookup(addr);
    if (fn) { fn(c); return; }
    if (in_module_code(addr & ~1u)) { interp_call(c, addr); return; }
    guest_fault(c, addr, "dispatch: no code at address");
}

jmp_buf *g_fault_jmp;
const char *g_fault_what;
uint32_t g_fault_addr;

void guest_fault(CPU *c, uint32_t addr, const char *what) {
    if (g_fault_jmp) {
        g_fault_what = what;
        g_fault_addr = addr;
        longjmp(*g_fault_jmp, 1);
    }
    LOG("GUEST FAULT at %08x: %s", addr, what);
    for (int i = 0; i < 16; i += 4)
        LOG("  r%-2d %08x  r%-2d %08x  r%-2d %08x  r%-2d %08x", i, c->r[i], i + 1, c->r[i + 1],
            i + 2, c->r[i + 2], i + 3, c->r[i + 3]);
    abort();
}

uint32_t hle_swi(CPU *c, uint32_t num) {
    guest_fault(c, num, "SWI from application code");
}

/* ---- import binding ---- */

static GuestFn *bound;
static uint32_t *bound_addr; /* import satisfied by another loaded module's export */
extern const HleEntry *const HLE_TABLES[];

void hle_bind(void) {
    bound = calloc(import_count ? import_count : 1, sizeof *bound);
    bound_addr = calloc(import_count ? import_count : 1, sizeof *bound_addr);
    unsigned missing = 0;
    for (unsigned i = 0; i < import_count; i++) {
        const ImportInfo *im = &import_info[i];
        for (unsigned m = 0; m < NMODULES; m++) {
            if (!strcmp(modules[m].name, im->lib) && im->ordinal >= 1 && im->ordinal <= modules[m].nexports)
                bound_addr[i] = modules[m].exports[im->ordinal - 1];
        }
        if (bound_addr[i]) continue;
        char ord[16];
        snprintf(ord, sizeof ord, "@%u", im->ordinal);
        const char *want = im->name ? im->name : ord; /* unnamed imports are registered as "@<ordinal>" */
        for (const HleEntry *const *tab = HLE_TABLES; *tab && !bound[i]; tab++) {
            for (const HleEntry *h = *tab; h->name; h++) {
                if (!strcmp(h->lib, im->lib) && !strcmp(h->name, want)) {
                    bound[i] = h->fn;
                    break;
                }
            }
        }
        if (!bound[i]) missing++;
    }
    LOG("hle: %u/%u imports implemented", import_count - missing, import_count);
}

void (*g_hle_fake)(CPU *c, uint32_t idx);
int g_trace_imports = -1;
int kernel_thread_id(void);

void hle_call(CPU *c, uint32_t idx) {
    if (g_trace_imports < 0) g_trace_imports = getenv("PTG_TRACE") != NULL;
    if (g_trace_imports) {
        const ImportInfo *im = &import_info[idx];
        LOG("[t%d] %s:%s r0=%08x r1=%08x lr=%08x", kernel_thread_id(), im->lib,
            im->name ? im->name : "?", c->r[0], c->r[1], c->r[14]);
    }
    if (g_hle_fake) {
        g_hle_fake(c, idx);
        return;
    }
    if (bound_addr[idx]) {
        dispatch(c, bound_addr[idx]);
        return;
    }
    if (!bound[idx]) {
        const ImportInfo *im = &import_info[idx];
        LOG("unimplemented import %s@%u %s", im->lib, im->ordinal, im->name ? im->name : "?");
        guest_fault(c, c->r[14], "unimplemented import");
    }
    bound[idx](c);
}

