/* Item use, the trophy bag and the pet pen. See items.h for the VA table. */
#include "items.h"
#include "game.h"
#include "scene.h"
#include "html.h"
#include "../engine/log.h"
#include "../engine/rng.h"
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ state */

static char last_message[192];
static int  attr_pool;                 /* FUN_004A6353 case 0x69 -> FUN_00449DF2 */
static int  throw_item_id;              /* FUN_004A4D70, class 5 */
static Pet  pen[PET_PEN_SLOTS];         /* FUN_0040FBCC / FUN_0040FBFD */

static const char *const ability_names[HERO_ABILITIES] = {
    "Strength", "Wisdom", "Stamina", "Agility", "Dexterity"
};

/* FUN_004823C4 seeds DAT_007CDE30 (stride 0x28, ids 0..13) and the +EQUIP
 * section overwrites 0..13 (FUN_004824B4). "Ring" is the literal at 0x4FD304. */
static const char *const equip_defaults[EQUIP_SLOT_COUNT] = {
    "Helmet", "Armor", "Boots", "Shield", "Ring", "Amulet"
};
static char equip_names[EQUIP_SLOT_COUNT][40];

const char *world_equip_slot_name(int slot)
{
    if (slot < 0 || slot >= EQUIP_SLOT_COUNT) return "";
    /* FUN_00482431 falls back to the literal when the +EQUIP row is absent. */
    return equip_names[slot][0] ? equip_names[slot] : equip_defaults[slot];
}

void world_equip_slot_set_name(int slot, const char *name)
{
    if (slot < 0 || slot >= EQUIP_SLOT_COUNT || !name) return;
    snprintf(equip_names[slot], sizeof equip_names[slot], "%.39s", name);
}

const char *items_class_slot_name(int item_class)
{
    if (item_class == ITEM_HELMET) return world_equip_slot_name(EQUIP_SLOT_HELMET);
    if (item_class == ITEM_ARMOR) return world_equip_slot_name(EQUIP_SLOT_ARMOR);
    if (item_class == ITEM_BOOTS) return world_equip_slot_name(EQUIP_SLOT_BOOTS);
    if (item_class == ITEM_SHIELD) return world_equip_slot_name(EQUIP_SLOT_SHIELD);
    if (item_class == ITEM_RING) return world_equip_slot_name(EQUIP_SLOT_RING);
    if (item_class == ITEM_AMULET) return world_equip_slot_name(EQUIP_SLOT_AMULET);
    return NULL;
}


const char *items_last_message(void) { return last_message; }
void items_clear_message(void) { last_message[0] = '\0'; }
static void say(const char *text) { snprintf(last_message, sizeof last_message, "%s", text); }

/* ------------------------------------------------------------ item apply */

/* FUN_004A6A6A reads a per-disease charge counter at hero+0x398+d*4. The port's
 * Hero carries one `ailments` bitmask, so "diseased" is exactly that counter
 * being non-zero. */
static int diseased(int d)
{
    return d >= 0 && d < 32 && (g_hero.ailments & (UINT32_C(1) << d)) != 0;
}

/* formats_online.md 6.2: map flag 4096 (NO_HEAL) makes every heal worth 1 HP.
 * FUN_004A6353 reads it through FUN_00482BB8(0x1000). */
static int no_heal(void)
{
    return g_hero.map >= 0 && g_hero.map < WORLD_MAX_MAPS &&
           (g_world.maps[g_hero.map].flags & 0x1000u) != 0;
}

/* FUN_004A6B55: cure one disease. 2..8 clear the ailment outright; 11 and 20+
 * burn one charge off the counter; 9, 10 and 12..19 return before touching
 * anything. Returns 1 when the hero's disease state changed. */
static int cure_disease(int disease)
{
    int d = disease < 0 ? -disease : disease;
    if (d < 2 || d > 24 || !diseased(d)) return 0;
    if (d == 9 || d == 10 || (d > 11 && d < 20)) return 0;
    g_hero.ailments &= ~(UINT32_C(1) << d);
    return 1;
}

