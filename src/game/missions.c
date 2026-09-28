/* missions.c — worlds/<W>/missions.ini, the per-hero mission log and the mission picker.
 * Every rule below cites the original; see src/game/missions.h for the module contract. */
#include "missions.h"
#include "hero.h"
#include "battle.h"
#include "items.h"
#include "world.h"
#include "../engine/font.h"
#include "../engine/ini.h"
#include "../engine/log.h"
#include "../engine/text.h"
#include "../game_main.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MISSIONS_MAX      512
#define MISSIONS_TROPHIES 100   /* FUN_004587C3 and FUN_00458923 both cap at 100 */
#define MIS_KEY_MAX       32
#define MIS_VALUE_MAX     1024
#define MIS_SECTION_MAX   16
#define MIS_PATH_MAX      1024

/* Battle-2 exports this from battle.c: FUN_00484D52's last act is an EncInt set of the local
 * combatant's level (combatant + 0x628 from + 0x64, VA 0x484e0b), four crt_rand(). Declared
 * here so missions.c does not depend on battle.h being extended first. */
void battle_hero_reseal_level(void);

/* Status strings, FUN_00458D1E (VA 0x458d1e): 0x4f12bc, 0x4f12b0, 0x4f12a4, 0x4f129c. */
static const char *const status_names[4] = { "Available", "In Progress", "Complete", "Done" };

typedef struct {
    int job;
    char section[MIS_SECTION_MAX];
} MissionDef;

typedef struct {
    char section[MIS_SECTION_MAX];
    char key[MIS_KEY_MAX];
    char value[MIS_VALUE_MAX];
} LogEntry;

typedef struct {
    int tab;                /* 0 available, 1 open, 2 completed */
    int selected, first;
    int jobs[MISSIONS_MAX], count;
    int detail;
    int confirm;            /* job awaiting an abandon confirmation, -1 when none */
    char message[512];
} Picker;

static struct {
    int loaded;
    MissionDef defs[MISSIONS_MAX];
    int def_count;
    char *ini_text;
    Ini ini;

    char log_path[MIS_PATH_MAX];
    char log_owner[HERO_NAME_MAX];
    LogEntry *log;
    int log_count, log_cap;

    int offer[MISSIONS_MAX];
    int offer_count, offer_armed;

    int panel;
    Picker picker;

    MissionCondFn cond;
    void *cond_user;
    MissionGiveFn give;
    void *give_user;
} missions;

/* --- small helpers ---------------------------------------------------------- */

static int number(const char *s)
{
    return s ? (int)strtol(s, 0, 10) : 0;
}

static int status_clamped(int status)
{
    return (status >= 0 && status < 4) ? status : MISSION_AVAILABLE;
}

/* --- missions.ini ----------------------------------------------------------- */

int missions_load(void)
{
    char path[MIS_PATH_MAX];
    size_t i;
    missions_free();
    if (!world_path(path, sizeof path, "missions.ini")) return -1;
    missions.ini_text = text_read_file(path, 0);
    if (!missions.ini_text) { missions.loaded = 1; return 0; }
    /* GetPrivateProfileString tolerates stray lines; the port's parser does not, so a partial
     * parse still leaves every section it did read usable. */
    (void)ini_parse(&missions.ini, missions.ini_text);
    for (i = 0; i < missions.ini.count; ++i) {
        const char *name = missions.ini.entries[i].section;
        int job, j, seen = 0;
        if (missions.def_count >= MISSIONS_MAX) break;
        for (j = 0; name[j]; ++j)
            if (!isdigit((unsigned char)name[j]) && name[j] != '-' && name[j] != '+') break;
        if (name[j]) continue;                 /* GetPrivateProfileSectionNames + atoi() */
        job = number(name);
        for (j = 0; j < missions.def_count; ++j)
            if (missions.defs[j].job == job) { seen = 1; break; }
        if (seen) continue;
        missions.defs[missions.def_count].job = job;
        snprintf(missions.defs[missions.def_count].section, MIS_SECTION_MAX, "%s", name);
        ++missions.def_count;
    }
    missions.loaded = 1;
    wos_log_event("missions_loaded", "jobs=%d", missions.def_count);
    return 0;
}

/* Releases the missions.ini table and the hero's mission log. The offered-job list and the open
 * picker are runtime UI state, not table state, so they survive a reload (missions_load() calls
 * this, and a scene that runs MISSIONS before anything has read missions.ini must not lose the
 * offer). Use missions_clear_offer()/missions_panel_close() to drop those. */
