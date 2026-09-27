#include "game_main.h"
#include "platform/platform.h"
#include "engine/log.h"
#include "engine/replay.h"
#include "engine/rng.h"
#include "engine/screen.h"
#include "engine/text.h"
#include "game/scene.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
static const char *data_path;
static const char *save_path;
static char default_save_path[4096];
static int quitting;
const char *game_data_path(void) { return data_path; }
const char *game_save_path(void) { return save_path; }
void game_request_quit(void) { quitting=1; }
static int unsigned_arg(const char *s,uint32_t *out)
{
    char *end; unsigned long n;
    if(!*s || *s=='-') return -1;
    errno=0; n=strtoul(s,&end,10);
    if(errno || *end || n>UINT32_MAX) return -1;
    *out=(uint32_t)n; return 0;
}
static uint64_t seen(const char *name,uint64_t since,void *user)
{ (void)user; return wos_log_seen_since(name,since); }
static void bmp_u32(unsigned char *p,uint32_t n)
{ p[0]=(unsigned char)n; p[1]=(unsigned char)(n>>8); p[2]=(unsigned char)(n>>16); p[3]=(unsigned char)(n>>24); }
static int save_shot(const Framebuffer *fb,const char *path)
{
    unsigned char header[54]={0},row[PLAT_SCREEN_W*3+3];
    size_t stride; int x,y,failed=0; FILE *file;
    if(fb->w<=0 || fb->w>PLAT_SCREEN_W || fb->h<=0 || fb->h>PLAT_SCREEN_H) return -1;
    stride=((size_t)fb->w*3+3)&~(size_t)3;
    header[0]='B'; header[1]='M'; bmp_u32(header+2,(uint32_t)(54+stride*fb->h));
    bmp_u32(header+10,54); bmp_u32(header+14,40);
    bmp_u32(header+18,(uint32_t)fb->w); bmp_u32(header+22,(uint32_t)fb->h);
    header[26]=1; header[28]=24; bmp_u32(header+34,(uint32_t)(stride*fb->h));
    file=plat_fopen(path,"wb");
    if(!file) return -1;
    if(fwrite(header,1,sizeof(header),file)!=sizeof(header)) failed=1;
    memset(row,0,stride);
    for(y=fb->h-1;y>=0 && !failed;--y) {
        for(x=0;x<fb->w;++x) {
            uint32_t c=fb->pixels[(size_t)y*fb->w+x];
            row[x*3]=(unsigned char)c; row[x*3+1]=(unsigned char)(c>>8); row[x*3+2]=(unsigned char)(c>>16);
        }
        if(fwrite(row,1,stride,file)!=stride) failed=1;
    }
    if(fclose(file)) failed=1;
    if(failed) return -1;
    wos_log_event("shot","path=%s",path); return 0;
}
static void usage(void)
{ fputs("Usage: wos --data DIR [--save DIR] [--headless] [--replay FILE] [--log FILE] [--seed N] [--max-frames N] [--shot-every N DIR]\n",stderr); }
int game_main(int argc,char **argv)
{
    const char *replay_path=NULL,*log_path=NULL,*shot_dir=NULL;
    uint32_t seed=1,max_frames=0,frame=0,next_tick,phase=0,shot_every=0;
    int capped=0,headless=0,i,result=0,initialized=0;
    char *script=NULL; Replay *replay=NULL; size_t error_line;
    uint32_t *pixels=NULL; Framebuffer fb; Input input={0};
    data_path=NULL; save_path=NULL; quitting=0;
    for(i=1;i<argc;++i) {
        const char *arg=argv[i];
        if(!strcmp(arg,"--headless")) { headless=1; continue; }
        if(i+1>=argc) { usage(); return 1; }
        if(!strcmp(arg,"--data")) data_path=argv[++i];
        else if(!strcmp(arg,"--save")) { save_path=argv[++i]; if(!*save_path) { usage(); return 1; } }
        else if(!strcmp(arg,"--replay")) replay_path=argv[++i];
        else if(!strcmp(arg,"--log")) log_path=argv[++i];
        else if(!strcmp(arg,"--seed")) { if(unsigned_arg(argv[++i],&seed)) { usage(); return 1; } }
        else if(!strcmp(arg,"--max-frames")) { capped=1; if(unsigned_arg(argv[++i],&max_frames)) { usage(); return 1; } }
        else if(!strcmp(arg,"--shot-every")) {
            if(i+2>=argc || unsigned_arg(argv[++i],&shot_every) || !shot_every) { usage(); return 1; }
            shot_dir=argv[++i]; if(!*shot_dir) { usage(); return 1; }
        }
        else { usage(); return 1; }
    }
    if(!data_path || !*data_path) { usage(); return 1; }
    if(!save_path) {
        int n=snprintf(default_save_path,sizeof(default_save_path),"%s/Save",data_path);
        if(n<0 || (size_t)n>=sizeof(default_save_path)) { fputs("Default save path is too long\n",stderr); return 1; }
        save_path=default_save_path;
    }
    if(wos_log_open(log_path)) { fputs("Cannot open event log\n",stderr); return 1; }
    if(replay_path) {
        script=text_read_file(replay_path,NULL); replay=malloc(sizeof(*replay));
        if(!script || !replay) { fputs("Cannot load replay\n",stderr); result=1; goto cleanup; }
        if(replay_parse(replay,script,&error_line)) {
            fprintf(stderr,"REPLAY FAIL parse line %lu\n",(unsigned long)error_line); result=2; goto cleanup;
        }
    }
    if(plat_init("Well of Souls",PLAT_SCREEN_W,PLAT_SCREEN_H,headless?PLAT_INIT_HEADLESS:0)) {
        fputs("Platform initialization failed\n",stderr); result=1; goto cleanup;
    }
    initialized=1; pixels=malloc(PLAT_SCREEN_W*PLAT_SCREEN_H*sizeof(*pixels));
    if(!pixels) { result=1; goto cleanup; }
    if(shot_dir && plat_mkdir(shot_dir)) { fputs("Cannot create screenshot directory\n",stderr); result=1; goto cleanup; }
    fb_init(&fb,pixels,PLAT_SCREEN_W,PLAT_SCREEN_H); rng_seed(game_rng(),seed);
    if(game_boot()) { result=1; goto cleanup; }
    next_tick=plat_ticks_ms();
    while(!quitting) {
        PlatEvent event; const Screen *screen;
        input_begin(&input);
        while(plat_poll_event(&event)) if(!replay || event.type==PLAT_EV_QUIT) input_event(&input,&event);
        if(input.quit) break;
        if(replay) {
            PlatEvent events[REPLAY_EVENTS_MAX]; size_t n,j;
            int status=replay_step(replay,events,&n,seen,NULL);
            if(status==2) { fprintf(stderr,"REPLAY FAIL expect %s\n",replay->failed_event); result=2; break; }
            if(status==1) break;
            for(j=0;j<n;++j) input_event(&input,&events[j]);
        }
        if(capped && frame>=max_frames) { result=3; break; }
        scene_tick(); /* global quest TIMER/COUNTDOWN clock, independent of the current screen */
        screen=screen_current(); if(screen && screen->update) screen->update(&input);
        fb_reset_clip(&fb); fb_clear(&fb,0);
        screen=screen_current(); if(screen && screen->render) screen->render(&fb);
        if(replay && replay->shot_path && save_shot(&fb,replay->shot_path)) {
            fprintf(stderr,"Cannot write screenshot: %s\n",replay->shot_path); result=1; break;
        }
        plat_present(fb.pixels,fb.w,fb.h); ++frame;
        if(shot_every && frame%shot_every==0) {
            char path[4096]; int n=snprintf(path,sizeof(path),"%s/frame_%06lu.bmp",shot_dir,(unsigned long)frame);
            if(n<0 || (size_t)n>=sizeof(path) || save_shot(&fb,path)) {
                fputs("Cannot write periodic screenshot\n",stderr); result=1; break;
            }
        }
        if(!replay) {
            uint32_t now;
            phase+=1000; next_tick+=phase/60; phase%=60; now=plat_ticks_ms();
            if((int32_t)(next_tick-now)>0) plat_sleep_ms(next_tick-now);
            /* Limit catch-up after suspension without changing simulation step size. */
            else if((uint32_t)(now-next_tick)>250) next_tick=now;
        }
    }
cleanup:
    screen_set(NULL); free(pixels); free(replay); free(script);
    if(initialized) plat_shutdown();
    wos_log_close(); return result;
}
