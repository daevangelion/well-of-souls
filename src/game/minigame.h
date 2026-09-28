/* The GAME opcode's mini-games (Souls.exe 0x421529 -> 0x478E04 -> 0x421563).
 *
 * `GAME n` (quest-script opcode 0x11) only *arms* a mini-game: FUN_00421529 (0x421529) stores the
 * game number in the global DAT_004E18A4 and calls FUN_00478E04 (0x478E04), which registers a
 * "GAME" button (FUN_00478673, 0x478673, slot 6, image "buttonGame.bmp", message id 0x493) on the
 * scene button bar. The mini-game window is created later, when the player clicks that button, by
 * FUN_00421563 (0x421563), which switches on DAT_004E18A4:
 *
 *   1 -> CDialog 0xa7  init 0x4666B1   "Quadris"-style tactics? see minigame_dump game= mapping
 *   2 -> CDialog 0xb7  init 0x4066A9
 *   3 -> CDialog 0xbf  init 0x4742F0
 *   4 -> CDialog 0xc0  init 0x40B9C6
 *   5 -> CDialog 0xc4  init 0x412716
 *   6 -> CDialog 0xc5  init 0x46E29B
#include "../engine/dump.h"
 *   7 -> CDialog 0xc6  init 0x499B61
 *   8 -> CWnd "Quadris" 400x430, init 0x46A698
 *
 * The opcode itself never blocks the script interpreter: case 0x11 in the scene VM does
 * `atoi(arg); FUN_00421529(); pc++` and returns state "continue". So minigame_start() arms the
 * game and returns immediately; the mini-game then runs as an overlay that the scene keeps ticking.
 *
 * Game numbers are 1-based exactly as written in quest.txt (quest.txt:1207-1214). `game` 0 closes
 * any open mini-game (FUN_00421529 calls FUN_00421563(0, n) on the n<1 path). Numbers outside
 * 1..8 are refused, matching the original's dispatcher, which has no default case.
 *
 * Every rand() in the original — including purely cosmetic ones — is mirrored by exactly one
 * crt_rand() at the same point, so the RNG stream stays in step with the original. See
 * src/engine/rng.h and the per-game notes in work/minigame_re/.
 */
#ifndef WOS_MINIGAME_H
#define WOS_MINIGAME_H

#include <stdint.h>
#include "../engine/fb.h"
#include "../engine/ui.h"
#include "../engine/dump.h"

/* Game numbers, 1-based as in quest.txt. 0 means "close whatever is open". */
enum {
    MINIGAME_SLOTS      = 1,
    MINIGAME_RACER      = 2,
    MINIGAME_PI         = 3,
    MINIGAME_BLACKJACK  = 4,
    MINIGAME_POKEGATCHI = 5,
    MINIGAME_ASTEROIDS  = 6,
    MINIGAME_STOCKS     = 7,
    MINIGAME_TETRIS     = 8,
    MINIGAME_COUNT      = 8
};

/* Arm mini-game `game` (1..8) and show the GAME button, as FUN_00421529 does.
 * `label` is the script text of the line following `GAME n`; it is recorded for the log and dump
 * only — the original's interpreter ignores it. Returns 1 when a game is now armed (0 when the
 * number is out of range or a game is already open). */
int minigame_start(int game, const char *label);

/* Dismiss the GAME button without opening anything (FUN_00478D00 path, all.c:25356). */
void minigame_disarm(void);

/* The armed game number, or 0 when no GAME button is showing (DAT_004E18A4). */
int minigame_armed(void);

/* Non-zero while a mini-game window is open (the original's DAT_004E483C guard). */
int minigame_active(void);

/* Non-zero on the tick a mini-game closes. Sticky until minigame_finished_clear(). */
int minigame_finished(void);
void minigame_finished_clear(void);

/* The closed game's number, valid after minigame_finished(); 0 if none closed yet. */
int minigame_result(void);

/* Virtual-clock tick. Advances every open mini-game. `now_ms` is Core's clock_ms(). */
void minigame_update(uint32_t now_ms);

/* Draw the open mini-game, if any. `base` is its top-left in 640x480 client coordinates. */
void minigame_render(Framebuffer *fb, int base_x, int base_y);

/* Forward a click/key to the open mini-game. `x`,`y` are 640x480 client coordinates, already
 * translated by the caller into the mini-game's own client space. */
void minigame_click(int x, int y, int pressed);
void minigame_key(int vk, int pressed);

/* Close the open mini-game (equivalent of the vtable +0x60 close slot). */
void minigame_close(void);
/* Per-game state for the differential test: minigame.<key>. */
void minigame_dump(DumpEmit emit, void *user);
/* True if the given game number is one the original implements. */
int minigame_exists(int game);

#endif
