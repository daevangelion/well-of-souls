/* Offline scene VM. Opcode/condition semantics: docs/re/script.md sections 2-4.
 * Time is measured in 60 Hz updates, never host-clock ticks. */
#include "scene.h"
#include "game.h"
#include "hero.h"
#include "battle.h"
#include "../game_main.h"
#include "../engine/screen.h"
#include "../engine/font.h"
#include "../engine/text.h"
#include "../engine/log.h"
#include "../engine/rng.h"
#include "../platform/platform.h"
#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ACTORS 64
#define ARGS 128
#define COOKIES 256
#define DIALOG 2048
#define UNKNOWN 128

typedef struct {
    int used, pose[3], poses, age, frame, speed;
    int x, y, tx, ty; /* percentage * 256 */
    char name[64];
    Sheet sheet;
} Actor;
typedef struct { char key[96], value[256]; } Cookie;
typedef enum { RUN, WAITING, SPEAKING, ASKING, FIGHTING, OFFERING } Suspend;
static struct {
    int number, pc, end, selected, wait, yes, locked, hide_hero;
    int64_t compare;
    int dialog_age, reveal, stats, theme_frames, theme_period;
    uint32_t flags;
    BattleResult outcome;
    Suspend suspended;
    Link link;
    Image background;
    Sheet hero;
    Actor actors[ACTORS], host;
    char title[128], dialog[DIALOG], speaker[64], reply[128];
    void *theme;
    size_t theme_size;
} vm;
static Cookie cookies[COOKIES];
static int cookie_count;
static char cookie_owner[640];
static char unknown[UNKNOWN][64];
static int unknown_count;
static const Screen scene_screen;

