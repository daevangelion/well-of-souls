/* Solo combat. Reference: Souls.exe FUN_00480499, FUN_00490e7c, FUN_00490723, FUN_0048f913,
 * FUN_0048f816, FUN_004904eb, FUN_004a7794, FUN_004a6974, FUN_00436c9d, FUN_0042b867,
 * FUN_0042bb5c, FUN_00494fcd; docs/re/battle.md and docs/re/rng_calls.md section 2.3.
 * All pacing is the original's GetTickCount arithmetic through clock_ms(); there is no frame
 * model (docs/re/timing.md). Every rand() is one crt_rand() at the same point, in the same order. */
#include "battle.h"
#include "front.h"
#include "game.h"
#include "hero.h"
#include "items.h"
#include "scene.h"
#include "../engine/clock.h"
#include "../engine/encint.h"
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
       /* FUN_00490723: GetTickCount() - roundStart > 2000 with nobody acting ends the round. */
       ROUND_IDLE_MS = 2000,
       /* FUN_0048b17a: the hero may not commit during the first 500 ms of a round it is in. */
       HERO_GATE_MS = 500,
       /* FUN_0048f913: GetTickCount() - rec[0x45C] > 1999 before a monster may flee or wander. */
       MONSTER_IDLE_MS = 2000,
       /* FUN_0048fe80: scene[0x3E05C], the shared attack duration. */
       PHYSICAL_MS = 1000, SPELL_MS = 4000, SPECIAL_MS = 1250,
       /* Damage-number float. Presentation only. */
       FLOAT_MS = 1500,
       /* FUN_004a4d70 `default: *param_5 = 0x10`, the scatter radius of a physical attack. */
       THROW_RADIUS = 16 };
/* FUN_00436c9d: the hero cannot simply walk out of a fight. */
enum { FLEE_LOCK_MS = 30000,        /* DAT_00502b14 == 1 or 2 (Tactics / PK)             */
       FLEE_LOCK_FIGHT2_MS = 20000, /* DAT_00502b14 == 3 (FIGHT2), and only undecided    */
       FLEE_INTERRUPT_PCT = 30 };   /* interrupting an attack already aimed at the hero */
/* DAT_00502b14, the sticky-fight mode. Offline random encounters are 0. */
enum { STICKY_NONE = 0, STICKY_TACTICS = 1, STICKY_PK = 2, STICKY_FIGHT2 = 3 };
/* Combatant turn state, rec[+0x38C]. */
enum { TURN_DONE = 0x2f, TURN_READY = 0x5f, TURN_COMMITTED = 0x94 };

typedef struct {
    int id, ally, hp, max_hp, mp, level, offense, defense, ability[5], element;
    uint32_t ailments;
    int ability_shift[HERO_ABILITIES];
    int xp, gold, x, y, ready, attacks, damage, floating;
    int last_action;      /* GetTickCount() of the last action, rec[0x47C] */
    int sheet_index;
    int ability_a, ability_b; /* rec[0x440] / rec[0x444], the two combat abilities */
    int rating;            /* rec[0x6D8], FUN_00491bb7's derived attack rating */
    int owner;             /* rec[+4]: -1 monster, >0 player account */
    int turn;             /* rec[+0x38C] */
    /* FUN_0049b71B/FUN_0049b734/FUN_0049b73F sealed companions. The original stores these fields
     * as encrypted ints: a plain int is the value (enc_get is free) and the EncInt beside it is
     * the seal, and every SET or ADD spends exactly 4 crt_rand() in the order k0,k1,k2,k3. The
     * site counts below are the original's own - see each call. */
    EncInt enc_hp, enc_max_hp, enc_mp, enc_level, enc_offense, enc_defense, enc_rating;
    EncInt enc_participation; /* rec[0x11A], FUN_0048b0c7 */
    EncInt enc_xp, enc_gold;   /* rec[0xAD]/rec[0xAC], FUN_0042bb5c's payout seals */
    int fled;             /* rec[+0x1B4]: already walking off, never re-rolled */
    int participation;    /* rec[0x11A] */
    uint32_t last_turn;   /* rec[+0x45C], GetTickCount() of the last turn */
} Combatant;
static struct {
    Combatant actors[ACTORS];
    Sheet sheets[ACTORS];
    int count, sheets_count, state, cursor, target;
    uint32_t tick, round_start, anim_start, flee_lock, hp_gauge_start, mp_gauge_start;
    int queued, attacker, victim, animation, hp_gauge, mp_gauge, regen_start;
    int duration_ms, bound_spell;
    int regen_initial, xp, gold, damage, attack_training;
    int spell, chosen_spell, spell_menu, spell_selection, spell_count, known[WORLD_MAX_SPELLS];
    int mp_start, mp_initial, attack_pp, attack_rating, fizzle, mods, sticky, pets, engaged;
    int throw_item, throw_radius, target_x, target_y;   /* FUN_004a4d70 / FUN_004a3a54 */
    int participation_total;  /* DAT_00502830 */
    int scene_kills;          /* DAT_004e4874 + 0x3E034: monsters killed this fight */
    int monster_kills[WORLD_MAX_MONSTERS]; /* DAT_00d2c7d8, zeroed with the fight block */
    int auto_resurrect;       /* DAT_00502a48: one free solo resurrect, cleared once spent */
    int killer_id;             /* DAT_00502b04: the monster that last struck the hero */
    int decided;              /* FUN_0048fd90 has already returned non-zero */
    char message[96];
    BattleResult result;
    Rect view;
} fight;

static BattleSceneEvent scene_event;
static int compute_rating(const Combatant *a);
/* battle_set_pets() runs before battle_begin_ex(), which resets the fight block, so the FIGHT
 * '+n' pet count has to live outside it. */
static int pending_pets;

static int clamp(int64_t n, int low, int high)
{
    return n < low ? low : n > high ? high : (int)n;
}
/* FUN_0049b71B: seal `value` into `e` (4 crt_rand) and publish it to the plain mirror the rest
 * of the port reads. The original's writes go through the encrypted object and its reads go
 * through FUN_0049b70F, which costs no rand, so the plain int is the authoritative value. */
static void enc_put(EncInt *e, int *mirror, int value)
{
    enc_set(e,value);
    *mirror = value;
}
/* Every rand() in the original is exactly one crt_rand() here, in the same order
 * (docs/re/rng_calls.md section 2.3). */
static int rand1(void) { return crt_rand(); }
/* rand() % bound. Always exactly one crt_rand(), so the count never drifts. */
static int roll(int bound) { int r = crt_rand(); return bound > 0 ? r % bound : 0; }
/* GetTickCount() arithmetic; the virtual tick wraps like the original's 32-bit one. */
static uint32_t tick_now(void) { return clock_ms(); }
static int past(uint32_t t0, uint32_t ms) { return (int32_t)(tick_now() - t0) > (int32_t)ms; }
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
    /* FUN_00480499 sets rec[0xAA]/[0xAB] (HP/maxHP), [0x3E] (MP), [0x40] (offense) and [0x3F]
     * (defense) through FUN_0049b71B - 5 seals, 20 rands, at 0x480593/5B0/5CA/5DB/604 - and then
     * FUN_0049b71b(rec[0x3B]) seals the level. Order matters: HP, MP, offense, defense, level. */
    enc_put(&a->enc_hp,&a->hp,derived(m->hp, a->level, 1162, s->hp));
    enc_put(&a->enc_max_hp,&a->max_hp,a->hp);
    enc_put(&a->enc_mp,&a->mp,derived(m->mp, a->level, 513, s->mp));
    enc_put(&a->enc_offense,&a->offense,derived(m->offense, a->level, 567, s->offense));
    enc_put(&a->enc_defense,&a->defense,derived(m->defense, a->level, 834, s->defense));
    enc_put(&a->enc_level,&a->level,a->level);
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
/* DAT_005078A0, the spell-effect table FUN_004a6d32 sums. It is INITIALISED IN THE IMAGE, not
 * filled at runtime: 25 rows of 0x150 bytes, each {int pad; int id; ...; five counters at
 * +0x14..+0x24}, i.e. the counters sit at row+0x10 counted from the id at 0x5078A4 - which is why
 * FUN_004a6a46 walks `&DAT_005078A4` and why FUN_004a6b55 computes `row*0x150 + 0x5078A0`. No code
 * ever writes the range: `tools/ghidra/query.sh writes 0x5078a0 0x3c64` returns four hits, all
 * at 0x50b500, which is FUN_004a7794/FUN_004a8677's unrelated "effect applied" flag.
 * FUN_004a6a46's `piVar2 += 0x54` with its `&DAT_0050b504` bound is a decompilation artefact of
 * the same walk; the real stride is 0x150 and 0x50b504 is the end of the 25 rows.
 *
 * Counter 0 blocks fleeing, the fear roll and the wander step - FUN_0048f816, FUN_00436c9d and
 * FUN_0048f913 all read FUN_004a6d32's first out. Counter 1 marks a magic-only actor, which
 * FUN_0048f913 also reads. The other three are read by nothing on the fight path. */
