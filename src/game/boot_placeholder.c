/* Temporary engine boot screen. The game-flow module will replace this file. */
#ifdef WOS_BOOT_PLACEHOLDER
#include "../game_main.h"
#include "../engine/screen.h"
#include "../engine/font.h"
#include "../engine/image.h"
#include "../engine/log.h"
#include <stdio.h>
static Image title;
static void enter(void) { wos_log_event("boot_menu","image=art/title.jpg"); }
static void update(const Input *input)
{ if(input->pressed[PLAT_KEY_ESCAPE]) game_request_quit(); }
static void render(Framebuffer *fb)
{
    fb_blit(fb,&title,(fb->w-title.w)/2,(fb->h-title.h)/2,-1);
    font_draw(fb,16,456,"WELL OF SOULS - ESC TO EXIT",0xffdc80);
}
static void leave(void) { image_free(&title); }
static const Screen boot={"boot",enter,update,render,leave};
int game_boot(void)
{
    char path[4096]; int n=snprintf(path,sizeof(path),"%s/art/title.jpg",game_data_path());
    if(n<0 || (size_t)n>=sizeof(path) || image_load(&title,path)) {
        fputs("Cannot load art/title.jpg\n",stderr); return -1;
    }
    screen_set(&boot); return 0;
}
#endif