int items_usable(int item_id)
{
    const ItemDef *item;
    if (item_id <= 0 || item_id >= WORLD_MAX_ITEMS) return 0;
    item = &g_world.items[item_id];
    if (!item->used || !g_hero.valid) return 0;
    /* FUN_0040D6B4 with param_4 == 1: minimum level, then the equip token. */
    if (g_hero.level < item->level) return 0;
    if (item->equip_token > 0 &&
        (item->equip_token >= HERO_TOKENS || !g_hero.tokens[item->equip_token])) return 0;
    return 1;
}

int items_attr_pool(void) { return attr_pool; }

int items_attr_assign(int ability)
{
    const ClassDef *c = &g_world.classes[g_hero.klass];
    int cap, give;
    if (ability < 0 || ability >= HERO_ABILITIES || attr_pool <= 0) return 0;
    cap = c->max_ability[ability];
    if (cap > 255) cap = 255;
    if (g_hero.ability[ability] >= cap) return 0;
    give = cap - g_hero.ability[ability];
    if (give > attr_pool) give = attr_pool;
    g_hero.ability[ability] += give;
    attr_pool -= give;
    wos_log_event("attr_assign","ability=%s points=%d",ability_names[ability],give);
    return 1;
}

/* FUN_0047A871 with the ticket's five dotted arguments (items.txt arg2.1..5):
 * mode 0 = go to `link` on map `num` (drop_in 1 enters the link's scene),
 * mode 1 = "drop into scene num", which is not really travelling. */
static int travel(const ItemDef *item)
{
    if (item->travel_scene != 0 && game_current_scene() != item->travel_scene) {
        wos_log_event("travel_failed","reason=scene scene=%d need=%d",
                      game_current_scene(), item->travel_scene);
        say("Travel Failed.");
        return 0;
    }
    if (item->travel_mode == 0) game_enter_map(item->travel_map_scene, item->travel_link,
                                              item->travel_drop_in);
    else game_enter_scene(item->travel_map_scene, NULL);
    say("Travel.");
    return 1;
}

