/* FBSCLI / BITGDI / WS32 bitmaps and the screen.
 * CFbsBitmap (6.1): vptr, iFbs@4, iAddressPointer@8, iHandle@12, iServerHandle@16; CWsBitmap adds
 * MWsClientClass {iWsHandle@20, iBuffer@24}. Pixel data lives in guest memory so the game can
 * write it directly via DataAddress()/TBitmapUtil. */
#include "hle.h"
#include <stdlib.h>

enum { ENone, EGray2, EGray4, EGray16, EGray256, EColor16, EColor256, EColor64K, EColor16M, ERgb, EColor4K };

#define SCREEN_W 176
#define SCREEN_H 208
#define BMP_DATA 8   /* iAddressPointer: we store the pixel buffer address here */
#define BMP_HANDLE 12

typedef struct { uint32_t obj, data; int w, h, mode, stride; } Bitmap;
#define MAX_BITMAPS 256
static Bitmap bitmaps[MAX_BITMAPS];

static int bpp(int mode) {
    switch (mode) {
    case EGray2: return 1; case EGray4: return 2; case EGray16: case EColor16: return 4;
    case EGray256: case EColor256: return 8; case EColor4K: case EColor64K: return 16;
    default: return 32;
    }
}

static Bitmap *bmp_of(uint32_t obj) {
    for (int i = 0; i < MAX_BITMAPS; i++) if (bitmaps[i].obj == obj) return &bitmaps[i];
    return NULL;
}

static void CWsBitmap_ctor(CPU *c) { (void)c; }

/* TInt CWsBitmap::Create(const TSize&, TDisplayMode) */
static void CWsBitmap_Create(CPU *c) {
    uint32_t t = ARG(c, 0), sz = ARG(c, 1);
    int w = (int32_t)rd32(sz), h = (int32_t)rd32(sz + 4), mode = (int)ARG(c, 2);
    Bitmap *b = bmp_of(t);
    if (!b) b = bmp_of(0);
    if (!b) { RET(KErrNoMemory); return; }
    int stride = ((w * bpp(mode) + 31) / 32) * 4;
    uint32_t data = gallocz((uint32_t)(stride * h));
    if (!data) { RET(KErrNoMemory); return; }
    *b = (Bitmap){ t, data, w, h, mode, stride };
    wr32(t + BMP_DATA, data);
    wr32(t + BMP_HANDLE, (uint32_t)(b - bitmaps) + 1);
    LOG("CWsBitmap::Create(%dx%d, mode %d) data=%08x", w, h, mode, data);
    RET(KErrNone);
}

static void CFbsBitmap_DataAddress(CPU *c) {
    Bitmap *b = bmp_of(ARG(c, 0));
    RET(b ? b->data : 0);
}

/* TBitmapUtil { CFbsBitmap* iFbsBitmap; TUint32* iWordPos; ... }: pixel access helper.
 * Begin/End only lock the font/bitmap server heap on the device; nothing to do here. */
static void TBitmapUtil_ctor(CPU *c) { wr32(ARG(c, 0), ARG(c, 1)); }
static void TBitmapUtil_nop(CPU *c) { (void)c; }

/* ---- screen ---- */
uint32_t g_screen_fb; /* guest framebuffer, EColor4K, 176x208, stride 352 */
int g_frames;

static void dump_bmp(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    uint32_t row = SCREEN_W * 3, pad = (4 - row % 4) % 4, size = 54 + (row + pad) * SCREEN_H;
    uint8_t h[54] = { 'B', 'M' };
    memcpy(h + 2, &size, 4);
    h[10] = 54; h[14] = 40;
    int32_t w = SCREEN_W, hh = SCREEN_H;
    memcpy(h + 18, &w, 4); memcpy(h + 22, &hh, 4);
    h[26] = 1; h[28] = 24;
    fwrite(h, 1, 54, f);
    for (int y = SCREEN_H - 1; y >= 0; y--) {
        for (int x = 0; x < SCREEN_W; x++) {
            uint32_t p = rd16(g_screen_fb + (uint32_t)(y * SCREEN_W + x) * 2);
            uint8_t px[3] = { (uint8_t)((p & 0xF) * 17), (uint8_t)(((p >> 4) & 0xF) * 17), (uint8_t)(((p >> 8) & 0xF) * 17) };
            fwrite(px, 1, 3, f);
        }
        fwrite("\0\0\0", 1, pad, f);
    }
    fclose(f);
}

