/* Solo hero model. Creation: boot_flow.md 3c / script.md 8.2.
 * Inventory/equipment: 0x403349, 0x40C694, 0x40D6B4; consumables: 0x4A6353.
 * Training: hand/element click handlers 0x419107/0x425C30, proficiency
 * consumers 0x419306/0x4258B8; learning spells: 0x44E3BD.
 * Saves deliberately use versioned, endian-independent WSH3, not retail .her. */
#include "hero.h"
#include "world.h"
#include "scene.h"
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
    if (!h || h->klass < 1 || h->klass >= WORLD_MAX_CLASSES) return NULL;
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
    memcpy(h->hand_pp, c->start_hand_pp, sizeof(h->hand_pp));
    memcpy(h->element_pp, c->start_element_pp, sizeof(h->element_pp));
    /* levels.txt header / FUN_004207BD: preferred hand starts trained to five. */
    if (c->right_hand >= 1 && c->right_hand <= 8 && h->hand_pp[c->right_hand - 1] < 5000)
        h->hand_pp[c->right_hand - 1] = 5000;
    /* New Soul reads the class's level-zero starting values (0x460A7E),
     * not the first cumulative level increment. */
    h->max_hp = c->auto_max_set ? c->auto_max[0] : c->levels[0].d_hp;
    h->max_mp = c->auto_max_set ? (c->auto_max[4] == 0 ? c->auto_max[2] : 0) : c->levels[0].d_mp;
    if (h->max_hp < 1) h->max_hp = 1;
    if (h->max_mp < 0) h->max_mp = 0;
    h->hp = h->max_hp; h->mp = h->max_mp;
    hero_add_gold(h, g_world.starting_gp);
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
    for (i = 0; i < c->start_token_count && i < 8; ++i) {
        int token = c->start_tokens[i];
        if (token >= 0 && token < HERO_TOKENS) h->tokens[token] = 1;
    }
    for (i = 0; i < c->start_spell_count && i < 8; ++i)
        hero_set_spell(h, c->start_spells[i], 1);
    for (i = 0; i < c->start_item_count && i < 8; ++i) {
        int id = c->start_items[i];
        if (hero_give_item(h, id, 1) && !h->right_hand &&
            g_world.items[id].klass == c->right_hand + 11 && hero_can_equip(h, id))
            h->right_hand = id;
    }
    scene_reset_timers();
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
    fwrite("WSH3", 1, 4, f);
    fwrite(h->name, 1, sizeof(h->name), f); fwrite(h->skin, 1, sizeof(h->skin), f);
    put32(f,h->gender); put32(f,h->klass); put32(f,h->level);
    put64(f,h->xp); put64(f,h->gold);
    put32(f,h->hp); put32(f,h->max_hp); put32(f,h->mp); put32(f,h->max_mp);
    for (i=0;i<HERO_ABILITIES;++i) put32(f,h->ability[i]);
    put32(f,h->right_hand);
    for (i=0;i<8;++i) put32(f,h->equip[i]);
    for (i=0;i<8;++i) put32(f,h->hand_pp[i]);
    for (i=0;i<8;++i) put32(f,h->element_pp[i]);
    put64(f,h->pp); put32(f,(int32_t)h->ailments);
    for (i=0;i<HERO_INVENTORY;++i) { put32(f,h->inventory[i].item_id); put32(f,h->inventory[i].count); }
    fwrite(h->tokens,1,HERO_TOKENS,f);
    fwrite(h->learned_spells,1,sizeof(h->learned_spells),f);
    put32(f,h->map); put32(f,h->link); put32(f,h->x); put32(f,h->y);
    failed = ferror(f); if (fclose(f)) failed = 1;
    return failed ? -1 : 0;
}

