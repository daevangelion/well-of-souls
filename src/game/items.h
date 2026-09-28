/* Item use, the trophy bag and the pet pen.
 *
 * Evidence (all VAs are Ghidra VAs in extracted/Souls.exe, ImageBase 0x400000):
 *   items.txt row layout            FUN_00482FC1  (arg2 class at record +0x88)
 *   item apply ("use")              FUN_004A6353
 *   disease cure                     FUN_004A6B55
 *   ailment get                      FUN_004A6A6A
 *   item requirements                FUN_0040D6B4
 *   item carry limit                 FUN_00403349
 *   item count / set count           FUN_0045FC2B / FUN_0045FC7A
 *   GIVE item (GP 0 clamps to one)   FUN_0042BAF3's caller, 0x49xxxx case 0x49 'I'
 *   travel ticket dispatch            FUN_0047A871
 *   trophy bag slot word             FUN_0046F60F / FUN_0046F726 / FUN_0046F779
 *   trophy bag queries               FUN_0046F9AF / FUN_0046FA2F / FUN_0046FA49 / FUN_0046FA87
 *   trophy bag room / pack / move    FUN_0046FAE9 / FUN_0046FE7B / FUN_0046FE3A
 *   trophy drop roll (rand)          FUN_0046FBF1
 *   trophy bag size cookie           FUN_0046FED9
 *   trophy bag dialog (resource 0xE7) FUN_0046FF98, cell 0x28 px
 *   pet spawn from a class-200 item  FUN_00413181 (arg3 = MONSTER id)
 *   pet EncInt construction          FUN_00413181 (10 x FUN_0049B75D then 6 x FUN_0049B71B)
 *   pet pen file                     FUN_0040FBFD (0xC5A4 bytes, 32 slots of 0x608)
 *   pet pen dialog art               FUN_00412716 (petButtons.bmp, petPen.jpg)
 *   equip slot names                 FUN_004823C4 / FUN_00482431 / FUN_004824B4
 * Docs: docs/re/script.md 6.2 and 6.6, docs/re/formats_online.md 6.7,
 *       docs/re/boot_flow.md 3 (hero record field table).
 * Owner: items.c.
 */
#ifndef WOS_ITEMS_H
#define WOS_ITEMS_H

#include <stddef.h>
#include "hero.h"
#include "world.h"
#include "../engine/dump.h"
#include "../engine/encint.h"

/* items.txt arg2 item classes. FUN_00482FC1 stores arg2 at record +0x88 and the
 * class decides how the item can be used (items.txt header, "arg 2 Class"). */
enum {
    ITEM_POTION        = 0,   /* items.bmp, arg10 HP / arg11 MP */
    ITEM_ANTIDOTE      = 1,   /* items.bmp, arg12 = the disease id it cures */
    ITEM_SPECIAL       = 2,   /* quest item; the header says "DO NOT USE YET" */
    ITEM_EXIT          = 3,   /* "get out of current trouble"; also DO NOT USE YET */
    ITEM_TRAVEL        = 4,   /* 4.mode.num.link.dropin[.sceneNum] */
    ITEM_THROWABLE     = 5,   /* darts.bmp; arg16 attack path drives the throw */
    ITEM_HELMET        = 10,
    ITEM_ARMOR         = 11,
    ITEM_RIGHT_FIRST   = 12,  /* swords, staffs, bows, music, right5..right8 */
    ITEM_RIGHT_LAST    = 19,
    ITEM_BOOTS         = 20,
    ITEM_SHIELD        = 21,
    ITEM_RING          = 22,
    ITEM_AMULET        = 23,
    ITEM_ATTR_STRENGTH = 100, /* arg12 ability points */
    ITEM_ATTR_WISDOM   = 101,
    ITEM_ATTR_STAMINA  = 102,
    ITEM_ATTR_AGILITY  = 103,
    ITEM_ATTR_DEXTERITY= 104,
    ITEM_ATTR_ALL      = 105, /* free assignment, FUN_004A6353 opens the dialog */
    ITEM_PET           = 200, /* arg3 is a MONSTER id, not an image index */
    ITEM_HTML          = 201  /* arg15 is the URL */
};

/* Equipment slot names, +EQUIP section (FUN_004824B4). Slot ids are the ones the
 * section uses, NOT the display order; right hands 2..9 are not in this table
 * (FUN_00482431 forwards them to the HANDS names, FUN_004825BF). */
enum { EQUIP_SLOT_HELMET, EQUIP_SLOT_ARMOR, EQUIP_SLOT_BOOTS, EQUIP_SLOT_SHIELD,
       EQUIP_SLOT_RING, EQUIP_SLOT_AMULET, EQUIP_SLOT_COUNT };
const char *world_equip_slot_name(int slot);
void world_equip_slot_set_name(int slot, const char *name);
/* Name for an item class 10..23, or NULL when the class is not equipment. */
const char *items_class_slot_name(int item_class);