/* Set by the platform front end; receives the EColor4K (0x0RGB) framebuffer on each Update(). */
void (*g_display_submit)(const uint8_t *fb, int w, int h);

void app_on_frame(void);

void screen_present(void) {
    g_frames++;
    app_on_frame();
    static int fps_on = -1, count;
    static uint64_t t0;
    #ifdef __ANDROID__
    if (fps_on < 0) fps_on = 1;
#endif
    if (fps_on < 0) fps_on = getenv("PTG_FPS") != NULL;
    if (fps_on) {
        uint64_t now = plat_now_us();
        if (!t0) t0 = now;
        count++;
        if (now - t0 >= 5000000) {
            LOG("fps: %.1f", count * 1e6 / (double)(now - t0));
            count = 0;
            t0 = now;
        }
    }
    if (g_display_submit) {
        g_display_submit(MEM + g_screen_fb, SCREEN_W, SCREEN_H);
        return;
    }
    if (g_frames % 30 == 1) {
        char path[64];
        snprintf(path, sizeof path, "frame_%05d.bmp", g_frames);
        dump_bmp(path);
        LOG("screen: frame %d dumped to %s", g_frames, path);
    }
}

static void CFbsScreenDevice_Update(CPU *c) { (void)c; screen_present(); }
static void CFbsBitGc_SetClippingRegion(CPU *c) { (void)c; }

/* ---- diagnostic vtables: every unimplemented virtual reports its slot ---- */
#define DIAG_SLOTS 64
static const char *diag_owner(uint32_t obj);
static void diag_slot(CPU *c, int slot) {
    LOG("unimplemented virtual: %s(%08x) slot %d", diag_owner(c->r[0]), c->r[0], slot);
    guest_fault(c, c->r[14], "unimplemented virtual");
}
#define D(n) static void diag_##n(CPU *c) { diag_slot(c, n); }
D(0) D(1) D(2) D(3) D(4) D(5) D(6) D(7) D(8) D(9) D(10) D(11) D(12) D(13) D(14) D(15)
D(16) D(17) D(18) D(19) D(20) D(21) D(22) D(23) D(24) D(25) D(26) D(27) D(28) D(29) D(30) D(31)
D(32) D(33) D(34) D(35) D(36) D(37) D(38) D(39) D(40) D(41) D(42) D(43) D(44) D(45) D(46) D(47)
D(48) D(49) D(50) D(51) D(52) D(53) D(54) D(55) D(56) D(57) D(58) D(59) D(60) D(61) D(62) D(63)
#undef D
static const GuestFn diag_fns[DIAG_SLOTS] = {
    diag_0, diag_1, diag_2, diag_3, diag_4, diag_5, diag_6, diag_7, diag_8, diag_9, diag_10, diag_11,
    diag_12, diag_13, diag_14, diag_15, diag_16, diag_17, diag_18, diag_19, diag_20, diag_21, diag_22,
    diag_23, diag_24, diag_25, diag_26, diag_27, diag_28, diag_29, diag_30, diag_31, diag_32, diag_33,
    diag_34, diag_35, diag_36, diag_37, diag_38, diag_39, diag_40, diag_41, diag_42, diag_43, diag_44,
    diag_45, diag_46, diag_47, diag_48, diag_49, diag_50, diag_51, diag_52, diag_53, diag_54, diag_55,
    diag_56, diag_57, diag_58, diag_59, diag_60, diag_61, diag_62, diag_63,
};

/* Host-created objects, remembered so diagnostics can name them. */
typedef struct { uint32_t obj; const char *name; } Named;
static Named named[64];
static int nnamed;
static const char *diag_owner(uint32_t obj) {
    for (int i = 0; i < nnamed; i++) if (named[i].obj == obj) return named[i].name;
    return "?";
}

/* Object with a vtable of `n` slots: `impl` overrides (NULL entries fall back to diagnostics). */
uint32_t host_object(const char *name, uint32_t size, const GuestFn *impl, int n) {
    GuestFn slots[DIAG_SLOTS];
    for (int i = 0; i < DIAG_SLOTS; i++) slots[i] = (i < n && impl[i]) ? impl[i] : diag_fns[i];
    uint32_t o = gallocz(size);
    wr32(o, make_vtable(slots, DIAG_SLOTS));
    if (nnamed < 64) named[nnamed++] = (Named){ o, name };
    return o;
}

/* ---- Direct Screen Access ----
 * CDirectScreenAccess : CActive { iGc@24, iScreenDevice@28, iDrawingRegion@32, ... } */