int hero_load(Hero *h, const char *name)
{
    Hero v = {0}; char path[1024], magic[4]; FILE *f; int i, failed;
    if (save_path(path,sizeof(path),name,0)) return -1;
    f=plat_fopen(path,"rb"); if (!f) return -1;
    if (fread(magic,1,4,f)!=4 || memcmp(magic,"WSH3",4)) { fclose(f); return -1; }
    if (fread(v.name,1,sizeof(v.name),f)!=sizeof(v.name) ||
        fread(v.skin,1,sizeof(v.skin),f)!=sizeof(v.skin)) { fclose(f); return -1; }
    v.gender=get32(f); v.klass=get32(f); v.level=get32(f);
    v.xp=get64(f); v.gold=get64(f);
    v.hp=get32(f); v.max_hp=get32(f); v.mp=get32(f); v.max_mp=get32(f);
    for(i=0;i<HERO_ABILITIES;++i) v.ability[i]=get32(f);
    v.right_hand=get32(f);
    for(i=0;i<8;++i) v.equip[i]=get32(f);
    for(i=0;i<8;++i) v.hand_pp[i]=get32(f);
    for(i=0;i<8;++i) v.element_pp[i]=get32(f);
    v.pp=get64(f); v.ailments=(uint32_t)get32(f);
    for(i=0;i<HERO_INVENTORY;++i) { v.inventory[i].item_id=get32(f); v.inventory[i].count=get32(f); }
    if(fread(v.tokens,1,HERO_TOKENS,f)!=HERO_TOKENS) { fclose(f); return -1; }
    if(fread(v.learned_spells,1,sizeof(v.learned_spells),f)!=sizeof(v.learned_spells)) { fclose(f); return -1; }
    v.map=get32(f); v.link=get32(f); v.x=get32(f); v.y=get32(f);
    failed=ferror(f)||feof(f); fclose(f);
    if(failed || !memchr(v.name,0,sizeof(v.name)) || !memchr(v.skin,0,sizeof(v.skin)) ||
       !safe_name(v.name) || text_casecmp(v.name,name) || !hero_class(&v) || !hero_class(&v)->used ||
       v.gender<0 || v.gender>3 || v.level<1 || v.level>100 || v.xp<0 || v.gold<0 || v.pp<0 ||
       v.max_hp<1 || v.hp<0 || v.hp>v.max_hp || v.max_mp<0 || v.mp<0 || v.mp>v.max_mp ||
       v.map<0 || v.map>=WORLD_MAX_MAPS || v.link<0 || v.link>=OBL_RECORDS ||
       v.right_hand<0 || v.right_hand>=WORLD_MAX_ITEMS) return -1;
    for(i=0;i<HERO_ABILITIES;++i) if(v.ability[i]<0 || v.ability[i]>255) return -1;
    for(i=0;i<8;++i) if(v.equip[i]<0 || v.equip[i]>=WORLD_MAX_ITEMS) return -1;
    for(i=0;i<8;++i) if(v.hand_pp[i]<0 || v.hand_pp[i]>5000000) return -1;
    for(i=0;i<8;++i) if(v.element_pp[i]<0 || v.element_pp[i]>5000000) return -1;
    for(i=0;i<HERO_INVENTORY;++i) {
        const HeroItem *item=&v.inventory[i];
        if(item->count<0 || item->count>(i+1>=1024?1:100) ||
           item->item_id!=(item->count?i+1:0)) return -1;
    }
    for(i=0;i<WORLD_MAX_SPELLS;++i) if(v.learned_spells[i]>1) return -1;
    if(v.ailments & ~UINT32_C(0x01fffffc)) return -1;
    v.valid=1;
    for(i=HERO_SLOT_HELMET;i<=HERO_SLOT_RIGHT_HAND;++i) {
        int id=hero_equipped(&v,i);
        if(id && (hero_item_slot(id)!=i || !hero_item_count(&v,id))) return -1;
    }
    *h=v; scene_reset_timers(); return 0;
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
    const ClassDef *c=hero_class(h); int gained=0;
    if(!c || !h->valid) return 0;
    if(xp>0) h->xp=xp>INT64_MAX-h->xp ? INT64_MAX : h->xp+xp;
    while(h->level<100 && h->xp>=hero_xp_for_level(h,h->level+1)) {
        int old_hp=h->max_hp, old_mp=h->max_mp;
        ++h->level; ++gained; maxima(c,h->level,&h->max_hp,&h->max_mp);
        h->hp=bounded_stat((int64_t)h->hp+h->max_hp-old_hp);
        h->mp=bounded_stat((int64_t)h->mp+h->max_mp-old_mp);
        wos_log_event("level_up","level=%d",h->level);
    }
    if(gold>0) hero_add_gold(h,gold);
    return gained;
}