void missions_free(void)
{
    int offer[MISSIONS_MAX], offer_count = missions.offer_count, offer_armed = missions.offer_armed;
    MissionCondFn cond = missions.cond;
    void *cond_user = missions.cond_user;
    MissionGiveFn give = missions.give;
    void *give_user = missions.give_user;
    memcpy(offer, missions.offer, sizeof offer);
    free(missions.ini_text);
    free(missions.log);
    memset(&missions, 0, sizeof missions);
    memcpy(missions.offer, offer, sizeof missions.offer);
    missions.offer_count = offer_count;
    missions.offer_armed = offer_armed;
    missions.cond = cond;
    missions.cond_user = cond_user;
    missions.give = give;
    missions.give_user = give_user;
    missions.picker.confirm = -1;
}

void missions_set_condition_evaluator(MissionCondFn fn, void *user)
{
    missions.cond = fn;
    missions.cond_user = user;
}

void missions_set_give_handler(MissionGiveFn fn, void *user)
{
    missions.give = fn;
    missions.give_user = user;
}

int missions_count(void) { if (!missions.loaded) (void)missions_load(); return missions.def_count; }

int missions_job_at(int index)
{
    if (!missions.loaded) (void)missions_load();
    return (index >= 0 && index < missions.def_count) ? missions.defs[index].job : -1;
}

static const MissionDef *find_def(int job)
{
    int i;
    if (!missions.loaded) (void)missions_load();
    for (i = 0; i < missions.def_count; ++i)
        if (missions.defs[i].job == job) return &missions.defs[i];
    return 0;
}

int missions_defined(int job) { return find_def(job) != 0; }

const char *missions_field(int job, const char *key)
{
    const MissionDef *def = find_def(job);
    const char *value;
    if (!def) return "";
    value = ini_get(&missions.ini, def->section, key, "");
    return value ? value : "";
}

/* --- the hero's <name>.mis mission log --------------------------------------- */

static void log_path_for(const char *hero_name, char *out, size_t cap)
{
    /* FUN_00460962 (VA 0x460962) builds "<save>\<world>\savedHeroes\" + name + "." + "mis"
     * (directory format string 0x4f1dc4, separator 0x4f1dc0, dot 0x4de650, extension 0x4f1264). */
    snprintf(out, cap, "%s/%s/savedHeroes/%s.mis", game_save_path(), g_world.name, hero_name);
}

static void log_add(const char *section, const char *key, const char *value)
{
    LogEntry *entry;
    int i;
    for (i = 0; i < missions.log_count; ++i)
        if (!text_casecmp(missions.log[i].section, section) &&
            !text_casecmp(missions.log[i].key, key)) {
            snprintf(missions.log[i].value, MIS_VALUE_MAX, "%s", value);
            return;
        }
    if (missions.log_count == missions.log_cap) {
        int cap = missions.log_cap ? missions.log_cap * 2 : 32;
        LogEntry *grown = realloc(missions.log, (size_t)cap * sizeof *grown);
        if (!grown) return;
        missions.log = grown;
        missions.log_cap = cap;
    }
    entry = &missions.log[missions.log_count++];
    snprintf(entry->section, MIS_SECTION_MAX, "%s", section);
    snprintf(entry->key, MIS_KEY_MAX, "%s", key);
    snprintf(entry->value, MIS_VALUE_MAX, "%s", value);
}

static void log_remove_section(const char *section)
{
    int i, n = 0;
    for (i = 0; i < missions.log_count; ++i)
        if (text_casecmp(missions.log[i].section, section))
            missions.log[n++] = missions.log[i];
    missions.log_count = n;
}

/* WritePrivateProfileString layout: CRLF, a blank line before each new section. */
static int log_write(void)
{
    FILE *file;
    char previous[MIS_SECTION_MAX];
    int i;
    previous[0] = 0;
    /* FUN_00460962 (VA 0x460962) creates "<save>", "<save>\<world>" and
     * "<save>\<world>\savedHeroes" before it opens the file. */
    {
        char dir[MIS_PATH_MAX];
        char *slash;
        snprintf(dir, sizeof dir, "%s", missions.log_path);
        for (slash = strchr(dir, '/'); slash; slash = strchr(slash + 1, '/')) {
            *slash = 0;
            if (*dir) (void)plat_mkdir(dir);
            *slash = '/';
        }
        slash = strrchr(dir, '/');
        if (slash) { *slash = 0; if (*dir) (void)plat_mkdir(dir); }
    }
    file = plat_fopen(missions.log_path, "wb");
    if (!file) return -1;
    for (i = 0; i < missions.log_count; ++i) {
        if (strcmp(previous, missions.log[i].section)) {
            if (i) fputs("\r\n", file);
            fprintf(file, "[%s]\r\n", missions.log[i].section);
            snprintf(previous, MIS_SECTION_MAX, "%s", missions.log[i].section);
        }
        fprintf(file, "%s=%s\r\n", missions.log[i].key, missions.log[i].value);
    }
    return fclose(file) ? -1 : 0;
}

