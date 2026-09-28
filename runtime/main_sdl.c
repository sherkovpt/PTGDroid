/* SDL2 front end: window/presentation and input on the host main thread, the game on its own
 * thread. Shared by the Windows build and Android (where SDL provides the Activity). */
#ifndef __ANDROID__
#define SDL_MAIN_HANDLED
#endif
#include <SDL.h>
#include "hle.h"
#include "touch.h"
#include "menu.h"
#include "video.h"
#include <stdlib.h>
#include <math.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif
#ifdef _WIN32
#include <windows.h>
#include <timeapi.h>
#endif

#define W 176
#define H 208

extern uint32_t g_export_new_application;
extern void (*g_display_submit)(const uint8_t *fb, int w, int h);
void app_boot(CPU *c);
void install_crash_handler(void);
void input_key(int down, uint32_t scancode, uint32_t code);
void input_focus(int gained);

static SDL_mutex *fb_lock;
static uint16_t fb_copy[W * H];
static int fb_dirty;

/* Test hook: PTG_DUMP=N saves every Nth frame as dump_<frame>.bmp */
static int dump_every, frame_no;
static void dump_frame(const uint16_t *px) {
    char path[64];
    snprintf(path, sizeof path, "dump_%06d.bmp", frame_no);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    uint8_t hdr[54] = { 'B', 'M' };
    uint32_t size = 54 + W * H * 3;
    int32_t w = W, h = -H;
    memcpy(hdr + 2, &size, 4);
    hdr[10] = 54; hdr[14] = 40;
    memcpy(hdr + 18, &w, 4); memcpy(hdr + 22, &h, 4);
    hdr[26] = 1; hdr[28] = 24;
    fwrite(hdr, 1, 54, f);
    for (int i = 0; i < W * H; i++) {
        uint16_t p = px[i];
        uint8_t bgr[3] = { (uint8_t)((p & 0xF) * 17), (uint8_t)(((p >> 4) & 0xF) * 17), (uint8_t)(((p >> 8) & 0xF) * 17) };
        fwrite(bgr, 1, 3, f);
    }
    fclose(f);
}

static void submit(const uint8_t *fb, int w, int h) {
    SDL_LockMutex(fb_lock);
    memcpy(fb_copy, fb, (size_t)w * h * 2);
    fb_dirty = 1;
    frame_no++;
    if (dump_every && frame_no % dump_every == 0) dump_frame(fb_copy);
    SDL_UnlockMutex(fb_lock);
}

/* ---- audio backend (queue mode) ---- */
static int sdl_audio_open(int rate, int channels) {
    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = rate;
    want.format = AUDIO_S16LSB;
    want.channels = (Uint8)channels;
    want.samples = 1024;
    SDL_AudioDeviceID dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!dev) { LOG("SDL_OpenAudioDevice: %s", SDL_GetError()); return 0; }
    SDL_PauseAudioDevice(dev, 0);
    return (int)dev;
}
static void sdl_audio_queue(int dev, const int16_t *pcm, uint32_t bytes) { SDL_QueueAudio((SDL_AudioDeviceID)dev, pcm, bytes); }
static uint32_t sdl_audio_queued(int dev) { return SDL_GetQueuedAudioSize((SDL_AudioDeviceID)dev); }
static void sdl_audio_clear(int dev) { SDL_ClearQueuedAudio((SDL_AudioDeviceID)dev); }
static void sdl_audio_close(int dev) { SDL_CloseAudioDevice((SDL_AudioDeviceID)dev); }

static int game_thread(void *p) {
    (void)p;
    static CPU cpu;
    kernel_init(&cpu);
    g_export_new_application = g_image_export1;
    app_boot(&cpu);
    LOG("game thread finished");
    return 0;
}

