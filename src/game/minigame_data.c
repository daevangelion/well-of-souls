/* Mini-game data files. Every reader mirrors the original's own load: build the path with the
 * shared world-path helper, probe it, and only then parse. A missing file is not an error — the
 * original falls back to hard-coded values in every case below. */
#include "minigame_data.h"
#include "world.h"
#include "../engine/ini.h"
#include "../engine/rng.h"
#include "../engine/text.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- slots.ini (FUN_00466242 0x466242, FUN_004662C8 0x4662C8, FUN_00466327 0x466327) ------
 * FUN_00466327 seeds three 8-entry tables with Evergreen's shipped values and then, for each
 * symbol 0..7, tries to override percent/double/triple from slots.ini. A key that is absent
 * leaves the compiled default in place: FUN_004662C8 returns 0 and the caller skips the store.
 *
 * The three tables (literal pool at 0x4F23A8..0x4F23FF, copied via local_64[0x10]/[8]/[0]):
 *   percent  DAT_004F23E8 = { 2, 5, 10, 10, 10, 10, 10, 43 }   (sums to 100)
 *   double   DAT_004F23C8 = { 50000, 5000, 50, 50, 50, 50, 50, 5 }
 *   triple   DAT_004F23A8 = { 1000000, 100000, 3000, 3000, 3000, 3000, 3000, 100 }
 * The section name is the decimal symbol index, built with the "%d" format at 0x4DC0F0; the
 * fallback string is "0" (0x4DCAF4). */
static const int default_percent[MG_SYMBOLS] = { 2, 5, 10, 10, 10, 10, 10, 43 };
static const int default_double[MG_SYMBOLS]  = { 50000, 5000, 50, 50, 50, 50, 50, 5 };
static const int default_triple[MG_SYMBOLS]  = { 1000000, 100000, 3000, 3000, 3000, 3000, 100 };

void mg_slots_defaults(MgSlotsConfig *cfg)
{
    memcpy(cfg->percent, default_percent, sizeof default_percent);
    memcpy(cfg->double_, default_double, sizeof default_double);
    memcpy(cfg->triple, default_triple, sizeof default_triple);
}

/* FUN_004662C8 reads one integer key out of section `symbol` and reports whether the key existed
 * at all; the caller keeps the compiled default when it returns 0. */
static int symbol_int(const Ini *ini, int symbol, const char *key, int *out)
{
    char section[16];
    const char *value;
    snprintf(section, sizeof section, "%d", symbol);
    value = ini_get(ini, section, key, NULL);
    if (!value) return 0;
    *out = atoi(value);
    return 1;
}

int mg_slots_load(MgSlotsConfig *cfg)
{
    char path[1024];
    char *text;
    Ini ini;
    int sym;

    mg_slots_defaults(cfg);
    if (!world_path(path, sizeof path, "slots.ini")) return 0;
    text = text_read_file(path, NULL);
    if (!text) return 0;
    if (ini_parse(&ini, text) == 0) {
        for (sym = 0; sym < MG_SYMBOLS; ++sym) {
            int v;
            if (symbol_int(&ini, sym, "percent", &v)) cfg->percent[sym] = v;
            if (symbol_int(&ini, sym, "double", &v))  cfg->double_[sym] = v;
            if (symbol_int(&ini, sym, "triple", &v))  cfg->triple[sym] = v;
        }
    }
    free(text);
    return 1;
}

/* FUN_004661E4 (0x4661E4), verified against the objdump at 0x4661e4:
 *
 *   scan wheel `w`'s 100 slots for the first entry == -1; remember its index in `first`.
 *   if the wheel is completely full (first == -1):
 *       v = rand() % 100;                        return v;
 *   else:
 *       do { v = (rand() >> 4) % 100; } while (wheel[w][v] != -1);
 *       return v;
 *
 * Ghidra prints the shift as CONCAT44(v>>31, v>>4) % 100, which is a sign-extended 64-bit
 * value; since rand() returns 0..0x7fff the high half is always 0 and it reduces to
 * (v >> 4) % 100. The disassembly confirms `sar eax,4` then `idiv 0x64`.
 *
 * FUN_00466327 fills the three wheels symbol by symbol: for symbol 0..7, place percent[symbol]
 * copies using this picker. The whole 3x100 block is memset to -1 first (0x4B0 bytes).
 *
 * `first` is recomputed per call exactly as the original does (it rescans from slot 0), so a
 * wheel that has become full switches to the un-retrying branch. */
void mg_slots_build(const MgSlotsConfig *cfg, uint8_t wheels[MG_WHEELS][MG_WHEEL_SLOTS])
{
    int w, sym, n;
    for (w = 0; w < MG_WHEELS; ++w) {
        int i;
        for (i = 0; i < MG_WHEEL_SLOTS; ++i) wheels[w][i] = 0xff;
        for (sym = 0; sym < MG_SYMBOLS; ++sym) {
            for (n = cfg->percent[sym]; n > 0; --n) {
                int first = -1, v;
                for (i = 0; i < MG_WHEEL_SLOTS; ++i) {
                    if (wheels[w][i] == 0xff) { first = i; break; }
                }
                if (first == -1) {
                    v = crt_rand() % 100;
                } else {
                    do { v = (crt_rand() >> 4) % 100; } while (wheels[w][v] != 0xff);
                }
                wheels[w][v] = (uint8_t)sym;
            }
        }
    }
}

