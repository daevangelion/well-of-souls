#include "options.h"
#include "game.h"
#include "../platform/platform.h"
#include "../engine/ini.h"
#include "../engine/text.h"
#include "game_main.h"
#include "../engine/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* DAT_006840D0. Indices outside 0..32 are never written by FUN_0046732B and read
 * back as 0 by FUN_00467312, so the array is zero-initialised and the load only
 * touches the indices the runtime table names. */
static int g_option[OPTIONS_COUNT];
static int g_named[OPT_NAMED_COUNT];
static int g_loaded;

/* FUN_004670B4's named keys, in the order it writes them. The defaults are
 * FUN_00466EF3's `GetProfileIntA(..., default)` third argument, read straight off
 * the decompilation: worldLocation and seanceInProgress pass 0, the rest pass 1. */
static const struct { const char *key; int dflt; } g_named_info[OPT_NAMED_COUNT] = {
    { "worldLocation",             0 },
    { "seanceInProgress",          0 },
    { "enableMusic",               1 },
    { "enableEnvironmentalSounds", 1 },
    { "enableSFX",                 1 },
    { "enableSoundCard",           1 },
    { "askForSkins",               1 },
    { "enableHowDoYou",            1 },
    { "eavesdropEnabled",          1 }
};

int options_get(int n)
{
    if (n < 0 || n > OPTIONS_COUNT - 1) return 0;   /* FUN_00467312's bound check */
    return g_option[n];
}

void options_set(int n, int value)
{
    if (n < 0 || n > OPTIONS_COUNT - 1) return;      /* FUN_0046732B's bound check */
    g_option[n] = value;
    options_save();                                  /* FUN_004670B4 rewrites all */
    /* FUN_00467349's two side effects: flipping option 0 re-derives the music
     * setting, flipping option 32 (0x20) re-derives the SFX setting. */
    if (n == 0) option_named_set(OPT_ENABLE_MUSIC, g_option[0]);
    else if (n == 32) option_named_set(OPT_ENABLE_SFX, g_option[32]);
}

int option_named_get(int n)
{
    if (n < 0 || n >= OPT_NAMED_COUNT) return 0;
    return g_named[n];
}

void option_named_set(int n, int value)
{
    if (n < 0 || n >= OPT_NAMED_COUNT) return;
    g_named[n] = value;
}

const char *option_named_key(int n)
{
    if (n < 0 || n >= OPT_NAMED_COUNT) return NULL;
    return g_named_info[n].key;
}

static void path_of(char *out, size_t cap)
{
    snprintf(out, cap, "%s/options.ini", game_save_path());
}

void options_save(void)
{
    char path[4096], key[32];
    FILE *file;
    int i;
    path_of(path, sizeof(path));
    file = plat_fopen(path, "wb");
    if (!file) { wos_log_event("options_save_failed", "path=%s", path); return; }
    fprintf(file, "[%s]\n", OPTIONS_PROFILE_SECTION);
    for (i = 0; i < OPTIONS_COUNT; ++i) {
        snprintf(key, sizeof(key), "option %d", i);
        fprintf(file, "%s=%d\n", key, g_option[i]);
    }
    for (i = 0; i < OPT_NAMED_COUNT; ++i)
        fprintf(file, "%s=%d\n", g_named_info[i].key, g_named[i]);
    fclose(file);
    wos_log_event("options_saved", "path=%s", path);
}

void options_load(void)
{
    char path[4096];
    Ini ini;
    char *text;
    int i, changed = 0, have_file;
    for (i = 0; i < OPT_NAMED_COUNT; ++i) g_named[i] = g_named_info[i].dflt;
    path_of(path, sizeof(path));
    text = text_read_file(path, NULL);
    have_file = text && ini_parse(&ini, text) == 0;
    if (have_file) {
        /* DELIBERATELY NOT LOADING "option N" FROM THE PROFILE.
         *
         * FUN_00466EF3 (0x00466EF3) guards its numbered-option loop with
         * `if (0 < DAT_004f2bd8)`, and the binary never writes DAT_004f2bd8 or
         * DAT_004f2a58: every one of the ten references to either address in
         * .text is a READ (verified by scanning the image for the little-endian
         * patterns d8 2b 4f 00 and 58 2a 4f 00 -- 6 and 4 hits, all `cmp`/`mov`
         * loads, no store). Both live in the zero-filled tail of .data, so both
         * are 0 for the whole life of the process and the loop never runs. The
         * runtime table of {id, default, label} that would populate them does not
         * exist in this build.
         *
         * Consequence, and it is the one the differential replay would catch:
         * DAT_006840D0 starts ENTIRELY ZERO, so every numbered option is OFF in
         * retail, and the only thing that ever changes one is FUN_0046732B
         * (0x0046732B), i.e. the options dialog's checkbox handler. Option 7,
         * "Enable automatic Way Point calculations", being off in retail is not a
         * documented default; it is the whole array being zero.
         *
         * The nine named scalars ARE read, each by its own unguarded
         * `GetProfileInt(..., default)` call, so those and only those come from
         * the profile. */
        for (i = 0; i < OPT_NAMED_COUNT; ++i) {
            const char *v = ini_get(&ini, OPTIONS_PROFILE_SECTION, g_named_info[i].key, NULL);
            if (v) g_named[i] = atoi(v);
        }
    }
    free(text);
    /* No profile yet (or it was unreadable): write the retail defaults out, so
     * both sides of a differential run start from an identical, inspectable
     * state. The original creates its registry keys on the first save too. */
    if (!have_file) options_save();
    g_loaded = 1;
    (void)changed;
    /* The original applies the waypoint flag when the profile is read. */
    map_set_waypoints(options_get(OPTION_ENABLE_WAYPOINTS) ? 1 : 0);
    wos_log_event("options_loaded", "path=%s waypoints=%d changed=%d",
                  path, options_get(OPTION_ENABLE_WAYPOINTS), changed);
}

int options_loaded(void) { return g_loaded; }

/* The `dialog` script op, in the shape FrontHero-2 documents: each entry is
 * "ctrl=value" with the strings kept as text, and the trailing "ok=<0|1>" is
 * consumed as the button. A control named "option <n>" sets option n to the
 * value that follows; the value is the original's checkbox state (0/1). */
int options_dialog_op(int dialog_id, const char *const *kv, int n, int ok)
{
    int i, applied = 0;
    (void)dialog_id;
    (void)ok;
    if (!kv) return 0;
    for (i = 0; i + 1 < n; ++i) {
        int idx, value;
        if (!kv[i] || !kv[i + 1]) continue;
        if (strcmp(kv[i], "ok") == 0) break;   /* the terminating button */
        if (strncmp(kv[i], "option ", 7)) continue;
        idx = atoi(kv[i] + 7);
        if (idx < 0 || idx >= OPTIONS_COUNT) continue;
        value = atoi(kv[i + 1]) != 0;
        options_set(idx, value);
        applied = 1;
        ++i;
    }
    return applied;
}

void options_dump(DumpEmit emit, void *user)
{
    int i;
    char key[32];
    for (i = 0; i < OPTIONS_COUNT; ++i) {
        snprintf(key, sizeof(key), "options.%d", i);
        dump_emit_int(emit, key, options_get(i), user);
    }
    for (i = 0; i < OPT_NAMED_COUNT; ++i) {
        char k[64];
        snprintf(k, sizeof(k), "options.named.%s", g_named_info[i].key);
        dump_emit_int(emit, k, option_named_get(i), user);
    }
}
