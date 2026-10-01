/* Solo Channel chat pane and the offline slash/console command dispatcher.
 * Order, comparison style and side effects follow FUN_00434f95 (0x434f95) and its
 * first-stage dispatcher FUN_00434756 (0x434756); the VA of each command string is cited. */
#include "chat.h"
#include "battle.h"
#include "game.h"
#include "minigame_data.h"
#include "world.h"
#include "../engine/clock.h"
#include "editors.h"
#include "../engine/fb.h"
#include "../engine/font.h"
#include "../engine/ini.h"
#include "../engine/log.h"
#include "../engine/rng.h"
#include "../engine/text.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHAT_TEXT_ID 1u
#define SPRINGY_MAX_OBJECTS 64
#define SPRINGY_MAX_MASSES  256
#define SPRINGY_MAX_SPRINGS 1024

/* The /springy world. FUN_0041e1aa builds "<world>\<name>", FUN_0047392e reads it and
 * FUN_00473967(1) starts the simulation; the ini is plain [General] plus one [ObjectN]
 * section per body, exactly as the shipping extracted/worlds/Evergreen/springy.ini documents. */
typedef struct SpringyWorld {
    char name[64];
    double width, gravity, air_friction, step_seconds, elasticity;
    int object_count;
    int masses[SPRINGY_MAX_OBJECTS][SPRINGY_MAX_MASSES][4];   /* kg, relX, relY, radius */
    int mass_count[SPRINGY_MAX_OBJECTS];
    int spring_a[SPRINGY_MAX_OBJECTS * SPRINGY_MAX_SPRINGS];
    int spring_b[SPRINGY_MAX_OBJECTS * SPRINGY_MAX_SPRINGS];
    int spring_style[SPRINGY_MAX_OBJECTS * SPRINGY_MAX_SPRINGS];
    int spring_count[SPRINGY_MAX_OBJECTS];
    double pos[SPRINGY_MAX_OBJECTS][2];
    double vel[SPRINGY_MAX_OBJECTS][2];
    double rotation[SPRINGY_MAX_OBJECTS];
    double scale[SPRINGY_MAX_OBJECTS];
} SpringyWorld;

static char log_lines[CHAT_LOG_LINES][CHAT_LINE_MAX];
static int log_count, log_first;
static char input_line[CHAT_INPUT_MAX];
static Rect pane = { 0, 420, PLAT_SCREEN_W, PLAT_SCREEN_H - 420 };
static int pane_open;
static Ui ui;
static int ui_ready;

/* The original's globals, kept under their own names so each toggle can be traced back. */
static int g_ouvrir;        /* DAT_004e6f60 */
static int g_share;         /* DAT_004e6f64 */
static int g_seance;        /* DAT_004e6f6c */
static int g_eavesdrop;     /* DAT_004e6f70 */
static int g_tune;          /* DAT_004e6f74 */
static int g_fps;           /* DAT_004e6f7c */
static int g_peek;          /* DAT_004e6f68 */
static int g_afk;           /* DAT_004e48d0 */
static char g_afk_name[256];/* DAT_004e48e0 */
static int g_gossip;        /* DAT_004e70a0 */
static int g_terrain;       /* DAT_004df8b4 */
static int g_monsters;      /* DAT_004df8b8 */
static int g_coord;         /* DAT_004e4828 */
static int g_weather;       /* *(int *)(DAT_005006bc + 0x6c) */
static int g_fx;            /* *(int *)(DAT_005006bc + 0x68) */
static int g_pal;           /* DAT_004e18b8 */
static int g_gender;        /* *(uint *)(DAT_0067fbf8 + 0xaa0) & 3 */
static int g_gimme_budget;  /* the /gimme gate: FUN_0043b7cf() must be non-zero */
static char g_world_override[128]; /* DAT_004e6f80, "worlds\\<name>" from /q <name> */
static MgSpringy springy;
static int springy_loaded;

/* --- small helpers ------------------------------------------------------------------------ */

static int ci_prefix(const char *s, const char *pfx, size_t n)
{
    size_t i;
    for (i = 0; i < n; ++i) {
        int a = tolower((unsigned char)s[i]), b = tolower((unsigned char)pfx[i]);
        if (a != b) return 0;
        if (!a) return 0;
    }
    return 1;
}

