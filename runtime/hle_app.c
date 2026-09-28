/* Application framework: APPARC / EIKCORE / AVKON / CONE.
 * Guest classes derive from these; base state lives in zero-initialised guest memory (CBase::new
 * zero-fills) plus the few fields the game reads inline. */
#include "hle.h"
#include <stdlib.h>

uint32_t scheduler_create_default(CPU *c);
uint32_t g_export_new_application;

uint32_t g_coeenv, g_app, g_doc, g_appui;

/* CCoeControl (6.1): vptr, iCoeEnv@4, iContext@8, iPosition@12, iSize@20, iFlags@28, iWin@32, iObserver@36 */
#define CTL_COEENV 4
#define CTL_POS 12
#define CTL_SIZE 20
#define CTL_FLAGS 28
#define CTL_WIN 32

static void nop(CPU *c) { (void)c; }
static void ret0(CPU *c) { RET(0); }
static void ret_err_none(CPU *c) { RET(KErrNone); }
static void dtor_free(CPU *c) { if (ARG(c, 1) & 1) gfree(ARG(c, 0)); }

/* ---- CCoeEnv ---- */
static void CCoeEnv_Static(CPU *c) { RET(g_coeenv); }
/* MEikAppUiFactory slot 19 returns a pane control (status pane / CBA) that the game hides with
 * MakeVisible(EFalse) (slot 2) to go full screen; there is no such pane here. */
static uint32_t g_appui_factory, g_hidden_pane;
static void pane_nop(CPU *c) { (void)c; }
static void factory_pane(CPU *c) {
    if (!g_hidden_pane) {
        GuestFn v[3] = { 0, 0, pane_nop };
        g_hidden_pane = host_object("AppUiFactory pane", 64, v, 3);
    }
    RET(g_hidden_pane);
}
static void CEikonEnv_AppUiFactory(CPU *c) {
    if (!g_appui_factory) {
        GuestFn v[20] = { 0 };
        v[19] = factory_pane;
        g_appui_factory = host_object("MEikAppUiFactory", 64, v, 20);
    }
    RET(g_appui_factory);
}

/* ---- application / document / appui ---- */
static void CEikApplication_ctor(CPU *c) { (void)c; }
static void CEikDocument_ctor(CPU *c) { wr32(ARG(c, 0) + 8, ARG(c, 1)); /* iApplication */ }
static void CEikAppUi_ctor(CPU *c) { wr32(ARG(c, 0) + 4, g_coeenv); /* CCoeAppUi::iCoeEnv */ }
static void CAknAppUi_BaseConstructL(CPU *c) {
    LOG("CAknAppUi::BaseConstructL(flags=%08x)", ARG(c, 1));
}
static void CEikAppUi_ConstructL(CPU *c) { c->r[1] = 0; CAknAppUi_BaseConstructL(c); }
static void CEikApplication_CreateDocumentL(CPU *c) { vcall(c, ARG(c, 0), 12); }
static void CApaApplication_AppFullName(CPU *c) {
    /* TFileName returned via hidden pointer: r0 = result, r1 = this */
    static const char name[] = "e:\\system\\apps\\6r72\\6r72.app";
    uint32_t r = ARG(c, 0);
    wr32(r, (EBuf << 28) | (uint32_t)(sizeof name - 1));
    wr32(r + 4, 256);
    for (uint32_t i = 0; i < sizeof name - 1; i++) wr16(r + 8 + 2 * i, (uint8_t)name[i]);
    RET(r);
}

#define SCREEN_W 176
#define SCREEN_H 208

/* TRect CEikAppUi::ApplicationRect() const: r0 = result, r1 = this */
static void CEikAppUi_ApplicationRect(CPU *c) {
    uint32_t r = ARG(c, 0);
    wr32(r, 0); wr32(r + 4, 0); wr32(r + 8, SCREEN_W); wr32(r + 12, SCREEN_H);
    RET(r);
}

