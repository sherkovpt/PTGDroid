#pragma once
#include <SDL.h>

typedef struct { int filter; int touch; } Settings;

enum { MENU_CLOSED, MENU_START, MENU_PAUSE };
typedef struct { int mode; } Menu;

enum { MENU_ACT_NONE, MENU_ACT_REDRAW, MENU_ACT_CHANGED, MENU_ACT_START, MENU_ACT_RESUME, MENU_ACT_QUIT };

void menu_open(Menu *m, int mode);
/* w,h = renderer output size in pixels; win_w,win_h = window size (mouse coordinates). */
int menu_event(Menu *m, Settings *s, const SDL_Event *e, int w, int h, int win_w, int win_h);
void menu_draw(const Menu *m, const Settings *s, SDL_Renderer *r, int w, int h);

void settings_load(Settings *s, const char *path);
void settings_save(const Settings *s, const char *path);