static int ci_equal(const char *a, const char *b) { return text_casecmp(a, b) == 0; }

/* _strnicmp(s, pfx, strlen(pfx)): the command word, ignoring whatever follows. */
static int is_word(const char *s, const char *word) { return ci_prefix(s, word, strlen(word)); }

static const char *arg_at(const char *line, int offset) { return line + offset; }

static int arg_int(const char *line, int offset)
{
    return (int)strtol(line + offset, NULL, 10);
}

/* Every recognized command logs once, so a replay can assert on it. */
static int took(const char *name, const char *arg, const char *state)
{
    wos_log_event("chat_command", "cmd=%s arg=%s state=%s", name, arg ? arg : "", state);
    return 1;
}

static void log_append(const char *text)
{
    int slot = (log_first + log_count) % CHAT_LOG_LINES;
    if (log_count == CHAT_LOG_LINES) {
        memmove(log_lines[0], log_lines[1], (CHAT_LOG_LINES - 1) * CHAT_LINE_MAX);
        slot = CHAT_LOG_LINES - 1;
    } else {
        ++log_count;
    }
    snprintf(log_lines[slot], CHAT_LINE_MAX, "%s", text);
}

void chat_say(const char *text)
{
    if (!text || !*text) return;
    log_append(text);
    wos_log_event("chat_say", "text=%s", text);
}

/* FUN_0049CB86's plain arm: "<name>:" in the name colour, then the text (both through
 * FUN_0049D3AF). An empty name is ChatSystemLine (0x46CE24): the text alone. */
void chat_echo(const char *name, const char *text)
{
    char line[CHAT_LINE_MAX];
    if (!text || !*text) return;
    if (name && *name) snprintf(line, sizeof(line), "%s: %s", name, text);
    else snprintf(line, sizeof(line), "%s", text);
    log_append(line);
    wos_log_event("chat_line", "name=%s text=%s", name ? name : "", text);
}

int chat_log_count(void) { return log_count; }

const char *chat_log_line(int index)
{
    if (index < 0 || index >= log_count) return NULL;
    return log_lines[(log_first + index) % CHAT_LOG_LINES];
}

/* --- pane --------------------------------------------------------------------------------- */

void chat_init(void)
{
    log_count = log_first = 0;
    log_lines[0][0] = 0;
    input_line[0] = 0;
    pane_open = 0;
    ui_ready = 0;
    g_ouvrir = g_share = g_seance = g_eavesdrop = g_fps = g_peek = 0;
    g_gossip = g_terrain = g_monsters = g_coord = 0;
    g_afk = 0; g_afk_name[0] = 0; g_tune = 0;
    g_weather = g_fx = g_pal = g_gender = 0;
    g_gimme_budget = 0;
    g_world_override[0] = 0;
    springy_loaded = 0;
    chat_say("Solo Channel");
}

void chat_layout(int x, int y, int w, int h)
{
    pane.x = x; pane.y = y; pane.w = w; pane.h = h;
}

int chat_is_open(void) { return pane_open; }

int chat_update(const Input *input)
{
    Rect grip;
    if (!ui_ready) { ui_begin(&ui, input); ui_ready = 1; } else ui.input = input;
    grip = (Rect){ pane.x, pane.y, pane.w, 4 };
    if (input->mouse_pressed & 2u) {   /* button 1 = left, so bit 1<<1 */
        int in_grip = input->mouse_x >= grip.x && input->mouse_y >= grip.y &&
                      input->mouse_x < grip.x + grip.w && input->mouse_y < grip.y + grip.h;
        if (in_grip) {
            pane_open = !pane_open;
            wos_log_event("chat_pane", "state=%s", pane_open ? "open" : "closed");
            return 1;
        }
        if (pane_open && input->mouse_y > grip.y + grip.h &&
            input->mouse_x >= pane.x && input->mouse_x < pane.x + pane.w) return 1;
    }
    if (pane_open && chat_submit_input()) return 1;
    if (pane_open && input->pressed[PLAT_KEY_ESCAPE]) {
        pane_open = 0;
        wos_log_event("chat_pane", "state=closed");
        return 1;
    }
    return 0;
}