static const ItemDef *hero_item(int id)
{
    return id > 0 && id < WORLD_MAX_ITEMS && g_world.items[id].used ? &g_world.items[id] : NULL;
}

int hero_item_count(const Hero *h, int id)
{
    if (!h || !hero_item(id)) return 0;
    return h->inventory[id - 1].count;
}

int hero_item_limit(const Hero *h, int id)
{
    const ItemDef *item = hero_item(id);
    int limit;
    if (!h || !h->valid || !item) return 0;
    /* FUN_00403349: the "wallet size" in items.txt is level+1, not current GP. */
    limit = id >= 1024 ? 1 : h->level + 1;
    if (limit > 100) limit = 100;
    if (id < 1024 && item->max_count > 0 && item->max_count < limit) limit = item->max_count;
    /* FUN_00484E72: zero-price quest objects never accumulate multiple copies. */
    if (!item->gp && limit > 1) limit = 1;
    return limit;
}

int hero_give_item(Hero *h, int id, int count)
{
    int have = hero_item_count(h,id), limit = hero_item_limit(h,id);
    if (!h || count <= 0 || count > limit - have) return 0;
    h->inventory[id - 1].item_id = id;
    h->inventory[id - 1].count += count;
    return 1;
}

int hero_take_item(Hero *h, int id, int count)
{
    int slot;
    if (!h || count <= 0 || hero_item_count(h,id) < count) return 0;
    h->inventory[id - 1].count -= count;
    if (!h->inventory[id - 1].count) {
        h->inventory[id - 1].item_id = 0;
        slot = hero_item_slot(id);
        if (slot >= 0 && hero_equipped(h,slot) == id) hero_unequip(h,slot);
    }
    return 1;
}

int64_t hero_wallet_limit(const Hero *h)
{
    const ClassDef *c = hero_class(h);
    int64_t cap;
    if (!c || !h->valid) return 0;
    /* FUN_0042BAA8, then the class's optional MAX_WALLET. */
    cap = ((int64_t)h->level + 1) * 10000;
    if (cap > 1000000) cap = 1000000;
    if (c->max_wallet >= 0 && cap > c->max_wallet) cap = c->max_wallet;
    return cap < 0 ? 0 : cap;
}

static int64_t add_wallet(int64_t *wallet, int64_t amount, int64_t limit)
{
    int64_t old = *wallet;
    if (amount > 0 && amount > limit - old) *wallet = limit;
    else if (amount < 0 && amount < -old) *wallet = 0;
    else *wallet += amount;
    return *wallet - old;
}

int64_t hero_add_gold(Hero *h, int64_t amount)
{
    if (!h || !h->valid) return 0;
    return add_wallet(&h->gold,amount,hero_wallet_limit(h));
}

int hero_item_slot(int id)
{
    const ItemDef *item = hero_item(id);
    if (!item) return -1;
    if (item->klass == 10) return HERO_SLOT_HELMET;
    if (item->klass == 11) return HERO_SLOT_ARMOR;
    if (item->klass >= 12 && item->klass <= 19) return HERO_SLOT_RIGHT_HAND;
    if (item->klass >= 20 && item->klass <= 23) return HERO_SLOT_BOOTS + item->klass - 20;
    return -1;
}

int hero_equipped(const Hero *h, int slot)
{
    if (!h) return 0;
    if (slot == HERO_SLOT_RIGHT_HAND) return h->right_hand;
    return slot >= HERO_SLOT_HELMET && slot <= HERO_SLOT_AMULET ? h->equip[slot] : 0;
}

static int item_requirements(const Hero *h, const ItemDef *item)
{
    if (!h || !h->valid || !item || h->level < item->level) return 0;
    return !item->equip_token ||
        (item->equip_token > 0 && item->equip_token < HERO_TOKENS && h->tokens[item->equip_token]);
}

int hero_can_equip(const Hero *h, int id)
{
    const ItemDef *item = hero_item(id);
    const ClassDef *c = hero_class(h);
    int hand;
    if (!c || hero_item_slot(id) < 0 || !item_requirements(h,item) || !hero_item_count(h,id)) return 0;
    hand = item->klass - 12;
    /* FUN_0040D6B4: preferred hand, or level-five proficiency in another hand. */
    if (hand >= 0 && hand < 8 && c->right_hand != hand + 1 && hero_pp_level(h->hand_pp[hand]) < 5)
        return 0;
    return 1;
}

