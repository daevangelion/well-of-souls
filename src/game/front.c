/* WoS A96 solo front end: the state machine FUN_0041B891 (all.c:21021) and the
 * clickable-text menu system it is built on (FUN_00405000 / 0x4056E7 register,
 * FUN_004054E8 advance, FUN_00405153 draw, FUN_00405765 hit-test), with
 * docs/re/boot_flow.md sections 1-3 and 5 as the map.
 *
 * Every screen's geometry comes from the same source as the original's: the
 * registration calls in FUN_0041D01E (title), FUN_0041D155 (main menu),
 * FUN_0041D31F/FUN_0041D3CC/FUN_0041D717 (world select) and FUN_0041D3CC's
 * per-row call, with the argument order confirmed against the disassembly of
 * FUN_00405000 at 0x405000. Coordinates are per-mille of the client rect and
 * the pixel anchor is anchor*client/1000, exactly as FUN_00405153 computes it.
 *
 * Deliberate deviations, all presentation:
 *  - MFC modeless and modal windows are framebuffer panels, and the animated
 *    text is drawn with the bundled 8x8 bitmap font instead of Tempus Sans ITC
 *    created through CFont::CreatePointFont. The original derives its font
 *    pixel size as min(client_w*80/100, client_h) * fontSizePerMille/1000,
 *    which at a 640x480 client is 240 px for the main menu's 500 and 1200 px
 *    for the title's 2500; the port cannot match that with an 8x8 font and
 *    does not try. The ANCHOR of every hotspot -- the value that decides which
 *    entry a click selects -- is the original's arithmetic and is exact; only
 *    the text extent, and therefore the rect's width and height, differ.
 *  - "Haunt", the registration dialog (100) and every online entry log a
 *    message instead of opening anything.
 */
#include "game.h"
#include "hero.h"
#include "front.h"
#include "editors.h"
#include "../game_main.h"
#include "../engine/screen.h"
#include "../engine/clock.h"
#include "../engine/font.h"
#include "../engine/image.h"
#include "../engine/text.h"
#include "../engine/ini.h"
#include "../engine/log.h"
#include "../engine/dump.h"
#include "../engine/rng.h"
#include "../platform/platform.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

#define MAX_CHOICES      100
#define HOTSPOT_SLOTS    100   /* DAT_005339F8, 100 entries (0x4056E7's scan bound) */
#define ROW_HEIGHT       18
#define STORY_MS         12000 /* the +STORY scroller's own dwell; see story_tick */

/* FUN_00416ABB's mode, and the value FUN_00417F1B forces before every save. */
#define CKSUM_CURRENT    1

/* --------------------------------------------------------- the hotspots ---
 * One entry is the original's 0xBC-byte record. The field names and offsets are
 * those of DAT_005339F8 as fixed by the disassembly of FUN_00405000:
 *   +0x00 state  0 free, 1 live, 2 animation finished, 3 dead
 *   +0x04 layer  0 or 1, freed wholesale by FUN_004055A2
 *   +0x08 flags  0x40 centre horizontally, 0x80 centre vertically,
 *                0x200 drawn as a shadow first, 0x400 the entry is clickable
 *                (FUN_00405765 requires the 0x400 bit SET)
 *   +0x0C t_start, +0x10 t_len   (GetTickCount at registration, ms)
 *   +0x14 text, +0x18 font name
 *   +0x6C message, +0x70 lparam
 *   +0x74 the rect the entry was last drawn into -- the hit test uses this
 *   +0x84/+0x94 font size per-mille, from and to
 *   +0x88/+0x98 x per-mille, from and to
 *   +0x8C/+0x9C y per-mille, from and to
 *   +0xB0/+0xB4 the current x and y, the lerped values
 *   +0xB8 colour
 * The five-argument registration helper below has the same parameter order as
 * FUN_004056E7's 15 arguments, minus the ones the port does not need. */
typedef struct {
    int state, layer, flags, t_start, t_len;
    int fs0, fs1, x0, x1, y0, y1, cx, cy, colour;
    int msg, lparam;
    char text[128];
    Rect rect;
} Hotspot;

static Hotspot spots[HOTSPOT_SLOTS];
static uint32_t state_tick;   /* the original's view+0x13F4, stamped by FUN_0041B891 */
static int state = FRONT_TITLE;
static int world_first;       /* view+0x13E8, the world list's scroll offset */
static int world_anim;        /* view+0x13DC, the list's slide factor */
static int layout_done;

/* FUN_004054A7: the lerp. `from` when the tick has not reached t_start, `to`
 * once it has passed t_start+t_len, and the straight interpolation between. */
static int lerp(int from, int to, int t_start, int t_len, uint32_t tick)
{
    if ((uint32_t)tick <= (uint32_t)t_start) return from;
    if ((uint32_t)tick >= (uint32_t)(t_start+t_len)) return to;
    return (int)(((int64_t)(to-from)*(int64_t)((uint32_t)tick-(uint32_t)t_start))/t_len)+from;
}

/* FUN_004056E7 + FUN_00405000: find a free slot (0 or 3) and register. */
static int spot_add(int layer, int flags, int t_len, const char *text, int colour,
                    int fs0, int x0, int y0, int fs1, int x1, int y1,
                    int msg, int lparam)
{
    int i;
    for (i = 0; i < HOTSPOT_SLOTS; ++i) {
        Hotspot *s = &spots[i];
        if (s->state && s->state != 3) continue;
        s->state = 1; s->layer = layer; s->flags = flags;
        s->t_start = (int)clock_ms(); s->t_len = t_len;
        snprintf(s->text,sizeof(s->text),"%s",text);
        s->colour = colour; s->msg = msg; s->lparam = lparam;
        s->fs0 = fs0; s->fs1 = fs1;
        s->x0 = x0; s->x1 = x1; s->y0 = y0; s->y1 = y1;
        /* FUN_004054E8 stores the FROM values into the current slots, so a
         * freshly registered entry starts at its from-position, not at rest. */
        s->cx = fs0; s->cy = x0;
        s->rect.x = s->rect.y = s->rect.w = s->rect.h = 0;
        return i;
    }
    return -1;
}
/* FUN_004054E8: advance the animation and retire the entry when it is done. */
static void spot_advance(void)
{
    uint32_t tick = clock_ms();
    int i;
    for (i = 0; i < HOTSPOT_SLOTS; ++i) {
        Hotspot *s = &spots[i];
        if (!s->state) continue;
        s->cx = lerp(s->fs0, s->fs1, s->t_start, s->t_len, tick);
        s->cy = lerp(s->x0,  s->x1,  s->t_start, s->t_len, tick);
        if ((uint32_t)s->t_len < (uint32_t)tick-(uint32_t)s->t_start) {
            s->state = (s->flags & 4) ? 3 : 2;
            /* FUN_004054E8's flag-8 branch: a one-shot sound, then the bit clears. */
            s->flags &= ~8;
        }
    }
}
/* FUN_004055A2: free a whole layer. */
static void spot_free_layer(int layer)
{
    int i;
    for (i = 0; i < HOTSPOT_SLOTS; ++i) if (spots[i].state && spots[i].layer==layer) spots[i].state = 0;
}
static void spot_free_all(void) { spot_free_layer(0); spot_free_layer(1); }

