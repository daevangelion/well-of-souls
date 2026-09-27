/* Solo physical combat. Reference: Souls.exe FUN_00480499, FUN_00490e7c,
 * FUN_004a7794; docs/re/battle.md. All durations are fixed 60 Hz steps. */
#include "battle.h"
#include "game.h"
#include "hero.h"
#include "scene.h"
#include "../engine/font.h"
#include "../engine/log.h"
#include "../engine/rng.h"
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

enum { ACTORS = 144, ROUND_START = 5, ROUND_ACT = 6, ROUND_DAMAGE = 7,
       ACTION_GATE = 30, ROUND_IDLE = 120, LUNGE_FRAMES = 60, FLOAT_FRAMES = 90 };
typedef struct {
    int id, ally, hp, max_hp, mp, level, offense, defense, ability[5];
    int xp, gold, x, y, ready, attacks, last_action, damage, floating;
    int sheet_index;
} Combatant;
static struct {
    Combatant actors[ACTORS];
    Sheet sheets[ACTORS];
    int count, sheets_count, state, frame, round_start, cursor, target;
    int queued, attacker, victim, animation, hp_gauge, mp_gauge, regen_start;
    int regen_initial, xp, gold, damage, attack_training;
    BattleResult result;
    Rect view;
} fight;

static int clamp(int64_t n, int low, int high)
{
    return n < low ? low : n > high ? high : (int)n;
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
static void random_group(int difficulty)
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
        /* API has no link distance percentage. Neutral 50% is invariant under
         * difficulty sign, within FUN_0049099b's 20..80 range. */
        for (i = 0; i < n; ++i) if (all || roll(100) < 50) spawn(group->members[i]);
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
void battle_begin(const int *ids, int count, int difficulty)
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
    hero = &fight.actors[0];
    hero->ally = 1;
    hero->hp = clamp(g_hero.hp,0,g_hero.max_hp);
    hero->max_hp = g_hero.max_hp;
    hero->mp = g_hero.mp;
    hero->level = clamp(g_hero.level,1,65535);
    for (i = 0; i < HERO_ABILITIES; ++i) hero->ability[i] = clamp(g_hero.ability[i],0,255);
    hero->x = 80; hero->y = 204;
    /* Equipment values supply the encrypted offense/defense fields in A96. */
    for (i = -1; i < 8; ++i) {
        int id = i < 0 ? g_hero.right_hand : g_hero.equip[i];
        if (id > 0 && id < WORLD_MAX_ITEMS && g_world.items[id].used) {
            hero->offense = clamp((int64_t)hero->offense + g_world.items[id].attack,0,65535);
            hero->defense = clamp((int64_t)hero->defense + g_world.items[id].defense,0,65535);
        }
    }
    sheet_load_skin(&fight.sheets[0],g_hero.skin);
    if (count > 0 && ids) for (i = 0; i < count && i < ACTORS-1; ++i) spawn(ids[i]);
    else random_group(difficulty);
    fight.target = first_enemy();
    wos_log_event("battle_start","monsters=%d",fight.count-1);
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
        wos_log_event("battle_won","xp=%d gold=%d",(int)(g_hero.xp-old_xp),(int)(g_hero.gold-old_gold));
    } else if (result == BATTLE_LOST) {
        g_hero.hp = 0;
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
    int miss = clamp(b->ability[ABIL_AGI]-a->ability[ABIL_DEX],0,50);
    int d, base;
    int64_t power, raw;
    if (miss && roll(100) < miss) return -1;
    power = (int64_t)(a->level+100)*(a->ability[ABIL_STR]+65)*(a->offense+5)/6500;
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
    raw = power*8000/((int64_t)(b->level+40)*(b->ability[ABIL_STA]+200));
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
static void start_attack(int actor, int target)
{
    Combatant *a = &fight.actors[actor];
    if (actor == 0) {
        int hand = g_world.classes[g_hero.klass].right_hand - 1;
        int item = g_hero.right_hand;
        int64_t pp;
        if (item > 0 && item < WORLD_MAX_ITEMS &&
            g_world.items[item].klass >= 12 && g_world.items[item].klass <= 19)
            hand = g_world.items[item].klass - 12;
        /* 0048b1ad and 00419306: charge adjusts training, then earns 20
         * hand PP. These functions do NOT heal HP/MP. */
        pp = hand >= 0 && hand < 8 ? (int64_t)(fight.hp_gauge+15)*g_hero.hand_pp[hand]/30 : 0;
        /* FUN_00424dd2(pp,10000): 10000*(1-1/(pp*.0002+1)). */
        fight.attack_training = pp > 0 ? (int)(10000*pp/(pp+5000)) : 0;
        if (hand >= 0 && hand < 8)
            g_hero.hand_pp[hand] = clamp((int64_t)g_hero.hand_pp[hand]+20,0,5000000);
        fight.hp_gauge = fight.regen_initial = 0;
        fight.regen_start = fight.frame;
        fight.queued = 0;
    }
    fight.damage = physical_damage(a,&fight.actors[target],actor != 0);
    a->last_action = fight.frame;
    a->ready = 0;
    ++a->attacks;
    fight.attacker = actor;
    fight.victim = target;
    fight.animation = 0;
    fight.state = ROUND_DAMAGE;
    fight.cursor = (actor+1)%fight.count;
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
BattleResult battle_update(const Input *in)
{
    int i, actor = -1;
    if (fight.result != BATTLE_RUNNING) return fight.result;
    ++fight.frame;
    for (i = 0; i < fight.count; ++i) if (fight.actors[i].floating) --fight.actors[i].floating;
    if (outcome()) return fight.result;
    if (in && (in->pressed['f'] || in->pressed[PLAT_KEY_ESCAPE])) {
        finish(BATTLE_FLED); return fight.result;
    }
    fight.hp_gauge = clamp(fight.regen_initial+(int64_t)(fight.frame-fight.regen_start)*
                           (fight.actors[0].ability[ABIL_STR]/2+100)/600,0,100);
    fight.mp_gauge = clamp(25+(int64_t)fight.frame*
                           (fight.actors[0].ability[ABIL_WIS]/2+100)/600,0,100);
    if (fight.target < 0 || fight.actors[fight.target].hp <= 0) fight.target = first_enemy();
    if (in) {
        if (in->pressed[PLAT_KEY_TAB]) {
            for (i = 1; i <= fight.count; ++i) {
                int t = (fight.target+i)%fight.count;
                if (!fight.actors[t].ally && fight.actors[t].hp > 0) { fight.target = t; break; }
            }
        }
        if (in->pressed['a'] || in->pressed[PLAT_KEY_SPACE]) fight.queued = 1;
        if (in->mouse_pressed & (1u << 1)) {
            for (i = fight.count-1; i > 0; --i) {
                if (!fight.actors[i].ally && fight.actors[i].hp > 0 && inside(actor_rect(i),in->mouse_x,in->mouse_y)) {
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
        if (++fight.animation >= LUNGE_FRAMES) {
            Combatant *victim = &fight.actors[fight.victim];
            victim->damage = fight.damage;
            victim->floating = FLOAT_FRAMES;
            wos_log_event("battle_hit","frame=%d attacker=%d target=%d dmg=%d",
                          fight.frame,fight.attacker,fight.victim,fight.damage);
            if (fight.damage >= 0) victim->hp = clamp((int64_t)victim->hp-fight.damage,0,victim->max_hp);
            if (fight.victim == 0) g_hero.hp = victim->hp;
            else if (!victim->hp && !victim->ally) {
                fight.xp = clamp((int64_t)fight.xp+victim->xp,0,INT_MAX);
                fight.gold = clamp((int64_t)fight.gold+roll(victim->gold+1)+victim->gold/2,0,32767);
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
            int fraction = lunge(fight.animation);
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
            if (a->damage < 0) snprintf(label,sizeof label,"MISS");
            else snprintf(label,sizeof label,"-%d",a->damage);
            font_draw(fb,sx-font_width(label)/2,sy-30-(FLOAT_FRAMES-a->floating)/4,label,0xffff70);
        }
    }
    font_draw(fb,view.x+6,view.y+view.h-24,"A/SPACE: Attack  TAB: Target",0xffffff);
    font_draw(fb,view.x+6,view.y+view.h-12,"F: Flee",0xffffff);
    fb_clip(fb,old_clip);
}
BattleResult battle_result(void) { return fight.result; }
int battle_active(void) { return fight.result == BATTLE_RUNNING; }
