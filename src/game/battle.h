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