/* --- racers.txt (FUN_00405F07, 0x405F07) -------------------------------------
 * DAT_00538368 starts at DAT_004DC964 (the hard cap) and is reset to 0 before the read; the loop
 * then pulls lines with fgets(buf, 100) and atoi()s them, appending only ids > 0 and stopping
 * once the count reaches the cap. A `;` comment is harmless: atoi stops at the first
 * non-digit, so "1\t\t;\tgreen jelly" yields 1. */
void mg_racers_load(MgRacers *racers)
{
    char path[1024];
    char *text, *cursor, *line;

    racers->count = 0;
    if (!world_path(path, sizeof path, "racers.txt")) return;
    text = text_read_file(path, NULL);
    if (!text) return;
    cursor = text;
    while ((line = text_next_line(&cursor)) && racers->count < MG_RACERS_MAX) {
        int id = atoi(line);
        if (id > 0) racers->ids[racers->count++] = id;
    }
    free(text);
}

/* --- stocks.ini (data root, not the world root) ------------------------------
 * all.c:89981 shows the original formatting world-relative paths as "%s\\worlds\\%s\\%s"; the
 * stock file sits beside that tree, at the data root. [GENERAL] num_stocks bounds the list,
 * then [STOCK<n>] supplies symbol and name. */
void mg_stocks_load(MgStocks *stocks)
{
    char path[1024];
    char *text;
    Ini ini;
    int i, want;

    stocks->count = 0;
    if (!world_data_path(path, sizeof path, "stocks.ini")) return;
    text = text_read_file(path, NULL);
    if (!text) return;
    if (ini_parse(&ini, text) == 0) {
        want = atoi(ini_get(&ini, "GENERAL", "num_stocks", "0"));
        if (want > MG_STOCKS_MAX) want = MG_STOCKS_MAX;
        for (i = 0; i < want; ++i) {
            char section[16];
            const char *value;
            snprintf(section, sizeof section, "STOCK%d", i);
            value = ini_get(&ini, section, "symbol", NULL);
            if (!value) continue;
            snprintf(stocks->stock[stocks->count].symbol,
                     sizeof stocks->stock[stocks->count].symbol, "%s", value);
            value = ini_get(&ini, section, "name", "");
            snprintf(stocks->stock[stocks->count].name,
                     sizeof stocks->stock[stocks->count].name, "%s", value);
            stocks->count++;
        }
    }
    free(text);
}

/* --- springy.ini (the /springy easter egg) -----------------------------------
 * FUN_0047392E (0x47392E) clears the 0x125320-byte world block and hands the path to
 * FUN_0047337A, which reads [General] (width, gravity, airfriction, stepSeconds, numObjects,
 * elasticity) and then one [Object<n>] section per object, each with numMasses and a
 * mass<k> = "kg, relX, relY, radius" list. Only the stage description is modelled: the mass-
 * spring solver behind /springy is a debug surface, not one of the eight GAME games. */

/* atof the next comma-separated field of *cursor and advance past it. */
static double springy_field(const char **cursor)
{
    const char *p = *cursor, *start;
    char buf[64];
    size_t n;
    while (*p == ' ' || *p == '\t' || *p == ',') ++p;
    start = p;
    while (*p && *p != ',') ++p;
    n = (size_t)(p - start);
    if (n >= sizeof buf) n = sizeof buf - 1;
    memcpy(buf, start, n);
    buf[n] = 0;
    *cursor = p;
    return atof(buf);
}

/* FUN_00434756 (0x434756) takes the ini name from the command tail when "/springy <file>"
 * is longer than 8 characters, and falls back to "springy.ini" otherwise. */
void mg_springy_load(MgSpringy *springy)
{
    mg_springy_load_named("springy.ini", springy);
}

int mg_springy_load_named(const char *ini_name, MgSpringy *springy)
{
    char path[1024];
    char *text;
    Ini ini;
    int i;

    memset(springy, 0, sizeof *springy);
    if (!ini_name || !*ini_name) return 0;
    if (!world_path(path, sizeof path, ini_name)) return 0;
    text = text_read_file(path, NULL);
    if (!text) return 0;
    if (ini_parse(&ini, text) == 0) {
        springy->width        = atof(ini_get(&ini, "General", "width", "0"));
        springy->gravity      = atof(ini_get(&ini, "General", "gravity", "0"));
        springy->air_friction = atof(ini_get(&ini, "General", "airfriction", "0"));
        springy->step_seconds = atof(ini_get(&ini, "General", "stepSeconds", "0"));
        springy->elasticity   = atof(ini_get(&ini, "General", "elasticity", "0"));
        {
            int objects = atoi(ini_get(&ini, "General", "numObjects", "0"));
            for (i = 0; i < objects && i < MG_SPRINGY_MAX; ++i) {
                char section[16], key[16];
                int masses, m;
                snprintf(section, sizeof section, "Object%d", i);
                masses = atoi(ini_get(&ini, section, "numMasses", "0"));
                for (m = 0; m < masses && springy->count < MG_SPRINGY_MAX; ++m) {
                    const char *spec, *p;
                    MgMass *mass = &springy->mass[springy->count];
                    snprintf(key, sizeof key, "mass%d", m);
                    spec = ini_get(&ini, section, key, NULL);
                    if (!spec) continue;
                    mass->object = i;
                    mass->pinned = 0;
                    p = spec;
                    mass->kg     = springy_field(&p);
                    mass->rel_x  = springy_field(&p);
                    mass->rel_y  = springy_field(&p);
                    mass->radius = springy_field(&p);
                    springy->count++;
                }
            }
        }
        free(text);
        return 1;
    }
    free(text);
    return 0;
}