static void log_load(void)
{
    char *text;
    Ini parsed;
    size_t i;
    free(missions.log);
    missions.log = 0;
    missions.log_count = missions.log_cap = 0;
    text = text_read_file(missions.log_path, 0);
    if (!text) return;
    if (ini_parse(&parsed, text) == 0 || parsed.count) {
        for (i = 0; i < parsed.count; ++i)
            log_add(parsed.entries[i].section, parsed.entries[i].key, parsed.entries[i].value);
    }
    free(text);
}

static void log_sync(void)
{
    char path[MIS_PATH_MAX];
    if (!g_hero.name[0]) return;
    log_path_for(g_hero.name, path, sizeof path);
    if (!missions.log_path[0] || strcmp(path, missions.log_path)) {
        snprintf(missions.log_path, sizeof missions.log_path, "%s", path);
        log_load();
    }
}

void mission_reload(void)
{
    missions.log_path[0] = 0;
    log_sync();
}

static const char *log_get(const char *section, const char *key)
{
    int i;
    log_sync();
    for (i = 0; i < missions.log_count; ++i)
        if (!text_casecmp(missions.log[i].section, section) &&
            !text_casecmp(missions.log[i].key, key))
            return missions.log[i].value;
    return 0;
}

int mission_status(int job)
{
    char section[MIS_SECTION_MAX];
    const char *value;
    snprintf(section, sizeof section, "%d", job);
    value = log_get(section, "Status");
    return value ? status_clamped(number(value)) : MISSION_AVAILABLE;
}

void mission_set_status(int job, int status)
{
    char section[MIS_SECTION_MAX], value[16];
    snprintf(section, sizeof section, "%d", job);
    log_sync();
    snprintf(value, sizeof value, "%d", status);
    log_add(section, "Status", value);
    if (log_write()) wos_log_event("mission_log_error", "job=%d", job);
    wos_log_event("mission_status", "job=%d status=%d name=%s", job, status, missions_field(job, "Name"));
}

void mission_clear(int job)
{
    char section[MIS_SECTION_MAX];
    snprintf(section, sizeof section, "%d", job);
    log_sync();
    log_remove_section(section);
    if (log_write()) wos_log_event("mission_log_error", "job=%d", job);
}

/* --- the IF conditions (FUN_004851F1 case 0x4A) ----------------------------- */

int mission_completed(int job) { return mission_status(job) == MISSION_DONE; }

int mission_accepted(int job)
{
    int status = mission_status(job);
    return status == MISSION_PROGRESS || status == MISSION_COMPLETE;
}

int mission_qualified(int job)
{
    char qualify[MIS_VALUE_MAX];
    const char *text;
    if (mission_completed(job)) return 0;
    snprintf(qualify, sizeof qualify, "%s", missions_field(job, "Qualify"));
    text = text_trim(qualify);
    if (!*text) return 1;                       /* no Qualify: always qualified */
    return missions.cond ? missions.cond(text, missions.cond_user) != 0 : 1;
}

/* Trophies="5x1,10x16": the count is before the 'x', the trophy id after (FUN_004587C3). */
static int parse_trophies(int job, int *ids, int *counts, int max)
{
    char buf[MIS_VALUE_MAX], *cursor = buf, *token;
    int n = 0;
    snprintf(buf, sizeof buf, "%s", missions_field(job, "Trophies"));
    while ((token = text_next_field(&cursor)) != 0) {
        char *x = strchr(token, 'x');
        int count, id;
        if (!x || n >= max) continue;
        *x = 0;
        count = abs(number(token));
        id = abs(number(x + 1));
        if (count <= 0) continue;
        ids[n] = id;
        counts[n] = count;
        ++n;
    }
    return n;
}

int mission_trophies_met(int job)
{
    int ids[MISSIONS_TROPHIES], counts[MISSIONS_TROPHIES];
    int n = parse_trophies(job, ids, counts, MISSIONS_TROPHIES);
    int i;
    for (i = 0; i < n; ++i)
        if (trophy_bag_count(ids[i]) < counts[i]) return 0;
    return 1;
}