/* ---- key mapping: N-Gage scan codes (TStdScanCode) and key codes (TKeyCode) ---- */
typedef struct { uint32_t scan, code; } NKey;
enum {
    SC_C = 0x01, SC_LEFT = 0x0E, SC_RIGHT = 0x0F, SC_UP = 0x10, SC_DOWN = 0x11,
    SC_STAR = 0x2A, SC_HASH = 0x7F, SC_SOFT_L = 0xA4, SC_SOFT_R = 0xA5, SC_SELECT = 0xA7,
};
static const NKey K_UP = { SC_UP, 0xF809 }, K_DOWN = { SC_DOWN, 0xF80A }, K_LEFT = { SC_LEFT, 0xF807 },
                  K_RIGHT = { SC_RIGHT, 0xF808 }, K_SELECT = { SC_SELECT, 0xF845 },
                  K_SOFT_L = { SC_SOFT_L, 0xF842 }, K_SOFT_R = { SC_SOFT_R, 0xF843 },
                  K_STAR = { SC_STAR, '*' }, K_HASH = { SC_HASH, '#' }, K_C = { SC_C, 8 }, K_NONE = { 0, 0 };
static NKey digit(int d) { NKey k = { 0x30u + (uint32_t)d, '0' + (uint32_t)d }; return k; }

static NKey map_key(SDL_Scancode s) {
    switch (s) {
    case SDL_SCANCODE_UP: return K_UP;
    case SDL_SCANCODE_DOWN: return K_DOWN;
    case SDL_SCANCODE_LEFT: return K_LEFT;
    case SDL_SCANCODE_RIGHT: return K_RIGHT;
    case SDL_SCANCODE_RETURN: case SDL_SCANCODE_SPACE: case SDL_SCANCODE_KP_ENTER: return K_SELECT;
    case SDL_SCANCODE_F1: case SDL_SCANCODE_Q: return K_SOFT_L;
    case SDL_SCANCODE_F2: case SDL_SCANCODE_W: return K_SOFT_R;
    case SDL_SCANCODE_KP_MULTIPLY: case SDL_SCANCODE_E: return K_STAR;
    case SDL_SCANCODE_KP_PERIOD: case SDL_SCANCODE_R: return K_HASH;
    case SDL_SCANCODE_BACKSPACE: case SDL_SCANCODE_DELETE: return K_C;
    case SDL_SCANCODE_0: case SDL_SCANCODE_KP_0: return digit(0);
    default: break;
    }
    if (s >= SDL_SCANCODE_1 && s <= SDL_SCANCODE_9) return digit(1 + (int)(s - SDL_SCANCODE_1));
    if (s >= SDL_SCANCODE_KP_1 && s <= SDL_SCANCODE_KP_9) return digit(1 + (int)(s - SDL_SCANCODE_KP_1));
    return K_NONE;
}

/* Gamepad: d-pad = arrows, A = select, B = C, X = 5, Y = 7, Start = left soft key, Back = right. */
static NKey map_button(Uint8 b) {
    switch (b) {
    case SDL_CONTROLLER_BUTTON_DPAD_UP: return K_UP;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return K_DOWN;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return K_LEFT;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return K_RIGHT;
    case SDL_CONTROLLER_BUTTON_A: return K_SELECT;
    case SDL_CONTROLLER_BUTTON_B: return K_C;
    case SDL_CONTROLLER_BUTTON_X: return digit(5);
    case SDL_CONTROLLER_BUTTON_Y: return digit(7);
    case SDL_CONTROLLER_BUTTON_START: return K_SOFT_L;
    case SDL_CONTROLLER_BUTTON_BACK: return K_SOFT_R;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return K_STAR;
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return K_HASH;
    default: return K_NONE;
    }
}

static void send_nkey(NKey k, int down) {
    if (k.scan) input_key(down, k.scan, k.code);
}
static void touch_send(uint32_t scan, uint32_t code, int down) { input_key(down, scan, code); }

/* Test hook: PTG_SCRIPT="ms:key,ms:key,..." taps keys (120 ms) at times since start.
 * Keys: up down left right select softl softr star hash c 0-9; "blur"/"focus" change window focus. */
