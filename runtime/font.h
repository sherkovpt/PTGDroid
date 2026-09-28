#pragma once
#include <SDL.h>

/* 5x7 glyphs drawn with the current render colour; px = size of one font pixel. */
void font_char(SDL_Renderer *r, char ch, int x, int y, int px);
void font_text(SDL_Renderer *r, const char *s, int x, int y, int px);
void font_text_centered(SDL_Renderer *r, const char *s, SDL_Rect box, int px);
int font_width(const char *s, int px);
