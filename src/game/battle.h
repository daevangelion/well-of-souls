/* Fight resolution inside a scene (quest FIGHT opcode, docs/re/script.md section 2.3; combat
 * docs/re/battle.md). Owner: battle.c. The scene module owns the screen and delegates to this
 * API while a fight is active. */
#ifndef WOS_BATTLE_H
#define WOS_BATTLE_H

#include "../engine/dump.h"
#include "../engine/fb.h"
#include "../engine/ui.h"

typedef enum {
    BATTLE_NONE = 0,
    BATTLE_RUNNING,
    BATTLE_WON,
    BATTLE_LOST,
    BATTLE_FLED
} BattleResult;

/* FIGHT opcode modifiers (FUN_0047d577 case 0xb, 0x47f9c8). */
enum {
    BATTLE_MOD_RANDOM   = 1u<<0, /* '*' or 'FIGHT 0': the listed monsters PLUS the normal
                                   * map/group encounter, which otherwise replaces them. */
    BATTLE_MOD_STICKY   = 1u<<1, /* FIGHT2 (DAT_00502b14 = 3): the host is dragged in until it
                                   * has stood still for ~20 s; flee unlocks at 20 s, not 30 s. */
    BATTLE_MOD_PET_HATE = 1u<<2  /* '+n': the listed monsters hunt the host's pets (FUN_0048b07f). */
};

/* Begin a fight against monsters.txt ids (negative id = ally/mercenary, FIGHT 1,2,-4 syntax).
 * count == 0: pick from groups.txt using `difficulty` (FUN_0049099b), else the map .mon list.
 * distance_pct is the map's clamped/sign-adjusted per-member inclusion probability.
 * mods is a mask of BATTLE_MOD_*; BATTLE_MOD_RANDOM adds the ordinary encounter on top of an
 * explicit list instead of replacing it.
 * Emits `battle_start monsters=<n> distance=<pct>`. */
void battle_begin_ex(const int *monster_ids, int count, int difficulty, int distance_pct,
                     unsigned mods);
/* battle_begin_ex(ids, count, difficulty, distance_pct, 0). */
void battle_begin(const int *ids, int count, int difficulty, int distance_pct);
/* SceneRunByNumber's draws: the scene block's jitter, then the local hero's combatant. Every
 * scene start makes them, in that order; battle_begin_ex uses the hero made here. */
void battle_scene_begin(void);
void battle_scene_hero(void);
/* '+n' from the FIGHT opcode: `pets` hero pets are present and the fight's monsters attack
 * them before the host (rec[+0x2B8] in FUN_0048b07f). 0 clears. */
void battle_set_pets(int pets);

/* Open the in-fight known-spell chooser (also bound to S). */
void battle_open_spells(void);
/* Advance one loop iteration. Returns BATTLE_RUNNING until resolved; on win emits
 * `battle_won xp=<n> gold=<n>` and applies rewards to g_hero. Timing is the original's
 * GetTickCount arithmetic through clock_ms() (FUN_00490e7c), not a frame count. */
BattleResult battle_update(const Input *input);
/* Dispatch a click on combatant `slot` as the scene window's WM_LBUTTONDOWN would in
 * target-selection mode (FUN_0048b1ad): validates the target and queues the queued action.
 * 1 = the click was accepted, 0 = ignored (dead/illegal/not this hero's turn). */
int battle_click_actor(int slot);
/* The original broadcasts FUN_004306f6(0x41, <event>, <slot>, <code>, ...) to the scene, which
 * re-enters the finished script at @eventActorClick<slot> / @eventActorAttack<slot> /
 * @eventActorSpell<slot>. `kind` is exactly "click", "attack" or "spell". A non-zero return asks
 * the scene to run that label. battle.c calls it at the original's broadcast points. */
typedef int (*BattleSceneEvent)(const char *kind, int slot, int code);
void battle_set_scene_event(BattleSceneEvent fn);
/* FUN_0047AB07 at 0x47AEA1 and FUN_00484D52 at 0x484E0B both do, after their own work:
 *     rec = FUN_0048AE32(hero_serial);   if (rec) enc_set(&rec[0x628], rec[0x64]);
 * i.e. the local player's COMBATANT level seal is re-derived from hero[0x64]. Call this at
 * those sites. It is a no-op when no fight is running, which is what the original does offline:
 * FUN_0048AE32 returns NULL unless FUN_0041bd7b() is set. */
