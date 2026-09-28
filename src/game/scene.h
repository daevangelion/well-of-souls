/* Offline scene VM. The keyword->opcode table, the one-line-per-tick interpreter, the condition
 * language and the %-substitution resolver all follow docs/re/script.md sections 1.5-4.5 and the
 * dumped table at DAT_004FAD50 (85 records, walked by FUN_0047A6B0). Time is the virtual
 * millisecond clock (Core's clock.h), never a frame counter: the original stores GetTickCount()
 * samples and compares them, and it has no simulation tick at all. */
#ifndef WOS_SCENE_H
#define WOS_SCENE_H
#include <stdint.h>
#include "../engine/dump.h"

/* Scene-local map rules (FLAGS, DAT_004FB030); also read by battle.c for NO_REWARD/NO_HEAL. */
uint32_t scene_flags(void);
/* Clears the per-scene cookie-scoped state (TIMER/COUNTDOWN/CALL stack) on a hero switch.
 * Called by hero_create/hero_load (FrontHero). */
void scene_reset_timers(void);
/* DEPRECATED compatibility no-op: game_main.c's fixed-step callback used to drive a 60 Hz frame
 * counter for TIMER/COUNTDOWN. That was a port deviation — the original has no simulation tick
 * (docs/re/timing.md) and every deadline is a GetTickCount() sample compared through clock_ms().
 * Declared only so the tree links; Core is dropping the call in game_main.c. */
void scene_tick(void);
/* The scene number game_enter_scene() last entered, or -1 when the scene screen is not current
 * (map mode). FUN_0047A2A7 stores the number at DAT_004E4874+0x10; a travel ticket's 5th dotted
 * arg is compared against it (FUN_004A6353). */
int game_current_scene(void);
/* FUN_004859A2: evaluate a scene condition list ("T10-YES|G5", "@label" stripped). 1 = true.
 * missions.ini's `Qualify` string is the same language, so this is also registered with
 * missions_set_condition_evaluator(). */
int scene_condition(const char *condition);
/* `scene.*` dump lines for the differential replay. */
void scene_dump(DumpEmit emit, void *user);

/* The scene colour table: 4 slots x 9 objects x 7 sealed EncInt channels, the
 * original's DAT_005BB760 block. Constructed at CRT index 38 (252 seals, 1008 draws). */
void scene_boot_register(void);
/* Core calls this next to boot_register_core(), before boot_run(). */
/* FUN_0048A7DA: seal `channel` into every object of every slot. */
void scene_colour_apply(int channel);
/* FUN_004436A5, reached via FUN_004436E3: clamp to 0..2 and reseal channel 0. */
int  scene_colour_jitter(int slot,int delta);
/* FUN_0044462C, the level-up colour flash: 3 seals at object +0x58/+0x90/+0x138,
 * 12 draws. `monster_9c` is the plain dword the original reads at
 * monster-table+0x9C (0x004446D3); pass < 0 when there is no monster record. */
void scene_colour_level_flash(int slot,int obj,int monster_9c);
#endif
