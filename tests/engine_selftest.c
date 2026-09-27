#include "../src/engine/replay.h"
#include "../src/engine/ini.h"
#include "../src/engine/image.h"
#include "../src/engine/fb.h"
#include "../src/engine/ui.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static Replay rp;
static int observed(const char *name,uint64_t since,void *user)
{ return !strcmp(name,"ready") && *(uint64_t *)user>since; }
static void test_replay(void)
{
    char script[]="# comment\nexpect ready 2\nkey RETURN\nhold LEFT 3\nclick 12 34 3\ntext Hero Name\nwait 2\nquit\n";
    char timeout[]="expect ready 1\nexpect ready 1\n";
    char bad[]="wait -1\n",hash[]="key #\n";
    size_t line,n; PlatEvent ev[REPLAY_EVENTS_MAX]; uint64_t serial=1;
    assert(!replay_parse(&rp,script,&line));
    assert(!replay_step(&rp,ev,&n,serial,observed,&serial));
    assert(n==1 && ev[0].type==PLAT_EV_KEY_DOWN && ev[0].key==PLAT_KEY_RETURN);
    assert(!replay_step(&rp,ev,&n,serial,observed,&serial)); assert(n==1 && ev[0].type==PLAT_EV_KEY_UP);
    assert(!replay_step(&rp,ev,&n,serial,observed,&serial)); assert(n==1 && ev[0].key==PLAT_KEY_LEFT);
    assert(!replay_step(&rp,ev,&n,serial,observed,&serial)); assert(n==0);
    assert(!replay_step(&rp,ev,&n,serial,observed,&serial)); assert(n==0);
    assert(!replay_step(&rp,ev,&n,serial,observed,&serial)); assert(n==1 && ev[0].type==PLAT_EV_KEY_UP);
    assert(!replay_step(&rp,ev,&n,serial,observed,&serial));
    assert(n==2 && ev[0].type==PLAT_EV_MOUSE_MOVE && ev[1].button==3 && ev[1].x==12 && ev[1].y==34);
    assert(!replay_step(&rp,ev,&n,serial,observed,&serial)); assert(n==1 && ev[0].type==PLAT_EV_MOUSE_UP);
    assert(!replay_step(&rp,ev,&n,serial,observed,&serial)); assert(n==1 && !strcmp(ev[0].text,"Hero Name"));
    assert(!replay_step(&rp,ev,&n,serial,observed,&serial)); assert(n==0);
    assert(!replay_step(&rp,ev,&n,serial,observed,&serial)); assert(n==0);
    assert(replay_step(&rp,ev,&n,serial,observed,&serial)==1);
    assert(!replay_parse(&rp,timeout,&line));
    assert(!replay_step(&rp,ev,&n,serial,observed,&serial));
    assert(replay_step(&rp,ev,&n,serial,observed,&serial)==2);
    assert(!strcmp(rp.failed_event,"ready"));
    assert(replay_parse(&rp,bad,&line)==-1 && line==1);
    assert(!replay_parse(&rp,hash,&line)); assert(rp.commands[0].key=='#');
    assert(replay_key("F12")==PLAT_KEY_F12 && replay_key("F13")==-1);
    puts("PASS replay: frame edges, waits, holds, click, text, expect consumption/timeout, invalid input");
}
static void test_ini(void)
{
    Ini ini; char text[]="; comment\r\n[Hero]\r\nName = Alice // note\r\nlevel=3\n[hero]\nLEVEL=4 ; override\n";
    char bad[]="[broken\n";
    assert(!ini_parse(&ini,text));
    assert(!strcmp(ini_get(&ini,"HERO","name",NULL),"Alice"));
    assert(!strcmp(ini_get(&ini,"hero","Level",NULL),"4"));
    assert(!strcmp(ini_get(&ini,"hero","missing","fallback"),"fallback"));
    assert(ini_parse(&ini,bad)==-1);
    puts("PASS ini: comments, CRLF, case-insensitive lookup, duplicate override, malformed section");
}
static void put32(unsigned char *p,uint32_t n)
{ p[0]=(unsigned char)n;p[1]=(unsigned char)(n>>8);p[2]=(unsigned char)(n>>16);p[3]=(unsigned char)(n>>24); }
static void test_bmp_formats(void)
{
    unsigned char bmp[1100]; Image im={0}; int bpps[]={1,4,8,24,32},k,top;
    for(k=0;k<5;++k) for(top=0;top<2;++top) {
        int bpp=bpps[k],colors=bpp<=8?1<<bpp:0,off=54+colors*4,row=bpp==32?8:bpp==24?8:4;
        memset(bmp,0,sizeof(bmp)); bmp[0]='B';bmp[1]='M';put32(bmp+10,(uint32_t)off);put32(bmp+14,40);
        put32(bmp+18,2);put32(bmp+22,top?UINT32_C(0xfffffffe):2);bmp[26]=1;bmp[28]=(unsigned char)bpp;
        if(colors) {
            bmp[58+2]=255; /* palette index 1 = red */
            bmp[off]=bpp==1?0x80:bpp==4?0x10:1;
        } else bmp[off+2]=255;
        assert(!image_decode(&im,bmp,(size_t)off+row*2));
        assert(im.w==2 && im.h==2 && im.bpp==bpp);
        assert(im.pixels[top?0:2]==0xff0000 && im.pixels[top?2:0]==0);
        if(colors) assert(im.indices[top?0:2]==1);
        assert(image_decode(&im,bmp,(size_t)off+row*2-1)==-1);
        assert(im.pixels[top?0:2]==0xff0000); /* failure preserves previous image */
        image_free(&im);
    }
    memset(bmp,0,sizeof(bmp));bmp[0]='B';bmp[1]='M';put32(bmp+10,62);put32(bmp+14,40);
    put32(bmp+18,2);put32(bmp+22,1);bmp[26]=1;bmp[28]=8;put32(bmp+30,1);put32(bmp+46,2);bmp[60]=255;
    bmp[62]=2;bmp[63]=1;bmp[64]=0;bmp[65]=1;
    assert(!image_decode(&im,bmp,66)); assert(im.indices[0]==1 && im.indices[1]==1 && im.pixels[0]==0xff0000);
    bmp[62]=3;assert(image_decode(&im,bmp,66)==-1);image_free(&im);
    puts("PASS bmp: 1/4/8/24/32-bit, both row orientations, RLE8, truncation and overrun rejection");
}
static void test_framebuffer(void)
{
    uint32_t pixels[16],source[]={1,2,3,4}; Framebuffer fb; Image im={0};
    fb_init(&fb,pixels,4,4);fb_clear(&fb,0);fb_clip(&fb,(Rect){1,1,2,2});
    fb_fill(&fb,(Rect){-1,-1,9,9},0xffffff);assert(pixels[5]==0xffffff && pixels[0]==0 && pixels[15]==0);
    fb_reset_clip(&fb);im.w=2;im.h=2;im.pixels=source;
    fb_blit_sub(&fb,&im,(Rect){0,0,2,2},-1,0,1,-1);assert(pixels[0]==1 && pixels[4]==3);
    fb_blit(&fb,&im,0,0,2);assert(pixels[0]==1 && pixels[1]==0);
    fb_clear(&fb,0);fb_line(&fb,-100,2,100,2,0xff);assert(pixels[8]==0xff && pixels[11]==0xff);
    fb_blend(&fb,(Rect){0,0,4,1},0xffffff,255);assert(pixels[0]==0xffffff);
    puts("PASS framebuffer: clipping, horizontal flip, color key, line clipping, alpha blend");
}
static void test_ui(void)
{
    uint32_t pixels[64*32]; Framebuffer fb; Input input={0}; Ui ui={0};
    PlatEvent event={0}; char text[16]="A"; Rect rect={0,0,32,16};
    fb_init(&fb,pixels,64,32);fb_clear(&fb,0);
    event.type=PLAT_EV_MOUSE_DOWN;event.button=1;event.x=8;event.y=8;input_event(&input,&event);
    ui_begin(&ui,&input);assert(!ui_button(&ui,&fb,1,rect,"OK"));
    input_begin(&input);event.type=PLAT_EV_MOUSE_UP;input_event(&input,&event);
    ui_begin(&ui,&input);assert(ui_button(&ui,&fb,1,rect,"OK"));
    input_begin(&input);event.type=PLAT_EV_MOUSE_DOWN;input_event(&input,&event);
    ui_begin(&ui,&input);assert(!ui_text_input(&ui,&fb,2,rect,text,sizeof(text)));
    input_begin(&input);event.type=PLAT_EV_TEXT;memcpy(event.text,"\xc3\xa9",3);input_event(&input,&event);
    ui_begin(&ui,&input);ui_text_input(&ui,&fb,2,rect,text,sizeof(text));assert(!strcmp(text,"A\xc3\xa9"));
    input_begin(&input);event.type=PLAT_EV_KEY_DOWN;event.key=PLAT_KEY_BACKSPACE;input_event(&input,&event);
    ui_begin(&ui,&input);ui_text_input(&ui,&fb,2,rect,text,sizeof(text));assert(!strcmp(text,"A"));
    input_begin(&input);event.key=PLAT_KEY_RETURN;input_event(&input,&event);
    ui_begin(&ui,&input);assert(ui_text_input(&ui,&fb,2,rect,text,sizeof(text)));
    fb_clear(&fb,0);fb_clip(&fb,(Rect){60,20,4,4});
    ui_text_input(&ui,&fb,2,rect,text,sizeof(text));assert(pixels[8*64+8]==0);
    puts("PASS UI: release-click, text input, UTF-8 backspace, Return, nested clipping");
}
int main(int argc,char **argv)
{
    const char *root=argc>1?argv[1]:"extracted";char path[4096];Image im={0};size_t i;unsigned histogram[256]={0};
    test_replay();test_ini();test_bmp_formats();test_framebuffer();test_ui();
    assert(snprintf(path,sizeof(path),"%s/worlds/Evergreen/maps/castle1.ter",root)>0);
    assert(!image_load(&im,path));assert(im.w==82 && im.h==87 && im.bpp==8 && im.indices);
    for(i=0;i<(size_t)im.w*im.h;++i) { ++histogram[im.indices[i]]; assert(im.pixels[i]==im.palette[im.indices[i]]); }
    printf("PASS retail terrain: %dx%d bpp=%d palette=%d index0=%u index1=%u\n",im.w,im.h,im.bpp,im.palette_size,histogram[0],histogram[1]);
    image_free(&im);snprintf(path,sizeof(path),"%s/art/title.jpg",root);assert(!image_load(&im,path));
    printf("PASS retail JPEG: %dx%d\n",im.w,im.h);image_free(&im);
    puts("engine_selftest: PASS"); return 0;
}
