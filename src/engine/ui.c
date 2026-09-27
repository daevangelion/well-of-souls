#include "ui.h"
#include "font.h"
#include <string.h>
void input_begin(Input *in)
{
    memset(in->pressed,0,sizeof(in->pressed)); memset(in->released,0,sizeof(in->released));
    in->mouse_pressed=0; in->mouse_released=0; in->wheel=0; in->text[0]=0;
}
void input_event(Input *in,const PlatEvent *ev)
{
    unsigned bit=ev->button>0 && ev->button<32?1u<<ev->button:0;
    switch(ev->type) {
    case PLAT_EV_QUIT: in->quit=1; break;
    case PLAT_EV_KEY_DOWN:
        if(ev->key>=0 && ev->key<INPUT_KEYS) { in->pressed[ev->key]=1; in->down[ev->key]=1; } break;
    case PLAT_EV_KEY_UP:
        if(ev->key>=0 && ev->key<INPUT_KEYS) { in->released[ev->key]=1; in->down[ev->key]=0; } break;
    case PLAT_EV_TEXT: {
        size_t have=strlen(in->text),n=0;
        while(n<sizeof(ev->text) && ev->text[n]) ++n;
        if(n<sizeof(ev->text) && n<sizeof(in->text)-have) memcpy(in->text+have,ev->text,n+1);
        break;
    }
    case PLAT_EV_MOUSE_MOVE: in->mouse_x=ev->x; in->mouse_y=ev->y; break;
    case PLAT_EV_MOUSE_DOWN:
        in->mouse_x=ev->x; in->mouse_y=ev->y; in->mouse_down|=bit; in->mouse_pressed|=bit; break;
    case PLAT_EV_MOUSE_UP:
        in->mouse_x=ev->x; in->mouse_y=ev->y; in->mouse_down&=~bit; in->mouse_released|=bit; break;
    case PLAT_EV_MOUSE_WHEEL: in->wheel+=ev->y; break;
    default: break;
    }
}
void ui_begin(Ui *ui,const Input *input)
{
    ui->input=input;
    if(!(input->mouse_down&2u) && !(input->mouse_released&2u)) ui->active=0;
}
static int inside(const Input *in,Rect r)
{ return in->mouse_x>=r.x && in->mouse_y>=r.y && (int64_t)in->mouse_x<(int64_t)r.x+r.w && (int64_t)in->mouse_y<(int64_t)r.y+r.h; }
int ui_button(Ui *ui,Framebuffer *fb,unsigned id,Rect r,const char *label)
{
    int hover=inside(ui->input,r),clicked=0;
    Rect old=fb->clip;
    if(hover && (ui->input->mouse_pressed&2u)) ui->active=id;
    if((ui->input->mouse_released&2u) && ui->active==id) { clicked=hover; ui->active=0; }
    fb_fill(fb,r,ui->active==id?0x66552d:hover?0x676b80:0x353a50);
    fb_rect(fb,r,hover?0xffdc80:0xa0a8b0);
    fb_clip_intersect(fb,r);
    font_draw(fb,r.x+(r.w-font_width(label))/2,r.y+(r.h-8)/2,label,0xffffff);
    fb->clip=old;
    return clicked;
}
int ui_text_input(Ui *ui,Framebuffer *fb,unsigned id,Rect r,char *text,size_t capacity)
{
    int result=0; Rect old=fb->clip;
    if(ui->input->mouse_pressed&2u) {
        if(inside(ui->input,r)) ui->focus=id;
        else if(ui->focus==id) ui->focus=0;
    }
    if(ui->focus==id && capacity) {
        size_t n=strlen(text),add=strlen(ui->input->text);
        if(ui->input->pressed[PLAT_KEY_BACKSPACE] && n) {
            do { --n; } while(n && ((unsigned char)text[n]&0xc0)==0x80);
            text[n]=0;
        }
        if(n<capacity && add<capacity-n) memcpy(text+n,ui->input->text,add+1);
        result=ui->input->pressed[PLAT_KEY_RETURN]!=0;
    }
    fb_fill(fb,r,0x101827); fb_rect(fb,r,ui->focus==id?0xffdc80:0x909090);
    fb_clip_intersect(fb,(Rect){r.x+3,r.y+2,r.w-6,r.h-4});
    font_draw(fb,r.x+4,r.y+(r.h-8)/2,text,0xffffff); fb->clip=old;
    return result;
}
int ui_list(Ui *ui,Framebuffer *fb,unsigned id,Rect r,const char *const *items,int count,int first,int *selection)
{
    int row,rows=r.h/12,old=*selection; const Input *in=ui->input;
    Rect clip=fb->clip;
    if(first<0) first=0;
    fb_fill(fb,r,0x151d2a); fb_rect(fb,r,0x8a94a0);
    if(inside(in,r) && (in->mouse_pressed&2u)) {
        int index=first+(in->mouse_y-r.y)/12;
        ui->focus=id; if(index>=0 && index<count) *selection=index;
    }
    if(ui->focus==id) {
        if(in->pressed[PLAT_KEY_UP] && *selection>0) --*selection;
        if(in->pressed[PLAT_KEY_DOWN] && *selection<count-1) ++*selection;
    }
    fb_clip_intersect(fb,r);
    for(row=0;row<rows && first+row<count;++row) {
        if(first+row==*selection) fb_fill(fb,(Rect){r.x+1,r.y+row*12,r.w-2,12},0x4b5670);
        font_draw(fb,r.x+4,r.y+row*12+2,items[first+row],0xffffff);
    }
    fb->clip=clip;
    return *selection!=old;
}
void ui_panel(Framebuffer *fb,Rect r,const char *title,const char *message)
{
    fb_fill(fb,r,0x202839); fb_rect(fb,r,0xc0a060);
    font_draw(fb,r.x+12,r.y+10,title,0xffdc80);
    font_wrap(fb,(Rect){r.x+12,r.y+28,r.w-24,r.h-40},message,0xffffff);
}
