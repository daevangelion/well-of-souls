/* WoS A96 solo front end: docs/re/boot_flow.md sections 1-3 and 5.
 * Deliberate port deviations: MFC modeless/modal windows are framebuffer panels;
 * hotspot text uses the bundled bitmap font instead of installed Windows fonts.
 * Online registration, Haunt and the 'Place Yourself On Gaiea' network prompt
 * are not part of solo play. New souls incarnate without the PK/follow-up dialogs.
 * Saves are the portable .wsh format, never the retail .her record.
 */
#include "game.h"
#include "hero.h"
#include "../game_main.h"
#include "../engine/screen.h"
#include "../engine/font.h"
#include "../engine/text.h"
#include "../engine/ini.h"
#include "../engine/log.h"
#include "../platform/platform.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

#define MAX_CHOICES 100
#define ROW_HEIGHT 18

enum { TITLE, MENU, WORLDS, WELL, NEW_SOUL };
static int state, ticks, selected_world, world_count, selected_soul, soul_count;
static int selected_class, class_count, selected_gender, gender_count, name_focus;
static int class_ids[WORLD_MAX_CLASSES], gender_ids[4];
static char worlds[MAX_CHOICES][64], souls[MAX_CHOICES][HERO_NAME_MAX];
static char genders[4][64], name[HERO_NAME_MAX], message[160];
static Image background, buttons[5];
static Sheet portrait;
static Input last_input;
static const Screen front_screen;
static const char *button_names[5] = {"Incarnate", "Haunt", "New", "Restore", "Map"};
static const Rect name_rect={104,88,432,26}, class_rect={104,146,264,180};
static const Rect ok_rect={354,398,86,28}, cancel_rect={450,398,86,28};

static int inside(const Input *in,Rect r)
{ return in->mouse_x>=r.x && in->mouse_y>=r.y && in->mouse_x<r.x+r.w && in->mouse_y<r.y+r.h; }
static int clicked(const Input *in,Rect r) { return (in->mouse_pressed&2u) && inside(in,r); }
static Rect hotspot(int x,int y,const char *label)
{ Rect r={x*640/1000,y*480/1000,font_width(label)+12,24}; return r; }
static Rect bar_rect(int slot) { Rect r={568-slot*51,8,48,48}; return r; }