void chat_render(Framebuffer *fb)
{
    int i, y;
    Rect old = fb->clip;
    fb_fill(fb, (Rect){ pane.x, pane.y, pane.w, 4 }, pane_open ? 0x676b80u : 0x353a50u);
    if (!pane_open) { fb->clip = old; return; }
    fb_fill(fb, pane, 0x000018);
    fb_clip_intersect(fb, pane);
    y = pane.y + 6;
    for (i = log_count - 1; i >= 0 && y + 8 <= pane.y + pane.h - 12; --i) {
        font_draw(fb, pane.x + 4, y, chat_log_line(i), 0xc8c8d8);
        y += 8;
    }
    fb_fill(fb, (Rect){ pane.x + 2, pane.y + pane.h - 12, pane.w - 4, 10 }, 0x101018);
    font_draw(fb, pane.x + 4, pane.y + pane.h - 11, input_line, 0xffffff);
    fb->clip = old;
}

/* --- /springy ------------------------------------------------------------------------------ */

/* The world itself is parsed by minigame_data.c, which owns springy.ini. FUN_0041e1aa builds
 * "<world>\\<name>" from the name the command carries and defaults it to "springy.ini" (string
 * 0x4ea624); mg_springy_load_named is that "%s/%s" world path with the caller's file name. */
int chat_springy_load(const char *ini_name)
{
    const char *name = ini_name && *ini_name ? ini_name : "springy.ini";
    mg_springy_load_named(name, &springy);
    springy_loaded = 1;
    wos_log_event("chat_command",
                  "cmd=springy arg=%s state=loaded objects=%d masses=%d width=%.3f gravity=%.3f step=%.3f",
                  name, chat_springy_object_count(), springy.count,
                  springy.width, springy.gravity, springy.step_seconds);
    {
        int i;
        for (i = 0; i < springy.count && i < 32; ++i)
            wos_log_event("springy_object", "index=%d pinned=%d kg=%.3f rel=%.3f,%.3f radius=%.3f",
                          i, springy.mass[i].pinned, springy.mass[i].kg,
                          springy.mass[i].rel_x, springy.mass[i].rel_y, springy.mass[i].radius);
    }
    return 0;
}

int chat_springy_loaded(void) { return springy_loaded; }
/* MgSpringy.count is the number of masses, not of [ObjectN] sections, so the object count is
 * the number of distinct MgMass.object values present. */
int chat_springy_object_count(void)
{
    int i, j, n = 0;
    if (!springy_loaded) return 0;
    for (i = 0; i < springy.count; ++i) {
        int seen = 0;
        for (j = 0; j < i; ++j) if (springy.mass[j].object == springy.mass[i].object) { seen = 1; break; }
        if (!seen) ++n;
    }
    return n;
}

/* The original's ini has one [ObjectN] section per body; the port's flat mass table keeps the
 * section index in MgMass.object, so the masses of one body are the rows that name it. */
int chat_springy_masses(int object)
{
    int i, n = 0;
    if (!springy_loaded) return 0;
    for (i = 0; i < springy.count; ++i) if (springy.mass[i].object == object) ++n;
    return n;
}

int chat_springy_mass(int object, int index, double mass[4])
{
    int i, n = 0;
    if (!springy_loaded) return -1;
    for (i = 0; i < springy.count; ++i) {
        if (springy.mass[i].object != object) continue;
        if (n++ == index) {
            mass[0] = springy.mass[i].kg;
            mass[1] = springy.mass[i].rel_x;
            mass[2] = springy.mass[i].rel_y;
            mass[3] = springy.mass[i].radius;
            return 0;
        }
    }
    return -1;
}

/* --- /dice and /pdice (FUN_00434574) -------------------------------------------------------- */