/* --- the MISSION / MISSIONS opcode (0x4A) ----------------------------------- */

void missions_offer(const int *jobs, int count)
{
    int i;
    missions.offer_count = count > MISSIONS_MAX ? MISSIONS_MAX : (count > 0 ? count : 0);
    for (i = 0; i < missions.offer_count; ++i) missions.offer[i] = jobs[i];
    missions.offer_armed = 1;
    wos_log_event("missions_offer", "count=%d jobs=%d,%d,%d,%d", missions.offer_count,
                  missions.offer_count > 0 ? missions.offer[0] : -1,
                  missions.offer_count > 1 ? missions.offer[1] : -1,
                  missions.offer_count > 2 ? missions.offer[2] : -1,
                  missions.offer_count > 3 ? missions.offer[3] : -1);
}

int missions_offered(void) { return missions.offer_armed; }

const int *missions_offer_list(int *count)
{
    if (count) *count = missions.offer_count;
    return missions.offer;
}

void missions_clear_offer(void)
{
    missions.offer_count = 0;
    missions.offer_armed = 0;
}

/* --- accepting, abandoning and paying out ----------------------------------- */

static void give(const char *spec, int count)
{
    if (!missions.give || !spec || !*spec) return;
    missions.give(spec, count, missions.give_user);
}

/* The "You receive: '<name>'" texts name the object (FUN_004847CD, the %I/%S resolver). */
static void object_name(const char *spec, char *out, size_t cap)
{
    long id;
    char *end;
    out[0] = 0;
    if (!spec || !*spec) return;
    id = strtol(spec + 1, &end, 10);
    switch (spec[0]) {
    case 'I': if (id > 0 && id < WORLD_MAX_ITEMS) snprintf(out, cap, "%s", g_world.items[id].name); break;
    case 'S': if (id > 0 && id < WORLD_MAX_SPELLS) snprintf(out, cap, "%s", g_world.spells[id].name); break;
    case 'T': snprintf(out, cap, "token %ld", id); break;
    default: break;
    }
}

static void say(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vsnprintf(missions.picker.message, sizeof missions.picker.message, fmt, args);
    va_end(args);
    wos_log_event("mission_msg", "text=%s", missions.picker.message);
}

/* FUN_0045A19B, VA 0x45a19b: Status = 1, then the accept give/take, then AcceptMsg. */
static void accept(int job)
{
    char name[MIS_VALUE_MAX];
    const char *spec;
    if (!missions_defined(job)) return;
    mission_set_status(job, MISSION_PROGRESS);
    spec = missions_field(job, "AcceptGive");
    if (*spec) {
        give(spec, 1);
        object_name(spec, name, sizeof name);
        if (name[0] && spec[0] != 'T') say("You receive: '%s' for accepting mission.", name);
    }
    spec = missions_field(job, "AcceptTake");
    if (*spec) {
        give(spec, -1);
        object_name(spec, name, sizeof name);
        if (name[0] && spec[0] != 'T') say("You lose: '%s' for accepting mission.", name);
    }
    spec = missions_field(job, "AcceptMsg");
    if (*spec) say("%s", spec);
    wos_log_event("mission_accept", "job=%d name=%s", job, missions_field(job, "Name"));
}

/* FUN_0045A31D, VA 0x45a31d: drop the section, run the abandon give/take, then AbandonMsg. */
static void abandon(int job)
{
    char name[MIS_VALUE_MAX];
    const char *spec;
    mission_clear(job);
    spec = missions_field(job, "AbandonGive");
    if (*spec) {
        give(spec, 1);
        object_name(spec, name, sizeof name);
        if (spec[0] != 'T') say("You receive: '%s' for abandoning mission.", name);
    }
    spec = missions_field(job, "AbandonTake");
    if (*spec) {
        give(spec, -1);
        object_name(spec, name, sizeof name);
        if (spec[0] != 'T') say("You lose: '%s' for abandoning mission.", name);
    }
    spec = missions_field(job, "AbandonMsg");
    if (*spec) say("%s", spec);
    wos_log_event("mission_abandon", "job=%d", job);
}

/* FUN_0045A5C9, VA 0x45a5c9. `RewardLevel` goes through atof()+ftol(), so "3.2" is level 3 in
 * the hero's own class (the .ini comment reads it as "class 3 level 2"; the code ignores the
 * fraction). The award is capped at the XP still owed to reach (level + 2) * 10. */