#define DSA_GC 24
#define DSA_DEVICE 28
#define DSA_REGION 32
static uint32_t g_dsa, g_screen_device, g_screen_gc, g_region;

static void dsa_dtor(CPU *c) { if (ARG(c, 1) & 1) gfree(ARG(c, 0)); }
static void dsa_docancel(CPU *c) { (void)c; }
static void dsa_runl(CPU *c) { (void)c; }
static void ret_arg1(CPU *c) { RET(ARG(c, 1)); }

/* CFbsBitGc slot 46 = BitBlt(const TPoint&, const CFbsBitmap*): the game's back buffer -> screen. */
static void gc_bitblt(CPU *c) {
    uint32_t pt = ARG(c, 1);
    Bitmap *b = bmp_of(ARG(c, 2));
    if (!b || b->mode != EColor4K) {
        LOG("BitBlt: unsupported bitmap %08x (mode %d)", ARG(c, 2), b ? b->mode : -1);
        return;
    }
    int dx = (int32_t)rd32(pt), dy = (int32_t)rd32(pt + 4);
    for (int y = 0; y < b->h; y++) {
        int sy = y + dy;
        if (sy < 0 || sy >= SCREEN_H) continue;
        for (int x = 0; x < b->w; x++) {
            int sx = x + dx;
            if (sx < 0 || sx >= SCREEN_W) continue;
            wr16(g_screen_fb + (uint32_t)(sy * SCREEN_W + sx) * 2, rd16(b->data + (uint32_t)(y * b->stride + x * 2)));
        }
    }
}

/* ws32@348 = CDirectScreenAccess::NewL(RWsSession&, CWsScreenDevice&, RWindowBase&, MDirectScreenAccess&) */
static void CDirectScreenAccess_NewL(CPU *c) {
    GuestFn v[] = { dsa_dtor, dsa_docancel, dsa_runl, ret_arg1 };
    g_dsa = host_object("CDirectScreenAccess", 64, v, 4);
    wr32(g_dsa + 20, 0); /* priority */
    if (!g_screen_fb) {
        g_screen_fb = gallocz(SCREEN_W * SCREEN_H * 2);
        g_screen_device = host_object("CFbsScreenDevice", 64, NULL, 0);
        GuestFn gc[47] = { 0 };
        gc[46] = gc_bitblt;
        g_screen_gc = host_object("CFbsBitGc", 128, gc, 47);
        /* RRegion: TRegion{iCount, iRectangleList, iError} + granularity; one full-screen rect */
        g_region = gallocz(64);
        uint32_t rect = gallocz(16);
        wr32(rect + 8, SCREEN_W); wr32(rect + 12, SCREEN_H);
        wr32(g_region, 1); wr32(g_region + 4, rect);
    }
    wr32(g_dsa + DSA_GC, g_screen_gc);
    wr32(g_dsa + DSA_DEVICE, g_screen_device);
    wr32(g_dsa + DSA_REGION, g_region);
    LOG("CDirectScreenAccess::NewL -> %08x (gc %08x, device %08x)", g_dsa, g_screen_gc, g_screen_device);
    RET(g_dsa);
}

/* ws32@350 = CDirectScreenAccess::StartL() */
static void CDirectScreenAccess_StartL(CPU *c) { LOG("CDirectScreenAccess::StartL(%08x)", ARG(c, 0)); }

const HleEntry HLE_GFX[] = {
    {"ws32", "__9CWsBitmapR10RWsSession", CWsBitmap_ctor},
    {"ws32", "Create__9CWsBitmapRC5TSize12TDisplayMode", CWsBitmap_Create},
    {"fbscli", "DataAddress__C10CFbsBitmap", CFbsBitmap_DataAddress},
    {"fbscli", "__11TBitmapUtilP10CFbsBitmap", TBitmapUtil_ctor},
    {"fbscli", "Begin__11TBitmapUtilRC6TPoint", TBitmapUtil_nop},
    {"fbscli", "End__11TBitmapUtil", TBitmapUtil_nop},
    {"bitgdi", "Update__16CFbsScreenDevice", CFbsScreenDevice_Update},
    {"bitgdi", "SetClippingRegion__9CFbsBitGcPC7TRegion", CFbsBitGc_SetClippingRegion},
    {"ws32", "@348", CDirectScreenAccess_NewL},
    {"ws32", "@350", CDirectScreenAccess_StartL},
    {0, 0, 0},
};
