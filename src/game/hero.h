/* The local (solo) hero: stats, position, inventory, tokens; creation and save/load.
 *
 * The ONLY save format is the original's: a fixed 0x16CC-byte record with a
 * checksum in the last 4 bytes (FUN_00416ABB, 0x416ABB), written by
 * FUN_00417F1B (0x417F1B) to game_save_path()/<world>/savedHeroes/<Name>.her.
 * The old portable .wsh format is gone; there is no reader and no writer for
 * it any more. hero_record_encode/decode map the in-memory Hero onto that
 * record field for field, so a port-written .her and an original-written .her
 * are the same bytes for the same hero.
 *
 * Evidence: docs/re/boot_flow.md sections 3 and 4 (new soul dialog, the .her
 * format and field table), docs/re/script.md section 8.2 (hero creation from
 * levels.txt), docs/re/battle.md section 9 (XP/level). Owner: hero.c. */
#ifndef WOS_HERO_H
#define WOS_HERO_H

#include <stddef.h>
#include <stdint.h>
#include "../engine/dump.h"

#define HERO_NAME_MAX   32
#define HERO_TOKENS     4096  /* T0..T4095; the record stores them as 4096 BITS
                                * at +0x76C, FUN_0044DDEA / FUN_00484CBC */
#define HERO_INVENTORY  5119  /* one entry for every valid retail item id; the
                                * record stores a count per id 0..0x13FF, the
                                * first 1024 as bytes at +0x272 and the rest as
                                * presence bits at +0x0AC8 (FUN_0045FC2B) */
/* THERE IS NO HERO-SIDE DISEASE LIST, and there is deliberately no field for one
 * here. The disease counters live only in the 0x6E0-stride combatant record at
 * +0x398 -- FUN_004A6E58 infects, FUN_004A6B55 cures, FUN_004A6C46 decays -- and
 * FUN_00491E45's memset(rec, 0, 0x6E0) zeroes them at the start of every fight,
 * so a disease never survives one. Battle3 found this after rebuilding the
 * counters, and the `ailments` bitmask this struct used to carry was an
 * invention of ours with no counterpart in the record. It is gone rather than
 * left dead: a field that looks live but never is is how the last round's pet and
 * bag bugs happened. Call battle_cure() and read the combatant record instead. */
#define HERO_ABILITIES  5      /* str, wis, sta, agi, dex (levels.txt START_ABILITY order) */

/* The .her record: 0x16CC bytes, no magic, no version, one fwrite(hero,1,0x16CC)
 * (all.c:17154). The checksum lives in the last 4 bytes, at +0x16C8. */
#define HERO_RECORD_SIZE 0x16CC
/* FUN_00460962 (0x460962) builds "<save>/<world>/savedHeroes"; the hero file
 * is that plus "\", the soul name, "." and the extension in DAT_004DF644
 * ("her"). The per-hero INI (cookies, kill counts) uses the same builder with
 * an EMPTY extension, so its file is "<save>/<world>/savedHeroes/<Name>". */
#define HERO_FILE_EXT   "her"
/* FUN_00452107 reads the bio edit with CWnd::GetWindowTextA into a 20000-byte
 * buffer, so the port sizes its editor buffer the same way. */
#define HERO_BIO_TEXT_MAX 20000

enum { ABIL_STR, ABIL_WIS, ABIL_STA, ABIL_AGI, ABIL_DEX };
enum {
    HERO_SLOT_HELMET, HERO_SLOT_ARMOR, HERO_SLOT_BOOTS, HERO_SLOT_SHIELD,
    HERO_SLOT_RING, HERO_SLOT_AMULET,
    HERO_SLOT_RIGHT_HAND = 8 /* stored in right_hand, not equip[] */
};
enum { HERO_TRAIN_HAND, HERO_TRAIN_ELEMENT };

typedef struct {
    int item_id;
    int count;
} HeroItem;