int items_apply(int item_id, int ability)
{
    const ItemDef *item;
    const ClassDef *c;
    char text[192];
    int klass, amount, index, give, hp, mp, before, after;
    if (item_id <= 0 || item_id >= WORLD_MAX_ITEMS) return 0;
    item = &g_world.items[item_id];
    if (!item->used) return 0;
    c = &g_world.classes[g_hero.klass];
    klass = item->klass;
    amount = item->ability_points;        /* arg12, record +0xB8 */
    items_clear_message();

    if (klass >= ITEM_ATTR_STRENGTH && klass <= ITEM_ATTR_DEXTERITY) {
        index = klass - ITEM_ATTR_STRENGTH;
        give = amount;
        if (give > c->max_ability[index] - g_hero.ability[index])
            give = c->max_ability[index] - g_hero.ability[index];
        if (give < 1) give = 0;
        g_hero.ability[index] += give;
        if (give != 0) {
            /* FUN_004A6353 calls FUN_00449AA2 after the seed: max HP/MP follow
             * the new ability values. */
            hero_maxima(&g_hero, &g_hero.max_hp, &g_hero.max_mp);
        }
        snprintf(text, sizeof text, "%s %d %s", item->name, give, ability_names[index]);
        say(text);
        wos_log_event("item_attr","item=%d ability=%s points=%d",item_id,ability_names[index],give);
        return 1;
    }
    if (klass == ITEM_ATTR_ALL) {
        /* FUN_004A6353 case 0x69: the points go into the pool and the
         * assignment dialog spends them. */
        if (amount < 1) return 0;
        if (ability >= 0 && ability < HERO_ABILITIES) {
            attr_pool += amount;
            return items_attr_assign(ability);
        }
        attr_pool += amount;
        snprintf(text, sizeof text, "%s %d Ability", item->name, amount);
        say(text);
        wos_log_event("item_attr_pool","item=%d points=%d",item_id,attr_pool);
        return 1;
    }
    if (klass == ITEM_POTION) {
        hp = item->hp;                     /* arg10, record +0xA8 */
        mp = item->mp;                     /* arg11, record +0xAC */
        /* FUN_004A6353: under NO_HEAL a positive potion heals exactly one HP. */
        if (hp > 0 && no_heal()) hp = 1;
        before = g_hero.hp;
        g_hero.hp += hp;
        if (g_hero.hp > g_hero.max_hp) g_hero.hp = g_hero.max_hp;
        if (g_hero.hp < 0) g_hero.hp = 0;
        after = g_hero.hp;
        if (mp != 0) {
            g_hero.mp += mp;
            if (g_hero.mp > g_hero.max_mp) g_hero.mp = g_hero.max_mp;
            if (g_hero.mp < 0) g_hero.mp = 0;
            snprintf(text, sizeof text, "%s %d MP", mp > 0 ? "+" : "-", mp > 0 ? mp : -mp);
            say(text);
        } else {
            snprintf(text, sizeof text, "%s %d", item->name, after - before);
            say(text);
        }
        wos_log_event("item_hp","item=%d hp=%d delta=%d",item_id,after,after-before);
        return 1;
    }
    if (klass == ITEM_ANTIDOTE) {
        if (!cure_disease(amount)) return 0;
        snprintf(text, sizeof text, "Antidote %d", amount);
        say(text);
        wos_log_event("item_cure","item=%d disease=%d",item_id,amount);
        return 1;
    }
    if (klass == ITEM_TRAVEL) return travel(item);
    if (klass == ITEM_HTML) {
        /* FUN_004A6353 case 0xC9 -> FUN_0048A69E(item + 0x1C5), the arg15 URL.
         * Only the host may use it (hero+4 == the host serial). */
        if (!item->sound[0]) return 0;
        if (html_open(item->sound)) wos_log_event("item_html","item=%d url=%s",item_id,item->sound);
        else wos_log_event("item_html_missing","item=%d url=%s",item_id,item->sound);
        return 1;
    }
    /* 2 SPECIAL and 3 EXIT are "DO NOT USE YET" in the shipped items.txt
     * header and FUN_004A6353 has no branch for either; 5 THROWABLE and
     * 200 PET are not applied here. */
    return 0;
}

int items_use(int item_id, int ability)
{
    const ItemDef *item;
    if (item_id <= 0 || item_id >= WORLD_MAX_ITEMS) return 0;
    item = &g_world.items[item_id];
    if (!item->used || !hero_item_count(&g_hero,item_id) || !items_usable(item_id)) return 0;
    /* FUN_004A4D70: a class-5 throwable is armed for the battle module, and
     * the battle code spends it when the throw resolves. */
    if (item->klass == ITEM_THROWABLE) return items_arm_throw(item_id);
    if (!items_apply(item_id, ability)) return 0;
    /* FUN_00449006(hero, 3): the use always spends one copy. */
    hero_take_item(&g_hero, item_id, 1);
    wos_log_event("item_use","item=%d klass=%d hp=%d mp=%d",
                  item_id,item->klass,g_hero.hp,g_hero.mp);
    return 1;
}

/* ------------------------------------------------------------- class 5 */

/* FUN_004A4D70 arms the throw. arg16 carries the attack-path grammar
 * (1 lunge, 2 jump, 3 leap, 4 stab, 5 dbl-stab, 6 trample, 7 hop, 30 arrow,
 * 31 stone, 32 lob, 33 swoop, -1 random at world load, 0 none). */
int items_arm_throw(int item_id)
{
    if (item_id <= 0 || item_id >= WORLD_MAX_ITEMS) return 0;
    if (g_world.items[item_id].klass != ITEM_THROWABLE) return 0;
    throw_item_id = item_id;
    wos_log_event("item_throw_armed","item=%d path=%d image=%d",
                  item_id,g_world.items[item_id].attack_path,g_world.items[item_id].attack_image);
    return 1;
}

int items_throw_armed(void) { return throw_item_id; }
int items_throw_item(void) { return throw_item_id; }

