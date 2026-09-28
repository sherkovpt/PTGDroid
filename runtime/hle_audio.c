/* MEDIACLIENTAUDIO / MEDIACLIENTAUDIOSTREAM.
 *
 * CMdaAudioRecorderUtility is used by the game only for multiplayer voice capture (OpenL on a
 * descriptor, SetPosition(0), CropL, RecordL). There is no microphone on the port: OpenL reports
 * KErrNotSupported through the observer, which makes the game leave voice capture off.
 * Slots identified from call sites: 4 RecordL, 6 CropL, 7 SetPosition, 21 OpenL,
 * 22 SetAudioDeviceMode, 24 MaxVolume, 26 SetVolume. */
#include "hle.h"
#include <stdlib.h>

enum { ENotReady = 0, EOpen = 1, EPlaying = 2, ERecording = 3 };

static uint32_t g_recorder, g_recorder_observer;

/* MMdaObjectStateChangeObserver::MoscoStateChangeEvent(CBase*, TInt aPrev, TInt aCur, TInt aErr): slot 0 */
static void recorder_notify(CPU *c, const uint32_t *a) {
    uint32_t args[5] = { g_recorder_observer, g_recorder, a[0], a[1], a[2] };
    guest_vcall(c, 0, 5, args);
}

static void recorder_openl(CPU *c) {
    uint32_t a[3] = { ENotReady, ENotReady, (uint32_t)KErrNotSupported };
    (void)c;
    post_callback(recorder_notify, 3, a);
}
static void recorder_nop(CPU *c) { (void)c; }
static void recorder_state(CPU *c) { RET(ENotReady); }
static void recorder_maxvolume(CPU *c) { RET(10); }
static void recorder_dtor(CPU *c) { if (ARG(c, 1) & 1) gfree(ARG(c, 0)); }

/* CMdaAudioRecorderUtility::NewL(MMdaObjectStateChangeObserver&, CMdaServer*, TInt, TMdaPriorityPreference) */
static void CMdaAudioRecorderUtility_NewL(CPU *c) {
    GuestFn v[27] = { 0 };
    v[0] = recorder_dtor;
    v[1] = recorder_state;
    v[2] = recorder_nop;  /* Close */
    v[5] = recorder_nop;  /* Stop */
    v[7] = recorder_nop;  /* SetPosition */
    v[21] = recorder_openl;
    v[22] = recorder_nop; /* SetAudioDeviceMode */
    v[24] = recorder_maxvolume;
    v[26] = recorder_nop; /* SetVolume */
    g_recorder_observer = ARG(c, 0);
    g_recorder = host_object("CMdaAudioRecorderUtility", 64, v, 27);
    RET(g_recorder);
}

/* ---- CMdaAudioOutputStream ----
 * Virtual slots: 0 dtor, 1 SetAudioPropertiesL(rate caps, channel caps), 2 Open(TMdaPackage*),
 * 3 MaxVolume, 4 Volume, 5 SetVolume, 6 SetPriority, 7 WriteL(const TDesC8&), 8 Stop, 9 Position.
 * Observer (MMdaAudioOutputStreamCallback): 0 MaoscOpenComplete(err), 1 MaoscBufferCopied(err, des),
 * 2 MaoscPlayComplete(err). PCM is 16-bit signed little endian.
 * Pacing: MaoscBufferCopied is sent once the host queue is below ~120 ms, so the game produces
 * audio in real time. Callbacks are delivered on the thread that created the stream. */
#include "plat.h"

AudioBackend g_audio;

#define MAX_VOLUME 256
#define MAX_PENDING 32
#define KErrAbort (-39)

typedef struct {
    uint32_t obj, observer, pump_ao;
    int tid, rate, channels, volume, dev, open;
    uint32_t pending[MAX_PENDING];
    int npending;
    int peak; /* max |sample| since the last throughput log */
    /* headless clock */
    uint64_t sim_until_us;
} Stream;
static Stream streams[4];

