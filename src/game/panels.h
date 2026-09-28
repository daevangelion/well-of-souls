/* Retail dialogs, adapted to the 364x416 main viewport.
 *
 * Original VAs (Ghidra, work/decomp/all.c):
 *   Equipment dialog  resource 0xB3 (179)  FUN_0040420E, fill FUN_0040D872,
 *                     notify FUN_0040DBBA, equip FUN_0040E04D, unequip-all
 *                     FUN_0040E0D8, sell FUN_0040E134, menu 0x8B0/0x8B1/0x8B2
 *   Items dialog      resource 200 (0xC8)  FUN_0045E493, init FUN_0045E8B3,
 *                     list control id 0x539, menu 0x8B1 Sell It / 0x8B2 Sell All,
 *                     sell FUN_0045FB04
 *   Shop dialog       resource 0xD4 (212)  FUN_00402803, init FUN_00402FBF,
 *                     fill FUN_004031B1, carry limit FUN_00403349
 *   Trophy bag        resource 0xE7 (231)  FUN_0046FF98, 40 px cells
 *   Paper doll        FUN_00466931, 48x48 cells
 *   Equip screen art  FUN_0040C371 (bkEquip.jpg + equipIcons.bmp)
 *   OFFER2 ordering   FUN_0047df.. case 0x12; sell price gp/2
 *   Art class mapping FUN_00482431; equipment deltas all.c:9645-9659
 * Owner: panels.c. */
#ifndef WOS_PANELS_H
#define WOS_PANELS_H
#include "../engine/ui.h"
#include "../engine/dump.h"

typedef enum {
    PANEL_NONE, PANEL_ITEMS, PANEL_SPELLS, PANEL_EQUIP, PANEL_STATS, PANEL_TRAIN,
    PANEL_SHOP, PANEL_TROPHY, PANEL_PET, PANEL_COUNT
} PanelKind;

void panel_open(PanelKind kind);
/* Consumes the temporary script arguments before returning. */
void panel_open_shop(const char *const *args, int argc, int offer2);
/* FUN_00412716: the pet pen (petButtons.bmp, petPen.jpg). */
void panel_open_pets(void);
int panel_active(void);
void panel_update(const Input *input);
void panel_render(Framebuffer *fb);
void panel_close(void);

/* Core's semantic MFC dialog op (`at <ms> dialog <id> <control>=<value>... ok|cancel`).
 * Control ids are the original's; see the table at the top of panels.c. */
typedef struct { int id; int ctrl; int value; int ok; } DialogOp;
int  panel_dialog_op(const DialogOp *op);
void panel_dialog_register(int (*fn)(const DialogOp *));

void panels_dump(DumpEmit emit, void *user);

#endif