/* FUN_0048F667: the battle module calls this once the throw has resolved. */
int items_spend_throw(void)
{
    int id = throw_item_id;
    if (!id) return 0;
    throw_item_id = 0;
    hero_take_item(&g_hero, id, 1);
    wos_log_event("item_thrown","item=%d",id);
    return 1;
}

void items_clear_throw(void) { throw_item_id = 0; }

/* ------------------------------------------------------------ trophy bag */

static uint32_t bag_word(int trophy_id, int count)
{
    if (trophy_id <= 0 || count <= 0) return 0;
    return (((uint32_t)trophy_id & 0xffffu) << 16 | ((uint32_t)count & 0xffu) << 8) ^ TROPHY_BAG_KEY;
}

/* FUN_0046F60F: the geometry word at hero+0xEE0. Returns the cell count. */
int trophy_bag_size(int *width, int *height)
{
    uint32_t v = g_hero.trophy_bag_geo ^ TROPHY_BAG_KEY;
    int w = (int)(v >> 16), h = (int)(v & 0xffffu);
    if (width) *width = w;
    if (height) *height = h;
    return w * h;
}

/* FUN_0046FED9: width and height clamp to 1..16 and the product to 128.
 * Shrinking packs first and then clears everything past the new size. */
int trophy_bag_resize(int width, int height)
{
    int cells, i;
    if (width > 16) width = 16;
    if (width < 1) width = 0;
    if (height > 16) height = 16;
    if (height < 1) height = 0;
    cells = width * height;
    if (cells > TROPHY_BAG_SLOTS) return 0;
    if (cells < trophy_bag_size(NULL, NULL)) trophy_bag_pack();
    g_hero.trophy_bag_geo = ((uint32_t)width << 16 | (uint32_t)height) ^ TROPHY_BAG_KEY;
    for (i = cells; i < TROPHY_BAG_SLOTS; ++i) g_hero.trophy_bag[i] = 0;
    wos_log_event("trophy_bag_resize","width=%d height=%d",width,height);
    return 1;
}

/* FUN_0046F726: slot 0..0x7F, the stored word is (count<<8|id<<16) ^ K. */
int trophy_bag_get(int slot, int *trophy_id, int *count)
{
    uint32_t w, v;
    if (slot < 0 || slot >= TROPHY_BAG_SLOTS) return 0;
    w = g_hero.trophy_bag[slot];
    if (w == 0) return 0;
    v = w ^ TROPHY_BAG_KEY;
    if (trophy_id) *trophy_id = (int)(v >> 16);
    if (count) *count = (int)((v >> 8) & 0xffu);
    return 1;
}

/* FUN_0046F779: the count is clamped to the trophy's stack size (trophies.txt
 * arg4, record +0x4C) and anything below one empties the slot. */
int trophy_bag_set(int slot, int trophy_id, int count)
{
    int limit;
    if (slot < 0 || slot >= TROPHY_BAG_SLOTS) return 0;
    if (trophy_id < 0 || trophy_id >= WORLD_MAX_TROPHIES) return 0;
    limit = trophy_bag_limit(trophy_id);
    if (count > limit) count = limit;
    if (count < 1) { count = 0; trophy_id = 0; }
    g_hero.trophy_bag[slot] = bag_word(trophy_id, count);
    return 1;
}

void trophy_bag_clear(void)
{
    int i;
    for (i = 0; i < TROPHY_BAG_SLOTS; ++i) g_hero.trophy_bag[i] = 0;
}

/* FUN_00470537: 40 px cells, row major. */
int trophy_bag_slot_at(int x, int y)
{
    int width, height, cx, cy;
    trophy_bag_size(&width, &height);
    if (width <= 0 || height <= 0) return -1;
    cx = x / TROPHY_BAG_CELL; cy = y / TROPHY_BAG_CELL;
    if (cx < 0 || cx >= width || cy < 0 || cy >= height) return -1;
    return width * cy + cx;
}

