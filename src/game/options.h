/* The original's user-options array, DAT_006840D0 (33 entries, 0..32).
 *
 * Read:  FUN_00467312 (0x00467312) -- `if (n < 0 || n > 0x20) return 0; else
 *        return DAT_006840D0[n]`, i.e. every out-of-range index reads 0 rather
 *        than faulting, and that is what the port must do too.
 * Write: FUN_0046732B (0x0046732B) -- same bound check, then store and
 *        FUN_004670B4 (0x004670B4), which rewrites the whole profile.
 * Load:  FUN_00466EF3 (0x00466EF3) -- for each record of the runtime-built table
 *        at DAT_004F2A58 (3 ints: option id, default, label pointer; count at
 *        DAT_004F2BD8): `CWinApp::GetProfileIntA("Preferences", "option <id>",
 *        default)`, then the same for the named scalars below.
 *
 * Persistence is MFC's profile API, i.e. the Windows REGISTRY under
 * HKEY_CURRENT_USER\Software\<company>\<app>\Preferences, NOT an INI file. The
 * port's stand-in is the INI at <save>/options.ini with the original's section
 * and key names verbatim, so the Oracle harness can seed the Wine registry with
 * the same names. Owner: Core. */
#ifndef WOS_OPTIONS_H
#define WOS_OPTIONS_H

#include "../engine/dump.h"

#define OPTIONS_COUNT 33       /* indices 0..32; DAT_006840D0 spans 0x21 dwords */
#define OPTIONS_PROFILE_SECTION "Preferences"

/* The option the retail build ships OFF and that gates the pathfinder. */
#define OPTION_ENABLE_WAYPOINTS 7

/* Reads the option, 0 for any index outside 0..OPTIONS_COUNT-1 (FUN_00467312). */
int options_get(int n);
/* Stores and persists the whole profile (FUN_0046732B + FUN_004670B4). */
void options_set(int n, int value);

/* The named scalars FUN_00466EF3/FUN_004670B4 read and write alongside the
 * numbered array. They are NOT part of DAT_006840D0; they have their own keys. */
enum {
    OPT_WORLD_LOCATION,             /* "worldLocation",          default 0 */
    OPT_SEANCE_IN_PROGRESS,         /* "seanceInProgress",       default 0 */
    OPT_ENABLE_MUSIC,               /* "enableMusic",            default 1 */
    OPT_ENABLE_ENVIRONMENTAL_SOUNDS,/* "enableEnvironmentalSounds", default 1 */
    OPT_ENABLE_SFX,                 /* "enableSFX",              default 1 */
    OPT_ENABLE_SOUND_CARD,          /* "enableSoundCard",        default 1 */
    OPT_ASK_FOR_SKINS,              /* "askForSkins",            default 1 */
    OPT_ENABLE_HOW_DO_YOU,          /* "enableHowDoYou",         default 1 */
    OPT_EAVESDROP_ENABLED,          /* "eavesdropEnabled",       default 1 */
    OPT_NAMED_COUNT
};
int         option_named_get(int n);
void        option_named_set(int n, int value);
const char *option_named_key(int n);

/* Load from / save to <game_save_path()>/options.ini. options_load() also pushes
 * the waypoint option into the map module, because the original applies it when
 * the profile is read rather than consulting the array per query. */
void options_load(void);
void options_save(void);
int  options_loaded(void);

/* The `at <ms> dialog <id> <ctrl>=<value>... ok|cancel` hook for the options
 * dialog. Returns 0 for ids it does not own. Dialog id and the control ids come
 * from Main once Oracle has read them out of .rsrc; until then this accepts the
 * documented option-index controls. */
int options_dialog_op(int dialog_id, const char *const *kv, int n, int ok);

void options_dump(DumpEmit emit, void *user);

#endif