/* FUN_00405153: the pixel position and the rect. x_px = x*w/1000 and
 * y_px = y*h/1000, then the 0x40/0x80 flags pull the text back by half its
 * extent so the anchor is the CENTRE of the string. */
static void spot_layout(void)
{
    int i;
    uint32_t tick = clock_ms();
    int W = PLAT_SCREEN_W, H = PLAT_SCREEN_H;
    for (i = 0; i < HOTSPOT_SLOTS; ++i) {
        Hotspot *s = &spots[i];
        int x, y, w, h, dx = 0, dy = 0;
        if (!s->state) continue;
        x = (int)((int64_t)lerp(s->x0,s->x1,s->t_start,s->t_len,tick)*W/1000);
        y = (int)((int64_t)lerp(s->y0,s->y1,s->t_start,s->t_len,tick)*H/1000);
        w = font_width(s->text); h = FONT_H;
        if (s->flags & 0x40) dx = -w/2;
        if (s->flags & 0x80) dy = -h/2;
        if (s->flags & 2) dy += 2; /* the shadow pass sits 2 px low */
        s->rect.x = x+dx; s->rect.y = y+dy;
        s->rect.w = w;     s->rect.h = h;
    }
    layout_done = 1;
}
/* FUN_00405765: the first live, clickable entry whose last-drawn rect holds the
 * point. Flag bit 0x400 must be set for an entry to be reachable at all. */
static int spot_hit(int px, int py)
{
    int i;
    spot_layout();
    for (i = 0; i < HOTSPOT_SLOTS; ++i) {
        Hotspot *s = &spots[i];
        if (s->state < 1 || s->state == 3 || !(s->flags & 0x400)) continue;
        if (px>=s->rect.x && py>=s->rect.y && px<s->rect.x+s->rect.w && py<s->rect.y+s->rect.h)
            return i;
    }
    return -1;
}
static const char *spot_text(int i) { return i>=0 && i<HOTSPOT_SLOTS ? spots[i].text : ""; }
static int spot_msg(int i) { return i>=0 && i<HOTSPOT_SLOTS ? spots[i].msg : 0; }
static int spot_lparam(int i) { return i>=0 && i<HOTSPOT_SLOTS ? spots[i].lparam : 0; }

/* ------------------------------------------------------------- messages ---
 * The original's window messages, kept as the dispatch key so the port's flow
 * can be read against the decomp. */
enum {
    MSG_PLAY_NOW    = 0x46B, /* FUN_0041F699 -> state 2                     */
    MSG_GOLDEN      = 0x46C,
    MSG_HELP        = 0x46D, /* WINHELP WELLOFSOULS.HLP                   */
    MSG_DEPART      = 0x46E, /* quit                                        */
    MSG_WEBSITE     = 0x483, /* synthetic-reality.com                      */
    MSG_WORLDS_PREV = 0x472,
    MSG_WORLDS_NEXT = 0x473,
    MSG_WORLD_ROW   = 0x474,
    MSG_NEW_SOUL    = 0x489,
    MSG_PURGE       = 0x464, /* the Pick-a-Soul window's Purge Soul button */
    MSG_INCARNATE   = 0x486,
    MSG_RESTORE     = 0x488,
    MSG_MAP         = 0x487,
    MSG_HAUNT       = 0x48F,
    MSG_ONLINE      = 0x498,
    MSG_CREATE_WORLD= 0x620, /* solo: "--- or Create Your Own World ---"    */
    MSG_BIO          = 0x63B  /* the "Edits" button, FUN_00452107               */
};

/* ----------------------------------------------------------------- state --- */
static int selected_world, world_count, selected_soul, soul_count;
static int selected_class, class_count, selected_gender, gender_count, name_focus;
static int class_ids[WORLD_MAX_CLASSES], gender_ids[4];
static char worlds[MAX_CHOICES][64], souls[MAX_CHOICES][HERO_NAME_MAX];
static char genders[4][64], name[HERO_NAME_MAX], message[160];
static int pending_death, resurrect_on_well, place_prompt, place_map, place_link;
static int show_credits;
/* The Terms of Service modal, FUN_00402A73 (all.c:16956), called from
 * FUN_0041B891 case 1 (all.c:21092). It shows a top-level dialog carrying
 * tos.rtf and returns 0 on Cancel, and the caller then posts 0x46E and
 * WM_CLOSE, so declining QUITS rather than returning to the menu. The accept
 * is remembered: FUN_00402A73 compares a date string derived from tos.rtf
 * against the stored one with _stricmp, and only re-prompts when they differ.
 * The button rects are Oracle4's live measurement of the original's #32770 at
 * a 640x480 client. */
static int tos_open;
static const Rect tos_accept = { 356, 371, 75, 23 };
static const Rect tos_cancel = { 442, 371, 75, 23 };
/* The BIO editor. FUN_00452107 is a custom CWnd whose commit handler reads the
 * edit at +0x338; the port keeps the body here and hands it to hero_bio_save.
 * The button that opens it is "Edits" (0x4F02C4) on the world-select screen. */
static int bio_open, bio_focus;
static char bio_text[HERO_BIO_TEXT_MAX];
static const Rect bio_edit_rect = { 96, 132, 448, 168 };
static FrontDialogFn dialog_fn;
static Image background, buttons[5];
static Sheet portrait;
static Input last_input;
static const Screen front_screen;
static const char *const button_names[5] = {"Incarnate","Haunt","New","Restore","Map"};

/* The New Soul panel's controls are dialog 138's: CEdit at dlg+0x32C, the
 * CListBox at dlg+0x2AC, the OK button at dlg+0x2EC. The port draws them in a
 * framebuffer panel at the same place on the 640x480 client. */
static Rect name_rect(void) { Rect r={104,88,432,26}; return r; }
static Rect class_rect(void) { Rect r={104,146,264,180}; return r; }
static Rect ok_rect(void) { Rect r={354,398,86,28}; return r; }
static int front_new_soul_pk;
static void label_button(Framebuffer *fb,Rect r,const char *text);
static void front_update_newsoul(const Input *in);
static int front_dialog_apply(int dialog_id, const char *const *kv, int n, int ok);

static int inside(const Input *in, Rect r)
{ return in->mouse_x>=r.x && in->mouse_y>=r.y && in->mouse_x<r.x+r.w && in->mouse_y<r.y+r.h; }
static int clicked(const Input *in, Rect r) { return (in->mouse_pressed&2u) && inside(in,r); }
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

/* --- the screens' setup, i.e. FUN_0041B891's switch ------------------------ */
static void art_title(void)
{
    art_background("title.jpg");
    /* FUN_0041D01E, all.c:21985. Layer 0, flags 0x2C0/0x240/0x340, 5000 ms, no
     * message, so nothing here is clickable: a mouse-down anywhere is handled
     * by FUN_0041C1CD case 0, which goes straight to state 1. */
    spot_add(0,0x2c0,5000,"Well",0x0000ff,2500,1000,333,2500,500,583,0,0);
    spot_add(0,0x240,5000,"Souls",0x0000ff,2500,   0,833,2500,500,583,0,0);
    spot_add(0,0x340,5000,"Synthetic Reality, Inc.",0x0000ff,1666,500,583,1666,500,583,0,0);
    spot_add(0,0x340,5000,"synthetic-reality.com",0x00ffff, 125,500,583,1000,500,115,0,0);
}

