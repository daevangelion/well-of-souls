/* GAME 2 — Monster Racer (CDialog 0xB7, class at main+0x15F4).
 *
 * VAs: ctor 0x405D88, OnInitDialog 0x405F07 (racers.txt), setup FUN_004066A9 (0x4066A9),
 * OnPaint/FUN_00405FD1 (0x405FD1), OnIdle 0x40664F, OnLButtonDown 0x4067B4, OnClose 0x405EC2.
 * Dispatcher: FUN_00421563 (0x421563) case 2. The dialog is 280x170 px, centred on the view.
 *
 * State machine (this+0x12C): 0 = pick, 1 = racing, 2 = result. There is NO SetTimer: the
 * animation advances inside OnPaint, and OnIdle (gated at 20 Hz by FUN_0040A7C7) invalidates
 * the window whenever state == 1. The port therefore advances on clock_ms() with no divisor.
 *
 * Setup FUN_004066A9 draws 4 racers from racers.txt into the four client quadrants — 8 rand().
 * Each paint then consumes 4 + Binomial(4, 0.2) rand() for the racer sprite frames, so the
 * repaint cadence is part of the global RNG stream and must be reproduced.
 *
 * Positions are pure functions of elapsed time: x = speed * (clock_ms() - t_start) / 1000, so
 * the LOWEST speed finishes FIRST. The race ends when every racer's x has passed the client
 * width. 100 cookies are charged on the click; 100 come back only when the player's racer has
 * the maximum speed value, and the "500" branch is dead because place1 is only ever -1.
 */
#ifndef WOS_MINIGAME_RACER_H
#define WOS_MINIGAME_RACER_H

#include <stdint.h>
#include "../engine/dump.h"
#include "../engine/fb.h"

int  minigame_racer_open(void);
void minigame_racer_update(uint32_t now_ms);
void minigame_racer_render(Framebuffer *fb, int base_x, int base_y);
void minigame_racer_click(int x, int y, int pressed);
void minigame_racer_key(int vk, int pressed);
void minigame_racer_close(void);
void minigame_racer_dump(DumpEmit emit, void *user);

#endif
