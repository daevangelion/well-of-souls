/* Solo combat. Reference: Souls.exe FUN_00480499, FUN_00490e7c,
 * FUN_004a7794; docs/re/battle.md. All durations are fixed 60 Hz steps. */
#include "battle.h"
#include "game.h"
#include "hero.h"
#include "scene.h"
#include "../engine/font.h"
#include "../engine/log.h"
#include "../engine/rng.h"
#include "../engine/text.h"
#include <ctype.h>
#include <stdlib.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

enum { ACTORS = 144, ROUND_START = 5, ROUND_ACT = 6, ROUND_DAMAGE = 7,
       ACTION_GATE = 30, ROUND_IDLE = 120, LUNGE_FRAMES = 60, FLOAT_FRAMES = 90 };
typedef struct {
    int id, ally, hp, max_hp, mp, level, offense, defense, ability[5], element;
    uint32_t ailments;
    int ability_shift[HERO_ABILITIES];
    int xp, gold, x, y, ready, attacks, last_action, damage, floating;
    int sheet_index;
} Combatant;
static struct {
    Combatant actors[ACTORS];
    Sheet sheets[ACTORS];
    int count, sheets_count, state, frame, round_start, cursor, target;
    int queued, attacker, victim, animation, hp_gauge, mp_gauge, regen_start;
    int regen_initial, xp, gold, damage, attack_training;
    int spell, chosen_spell, spell_menu, spell_selection, spell_count, known[WORLD_MAX_SPELLS];
    int mp_start, mp_initial, attack_pp, fizzle, duration, bound_spell;
    char message[96];
    BattleResult result;
    Rect view;
} fight;