typedef struct { short id; unsigned char counter[5]; } EffectRow;
static const EffectRow effect_table[] = {
    { -2,  {0,0,0,1,0} }, { -3,  {0,0,0,0,1} }, { -4,  {1,1,1,0,0} },
    { -5,  {0,0,1,0,0} }, { -6,  {0,1,0,0,0} }, { -7,  {1,1,1,0,0} },
    { -8,  {0,0,0,0,0} }, { -9,  {0,0,0,0,0} }, { -10, {0,0,0,0,0} },
    { -11, {1,0,0,0,0} }, { -20, {0,0,0,0,0} }, { -21, {0,0,0,0,0} },
    { -22, {0,0,0,0,0} }, { -23, {0,0,0,0,0} }, { -24, {0,0,0,0,0} },
    { -25, {0,0,0,0,0} }, { -26, {0,0,0,0,0} }, { -27, {0,0,0,0,0} },
    { -28, {0,0,0,0,0} }, { -29, {0,0,0,0,0} }, { -30, {0,0,0,0,0} },
    { -31, {0,0,0,0,0} }, { -32, {0,0,0,0,0} }, { -102,{0,0,0,0,0} },
    { -103,{0,0,0,0,0} }
};
/* A combatant's ailments word is FUN_004a6a6a's per-slot effect list, rec[0x398 + slot*4], so bit
 * n is effect id -n. FUN_004a6d32 adds up the matching rows' counters. */
static int effect_count(const Combatant *a, int which)
{
    int n, total = 0;
    for (n = 0; n < 34; ++n) {
        int i;
        if (!(a->ailments & (1u<<n))) continue;
        for (i = 0; i < (int)(sizeof effect_table / sizeof effect_table[0]); ++i)
            if (effect_table[i].id == -n) { total += effect_table[i].counter[which]; break; }
    }
    return total;
}
/* FUN_004a6d32's first counter: this actor will not run, and will not flee. */
static int cowardice(const Combatant *a) { return effect_count(a,0) != 0; }
/* FUN_004a6d32's second counter: a magic-only actor. */
static int magic_only(const Combatant *a) { return effect_count(a,1) != 0; }
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
    a->owner = -1;
    a->ability_a = clamp(ability(a,ABIL_WIS)/4+50,0,100);  /* FUN_00480499 */
    a->ability_b = 100;
    a->turn = TURN_DONE;
    a->fled = 0;
    a->participation = 0;
    monster_stats(a, m);
    /* FUN_00491e45: rec[0x11F] = GetTickCount() - rand()%5000 - 3000. One rand. */
    a->last_action = (int)(tick_now() - (uint32_t)roll(5000) - 3000u);
    ordinal = (fight.count - 1) % 9;
    /* Nine-slot formation; logical ground y>=128 (battle.md section 3/4). The original's entry
     * offsets are tiny: FUN_00491e45 sets rec[0x9E] = -(rand()%64) and FUN_00480499 sets
     * rec[0x9E] = FUN_0048b13c() + rand()%32, both in 1/256 of the scene width, so a combatant
     * is at most a quarter of a logical pixel off its slot and FUN_004915ed walks it the rest of
     * the way over the next few frames. The port draws at the formation slot straight away and
     * spends both randoms, which is what parity needs; drawing them off-screen instead would
     * trip FUN_0048f913's `x < -10` removal and the flee checks on every monster. */
    roll(64);
    if (a->ally) {
        /* FUN_00480499's `param_2 == 1` arm: a second rand for the entry offset, then
         * rec[0x114] = 0x7FFFFFFF and the ally's own two sealed writes. This is the branch the
         * original takes for every mercenary and pet, so allies pay two extra sets - 8 rands -
         * that enemies do not. */
        roll(32);
        a->x = 55 + (ordinal % 3) * 36;
        enc_put(&a->enc_offense,&a->offense,a->offense);
        enc_put(&a->enc_defense,&a->defense,a->defense);
    } else a->x = 210 + (ordinal % 3) * 48;
    a->y = 152 + (ordinal / 3) * 40;
    a->last_turn = tick_now();
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
    wos_log_event("battle_actor","slot=%d monster=%d hp=%d level=%d offense=%d defense=%d str=%d sta=%d charge=%d",
                  fight.count,id,a->hp,a->level,a->offense,a->defense,
                  a->ability[ABIL_STR],a->ability[ABIL_STA],
                  (int)(tick_now()-(uint32_t)a->last_action));
    ++fight.count;
}
/* FUN_00464daf: the .mon proximity resolver. Inside a placement's radius the monster is spawned on
 * a 25 % roll; inside half of it a second copy on a 15 % roll; inside a quarter a third on 5 %.
 * The two falloff fractions are 0.5 and 0.25 (_DAT_004cd548 / _DAT_004cd578, read from .data).
 *
 * `tools/ghidra/query.sh callers FUN_00464daf` returns exactly one site, 0x004909EC inside
 * FUN_0049099b, which the fight state machine calls once at case 4. So the ORIGINAL resolves the
 * roster exactly once per encounter. If the map module already stashed one in the pending fight,
 * spend those ids rather than rolling a second, different roster - the extra roll would spend
 * randoms the original never spends and shift everything after it. FUN_00464daf also never comes
 * up empty, so this always yields at least one combatant. */
static int map_group(void)
{
    const Map *map = game_current_map();
    int i, before = fight.count, closest = -1;
    int64_t best = INT64_MAX;
    {
        int ids[MON_RECORDS+1], count = 0, difficulty = INT_MIN, pct = 0;
        game_take_pending_fight(ids,MON_RECORDS+1,&difficulty,&pct);
        for (i = 0; i < count; ++i) spawn(ids[i]);
        if (fight.count != before) return 1;
    }
    if (map) {
        for (i = 0; i < map->mon_count && i < MON_RECORDS; ++i) {
            int id = map->mons[i].monster_id;
            int64_t dx = ((int64_t)map->mons[i].x - g_hero.x) * 256;
            int64_t dy = ((int64_t)map->mons[i].y - g_hero.y) * 256;
            int64_t d2 = dx*dx + dy*dy, r = (int64_t)map->mons[i].radius * 256;
            if (id <= 0 || id >= WORLD_MAX_MONSTERS || !g_world.monsters[id].used) continue;
            if (r > 0 && d2 < r*r) {
                if (roll(100) < 25) spawn(id);
                if (d2 < r*r/2) { spawn(id); if (roll(100) < 15) spawn(id); }
                if (d2 < r*r/4) { spawn(id); if (roll(100) < 5) spawn(id); }
            }
            if (d2 < best) { best = d2; closest = id; }
        }
    }
    if (fight.count != before) return 1;
    if (closest > 0) { spawn(closest); return 1; }
    spawn(1);
    return 1;
}
/* FUN_0049099b: groups.txt by the link's signed difficulty. `pct` is the clamped 20..80
 * inclusion chance, already inverted for a negative difficulty by the map module. */
static int groups_group(int difficulty, int pct)
{
    const GroupDef *group;
    int i, n, before = fight.count, all;
    if (difficulty == INT_MIN) return 0;
    if (difficulty < 0) difficulty = -difficulty;
    if (difficulty < WORLD_MAX_GROUPS && g_world.groups[difficulty].used && difficulty) {
        group = &g_world.groups[difficulty];
        n = clamp(group->count,0,GROUP_MAX_MEMBERS);
        all = g_world.groups[0].used && g_world.groups[0].count > 0; /* DAT_00502a38 */
        for (i = 0; i < n; ++i) if (all || roll(100) < pct) spawn(group->members[i]);
        if (fight.count == before && n) {
            int start = roll(n);
            for (i = 0; i < n && fight.count == before; ++i) spawn(group->members[(start+i)%n]);
        }
    }
    return fight.count != before;
}
void battle_set_pets(int pets)
{
    pending_pets = pets < 0 ? 0 : pets;
}
void battle_set_scene_event(BattleSceneEvent fn) { scene_event = fn; }
void battle_hero_reseal_level(void)
{
    /* FUN_0048AE32 is a pure lookup - it walks DAT_004e4874 + 0x128 at stride 0x6E0 for the first
     * live combatant whose rec[+4] == the hero serial - and it bails out with NULL unless
     * FUN_0041bd7b() is set, so offline there is no hero combatant outside a fight. That is the
     * only correct behaviour for this call site, and it matches both originals: a hero with no
     * live combatant spends ZERO rands here, so callers invoke it unconditionally.
     *
     * FUN_0041BD7B, the gate, is a FRONT-END-STATE predicate, not a connectivity check - three
     * of us misread it as one and nearly deleted a correct flee port over it. DAT_004E483C is
     * the CSoulsView window pointer, assigned only at view construction (all.c:17996) and at
     * destruction (all.c:20105), so it is non-NULL for the whole life of a running game.
     * DAT_004DF8A4 is the front state (labels.csv 0041B891: 5 = well, 7 = scene). So the whole
     * expression reduces to: return 0 if there is no main window, or if the state is neither 5
     * nor 7 and no live soul occupies a combatant slot. A fight runs in a scene, i.e. state 7,
     * where it returns 1 - which is why FUN_0048B1AD can find the hero's combatant offline and
     * why FUN_00436C9d's `rec == 0 -> may flee` arm is NOT reached during a real fight. Do not
     * "simplify" this guard to a server check.
     *
     * CALLERS: this must stay the LAST thing on the path. At 0x47AEA1 the seal follows the whole
     * derived-stat recomputation between 0x47AE2A and 0x47AEA1 (the +0x68/+0x690 clamp,
     * FUN_00449AA2, FUN_0040E04D, FUN_0040C694), and at 0x484E0B it follows the hero record being
     * copied at 0x484DF0..0x484DF3. Moving either call earlier spends the 4 draws in the wrong
     * place and shifts everything after it. Do not hoist it. */
    if (fight.result == BATTLE_NONE || fight.count < 1) return;
    enc_put(&fight.actors[0].enc_level,&fight.actors[0].level,g_hero.level);
}
/* FUN_0048e19a clears the fight block and the per-fight kill table, and jitters the block's
 * base with one rand: `rand()%0x3E0A0 & 0xFFFFFFF0`. */