typedef struct {
    int valid;
    /* Slot allocation, which the original does at STARTUP and which is NOT the
     * same thing as a loaded soul. FUN_00427D89 (0x427D89, run during
     * InitInstance) does:
     *     strcpy(&DAT_004e0bd0, ""); FUN_0048e19a();
     *     DAT_00507838 = -1;
     *     *DAT_0067fbf8 = 1;                 <- slot 0 in use
     *     DAT_0067fbf8[1] = DAT_004dd20c;    <- with the local serial
     * and, crucially, NO memset and NO name. So a fresh boot has an allocated
     * slot carrying in_use=1 and a serial and an all-zero name, which is what the
     * original's slot-0 record reads at the title screen. `valid` below means
     * "a soul is LOADED" and is a different question. */
    int slot_in_use;
    char name[HERO_NAME_MAX];
    char skin[64];
    int gender;                 /* 0..3 (gender.ini) */
    int klass;                  /* levels.txt class number (1..88) */
    int level;
    int64_t xp;
    int64_t gold;
    int hp, max_hp, mp, max_mp;
    int ability[HERO_ABILITIES];
    int right_hand;             /* equipped right-hand item id (0 = bare hands) */
    int equip[8];               /* helmet, armor, boots, shield, ring, amulet, ... (item ids) */
    int hand_pp[8];             /* hands 1..8; preferred hand begins at 5000 PP */
    int element_pp[8];          /* element indices 0..7 */
    int64_t pp;                 /* unspent proficiency wallet */
    HeroItem inventory[HERO_INVENTORY]; /* item id n owns slot n-1; empty = {0,0} */
    unsigned char tokens[HERO_TOKENS];
    unsigned char learned_spells[768]; /* explicit GIVE S / START_SPELLS grants */
    /* map position */
    int map;                    /* current map id, hero+0x0090, compared
                                 * record-to-record at all.c:23511/26342/70892 */
    int link;                   /* the current link index, hero+0x067C. FUN_00463853
                                 * writes it and then indexes the link table at
                                 * (&DAT_00604818)[link * 200], 200 bytes a record */
    /* The walk leg, which the original SAVES: a save made mid-walk carries it, so
     * leaving these zero is a divergence, not a simplification. Oracle4.MapVAs's
     * map.* audit found them in record 0 of the 44-entry actor array whose base
     * pointer is the global at 0x0067FBF8 - the hero table. MapView-2 writes them
     * as the hero walks; hero_walk_leg() is the read side. */
    /* The walk position and destination, stored in the ORIGINAL'S OWN 24.8 fixed
     * point, as Main requires: a save made mid-walk carries the fractional bits,
     * and rounding to map units at save time would make the .her differ from
     * what the original writes. 24.8 and not 16.16 is settled by all.c:70730,
     * `FUN_004631C6(iVar7 >> 8, iVar2 >> 8, ...)` consuming exactly these words,
     * and by the distance test at all.c:1918. Hero.x/Hero.y therefore are NOT
     * map units; read them through hero_x_units()/hero_y_units(). */
    int32_t x, y;               /* LIVE position, 24.8 fixed; hero+0x94/+0x98, written
                                 * every tick by FUN_0046230E (all.c:70734-70735) and
                                 * latched on arrival by FUN_00461948 (70152-70153) */
    int32_t target_x, target_y; /* the walk destination, 24.8 fixed; hero+0x9C/+0xA0,
                                 * all.c:70613-70614 */
    int walk_speed;             /* hero+0xAC */
    uint32_t walk_start_tick;   /* GetTickCount stamp for the current leg */
    int walk_duration;          /* hero+0xB4 */
    int facing;                 /* hero+0x88, STORED IN THE ORIGINAL'S ENCODING:
                                 * fy*4 + fx with a 5 -> 9 remap, 0x46230E
                                 * (all.c:70725-70728). NOT the port's fy*3+fx. */
    /* --- fields added for .her parity -------------------------------------
     * kills/deaths are two SEPARATE 32-bit counters in the record, not a
     * 64-bit pair: hero+0x728 is Kills and hero+0x724 is Deaths, per
     * "Kills: %d, PKs: %d, Deaths: %d, PKe %s" (all.c:26557). */
    int kills;                  /* hero+0x728 */
    int deaths;                 /* hero+0x724 */
    int serial;                 /* hero+0x0004, the player id that owns the record */
    int incarnations;           /* hero+0x0730, ++ by FUN_00420240 on every incarnate */
    int saves;                 /* hero+0x0A4C, ++ by every save (FUN_00417F1B) */
    int seconds_played;         /* hero+0x0734 / +0x01CD, the autosave clock */
    uint32_t flags;              /* hero+0x0A58, the script's IF Mn flag word.
                                 * Bit 2 = "Modified Quest File Detected"
                                 * (FUN_0044B196, read by FUN_004142F2's pet
                                 * gate); bit 4 = set by FUN_004978D5 when
                                 * avoidModifiedQuestFiles is off. Negated
                                 * mirror at +0x0A5C, and the mirror is
                                 * checked on load, like the XP/PP pairs. */
    int world_crc;              /* hero+0x06C4, the world CRC the hero last camped in */
    int hunting;                /* hero+0x0A04, the raw signed hunt-training field the
                                 * map encounter roll uses both as a level and
                                 * as a /10000 threshold (FUN_0046260E) */
    /* The trophy bag and pet ids live INSIDE the record, not in module state,
     * so they must round-trip through hero_record_encode/decode. Each slot word
     * is stored RAW exactly as the original stores it -- (count<<8 | id<<16) ^
     * 0x1D43E217, with a literal 0 for an empty slot (FUN_0046F726). items.c
     * owns the ^K; hero.c never interprets these. */
    uint32_t trophy_bag[128];   /* hero+0x0CE0 .. 0x0EDF */
    uint32_t trophy_bag_geo;     /* hero+0x0EE0, (w<<16|h) ^ 0x1D43E217 */
    /* NOT two pet ids. hero+0x0E88 is read only as a "!= 0" predicate (there is
     * no second pet slot); hero+0x0EEC is a GetTickCount() stamp written by the
     * combatant-slot setter, all.c:42151 - I had it as a second pet id and
     * Oracle4.ItemsPanelsVAs' VA audit caught it. The record keeps both, so the
     * .her bytes stay right, but the names are what they are. */
    /* The name is plural and the meaning is not: there is ONE pet. The plural is
     * kept deliberately -- a rename to pet_id/pet_stamp still reads as two
     * symmetric slots, which is the exact inference that produced the bug, so
     * the comment carries the correction instead of a tidier spelling.
     *   pet_ids[0] = the pet's live id, hero+0x0EE8. Read ONLY as "!= 0"
     *                (all.c:42091, 71066) -- never compared against a monster id.
     *   pet_ids[1] = NOT a pet id. It is a GetTickCount() stamp at hero+0x0EEC,
     *                written by FUN_0043B8E7 at 0x42151 alongside the 0x6BC/0x71C
     *                pair, and lazily re-stamped at all.c:42177-42180 when the
     *                field is still zero. Read as a "!= 0" predicate at
     *                all.c:42173 and 17257.
     * It shares a life with the pet-out state only because FUN_0043B8E7 sets
     * all three words in one go. 0x6BC/0x71C, for the avoidance of doubt, are
     * NOT the pet and NOT the equipped right-hand item: they are the cheat-point
     * counter and its negative mirror (FUN_0043B8E7 is called with 0x25 at
     * 42170, and all.c:4867 pairs 0x6BC with the +0xA58 bit-2 flag I added).
     * Index 1 exists only so the field count matches the record's two words. */
    uint32_t pet_ids[2];
} Hero;

