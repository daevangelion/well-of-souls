/* Offline scene VM: the quest.txt scene interpreter.
 *
 * The keyword->opcode table is the one the original really has: 85 `{int opcode; char *name;}`
 * records at DAT_004FAD50, count at DAT_004FAFF8, walked by FUN_0047A6B0 (0x47A6B0) with
 * _stricmp.  The switch below is FUN_0047D577 (0x47D577), one line per call, every case
 * advancing DAT_004E4874+0x120 unless it jumps.  docs/re/script.md section 2.2 lists several
 * opcodes wrongly; this file follows the dumped table and the decompilation.
 *
 * Time: the original stores GetTickCount() samples and compares them (FUN_0049454F stores a
 * start tick and a length in ms; FUN_00490E7C cases 2/9/0xb compare `now - start > length`).
 * There is no simulation tick (docs/re/timing.md), so everything here runs on clock_ms().
 *
 * Owner: scene.c. */
#include "scene.h"
#include "game.h"
#include "hero.h"
#include "battle.h"
#include "panels.h"
#include "items.h"
#include "minigame.h"
#include "html.h"
#include "missions.h"
#include "../game_main.h"
#include "../engine/clock.h"
#include "../engine/screen.h"
#include "../engine/font.h"
#include "../engine/text.h"
#include "../engine/log.h"
#include "../engine/rng.h"
#include "../engine/boot.h"
#include "../engine/encint.h"
#include "../platform/platform.h"
#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ACTORS 64
#define ARGS 128
#define DIALOG 2048
#define COOKIE_BUF 1024
#define SCENE_TIMERS 10    /* FUN_0049454F: `0 < id < 10`; ids 0..9 */
#define CALL_DEPTH 10000  /* FUN_00479120: `DAT_004FA690 < 9999` */
#define PUSH_DEPTH 1000   /* FUN_004792D1: `DAT_004FA694 < 999` */
#define SCENE_W 360       /* FUN_0048B13C: the logical scene width, DAT_004E4874+0x3E080 < 100 */
#define SCENE_H 256       /* the logical scene height (actor y is 8.8 percent of it) */

/* --- opcode table (DAT_004FAD50, verbatim order) ---------------------------- */
enum {
    OP_THEME=0, OP_MUSIC, OP_SOUND, OP_IF, OP_GOTO, OP_ACTOR, OP_MOVE, OP_POSE,
    OP_SELECT, OP_QUOTE, OP_WAIT, OP_FIGHT, OP_GIVE, OP_TAKE, OP_OFFER, OP_ASK,
    OP_END, OP_GAME, OP_OFFER2, OP_BKGND, OP_WEATHER, OP_FX, OP_LOCK, OP_TOKEN,
    OP_COUNTDOWN, OP_FIGHT2, OP_SET, OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD,
    OP_PARTY, OP_COMPARE, OP_IF_EQ, OP_IF_GT, OP_IF_LT, OP_HOST, OP_COLOR, OP_FACE,
    OP_NARRATION, OP_HTML, OP_FLAGS, OP_MENU, OP_STRCMP, OP_STRSTR, OP_HOST_GIVE,
    OP_HOST_TAKE, OP_SET_LEN, OP_SET_SUBSTR, OP_EJECT, OP_PARTY_GIVE, OP_PARTY_TAKE,
    OP_GET_SERVER_VAR, OP_SET_SERVER_VAR, OP_IF_LE, OP_IF_GE, OP_IF_NE, OP_CALL,
    OP_RETURN, OP_PUSH, OP_POP, OP_F_ADD, OP_F_SUB, OP_F_MUL, OP_F_DIV, OP_F_MOD,
    OP_TIMER, OP_AND, OP_OR, OP_NOT, OP_XOR, OP_IF_EVEN, OP_IF_ODD, OP_MISSION,
    OP_F_COMPARE, OP_SHUFFLE, OP_STRLWR, OP_NTH_TOKEN,
    OP_LABEL = -2,        /* a `@name` line, no-op */
    OP_SPEECH = 9,        /* ' speech, N: and N: narration, and "digit:" all land here */
    OP_UNKNOWN = -1
};
typedef struct { const char *name; int op; } Keyword;
static const Keyword keywords[] = {
    {"THEME",OP_THEME},{"MUSIC",OP_MUSIC},{"SOUND",OP_SOUND},{"IF",OP_IF},{"GOTO",OP_GOTO},
    {"ACTOR",OP_ACTOR},{"MOVE",OP_MOVE},{"POSE",OP_POSE},{"SELECT",OP_SELECT},{"SEL",OP_SELECT},
    {"SAY",OP_SELECT},{"'",OP_QUOTE},{":",OP_QUOTE},{"WAIT",OP_WAIT},{"FIGHT",OP_FIGHT},
    {"GIVE",OP_GIVE},{"TAKE",OP_TAKE},{"OFFER",OP_OFFER},{"ASK",OP_ASK},{"END",OP_END},
    {"GAME",OP_GAME},{"OFFER2",OP_OFFER2},{"BKGND",OP_BKGND},{"WEATHER",OP_WEATHER},
    {"FX",OP_FX},{"LOCK",OP_LOCK},{"TOKEN",OP_TOKEN},{"COUNTDOWN",OP_COUNTDOWN},
    {"FIGHT2",OP_FIGHT2},{"SET",OP_SET},{"ADD",OP_ADD},{"SUB",OP_SUB},{"MUL",OP_MUL},
    {"DIV",OP_DIV},{"MODULUS",OP_MOD},{"MOD",OP_MOD},{"PARTY",OP_PARTY},{"COMPARE",OP_COMPARE},
    {"IF=",OP_IF_EQ},{"IF>",OP_IF_GT},{"IF<",OP_IF_LT},{"IF<=",OP_IF_LE},{"IF>=",OP_IF_GE},
    {"IF!=",OP_IF_NE},{"IF<>",OP_IF_NE},{"IFEVEN",OP_IF_EVEN},{"IFODD",OP_IF_ODD},
    {"HOST",OP_HOST},{"COLOR",OP_COLOR},{"FACE",OP_FACE},{"NARRATION",OP_NARRATION},
    {"HTML",OP_HTML},{"FLAGS",OP_FLAGS},{"MENU",OP_MENU},{"STRCMP",OP_STRCMP},
    {"STRSTR",OP_STRSTR},{"HOST_GIVE",OP_HOST_GIVE},{"HOST_TAKE",OP_HOST_TAKE},
    {"SET_LEN",OP_SET_LEN},{"SET_SUBSTR",OP_SET_SUBSTR},{"EJECT",OP_EJECT},
    {"PARTY_GIVE",OP_PARTY_GIVE},{"PARTY_TAKE",OP_PARTY_TAKE},
    {"GET_SERVER_VAR",OP_GET_SERVER_VAR},{"SET_SERVER_VAR",OP_SET_SERVER_VAR},
    {"CALL",OP_CALL},{"RETURN",OP_RETURN},{"PUSH",OP_PUSH},{"POP",OP_POP},
    {"F_ADD",OP_F_ADD},{"F_SUB",OP_F_SUB},{"F_MUL",OP_F_MUL},{"F_DIV",OP_F_DIV},
    {"F_MOD",OP_F_MOD},{"TIMER",OP_TIMER},{"AND",OP_AND},{"OR",OP_OR},{"NOT",OP_NOT},
    {"XOR",OP_XOR},{"MISSION",OP_MISSION},{"MISSIONS",OP_MISSION},{"F_COMPARE",OP_F_COMPARE},
    {"SHUFFLE",OP_SHUFFLE},{"STRLWR",OP_STRLWR},{"NTH_TOKEN",OP_NTH_TOKEN}
};
#define KEYWORD_COUNT ((int)(sizeof keywords/sizeof keywords[0]))

/* The interpreter's suspended states (FUN_00490E7C's switch on scene+0x84). */
typedef enum {
    ST_RUN = 1,    /* running the script */
    ST_WAIT,       /* 2: WAIT, `now - start > length` */
    ST_ENDED,      /* 3 */
    ST_FIGHT_MAP,  /* 4: ordinary encounter */
    ST_FIGHT,      /* 5: forced encounter */
    ST_FIGHT_ROUND,/* 6 */
    ST_ASK,        /* 9 */
    ST_HTML,       /* 10 */
    ST_PARTY_WAIT  /* 0xb */
} Suspend;

typedef struct {
    int used, pose[3], poses, frame, face;
    uint32_t started;   /* GetTickCount() sample the pose cycle began */
    int x, y, tx, ty;   /* percentage * 256 (the original keeps y as 8.8 percent) */
    char name[32];
    Sheet sheet;
} Actor;

typedef struct { uint32_t start, length; } SceneTimer; /* FUN_0049454F's three int triples */

typedef struct { int return_line, return_scene; } CallFrame;

static struct {
    int number, pc, end, selected, locked, hide_hero;
    int64_t condition_code;               /* DAT_004FB008 */
    Suspend state;
    BattleResult outcome;
    Link link;
    Image background;
    Sheet hero;
    Actor actors[ACTORS], host;
    /* speech bubble */
    char dialog[DIALOG], speaker[32], reply[128], ask[128];
    int bubble_owner, reveal; uint32_t bubble_start, bubble_ms;

    int yes, asked_seconds;
    /* WAIT / ASK deadline: FUN_0047D577 stores GetTickCount() at scene+0x3E038 and
     * ftol(atof(arg)) at +0x3E03C; FUN_00490E7C compares the two. */
    uint32_t wait_start, wait_length;
    uint32_t countdown_start, countdown_length;  /* DAT_004FA850 / DAT_004FA854 */
    SceneTimer timers[SCENE_TIMERS];
    CallFrame calls[1];                    /* 0..CALL_DEPTH-1, grown lazily */
    int call_depth;
    char pushes[PUSH_DEPTH][COOKIE_BUF];  /* FUN_004792D1's DAT_007F14B0 */
    int push_depth;
    char title[128];
    uint32_t flags;                        /* DAT_004FB030 */
    int weather, fx, color_table;          /* scene+0x3E064 / +0x3E060 / +0x3E06C */
    int theme_id, theme_period, theme_left;
    void *theme;
    size_t theme_size;
    int last_give, last_attack, last_spell; /* DAT_00502B00 / +0x02B08 / +0x02B04 */
    /* Anti-tamper / PK state the original keeps in the hero record at +0x296 (the M<n>
     * cheat bitmask), +0x1A6 (the PK flag) and +0x28B (PK kills). FrontHero does not expose
     * them, so the VM owns them; they stay 0 offline, which is the correct solo answer. */
    uint32_t cheat_mask;
    int pk_kills;
    int item_id, spell_id, monster_id;
} vm;
static const Screen scene_screen;

/* ------------------------------------------------------------------ helpers */
static int eq(const char *a, const char *b) { return text_casecmp(a,b)==0; }
/* _strnicmp(s, lit, n): at most n characters, case-insensitive. */
static int eq_n(const char *s,const char *lit,int n)
{
    int i;
    for(i=0;i<n;i++) {
        int a=tolower((unsigned char)s[i]),b=tolower((unsigned char)lit[i]);
        if(a!=b) return 0;
        if(!a) return 1;
    }
    return 1;
}
/* _strnicmp: case-insensitive prefix compare, the original's cookie-name test. */
static int prefix(const char *s,const char *pfx,size_t n)
{
    size_t i;
    for(i=0;i<n;i++) {
        int a=tolower((unsigned char)s[i]),b=tolower((unsigned char)pfx[i]);
        if(a!=b) return 0;
        if(!a) return 1;
    }
    return 1;
}
static void copy(char *out, size_t cap, const char *in)
{
    size_t n = strlen(in);
    if (!cap) return;
    if (n >= cap) n=cap-1;
    memmove(out,in,n); out[n]=0;
}
static void append(char *out,size_t cap,size_t *used,const char *s)
{
    while(*s && *used+1<cap) out[(*used)++]=*s++;
    out[*used]=0;
}
static int number(const char *s)
{
    char *end;
    long n=strtol(s,&end,(s[0]=='0'&&(s[1]=='x'||s[1]=='X'))?16:10);
    if (end==s) return 0;
    return n>INT_MAX ? INT_MAX : n<INT_MIN ? INT_MIN : (int)n;
}
static int bounded_add(int value, int amount, int max)
{
    int64_t n=(int64_t)value+amount;
    return n<0 ? 0 : n>max ? max : (int)n;
}
static void opcode_of(const char *tok0,int *op)
{
    unsigned char b=(unsigned char)tok0[0];
    int i;
    if(b=='@') { *op=OP_LABEL; return; }
    if(b=='\''||b=='"') { *op=OP_QUOTE; return; }
    if(isdigit(b)&&tok0[1]==':') { *op=OP_QUOTE; return; }
    if(toupper(b)=='H'&&tok0[1]==':') { *op=OP_HOST; return; }
    if(toupper(b)=='N'&&tok0[1]==':') { *op=OP_NARRATION; return; }
    for(i=0;i<KEYWORD_COUNT;i++) if(eq(tok0,keywords[i].name)) { *op=keywords[i].op; return; }
    *op=OP_UNKNOWN;
}

/* --- cookies (FUN_0047AB07 writer / FUN_0047C6DA reader) ------------------- */
/* Storage is the hero's own per-hero INI [cookies] section, owned by hero.c
 * (hero_cookie_get/hero_cookie_set). The interception of the special names below is the
 * opcode layer's half of FUN_0047AB07 and lives here, not in the INI writer. */
