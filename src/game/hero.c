/* Solo hero model. Creation: boot_flow.md 3c / script.md 8.2.
 * Saves deliberately use a versioned, endian-independent port format, not retail .her. */
#include "hero.h"
#include "world.h"
#include "../game_main.h"
#include "../engine/ini.h"
#include "../engine/text.h"
#include "../engine/log.h"
#include "../platform/platform.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <limits.h>
#include <math.h>

Hero g_hero;

static const ClassDef *hero_class(const Hero *h)
{
    if (h->klass < 1 || h->klass >= WORLD_MAX_CLASSES) return NULL;
    return &g_world.classes[h->klass];
}

static int bounded_stat(int64_t n) { return n < 0 ? 0 : n > INT_MAX ? INT_MAX : (int)n; }

static void maxima(const ClassDef *c, int level, int *hp, int *mp)
{
    int n, dh = 0, dm = 0;
    int64_t h = c->levels[0].d_hp, m = c->levels[0].d_mp;
    /* FUN_00483984: sparse positive increments carry forward. AUTO_MAX
     * replaces them with the capped linear-target curve, not a straight lerp. */
    if (c->auto_max_set) {
        int start = c->auto_max[4];
        int64_t hrange = (int64_t)c->auto_max[1] - c->auto_max[0];
        int64_t mrange = (int64_t)c->auto_max[3] - c->auto_max[2];
        h = c->auto_max[0]; m = 0;
        for (n = 0; n <= level; ++n) {
            if (n >= 2) {
                int64_t inc = (n - 1) * hrange / 99 - h + c->auto_max[0];
                int64_t cap = (hrange / 5000 + 1) * n;
                if (inc > cap) inc = cap;
                if (inc > 0) h += inc;
            }
            if (n == start) m = c->auto_max[2];
            else if (n > start && start < 100) {
                int64_t inc = (n - start) * mrange / (100 - start) - m + c->auto_max[2];
                int64_t cap = (mrange / 5000 + 1) * (n - start + 1);
                if (inc > cap) inc = cap;
                if (inc > 0) m += inc;
            }
        }
    } else {
        for (n = 1; n <= level; ++n) {
            int row = n == 100 ? 99 : n;
            if (c->levels[row].d_hp > 0) dh = c->levels[row].d_hp;
            if (c->levels[row].d_mp > 0) dm = c->levels[row].d_mp;
            h += dh; m += dm;
        }
    }
    *hp = bounded_stat(h); if (*hp < 1) *hp = 1;
    *mp = bounded_stat(m);
}

void hero_create(Hero *h, const char *name, int klass, int gender, const char *skin)
{
    const ClassDef *c;
    int i;
    memset(h, 0, sizeof(*h));
    h->klass = klass; c = hero_class(h);
    if (!c || !c->used) return;
    h->valid = 1; h->level = 1;
    h->gender = gender >= 0 && gender < 4 ? gender : 0;
    snprintf(h->name, sizeof(h->name), "%s", name ? name : "");
    memcpy(h->ability, c->start_ability, sizeof(h->ability));
    /* New Soul reads the class's level-zero starting values (0x460A7E),
     * not the first cumulative level increment. */
    h->max_hp = c->auto_max_set ? c->auto_max[0] : c->levels[0].d_hp;
    h->max_mp = c->auto_max_set ? (c->auto_max[4] == 0 ? c->auto_max[2] : 0) : c->levels[0].d_mp;
    if (h->max_hp < 1) h->max_hp = 1;
    if (h->max_mp < 0) h->max_mp = 0;
    h->hp = h->max_hp; h->mp = h->max_mp;
    h->gold = g_world.starting_gp;
    if (h->gold < 0) h->gold = 0;
    if (h->gold > 20000) h->gold = 20000;
    if (c->max_wallet >= 0 && h->gold > c->max_wallet) h->gold = c->max_wallet;
    h->map = c->start_location_set ? c->start_map : 0;
    h->link = c->start_location_set ? c->start_link : 0;
    if (!skin || !*skin) skin = c->default_skin[h->gender];
    if (skin && *skin && strcmp(skin, ".")) snprintf(h->skin, sizeof(h->skin), "%s", skin);
    else {
        char path[768], section[8];
        char *text;
        Ini ini;
        world_path(path, sizeof(path), "gender.ini");
        text = text_read_file(path, NULL);
        snprintf(section, sizeof(section), "%d", h->gender);
        if (text && ini_parse(&ini, text) == 0)
            snprintf(h->skin, sizeof(h->skin), "%s", ini_get(&ini, section, "adventurer", "adventurer"));
        else snprintf(h->skin, sizeof(h->skin), "adventurer");
        free(text);
    }
    for (i = 0; i < c->start_item_count && i < 8; ++i) {
        int id = c->start_items[i], j;
        if (id <= 0 || id >= WORLD_MAX_ITEMS || !g_world.items[id].used) continue;
        for (j = 0; j < HERO_INVENTORY; ++j) {
            if (h->inventory[j].item_id == id || !h->inventory[j].count) {
                h->inventory[j].item_id = id; ++h->inventory[j].count; break;
            }
        }
        /* Item classes 12..19 are right hands 1..8 (art.md 5.4). */
        if (!h->right_hand && g_world.items[id].klass == c->right_hand + 11)
            h->right_hand = id;
    }
}