extern Hero g_hero;

    /* The bag GEOMETRY is hero+0x0EE0 and is a raw obfuscated word,
     * (w<<16 | h) ^ 0x1D43E217, NOT a decoded pair. A stored 0 is how the
     * original records "no bag": FUN_004200CA memsets the whole 0x16CC record, so
     * a fresh hero's +0x0EE0 is zero, and hero_record_encode writes it back as
     * zero to keep the .her byte-identical. Seeding the field to the KEY instead
     * would make a fresh hero's file differ from the original's, which is the one
     * thing this format must not do -- so the decode side, not this side, is
     * where "absent" is decided. Callers MUST test this before XOR-decoding:
     * decoding a stored 0 yields w=0x1D43 (7491) and h=0xE217 (57879), and a
     * 7491x57879 grid is a 433-million-iteration pack over a 128-slot array.
     * items.c's trophy_bag_size() needs this guard; hero_dump's trophy_bag_size
     * key already has it via the same test. */
/* 1 while the hero has no trophy bag, i.e. the stored geometry word is 0. */
int trophy_bag_absent(const Hero *hero);



/* Initialise a new level-1 hero of class `klass` from g_world.classes (START_ABILITY, AUTO_MAX,
 * DEFAULT_SKIN, START_ITEMS, START_LOCATION, config.ini startingGP). skin NULL = class default. */
