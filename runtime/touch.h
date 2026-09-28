#pragma once
#include <SDL.h>
#include <stdint.h>

typedef void (*TouchSend)(uint32_t scan, uint32_t code, int down);

void touch_init(TouchSend send);
/* Lays out controls for a w x h pixel window and returns where the game screen goes. */
void touch_layout(int w, int h, SDL_Rect *game);
/* Returns 1 if the event was a touch/mouse event consumed by the controls. */
int touch_event(const SDL_Event *e, int w, int h);
void touch_draw(SDL_Renderer *r);