typedef struct { uint32_t at; NKey key; int released; int focus; /* 1 gain, 2 lose, 3 menu, 4 resume */ } Tap;
static int script_menu_request; /* 1 open pause menu, 2 close it */
static Tap taps[64];
static int ntaps;
static void parse_script(const char *s) {
    while (s && *s && ntaps < 64) {
        unsigned ms;
        char name[16];
        if (sscanf(s, "%u:%15[a-z0-9]", &ms, name) != 2) break;
        NKey k = K_NONE;
        if (!strcmp(name, "up")) k = K_UP; else if (!strcmp(name, "down")) k = K_DOWN;
        else if (!strcmp(name, "left")) k = K_LEFT; else if (!strcmp(name, "right")) k = K_RIGHT;
        else if (!strcmp(name, "select")) k = K_SELECT; else if (!strcmp(name, "softl")) k = K_SOFT_L;
        else if (!strcmp(name, "softr")) k = K_SOFT_R; else if (!strcmp(name, "star")) k = K_STAR;
        else if (!strcmp(name, "hash")) k = K_HASH; else if (!strcmp(name, "c")) k = K_C;
        else if (name[0] >= '0' && name[0] <= '9' && !name[1]) k = digit(name[0] - '0');
        int focus = !strcmp(name, "focus") ? 1 : !strcmp(name, "blur") ? 2 : !strcmp(name, "menu") ? 3 : !strcmp(name, "resume") ? 4 : 0;
        taps[ntaps++] = (Tap){ ms, k, 0, focus };
        s = strchr(s, ',');
        if (s) s++;
    }
}
static void run_script(uint32_t now) {
    for (int i = 0; i < ntaps; i++) {
        if (taps[i].focus && !taps[i].released && now >= taps[i].at) {
            LOG("script: action %d", taps[i].focus);
            if (taps[i].focus >= 3) script_menu_request = taps[i].focus - 2;
            else input_focus(taps[i].focus == 1);
            taps[i].released = 2;
            continue;
        }
        if (!taps[i].released && now >= taps[i].at && taps[i].at != UINT32_MAX) {
            LOG("script: key %02x down", taps[i].key.scan);
            send_nkey(taps[i].key, 1);
            taps[i].at = now + 120;
            taps[i].released = 1;
        } else if (taps[i].released == 1 && now >= taps[i].at) {
            send_nkey(taps[i].key, 0);
            taps[i].released = 2;
        }
    }
}

int main(int argc, char **argv) {
#ifndef __ANDROID__
    SDL_SetMainReady();
#endif
#ifdef __ANDROID__
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1"); /* back opens the menu */
#endif
#ifdef _WIN32
    timeBeginPeriod(1); /* 1 ms timer resolution: the game paces itself with short RTimer waits */
#endif
    if (argc < 2) {
        LOG("usage: %s <path to system/apps/6r72/6r72.app> [writable dir for drive C:]", argv[0]);
        return 1;
    }
    extern char g_c_root[512];
    if (argc > 2) snprintf(g_c_root, sizeof g_c_root, "%s", argv[2]);
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS)) {
        LOG("SDL_Init: %s", SDL_GetError());
        return 1;
    }
    install_crash_handler();
    set_card_root_from_app(argv[1]);
    mem_init();
    if (!load_image(argv[1])) { LOG("cannot load %s", argv[1]); return 1; }
    hle_bind();

#ifdef __ANDROID__
    /* immersive (no status/navigation bars); resizable + hint so SDL keeps all orientations */
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "Portrait PortraitUpsideDown LandscapeLeft LandscapeRight");
    Uint32 win_flags = SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
#else
    Uint32 win_flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