static Stream *stream_of(uint32_t obj) {
    for (int i = 0; i < 4; i++) if (streams[i].obj == obj) return &streams[i];
    return NULL;
}

static int rate_from_caps(uint32_t caps) {
    static const int rates[] = { 8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000, 96000, 64000 };
    for (int i = 0; i < 11; i++) if (caps & (0x10u << i)) return rates[i];
    return 8000;
}
static int channels_from_caps(uint32_t caps) { return (caps & 0x04000000u) ? 2 : 1; }

static uint32_t bytes_per_sec(Stream *s) { return (uint32_t)(s->rate * s->channels * 2); }

static uint32_t queued(Stream *s) {
    if (g_audio.queued_bytes && s->dev > 0) return g_audio.queued_bytes(s->dev);
    uint64_t now = plat_now_us();
    return s->sim_until_us > now ? (uint32_t)((s->sim_until_us - now) * bytes_per_sec(s) / 1000000) : 0;
}

static void device_open(Stream *s) {
    if (g_audio.close && s->dev > 0) g_audio.close(s->dev);
    s->dev = g_audio.open ? g_audio.open(s->rate, s->channels) : 0;
    LOG("audio: output stream %d Hz, %d ch (device %d)", s->rate, s->channels, s->dev);
}

static void notify(CPU *c, const uint32_t *a) {
    uint32_t args[4] = { a[0], a[2], a[3], 0 };
    guest_vcall(c, (int)a[1], a[1] == 1 ? 3 : 2, args);
}
static void post_notify(Stream *s, int slot, int32_t err, uint32_t des) {
    uint32_t a[4] = { s->observer, (uint32_t)slot, (uint32_t)err, des };
    post_callback_to(s->tid, notify, 4, a);
}

static void pump(Stream *s) {
    uint32_t target = bytes_per_sec(s) * 120 / 1000;
    while (s->npending && queued(s) <= target) {
        uint32_t des = s->pending[0];
        memmove(s->pending, s->pending + 1, sizeof(uint32_t) * (size_t)--s->npending);
        post_notify(s, 1, KErrNone, des);
    }
    if (s->npending) host_ao_after(s->pump_ao, 10000);
}
static void pump_runl(CPU *c) {
    for (int i = 0; i < 4; i++) if (streams[i].pump_ao == ARG(c, 0)) pump(&streams[i]);
}

static void os_dtor(CPU *c) {
    Stream *s = stream_of(ARG(c, 0));
    if (s) {
        if (g_audio.close && s->dev > 0) g_audio.close(s->dev);
        s->obj = 0;
    }
    if (ARG(c, 1) & 1) gfree(ARG(c, 0));
}
static void os_set_properties(CPU *c) {
    Stream *s = stream_of(ARG(c, 0));
    LOG("audio: SetAudioPropertiesL(rate caps %08x, channel caps %08x)", ARG(c, 1), ARG(c, 2));
    s->rate = rate_from_caps(ARG(c, 1));
    s->channels = channels_from_caps(ARG(c, 2));
    device_open(s);
}
static void os_open(CPU *c) {
    Stream *s = stream_of(ARG(c, 0));
    uint32_t pkg = ARG(c, 1);
    uint32_t rate = pkg ? rd32(pkg + 0x1c) : 0, ch = pkg ? rd32(pkg + 0x20) : 0;
    s->rate = rate ? rate_from_caps(rate) : 8000;
    s->channels = ch ? channels_from_caps(ch) : 1;
    device_open(s);
    s->open = 1;
    post_notify(s, 0, KErrNone, 0);
}
static void os_maxvolume(CPU *c) { RET(MAX_VOLUME); }
static void os_volume(CPU *c) { RET(stream_of(ARG(c, 0))->volume); }
static void os_setvolume(CPU *c) {
    int v = (int32_t)ARG(c, 1);
    stream_of(ARG(c, 0))->volume = v < 0 ? 0 : (v > MAX_VOLUME ? MAX_VOLUME : v);
}
static void os_nop(CPU *c) { (void)c; }

