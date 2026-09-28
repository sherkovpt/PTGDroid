#pragma once
#include "cpu.h"
#include <setjmp.h>
#include <stdio.h>

#define CODE_BASE 0x10000000u

typedef struct { uint32_t addr; GuestFn fn; } DispatchEntry;
typedef struct { const char *lib; uint32_t ordinal; const char *name; } ImportInfo;
typedef struct { const char *lib; const char *name; GuestFn fn; } HleEntry;
typedef struct { const char *lib; uint32_t ordinal; const char *name; } SymbianOrdinal;


extern uint32_t g_image_export1; /* ordinal 1 export of the loaded image (NewApplication) */
void mem_init(void);
void mem_commit(uint32_t addr, uint32_t size);
/* Card root = directory containing "system/apps/6r72/6r72.app"; derived from that path. */
void set_card_root_from_app(const char *app_path);
int load_image(const char *path);
void hle_bind(void);
GuestFn dispatch_lookup(uint32_t addr);

/* Guest-visible TRAP frames (TTrap::Trap / User::Leave). */
typedef struct TrapFrame {
    jmp_buf jb;
    uint32_t saved[16];
    uint32_t result_ptr;
    int host;     /* frame pushed by host_trap rather than guest TTrap::Trap */
    int32_t code; /* leave code, for host frames */
} TrapFrame;

jmp_buf *trap_enter(CPU *c);
void trap_resume(CPU *c);
void trap_hle_trap(CPU *c);
void trap_hle_untrap(CPU *c);
NORETURN void trap_leave(CPU *c, int32_t code);

/* When set, guest_fault longjmps here instead of aborting (test harness). */
extern jmp_buf *g_fault_jmp;
extern const char *g_fault_what;
extern uint32_t g_fault_addr;
/* When set, replaces every import call (test harness). */
extern void (*g_hle_fake)(CPU *c, uint32_t idx);

#ifdef __ANDROID__
#include <android/log.h>
#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "PTG", __VA_ARGS__)
#else
#define LOG(...) do { fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } while (0)
#endif