void chat_roll_dice(const char *spec, int is_public, char *out, size_t out_size)
{
    /* default 1d6; "[N]dM" with N clamped to 1..9 and M clamped to 2..32767 */
    int count = 1, sides = 6, i, total = 0;
    char work[128], *w;
    const char *p = spec && *spec ? spec : "";
    snprintf(work, sizeof(work), "%s", p);
    w = work;
    while (*w == ' ') ++w;
    if (strlen(w) > 2 && toupper((unsigned char)w[1]) == 'D') {
        count = w[0] - '0';
        if (count < 2) count = 1;
        if (count > 8) count = 9;
        sides = 0;
        p = w + 2;
        while (*p && isdigit((unsigned char)*p)) { sides = *p - '0' + sides * 10; ++p; }
    }
    if (sides > 0x7ffe) sides = 0x7fff;
    if (sides < 2) sides = 1;
    out[0] = 0;
    {
        size_t used = 0;
        used += (size_t)snprintf(out + used, out_size - used, "%d %d-sided %s for: ",
                                 count, sides, count == 1 ? "die" : "dice");
    }
    for (i = 0; i < count; ++i) {
        /* one crt_rand() per rand(), in the original's order */
        int roll = crt_rand() % sides + 1;
        size_t used = strlen(out);
        total += roll;
        if (used + 8 < out_size) snprintf(out + used, out_size - used, "[%d]  ", roll);
    }
    {
        size_t used = strlen(out);
        if (used + 16 < out_size) snprintf(out + used, out_size - used, "= %d", total);
    }
    if (!is_public) {
        char line[CHAT_LINE_MAX];
        snprintf(line, sizeof(line), "You privately roll %s", out);
        chat_say(line);
    } else {
        char line[CHAT_LINE_MAX];
        snprintf(line, sizeof(line), "You roll %s", out);
        chat_say(line);
    }
    wos_log_event("chat_dice", "sides=%d count=%d total=%d public=%d", sides, count, total, is_public);
}

/* --- FUN_00434756: the offline-safe command dispatcher -------------------------------------- */

static int dispatch_offline(const char *line, const char *cmd)
{
    /* _stricmp against the whole word */
    if (ci_equal(cmd, "version")) {
        char creator[WORLD_VER_NAME_MAX], last_mod[WORLD_VER_NAME_MAX];
        uint32_t serial = 0;
        chat_say(CHAT_GAME_VERSION);
        took("version", "", "shown");
        if (editors_world_version(NULL, creator, last_mod, &serial) == 0) {
            char line2[CHAT_LINE_MAX];
            snprintf(line2, sizeof(line2), "World: %s  created by %s  SN:%08X", g_world.name, creator, serial);
            chat_say(line2);
        }
        return 1;
    }
    if (ci_equal(cmd, "news"))    return took("news", "", "browser");
    if (ci_equal(cmd, "forums"))  return took("forums", "", "browser");
    if (is_word(cmd, "tacticsBalance")) return took("tacticsBalance", "", "unavailable");
    if (is_word(cmd, "stupidTest"))     return took("stupidTest", "", "unavailable");
    if (is_word(cmd, "path "))    { took("path", arg_at(line, 6), "unavailable"); return 1; }
    if (is_word(cmd, "weather ")) {
        g_weather = arg_int(line, 9);
        took("weather", arg_at(line, 9), "set");
        wos_log_event("chat_weather", "value=%d", g_weather);
        return 1;
    }
    if (is_word(cmd, "bkgnd "))   { took("bkgnd", arg_at(line, 7), "unavailable"); return 1; }
    if (is_word(cmd, "theme "))   { took("theme", arg_at(line, 7), "unavailable"); return 1; }
    /* /springy [file] - the default is the world's own springy.ini (string 0x4ea624) */
    if (is_word(cmd, "springy")) {
        const char *name = strlen(cmd) > 8 ? arg_at(line, 9) : "springy.ini";
        chat_springy_load(name);
        took("springy", name, springy_loaded ? "loaded" : "failed");
        return 1;
    }
    if (is_word(cmd, "midi "))    { took("midi", arg_at(line, 6), "unavailable"); return 1; }
    if (is_word(cmd, "wav "))     { took("wav", arg_at(line, 6), "unavailable"); return 1; }
    if (ci_equal(cmd, "pwd"))      return took("pwd", "", "unavailable");
    if (ci_equal(cmd, "mags"))     return took("mags", "", "unavailable");
    if (ci_equal(cmd, "villagers")) return took("villagers", "", "unavailable");
    if (ci_equal(cmd, "art"))       return took("art", "", "unavailable");
    if (is_word(cmd, "dist "))     { took("dist", arg_at(line, 6), "unavailable"); return 1; }
    if (ci_equal(cmd, "coverage")) return took("coverage", "", "unavailable");
    if (is_word(cmd, "pal ")) {
        g_pal = arg_int(line, 5);
        took("pal", arg_at(line, 5), "set");
        wos_log_event("chat_pal", "value=%d", g_pal);
        return 1;
    }
    if (ci_equal(cmd, "doldrum")) return took("doldrum", "", "unavailable");
    if (ci_equal(cmd, "guilt"))   return took("guilt", "", "unavailable");
    if (ci_equal(cmd, "trophy") || ci_equal(cmd, "bag")) return took("trophy", "", "unavailable");
    if (ci_equal(cmd, "missions"))    return took("missions", "", "unavailable");
    if (ci_equal(cmd, "oldmissions")) return took("oldmissions", "", "unavailable");
    if (ci_equal(cmd, "battle") || ci_equal(cmd, "battle2") || ci_equal(cmd, "battle3")) {
        int mode = ci_equal(cmd, "battle") ? 1 : ci_equal(cmd, "battle2") ? 0 : 2;
        took(cmd, "", "run");
        battle_run_simulator(mode);
        return 1;
    }
    if (is_word(cmd, "props")) {
        int mode = arg_int(line, 6) % 3;
        took("props", arg_at(line, 6), "unavailable");
        wos_log_event("chat_props", "mode=%d", mode);
        return 1;
    }
    if (ci_equal(cmd, "clear"))  return took("clear", "", "unavailable");
    if (ci_equal(cmd, "coord")) {
        g_coord = !g_coord;
        took("coord", "", g_coord ? "on" : "off");
        wos_log_event("chat_overlay", "name=coord on=%d", g_coord);
        return 1;
    }
    return 0;
}