static void reward(int job)
{
    int ids[MISSIONS_TROPHIES], counts[MISSIONS_TROPHIES];
    char spec[64], name[MIS_VALUE_MAX];
    int n, i, level, gold, pp, wp, token;
    const char *text, *foot;
    n = parse_trophies(job, ids, counts, MISSIONS_TROPHIES);
    for (i = 0; i < n; ++i) (void)trophy_bag_take(ids[i], -counts[i]);
    gold = number(missions_field(job, "RewardGold"));
    if (gold > 0) {
        (void)hero_add_gold(&g_hero, gold);
        say("You receive %d %s", gold, world_gold_name());
    }
    text = missions_field(job, "RewardGive");
    if (*text) {
        give(text, 1);
        if (text[0] != 'T') { object_name(text, name, sizeof name); say("You receive: %s", name); }
    }
    text = missions_field(job, "RewardTake");
    if (*text) {
        give(text, -1);
        if (text[0] != 'T') { object_name(text, name, sizeof name); say("You lose: %s", name); }
    }
    token = number(missions_field(job, "RewardToken"));
    if (token != 0) {
        snprintf(spec, sizeof spec, "T%d", abs(token));
        give(spec, token > 0 ? 1 : -1);
    }
    text = missions_field(job, "RewardLevel");
    if (*text) {
        level = (int)atof(text);
        if (level > 0 && level < 1000) {
            int64_t want = hero_xp_for_level(&g_hero, level);
            int64_t cap = hero_xp_for_level(&g_hero, (g_hero.level + 2) * 10) - g_hero.xp;
            if (cap < want) want = cap;
            if (want > 0 && want < 0xf3b9b) {
                (void)hero_award(&g_hero, want, 0);
                /* FUN_00484D52's last act, VA 0x484e0b: re-seal the local combatant's level. */
                battle_hero_reseal_level();
            }
        }
    }
    pp = number(missions_field(job, "RewardPP"));
    if (pp > 0 && pp < 100000) (void)hero_add_pp(&g_hero, pp);
    wp = number(missions_field(job, "RewardWP"));
    (void)wp;  /* FUN_00401460 is war points: online-only, no offline effect. */
    /* The original logs the mission NAME here (FUN_00458CF0 reads the "Name" key, not FootNote). */
    foot = missions_field(job, "Name");
    if (*foot) say("You have been rewarded for completing mission: %s", foot);
    mission_set_status(job, MISSION_DONE);
    wos_log_event("mission_reward", "job=%d name=%s", job, missions_field(job, "Name"));
}

/* FUN_0045A5C9 again: the "Complete" action. */
static void turn_in(int job)
{
    int status;
    if (mission_qualified(job) && mission_trophies_met(job)) { reward(job); return; }
    status = mission_status(job);
    if (status < MISSION_COMPLETE) return;
    mission_clear(job);   /* FUN_00458C63(job, NULL, NULL): the whole section goes */
    wos_log_event("mission_discarded", "job=%d", job);
}

/* --- the picker (dialog resource 0xED) -------------------------------------- */

/* Control ids recovered from the dialog template of resource 237 (0xED): 1 IDOK, 2 IDCANCEL,
 * 1300 the corner X, 1302 the tab control, 1303 SysListView32 "List1", 1304/1305 the two fixed
 * footer buttons. 1306 is port-assigned for the third tab; the abandon confirm is a MessageBox
 * in the original (FUN_00458343, MB_YESNO), so it uses the Win32 ids IDYES 6 / IDNO 7. */
enum {
    CTL_YES = 6, CTL_NO = 7, CTL_OK = 1, CTL_CANCEL = 2, CTL_X = 1300,
    CTL_LIST = 1303, CTL_TAB_AVAILABLE = 1304, CTL_TAB_OPEN = 1305, CTL_TAB_DONE = 1306
};

static const Rect panel_rect  = { 92,  40, 456, 400};
static const Rect x_rect      = {512,  44,  32,  20};
static const Rect tab_buttons[3] = {{100, 64, 140, 20}, {244, 64, 140, 20}, {388, 64, 152, 20}};
static const Rect list_rect   = {100,  90, 208, 268};
static const Rect detail_rect = {316,  90, 224, 150};
static const Rect trophy_rect = {316, 246, 224, 112};
static const Rect cancel_rect = {100, 372,  96,  24};
static const Rect ok_rect     = {312, 372,  96,  24};
static const Rect yes_rect    = {244, 250,  96,  24};
static const Rect no_rect     = {100, 250,  96,  24};

