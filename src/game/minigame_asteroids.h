/* GAME 6 — Asteroids (CDialog 0xC5, vtable PTR_LAB_004CE958). Dialog 383x277 px, centred.
 *
 * VAs: ctor 0x46DEB9, OnInitDialog 0x46DFED, start FUN_0046E29B (0x46E29B), level start
 * 0x46E3AA, spawn one asteroid 0x46E14C, key poll 0x46E457, the simulation step
 * FUN_0046E4D1 (0x46E4D1), OnPaint 0x46ECC3, per-idle update 0x46E060. Dispatcher case 6.
 *
 * There is NO SetTimer in the module (FUN_0046E043's KillTimer is dead because this+0x78 is
 * never set). The simulation is stepped from the app's OnIdle chain
 * (0x41BDB4 -> 0x4246B6 -> 0x46E060) at the 19 ms idle gate, and input is GetAsyncKeyState
 * polling only — no keyboard message handler and no mouse at all.
 *
 * State: ship x/y/heading/velocity at this+0x3C0..0x3E4, W/H at 0x3D8/0x3DC, key latches at
 * 0x3A0..0x3BC, score/kills/shots/wave/ships/shields/invuln/gameover at 0x7D0..0x7F8, plus
 * 20 asteroid records (0x100 B) at 0x698FF0, 48 explosion records (0x78 B) at 0x69A3F0 and
 * 20 bullet records (40 B) at 0x69BA70.
 *
 * RNG: 200 rand() per (re)start for the starfield (0x46E29B), 3 per field asteroid
 * (0x46E3AA), 16 per asteroid (0x46E14C), and 1-3 per split kill. Nothing else consumes rand().
 *
 * Payout: NONE. No asteroid code writes the hero record or any gold field, and nothing
 * outside reads the score, so the script VM sees unchanged state after the game.
 */
#ifndef WOS_MINIGAME_ASTEROIDS_H
#define WOS_MINIGAME_ASTEROIDS_H

#include <stdint.h>
#include "../engine/dump.h"
#include "../engine/fb.h"

int  minigame_asteroids_open(void);
void minigame_asteroids_update(uint32_t now_ms);
void minigame_asteroids_render(Framebuffer *fb, int base_x, int base_y);
void minigame_asteroids_click(int x, int y, int pressed);
void minigame_asteroids_key(int vk, int pressed);
void minigame_asteroids_close(void);
void minigame_asteroids_dump(DumpEmit emit, void *user);

#endif
