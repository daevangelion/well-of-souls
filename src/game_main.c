#include "game_main.h"
#include "platform/platform.h"
#include "engine/log.h"
#include "engine/replay.h"
#include "engine/rng.h"
#include "engine/screen.h"
#include "engine/text.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
static const char *data_path;
static int quitting;
const char *game_data_path(void) { return data_path; }
void game_request_quit(void) { quitting=1; }
static int unsigned_arg(const char *s,uint32_t *out)
{
    char *end; unsigned long n;
    if(!*s || *s=='-') return -1;
    errno=0; n=strtoul(s,&end,10);
    if(errno || *end || n>UINT32_MAX) return -1;
    *out=(uint32_t)n; return 0;
}
static int seen(const char *name,uint64_t since,void *user)
{ (void)user; return wos_log_seen_since(name,since); }
static void usage(void)
{ fputs("Usage: wos --data DIR [--headless] [--replay FILE] [--log FILE] [--seed N] [--max-frames N]\n",stderr); }
int game_main(int argc,char **argv)
{
    const char *replay_path=NULL,*log_path=NULL;
    uint32_t seed=1,max_frames=0,frame=0,next_tick,phase=0;
    int capped=0,headless=0,i,result=0,initialized=0;
    char *script=NULL; Replay *replay=NULL; size_t error_line;
    uint32_t *pixels=NULL; Framebuffer fb; Input input={0};
    data_path=NULL; quitting=0;
    for(i=1;i<argc;++i) {
        const char *arg=argv[i];
        if(!strcmp(arg,"--headless")) { headless=1; continue; }
        if(i+1>=argc) { usage(); return 1; }
        if(!strcmp(arg,"--data")) data_path=argv[++i];
        else if(!strcmp(arg,"--replay")) replay_path=argv[++i];
        else if(!strcmp(arg,"--log")) log_path=argv[++i];
        else if(!strcmp(arg,"--seed")) { if(unsigned_arg(argv[++i],&seed)) { usage(); return 1; } }
        else if(!strcmp(arg,"--max-frames")) { capped=1; if(unsigned_arg(argv[++i],&max_frames)) { usage(); return 1; } }
        else { usage(); return 1; }
    }
    if(!data_path || !*data_path) { usage(); return 1; }
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
            int status=replay_step(replay,events,&n,wos_log_serial(),seen,NULL);
            if(status==2) { fprintf(stderr,"REPLAY FAIL expect %s\n",replay->failed_event); result=2; break; }
            if(status==1) break;
            for(j=0;j<n;++j) input_event(&input,&events[j]);
        }
        if(capped && frame>=max_frames) { result=3; break; }
        screen=screen_current(); if(screen && screen->update) screen->update(&input);
        fb_reset_clip(&fb); fb_clear(&fb,0);
        screen=screen_current(); if(screen && screen->render) screen->render(&fb);
        plat_present(fb.pixels,fb.w,fb.h); ++frame;
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