static int safe_name(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    if (!p || !*p || strlen(s) >= HERO_NAME_MAX || !strcmp(s,".") || !strcmp(s,"..")) return 0;
    for (; *p; ++p) if (*p < 32 || strchr("/\\:", *p)) return 0;
    return 1;
}

static int save_path(char *path, size_t size, const char *name, int create)
{
    int n;
    if (name && !safe_name(name)) return -1;
    if (!*g_world.name || strchr(g_world.name, '/') || strchr(g_world.name, '\\')) return -1;
    n = snprintf(path, size, "%s", game_save_path());
    if (n < 0 || (size_t)n >= size || (create && plat_mkdir(path))) return -1;
    n = snprintf(path, size, "%s/%s", game_save_path(), g_world.name);
    if (n < 0 || (size_t)n >= size || (create && plat_mkdir(path))) return -1;
    n = snprintf(path, size, "%s/%s/savedHeroes", game_save_path(), g_world.name);
    if (n < 0 || (size_t)n >= size || (create && plat_mkdir(path))) return -1;
    if (name) n = snprintf(path, size, "%s/%s/savedHeroes/%s.wsh", game_save_path(), g_world.name, name);
    return n < 0 || (size_t)n >= size ? -1 : 0;
}

/* Explicit little endian primitives: no struct padding, host width, or pointers on disk. */
static void put32(FILE *f, int32_t value)
{
    uint32_t n = (uint32_t)value; int i;
    for (i = 0; i < 4; ++i) { fputc((int)(n & 255), f); n >>= 8; }
}
static int32_t get32(FILE *f)
{
    uint32_t n = 0; int i;
    for (i = 0; i < 4; ++i) { int c = fgetc(f); if (c == EOF) return 0; n |= (uint32_t)c << (i * 8); }
    return n <= INT32_MAX ? (int32_t)n : -1 - (int32_t)(UINT32_MAX - n);
}
static void put64(FILE *f, int64_t n)
{ put32(f, (int32_t)((uint64_t)n & UINT32_MAX)); put32(f, (int32_t)((uint64_t)n >> 32)); }
static int64_t get64(FILE *f)
{
    uint64_t lo = (uint32_t)get32(f), hi = (uint32_t)get32(f);
    uint64_t n = lo | hi << 32;
    return n <= INT64_MAX ? (int64_t)n : -1 - (int64_t)(UINT64_MAX - n);
}

int hero_save(const Hero *h)
{
    char path[1024]; FILE *f; int i, failed;
    if (!h->valid || save_path(path, sizeof(path), h->name, 1)) return -1;
    f = plat_fopen(path, "wb"); if (!f) return -1;
    fwrite("WSH1", 1, 4, f);
    fwrite(h->name, 1, sizeof(h->name), f); fwrite(h->skin, 1, sizeof(h->skin), f);
    put32(f,h->gender); put32(f,h->klass); put32(f,h->level);
    put64(f,h->xp); put64(f,h->gold);
    put32(f,h->hp); put32(f,h->max_hp); put32(f,h->mp); put32(f,h->max_mp);
    for (i=0;i<HERO_ABILITIES;++i) put32(f,h->ability[i]);
    put32(f,h->right_hand);
    for (i=0;i<8;++i) put32(f,h->equip[i]);
    for (i=0;i<HERO_INVENTORY;++i) { put32(f,h->inventory[i].item_id); put32(f,h->inventory[i].count); }
    fwrite(h->tokens,1,HERO_TOKENS,f);
    put32(f,h->map); put32(f,h->link); put32(f,h->x); put32(f,h->y);
    failed = ferror(f); if (fclose(f)) failed = 1;
    return failed ? -1 : 0;
}