/* FUN_0041D121, all.c:22005: one rand() per drawn string, the draw being a
 * modulo of rand() by the table size, index 0 skipped unless the serial check
 * says otherwise. DAT_004E6910 == 0 is the solo channel, so index 0 is never
 * taken. */
/* The label strings are the ORIGINAL'S BYTES, read out of .data rather than
 * transcribed from Ghidra's symbol names, which mangle punctuation into `_` and
 * cannot distinguish a space from a hyphen:
 *   0x4E1FE8  "Check On-Line for New Worlds"   (0x2d is a hyphen, not a space)
 *   0x4E1FD0  "Play now (it's free!)."        (parenthesised, trailing period)
 *   0x4E1FB8  "Play now, Golden Soul!"         (comma, no parentheses)
 *   0x4E1F98  "Read the attractive help file."
 *   0x4E1F7B  "Visit synthetic-reality.com."
 *   0x4E1F63  "Depart this realm."
 *   0x4E2008  "Where Do You Want To Play Today?"
 *   0x4E21CC  "Choose Your World..."
 *   0x4E20D0  "... or Create Your Own World..."
 * Oracle4 read these as live heap strings at record+0x14 and its dump was right;
 * the decomp symbol names were not evidence against it. */
static const char *golden_soul_string(void)
{
    static const char *const table[] = {
        "Thank you, Golden Soul!"
    };
    int n = (int)(sizeof(table)/sizeof(table[0]));
    int i = crt_rand() % n;
    if (i == 0) i = 1;
    return table[i];
}

/* FUN_0044BA39/FUN_0045270F reduce tos.rtf to a date string; the original then
 * compares it with the stored one and re-prompts on a mismatch. The port has no
 * profile store, so the identity is the file's size and mtime, written to
 * <save>/legal.ini under [LEGAL] TOS_DATE - the same slot the original keeps in
 * WIN.INI, and the same "changed file means re-prompt" rule. */
static int tos_identity(char *out, size_t cap)
{
    char path[768]; FILE *f; long size; char *text; size_t n;
    snprintf(path,sizeof(path),"%s/tos.rtf",game_data_path());
    f = plat_fopen(path,"rb");
    if (!f) return 0;
    if (fseek(f,0,SEEK_END)) { fclose(f); return 0; }
    size = ftell(f);
    fclose(f);
    n = snprintf(out,cap,"%ld",size);
    text = text_read_file(path,&n);
    if (text) {
        size_t i, sum = 0;
        for (i = 0; i < n; ++i) sum = sum * 31u + (unsigned char)text[i];
        free(text);
        snprintf(out,cap,"%ld-%08lx",size,(unsigned long)sum);
    }
    return 1;
}
static int tos_already_accepted(const char *id)
{
    char path[1024], line[256]; FILE *f; int seen = 0;
    snprintf(path,sizeof(path),"%s/legal.ini",game_save_path());
    f = plat_fopen(path,"rb"); if (!f) return 0;
    while (fgets(line,sizeof(line),f)) {
        char *nl = strchr(line,'\n'); if (nl) *nl = 0;
        if (seen && !strcmp(line,id)) { fclose(f); return 1; }
        if (!strcmp(line,"[LEGAL]")) seen = 1;
    }
    fclose(f);
    return 0;
}
static void tos_record_accept(const char *id)
{
    char path[1024]; FILE *f; int seen = 0;
    snprintf(path,sizeof(path),"%s/legal.ini",game_save_path());
    f = plat_fopen(path,"rb");
    if (f) { char line[256];
        while (fgets(line,sizeof(line),f)) if (!strcmp(line,"[LEGAL]\n")) seen = 1;
        fclose(f); }
    f = plat_fopen(path,"ab"); if (!f) return;
    if (!seen) fputs("[LEGAL]\n",f);
    fprintf(f,"TOS_DATE=%s\n",id);
    fclose(f);
}
/* Returns 1 to continue into the menu, 0 when the user declined. */
static int tos_gate(void)
{
    char id[64];
    if (!tos_identity(id,sizeof(id))) return 1;          /* no tos.rtf: nothing to ask */
    if (tos_already_accepted(id)) return 1;             /* same revision: do not re-ask */
    tos_open = 1;
    wos_log_event("tos_prompt","id=%s",id);
    return 2;                                           /* pending; resolved on input */
}

static void art_menu(void)
{
    art_background("beg.jpg");
    /* FUN_0041D155, all.c:22028. Flags 0xE00: clickable, no centring, so the
     * per-mille anchor is the text's top-left corner. */
    spot_add(0,0xe00, 750,"Check On-Line for New Worlds",0x00ff00,500,1062,125,500, 62,125,MSG_ONLINE,0);
    spot_add(0,0xe00,1000,"Play now (it's free!).",         0x00ff00,500,1124,250,500,124,250,MSG_PLAY_NOW,0);
    spot_add(0,0xe00,1250,golden_soul_string(),             0x00ff00,500,1186,375,500,186,375,MSG_GOLDEN,0);
    spot_add(0,0xe00,1500,"Read the attractive help file.", 0x00ff00,500,1248,500,500,248,500,MSG_HELP,0);
    spot_add(0,0xe00,1750,"Visit synthetic-reality.com.",  0x00ff00,500,1310,625,500,310,625,MSG_WEBSITE,0);
    spot_add(0,0xe00,2000,"Depart this realm.",            0x00ff00,500,1372,750,500,372,750,MSG_DEPART,0);
    /* State 1 gates on the Terms of Service, FUN_00402A73 (all.c:21092), which
     * is NOT an external open: it is a modal with the file rendered inside it,
     * and declining it posts 0x46E then WM_CLOSE, i.e. it quits. */
}

static int compare_worlds(const void *a,const void *b) { return text_casecmp(a,b); }
static void world_entry(const char *entry,int is_dir,void *user)
{
    (void)user;
    if(is_dir && world_count<MAX_CHOICES && strlen(entry)<sizeof(worlds[0]))
        snprintf(worlds[world_count++],sizeof(worlds[0]),"%s",entry);
}

static void art_where(void)
{
    /* FUN_0041D31F, all.c:22084: the title only, flags 0x200, no message. */
    art_background("where.jpg");
    spot_add(0,0x200, 750,"Where Do You Want To Play Today?",0x00ffff,500,1062,125,500, 62,125,0,0);
}