/* FUN_0046FE3A: raw slot copy, then clear the source. */
void trophy_bag_move(int from, int to)
{
    if (from < 0 || from >= TROPHY_BAG_SLOTS || to < 0 || to >= TROPHY_BAG_SLOTS) return;
    g_hero.trophy_bag[to] = g_hero.trophy_bag[from];
    g_hero.trophy_bag[from] = 0;
}

/* FUN_0046FE7B: close the gaps a shrink leaves behind. */
void trophy_bag_pack(void)
{
    int total = trophy_bag_size(NULL, NULL), i, j;
    if (total < 1) return;
    for (i = 0; i < total; ++i) {
        if (g_hero.trophy_bag[i]) continue;
        for (j = i + 1; j < total; ++j)
            if (g_hero.trophy_bag[j]) break;
        if (j >= total) return;
        trophy_bag_move(j, i);
    }
}

/* FUN_0046FAC5: the trophy's stack size, trophies.txt arg4. */
int trophy_bag_limit(int trophy_id)
{
    if (trophy_id <= 0 || trophy_id >= WORLD_MAX_TROPHIES) return 0;
    return g_world.trophies[trophy_id].stack_size;
}

int trophy_bag_count(int trophy_id)
{
    int i, id, count, total = 0;
    for (i = 0; i < TROPHY_BAG_SLOTS; ++i)
        if (trophy_bag_get(i,&id,&count) && id == trophy_id) total += count;
    return total;
}

int trophy_bag_have(int trophy_id, int count) { return trophy_bag_count(trophy_id) >= count; }

/* FUN_0046F8F7: top the first stack of this trophy that is not at its limit,
 * else the first free slot. */
int trophy_bag_add(int trophy_id)
{
    int limit = trophy_bag_limit(trophy_id), i, id, count, free_slot = -1;
    if (limit < 1) return 0;
    for (i = 0; i < TROPHY_BAG_SLOTS; ++i) {
        if (!trophy_bag_get(i,&id,&count)) { if (free_slot < 0) free_slot = i; continue; }
        if (id == trophy_id && count < limit) { trophy_bag_set(i,id,count+1); return 1; }
    }
    if (free_slot < 0) return 0;
    return trophy_bag_set(free_slot,trophy_id,1);
}

/* FUN_0046F95F: drop one from the first stack holding it. */
int trophy_bag_remove(int trophy_id)
{
    int i, id, count;
    for (i = 0; i < TROPHY_BAG_SLOTS; ++i)
        if (trophy_bag_get(i,&id,&count) && id == trophy_id && count > 0) {
            trophy_bag_set(i,id,count-1);
            return 1;
        }
    return 0;
}

/* FUN_0046F819: remove `count`, but only from a slot that really holds them. */
int trophy_bag_take(int trophy_id, int count)
{
    int i, id, have;
    if (count < 1) return 0;
    for (i = 0; i < TROPHY_BAG_SLOTS; ++i)
        if (trophy_bag_get(i,&id,&have) && id == trophy_id) {
            if (have < count) return 0;
            trophy_bag_set(i,id,have-count);
            return 1;
        }
    return 0;
}

int trophy_bag_used(void)   /* FUN_0046FA49 */
{
    int i, id, count, total = 0;
    for (i = 0; i < TROPHY_BAG_SLOTS; ++i)
        if (trophy_bag_get(i,&id,&count) && id != 0 && count != 0) ++total;
    return total;
}

int trophy_bag_free(void)   /* FUN_0046FA87 */
{
    int i, id, count, total = 0, cells = trophy_bag_size(NULL, NULL);
    if (cells < 0) cells = 0;
    for (i = 0; i < cells; ++i)
        if (!trophy_bag_get(i,&id,&count) || id == 0 || count == 0) ++total;
    return total;
}

/* FUN_0046FAE9: how many more of `trophy_id` the bag can still take. */
int trophy_bag_room(int trophy_id)
{
    int limit = trophy_bag_limit(trophy_id), i, id, count, room = 0;
    if (limit < 1) return 0;
    for (i = 0; i < TROPHY_BAG_SLOTS; ++i) {
        if (!trophy_bag_get(i,&id,&count) || id == 0 || count == 0) room += limit;
        else if (id == trophy_id) room -= count;
    }
    return room < 0 ? 0 : room;
}