static const char *const tab_titles[3] = { "Available Missions", "Open Missions", "Completed Missions" };

#define LIST_PAGE 12
#define ROW_H 10

static void picker_rebuild(void)
{
    Picker *p = &missions.picker;
    int i, defs = missions_count();   /* lazy loads worlds/<W>/missions.ini */
    p->count = 0;
    for (i = 0; i < defs; ++i) {
        int job = missions.defs[i].job;
        int status = mission_status(job);
        int wanted;
        if (p->tab == 0) {
            int k, offered = 0;
            for (k = 0; k < missions.offer_count; ++k)
                if (missions.offer[k] == job) { offered = 1; break; }
            if (!offered) continue;
            wanted = status == MISSION_AVAILABLE && mission_qualified(job);
        } else if (p->tab == 1) {
            wanted = status == MISSION_PROGRESS || status == MISSION_COMPLETE;
        } else {
            wanted = status == MISSION_DONE;
        }
        if (wanted && p->count < MISSIONS_MAX) p->jobs[p->count++] = job;
    }
    if (p->selected >= p->count) p->selected = p->count - 1;
    if (p->selected < 0) p->selected = 0;
    p->first = p->selected / LIST_PAGE * LIST_PAGE;
    p->detail = p->count ? p->jobs[p->selected] : -1;
    /* FUN_004597F5 (VA 0x4597f5) advances In Progress to Complete as soon as the required
     * trophies are all held; it does this on every selection change. */
    if (p->detail >= 0 && mission_status(p->detail) == MISSION_PROGRESS &&
        mission_trophies_met(p->detail))
        mission_set_status(p->detail, MISSION_COMPLETE);
}

void missions_open_picker(void)
{
    Picker *p = &missions.picker;
    (void)missions_count();   /* the table must be in before the tab choice */
    p->tab = missions.offer_count ? 0 : 1;
    p->selected = p->first = 0;
    p->message[0] = 0;
    p->detail = -1;
    p->confirm = -1;
    picker_rebuild();
    missions.panel = 1;
    wos_log_event("missions_panel", "open=1 tab=%d listed=%d offered=%d",
                  p->tab, p->count, missions.offer_count);
}

int missions_panel_active(void) { return missions.panel; }

void missions_panel_close(void)
{
    if (!missions.panel) return;
    missions.panel = 0;
    missions.picker.confirm = -1;
    missions_clear_offer();
    wos_log_event("missions_panel", "open=0");
}

static int clicked(const Input *in, Rect r)
{
    return (in->mouse_pressed & 2u) && in->mouse_x >= r.x && in->mouse_y >= r.y &&
           in->mouse_x < r.x + r.w && in->mouse_y < r.y + r.h;
}

static const char *action_label(int job)
{
    int status = mission_status(job);
    if (status == MISSION_DONE) return "Close";
    if (status == MISSION_PROGRESS || status == MISSION_COMPLETE) return "Complete";
    return mission_qualified(job) ? "Accept" : "Unavailable";
}