static int eq(const char *a, const char *b) { return text_casecmp(a,b)==0; }
static void copy(char *out, size_t cap, const char *in)
{
    size_t n = strlen(in);
    if (!cap) return;
    if (n >= cap) n=cap-1;
    memmove(out,in,n); out[n]=0;
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
static void unknown_op(const char *op)
{
    int i;
    for(i=0;i<unknown_count;i++) if(eq(op,unknown[i])) return;
    if(unknown_count<UNKNOWN) copy(unknown[unknown_count++],64,op);
    else return;
    wos_log_event("scene_unknown","op=%s",op);
}
static int inventory_count(int id)
{
    int i;
    for(i=0;i<HERO_INVENTORY;i++) if(g_hero.inventory[i].item_id==id) return g_hero.inventory[i].count;
    return 0;
}
static void cookie_path(char *out, size_t cap)
{
    size_t i;
    int n;
    out[0]=0;
    if(!*g_hero.name||!*g_world.name||strpbrk(g_world.name,"/\\")) return;
    for(i=0;g_hero.name[i];i++)
        if((unsigned char)g_hero.name[i]<32||strchr("/\\:",g_hero.name[i])) return;
    n=snprintf(out,cap,"%s/%s/savedHeroes/%s.cookies",game_save_path(),g_world.name,g_hero.name);
    if(n<0||(size_t)n>=cap) out[0]=0;
}
static void cookies_load(void)
{
    char path[640];
    FILE *f;
    int i;
    cookie_path(path,sizeof path);
    if(eq(path,cookie_owner)) return;
    cookie_count=0; memset(cookies,0,sizeof cookies);
    copy(cookie_owner,sizeof cookie_owner,path);
    f=plat_fopen(path,"rb");
    if(!f) return;
    for(i=0;i<COOKIES && fread(&cookies[i],sizeof cookies[i],1,f)==1;i++) {
        cookies[i].key[sizeof cookies[i].key-1]=0;
        cookies[i].value[sizeof cookies[i].value-1]=0;
        cookie_count++;
    }
    fclose(f);
}
static void cookies_save(void)
{
    char path[640];
    FILE *f;
    if(!g_hero.valid||!*cookie_owner) return;
    plat_mkdir(game_save_path());
    snprintf(path,sizeof path,"%s/%s",game_save_path(),g_world.name);
    plat_mkdir(path);
    snprintf(path,sizeof path,"%s/%s/savedHeroes",game_save_path(),g_world.name);
    plat_mkdir(path);
    f=plat_fopen(cookie_owner,"wb");
    if(!f) { wos_log_event("scene_error","op=SET reason=cookie_save"); return; }
    if(fwrite(cookies,sizeof cookies[0],(size_t)cookie_count,f)!=(size_t)cookie_count)
        wos_log_event("scene_error","op=SET reason=cookie_write");
    fclose(f);
}
static void cookie_set(const char *key,const char *value)
{
    int i;
    if(eq(key,"g.num")) { g_hero.gender=number(value)&3; return; }
    for(i=0;i<cookie_count;i++) if(eq(cookies[i].key,key)) break;
    if(i==COOKIES) { wos_log_event("scene_error","op=SET reason=cookie_capacity"); return; }
    if(i==cookie_count) cookie_count++;
    copy(cookies[i].key,sizeof cookies[i].key,key);
    copy(cookies[i].value,sizeof cookies[i].value,value);
    cookies_save();
}
static const char *cookie_get(const char *key,char *buf,size_t cap)
{
    int i;
    int64_t n=0;
    if(eq(key,"str.name")||eq(key,"str.soul")) return g_hero.name;
    if(eq(key,"str.worldName")) return g_world.name;
    if(eq(key,"str.mapName") && g_hero.map>=0 && g_hero.map<WORLD_MAX_MAPS) return g_world.maps[g_hero.map].name;
    if(eq(key,"num.mapNum")) n=g_hero.map;
    else if(eq(key,"num.mapFlags")||eq(key,"num.sceneMapFlags")) n=vm.flags;
    else if(eq(key,"num.hostClass")) n=g_hero.klass;
    else if(eq(key,"num.hostLevel")) n=g_hero.level;
    else if(eq(key,"num.hostHP")) n=g_hero.hp;
    else if(eq(key,"num.hostMP")) n=g_hero.mp;
    else if(eq(key,"num.hostMaxHP")) n=g_hero.max_hp;
    else if(eq(key,"num.hostMaxMP")) n=g_hero.max_mp;
    else if(eq(key,"num.hostGP")) n=g_hero.gold;
    else if(eq(key,"num.hostXP")) n=g_hero.xp;
    else if(eq(key,"num.hostX")) n=g_hero.x;
    else if(eq(key,"num.hostY")) n=g_hero.y;
    else if(eq(key,"g.num")) n=g_hero.gender;
    else {
        for(i=0;i<cookie_count;i++) if(eq(cookies[i].key,key)) return cookies[i].value;
        return "0";
    }
    snprintf(buf,cap,"%lld",(long long)n); return buf;
}
static void append(char *out,size_t cap,size_t *used,const char *s)
{
    while(*s && *used+1<cap) out[(*used)++]=*s++;
    out[*used]=0;
}
static void expand(const char *s,char *out,size_t cap)
{
    size_t n=0;
    out[0]=0;
    while(*s && n+1<cap) {
        if(s[0]=='#' && s[1]=='<') {
            const char *end=strchr(s+2,'>');
            if(end) {
                char key[96],buf[64]; size_t k=(size_t)(end-s-2);
                if(k>=sizeof key) k=sizeof key-1;
                memcpy(key,s+2,k); key[k]=0;
                append(out,cap,&n,cookie_get(key,buf,sizeof buf)); s=end+1; continue;
            }
        }
        if(*s=='%' && s[1]) {
            char code=(char)toupper((unsigned char)s[1]);
            const char *value=NULL;
            char buf[64]; int id=0;
            s+=2;
            if(code=='%') value="%";
            else if(code=='0') value=vm.actors[vm.selected].name;
            else if(code=='1'||code=='2') value=g_hero.name;
            else if(code=='C') {
                id=g_hero.klass;
                if(isdigit((unsigned char)*s)) { id=number(s); while(isdigit((unsigned char)*s)) s++; }
                if(id>=0&&id<WORLD_MAX_CLASSES) value=g_world.classes[id].name;
            } else if(code=='I'||code=='M'||code=='S'||code=='R'||code=='L') {
                id=number(s); while(isdigit((unsigned char)*s)||*s=='-') s++;
                if(code=='I'&&id>0&&id<WORLD_MAX_ITEMS) value=g_world.items[id].name;
                if(code=='M'&&id>0&&id<WORLD_MAX_MONSTERS) value=g_world.monsters[id].name;
                if(code=='S'&&id>0&&id<WORLD_MAX_SPELLS) value=g_world.spells[id].name;
                if(code=='R') { snprintf(buf,sizeof buf,"%u",id>0?1+rng_bounded(game_rng(),(uint32_t)id):0); value=buf; }
                if(code=='L') { snprintf(buf,sizeof buf,"%d",g_hero.level); value=buf; }
            }
            if(value) append(out,cap,&n,value);
            else { buf[0]='%';buf[1]=code;buf[2]=0;append(out,cap,&n,buf); }
            continue;
        }
        out[n++]=*s++; out[n]=0;
    }
}
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
static void theme(int id)
{
    int i,inside=0,n;
    char t[ARGS][256],path[640];
    free(vm.theme);vm.theme=NULL;vm.theme_size=0;vm.theme_period=0;
    if(id<0) return;
    if(!id) id=vm.link.theme;
    for(i=0;i<g_world.line_count;i++) {
        n=world_tokenize(g_world.lines[i],t,ARGS);
        if(n<=0) continue;
        if(eq(t[0],"+THEMES")) { inside=1;continue; }
        if(eq(t[0],"-THEMES")) break;
        if(inside && n>=3 && number(t[0])==id) {
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
                if(rate && data && data<=INT_MAX/60) vm.theme_period=(int)(data*60/rate);
                vm.theme_frames=vm.theme_period;
                plat_sound_play(vm.theme,vm.theme_size);
            }
            return;
        }
    }
}
static void finish(void)
{
    hero_save(&g_hero);
    if(vm.number==0) game_go_well(); else game_return_to_map();
}
static void say(const char *text,const char *speaker,Suspend kind)
{
    expand(text,vm.dialog,sizeof vm.dialog);
    copy(vm.speaker,sizeof vm.speaker,speaker);
    vm.reveal=0;vm.dialog_age=0;vm.suspended=kind;
}
static int atom(const char *s)
{
    int id=number(s+1),count=1,i; const char *dot=strchr(s,'.');
    if(dot) count=number(dot+1);
    if(eq(s,"ALIVE")) return g_hero.hp>0;
    if(eq(s,"DEAD")) return g_hero.hp<=0;
    if(eq(s,"WIN")||eq(s,"WON")) return vm.outcome==BATTLE_WON;
    if(eq(s,"LOSE")) return vm.outcome==BATTLE_LOST||vm.outcome==BATTLE_FLED;
    if(eq(s,"YES")) return vm.yes;
    if(eq(s,"NO")) return !vm.yes;
    switch(toupper((unsigned char)*s)) {
    case '#': return g_hero.map==id;
    case 'C': return g_hero.klass==id;
    case 'F': return (vm.flags&(uint32_t)id)!=0;
    case 'G': return g_hero.gold>=id;
    case 'I': return inventory_count(id)>=count;
    case 'T': return id>=0&&id<HERO_TOKENS&&g_hero.tokens[id];
    case 'V': return g_hero.level>=id;
    case 'R': return (int)rng_bounded(game_rng(),100)<id;
    case 'E':
        if(g_hero.right_hand==id) return 1;
        for(i=0;i<8;i++) if(g_hero.equip[i]==id) return 1;
        return 0;
    case 'Q': return strstr(vm.reply,s+1)!=NULL;
    case 'Z': { char key[96],buf[64];snprintf(key,sizeof key,"num.Trophy%d",id);return number(cookie_get(key,buf,sizeof buf))>=count; }
    default:return 0;
    }
}
static int condition(const char *s)
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
static void give(const char *arg,int take)
{
    int id=number(arg+1),count=1,i; const char *dot=strchr(arg,'.');
    if(dot) count=number(dot+1);
    if(id<0||count<0) return;
    switch(toupper((unsigned char)*arg)) {
    case 'T': if(id<HERO_TOKENS) g_hero.tokens[id]=(unsigned char)!take;break;
    case 'G':
        if(take) g_hero.gold=g_hero.gold>id?g_hero.gold-id:0;
        else if(g_hero.gold<=INT64_MAX-id) g_hero.gold+=id;
        break;
    case 'H': if(g_hero.hp>0) g_hero.hp=bounded_add(g_hero.hp,take?-id:id,g_hero.max_hp);break;
    case 'M': g_hero.mp=bounded_add(g_hero.mp,take?-id:id,g_hero.max_mp);break;
    /* Retail Evergreen uses GIVE L1 to resurrect (quest.txt:1625). */
    case 'L': if(id==1) { if(take) g_hero.hp=0;else if(g_hero.hp<=0) g_hero.hp=1; }break;
    case 'I':
        if(!id||id>=WORLD_MAX_ITEMS||!g_world.items[id].used) break;
        for(i=0;i<HERO_INVENTORY;i++) if(g_hero.inventory[i].item_id==id) break;
        if(i==HERO_INVENTORY && !take) for(i=0;i<HERO_INVENTORY;i++) if(!g_hero.inventory[i].count) break;
        if(i<HERO_INVENTORY) {
            g_hero.inventory[i].item_id=id;
            g_hero.inventory[i].count=bounded_add(g_hero.inventory[i].count,take?-count:count,INT_MAX);
            if(!g_hero.inventory[i].count) g_hero.inventory[i].item_id=0;
        }
        break;
    case 'Z': { char key[96],buf[64],val[64];int old;
        snprintf(key,sizeof key,"num.Trophy%d",id);old=number(cookie_get(key,buf,sizeof buf));
        snprintf(val,sizeof val,"%d",bounded_add(old,take?-count:count,INT_MAX));cookie_set(key,val);break; }
    default: unknown_op(arg);break;
    }
}
static int jump(const char *label)
{
    const char *at=strchr(label,'@');
    int scene=vm.number,i,n;char t[ARGS][256];
    if(at && at!=label) scene=number(label);
    if(!at) at=label;
    if(scene<0||scene>=WORLD_MAX_SCENES||!g_world.scenes[scene].used) return 0;
    for(i=g_world.scenes[scene].first_line+1;i<g_world.scenes[scene].end_line;i++) {
        n=world_tokenize(g_world.lines[i],t,ARGS);
        if(n>0 && eq(t[0],at)) {
            vm.number=scene;vm.pc=i+1;vm.end=g_world.scenes[scene].end_line;return 1;
        }
    }
    wos_log_event("scene_error","op=GOTO label=%s",label);return 0;
}
static void offer(char t[][256],int n,int filtered)
{
    size_t used=0;int i,j;
    vm.dialog[0]=0;
    append(vm.dialog,sizeof vm.dialog,&used,"Available wares:\n");
    for(i=1;i<WORLD_MAX_ITEMS;i++) {
        const ItemDef *item=&g_world.items[i];int show=0;
        if(!item->used) continue;
        if(!filtered) { for(j=1;j<n;j++) if(number(t[j])==i) show=1; }
        else for(j=1;j+1<n;j+=3) if(item->gp>0 && item->level>=number(t[j])&&item->level<=number(t[j+1]) && (j+2>=n||item->klass==number(t[j+2]))) show=1;
        if(show) { char line[128];snprintf(line,sizeof line,"%s (%d gold)\n",item->name,item->gp);append(vm.dialog,sizeof vm.dialog,&used,line); }
        if(used>650) { append(vm.dialog,sizeof vm.dialog,&used,"...\n");break; }
    }
    copy(vm.speaker,sizeof vm.speaker,"Shop - press Enter to close");
    vm.reveal=(int)strlen(vm.dialog);vm.dialog_age=0;vm.suspended=OFFERING;
}
static void step(void)
{
    char t[ARGS][256],expanded[256];
    const char *raw;
    int n,i;
    if(vm.pc>=vm.end) { finish();return; }
    raw=g_world.lines[vm.pc];
    n=world_tokenize(raw,t,ARGS);
    if(n<=0) { vm.pc++;return; }
    wos_log_event("scene_op","scene=%d line=%d op=%s",vm.number,vm.pc,t[0]);
    for(i=1;i<n;i++) { expand(t[i],expanded,sizeof expanded);copy(t[i],sizeof t[i],expanded); }
    vm.pc++; /* Suspensions resume at the successor, and never re-execute side effects. */
    while(isspace((unsigned char)*raw)) raw++;
    if(*raw=='\'' || *raw=='"' || (raw[0]&&raw[1]==':')) {
        const char *speaker=vm.actors[vm.selected].name;
        if(raw[1]==':') {
            if(isdigit((unsigned char)raw[0])) { vm.selected=raw[0]-'0';speaker=vm.actors[vm.selected].name; }
            else if(toupper((unsigned char)raw[0])=='N') speaker="";
            else speaker=g_hero.name;
            raw+=2;
        } else raw++;
        while(*raw==' ') raw++;
        { char text[DIALOG];const char *end=strchr(raw,';');size_t len=end?(size_t)(end-raw):strlen(raw);
          if(len>=sizeof text) len=sizeof text-1;
          memcpy(text,raw,len);text[len]=0;say(text,speaker,SPEAKING); }
        return;
    }
    if(t[0][0]=='@'||eq(t[0],"TOKEN")) return;
    if(eq(t[0],"SCENE")) {
        background(n>2?t[2]:(*vm.link.background?vm.link.background:"fight"));
        vm.hide_hero=n>3&&eq(t[3],"CUT");
        copy(vm.title,sizeof vm.title,n>4?t[4]:vm.link.name);return;
    }
    if(eq(t[0],"THEME")) { theme(n>1?number(t[1]):0);return; }
    if(eq(t[0],"MUSIC")) {
        char path[640];
        if(n<2||!t[1][0]) plat_music_stop();
        else if(asset_path(path,sizeof path,"MIDI",t[1],".mid")) plat_music_play(path,1);
        return;
    }
    if(eq(t[0],"SOUND")) { if(n>1) sound(t[1]);return; }
    if(eq(t[0],"ACTOR")&&n>=5) {
        Actor *a;int id=number(t[1])&63;
        int x=n>5?number(t[5]):50,y=n>6?number(t[6]):75;
        char path[640];
        if(x>INT_MAX/1024||x<INT_MIN/1024||y>INT_MAX/1024||y<INT_MIN/1024) {
            wos_log_event("scene_error","op=ACTOR reason=coordinate_range");return;
        }
        vm.selected=id;a=&vm.actors[id];sheet_free(&a->sheet);memset(a,0,sizeof *a);
        a->used=1;a->poses=1;a->pose[0]=number(t[4]);a->speed=256;
        copy(a->name,sizeof a->name,t[2]);
        if(asset_path(path,sizeof path,"skins",t[3],".bmp")) sheet_load_skin(&a->sheet,t[3]);
        else sheet_load_monster(&a->sheet,t[3]);
        a->x=a->tx=x*256;
        a->y=a->ty=y*256;return;
    }
    if(eq(t[0],"SEL")) { if(n>1) vm.selected=number(t[1])&63;return; }
    if(eq(t[0],"POSE")) {
        Actor *a=&vm.actors[vm.selected];a->poses=n-1;if(a->poses>3)a->poses=3;
        for(i=0;i<a->poses;i++) a->pose[i]=number(t[i+1]);
        a->frame=0;a->age=0;return;
    }
    if(eq(t[0],"MOVE")&&n>=4) {
        Actor *a=eq(t[1],"H")?&vm.host:&vm.actors[number(t[1])&63];int mode=n>4?number(t[4]):0;
        int x=number(t[2]),y=number(t[3]);
        if(x>INT_MAX/1024||x<INT_MIN/1024||y>INT_MAX/1024||y<INT_MIN/1024) {
            wos_log_event("scene_error","op=MOVE reason=coordinate_range");return;
        }
        a->tx=x*256;a->ty=y*256;a->speed=mode>1&&mode<=5?mode*128:128;
        if(mode==1) {a->x=a->tx;a->y=a->ty;}return;
    }
    if(eq(t[0],"WAIT")) {
        double seconds=n>1?strtod(t[1],NULL):0;
        vm.wait=seconds>3600?216000:seconds>0?(int)(seconds*60):0;
        vm.suspended=WAITING;return;
    }
    if(eq(t[0],"ASK")) {
        double seconds=n>1?strtod(t[1],NULL):30;
        vm.wait=seconds>3600?216000:seconds>0?(int)(seconds*60):1800;
        vm.suspended=ASKING;vm.yes=0;vm.reply[0]=0;vm.reveal=(int)strlen(vm.dialog);return;
    }
    if(eq(t[0],"FIGHT")||eq(t[0],"FIGHT2")) {
        int ids[ARGS],count=0,difficulty=vm.link.difficulty;
        for(i=1;i<n;i++) if(number(t[i])) ids[count++]=number(t[i]);
        if(!count) count=game_take_pending_fight(ids,ARGS,&difficulty);
        if(count<0) count=0;
        battle_begin(ids,count,n>1&&count?0:difficulty);vm.suspended=FIGHTING;return;
    }
    if(eq(t[0],"SET")) { if(n>2) cookie_set(t[1],t[2]);return; }
    if(eq(t[0],"COMPARE")) {
        int64_t d=n>2?(int64_t)number(t[1])-number(t[2]):0;
        vm.compare=d;return;
    }
    if(eq(t[0],"IF")&&n>=3) {
        char cond[DIALOG];size_t used=0;cond[0]=0;
        for(i=1;i<n-1;i++) append(cond,sizeof cond,&used,t[i]);
        if(condition(cond)) jump(t[n-1]);
        return;
    }
    if(toupper((unsigned char)t[0][0])=='I'&&toupper((unsigned char)t[0][1])=='F'&&n>=2) {
        int ok=0;
        if(eq(t[0],"IF="))ok=vm.compare==0;
        else if(eq(t[0],"IF>"))ok=vm.compare>0;
        else if(eq(t[0],"IF<"))ok=vm.compare<0;
        else if(eq(t[0],"IF>="))ok=vm.compare>=0;
        else if(eq(t[0],"IF<="))ok=vm.compare<=0;
        else if(eq(t[0],"IF<>")||eq(t[0],"IF!="))ok=vm.compare!=0;
        else if(eq(t[0],"IFEVEN"))ok=(vm.compare&1)==0;
        else if(eq(t[0],"IFODD"))ok=(vm.compare&1)!=0;
        else {unknown_op(t[0]);return;}
        if(ok)jump(t[n-1]);
        return;
    }
    if(eq(t[0],"GOTO")&&n>1) {
        if(eq(t[1],"EXIT"))finish();
        else if(eq(t[1],"SCENE")&&n>2) { Link link=vm.link;game_enter_scene(number(t[2]),&link); }
        else if(eq(t[1],"LINK")&&n>3) game_enter_map(number(t[2]),number(t[3]),n>4?number(t[4]):0);
        else jump(t[1]);
        return;
    }
    if(eq(t[0],"GIVE")||eq(t[0],"TAKE")||eq(t[0],"HOST_GIVE")||eq(t[0],"HOST_TAKE")) {
        int take=eq(t[0],"TAKE")||eq(t[0],"HOST_TAKE");
        for(i=1;i<n;i++)give(t[i],take);
        return;
    }
    if(eq(t[0],"FLAGS")) {vm.flags=n>1?(uint32_t)strtoul(t[1],NULL,0):0;return;}
    if(eq(t[0],"LOCK")) {vm.locked=n>1&&number(t[1]);return;}
    if(eq(t[0],"OFFER")||eq(t[0],"OFFER2")) {offer(t,n,eq(t[0],"OFFER2"));return;}
    if(eq(t[0],"END")) {finish();return;}
    unknown_op(t[0]);
}