void battle_begin_ex(const int *ids, int count, int difficulty, int distance_pct, unsigned mods)
{
    Combatant *hero;
    int i;
    for (i = 0; i < fight.sheets_count; ++i) sheet_free(&fight.sheets[i]);
    memset(&fight,0,sizeof fight);
    rand1(); /* FUN_0048e19A: rand()%0x3E0A0 & 0xFFFFFFF0 */
    fight.view = (Rect){0,0,364,416};
    fight.count = fight.sheets_count = 1;
    fight.state = ROUND_START;
    fight.result = BATTLE_RUNNING;
    fight.attacker = fight.victim = -1;
    fight.engaged = 1;              /* state 4 sets scene[0x78] = 1 */
    fight.auto_resurrect = 1;      /* DAT_00502a48 */
    fight.mods = mods | (pending_pets > 0 ? (unsigned)BATTLE_MOD_PET_HATE : 0u);
    fight.pets = pending_pets;
    fight.sticky = (mods & BATTLE_MOD_STICKY) ? STICKY_FIGHT2 : STICKY_NONE;
    fight.tick = tick_now();
    fight.round_start = fight.flee_lock = fight.tick;  /* _DAT_00502b18 */
    fight.hp_gauge_start = fight.mp_gauge_start = fight.tick; /* _DAT_004f91cc / d0 */
    fight.hp_gauge = fight.mp_gauge = 25;
    fight.regen_initial = 25;
    fight.mp_initial = 25;
    distance_pct = clamp(distance_pct,0,100);
    hero = &fight.actors[0];
    hero->ally = 1;
    /* FUN_00491E45 seals rec[0xAA], [0xAC], [0xAD] and rec[0x3B] at 0x00491F35/41/4D/59 - four
     * seals, 16 rands - for the hero combatant. */
    enc_put(&hero->enc_hp,&hero->hp,clamp(g_hero.hp,0,g_hero.max_hp));
    enc_put(&hero->enc_max_hp,&hero->max_hp,g_hero.max_hp);
    enc_put(&hero->enc_mp,&hero->mp,g_hero.mp);
    enc_put(&hero->enc_level,&hero->level,clamp(g_hero.level,1,65535));
    for (i = 0; i < HERO_ABILITIES; ++i) hero->ability[i] = hero_ability(&g_hero,i);
    hero->ailments = g_hero.ailments;
    hero->x = 80; hero->y = 204;
    hero->offense = hero_offense(&g_hero);
    hero->defense = hero_defense(&g_hero);
    hero->turn = TURN_DONE;
    hero->owner = 1;                     /* solo: the local player's account id */
    hero->ability_a = 100;
    hero->ability_b = 100;
    hero->rating = compute_rating(hero);
    sheet_load_skin(&fight.sheets[0],g_hero.skin);
    if (count > 0 && ids) {
        for (i = 0; i < count && i < ACTORS-1; ++i) spawn(ids[i]);
        /* FIGHT * (0x47f9c8): the listed monsters PLUS the normal random encounter. */
        if (mods & BATTLE_MOD_RANDOM) {
            int map_ids[GROUP_MAX_MEMBERS+1], map_count = 0;
            int map_diff = difficulty, map_pct = distance_pct;
            game_take_pending_fight(map_ids,GROUP_MAX_MEMBERS+1,&map_diff,&map_pct);
            if (map_count) { int k; for (k = 0; k < map_count; ++k) spawn(map_ids[k]); }
            else if (!groups_group(map_diff,map_pct)) map_group();
        }
    } else if (!groups_group(difficulty,distance_pct)) map_group();
    fight.target = first_enemy();
    battle_music("fight");
    wos_log_event("battle_start","monsters=%d distance=%d sticky=%d pets=%d",
                  fight.count-1,distance_pct,fight.sticky,fight.pets);
}
void battle_begin(const int *ids, int count, int difficulty, int distance_pct)
{
    battle_begin_ex(ids,count,difficulty,distance_pct,0);
}
/* FUN_0042b867 from the victory path (all.c:32450-32490, called from FUN_0042bb5c at
 * all.c:32689-32692). The original draws exactly three rands, always all three - `rand()&0x400`
 * (the item-table step), the start index, and `1 + (rand()&1)` picks - then walks the whole item
 * table from that index, and for every candidate whose weight (items.txt arg13's "find
 * probability", a chance in 2000) is non-zero and whose bound spell is usable, draws
 * `rand()%2000 < weight`. A find needs the item within 5 levels of the hero and the highest-level
 * monster killed this fight within 11. The handover itself is a server call, so offline the only
 * effect is the "You found %s's %s" line. */
static void victory_find(int picks, int start, int step)
{
    int i, n, id = start % WORLD_MAX_ITEMS, top = 0;
    for (n = 1; n < WORLD_MAX_MONSTERS; ++n)          /* DAT_00d2c7d8, the fight's kill table */
        if (fight.monster_kills[n] > 0 && g_world.monsters[n].used && g_world.monsters[n].level > top)
            top = g_world.monsters[n].level;
    if (id < 1) id = 1;
    for (i = 0; i < picks; ++i) {
        for (n = 0; n < WORLD_MAX_ITEMS; ++n) {
            const ItemDef *item = &g_world.items[id];
            int spell = item->spell_binding;
            int weight = item->used ? item->find_probability : 0;
            id = (id + (step ? 1 : 0x13ff)) % WORLD_MAX_ITEMS;
            if (id < 1) id = 1;
            if (weight <= 0) continue;
            if (spell != 0 && (spell < 0 || spell >= WORLD_MAX_SPELLS || !g_world.spells[spell].used))
                continue;
            if (roll(2000) >= weight) continue;
            if (item->level - g_hero.level >= 5) continue;      /* hero[0x64] - item[0x250] < 5 */
            if (top - g_hero.level >= 0xb) continue;             /* top - hero[0x64] < 11 */
            wos_log_event("battle_found_item","item=%d name=%s",id,item->name);
            if (--picks < 1) return;
        }
    }
}
static void victory_trophy_roll(void)
{
    int step = rand1() & 0x400;
    int start = rand1();
    int picks = (rand1() & 1) + 1;
    victory_find(picks,start,step);
}
static void hero_died(void);
static void finish(BattleResult result)
{
    fight.result = result;
    /* FUN_0048fd90's caller stamps the post-fight grace (DAT_004e70a8) so the map's encounter
     * roll does not re-trigger on the very next step; without it the 1 s grace never expires. */
    map_note_battle_end();
    if (result == BATTLE_WON) {
        int xp = fight.xp, gold = fight.gold;
        int64_t old_xp = g_hero.xp, old_gold = g_hero.gold;
        /* NO_REWARD is the scene/map flag, docs/re/script.md FLAGS. */
        if ((scene_flags() & (1u << 11)) || !fight.actors[0].attacks) xp = gold = 0;
        if (g_hero.level < 96) {
            int64_t cap = (hero_xp_for_level(&g_hero,g_hero.level+2)-g_hero.xp+3)/2;
            xp = clamp(xp,0,clamp(cap,0,INT_MAX));
        }
        /* FUN_0048b0c7: every actor with an owner adds its own participation to both its record
         * and the fight total; the payout splits the pot by that ratio. */
        {
            int share_gold = fight.participation_total ?
                (int)((int64_t)gold*fight.actors[0].participation/fight.participation_total) : gold;
            int share_xp = fight.participation_total ?
                (int)((int64_t)xp*fight.actors[0].participation/fight.participation_total) : xp;
            wos_log_event("battle_share","participation=%d total=%d gold=%d xp=%d",
                          fight.actors[0].participation,fight.participation_total,share_gold,share_xp);
        }
        /* FUN_0042bb5c seals the hero's XP (hero[0x68]), gold (hero[0x6C]) and the level
         * (hero[0x64]) through FUN_0049b71b at 0x0042BF75/0x0042C0AB/0x0042C0B9 - 3 seals, 12
         * rands - before the trophy roll's three. */
        /* FUN_0042bb5c has THREE seals, and they come before the trophy roll's three rands:
         * the combatant's XP (rec[0xAD]) and gold (rec[0xAC]) are re-sealed with the awarded
         * values, then the level (rec[0xAE]) is re-sealed from FUN_0042b6c1's result. Ghidra
         * drops the this-pointer, so the decomp shows them as seals on the hero record; the hero
         * record has no EncInt, so they belong to the hero's combatant, which is actors[0]. */
        enc_put(&fight.actors[0].enc_xp,&fight.actors[0].xp,fight.xp);
        enc_put(&fight.actors[0].enc_gold,&fight.actors[0].gold,fight.gold);
        enc_put(&fight.actors[0].enc_level,&fight.actors[0].level,g_hero.level);
        hero_award(&g_hero,xp,gold);
        victory_trophy_roll();
        battle_music("victory");
        wos_log_event("battle_won","xp=%d gold=%d share=%d/%d kills=%d",
                      (int)(g_hero.xp-old_xp),(int)(g_hero.gold-old_gold),
                      fight.actors[0].participation,fight.participation_total,fight.scene_kills);
    } else if (result == BATTLE_LOST) {
        g_hero.hp = 0;
        battle_music("lost");
        hero_died();
        wos_log_event("battle_lost",NULL);
    }
}
/* FUN_00494fcd: the local hero dies. scene[0x3E034] counts the fight's kills; with the solo
 * auto-resurrect flag (DAT_00502a48) the hero loses a tenth of a level of XP the first time and
 * the flag is then cleared, so the next death is a real one. */