int hero_equip(Hero *h, int id)
{
    int slot = hero_item_slot(id);
    if (!hero_can_equip(h,id) || hero_equipped(h,slot) == id) return 0;
    if (slot == HERO_SLOT_RIGHT_HAND) h->right_hand = id;
    else h->equip[slot] = id;
    wos_log_event("equip","slot=%d item=%d",slot,id);
    return 1;
}

int hero_unequip(Hero *h, int slot)
{
    int id = hero_equipped(h,slot);
    if (!h || !h->valid || !id) return 0;
    if (slot == HERO_SLOT_RIGHT_HAND) h->right_hand = 0;
    else h->equip[slot] = 0;
    wos_log_event("unequip","slot=%d item=%d",slot,id);
    return 1;
}

int hero_use_item(Hero *h, int id, int ability)
{
    const ItemDef *item = hero_item(id);
    const ClassDef *c = hero_class(h);
    int hp, mp, points, cap;
    if (!c || !item_requirements(h,item) || !hero_item_count(h,id)) return 0;
    if (item->klass == 0) {
        hp = item->hp; mp = item->mp;
        /* FUN_004A6353: poisoning reduces positive potion healing to one HP. */
        if (hp > 0 && (h->ailments & (1u << 2))) hp = 1;
        if (h->hp > 0 || hp == 1) {
            h->hp = bounded_stat((int64_t)h->hp + hp);
            if (h->hp > h->max_hp) h->hp = h->max_hp;
        }
        h->mp = bounded_stat((int64_t)h->mp + mp);
        if (h->mp > h->max_mp) h->mp = h->max_mp;
    } else if (item->klass == 1) {
        int disease = item->ability_points;
        if (disease > -2 || disease < -24) return 0;
        h->ailments &= ~(UINT32_C(1) << -disease);
    } else if (item->klass >= 100 && item->klass <= 105) {
        if (item->klass != 105) ability = item->klass - 100;
        if (ability < 0 || ability >= HERO_ABILITIES) return 0;
        cap = c->max_ability[ability];
        if (cap > 255) cap = 255;
        points = item->ability_points;
        if (points > cap - h->ability[ability]) points = cap - h->ability[ability];
        if (points > 0) h->ability[ability] += points;
    } else return 0;
    hero_take_item(h,id,1);
    wos_log_event("item_use","item=%d hp=%d mp=%d",id,h->hp,h->mp);
    return 1;
}

static int equipment_stat(const Hero *h, int offense)
{
    int slot;
    int64_t value = 0;
    if (!h || !h->valid) return 0;
    /* FUN_0040C694 sums every equipped item's signed attack/defense, clamps
     * once at the end. Cursed items can cancel positive equipment bonuses. */
    for (slot = HERO_SLOT_HELMET; slot <= HERO_SLOT_RIGHT_HAND; ++slot) {
        int id = hero_equipped(h,slot);
        const ItemDef *item = hero_item(id);
        if (item && hero_can_equip(h,id)) value += offense ? item->attack : item->defense;
    }
    return value > 32767 ? 32767 : value > 0 ? (int)value : 0;
}

int hero_offense(const Hero *h) { return equipment_stat(h,1); }
int hero_defense(const Hero *h) { return equipment_stat(h,0); }
int hero_ability(const Hero *h, int ability)
{
    const ClassDef *c = hero_class(h);
    int value, cap;
    if (!c || ability < 0 || ability >= HERO_ABILITIES) return 0;
    /* FUN_00449006: equipment changes attack/defense, not ability points. */
    value = h->ability[ability]; cap = c->max_ability[ability];
    if (cap > 255) cap = 255;
    if (value > cap) value = cap;
    return value < 0 ? 0 : value;
}

int hero_pp_level(int pp)
{
    /* levels.txt PP Levels, L0..L9; integer boundaries are part of the API. */
    static const int threshold[10] = {0,555,1250,2142,3333,5000,7500,11666,20000,45000};
    int level = 9;
    while (level > 0 && pp < threshold[level]) --level;
    return level;
}