void battle_hero_reseal_level(void);
/* FUN_004A6B55, the cure, applied to combatant `slot`'s disease `n` (1..24 - the original's
 * bound, `iVar4 > -0x19`). The three-way branch is the original's, not a blanket clear:
 *   n >= 20, or n == 11  -> one step off, the counter clamped at 0
 *   n in 2..8            -> the counter is zeroed outright
 *   n == 1, 9, 10, 12..19 -> no effect at all, silently
 * Returns 1 if the counter moved. Slots 20..24 are the stat debuffs, carried in ability_shift[],
 * which is where the original's rec[0x398 + 4n] lands for them too. */
int battle_cure(int slot, int n);

/* FUN_00414059's `param_1 == 0` arm, the AUTHORED-STAT SPAWNER, exported for the pet pen
 * (FUN_004142F2 -> 0x00414837) and used by the summon effects (FUN_004A6E58 -> 0x004A70B4).
 * Every field below is one of FUN_00414059's parameters - the comment on each gives its
 * number, and the numbers run 2,4,5,6,7,8,9,10,11,12..16 in the order the original writes
 * them, not in the order declared here. The monster_id is param_3, the id FUN_00480499
 * builds the slot from, so it must be a live monsters.txt row.
 *
 * Spends exactly what the original spends: the ordinary spawn (FUN_00491E45's four seals at
 * 0x00491F35..59 plus FUN_00480499's five at 0x00480593..604), then this arm's five seals at
 * 0x00414195/1AC/1C3/1D1/1E8, then one rand()%32 at 0x00414273, then FUN_00491BB7's two
 * seals at 0x00491D2E/0x00491D4E. Returns the new combatant's slot, or -1 if it was
 * refused - which is FUN_00414059's own return value, the number FUN_004142F2 propagates. */
typedef struct {
    int monster_id;   /* param_3 */
    int allegiance;   /* param_2: rec[0x114]; 0 = the caster's own side */
    int level;        /* param_4 */
    int hp;           /* param_5 */
    int max_hp;       /* param_6, rec[0x2A8], plain */
    int mp;           /* param_7 */
    int max_mp;       /* param_8, rec[0x2AC], plain */
    int offense;      /* param_9 */
    int defense;      /* param_10 */
    int xp;           /* param_11, rec[0x2B4], plain */
    int ability[5];   /* param_12..16 -> rec[0x10B], [0x10C], [0x10D], [0x10E], [0x10F] */
} BattleSpawn;
int battle_spawn_authored(const BattleSpawn *s);

/* FUN_00414059's `param_1 == 1` arm, the RECALL (0x0041409F..0x00414156). The argument is NOT
 * param_1 and NOT a slot: it is `allegiance`, the value the original compares against the
 * combatant's rec[0x114]. Every live combatant that is a monster (rec[+4] == -1) with that
 * allegiance prints `battle_recalled slot=<n> monster=<id> owner=<id>` and is dropped by
 * FUN_0048E16E. It draws nothing - all five of FUN_00414059's seals are on the spawner arm.
 * Returns the index of the LAST combatant it dropped, or -1 if it dropped none, which is the
 * original's `local_8` and exactly what FUN_004142F2 hands to FUN_004306F6(0x41, 4, ...). */
int battle_recall(int allegiance);

/* Draw the fight (combatants, effects, damage numbers) into the scene view rect, which maps the
 * 360x256 logical scene space. */
void battle_render(Framebuffer *fb, Rect view);
BattleResult battle_result(void);
int battle_active(void);
/* `battle.*` dump lines: state, per-slot id/hp/maxhp/mp/charge/x/y/state, scene totals. */
void battle_dump(DumpEmit emit, void *user);

/* Chat-command battle simulator (FUN_00432c28): mode 1 = /battle (all-vs-hero), 0 = /battle2,
 * 2 = /battle3 (all-vs-all). Emits the same battle_* events as a live fight. */
void battle_run_simulator(int mode);

#endif