static void art_choose(void)
{
    art_background("chapter.jpg");
    spot_add(0,0x200, 750,"Choose Your World...",0x00ffff,500, 62,125,500, 62,125,0,0);
    /* FUN_0041D3CC, all.c:22112. Layer 1, flags 0x600 (clickable, no
     * centring). fontSize = h*2/w, x = 2*((w+7)>>3) for every row, and the
     * row's y comes from the per-row lerp that starts at h/20 + (h+3)/4. */
    {
        int W = PLAT_SCREEN_W, H = PLAT_SCREEN_H;
        int fs = (H*2)/W;
        int x  = 2*((W+7)>>3);
        int step = (H/-20 + ((H+3)>>2)*-2 + H)/W;
        int y0  = H/20 + ((H+3)>>2);
        if (world_first>0)
            spot_add(1,0x600, 250,"< Previous",0x00ff00,fs,
                     x+((W+15)>>4)+((W+7)>>3), ((H+3)>>2), fs,
                     x+((W+15)>>4)+((W+7)>>3), ((H+3)>>2), MSG_WORLDS_PREV,0);
        if (world_first+4 < world_count)
            spot_add(1,0x600, 250,"Next >",0x00ff00,fs,
                     x+((W+15)>>4)+((W+7)>>3)-(W/W)*x, H-((H+3)>>2), fs,
                     x+((W+15)>>4)+((W+7)>>3)-(W/W)*x, H-((H+3)>>2), MSG_WORLDS_NEXT,0);
        {
            int n = world_count, last = world_first+4;
            int i;
            if (last>n) last=n;
            for (i=world_first;i<last;++i) {
                int row = (i-world_first)*step + y0 + (H/(W+4))*world_anim;
                spot_add(1,0x600, 250,worlds[i], i==selected_world?0x00ff80:0x00ff00,
                         fs, x, row, fs, x, row, MSG_WORLD_ROW, i);
            }
        }
    }
    /* The "Edits" button (0x4F02C4) that opens the personal BIO editor,
     * FUN_00452107. It lives on this screen in the original, whose own prompt
     * string says so: "Use the BIO button on the 'Where would you like to play'
     * screen to set your personal BIO info." (0x4F0314). */
    spot_add(1,0x620, 250,"Edits",0x00ff00,500,20,10,500,20,10, MSG_BIO,0);
    /* DAT_004E6910 == 0 is the solo channel, so the link reads
     * "--- or Create Your Own World ---" and hands over to the world editor. */
    spot_add(1,0x620, 750,"... or Create Your Own World...",0x00ff00,500,1000,875,500,875,875,
             MSG_CREATE_WORLD,0);
}

static void scan_worlds(void)
{
    char path[768];
    world_count=0; world_first=0; world_anim=1; selected_world=0;
    snprintf(path,sizeof(path),"%s/worlds",game_data_path());
    plat_list_dir(path,world_entry,NULL);
    qsort(worlds,(size_t)world_count,sizeof(worlds[0]),compare_worlds);
}

static void art_choose_world(void) { scan_worlds(); art_choose(); }

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
static void pick_a_soul(void)
{
    int i; char rel[128], path[768];
    message[0]=0;
    soul_count=hero_list_saves(souls,MAX_CHOICES);
    selected_soul=soul_count?0:-1;
    if(g_hero.valid) for(i=0;i<soul_count;++i) if(!text_casecmp(souls[i],g_hero.name)) selected_soul=i;
    well_background();
    for(i=0;i<5;++i) {
        snprintf(rel,sizeof(rel),"art/button%s.bmp",button_names[i]);
        world_data_path(path,sizeof(path),rel); image_load(&buttons[i],path);
    }
    load_selected();
}
static void art_story(void)
{
    /* State 4: the +STORY scroller. FUN_0041B891 case 4 stamps view+0x1388
     * with GetTickCount, sets DAT_004DF8A8 = 1 (the scroller is live) and plays
     * title.mid; when the scroller finishes, FUN_0041BDB4 case 4 prints
     * "Story Over", waits 1000 ms and posts 0x478. */
    art_background("chapter.jpg");
    game_music("title.mid");
}

/* --------------------------------------------------------- state machine --- */
static void front_goto(int next);

int front_enter_title(void)
{
    plat_text_input(0);
    state=FRONT_TITLE; message[0]=0; pending_death=0; place_prompt=0;
    spot_free_all(); art_title();
    state_tick=clock_ms();
    screen_set(&front_screen);
    wos_log_event("front_state","state=%d",state);
    return 0;
}

int front_state(void) { return state; }
int front_active(void) { return state>=0; }

static void front_goto(int next)
{
    state = next;
    state_tick = clock_ms();
    message[0]=0;
    spot_free_all();
    switch(next) {
    case FRONT_TITLE:  art_title(); break;
    case FRONT_MENU:   art_menu(); game_music("MainMenu.wav");
                       wos_log_event("boot_menu","");
                       if (tos_gate()==2) tos_open=1;    /* FUN_00402A73's modal */
                       break;
    case FRONT_WHERE:  art_where(); game_music("MainMenu.wav"); break;
    case FRONT_CHOOSE: art_choose_world(); break;
    case FRONT_STORY:  art_story(); break;
    case FRONT_WELL:   pick_a_soul(); break;
    case FRONT_DEATH:  art_background("death.jpg"); break;
    default: break;
    }
    wos_log_event("front_state","state=%d",state);
    layout_done=0;
}

void front_reenter(void) { front_goto(state); }
void game_go_front(void) { front_goto(FRONT_WHERE); }
void game_go_well(void)  { front_goto(FRONT_WELL); }

/* FUN_004978D5(hero,1), the recharge the Incarnate handler performs. */
static void hero_recharge(void)
{
    /* FUN_004978D5(hero,1) recharges the hero. It cannot clear disease, because
     * there is no hero-side disease state to clear: the counters live in the
     * combatant record and are zeroed per fight (FUN_00491E45). */
    g_hero.hp=g_hero.max_hp; g_hero.mp=g_hero.max_mp;
}

/* FUN_00420240, all.c:24052: time() into 0x1BE*4/0x2AA*4, the save, the
 * incarnation counter, then the map change. FUN_00420714 closes the overlays
 * and lands in state 6 (in a scene); the port's map module owns that, so all
 * this does is the save, the counters and the entry point. */
static void incarnate(void)
{
    const ClassDef *c;
    if(!g_hero.valid) { snprintf(message,sizeof(message),"My Child, how can you incarnate before you have a soul?"); return; }
    c=&g_world.classes[g_hero.klass];
    hero_recharge();
    g_hero.incarnations++;
    g_hero.seconds_played = (int)((clock_ms()/1000u) - (clock_ms()/1000u)); /* 0 at the first tick */
    if(hero_save(&g_hero)) { snprintf(message,sizeof(message),"Could not save your soul."); return; }
    wos_log_event("hero_ready","name=%s class=%d",g_hero.name,g_hero.klass);
    wos_log_event("incarnate","link=%d map=%d",c->start_location_set?c->start_link:0,
                  c->start_location_set?c->start_map:0);
    game_enter_map(c->start_location_set?c->start_map:0,
                   c->start_location_set?c->start_link:0,
                   c->start_location_set?c->start_drop_in:0);
    /* hero[0x336] (dropIn) == 0 is the original's test for "no START_LOCATION";
     * it then asks, and answering Yes opens FUN_00434F95("earth"). */
    if(!c->start_location_set) { place_prompt=1; place_map=0; place_link=0; }
}

/* --- New Soul, dialog 138 (0x8A) ----------------------------------------- */
static void new_soul(void)
{
    char path[768], section[8]; char *text; Ini ini; int i;
    state=FRONT_WELL; name[0]=0; message[0]=0; name_focus=1;
    plat_text_input(1);
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
    wos_log_event("new_soul_dialog","classes=%d genders=%d",class_count,gender_count);
}

/* FUN_00460765 sanitises and FUN_00460805 validates; both are reproduced in
 * front_validate_name. */
static const char *front_validate_name(void)
{
    /* FUN_0049D015's bad-word table, from Souls.exe 0x505188. */
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
    for(i=0;i<(size_t)soul_count;++i) if(!text_casecmp(name,souls[i])) return "A soul already exists in this world";
    return NULL;
}

/* FUN_00477461 (all.c:87910) is the PURGE path: the original answers the
 * duplicate-name box with "Delete the existing soul?" and, on Yes, removes the
 * .her before creating the new one. It also removes the per-hero INI, which is
 * the same base name with no extension. */
