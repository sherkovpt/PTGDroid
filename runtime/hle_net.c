/* ESOCK / BLUETOOTH / BTMANCLIENT / SYSAGT: multiplayer and phone-state services.
 * There is no Bluetooth link on the port: sessions connect, but sockets, resolvers and security
 * registration fail with KErrNotSupported (async requests complete immediately with it).
 * System agent reports an idle phone; event notifications stay pending until cancelled. */
#include "hle.h"

static void nop(CPU *c) { (void)c; }
static void ret_ok(CPU *c) { RET(KErrNone); }
static void ret_unsupported(CPU *c) { RET(KErrNotSupported); }
static void ret_notfound(CPU *c) { RET(KErrNotFound); }
static void zero_handle(CPU *c) { wr32(ARG(c, 0), 0); }

/* Complete the TRequestStatus passed as argument `i` with KErrNotSupported. */
static void fail_async(CPU *c, int i) { request_complete(cur_thread(), ARG(c, i), KErrNotSupported); }
static void fail_async1(CPU *c) { fail_async(c, 1); }
static void fail_async2(CPU *c) { fail_async(c, 2); }
static void fail_async3(CPU *c) { fail_async(c, 3); }
static void fail_async4(CPU *c) { fail_async(c, 4); }

/* TSockAddr : TBuf8<32>; the first 8 bytes hold family and port. */
static void TSockAddr_ctor(CPU *c) {
    uint32_t t = ARG(c, 0);
    wr32(t, (EBuf << 28) | 8);
    wr32(t + 4, 32);
    memset(MEM + t + 8, 0, 32);
}
static void TSockAddr_copy(CPU *c) { memmove(MEM + ARG(c, 0), MEM + ARG(c, 1), 40); }
static void TSockAddr_SetPort(CPU *c) { wr32(ARG(c, 0) + 12, ARG(c, 1)); }
/* TBTDevAddr TBTSockAddr::BTAddr() const: 6-byte object via hidden return pointer */
static void TBTSockAddr_BTAddr(CPU *c) { memset(MEM + ARG(c, 0), 0, 6); RET(ARG(c, 0)); }

/* TSysAgentEvent { TUid iUid; TInt iState; TRequestStatus* iStatus; } */
static void TSysAgentEvent_ctor(CPU *c) { memset(MEM + ARG(c, 0), 0, 12); }
static void TSysAgentEvent_SetUid(CPU *c) { wr32(ARG(c, 0), ARG(c, 1)); }
static void TSysAgentEvent_SetRequestStatus(CPU *c) { wr32(ARG(c, 0) + 8, ARG(c, 1)); }
static void TSysAgentEvent_State(CPU *c) { RET(rd32(ARG(c, 0) + 4)); }

static uint32_t sysagt_pending; /* TRequestStatus* of the outstanding notification */
static void RSystemAgent_NotifyOnEvent(CPU *c) {
    uint32_t st = rd32(ARG(c, 1) + 8);
    if (st) { wr32(st, KRequestPending); sysagt_pending = st; }
}
static void RSystemAgent_NotifyEventCancel(CPU *c) {
    (void)c;
    if (sysagt_pending) { request_complete(cur_thread(), sysagt_pending, KErrCancel); sysagt_pending = 0; }
}
static void RSystemAgent_GetState(CPU *c) {
    LOG("RSystemAgent::GetState(uid %08x) -> 0", ARG(c, 1));
    RET(0);
}

/* gamecomms@27: NewL of the Bluetooth game-session object. The game calls it under a TRAP to probe
 * whether multiplayer is available; leaving makes it take the "not available" path. */
static void GameComms_NewL(CPU *c) { trap_leave(c, KErrNotSupported); }

/* gameutils@19 creates a watcher over an S60 shared-data setting; gameutils@17 asks it whether the
 * game should be interrupted (the game pauses when it returns non-zero). Nothing interrupts here. */
static void GameUtils_dtor(CPU *c) { if (ARG(c, 1) & 1) gfree(ARG(c, 0)); }
static void GameUtils_NewL(CPU *c) {
    GuestFn v[] = { GameUtils_dtor };
    RET(host_object("GameUtils watcher", 32, v, 1));
}
static void GameUtils_ShouldInterrupt(CPU *c) { RET(0); }