/* --- FUN_00434f95: the rest of the chain ---------------------------------------------------- */

static int dispatch_chat(const char *line, const char *cmd)
{
    if (ci_equal(cmd, "ouvrir")) {
        g_ouvrir = 1;
        return took("ouvrir", "", "on");
    }
    if (ci_equal(cmd, "funpak"))   return took("funpak", "", "unavailable");
    if (ci_equal(cmd, "battleLog")) return took("battleLog", "", "unavailable");
    if (ci_equal(cmd, "scope")) {
        if (!g_ouvrir) return 0;            /* the original ignores it with the channel shut */
        return took("scope", "", "unavailable");
    }
    if (ci_equal(cmd, "mix") || ci_equal(cmd, "admin")) return took(cmd, "", "unavailable");
    if (ci_equal(cmd, "fail"))     return took("fail", "", "unavailable");
    if (ci_equal(cmd, "inn"))      return took("inn", "", "unavailable");
    if (is_word(cmd, "scene ")) {
        took("scene", arg_at(line, 7), "unavailable");
        return 1;
    }
    if (is_word(cmd, "easter ")) { took("easter", arg_at(line, 8), "unavailable"); return 1; }
    if (is_word(cmd, "tune ")) {
        g_tune = arg_int(line, 6);
        took("tune", arg_at(line, 6), "set");
        wos_log_event("chat_tune", "channel=%d", g_tune);
        return 1;
    }
    if (ci_equal(cmd, "tune")) {
        char text[CHAT_LINE_MAX];
        snprintf(text, sizeof(text), "You are currently tuned to channel %d.", g_tune);
        chat_say(text);
        return took("tune", "", "reported");
    }
    if (ci_equal(cmd, "bubbles")) { took("bubbles", "", "unavailable"); return 1; }
    if (ci_equal(cmd, "http") || ci_equal(cmd, "www") || ci_equal(cmd, "web")) {
        return took(cmd, "", "unavailable");
    }
    if (ci_equal(cmd, "fps")) {
        g_fps = !g_fps;
        took("fps", "", g_fps ? "on" : "off");
        return 1;
    }
    if (is_word(cmd, "gender ")) {
        g_gender = arg_int(line, 8) & 3;
        took("gender", arg_at(line, 8), "set");
        wos_log_event("chat_gender", "value=%d", g_gender);
        return 1;
    }
    if (is_word(cmd, "afk")) {
        g_afk = 1;
        if (strlen(cmd) < 4) {
            g_afk_name[0] = 0;               /* the original re-reads the MFC profile string */
            took("afk", "", "cleared");
        } else {
            snprintf(g_afk_name, sizeof(g_afk_name), "%s", arg_at(line, 5));
            took("afk", g_afk_name, "set");
        }
        wos_log_event("chat_afk", "name=%s since=%u", g_afk_name, clock_ms());
        return 1;
    }
    if (is_word(cmd, "shout ")) {
        char text[CHAT_LINE_MAX];
        snprintf(text, sizeof(text), "!%s", arg_at(line, 7));
        chat_say(text);
        return took("shout", arg_at(line, 7), "sent");
    }
    if (is_word(cmd, "w ")) {
        char text[CHAT_LINE_MAX];
        snprintf(text, sizeof(text), ";%s", arg_at(line, 3));
        chat_say(text);
        return took("w", arg_at(line, 3), "sent");
    }
    if (is_word(cmd, "r ")) {
        char text[CHAT_LINE_MAX];
        snprintf(text, sizeof(text), ";%s", arg_at(line, 3));
        chat_say(text);
        return took("r", arg_at(line, 3), "sent");
    }
    if (is_word(cmd, "fx ")) {
        g_fx = arg_int(line, 4);
        took("fx", arg_at(line, 4), "set");
        wos_log_event("chat_fx", "value=%d", g_fx);
        return 1;
    }
    if (is_word(cmd, "level ")) return took("level", arg_at(line, 6), "unavailable");
    if (is_word(cmd, "seed ")) {
        /* FUN_00434f95 draws a 100000-step rand() burn here, so every /seed consumes exactly
         * that many crt_rand() calls before the branch ends. */
        int i;
        for (i = 0; i < 100000; ++i) (void)crt_rand();
        took("seed", arg_at(line, 5), "burned");
        return 1;
    }
    if (ci_equal(cmd, "sayings")) { took("sayings", "", "unavailable"); return 1; }
    if (ci_equal(cmd, "diary"))    { took("diary", "", "unavailable"); return 1; }
    if (ci_equal(cmd, "colors"))   { took("colors", "", "unavailable"); return 1; }
    if (ci_equal(cmd, "tactics"))  { took("tactics", "", "unavailable"); return 1; }
    if (ci_equal(cmd, "peek")) {
        if (!g_ouvrir) return 1;
        g_peek = !g_peek;
        return took("peek", "", g_peek ? "on" : "off");
    }
    if (ci_equal(cmd, "skin")) { took("skin", "", "unavailable"); return 1; }
    if (ci_equal(cmd, "pi"))   { took("pi", "", "unavailable"); return 1; }
    if (ci_equal(cmd, "lag"))  { took("lag", "", "unavailable"); return 1; }
    if (ci_equal(cmd, "share")) {
        g_share = !g_share;
        return took("share", "", g_share ? "on" : "off");
    }
    if (ci_equal(cmd, "volume")) { took("volume", "", "unavailable"); return 1; }
    if (ci_equal(cmd, "earth") || ci_equal(cmd, "gaiea")) return took(cmd, "", "unavailable");
    if (ci_equal(cmd, "halo"))   { took("halo", "", "unavailable"); return 1; }
    if (is_word(cmd, "stepper")) { took("stepper", arg_at(line, 9), "unavailable"); return 1; }
    if (is_word(cmd, "give ")) { took("give", arg_at(line, 6), "unavailable"); return 1; }
    if (is_word(cmd, "take ")) { took("take", arg_at(line, 6), "unavailable"); return 1; }
    if (is_word(cmd, "fight ")) { took("fight", arg_at(line, 7), "unavailable"); return 1; }
    if (ci_equal(cmd, "password")) { took("password", "", "unavailable"); return 1; }
    if (ci_equal(cmd, "pokedex"))   { took("pokedex", "", "unavailable"); return 1; }
    if (ci_equal(cmd, "pet"))       { took("pet", "", "unavailable"); return 1; }
    if (ci_equal(cmd, "asteroid"))  { took("asteroid", "", "unavailable"); return 1; }
    if (is_word(cmd, "im ")) { took("im", arg_at(line, 4), "unavailable"); return 1; }
    if (ci_equal(cmd, "g")) {
        g_gossip = !g_gossip;
        /* the original refuses gossip on a tuned channel */
        if (!g_tune) g_gossip = 0;
        took("g", "", g_gossip ? "on" : "off");
        wos_log_event("chat_gossip", "on=%d", g_gossip);
        return 1;
    }
    if (ci_equal(cmd, "q"))  { took("q", "", "unavailable"); return 1; }
    if (is_word(cmd, "q ")) {
        snprintf(g_world_override, sizeof(g_world_override), "worlds\\%s", arg_at(line, 3));
        took("q", arg_at(line, 3), "set");
        wos_log_event("chat_world", "path=%s", g_world_override);
        return 1;
    }
    if (ci_equal(cmd, "eavesdrop")) {
        g_eavesdrop = !g_eavesdrop;
        return took("eavesdrop", "", g_eavesdrop ? "on" : "off");
    }
    if (ci_equal(cmd, "reload"))  { took("reload", "", "unavailable"); return 1; }
    if (ci_equal(cmd, "homework")) { took("homework", "", "unavailable"); return 1; }
    if (is_word(cmd, "math "))  { took("math", arg_at(line, 6), "unavailable"); return 1; }
    if (is_word(cmd, "class ")) {
        took("class", arg_at(line, 7), "unavailable");
        return 1;
    }
    if (ci_equal(cmd, "seance")) {
        g_seance = !g_seance;
        return took("seance", "", g_seance ? "on" : "off");
    }
    if (is_word(cmd, "bleep "))   { took("bleep", arg_at(line, 7), "unavailable"); return 1; }
    if (is_word(cmd, "unbleep ")) { took("unbleep", arg_at(line, 9), "unavailable"); return 1; }
    if (ci_equal(cmd, "mic"))   { took("mic", "", "unavailable"); return 1; }
    if (ci_equal(cmd, "nomic")) { took("nomic", "", "unavailable"); return 1; }
    if (ci_equal(cmd, "speech")) { took("speech", "", "unavailable"); return 1; }
    if (ci_equal(cmd, "terrain")) {
        g_terrain = !g_terrain;
        took("terrain", "", g_terrain ? "on" : "off");
        wos_log_event("chat_overlay", "name=terrain on=%d", g_terrain);
        return 1;
    }
    if (ci_equal(cmd, "monsters")) {
        /* FUN_00434f95 gates this on FUN_0043b9b6() being non-zero, i.e. a map being loaded */
        if (!game_current_map()) {
            took("monsters", "", "no_map");
            return 1;
        }
        g_monsters = !g_monsters;
        took("monsters", "", g_monsters ? "on" : "off");
        wos_log_event("chat_overlay", "name=monsters on=%d", g_monsters);
        return 1;
    }
    if (is_word(cmd, "help")) { took("help", arg_at(line, 6), "unavailable"); return 1; }
    if (ci_equal(cmd, "element")) { took("element", "", "unavailable"); return 1; }
    if (ci_equal(cmd, "malloc"))  { took("malloc", "", "unavailable"); return 1; }
    if (ci_equal(cmd, "phist"))   { took("phist", "", "unavailable"); return 1; }
    if (ci_equal(cmd, "preset"))  { took("preset", "", "unavailable"); return 1; }
    if (ci_equal(cmd, "tile"))    { took("tile", "", "unavailable"); return 1; }
    if (is_word(cmd, "dice") || is_word(cmd, "pdice")) {
        char roll[CHAT_LINE_MAX];
        /* FUN_00434574 is handed the rest of the line after the command word */
        const char *spec = line + (is_word(cmd, "pdice") ? 6 : 5);
        chat_roll_dice(spec, 1, roll, sizeof(roll));
        return took(is_word(cmd, "pdice") ? "pdice" : "dice", spec, "rolled");
    }
    if (is_word(cmd, "gimme")) { took("gimme", "", "unavailable"); return 1; }
    if (is_word(cmd, "a ")) {
        chat_say(arg_at(line, 3));
        return took("a", arg_at(line, 3), "sent");
    }
    return 0;
}

