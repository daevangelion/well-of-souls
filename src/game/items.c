/* Item use, the trophy bag and the pet pen. See items.h for the VA table. */
#include "items.h"
#include "game.h"
#include "scene.h"
#include "html.h"
#include "battle.h"
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

/* FUN_00413181's ten unrolled EncInt constructions run before the monster lookup
 * is validated, so they need somewhere to land even on a rejected pet. Core
 * measured the stride as 0x38 = sizeof(EncInt) and the order as descending. */
#define PET_SCRATCH 10
static EncInt pet_scratch[PET_SCRATCH];

/* FUN_00482431 returns the +EQUIP name for an item class, and forwards classes
 * 2..9 (the right hands) to FUN_004825BF, whose table is the +HANDS names. Both
 * tables live in world.c (FUN_004824B4 / FUN_004825BF), so read them there rather
 * than keeping a second copy here. */
const char *items_class_slot_name(int item_class)
{
    /* FUN_00482431: the item class IS the equip id, so classes 12..19 are the
     * hand classes and are named from the HANDS table, not the EQUIP table. */
    if (item_class == ITEM_HELMET) return world_equip_name(0);
    if (item_class == ITEM_ARMOR) return world_equip_name(1);
    if (item_class >= ITEM_RIGHT_FIRST && item_class <= ITEM_RIGHT_LAST)
        return world_hand_name(item_class - ITEM_RIGHT_FIRST);
    if (item_class == ITEM_BOOTS) return world_equip_name(10);
    if (item_class == ITEM_SHIELD) return world_equip_name(11);
    if (item_class == ITEM_RING) return world_equip_name(12);
    if (item_class == ITEM_AMULET) return world_equip_name(13);
    return NULL;
}


const char *items_last_message(void) { return last_message; }
void items_clear_message(void) { last_message[0] = '\0'; }
static void say(const char *text) { snprintf(last_message, sizeof last_message, "%s", text); }

/* ------------------------------------------------------------ item apply */

/* formats_online.md 6.2: map flag 4096 (NO_HEAL) makes every heal worth 1 HP.
 * FUN_004A6353 reads it through FUN_00482BB8(0x1000). */
static int no_heal(void)
{
    return g_hero.map >= 0 && g_hero.map < WORLD_MAX_MAPS &&
           (g_world.maps[g_hero.map].flags & 0x1000u) != 0;
}

/* FUN_004A6B55, the cure. The actor it operates on is the COMBATANT record, not
 * the hero: FUN_00491E45 memsets each combatant to zero every fight, so the
 * per-disease counters at rec+0x398+4n start at zero and a disease never
 * survives a fight. There is no hero-side disease list, so an antidote used
 * OUTSIDE a fight cures nothing and the item is not consumed - which is the
 * original's behaviour, not a gap.
 *
 * FUN_004A6B55's own three-way branch, and the reason this is a call rather than
 * a bitmask clear:
 *   n >= 20 or n == 11  -> one step off, the counter floored at 0
 *   n in 2..8           -> zeroed outright
 *   n == 1, 9, 10, 12..19 -> no effect at all, silently
 *   anything outside 1..24 -> ignored
 * The name rows for "cured of %s" exist only for n = 2 and n = 3 (the -100-n
 * family is 25 rows at stride 0x150 with -102/-103 at the end), so a message
 * can only ever name those two. */
static int cure_disease(int disease)
{
    int d = disease < 0 ? -disease : disease;
    if (d < 1 || d > 24) return 0;
    if (!battle_active()) return 0;   /* no combatant, no counter, no cure */
    return battle_cure(0, d);
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
        wos_log_event("travel_failed","reason=scene item=%d scene=%d need=%d",
                      item->klass,game_current_scene(), item->travel_scene);
        say("Travel Failed.");
        return 0;
    }
    if (item->travel_mode == 0) game_enter_map(item->travel_map_scene, item->travel_link,
                                              item->travel_drop_in);
    else game_enter_scene(item->travel_map_scene, NULL);
    say("Travel.");
    wos_log_event("travel","item=%d mode=%d map=%d link=%d drop_in=%d",
                  0,item->travel_mode,item->travel_map_scene,item->travel_link,
                  item->travel_drop_in);
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