int hero_load(Hero *h, const char *name)
{
    Hero v = {0}; char path[1024], magic[4]; FILE *f; int i, failed;
    if (save_path(path,sizeof(path),name,0)) return -1;
    f=plat_fopen(path,"rb"); if (!f) return -1;
    if (fread(magic,1,4,f)!=4 || memcmp(magic,"WSH1",4)) { fclose(f); return -1; }
    if (fread(v.name,1,sizeof(v.name),f)!=sizeof(v.name) ||
        fread(v.skin,1,sizeof(v.skin),f)!=sizeof(v.skin)) { fclose(f); return -1; }
    v.gender=get32(f); v.klass=get32(f); v.level=get32(f);
    v.xp=get64(f); v.gold=get64(f);
    v.hp=get32(f); v.max_hp=get32(f); v.mp=get32(f); v.max_mp=get32(f);
    for(i=0;i<HERO_ABILITIES;++i) v.ability[i]=get32(f);
    v.right_hand=get32(f);
    for(i=0;i<8;++i) v.equip[i]=get32(f);
    for(i=0;i<HERO_INVENTORY;++i) { v.inventory[i].item_id=get32(f); v.inventory[i].count=get32(f); }
    if(fread(v.tokens,1,HERO_TOKENS,f)!=HERO_TOKENS) { fclose(f); return -1; }
    v.map=get32(f); v.link=get32(f); v.x=get32(f); v.y=get32(f);
    failed=ferror(f)||feof(f); fclose(f);
    if(failed || !memchr(v.name,0,sizeof(v.name)) || !memchr(v.skin,0,sizeof(v.skin)) ||
       !safe_name(v.name) || text_casecmp(v.name,name) || !hero_class(&v) || !hero_class(&v)->used ||
       v.gender<0 || v.gender>3 || v.level<1 || v.level>100 || v.xp<0 || v.gold<0 ||
       v.max_hp<1 || v.hp<0 || v.hp>v.max_hp || v.max_mp<0 || v.mp<0 || v.mp>v.max_mp ||
       v.map<0 || v.map>=WORLD_MAX_MAPS || v.link<0 || v.link>=OBL_RECORDS ||
       v.right_hand<0 || v.right_hand>=WORLD_MAX_ITEMS) return -1;
    for(i=0;i<HERO_ABILITIES;++i) if(v.ability[i]<0 || v.ability[i]>255) return -1;
    for(i=0;i<8;++i) if(v.equip[i]<0 || v.equip[i]>=WORLD_MAX_ITEMS) return -1;
    for(i=0;i<HERO_INVENTORY;++i)
        if(v.inventory[i].item_id<0 || v.inventory[i].item_id>=WORLD_MAX_ITEMS || v.inventory[i].count<0) return -1;
    v.valid=1; *h=v; return 0;
}

typedef struct { char (*names)[HERO_NAME_MAX]; int count, max; } SaveList;
static void save_entry(const char *name,int is_dir,void *user)
{
    SaveList *l=user; size_t n=strlen(name);
    if(is_dir || n<5 || n-4>=HERO_NAME_MAX || text_casecmp(name+n-4,".wsh") || l->count>=l->max) return;
    memcpy(l->names[l->count],name,n-4); l->names[l->count][n-4]=0;
    if(safe_name(l->names[l->count])) ++l->count;
}
static int compare_names(const void *a,const void *b) { return text_casecmp(a,b); }
int hero_list_saves(char names[][HERO_NAME_MAX],int max)
{
    char path[1024]; SaveList l={names,0,max};
    if(max<=0 || save_path(path,sizeof(path),NULL,0)) return 0;
    plat_list_dir(path,save_entry,&l);
    qsort(names,(size_t)l.count,HERO_NAME_MAX,compare_names); return l.count;
}

int64_t hero_xp_for_level(const Hero *h,int level)
{
    int n; int64_t total=0, step=20;
    (void)h;
    if(level>100) level=100;
    /* FUN_00483984: cumulative shared-class curve. The decomp drops the
     * FMUL at 0x4846F3: doubles 0x4D0220=.01, 0x4D0228=450000. */
    for(n=1;n<level;++n) {
        int64_t cap=(int64_t)(pow(n * 0.01,2.5) * 450000.0)+10;
        step=step*150/100; if(step>cap) step=cap; total+=step;
    }
    return total;
}

int hero_award(Hero *h,int64_t xp,int64_t gold)
{
    const ClassDef *c=hero_class(h); int gained=0; int64_t cap;
    if(!h->valid || !c) return 0;
    if(xp>0) h->xp=xp>INT64_MAX-h->xp ? INT64_MAX : h->xp+xp;
    while(h->level<100 && h->xp>=hero_xp_for_level(h,h->level+1)) {
        int old_hp=h->max_hp, old_mp=h->max_mp;
        ++h->level; ++gained; maxima(c,h->level,&h->max_hp,&h->max_mp);
        h->hp=bounded_stat((int64_t)h->hp+h->max_hp-old_hp);
        h->mp=bounded_stat((int64_t)h->mp+h->max_mp-old_mp);
        wos_log_event("level_up","level=%d",h->level);
    }
    cap=(int64_t)(h->level+1)*10000; if(cap>1000000) cap=1000000;
    if(c->max_wallet>=0 && cap>c->max_wallet) cap=c->max_wallet;
    if(gold>0) h->gold=gold>INT64_MAX-h->gold ? INT64_MAX : h->gold+gold;
    if(h->gold>cap) h->gold=cap;
    return gained;
}
