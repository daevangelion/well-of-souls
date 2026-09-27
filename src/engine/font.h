#ifndef WOS_FONT_H
#define WOS_FONT_H
#include "fb.h"
#define FONT_W 8
#define FONT_H 8
int font_width(const char *text);
void font_measure(const char *text, int *w, int *h);
void font_draw(Framebuffer *fb, int x, int y, const char *text, uint32_t color);
/* Clips to rect and wraps at word boundaries; returns occupied height. */
int font_wrap(Framebuffer *fb, Rect rect, const char *text, uint32_t color);
#endif
