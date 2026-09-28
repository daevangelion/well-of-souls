/* GAME 4 — Blackjack (CDialog 0xC0, class at main+0x1C18, class body 0x4045B0).
 *
 * VAs: ctor 0x40AB49, init FUN_0040B9C6 (0x40B9C6), Deal FUN_0040B8DE (0x40B8DE), Stand
 * FUN_0040B783 (0x40B783), state setter 0x40B9E6, scoring FUN_0040AE6C (0x40AE6C) shared by
 * both hands, shuffle 0x40ADBB. Dialog 196x167 px, centred on the view.
 *
 * Member layout: player cards this+0x68 (101 ints), player count this+0x1F8, dealer cards
 * this+0x1FC (100 ints), dealer count this+0x38C, wager this+0x60, state this+0x64, payout
 * this+0x778, result clock t0 this+0x780, session profit this+0x784, reset delay this+0x788
 * = 3000, status string this+0x390 (1000 bytes), three buttons at this+0x78C/0x7CC/0x80C and
 * the bet CEdit at this+0x84C. Object size 0x88C.
 *
 * One 52-card shoe at DAT_00538670 with a global cursor at DAT_004DE038, reshuffled when
 * exhausted. Gold is hero+0x6C and is moved only through FUN_00484E72("G%d") -> FUN_0042BAF3.
 *
 * The shuffle at 0x40ADBB is 52000 rand() calls (the loop bound is the literal 52000, not
 * 51), each drawing `j = (rand() >> 4) % 52`.
 *
 * There is NO SetTimer. Instead each dealt card costs Sleep(250), and the result is held for
 * 3000 ms measured with GetTickCount and checked from the dialog's OnIdle on the app's ~20 ms
 * idle gate. The port uses clock_ms() with the same constants and no frame divisor.
 */
#ifndef WOS_MINIGAME_CARDS_H
#define WOS_MINIGAME_CARDS_H

#include <stdint.h>
#include "../engine/dump.h"
#include "../engine/fb.h"

int  minigame_blackjack_open(void);
void minigame_blackjack_update(uint32_t now_ms);
void minigame_blackjack_render(Framebuffer *fb, int base_x, int base_y);
void minigame_blackjack_click(int x, int y, int pressed);
void minigame_blackjack_key(int vk, int pressed);
void minigame_blackjack_close(void);
void minigame_blackjack_dump(DumpEmit emit, void *user);

#endif
