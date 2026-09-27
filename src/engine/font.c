#include "font.h"
#include "../third_party/font8x8_basic.h"
#include <limits.h>
static void glyph(Framebuffer *fb,int x,int y,unsigned char ch,uint32_t color)
{
    int row,col; if(ch>=128) ch='?';
    for(row=0;row<8;++row) for(col=0;col<8;++col)
        if((unsigned char)font8x8_basic[ch][row]&(1u<<col)) fb_pixel(fb,x+col,y+row,color);
}
void font_measure(const char *text,int *w,int *h)
{
    int x=0,width=0,height=8;
    for(;*text;++text) {
        if(*text=='\n') { if(x>width) width=x; x=0; if(height<=INT_MAX-8) height+=8; }
        else if(x<=INT_MAX-8) x+=8;
    }
    if(x>width) width=x;
    if(w) *w=width;
    if(h) *h=height;
}
int font_width(const char *text) { int w; font_measure(text,&w,0); return w; }
void font_draw(Framebuffer *fb,int x,int y,const char *text,uint32_t color)
{
    int start=x;
    for(;*text;++text) {
        if(*text=='\n') { x=start; if(y>INT_MAX-8) break; y+=8; }
        else { if(x>INT_MAX-8 || y>INT_MAX-8) break; glyph(fb,x,y,(unsigned char)*text,color); x+=8; }
    }
}
int font_wrap(Framebuffer *fb,Rect rect,const char *text,uint32_t color)
{
    Rect old=fb->clip;
    int cols=rect.w/8,col=0,row=0;
    if(cols<1 || rect.h<1) return 0;
    fb_clip_intersect(fb,rect);
    while(*text && row<rect.h/8+1) {
        const char *end; int len=0;
        if(*text=='\n') { ++text; ++row; col=0; continue; }
        if(*text==' ' || *text=='\t' || *text=='\r') {
            ++text; if(col) { if(++col>=cols) { ++row; col=0; } } continue;
        }
        end=text;
        while(*end && *end!=' ' && *end!='\t' && *end!='\r' && *end!='\n') { ++end; if(len<cols+1) ++len; }
        if(col && len>cols-col) { ++row; col=0; }
        while(text<end && row<rect.h/8+1) {
            if(col==cols) { col=0; ++row; }
            glyph(fb,rect.x+col*8,rect.y+row*8,(unsigned char)*text++,color); ++col;
        }
    }
    fb->clip=old; return (row+1)*8;
}
