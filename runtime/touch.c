/* On-screen N-Gage controls for touch screens: 8-way d-pad (centre = select), soft keys, C and
 * the 0-9 * # keypad. Multi-touch; each finger holds one control and may slide across the d-pad. */
#include "touch.h"
#include <math.h>
#include <string.h>

enum { B_SOFT_L, B_C, B_SOFT_R, B_KEY0 /* 12 keypad keys follow */, NBUTTONS = B_KEY0 + 12 };
typedef struct { SDL_Rect r; uint32_t scan, code; char label; int pressed; } Button;

static Button buttons[NBUTTONS];
static SDL_Rect dpad;
static int dpad_dirs; /* bit mask of currently pressed d-pad keys, for drawing */
static TouchSend send_key;

enum { D_UP = 1, D_DOWN = 2, D_LEFT = 4, D_RIGHT = 8, D_OK = 16 };
static const uint32_t dir_scan[5] = { 0x10, 0x11, 0x0E, 0x0F, 0xA7 };
static const uint32_t dir_code[5] = { 0xF809, 0xF80A, 0xF807, 0xF808, 0xF845 };

#define MAX_FINGERS 10
typedef struct { SDL_FingerID id; int active, button, dirs; } Finger;
static Finger fingers[MAX_FINGERS];

static const char keypad_labels[12] = { '1', '2', '3', '4', '5', '6', '7', '8', '9', '*', '0', '#' };

void touch_init(TouchSend send) {
    send_key = send;
    buttons[B_SOFT_L] = (Button){ { 0 }, 0xA4, 0xF842, '-', 0 };
    buttons[B_C] = (Button){ { 0 }, 0x01, 8, 'C', 0 };
    buttons[B_SOFT_R] = (Button){ { 0 }, 0xA5, 0xF843, '-', 0 };
    for (int i = 0; i < 12; i++) {
        char ch = keypad_labels[i];
        uint32_t scan = ch == '*' ? 0x2A : ch == '#' ? 0x7F : (uint32_t)ch;
        buttons[B_KEY0 + i] = (Button){ { 0 }, scan, (uint32_t)ch, ch, 0 };
    }
}

static SDL_Rect rect(int x, int y, int w, int h) { SDL_Rect r = { x, y, w, h }; return r; }

void touch_layout(int w, int h, SDL_Rect *game) {
    int pad = (w < h ? w : h) / 40;
    if (h > w) {
        /* portrait: game on top, controls below */
        /* keep clear of the camera cut-out at the top edge */
        int top_margin = h * 35 / 1000;
        float s = fminf((float)w / 176.f, (float)h * 0.54f / 208.f);
        int gw = (int)(176 * s), gh = (int)(208 * s);
        *game = rect((w - gw) / 2, top_margin, gw, gh);
        int top = top_margin + gh + 2 * pad, ch = h - top - pad;
        int row = ch / 8;
        int bw = w / 4;
        buttons[B_SOFT_L].r = rect(pad, top, bw, row);
        buttons[B_C].r = rect((w - bw / 2) / 2, top, bw / 2, row);
        buttons[B_SOFT_R].r = rect(w - pad - bw, top, bw, row);
        int area_top = top + row + pad, area_h = h - area_top - pad;
        int d = (int)fminf((float)w * 0.46f, (float)area_h);
        dpad = rect(pad, area_top + (area_h - d) / 2, d, d);
        int kx = w / 2 + pad, kw = w - kx - pad, kh = area_h;
        int cw = kw / 3, cellh = kh / 4;
        if (cellh > cw) cellh = cw;
        int ky = area_top + (area_h - cellh * 4) / 2;
        for (int i = 0; i < 12; i++)
            buttons[B_KEY0 + i].r = rect(kx + (i % 3) * cw + pad / 2, ky + (i / 3) * cellh + pad / 2, cw - pad, cellh - pad);
    } else {
        /* landscape: game centred, d-pad left, keypad right */
        float s = fminf((float)h / 208.f, (float)w * 0.5f / 176.f);
        int gw = (int)(176 * s), gh = (int)(208 * s);
        *game = rect((w - gw) / 2, (h - gh) / 2, gw, gh);
        int side = (w - gw) / 2 - 2 * pad;
        int row = h / 8;
        buttons[B_SOFT_L].r = rect(pad, pad, side, row);
        buttons[B_SOFT_R].r = rect(w - pad - side, pad, side, row);
        buttons[B_C].r = rect(w - pad - side, h - pad - row, side, row);
        int d = (int)fminf((float)side, (float)(h - row - 3 * pad));
        dpad = rect(pad + (side - d) / 2, row + 2 * pad + (h - row - 3 * pad - d) / 2, d, d);
        int kx = w - pad - side, ky = row + 2 * pad, kh = h - 2 * row - 4 * pad;
        int cw = side / 3, cellh = kh / 4;
        for (int i = 0; i < 12; i++)
            buttons[B_KEY0 + i].r = rect(kx + (i % 3) * cw + pad / 4, ky + (i / 3) * cellh + pad / 4, cw - pad / 2, cellh - pad / 2);
    }
}

