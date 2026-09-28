/* GAME 3 — "The Search for Pi at Home" (CDialog 0xBF, class at main+0x1B20).
 *
 * VAs: ctor 0x474162, OnInitDialog 0x4742DA, start FUN_004742F0 (0x4742F0), OnPaint 0x47433D
 * (a no-op CPaintDC, so the point cloud is persistent), sample 0x47437E, in-circle test
 * 0x4743B5, OnTimer 0x4743E8. Dialog 142x178 px, centred on the view.
 *
 * It is a self-drawing Monte-Carlo estimate of pi and has NO player input, NO gold, NO cookie
 * and NO win condition: it runs until the window closes. The only thing the script or the
 * player observes is the burned RNG stream.
 *
 * FUN_004742F0 zeroes four doubles (+0x68 in-circle count, +0x6C dead, +0x70 total, +0x74 dead)
 * and arms SetTimer(id 2, 50 ms). Each OnTimer runs 100 samples, and each sample burns
 * 2 rand() for the plot point plus two calls of FUN_0047437E (1 + 8 rand() folded into a
 * 16-bit value by `u = (u << 4) ^ (r >> 4)`, masked with 0x7FFFFFFF) = 20 rand() per sample,
 * so 2000 rand() per 50 ms tick. That is 40 rand() per sample in total and it is fixed.
 */
#ifndef WOS_MINIGAME_PI_H
#define WOS_MINIGAME_PI_H

#include <stdint.h>
#include "../engine/dump.h"
#include "../engine/fb.h"

/* FUN_004742F0's SetTimer: id 2, 50 ms. */
#define MG_PI_TIMER_MS 50
#define MG_PI_SAMPLES_PER_TICK 100
#define MG_PI_TICKS_PER_SAMPLE_LOOP 1

int  minigame_pi_open(void);
void minigame_pi_update(uint32_t now_ms);
void minigame_pi_render(Framebuffer *fb, int base_x, int base_y);
void minigame_pi_click(int x, int y, int pressed);
void minigame_pi_key(int vk, int pressed);
void minigame_pi_close(void);
void minigame_pi_dump(DumpEmit emit, void *user);

#endif