int hero_train_limit(const Hero *h, int kind, int index)
{
    const ClassDef *c = hero_class(h);
    int cap;
    if (!c || index < 0 || index >= 8 || (kind != HERO_TRAIN_HAND && kind != HERO_TRAIN_ELEMENT)) return 0;
    cap = kind == HERO_TRAIN_HAND ? c->max_hand_pp[index] : c->max_element_pp[index];
    return cap < 0 ? 0 : cap > 5000000 ? 5000000 : cap;
}

int64_t hero_add_pp(Hero *h, int64_t amount)
{
    if (!h || !h->valid) return 0;
    return add_wallet(&h->pp,amount,g_world.max_unspent_pp);
}

void hero_gain_training(Hero *h, int kind, int index, int amount)
{
    int cap = hero_train_limit(h,kind,index), *pp, i;
    if (!h || !h->valid || amount <= 0 || amount > 2000 || cap <= 0) return;
    pp = kind == HERO_TRAIN_HAND ? &h->hand_pp[index] : &h->element_pp[index];
    if (amount > cap - *pp) amount = cap - *pp;
    if (amount <= 0) return;
    /* FUN_00419306 / FUN_004258B8; combat training does not debit the wallet. */
    *pp += amount;
    if (kind == HERO_TRAIN_ELEMENT) {
        h->element_pp[(index+4)&7] -= amount*15/100;
        h->element_pp[(index+3)&7] -= amount*5/100;
        h->element_pp[(index+5)&7] -= amount*5/100;
        h->element_pp[(index+2)&7] -= amount/100;
        h->element_pp[(index+6)&7] -= amount/100;
        for (i=0;i<8;++i) if (h->element_pp[i]<20) h->element_pp[i]=20;
    }
}

int hero_train(Hero *h, int kind, int index, int amount)
{
    int cap = hero_train_limit(h,kind,index), *pp;
    if (!h || !h->valid || amount <= 0 || amount > 2000 || cap < amount || h->pp < amount) return 0;
    pp = kind == HERO_TRAIN_HAND ? &h->hand_pp[index] : &h->element_pp[index];
    if (*pp > cap - amount) return 0;
    /* 0x419107/0x425C30: wallet purchase uses the same update as combat. */
    h->pp -= amount;
    hero_gain_training(h,kind,index,amount);
    wos_log_event("train","kind=%s index=%d pp=%d level=%d",
        kind == HERO_TRAIN_HAND ? "hand" : "element",index,*pp,hero_pp_level(*pp));
    return 1;
}

static int spell_requirements(const Hero *h, int id)
{
    const SpellDef *spell;
    if (!h || !h->valid || id <= 0 || id >= WORLD_MAX_SPELLS) return 0;
    spell = &g_world.spells[id];
    if (!spell->used || spell->element < 0 || spell->element >= 8 || h->level < spell->min_level) return 0;
    if (spell->token && (spell->token < 0 || spell->token >= HERO_TOKENS || !h->tokens[spell->token])) return 0;
    return hero_pp_level(h->element_pp[spell->element]) >= spell->req_affinity;
}

int hero_spell_known(const Hero *h, int id)
{
    return spell_requirements(h,id) && h->learned_spells[id];
}

int hero_can_learn_spell(const Hero *h, int id)
{
    if (!spell_requirements(h,id) || h->learned_spells[id]) return 0;
    return !(g_world.spells[id].flags & 2) && g_world.spells[id].pp_cost >= 0 &&
        h->pp >= g_world.spells[id].pp_cost;
}

int hero_learn_spell(Hero *h, int id)
{
    /* FUN_0044E3BD: eligibility first, debit cost once, then set learned bit. */
    if (!hero_can_learn_spell(h,id)) return 0;
    h->pp -= g_world.spells[id].pp_cost;
    h->learned_spells[id] = 1;
    wos_log_event("spell_learn","spell=%d pp=%lld",id,(long long)h->pp);
    return 1;
}

int hero_set_spell(Hero *h, int id, int known)
{
    if (!h || !h->valid || id <= 0 || id >= WORLD_MAX_SPELLS || !g_world.spells[id].used) return 0;
    h->learned_spells[id] = known != 0;
    return 1;
}