void hero_create(Hero *hero, const char *name, int klass, int gender, const char *skin);

/* --- the .her save file, the only hero save format ----------------------
 * game_save_path()/<world>/savedHeroes/<Name>.her, built by FUN_00460962
 * (0x460962) exactly as the original builds it and created on demand
 * (save, save/<world>, save/<world>/savedHeroes). Save = FUN_00417F1B: bump
 * the save counter, set the checksum-algorithm selector at +0x768 to 1, write
 * FUN_00416ABB(hero) into +0x16C8, then fwrite 0x16CC bytes. Load =
 * FUN_004181A2. Both return 0 on success and -1 otherwise; load is
 * transactional and never touches *hero unless it succeeds. */
int hero_save(const Hero *hero);
int hero_load(Hero *hero, const char *name);
/* Delete <Name>.her, the way FUN_00477461 (0x477461, the PURGE / duplicate-name
 * path of the New Soul dialog) does. Returns 0 on success. */
int hero_delete(const char *name);
/* List saved hero names for the current world from savedHeroes\*.her, the way
 * FUN_00477060 (0x477060) does: each name is truncated at its last '.'.
 * Returns the count (<= max). */
int hero_list_saves(char names[][HERO_NAME_MAX], int max);

/* --- the record, exposed so the differential test can compare bytes ------
 * hero_record_encode fills all HERO_RECORD_SIZE bytes from g_hero, including
 * the checksum in the last four, so the buffer is byte-for-byte what
 * FUN_00417F1B would fwrite. It does not allocate and does not read g_hero's
 * name. hero_record_decode applies FUN_004181A2's validation exactly (short
 * read, checksum, and the four RAM-only fields at +0x68/+0x6C8 and the tamper
 * pair at +0x69C/+0x760, which the original requires to sum to zero) and then
 * decodes the record into *out, which it leaves untouched on rejection.
 * Returns 0 on success, -1 when the record is rejected. */
void hero_record_encode(uint8_t out[HERO_RECORD_SIZE]);
int  hero_record_decode(const uint8_t in[HERO_RECORD_SIZE], Hero *out);
/* FUN_00416ABB (0x416ABB): the checksum over bytes [0, 0x16C8), selected by
 * mode = the record's +0x768 field. 0 is the legacy sign-extending variant, 1
 * is the one the save path always writes, and any other value returns the
 * bare seed 0x379ADE because the original has no branch for it. */
uint32_t hero_record_checksum(const uint8_t in[HERO_RECORD_SIZE], int mode);

/* The soul-switch world check, FUN_0044B196. `avoid` is the
 * avoidModifiedQuestFiles profile key (default 1). Returns 1 when the soul may
 * be switched, 0 when it must be refused, and sets *mismatch to 0 when the
 * stamp matches, 1 on a different-world-version stamp and 2 on the
 * never-camped case (a stored 0) - the two wordings the original distinguishes. */
int  hero_world_crc_check(int avoid, int *mismatch);
/* Set or clear one bit of Hero.flags (hero+0x0A58). Callers mask with
 * hero_flag(); the word is exposed whole rather than pre-selected. */
void hero_set_flag(Hero *hero, unsigned bit, int on);
int  hero_flag(const Hero *hero, unsigned bit);