/* The [cookies] INI is hero.c's, and it has no enumerator, so the VM remembers every key
 * it has written (FUN_0047AB07 is the only offline writer) so scene_dump can re-read them. */
#define COOKIE_KEYS 512
static char cookie_keys[COOKIE_KEYS][128];
static int cookie_key_count;
static void cookie_remember(const char *key)
{
    int i;
    for(i=0;i<cookie_key_count;i++) if(eq(cookie_keys[i],key)) return;
    if(cookie_key_count<COOKIE_KEYS) copy(cookie_keys[cookie_key_count++],128,key);
}
static void cookie_set(const char *key,const char *value)
{
    /* FUN_0047AB07 special names, handled inline and never written to the INI. */
    if(eq(key,"g.num")) { g_hero.gender=number(value)&3; return; }
    if(eq(key,"item.id")) { vm.item_id=number(value); return; }
    if(eq(key,"spell.id")) { vm.spell_id=number(value); return; }
    if(eq(key,"monster.id")) { vm.monster_id=number(value); return; }
    /* trophies.txt lines 54-60 document the bag-geometry and slot cookies as READ/WRITE, and
     * the write side is items.c's bag, not the INI: a SET of num.TrophyBagWidth/Height resizes
     * it (FUN_0046FED9), which is the only way any scene can reach trophy_bag_resize(). */
    if(eq(key,"num.trophybagwidth")||eq(key,"num.trophybagheight")) {
        int w=0,h=0,v=number(value);
        trophy_bag_size(&w,&h);
        if(eq(key,"num.trophybagwidth")) w=v; else h=v;
        trophy_bag_resize(w,h);
        return;
    }
    if(eq(key,"num.trophybagopen")) return;              /* read-only state, written nowhere */
    if(eq(key,"num.trophybagslotsinuse")) { trophy_bag_clear(); return; }
    { /* "write REPLACEs slot with a SINGLE trophy" / "REPLACEs count, cannot set count if
       * id=0, cannot set count above stackHeight for id" */
        int set_id=prefix(key,"num.trophyidinSlot",18);
        int set_count=prefix(key,"num.trophycountinslot",21);
        if(set_id||set_count) {
            size_t n=set_id?18u:21u;
            int slot=number(key+n),id=0,count=0;
            if(!key[n]) return;                 /* no slot index: not this family */
            trophy_bag_get(slot,&id,&count);
            if(set_id) id=number(value);
            else {
                int stack=trophy_bag_count(id);
                count=number(value);
                if(!id) return;                 /* "cannot set count if id=0" */
                if(stack>0&&count>stack) count=stack;
            }
            trophy_bag_set(slot,id,count);
            return;
        }
    }
    cookie_remember(key);
    hero_cookie_set(key,value);
    /* FUN_0047AB07's common tail, 0x47AE85-0x47AEA1: the derived-stat recomputation ends by
     * re-sealing the local player's COMBATANT level EncInt from the hero's level field --
     * `rec = FUN_0048AE32(hero_serial); if (rec) enc_set(&rec[0x628], rec[0x64]);`. It sits
     * AFTER the g.num / item_id / spell_id / num_hostClass early returns above, so those
     * four names spend no draws, and it costs 4 crt_rand() when the serial lookup hits.
     * The empty-value delete path also reaches this tail, so one call covers both. */
    battle_hero_reseal_level();
}
static const char *cookie_raw(const char *key)
{
    const char *v=hero_cookie_get(key);
    return v ? v : "";
}
/* FUN_00494592: remaining ms of timer `id`; 0 when unset or expired. */
static uint32_t timer_left(int id)
{
    uint32_t now;
    if(id<0||id>=SCENE_TIMERS) return 0;
    if(!vm.timers[id].length) return 0;
    now=clock_ms();
    if(now-vm.timers[id].start>=vm.timers[id].length) return 0;
    return vm.timers[id].length-(now-vm.timers[id].start);
}
/* The stock cookie families of FUN_0047C6DA that exist offline. Anything unmatched falls
 * through to the hero INI, which is what the original does last. */
static const char *stock_cookie(const char *key,char *buf,size_t cap,int *handled)
{
    int n=0;
    *handled=1;
    /* prefix() returns 1 ON MATCH. These were inverted, which made the timer branch swallow
     * every non-timer key and read past the end of short ones. */
    if(prefix(key,"num.timerLength",14)) {
        int id=key[14]?number(key+14):-1;
        snprintf(buf,cap,"%u",(id>=0&&id<SCENE_TIMERS)?vm.timers[id].length:0u); return buf; }
    if(prefix(key,"num.timerLeft",13)) {
        int id=key[13]?number(key+13):-1;
        snprintf(buf,cap,"%u",(id>=0&&id<SCENE_TIMERS)?timer_left(id):0u); return buf; }
    if(eq(key,"num.peopleInScene")) { snprintf(buf,cap,"%d",1); return buf; }
    if(eq(key,"num.peopleInParty")) { snprintf(buf,cap,"%d",1); return buf; }
    if(eq(key,"num.countDown")) {
        n=0;
        if(vm.countdown_start) {
            uint32_t now=clock_ms();
            if(now-vm.countdown_start<vm.countdown_length)
                n=(int)((vm.countdown_length-(now-vm.countdown_start))/1000);
        }
        snprintf(buf,cap,"%d",n<0?0:n); return buf;
    }
    if(eq(key,"num.isPKAttack")) { snprintf(buf,cap,"%d",0); return buf; }
    if(eq(key,"num.isTactics"))  { snprintf(buf,cap,"%d",0); return buf; }
    if(eq(key,"num.mapNum"))     { snprintf(buf,cap,"%d",g_hero.map); return buf; }
    if(eq(key,"num.mapFlags")||eq(key,"num.sceneMapFlags")) { snprintf(buf,cap,"%u",vm.flags); return buf; }
    if(eq(key,"num.wosVersion")) { snprintf(buf,cap,"%d",0xa97); return buf; } /* 0x4FC52C */
    if(eq(key,"num.hostClass"))  { snprintf(buf,cap,"%d",g_hero.klass+1); return buf; }
    if(eq(key,"num.hostLevel"))  { snprintf(buf,cap,"%d",g_hero.level); return buf; }
    if(eq(key,"num.hostHP"))     { snprintf(buf,cap,"%d",g_hero.hp); return buf; }
    if(eq(key,"num.hostMP"))     { snprintf(buf,cap,"%d",g_hero.mp); return buf; }
    if(eq(key,"num.hostMaxHP"))  { snprintf(buf,cap,"%d",g_hero.max_hp); return buf; }
    if(eq(key,"num.hostMaxMP"))  { snprintf(buf,cap,"%d",g_hero.max_mp); return buf; }
    if(eq(key,"num.hostGP"))     { snprintf(buf,cap,"%lld",(long long)g_hero.gold); return buf; }
    if(eq(key,"num.hostXP"))     { snprintf(buf,cap,"%lld",(long long)g_hero.xp); return buf; }
    if(eq(key,"num.hostTotalXP")){ snprintf(buf,cap,"%lld",(long long)g_hero.xp); return buf; }
    if(eq(key,"num.hostAttack")) { snprintf(buf,cap,"%d",hero_offense(&g_hero)); return buf; }
    if(eq(key,"num.hostDefense")){ snprintf(buf,cap,"%d",hero_defense(&g_hero)); return buf; }
    /* FUN_0047B61A: these are NOT the hero's map position. hostX reads scene+0x3A8 (the host's
     * scene x, 0..FUN_0048B13C() == 0x168) and hostY reads scene+0x3AC (the scene y, 8.8 of 256),
     * and BOTH are reported as a PERCENTAGE:
     *   hostX = (scene+0x3A8) * 100 / FUN_0048B13C()
     *   hostY = ((scene+0x3AC) * 100) >> 8
     * vm.host holds the same scene position in percent*256, which is the port's convention. */
    if(eq(key,"num.hostX"))      { snprintf(buf,cap,"%d",vm.host.x*100/256); return buf; }
    if(eq(key,"num.hostY"))      { snprintf(buf,cap,"%d",vm.host.y*100/256); return buf; }
    if(eq(key,"num.hostAge"))    { snprintf(buf,cap,"%d",0); return buf; }
    if(eq(key,"num.hostPets"))   { snprintf(buf,cap,"%d",pet_count()); return buf; }
    if(eq(key,"num.item"))       { snprintf(buf,cap,"%d",vm.item_id); return buf; }
    if(eq(key,"g.num"))          { snprintf(buf,cap,"%d",g_hero.gender); return buf; }
    if(eq(key,"str.name")||eq(key,"str.soul")) return g_hero.name;
    if(eq(key,"str.worldName"))  return g_world.name;
    if(eq(key,"str.mapName") && g_hero.map>=0 && g_hero.map<WORLD_MAX_MAPS)
        return g_world.maps[g_hero.map].name;
    if(eq(key,"str.actorName"))  return vm.actors[number(key+12)].name;
    if(eq(key,"lastAsk"))        return vm.ask;
    cookie_remember(key);
    *handled=0;
    return cookie_raw(key);
}
/* The `num.trophy<n>` family is owned by items.c's trophy bag. */
static const char *cookie_get(const char *key,char *buf,size_t cap)
{
    int handled=0;
    const char *v;
    if(prefix(key,"num.trophy",10)) {
        /* trophies.txt lines 54-60 document these as live read/write cookies, and they are
         * backed by items.c's real bag, not by the INI. The geometry and slot families must
         * be matched BEFORE the plain <n> count, or they would be eaten by it. */
        static const struct { const char *name; int op; } bag[] = {
            {"num.TrophyBagWidth",0},{"num.TrophyBagHeight",1},{"num.TrophyBagOpen",2},
            {"num.TrophyBagSlots",3},{"num.TrophyBagSlotsInUse",4},{"num.TrophyBagEmptySlots",5},
            {"num.TrophyBagRoom",6},{"num.TrophyIdInSlot",7},{"num.TrophyCountInSlot",8},
            {"num.Trophy",9}
        };
        int b;
        for(b=0;b<10;b++) {
            size_t n=strlen(bag[b].name);
            if(!prefix(key,bag[b].name,n)) continue;
            if(key[n]=='\0' || (b==9 && !isdigit((unsigned char)key[n]))) break; /* not this family */
            if(b==9) { snprintf(buf,cap,"%d",trophy_bag_count(number(key+n))); return buf; }
            { int w=0,h=0,slot,tid,count;
              trophy_bag_size(&w,&h);
              if(b==0){snprintf(buf,cap,"%d",w);return buf;}
              if(b==1){snprintf(buf,cap,"%d",h);return buf;}
              if(b==2){snprintf(buf,cap,"%d",trophy_bag_used());return buf;}
              if(b==3){snprintf(buf,cap,"%d",w*h);return buf;}
              if(b==4){snprintf(buf,cap,"%d",trophy_bag_used());return buf;}
              if(b==5){snprintf(buf,cap,"%d",trophy_bag_free());return buf;}
              if(b==6){snprintf(buf,cap,"%d",trophy_bag_room(number(key+n)));return buf;}
              if(b==7){int s2=number(key+n);
                       if(trophy_bag_get(s2,&tid,&count)){snprintf(buf,cap,"%d",tid);return buf;}
                       snprintf(buf,cap,"0");return buf;}
              slot=number(key+n);
              if(trophy_bag_get(slot,&tid,&count)){snprintf(buf,cap,"%d",count);return buf;}
              snprintf(buf,cap,"0");return buf; }
        }
    }
    v=stock_cookie(key,buf,cap,&handled);
    return handled ? v : cookie_raw(key);
}

/* --- % substitution (FUN_004847CD + FUN_00484B3E) -------------------------- */
/* FUN_00485FF5: the `%3` random insult. It draws 1-3 clauses and CONSUMES the discarded
 * rands at both `(rand()&0xc00)==0` gates whether or not the branch is taken
 * (docs/re/rng_calls.md section 4.3), so the count must be reproduced exactly. */