/* FUN_0040D6B4: level + equip token + hand proficiency. 1 = usable. */
int items_usable(int item_id);
/* FUN_004A6353 with consumption. `ability` is the class-105 target ability
 * (ABIL_*), or -1 to leave the points in the unassigned pool. Returns 1 when
 * the item was used and consumed. Rejected uses do not consume. */
int items_use(int item_id, int ability);
/* FUN_004A6353 alone: applies the effect and never consumes. */
int items_apply(int item_id, int ability);
/* Class-105 leftovers, FUN_004A6353 case 0x69 -> FUN_00449DF2 + the dialog. */
int  items_attr_pool(void);
int  items_attr_assign(int ability);

/* The last message FUN_004A6353 would have put in the message line
 * (DAT_00D64CC8). Empty when there is nothing to say. */
const char *items_last_message(void);
void items_clear_message(void);

/* Class 5: arm a throw for the battle module. FUN_004A4D70 + arg16. */
int  items_arm_throw(int item_id);
int  items_throw_armed(void);
int  items_throw_item(void);
int  items_spend_throw(void);   /* FUN_0048F667: the battle module spends it once the throw lands */
void items_clear_throw(void);

/* ---- trophy bag (FUN_0046F60F..FUN_0046FED9, dialog resource 0xE7) ---- */
#define TROPHY_BAG_SLOTS   128   /* FUN_0046F726 range check 0..0x7F */
#define TROPHY_BAG_CELL     40   /* FUN_0047020F: 0x28 px cells */
#define TROPHY_BAG_KEY 0x1D43E217u

int  trophy_bag_size(int *width, int *height);   /* FUN_0046F60F */
int  trophy_bag_resize(int width, int height);   /* FUN_0046FED9 */
int  trophy_bag_get(int slot, int *trophy_id, int *count); /* FUN_0046F726 */
int  trophy_bag_set(int slot, int trophy_id, int count);   /* FUN_0046F779 */
void trophy_bag_clear(void);                     /* FUN_0046F802 */
int  trophy_bag_slot_at(int x, int y);           /* FUN_00470537, 40 px cells */
void trophy_bag_move(int from, int to);          /* FUN_0046FE3A */
void trophy_bag_pack(void);                      /* FUN_0046FE7B */
int  trophy_bag_add(int trophy_id);              /* FUN_0046F8F7 */
int  trophy_bag_remove(int trophy_id);           /* FUN_0046F95F */
int  trophy_bag_take(int trophy_id, int count);  /* FUN_0046F819 */
int  trophy_bag_have(int trophy_id, int count);  /* FUN_0046FA2F */
int  trophy_bag_count(int trophy_id);            /* FUN_0046F9AF */
int  trophy_bag_used(void);                      /* FUN_0046FA49 */
int  trophy_bag_free(void);                      /* FUN_0046FA87 */
int  trophy_bag_room(int trophy_id);             /* FUN_0046FAE9 */
int  trophy_bag_limit(int trophy_id);            /* FUN_0046FAC5 */
/* FUN_0046FBF1: one rand() per eligible trophies.txt row, in table order. */
int  trophy_bag_roll(int monster_id, int *out, int max);
/* FUN_0046FCC4: roll, then add each result to the bag. */
int  trophy_bag_award_kill(int monster_id);

/* ---- pets (class 200, FUN_00413181 / FUN_0040FBFD) ---- */
#define PET_PEN_SLOTS 32        /* 0xC100 / 0x608 */
typedef struct {
    int used;
    int monster_id;             /* the MONSTER row the pet was cloned from */
    /* The pet's sealed stat block. Disassembling FUN_00413181
     * (tools/ghidra/query.sh disasm 0x413181) shows exactly FIVE EncInts: the
     * first five FUN_0049B75D ctors build the stack slots at EBP-0x128,
     * EBP-0xF0, EBP-0xB8, EBP-0x80 and EBP-0x48, and the five FUN_0049B71B sets
     * fill those same five, in that order, from monster-record offsets +0xEC,
     * +0xF4, +0xF8, +0xFC and +0x100 - the monster's five stat fields. The
     * remaining five ctors build slots nothing later seals.
     * Cost: 10 ctors + 5 sets = 10*4 + 5*4 = 60 crt_rand(). */
    EncInt str, sta, agi, dex, wis;
    int level, hp, max_hp;        /* plain: set outside the sealed block */
    int owner_class;              /* 0x224, the class that owns the pen slot */
    int token;                    /* 0x228, negative hero serial by default */
    int flags;
} Pet;

int  pet_count(void);
Pet *pet_at(int index);
int  pet_spawn(int monster_id);      /* FUN_00413181: clone monsters[monster_id] */
int  pet_release(int index);         /* "release this pet back to the wild" */
int  pet_summon(int index);          /* bring the pet out of the pen */
int  pet_dismiss(int index);
void pet_reset(void);                /* FUN_0040FBCC */
int  pet_level_of(int monster_id);   /* the cookie-backed pet level, if any */
void pet_dump(DumpEmit emit, void *user);

void items_dump(DumpEmit emit, void *user);

/* Module init: seed the +EQUIP name table (FUN_004823C4) and clear the
 * unassigned ability pool and any armed throw. */
void items_init(void);

#endif
