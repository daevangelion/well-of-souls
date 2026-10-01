/* Environmental sound themes and the SRN boot mixer. Both run in offline solo play and
 * both consume crt_rand(), so the port carries them whatever they sound like.
 * Owner: Core. */
#include "sched.h"
#include "options.h"
#include "world.h"
#include "../game_main.h"
#include "../engine/clock.h"
#include "../engine/log.h"
#include "../engine/rng.h"
#include "../engine/text.h"
#include "../platform/platform.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- sound ids: FUN_00429A31 / FUN_00429BCC ------------------------------------
 * The built-in table is initialised .data at 0x4E62E0, 0x42 {handle, name} pairs with the
 * count at 0x4E64F0. Ids 39..42 and 43..50 really do repeat sword1/magic1. */
static const char *const builtin_sounds[] = {
    "stream1.wav", "beg.wav", "stream1.wav", "splash1.wav", "splash2.wav",
    "chirp1.wav", "chirp2.wav", "chirp3.wav", "chirp4.wav", "chirp5.wav",
    "chirp6.wav", "chirp7.wav", "chirp8.wav", "rain1.wav", "thunder1.wav",
    "thunder2.wav", "thunder3.wav", "thunder4.wav", "wind1.wav", "wind2.wav",
    "wind3.wav", "wind4.wav", "wind5.wav", "wind6.wav", "wind7.wav",
    "wind8.wav", "waterfall1.wav", "campfire1.wav", "fire1.wav", "fire2.wav",
    "fire3.wav", "fire4.wav", "growl1.wav", "growl2.wav", "growl3.wav",
    "growl4.wav", "growl5.wav", "growl6.wav", "growl7.wav", "sword1.wav",
    "sword1.wav", "sword1.wav", "sword1.wav", "magic1.wav", "magic1.wav",
    "magic1.wav", "magic1.wav", "magic1.wav", "magic1.wav", "magic1.wav",
    "magic1.wav", "pain1.wav", "pain2.wav", "pain3.wav", "pain4.wav",
    "summon1.wav", "travel1.wav", "type.wav", "listen.wav", "actor.wav",
    "boom3.wav", "bell1.wav", "ding1.wav", "bell2.wav", "thud.wav", "bell2.wav"
};
#define BUILTIN_SOUNDS ((int)(sizeof builtin_sounds / sizeof builtin_sounds[0]))

/* 0x54D578: 256 slots of 0x2C bytes, {handle, name[40]}; "" marks a free slot. Cleared
 * by FUN_00429A07 at boot and when the sound device is reinitialised, never per world. */
static char custom_sounds[ENV_SOUND_CUSTOM_MAX][40];

int env_sound_id(const char *name)
{
    char key[1000];
    int i, free_slot = -1;
    size_t n;
    if (!name) return -1;
    snprintf(key, sizeof key, "%s", name);
    for (n = 0; key[n]; ++n) key[n] = (char)tolower((unsigned char)key[n]);   /* _strlwr */
    if (!strstr(key, ".wav") && n + 4 < sizeof key) strcat(key, ".wav");
    for (i = 0; i < ENV_SOUND_CUSTOM_MAX; ++i) {
        if (!custom_sounds[i][0]) { if (free_slot == -1) free_slot = i; continue; }
        if (text_casecmp(key, custom_sounds[i]) == 0) return ENV_SOUND_CUSTOM_BASE + i;
    }
    if (free_slot < 0) return -1;
    strncpy(custom_sounds[free_slot], key, 0x27);                 /* strncpy(.., 0x27) */
    custom_sounds[free_slot][0x27] = 0;
    return ENV_SOUND_CUSTOM_BASE + free_slot;
}

static const char *sound_name(int id)
{
    if (id >= ENV_SOUND_CUSTOM_BASE && id < ENV_SOUND_CUSTOM_BASE + ENV_SOUND_CUSTOM_MAX)
        return custom_sounds[id - ENV_SOUND_CUSTOM_BASE][0] ? custom_sounds[id - ENV_SOUND_CUSTOM_BASE] : NULL;
    if (id >= 0 && id < BUILTIN_SOUNDS) return builtin_sounds[id];
    return NULL;
}

/* "sfx\%s" through ResolveArtPath: the world's SFX folder when a world is loaded, then
 * the install root's. The title plays before any world exists, so the second step uses
 * game_data_path(), not world_data_path() (whose root world_load() sets). */
static char *load_sound(const char *name, size_t *len)
{
    char rel[128], path[1024];
    char *bytes = NULL;
    int n;
    snprintf(rel, sizeof rel, "SFX/%s", name);
    if (g_world.dir[0]) bytes = text_read_file(world_path(path, sizeof path, rel), len);
    n = snprintf(path, sizeof path, "%s/%s", game_data_path(), rel);
    if (!bytes && n > 0 && (size_t)n < sizeof path) bytes = text_read_file(path, len);
    return bytes;
}