static const char *insult(int clauses,char *buf,size_t cap)
{
    /* PTR_s_artless_004FB050 (49), PTR_s_base_court_004FB118 (49),
     * PTR_s_apple_john_004FB1E0 (50) and the " " separator DAT_004F7590. */
    static const char *const artless[49] = {
        "artless", "bawdy", "beslubbering", "bootless", "churlish", "clouted", "craven", "currish",
        "dankish", "dissembling", "droning", "errant", "fawning", "fobbing", "froward", "frothy",
        "gleeking", "goatish", "gorbellied", "impertinent", "infectious", "jarring", "loggerheaded", "lumpish",
        "mammering", "mangled", "mewling", "paunchy", "pribbling", "puking", "puny", "qualling",
        "rank", "reeky", "roguish", "ruttish", "saucy", "spleeny", "spongy", "surly",
        "tottering", "unmuzzled", "vain", "venomed", "villainous", "warped", "wayward", "weedy",
        "yeasty"
    };
    static const char *const base_court[49] = {
        "base-court", "bat-fowling", "beef-witted", "beetle-headed", "boil-brained", "clapper-clawed", "clay-brained",
        "common-kissing", "crook-pated", "dismal-dreaming", "dizzy-eyed", "doghearted", "dread-bolted", "earth-vexing",
        "elf-skinned", "fat-kidneyed", "fen-sucked", "flap-mouthed", "fly-bitten", "folly-fallen", "fool-born",
        "full-gorged", "guts-griping", "half-faced", "hasty-witted", "hedge-born", "idle-headed", "ill-breeding",
        "ill-nurtured", "knotty-pated", "milk-livered", "motley-minded", "onion-eyed", "plume-plucked", "pottle-deep",
        "pox-marked", "reeling-ripe", "rough-hewn", "rude-growing", "rump-fed", "shard-borne", "sheep-biting",
        "spur-galled", "swag-bellied", "tardy-gaited", "tickle-brained", "toad-spotted", "unchin-snouted", "weather-bitten"
    };
    static const char *const apple_john[50] = {
        "apple-john", "baggage", "barnacle", "bladder", "boar-pig", "bugbear", "bum-bailey",
        "canker-blossom", "clack-dish", "clotpole", "coxcomb", "codpiece", "death-token", "dewberry",
        "flap-dragon", "flax-wench", "flirt-gill", "foot-licker", "fustilarian", "giglet", "gudgeon",
        "haggard", "harpy", "hedge-pig", "horn-beast", "hugger-mugger", "joithead", "lewdster",
        "lout", "maggot-pie", "malt-worm", "mammet", "measle", "minnow", "miscreant",
        "moldwarp", "mumble-news", "nut-hook", "pigeon-egg", "pignut", "puttock", "pumpion",
        "ratsbane", "scut", "skainsmate", "strumpet", "varlet", "vassal", "whey-face",
        "wagtail"
    };
    size_t n=0;
    int have=0;
    buf[0]=0;
    if(clauses==1) goto pick1;
    if((crt_rand()&0xc00)!=0) {
pick1: have=1;
        snprintf(buf,cap,"%s",artless[crt_rand()%49]);
        n=strlen(buf);
    }
    if(clauses!=2 && (crt_rand()&0xc00)==0) return buf;
    if(have) append(buf,cap,&n," ");
    snprintf(buf+n,cap-n,"%s",base_court[crt_rand()%49]);
    n=strlen(buf);
    if(clauses==3) {
        append(buf,cap,&n," ");
        snprintf(buf+n,cap-n,"%s",apple_john[crt_rand()%50]);
    }
    return buf;
}
/* FUN_004847CD(code, n): the %X resolver. Unknown codes emit a literal "%%C". */
static const char *resolve(int code,int n,char *buf,size_t cap)
{
    buf[0]=0;
    switch(toupper(code)) {
    case '%': buf[0]='%'; buf[1]=0; return buf;
    case '0': return vm.actors[vm.selected].name;
    case '1': return g_hero.name;
    case '2': return g_hero.name;
    case '3': return insult(3,buf,cap);
    case '4': return "";
    case '5': return "";
    case 'C': /* class name: FUN_0049C1EF indexes class*(0x1ACD0)+gender*0x105 */
        if(n<1) return g_hero.klass>=0&&g_hero.klass<WORLD_MAX_CLASSES?g_world.classes[g_hero.klass].name:"";
        return n-1<WORLD_MAX_CLASSES?g_world.classes[n-1].name:"";
    case 'E': /* equipped item id in +EQUIP slot n-10; out of range is hero+0x6A4 (0 offline) */
        n-=10;
        if(n==0) { snprintf(buf,cap,"%d",g_hero.right_hand); return buf; }
        if(n>0&&n<=WORLD_MAX_EQUIP) {
            /* hero.h's slots are 0..5 plus HERO_SLOT_RIGHT_HAND; +EQUIP order is
             * 0 helmet 1 armor 2..9 hands 10 boots 11 shield 12 ring 13 amulet. */
            static const int map[14] = { HERO_SLOT_HELMET, HERO_SLOT_ARMOR, 0,0,0,0,0,0,0,
                                         HERO_SLOT_BOOTS, HERO_SLOT_SHIELD, HERO_SLOT_RING,
                                         HERO_SLOT_AMULET, 0 };
            if(n==2) { snprintf(buf,cap,"%d",g_hero.right_hand); return buf; }
            snprintf(buf,cap,"%d",hero_equipped(&g_hero,map[n]));
            return buf;
        }
        snprintf(buf,cap,"%d",0);
        return buf;
    case 'I': return (n>0&&n<WORLD_MAX_ITEMS)?g_world.items[n].name:"";
    case 'K': /* kills: %K0 total, %Kn of monster n (FrontHero's per-hero INI) */
        if(n==0) { snprintf(buf,cap,"%u",hero_kills_total()); return buf; }
        snprintf(buf,cap,"%d",hero_kills_of_monster(n));
        return buf;
    case 'L': /* FUN_0049C28B: -1 is the host's level number, <1 its name, else level n's */
        if(n==-1) { snprintf(buf,cap,"%d",g_hero.level); return buf; }
        if(n<1) n=g_hero.level;
        if(g_hero.klass<0||g_hero.klass>=WORLD_MAX_CLASSES) { buf[0]=0; return buf; }
        return (n>=0&&n<=WORLD_MAX_LEVELS)?g_world.classes[g_hero.klass].levels[n].name:"";
    case 'M': return (n>0&&n<WORLD_MAX_MONSTERS)?g_world.monsters[n].name:"";
    case 'R': /* FUN_004847CD case 0x52: `rand()%n + 1` */
        if(n>0) { snprintf(buf,cap,"%d",crt_rand()%n+1); return buf; }
        return "";
    case 'S': return (n>0&&n<WORLD_MAX_SPELLS)?g_world.spells[n].name:"";
    case 'T': return world_token_text(n);
    case 'Z': return (n>=0&&n<WORLD_MAX_TROPHIES&&g_world.trophies[n].used)?g_world.trophies[n].name:"";
    default: snprintf(buf,cap,"%%%c",toupper(code)); return buf;
    }
}
/* Expand `%X[n]` and `#<cookie>` in one argument (FUN_00484B3E + FUN_0047CFDE). */
static void expand(const char *s,char *out,size_t cap)
{
    size_t n=0;
    out[0]=0;
    while(*s && n+1<cap) {
        if(s[0]=='#' && s[1]=='<') {
            const char *end=strchr(s+2,'>');
            if(end) {
                char key[128],buf[COOKIE_BUF];
                size_t k=(size_t)(end-s-2);
                if(k>=sizeof key) k=sizeof key-1;
                memcpy(key,s+2,k); key[k]=0;
                append(out,cap,&n,cookie_get(key,buf,sizeof buf));
                s=end+1; continue;
            }
        }
        if(s[0]=='%' && s[1]) {
            int code=(unsigned char)s[1], arg=0, neg=0;
            const char *p=s+2;
            s+=2;
            if(code=='%') { out[n++]='%'; out[n]=0; continue; }
            if(code>='0'&&code<='9') {
                while(*p>='0'&&*p<='9') arg=arg*10+(*p++-'0');
                if(*p=='-') { neg=1; p++; while(*p>='0'&&*p<='9') arg=arg*10+(*p++-'0'); }
                s=p;
            }
            if(neg) arg=-arg;
            { char buf[COOKIE_BUF];
              append(out,cap,&n,resolve(code,arg,buf,sizeof buf)); }
            continue;
        }
        out[n++]=*s++; out[n]=0;
    }
}

/* --- conditions (FUN_004851F1 / FUN_004858B5 / FUN_004859A2) --------------- */
static int atom(const char *term)
{
    size_t len=strlen(term);
    int code,n1=0,n2=0,negate=0;
    char work[256],*dot;
    if(len<2) return 0;
    copy(work,sizeof work,term);
    code=toupper((unsigned char)work[0]);
    n1=atoi(work+1);
    if(len>2) n2=atoi(work+2);
    if(code=='A') return g_hero.hp>0;                          /* ALIVE  */
    if(code=='D') return g_hero.hp<=0;                         /* DEAD   */
    /* FUN_004851F1 cases 0x4C ('L') and 0x57 ('W') both call FUN_0048FD90, which sums
     * FUN_0049B70F (enc_get) over the scene's actor slots and is < 0 only in circumstances
     * the port does not reproduce, so both are false offline. Note this is NOT the fight
     * outcome: despite what docs/re/script.md 3.1 says, `IF WIN` / `IF LOSE` land here. */
    if(code=='W'||code=='L') return 0;
    /* Case 0x58 ('X'): the COUNTDOWN has expired. `IF XP` is this case, since 'X' is the
     * leading letter and atoi("P") is 0 (DAT_004FA850 / DAT_004FA854, all.c:97720). */
    if(code=='X') return vm.countdown_start==0
                    || (clock_ms()-vm.countdown_start)>vm.countdown_length;
    /* Case 0x59 ('Y'): the last ASK reply (DAT_0054C7F8) against seven fixed words, all
     * case-insensitive. Six are _strnicmp at a fixed length and one ("DA") is a full
     * _stricmp. `IF -YES, @saidNo` in Evergreen scene 3 depends on this (quest.txt:1608). */
    if(code=='Y') {
        static const char *const yes[] = {"YES","YEAH","SURE","SI","OK","DA","JA"};
        static const int nlen[] = {3,4,4,2,2,-1,2}; /* -1 = compare the whole string */
        int k;
        for(k=0;k<7;k++) {
            int m=nlen[k];
            if(m<0 ? text_casecmp(vm.ask,yes[k])==0
                   : eq_n(vm.ask,yes[k],m)==0) return 1;
        }
        return 0;
    }
    if(code=='#') return g_hero.map==n1;
    if(code=='C') return g_hero.klass==n1-1;                   /* FUN_004851F1: class == n-1 */
    if(code=='E') { int i;
        if(g_hero.right_hand==n1) return 1;
        for(i=0;i<6;i++) if(g_hero.equip[i]==n1) return 1;
        return 0; }
    if(code=='F') return (vm.flags&(uint32_t)n1)!=0;
    if(code=='G') return g_hero.gold>=n1;
    if(code=='H') return 0;                                    /* age in hours (0 offline) */
    if(code=='I') { dot=strchr(work+1,'.'); if(dot) { *dot=0; n2=atoi(dot+1); } n1=atoi(work+1);
        if(n1<0||n1>0x13ff) return 0;
        return hero_item_count(&g_hero,n1)>=n2; }
    if(code=='J') { /* J nn / JA / JQ / JF: FUN_0045AB32 / 45AB4B / 45A9C7 / 458616 */
        int job=n2?n2:n1;
        if(work[1]=='A') return mission_accepted(job);
        if(work[1]=='Q') return mission_qualified(job);
        return mission_completed(job); }
    if(code=='K') { /* KB n[.x] deaths by monster n; KM n[.x] kills of monster n */
        dot=strchr(work+2,'.'); if(dot) { *dot=0; n2=atoi(dot+1); }
        n1=atoi(work+2);
        if(n1==0) return code=='M'?hero_kills_total()>=(unsigned)n2:hero_deaths_total()>=(unsigned)n2;
        return code=='M'?hero_kills_of_monster(n1)>=n2:hero_deaths_by_monster(n1)>=n2; }
    if(code=='M') { /* M0 cheater / M<n> modified-world bitmask (hero+0x296) */
        if(n1==0) return vm.cheat_mask!=0;
        return (vm.cheat_mask&(uint32_t)n1)!=0; }
    if(code=='N') return 0;                                    /* anti-tamper strings */
    if(code=='P') return vm.pk_kills>=0&&(n1<1||vm.pk_kills>=n1);
    if(code=='Q') { /* the last ASK reply, `^` is a space (FUN_004851F1 case 0x51) */
        char want[256],have[128];
        size_t i;
        for(i=0;work[1+i];i++) want[i]=(work[1+i]=='^')?' ':(char)tolower((unsigned char)work[1+i]);
        want[i]=0;
        for(i=0;vm.ask[i]&&i+1<sizeof have;i++) have[i]=(char)tolower((unsigned char)vm.ask[i]);
        have[i]=0;
        return strstr(have,want)!=NULL; }
    if(code=='R') return crt_rand()%100<n1;                    /* one rand, always */
    if(code=='S') return hero_spell_known(&g_hero,n1);
    if(code=='T') return n1>=0&&n1<HERO_TOKENS&&g_hero.tokens[n1];
    if(code=='V') return g_hero.level>=n1;
    if(code=='Z') { char key[64],buf[64];
        snprintf(key,sizeof key,"num.trophy%d",n1);
        (void)buf; return trophy_bag_count(n1)>=n2; }
    (void)negate;
    return 0;
}
static int condition_evaluate(const char *s)
{
    int clause=1,negate=0;
    while(*s) {
        char term[256];size_t n=0;
        while(*s=='+'||*s=='-'||isspace((unsigned char)*s)) { if(*s=='-') negate=!negate;s++; }
        while(*s&&*s!='+'&&*s!='-'&&*s!='|') { if(n+1<sizeof term) term[n++]=*s;s++; }
        term[n]=0;
        if(n) { int ok=atom(term);clause=clause&&(negate?!ok:ok); }
        negate=0;
        if(*s=='|') { if(clause) return 1;clause=1;s++; }
    }
    return clause;
}
static int mission_condition(const char *condition,void *user)
{
    char buf[4000];
    (void)user;
    copy(buf,sizeof buf,condition);   /* FUN_004859A2 strcpy()s before parsing: it mutates */
    return condition_evaluate(buf);
}
int scene_condition(const char *condition)
{
    return mission_condition(condition,NULL);
}