static int purge_soul(const char *soul)
{
    int rc = hero_delete(soul);
    char path[1024];
    if (rc==0) {
        snprintf(path,sizeof(path),"%s/%s/savedHeroes/%s",game_save_path(),g_world.name,soul);
        remove(path);
        wos_log_event("soul_purged","name=%s",soul);
    }
    return rc;
}

/* The New Soul OK path, FUN_00460A7E step by step. `pk` is the checkbox at
 * dlg+0x30C; the original always shows the review box and aborts on any answer
 * other than IDYES. In solo there is nothing to opt into, so the box is shown
 * and the answer defaults to NO unless the caller supplied one. */
static void create_soul(int pk)
{
    const char *error;
    if(!*name) { snprintf(message,sizeof(message),"Please choose a name for your soul"); return; }
    error=front_validate_name();
    if(error) { snprintf(message,sizeof(message),"%s",error); return; }
    if(!class_count) { snprintf(message,sizeof(message),"This world has no playable classes."); return; }
    wos_log_event("pk_choice","pk=%d",pk?1:0);
    hero_create(&g_hero,name,class_ids[selected_class],gender_ids[selected_gender],NULL);
    g_hero.serial = 1;
    if(!g_hero.valid) { snprintf(message,sizeof(message),"Could not create your soul."); return; }
    soul_count=hero_list_saves(souls,MAX_CHOICES);
    if(hero_save(&g_hero)) { snprintf(message,sizeof(message),"Could not save your soul."); return; }
    load_portrait();
    /* Dialog 149 (0x95), the post-creation follow-up, then FUN_0042095E's own
     * recursion into Incarnate. */
    wos_log_event("new_soul_created","name=%s class=%d",g_hero.name,g_hero.klass);
    plat_text_input(0);
    incarnate();
}

/* -------------------------------------------------------- the story ----- */
static int story_line_at(uint32_t since)
{
    int n = world_story_count();
    int per = n>0 ? (int)(STORY_MS/(uint32_t)n) : 0;
    int i = per>0 ? (int)((clock_ms()-since)/(uint32_t)per) : 0;
    if (i>=n) i = n>0 ? n-1 : 0;
    return i;
}

/* ----------------------------------------------------------------- input --- */
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

static void place_yourself_draw(Framebuffer *fb)
{
    ui_panel(fb,(Rect){120,140,400,200},"Place Yourself On Gaiea",
             "You have not yet specified your position");
    font_draw(fb,140,190,"Map",0xffffff);
    snprintf(message,sizeof(message),"%d  (link %d)",place_map,place_link);
    font_draw(fb,200,190,message,0xffdf80);
    label_button(fb,(Rect){140,240,120,28},"Where");
    label_button(fb,(Rect){280,240,120,28},"Onward");
}

int front_bio_active(void) { return bio_open; }

int front_bio_op(const char *text, int ok)
{
    if (text) { snprintf(bio_text,sizeof(bio_text),"%s",text); }
    if (ok) {
        if (!g_hero.valid) { snprintf(message,sizeof(message),"You have no soul yet."); return 1; }
        if (hero_bio_save(bio_text)) snprintf(message,sizeof(message),"Could not save your BIO.");
        else wos_log_event("hero_bio","serial=%08x",(unsigned)g_hero.serial);
    }
    bio_open=0; bio_focus=0; plat_text_input(0);
    return 1;
}

static void front_bio_open_panel(void)
{
    char *text; size_t n=0;
    bio_text[0]=0;
    text = hero_bio_text(&n);
    if (text) { snprintf(bio_text,sizeof(bio_text),"%s",text); free(text); }
    bio_open=1; bio_focus=1; plat_text_input(1);
    wos_log_event("bio_open","bytes=%d",(int)n);
}

static void front_bio_draw(Framebuffer *fb)
{
    ui_panel(fb,(Rect){64,88,512,300},"Personal BIO","");
    font_draw(fb,80,110,"Your biography is stored with your soul.",0xf0e0bd);
    fb_fill(fb,bio_edit_rect,bio_focus?0x181830:0x101018);
    fb_rect(fb,bio_edit_rect,bio_focus?0xffdf80:0xbcad80);
    font_wrap(fb,(Rect){bio_edit_rect.x+4,bio_edit_rect.y+4,bio_edit_rect.w-8,bio_edit_rect.h-8},
              bio_text,0xffffff);
    label_button(fb,(Rect){300,316,110,26},"OK");
    label_button(fb,(Rect){430,316,110,26},"Cancel");
}

static int front_bio_update(const Input *in)
{
    if (in->pressed[PLAT_KEY_ESCAPE] || clicked(in,(Rect){430,316,110,26})) {
        front_bio_op(NULL,0); return 1;
    }
    if (clicked(in,bio_edit_rect)) { bio_focus=1; plat_text_input(1); }
    if (bio_focus) {
        size_t len=strlen(bio_text), add=strlen(in->text);
        if (in->pressed[PLAT_KEY_BACKSPACE] && len) bio_text[--len]=0;
        if (add>sizeof(bio_text)-1-len) add=sizeof(bio_text)-1-len;
        memcpy(bio_text+len,in->text,add); bio_text[len+add]=0;
    }
    if (in->pressed[PLAT_KEY_RETURN] || clicked(in,(Rect){300,316,110,26})) front_bio_op(NULL,1);
    return 1;
}