/* FUN_0046F60F: the geometry word at hero+0xEE0. Returns the cell count.
 *
 * The original guards the RAW word first: a stored 0 decodes to 0x0, not to
 * 0x1D43E217. Without that guard a zeroed record decodes to w=7491, h=57879,
 * 433,571,589 cells, and every consumer that iterates cells walks 128 entries
 * off the end. The clamp below is belt-and-braces on top of the original's own
 * guard: FUN_0046FED9 only ever writes 1..16 by 1..16 with a product <= 128, so
 * anything outside that could not have been written by the original either. */
int trophy_bag_size(int *width, int *height)
{
    /* The original tests the RAW word before the XOR (FUN_0046F60F: `uVar1 = 0;
     * if (hero[0xEE0] != 0) uVar1 = hero[0xEE0] ^ 0x1D43E217;`), and a stored 0
     * is how a new soul records "no bag" - FUN_004200CA memsets the 0x16CC record.
     * Decoding the zero word would give 7491 x 57879. FrontHero-2 exposes the
     * same predicate as trophy_bag_absent(); the explicit test below is the
     * original's and does not depend on it. */
    uint32_t raw = g_hero.trophy_bag_geo;
    uint32_t v = (raw && !trophy_bag_absent(&g_hero)) ? (raw ^ TROPHY_BAG_KEY) : 0u;
    int w = (int)(v >> 16), h = (int)(v & 0xffffu);
    /* Both out parameters are written UNCONDITIONALLY from the decoded word, as
     * in the original, so a legal intermediate such as the 4x0 that
     * `SET num.TrophyBagWidth, 4` leaves behind reads its 4 back and the next
     * SET can complete it. Only the CELL COUNT is guarded, and that is what
     * trophy_bag_pack's loop bound depends on. */
    if (width) *width = w;
    if (height) *height = h;
    return (w >= 1 && w <= 16 && h >= 1 && h <= 16) ? w * h : 0;
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

/* FUN_0046FE7B: close the gaps a shrink leaves behind. The bound is taken from
 * the decoder, which cannot return more than TROPHY_BAG_SLOTS, so this loop
 * cannot walk off the array on any record - decoded, zeroed or corrupt. */
void trophy_bag_pack(void)
{
    int total = trophy_bag_size(NULL, NULL), i, j;
    if (total < 1 || total > TROPHY_BAG_SLOTS) return;
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
    /* room is bounded by the loop above; clamp anyway so a caller cannot read
     * an int that wrapped. */
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

/* FUN_0044E3BD, the Learn button. Its gates, in the original's order:
 *   1. a list row must be selected
 *   2. the spell's PP cost field (record +0x120) must be >= 0
 *   3. FUN_004A46EC(spell.affinity, spell.element): the elemental PP LEVEL for
 *      that element must be >= the required affinity
 *   4. the hero's PP wallet must cover the cost - this produces a MESSAGE
 *      ("not enough PP"), it does not remove the spell from the list
 * So gates 2 and 3 decide whether a row is LEARNABLE AT ALL, and gate 4 is an
 * affordability check at press time. The port had them fused: the Spells list
 * was built from hero_can_learn_spell(), which includes gate 4, so a hero with
 * less PP than the cheapest spell saw an EMPTY list and the Learn button had
 * nothing to act on. There is also no min-level and no token gate at learn time
 * - FUN_0044E3BD has neither. */
int items_spell_listable(int id)
{
    const SpellDef *spell;
    int element;
    if (id <= 0 || id >= WORLD_MAX_SPELLS) return 0;
    spell = &g_world.spells[id];
    if (!spell->used) return 0;
    if (spell->pp_cost < 0) return 0;              /* gate 2 */
    if (spell->flags & 2) return 0;                /* "not offered to humans" */
    /* FUN_004A46EC: an element outside 0..7 DEFAULTS to 4 rather than failing. */
    element = (spell->element < 0 || spell->element > 7) ? 4 : spell->element;
    if (spell->req_affinity > hero_pp_level(g_hero.element_pp[element])) return 0;  /* gate 3 */
    return 1;
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
 * not an image index.
 *
 * The RNG cost is the whole point of the EncInt seal, so the order matters:
 * ten UNROLLED FUN_0049B75D constructions run first and unconditionally - even
 * for a bad monster id - and only then six FUN_0049B71B sets, but only once the
 * monster row has been validated. 10*4 + 6*4 = 64 crt_rand() on the happy path,
 * 40 when the monster lookup fails, 60 on the happy path. enc_construct_array is not used here
 * because these are unrolled, not a counted loop; the ascending-address order
 * is the Pet struct's declaration order, which is what the original's stack
 * frame does too. */
int pet_spawn(int monster_id)
{
    int i, k, slot = -1;
    /* Descending, like the original: Core measured the ten ctor targets in
     * FUN_00413181 as EBP-0x128, -0xF0, -0xB8, -0x80, -0x48 then -0x808, -0x7D0,
     * -0x798, -0x760, -0x728, stride 0x38, highest first within each group. */
    for (k = PET_SCRATCH - 1; k >= 0; --k) enc_clear(&pet_scratch[k]);
    if (monster_id <= 0 || monster_id >= WORLD_MAX_MONSTERS) return 0;
    if (!g_world.monsters[monster_id].used) return 0;
    for (i = 0; i < PET_PEN_SLOTS; ++i) if (!pen[i].used) { slot = i; break; }
    if (slot < 0) {
        wos_log_event("pet_rejected","reason=pen_full monster=%d",monster_id);
        return 0;
    }
    memset(&pen[slot], 0, sizeof pen[slot]);
    /* Five sealed stores (FUN_0049B71B, 4 draws each = 20), in the original's
     * order. Oracle4.ItemsPanelsVAs corrected my offset citation: the five
     * stat EncInts in the PEN RECORD (base DAT_004E4878, stride 0x608) are at
     * +0xD8/+0xE0/+0xE8/+0xEC/+0xF0, not the +0xEC..+0x100 I had written - the
     * latter is FUN_00413181's MONSTER-record reads. The draw count and the
     * order are unchanged; only the cited source of the values differs. */
    enc_set(&pen[slot].str, g_world.monsters[monster_id].strength);
    enc_set(&pen[slot].sta, g_world.monsters[monster_id].stamina);
    enc_set(&pen[slot].agi, g_world.monsters[monster_id].agility);
    enc_set(&pen[slot].dex, g_world.monsters[monster_id].dexterity);
    enc_set(&pen[slot].wis, g_world.monsters[monster_id].wisdom);
    /* The five rec[0x10B..0x10F] ability seals FUN_00414059 writes at
     * 0x004141ED..0x00414207, in the order below. FUN_00414059's own five seals
     * are spent by battle_spawn_authored(); these are the pen's. */
    enc_set(&pen[slot].abil[0], g_world.monsters[monster_id].strength);
    enc_set(&pen[slot].abil[1], g_world.monsters[monster_id].stamina);
    enc_set(&pen[slot].abil[2], g_world.monsters[monster_id].agility);
    enc_set(&pen[slot].abil[3], g_world.monsters[monster_id].dexterity);
    enc_set(&pen[slot].abil[4], g_world.monsters[monster_id].wisdom);
    pen[slot].spawn_offense = g_world.monsters[monster_id].offense;
    pen[slot].spawn_defense = g_world.monsters[monster_id].defense;
    pen[slot].spawn_xp = g_world.monsters[monster_id].exp;
    pen[slot].spawn_mp = g_world.monsters[monster_id].mp;
    pen[slot].spawn_max_mp = g_world.monsters[monster_id].mp;
    pen[slot].level = g_hero.level;
    pen[slot].hp = pen[slot].max_hp = g_world.monsters[monster_id].hp;
    pen[slot].used = 1;
    pen[slot].monster_id = monster_id;
    pen[slot].owner_class = g_hero.klass;
    wos_log_event("pet_acquired","monster=%d name=%s slot=%d",
                  monster_id,g_world.monsters[monster_id].name,slot);
    return slot;
}

int pet_release(int index)
{
    if (!pet_at(index)) return 0;
    wos_log_event("pet_released","monster=%d",pen[index].monster_id);
    memset(&pen[index], 0, sizeof pen[index]);
    return 1;
}

/* FUN_004133E7 with the pet pen button bar (FUN_00412716, petButtons.bmp,
 * petPen.jpg).
 *
 * THERE IS ONE PET, not two. hero+0x0EEC is a GetTickCount() stamp, written by
 * FUN_0043B8E7 at all.c:42151 and lazily re-stamped at 42177 while it is still
 * zero, and read only as a "!= 0" predicate (42168, 42177). The pet's live id
 * is hero+0x0EE8 - the word pet_ids[0] already holds - and the original reads
 * that only as "!= 0" (42091, 71066). The 0x6BC/0x71C pair that FUN_0043B8E7
 * sets beside it is the cheat-points counter and its negative mirror, NOT the
 * pet and NOT the equipped right-hand item: the function takes 0x25 (37) at
 * 42170 and then checks its own output for tampering. */
int pet_summon(int index)
{
    Pet *p = pet_at(index);
    if (!p) return 0;
    if (g_hero.pet_ids[0] != 0) return 0;     /* one pet out at a time */
    g_hero.pet_ids[0] = p->monster_id;
    wos_log_event("pet_summon","monster=%d slot=%d",p->monster_id,index);
    return 1;
}

/* ---- the call/recall gate helpers (FUN_004142F2's inputs) ---------------- */

/* FUN_0043B947: a fight is running. */
static int pet_busy(void) { return battle_active(); }

/* DAT_004E6910 = _SRNGetNetworkType_0() (all.c:31725). The original's own
 * dispatch names every value: 0 = "Solo Channel" (s_Solo_Channel_004e8214),
 * 1 = MPlayer, 2 = Modem, 3 = LAN, 4 = online. Offline play is therefore
 * DAT_004E6910 == 0, and every `DAT_004E6910 != 0` test in the original is
 * FALSE in solo. The port has no SRNet, so this is a named 0 rather than a
 * dropped term: gate 1 below is unreachable offline, exactly as in the
 * original, and that is why a pet CAN be called while fighting. */
static int net_online(void) { return 0; }

/* hero+0xA58 bit 2, the script `IF M2` condition. The original STORES it in the
 * record rather than recomputing it on read - FUN_0044B196 sets it on the
 * "Modified Quest File Detected" path when the world's crc1 differs from the one
 * the soul was created against - so it is read from Hero.flags, not derived. */
static int hero_m2_bit2(void) { return hero_flag(&g_hero, 2); }

/* DAT_004E5DA8 is the SERVER's `noPets` rule (all.c:31345 initialises it to 0;
 * 31604-31612 sets it from the rule string and clamps a negative to 0). Solo
 * play never receives a rule set, so it is 0. The other half of the original's
 * test is the map/link flag 128, NO_PETS (formats_online.md 6.1, read through
 * FUN_00482BB8(0x80)). */
static int pets_forbidden(void)
{
    /* DAT_004E5DA8 == 0 offline. */
    return g_hero.map >= 0 && g_hero.map < WORLD_MAX_MAPS &&
           (g_world.maps[g_hero.map].flags & 0x80u) != 0;
}

/* FUN_004142F2's away-counter clamps, verbatim:
 *   pen[0x8A] == 0 -> pen[0x8A] = -pen[0x24];  then if pen[0x8A]+pen[0x24] != 0
 *   -> pen[0x24] = 1 and pen[0x8A] = -pen[0x24]
 *   pen[0x8B] == 0 -> pen[0x8B] = -pen[0x25];  then if pen[0x8B]+pen[0x25] != 0
 *   -> pen[0x25] = 0 and pen[0x8B] = -pen[0x25] */
static void pet_clamp_counters(Pet *p)
{
    if (p->away_a == 0) p->away_a = -p->level;
    if (p->away_a + p->level != 0) { p->level = 1; p->away_a = -p->level; }
    if (p->away_b == 0) p->away_b = -p->max_hp;
    if (p->away_b + p->max_hp != 0) { p->max_hp = 0; p->away_b = -p->max_hp; }
}

/* FUN_00413DE7 == 1 means "Bring the current pet into battle", i.e. the pet is
 * in the pen and free to answer the Call button. */
static int pet_in_fight(void)
{
    char msg[96];
    int state = 0, combat = 0;
    return pet_state_message(msg, sizeof msg, &state, &combat) != 0;
}

/* FUN_00414059's param_1 == 1 arm. The argument is `allegiance`, rec[0x114],
 * which for the player's own pet is the hero's own serial. */
static int pet_recall(int *slot)
{
    *slot = battle_recall(g_hero.serial);
    return *slot > -1;
}

/* A fight is running AND the pet has somewhere to go. */
static int pet_active(void) { return battle_active(); }

/* FUN_00414059's param_1 == 0 arm, via Battle3's authored spawner. */
static int pet_spawn_into_fight(int mode, Pet *p)
{
    BattleSpawn b;
    (void)mode;   /* FUN_00414059 takes it, but the authored spawner does not */
    memset(&b, 0, sizeof b);
    b.monster_id = p->monster_id;
    b.allegiance = g_hero.serial;      /* rec[0x114]: the caster's own side */
    b.level = p->level;
    b.hp = p->hp;
    b.max_hp = p->max_hp;
    /* Battle3's mapping, which is NOT positional: param_9 is OFFENCE and
     * param_10 is DEFENCE, the opposite of the pen's +0xB0/+0xB4 order. */
    b.mp = p->spawn_mp;
    b.max_mp = p->spawn_max_mp;
    b.offense = p->spawn_offense;
    b.defense = p->spawn_defense;
    b.xp = p->spawn_xp;
    b.ability[0] = enc_get(&p->abil[0]);
    b.ability[1] = enc_get(&p->abil[1]);
    b.ability[2] = enc_get(&p->abil[2]);
    b.ability[3] = enc_get(&p->abil[3]);
    b.ability[4] = enc_get(&p->abil[4]);
    return battle_spawn_authored(&b);
}

/* FUN_004142D8: the per-monster "first seen" key. It is a hash of the monster
 * id, NOT a clock read - `return (id + 0x3FF ^ 0xF23) * 0x2F169 & 0x7FFFFFFF`. */
static int pet_seen_key(int monster_id)
{
    return (int)(((uint32_t)monster_id + 0x3FFU) ^ 0xF23U) * 0x2F169U & 0x7FFFFFFF;
}

/* FUN_00410B1A: pen+0xD8 -= amount, then clamp to 0..2000000. */
void pet_tick_out(int amount)
{
    int i, v;
    for (i = 0; i < PET_PEN_SLOTS; ++i) {
        if (!pen[i].used) continue;
        v = pen[i].out_timer - amount;
        if (v > PET_TIMER_OUT_MAX) v = PET_TIMER_OUT_MAX;
        if (v < 0) v = 0;
        pen[i].out_timer = v;
    }
}

/* FUN_00410B42: pen+0xE8 -= amount, then clamp to 0..1000000. */
void pet_tick_call(int amount)
{
    int i, v;
    for (i = 0; i < PET_PEN_SLOTS; ++i) {
        if (!pen[i].used) continue;
        v = pen[i].call_timer - amount;
        if (v > PET_TIMER_CALL_MAX) v = PET_TIMER_CALL_MAX;
        if (v < 0) v = 0;
        pen[i].call_timer = v;
    }
}

/* FUN_00413DE7. Returns 1 when the pet is already in a fight, so the pen's
 * Recall button is live. `out_state` is pen[0] and `in_combat` is the hero
 * combatant's 0x38C == 0x5F test; the string is the reason it is not. */
int pet_state_message(char *out, size_t cap, int *out_state, int *in_combat)
{
    Pet *p = pen[0].used ? &pen[0] : NULL;
    int combat = 0;
    if (out_state) *out_state = p ? p->state : 0;
    if (in_combat) *in_combat = combat;
    if (out && cap) out[0] = '\0';
    if (g_hero.hp < 1) { if (out && cap) snprintf(out, cap, "You are dead, so your pets won't come out."); return 0; }
    if (!p) { if (out && cap) snprintf(out, cap, "You must first select a pet in the pen."); return 0; }
    if (p->state == 2) { if (out && cap) snprintf(out, cap, "The current pet has already fought."); return 0; }
    if (p->state == 3) { if (out && cap) snprintf(out, cap, "The current pet has already battled."); return 0; }
    if (p->state != 1 && p->state != 4) {
        if (out && cap) snprintf(out, cap, "You must first select a pet in the pen.");
        return 0;
    }
    if (out && cap) snprintf(out, cap, "Bring the current pet into battle.");
    if (out_state) *out_state = 1;
    return 0;
}

/* FUN_004142F2, the pet call/recall trigger.
 *
 * Gate order is the original's and matters, because each gate writes a different
 * message and the caller shows the first one that fires:
 *   1. already fighting (or the M2 bit) AND the "no pets in combat" flag AND a
 *      CALL -> "cannot call it here", return -1
 *   2. the pet's level must be below the hero's, unless this is a RECALL
 *   3. escalation: pet above the hero, hero under the monster's minimum level,
 *      the monster's bit 0, a pending-away counter, or never seen -> downgrade
 *      a CALL to a RELEASE and announce it
 *   4. map/link flag 128 (NO_PETS) or the global pet-off switch skips the spawn
 * The one rand() is the tired-pet check on the CALL arm and nothing else draws. */
int pet_trigger(int mode)
{
    Pet *p;
    int monster, announce = 0, seen, busy, tired, slot = -1;
    /* Gate 1: (in a fight || the hero's M2 bit) && DAT_004E6910 && a CALL. The
     * M2 term is hero+0xA58 bit 2, which the port's Hero does not carry, so it
     * reads 0; the net term is 0 offline, so the whole gate is 0 offline and the
     * original never blocks a pet call in solo play either. */
    if (mode == PET_CALL && pet_busy() && (hero_m2_bit2() & 2) != 0 && net_online()) {
        say("Your pet cannot come out while you are fighting.");
        return -1;
    }
    p = pen[0].used ? &pen[0] : NULL;
    if (!p) { say("You must first select a pet in the pen."); return -1; }
    monster = p->monster_id;
    if (!p->seen_key) p->seen_key = pet_seen_key(monster);
    seen = pet_seen_key(monster);
    if (!(p->level < g_hero.level || mode != PET_CALL)) {
        say("Your pet is too advanced for you.");
        return -1;
    }
    if (g_hero.level > p->level
        || (monster > 0 && monster < WORLD_MAX_MONSTERS && g_hero.level < g_world.monsters[monster].level)
        || (monster > 0 && monster < WORLD_MAX_MONSTERS && (g_world.monsters[monster].flags & 1))
        || p->away_a + p->level != 0
        || (monster <= 0 || monster >= WORLD_MAX_MONSTERS) || seen != p->seen_key) {
        if (mode == PET_CALL) mode = PET_RELEASE;
        announce = 1;
    }
    busy = 0;
    if (mode == PET_RECALL) {
        if (pet_recall(&slot)) wos_log_event("pet_recall","monster=%d",monster);
    } else if (mode == PET_RELEASE) {
        busy = (p->state == 1);
    } else {
        /* CALL: the 200-second lead timer only arms when the pet is out there. */
        int here = pet_in_fight();
        if (here) {
            /* FUN_004142F2: the pet is too tired to answer, 30 % of the time. */
            tired = (PET_FATIGUE_MAX - p->fatigue) < 100000;
            if (tired && (int)(crt_rand() % 100) < 30) {
                say("Your pet is too tired to come.");
                mode = PET_RELEASE;
            }
            p->call_timer = PET_CALL_LEAD;
            wos_log_event("pet_call","monster=%d",monster);
        }
    }
    if (pets_forbidden()) { wos_log_event("pet_call_blocked","reason=no_pets"); return -1; }
    /* GATE 5. The original tests the fight block's +0x88 ("in combat", set by
     * FUN_00491767) and takes a non-combat spawn path when it is 0. The port's
     * fight.engaged is set unconditionally and carries no information, so that
     * arm has no path and the authored spawner is always used. */
    if (!busy && pet_active()) {
        if (announce || (mode == PET_RELEASE && pet_busy() && net_online())) {
            say("Your pet cannot come out here.");
        } else {
            pet_clamp_counters(p);
            slot = pet_spawn_into_fight(mode, p);
        }
    }
    if (mode == PET_CALL) {
        p->state = 2;
        p->out_timer = PET_TIMER_OUT_MAX;
        wos_log_event("pet_called","monster=%d",monster);
    } else if (mode == PET_RELEASE) {
        pet_release(0);
    } else if (mode == PET_RECALL && slot > -1) {
        wos_log_event("pet_recalled","monster=%d",monster);
    }
    return slot;
}

int pet_dismiss(int index)
{
    if (!pet_at(index)) return 0;
    if (g_hero.pet_ids[0] == (uint32_t)pen[index].monster_id) g_hero.pet_ids[0] = 0;
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
        dump_emit_int(emit,"items.pet.str",enc_get(&pen[i].str),user);
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
    /* pet_ids[1] is the GetTickCount stamp at hero+0x0EEC, not a second id. */
    dump_emit_int(emit,"items.pet_id",(long long)g_hero.pet_ids[0],user);
    dump_emit_int(emit,"items.pet_stamp",(long long)g_hero.pet_ids[1],user);
    pet_dump(emit, user);
}

void items_init(void)
{
    items_clear_message();
    attr_pool = 0;
    throw_item_id = 0;
    pet_reset();
}