/* ---- CCoeControl ---- */
static void CCoeControl_ctor(CPU *c) { wr32(ARG(c, 0) + CTL_COEENV, g_coeenv); }
static void CCoeControl_dtor(CPU *c) { dtor_free(c); }
static void CCoeControl_CreateWindowL(CPU *c) {
    uint32_t w = gallocz(64);
    wr32(ARG(c, 0) + CTL_WIN, w);
}
static void CCoeControl_Window(CPU *c) { RET(rd32(ARG(c, 0) + CTL_WIN)); }
static void CCoeControl_SetContainerWindowL(CPU *c) {
    wr32(ARG(c, 0) + CTL_WIN, rd32(ARG(c, 1) + CTL_WIN));
}
static void CCoeControl_ActivateL(CPU *c) { wr32(ARG(c, 0) + CTL_FLAGS, rd32(ARG(c, 0) + CTL_FLAGS) | 1); }
/* TSize MinimumSize(): r0 = result, r1 = this */
static void CCoeControl_MinimumSize(CPU *c) {
    uint32_t r = ARG(c, 0), t = ARG(c, 1);
    wr32(r, rd32(t + CTL_SIZE)); wr32(r + 4, rd32(t + CTL_SIZE + 4));
    RET(r);
}
/* cone@318 = CCoeControl::SetRect(const TRect&) (N-Gage cone, identified from its call site). */
static void CCoeControl_SetRect(CPU *c) {
    uint32_t t = ARG(c, 0), r = ARG(c, 1);
    int32_t x0 = (int32_t)rd32(r), y0 = (int32_t)rd32(r + 4), x1 = (int32_t)rd32(r + 8), y1 = (int32_t)rd32(r + 12);
    wr32(t + CTL_POS, (uint32_t)x0); wr32(t + CTL_POS + 4, (uint32_t)y0);
    wr32(t + CTL_SIZE, (uint32_t)(x1 - x0)); wr32(t + CTL_SIZE + 4, (uint32_t)(y1 - y0));
    vcall(c, t, 20); /* SizeChanged */
}
static void ret_struct8_zero(CPU *c) { wr32(ARG(c, 0), 0); wr32(ARG(c, 0) + 4, 0); RET(ARG(c, 0)); }
static void CCoeControl_OfferKeyEventL(CPU *c) { RET(0); /* EKeyWasNotConsumed */ }

/* ---- control stack (keys) ---- */
#define MAX_STACK 32
static uint32_t ctl_stack[MAX_STACK];
static int ctl_stack_n;
static void CCoeAppUi_AddToStackL(CPU *c) {
    if (ctl_stack_n < MAX_STACK) ctl_stack[ctl_stack_n++] = ARG(c, 1);
}

static void CAknAppUi_dtor(CPU *c) { dtor_free(c); }
static void CAknAppUi_HandleError(CPU *c) {
    LOG("CAknAppUi::HandleError(%d)", (int32_t)ARG(c, 1));
    RET(0);
}

/* ---- window server session: the app owns window group 1, which always has focus ---- */
#define APP_WINDOW_GROUP 1
static void RWsSession_FindWindowGroupIdentifier(CPU *c) {
    RET(ARG(c, 1) == 0 ? APP_WINDOW_GROUP : (uint32_t)KErrNotFound);
}
static void RWsSession_GetFocusWindowGroup(CPU *c) { RET(APP_WINDOW_GROUP); }

/* CAknKeySoundSystem* CAknAppUi::KeySounds() const; key click sounds are not reproduced. */
static uint32_t g_keysounds;
static void CAknAppUi_KeySounds(CPU *c) {
    if (!g_keysounds) g_keysounds = gallocz(32);
    RET(g_keysounds);
}

/* ---- window server events ----
 * TWsEvent: iType@0, iHandle@4, iTime@8 (TTime), iEventData@16. TKeyEvent at iEventData:
 * iCode@16, iScanCode@20, iModifiers@24, iRepeats@28. The game's AppUi overrides HandleWsEventL
 * (slot 1) and reads scan codes of EEventKeyDown/EEventKeyUp. */
enum { EEventKey = 1, EEventKeyUp = 2, EEventKeyDown = 3, EEventFocusLost = 10, EEventFocusGained = 11 };
#define TWSEVENT_SIZE 48
static uint32_t g_wsevent;