/* --- entry points --------------------------------------------------------------------------- */

int chat_submit(const char *line)
{
    const char *cmd;
    char work[CHAT_LINE_MAX];
    if (!line) return 0;
    snprintf(work, sizeof(work), "%s", line);
    /* trailing spaces are what the MFC edit would have stripped */
    while (*work && (work[strlen(work) - 1] == ' ' || work[strlen(work) - 1] == '\r')) work[strlen(work) - 1] = 0;
    if (!*work) return 0;
    if (*work != '/') {
        /* FUN_004341b3 posts the line to the channel; offline that is the Solo Channel. */
        chat_say(work);
        wos_log_event("chat_send", "channel=solo text=%s", work);
        return 0;
    }
    cmd = work + 1;
    if (dispatch_offline(work, cmd)) return 1;
    if (dispatch_chat(work, cmd)) return 1;
    wos_log_event("chat_command", "cmd=%s state=unknown", cmd);
    return 1;
}

int chat_submit_input(void)
{
    int consumed = 0;
    size_t n = strlen(input_line);
    if (ui.input->pressed[PLAT_KEY_BACKSPACE]) {
        if (n) { do { --n; } while (n && ((unsigned char)input_line[n] & 0xc0) == 0x80); input_line[n] = 0; }
        consumed = 1;
    }
    if (ui.input->pressed[PLAT_KEY_DELETE]) { input_line[0] = 0; consumed = 1; }
    if (ui.input->text[0]) {
        size_t add = strlen(ui.input->text);
        if (n + add + 1 < sizeof(input_line)) {
            memcpy(input_line + n, ui.input->text, add + 1);
            consumed = 1;
        }
    }
    if (ui.input->pressed[PLAT_KEY_RETURN]) {
        chat_submit(input_line);
        input_line[0] = 0;
        return 1;
    }
    return consumed;
}

