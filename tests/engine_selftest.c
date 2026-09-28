#include "../src/engine/clock.h"
#include "../src/engine/encint.h"
#include "../src/engine/rng.h"
#include "../src/engine/dscript.h"
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
static uint64_t observed(const char *name,uint64_t since,void *user)
{ return (!strcmp(name,"ready") && *(uint64_t *)user>since) ? *(uint64_t *)user : 0; }
static void test_replay(void)
{
    char script[]="# comment\nexpect ready 2\nkey RETURN\nhold LEFT 3\nclick 12 34 3\ntext Hero Name\nwait 2\nquit\n";
    char timeout[]="expect ready 1\nexpect ready 1\n";
    char bad[]="wait -1\n",hash[]="key #\n";
    char shot[]="shot /tmp/with space.bmp\nwait 1\nshot final.bmp\n",bad_shot[]="shot \n";
    size_t line,n; PlatEvent ev[REPLAY_EVENTS_MAX]; uint64_t serial=1;
    assert(!replay_parse(&rp,script,&line));
    assert(!replay_step(&rp,ev,&n,observed,&serial));
    assert(n==1 && ev[0].type==PLAT_EV_KEY_DOWN && ev[0].key==PLAT_KEY_RETURN);
    assert(!replay_step(&rp,ev,&n,observed,&serial)); assert(n==1 && ev[0].type==PLAT_EV_KEY_UP);
    assert(!replay_step(&rp,ev,&n,observed,&serial)); assert(n==1 && ev[0].key==PLAT_KEY_LEFT);
    assert(!replay_step(&rp,ev,&n,observed,&serial)); assert(n==0);
    assert(!replay_step(&rp,ev,&n,observed,&serial)); assert(n==0);
    assert(!replay_step(&rp,ev,&n,observed,&serial)); assert(n==1 && ev[0].type==PLAT_EV_KEY_UP);
    assert(!replay_step(&rp,ev,&n,observed,&serial));
    assert(n==2 && ev[0].type==PLAT_EV_MOUSE_MOVE && ev[1].button==3 && ev[1].x==12 && ev[1].y==34);
    assert(!replay_step(&rp,ev,&n,observed,&serial)); assert(n==1 && ev[0].type==PLAT_EV_MOUSE_UP);
    assert(!replay_step(&rp,ev,&n,observed,&serial)); assert(n==1 && !strcmp(ev[0].text,"Hero Name"));
    assert(!replay_step(&rp,ev,&n,observed,&serial)); assert(n==0);
    assert(!replay_step(&rp,ev,&n,observed,&serial)); assert(n==0);
    assert(replay_step(&rp,ev,&n,observed,&serial)==1);
    assert(!replay_parse(&rp,timeout,&line));
    assert(!replay_step(&rp,ev,&n,observed,&serial));
    assert(replay_step(&rp,ev,&n,observed,&serial)==2);
    assert(!strcmp(rp.failed_event,"ready"));
    assert(replay_parse(&rp,bad,&line)==-1 && line==1);
    assert(!replay_parse(&rp,hash,&line)); assert(rp.commands[0].key=='#');
    assert(replay_key("F12")==PLAT_KEY_F12 && replay_key("F13")==-1);
    assert(!replay_parse(&rp,shot,&line));
    assert(!replay_step(&rp,ev,&n,observed,&serial));
    assert(n==0 && !strcmp(rp.shot_path,"/tmp/with space.bmp"));
    assert(!replay_step(&rp,ev,&n,observed,&serial)); assert(!rp.shot_path);
    assert(!replay_step(&rp,ev,&n,observed,&serial)); assert(!strcmp(rp.shot_path,"final.bmp"));
    assert(replay_step(&rp,ev,&n,observed,&serial)==1 && !rp.shot_path);
    assert(replay_parse(&rp,bad_shot,&line)==-1 && line==1);
    puts("PASS replay shot: spaced paths, end-of-frame request, one-shot reset, final command, missing path");
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
/* --- virtual clock and Win32 timer emulation (src/engine/clock.c) -------------
 * Contract under test: the 20 Hz gate re-arms from the PREVIOUS BOUNDARY, so a
 * clock jump of N ms costs floor(N/20) idle ticks and never skips one, and a
 * timer that is k intervals overdue fires ONCE (Win32 coalescing). */
static int idle_count, fired_count;
static int fired_log[64];
static void on_fast(void *owner, void *user) { (void)owner; (void)user; if (fired_count < 64) fired_log[fired_count++] = 100; }
static void on_slow(void *owner, void *user) { (void)owner; (void)user; if (fired_count < 64) fired_log[fired_count++] = 1000; }
static void on_none(void *owner, void *user) { (void)owner; (void)user; }
static void test_clock(void)
{
    uint32_t idle20;
    clock_reset();
    assert(clock_ms() == 0);
    /* clock_time_s() is the WALL CLOCK, not the virtual one: the original derives
     * none of its time() reads from GetTickCount. With the clock pinned, it is the
     * pin; unpinned, it follows the host. */
    assert(clock_time_s() == plat_time_s());
    /* The gate is the original's `GetTickCount() - last > 19`: 20 ms, not 16.7. */
    assert(!clock_idle_due());
    clock_advance(19);
    assert(!clock_idle_due());
    clock_advance(1);
    assert(clock_idle_due());
    assert(!clock_idle_due());
    /* A stall costs ONE idle tick, not floor(N/20): FUN_0040a7c7 re-stamps with the
     * OBSERVED time (`_DAT_004dd510 = GetTickCount()`) and never replays the boundaries
     * it slept through, so the differential harness has to agree even on a spin-wait
     * path where the virtual clock jumps. */
    idle_count = 0;
    clock_advance(1000);
    while (clock_idle_due()) if (++idle_count == 200) break;
    assert(idle_count == 1);
    /* Stepping onto each boundary, which is what the script loop does, still gives
     * exactly one tick per 20 ms: 50 over the closed interval [0,1000]. */
    clock_reset();
    idle_count = 0;
    for (idle20 = 0; idle20 <= 1000; idle20 = clock_20hz_next()) {
        clock_set_now(idle20);
        if (clock_idle_due()) ++idle_count;
    }
    assert(idle_count == 50);
    /* The gate never runs backwards, and clock_20hz_next() is the next boundary. */
    clock_set_now(10);
    assert(clock_ms() == 1000);
    idle20 = clock_20hz_next();
    assert(idle20 == clock_ms() + 20);   /* the next boundary, never the current one */

    /* Timers: Win32 clamps uElapse to USER_TIMER_MINIMUM (10 ms). */
    clock_reset();
    assert(!clock_timer_active((void *)0x1, 2));
    clock_set_timer((void *)0x1, 2, 0, on_none, NULL);
    assert(clock_timer_active((void *)0x1, 2));
    clock_kill_timer((void *)0x1, 2);
    assert(!clock_timer_active((void *)0x1, 2));

    /* Overdue timers walk forward one interval per message (the rule agreed with
     * the Oracle): a SetTimer(hwnd,2,100) idle until t=300 delivers at the
     * deadlines 100, 200 and 300 -- never a replayed burst at a single stamp --
     * and the clock ends exactly on the last deadline. */
    clock_reset();
    clock_set_timer((void *)0x1, 2, 100, on_fast, NULL);
    clock_advance(300);
    fired_count = 0;
    assert(clock_dispatch_timers() == 3);
    assert(fired_count == 3);
    assert(clock_ms() == 300);
    assert(!clock_dispatch_timers());          /* caught up: nothing left overdue */

    /* A timer 3x over due by only one interval fires once, not three times. */
    clock_reset();
    clock_set_timer((void *)0x1, 2, 1000, on_slow, NULL);
    clock_advance(1100);
    fired_count = 0;
    assert(clock_dispatch_timers() == 1);
    assert(fired_count == 1);
    assert(clock_ms() == 1100);   /* the clock never rewinds to the deadline */

    /* Ordering: oldest deadline first, whichever was registered first. The 1000 ms
     * timer is armed before the 100 ms one but must still fire second. */
    clock_reset();
    clock_set_timer((void *)0x1, 4, 1000, on_slow, NULL);
    clock_set_timer((void *)0x1, 2, 100, on_fast, NULL);
    fired_count = 0;
    clock_advance(100);
    assert(clock_dispatch_timers() == 1);
    assert(fired_count == 1 && fired_log[0] == 100);
    clock_kill_timer((void *)0x1, 2);
    clock_advance(900);
    assert(clock_dispatch_timers() == 1);
    assert(fired_count == 2 && fired_log[1] == 1000);
    /* Equal deadlines break by registration order. */
    clock_reset();
    clock_set_timer((void *)0x1, 2, 100, on_slow, NULL);
    clock_set_timer((void *)0x1, 2 + 1000, 100, on_fast, NULL);
    clock_advance(100);
    fired_count = 0;
    assert(clock_dispatch_timers() == 2);
    assert(fired_count == 2 && fired_log[0] == 1000 && fired_log[1] == 100);

    /* A killed timer stops the backlog walk. */
    clock_reset();
    clock_set_timer((void *)0x1, 2, 10, on_none, NULL);
    clock_advance(1000);
    assert(clock_dispatch_timers() >= 1);
    clock_kill_timer((void *)0x1, 2);
    assert(clock_dispatch_timers() == 0);

    /* time() tracks the virtual clock and the epoch base. */
    clock_reset();
    clock_set_time_base(1234567890);
    assert(clock_time_s() == 1234567890);
    clock_advance(2500);        /* the virtual clock must NOT drag time() with it */
    assert(clock_time_s() == 1234567890);
    assert(clock_ms() == 2500);
    clock_set_now(1);           /* never rewinds */
    assert(clock_ms() == 2500);
    clock_set_time_base(0);     /* unpin: back to the host wall clock */
    puts("PASS clock: 20 ms gate (one tick per stall, one per 20 ms stepped boundary), timers");
}

/* --- .dsc parser (src/engine/dscript.c) ------------------------------------- */
static void test_dscript(void)
{
    Dscript ds;
    char good[] =
        "# comment\n"
        "at 0 key RETURN\n"
        "at 100 click 320 240 3\n"
        "at 150 move 10 20\n"
        "at 200 down 5 6 1\n"
        "at 250 up 5 6 1\n"
        "at 300 rclick 1 2\n"
        "at 400 text Hero Name\n"
        "at 500 dialog 138 1000=1 1001=2 ok\n"
        "at 600 dialog 149 1000=3 cancel\n"
        "at 700 dump hero\n"
        "end 1000\n";
    char no_at[]="click 1 2\n";
    char neg[]="at -1 key a\n";
    char badkind[]="at 5 teleport 1\n";
    char badpair[]="at 5 dialog 1 1000 ok\n";
    char badbtn[]="at 5 click 1 2 9\n";
    char badend[]="end\n";
    char okfine[]="at 5 dialog 1 ok\n";
    size_t line=0;
    const DscriptOp *op;

    assert(!dscript_parse(&ds,good,&line));
    assert(ds.count == 10 && ds.has_end && ds.end_ms == 1000);
    assert(ds.ops[0].kind==DS_KEY && ds.ops[0].at_ms==0 && ds.ops[0].key==PLAT_KEY_RETURN);
    assert(ds.ops[1].kind==DS_CLICK && ds.ops[1].x==320 && ds.ops[1].y==240 && ds.ops[1].button==3);
    assert(ds.ops[2].kind==DS_MOVE && ds.ops[2].x==10 && ds.ops[2].y==20 && ds.ops[2].button==0);
    assert(ds.ops[3].kind==DS_DOWN && ds.ops[4].kind==DS_UP);
    assert(ds.ops[5].kind==DS_RCLICK && ds.ops[5].button==3);
    assert(ds.ops[6].kind==DS_TEXT && !strcmp(ds.ops[6].text,"Hero Name"));
    assert(ds.ops[7].kind==DS_DIALOG && ds.ops[7].id==138 && ds.ops[7].ok==1 &&
           ds.ops[7].control_count==2 && ds.ops[7].control[1]==1001 && ds.ops[7].value[1]==2);
    assert(ds.ops[8].ok==0);
    assert(ds.ops[9].kind==DS_DUMP && !strcmp(ds.ops[9].text,"hero"));

    /* The schedule is consumed in file order, never pre-empted out of order. */
    assert(dscript_next_time(&ds,0) == 0);
    assert(dscript_next_time(&ds,1) == 100);
    op = dscript_take(&ds,0); assert(op && op->kind==DS_KEY);
    assert(dscript_take(&ds,0) == NULL);
    assert(dscript_take(&ds,99) == NULL);
    op = dscript_take(&ds,100); assert(op && op->kind==DS_CLICK);
    while (dscript_take(&ds,1000)) { }
    assert(ds.pc == ds.count);
    assert(dscript_next_time(&ds,1000) == 1000);   /* the `end` still terminates */

    assert(dscript_parse(&ds,no_at,&line) && line==1);
    assert(dscript_parse(&ds,neg,&line) && line==1);
    assert(dscript_parse(&ds,badkind,&line) && line==1);
    assert(dscript_parse(&ds,badpair,&line) && line==1);
    assert(dscript_parse(&ds,badbtn,&line) && line==1);
    assert(dscript_parse(&ds,badend,&line) && line==1);
    assert(!dscript_parse(&ds,okfine,&line));   /* `ok` with no controls is legal */
    puts("PASS dscript: ops, dialog pairs, schedule order, rejection cases");
}

/* --- EncInt (src/engine/encint.c) ---------------------------------------------
 * Contract: set/clear/add each consume exactly four crt_rand() in k0,k1,k2,k3
 * order, get consumes none, and the three doubles are re-derived from v. */
static void test_encint(void)
{
    EncInt e;
    uint64_t before, after;
    crt_srand(1);
    memset(&e, 0, sizeof(e));
    before = crt_rand_calls();
    enc_clear(&e);
    after = crt_rand_calls();
    assert(after - before == 4);          /* FUN_0049B734 */
    assert(e.v == 0 && e.d0 == 0.0 && e.d1 == 0.0 && e.d2 == 0.0);
    assert(!enc_cheat_flag());
    assert(enc_valid(&e));
    before = crt_rand_calls();
    enc_set(&e, 7);
    assert(crt_rand_calls() - before == 4);   /* FUN_0049B71B */
    assert(e.d0 == (double)7 * ENCINT_C0);
    assert(e.d1 == (double)7 * ENCINT_C1);
    assert(e.d2 == (double)7 * ENCINT_C2);
    before = crt_rand_calls();
    assert(enc_get(&e) == 7);
    assert(crt_rand_calls() - before == 0);   /* FUN_0049B70F reads nothing */
    before = crt_rand_calls();
    assert(enc_add(&e, 5) == 12);
    assert(crt_rand_calls() - before == 4);   /* FUN_0049B73F: a get then a set */
    /* The keys are write-only: tampering with one is NOT detected, tampering with
     * a double is. That asymmetry is the point of the object. */
    e.k0 = 12345;
    assert(enc_get(&e) == 12);
    assert(!enc_cheat_flag());
    e.d0 = 1.0;
    assert(!(e.d0 == (double)12 * ENCINT_C0));
    enc_cheat_clear();
    puts("PASS encint: 4 rands per set/clear/add, 0 on get, doubles seal v not the keys");
}

int main(int argc,char **argv)
{
    const char *root=argc>1?argv[1]:"extracted";char path[4096];Image im={0};size_t i;unsigned histogram[256]={0};
    test_replay();test_clock();test_dscript();test_encint();test_ini();test_bmp_formats();test_framebuffer();test_ui();
    assert(snprintf(path,sizeof(path),"%s/worlds/Evergreen/maps/castle1.ter",root)>0);
    assert(!image_load(&im,path));assert(im.w==82 && im.h==87 && im.bpp==8 && im.indices);
    for(i=0;i<(size_t)im.w*im.h;++i) { ++histogram[im.indices[i]]; assert(im.pixels[i]==im.palette[im.indices[i]]); }
    printf("PASS retail terrain: %dx%d bpp=%d palette=%d index0=%u index1=%u\n",im.w,im.h,im.bpp,im.palette_size,histogram[0],histogram[1]);
    image_free(&im);snprintf(path,sizeof(path),"%s/art/title.jpg",root);assert(!image_load(&im,path));
    printf("PASS retail JPEG: %dx%d\n",im.w,im.h);image_free(&im);
    puts("engine_selftest: PASS"); return 0;
}