void missions_panel_update(const Input *input)
{
    Picker *p = &missions.picker;
    int i, row, hit = -1, y;
    if (!missions.panel) return;
    if (p->confirm >= 0) {
        /* FUN_00458343, MB_YESNO: only IDYES (6) goes on with the abandon. */
        if (input->pressed[PLAT_KEY_ESCAPE] || input->pressed['n'] || clicked(input, no_rect)) {
            p->confirm = -1;
            wos_log_event("mission_confirm_result", "answer=no");
        } else if (input->pressed[PLAT_KEY_RETURN] || input->pressed['y'] || clicked(input, yes_rect)) {
            int job = p->confirm;
            p->confirm = -1;
            wos_log_event("mission_confirm_result", "answer=yes job=%d", job);
            abandon(job);
            picker_rebuild();
        }
        return;
    }
    if (input->pressed[PLAT_KEY_ESCAPE] || clicked(input, cancel_rect) || clicked(input, x_rect)) {
        missions_panel_close();
        return;
    }
    if (input->pressed[PLAT_KEY_DOWN]) ++p->selected;
    if (input->pressed[PLAT_KEY_UP]) --p->selected;
    if (input->pressed[PLAT_KEY_RIGHT] || input->pressed[PLAT_KEY_TAB]) p->tab = (p->tab + 1) % 3;
    if (input->pressed[PLAT_KEY_LEFT]) p->tab = (p->tab + 2) % 3;
    if (input->pressed['a']) p->tab = 0;
    if (input->pressed['o']) p->tab = 1;
    if (input->pressed['d']) p->tab = 2;
    if (input->wheel) p->selected -= input->wheel;
    for (i = 0; i < 3; ++i) if (clicked(input, tab_buttons[i])) p->tab = i;
    if (p->tab != missions.picker.tab) p->selected = 0;
    missions.picker.tab = p->tab;
    picker_rebuild();
    y = list_rect.y + 2;
    for (row = 0; row < LIST_PAGE && p->first + row < p->count; ++row) {
        if (clicked(input, (Rect){list_rect.x, y - 1, list_rect.w, ROW_H})) { hit = p->first + row; break; }
        y += ROW_H;
    }
    if (hit >= 0) p->selected = hit;
    picker_rebuild();
    if (input->pressed[PLAT_KEY_RETURN] || clicked(input, ok_rect)) {
        if (p->detail < 0) return;
        switch (mission_status(p->detail)) {
        case MISSION_DONE: missions_panel_close(); break;
        case MISSION_PROGRESS:
        case MISSION_COMPLETE: turn_in(p->detail); break;
        default:
            /* FUN_0045A19B keeps the dialog open and refreshes the row (FUN_0045A521): the
             * mission moves to the Open list. */
            if (mission_qualified(p->detail)) { accept(p->detail); p->tab = 1; p->selected = 0; }
            break;
        }
        picker_rebuild();
        return;
    }
    if (input->pressed[PLAT_KEY_BACKSPACE] && p->detail >= 0 && mission_accepted(p->detail)) {
        const char *name = missions_field(p->detail, "Name");
        snprintf(p->message, sizeof p->message,
                 "Do you no longer plan on fulfilling your commitment to complete the mission: %s",
                 *name ? name : "Unknown Mission");
        p->confirm = p->detail;
        wos_log_event("mission_confirm", "job=%d", p->confirm);
    }
}

static void frame(Framebuffer *fb, Rect r, uint32_t color)
{
    fb_fill(fb, r, 0x00f8f8f0u);
    fb_rect(fb, r, color);
}

static void label(Framebuffer *fb, Rect r, const char *text, int selected)
{
    if (selected) fb_fill(fb, r, 0x00675030u);
    fb_rect(fb, r, 0x00404040u);
    font_draw(fb, r.x + 4, r.y + (r.h - FONT_H) / 2, text, 0x00000000u);
}

