/* GAME 1 — Slobber Slots (CDialog 0xa7, class at main+0x14C4).
 *
 * The original: FUN_00421563 case 1 creates the dialog and calls FUN_004666B1 (0x4666B1);
 * FUN_0046664C (0x46664C, OnClose) and FUN_0046668E (0x46668E) kill the timer; FUN_00466788
 * (0x466788) is the per-tick handler and FUN_00466931 (0x466931) paints.
 *
 * Exact rules, all from the disassembly:
 *
 * Open (FUN_004666B1, 0x4666B1)
 *   armed = 0; last_sound = 0; the spin button is enabled.
 *   For each of the 3 wheels: pos[i] = rand() % 0x12C0 (4800 = 100 symbols * 48 px),
 *                              count[i] = 0.                       -> 3 rand()
 *   FUN_00466327 (0x466327) then loads slots.ini and rebuilds the three 100-slot wheels,
 *   consuming rand() through FUN_004661E4 (0x4661E4).
 *
 * Spin button (0x466BBB)
 *   The hero's gold is at hero+0x6C. If gold < 10 the spin is refused (a sound and the
 *   "You lack sufficient funds to bet." bubble) and NO rand() is consumed.
 *   Otherwise, for each of the 3 wheels:
 *       pos[i]   = rand() % 0x12C0
 *       count[i] = 10 + rand() % 10          (so 10..19 ticks)    -> 6 rand()
 *   The 10 GP is then charged: sprintf(buf, "G%d", 10) and
 *   FUN_00484E72 (0x484E72) with sign -1 removes it. Total wagered += 10, spin count += 1.
 *   armed = 1.
 *
 * Tick (FUN_00466788, 0x466788)
 *   For each wheel with count[i] > 2:
 *       pos[i] = (pos[i] + count[i]) % 0x12C0
 *       sym    = pos[i] / 0x30               (48 px per symbol)
 *       if sym != last[i]:  last[i] = sym; count[i]--;
 *           and if GetTickCount() - last_sound > 500, play the reel sound and stamp last_sound.
 *   Once no wheel is spinning and armed != 0, settle (see below) and armed = 0.
 *
 * Settle (0x466830 onwards, verified against the objdump at 0x466830)
 *   matches = 0; ref = -1;
 *   for i in 0..2 (wheel stride 100 ints):
 *       sym = wheel[i][ (last[i] + 1) % 100 ];      <-- the +1 is the original's own off-by-one
 *       if i == 0: ref = sym
 *       if sym == ref and ref != -1: matches++
 *   if matches == 3: pay = triple[ref]   (DAT_004F23A8)
 *   elif matches == 2: pay = double[ref] (DAT_004F23C8)
 *   if pay > 0: total_won += pay; grant pay gold; show the win bubble.
 *
 * Note the "two of a kind" rule matches slots.ini's own documentation: the comparison always
 * uses the FIRST wheel's symbol, so XX0 and X0X pay but 0XX does not.
 */
#ifndef WOS_MINIGAME_SLOTS_H
#define WOS_MINIGAME_SLOTS_H

#include <stdint.h>
#include "../engine/dump.h"
#include "../engine/fb.h"

int  minigame_slots_open(void);
void minigame_slots_update(uint32_t now_ms);
void minigame_slots_render(Framebuffer *fb, int base_x, int base_y);
void minigame_slots_click(int x, int y, int pressed);
void minigame_slots_key(int vk, int pressed);
void minigame_slots_close(void);
void minigame_slots_dump(DumpEmit emit, void *user);

#endif
