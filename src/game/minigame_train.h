/* GAME 5 — "Pokegatchi Training Center", whose dialog is internally titled "Pet Center"
 * (CDialog 0xC4, object at main+0xBEC, class body rooted at ctor 0x411348, vtable
 * PTR_LAB_004C8128). Dialog 326x213 px. Opened by FUN_00421563 (0x421563) case 5, which
 * notably does NOT CenterWindow it.
 *
 * The quest text's "same as 'train pet' on Equip Screen" is literally true at the object
 * level: there is exactly one CDialog::Create(..., 0xC4, ...) in the binary and only two
 * sites that show it, so the Equip screen and GAME 5 share one implementation. It is NOT the
 * port's PANEL_TRAIN, which is a PP/hand-vs-element skill trainer with no counterpart here.
 *
 * It is a pet pen, not a gambling game: it never touches hero gold, cookies, PP or XP. The
 * hero record is read only for the hero's name (0x14) and HP (0x70).
 *
 * State lives in a 32-slot pet array at DAT_004E4878 (stride 0x608, 0xC100 bytes total),
 * the current pet at DAT_00538C40. Each pet has 8 normalised "need" meters in 0..1,000,000
 * units — Wild / Sleepy / Hungry / Stupid / Sickly / Lazy / Dirty / Angry — and an `energy`
 * field. The five care buttons subtract from the relevant meter and from energy; the meters
 * drift upward on their own via the periodic FUN_00412840.
 *
 * There is no SetTimer (the binary has only six SetTimer calls, none in this class), so
 * FUN_00412797's KillTimer is a no-op guard and FUN_004126C8's OnTimer is dead. The
 * animation clock is a GetTickCount delta inside the paint path.
 *
 * The only rand() in the dialog are visual: the pet sprite frame pick in the pen draw
 * (1-2 rand() per pet per paint, FUN_004135DA) and the soap-bubble spray (3 rand() x 50 per
 * paint, FUN_00410D9A). No stats and no gold depend on them.
 */
#ifndef WOS_MINIGAME_TRAIN_H
#define WOS_MINIGAME_TRAIN_H

#include <stdint.h>
#include "../engine/dump.h"
#include "../engine/fb.h"

/* the 8 need meters, in the order FUN_00410D9A / the paint path walk them */
enum { NEED_WILD, NEED_SLEEPY, NEED_HUNGRY, NEED_STUPID, NEED_SICKLY,
       NEED_LAZY, NEED_DIRTY, NEED_ANGRY, NEED_COUNT };

int  minigame_train_open(void);
void minigame_train_update(uint32_t now_ms);
void minigame_train_render(Framebuffer *fb, int base_x, int base_y);
void minigame_train_click(int x, int y, int pressed);
void minigame_train_key(int vk, int pressed);
void minigame_train_close(void);
void minigame_train_dump(DumpEmit emit, void *user);

#endif