static void deliver_ws_event(CPU *c, const uint32_t *a) {
    if (!g_appui) return;
    if (a[0] == EEventFocusGained || a[0] == EEventFocusLost) {
        uint32_t game = rd32(g_appui + 0x60);
        LOG("[%llu ms] ws: focus %s (game %08x, engine %08x)", (unsigned long long)(plat_now_us() / 1000 % 1000000), a[0] == EEventFocusGained ? "gained" : "lost",
            game, game ? rd32(game + 0x38) : 0);
    }
    memset(MEM + g_wsevent, 0, TWSEVENT_SIZE);
    wr32(g_wsevent, a[0]);
    wr32(g_wsevent + 4, 1); /* window group handle */
    wr32(g_wsevent + 16, a[2]);
    wr32(g_wsevent + 20, a[1]);
    uint32_t args[3] = { g_appui, g_wsevent, 0 };
    guest_vcall(c, 1, 3, args);
}

void kernel_lock(void);
void kernel_unlock(void);
int kernel_ready(void);

/* Called from the host UI thread. */
void input_key(int down, uint32_t scancode, uint32_t code) {
    if (!kernel_ready()) return;
    kernel_lock();
    uint32_t a[3] = { down ? EEventKeyDown : EEventKeyUp, scancode, 0 };
    post_callback(deliver_ws_event, 3, a);
    if (down && code) {
        uint32_t k[3] = { EEventKey, scancode, code };
        post_callback(deliver_ws_event, 3, k);
    }
    kernel_unlock();
}
/* Focus: the window server gives the app focus once it is visible. The engine starts its sound
 * only on a FocusGained received after its sound thread is up, and ignores repeated FocusGained,
 * so the initial one is delivered shortly after the first frame, and later host focus changes are
 * forwarded only as real transitions. */
enum { FOCUS_NONE, FOCUS_GAINED, FOCUS_LOST };
static int focus_state;
static uint32_t focus_ao;

static void focus_ao_runl(CPU *c) {
    if (focus_state == FOCUS_GAINED) return;
    focus_state = FOCUS_GAINED;
    uint32_t a[3] = { EEventFocusGained, 0, 0 };
    deliver_ws_event(c, a);
}

/* Debug (PTG_DEBUGSND): state of the game's sound engine/manager every 60 frames. */
static void debug_sound_state(void) {
    static int frames, on = -1;
    if (on < 0) on = getenv("PTG_DEBUGSND") != NULL;
    if (!on || ++frames % 60) return;
    uint32_t game = g_appui ? rd32(g_appui + 0x60) : 0, eng = game ? rd32(game + 0x38) : 0;
    uint32_t mgr = eng ? rd32(eng + 0x88) : 0;
    if (!eng) return;
    LOG("snd: eng req=%d 6c=%u 6d=%u 6e=%u 6f=%u a4=%u | mgr %08x vol=%d max=%d 444=%u 44c=%u stream=%08x",
        (int32_t)rd32(eng + 0x14), rd8(eng + 0x6c), rd8(eng + 0x6d), rd8(eng + 0x6e), rd8(eng + 0x6f), rd8(eng + 0xa4),
        mgr, mgr ? (int32_t)rd32(mgr + 0x43c) : 0, mgr ? (int32_t)rd32(mgr + 0x448) : 0,
        mgr ? rd8(mgr + 0x444) : 0, mgr ? rd8(mgr + 0x44c) : 0, mgr ? rd32(mgr + 0x1c) : 0);
}

/* Called on the main guest thread for every presented frame. */
void app_on_frame(void) {
    debug_sound_state();
    if (focus_ao) return;
    focus_ao = host_ao_new("InitialFocus", focus_ao_runl);
    host_ao_after(focus_ao, 500000);
}

void input_focus(int gained) {
    if (!kernel_ready()) return;
    kernel_lock();
    if ((gained && focus_state == FOCUS_LOST) || (!gained && focus_state == FOCUS_GAINED)) {
        focus_state = gained ? FOCUS_GAINED : FOCUS_LOST;
        uint32_t a[3] = { gained ? EEventFocusGained : EEventFocusLost, 0, 0 };
        post_callback(deliver_ws_event, 3, a);
    }
    kernel_unlock();
}

/* CEikAppUi::HandleWsEventL default: key routing to controls is not used by this game. */
static void CEikAppUi_HandleWsEventL(CPU *c) { (void)c; }

/* ---- boot ---- */
void callbacks_init(void);