static int hit_button(int x, int y) {
    for (int i = 0; i < NBUTTONS; i++) {
        SDL_Rect *r = &buttons[i].r;
        if (x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h) return i;
    }
    return -1;
}

/* D-pad: centre zone = OK; otherwise up to two directions (8-way). */
static int dpad_dirs_at(int x, int y) {
    float cx = dpad.x + dpad.w / 2.f, cy = dpad.y + dpad.h / 2.f, r = dpad.w / 2.f;
    float dx = (x - cx) / r, dy = (y - cy) / r;
    if (dx < -1.3f || dx > 1.3f || dy < -1.3f || dy > 1.3f) return -1; /* outside */
    if (dx * dx + dy * dy < 0.12f) return D_OK;
    int m = 0;
    float ax = fabsf(dx), ay = fabsf(dy);
    if (ay > 0.41f * ax) m |= dy < 0 ? D_UP : D_DOWN;
    if (ax > 0.41f * ay) m |= dx < 0 ? D_LEFT : D_RIGHT;
    return m;
}

static void set_dirs(Finger *f, int dirs) {
    for (int b = 0; b < 5; b++) {
        int was = f->dirs & (1 << b), now = dirs & (1 << b);
        if (was != now) send_key(dir_scan[b], dir_code[b], now != 0);
    }
    f->dirs = dirs;
}

static void recompute_dpad_mask(void) {
    dpad_dirs = 0;
    for (int i = 0; i < MAX_FINGERS; i++) if (fingers[i].active) dpad_dirs |= fingers[i].dirs;
}

static Finger *finger(SDL_FingerID id, int create) {
    for (int i = 0; i < MAX_FINGERS; i++) if (fingers[i].active && fingers[i].id == id) return &fingers[i];
    if (!create) return NULL;
    for (int i = 0; i < MAX_FINGERS; i++) {
        if (!fingers[i].active) {
            fingers[i] = (Finger){ id, 1, -1, 0 };
            return &fingers[i];
        }
    }
    return NULL;
}

static void finger_at(Finger *f, int x, int y, int down) {
    if (down) {
        int b = hit_button(x, y);
        if (b >= 0) {
            f->button = b;
            buttons[b].pressed = 1;
            send_key(buttons[b].scan, buttons[b].code, 1);
            return;
        }
    }
    if (f->button >= 0) return; /* keypad presses do not slide */
    int d = dpad_dirs_at(x, y);
    if (down && d < 0) return;
    set_dirs(f, d < 0 ? 0 : d);
}

static void finger_up(Finger *f) {
    if (f->button >= 0) {
        buttons[f->button].pressed = 0;
        send_key(buttons[f->button].scan, buttons[f->button].code, 0);
    }
    set_dirs(f, 0);
    f->active = 0;
}

int touch_event(const SDL_Event *e, int w, int h) {
    SDL_FingerID id;
    int x, y, type;
    if (e->type == SDL_FINGERDOWN || e->type == SDL_FINGERUP || e->type == SDL_FINGERMOTION) {
        id = e->tfinger.fingerId;
        x = (int)(e->tfinger.x * w);
        y = (int)(e->tfinger.y * h);
        type = e->type == SDL_FINGERDOWN ? 0 : e->type == SDL_FINGERUP ? 2 : 1;
    } else if (e->type == SDL_MOUSEBUTTONDOWN || e->type == SDL_MOUSEBUTTONUP ||
               (e->type == SDL_MOUSEMOTION && (e->motion.state & SDL_BUTTON_LMASK))) {
        if (e->type == SDL_MOUSEMOTION ? e->motion.which == SDL_TOUCH_MOUSEID : e->button.which == SDL_TOUCH_MOUSEID)
            return 0; /* synthesized from touch, already handled */
        id = -12345;
        x = e->type == SDL_MOUSEMOTION ? e->motion.x : e->button.x;
        y = e->type == SDL_MOUSEMOTION ? e->motion.y : e->button.y;
        type = e->type == SDL_MOUSEBUTTONDOWN ? 0 : e->type == SDL_MOUSEBUTTONUP ? 2 : 1;
    } else {
        return 0;
    }
    Finger *f = finger(id, type == 0);
    if (!f) return 1;
    if (type == 2) finger_up(f);
    else finger_at(f, x, y, type == 0);
    recompute_dpad_mask();
    return 1;
}