/* --- label resolution (FUN_0047A77B) --------------------------------------- */
/* Scoped to the current scene and stopped at the next SCENE line; matching is
 * case-insensitive and must end on a token boundary. `47@label` switches scene first. */
static char vlabel_toks[ARGS][256];
static int resolve_label(const char *arg,int *out_scene)
{
    char label[256];
    int scene=vm.number,i,n;
    char (*t)[256]=vlabel_toks;
    const char *at=strchr(arg,'@');
    if(out_scene) *out_scene=0;
    if(at && at!=arg) {
        scene=number(arg);
        if(scene<0||scene>=WORLD_MAX_SCENES||!g_world.scenes[scene].used) return -1;
    }
    copy(label,sizeof label,at?at:arg);
    for(i=scene>=0&&scene<WORLD_MAX_SCENES?g_world.scenes[scene].first_line+1:0;
        i<g_world.line_count; i++) {
        n=world_tokenize(g_world.lines[i],t,ARGS);
        if(prefix(g_world.lines[i],"SCENE",5)) return -1;
        if(n>0 && eq(t[0],label)) {
            if(out_scene) *out_scene=scene;
            return i;
        }
    }
    return -1;
}
static int jump_to(const char *arg)
{
    int scene=0,line=resolve_label(arg,&scene);
    if(line<0) {
        wos_log_event("scene_error","op=GOTO reason=label_not_found label=%s",arg);
        return 0;
    }
    vm.number=scene;
    vm.pc=line+1;                        /* FUN_0047A77B: resume AFTER the label */
    vm.end=g_world.scenes[scene].end_line;
    return 1;
}

/* --- arithmetic / logic (FUN_0047D1DF, FUN_0047D374) ----------------------- */
/* op: 0 add, 1 sub, 2 mul, 3 div, 4 mod. is_float selects the double path and the
 * "%f" cookie format (DAT_004ECECC). The integer path uses atoi and "%d". */
static void math_op(int op,int is_float,const char *cookie,const char *amount)
{
    char value[COOKIE_BUF],buf[64];
    int a=atoi(cookie_get(cookie,buf,sizeof buf)),b=atoi(amount),i=a;
    double fa=atof(cookie_get(cookie,buf,sizeof buf)),fb=atof(amount);
    if(op==3||op==4) {
        if(is_float) {
            double r = op==3 ? fa/fb : fmod(fa,fb);
            snprintf(value,sizeof value,"%f",r);
            /* FUN_0047D1B9: the condition code is 0 unless the result is out of range. */
            vm.condition_code = (r<=-1.0e9||r>=1.0e9) ? 0 : -1;
            cookie_set(cookie,value);
            return;
        }
        if(!b) return;                  /* FUN_0047D1DF: division by zero is a no-op */
        i = op==3 ? a/b : a%b;
    } else if(op==0) i=a+b;
    else if(op==1) i=a-b;
    else if(op==2) i=a*b;
    if(is_float) {
        double r = op==0 ? fa+fb : op==1 ? fa-fb : op==2 ? fa*fb : (op==3?fa/fb:fmod(fa,fb));
        snprintf(value,sizeof value,"%f",r);
        vm.condition_code = (r<=-1.0e9||r>=1.0e9) ? 0 : -1;
    } else {
        snprintf(value,sizeof value,"%d",i);
        vm.condition_code=i;
    }
    cookie_set(cookie,value);
}
/* FUN_0047D374: 0 AND, 1 OR, 2 NOT (~amount, ignoring the cookie), 3 XOR -> 0. */
static void logic_op(int op,const char *cookie,const char *amount)
{
    char value[64],buf[64];
    unsigned a=(unsigned)atoi(cookie_get(cookie,buf,sizeof buf)),b=(unsigned)atoi(amount),r;
    if(op==0) r=a&b; else if(op==1) r=a|b; else if(op==2) r=~b; else r=0;
    snprintf(value,sizeof value,"%u",r);
    cookie_set(cookie,value);
    vm.condition_code=(int)r;
}

/* --- GIVE / TAKE (FUN_00484E72) -------------------------------------------- */
/* A spec is "<letter><id>[.count]"; param_3 < 0 takes. */
static int give_object(const char *spec,int count)
{
    char work[128],*dot;
    int id;
    copy(work,sizeof work,spec);
    dot=strchr(work,'.'); if(dot) *dot=0;
    id=atoi(work+1);
    if(dot) { int c=atoi(dot+1); if(c<2) c=1; if(c>9999) c=10000; count*=c; }
    switch(toupper((unsigned char)work[0])) {
    case 'G': hero_add_gold(&g_hero,count<0?-(int64_t)id:id); return 1;
    case 'H': if(g_hero.hp>0) g_hero.hp=bounded_add(g_hero.hp,count<0?-id:id,g_hero.max_hp); return 1;
    case 'L': if(count<0) g_hero.hp=0; else if(g_hero.hp<=0) g_hero.hp=1; return 1;  /* resurrect */
    case 'M': g_hero.mp=bounded_add(g_hero.mp,count<0?-id:id,g_hero.max_mp); return 1;
    case 'P': hero_add_pp(&g_hero,count<0?-(int64_t)id:id); return 1;
    case 'S': hero_set_spell(&g_hero,id,count>0); return 1;
    case 'T': if(id>=0&&id<HERO_TOKENS) g_hero.tokens[id]=(unsigned char)(count>0); return 1;
    case 'I': if(count<0) { int have=hero_item_count(&g_hero,id); if(id>have) id=have;
                 if(id>0) hero_take_item(&g_hero,id,id); }
               else hero_give_item(&g_hero,id,count);
               return 1;
    case 'Z': return trophy_bag_take(id,count<0?-count:count) || count>0;
    default: return 0;
    }
}
static int mission_give(const char *spec,int count,void *user) { (void)user; return give_object(spec,count); }

/* --- presentation helpers ------------------------------------------------- */
static int asset_path(char *path,size_t cap,const char *folder,const char *name,const char *ext)
{
    char rel[512]; FILE *f;
    snprintf(rel,sizeof rel,"%s/%s%s",folder,name,strrchr(name,'.')?"":ext);
    world_path(path,(int)cap,rel);
    f=plat_fopen(path,"rb");
    if(f) { fclose(f);return 1; }
    world_data_path(path,(int)cap,rel);
    f=plat_fopen(path,"rb");
    if(f) { fclose(f);return 1; }
    return 0;
}
static void background(const char *name)
{
    char path[640];
    if(!*name) return;
    image_free(&vm.background);
    if(asset_path(path,sizeof path,"scenes",name,".jpg")) image_load(&vm.background,path);
}
static void sound(const char *name)
{
    char path[640];size_t len;char *bytes;
    if(!*name || !asset_path(path,sizeof path,"sfx",name,".wav")) return;
    bytes=text_read_file(path,&len);
    if(bytes) { plat_sound_play(bytes,len);free(bytes); }
}
static unsigned le32(const unsigned char *p)
{
    return (unsigned)p[0]|((unsigned)p[1]<<8)|((unsigned)p[2]<<16)|((unsigned)p[3]<<24);
}
/* THEME (opcode 0). -1 stops, 0 uses the nearest link's theme; the WAV loops until
 * a different theme or theme -1. */
static void theme(int id)
{
    int i,inside=0,n;
    char (*t)[256]=vlabel_toks;
    char path[640];
    free(vm.theme);vm.theme=NULL;vm.theme_size=0;vm.theme_period=0;vm.theme_left=0;
    if(id<0) return;
    if(!id) id=vm.link.theme;
    vm.theme_id=id;
    for(i=0;i<g_world.line_count;i++) {
        n=world_tokenize(g_world.lines[i],t,ARGS);
        if(n<=0) continue;
        if(eq(t[0],"+THEMES")) { inside=1;continue; }
        if(eq(t[0],"-THEMES")) break;
        if(inside && n>=3 && number(t[0])==id) {
            const char *ext=strrchr(t[2],'.');
            if(ext&&(eq(ext,".mid")||eq(ext,".midi"))) { game_music(t[2]);return; }
            if(asset_path(path,sizeof path,"sfx",t[2],".wav")) {
                size_t off=12,data=0;unsigned rate=0;
                vm.theme=text_read_file(path,&vm.theme_size);
                if(!vm.theme) return;
                while(off+8<=vm.theme_size) {
                    const unsigned char *p=(const unsigned char *)vm.theme+off;
                    unsigned len=le32(p+4);
                    if(len>vm.theme_size-off-8) break;
                    if(!memcmp(p,"fmt ",4)&&len>=16) rate=le32(p+16);
                    if(!memcmp(p,"data",4)) data=len;
                    off+=8+(size_t)len+(len&1u);
                }
                if(rate && data && data<=INT_MAX/1000) vm.theme_period=(int)(data*1000/rate);
                vm.theme_left=vm.theme_period;
                plat_sound_play(vm.theme,vm.theme_size);
            }
            return;
        }
    }
}
static void finish(void)
{
    hero_save(&g_hero);
    html_close();
    if(vm.number==0) game_go_well(); else game_return_to_map();
}
/* FUN_0046C3E0: the bubble lifetime is max(1500, min(strlen*100, 10000)) ms. */
static uint32_t bubble_ms(const char *text)
{
    uint32_t n=(uint32_t)strlen(text)*100u;
    if(n>10000u) n=10000u;
    if(n<1500u) n=1500u;
    return n;
}
/* FUN_0047CFDE stores the text at actor+0x4E with its start tick at actor+0x9B. */
static void say(const char *text,int owner)
{
    expand(text,vm.dialog,sizeof vm.dialog);
    vm.bubble_owner=owner;
    copy(vm.speaker,sizeof vm.speaker,owner>=0&&owner<ACTORS?vm.actors[owner].name:"");
    vm.reveal=0;
    vm.bubble_start=clock_ms();
    vm.bubble_ms=bubble_ms(vm.dialog);
    vm.state=ST_RUN;
}
/* FUN_0046CEAB queues narration (DAT_004F8148, capped at 4) through the same channel, but
 * it is not a speech bubble: the original does not block on it. */
static void narration(const char *text)
{
    expand(text,vm.dialog,sizeof vm.dialog);
    copy(vm.speaker,sizeof vm.speaker,"");
    vm.bubble_owner=-2;
    vm.reveal=0;
    vm.bubble_start=clock_ms();
    vm.bubble_ms=bubble_ms(vm.dialog);
    vm.state=ST_RUN;
}

/* --- FIGHT (FUN_0047D577 switchD_0047D643_caseD_b, 0x47F9C8) --------------- */
static void begin_fight(const int *ids,int count,unsigned mods)
{
    int difficulty=vm.link.difficulty,distance_pct=20;
    if(!count && !(mods&BATTLE_MOD_RANDOM)) {
        int n=game_take_pending_fight(NULL,0,&difficulty,&distance_pct);
        (void)n;
    }
    battle_begin_ex(ids,count,difficulty,distance_pct,mods);
    vm.state=(mods&BATTLE_MOD_RANDOM)?ST_FIGHT_MAP:ST_FIGHT;
    wos_log_event("scene_fight","scene=%d monsters=%d mods=%u",vm.number,count,mods);
}
static void fight_op(char t[][256],int n,int sticky)
{
    int ids[ARGS],count=0,i,id;
    unsigned mods=sticky?BATTLE_MOD_STICKY:0;
    int saw_star=0,use_map=0;
    if(n==2 && number(t[1])==0) { use_map=1; mods|=BATTLE_MOD_RANDOM; }  /* FIGHT 0 */
    for(i=1;i<n;i++) {
        if(eq(t[i],"*")) { saw_star=1; mods|=BATTLE_MOD_RANDOM; continue; }
        if(t[i][0]=='+') { mods|=BATTLE_MOD_PET_HATE; }
        id=number(t[i]);
        if(id) ids[count++]=id;
    }
    if(saw_star && !use_map) mods|=BATTLE_MOD_RANDOM;
    if(use_map) { count=0; mods|=BATTLE_MOD_RANDOM; }
    if(mods&BATTLE_MOD_PET_HATE) battle_set_pets(pet_count());
    begin_fight(ids,count,mods);
}

/* --- the interpreter ------------------------------------------------------ */
static void offer(char t[][256],int n,int filtered)
{
    const char *args[ARGS];
    int i;
    for(i=1;i<n;i++) args[i-1]=t[i];
    panel_open_shop(args,n-1,filtered);
}
/* SET_SUBSTR (0x31): clamp start and length into the string, then truncate. */
static void set_substr(const char *cookie,int start,int len)
{
    char value[COOKIE_BUF],buf[64];
    size_t slen;
    copy(value,sizeof value,cookie_get(cookie,buf,sizeof buf));
    slen=strlen(value);
    if(start<1) start=0;
    if(len<1) len=0;
    if(start>(int)slen-1) start=(int)slen-1;
    if(start<0) start=0;
    if(len>(int)slen-start) len=(int)slen-start;
    if(start>(int)slen) start=(int)slen;
    value[start+len]=0;
    cookie_set(cookie,value);
}
/* NTH_TOKEN (0x4E): skip `n` space/comma-separated tokens, keep from the next one. */
static void nth_token(const char *cookie,int skip,int take,const char *delims,const char *src)
{
    char work[COOKIE_BUF],*p;
    copy(work,sizeof work,src);
    p=work;
    for(;skip>0;skip--) {
        char *q;
        while(*p&&!strchr(delims,*p)) p++;
        if(!*p) { cookie_set(cookie,""); return; }
        while(*p&&strchr(delims,*p)) p++;
        (void)q;
    }
    if(take) while(*p==' ') p++;
    cookie_set(cookie,p);
}
/* SHUFFLE (0x4C -> FUN_0047D49C): Fisher-Yates with 100000 iterations of 2 rands,
 * writing <base><i> for i in 0..n-1 and <base>Count. Requires 0 < n < 256. */