static void os_writel(CPU *c) {
    Stream *s = stream_of(ARG(c, 0));
    uint32_t des = ARG(c, 1), len = des_len(des) & ~1u, p = des_ptr(des);
    if (s->npending == MAX_PENDING) trap_leave(c, KErrOverflow);
    static int16_t buf[32768];
    for (uint32_t off = 0; off < len; off += sizeof buf) {
        uint32_t n = len - off < sizeof buf ? len - off : (uint32_t)sizeof buf;
        for (uint32_t i = 0; i < n / 2; i++) {
            buf[i] = (int16_t)(((int16_t)rd16(p + off + 2 * i) * s->volume) / MAX_VOLUME);
            int a = buf[i] < 0 ? -buf[i] : buf[i];
            if (a > s->peak) s->peak = a;
        }
        if (g_audio.queue && s->dev > 0) g_audio.queue(s->dev, buf, n);
    }
    if (!g_audio.queue || s->dev <= 0) {
        uint64_t now = plat_now_us(), start = s->sim_until_us > now ? s->sim_until_us : now;
        s->sim_until_us = start + (uint64_t)len * 1000000 / bytes_per_sec(s);
    }
    s->pending[s->npending++] = des;
    static uint64_t t0, total;
    static int debug = -1;
    if (debug < 0) debug = getenv("PTG_DEBUGSND") != NULL;
    total += len;
    if (!t0) t0 = plat_now_us();
    if (debug && plat_now_us() - t0 > 1000000) {
        LOG("audio: game wrote %llu bytes/s (buffer %u bytes), peak %d, volume %d",
            (unsigned long long)(total * 1000000 / (plat_now_us() - t0)), len, s->peak, s->volume);
        s->peak = 0;
        t0 = plat_now_us();
        total = 0;
    }
    pump(s);
}

static void os_stop(CPU *c) {
    Stream *s = stream_of(ARG(c, 0));
    if (g_audio.clear && s->dev > 0) g_audio.clear(s->dev);
    s->sim_until_us = 0;
    for (int i = 0; i < s->npending; i++) post_notify(s, 1, KErrAbort, s->pending[i]);
    s->npending = 0;
    post_notify(s, 2, KErrCancel, 0);
}

static uint32_t g_position;
static void os_position(CPU *c) {
    if (!g_position) g_position = gallocz(8);
    RET(g_position);
}

/* mediaclientaudiostream@2 = CMdaAudioOutputStream::NewL(MMdaAudioOutputStreamCallback&, CMdaServer*)
 * (EKA2L1's export list names this ordinal wrongly; identified from the call site). */
static void CMdaAudioOutputStream_NewL(CPU *c) {
    Stream *s = stream_of(0);
    if (!s) trap_leave(c, KErrNoMemory);
    GuestFn v[10] = { os_dtor, os_set_properties, os_open, os_maxvolume, os_volume, os_setvolume,
                      os_nop, os_writel, os_stop, os_position };
    callbacks_init();
    memset(s, 0, sizeof *s);
    s->obj = host_object("CMdaAudioOutputStream", 32, v, 10);
    s->observer = ARG(c, 0);
    s->tid = kernel_thread_id();
    s->rate = 8000;
    s->channels = 1;
    s->volume = MAX_VOLUME;
    s->pump_ao = host_ao_new("AudioPump", pump_runl);
    RET(s->obj);
}

const HleEntry HLE_AUDIO[] = {
    {"mediaclientaudiostream", "CMdaAudioOutputStreamPadFunction__Fv", CMdaAudioOutputStream_NewL},
    {"mediaclientaudio", "NewL__24CMdaAudioRecorderUtilityR29MMdaObjectStateChangeObserverP10CMdaServeri22TMdaPriorityPreference",
     CMdaAudioRecorderUtility_NewL},
    {0, 0, 0},
};