static int clamp(int64_t n, int low, int high)
{
    return n < low ? low : n > high ? high : (int)n;
}
/* FUN_004a761f: repeated debuffs divide, repeated buffs multiply. */
static int ability(const Combatant *a, int kind)
{
    int shift = a->ability_shift[kind], value = a->ability[kind];
    if (!shift && (a->ailments & (1u<<(20+kind)))) shift = 1;
    return clamp(shift < 0 ? (int64_t)value*(1-shift) : value/(1+shift),0,255);
}
static void battle_music(const char *key)
{
    const Map *map = game_current_map();
    game_music(world_music(map && map->def ? map->def->root : g_world.name,key));
}
static int roll(int bound)
{
    return (int)rng_bounded(game_rng(), (uint32_t)bound);
}
/* FUN_00479509: the scale row applies only percentages 1..500. */
static int scale(int value, int pct)
{
    return clamp(pct > 0 && pct <= 500 ? (int64_t)value * pct / 100 : value, 0, 65535);
}
static int derived(int value, int level, int coefficient, int pct)
{
    return value > 0 ? clamp(value, 1, 65535) : scale(level * coefficient / 100, pct);
}
static void monster_stats(Combatant *a, const MonsterDef *m)
{
    const MonsterDef *s = &g_world.monsters[0];
    int64_t xp;
    a->level = clamp(m->level, 1, 65535);
    a->element = m->element;
    /* Assembly 004808e5..0048091e: these are 1162,513,567,834 / 100,
     * not the decimal multipliers transcribed in battle.md section 2. */
    a->max_hp = a->hp = derived(m->hp, a->level, 1162, s->hp);
    a->mp = derived(m->mp, a->level, 513, s->mp);
    a->offense = derived(m->offense, a->level, 567, s->offense);
    a->defense = derived(m->defense, a->level, 834, s->defense);
    /* 00480e82..00481094, each stat clamped to 255 after row-0 scaling. */
    a->ability[ABIL_STR] = clamp(derived(m->strength,a->level,197,s->strength),0,255);
    a->ability[ABIL_STA] = clamp(derived(m->stamina,a->level,212,s->stamina),0,255);
    a->ability[ABIL_AGI] = clamp(derived(m->agility,a->level,189,s->agility),0,255);
    a->ability[ABIL_DEX] = clamp(derived(m->dexterity,a->level,175,s->dexterity),0,255);
    a->ability[ABIL_WIS] = m->wisdom == 0 ? 0 : clamp(derived(m->wisdom,a->level,231,s->wisdom),0,255);
    /* FUN_004807d3 actually starts with HP*40/100, NOT level%28. */
    xp = (int64_t)a->max_hp * 40 / 100;
    xp = xp * (a->ability[ABIL_STA] + 300) / 300;
    xp = xp * (a->offense + 200) / 200;
    xp = xp * (a->defense + 300) / 300;
    xp = xp * (a->level + 70) / 70;
    a->xp = m->exp == -2 ? 0 : m->exp > 0 ? clamp(m->exp,0,60000) :
        scale(clamp(xp,1,60000),s->exp);
    a->gold = m->gold == -2 ? 0 : m->gold > 0 ? clamp(m->gold,0,32767) :
        clamp(scale((a->xp * 4 / 3) * 50 / (a->level + 50),s->gold),5,32767);
}
static int first_enemy(void)
{
    int i;
    for (i = 1; i < fight.count; ++i)
        if (!fight.actors[i].ally && fight.actors[i].hp > 0) return i;
    return -1;
}
static void spawn(int signed_id)
{
    Combatant *a;
    const MonsterDef *m;
    int i, id, ordinal;
    if (signed_id == INT_MIN || fight.count == ACTORS) return;
    id = signed_id < 0 ? -signed_id : signed_id;
    if (id < 1 || id >= WORLD_MAX_MONSTERS || !g_world.monsters[id].used) return;
    m = &g_world.monsters[id];
    a = &fight.actors[fight.count];
    a->id = id;
    a->ally = signed_id < 0;
    monster_stats(a, m);
    ordinal = (fight.count - 1) % 9;
    /* Nine-slot formation; logical ground y>=128 (battle.md section 3/4). */
    a->x = a->ally ? 55 + (ordinal % 3) * 36 : 210 + (ordinal % 3) * 48;
    a->y = 152 + (ordinal / 3) * 40;
    a->sheet_index = -1;
    for (i = 1; i < fight.count; ++i) {
        const MonsterDef *other = &g_world.monsters[fight.actors[i].id];
        if (!strcmp(other->skin, m->skin)) { a->sheet_index = fight.actors[i].sheet_index; break; }
    }
    if (a->sheet_index < 0) {
        a->sheet_index = fight.sheets_count++;
        if (!strcmp(m->skin,"mirror")) sheet_load_skin(&fight.sheets[a->sheet_index],g_hero.skin);
        else sheet_load_monster(&fight.sheets[a->sheet_index],m->skin);
    }
    /* FUN_00491e45 initializes monsters' last-action tick to
     * now - rand()%5000 - 3000, not a full eight-second charge. */
    a->last_action = -(180 + roll(300));
    wos_log_event("battle_actor","slot=%d monster=%d hp=%d level=%d offense=%d defense=%d str=%d sta=%d charge=%d",
                  fight.count,id,a->hp,a->level,a->offense,a->defense,
                  a->ability[ABIL_STR],a->ability[ABIL_STA],-a->last_action);
    ++fight.count;
}
static void random_group(int difficulty, int distance_pct)
{
    const GroupDef *group;
    const Map *map;
    int i, n, all, before = fight.count;
    if (difficulty == INT_MIN) return;
    if (difficulty < 0) difficulty = -difficulty;
    if (difficulty < WORLD_MAX_GROUPS && g_world.groups[difficulty].used && difficulty) {
        group = &g_world.groups[difficulty];
        n = clamp(group->count,0,GROUP_MAX_MEMBERS);
        all = g_world.groups[0].count > 0 && g_world.groups[0].members[0] != 0;
        /* FUN_0049099b: MapView supplies the clamped/sign-adjusted chance. */
        for (i = 0; i < n; ++i) if (all || roll(100) < distance_pct) spawn(group->members[i]);
        if (fight.count == before && n) {
            int start = roll(n);
            for (i = 0; i < n && fight.count == before; ++i) spawn(group->members[(start+i)%n]);
        }
    }
    if (fight.count != before) return;
    map = game_current_map();
    if (map && map->mon_count > 0) {
        int closest = -1;
        int64_t distance = INT64_MAX;
        for (i = 0; i < map->mon_count && i < MON_RECORDS; ++i) {
            int id = map->mons[i].monster_id;
            int64_t x = (int64_t)map->mons[i].x-g_hero.x, y = (int64_t)map->mons[i].y-g_hero.y;
            if (id > 0 && id < WORLD_MAX_MONSTERS && g_world.monsters[id].used && x*x+y*y < distance) {
                closest = id; distance = x*x+y*y;
            }
        }
        if (closest > 0) spawn(closest);
    }
    if (fight.count == before) spawn(1); /* FUN_00464daf fallback. */
}
void battle_begin(const int *ids, int count, int difficulty, int distance_pct)
{
    Combatant *hero;
    int i;
    for (i = 0; i < fight.sheets_count; ++i) sheet_free(&fight.sheets[i]);
    memset(&fight,0,sizeof fight);
    fight.view = (Rect){0,0,364,416};
    fight.count = fight.sheets_count = 1;
    fight.state = ROUND_START;
    fight.result = BATTLE_RUNNING;
    fight.attacker = fight.victim = -1;
    fight.hp_gauge = fight.mp_gauge = fight.regen_initial = 25;
    fight.mp_initial = 25;
    distance_pct = clamp(distance_pct,0,100);
    hero = &fight.actors[0];
    hero->ally = 1;
    hero->hp = clamp(g_hero.hp,0,g_hero.max_hp);
    hero->max_hp = g_hero.max_hp;
    hero->mp = g_hero.mp;
    hero->level = clamp(g_hero.level,1,65535);
    for (i = 0; i < HERO_ABILITIES; ++i) hero->ability[i] = hero_ability(&g_hero,i);
    hero->ailments = g_hero.ailments;
    hero->x = 80; hero->y = 204;
    hero->offense = hero_offense(&g_hero);
    hero->defense = hero_defense(&g_hero);
    sheet_load_skin(&fight.sheets[0],g_hero.skin);
    if (count > 0 && ids) for (i = 0; i < count && i < ACTORS-1; ++i) spawn(ids[i]);
    else random_group(difficulty,distance_pct);
    fight.target = first_enemy();
    battle_music("fight");
    wos_log_event("battle_start","monsters=%d distance=%d",fight.count-1,distance_pct);
}
static void finish(BattleResult result)
{
    fight.result = result;
    if (result == BATTLE_WON) {
        int xp = fight.xp, gold = fight.gold;
        int64_t old_xp = g_hero.xp, old_gold = g_hero.gold;
        /* NO_REWARD is the scene/map flag, docs/re/script.md FLAGS. */
        if ((scene_flags() & (1u << 11)) || !fight.actors[0].attacks) xp = gold = 0;
        if (g_hero.level < 96) {
            int64_t cap = (hero_xp_for_level(&g_hero,g_hero.level+2)-g_hero.xp+3)/2;
            xp = clamp(xp,0,clamp(cap,0,INT_MAX));
        }
        hero_award(&g_hero,xp,gold);
        battle_music("victory");
        wos_log_event("battle_won","xp=%d gold=%d",(int)(g_hero.xp-old_xp),(int)(g_hero.gold-old_gold));
    } else if (result == BATTLE_LOST) {
        g_hero.hp = 0;
        battle_music("lost");
        wos_log_event("battle_lost",NULL);
    }
}
static int outcome(void)
{
    if (g_hero.hp <= 0) { finish(BATTLE_LOST); return 1; }
    if (first_enemy() < 0) { finish(BATTLE_WON); return 1; }
    return 0;
}
/* Corrected FUN_004a7794: FUN_0049b70f is an encrypted field reader,
 * NOT rand(). +628=level, +660=defense, +698=offense (004805b8..00480604).
 * Physical variance/crit alone use RNG (FUN_004a6974). */