/* --- the personal BIO (FUN_00452107 / FUN_00438C05) -----------------------
 * Two files per soul, both under the port's save root in a "bio" directory, the
 * way the original writes them under its install root:
 *   <save>/bio/%08X-<Name>.ini   the structured record  (0x4EB544, uppercase as
 *                                %s\BIO\%08X-%s.ini at 0x4EEE6C - the original's filesystem
 *                                was case-insensitive and both spellings are the
 *                                same directory, so the port uses the lower-case
 *                                one and documents the mapping here)
 *   <save>/bio/%08X.txt          the free text body     (0x4F02D8; the sibling
 *                                form %s\BIO\%s.txt is 0x4F02C8)
 * %08X is the hero's serial, which is what the original keys them on. The INI's
 * section is "Bio" (0x4EB538) and its keys are serNum, className, levelName,
 * worldLocation and skin; the text file is the bio edit's contents verbatim.
 * hero_bio_save writes both and unlinks the name-only siblings, which is what
 * FUN_00452107 does on its +0x94 == 0 path. `text` is the edit control's
 * contents, as FUN_00452107 reads it off CWnd+0x338; NULL or empty deletes the
 * text file. Returns 0 on success. */
int   hero_bio_save(const char *text);
/* The bio text into a malloc'd NUL-terminated buffer the caller frees, or NULL
 * when no bio exists. *size, when given, receives the length. */
char *hero_bio_text(size_t *size);

/* Per-hero INI, "<save>/<world>/savedHeroes/<Name>" (FUN_00460962 with the
 * empty extension in DAT_004DCBAC). Section "cookies" is the quest SET store
 * (FUN_0047A9F5 reads, FUN_0047AB07 writes, both case-insensitive on the key,
 * and an empty value deletes the key). Kills by monster id live in the same
 * file in the sections the original uses (FUN_0043B0FD): "monsters killed",
 * "killed by monster" and "monsters seen", each keyed by the decimal monster
 * id. hero_cookie_get returns a pointer to a static buffer that the next call
 * overwrites, or NULL when the key is unset. */
const char *hero_cookie_get(const char *key);
int         hero_cookie_set(const char *key, const char *value); /* 0 on success */
void        hero_cookie_del(const char *key);
uint32_t    hero_kills_total(void);            /* g_hero.kills,  hero+0x728 */
uint32_t    hero_deaths_total(void);           /* g_hero.deaths, hero+0x724 */
int         hero_kills_of_monster(int monster_id);   /* 0 when absent  */
int         hero_deaths_by_monster(int monster_id);  /* 0 when absent  */
int         hero_monsters_seen(int monster_id);      /* -1 when absent */
int         hero_kill_monster(int monster_id);       /* +1, new count */
int         hero_killed_by_monster(int monster_id);  /* +1, new count */
/* Battle-2 calls these from its port of FUN_00494FCD (all.c:109382). */
int hero_add_kill(Hero *hero);
int hero_add_kills(Hero *hero, int n);
int hero_add_death(Hero *hero);

/* Differential dump. Emits hero.record (the whole 0x16CC record as lowercase
 * hex, the comparison unit) plus the decoded fields, all lowercase decimal
 * with no units, and hero.abil.<str|wis|sta|agi|dex>. Does not allocate. */
void hero_dump(DumpEmit emit, void *user);
/* Allocate slot 0 at startup the way FUN_00427D89 does. */
void hero_allocate_slot(int serial);

/* A read-only view of the hero's walk leg, for mapview.c. No arguments: it
 * reads the hero's own state, exactly as the original reads the record. */
typedef struct {
    int x, y;                /* live position, map units */
    int target_x, target_y;  /* the walk destination */
    int speed;
    uint32_t start_tick;
    int duration;
    int facing;              /* the port's NATIVE encoding, fy*4+fx with the 5->9 remap */
} WalkLeg;
const WalkLeg *hero_walk_leg(const Hero *hero);
/* Map units for a reader that wants them. Hero.x/Hero.y are 24.8 fixed and these
 * reproduce the ORIGINAL's conversion, which is NOT a plain shift: the decomp
 * uses `(x + (x >> 31 & 0xFF)) >> 8` at seven sites (all.c:23512, 23513,
 * 70882, 70885, 70971, 70972, 72525, 72527), i.e. MSVC's round toward zero for
 * a signed divide by 256. A plain `>> 8` floors, which is one map unit out for
 * every negative coordinate - and the walk produces negative y routinely. */
