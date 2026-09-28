/* Presentation filters for the 176x208 EColor4K (RGB444) game screen. */
#include "video.h"
#include <string.h>

#define W 176
#define H 208
#define K 4 /* intermediate canvas scale */

static SDL_Texture *src, *canvas, *smooth, *grid;
static uint16_t frame[W * H];
static int smooth_dirty = 1;

static const char *const names[FILTER_COUNT] = { "PIXEL PERFECT", "SHARP", "SMOOTH", "LCD" };
const char *video_filter_name(int f) { return names[(unsigned)f % FILTER_COUNT]; }

static void destroy(SDL_Texture **t) {
    if (*t) SDL_DestroyTexture(*t);
    *t = NULL;
}

void video_init(SDL_Renderer *r) {
    destroy(&src); destroy(&canvas); destroy(&smooth); destroy(&grid);
    src = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGB444, SDL_TEXTUREACCESS_STREAMING, W, H);
    SDL_SetTextureScaleMode(src, SDL_ScaleModeNearest);
    canvas = SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, W * K, H * K);
    SDL_SetTextureScaleMode(canvas, SDL_ScaleModeLinear);
    smooth = SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, W * K, H * K);
    SDL_SetTextureScaleMode(smooth, SDL_ScaleModeLinear);

    /* LCD grid: each game pixel becomes a KxK cell with a darker right/bottom edge (multiplied). */
    grid = SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, W * K, H * K);
    static uint32_t px[W * K * H * K];
    for (int y = 0; y < H * K; y++)
        for (int x = 0; x < W * K; x++) {
            int edge = (x % K == K - 1) + (y % K == K - 1);
            uint32_t v = edge == 0 ? 255 : edge == 1 ? 170 : 120;
            px[y * W * K + x] = 0xFF000000u | (v << 16) | (v << 8) | v;
        }
    SDL_UpdateTexture(grid, NULL, px, W * K * 4);
    SDL_SetTextureBlendMode(grid, SDL_BLENDMODE_MOD);
    SDL_UpdateTexture(src, NULL, frame, W * 2);
    smooth_dirty = 1;
}

void video_upload(const uint16_t *fb) {
    memcpy(frame, fb, sizeof frame);
    SDL_UpdateTexture(src, NULL, frame, W * 2);
    smooth_dirty = 1;
}

/* Scale2x (EPX): doubles pixel art, rounding diagonal edges without blurring. */
static void scale2x(const uint16_t *in, int w, int h, uint16_t *out) {
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint16_t p = in[y * w + x];
            uint16_t a = y > 0 ? in[(y - 1) * w + x] : p;
            uint16_t b = x < w - 1 ? in[y * w + x + 1] : p;
            uint16_t c = x > 0 ? in[y * w + x - 1] : p;
            uint16_t d = y < h - 1 ? in[(y + 1) * w + x] : p;
            uint16_t *o = out + (2 * y) * (2 * w) + 2 * x;
            o[0] = (c == a && c != d && a != b) ? a : p;
            o[1] = (a == b && a != c && b != d) ? b : p;
            o[2 * w] = (d == c && d != b && c != a) ? c : p;
            o[2 * w + 1] = (b == d && b != a && d != c) ? d : p;
        }
    }
}

static void update_smooth(void) {
    static uint16_t x2[W * 2 * H * 2], x4[W * 4 * H * 4];
    static uint32_t argb[W * 4 * H * 4];
    scale2x(frame, W, H, x2);
    scale2x(x2, W * 2, H * 2, x4);
    for (int i = 0; i < W * 4 * H * 4; i++) {
        uint32_t p = x4[i];
        argb[i] = 0xFF000000u | (((p >> 8) & 0xF) * 17u << 16) | (((p >> 4) & 0xF) * 17u << 8) | ((p & 0xF) * 17u);
    }
    SDL_UpdateTexture(smooth, NULL, argb, W * 4 * 4);
    smooth_dirty = 0;
}

void video_draw(SDL_Renderer *r, int filter, SDL_Rect dst) {
    switch (filter) {
    case FILTER_SMOOTH:
        if (smooth_dirty) update_smooth();
        SDL_RenderCopy(r, smooth, NULL, &dst);
        break;
    case FILTER_SHARP:
    case FILTER_LCD:
        SDL_SetRenderTarget(r, canvas);
        SDL_RenderCopy(r, src, NULL, NULL);
        if (filter == FILTER_LCD) SDL_RenderCopy(r, grid, NULL, NULL);
        SDL_SetRenderTarget(r, NULL);
        SDL_RenderCopy(r, canvas, NULL, &dst);
        break;
    default:
        SDL_RenderCopy(r, src, NULL, &dst);
        break;
    }
}
