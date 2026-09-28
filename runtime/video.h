#pragma once
#include <SDL.h>
#include <stdint.h>

enum { FILTER_PIXEL, FILTER_SHARP, FILTER_SMOOTH, FILTER_LCD, FILTER_COUNT };

const char *video_filter_name(int filter);
/* (Re)creates the textures; call again after SDL_RENDER_DEVICE_RESET / TARGETS_RESET. */
void video_init(SDL_Renderer *r);
/* New 176x208 RGB444 frame. */
void video_upload(const uint16_t *fb);
void video_draw(SDL_Renderer *r, int filter, SDL_Rect dst);