/* ---- drawing ---- */
static const uint8_t font[][8] = {
    { '0', 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E }, { '1', 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E },
    { '2', 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F }, { '3', 0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E },
    { '4', 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 }, { '5', 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E },
    { '6', 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E }, { '7', 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 },
    { '8', 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E }, { '9', 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C },
    { '*', 0x00, 0x04, 0x15, 0x0E, 0x15, 0x04, 0x00 }, { '#', 0x0A, 0x0A, 0x1F, 0x0A, 0x1F, 0x0A, 0x0A },
    { 'C', 0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E }, { '-', 0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00 },
};

static void draw_char(SDL_Renderer *r, char ch, SDL_Rect box) {
    for (size_t g = 0; g < sizeof font / sizeof font[0]; g++) {
        if (font[g][0] != ch) continue;
        int px = box.h / 14;
        if (px < 1) px = 1;
        int ox = box.x + (box.w - 5 * px) / 2, oy = box.y + (box.h - 7 * px) / 2;
        for (int row = 0; row < 7; row++)
            for (int col = 0; col < 5; col++)
                if (font[g][1 + row] & (0x10 >> col)) {
                    SDL_Rect p = { ox + col * px, oy + row * px, px, px };
                    SDL_RenderFillRect(r, &p);
                }
        return;
    }
}

static void fill(SDL_Renderer *r, SDL_Rect b, int pressed) {
    SDL_SetRenderDrawColor(r, 255, 255, 255, pressed ? 110 : 38);
    SDL_RenderFillRect(r, &b);
    SDL_SetRenderDrawColor(r, 255, 255, 255, 90);
    SDL_RenderDrawRect(r, &b);
}

static void triangle(SDL_Renderer *r, int cx, int cy, int size, int dir, int pressed) {
    SDL_SetRenderDrawColor(r, 255, 255, 255, pressed ? 230 : 140);
    for (int i = 0; i < size; i++) {
        int half = i / 2;
        switch (dir) {
        case D_UP: SDL_RenderDrawLine(r, cx - half, cy - size / 2 + i, cx + half, cy - size / 2 + i); break;
        case D_DOWN: SDL_RenderDrawLine(r, cx - half, cy + size / 2 - i, cx + half, cy + size / 2 - i); break;
        case D_LEFT: SDL_RenderDrawLine(r, cx - size / 2 + i, cy - half, cx - size / 2 + i, cy + half); break;
        case D_RIGHT: SDL_RenderDrawLine(r, cx + size / 2 - i, cy - half, cx + size / 2 - i, cy + half); break;
        }
    }
}

void touch_draw(SDL_Renderer *r) {
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    for (int i = 0; i < NBUTTONS; i++) {
        fill(r, buttons[i].r, buttons[i].pressed);
        SDL_SetRenderDrawColor(r, 255, 255, 255, 200);
        draw_char(r, buttons[i].label, buttons[i].r);
    }
    /* d-pad: cross of three cells plus centre */
    int c = dpad.w / 3;
    SDL_Rect up = { dpad.x + c, dpad.y, c, c }, down = { dpad.x + c, dpad.y + 2 * c, c, c };
    SDL_Rect left = { dpad.x, dpad.y + c, c, c }, right = { dpad.x + 2 * c, dpad.y + c, c, c };
    SDL_Rect mid = { dpad.x + c, dpad.y + c, c, c };
    fill(r, up, dpad_dirs & D_UP);
    fill(r, down, dpad_dirs & D_DOWN);
    fill(r, left, dpad_dirs & D_LEFT);
    fill(r, right, dpad_dirs & D_RIGHT);
    fill(r, mid, dpad_dirs & D_OK);
    triangle(r, up.x + c / 2, up.y + c / 2, c / 2, D_UP, dpad_dirs & D_UP);
    triangle(r, down.x + c / 2, down.y + c / 2, c / 2, D_DOWN, dpad_dirs & D_DOWN);
    triangle(r, left.x + c / 2, left.y + c / 2, c / 2, D_LEFT, dpad_dirs & D_LEFT);
    triangle(r, right.x + c / 2, right.y + c / 2, c / 2, D_RIGHT, dpad_dirs & D_RIGHT);
    SDL_SetRenderDrawColor(r, 255, 255, 255, 140);
    SDL_Rect dot = { mid.x + c / 3, mid.y + c / 3, c / 3, c / 3 };
    SDL_RenderFillRect(r, &dot);
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
}