static int physical_damage(Combatant *a, Combatant *b, int monster)
{
    int miss = clamp(ability(b,ABIL_AGI)-ability(a,ABIL_DEX),0,50);
    int d, base;
    int64_t power, raw;
    if (miss && roll(100) < miss) return -1;
    power = (int64_t)(a->level+100)*(ability(a,ABIL_STR)+65)*(a->offense+5)/6500;
    if (monster) {
        int elapsed = clamp(fight.frame-a->last_action,0,480);
        power = power*(a->level+75)/100;
        power = power*elapsed/480;
        if (power < 1) power = 1;
    }
    else if (fight.attack_training > 5000) {
        /* 004a7ba6..004a7c00: training above five adds an exponential
         * fraction of the original power (FMUL before ftol in the binary). */
        power += (int64_t)(power * pow(1.1892, fight.attack_training * 0.001 - 5.0));
    }
    raw = power*8000/((int64_t)(b->level+40)*(ability(b,ABIL_STA)+200));
    raw = raw*200/(b->defense+200);
    d = clamp(raw,1,32000);
    base = clamp((int64_t)d*9/10,1,32000);
    if (d > 1) d = roll(d)/5 + base;
    if (roll(50) == 0) d = (monster ? 2 : roll(3)+2)*base;
    return clamp(d,1,32000);
}
static int ai_target(int actor)
{
    int legal[ACTORS], n = 0, i;
    for (i = 0; i < fight.count; ++i)
        if (fight.actors[i].hp > 0 && fight.actors[i].ally != fight.actors[actor].ally) legal[n++] = i;
    return n ? legal[roll(n)] : -1;
}
/* Spell element resistance table, FUN_00482116. */
static int resistance(int element, int target)
{
    static const int wheel[8] = {400,100,35,15,0,15,35,100};
    if (element == 0 || element == 4 || element > 7) return target == element ? 400 : 15;
    return wheel[(target-element+256)&7];
}
static int spell_helpful(const SpellDef *s)
{
    return s->element == 0 || (s->damage <= -102 && s->damage >= -124);
}
static int legal_target(int actor, int target, int spell)
{
    const Combatant *a = &fight.actors[actor], *b = &fight.actors[target];
    const SpellDef *s = spell > 0 ? &g_world.spells[spell] : NULL;
    if (s && (s->flags & 4) && actor != target) return 0;
    if (s && s->damage == -1) return a->ally == b->ally && b->hp == 0;
    if (b->hp <= 0) return 0;
    return s && spell_helpful(s) ? a->ally == b->ally : a->ally != b->ally;
}
static int spell_target(int actor, int spell)
{
    int targets[ACTORS], n = 0, i;
    for (i = 0; i < fight.count; ++i) if (legal_target(actor,i,spell)) targets[n++] = i;
    return n ? targets[actor ? roll(n) : 0] : -1;
}
static int spell_cost(const Combatant *a, const SpellDef *s)
{
    return clamp((int64_t)s->mp_cost*((a->ailments & (1u<<3)) ? 2 : 1),0,INT_MAX);
}
/* FUN_0048e773: intelligence limits both element distance and affinity. */
static int monster_knows(const Combatant *a, const SpellDef *s)
{
    const MonsterDef *m = &g_world.monsters[a->id];
    int distance, wisdom = ability(a,ABIL_WIS);
    if (!s->used || (s->flags&1) || wisdom <= 0 || spell_cost(a,s) > a->mp) return 0;
    if ((m->flags&2) || s->element > 7)
        return s->element == a->element && s->req_affinity <= wisdom/8;
    distance = (s->element-a->element+256)&7;
    if (distance > 4) distance = 8-distance;
    return distance <= wisdom/40 && s->req_affinity <= wisdom/(distance*8+8);
}
static int prefix(const char *s, const char *word)
{
    while (*word && tolower((unsigned char)*s) == tolower((unsigned char)*word)) { ++s; ++word; }
    return !*word;
}
/* A96 pet verbs at 00502998: HEEL, HEEL TO, BITE, HIT, ATTACK, HEAL,
 * USE <spell-name|#id,id,...>, DUMP. USE uses the same knowledge checks. */