static void front_update(const Input *in)
{
    int i, hit=-1, key=0; last_input=*in;
    if (tos_open) {
        if (clicked(in,tos_accept) || in->pressed[PLAT_KEY_RETURN]) {
            char id[64];
            tos_open=0;
            if (tos_identity(id,sizeof(id))) tos_record_accept(id);
            wos_log_event("tos_accept","");
            return;
        }
        if (clicked(in,tos_cancel) || in->pressed[PLAT_KEY_ESCAPE]) {
            tos_open=0;
            wos_log_event("tos_decline","");
            game_request_quit();
            return;
        }
        return;
    }
    if (bio_open) { front_bio_update(in); return; }
    if (show_credits) {
        /* FUN_0042198F shows it for 1000 ms with the text centred; the port
         * keeps it up until any key or click, which is the same information. */
        if (key || (in->mouse_pressed&2u)) { show_credits=0; return; }
        return;
    }
    for (i=0;i<INPUT_KEYS;++i) key|=in->pressed[i];
    spot_advance();
    spot_layout();
    if (state!=FRONT_STORY && (in->mouse_pressed&2u)) hit = spot_hit(in->mouse_x,in->mouse_y);
    if (pending_death) { pending_death=0; front_goto(FRONT_DEATH); wos_log_event("hero_death",""); return; }

    switch(state) {
    case FRONT_TITLE:
        /* FUN_0041BDB4 case 0: `if (13000 < GetTickCount() - view+0x13F4)`, strict.
         * FUN_0041C1CD case 0: any mouse-down also advances. */
        if (13000u < clock_ms()-state_tick || (in->mouse_pressed&2u) || key) front_goto(FRONT_MENU);
        break;
    case FRONT_MENU:
        if (hit>=0) {
            int msg=spot_msg(hit);
            switch (msg) {
            case MSG_PLAY_NOW:   front_goto(FRONT_WHERE); return;
            case MSG_DEPART:     game_request_quit(); return;
            case MSG_HELP:       wos_log_event("front_external_open","target=WELLOFSOULS.HLP");
                                 plat_open_external("WELLOFSOULS.HLP"); return;
            case MSG_WEBSITE:    wos_log_event("front_external_open","target=synthetic-reality.com");
                                 plat_open_external("http://synthetic-reality.com"); return;
            case MSG_ONLINE:     wos_log_event("front_online_unavailable","what=new_worlds");
                                 snprintf(message,sizeof(message),"There is no network connection.");
                                 return;
            case MSG_GOLDEN:     snprintf(message,sizeof(message),"%s",spot_text(hit)); return;
            default: break;
            }
        }
        if (in->pressed[PLAT_KEY_RETURN]) { front_goto(FRONT_WHERE); return; }
        /* The original has no credits hotspot: FUN_004861C8 joins the +CREDITS
         * lines and the About-this-world dialog (0x49751B) shows them, so the
         * port reaches the same text from the menu without adding a clickable
         * entry that the original does not have and that would move the other
         * entries' hit test. */
        if (in->pressed['c'] || in->pressed['C']) {
            show_credits=1;
            wos_log_event("credits","text=%s",world_credits_text());
            return;
        }
        break;
    case FRONT_WHERE:
        /* OPEN ITEM, deliberately not "fixed" - the solo exit from state 2 is
         * UNRESOLVED, and both readings of it have been measured wrong once
         * already. What is established:
         *
         *  - The original does NOT leave this state on any input. Oracle4 ran
         *    the original headless (script in their message, dumps front@t1000,
         *    t2000, t5000, t9000 after clicking Play now) and front_state stayed
         *    2 for the full nine seconds. Clicks at 240/200/300/400, RETURN,
         *    ESC, SPACE and idle were all measured to do nothing.
         *  - FUN_0041BDB4 case 2 (all.c:21253) is
         *        iVar5 = FUN_004057D3(); if (iVar5 < 1) goto default;
         *        InvalidateRect(...);
         *    It repaints while hotspots are live and otherwise does nothing:
         *    there is no transition in it. Cases 1 and 3 (all.c:21249) are bare
         *    repaints. So the tick is not the route.
         *  - FUN_0041B891(3) has EXACTLY ONE caller in the binary:
         *    FUN_00428D43 (all.c:30698), the abort/back handler, which sets
         *    DAT_004df8a4 = -1 first. State 3 is therefore reached via ABORT,
         *    not as state 2's forward destination.
         *  - FUN_0041F699, the 0x46B handler (all.c:23549), does
         *        FUN_0041B891(2);
         *        if (SendMessageA(frame, 0x46F, 0, 0) == 0) FUN_0041B891(1);
         *        else { FUN_0041D374(); FUN_00429C9C("MainMenu.wav"); }
         *    0x46F's only posters are the REGISTRATION dialog's DoModal path
         *    (all.c:7308, 7325, 7336 in FUN_0040930D), wParam 1/2/3/4 for
         *    ok/retry/needs-serial/gold. I reasoned that solo therefore returns
         *    0 and bounces to state 1 -- ORACLE4'S MEASUREMENT REFUTES THAT: the
         *    state stayed 2, so 0x46F did NOT return 0 and the else branch ran,
         *    which means the game considers a world loaded at that point in an
         *    offline run. That is an upstream fact about solo boot nobody has
         *    chased yet, and it may be the missing route.
         *  - The online route is MFC WM_COMMAND via a message map, not a switch,
         *    so it does not appear as a comparison in the decomp and I have not
         *    decoded it.
         *
         * WHY THE PORT ADVANCES ANYWAY: the original's behaviour here is to park,
         * and a front end that cannot leave state 2 is unreachable for every
         * other test. This advance is the port's escape hatch and is NOT a claim
         * about the original. It stays until the route is known.
         *
         * The thing that made this look settled when it was not: the TOS modal
         * was covering the main menu and swallowing every click aimed at "Play
         * now", so the scripts were stopping at state 1 for an unrelated reason
         * and state 2 was never actually reached. front_dump emits front_state so
         * that the next attempt is compared rather than inferred. */
        if ((in->mouse_pressed&2u) || key) front_goto(FRONT_CHOOSE);
        break;
    case FRONT_CHOOSE:
        if (in->pressed[PLAT_KEY_ESCAPE]) { front_goto(FRONT_MENU); return; }
        /* Arrow keys move the row cursor and RETURN takes it. The original has
         * no highlight -- its rows are plain clickable hotspots -- so this is
         * the plan's "keyboard shortcut that triggers the action a click
         * would", not a new rule: RETURN loads the same world the click would. */
        if (in->pressed[PLAT_KEY_UP]   && selected_world>0) --selected_world;
        if (in->pressed[PLAT_KEY_DOWN] && selected_world+1<world_count) ++selected_world;
        if (in->pressed[PLAT_KEY_RETURN] && world_count) {
            char chosen[64];
            snprintf(chosen,sizeof(chosen),"%s",worlds[selected_world]);
            if(world_load(game_data_path(),chosen))
                snprintf(message,sizeof(message),"Could not load that world.");
            else {
                memset(&g_hero,0,sizeof(g_hero));
                wos_log_event("world_chosen","name=%s",chosen);
                front_goto(FRONT_WELL);
            }
            return;
        }
        if (hit>=0) {
            int msg=spot_msg(hit), arg=spot_lparam(hit);
            if (msg==MSG_WORLDS_PREV) { world_first-=4; if(world_first<0) world_first=0; front_goto(FRONT_CHOOSE); return; }
            if (msg==MSG_WORLDS_NEXT) { world_first+=4; if(world_first+4>world_count) world_first=world_count>4?world_count-4:0; front_goto(FRONT_CHOOSE); return; }
            if (msg==MSG_BIO) { front_bio_open_panel(); return; }
            if (msg==MSG_CREATE_WORLD) {
                wos_log_event("front_state","state=%d",FRONT_WE_WORLD);
                if (editors_enter(NULL)) snprintf(message,sizeof(message),"The world editor could not open.");
                return;
            }
            if (msg==MSG_WORLD_ROW && arg<world_count) {
                char chosen[64];
                selected_world = arg;
                snprintf(chosen,sizeof(chosen),"%s",worlds[arg]);
                if(world_load(game_data_path(),chosen))
                    snprintf(message,sizeof(message),"Could not load that world.");
                else {
                    memset(&g_hero,0,sizeof(g_hero));
                    wos_log_event("world_chosen","name=%s",chosen);
                    front_goto(FRONT_WELL);
                }
                return;
            }
        }
        break;
    case FRONT_STORY:
        /* FUN_0041BDB4 case 4: while DAT_004DF8A8 is set the scroller runs;
         * otherwise "Story Over", a 1000 ms wait and PostMessage 0x478. The
         * port ends the scroller on the same dwell and returns to the menu. */
        if (key || (in->mouse_pressed&2u) || clock_ms()-state_tick > STORY_MS) {
            wos_log_event("story_over","lines=%d",world_story_count());
            front_goto(FRONT_MENU);
        }
        break;
    case FRONT_WELL: {
        int old=selected_soul;
        if (place_prompt) {
            if (clicked(in,(Rect){140,240,120,28})) { place_prompt=0; front_goto(FRONT_CHOOSE); return; }
            if (clicked(in,(Rect){280,240,120,28}) || in->pressed[PLAT_KEY_RETURN]) {
                g_hero.map=place_map; g_hero.link=place_link;
                if (hero_save(&g_hero)) snprintf(message,sizeof(message),"Could not save your soul.");
                place_prompt=0;
                wos_log_event("placed","map=%d link=%d",place_map,place_link);
            }
            return;
        }
        if (resurrect_on_well) { resurrect_on_well=0; hero_recharge(); if(hero_save(&g_hero)) { /* keep going */ } }
        select_list(in,(Rect){376,106,232,180},soul_count,&selected_soul);
        if(in->pressed[PLAT_KEY_UP] && selected_soul>0) --selected_soul;
        if(in->pressed[PLAT_KEY_DOWN] && selected_soul+1<soul_count) ++selected_soul;
        if(old!=selected_soul) load_selected();
        if(in->pressed['n'] || clicked(in,bar_rect(2)) || clicked(in,(Rect){376,300,232,28})) new_soul();
        else if(in->pressed['i'] || clicked(in,bar_rect(0)) || clicked(in,(Rect){376,336,232,28})) incarnate();
        else if(clicked(in,bar_rect(3))) { soul_count=hero_list_saves(souls,MAX_CHOICES); selected_soul=soul_count?0:-1; load_selected(); }
        else if(clicked(in,bar_rect(1))) snprintf(message,sizeof(message),"Haunting other players is an online feature.");
        else if(clicked(in,bar_rect(4))) snprintf(message,sizeof(message),"Incarnate your soul to explore the world map.");
        else if(in->pressed[PLAT_KEY_ESCAPE]) front_goto(FRONT_MENU);
        return;
    }
    case FRONT_DEATH:
        /* FUN_0041C1CD case 8: a click goes straight back to state 5. */
        if ((in->mouse_pressed&2u) || in->pressed[PLAT_KEY_RETURN] || in->pressed[PLAT_KEY_ESCAPE]) {
            resurrect_on_well=1;
            front_goto(FRONT_WELL);
        }
        break;
    default: break;
    }
    (void)key;
}