int env_sound_play(int id, int mode)
{
    const char *name;
    char *bytes;
    size_t len;
    if (id < 0 || !option_named_get(OPT_ENABLE_SOUND_CARD)) return -1;
    name = sound_name(id);
    if (!name) return -1;
    if (mode == ENV_PLAY_STOP) { plat_sound_key_stop(id); return id; }
    /* FUN_0046890D: a one-shot also needs enableSFX; the loop (mode 2) does not. */
    if (mode == ENV_PLAY_ONCE && !option_named_get(OPT_ENABLE_SFX)) return id;
    bytes = load_sound(name, &len);
    if (!bytes) { wos_log_event("sound_missing", "name=%s", name); return -1; }
    plat_sound_key_play(id, bytes, len, mode == ENV_PLAY_LOOP);
    free(bytes);
    wos_log_event("env_sound", "id=%d mode=%d name=%s", id, mode, name);
    return id;
}

/* FUN_00429CE0: stop every sound id there is, built-in and custom. */
static void stop_all_sounds(void)
{
    int i;
    for (i = 0; i < BUILTIN_SOUNDS; ++i) plat_sound_key_stop(i);
    for (i = 0; i < ENV_SOUND_CUSTOM_MAX; ++i)
        if (custom_sounds[i][0]) plat_sound_key_stop(ENV_SOUND_CUSTOM_BASE + i);
}

/* --- the theme table: 0x80C bytes, built on the stack, installed at 0x5EB0E0 --- */
typedef struct {
    int      loop;                        /* +0x004, sound id or -1 */
    int      count;                       /* +0x008 */
    int      id[ENV_ENTRIES_MAX];         /* +0x00C */
    uint32_t stamp[ENV_ENTRIES_MAX];      /* +0x20C, GetTickCount at (re)arm */
    int      period[ENV_ENTRIES_MAX];     /* +0x40C, seconds */
    uint32_t delay[ENV_ENTRIES_MAX];      /* +0x60C, ms */
} EnvTable;

static EnvTable g_active;                 /* 0x5EB0E4.. */
static int g_active_on;                   /* 0x5EB0E0 */
static int g_current_theme;               /* _DAT_004F0E28 */

/* rand() % (period*2) with MSVC's truncating %. A period of 0 divides by zero in the
 * original (a crash); the port still takes the draw and uses 0. */
static int random_seconds(int period)
{
    int r = crt_rand();
    return period * 2 != 0 ? r % (period * 2) : 0;
}

/* FUN_00456B87: append one one-shot, consuming exactly one rand(). */
static void env_append(EnvTable *t, int id, int period)
{
    int i = t->count;
    if (i >= ENV_ENTRIES_MAX) return;
    t->count = i + 1;
    t->id[i] = id;
    t->stamp[i] = clock_ms();
    t->period[i] = period;
    t->delay[i] = (uint32_t)(random_seconds(period) * 1000);
}

/* FUN_00456B2C: stop the installed theme, if one is. */
static void env_stop(void)
{
    if (g_active_on > 0) { stop_all_sounds(); g_active_on = 0; }
}

/* FUN_00456B46: replace the installed theme and start its loop. */
static void env_install(const EnvTable *t)
{
    env_stop();
    g_active = *t;
    if (g_active.loop >= 0) env_sound_play(g_active.loop, ENV_PLAY_LOOP);
    g_active_on = 1;
}

/* +THEMES, as SoundThemesParser leaves it at 0x5EB8F0: 0xA8 bytes per id. */
static struct {
    int used, loop;
    int id[ENV_THEME_SHOTS], period[ENV_THEME_SHOTS];
} g_themes[ENV_THEMES_MAX];

void env_world_loaded(void)
{
    static char tok[32][256];
    int i, inside = 0;
    memset(g_themes, 0, sizeof g_themes);
    for (i = 0; i < g_world.line_count; ++i) {
        int n = world_tokenize(g_world.lines[i], tok, 32), id, k;
        if (n <= 0) continue;
        if (!inside) { if (!text_casecmp(tok[0], "+THEMES")) inside = 1; continue; }
        if (!text_casecmp(tok[0], "-THEMES")) break;
        if (n < 3) {
            wos_log_event("world_error", "msg=\"Not enough arguments in Sound Theme\" line=%d", i + 1);
            continue;
        }
        id = atoi(tok[0]);
        if (id > 255 || id < 0) id = 0;      /* the original reports it, then uses id 0 */
        g_themes[id].used = 1;
        if (tok[2][0]) g_themes[id].loop = env_sound_id(tok[2]);
        /* One slot per token, filled or not: an empty or malformed token still advances.
         * The original has no bound here and runs into the next id's record past 20. */
        for (k = 3; k < n && k - 3 < ENV_THEME_SHOTS; ++k) {
            char *eq;
            if (!tok[k][0]) continue;
            eq = strchr(tok[k], '=');
            if (!eq) {
                wos_log_event("world_error", "msg=\"Missing seconds=sound in sound theme\" id=%d", id);
                continue;
            }
            *eq = 0;
            g_themes[id].period[k - 3] = atoi(tok[k]);
            g_themes[id].id[k - 3] = env_sound_id(eq + 1);
        }
    }
}

