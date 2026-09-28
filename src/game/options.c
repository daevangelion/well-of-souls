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

/* The original's option table, DAT_004F2A58, verbatim: 32 records of
 * {id, default, label pointer}, count 32 at DAT_004F2BD8. Both are INITIALISED
 * .data at RVA 0xF2A58 / 0xF2BD8 (file offset 0xF0E58 / 0xF0FD8) -- there is no
 * start-up initialiser and no indirect store; the table is in the image. Read with
 * pefile and cross-checked row for row against the Oracle's live probe of the running
 * original, which reported the same 32 ids, the same defaults and the same labels.
 * Ids 0..32 are the array DAT_006840D0 (33 slots); id 9 has NO record, so it is never
 * named, never given a default, and stays 0. */
typedef struct { int id; int dflt; const char *label; } OptionDef;
static const OptionDef g_option_def[OPTIONS_TABLE_COUNT] = {
    {13, 1, "Don't let me use cheat codes."},
    { 1, 1, "Use High-Resolution world maps. (very slow)."},
    {30, 1, "Improve jpeg image quality in scenes and maps. (slowish)."},
    { 2, 1, "Auto-Smooth Low-Resolution world maps (slow)."},
    { 3, 1, "Notify me when other players learn spells."},
    { 4, 1, "Notify me about which spells are cast in fights."},
    {20, 1, "Notify me when my character changes deciLevel."},
    { 5, 1, "Enable Player Chat Bubbles while in scenes."},
    {12, 1, "Make non-player character chat bubbles pop faster."},
    {24, 1, "In wide scenes, center camera on player chat bubbles."},
    { 8, 0, "Don't pick up low-level junk from dead monsters."},
    {19, 1, "Hide Spells I can't learn yet"},
    {17, 1, "Show monster radar during hunts. (Golden Soul/Demo)"},
    { 0, 0, "Show Hot-Key popup window while in scenes."},
    {32, 1, "Show Hot-Key button bar while in scenes."},
    {21, 1, "Show an icon when NPCs are waiting for an answer."},
    { 6, 0, "Show Pet's Owner Tags"},
    {25, 1, "Show character index numbers in scenes."},
    {22, 1, "Show HTML pages in scenes, when scripted."},
    {14, 0, "Auto-open an IM window when people whisper to me."},
    {11, 0, "Log all chat to disk (warning - uses lots of disk)."},
    {27, 0, "Log all battles to disk (warning - uses lots of disk)."},
    {28, 1, "Log all death sentences to disk."},
    {10, 1, "My computer is slow, cut animations during dialogs."},
    {23, 1, "Stop all web page stuff on return to game."},
    {16, 1, "Remember changes to window size and positions."},
    {18, 1, "Confirm link images when adding new links."},
    { 7, 1, "Enable automatic Way Point calculations."},
    {15, 1, "Show 3D outline around button bar buttons."},
    {26, 0, "Don't use 100% cpu on WoS"},
    {29, 0, "Don't allow cheat characters when I host scenes."},
    {31, 1, "Zoom in on WoS Tactics attacks"}
};

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
    char path[4096], key[32];
    Ini ini;
    char *text;
    int i, changed = 0, have_file;
    /* GetProfileInt returns the record's default when the key is absent, so the
     * defaults apply whether or not a profile exists -- this is not an
     * "only if have_file" step. */
    for (i = 0; i < OPT_NAMED_COUNT; ++i) g_named[i] = g_named_info[i].dflt;
    for (i = 0; i < OPTIONS_COUNT; ++i) g_option[i] = 0;
    for (i = 0; i < OPTIONS_TABLE_COUNT; ++i) g_option[g_option_def[i].id] = g_option_def[i].dflt;
    path_of(path, sizeof(path));
    text = text_read_file(path, NULL);
    have_file = text && ini_parse(&ini, text) == 0;
    if (have_file) {
        /* FUN_00466EF3: for each of the OPTIONS_TABLE_COUNT records, the value is
         * GetProfileInt("option <id>", <default from the record>). The count is 32,
         * NOT 33: the array has 33 slots but id 9 has no record, so option 9 is never
         * loaded, never named, and stays 0. */
        for (i = 0; i < OPTIONS_TABLE_COUNT; ++i) {
            const char *v;
            int id = g_option_def[i].id;
            snprintf(key, sizeof(key), "option %d", id);
            v = ini_get(&ini, OPTIONS_PROFILE_SECTION, key, NULL);
            if (v) g_option[id] = atoi(v);
        }
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
