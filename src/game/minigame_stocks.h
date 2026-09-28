/* GAME 7 — Stock Market (CDialog 0xC6, object at main+0x2EE0). Dialog 287x220 px.
 *
 * VAs: ctor 0x498CCE, init FUN_00499B61 (0x499B61), OnPaint 0x4997CA, chart 0x499FEA,
 * BUY 0x49A3F6, SELL 0x49A5A2, price FUN_0049A7E8 (0x49A7E8), portfolio recompute 0x49A8EE,
 * list row 0x49A9AB, ticker codec 0x499B1A/0x499B33, comma formatter 0x49974E, portfolio
 * load/save 0x4990B4/0x49923B. Dispatcher case 7 — note it does NOT CenterWindow.
 *
 * There is NO rand() anywhere in this dialog. The price is a pure deterministic function of
 * the 32-bit ticker derived from the stock's `symbol` string and of absolute wall-clock
 * minutes, so it is globally deterministic and shared by every player. There is no SetTimer
 * and no GetTickCount either: the market moves because time() moves, and it is re-sampled on
 * every paint, every sell and every portfolio recompute.
 *
 * FUN_0049A7E8 sums, over the 32 set bits of the ticker, a sine term weighted by the bit
 * index, plus a netgame "mood" term that is identically zero in solo play, and floors the
 * result at 0. Trading is 1-share granularity with no fees, no commission and no lot size;
 * a buy is capped at 10000 shares. Gold is hero+0x6C and moves only through FUN_0042BAF3.
 * Holdings live in the global portfolio block at DAT_004E487C (0x7C8 bytes), persisted to
 * <savefile>.por, not by the dialog.
 */
#ifndef WOS_MINIGAME_STOCKS_H
#define WOS_MINIGAME_STOCKS_H

#include <stdint.h>
#include "../engine/dump.h"
#include "../engine/fb.h"

int  minigame_stocks_open(void);
void minigame_stocks_update(uint32_t now_ms);
void minigame_stocks_render(Framebuffer *fb, int base_x, int base_y);
void minigame_stocks_click(int x, int y, int pressed);
void minigame_stocks_key(int vk, int pressed);
void minigame_stocks_close(void);
void minigame_stocks_dump(DumpEmit emit, void *user);

#endif
