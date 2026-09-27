/* Fight resolution inside a scene (quest FIGHT opcode, docs/re/script.md section 2.3; combat
 * docs/re/battle.md). Owner: battle.c. The scene module owns the screen and delegates to this
 * API while a fight is active. */
#ifndef WOS_BATTLE_H
#define WOS_BATTLE_H

#include "../engine/fb.h"
#include "../engine/ui.h"

typedef enum {
    BATTLE_NONE = 0,
    BATTLE_RUNNING,
    BATTLE_WON,
    BATTLE_LOST,
    BATTLE_FLED
} BattleResult;

/* Begin a fight against monsters.txt ids (negative id = ally, FIGHT 1,2,-4 syntax).
 * count == 0: pick from groups.txt using `difficulty` (FUN_0049099b), else the map .mon list.
 * Emits `battle_start monsters=<n>`. */
void battle_begin(const int *monster_ids, int count, int difficulty);
/* Advance one 60 Hz step. Returns BATTLE_RUNNING until resolved; on win emits
 * `battle_won xp=<n> gold=<n>` and applies rewards to g_hero. */
BattleResult battle_update(const Input *input);
/* Draw the fight (combatants, effects, damage numbers) into the scene view rect, which maps the
 * 360x256 logical scene space. */
void battle_render(Framebuffer *fb, Rect view);
BattleResult battle_result(void);
int battle_active(void);

#endif
