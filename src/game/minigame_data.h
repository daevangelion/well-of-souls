/* Mini-game data files, loaded through the same world_path/ini path the original uses.
 *
 * VAs:
 *   FUN_00466242 (0x466242) slots.ini string read: builds "<world>/slots.ini" with
 *       FUN_0041E1AA (0x41E1AA, the same "%s/%s" world-path helper every data file uses),
 *       checks it with _access(path, 0) and only then calls GetPrivateProfileStringA.
 *   FUN_004662C8 (0x4662C8) the atoi variant of the same.
 *   FUN_00405F07 (0x405F07) racers.txt: FUN_0041E1AA + fopen + fgets(100) + atoi, bounded by
 *       DAT_004DC964 entries, stopping early at a non-positive id.
 *   stocks.ini is at the DATA root, not the world root: all.c:89981 formats
 *       "%s\\worlds\\%s\\%s" for world files and a separate path for the data root.
 */
#ifndef WOS_MINIGAME_DATA_H
#define WOS_MINIGAME_DATA_H

#include <stdint.h>

/* --- slots.ini (FUN_00466327, 0x466327) -------------------------------------
 * Three parallel 8-entry tables indexed by symbol 0..7, hard-coded to Evergreen's values and
 * then overridden per symbol from slots.ini. The tables live at DAT_004F23E8 (percent),
 * DAT_004F23C8 (double) and DAT_004F23A8 (triple). */
#define MG_SYMBOLS 8
#define MG_WHEEL_SLOTS 100
#define MG_WHEELS 3

typedef struct {
    int percent[MG_SYMBOLS];  /* copies of this symbol on EACH of the 3 wheels; must total 100 */
    int triple[MG_SYMBOLS];   /* GP for three in a row */
    int double_[MG_SYMBOLS];  /* GP for the first wheel matching either of the other two */
} MgSlotsConfig;

/* Build the three 100-slot wheels. `wheels[w][s]` is the symbol at slot s of wheel w. Consumes
 * rand() calls exactly as FUN_004661E4 (0x4661E4) does; the caller must have crt_srand()ed. */
void mg_slots_defaults(MgSlotsConfig *cfg);
/* Returns 1 when slots.ini was found and parsed (FUN_00466242's _access test). */
int  mg_slots_load(MgSlotsConfig *cfg);
void mg_slots_build(const MgSlotsConfig *cfg, uint8_t wheels[MG_WHEELS][MG_WHEEL_SLOTS]);

/* --- racers.txt (FUN_00405F07, 0x405F07) ----------------------------------- */
#define MG_RACERS_MAX 32
typedef struct { int ids[MG_RACERS_MAX]; int count; } MgRacers;
void mg_racers_load(MgRacers *racers);

/* --- stocks.ini (data root) ------------------------------------------------ */
#define MG_STOCKS_MAX 64
#define MG_STOCK_NAME 48
typedef struct {
    int count;
    struct { char symbol[16]; char name[MG_STOCK_NAME]; } stock[MG_STOCKS_MAX];
} MgStocks;
void mg_stocks_load(MgStocks *stocks);

/* --- springy.ini (the /springy easter egg) --------------------------------- */
#define MG_SPRINGY_MAX 64
typedef struct { int object, pinned; double kg, rel_x, rel_y, radius; } MgMass;
typedef struct {
    double width, gravity, air_friction, step_seconds, elasticity;
    int count;
    MgMass mass[MG_SPRINGY_MAX];
} MgSpringy;

/* Same parse, but from "<world>/<ini_name>" (FUN_0041E1AA's "%s/%s" world path with the
 * caller's file name) instead of the fixed "springy.ini". FUN_00434756 (0x434756) takes the
 * name from the command tail when "/springy <file>" is longer than 8 characters.
 * Returns 1 on success, 0 when the file is missing or unparsable. */
int mg_springy_load_named(const char *ini_name, MgSpringy *springy);
void mg_springy_load(MgSpringy *springy);

#endif