static void shuffle(const char *base,int n)
{
    int order[256],i,k;
    char name[128],value[64];
    if(n<1||n>0xff) { snprintf(name,sizeof name,"%sCount",base); cookie_set(name,"0"); return; }
    for(i=0;i<n;i++) order[i]=i;
    for(k=0;k<100000;k++) {
        int a=crt_rand(),b=crt_rand();
        int ia=(int)(((unsigned)(a>>31)<<31)+((unsigned)a>>2))%(unsigned)n;
        int ib=(int)(((unsigned)(b>>31)<<31)+((unsigned)b>>2))%(unsigned)n;
        int t=order[ia]; order[ia]=order[ib]; order[ib]=t;
    }
    for(i=0;i<n;i++) { snprintf(name,sizeof name,"%s%d",base,i);
        snprintf(value,sizeof value,"%d",order[i]); cookie_set(name,value); }
    snprintf(name,sizeof name,"%sCount",base);
    snprintf(value,sizeof value,"%d",n);
    cookie_set(name,value);
}
/* TIMER (0x43 -> FUN_0049454F): ids 0..9; a length of 0 disarms. */
static void timer_set(int id,uint32_t length_ms)
{
    if(id<0) { memset(vm.timers,0,sizeof vm.timers); return; }
    if(id>=SCENE_TIMERS) { wos_log_event("scene_error","op=TIMER reason=id"); return; }
    vm.timers[id].length=length_ms;
    vm.timers[id].start=clock_ms();
    if(!length_ms) vm.timers[id].start=0;
}
/* CALL (0x3A) / RETURN (0x3B) / PUSH (0x3C) / POP (0x3D). */
static void call_push(char t[][256],int n)
{
    int i,count=0;
    char key[64],value[COOKIE_BUF];
    /* FUN_004791AA stores the arguments as arg0..arg9 and counts them in numArgs. */
    for(i=1;i<n && i<=10;i++) {
        if(!t[i][0]) { snprintf(key,sizeof key,"arg%d",i-1); cookie_set(key,""); continue; }
        expand(t[i],value,sizeof value);
        snprintf(key,sizeof key,"arg%d",i-1);
        cookie_set(key,value);
        count++;
    }
    snprintf(key,sizeof key,"numArgs"); snprintf(value,sizeof value,"%d",count);
    cookie_set(key,value);
    if(vm.call_depth<CALL_DEPTH) {
        vm.calls[vm.call_depth].return_line=vm.pc;
        vm.calls[vm.call_depth].return_scene=vm.number;
    }
    vm.call_depth++;
}
static int call_pop(int *out_scene)
{
    if(vm.call_depth<=0) return -1;
    vm.call_depth--;
    if(out_scene) *out_scene=vm.calls[vm.call_depth].return_scene;
    return vm.calls[vm.call_depth].return_line;
}
static void push_cookie(const char *key)
{
    if(vm.push_depth>=PUSH_DEPTH) { wos_log_event("scene_error","op=PUSH reason=full"); return; }
    copy(vm.pushes[vm.push_depth],COOKIE_BUF,cookie_raw(key));
    vm.push_depth++;
}
static void pop_cookie(const char *key)
{
    if(vm.push_depth<=0) { wos_log_event("scene_error","op=POP reason=empty"); return; }
    vm.push_depth--;
    cookie_set(key,vm.pushes[vm.push_depth]);
    vm.pushes[vm.push_depth][0]=0;
}
/* FACE (0x27 -> FUN_0048A8AE): 0/1 fixed, 2 = toward the host, 3 = away, 4 = toggle. */
/* FUN_0048A8AE only accepts slots 0..0x8F, and the host is reached as slot -1. */
static void face(int slot,int dir)
{
    Actor *a = slot<0 ? &vm.host : (slot<ACTORS ? &vm.actors[slot] : NULL);
    int toward;
    if(!a || !a->used) return;
    /* dir 2/3 compare the actor's x against the host's x (scene+0x3A8 vs actor+0x280). */
    toward = a->x<=vm.host.x ? 0 : 1;
    switch(dir) {
    case 0: a->face=0; break;
    case 1: a->face=1; break;
    case 2: a->face=toward; break;
    case 3: a->face=!toward; break;
    case 4: a->face=!a->face; break;
    default: break;
    }
}
/* FUN_0047D577 case 0x0C..0x34 all funnel into FUN_00484E72 per argument. The
 * item.id / spell.id / monster.id cookies are what the LAST give wrote, so a GIVE I<id>
 * updates them (they exist for the `item.*` / `spell.*` / `monster.*` cookie families). */
static void give_all(char toks[][256],int n,int take,int host_only)
{
    int i;
    for(i=1;i<n;i++) {
        const char *spec=toks[i];
        char letter;
        if(!spec || !spec[0]) continue;
        letter=(char)toupper((unsigned char)spec[0]);
        /* FUN_00484E72 guards on strlen(spec) > 1 and then switches on the leading letter with
         * NO default case, so an argument it does not recognise is dropped in silence. Lines
         * with a mid-line ";" comment hand it trailing words constantly (quest.txt:1629, 2012),
         * and the original emits nothing for them. Logging here would be a port-only difference
         * in the very output the differential comparison reads, so it stays suppressed. */
        if(!give_object(spec,take?-1:1)) continue;
        if(spec[1]=='\0') continue;   /* a bare letter has no id to record */
        if(letter=='I') cookie_set("item.id",spec+1);
        else if(letter=='S') cookie_set("spell.id",spec+1);
        else if(letter=='L') cookie_set("monster.id",spec+1);
    }
    wos_log_event("scene_give","count=%d take=%d host=%d",n-1,take,host_only);
}
/* FUN_0047A0E4 writes up to 128 tokens at a 0x104 stride; the original keeps that array in
 * .data, not on the stack, and 32 KB of locals would overflow here. */
static char vt[ARGS][256];
static char vexpanded[COOKIE_BUF];