/* Scale on loading, not in every render. The source is never mutated. */
static int load_background(const char *path,int w,int h)
{
    Image src={0}, scaled={0}; int x,y;
    image_free(&background);
    if(image_load(&src,path)) return -1;
    scaled.w=w; scaled.h=h;
    scaled.pixels=malloc((size_t)w*(size_t)h*sizeof(*scaled.pixels));
    if(!scaled.pixels) { image_free(&src); return -1; }
    for(y=0;y<h;++y) for(x=0;x<w;++x)
        scaled.pixels[y*w+x]=src.pixels[(int)((int64_t)y*src.h/h)*src.w+(int)((int64_t)x*src.w/w)];
    image_free(&src); background=scaled; return 0;
}
static void art_background(const char *file)
{
    char path[768]; snprintf(path,sizeof(path),"%s/art/%s",game_data_path(),file);
    load_background(path,640,480);
}
static void menu(void)
{
    state=MENU; ticks=0; message[0]=0; art_background("beg.jpg");
    wos_log_event("boot_menu","");
}
static int compare_worlds(const void *a,const void *b) { return text_casecmp(a,b); }
static void world_entry(const char *entry,int is_dir,void *user)
{
    (void)user;
    if(is_dir && world_count<MAX_CHOICES && strlen(entry)<sizeof(worlds[0]))
        snprintf(worlds[world_count++],sizeof(worlds[0]),"%s",entry);
}
static void choose_world(void)
{
    char path[768]; state=WORLDS; ticks=0; world_count=0; selected_world=0;
    art_background("where.jpg"); snprintf(path,sizeof(path),"%s/worlds",game_data_path());
    plat_list_dir(path,world_entry,NULL);
    qsort(worlds,(size_t)world_count,sizeof(worlds[0]),compare_worlds);
    if(!world_count) snprintf(message,sizeof(message),"No worlds found in the data directory.");
}
static void load_portrait(void)
{
    sheet_free(&portrait);
    if(g_hero.valid) sheet_load_skin(&portrait,g_hero.skin);
}
static void load_selected(void)
{
    if(selected_soul>=0 && selected_soul<soul_count) {
        if(hero_load(&g_hero,souls[selected_soul])) {
            g_hero.valid=0; sheet_free(&portrait);
            snprintf(message,sizeof(message),"Cannot load that soul.");
        }
        else { message[0]=0; load_portrait(); }
    }
}
static void well_background(void)
{
    char path[768], rel[256], tok[16][256]; const char *scene="temple";
    if(g_world.scenes[0].used && world_tokenize(g_world.lines[g_world.scenes[0].first_line],tok,16)>2)
        scene=tok[2];
    snprintf(rel,sizeof(rel),"scenes/%.220s%s",scene,strchr(scene,'.')?"":".jpg");
    world_path(path,sizeof(path),rel);
    if(load_background(path,364,416)) { world_data_path(path,sizeof(path),rel); load_background(path,364,416); }
}
void game_go_well(void)
{
    int i; char rel[128], path[768];
    state=WELL; message[0]=0; soul_count=hero_list_saves(souls,MAX_CHOICES);
    selected_soul=soul_count?0:-1;
    if(g_hero.valid) for(i=0;i<soul_count;++i) if(!text_casecmp(souls[i],g_hero.name)) selected_soul=i;
    well_background();
    for(i=0;i<5;++i) {
        snprintf(rel,sizeof(rel),"art/button%s.bmp",button_names[i]);
        world_data_path(path,sizeof(path),rel); image_load(&buttons[i],path);
    }
    load_selected(); screen_set(&front_screen);
}
static void incarnate(void)
{
    const ClassDef *c;
    if(!g_hero.valid) { snprintf(message,sizeof(message),"Pick a Soul, then INCARNATE."); return; }
    c=&g_world.classes[g_hero.klass];
    g_hero.hp=g_hero.max_hp; g_hero.mp=g_hero.max_mp;
    wos_log_event("hero_ready","name=%s class=%d",g_hero.name,g_hero.klass);
    game_enter_map(c->start_location_set?c->start_map:0,
                   c->start_location_set?c->start_link:0,
                   c->start_location_set?c->start_drop_in:0);
}
static void new_soul(void)
{
    char path[768], section[8]; char *text; Ini ini; int i;
    state=NEW_SOUL; name[0]=0; message[0]=0; name_focus=1;
    class_count=0; selected_class=0; selected_gender=0; gender_count=0;
    for(i=1;i<WORLD_MAX_CLASSES;++i)
        if(g_world.classes[i].used && !g_world.classes[i].hidden) class_ids[class_count++]=i;
    world_path(path,sizeof(path),"gender.ini"); text=text_read_file(path,NULL);
    if(text && !ini_parse(&ini,text)) for(i=0;i<4;++i) {
        const char *s; snprintf(section,sizeof(section),"%d",i); s=ini_get(&ini,section,"menu","");
        if(*s && strcmp(s,".")) { gender_ids[gender_count]=i; snprintf(genders[gender_count++],64,"%s",s); }
    }
    free(text);
    if(!gender_count) { gender_count=1; gender_ids[0]=0; snprintf(genders[0],64,"Unspecified"); }
}
static const char *validate_name(void)
{
    /* FUN_00460805's punctuation comparison is '<', despite the reversed
     * sign in boot_flow.md 3d. Table strings below are Souls.exe 0x505188. */
    static const char *const bad[]={"fuck","f.u.c.k","f u c k","f uck","fuc k","fuc*","fuk","f.uck","f*ck","phuck","shit","sh1t","sh*t","sh!t"," shi t"," s hit","cunt","kunt"," nigger"," nigga"," cock","bitch","b1tch","b!tch","b*tch"," b itch","bitc h","biatch","biotch"," rape yo"," rape u","suck my","suk my","suck his","suck ass","masterbat","masturbat"};
    char folded[HERO_NAME_MAX], *trim=text_trim(name); size_t i,j,n; int letters=0,spaces=0;
    if(trim!=name) memmove(name,trim,strlen(trim)+1);
    n=strlen(name);
    if(n<2) return "Too Short!";
    for(i=0;i<n;++i) {
        unsigned char c=(unsigned char)name[i];
        if(c>=128 || c<32 || strchr("\"'`()*, -./:;<>?[]|~\\",c)) {
            if(c!=' ') name[i]='_';
        }
        if(isalpha((unsigned char)name[i])) ++letters;
        if(name[i]==' ') ++spaces;
        folded[i]=(char)tolower((unsigned char)name[i]);
    }
    folded[n]=0;
    if(letters<2) return "Too Few Letters!";
    if(letters+4<(int)n-letters) return "Too Much Punctuation!";
    if(spaces>1) return "Too Many Spaces!";
    for(j=0;j<sizeof(bad)/sizeof(bad[0]);++j) if(strstr(folded,bad[j])) return "Too Naughty!";
    if(!text_casecmp(name,"Samsyn") || !text_casecmp(name,"Uncle Dan")) return "Reserved Name";
    for(i=0;i<(size_t)soul_count;++i) if(!text_casecmp(name,souls[i])) return "A soul already exists in this world.";
    return NULL;
}
static void create_soul(void)
{
    const char *error=validate_name();
    if(error) { snprintf(message,sizeof(message),"%s",error); return; }
    if(!class_count) { snprintf(message,sizeof(message),"This world has no playable classes."); return; }
    hero_create(&g_hero,name,class_ids[selected_class],gender_ids[selected_gender],NULL);
    if(!g_hero.valid || hero_save(&g_hero)) { snprintf(message,sizeof(message),"Could not save your soul."); return; }
    incarnate();
}
static int first_row(int selection,int rows) { return selection>=rows ? selection-rows+1 : 0; }
static void select_list(const Input *in,Rect r,int count,int *selection)
{
    int rows=r.h/ROW_HEIGHT, first=first_row(*selection,rows);
    if(clicked(in,r)) {
        int n=first+(in->mouse_y-r.y)/ROW_HEIGHT;
        if(n<count) *selection=n;
    }
    if(inside(in,r) && in->wheel) {
        *selection-=in->wheel;
        if(*selection<0) *selection=0;
        if(*selection>=count) *selection=count-1;
    }
}
static void front_update(const Input *in)
{
    int i; last_input=*in; ++ticks;
    if(state==TITLE) {
        int key=0; for(i=0;i<INPUT_KEYS;++i) key|=in->pressed[i];
        if(ticks>=780 || in->mouse_pressed || key) menu();
    } else if(state==MENU) {
        if(in->pressed[PLAT_KEY_RETURN] || clicked(in,hotspot(124,250,"Play now, it's free!"))) choose_world();
        else if(clicked(in,hotspot(372,750,"Depart this realm")) || in->pressed[PLAT_KEY_ESCAPE]) game_request_quit();
    } else if(state==WORLDS) {
        Rect r={96,140,448,216};
        if(in->pressed[PLAT_KEY_ESCAPE]) { menu(); return; }
        if(in->pressed[PLAT_KEY_UP] && selected_world>0) --selected_world;
        if(in->pressed[PLAT_KEY_DOWN] && selected_world+1<world_count) ++selected_world;
        select_list(in,r,world_count,&selected_world);
        if(world_count && (in->pressed[PLAT_KEY_RETURN] || clicked(in,r))) {
            if(world_load(game_data_path(),worlds[selected_world])) snprintf(message,sizeof(message),"Could not load that world.");
            else { memset(&g_hero,0,sizeof(g_hero)); game_go_well(); }
        }
    } else if(state==WELL) {
        int old=selected_soul;
        select_list(in,(Rect){376,106,232,180},soul_count,&selected_soul);
        if(in->pressed[PLAT_KEY_UP] && selected_soul>0) --selected_soul;
        if(in->pressed[PLAT_KEY_DOWN] && selected_soul+1<soul_count) ++selected_soul;
        if(old!=selected_soul) load_selected();
        if(in->pressed['n'] || clicked(in,bar_rect(2)) || clicked(in,(Rect){376,300,232,28})) new_soul();
        else if(in->pressed['i'] || clicked(in,bar_rect(0)) || clicked(in,(Rect){376,336,232,28})) incarnate();
        else if(clicked(in,bar_rect(3))) { soul_count=hero_list_saves(souls,MAX_CHOICES); selected_soul=soul_count?0:-1; load_selected(); }
        else if(clicked(in,bar_rect(1))) snprintf(message,sizeof(message),"Haunting other players is an online feature.");
        else if(clicked(in,bar_rect(4))) snprintf(message,sizeof(message),"Incarnate your soul to explore the world map.");
        else if(in->pressed[PLAT_KEY_ESCAPE]) { menu(); }
    } else if(state==NEW_SOUL) {
        if(in->pressed[PLAT_KEY_ESCAPE] || clicked(in,cancel_rect)) { state=WELL; message[0]=0; return; }
        if(clicked(in,name_rect)) name_focus=1;
        if(name_focus) {
            size_t len=strlen(name), add=strlen(in->text);
            if(in->pressed[PLAT_KEY_BACKSPACE] && len) name[--len]=0;
            if(add>sizeof(name)-1-len) add=sizeof(name)-1-len;
            memcpy(name+len,in->text,add); name[len+add]=0;
        }
        if(in->pressed[PLAT_KEY_UP] && selected_class>0) --selected_class;
        if(in->pressed[PLAT_KEY_DOWN] && selected_class+1<class_count) ++selected_class;
        select_list(in,class_rect,class_count,&selected_class);
        for(i=0;i<gender_count;++i) if(clicked(in,(Rect){384,146+i*24,152,22})) selected_gender=i;
        if(in->pressed[PLAT_KEY_RETURN] || clicked(in,ok_rect)) create_soul();
    }
}
static void label_button(Framebuffer *fb,Rect r,const char *text)
{
    fb_fill(fb,r,inside(&last_input,r)?0x62543b:0x343040); fb_rect(fb,r,0xbcad80);
    font_draw(fb,r.x+6,r.y+(r.h-8)/2,text,0xffffff);
}
static void menu_text(Framebuffer *fb,int x,int y,const char *text)
{
    Rect r=hotspot(x,y,text); font_draw(fb,r.x+2,r.y+10,text,0x101010);
    font_draw(fb,r.x,r.y+8,text,inside(&last_input,r)?0xffe080:0xffffff);
}
static void draw_list_row(Framebuffer *fb,Rect r,int row,const char *label,int selected)
{
    Rect old=fb->clip;
    Rect line={r.x,r.y+row*ROW_HEIGHT,r.w,ROW_HEIGHT};
    if(selected) fb_fill(fb,line,0x62543b);
    fb_clip_intersect(fb,line); font_draw(fb,line.x+4,line.y+5,label,selected?0xffdf80:0xffffff); fb->clip=old;
}
static void front_render(Framebuffer *fb)
{
    int i,first; char label[128];
    fb_clear(fb,0x151322); fb_blit(fb,&background,0,0,-1);
    if(state==TITLE) {
        menu_text(fb,95,750,"Well of Souls"); menu_text(fb,95,820,"Click or press any key");
    } else if(state==MENU) {
        menu_text(fb,62,125,"Well of Souls - Solo play");
        menu_text(fb,124,250,"Play now, it's free!");
        menu_text(fb,372,750,"Depart this realm");
    } else if(state==WORLDS) {
        Rect r={96,140,448,216};
        menu_text(fb,100,120,"Where Do You Want To Play Today?");
        fb_blend(fb,r,0x101018,200); first=first_row(selected_world,12);
        for(i=first;i<world_count && i<first+12;++i) draw_list_row(fb,r,i-first,worlds[i],i==selected_world);
        menu_text(fb,150,820,"Return: choose world    Escape: back");
    } else {
        fb_fill(fb,(Rect){364,0,276,416},0x272331);
        for(i=0;i<5;++i) {
            Rect r=bar_rect(i); int cell=inside(&last_input,r)?3:1;
            if(i==1 || (i==0 && !g_hero.valid)) cell=5;
            if(buttons[i].pixels) fb_blit_sub(fb,&buttons[i],(Rect){cell*48,0,48,48},r.x,r.y,0,buttons[i].pixels[(buttons[i].h-1)*buttons[i].w]);
            else label_button(fb,r,button_names[i]);
        }
        ui_panel(fb,(Rect){368,76,248,300},"Pick a Soul","");
        first=first_row(selected_soul,10);
        for(i=first;i<soul_count && i<first+10;++i) draw_list_row(fb,(Rect){376,106,232,180},i-first,souls[i],i==selected_soul);
        if(!soul_count) font_draw(fb,380,120,"Press NEW SOUL button",0xffdf80);
        label_button(fb,(Rect){376,300,232,28},"NEW SOUL (N)");
        label_button(fb,(Rect){376,336,232,28},"INCARNATE THIS SOUL (I)");
        if(g_hero.valid) {
            snprintf(label,sizeof(label),"%s  Level %d",g_hero.name,g_hero.level);
            font_draw(fb,12,376,label,0xffffff);
            sheet_draw(fb,&portrait,0,110,130,0);
            fb_fill(fb,(Rect){8,420,240,16},0x38151b);
            fb_fill(fb,(Rect){8,420,(int)((int64_t)240*g_hero.hp/g_hero.max_hp),16},0x8b2934);
            fb_fill(fb,(Rect){264,420,240,16},0x151d38);
            if(g_hero.max_mp) fb_fill(fb,(Rect){264,420,(int)((int64_t)240*g_hero.mp/g_hero.max_mp),16},0x294c9b);
            snprintf(label,sizeof(label),"HP %d/%d",g_hero.hp,g_hero.max_hp);
            font_draw(fb,12,424,label,0xffffff);
            snprintf(label,sizeof(label),"MP %d/%d",g_hero.mp,g_hero.max_mp);
            font_draw(fb,268,424,label,0xffffff);
        }
        if(state==NEW_SOUL) {
            ui_panel(fb,(Rect){88,52,464,386},"New Soul","");
            font_draw(fb,104,74,"Name",0xffffff);
            fb_fill(fb,name_rect,0x101018); fb_rect(fb,name_rect,0xffdf80); font_draw(fb,110,97,name,0xffffff);
            font_draw(fb,104,128,"Class (Up/Down)",0xffffff); font_draw(fb,384,128,"Gender",0xffffff);
            first=first_row(selected_class,10);
            for(i=first;i<class_count && i<first+10;++i) {
                const ClassDef *c=&g_world.classes[class_ids[i]];
                snprintf(label,sizeof(label),"%s - magic %d",c->name,c->magic_ratio);
                draw_list_row(fb,class_rect,i-first,label,i==selected_class);
            }
            for(i=0;i<gender_count;++i) {
                Rect r={384,146+i*24,152,22}; label_button(fb,r,genders[i]);
                if(i==selected_gender) fb_rect(fb,r,0xffdf80);
            }
            if(class_count) font_wrap(fb,(Rect){104,334,432,48},g_world.classes[class_ids[selected_class]].description,0xf0e0bd);
            label_button(fb,ok_rect,"OK"); label_button(fb,cancel_rect,"Cancel");
        }
    }
    if(*message) {
        fb_fill(fb,(Rect){0,444,640,36},0x241820); font_wrap(fb,(Rect){8,450,624,28},message,0xffd080);
    }
}
static const Screen front_screen={"front",NULL,front_update,front_render,NULL};
void game_go_front(void) { menu(); screen_set(&front_screen); }
int game_boot(void)
{
    state=TITLE; ticks=0; art_background("title.jpg"); screen_set(&front_screen); return 0;
}
