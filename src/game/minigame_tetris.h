/* GAME 8 — "Quadris" (Tetris). A raw MFC CWnd, not a CDialog, at main+0x3328 (HWND at
 * main+0x3348), created with CreateEx(..., "Quadris", WS_POPUP|WS_CAPTION|WS_SYSMENU,
 * 0,0, 400, 430, ...) and then CenterWindow'd over the view. Dispatcher case 8.
 *
 * VAs: ctor 0x469DC7, WM_CREATE 0x469EE8, the step FUN_00469FFC (0x469FFC), new game
 * 0x469F4E, OnPaint 0x46A0B2, close 0x469ED5, init FUN_0046A698 (0x46A698), tick 0x46A6DB.
 *
 * There are NO controls, NO SetTimer and no mouse or WM_KEY handlers: all input is
 * GetAsyncKeyState polled once per idle pass, and the step is driven from the MAIN window's
 * OnIdle (0x41BDB4 -> 0x4246B6), gated only on IsWindow+IsWindowVisible, ending with
 * InvalidateRect over the whole client. Fall speed is therefore expressed in pixels/second
 * off a GetTickCount base taken at spawn, never off a frame rate.
 *
 * The board is 10x20 int32 at 0x00698C98; the piece queue is 5 ints at 0x00698FB8 where
 * slot 0 doubles as the HOLD slot; the shape and colour tables live in .rdata at
 * 0x4F6DD8 / 0x4F6DE8. State at this+0xB8: 0 title, 1 playing, 2 "paused" (dead code),
 * 3 line-clear flash.
 *
 * RNG: 5 rand() at a new game and 1 per piece locked. Nothing in the paint path consumes any.
 * Payout: none — nothing here writes the hero record at 0x0067FBF8, so score and high score
 * are cosmetic RAM only and the script VM sees unchanged state.
 */
#ifndef WOS_MINIGAME_TETRIS_H
#define WOS_MINIGAME_TETRIS_H

#include <stdint.h>
#include "../engine/dump.h"
#include "../engine/fb.h"

/* CreateEx's 400x430 window rect; the client is that minus the caption and borders. */
#define MG_TETRIS_WIN_W 400
#define MG_TETRIS_WIN_H 430

int  minigame_tetris_open(void);
void minigame_tetris_update(uint32_t now_ms);
void minigame_tetris_render(Framebuffer *fb, int base_x, int base_y);
void minigame_tetris_click(int x, int y, int pressed);
void minigame_tetris_key(int vk, int pressed);
void minigame_tetris_close(void);
void minigame_tetris_dump(DumpEmit emit, void *user);

#endif