static void step(void)
{
    char (*t)[256]=vt;
    const char *raw;
    int n,i,op,scene_out;
    if(vm.pc<0 || vm.pc>=vm.end) { finish();return; }
    raw=g_world.lines[vm.pc];
    n=world_tokenize(raw,t,ARGS);
    if(n<=0) { vm.pc++;return; }
    opcode_of(t[0],&op);
    wos_log_event("scene_op","scene=%d line=%d op=%s",vm.number,vm.pc,t[0]);
    for(i=1;i<n;i++) { expand(t[i],vexpanded,sizeof vexpanded);copy(t[i],256,vexpanded); }
    vm.pc++;

    /* The speech forms take their text from the RAW LINE, not the token buffer
     * (FUN_0047D577 case 9: `line + 2`), and N:/H: first skip past the first space. */
    if(op==OP_QUOTE || op==OP_HOST || op==OP_NARRATION) {
        int owner=vm.selected;
        const char *p=raw;
        char text[DIALOG];
        size_t len;
        while(isspace((unsigned char)*p)) p++;
        if(p[1]==':') {
            if(isdigit((unsigned char)p[0])) { owner=p[0]-'0'; vm.selected=owner; }
            else if(toupper((unsigned char)*p)=='H') owner=-1;      /* the host speaks */
            else if(toupper((unsigned char)*p)=='N') owner=-2;      /* narration */
            p+=2;
            while(*p==' ') p++;
        } else p++;
        len=strlen(p);
        if(len>=sizeof text) len=sizeof text-1;
        memcpy(text,p,len); text[len]=0;
        if(op==OP_NARRATION) narration(text);
        else say(text,op==OP_HOST?-1:owner);
        return;
    }
    switch(op) {
    case OP_LABEL: return;                                   /* case -2: no-op */
    case OP_THEME: theme(n>1?number(t[1]):0); return;
    case OP_MUSIC: game_music(n>1?t[1]:NULL); return;
    case OP_SOUND: if(n>1) sound(t[1]); return;
    case OP_IF: { /* the label is the last argument; the condition list is everything before it.
        * Every IF label in every world starts with '@', so when a line carries a trailing
        * ";" comment that the loader did not strip, the first '@' token is the label and the
        * trailing words are neither label nor condition. Taking "last token starting with '@'"
        * is correct whether or not the comment survived, and it degrades to the plain last
        * token when a script ever omits the '@'. */
        char cond[DIALOG];size_t used=0;cond[0]=0;
        int label=n-1;
        for(i=1;i<n;i++) if(t[i][0]=='@') { label=i; break; }
        for(i=1;i<label;i++) append(cond,sizeof cond,&used,t[i]);
        if(condition_evaluate(cond)) jump_to(t[label]);
        return; }
    case OP_GOTO:
        if(n<2) return;
        if(eq(t[1],"EXIT")) { finish();return; }
        if(eq(t[1],"SCENE")&&n>2) { Link l=vm.link; game_enter_scene(number(t[2]),&l); return; }
        if(eq(t[1],"LINK")&&n>3) { game_enter_map(number(t[2]),number(t[3]),n>4?number(t[4]):0); return; }
        jump_to(t[1]);
        return;
    case OP_ACTOR: { /* id[.layer], "name", skin, pose, x, y [,colorTable][,pain][,mode] */
        Actor *a; char path[640];
        int id,x,y;
        if(n<5) return;
        id=number(t[1])&63;
        x=n>5?number(t[5]):50; y=n>6?number(t[6]):75;
        vm.selected=id; a=&vm.actors[id];
        sheet_free(&a->sheet); memset(a,0,sizeof *a);
        a->used=1; a->poses=1; a->pose[0]=number(t[4]);
        copy(a->name,sizeof a->name,t[2]);
        if(asset_path(path,sizeof path,"skins",t[3],".bmp")) sheet_load_skin(&a->sheet,t[3]);
        else sheet_load_monster(&a->sheet,t[3]);
        a->x=a->tx=x*256; a->y=a->ty=y*256;
        a->started=clock_ms();
        return; }
    case OP_POSE: { Actor *a=&vm.actors[vm.selected];
        a->poses=n-1; if(a->poses>3) a->poses=3;
        for(i=0;i<a->poses;i++) a->pose[i]=number(t[i+1]);
        a->frame=0; a->started=clock_ms(); return; }
    case OP_MOVE: { Actor *a; int mode=n>4?number(t[4]):0,x,y;
        if(n<4) return;
        a=eq(t[1],"H")?&vm.host:&vm.actors[number(t[1])&63];
        x=number(t[2]); y=number(t[3]);
        a->tx=x*256; a->ty=y*256;
        if(mode==1) { a->x=a->tx; a->y=a->ty; }   /* teleport */
        return; }
    case OP_SELECT: if(n>1) vm.selected=number(t[1])&63; return;
    case OP_WAIT: /* stores the tick and the length, then the PC still advances */
        vm.wait_start=clock_ms();
        vm.wait_length=(uint32_t)(n>1?strtod(t[1],NULL):0)*1000u;
        vm.state=ST_WAIT;
        return;
    case OP_FIGHT: fight_op(t,n,0); return;
    case OP_FIGHT2: fight_op(t,n,1); return;
    case OP_GIVE: give_all(t,n,0,0); return;
    case OP_TAKE: give_all(t,n,1,0); return;
    case OP_HOST_GIVE: give_all(t,n,0,1); return;
    case OP_HOST_TAKE: give_all(t,n,1,1); return;
    case OP_PARTY_GIVE: give_all(t,n,0,0); return;
    case OP_PARTY_TAKE: give_all(t,n,1,0); return;
    case OP_OFFER: offer(t,n,0); return;
    case OP_OFFER2: offer(t,n,1); return;
    case OP_ASK: /* sets state 9, records the timeout, clears the reply (FUN_00429C9C) */
        vm.state=ST_ASK;
        vm.wait_start=clock_ms();
        vm.wait_length=(uint32_t)(n>1?strtod(t[1],NULL):30.0)*1000u;
        vm.reply[0]=0; vm.ask[0]=0; vm.yes=0; vm.reveal=(int)strlen(vm.dialog);
        return;
    case OP_END: vm.state=ST_ENDED; finish(); return;
    case OP_GAME: /* does NOT block: FUN_00421529 arms the button, then pc++ */
        if(n>1) minigame_start(number(t[1]),vm.pc<vm.end?g_world.lines[vm.pc]:"");
        return;
    case OP_BKGND: if(n>1) background(t[1]); return;
    case OP_WEATHER: /* FUN_0048A658 stores it; 0 none, 1-3 rain, 7-9 snow */
        vm.weather=n>1?number(t[1]):0;
        wos_log_event("scene_effect","op=WEATHER value=%d",vm.weather);
        return;
    case OP_FX: /* FUN_0048A67B; 0 none, 1 underwater, 2 lake, 3 video, 4 jitter, 5 quake */
        vm.fx=n>1?number(t[1]):0;
        wos_log_event("scene_effect","op=FX value=%d",vm.fx);
        return;
    case OP_LOCK: vm.locked=n>1&&number(t[1]); return;
    case OP_TOKEN: /* legal inside +SCENES; the loader (FUN_00481654) counted these */
        return;
    case OP_COUNTDOWN: /* DAT_004FA854 = n*1000; 0 disarms (DAT_004FA850 = 0) */
        vm.countdown_length=(uint32_t)(n>1?number(t[1]):0)*1000u;
        vm.countdown_start=vm.countdown_length?clock_ms():0;
        wos_log_event("scene_countdown","ms=%u",vm.countdown_length);
        return;
    case OP_SET: if(n>2) cookie_set(t[1],t[2]); return;
    case OP_ADD: if(n>2) math_op(0,0,t[1],t[2]); return;
    case OP_SUB: if(n>2) math_op(1,0,t[1],t[2]); return;
    case OP_MUL: if(n>2) math_op(2,0,t[1],t[2]); return;
    case OP_DIV: if(n>2) math_op(3,0,t[1],t[2]); return;
    case OP_MOD: if(n>2) math_op(4,0,t[1],t[2]); return;
    case OP_F_ADD: if(n>2) math_op(0,1,t[1],t[2]); return;
    case OP_F_SUB: if(n>2) math_op(1,1,t[1],t[2]); return;
    case OP_F_MUL: if(n>2) math_op(2,1,t[1],t[2]); return;
    case OP_F_DIV: if(n>2) math_op(3,1,t[1],t[2]); return;
    case OP_F_MOD: if(n>2) math_op(4,1,t[1],t[2]); return;
    case OP_AND: if(n>2) logic_op(0,t[1],t[2]); return;
    case OP_OR: if(n>2) logic_op(1,t[1],t[2]); return;
    case OP_NOT: if(n>2) logic_op(2,t[1],t[2]); return;
    case OP_XOR: if(n>2) logic_op(3,t[1],t[2]); return;
    case OP_PARTY: /* offline: the host has no remote party members to change */
        wos_log_event("scene_offline","op=PARTY");
        return;
    case OP_EJECT:
        wos_log_event("scene_offline","op=EJECT");
        return;
    case OP_COMPARE:
        if(n>2) vm.condition_code=(int64_t)number(t[1])-number(t[2]);
        return;
    case OP_F_COMPARE: /* two doubles; the code is FUN_0047D1B9's range verdict */
        if(n>2) { double d=atof(t[2])-atof(t[1]);
                  vm.condition_code=(d<=-1.0e9||d>=1.0e9)?0:-1; }
        return;
    /* The IF= family is a single keyword + a label, so the label is token 1. */
    case OP_IF_EQ: if(vm.condition_code==0) jump_to(t[n-1]); return;
    case OP_IF_GT: if(vm.condition_code>=1) jump_to(t[n-1]); return;
    case OP_IF_LT: if(vm.condition_code<=-1) jump_to(t[n-1]); return;
    case OP_IF_LE: if(vm.condition_code<=0) jump_to(t[n-1]); return;
    case OP_IF_GE: if(vm.condition_code>=0) jump_to(t[n-1]); return;
    case OP_IF_NE: if(vm.condition_code!=0) jump_to(t[n-1]); return;
    case OP_IF_EVEN: if((vm.condition_code&1)==0) jump_to(t[n-1]); return;
    case OP_IF_ODD: if((vm.condition_code&1)!=0) jump_to(t[n-1]); return;
    case OP_COLOR: /* FUN_0048A7DA: `n = channel + 1000*mode`; channel = n%1000 & 0xFF and the
        * mode bits are (n/1000)&1 = recolour the players, (n/1000)&2 = the monsters. All three
        * are applied to EVERY slot and EVERY object, which is why the seals exist at all. */
        if(n>1) { int v=number(t[1]); vm.color_table=v%1000&0xff; scene_colour_apply(vm.color_table); }
        return;
    case OP_FACE: if(n>2) face(number(t[1]),number(t[2])); return;
    case OP_HTML: /* FUN_0048A69E; state 10 suspends until the viewer closes */
        if(n>1) { wos_log_event("scene_html","arg=%s",t[1]);
                  if(html_open(t[1])) vm.state=ST_HTML; }
        return;
    case OP_FLAGS: vm.flags=n>1?(uint32_t)strtoul(t[1],NULL,0):0; return;
    case OP_MENU: /* FUN_0048C5FE is the popup menu; the port opens its own panel */
        wos_log_event("scene_menu","args=%d",n-1);
        return;
    case OP_STRCMP: if(n>2) vm.condition_code=text_casecmp(t[1],t[2]); return;
    case OP_STRSTR: { char a[COOKIE_BUF],b[COOKIE_BUF];
        if(n>2) { copy(a,sizeof a,t[1]); copy(b,sizeof b,t[2]);
                  vm.condition_code=strstr(a,b)!=NULL; }
        return; }
    case OP_SET_LEN: if(n>1) { char v[64]; snprintf(v,sizeof v,"%u",(unsigned)strlen(t[1]));
                              cookie_set(t[1],v); } return;
    case OP_SET_SUBSTR: if(n>3) set_substr(t[1],number(t[2]),number(t[3])); return;
    case OP_GET_SERVER_VAR: /* SRNet only: the original writes 0 and logs */
        wos_log_event("scene_offline","op=GET_SERVER_VAR");
        return;
    case OP_SET_SERVER_VAR:
        wos_log_event("scene_offline","op=SET_SERVER_VAR");
        return;
    case OP_NTH_TOKEN: if(n>3) nth_token(t[1],number(t[2]),number(t[3]),",",t[4]); return;
    case OP_CALL: call_push(t,n); {
        int line=resolve_label(t[1],&scene_out);
        if(line<0) { wos_log_event("scene_error","op=CALL reason=label label=%s",t[1]); return; }
        if(vm.call_depth>CALL_DEPTH) { wos_log_event("scene_error","op=CALL reason=stack_full"); return; }
        vm.pc=line+1;
        if(scene_out>0) { vm.number=scene_out; vm.end=g_world.scenes[scene_out].end_line; }
        return; }
    case OP_RETURN: { int line=call_pop(&scene_out);
        if(line<0) { wos_log_event("scene_error","op=RETURN reason=stack_empty"); return; }
        vm.pc=line; vm.number=scene_out;
        vm.end=g_world.scenes[scene_out].end_line;
        return; }
    case OP_PUSH: if(n>1) push_cookie(t[1]); return;
    case OP_POP: if(n>1) pop_cookie(t[1]); return;
    case OP_TIMER: if(n>2) timer_set(number(t[1]),(uint32_t)(strtod(t[2],NULL)*1000.0));
                   else if(n>1) timer_set(number(t[1]),0);
                   return;
    case OP_MISSION: { /* "MISSIONS 1,2,3" -> "%04X" per arg, then arm the button */
        int jobs[ARGS],count=0;
        for(i=1;i<n;i++) jobs[count++]=number(t[i]);
        missions_offer(jobs,count);
        wos_log_event("scene_mission","count=%d",count);
        return; }
    case OP_SHUFFLE: if(n>2) shuffle(t[1],number(t[2])); return;
    case OP_STRLWR: if(n>2) { char v[COOKIE_BUF]; size_t k;
        copy(v,sizeof v,t[2]);
        for(k=0;v[k];k++) v[k]=(char)tolower((unsigned char)v[k]);
        cookie_set(t[1],v); } return;
    case OP_UNKNOWN:
        wos_log_event("scene_unknown","op=%s in_line=%d scene=%d",t[0],vm.pc,vm.number);
        return;
    default: return;
    }
}

uint32_t scene_flags(void) { return vm.flags; }
int game_current_scene(void) { return screen_current()==&scene_screen ? vm.number : -1; }
void scene_tick(void)
{
    /* No-op: the original has no simulation tick (docs/re/timing.md). Kept only so the
     * tree links while Core removes the call from game_main.c. */
}
void scene_reset_timers(void)
{
    cookie_key_count=0;
    memset(vm.timers,0,sizeof vm.timers);
    vm.countdown_start=vm.countdown_length=0;
    vm.call_depth=0;
    vm.push_depth=0;
}

/* --- input geometry (FUN_004787B2: ten 48x48 buttons along the top right) --- */
static int hit(const Input *in,Rect r)
{
    return (in->mouse_pressed&(1u<<1)) && in->mouse_x>=r.x && in->mouse_y>=r.y
        && in->mouse_x<r.x+r.w && in->mouse_y<r.y+r.h;
}
/* Button slot i: x from (W-48-3-51*i) to (W-3-51*i), y 8..56 (FUN_004787B2 at 640x480). */
static Rect button_rect(int i) { Rect r={640-51-51*i,8,48,48}; return r; }
/* Actor hit box: the logical 360x256 scene mapped over the whole client. */
static Rect actor_rect(const Actor *a)
{
    int w=a->sheet.cell?a->sheet.cell*SCENE_W/360:32;
    int h=a->sheet.cell?a->sheet.cell*SCENE_H/256:32;
    int x=(int)((int64_t)a->x*640/25600), y=(int)((int64_t)a->y*480/25600);
    Rect r={x-w/2,y-h,w,h};
    return r;
}
/* Re-enter the script at a reserved event label (docs/re/script.md 2.6). The original
 * broadcasts FUN_004306F6(0x41, <event>, <slot>, ...) and the VM jumps to the label. */
static int scene_event(const char *kind,int slot,int code)
{
    char label[64];
    int line,scene=0;
    if(slot<0||slot>=ACTORS) return 0;
    if(!eq(kind,"click") && !eq(kind,"attack") && !eq(kind,"spell")) return 0;
    snprintf(label,sizeof label,"@eventActor%s%d",
             eq(kind,"click")?"Click":eq(kind,"attack")?"Attack":"Spell",slot);
    line=resolve_label(label,&scene);
    if(line<0) return 0;
    wos_log_event("scene_event","kind=%s slot=%d code=%d",kind,slot,code);
    vm.number=scene; vm.pc=line+1; vm.end=g_world.scenes[scene].end_line;
    vm.state=ST_RUN;
    return 1;
}
static void actors_update(uint32_t now)
{
    int i;
    for(i=0;i<=ACTORS;i++) {
        Actor *a=i==ACTORS?&vm.host:&vm.actors[i];
        if(!a->used) continue;
        if(a->tx!=a->x || a->ty!=a->y) {
            /* MOVE walks 2x..5x the base speed; the base is one 1/256 percent step per ms. */
            int speed=256;
            int d;
            d=a->tx-a->x; if(d>speed)d=speed; if(d< -speed)d=-speed; a->x+=d;
            d=a->ty-a->y; if(d>speed)d=speed; if(d< -speed)d=-speed; a->y+=d;
        } else if(a->poses>1 && now-a->started>=100u) {
            /* FUN_0046C3E0-paced random dwell, cycling pose[0] <-> pose[1] */
            a->started=now;
            a->frame=(a->frame+1)%a->poses;
        }
    }
}
static void scene_update(const Input *in)
{
    uint32_t now=clock_ms();
    int advance=in->pressed[PLAT_KEY_RETURN]||in->pressed[PLAT_KEY_SPACE]
        ||((in->mouse_pressed&((1u<<1)|(1u<<3)))&&in->mouse_x<400);
    if(minigame_active()) { minigame_update(now); minigame_click(in->mouse_x,in->mouse_y,
        /* bit 1 is the port's LEFT button (PLAT_EV_MOUSE_DOWN button=1, ui.c's 1u<<ev->button).
         * Bit 0 is the Win32 MK_LBUTTON mask and this input layer never produces it, so `&1u`
         * made every mini-game dialog inert while still opening. Same mask as hit() above. */
        (in->mouse_pressed&(1u<<1))?1:0); return; }
    if(html_active()) { html_update(in); return; }
    if(missions_panel_active()) { missions_panel_update(in); return; }
    if(panel_active()) { panel_update(in); return; }
    actors_update(now);
    if(vm.theme_period>0 && now-(uint32_t)vm.theme_left>= (uint32_t)vm.theme_period) {
        vm.theme_left=(int)now;
        plat_sound_play(vm.theme,vm.theme_size);
    }
    /* The button bar (FUN_00478673 registers up to ten slots; slot 6 is GAME/MISSIONS). */
    if(hit(in,button_rect(0))) { panel_open(PANEL_STATS); return; }
    if(hit(in,button_rect(6))) {
        /* Slot 6 is shared: whichever of GAME / MISSIONS / SHOP armed it last owns the
         * click (FUN_00478E04 and FUN_00478D53 both register slot 6). */
        if(missions_offered()) { missions_open_picker(); return; }
        if(minigame_armed()&&!minigame_active()) { minigame_click(in->mouse_x,in->mouse_y,1); return; }
        panel_open(PANEL_EQUIP);
        return;
    }
    if(hit(in,button_rect(4))) { panel_open(PANEL_ITEMS); return; }
    if(hit(in,button_rect(5))) {
        if(vm.state==ST_FIGHT||vm.state==ST_FIGHT_MAP) battle_open_spells();
        else panel_open(PANEL_SPELLS);
        return;
    }
    if(vm.state==ST_FIGHT||vm.state==ST_FIGHT_MAP) {
        BattleResult result;
        if(hit(in,button_rect(3))) { Input flee=*in; flee.pressed[PLAT_KEY_ESCAPE]=1; result=battle_update(&flee); }
        else result=battle_update(in);
        if(!battle_active()&&result!=BATTLE_RUNNING) { vm.outcome=result; vm.state=ST_RUN; }
        return;
    }
    if(hit(in,button_rect(1))||hit(in,button_rect(3))||in->pressed[PLAT_KEY_ESCAPE]) { finish();return; }
    if(hit(in,button_rect(2))) { hero_save(&g_hero); game_go_well(); return; }
    /* Actor clicks re-enter the script at the reserved labels. */
    if(in->mouse_pressed&(1u<<1)) {
        int i;
        for(i=0;i<ACTORS;i++) {
            if(!vm.actors[i].used) continue;
            if(in->mouse_x>=actor_rect(&vm.actors[i]).x&&in->mouse_x<actor_rect(&vm.actors[i]).x
                 +actor_rect(&vm.actors[i]).w
               &&in->mouse_y>=actor_rect(&vm.actors[i]).y&&in->mouse_y<actor_rect(&vm.actors[i]).y
                 +actor_rect(&vm.actors[i]).h) {
                if(scene_event("click",i,0)) return;
                if(battle_click_actor(i)) return;
                break;
            }
        }
    }
    switch(vm.state) {
    case ST_WAIT: /* FUN_00490E7C case 2: `length < now - start` */
        if(vm.wait_length<now-vm.wait_start) vm.state=ST_RUN;
        return;
    case ST_ASK: { /* FUN_00490E7C case 9: the reply clears the bubble immediately */
        size_t len=strlen(vm.reply),i2;
        for(i2=0;in->text[i2]&&len+1<sizeof vm.reply;i2++)
            if((unsigned char)in->text[i2]>=32) vm.reply[len++]=in->text[i2];
        vm.reply[len]=0;
        if(in->pressed[PLAT_KEY_BACKSPACE]&&len) vm.reply[--len]=0;
        if(hit(in,(Rect){60,350,100,32})||in->pressed['y']) { copy(vm.reply,sizeof vm.reply,"YES"); vm.yes=1; copy(vm.ask,sizeof vm.ask,vm.reply); vm.state=ST_RUN; }
        else if(hit(in,(Rect){190,350,100,32})||in->pressed['n']) { copy(vm.reply,sizeof vm.reply,"NO"); vm.yes=0; copy(vm.ask,sizeof vm.ask,vm.reply); vm.state=ST_RUN; }
        else if(in->pressed[PLAT_KEY_RETURN]||vm.wait_length<now-vm.wait_start) {
            vm.yes=eq(vm.reply,"YES")||eq(vm.reply,"Y");
            copy(vm.ask,sizeof vm.ask,vm.reply);
            vm.state=ST_RUN;
        }
        return; }
    case ST_HTML:
        if(!html_active()) vm.state=ST_RUN;
        return;
    case ST_FIGHT_MAP: case ST_FIGHT: return;
    default: break;
    }
    /* FUN_0046C44A: the interpreter blocks while `now - start < FUN_0046C3E0(text)`, and a
     * click clears the bubble early. FUN_0049331E reveals (now-start)*30/1000 characters. */
    if(vm.dialog[0]) {
        int len=(int)strlen(vm.dialog);
        uint32_t shown=(now-vm.bubble_start)*30u/1000u;
        if(advance||now-vm.bubble_start>=vm.bubble_ms) {
            vm.dialog[0]=0; vm.speaker[0]=0; vm.state=ST_RUN;
            return;
        }
        vm.reveal = (int)(shown>(uint32_t)len?(uint32_t)len:shown);
        return;
    }
    step();
}

