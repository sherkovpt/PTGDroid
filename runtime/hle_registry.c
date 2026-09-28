#include "hle.h"
#include <stdlib.h>

/* HALData::TAttribute values are logged so each one can be answered deliberately. */
enum { EMachineUid = 5, ECPUSpeed = 11, ESystemTickPeriod = 14, EMemoryRAM = 15, EMemoryRAMFree = 16,
       EMemoryROM = 17, EMemoryPageSize = 18 };

static void HAL_Get(CPU *c) {
    uint32_t attr = ARG(c, 0), out = ARG(c, 1);
    int32_t v;
    switch (attr) {
    case EMachineUid: v = 0x101F8C19; break; /* N-Gage */
    case ECPUSpeed: v = 104000; break;       /* kHz */
    case ESystemTickPeriod: v = 15625; break;
    case EMemoryRAM: v = 16 << 20; break;
    case EMemoryRAMFree: v = 8 << 20; break;
    case EMemoryROM: v = 16 << 20; break;
    case EMemoryPageSize: v = 4096; break;
    default:
        LOG("HAL::Get(attribute %u) not supported", attr);
        wr32(out, 0);
        RET(KErrNotSupported);
        return;
    }
    wr32(out, (uint32_t)v);
    RET(KErrNone);
}

extern char g_card_root[512]; /* host directory mapped as the N-Gage memory card (E:) */

/* nokiafc ordinal 1: (const TDesC8& aGameId, const TDesC16& aErrorText). Verifies the memory card
 * holds this game by comparing aGameId with the text in \game.id; shows aErrorText and exits
 * otherwise. */
static void NokiaFC_CheckGameId(CPU *c) {
    char want[128], msg[128], path[600], line[256] = "";
    des_to_cstr(ARG(c, 0), 0, want, sizeof want);
    snprintf(path, sizeof path, "%s/game.id", g_card_root);
    FILE *f = fopen(path, "rb");
    if (f) { fgets(line, sizeof line, f); fclose(f); }
    if (!strncmp(line, want, strlen(want))) return;
    LOG("card check failed (%s): %s", path, des_to_cstr(ARG(c, 1), 1, msg, sizeof msg));
    exit(1);
}

const HleEntry HLE_MISC[] = {
    {"hal", "Get__3HALQ27HALData10TAttributeRi", HAL_Get},
    {"nokiafc", "@1", NokiaFC_CheckGameId},
    {0, 0, 0},
};

extern const HleEntry HLE_GFX[], HLE_FS[], HLE_NET[], HLE_AUDIO[];
const HleEntry *const HLE_TABLES[] = { HLE_EUSER, HLE_LIBC, HLE_APP, HLE_GFX, HLE_FS, HLE_NET, HLE_AUDIO, HLE_MISC, 0 };