/* ------------------------------------------------------- new soul panel --- */
static void front_update_newsoul(const Input *in)
{
    int i;
    if(in->pressed[PLAT_KEY_ESCAPE] || clicked(in,(Rect){450,398,86,28})) {
        plat_text_input(0); name_focus=0; front_goto(FRONT_WELL); return;
    }
    if(clicked(in,name_rect())) name_focus=1;
    if(name_focus) {
        size_t len=strlen(name), add=strlen(in->text);
        if(in->pressed[PLAT_KEY_BACKSPACE] && len) name[--len]=0;
        if(add>sizeof(name)-1-len) add=sizeof(name)-1-len;
        memcpy(name+len,in->text,add); name[len+add]=0;
    }
    if(in->pressed[PLAT_KEY_UP] && selected_class>0) --selected_class;
    if(in->pressed[PLAT_KEY_DOWN] && selected_class+1<class_count) ++selected_class;
    select_list(in,class_rect(),class_count,&selected_class);
    for(i=0;i<gender_count;++i) if(clicked(in,(Rect){384,146+i*24,152,22})) selected_gender=i;
    if(in->pressed[PLAT_KEY_RETURN] || clicked(in,ok_rect())) {
        name_focus=0; plat_text_input(0);
        create_soul(front_new_soul_pk);
    }
}

/* --------------------------------------------------------- dialog hook --- */
void front_dialog_register(FrontDialogFn fn) { dialog_fn = fn; }

int front_dialog_op(int dialog_id, const char *const *kv, int n, int ok)
{
    if (dialog_fn) return dialog_fn(dialog_id,kv,n,ok);
    return front_dialog_apply(dialog_id,kv,n,ok);
}

static int front_dialog_apply(int dialog_id, const char *const *kv, int n, int ok)
{
    int i;
    if (dialog_id==138) {                     /* New Soul, 0x8A */
        if (!ok) { plat_text_input(0); name_focus=0; front_goto(FRONT_WELL); return 1; }
        for (i=0;i<n;++i) {
            const char *eq = strchr(kv[i],'=');
            char key[16];
            size_t klen;
            if (!eq) continue;
            klen = (size_t)(eq-kv[i]);
            if (klen>=sizeof(key)) continue;
            memcpy(key,kv[i],klen); key[klen]=0;
            while (klen && (key[klen-1]==' ' || key[klen-1]=='\t')) key[--klen]=0;
            if (!text_casecmp(key,"name")) {
                snprintf(name,sizeof(name),"%s",eq+1);
            } else if (!text_casecmp(key,"class")) {
                selected_class = atoi(eq+1);
                if (selected_class<0 || selected_class>=class_count) selected_class=0;
            } else if (!text_casecmp(key,"gender")) {
                selected_gender = atoi(eq+1);
                if (selected_gender<0 || selected_gender>=gender_count) selected_gender=0;
            } else if (!text_casecmp(key,"pk")) {
                front_new_soul_pk = atoi(eq+1);
            } else if (!text_casecmp(key,"purge")) {
                /* The duplicate-name branch of FUN_00460A7E step 4: answering
                 * the "A soul already exists in this world" box with Yes runs
                 * FUN_00477461, which deletes the old .her. */
                if (atoi(eq+1)) (void)purge_soul(name);
            }
        }
        create_soul(front_new_soul_pk);
        return 1;
    }
    if (dialog_id==FRONT_DIALOG_BIO) {        /* the port's own pseudo-id */
        const char *text = NULL;
        int i2;
        for (i2=0;i2<n;++i2)
            if (!strncmp(kv[i2],"bio=",4)) { text = kv[i2]+4; break; }
        front_bio_op(text,ok);
        return 1;
    }
    if (dialog_id==149) {                     /* the post-creation follow-up */
        wos_log_event("new_soul_followup","ok=%d",ok?1:0);
        return 1;
    }
    return 0;
}

void front_hero_death(const char *killer)
{
    pending_death=1;
    g_hero.deaths++;
    wos_log_event("hero_death","killer=%s",killer?killer:"");
}