/* --- rendering ------------------------------------------------------------ */
static void scaled(Framebuffer *fb,const Image *image,Rect src,Rect dst,int64_t key)
{
    int x,y;
    if(!image->pixels||src.w<=0||src.h<=0||dst.w<=0||dst.h<=0)return;
    for(y=0;y<dst.h;y++) {
        int sy=src.y+(int)((int64_t)y*src.h/dst.h),dy=dst.y+y;
        if(dy<fb->clip.y||dy>=fb->clip.y+fb->clip.h||sy<0||sy>=image->h)continue;
        for(x=0;x<dst.w;x++) {
            int sx=src.x+(int)((int64_t)x*src.w/dst.w),dx=dst.x+x;uint32_t c;
            if(dx<fb->clip.x||dx>=fb->clip.x+fb->clip.w||sx<0||sx>=image->w)continue;
            c=image->pixels[(size_t)sy*image->w+sx];
            if(key<0||c!=(uint32_t)key)fb->pixels[(size_t)dy*fb->w+dx]=c;
        }
    }
}
static void draw_actor(Framebuffer *fb,const Sheet *sheet,int pose,int x,int y)
{
    int w=sheet->cell*SCENE_W/360,h=sheet->cell*SCENE_H/256;
    if(pose<0||pose>=sheet->count)return;
    scaled(fb,&sheet->image,(Rect){pose*sheet->cell,0,sheet->cell,sheet->cell},(Rect){x-w/2,y-h,w,h},sheet->key);
}
static void draw_button(Framebuffer *fb,Rect r,const char *label,int enabled)
{
    fb_fill(fb,r,enabled?0x34485c:0x222832);fb_rect(fb,r,0x8090a0);
    font_draw(fb,r.x+(r.w-(int)font_width(label))/2,r.y+20,label,enabled?0xffffff:0x778088);
}
static void scene_render(Framebuffer *fb)
{
    int i;char buf[DIALOG];
    fb_clear(fb,0x151d28);
    /* FUN_00487D76/0x49699E: the background is stretched over the whole client. */
    scaled(fb,&vm.background,(Rect){0,0,vm.background.w,vm.background.h},(Rect){0,0,640,480},-1);
    if(vm.state==ST_FIGHT||vm.state==ST_FIGHT_MAP) battle_render(fb,(Rect){0,0,640,480});
    else {
        if(!vm.hide_hero&&g_hero.valid) draw_actor(fb,&vm.hero,1,
            (int)((int64_t)vm.host.x*640/25600),(int)((int64_t)vm.host.y*480/25600));
        for(i=0;i<ACTORS;i++) if(vm.actors[i].used) {
            Actor *a=&vm.actors[i];
            draw_actor(fb,&a->sheet,a->pose[a->frame],
                (int)((int64_t)a->x*640/25600),(int)((int64_t)a->y*480/25600));
        }
        if(vm.dialog[0]) {
            size_t len=strlen(vm.dialog),shown=vm.reveal<0?0:(size_t)vm.reveal;
            if(shown>len) shown=len;
            memcpy(buf,vm.dialog,shown);buf[shown]=0;
            /* FUN_0049331E: the bubble box is at most a third of the client wide and a
             * quarter tall, clamped to 200x120, and sits 110 px above the speaker. */
            fb_fill(fb,(Rect){0,240,640,470},0xeee5cd);fb_rect(fb,(Rect){0,240,640,470},0x614b30);
            font_draw(fb,8,250,vm.speaker,0x653616);
            font_wrap(fb,(Rect){8,268,624,196},buf,0x241a11);
            if(vm.state==ST_ASK) {
                draw_button(fb,(Rect){60,350,100,32},"YES [Y]",1);
                draw_button(fb,(Rect){190,350,100,32},"NO [N]",1);
                font_draw(fb,16,330,vm.reply,0x241a11);
            }
        }
    }
    draw_button(fb,button_rect(0),"Stats",1);
    draw_button(fb,button_rect(1),"Map",vm.state!=ST_FIGHT&&vm.state!=ST_FIGHT_MAP);
    draw_button(fb,button_rect(2),"Well",vm.state!=ST_FIGHT&&vm.state!=ST_FIGHT_MAP);
    draw_button(fb,button_rect(3),vm.state==ST_FIGHT?"Flee":"Exit",1);
    draw_button(fb,button_rect(4),"Items",1);
    draw_button(fb,button_rect(5),"Spell",1);
    /* Slot 6 is shared: GAME (FUN_00478E04), MISSIONS (FUN_00478D53) and SHOP all register
     * it and the last one registered is the one shown and clicked, so the label follows the
     * armed state rather than living on a slot of its own. */
    if(missions_offered()) draw_button(fb,button_rect(6),"Missions",1);
    else if(minigame_armed()) draw_button(fb,button_rect(6),"Game",1);
    else draw_button(fb,button_rect(6),"Equip",1);
    /* The status strip (FUN_004589E9 at (8, H-40, W, H)). */
    fb_fill(fb,(Rect){0,440,640,40},0x202c38);
    fb_fill(fb,(Rect){8,444,176,12},0x501010);
    fb_fill(fb,(Rect){8,444,g_hero.max_hp>0?(int)((int64_t)176*g_hero.hp/g_hero.max_hp):0,12},0xb02e2e);
    fb_fill(fb,(Rect){194,444,176,12},0x101050);
    fb_fill(fb,(Rect){194,444,g_hero.max_mp>0?(int)((int64_t)176*g_hero.mp/g_hero.max_mp):0,12},0x3059b0);
    snprintf(buf,sizeof buf,"HP %d/%d",g_hero.hp,g_hero.max_hp); font_draw(fb,14,446,buf,0xffffff);
    snprintf(buf,sizeof buf,"MP %d/%d",g_hero.mp,g_hero.max_mp); font_draw(fb,200,446,buf,0xffffff);
    if(vm.title[0]) font_wrap(fb,(Rect){380,444,256,24},vm.title,0xc8d9e8);
    if(vm.countdown_start) {
        /* FUN_0047C6DA: (length - (now - start)) / 1000, floored at 0. */
        uint32_t now=clock_ms(),left=0;
        if(now-vm.countdown_start<vm.countdown_length)
            left=(vm.countdown_length-(now-vm.countdown_start))/1000;
        snprintf(buf,sizeof buf,"Countdown: %u",left);
        font_draw(fb,380,462,buf,0xffe4a0);
    }
    if(minigame_active()) minigame_render(fb,0,0);
    if(html_active()) html_render(fb);
    if(missions_panel_active()) missions_panel_render(fb);
    if(panel_active()) panel_render(fb);
}
static void scene_leave(void)
{
    int i;
    panel_close();
    missions_panel_close();
    minigame_disarm();
    html_close();
    image_free(&vm.background);sheet_free(&vm.hero);
    for(i=0;i<ACTORS;i++) sheet_free(&vm.actors[i].sheet);
    free(vm.theme);vm.theme=NULL;vm.theme_size=0;
}
static const Screen scene_screen={"scene",NULL,scene_update,scene_render,scene_leave};

/* --- state dump (scene.*) ------------------------------------------------- */
void scene_dump(DumpEmit emit,void *user)
{
    char key[192],value[COOKIE_BUF];
    int i;
    dump_emit_int(emit,"scene.number",vm.number,user);
    dump_emit_int(emit,"scene.pc",vm.pc,user);
    dump_emit_int(emit,"scene.selected",vm.selected,user);
    dump_emit_int(emit,"scene.state",(int)vm.state,user);
    dump_emit_int(emit,"scene.flags",(long long)vm.flags,user);
    dump_emit_int(emit,"scene.weather",vm.weather,user);
    dump_emit_int(emit,"scene.fx",vm.fx,user);
    dump_emit_int(emit,"scene.colorTable",vm.color_table,user);
    dump_emit_int(emit,"scene.locked",vm.locked,user);
    dump_emit_int(emit,"scene.hideHero",vm.hide_hero,user);
    dump_emit_int(emit,"scene.conditionCode",(long long)vm.condition_code,user);
    dump_emit_int(emit,"scene.callDepth",vm.call_depth,user);
    dump_emit_int(emit,"scene.pushDepth",vm.push_depth,user);
    dump_emit_int(emit,"scene.yes",vm.yes,user);
    dump_emit_int(emit,"scene.outcome",(int)vm.outcome,user);
    dump_emit_int(emit,"scene.countdownMs",(long long)vm.countdown_length,user);
    dump_emit_int(emit,"scene.countdownLeftMs",
        (long long)(vm.countdown_length?(uint32_t)(vm.countdown_length-(clock_ms()-vm.countdown_start)):0),user);
    emit("scene.ask",vm.ask,user);
    emit("scene.dialog",vm.dialog,user);
    for(i=0;i<ACTORS;i++) if(vm.actors[i].used) {
        snprintf(key,sizeof key,"scene.actor.%d.name",i); emit(key,vm.actors[i].name,user);
        snprintf(key,sizeof key,"scene.actor.%d.x",i);
        snprintf(value,sizeof value,"%d",(int)((int64_t)vm.actors[i].x*640/25600)); emit(key,value,user);
        snprintf(key,sizeof key,"scene.actor.%d.y",i);
        snprintf(value,sizeof value,"%d",(int)((int64_t)vm.actors[i].y*480/25600)); emit(key,value,user);
        snprintf(key,sizeof key,"scene.actor.%d.pose",i);
        snprintf(value,sizeof value,"%d",vm.actors[i].pose[vm.actors[i].frame]); emit(key,value,user);
    }
    for(i=0;i<SCENE_TIMERS;i++) {
        snprintf(key,sizeof key,"scene.timer.%d.length",i);
        snprintf(value,sizeof value,"%u",vm.timers[i].length); emit(key,value,user);
        snprintf(key,sizeof key,"scene.timer.%d.left",i);
        snprintf(value,sizeof value,"%u",timer_left(i)); emit(key,value,user);
    }
    /* Cookies: the [cookies] INI belongs to hero.c and has no enumerator, so re-read every
     * key the VM has written this session (FUN_0047AB07 is the only offline writer). */
    for(i=0;i<cookie_key_count;i++) {
        snprintf(key,sizeof key,"scene.cookie.%s",cookie_keys[i]);
        copy(value,sizeof value,cookie_raw(cookie_keys[i]));
        emit(key,value,user);
    }
    /* Push stack (FUN_004792D1's DAT_007F14B0). */
    for(i=0;i<vm.push_depth;i++) {
        snprintf(key,sizeof key,"scene.push.%d",i);
        emit(key,vm.pushes[i],user);
    }
}

