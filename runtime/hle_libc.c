/* ESTLIB (C library) and libgcc soft-float helpers.
 * Doubles use the FPA layout of pre-EABI Symbian: most significant word first (r0 = hi, r1 = lo). */
#include "hle.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static float argf(CPU *c, int i) { uint32_t u = ARG(c, i); float f; memcpy(&f, &u, 4); return f; }
static void retf(CPU *c, float f) { uint32_t u; memcpy(&u, &f, 4); RET(u); }
static double argd(CPU *c, int i) { /* words i (hi), i+1 (lo) */
    uint64_t u = ((uint64_t)ARG(c, i) << 32) | ARG(c, i + 1);
    double d; memcpy(&d, &u, 8); return d;
}
static void retd(CPU *c, double d) {
    uint64_t u; memcpy(&u, &d, 8);
    c->r[0] = (uint32_t)(u >> 32);
    c->r[1] = (uint32_t)u;
}

static void addsf3(CPU *c) { retf(c, argf(c, 0) + argf(c, 1)); }
static void mulsf3(CPU *c) { retf(c, argf(c, 0) * argf(c, 1)); }
static void divsf3(CPU *c) { retf(c, argf(c, 0) / argf(c, 1)); }
static void ltsf2(CPU *c) {
    float a = argf(c, 0), b = argf(c, 1);
    RET(a < b ? -1 : (a == b ? 0 : 1)); /* NaN compares as "not less" */
}
static void fixsfsi(CPU *c) { RET((int32_t)argf(c, 0)); }
static void floatsisf(CPU *c) { retf(c, (float)(int32_t)ARG(c, 0)); }
static void adddf3(CPU *c) { retd(c, argd(c, 0) + argd(c, 2)); }
static void divdf3(CPU *c) { retd(c, argd(c, 0) / argd(c, 2)); }
static void fixdfsi(CPU *c) { RET((int32_t)argd(c, 0)); }
static void floatsidf(CPU *c) { retd(c, (double)(int32_t)ARG(c, 0)); }
static void extendsfdf2(CPU *c) { retd(c, (double)argf(c, 0)); }
static void l_ceil(CPU *c) { retd(c, ceil(argd(c, 0))); }

/* ---- string / memory ---- */
static uint32_t gstrlen(uint32_t s) { uint32_t n = 0; while (rd8(s + n)) n++; return n; }
static void l_strlen(CPU *c) { RET(gstrlen(ARG(c, 0))); }
static void l_memmove(CPU *c) { memmove(MEM + ARG(c, 0), MEM + ARG(c, 1), ARG(c, 2)); }
static void l_memcmp(CPU *c) { RET(memcmp(MEM + ARG(c, 0), MEM + ARG(c, 1), ARG(c, 2))); }
static void l_strcpy(CPU *c) { uint32_t d = ARG(c, 0); memmove(MEM + d, MEM + ARG(c, 1), gstrlen(ARG(c, 1)) + 1); }
static void l_strcat(CPU *c) {
    uint32_t d = ARG(c, 0);
    memmove(MEM + d + gstrlen(d), MEM + ARG(c, 1), gstrlen(ARG(c, 1)) + 1);
}
static void l_strncpy(CPU *c) {
    uint32_t d = ARG(c, 0), s = ARG(c, 1), n = ARG(c, 2), i = 0;
    for (; i < n && rd8(s + i); i++) wr8(d + i, rd8(s + i));
    for (; i < n; i++) wr8(d + i, 0);
}
static void l_strcmp(CPU *c) { RET(strcmp((char *)MEM + ARG(c, 0), (char *)MEM + ARG(c, 1))); }
static void l_strncmp(CPU *c) { RET(strncmp((char *)MEM + ARG(c, 0), (char *)MEM + ARG(c, 1), ARG(c, 2))); }
static void l_strchr(CPU *c) {
    uint32_t s = ARG(c, 0);
    uint8_t ch = (uint8_t)ARG(c, 1);
    for (;; s++) {
        if (rd8(s) == ch) { RET(s); return; }
        if (!rd8(s)) { RET(0); return; }
    }
}
static void l_atoi(CPU *c) { RET(atoi((char *)MEM + ARG(c, 0))); }
static void l_isalpha(CPU *c) { RET(isalpha((int)(ARG(c, 0) & 0xFF))); }
static void l_isspace(CPU *c) { RET(isspace((int)(ARG(c, 0) & 0xFF))); }
static void l_abort(CPU *c) { guest_fault(c, c->r[14], "abort()"); }

static void l_malloc(CPU *c) { RET(galloc(ARG(c, 0))); }
static void l_calloc(CPU *c) { RET(gallocz(ARG(c, 0) * ARG(c, 1))); }
static void l_free(CPU *c) { gfree(ARG(c, 0)); }

static uint32_t g_errno;
static void l_errno(CPU *c) { if (!g_errno) g_errno = gallocz(4); RET(g_errno); }
static uint32_t g_stderr;
static void l_stderr(CPU *c) { if (!g_stderr) g_stderr = gallocz(64); RET(g_stderr); }