const HleEntry HLE_NET[] = {
    {"gamecomms", "@27", GameComms_NewL},
    {"gameutils", "@19", GameUtils_NewL},
    {"gameutils", "@17", GameUtils_ShouldInterrupt},
    {"esock", "__11RSocketServ", zero_handle},
    {"esock", "__7RSocket", zero_handle},
    {"esock", "__9TSockAddr", TSockAddr_ctor},
    {"esock", "Connect__11RSocketServUi", ret_ok},
    {"esock", "FindProtocol__11RSocketServRCt4TBuf1i32R13TProtocolDesc", ret_notfound},
    {"esock", "Open__7RSocketR11RSocketServ", ret_unsupported},
    {"esock", "Open__7RSocketR11RSocketServRC7TDesC16", ret_unsupported},
    {"esock", "Open__7RSocketR11RSocketServUiUiUi", ret_unsupported},
    {"esock", "Open__13RHostResolverR11RSocketServUiUi", ret_unsupported},
    {"esock", "Close__7RSocket", zero_handle},
    {"esock", "Close__13RHostResolver", zero_handle},
    {"esock", "Cancel__13RHostResolver", nop},
    {"esock", "CancelAccept__7RSocket", nop},
    {"esock", "CancelAll__7RSocket", nop},
    {"esock", "CancelRecv__7RSocket", nop},
    {"esock", "CancelWrite__7RSocket", nop},
    {"esock", "Bind__7RSocketR9TSockAddr", ret_unsupported},
    {"esock", "Listen__7RSocketUi", ret_unsupported},
    {"esock", "SetLocalPort__7RSocketi", ret_unsupported},
    {"esock", "GetOpt__7RSocketUiUiR5TDes8", ret_unsupported},
    {"esock", "RemoteName__7RSocketR9TSockAddr", nop},
    {"esock", "GetHostName__13RHostResolverR6TDes16", ret_unsupported},
    {"esock", "SetPort__9TSockAddrUi", TSockAddr_SetPort},
    {"esock", "Accept__7RSocketR7RSocketR14TRequestStatus", fail_async2},
    {"esock", "Connect__7RSocketR9TSockAddrR14TRequestStatus", fail_async2},
    {"esock", "Read__7RSocketR5TDes8R14TRequestStatus", fail_async2},
    {"esock", "Write__7RSocketRC6TDesC8R14TRequestStatus", fail_async2},
    {"esock", "RecvOneOrMore__7RSocketR5TDes8UiR14TRequestStatusRt8TPckgBuf1Zi", fail_async3},
    {"esock", "GetByAddress__13RHostResolverRC9TSockAddrRt8TPckgBuf1Z11TNameRecordR14TRequestStatus", fail_async3},
    {"esock", "Next__13RHostResolverRt8TPckgBuf1Z11TNameRecordR14TRequestStatus", fail_async2},

    {"bluetooth", "__11TBTSockAddr", TSockAddr_ctor},
    {"bluetooth", "__11TBTSockAddrRC9TSockAddr", TSockAddr_copy},
    {"bluetooth", "__16TInquirySockAddr", TSockAddr_ctor},
    {"bluetooth", "BTAddr__C11TBTSockAddr", TBTSockAddr_BTAddr},

    {"btmanclient", "__6RBTMan", zero_handle},
    {"btmanclient", "Connect__6RBTMan", ret_unsupported},
    {"btmanclient", "Open__19RBTSecuritySettingsR6RBTMan", ret_unsupported},
    {"btmanclient", "Close__19RBTSecuritySettings", nop},
    {"btmanclient", "RegisterService__19RBTSecuritySettingsRC18TBTServiceSecurityR14TRequestStatus", fail_async2},
    {"btmanclient", "__18TBTServiceSecurity", nop},
    {"btmanclient", "__18TBTServiceSecurityG4TUidii", nop},
    {"btmanclient", "SetAuthentication__18TBTServiceSecurityi", nop},
    {"btmanclient", "SetAuthorisation__18TBTServiceSecurityi", nop},
    {"btmanclient", "SetEncryption__18TBTServiceSecurityi", nop},

    {"sysagt", "__12RSystemAgent", zero_handle},
    {"sysagt", "Connect__16RSystemAgentBase", ret_ok},
    {"sysagt", "GetState__12RSystemAgentG4TUid", RSystemAgent_GetState},
    {"sysagt", "NotifyOnEvent__12RSystemAgentR14TSysAgentEvent", RSystemAgent_NotifyOnEvent},
    {"sysagt", "NotifyEventCancel__12RSystemAgent", RSystemAgent_NotifyEventCancel},
    {"sysagt", "__14TSysAgentEvent", TSysAgentEvent_ctor},
    {"sysagt", "SetUid__14TSysAgentEventG4TUid", TSysAgentEvent_SetUid},
    {"sysagt", "SetRequestStatus__14TSysAgentEventR14TRequestStatus", TSysAgentEvent_SetRequestStatus},
    {"sysagt", "State__14TSysAgentEvent", TSysAgentEvent_State},
    {0, 0, 0},
};
