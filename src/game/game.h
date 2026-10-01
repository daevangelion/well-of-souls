/* Game-mode transitions shared by front-end, map, scene and battle modules.
 * Original: the main window switches between MAP mode (aerial walking) and SCENE mode (side view,
 * where all dialog and fighting happen); docs/re/formats_online.md section 5.2. */
#ifndef WOS_GAME_H
#define WOS_GAME_H

#include "world.h"
#include "../engine/dump.h"

#define GAME_WORLD_DEFAULT "Evergreen"

/* Front end (front.c): title/main menu (emits boot_menu), world select, Well of Souls hero picker
 * (NEW / RESTORE / INCARNATE; emits hero_ready when a hero is created or loaded). */
void game_go_front(void);
void game_go_well(void);

/* Map mode (mapview.c). Loads map `map_id`, places the hero just above link `link` (or into it
 * when drop_in != 0, which triggers that link's scene), emits `map_enter map=<id> x= y=`. */
void game_enter_map(int map_id, int link, int drop_in);
/* Return to the current map after a scene, above the link the scene was entered from. */
void game_return_to_map(void);
const Map *game_current_map(void);

/* Scene mode (scene.c). Runs quest.txt scene `scene_no`. `link` = the link it was entered from
 * (supplies default background/theme), or NULL. */
void game_enter_scene(int scene_no, const Link *link);

/* Random encounter (map -> scene 2). The monster list is consumed by the next argument-less FIGHT.
 * count == 0 means "pick from groups.txt by the link difficulty".
 * distance_pct is the clamped 20..80 Manhattan-distance inclusion percentage,
 * inverted for negative groups (FUN_0049099b). */
void game_set_pending_fight(const int *monster_ids, int count, int difficulty, int distance_pct);
int  game_take_pending_fight(int *monster_ids, int max, int *difficulty, int *distance_pct);

/* Resolve world MIDI first, then shared MIDI; NULL/empty stops playback. */
void game_music(const char *midi_name);

/* Map mode internals the rest of the port needs (mapview.c).
 *
 * map_set_waypoints maps FUN_004620F3's `FUN_00467312(7)` gate, option 27 in the Options
 * dialog ("Enable automatic Way Point calculations", read into DAT_006840D0[7] at
 * 0x466F3F). It is the ONLY thing that makes a map click run FUN_0046206D -> FUN_00461DCC;
 * retail leaves it off, so the hero walks in a straight line and stops at an obstacle.
 * map_close_overlays is CloseOverlaysGoMap(1) (0x420714): the encounter stamp DAT_004F2220,
 * the latch when the hero comes back from a scene, the hunt stamp cleared, front state 6.
 * map_dump is the `map.*` differential namespace (parity_plan contract 3). */
void map_set_waypoints(int on);
void map_close_overlays(int from_scene);
void map_dump(DumpEmit emit, void *user);

#endif