void app_boot(CPU *c) {
    scheduler_create_default(c);
    callbacks_init();
    g_wsevent = gallocz(TWSEVENT_SIZE);
    g_coeenv = gallocz(0x1000);

    c->r[0] = 0;
    dispatch(c, g_export_new_application);
    g_app = c->r[0];
    LOG("app: NewApplication -> %08x", g_app);
    vcall(c, g_app, 1); /* PreDocConstructL */
    vcall(c, g_app, 12); /* CreateDocumentL() */
    g_doc = c->r[0];
    LOG("app: document %08x", g_doc);
    vcall(c, g_doc, 17); /* CreateAppUiL */
    g_appui = c->r[0];
    wr32(g_coeenv + 24, g_appui);
    LOG("app: appui %08x, calling ConstructL", g_appui);
    vcall(c, g_appui, 13); /* ConstructL */
    LOG("app: ConstructL returned, starting active scheduler");
    extern void CActiveScheduler_Start_entry(CPU *c);
    CActiveScheduler_Start_entry(c);
}

const HleEntry HLE_APP[] = {
    {"cone", "Static__7CCoeEnv", CCoeEnv_Static},
    {"eikcore", "AppUiFactory__C9CEikonEnv", CEikonEnv_AppUiFactory},
    {"eikcore", "__15CEikApplication", CEikApplication_ctor},
    {"eikcore", "_._15CEikApplication", dtor_free},
    {"eikcore", "__12CEikDocumentR15CEikApplication", CEikDocument_ctor},
    {"eikcore", "_._12CEikDocument", dtor_free},
    {"eikcore", "__9CEikAppUi", CEikAppUi_ctor},
    {"eikcore", "ConstructL__9CEikAppUi", CEikAppUi_ConstructL},
    {"eikcore", "CreateDocumentL__15CEikApplicationP11CApaProcess", CEikApplication_CreateDocumentL},
    {"eikcore", "ApplicationRect__C9CEikAppUi", CEikAppUi_ApplicationRect},
    {"apparc", "AppFullName__C15CApaApplication", CApaApplication_AppFullName},
    {"avkon", "BaseConstructL__CAknAppUii", CAknAppUi_BaseConstructL},
    {"avkon", "PreDocConstructL__CAknApplication", nop},
    {"avkon", "_._CAknAppUi", CAknAppUi_dtor},
    {"avkon", "HandleError__CAknAppUiiRCSExtendedErrorRTDes16T3", CAknAppUi_HandleError},
    {"avkon", "HandleForegroundEventL__CAknAppUii", nop},
    {"avkon", "HandleStatusPaneSizeChange__CAknAppUi", nop},
    {"avkon", "HandleSystemEventL__CAknAppUiRCTWsEvent", nop},
    {"avkon", "PrepareToExit__CAknAppUi", nop},
    {"avkon", "Reserved_MtsmObject__CAknAppUi", nop},
    {"avkon", "Reserved_MtsmPosition__CAknAppUi", nop},
    {"avkon", "SetKeyBlockMode__CAknAppUiTAknKeyBlockMode", nop},
    {"avkon", "KeySounds__CAknAppUi", CAknAppUi_KeySounds},
    {"avkon", "PushContextL__CAknKeySoundSystemi", nop},
    {"avkon", "PopContext__CAknKeySoundSystem", nop},
    {"cone", "__11CCoeControl", CCoeControl_ctor},
    {"cone", "_._11CCoeControl", CCoeControl_dtor},
    {"cone", "CreateWindowL__11CCoeControl", CCoeControl_CreateWindowL},
    {"cone", "Window__C11CCoeControl", CCoeControl_Window},
    {"cone", "SetContainerWindowL__11CCoeControlRC11CCoeControl", CCoeControl_SetContainerWindowL},
    {"cone", "ActivateL__11CCoeControl", CCoeControl_ActivateL},
    {"cone", "AddToStackL__9CCoeAppUiP11CCoeControlii", CCoeAppUi_AddToStackL},
    {"cone", "MinimumSize__11CCoeControl", CCoeControl_MinimumSize},
    {"cone", "@318", CCoeControl_SetRect},
    {"ws32", "FindWindowGroupIdentifier__10RWsSessioniG9TThreadId", RWsSession_FindWindowGroupIdentifier},
    {"ws32", "GetFocusWindowGroup__10RWsSession", RWsSession_GetFocusWindowGroup},
    {"eikcore", "HandleWsEventL__9CEikAppUiRC8TWsEventP11CCoeControl", CEikAppUi_HandleWsEventL},
    {"cone", "InputCapabilities__C11CCoeControl", ret_struct8_zero},
    {"cone", "InputCapabilities__C9CCoeAppUi", ret_struct8_zero},
    {"cone", "CountComponentControls__C11CCoeControl", ret0},
    {"cone", "ComponentControl__C11CCoeControli", ret0},
    {"cone", "HasBorder__C11CCoeControl", ret0},
    {"cone", "Draw__C11CCoeControlRC5TRect", nop},
    {"cone", "SizeChanged__11CCoeControl", nop},
    {"cone", "PositionChanged__11CCoeControl", nop},
    {"cone", "FocusChanged__11CCoeControl8TDrawNow", nop},
    {"cone", "MakeVisible__11CCoeControli", nop},
    {"cone", "SetDimmed__11CCoeControli", nop},
    {"cone", "SetAdjacent__11CCoeControli", nop},
    {"cone", "SetNeighbor__11CCoeControlP11CCoeControl", nop},
    {"cone", "HandleResourceChange__11CCoeControli", nop},
    {"cone", "PrepareForFocusLossL__11CCoeControl", nop},
    {"cone", "PrepareForFocusGainL__11CCoeControl", nop},
    {"cone", "HandlePointerEventL__11CCoeControlRC13TPointerEvent", nop},
    {"cone", "HandlePointerBufferReadyL__11CCoeControl", nop},
    {"cone", "GetColorUseListL__C11CCoeControlRt9CArrayFix1Z12TCoeColorUse", nop},
    {"cone", "GetHelpContext__C11CCoeControlR15TCoeHelpContext", nop},
    {"cone", "WriteInternalStateL__C11CCoeControlR12RWriteStream", nop},
    {"cone", "Reserved_2__11CCoeControl", nop},
    {"cone", "ConstructFromResourceL__11CCoeControlR15TResourceReader", nop},
    {"cone", "SetAndDrawFocus__9CCoeAppUii", nop},
    {"cone", "HelpContextL__C9CCoeAppUi", ret0},
    {"cone", "HandleSwitchOnEventL__9CCoeAppUiP11CCoeControl", nop},
    {"cone", "MCoeMessageObserver_Reserved_1__19MCoeMessageObserver", nop},
    {"cone", "MCoeMessageObserver_Reserved_2__19MCoeMessageObserver", nop},
    {"cone", "MCoeViewDeactivationObserver_Reserved_1__28MCoeViewDeactivationObserver", nop},
    {"cone", "MCoeViewDeactivationObserver_Reserved_2__28MCoeViewDeactivationObserver", nop},
    {"eikcore", "HandleModelChangeL__9CEikAppUi", nop},
    {"eikcore", "HandleCommandL__9CEikAppUii", nop},
    {"eikcore", "HandleResourceChangeL__9CEikAppUii", nop},
    {"eikcore", "Reserved_1__9CEikAppUi", nop},
    {"eikcore", "Reserved_2__9CEikAppUi", nop},
    {"eikcore", "Reserved_3__9CEikAppUi", nop},
    {"eikcore", "Reserved_4__9CEikAppUi", nop},
    {"eikcore", "Reserved_1__12CEikDocument", nop},
    {"eikcore", "Reserved_2__12CEikDocument", nop},
    {"eikcore", "Reserved_1__15CEikApplication", nop},
    {"eikcore", "StopDisplayingMenuBar__9CEikAppUi", nop},
    {"eikcore", "IsEmpty__C12CEikDocument", ret0},
    {"eikcore", "HasChanged__C12CEikDocument", ret0},
    {"eikcore", "NewDocumentL__12CEikDocument", nop},
    {"eikcore", "SaveL__12CEikDocument", nop},
    {"eikcore", "Reserved_1_MenuObserver__16MEikMenuObserver", nop},
    {"eikcore", "CancelTrigger__13EikBubbleHelp", nop},
    {0, 0, 0},
};