/* ------------------------------------------------------------- rendering --- */
static void label_button(Framebuffer *fb,Rect r,const char *text)
{
    fb_fill(fb,r,inside(&last_input,r)?0x62543b:0x343040); fb_rect(fb,r,0xbcad80);
    font_draw(fb,r.x+6,r.y+(r.h-8)/2,text,0xffffff);
}
static void draw_hotspot_text(Framebuffer *fb)
{
    int i;
    for (i=0;i<HOTSPOT_SLOTS;++i) {
        Hotspot *s=&spots[i];
        if (!s->state) continue;
        if (s->flags & 2) font_draw(fb,s->rect.x+2,s->rect.y+2,s->text,0x101010);
        font_draw(fb,s->rect.x,s->rect.y,s->text,s->colour);
    }
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
    spot_advance();
    spot_layout();
    fb_clear(fb,0x151322); fb_blit(fb,&background,0,0,-1);
    if (tos_open) {
        Rect body = { 272, 200, 356, 150 };
        ui_panel(fb,(Rect){260,180,380,220},"Terms of Service","");
        font_wrap(fb,body,"This program is provided as is, and you accept the terms shipped in tos.rtf. Decline and the program quits.",0xf0e0bd);
        label_button(fb,tos_accept,"I Accept");
        label_button(fb,tos_cancel,"Cancel");
    } else if (bio_open) {
        front_bio_draw(fb);
    } else if (show_credits) {
        ui_panel(fb,(Rect){80,60,480,360},"Credits","");
        font_wrap(fb,(Rect){96,88,448,300},world_credits_text(),0xf0e0bd);
    } else if (state==FRONT_STORY) {
        int line = story_line_at(state_tick);
        ui_panel(fb,(Rect){0,0,640,480},"",world_story_line(line)?world_story_line(line):"");
        font_draw(fb,16,440,"Story Over in a moment",0xffffff);
    } else if (state==FRONT_WELL || state==FRONT_DEATH) {
        if (state==FRONT_WELL) {
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
            else if(!g_hero.valid) font_draw(fb,380,120,"Pick a Soul, then INCARNATE",0xffdf80);
            label_button(fb,(Rect){376,300,232,28},"NEW SOUL (N)");
            label_button(fb,(Rect){376,336,232,28},"INCARNATE THIS SOUL (I)");
            if (g_hero.valid) {
                snprintf(label,sizeof(label),"%s  Level %d",g_hero.name,g_hero.level);
                font_draw(fb,12,376,label,0xffffff);
                sheet_draw(fb,&portrait,0,110,130,0);
                fb_fill(fb,(Rect){8,420,240,16},0x38151b);
                fb_fill(fb,(Rect){8,420,(int)((int64_t)240*g_hero.hp/g_hero.max_hp),16},0x8b2934);
                fb_fill(fb,(Rect){264,420,240,16},0x151d38);
                if (g_hero.max_mp) fb_fill(fb,(Rect){264,420,(int)((int64_t)240*g_hero.mp/g_hero.max_mp),16},0x294c9b);
                snprintf(label,sizeof(label),"HP %d/%d",g_hero.hp,g_hero.max_hp);
                font_draw(fb,12,424,label,0xffffff);
                snprintf(label,sizeof(label),"MP %d/%d",g_hero.mp,g_hero.max_mp);
                font_draw(fb,268,424,label,0xffffff);
            }
        }
        if (name_focus) {
            ui_panel(fb,(Rect){88,52,464,386},"New Soul","");
            font_draw(fb,104,74,"Name",0xffffff);
            fb_fill(fb,name_rect(),0x101018); fb_rect(fb,name_rect(),0xffdf80); font_draw(fb,110,97,name,0xffffff);
            font_draw(fb,104,128,"Class (Up/Down)",0xffffff); font_draw(fb,384,128,"Gender",0xffffff);
            first=first_row(selected_class,10);
            for(i=first;i<class_count && i<first+10;++i) {
                const ClassDef *c=&g_world.classes[class_ids[i]];
                snprintf(label,sizeof(label),"%s - magic %d",c->name,c->magic_ratio);
                draw_list_row(fb,class_rect(),i-first,label,i==selected_class);
            }
            for(i=0;i<gender_count;++i) {
                Rect r={384,146+i*24,152,22}; label_button(fb,r,genders[i]);
                if(i==selected_gender) fb_rect(fb,r,0xffdf80);
            }
            if(class_count) font_wrap(fb,(Rect){104,334,432,48},g_world.classes[class_ids[selected_class]].description,0xf0e0bd);
            label_button(fb,ok_rect(),"OK");
            label_button(fb,(Rect){450,398,86,28},"Cancel");
        }
        if (place_prompt) place_yourself_draw(fb);
    }
    draw_hotspot_text(fb);
    if(*message) {
        fb_fill(fb,(Rect){0,444,640,36},0x241820); font_wrap(fb,(Rect){8,450,624,28},message,0xffd080);
    }
    (void)label;
}

/* The New Soul panel owns the input while it is up; every other state runs the
 * state machine's own handler. */
static void front_screen_update(const Input *in)
{
    if (name_focus) { front_update_newsoul(in); return; }
    front_update(in);
}

/* The front end's own state, for the differential dump. The original keeps it in
 * the 100-slot hotspot table at DAT_005339F8, 0xBC bytes per record, and
 * FUN_00405765 hit-tests clicks directly against it -- so the table IS the front
 * end's state, and front.c's `spots[]` is the same array. front.client is not
 * decoration: the rects are laid out against the client size on entry and then
 * ANIMATED, so a rect can describe a client area that no longer exists, and that
 * is the key that tells a mid-slide rect from a settled one. */
void front_dump(DumpEmit emit, void *user)
{
    int i, live = 0;
    char key[64];
    spot_advance();
    spot_layout();
    for (i = 0; i < HOTSPOT_SLOTS; ++i)
        if (spots[i].state >= 1 && spots[i].state <= 2 && (spots[i].rect.w || spots[i].rect.h))
            ++live;
    /* DAT_004DF8A4, the state var. The oracle emits this as `front_state`, and it
     * is the key that tells a script which screen it is actually looking at:
     * 0 title, 1 menu, 2 "Where Do You Want To Play Today?", 3 world list,
     * 4 story, 5 Well, 6 scene, 7 join, 8 death, 9/10 editor, 11 web.
     * Oracle4's live run shows the ORIGINAL parking at 2 for 9 s after Play now
     * in solo, so this key is how that gets compared rather than guessed at. */
    dump_emit_int(emit,"front_state",state,user);
    dump_emit_int(emit,"front.tos_open",tos_open,user);
    dump_emit_int(emit,"front.hotspot_count",live,user);
    dump_emit_int(emit,"front.client_w",PLAT_SCREEN_W,user);
    dump_emit_int(emit,"front.client_h",PLAT_SCREEN_H,user);
    for (i = 0; i < HOTSPOT_SLOTS; ++i) {
        const Hotspot *sp = &spots[i];
        if (sp->state < 1 || sp->state > 2) continue;
        if (!sp->rect.w && !sp->rect.h) continue;
        snprintf(key,sizeof(key),"front.hotspot.%d.state",i);
        dump_emit_int(emit,key,sp->state,user);
        snprintf(key,sizeof(key),"front.hotspot.%d.clickable",i);
        dump_emit_int(emit,key,(sp->flags & 0x400)?1:0,user);
        snprintf(key,sizeof(key),"front.hotspot.%d.msg",i);
        dump_emit_int(emit,key,sp->msg,user);
        snprintf(key,sizeof(key),"front.hotspot.%d.target",i);
        dump_emit_int(emit,key,sp->lparam,user);
        snprintf(key,sizeof(key),"front.hotspot.%d.rect",i);
        snprintf(message,sizeof(message),"%d,%d,%d,%d",
                 sp->rect.x, sp->rect.y,
                 sp->rect.x + sp->rect.w, sp->rect.y + sp->rect.h);
        emit(key,message,user);
        snprintf(key,sizeof(key),"front.hotspot.%d.label",i);
        emit(key,sp->text,user);
    }
}

static void front_leave(void) { plat_text_input(0); }
static const Screen front_screen={"front",NULL,front_screen_update,front_render,front_leave};

int game_boot(void)
{
    /* FUN_00427D89 runs during InitInstance and allocates slot 0 with the local
     * serial before any soul exists, so the port's very first .her-shaped record
     * must already read in_use=1 with everything else zero. */
    hero_allocate_slot(1);
    front_new_soul_pk=0;
    world_first=0; world_anim=1;
    front_enter_title();
    return 0;
}