static void hero_died(void)
{
    int64_t base = hero_xp_for_level(&g_hero,g_hero.level);
    int64_t next = hero_xp_for_level(&g_hero,g_hero.level+1);
    int64_t span = next - base;
    int tenths = span > 0 ? (int)(((g_hero.xp - base)*10 + span - 1)/span) : 0;
    if (tenths > 9) tenths = 9;
    wos_log_event("battle_hero_death","level=%d xp=%lld tnl=%d kills=%d killer=%d resurrect=%d",
                  g_hero.level,(long long)g_hero.xp,tenths,fight.scene_kills,fight.killer_id,
                  fight.auto_resurrect);
    if (fight.killer_id > 0) hero_killed_by_monster(fight.killer_id);
    /* FUN_00494fcd seals the hero's XP at 0x004950B8 (FUN_0049b71b) - one seal, 4 rands. */
    enc_put(&fight.actors[0].enc_level,&fight.actors[0].level,g_hero.level);
    if (fight.auto_resurrect) {
        /* FUN_0042b765: the XP interpolated at the fractional level the hero had reached, so
 * the progress toward the next level is what is lost. */
        g_hero.xp = clamp(base + span*tenths/10, 0, g_hero.xp);
        fight.auto_resurrect = 0;
        wos_log_event("hero_resurrect","xp=%lld",(long long)g_hero.xp);
    } else {
        /* DAT_00502a48 is clear, so this is a real death: the front end shows the death screen. */
        front_hero_death(fight.killer_id > 0 ? g_world.monsters[fight.killer_id].name : NULL);
    }
    hero_add_death(&g_hero);
}
static int outcome(void)
{
    if (g_hero.hp <= 0) { fight.decided = 1; finish(BATTLE_LOST); return 1; }
    if (first_enemy() < 0) { fight.decided = 1; finish(BATTLE_WON); return 1; }
    return 0;
}
/* Corrected FUN_004a7794: FUN_0049b70f is an encrypted field reader, NOT rand().
 * +628=level, +660=defense, +698=offense (004805b8..00480604). */