uint32_t scene_flags(void) { return vm.flags; }
static int hit(const Input *in,Rect r)
{
    return (in->mouse_pressed&(1u<<1)) && in->mouse_x>=r.x && in->mouse_y>=r.y && in->mouse_x<r.x+r.w && in->mouse_y<r.y+r.h;
}
static Rect button_rect(int i) { Rect r={415+i*51,8,48,48};return r; }
static void actors_update(void)
{
    int i;
    for(i=0;i<=ACTORS;i++) {
        Actor *a=i==ACTORS?&vm.host:&vm.actors[i];int d;
        if(!a->used)continue;
        d=a->tx-a->x;if(d>a->speed)d=a->speed;if(d< -a->speed)d= -a->speed;a->x+=d;
        d=a->ty-a->y;if(d>a->speed)d=a->speed;if(d< -a->speed)d= -a->speed;a->y+=d;
        if(++a->age>=60) {a->age=0;if(a->poses>1)a->frame=(a->frame+1)%a->poses;}
    }
}
static void scene_update(const Input *in)
{
    int advance=in->pressed[PLAT_KEY_RETURN]||in->pressed[PLAT_KEY_SPACE]||((in->mouse_pressed&((1u<<1)|(1u<<3)))&&in->mouse_x<364);
    actors_update();
    if(vm.theme_period>0&&--vm.theme_frames<=0) {plat_sound_play(vm.theme,vm.theme_size);vm.theme_frames=vm.theme_period;}
    if(hit(in,button_rect(0)))vm.stats=!vm.stats;
    if(vm.suspended==FIGHTING) {
        BattleResult result;
        if(hit(in,button_rect(3))) {Input flee=*in;flee.pressed[PLAT_KEY_ESCAPE]=1;result=battle_update(&flee);}
        else result=battle_update(in);
        if(!battle_active()&&result!=BATTLE_RUNNING) {vm.outcome=result;vm.suspended=RUN;}
        return;
    }
    if(in->pressed[PLAT_KEY_ESCAPE]||hit(in,button_rect(1))||hit(in,button_rect(3))) {finish();return;}
    if(hit(in,button_rect(2))) {hero_save(&g_hero);game_go_well();return;}
    if(vm.suspended==WAITING) {if(vm.wait>0)vm.wait--;else vm.suspended=RUN;return;}
    if(vm.suspended==SPEAKING||vm.suspended==OFFERING) {
        int len=(int)strlen(vm.dialog);
        vm.dialog_age++;
        if(vm.reveal<len && vm.dialog_age%2==0)vm.reveal++; /* 30 chars/s, battle.md section 4. */
        if(advance) {if(vm.reveal<len)vm.reveal=len;else vm.suspended=RUN;}
        else if(vm.suspended==SPEAKING&&vm.dialog_age>len*2+180)vm.suspended=RUN;
        return;
    }
    if(vm.suspended==ASKING) {
        size_t len=strlen(vm.reply),i;
        for(i=0;in->text[i]&&len+1<sizeof vm.reply;i++) if((unsigned char)in->text[i]>=32)vm.reply[len++]=in->text[i];
        vm.reply[len]=0;
        if(in->pressed[PLAT_KEY_BACKSPACE]&&len)vm.reply[--len]=0;
        if(in->pressed['y']||hit(in,(Rect){60,350,100,32})) {copy(vm.reply,sizeof vm.reply,"YES");vm.yes=1;vm.suspended=RUN;}
        else if(in->pressed['n']||hit(in,(Rect){190,350,100,32})) {copy(vm.reply,sizeof vm.reply,"NO");vm.yes=0;vm.suspended=RUN;}
        else if(in->pressed[PLAT_KEY_RETURN]||--vm.wait<=0) {vm.yes=eq(vm.reply,"YES")||eq(vm.reply,"Y");vm.suspended=RUN;}
        return;
    }
    step();
}
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
    int w=sheet->cell*364/360,h=sheet->cell*416/256;
    if(pose<0||pose>=sheet->count)return;
    scaled(fb,&sheet->image,(Rect){pose*sheet->cell,0,sheet->cell,sheet->cell},(Rect){x-w/2,y-h,w,h},sheet->key);
}
static void draw_button(Framebuffer *fb,Rect r,const char *label,int enabled)
{
    fb_fill(fb,r,enabled?0x34485c:0x222832);fb_rect(fb,r,0x8090a0);
    font_draw(fb,r.x+(r.w-font_width(label))/2,r.y+20,label,enabled?0xffffff:0x778088);
}
static void scene_render(Framebuffer *fb)
{
    int i;char buf[DIALOG];
    fb_clear(fb,0x151d28);
    fb_clip(fb,(Rect){0,0,364,416});
    scaled(fb,&vm.background,(Rect){0,0,vm.background.w,vm.background.h},(Rect){0,0,364,416},-1);
    if(vm.suspended==FIGHTING)battle_render(fb,(Rect){0,0,364,416});
    else {
        if(!vm.hide_hero&&g_hero.valid)draw_actor(fb,&vm.hero,1,(int)((int64_t)vm.host.x*364/25600),(int)((int64_t)vm.host.y*416/25600));
        for(i=0;i<ACTORS;i++)if(vm.actors[i].used) {
            Actor *a=&vm.actors[i];
            draw_actor(fb,&a->sheet,a->pose[a->frame],(int)((int64_t)a->x*364/25600),(int)((int64_t)a->y*416/25600));
        }
        if(vm.suspended==SPEAKING||vm.suspended==ASKING||vm.suspended==OFFERING) {
            size_t len=strlen(vm.dialog),shown=vm.reveal<0?0:(size_t)vm.reveal;
            if(shown>len)shown=len;
            memcpy(buf,vm.dialog,shown);buf[shown]=0;
            fb_fill(fb,(Rect){8,12,348,220},0xeee5cd);fb_rect(fb,(Rect){8,12,348,220},0x614b30);
            font_draw(fb,16,22,vm.speaker,0x653616);
            font_wrap(fb,(Rect){16,40,330,180},buf,0x241a11);
            if(vm.suspended==ASKING) {
                draw_button(fb,(Rect){60,350,100,32},"YES [Y]",1);draw_button(fb,(Rect){190,350,100,32},"NO [N]",1);
                font_draw(fb,16,330,vm.reply,0xffffff);
            }
        }
    }
    fb_reset_clip(fb);
    draw_button(fb,button_rect(0),"Stats",1);
    draw_button(fb,button_rect(1),"Map",vm.suspended!=FIGHTING);
    draw_button(fb,button_rect(2),"Well",vm.suspended!=FIGHTING);
    draw_button(fb,button_rect(3),vm.suspended==FIGHTING?"Flee":"Exit",1);
    fb_rect(fb,(Rect){364,70,256,256},0x718294);
    if(g_hero.valid)sheet_draw(fb,&vm.hero,1,468-vm.hero.cell/2,285-vm.hero.cell,0);
    font_wrap(fb,(Rect){376,82,232,32},g_hero.name,0xffe4a0);
    snprintf(buf,sizeof buf,"Level %d\nHP %d / %d\nMP %d / %d\nGold %lld",g_hero.level,g_hero.hp,g_hero.max_hp,g_hero.mp,g_hero.max_mp,(long long)g_hero.gold);
    font_wrap(fb,(Rect){376,115,232,80},buf,0xffffff);
    if(vm.stats) {
        snprintf(buf,sizeof buf,"STR %d  WIS %d\nSTA %d  AGI %d  DEX %d",g_hero.ability[0],g_hero.ability[1],g_hero.ability[2],g_hero.ability[3],g_hero.ability[4]);
        font_wrap(fb,(Rect){376,290,232,32},buf,0xffffff);
    }
    font_wrap(fb,(Rect){376,340,232,60},vm.title,0xc8d9e8);
    if(vm.locked)font_draw(fb,376,400,"Scene locked",0xffc060);
    fb_fill(fb,(Rect){0,416,640,64},0x202c38);
    fb_fill(fb,(Rect){8,424,176,12},0x501010);
    fb_fill(fb,(Rect){8,424,g_hero.max_hp>0?(int)((int64_t)176*g_hero.hp/g_hero.max_hp):0,12},0xb02e2e);
    fb_fill(fb,(Rect){194,424,176,12},0x101050);
    fb_fill(fb,(Rect){194,424,g_hero.max_mp>0?(int)((int64_t)176*g_hero.mp/g_hero.max_mp):0,12},0x3059b0);
    snprintf(buf,sizeof buf,"HP %d/%d",g_hero.hp,g_hero.max_hp);
    font_draw(fb,14,426,buf,0xffffff);
    snprintf(buf,sizeof buf,"MP %d/%d",g_hero.mp,g_hero.max_mp);
    font_draw(fb,200,426,buf,0xffffff);
    font_wrap(fb,(Rect){8,444,624,28},vm.suspended==FIGHTING?"Fight: choose an action, or Escape to flee.":"Click / Space / Enter: advance dialog. Escape: leave scene.",0xd5dce8);
}
static void scene_leave(void)
{
    int i;
    image_free(&vm.background);sheet_free(&vm.hero);
    for(i=0;i<ACTORS;i++)sheet_free(&vm.actors[i].sheet);
    free(vm.theme);vm.theme=NULL;vm.theme_size=0;
}
static const Screen scene_screen={"scene",NULL,scene_update,scene_render,scene_leave};
void game_enter_scene(int scene_no,const Link *link)
{
    Link saved;
    memset(&saved,0,sizeof saved);if(link)saved=*link;
    screen_set(NULL);memset(&vm,0,sizeof vm);
    vm.number=scene_no;vm.link=saved;
    vm.host.used=1;vm.host.speed=128;
    vm.host.x=vm.host.tx=20*256;vm.host.y=vm.host.ty=87*256;
    vm.flags=g_hero.map>=0&&g_hero.map<WORLD_MAX_MAPS?g_world.maps[g_hero.map].flags:0;
    if(scene_no<0||scene_no>=WORLD_MAX_SCENES||!g_world.scenes[scene_no].used) {
        wos_log_event("scene_error","scene=%d reason=missing",scene_no);finish();return;
    }
    cookies_load();vm.pc=g_world.scenes[scene_no].first_line;vm.end=g_world.scenes[scene_no].end_line;
    if(g_hero.valid)sheet_load_skin(&vm.hero,g_hero.skin);
    screen_set(&scene_screen);
    wos_log_event("scene_enter","scene=%d",scene_no);
}