int  hero_x_units(const Hero *hero);
int  hero_y_units(const Hero *hero);
void hero_set_x_units(Hero *hero, int units);
void hero_set_y_units(Hero *hero, int units);
/* The facing codec, so there is one implementation of the 0x46230E remap. */
int  hero_facing_encode(int fx, int fy);
void hero_facing_decode(int facing, int *fx, int *fy);

/* The hero table's random offset, NOT a seed. FUN_004269AF at 0x426B21 draws
 * rand() (draw 1409, after the 352 EncInt constructions) and reduces it at
 * 0x426B27..0x426B36 to
 *      (rand() % 0x4000) & ~15        -- 0..0x3FF0, always a multiple of 16
 * which is stored to DAT_004E4870 at 0x426B94 and added into the hero table
 * base at 0x426B9A. Core owns the RNG and the boot ordering, so Core records
 * the consumed value here and hero_dump reports it; returning the seed would
 * mismatch on every run and look like an RNG divergence rather than a modelling
 * error. 0 until Core sets it. */
uint32_t hero_base_offset(void);
void    hero_set_base_offset(uint32_t value);

/* Max HP and MP for this hero's class at its current level, from the levels.txt
 * table (FUN_00483984's curve plus FUN_004207BD's AUTO_MAX handling). items.c
 * calls this after a class 100..105 seed changes an ability, exactly as
 * FUN_004A6353 does. Either output pointer may be NULL. An unusable class yields
 * hp = 1, mp = 0. */
void hero_maxima(const Hero *hero, int *hp, int *mp);

/* XP needed to reach `level` for this hero's class (levels.txt maths). */
int64_t hero_xp_for_level(const Hero *hero, int level);
/* Add XP/gold; performs level-ups (max HP/MP growth). Returns number of levels gained. */
int hero_award(Hero *hero, int64_t xp, int64_t gold);

/* Inventory operations are all-or-nothing: 1 success, 0 rejected. Equipped items
 * remain in inventory; taking the final copy automatically unequips it. */
int hero_item_count(const Hero *hero, int item_id);
int hero_item_limit(const Hero *hero, int item_id);
int hero_give_item(Hero *hero, int item_id, int count);
int hero_take_item(Hero *hero, int item_id, int count);
int64_t hero_wallet_limit(const Hero *hero);
int64_t hero_add_gold(Hero *hero, int64_t amount); /* actual signed change */
int hero_item_slot(int item_id); /* HERO_SLOT_* or -1 for non-equipment */
int hero_equipped(const Hero *hero, int slot);
int hero_can_equip(const Hero *hero, int item_id);
int hero_equip(Hero *hero, int item_id);
int hero_unequip(Hero *hero, int slot);
/* ability is ABIL_* for class-105 free-choice seeds; otherwise ignored.
 * Rejected uses do not consume the item. Success emits item_use. */
int hero_use_item(Hero *hero, int item_id, int ability);
int hero_offense(const Hero *hero);
int hero_defense(const Hero *hero);
int hero_ability(const Hero *hero, int ability);
int hero_pp_level(int pp); /* documented thresholds, integer level 0..9 */
int hero_train_limit(const Hero *hero, int kind, int index);
/* FUN_00425854 wallet clamp; Battle awards PP on qualifying attacks, not XP payout. */
int64_t hero_add_pp(Hero *hero, int64_t amount); /* actual signed change */
/* Spend exactly 1..2000 wallet PP, or reject without mutation; indices 0..7. */
int hero_train(Hero *hero, int kind, int index, int amount);
/* Combat proficiency gain (1..2000), clamped to class cap; no wallet debit. */
void hero_gain_training(Hero *hero, int kind, int index, int amount);
int hero_spell_known(const Hero *hero, int spell_id);
/* Explicit grant/removal, not a PP purchase; used by quest GIVE/TAKE S. */
int hero_set_spell(Hero *hero, int spell_id, int known);
int hero_can_learn_spell(const Hero *hero, int spell_id);
int hero_learn_spell(Hero *hero, int spell_id); /* debit PP and emit spell_learn */

#endif