#endif
    SDL_Window *win = SDL_CreateWindow("PTGDroid", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       W * 3, H * 3, win_flags);
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC |
                                                    SDL_RENDERER_TARGETTEXTURE);
    if (!win || !ren) { LOG("SDL window/renderer: %s", SDL_GetError()); return 1; }
    video_init(ren);

    fb_lock = SDL_CreateMutex();
    g_display_submit = submit;
    g_audio.open = sdl_audio_open;
    g_audio.queue = sdl_audio_queue;
    g_audio.queued_bytes = sdl_audio_queued;
    g_audio.clear = sdl_audio_clear;
    g_audio.close = sdl_audio_close;
    if (getenv("PTG_DUMP")) dump_every = atoi(getenv("PTG_DUMP"));
    parse_script(getenv("PTG_SCRIPT"));
    touch_init(touch_send);

    /* settings, kept on the writable drive */
    Settings settings = { FILTER_SHARP, 0 };
#ifdef __ANDROID__
    settings.touch = 1;
#else
    settings.touch = getenv("PTG_TOUCH") != NULL;
#endif
    char cfg[600];
#ifdef _WIN32
    _mkdir(g_c_root);
#else
    mkdir(g_c_root, 0755);
#endif
    snprintf(cfg, sizeof cfg, "%s/ptgdroid.cfg", g_c_root);
    settings_load(&settings, cfg);
    if (getenv("PTG_FILTER")) settings.filter = atoi(getenv("PTG_FILTER")) % FILTER_COUNT;

    /* The start menu is skipped for scripted test runs. */
    Menu menu = { MENU_CLOSED };
    int game_started = 0;
    if (ntaps) {
        SDL_CreateThreadWithStackSize(game_thread, "guest", 8 << 20, NULL);
        game_started = 1;
    } else {
        menu_open(&menu, MENU_START);
    }
    uint32_t t0 = SDL_GetTicks();
    int shot_at = getenv("PTG_SHOT") ? atoi(getenv("PTG_SHOT")) : 0;
    int background = 0, redraw = 1;

    for (int running = 1; running;) {
        uint32_t now = SDL_GetTicks() - t0;
        run_script(now);
        if (shot_at && now >= (uint32_t)shot_at) redraw = 1;
        if (script_menu_request == 1 && menu.mode == MENU_CLOSED) {
            menu_open(&menu, MENU_PAUSE);
            input_focus(0);
            redraw = 1;
        } else if (script_menu_request == 2 && menu.mode == MENU_PAUSE) {
            menu.mode = MENU_CLOSED;
            input_focus(1);
            redraw = 1;
        }
        script_menu_request = 0;
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, 4)) {
            do {
                int ow, oh, ww, wh;
                SDL_GetRendererOutputSize(ren, &ow, &oh);
                SDL_GetWindowSize(win, &ww, &wh);
                if (e.type == SDL_QUIT) { running = 0; break; }
                if (e.type == SDL_RENDER_TARGETS_RESET || e.type == SDL_RENDER_DEVICE_RESET) {
                    video_init(ren);
                    redraw = 1;
                    continue;
                }
                if (e.type == SDL_CONTROLLERDEVICEADDED) { SDL_GameControllerOpen(e.cdevice.which); continue; }
                if (e.type == SDL_APP_WILLENTERBACKGROUND) { background = 1; input_focus(0); continue; }
                if (e.type == SDL_APP_DIDENTERFOREGROUND) {
                    background = 0;
                    redraw = 1;
                    if (menu.mode == MENU_CLOSED) input_focus(1);
                    continue;
                }
                if (e.type == SDL_WINDOWEVENT) {
                    redraw = 1;
                    if (ntaps || menu.mode != MENU_CLOSED) continue;
                    if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) input_focus(0);
                    if (e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) input_focus(1);
                    continue;
                }

                if (menu.mode != MENU_CLOSED) {
                    int act = menu_event(&menu, &settings, &e, ow, oh, ww, wh);
                    if (act != MENU_ACT_NONE) redraw = 1;
                    if (act == MENU_ACT_CHANGED) settings_save(&settings, cfg);
                    if (act == MENU_ACT_START) {
                        menu.mode = MENU_CLOSED;
                        SDL_CreateThreadWithStackSize(game_thread, "guest", 8 << 20, NULL);
                        game_started = 1;
                    } else if (act == MENU_ACT_RESUME) {
                        menu.mode = MENU_CLOSED;
                        input_focus(1);
                    } else if (act == MENU_ACT_QUIT) {
                        running = 0;
                    }
                    continue;
                }

                /* in game: Android back / Escape / gamepad guide pauses and opens the menu */
                if ((e.type == SDL_KEYDOWN && (e.key.keysym.scancode == SDL_SCANCODE_AC_BACK ||
                                               e.key.keysym.scancode == SDL_SCANCODE_ESCAPE)) ||
                    (e.type == SDL_CONTROLLERBUTTONDOWN && e.cbutton.button == SDL_CONTROLLER_BUTTON_GUIDE)) {
                    menu_open(&menu, MENU_PAUSE);
                    input_focus(0);
                    redraw = 1;
                    continue;
                }
                if (settings.touch) {
                    SDL_Event t = e;
                    if (t.type == SDL_MOUSEBUTTONDOWN || t.type == SDL_MOUSEBUTTONUP) {
                        t.button.x = t.button.x * ow / (ww ? ww : 1);
                        t.button.y = t.button.y * oh / (wh ? wh : 1);
                    } else if (t.type == SDL_MOUSEMOTION) {
                        t.motion.x = t.motion.x * ow / (ww ? ww : 1);
                        t.motion.y = t.motion.y * oh / (wh ? wh : 1);
                    }
                    if (touch_event(&t, ow, oh)) { redraw = 1; continue; }
                }
                switch (e.type) {
                case SDL_KEYDOWN: case SDL_KEYUP:
                    if (!e.key.repeat) send_nkey(map_key(e.key.keysym.scancode), e.type == SDL_KEYDOWN);
                    break;
                case SDL_CONTROLLERBUTTONDOWN: case SDL_CONTROLLERBUTTONUP:
                    send_nkey(map_button(e.cbutton.button), e.type == SDL_CONTROLLERBUTTONDOWN);
                    break;
                default: break;
                }
            } while (running && SDL_PollEvent(&e));
        }
        SDL_LockMutex(fb_lock);
        int dirty = fb_dirty;
        if (dirty) {
            video_upload(fb_copy);
            fb_dirty = 0;
        }
        SDL_UnlockMutex(fb_lock);
        if ((dirty || redraw) && !background) {
            int ow, oh;
            SDL_GetRendererOutputSize(ren, &ow, &oh);
            SDL_Rect game;
            if (settings.touch) {
                touch_layout(ow, oh, &game);
            } else {
                float sc = fminf((float)ow / W, (float)oh / H);
                game.w = (int)(W * sc); game.h = (int)(H * sc);
                game.x = (ow - game.w) / 2; game.y = (oh - game.h) / 2;
            }
            SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
            SDL_RenderClear(ren);
            if (game_started) video_draw(ren, settings.filter, game);
            if (settings.touch && game_started && menu.mode == MENU_CLOSED) touch_draw(ren);
            if (menu.mode != MENU_CLOSED) menu_draw(&menu, &settings, ren, ow, oh);
            if (shot_at && SDL_GetTicks() - t0 >= (uint32_t)shot_at) {
                SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormat(0, ow, oh, 24, SDL_PIXELFORMAT_BGR24);
                SDL_RenderReadPixels(ren, NULL, SDL_PIXELFORMAT_BGR24, surf->pixels, surf->pitch);
                SDL_SaveBMP(surf, "shot.bmp");
                SDL_FreeSurface(surf);
                LOG("screenshot saved to shot.bmp");
                shot_at = 0;
            }
            SDL_RenderPresent(ren);
            redraw = 0;
        }
    }
    SDL_Quit();
    exit(0);
}