static int physical_damage(Combatant *a, Combatant *b, int monster)
{
    int miss = clamp(ability(b,ABIL_AGI)-ability(a,ABIL_DEX),0,50);
    int d, base;
    int64_t power, raw;
    if (miss && roll(100) < miss) return -1;
    power = (int64_t)(a->level+100)*(ability(a,ABIL_STR)+65)*(a->offense+5)/6500;
    if (monster) {
        int elapsed = clamp(fight.tick-(uint32_t)a->last_action,0,480);
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
    /* FUN_004a7794 writes the two attacker's combat abilities and the two derived stats through
     * FUN_0049b71B at 0x004A7915/959/9A6/9F8 - four seals, 16 rands, before the variance roll. */
    enc_put(&a->enc_offense,&a->offense,a->offense);
    enc_put(&a->enc_defense,&a->defense,a->defense);
    enc_put(&a->enc_hp,&a->hp,a->hp);
    enc_put(&a->enc_max_hp,&a->max_hp,a->max_hp);
    d = clamp(raw,1,32000);
    base = clamp((int64_t)d*9/10,1,32000);
    /* FUN_004a6974: variance is one rand, the crit gate is `rand()%param_5 == 0`, and the crit
     * itself is `2*base` for a monster or `rand()%3+2` for a player - always the same 2..4 rands. */
    if (d > 1) d = roll(d)/5 + base;
    if (roll(50) == 0) d = (monster ? 2 : roll(3)+2)*base;
    return clamp(d,1,32000);
}
static int ai_target(int actor)
{
    int legal[ACTORS], n = 0, i;
    for (i = 0; i < fight.count; ++i)
        if (fight.actors[i].hp > 0 && fight.actors[i].ally != fight.actors[actor].ally) legal[n++] = i;
    /* FUN_0048e62c: one uniform pick out of the legal list. */
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
/* FUN_0048e773 exactly: (spell element, req affinity, own element, wisdom, own-element-only).
 * The own-element branch divides by 8 with the x87 rounding the binary does; the normal branch
 * clamps the element distance to 0..4 and needs wisdom/40 >= distance. A chaos element
 * (>= 8) is never cast unless the monster is locked to its own element. */
static int spell_affinity(int element, int affinity, int own, int wisdom, int own_only)
{
    int d;
    if (own_only) {
        if (own != element) return 0;
        return ((wisdom + (wisdom >> 31 & 7)) >> 3) >= affinity;
    }
    if (element >= 0) {
        if (element >= 8) return 0;                 /* chaos spells are never chosen */
        d = element - own;
        if (d < 0) d += 8;
        if (4 < d) d = 8 - d;
    } else d = 1;
    if (d < 0) d = 0;
    if (4 < d) d = 4;
    if (wisdom / 40 < d) return 0;
    return wisdom / (d*8+8) >= affinity;
}
/* FUN_0048b119: the summon codes carried in a spell's power field ([0x128]). */
static int is_summon(int power)
{
    return power == -300 || (power < -199 && power > -204);
}
/* FUN_00491b6a via FUN_00491bb7: the combatant's derived attack rating, rec[0x6D8]. The two
 * FUN_0049b70f reads are the encrypted copies of defense (+0x660) and offense (+0x698); the two
 * raw terms are the combat abilities at +0x444 and +0x440. */
static int compute_rating(const Combatant *a)
{
    if (a->owner <= 0) return 0;
    return (a->offense + 0x11) * (a->defense*4 + 0x5c) + a->ability_b*0x16 + a->ability_a*0x57;
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
                spell_affinity(g_world.spells[i].element,g_world.spells[i].req_affinity,a->element,
                                 ability(a,ABIL_WIS),g_world.monsters[a->id].flags & 2) && a->hp < a->max_hp/2) {
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
            if (id > 0 && id < WORLD_MAX_SPELLS && spell_affinity(g_world.spells[id].element,g_world.spells[id].req_affinity,a->element,
                                 ability(a,ABIL_WIS),g_world.monsters[a->id].flags & 2) &&
                roll(++n) == 0) chosen = (int)id;
            p = end;
            if (*p != ',') break;
            ++p;
        } while (*p);
    } else {
        for (i = 1; i < WORLD_MAX_SPELLS; ++i)
            if (g_world.spells[i].used && prefix(p,g_world.spells[i].name) &&
                spell_affinity(g_world.spells[i].element,g_world.spells[i].req_affinity,a->element,
                                 ability(a,ABIL_WIS),g_world.monsters[a->id].flags & 2)) {
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
/* FUN_0048e810 exactly. `mode` is the original's param_2 (1 forces "no spell", -1); `slot` is
 * param_3 (-1 scans the whole array, otherwise it is the slot FUN_00490645 already picked, which
 * also forces the healing pass to look only there). Returns the spell id, 0 for a physical attack
 * or -1 when the monster does nothing this turn.
 *
 * Pass 1 is a deterministic walk of spells 0x2FF..1 looking for a heal ([0x134] == 0 and element
 * 0) with a usable target. Pass 2 is the ring walk: ONE seed rand, then 0x300 steps of
 * (seed + k) % 0x300 for k = 0x2FF..0. A summon candidate is taken unconditionally the first
 * time and costs a rand every time after (`local_18 == -1 || rand()%3000 < wisdom` short
 * circuits), so the draw count is exactly (summon candidates - 1). The walk is closed by one
 * rand for the `wisdom <= r%500` gate and a second only when that gate passes. */
static int monster_action(int actor, int mode, int slot)
{
    Combatant *a = &fight.actors[actor];
    const MonsterDef *m = &g_world.monsters[a->id];
    int wisdom = ability(a,ABIL_WIS);
    int best = 0, best_score = 0, summon = -1, n, s;

    if (a->element == 0) {                    /* FUN_0048e810's `rec[0x2A4] == 0` gate */
        for (n = WORLD_MAX_SPELLS-1; n > 0; --n) {
            const SpellDef *sp = &g_world.spells[n];
            int lo = slot < 0 ? 0 : slot, hi = slot < 0 ? ACTORS : slot+1;
            if (!sp->used || sp->all_targets || sp->element) continue;
            if (!spell_affinity(0,sp->req_affinity,0,wisdom,m->flags & 2)) continue;
            if (sp->min_level > a->level || (sp->flags & 1)) continue;
            if (is_summon(sp->damage)) continue;
            for (s = lo; s < hi; ++s) {
                Combatant *r = &fight.actors[s];
                if (s >= fight.count || r->hp < 0 || r->ally != a->ally) continue;
                /* power == -1 revives a dead ally; the `power > 0` arm tests
                 * power <= (hp - maxhp)*4, which can never hold for a living actor. */
                if (sp->damage == -1 ? (r->hp < 1 && !r->ally)
                                     : (r->hp > 0 && a->level <= r->hp/2
                                        && sp->damage <= (r->hp - r->max_hp)*4)) {
                    fight.target = s;
                    return n;
                }
            }
        }
    }
    if (mode) return -1;

    {
        int seed = rand1();
        for (n = 0x2ff; n >= 0; --n) {
            int id = (seed + n) % 0x300, score;
            const SpellDef *sp;
            if (id <= 0 || id >= WORLD_MAX_SPELLS) continue;
            sp = &g_world.spells[id];
            if (!sp->used) continue;
            if (!spell_affinity(sp->element,sp->req_affinity,a->element,wisdom,m->flags & 2)) continue;
            if (!sp->element) continue;                     /* [0x124] != 0: not a heal */
            if (sp->min_level > a->level || (sp->flags & 1)) continue;
            if (is_summon(sp->damage) && !(m->flags & 4)) continue;  /* "may summon" */
            if (sp->damage < 0) {
                if (summon == -1 || rand1() % 3000 < wisdom) summon = id;
            } else {
                score = sp->damage * (!fight.target || fight.actors[fight.target].ally
                                      ? 100
                                      : 500 - resistance(sp->element,fight.actors[fight.target].element));
                if (score != best_score && best_score <= score) { best_score = score; best = id; }
            }
        }
    }
    {   /* FUN_0048e810's closing gate: one rand always, a second only if wisdom <= r%500. */
        int r = rand1();
        if (wisdom <= r % 500) {
            int r2 = rand1();
            if (wisdom <= r2 % 1000 || (best = summon, summon < 0)) best = 0;
        }
    }
    /* FUN_0048f913's magic-only actor (FUN_004a6d32's second counter): it may not resolve to a
     * physical swing. Effects 4, 6, 7 and 11 carry that counter in the image table above. */
    if (!best && magic_only(a)) best = summon > 0 ? summon : best;
    if (best) fight.target = fight.target >= 0 && fight.target < fight.count ? fight.target
                                                                       : first_enemy();
    return best;
}
static int monster_spell(int actor, int *target)
{
    int spell = monster_action(actor, 0, -1);
    if (spell < 0) return 0;
    if (spell > 0) {
        const SpellDef *sp = &g_world.spells[spell];
        if (sp->element == 0) *target = actor;
        else if (*target < 0 || !legal_target(actor,*target,spell)) *target = spell_target(actor,spell);
    }
    return spell;
}
/* FUN_004a7456: the cast-success roller. `base` is 50 below ratio 5, 100 from 95, and
 * 50 + (ratio-5)*50/90 in between; a nonzero practice value pulls it toward 100 by
 * `100 - (100-pp)*(100-base)/100`. config.ini spellSuccessPercent then scales it, clamped at
 * 100, and the result is truncated after +0.5. The original does this in x87 doubles. */
static int cast_success(int ratio, int pp)
{
    double base, pct;
    if (ratio <= 5) base = 50.0;
    else if (ratio >= 95) base = 100.0;
    else base = 50.0 + (ratio-5)*50.0/90.0;
    if (pp > 0) base = 100.0 - (100.0-(double)pp)*(100.0-base)/100.0;
    pct = g_world.spell_success_percent;
    if (pct != 100.0) {
        double scaled = pct*base*0.01;
        base = scaled > 100.0 ? 100.0 : scaled;
    }
    return (int)(base + 0.5);
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
    /* FUN_004a7794's spell branch seals the same four fields, 0x004A7915/959/9A6/9F8. */
    enc_put(&a->enc_offense,&a->offense,a->offense);
    enc_put(&a->enc_defense,&a->defense,a->defense);
    enc_put(&a->enc_hp,&a->hp,a->hp);
    enc_put(&a->enc_max_hp,&a->max_hp,a->max_hp);
    d = clamp(raw,1,32000); base = clamp((int64_t)d*9/10,1,32000);
    /* FUN_004a6974, same rand shape as the physical branch. */
    if (d > 1) d = roll(d)/5+base;
    if (!roll(50)) d = (actor ? 2 : roll(3)+2)*base;
    return clamp(d,1,32000);
}
/* FUN_00480875: a monster's XP award is its table value scaled by the ratio of the combatant's
 * level to the table level (NOT a rand - docs/re/battle.md section 2's reading is wrong).
 * FUN_004946b2 pays out `rand()%(gold+1) + gold/2` and the XP, both clamped to 0x7FFF. */
static void kill_payout(Combatant *b)
{
    const MonsterDef *m = &g_world.monsters[b->id];
    int gold = b->gold;
    if (!b->ally && b->id > 0 && b->id < WORLD_MAX_MONSTERS) {
        /* DAT_00d2c7d8, the fight-scoped kill table FUN_00494fcd increments; the hero's own
         * persistent count is what quest `IF KB <id>` / `IF KM <id>` read (FUN_0043b0fd). */
        ++fight.scene_kills;
        ++fight.monster_kills[b->id];
        hero_kill_monster(b->id);
        /* FUN_00494fcd's order is the kill count, then the gold rand, then the XP
         * (FUN_00480875 draws nothing), and only then the participation gate and the trophy
         * roll - so the trophy randoms come AFTER the gold one. */
        fight.gold = clamp((int64_t)fight.gold + roll(gold+1) + gold/2, 0, 32767);
        if (m->used) {
            int xp = m->exp > 0 ? m->exp : b->xp;
            if (m->level > 0 && b->level != m->level) xp = (int)((int64_t)b->level*xp/m->level);
            fight.xp = clamp((int64_t)fight.xp + clamp(xp,0,60000), 0, INT_MAX);
        }
        /* FUN_00494fcd's only trophy roll in the whole binary (FUN_0046fcc4 is called from
         * exactly one site, 0x49526D): the local hero's participation share of rec[0x468] must
         * exceed 10 % of DAT_00502830, then FUN_00458988 sets two globals and FUN_0046fcc4
         * rolls the bag for this monster. Not on the hero's death, and not for allies. */
        if (fight.participation_total > 0 &&
            fight.actors[0].participation*100/fight.participation_total > 10) {
            wos_log_event("battle_trophy","monster=%d",b->id);
            trophy_bag_award_kill(b->id);
        }
    }
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
            if (miss && roll(100) < miss) { b->damage = INT_MIN; b->floating = (int)tick_now(); continue; }
            damage = spell_damage(actor,i,fight.spell,targets);
            /* FUN_004a6255 applies the delta through FUN_0049b71b - one seal, 4 rands, at
             * 0x004A6285 - after clamping to [0, max]. */
            enc_put(&b->enc_hp,&b->hp,clamp((int64_t)b->hp-damage,0,b->max_hp));
        }
        b->damage = damage; b->floating = (int)tick_now();
        if (!i) { g_hero.hp = b->hp; g_hero.ailments = b->ailments; }
        if (alive && !b->hp) kill_payout(b);
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
        /* rec[+0x390] is NOT a locally derived rating: the only writer in the binary is net
         * opcode 0x53 (all.c:34246), which the server pushes together with the spell id and the
         * target. Offline it is therefore always 0 and FUN_004a7456's practice term
         * `100 - (100-pp)*(100-base)/100` never applies. rec[+0x6D8] (FUN_00491b6a via
         * FUN_00491bb7, kept in a->rating) is a different field, read by the desync checker. */
        fight.attack_rating = 0;
        if (category >= 0 && category < 8) {
            int gain = kind == HERO_TRAIN_ELEMENT ? s->req_affinity*10+20 : 20;
            hero_gain_training(&g_hero,kind,category,gain);
        }
        if (kind == HERO_TRAIN_ELEMENT) {
            fight.mp_gauge = 0; fight.mp_start = fight.tick;
        } else { fight.hp_gauge = 0; fight.regen_start = fight.tick; }
        if (!s || !spell_helpful(s)) hero_add_pp(&g_hero,a->ability[ABIL_WIS]*200/255+5);
        fight.queued = 0;
        fight.message[0] = 0;
        /* _DAT_00502b18: the flee lock is measured from the hero's last committed action. */
        fight.flee_lock = tick_now();
    }
    a->mp -= cost;
    if (!actor) g_hero.mp = a->mp;
    /* rec[0x390]; see the comment above - 0 offline. */
    fight.spell = spell; fight.bound_spell = bound; fight.fizzle = 0;
    /* FUN_0048fe80: 1000 ms physical, 4000 ms spell, 1250 ms for the -3/-4/-5 specials. */
    fight.duration_ms = spell ? SPELL_MS : PHYSICAL_MS;
    if (spell) {
        int ratio = actor ? clamp(ability(a,ABIL_WIS)/4+50,0,100) : g_world.classes[g_hero.klass].magic_ratio;
        if (s->element > 0 && s->element < 8 && roll(100) >= cast_success(ratio,fight.attack_rating)) {
            fight.fizzle = 1;
            wos_log_event("spell_fizzle","caster=%s spell=%d target=%d",actor?"monster":"hero",spell,target);
            snprintf(fight.message,sizeof fight.message,"%s fizzles",s->name);
        }
    } else if (!actor && fight.throw_radius) {
        /* FUN_004a4d70 leaves the scatter radius at its default 0x10 for a physical attack
         * (case 0 falls through to `default: *radius = 0x10`), and FUN_004a3a54 then draws the
         * paired `x += rand()%(r*2) - r`, `y += rand()%(r*2) - r`, guarded on r != 0. */
        fight.target_x = fight.actors[target].x + roll(fight.throw_radius*2) - fight.throw_radius;
        fight.target_y = fight.actors[target].y + roll(fight.throw_radius*2) - fight.throw_radius;
        items_arm_throw(fight.throw_item);
        wos_log_event("battle_throw_scatter","radius=%d x=%d y=%d",fight.throw_radius,
                      fight.target_x,fight.target_y);
    } else {
        if (!actor) {
            int item = g_hero.right_hand;
            fight.throw_item = 0; fight.throw_radius = 0;
            if (item > 0 && item < WORLD_MAX_ITEMS && g_world.items[item].klass == ITEM_THROWABLE) {
                fight.throw_item = item;
                fight.throw_radius = THROW_RADIUS;
            }
        }
        fight.damage = physical_damage(a,&fight.actors[target],actor != 0);
    }
    /* FUN_0048FE80 seals rec[0x6D8] at 0x00490499 (4 rands) and FUN_0048b0c7 ADDs to rec[0x11A]
     * and DAT_00502830, which is a get followed by a set, so 4 more. */
    enc_put(&a->enc_rating,&a->rating,compute_rating(a));
    /* FUN_0048fe80 rec[0x428]++ (the attack count) and FUN_0048b0c7, which credits an actor only
     * when it has an owner - in solo that is the hero alone, so it always takes the whole pot. */
    a->last_turn = a->last_action = fight.tick;
    a->turn = TURN_DONE; a->ready = 0; ++a->attacks;
    if (actor && !fight.actors[actor].ally) fight.killer_id = fight.actors[actor].id;
    if (!actor) {
        /* FUN_0048b0c7: rec[0x11A] and DAT_00502830 both take the actor's participation. */
        /* FUN_0048b0c7: rec[0x11A] += the actor's rating and DAT_00502830 takes the same value.
         * The ADD is a get followed by a set, so it costs 4 rands like any other write. */
        a->participation = enc_add(&a->enc_participation,1);
        a->attacks = a->attacks;   /* rec[0x10A] is a plain counter, not an EncInt */
        fight.participation_total = a->participation;
    }
    fight.attacker = actor; fight.victim = target; fight.animation = 0;
    fight.anim_start = tick_now();
    fight.state = ROUND_DAMAGE; fight.cursor = (actor+1)%fight.count;
    if (scene_event) scene_event(actor && spell ? "spell" : "attack", actor, 0);
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
/* FUN_0048b1ad target-selection click: validate the target, then queue
 * rec[0xBA] = spell, rec[0xBB] = target, rec[0x38C] = 0x2F. */
int battle_click_actor(int slot)
{
    if (fight.result != BATTLE_RUNNING || fight.spell_menu) return 0;
    if (slot < 0 || slot >= fight.count) return 0;
    if (!legal_target(0,slot,fight.chosen_spell)) return 0;
    fight.target = slot;
    fight.queued = 1;
    if (scene_event) scene_event("click", slot, 0);
    return 1;
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
/* FUN_0048f816: the cowardice flee. The monster is unaligned (rec[0x114] == 0) and not flagged
 * never-flee (monsters.txt arg3 flag 32, read through FUN_0048f7e6). Each live player combatant
 * more than 20 levels below it contributes `(delta*100)/500`; above 50, with wisdom > 100 and
 * HP > 0, the chance rises by `100 - rand()*100/HP`. One or two rands, in that order. */
static int coward_flee(Combatant *m, int *out_hero)
{
    int i;
    *out_hero = -1;
    if (g_world.monsters[m->id].flags & 32) return 0;
    for (i = 0; i < fight.count; ++i) {
        const Combatant *o = &fight.actors[i];
        int delta, chance, r;
        if (o->hp <= 0 || o->ally || o == m) continue;
        delta = o->level - m->level;
        if (delta <= 20) continue;
        chance = delta*100/500;
        if (chance > 50 && ability(m,ABIL_WIS) > 100 && m->hp > 0) {
            r = rand1();
            chance = chance - (r*100)/m->hp + 100;
        }
        r = rand1();
        if (r % 100 < chance) { *out_hero = i; return 1; }
    }
    return 0;
}
/* FUN_0048f913's monster block, which is also the actor picker. For a monster whose owner is -1,
 * is not already 0x2F and has been idle more than 2000 ms (GetTickCount() - rec[0x45C] > 1999)
 * it runs the flee/wander/act decision. Three randoms are possible and the count depends on the
 * branch:
 *   (a) `(rand()+4)*10` for the flee chance, always;
 *   (b) when the flee chance fires, `rand()%100 > 33` decides act-vs-close-in, and the spell
 *       pick behind it draws its own; when it does not act it walks a quarter of the way
 *       toward its target instead;
 *   (c) when the flee chance fails, a paired `x += rand()%10-5`, `y += rand()%10-5` wander.
 * A monster that reaches the block and does not act is left at 0x2F, which state 5 re-arms to
 * 0x5F next round. A fear effect (FUN_004a6d32's first counter) suppresses the fear and the
 * wander. Returns `slot` if this monster is the one to act, -1 otherwise. */
static int monster_idle_one(int slot)
{
    {
        Combatant *a = &fight.actors[slot];
        int afraid, chance, target = -1, spell;
        if (a->ally || a->hp <= 0 || a->turn == TURN_DONE) return -1;
        if (!past(a->last_turn,MONSTER_IDLE_MS-1)) return -1;
        afraid = cowardice(a);
        chance = (roll(10) + 4) * 10;            /* (a) FUN_0048f913's `(rand()+4)*10` */
        if (chance > 100) chance = 100;
        if (!a->fled) {
            if (!afraid && a->x > 20) {
                int hero_slot;
                if (coward_flee(a,&hero_slot)) {
                    a->fled = 1; a->x = -30;
                    wos_log_event("battle_monster_flee","slot=%d monster=%d mode=cowardice target=%d",
                                  slot,a->id,hero_slot);
                    snprintf(fight.message,sizeof fight.message,
                             "%s takes fear and flees.",g_world.monsters[a->id].name);
                }
            }
            if (!a->fled) {
                if (roll(100) < chance) {
                    /* FUN_0048f56c: the monsters.txt arg-20 pet-verb command runs first and owns
                     * both the target (local_28) and the spell (local_24). */
                    spell = commanded_spell(slot,&target);
                    if (target < 0) target = ai_target(slot);  /* FUN_0048e62c retarget */
                    if (target < 0) return -1;    /* no legal target: it leaves the fight */
                    if (a->x > 0 && a->y > 0 && roll(100) > 33) {   /* (b) it acts this pass */
                        /* FUN_0048f913 runs FUN_0048e810 only when `local_24 == 0` - the monster
                         * has no spell from its command - and the monster is not AI-only
                         * (monsters.txt arg3 flag 0x10). */
                        if (spell <= 0 && !(g_world.monsters[a->id].flags & 16))
                            spell = monster_spell(slot,&target);
                        /* Flag 4 lets a pet/escort be dragged in instead: FUN_0048ebe3, gated
                         * by `rand()%1000 < 250` (0xFA). */
                        if (spell > 0 && (g_world.monsters[a->id].flags & 4) && roll(1000) < 250) {
                            int ally = 0;
                            for (ally = 0; ally < fight.count; ++ally)
                                if (fight.actors[ally].ally && fight.actors[ally].hp > 0) break;
                            if (ally < fight.count) { target = ally; spell = 0; }
                        }
                        wos_log_event("battle_monster_act","slot=%d monster=%d target=%d spell=%d",
                                      slot,a->id,target,spell);
                        a->turn = TURN_DONE;
                        a->last_turn = tick_now();
                        return slot;
                    }
                } else if (!afraid) {
                    /* (c) The failed flee wanders: a paired rand, always both. */
                    a->x += roll(10) - 5;
                    a->y += roll(10) - 5;
                }
                a->turn = TURN_DONE;
                a->last_turn = tick_now();
                return -1;
            }
        }
        if (a->x < -10) { a->turn = TURN_DONE; a->hp = 0; }  /* FUN_0048e16e clears the record */
    }
    return -1;
}
/* FUN_00436c9d: may the hero leave the fight? 1 = yes, 0 = "dragged back". Offline there is no
 * Tactics abandon dialog (FUN_00447431 returns 0), so an ordinary random encounter has no timer
 * escape at all: only the 30 % interrupt of an attack already aimed at the hero gets you out. */
static int hero_can_flee(void)
{
    int blocked = 1;
    if (g_hero.max_hp < 1) return 1;
    if (fight.actors[0].turn == TURN_DONE && fight.engaged) return 0;
    fight.actors[0].turn = TURN_DONE;
    if (cowardice(&fight.actors[0])) return 0;
    if (fight.sticky == STICKY_TACTICS || fight.sticky == STICKY_PK) {
        if (past(fight.flee_lock,FLEE_LOCK_MS)) blocked = 0;
    } else if (fight.sticky == STICKY_FIGHT2) {
        if (past(fight.flee_lock,FLEE_LOCK_FIGHT2_MS) && !fight.decided) blocked = 0;
    }
    if (fight.victim == 0 && fight.state == ROUND_DAMAGE &&
        !past(fight.anim_start,(uint32_t)fight.duration_ms) &&
        roll(100) < FLEE_INTERRUPT_PCT)
        blocked = 0;
    if (g_hero.max_hp < 1) blocked = 1;
    return !blocked;
}
BattleResult battle_update(const Input *in)
{
    int i, actor = -1;
    if (fight.result != BATTLE_RUNNING) return fight.result;
    if (fight.spell_menu) { spell_menu_update(in); return fight.result; }
    if (in && in->pressed['s']) { battle_open_spells(); return fight.result; }
    fight.tick = tick_now();
    /* FUN_00449006 is the hero-stats-into-the-live-combatant push, and it has FIVE seals, not the
     * two I first placed. Ghidra drops the this-pointer on a FUN_0049B71B call, so the decomp
     * reads them as seals on the hero record - which is wrong, the hero record has no EncInt.
     * Each is really `enc_set(&combatant_field, <value loaded from the hero>)`:
     *   & 1:  seal(HP)   hero+0x70 -> rec[0x2A8] = hero+0x74   (maxHP, plain)
     *         seal(maxHP) hero+0x78 -> rec[0x2AC] = hero+0x7C   (maxMP, plain)
     *   & 2:  five FUN_00416c60(hero+0x680..0x690, 0xFF) reads land in rec[0x42C..0x43C] PLAIN,
     *         then seal(hero+0x84), seal(hero+0x80), and finally seal(hero+0x64 = the level),
     *         with rec[0x440]/[0x444]/[0x2F8]/[0x2B4] as plain copies around them.
     * The order of the five is the contract, so it is reproduced exactly. */
    enc_put(&fight.actors[0].enc_hp,&fight.actors[0].hp,g_hero.max_hp);
    enc_put(&fight.actors[0].enc_max_hp,&fight.actors[0].max_hp,g_hero.max_mp);
    /* The three seals in the & 2 arm are re-seals of values the hero holds at +0x84 and +0x80,
     * which are not fields the port carries, so each re-seals the combatant's own value. That
     * spends the same 12 draws in the same order without inventing data; when the port grows the
     * real hero fields these three become copies of them. */
#ifdef HERO_HAS_ATTACK_DEFENCE
    enc_put(&fight.actors[0].enc_mp,&fight.actors[0].mp,g_hero.attack);
    enc_put(&fight.actors[0].enc_offense,&fight.actors[0].offense,g_hero.defense);
#else
    enc_put(&fight.actors[0].enc_mp,&fight.actors[0].mp,fight.actors[0].mp);
    enc_put(&fight.actors[0].enc_offense,&fight.actors[0].offense,fight.actors[0].offense);
#endif
    enc_put(&fight.actors[0].enc_level,&fight.actors[0].level,g_hero.level);
    fight.actors[0].hp = g_hero.hp; fight.actors[0].mp = g_hero.mp;
    fight.actors[0].ailments = g_hero.ailments;
    if (outcome()) return fight.result;
    if (in && (in->pressed['f'] || in->pressed[PLAT_KEY_ESCAPE])) {
        if (hero_can_flee()) {
            finish(BATTLE_FLED);
            snprintf(fight.message,sizeof fight.message,"You flee.");
            wos_log_event("battle_fled","sticky=%d held=%u",fight.sticky,
                          (unsigned)(tick_now()-fight.flee_lock));
        } else {
            snprintf(fight.message,sizeof fight.message,
                     "You attempt to flee... but are dragged back");
            wos_log_event("battle_flee_failed","sticky=%d held=%u",fight.sticky,
                          (unsigned)(tick_now()-fight.flee_lock));
        }
        return fight.result;
    }
    /* FUN_00478840: the panel timer recomputes both gauges from scratch every tick, so the 25 a
     * new round starts with is immediately overwritten. */
    fight.hp_gauge = clamp((int)(((int64_t)(tick_now()-fight.regen_start)*
                                  (fight.actors[0].ability[ABIL_STR]/2+100))/10000),0,100);
    fight.mp_gauge = clamp((int)(((int64_t)(tick_now()-fight.mp_start)*
                                  (fight.actors[0].ability[ABIL_WIS]/2+100))/10000),0,100);
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
            for (i = fight.count-1; i >= 0; --i)
                if (legal_target(0,i,fight.chosen_spell) && inside(actor_rect(i),in->mouse_x,in->mouse_y))
                    { battle_click_actor(i); break; }
        }
    }
    if (fight.state == ROUND_START) {
        for (i = 0; i < fight.count; ++i) {
            Combatant *c = &fight.actors[i];
            c->ready = c->hp > 0 && !c->fled;
            c->turn = c->ready ? TURN_READY : TURN_DONE;
        }
        /* FUN_00490E7C case 5 runs FUN_00491bd1 (the desync check) on every combatant, and that
         * seals twice - 0x00491D2E and 0x00491D4E, 8 rands. */
        for (i = 0; i < fight.count; ++i) {
            enc_put(&fight.actors[i].enc_rating,&fight.actors[i].rating,
                    fight.actors[i].rating);
            enc_put(&fight.actors[i].enc_defense,&fight.actors[i].defense,
                    fight.actors[i].defense);
        }
        fight.round_start = fight.tick;
        fight.state = ROUND_ACT;
    } else if (fight.state == ROUND_DAMAGE) {
        if (past(fight.anim_start,(uint32_t)fight.duration_ms)) {
            Combatant *victim = &fight.actors[fight.victim];
            if (fight.spell) apply_spell();
            else {
                victim->damage = fight.damage < 0 ? INT_MIN : fight.damage;
                victim->floating = (int)tick_now();
                wos_log_event("battle_hit","ms=%u attacker=%d target=%d dmg=%d",
                              (unsigned)(tick_now()-fight.anim_start),fight.attacker,fight.victim,fight.damage);
                if (fight.damage >= 0)   /* FUN_004a6255, 0x004A6285 - one seal, 4 rands */
                    enc_put(&victim->enc_hp,&victim->hp,
                            clamp((int64_t)victim->hp-fight.damage,0,victim->max_hp));
                if (fight.victim == 0) g_hero.hp = victim->hp;
                if (victim->hp <= 0) kill_payout(victim);
                items_spend_throw();   /* FUN_0048f667 */
            }
            fight.attacker = fight.victim = -1;
            fight.state = ROUND_ACT;
            outcome();
        }
    } else {
        int ready = 0;
        /* FUN_0048f913 walks the slots in cursor order and returns the first live combatant that
         * is not 0x2F, running the monster idle block for each monster it passes. Slot 0 is the
         * hero, so a queued hero always pre-empts the monsters. */
        for (i = 0; i < fight.count; ++i) {
            int n = (fight.cursor+i)%fight.count;
            Combatant *c = &fight.actors[n];
            if (c->hp <= 0 || !c->ready || c->turn == TURN_DONE) continue;
            ready = 1;
            if (n != 0) {
                if (monster_idle_one(n) >= 0) { actor = n; break; }
                continue;                 /* it gave up its turn; keep scanning */
            }
            if (fight.queued) { actor = 0; break; }
        }
        /* FUN_0048b17a: the hero may not commit during the first 500 ms of its round. */
        if (actor == 0 && !past(fight.round_start,HERO_GATE_MS-1)) actor = -1;
        if (actor >= 0) {
            int target = actor == 0 ? fight.target : ai_target(actor);
            if (target >= 0) start_attack(actor,target);
        } else if (!ready || past(fight.round_start,ROUND_IDLE_MS)) fight.state = ROUND_START;
    }
    return fight.result;
}
/* Bhaskara's integer sine approximation over [0,pi], endpoints 0, midpoint 256. */
static int lunge(int t)
{
    int n = PHYSICAL_MS;
    int p = t*(n-t);
    return 16*p*256/(5*n*n-4*p);
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
    uint32_t now = tick_now();
    if (view.w <= 0 || view.h <= 0 || fight.result == BATTLE_NONE) return;
    fight.view = view;
    fb_clip_intersect(fb,view);
    for (i = 0; i < fight.count; ++i) {
        Combatant *a = &fight.actors[i];
        const Sheet *s = &fight.sheets[a->sheet_index];
        int x = a->x, y = a->y, pose = 1, flip = i == 0;
        int sx, sy, w, anim;
        char label[80];
        const char *name = i == 0 ? g_hero.name : g_world.monsters[a->id].name;
        if (a->hp <= 0 && !(a->floating && !past((uint32_t)a->floating,FLOAT_MS))) continue;
        if (a->hp < a->max_hp/4 || (a->floating && !past((uint32_t)a->floating,FLOAT_MS))) pose = 3;
        if (fight.attacker == i) {
            Combatant *b = &fight.actors[fight.victim];
            int elapsed = (int)(now - fight.anim_start);
            int fraction = fight.spell ? 0 : lunge(clamp(elapsed,0,PHYSICAL_MS));
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
        if (a->floating && !past((uint32_t)a->floating,FLOAT_MS)) {
            anim = (int)(now - (uint32_t)a->floating);
            if (a->damage == INT_MIN) snprintf(label,sizeof label,"MISS");
            else if (a->damage < 0) snprintf(label,sizeof label,"+%d",-a->damage);
            else snprintf(label,sizeof label,"-%d",a->damage);
            font_draw(fb,sx-font_width(label)/2,sy-30-anim/4,label,0xffff70);
        }
    }
    if (fight.state == ROUND_DAMAGE && fight.spell && !fight.fizzle) {
        static const uint32_t colors[8] = {0x80ff80,0x60a0ff,0x40d070,0xb08040,0xb080d0,0xff7040,0xffffff,0xffff70};
        int el = g_world.spells[fight.spell].element;
        Rect r = actor_rect(fight.victim);
        fb_blend(fb,r,colors[el&7],(unsigned)(40+((now-fight.anim_start)%30)*4));
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
void battle_dump(DumpEmit emit, void *user)
{
    char key[48], value[64];
    int i;
    dump_emit_int(emit,"battle.state",fight.state,user);
    dump_emit_int(emit,"battle.result",fight.result,user);
    dump_emit_int(emit,"battle.count",fight.count,user);
    dump_emit_int(emit,"battle.target",fight.target,user);
    dump_emit_int(emit,"battle.attacker",fight.attacker,user);
    dump_emit_int(emit,"battle.victim",fight.victim,user);
    dump_emit_int(emit,"battle.spell",fight.spell,user);
    dump_emit_int(emit,"battle.participation_total",fight.participation_total,user);
    dump_emit_int(emit,"battle.scene_kills",fight.scene_kills,user);
    dump_emit_int(emit,"battle.auto_resurrect",fight.auto_resurrect,user);
    dump_emit_int(emit,"battle.sticky",fight.sticky,user);
    dump_emit_int(emit,"battle.hp_gauge",fight.hp_gauge,user);
    dump_emit_int(emit,"battle.mp_gauge",fight.mp_gauge,user);
    for (i = 0; i < fight.count; ++i) {
        const Combatant *a = &fight.actors[i];
        snprintf(key,sizeof key,"battle.slot%d.id",i);
        snprintf(value,sizeof value,"%d",a->id); emit(key,value,user);
        snprintf(key,sizeof key,"battle.slot%d.hp",i);
        snprintf(value,sizeof value,"%d",a->hp); emit(key,value,user);
        snprintf(key,sizeof key,"battle.slot%d.maxhp",i);
        snprintf(value,sizeof value,"%d",a->max_hp); emit(key,value,user);
        snprintf(key,sizeof key,"battle.slot%d.mp",i);
        snprintf(value,sizeof value,"%d",a->mp); emit(key,value,user);
        snprintf(key,sizeof key,"battle.slot%d.charge",i);
        snprintf(value,sizeof value,"%d",(int)(tick_now()-(uint32_t)a->last_action)); emit(key,value,user);
        snprintf(key,sizeof key,"battle.slot%d.x",i);
        snprintf(value,sizeof value,"%d",a->x); emit(key,value,user);
        snprintf(key,sizeof key,"battle.slot%d.y",i);
        snprintf(value,sizeof value,"%d",a->y); emit(key,value,user);
        snprintf(key,sizeof key,"battle.slot%d.state",i);
        snprintf(value,sizeof value,"%d",a->turn); emit(key,value,user);
        snprintf(key,sizeof key,"battle.slot%d.participation",i);
        snprintf(value,sizeof value,"%d",a->participation); emit(key,value,user);
    }
}
/* FUN_00432c28, the /battle, /battle2 and /battle3 simulators: an outer loop over the group
 * table and an inner one over its members. `mode` is the original's third parameter - 1 for
 * /battle (every member against the hero), 0 for /battle2 (member against member) and 2 for
 * /battle3. The original resolves each pairing through FUN_00432ac8, which sweeps the whole
 * item table to find the best weapon before rolling; the port reuses the live fight's spawn and
 * damage path with the hero's equipped right hand, so the formula is the same but the item
 * sweep is not. Emits the same battle_* events a live fight does. */
void battle_run_simulator(int mode)
{
    int g, i, members = 0;
    for (g = 1; g < WORLD_MAX_GROUPS; ++g) {
        const GroupDef *group = &g_world.groups[g];
        int n;
        if (!group->used) continue;
        n = clamp(group->count,0,GROUP_MAX_MEMBERS);
        for (i = 0; i < n; ++i) {
            int id = group->members[i];
            int attacker, target, damage;
            if (id <= 0 || id >= WORLD_MAX_MONSTERS || !g_world.monsters[id].used) continue;
            battle_begin_ex(&id,1,INT_MIN,0,0);
            attacker = mode ? 1 : (i % (fight.count-1)) + 1;
            if (attacker >= fight.count) attacker = fight.count-1;
            target = mode ? 0 : (attacker % (fight.count-1)) + 1;
            if (target >= fight.count || target == attacker) target = attacker == 0 ? 1 : 0;
            /* FUN_00432AC8 memsets its scratch combatant and then seals it; the simulator is an
             * admin command, so the seals are what keep its stream honest rather than its rules. */
            enc_put(&fight.actors[attacker].enc_level,&fight.actors[attacker].level,
                    fight.actors[attacker].level);
            enc_put(&fight.actors[attacker].enc_hp,&fight.actors[attacker].hp,
                    fight.actors[attacker].hp);
            enc_put(&fight.actors[attacker].enc_max_hp,&fight.actors[attacker].max_hp,
                    fight.actors[attacker].max_hp);
            enc_put(&fight.actors[target].enc_hp,&fight.actors[target].hp,fight.actors[target].hp);
            enc_put(&fight.actors[target].enc_max_hp,&fight.actors[target].max_hp,
                    fight.actors[target].max_hp);
            fight.attacker = attacker; fight.victim = target; fight.spell = 0;
            damage = physical_damage(&fight.actors[attacker],&fight.actors[target],attacker != 0);
            wos_log_event("battle_hit","ms=0 attacker=%d target=%d dmg=%d simulator=%d",
                          attacker,target,damage,mode);
            if (damage >= 0)
                fight.actors[target].hp = clamp((int64_t)fight.actors[target].hp-damage,0,
                                                fight.actors[target].max_hp);
            if (!mode) ++members;
        }
        wos_log_event("battle_sim_group","g=%d mode=%d result=%s",g,mode,members?"members":"empty");
        /* /battle2 alone prints the original's per-group "---" (string 0x4ea558, all.c:37673). */
        if (!mode) wos_log_event("battle_sim_separator","g=%d",g);
        members = 0;
    }
}