/* FUN_0046FB5E: trophies.txt arg6 is a dotted list of up to ten monster ids or
 * `a-b` ranges; the row belongs to the kill when the id is in one of them. */
static int demon_matches(const TrophyDef *t, int monster_id)
{
    int i, first, last;
    const char *s = t->image;             /* placeholder, replaced below */
    (void)s;
    for (i = 0; i < t->monster_range_count; ++i) {
        first = t->monster_first[i];
        last = t->monster_last[i];
        if (last < first) last = first;
        if (monster_id >= first && monster_id <= last) return 1;
    }
    return 0;
}

/* FUN_0046FBF1: walk the whole trophies table and roll every eligible row once.
 * The single rand() at 0x46FBF1 is mirrored so the roll follows the original's
 * stream. trophies.txt arg9 bit 3 is the online "cannot be traded away" flag
 * and is not checked in solo play. */
int trophy_bag_roll(int monster_id, int *out, int max)
{
    int found = 0, id;
    if (!out || max < 1) return 0;
    for (id = 0; id < WORLD_MAX_TROPHIES; ++id) {
        const TrophyDef *t = &g_world.trophies[id];
        if (!t->used || t->probability == 0) continue;
        if (t->token != 0 && (t->token < 0 || t->token >= HERO_TOKENS || !g_hero.tokens[t->token]))
            continue;
        if (!demon_matches(t, monster_id)) continue;
        if ((int)(crt_rand() % 100) <= t->probability) {
            out[found++] = id;
            if (found >= max) break;
        }
    }
    return found;
}

/* FUN_0046FCC4: roll, then add each result, reporting the ones that did not fit. */
int trophy_bag_award_kill(int monster_id)
{
    int rolled[20], count, i, kept = 0;
    count = trophy_bag_roll(monster_id, rolled, 20);
    for (i = 0; i < count; ++i) {
        if (trophy_bag_add(rolled[i])) {
            ++kept;
            wos_log_event("trophy_found","trophy=%d name=%s",rolled[i],g_world.trophies[rolled[i]].name);
        } else {
            wos_log_event("trophy_bag_full","trophy=%d name=%s",rolled[i],g_world.trophies[rolled[i]].name);
        }
    }
    return kept;
}

/* ----------------------------------------------------------------- pets */

int pet_count(void)
{
    int i, n = 0;
    for (i = 0; i < PET_PEN_SLOTS; ++i) if (pen[i].used) ++n;
    return n;
}

Pet *pet_at(int index)
{
    if (index < 0 || index >= PET_PEN_SLOTS || !pen[index].used) return NULL;
    return &pen[index];
}

void pet_reset(void) { memset(pen, 0, sizeof pen); }

/* FUN_00413181: a class-200 item is a pet voucher whose arg3 is a MONSTER id,
 * not an image index. The original rolls ten names (FUN_0049B70F) and five
 * trait values (FUN_0049B71B); each is one crt_rand() in the same order. */
int pet_spawn(int monster_id)
{
    int i, k;
    if (monster_id <= 0 || monster_id >= WORLD_MAX_MONSTERS) return 0;
    if (!g_world.monsters[monster_id].used) return 0;
    for (i = 0; i < PET_PEN_SLOTS; ++i) if (!pen[i].used) break;
    if (i == PET_PEN_SLOTS) {
        wos_log_event("pet_rejected","reason=pen_full monster=%d",monster_id);
        return 0;
    }
    for (k = 0; k < 10; ++k) (void)crt_rand();
    for (k = 0; k < 5; ++k) (void)crt_rand();
    memset(&pen[i], 0, sizeof pen[i]);
    pen[i].used = 1;
    pen[i].monster_id = monster_id;
    pen[i].level = g_hero.level;
    pen[i].owner_class = g_hero.klass;
    pen[i].hp = pen[i].max_hp = g_world.monsters[monster_id].hp;
    wos_log_event("pet_acquired","monster=%d name=%s slot=%d",
                  monster_id,g_world.monsters[monster_id].name,i);
    return i;
}