/* ---- printf family: format guest varargs starting at argument index `ai` ---- */
static size_t gformat(CPU *c, uint32_t fmt, int ai, char *out, size_t cap) {
    size_t n = 0;
    char spec[32], tmp[512];
    for (uint32_t p = fmt; rd8(p); p++) {
        char ch = (char)rd8(p);
        if (ch != '%') { if (n + 1 < cap) out[n++] = ch; continue; }
        size_t k = 0;
        spec[k++] = '%';
        p++;
        while (rd8(p) && strchr("-+ #0123456789.lh", rd8(p)) && k < sizeof spec - 2) spec[k++] = (char)rd8(p++);
        char conv = (char)rd8(p);
        if (!conv) break;
        /* drop length modifiers; everything is 32-bit on the guest */
        size_t j = 0;
        for (size_t i = 0; i < k; i++) if (spec[i] != 'l' && spec[i] != 'h') spec[j++] = spec[i];
        k = j;
        spec[k++] = conv;
        spec[k] = 0;
        tmp[0] = 0;
        switch (conv) {
        case 'd': case 'i': snprintf(tmp, sizeof tmp, spec, (int32_t)ARG(c, ai++)); break;
        case 'u': case 'x': case 'X': case 'o': case 'c': snprintf(tmp, sizeof tmp, spec, ARG(c, ai++)); break;
        case 'p': snprintf(tmp, sizeof tmp, "%08x", ARG(c, ai++)); break;
        case 's': {
            uint32_t s = ARG(c, ai++);
            snprintf(tmp, sizeof tmp, spec, s ? (char *)MEM + s : "(null)");
            break;
        }
        case 'f': case 'g': case 'e': case 'G': case 'E':
            snprintf(tmp, sizeof tmp, spec, argd(c, ai));
            ai += 2;
            break;
        case '%': strcpy(tmp, "%"); break;
        default: snprintf(tmp, sizeof tmp, "<%%%c?>", conv); break;
        }
        for (char *t = tmp; *t && n + 1 < cap; t++) out[n++] = *t;
    }
    out[n] = 0;
    return n;
}

static void l_sprintf(CPU *c) {
    char buf[4096];
    size_t n = gformat(c, ARG(c, 1), 2, buf, sizeof buf);
    memcpy(MEM + ARG(c, 0), buf, n + 1);
    RET(n);
}
static void l_fprintf(CPU *c) {
    char buf[4096];
    size_t n = gformat(c, ARG(c, 1), 2, buf, sizeof buf);
    fprintf(stderr, "[guest] %s", buf);
    RET(n);
}

/* sscanf subset: %d %i %u %x %s %c %f and literal matching. */
static void l_sscanf(CPU *c) {
    const char *s = (char *)MEM + ARG(c, 0);
    uint32_t f = ARG(c, 1);
    int ai = 2, assigned = 0;
    while (rd8(f)) {
        char ch = (char)rd8(f++);
        if (isspace((unsigned char)ch)) { while (isspace((unsigned char)*s)) s++; continue; }
        if (ch != '%') { if (*s != ch) break; s++; continue; }
        int width = 0;
        while (isdigit(rd8(f))) width = width * 10 + (rd8(f++) - '0');
        while (rd8(f) == 'l' || rd8(f) == 'h') f++;
        char conv = (char)rd8(f++);
        char *end;
        if (conv != 'c') while (isspace((unsigned char)*s)) s++;
        if (!*s) break;
        if (conv == 'd' || conv == 'i' || conv == 'u' || conv == 'x') {
            long v = strtol(s, &end, conv == 'x' ? 16 : (conv == 'i' ? 0 : 10));
            if (end == s) break;
            wr32(ARG(c, ai++), (uint32_t)v);
            s = end;
        } else if (conv == 'f') {
            float v = strtof(s, &end);
            if (end == s) break;
            uint32_t u; memcpy(&u, &v, 4);
            wr32(ARG(c, ai++), u);
            s = end;
        } else if (conv == 's') {
            uint32_t d = ARG(c, ai++);
            int i = 0;
            while (*s && !isspace((unsigned char)*s) && (!width || i < width)) wr8(d + i++, (uint8_t)*s++);
            wr8(d + i, 0);
        } else if (conv == 'c') {
            wr8(ARG(c, ai++), (uint8_t)*s++);
        } else {
            break;
        }
        assigned++;
    }
    RET(assigned);
}

const HleEntry HLE_LIBC[] = {
    {"estlib", "__errno", l_errno},
    {"estlib", "malloc", l_malloc},
    {"estlib", "calloc", l_calloc},
    {"estlib", "free", l_free},
    {"estlib", "memmove", l_memmove},
    {"estlib", "strlen", l_strlen},
    {"estlib", "ceil", l_ceil},
    {"estlib", "__stderr", l_stderr},
    {"estlib", "fprintf", l_fprintf},
    {"estlib", "sprintf", l_sprintf},
    {"estlib", "sscanf", l_sscanf},
    {"estlib", "abort", l_abort},
    {"estlib", "atoi", l_atoi},
    {"estlib", "isalpha", l_isalpha},
    {"estlib", "isspace", l_isspace},
    {"estlib", "memcmp", l_memcmp},
    {"estlib", "strcat", l_strcat},
    {"estlib", "strchr", l_strchr},
    {"estlib", "strcmp", l_strcmp},
    {"estlib", "strcpy", l_strcpy},
    {"estlib", "strncmp", l_strncmp},
    {"estlib", "strncpy", l_strncpy},
    {"euser", "__adddf3", adddf3},
    {"euser", "__addsf3", addsf3},
    {"euser", "__divdf3", divdf3},
    {"euser", "__divsf3", divsf3},
    {"euser", "__extendsfdf2", extendsfdf2},
    {"euser", "__fixdfsi", fixdfsi},
    {"euser", "__fixsfsi", fixsfsi},
    {"euser", "__floatsidf", floatsidf},
    {"euser", "__floatsisf", floatsisf},
    {"euser", "__ltsf2", ltsf2},
    {"euser", "__mulsf3", mulsf3},
    {0, 0, 0},
};
