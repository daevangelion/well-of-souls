#ifndef WOS_FB_H
#define WOS_FB_H
#include <stdint.h>
#include "image.h"
typedef struct { int x, y, w, h; } Rect;
typedef struct Framebuffer { uint32_t *pixels; int w, h; Rect clip; } Framebuffer;
void fb_init(Framebuffer *fb, uint32_t *pixels, int w, int h);
void fb_clip(Framebuffer *fb, Rect rect);
void fb_clip_intersect(Framebuffer *fb, Rect rect);
void fb_reset_clip(Framebuffer *fb);
void fb_clear(Framebuffer *fb, uint32_t color);
void fb_pixel(Framebuffer *fb, int x, int y, uint32_t color);
void fb_fill(Framebuffer *fb, Rect rect, uint32_t color);
void fb_rect(Framebuffer *fb, Rect rect, uint32_t color);
void fb_line(Framebuffer *fb, int x0, int y0, int x1, int y1, uint32_t color);
/* key < 0 disables color key; flip reverses source subrect horizontally. */
void fb_blit(Framebuffer *fb, const Image *image, int x, int y, int64_t key);
void fb_blit_sub(Framebuffer *fb, const Image *image, Rect src, int x, int y, int flip, int64_t key);
void fb_blend(Framebuffer *fb, Rect rect, uint32_t color, unsigned alpha); /* alpha 0..255 */
#endif
