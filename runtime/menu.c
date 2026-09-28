/* Front-end menu: shown before the game starts and when paused (Android back / Escape). */
#include "menu.h"
#include "font.h"
#include "video.h"
#include <stdio.h>
#include <string.h>

enum { ITEM_PLAY, ITEM_FILTER, ITEM_TOUCH, ITEM_QUIT, NITEMS };

static int selected, pressed_item = -1;
static SDL_Rect item_rect[NITEMS];

void menu_open(Menu *m, int mode) {
    m->mode = mode;
    selected = 0;
    pressed_item = -1;
}

static void item_label(const Menu *m, const Settings *s, int i, char *buf, size_t n) {
    switch (i) {
    case ITEM_PLAY: snprintf(buf, n, "%s", m->mode == MENU_START ? "START GAME" : "RESUME"); break;
    case ITEM_FILTER: snprintf(buf, n, "FILTER: %s", video_filter_name(s->filter)); break;
    case ITEM_TOUCH: snprintf(buf, n, "TOUCH CONTROLS: %s", s->touch ? "ON" : "OFF"); break;
    default: snprintf(buf, n, "QUIT"); break;
    }
}

static void layout(int w, int h, int *px) {
    int pw = w * 9 / 10, ih = h / 11;
    if (pw > 900) pw = 900;
    if (ih > 110) ih = 110;
    *px = ih / 14 < 2 ? 2 : ih / 14;
    while (font_width("TOUCH CONTROLS: OFF", *px) > pw - 2 * *px && *px > 1) (*px)--;
    int top = h / 2 - (NITEMS * ih) / 2 + ih / 2;
    for (int i = 0; i < NITEMS; i++) {
        item_rect[i].x = (w - pw) / 2;
        item_rect[i].y = top + i * ih;
        item_rect[i].w = pw;
        item_rect[i].h = ih * 85 / 100;
    }
}

static int activate(Menu *m, Settings *s, int item, int dir) {
    switch (item) {
    case ITEM_PLAY: return m->mode == MENU_START ? MENU_ACT_START : MENU_ACT_RESUME;
    case ITEM_FILTER: s->filter = (s->filter + FILTER_COUNT + dir) % FILTER_COUNT; return MENU_ACT_CHANGED;
    case ITEM_TOUCH: s->touch = !s->touch; return MENU_ACT_CHANGED;
    default: return MENU_ACT_QUIT;
    }
}

static int hit(int x, int y) {
    for (int i = 0; i < NITEMS; i++) {
        SDL_Rect *r = &item_rect[i];
        if (x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h) return i;
    }
    return -1;
}

int menu_event(Menu *m, Settings *s, const SDL_Event *e, int w, int h, int win_w, int win_h) {
    int px;
    layout(w, h, &px);
    int back = MENU_ACT_NONE;
    if (m->mode == MENU_PAUSE) back = MENU_ACT_RESUME;
    switch (e->type) {
    case SDL_KEYDOWN:
        switch (e->key.keysym.scancode) {
        case SDL_SCANCODE_UP: selected = (selected + NITEMS - 1) % NITEMS; return MENU_ACT_REDRAW;
        case SDL_SCANCODE_DOWN: selected = (selected + 1) % NITEMS; return MENU_ACT_REDRAW;
        case SDL_SCANCODE_LEFT: return selected == ITEM_FILTER ? activate(m, s, selected, -1) : MENU_ACT_NONE;
        case SDL_SCANCODE_RIGHT: return selected == ITEM_FILTER ? activate(m, s, selected, 1) : MENU_ACT_NONE;
        case SDL_SCANCODE_RETURN: case SDL_SCANCODE_SPACE: case SDL_SCANCODE_KP_ENTER:
            return activate(m, s, selected, 1);
        case SDL_SCANCODE_ESCAPE: case SDL_SCANCODE_AC_BACK: return back;
        default: return MENU_ACT_NONE;
        }
    case SDL_CONTROLLERBUTTONDOWN:
        switch (e->cbutton.button) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP: selected = (selected + NITEMS - 1) % NITEMS; return MENU_ACT_REDRAW;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: selected = (selected + 1) % NITEMS; return MENU_ACT_REDRAW;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return selected == ITEM_FILTER ? activate(m, s, selected, -1) : MENU_ACT_NONE;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return selected == ITEM_FILTER ? activate(m, s, selected, 1) : MENU_ACT_NONE;
        case SDL_CONTROLLER_BUTTON_A: return activate(m, s, selected, 1);
        case SDL_CONTROLLER_BUTTON_B: case SDL_CONTROLLER_BUTTON_BACK: case SDL_CONTROLLER_BUTTON_START: return back;
        default: return MENU_ACT_NONE;
        }
    case SDL_FINGERDOWN: case SDL_FINGERUP: {
        int i = hit((int)(e->tfinger.x * w), (int)(e->tfinger.y * h));
        if (e->type == SDL_FINGERDOWN) {
            pressed_item = i;
            if (i >= 0) selected = i;
            return MENU_ACT_REDRAW;
        }
        int was = pressed_item;
        pressed_item = -1;
        return (i >= 0 && i == was) ? activate(m, s, i, 1) : MENU_ACT_REDRAW;
    }
    case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP: {
        if (e->button.which == SDL_TOUCH_MOUSEID) return MENU_ACT_NONE;
        int i = hit(e->button.x * w / (win_w ? win_w : 1), e->button.y * h / (win_h ? win_h : 1));
        if (e->type == SDL_MOUSEBUTTONDOWN) {
            pressed_item = i;
            if (i >= 0) selected = i;
            return MENU_ACT_REDRAW;
        }
        int was = pressed_item;
        pressed_item = -1;
        return (i >= 0 && i == was) ? activate(m, s, i, e->button.button == SDL_BUTTON_RIGHT ? -1 : 1) : MENU_ACT_REDRAW;
    }
    default:
        return MENU_ACT_NONE;
    }
}

void menu_draw(const Menu *m, const Settings *s, SDL_Renderer *r, int w, int h) {
    int px;
    layout(w, h, &px);
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, 0, 0, 0, m->mode == MENU_START ? 255 : 190);
    SDL_Rect all = { 0, 0, w, h };
    SDL_RenderFillRect(r, &all);

    /* title */
    int tpx = px * 2;
    const char *title = m->mode == MENU_START ? "PTGDROID" : "PAUSED";
    SDL_Rect tbox = { 0, item_rect[0].y - 7 * tpx * 3, w, 7 * tpx * 2 };
    SDL_SetRenderDrawColor(r, 232, 184, 58, 255);
    font_text_centered(r, title, tbox, tpx);

    for (int i = 0; i < NITEMS; i++) {
        char label[64];
        item_label(m, s, i, label, sizeof label);
        int sel = i == selected;
        SDL_SetRenderDrawColor(r, 62, 74, 30, sel ? 255 : 150);
        SDL_RenderFillRect(r, &item_rect[i]);
        SDL_SetRenderDrawColor(r, 232, 184, 58, sel ? 255 : 90);
        SDL_RenderDrawRect(r, &item_rect[i]);
        SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
        font_text_centered(r, label, item_rect[i], px);
    }
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
}

/* ---- settings file ---- */
void settings_load(Settings *s, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof line, f)) {
        int v;
        if (sscanf(line, "filter=%d", &v) == 1 && v >= 0 && v < FILTER_COUNT) s->filter = v;
        if (sscanf(line, "touch=%d", &v) == 1) s->touch = v != 0;
    }
    fclose(f);
}

void settings_save(const Settings *s, const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "filter=%d\ntouch=%d\n", s->filter, s->touch);
    fclose(f);
}