int pet_release(int index)
{
    if (!pet_at(index)) return 0;
    wos_log_event("pet_released","monster=%d",pen[index].monster_id);
    memset(&pen[index], 0, sizeof pen[index]);
    return 1;
}

/* FUN_004133E7 with the pet pen button bar (FUN_00412716, petButtons.bmp,
 * petPen.jpg). hero+0x0EE8 and hero+0x0EEC hold the two out pets. */
int pet_summon(int index)
{
    Pet *p = pet_at(index);
    if (!p) return 0;
    if (g_hero.pet_ids[0] == 0) g_hero.pet_ids[0] = p->monster_id;
    else if (g_hero.pet_ids[1] == 0) g_hero.pet_ids[1] = p->monster_id;
    else return 0;
    wos_log_event("pet_summon","monster=%d slot=%d",p->monster_id,index);
    return 1;
}

int pet_dismiss(int index)
{
    int i;
    if (!pet_at(index)) return 0;
    for (i = 0; i < 2; ++i)
        if (g_hero.pet_ids[i] == (uint32_t)pen[index].monster_id) g_hero.pet_ids[i] = 0;
    wos_log_event("pet_dismiss","monster=%d",pen[index].monster_id);
    return 1;
}

int pet_level_of(int monster_id)
{
    int i;
    for (i = 0; i < PET_PEN_SLOTS; ++i)
        if (pen[i].used && pen[i].monster_id == monster_id) return pen[i].level;
    return 0;
}

/* ---------------------------------------------------------------- dumps */

void pet_dump(DumpEmit emit, void *user)
{
    int i;
    for (i = 0; i < PET_PEN_SLOTS; ++i) {
        if (!pen[i].used) continue;
        dump_emit_int(emit,"items.pet.slot",i,user);
        dump_emit_int(emit,"items.pet.monster",pen[i].monster_id,user);
        dump_emit_int(emit,"items.pet.level",pen[i].level,user);
        dump_emit_int(emit,"items.pet.hp",pen[i].hp,user);
    }
}

void items_dump(DumpEmit emit, void *user)
{
    char text[1024];
    int i, id, count, used = 0, len = 0, width = 0, height = 0;
    trophy_bag_size(&width, &height);
    for (i = 0; i < TROPHY_BAG_SLOTS && len < (int)sizeof text - 40; ++i)
        if (trophy_bag_get(i,&id,&count) && id != 0 && count != 0) {
            len += snprintf(text + len, sizeof text - (size_t)len,
                            "%s%d:%d", len ? "," : "", id, count);
            ++used;
        }
    if (!used) snprintf(text, sizeof text, "0");
    emit("items.trophy_bag", text, user);
    snprintf(text, sizeof text, "%dx%d", width, height);
    emit("items.trophy_bag_size", text, user);
    dump_emit_int(emit,"items.trophy_bag_used",used,user);
    dump_emit_int(emit,"items.trophy_bag_free",trophy_bag_free(),user);
    dump_emit_int(emit,"items.attr_pool",attr_pool,user);
    dump_emit_int(emit,"items.throw_armed",throw_item_id,user);
    dump_emit_int(emit,"items.pet_count",pet_count(),user);
    dump_emit_int(emit,"items.pet_ids0",(long long)g_hero.pet_ids[0],user);
    dump_emit_int(emit,"items.pet_ids1",(long long)g_hero.pet_ids[1],user);
    pet_dump(emit, user);
}

void items_init(void)
{
    int i;
    for (i = 0; i < EQUIP_SLOT_COUNT; ++i)
        snprintf(equip_names[i], sizeof equip_names[i], "%s", equip_defaults[i]);
    items_clear_message();
    attr_pool = 0;
    throw_item_id = 0;
    pet_reset();
}