/* --- accessors ------------------------------------------------------------------------------ */

int chat_overlay_terrain(void)  { return g_terrain; }
int chat_overlay_monsters(void) { return g_monsters; }
int chat_overlay_coord(void)    { return g_coord; }
int chat_share(void)            { return g_share; }
int chat_eavesdrop(void)        { return g_eavesdrop; }
int chat_seance(void)           { return g_seance; }
int chat_gossip(void)           { return g_gossip; }
int chat_show_fps(void)         { return g_fps; }
int chat_peek(void)             { return g_peek; }
int chat_channel_open(void)     { return g_ouvrir; }
int chat_tune(void)             { return g_tune; }
int chat_gimme_budget(void)     { return g_gimme_budget; }

/* --- dump ----------------------------------------------------------------------------------- */

void chat_dump(DumpEmit emit, void *user)
{
    dump_emit_int(emit, "chat.open", pane_open, user);
    dump_emit_int(emit, "chat.terrain_overlay", g_terrain, user);
    dump_emit_int(emit, "chat.monsters_overlay", g_monsters, user);
    dump_emit_int(emit, "chat.coord", g_coord, user);
    dump_emit_int(emit, "chat.share", g_share, user);
    dump_emit_int(emit, "chat.eavesdrop", g_eavesdrop, user);
    dump_emit_int(emit, "chat.seance", g_seance, user);
    dump_emit_int(emit, "chat.gossip", g_gossip, user);
    dump_emit_int(emit, "chat.fps", g_fps, user);
    dump_emit_int(emit, "chat.peek", g_peek, user);
    dump_emit_int(emit, "chat.tune", g_tune, user);
    dump_emit_int(emit, "chat.afk", g_afk, user);
    dump_emit_int(emit, "chat.weather", g_weather, user);
    dump_emit_int(emit, "chat.fx", g_fx, user);
    dump_emit_int(emit, "chat.pal", g_pal, user);
    dump_emit_int(emit, "chat.gender", g_gender, user);
    dump_emit_int(emit, "chat.messages", log_count, user);
    dump_emit_int(emit, "chat.springy", springy_loaded, user);
}