static int commanded_spell(int actor, int *target)
{
    const Combatant *a = &fight.actors[actor];
    const char *p = g_world.monsters[a->id].ai;
    int i, chosen = 0, n = 0;
    while (isspace((unsigned char)*p)) ++p;
    if (!*p) return -1;
    if (prefix(p,"heal")) {
        for (i = WORLD_MAX_SPELLS-1; i > 0; --i)
            if (g_world.spells[i].element == 0 && g_world.spells[i].damage > 0 &&
                monster_knows(a,&g_world.spells[i]) && a->hp < a->max_hp/2) {
                *target = actor; return i;
            }
        return 0;
    }
    if (!prefix(p,"use")) return 0;
    p += 3;
    while (isspace((unsigned char)*p)) ++p;
    if (*p == '#') {
        ++p;
        do {
            char *end;
            long id = strtol(p,&end,10);
            if (end == p) break;
            if (id > 0 && id < WORLD_MAX_SPELLS && monster_knows(a,&g_world.spells[id]) &&
                roll(++n) == 0) chosen = (int)id;
            p = end;
            if (*p != ',') break;
            ++p;
        } while (*p);
    } else {
        for (i = 1; i < WORLD_MAX_SPELLS; ++i)
            if (g_world.spells[i].used && prefix(p,g_world.spells[i].name) &&
                monster_knows(a,&g_world.spells[i])) {
                chosen = i; p += strlen(g_world.spells[i].name); break;
            }
    }
    if (chosen) {
        while (isspace((unsigned char)*p)) ++p;
        if (prefix(p,"on ")) p += 3;
        if (prefix(p,"yourself")) *target = legal_target(actor,actor,chosen) ? actor : spell_target(actor,chosen);
        else *target = spell_target(actor,chosen);
    }
    return chosen;
}
static int monster_spell(int actor, int *target)
{
    Combatant *a = &fight.actors[actor];
    const MonsterDef *m = &g_world.monsters[a->id];
    int i, best = 0, score = -1, command = commanded_spell(actor,target);
    if (command >= 0) return command;
    if (m->flags&16) return 0;
    /* FUN_0048e810 prioritizes life healing below half HP, then chooses
     * strongest effective attack with wisdom/500 spell cadence. */
    if (a->element == 0 && a->hp <= a->max_hp/2) {
        for (i = WORLD_MAX_SPELLS-1; i > 0; --i) {
            const SpellDef *s = &g_world.spells[i];
            if (s->element == 0 && s->damage > 0 && monster_knows(a,s)) { *target = actor; return i; }
        }
    }
    if (!(m->flags&8) && roll(500) >= ability(a,ABIL_WIS)) return 0;
    for (i = 1; i < WORLD_MAX_SPELLS; ++i) {
        const SpellDef *s = &g_world.spells[i];
        if (s->element && s->damage >= 0 && monster_knows(a,s)) {
            int value = s->damage*(500-resistance(s->element,fight.actors[*target].element));
            if (value > score) { best = i; score = value; }
        }
    }
    return best;
}
static int cast_success(int ratio, int pp)
{
    /* FUN_004a7456: ratio gives 50..100%; PP reduces remaining failure
     * probability by up to half, and the final +.5 rounds to nearest. */
    int base = ratio <= 5 ? 5000 : ratio >= 95 ? 10000 : 5000+(ratio-5)*5000/90;
    int trained = pp > 0 ? (int)((int64_t)50*pp/(pp+5000)) : 0;
    return (10000-(100-trained)*(10000-base)/100+50)/100;
}
static int spell_damage(int actor, int target, int spell, int targets)
{
    Combatant *a = &fight.actors[actor], *b = &fight.actors[target];
    const SpellDef *s = &g_world.spells[spell];
    int ratio = actor ? clamp(ability(a,ABIL_WIS)/4+50,0,100) : g_world.classes[g_hero.klass].magic_ratio;
    int wisdom = ability(a,ABIL_WIS), d, base;
    int64_t power, raw;
    if (!actor && fight.bound_spell) {
        const ClassDef *c = &g_world.classes[g_hero.klass];
        int hand = c->hand_ratio ? c->hand_ratio : 100-ratio;
        wisdom = (wisdom*ratio+ability(a,ABIL_STR)*hand)/(ratio+hand+30);
        if (g_world.items[g_hero.right_hand].klass != c->right_hand+11) wisdom = wisdom*50/100;
    }
    if (s->element == 0) {
        if ((b->ailments&(1u<<2)) || (scene_flags()&4096)) return -1;
        return -clamp(s->damage,0,32000);
    }
    /* FUN_004a7794 spell branch; encrypted getters read level/offense. */
    power = (int64_t)ratio*((int64_t)(a->level+25)*(wisdom+65)*s->damage/1625)/100;
    if (!actor) {
        int hand_ratio = g_world.classes[g_hero.klass].hand_ratio;
        if (!hand_ratio) hand_ratio = 100-ratio;
        power = ((int64_t)a->offense*hand_ratio/100+200)*power/200;
        if (fight.attack_training > s->req_affinity*1000)
            power += (int64_t)(power*pow(1.1892,fight.attack_training*.001-s->req_affinity));
    }
    if (targets > 1) power /= targets;
    if (target) {
        raw = power*2000/((int64_t)(b->level+20)*(ability(b,ABIL_STA)+100));
        raw = raw*25/(resistance(s->element,b->element)+25);
    } else {
        raw = power*8000/((int64_t)(b->level+40)*(ability(b,ABIL_STA)+200));
        raw = raw*25/35;
    }
    raw = raw*200/(b->defense/2+200);
    d = clamp(raw,1,32000); base = clamp((int64_t)d*9/10,1,32000);
    if (d > 1) d = roll(d)/5+base;
    if (!roll(50)) d = (actor ? 2 : roll(3)+2)*base;
    return clamp(d,1,32000);
}
static void apply_spell(void)
{
    const SpellDef *s = &g_world.spells[fight.spell];
    int i, targets = 0, actor = fight.attacker;
    if (fight.fizzle) return;
    if (s->damage <= -200) {
        Combatant *a = &fight.actors[actor];
        int monster = 0, choices = 0;
        if (s->damage == -300) {
            if (fight.victim == 0) hero_give_item(&g_hero,s->summon_id,1);
        } else {
            if (s->damage == -203) monster = s->summon_id;
            else if (s->damage == -200) monster = a->id;
            else for (i = 1; i < WORLD_MAX_MONSTERS; ++i) {
                const MonsterDef *m = &g_world.monsters[i];
                if (m->used && m->level <= a->level && m->level >= a->level-10 &&
                    (s->damage != -201 || m->element == a->element) && !roll(++choices)) monster = i;
            }
            if (monster > 0) spawn(a->ally ? -monster : monster);
        }
        wos_log_event("spell_cast","caster=%s spell=%d target=%d dmg=0",actor?"monster":"hero",fight.spell,fight.victim);
        return;
    }
    for (i = 0; i < fight.count; ++i)
        if ((i == fight.victim || s->all_targets) &&
            fight.actors[i].ally == fight.actors[fight.victim].ally &&
            legal_target(actor,i,fight.spell)) ++targets;
    for (i = 0; i < fight.count; ++i) {
        Combatant *b = &fight.actors[i];
        int damage = 0, alive = b->hp > 0;
        if ((i != fight.victim && !s->all_targets) ||
            b->ally != fight.actors[fight.victim].ally || !legal_target(actor,i,fight.spell)) continue;
        if (s->damage == -1) b->hp = 1;
        else if (s->damage <= -120 && s->damage >= -124) {
            int stat = -s->damage-120;
            b->ailments &= ~(1u<<(20+stat));
            b->ability_shift[stat] = clamp((int64_t)b->ability_shift[stat]-1,-255,255);
        } else if (s->damage <= -102 && s->damage >= -119) b->ailments &= ~(1u<<(-s->damage-100));
        else if (s->damage <= -2 && s->damage >= -24) {
            b->ailments |= 1u<<(-s->damage);
            if (s->damage <= -20) {
                int stat = -s->damage-20;
                b->ability_shift[stat] = clamp((int64_t)b->ability_shift[stat]+1,-255,255);
            }
            if (s->damage == -9 && i) b->ally = fight.actors[actor].ally;
        }
        else {
            int miss = spell_helpful(s) ? 0 : clamp(ability(b,ABIL_AGI)-ability(&fight.actors[actor],ABIL_DEX),0,50);
            if (miss && roll(100) < miss) { b->damage = INT_MIN; b->floating = FLOAT_FRAMES; continue; }
            damage = spell_damage(actor,i,fight.spell,targets);
            b->hp = clamp((int64_t)b->hp-damage,0,b->max_hp);
        }
        b->damage = damage; b->floating = FLOAT_FRAMES;
        if (!i) { g_hero.hp = b->hp; g_hero.ailments = b->ailments; }
        if (alive && !b->hp && !b->ally) {
            fight.xp = clamp((int64_t)fight.xp+b->xp,0,INT_MAX);
            fight.gold = clamp((int64_t)fight.gold+roll(b->gold+1)+b->gold/2,0,32767);
        }
        wos_log_event("spell_cast","caster=%s spell=%d target=%d dmg=%d",actor?"monster":"hero",fight.spell,i,damage);
    }
}
void battle_open_spells(void)
{
    int i;
    if (!battle_active()) return;
    fight.spell_count = 1; fight.known[0] = 0;
    for (i = 1; i < WORLD_MAX_SPELLS; ++i)
        if (hero_spell_known(&g_hero,i)) fight.known[fight.spell_count++] = i;
    fight.spell_selection = 0; fight.spell_menu = 1; fight.queued = 0;
}
static void start_attack(int actor, int target)
{
    Combatant *a = &fight.actors[actor];
    int spell = actor ? monster_spell(actor,&target) : fight.chosen_spell;
    int bound = 0, cost = 0;
    const SpellDef *s = NULL;
    if (!actor && !spell && g_hero.right_hand > 0 && g_hero.right_hand < WORLD_MAX_ITEMS) {
        spell = g_world.items[g_hero.right_hand].spell_binding;
        bound = spell > 0;
    }
    if (spell < 0 || spell >= WORLD_MAX_SPELLS || (spell && !g_world.spells[spell].used)) spell = 0;
    if (spell) {
        s = &g_world.spells[spell];
        if (target < 0 || !legal_target(actor,target,spell)) target = spell_target(actor,spell);
        cost = bound ? 0 : spell_cost(a,s);
        if (target < 0 || cost > a->mp || (a->ailments&(1u<<5)) ||
            (!actor && !bound && !hero_spell_known(&g_hero,spell))) {
            if (!actor) { snprintf(fight.message,sizeof fight.message,"Cannot cast: target, MP or silence"); fight.queued = 0; }
            else a->ready = 0;
            return;
        }
    }
    if (a->ailments & ((1u<<4)|(1u<<7))) { a->ready = 0; return; }
    if (!spell && (a->ailments&(1u<<6))) { a->ready = 0; return; }
    fight.attack_training = fight.attack_pp = 0;
    if (!actor) {
        int hand = g_world.classes[g_hero.klass].right_hand-1;
        int item = g_hero.right_hand, category, kind, gauge, *pp;
        int64_t effective;
        if (item > 0 && item < WORLD_MAX_ITEMS && g_world.items[item].klass >= 12 && g_world.items[item].klass <= 19)
            hand = g_world.items[item].klass-12;
        kind = spell && !bound ? HERO_TRAIN_ELEMENT : HERO_TRAIN_HAND;
        category = kind == HERO_TRAIN_ELEMENT ? (s->element < 8 ? s->element : 4) : hand;
        gauge = kind == HERO_TRAIN_ELEMENT ? fight.mp_gauge : fight.hp_gauge;
        pp = kind == HERO_TRAIN_ELEMENT ? g_hero.element_pp : g_hero.hand_pp;
        effective = category >= 0 && category < 8 ? (int64_t)(gauge+15)*pp[category]/30 : 0;
        fight.attack_pp = clamp(effective,0,INT_MAX-5000);
        fight.attack_training = effective > 0 ? (int)(10000*effective/(effective+5000)) : 0;
        if (category >= 0 && category < 8) {
            int gain = kind == HERO_TRAIN_ELEMENT ? s->req_affinity*10+20 : 20;
            hero_gain_training(&g_hero,kind,category,gain);
        }
        if (kind == HERO_TRAIN_ELEMENT) {
            fight.mp_gauge = fight.mp_initial = 0; fight.mp_start = fight.frame;
        } else { fight.hp_gauge = fight.regen_initial = 0; fight.regen_start = fight.frame; }
        if (!s || !spell_helpful(s)) hero_add_pp(&g_hero,a->ability[ABIL_WIS]*200/255+5);
        fight.queued = 0;
        fight.message[0] = 0;
    }
    a->mp -= cost;
    if (!actor) g_hero.mp = a->mp;
    fight.spell = spell; fight.bound_spell = bound; fight.fizzle = 0;
    fight.duration = spell ? 240 : LUNGE_FRAMES; /* 0048fe80: physical1s, spell4s. */
    if (spell) {
        int ratio = actor ? clamp(ability(a,ABIL_WIS)/4+50,0,100) : g_world.classes[g_hero.klass].magic_ratio;
        if (s->element > 0 && s->element < 8 && roll(100) >= cast_success(ratio,fight.attack_pp)) {
            fight.fizzle = 1;
            wos_log_event("spell_fizzle","caster=%s spell=%d target=%d",actor?"monster":"hero",spell,target);
            snprintf(fight.message,sizeof fight.message,"%s fizzles",s->name);
        }
    } else fight.damage = physical_damage(a,&fight.actors[target],actor != 0);
    a->last_action = fight.frame; a->ready = 0; ++a->attacks;
    fight.attacker = actor; fight.victim = target; fight.animation = 0;
    fight.state = ROUND_DAMAGE; fight.cursor = (actor+1)%fight.count;
}
static int inside(Rect r, int x, int y)
{
    return x >= r.x && y >= r.y && x-r.x < r.w && y-r.y < r.h;
}
static Rect actor_rect(int i)
{
    const Combatant *a = &fight.actors[i];
    const Sheet *s = &fight.sheets[a->sheet_index];
    int cell = s->cell > 0 ? s->cell : 48;
    Rect v = fight.view;
    return (Rect){v.x+(a->x-cell/2)*v.w/360,v.y+(a->y-cell)*v.h/256,
                  cell*v.w/360,cell*v.h/256};
}
static void spell_menu_update(const Input *in)
{
    int i, page = fight.spell_selection/9*9;
    if (!in) return;
    if (in->pressed[PLAT_KEY_ESCAPE] || in->pressed['s']) { fight.spell_menu = 0; return; }
    if (in->pressed[PLAT_KEY_UP] && fight.spell_selection) --fight.spell_selection;
    if (in->pressed[PLAT_KEY_DOWN] && fight.spell_selection+1 < fight.spell_count) ++fight.spell_selection;
    for (i = 0; i < 9; ++i) if (in->pressed['1'+i] && page+i < fight.spell_count) fight.spell_selection = page+i;
    if (in->mouse_pressed&(1u<<1)) {
        Rect v = fight.view;
        for (i = 0; i < 9 && page+i < fight.spell_count; ++i)
            if (inside((Rect){v.x+12,v.y+40+i*22,v.w-24,22},in->mouse_x,in->mouse_y))
                fight.spell_selection = page+i;
    }
    if (in->pressed[PLAT_KEY_RETURN]) {
        fight.chosen_spell = fight.known[fight.spell_selection];
        fight.target = spell_target(0,fight.chosen_spell);
        fight.spell_menu = 0;
    }
}
BattleResult battle_update(const Input *in)
{
    int i, actor = -1;
    if (fight.result != BATTLE_RUNNING) return fight.result;
    if (fight.spell_menu) { spell_menu_update(in); return fight.result; }
    if (in && in->pressed['s']) { battle_open_spells(); return fight.result; }
    fight.actors[0].hp = g_hero.hp; fight.actors[0].mp = g_hero.mp;
    fight.actors[0].ailments = g_hero.ailments;
    ++fight.frame;
    for (i = 0; i < fight.count; ++i) if (fight.actors[i].floating) --fight.actors[i].floating;
    if (outcome()) return fight.result;
    if (in && (in->pressed['f'] || in->pressed[PLAT_KEY_ESCAPE])) {
        finish(BATTLE_FLED); return fight.result;
    }
    fight.hp_gauge = clamp(fight.regen_initial+(int64_t)(fight.frame-fight.regen_start)*
                           (fight.actors[0].ability[ABIL_STR]/2+100)/600,0,100);
    fight.mp_gauge = clamp(fight.mp_initial+(int64_t)(fight.frame-fight.mp_start)*
                           (fight.actors[0].ability[ABIL_WIS]/2+100)/600,0,100);
    if (fight.target < 0 || !legal_target(0,fight.target,fight.chosen_spell))
        fight.target = spell_target(0,fight.chosen_spell);
    if (in) {
        if (in->pressed[PLAT_KEY_TAB]) {
            for (i = 1; i <= fight.count; ++i) {
                int t = (fight.target+i)%fight.count;
                if (legal_target(0,t,fight.chosen_spell)) { fight.target = t; break; }
            }
        }
        if (in->pressed['a'] || in->pressed[PLAT_KEY_SPACE]) fight.queued = 1;
        if (in->mouse_pressed & (1u << 1)) {
            for (i = fight.count-1; i >= 0; --i) {
                if (legal_target(0,i,fight.chosen_spell) && inside(actor_rect(i),in->mouse_x,in->mouse_y)) {
                    fight.target = i; fight.queued = 1; break;
                }
            }
        }
    }
    if (fight.state == ROUND_START) {
        for (i = 0; i < fight.count; ++i) fight.actors[i].ready = fight.actors[i].hp > 0;
        fight.round_start = fight.frame;
        fight.state = ROUND_ACT;
    } else if (fight.state == ROUND_DAMAGE) {
        if (++fight.animation >= fight.duration) {
            Combatant *victim = &fight.actors[fight.victim];
            if (fight.spell) apply_spell();
            else {
                victim->damage = fight.damage < 0 ? INT_MIN : fight.damage;
                victim->floating = FLOAT_FRAMES;
                wos_log_event("battle_hit","frame=%d attacker=%d target=%d dmg=%d",
                              fight.frame,fight.attacker,fight.victim,fight.damage);
                if (fight.damage >= 0) victim->hp = clamp((int64_t)victim->hp-fight.damage,0,victim->max_hp);
                if (fight.victim == 0) g_hero.hp = victim->hp;
                else if (!victim->hp && !victim->ally) {
                    fight.xp = clamp((int64_t)fight.xp+victim->xp,0,INT_MAX);
                    fight.gold = clamp((int64_t)fight.gold+roll(victim->gold+1)+victim->gold/2,0,32767);
                }
            }
            fight.attacker = fight.victim = -1;
            fight.state = ROUND_ACT;
            outcome();
        }
    } else if (fight.frame-fight.round_start >= ACTION_GATE) {
        int ready = 0;
        for (i = 0; i < fight.count; ++i) {
            int n = (fight.cursor+i)%fight.count;
            if (fight.actors[n].hp > 0 && fight.actors[n].ready) {
                ready = 1;
                if (n != 0 || fight.queued) { actor = n; break; }
            }
        }
        if (actor >= 0) {
            int target = actor == 0 ? fight.target : ai_target(actor);
            if (target >= 0) start_attack(actor,target);
        } else if (!ready || fight.frame-fight.round_start >= ROUND_IDLE) fight.state = ROUND_START;
    }
    return fight.result;
}
/* Bhaskara's integer sine approximation over [0,pi], endpoints 0, midpoint 256. */
static int lunge(int t)
{
    int p = t*(LUNGE_FRAMES-t);
    return 16*p*256/(5*LUNGE_FRAMES*LUNGE_FRAMES-4*p);
}
static void sprite(Framebuffer *fb, const Sheet *s, int pose, int x, int y, int flip, Rect v)
{
    int dx, dy, width, height, left, top;
    if (!s->image.pixels || s->cell < 1 || s->count < 1) return;
    pose = clamp(pose,0,s->count-1);
    width = s->cell*v.w/360; height = s->cell*v.h/256;
    left = v.x+(x-s->cell/2)*v.w/360; top = v.y+(y-s->cell)*v.h/256;
    for (dy = 0; dy < height; ++dy) {
        int sy = dy*s->cell/height;
        for (dx = 0; dx < width; ++dx) {
            int sx = dx*s->cell/width;
            uint32_t color;
            if (flip) sx = s->cell-1-sx;
            color = s->image.pixels[sy*s->image.w+pose*s->cell+sx];
            if ((int64_t)color != s->key) fb_pixel(fb,left+dx,top+dy,color);
        }
    }
}
void battle_render(Framebuffer *fb, Rect view)
{
    int i;
    Rect old_clip = fb->clip;
    if (view.w <= 0 || view.h <= 0 || fight.result == BATTLE_NONE) return;
    fight.view = view;
    fb_clip_intersect(fb,view);
    for (i = 0; i < fight.count; ++i) {
        Combatant *a = &fight.actors[i];
        const Sheet *s = &fight.sheets[a->sheet_index];
        int x = a->x, y = a->y, pose = 1, flip = i == 0;
        int sx, sy, w;
        char label[80];
        const char *name = i == 0 ? g_hero.name : g_world.monsters[a->id].name;
        if (a->hp <= 0 && !a->floating) continue;
        if (a->hp < a->max_hp/4 || a->floating > FLOAT_FRAMES-18) pose = 3;
        if (fight.attacker == i) {
            Combatant *b = &fight.actors[fight.victim];
            int fraction = fight.spell ? 0 : lunge(fight.animation);
            x += (b->x-a->x)*fraction/256;
            y += (b->y-a->y)*fraction/256;
            pose = 2;
            if (i != 0) flip = b->x < a->x;
        }
        sprite(fb,s,pose,x,y,flip,view);
        sx = view.x+a->x*view.w/360;
        sy = view.y+(a->y-(s->cell > 0 ? s->cell : 48))*view.h/256;
        w = 56*view.w/360;
        if (i == fight.target && a->hp > 0) fb_rect(fb,actor_rect(i),0xffdc60);
        fb_fill(fb,(Rect){sx-w/2,sy-8,w,5},0x301818);
        fb_fill(fb,(Rect){sx-w/2,sy-8,a->max_hp ? w*a->hp/a->max_hp : 0,5},a->ally?0x50d080:0xe05050);
        snprintf(label,sizeof label,"%.20s",name);
        font_draw(fb,sx-font_width(label)/2,sy-18,label,0xffffff);
        if (a->floating) {
            if (a->damage == INT_MIN) snprintf(label,sizeof label,"MISS");
            else if (a->damage < 0) snprintf(label,sizeof label,"+%d",-a->damage);
            else snprintf(label,sizeof label,"-%d",a->damage);
            font_draw(fb,sx-font_width(label)/2,sy-30-(FLOAT_FRAMES-a->floating)/4,label,0xffff70);
        }
    }
    if (fight.state == ROUND_DAMAGE && fight.spell && !fight.fizzle) {
        static const uint32_t colors[8] = {0x80ff80,0x60a0ff,0x40d070,0xb08040,0xb080d0,0xff7040,0xffffff,0xffff70};
        int el = g_world.spells[fight.spell].element;
        Rect r = actor_rect(fight.victim);
        fb_blend(fb,r,colors[el&7],(unsigned)(40+(fight.animation%30)*4));
    }
    font_draw(fb,view.x+6,view.y+view.h-36,fight.message,0xffdc80);
    font_draw(fb,view.x+6,view.y+view.h-24,"A/SPACE: Act TAB: Target S: Spells",0xffffff);
    font_draw(fb,view.x+6,view.y+view.h-12,"F: Flee",0xffffff);
    if (fight.chosen_spell) font_draw(fb,view.x+6,view.y+8,g_world.spells[fight.chosen_spell].name,0xffff80);
    if (fight.spell_menu) {
        int page = fight.spell_selection/9*9;
        char line[96];
        fb_fill(fb,(Rect){view.x+8,view.y+8,view.w-16,258},0x182030);
        fb_rect(fb,(Rect){view.x+8,view.y+8,view.w-16,258},0x90b0d0);
        snprintf(line,sizeof line,"Spells - MP %d/%d",g_hero.mp,g_hero.max_mp);
        font_draw(fb,view.x+16,view.y+20,line,0xffffff);
        for (i = 0; i < 9 && page+i < fight.spell_count; ++i) {
            int id = fight.known[page+i];
            if (page+i == fight.spell_selection)
                fb_fill(fb,(Rect){view.x+12,view.y+40+i*22,view.w-24,22},0x405878);
            if (id) snprintf(line,sizeof line,"%d %.27s MP:%d",i+1,g_world.spells[id].name,g_world.spells[id].mp_cost);
            else snprintf(line,sizeof line,"%d Physical attack",i+1);
            font_draw(fb,view.x+16,view.y+47+i*22,line,0xffffff);
        }
        font_draw(fb,view.x+16,view.y+244,"UP/DOWN  1-9  ENTER  ESC:Cancel",0xffffff);
    }
    fb_clip(fb,old_clip);
}
BattleResult battle_result(void) { return fight.result; }
int battle_active(void) { return fight.result == BATTLE_RUNNING; }
