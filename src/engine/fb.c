#include "fb.h"
#include <stdlib.h>
static Rect intersect(Rect a, Rect b)
{
    int64_t x = a.x > b.x ? a.x : b.x, y = a.y > b.y ? a.y : b.y;
    int64_t ar = (int64_t)a.x + a.w, br = (int64_t)b.x + b.w;
    int64_t ab = (int64_t)a.y + a.h, bb = (int64_t)b.y + b.h;
    int64_t r = ar < br ? ar : br, d = ab < bb ? ab : bb;
    Rect out = {0, 0, 0, 0};
    if (a.w <= 0 || a.h <= 0 || b.w <= 0 || b.h <= 0 || r <= x || d <= y) return out;
    out.x = (int)x; out.y = (int)y; out.w = (int)(r-x); out.h = (int)(d-y); return out;
}
void fb_init(Framebuffer *fb, uint32_t *pixels, int w, int h)
{ fb->pixels = pixels; fb->w = w; fb->h = h; fb_reset_clip(fb); }
void fb_reset_clip(Framebuffer *fb) { fb->clip = (Rect){0,0,fb->w,fb->h}; }
void fb_clip(Framebuffer *fb, Rect r) { fb->clip = intersect(r, (Rect){0,0,fb->w,fb->h}); }
void fb_clip_intersect(Framebuffer *fb, Rect r) { fb->clip = intersect(r, fb->clip); }
void fb_clear(Framebuffer *fb, uint32_t c)
{
    size_t i, n = (size_t)fb->w * (size_t)fb->h;
    for (i=0; i<n; ++i) fb->pixels[i] = c & 0xffffff;
}
void fb_pixel(Framebuffer *fb, int x, int y, uint32_t c)
{
    Rect r = fb->clip;
    if (x >= r.x && y >= r.y && x < r.x+r.w && y < r.y+r.h)
        fb->pixels[(size_t)y*fb->w+x] = c & 0xffffff;
}
void fb_fill(Framebuffer *fb, Rect r, uint32_t c)
{
    int x,y; r = intersect(r,fb->clip);
    for (y=r.y; y<r.y+r.h; ++y) for (x=r.x; x<r.x+r.w; ++x)
        fb->pixels[(size_t)y*fb->w+x] = c & 0xffffff;
}
void fb_rect(Framebuffer *fb, Rect r, uint32_t c)
{
    int64_t right=(int64_t)r.x+r.w-1, bottom=(int64_t)r.y+r.h-1;
    if (r.w<=0 || r.h<=0) return;
    fb_fill(fb,(Rect){r.x,r.y,r.w,1},c);
    fb_fill(fb,(Rect){r.x,r.y,1,r.h},c);
    if (bottom>=0 && bottom<fb->h) fb_fill(fb,(Rect){r.x,(int)bottom,r.w,1},c);
    if (right>=0 && right<fb->w) fb_fill(fb,(Rect){(int)right,r.y,1,r.h},c);
}
static int line_code(double x,double y,Rect r)
{ return (x<r.x?1:0)|(x>r.x+r.w-1?2:0)|(y<r.y?4:0)|(y>r.y+r.h-1?8:0); }
void fb_line(Framebuffer *fb,int ax,int ay,int bx,int by,uint32_t color)
{
    double x0=ax,y0=ay,x1=bx,y1=by;
    int a,b,dx,dy,sx,sy,err;
    Rect r=fb->clip;
    if (!r.w || !r.h) return;
    for (;;) {
        int code; double x,y;
        a=line_code(x0,y0,r); b=line_code(x1,y1,r);
        if (!(a|b)) break;
        if (a&b) return;
        code=a?a:b;
        if (code&8) { y=r.y+r.h-1; x=x0+(x1-x0)*(y-y0)/(y1-y0); }
        else if (code&4) { y=r.y; x=x0+(x1-x0)*(y-y0)/(y1-y0); }
        else if (code&2) { x=r.x+r.w-1; y=y0+(y1-y0)*(x-x0)/(x1-x0); }
        else { x=r.x; y=y0+(y1-y0)*(x-x0)/(x1-x0); }
        if (code==a) { x0=x; y0=y; } else { x1=x; y1=y; }
    }
    ax=(int)x0; ay=(int)y0; bx=(int)x1; by=(int)y1;
    dx=abs(bx-ax); dy=-abs(by-ay); sx=ax<bx?1:-1; sy=ay<by?1:-1; err=dx+dy;
    for (;;) {
        int e=2*err;
        fb_pixel(fb,ax,ay,color);
        if (ax==bx && ay==by) break;
        if (e>=dy) { err+=dy; ax+=sx; }
        if (e<=dx) { err+=dx; ay+=sy; }
    }
}
void fb_blit_sub(Framebuffer *fb,const Image *im,Rect src,int x,int y,int flip,int64_t key)
{
    Rect dst=intersect((Rect){x,y,src.w,src.h},fb->clip);
    int px,py;
    if (!im || !im->pixels) return;
    for (py=dst.y; py<dst.y+dst.h; ++py) {
        int64_t sy=(int64_t)src.y+py-y;
        if (sy<0 || sy>=im->h) continue;
        for (px=dst.x; px<dst.x+dst.w; ++px) {
            int64_t offset=(int64_t)px-x;
            int64_t sx=(int64_t)src.x+(flip?(int64_t)src.w-1-offset:offset);
            uint32_t c;
            if (sx<0 || sx>=im->w) continue;
            c=im->pixels[(size_t)sy*im->w+(size_t)sx];
            if (key<0 || c!=(uint32_t)key) fb->pixels[(size_t)py*fb->w+px]=c;
        }
    }
}
void fb_blit(Framebuffer *fb,const Image *im,int x,int y,int64_t key)
{ if (im) fb_blit_sub(fb,im,(Rect){0,0,im->w,im->h},x,y,0,key); }
void fb_blend(Framebuffer *fb,Rect r,uint32_t c,unsigned alpha)
{
    int x,y; unsigned a=alpha>255?255:alpha;
    r=intersect(r,fb->clip);
    for(y=r.y;y<r.y+r.h;++y) for(x=r.x;x<r.x+r.w;++x) {
        uint32_t *p=&fb->pixels[(size_t)y*fb->w+x], d=*p;
        unsigned rr=(((c>>16)&255)*a+((d>>16)&255)*(255-a)+127)/255;
        unsigned gg=(((c>>8)&255)*a+((d>>8)&255)*(255-a)+127)/255;
        unsigned bb=((c&255)*a+(d&255)*(255-a)+127)/255;
        *p=(rr<<16)|(gg<<8)|bb;
    }
}