/* FUN_00456EC1: a world-defined theme replaces the built-in one of the same id. */
static int world_theme(int theme, EnvTable *t)
{
    int k;
    if (theme < 1 || theme > 0xFF || !g_themes[theme].used) return 0;
    if (g_themes[theme].loop > 0) t->loop = g_themes[theme].loop;
    for (k = 0; k < ENV_THEME_SHOTS; ++k)
        if (g_themes[theme].id[k] > 0) env_append(t, g_themes[theme].id[k], g_themes[theme].period[k]);
    return 1;
}

static void append_chirps(EnvTable *t)    /* FUN_00456BE1 */
{
    int id;
    for (id = 5; id <= 0xC; ++id) env_append(t, id, 0x1E);
}

void env_theme(int theme)
{
    EnvTable t;
    int id;
    g_current_theme = theme;
    if (!option_named_get(OPT_ENABLE_SFX) || !option_named_get(OPT_ENABLE_ENVIRONMENTAL_SOUNDS) ||
        !option_named_get(OPT_ENABLE_SOUND_CARD))
        return;
    memset(&t, 0, sizeof t);
    t.loop = -1;
    if (world_theme(theme, &t)) { env_install(&t); goto done; }
    switch (theme) {
    case 1: t.loop = 2;    env_append(&t, 3, 10); env_append(&t, 4, 10); append_chirps(&t); break;
    case 2: t.loop = 0xD;  for (id = 0xE; id <= 0x11; ++id) env_append(&t, id, 0x1E); break; /* FUN_00456C51 */
    case 3: t.loop = 0x12; for (id = 0x13; id <= 0x19; ++id) env_append(&t, id, 0x1E); break; /* FUN_00456C8E */
    case 4: t.loop = -1;   for (id = 0x13; id <= 0x19; ++id) env_append(&t, id, 0x1E); append_chirps(&t); break;
    case 5: t.loop = 0x1A; append_chirps(&t); break;
    case 6: t.loop = 0x1B; append_chirps(&t); break;
    case 7: t.loop = 0x1B; for (id = 0x1C; id <= 0x1F; ++id) env_append(&t, id, 10); break;   /* FUN_00456CF2 */
    case 8: t.loop = -1;   append_chirps(&t); break;
    default: env_stop(); wos_log_event("env_theme", "theme=%d count=-1", theme); return;
    }
    env_install(&t);
done:
    wos_log_event("env_theme", "theme=%d loop=%d count=%d", theme, g_active.loop, g_active.count);
}

void env_tick(void)
{
    int i;
    if (g_active_on <= 0) return;
    for (i = 0; i < g_active.count; ++i) {
        uint32_t now = clock_ms();
        int p = g_active.period[i];
        if (g_active.delay[i] < now - g_active.stamp[i]) {                /* unsigned compare */
            int base;
            env_sound_play(g_active.id[i], ENV_PLAY_ONCE);
            g_active.stamp[i] = clock_ms();
            base = p / 4 * 1000;                                          /* truncating /4 */
            g_active.delay[i] = (uint32_t)(random_seconds(p) * 1000 + base);
        }
    }
}

int env_current_theme(void) { return g_current_theme; }
int env_active_count(void) { return g_active_on > 0 ? g_active.count : -1; }

/* FUN_0042B4E0 (0x0042B4E0): the SRN warm-up mixer. From the disassembly:
 *
 *   do { a=rand(); b=rand(); c=rand(); d=rand(); t=GetTickCount(); e=rand(); }
 *   while ((((a<<4 ^ b)<<4 ^ c)<<4 ^ d<<16 ^ t ^ e) & 0x3FFFFFFF) == 0;
 *
 * Five `call esi` (rand) per iteration, at 0x42B4E8/EC/F3/FA/50C, and the oracle's rand
 * trace returns to all five (0x42B4EA/EE/F5/FC/50E). The clock is clock_ms() because the
 * fold must see the harness's virtual GetTickCount. */
void srn_mix(void)
{
    uint32_t fold;
    unsigned draws = 0;
    do {
        uint32_t a = (uint32_t)crt_rand();
        uint32_t b = (uint32_t)crt_rand();
        uint32_t c = (uint32_t)crt_rand();
        uint32_t d = (uint32_t)crt_rand();
        uint32_t t = clock_ms();
        uint32_t e = (uint32_t)crt_rand();
        fold = (((((a << 4) ^ b) << 4 ^ c) << 4) ^ (d << 16) ^ t ^ e) & 0x3FFFFFFFu;
        draws += 5;
    } while (fold == 0);
    wos_log_event("srn_mix", "draws=%u fold=%lu", draws, (unsigned long)fold);
}