/* --- the scene colour table (CRT index 38) ---------------------------------
 * The original's CRT thunk at 0x43BB8A (`jmp 0x43BB8F`, entry 38) walks
 * 4 slots of 0x1798 at base 0x5BB760, and 9 objects of 0x298 per slot starting at
 * slot+0x8, and FUN_0043BBD1 (0x43BBD1) seals SEVEN EncInts in each object at
 * +0x20/+0x58/+0x90/+0xC8/+0x100/+0x138/+0x170, ascending. That is 4x9x7 = 252
 * seals = 1008 draws. The loop seeds are 3 and 8 with dec/jns, so each runs one
 * MORE time than the seed: 4 slots and 9 objects, not 3 and 8.
 * Only those seven words are sealed - the rest of the 0x298 object is plain. */
#define SCOLOUR_SLOTS        4
#define SCOLOUR_SLOT_STRIDE  0x1798
#define SCOLOUR_OBJ_STRIDE   0x298
#define SCOLOUR_OBJS         9
#define SCOLOUR_CHANNELS     7
static unsigned char colour_table[SCOLOUR_SLOTS*SCOLOUR_SLOT_STRIDE];
static const int colour_channel_off[SCOLOUR_CHANNELS] = {
    0x20,0x58,0x90,0xC8,0x100,0x138,0x170
};
#define SCOLOUR_OBJS_NAMED 10  /* boot seals 0..8, FUN_0043BE95 names 1..9; the slot holds both */
static EncInt *colour_slot_obj(int slot,int obj)
{
    if(slot<0||slot>=SCOLOUR_SLOTS||obj<0||obj>=SCOLOUR_OBJS_NAMED) return NULL;
    return (EncInt *)(void *)(colour_table + (size_t)slot*SCOLOUR_SLOT_STRIDE
                              + 8 + (size_t)obj*SCOLOUR_OBJ_STRIDE);
}
/* The object's plain header words, from FUN_0043BD0A's record fill. The palette
 * prints five of them: the words at dword 0, 2, 4 and 5, and the last channel. */
typedef struct {
    int w0, w1, w2, index, w4, w5, bitflag;
    unsigned char pad[8];              /* to +0x20, where the first EncInt starts */
    EncInt ch[SCOLOUR_CHANNELS];       /* +0x20 .. +0x170, seven of them */
    int tail0, tail1, tail2;            /* +0x1A8: 0, 1, 0 */
} ColourObj;
/* FUN_0043BC68 formats "%02X" five times: w0, w2, w4, w5 and channel 6. */
static char colour_palette[SCOLOUR_OBJS*10+1];
static void colour_palette_build(void)
{
    int o;
    size_t n=0;
    colour_palette[0]=0;
    for(o=0;o<SCOLOUR_OBJS;o++) {
        const ColourObj *ob=(const ColourObj *)(const void *)colour_slot_obj(0,o);
        if(!ob) break;
        n+=(size_t)snprintf(colour_palette+n,sizeof colour_palette-n,"%02X%02X%02X%02X%02X",
                            ob->w0&0xff,ob->w2&0xff,ob->w4&0xff,ob->w5&0xff,
                            (int)(enc_raw(&ob->ch[6])&0xff));
    }
}
/* FUN_0048A7DA's first branch: seal `channel` into every object of every slot. */
void scene_colour_apply(int channel)
{
    int s,o;
    channel&=0xff;
    for(s=0;s<SCOLOUR_SLOTS;s++)
        for(o=0;o<SCOLOUR_OBJS;o++) {
            const ColourObj *ob=(const ColourObj *)(const void *)colour_slot_obj(s,o);
            if(ob) enc_set((EncInt *)&ob->ch[channel%SCOLOUR_CHANNELS],channel);
        }
}
/* FUN_004436A5: clamp the delta to 0..2 and reseal channel 0. One enc_get (free)
 * and one enc_set (4 draws) per object, in ascending address order. */
int scene_colour_jitter(int slot,int delta)
{
    int o,v=delta+((delta>=0&&delta<3)?0:0);
    (void)v;
    for(o=0;o<SCOLOUR_OBJS;o++) {
        const ColourObj *ob=(const ColourObj *)(const void *)colour_slot_obj(slot,o);
        int n;
        if(!ob||!ob->w0) continue;       /* the original skips a record whose w0 is 0 */
        n=delta+enc_raw(&ob->ch[0]);
        if(n<0) n=0;
        if(2<n) n=2;
        enc_set((EncInt *)&ob->ch[0],n);
    }
    return 0;
}
static void construct_table_b(void *user)
{
    int s,o,i;
    (void)user;
    for(s=0;s<SCOLOUR_SLOTS;s++)
        for(o=0;o<SCOLOUR_OBJS;o++) {
            unsigned char *obj=colour_table + (size_t)s*SCOLOUR_SLOT_STRIDE
                             + 8 + (size_t)o*SCOLOUR_OBJ_STRIDE;
            for(i=0;i<SCOLOUR_CHANNELS;i++)
                enc_clear((EncInt *)(void *)(obj+colour_channel_off[i]));
        }
    colour_palette_build();
}
/* FUN_0044462C, the level-up colour flash: three seals into ONE colour record, in
 * ascending address order, at object offsets +0x58, +0x90 and +0x138. The 0x38
 * spacing is the EncInt stride and the record carries EncInts at +0x58, +0x90,
 * +0xC8, +0x100 and +0x138, so this seals the 1st, 2nd and 5th of the five.
 * Each enc_set costs 4 crt_rand, so the sequence is 12 draws.
 *   VA 0x0044469A / 0x004446E4 / 0x004446F1.
 *
 * `monster_9c` is the PLAIN dword the original reads at monster-table+0x9C
 * (0x004446D3, `add 0x9c(%edi),%eax`) - note it is not one of the two enc_get
 * targets, which are plain ints feeding the arithmetic. The monster table is
 * Battle3's, so the caller supplies that word rather than my file reaching into
 * a table it does not own. Pass a negative value when there is no monster record:
 * the original guards the whole block on DAT_004EC428 and the record pointer, and
 * then seals nothing.
 *
 * Seal 3 is the literal 2, a plain re-seal that still costs its 4 draws. */
void scene_colour_level_flash(int slot,int obj,int monster_9c)
{
    ColourObj *ob=(ColourObj *)(void *)colour_slot_obj(slot,obj);
    int a;
    if(!ob||monster_9c<0) return;
    a=enc_raw(&ob->ch[1])*2-2+monster_9c;
    enc_set(&ob->ch[1],a);        /* +0x58 */
    a=enc_raw(&ob->ch[2])*2-2+monster_9c;
    enc_set(&ob->ch[2],a);        /* +0x90 */
    enc_set(&ob->ch[5],2);        /* +0x138, the literal 2 */
}
/* FUN_0042620D: two hex characters. */
static int hex2(const char *p)
{
    int v=0,i;
    for(i=0;i<2;i++) {
        int c=(unsigned char)p[i],d;
        if(c>='0'&&c<='9') d=c-'0';
        else if(c>='A'&&c<='F') d=c-'A'+10;
        else if(c>='a'&&c<='f') d=c-'a'+10;
        else return v;                       /* the original's parser just stops */
        v=v*16+d;
    }
    return v;
}
/* FUN_0043BD0A: parse one 23-character (0x17) hex record into object `index` of `slot`.
 * Seven enc_set calls, so 28 draws per record. Returns 0x17 on success, 0 if the
 * record is shorter than 23 characters, which is the original's only validation. */
static int colour_record_parse(const char *rec,int slot,int index)
{
    ColourObj *ob=(ColourObj *)(void *)colour_slot_obj(slot,index);
    if(!rec || strlen(rec)<0x17) return 0;
    memset(ob,0,0x298);
    ob->w0 = hex2(rec);
    ob->w1 = 0;                       /* *(base+4): a global the caller sets, not part of the name */
    ob->w2 = hex2(rec+2);
    ob->index = index;
    ob->bitflag = 1 << (index & 0x1f);
    ob->w4 = hex2(rec+4);
    ob->w5 = hex2(rec+6);
    /* the seven channels, in ascending address order, 4 draws each */
    enc_set(&ob->ch[0],hex2(rec+0x08));
    enc_set(&ob->ch[1],hex2(rec+0x0A));
    enc_set(&ob->ch[2],hex2(rec+0x0C));
    enc_set(&ob->ch[3],hex2(rec+0x0E));
    enc_set(&ob->ch[4],hex2(rec+0x10));
    enc_set(&ob->ch[5],hex2(rec+0x12));
    enc_set(&ob->ch[6],hex2(rec+0x14));
    ob->tail0 = 0; ob->tail1 = 1; ob->tail2 = 0;   /* _Dst[0x6a/0x6b/0x6c] */
    return 0x17;
}
/* FUN_0043BE95, called from FUN_00446F77 at VA 0x00447236 with (records, slot_base).
 * Nine records of 23 characters, indices 1..9, advancing 0x17 each: 9*7 = 63 seals. */
void scene_colour_populate(int slot,const char *records)
{
    int i;
    if(!records) return;
    for(i=1;i<10;i++) colour_record_parse(records+(size_t)(i-1)*0x17,slot,i);
    colour_palette_build();
}
/* Core calls this next to boot_register_core(), before boot_run(). */
void scene_boot_register(void)
{
    boot_register(BOOT_CRT_TABLE_B,"tableB_4x9x7",construct_table_b,NULL);
}

/* --- entry ---------------------------------------------------------------- */
void game_enter_scene(int scene_no,const Link *link)
{
    Link saved;
    memset(&saved,0,sizeof saved);
    if(link) saved=*link;
    screen_set(NULL);
    /* FUN_0047A2A7 keeps the per-hero scene slots and only re-seeds the program counter, so
     * the cookie registry, the call stack and the timers survive a scene change. Only the
     * presentation state is rebuilt. */
    {
        int keep_call=vm.call_depth, keep_push=vm.push_depth, keep_keys=cookie_key_count;
        char saved_keys[COOKIE_KEYS][128];
        memcpy(saved_keys,cookie_keys,sizeof saved_keys);
        memset(&vm,0,sizeof vm);
        vm.call_depth=keep_call; vm.push_depth=keep_push; cookie_key_count=keep_keys;
        memcpy(cookie_keys,saved_keys,sizeof saved_keys);
    }
    /* missions.ini's `Qualify` is the same condition language (FUN_004859A2), and its
     * Reward/Accept/Abandon Give/Take use the same GIVE/TAKE object handler (FUN_00484E72). */
    missions_set_condition_evaluator(mission_condition,NULL);
    missions_set_give_handler(mission_give,NULL);
    /* The original broadcasts FUN_004306F6(0x41, <event>, <slot>, <code>, ...) from the
     * fight; battle.c calls back so the VM can re-enter @eventActorClick/Attack/Spell<n>. */
    battle_set_scene_event(scene_event);
    vm.number=scene_no; vm.link=saved; vm.state=ST_RUN;
    vm.outcome=BATTLE_NONE;
    vm.host.used=1; vm.host.x=vm.host.tx=20*256; vm.host.y=vm.host.ty=87*256;
    vm.flags=g_hero.map>=0&&g_hero.map<WORLD_MAX_MAPS?g_world.maps[g_hero.map].flags:0;
    if(scene_no<0||scene_no>=WORLD_MAX_SCENES||!g_world.scenes[scene_no].used) {
        wos_log_event("scene_error","scene=%d reason=missing",scene_no); finish(); return;
    }
    /* FUN_0047A2A7: the SCENE line is NOT executed. It seeds the program counter to
     * line+1 and reads its own arguments: id, background, style, name, fx, weather,
     * [colorTable]. Missing arguments inherit from the link the hero dropped in through,
     * and a `CUT` style hides the heroes. `SCENE` is not in the keyword table, so running
     * it through the interpreter would log "*** Unknown keyword". */
    {
        char st[ARGS][256];
        int sn=world_tokenize(g_world.lines[g_world.scenes[scene_no].first_line],st,ARGS);
        if(sn>2) background(st[2]);
        else background(vm.link.background[0]?vm.link.background:"fight");
        if(sn>3&&eq(st[3],"CUT")) vm.hide_hero=1;
        copy(vm.title,sizeof vm.title,sn>4?st[4]:(vm.link.name[0]?vm.link.name:"Lake Louise"));
        vm.fx = sn>5?number(st[5]):vm.link.fx;
        vm.weather = sn>6?number(st[6]):vm.link.weather;
        if(sn>7) vm.color_table=number(st[7]);
    }
    vm.pc=g_world.scenes[scene_no].first_line+1;
    vm.end=g_world.scenes[scene_no].end_line;
    if(g_hero.valid) sheet_load_skin(&vm.hero,g_hero.skin);
    screen_set(&scene_screen);
    wos_log_event("scene_enter","scene=%d",scene_no);
}