void missions_panel_render(Framebuffer *fb)
{
    Rect clip = fb->clip;
    Picker *p = &missions.picker;
    int i, y, row;
    if (!missions.panel) return;
    fb_reset_clip(fb);
    fb_fill(fb, (Rect){0, 0, PLAT_SCREEN_W, PLAT_SCREEN_H}, 0x00080808u);
    fb_fill(fb, panel_rect, 0x00f0e6d0u);
    fb_rect(fb, panel_rect, 0x00604020u);
    font_draw(fb, panel_rect.x + 8, panel_rect.y + 3, "Book of Missions", 0x00202020u);
    fb_fill(fb, x_rect, 0x00a03020u);
    font_draw(fb, x_rect.x + 12, x_rect.y + 6, "X", 0x00ffffffu);
    for (i = 0; i < 3; ++i) label(fb, tab_buttons[i], tab_titles[i], p->tab == i);
    frame(fb, list_rect, 0x00404040u);
    frame(fb, detail_rect, 0x00404040u);
    frame(fb, trophy_rect, 0x00404040u);
    fb_clip(fb, list_rect);
    y = list_rect.y + 3;
    for (row = 0; row < LIST_PAGE && p->first + row < p->count; ++row) {
        int job = p->jobs[p->first + row];
        char text[128];
        snprintf(text, sizeof text, "%s", missions_field(job, "Name"));
        if (p->first + row == p->selected)
            fb_fill(fb, (Rect){list_rect.x + 1, y - 2, list_rect.w - 2, ROW_H}, 0x0080b0e0u);
        font_draw(fb, list_rect.x + 3, y, text, 0x00202020u);
        font_draw(fb, list_rect.x + list_rect.w - 68, y, status_names[status_clamped(mission_status(job))], 0x00404040u);
        y += ROW_H;
    }
    if (!p->count) font_draw(fb, list_rect.x + 6, list_rect.y + 6, "No missions", 0x00404040u);
    fb_clip(fb, detail_rect);
    if (p->detail >= 0) {
        /* The original's detail text (FUN_0049D3AF chain, VA 0x459d3a): name, description,
         * the "all expectations" line once Complete, and the FootNote once Done. */
        int status = status_clamped(mission_status(p->detail));
        const char *name = missions_field(p->detail, "Name");
        const char *foot = missions_field(p->detail, "FootNote");
        Rect r = {detail_rect.x + 4, detail_rect.y + 4, detail_rect.w - 8, detail_rect.h - 8};
        y = r.y + font_wrap(fb, r, name, 0x00202020u) + 2;
        r.y = y;
        y += font_wrap(fb, r, missions_field(p->detail, "Desc"), 0x00202020u) + 2;
        r.y = y;
        if (status == MISSION_COMPLETE) {
            y += font_wrap(fb, r, "You have met all expectations, you may claim your reward.",
                           0x00404040u) + 2;
            r.y = y;
        }
        if (status == MISSION_DONE && *foot) {
            char text[MIS_VALUE_MAX];
            snprintf(text, sizeof text, "Footnote: %s", foot);
            r.y = y;
            (void)font_wrap(fb, r, text, 0x00404040u);
        }
    } else {
        font_draw(fb, detail_rect.x + 6, detail_rect.y + 6, "No Mission Selected", 0x00404040u);
    }
    fb_clip(fb, trophy_rect);
    if (p->detail >= 0) {
        int ids[MISSIONS_TROPHIES], counts[MISSIONS_TROPHIES];
        int n = parse_trophies(p->detail, ids, counts, MISSIONS_TROPHIES);
        font_draw(fb, trophy_rect.x + 4, trophy_rect.y + 4, "Trophy Bag", 0x00404040u);
        y = trophy_rect.y + 16;
        for (i = 0; i < n && y < trophy_rect.y + trophy_rect.h - 8; ++i) {
            char text[128];
            snprintf(text, sizeof text, "%d/%d %s", trophy_bag_count(ids[i]), counts[i],
                     (ids[i] > 0 && ids[i] < WORLD_MAX_TROPHIES) ? g_world.trophies[ids[i]].name : "");
            font_draw(fb, trophy_rect.x + 6, y, text, 0x00202020u);
            y += ROW_H;
        }
    }
    if (p->confirm >= 0) {
        fb_fill(fb, (Rect){100, 190, 440, 110}, 0x00f0e6d0u);
        fb_rect(fb, (Rect){100, 190, 440, 110}, 0x00604020u);
        font_draw(fb, 108, 198, "Abandon Mission?", 0x00202020u);
        font_wrap(fb, (Rect){108, 212, 424, 32}, p->message, 0x00202020u);
        label(fb, no_rect, "No", 0);
        label(fb, yes_rect, "Yes", 1);
    }
    fb_clip(fb, (Rect){panel_rect.x, panel_rect.y + 340, panel_rect.w, 24});
    if (p->message[0] && p->confirm < 0)
        font_draw(fb, panel_rect.x + 8, panel_rect.y + 346, p->message, 0x00202020u);
    fb_reset_clip(fb);
    label(fb, cancel_rect, "Cancel", 0);
    label(fb, ok_rect, p->detail >= 0 ? action_label(p->detail) : "Close", 1);
    fb->clip = clip;
}

/* --- dump ------------------------------------------------------------------- */

void missions_dump(DumpEmit emit, void *user)
{
    char key[48], value[256];
    int i, n = missions_count();
    snprintf(value, sizeof value, "%d", n);            emit("missions.defined", value, user);
    snprintf(value, sizeof value, "%d", missions.offer_count); emit("missions.offered", value, user);
    snprintf(value, sizeof value, "%d", missions.offer_armed); emit("missions.button", value, user);
    snprintf(value, sizeof value, "%d", missions.panel);      emit("missions.panel", value, user);
    if (missions.panel) {
        snprintf(value, sizeof value, "%d", missions.picker.tab);      emit("missions.tab", value, user);
        snprintf(value, sizeof value, "%d", missions.picker.selected); emit("missions.selection", value, user);
        snprintf(value, sizeof value, "%d", missions.picker.count);    emit("missions.listed", value, user);
        snprintf(value, sizeof value, "%d", missions.picker.detail);   emit("missions.detail", value, user);
    }
    for (i = 0; i < n && i < MISSIONS_MAX; ++i) {
        int job = missions.defs[i].job;
        snprintf(key, sizeof key, "mission.%d.status", job);
        snprintf(value, sizeof value, "%d", mission_status(job));
        emit(key, value, user);
        snprintf(key, sizeof key, "mission.%d.name", job);
        emit(key, missions_field(job, "Name"), user);
    }
}
