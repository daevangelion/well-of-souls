#include "game_main.h"
#include "platform/platform.h"
#include "engine/boot.h"
#include "engine/clock.h"
#include "engine/dscript.h"
#include "engine/dump.h"
#include "engine/fb.h"
#include "engine/log.h"
#include "engine/replay.h"
#include "engine/rng.h"
#include "engine/screen.h"
#include "engine/text.h"
#include "engine/ui.h"
#include <errno.h>

/* The `dialog` script op. Panels-2 owns the implementation and the control-id
 * table; this is the port's call site. The declaration is weak so the port
 * links (and the .rpl tests run) before panels.c has converted, and so a build
 * that drops panels entirely still links. DialogOp must stay field-for-field
 * identical to panels.h's. */
typedef struct { int id; int ctrl; int value; int ok; } DialogOp;
__attribute__((weak)) int panel_dialog_op(const DialogOp *op);

/* SceneVM-2 owns the quest VM's ms-driven TIMER/COUNTDOWN and is removing the
 * old per-frame entry point. The weak declaration bridges the migration: the
 * call disappears with the symbol. */
__attribute__((weak)) void scene_tick(void);


__attribute__((weak)) int front_dialog_op(int dialog_id, const char *const *kv, int n, int ok);
#include "game/options.h"
#include "game/front.h"
#include "game/sched.h"
#include "game/scene.h"
static void scene_tick_if_any(void) { if (scene_tick) scene_tick(); }
#include <errno.h>
#include <stdlib.h>
#include <string.h>

static const char *data_path;
static const char *save_path;
static char default_save_path[4096];
static int quitting;

const char *game_data_path(void) { return data_path; }
const char *game_save_path(void) { return save_path; }
void game_request_quit(void) { quitting = 1; }

/* --- module state dumps -----------------------------------------------------
 * One entry per module. A module that has not converted yet has no entry and is
 * simply skipped, so the build and the .rpl tests keep working during the
 * migration. `label` is what `at <ms> dump <label>` names; the key registry
 * itself is owned by the Oracle (docs/re/oracle.md) and mirrored here. */
typedef void (*DumpFn)(DumpEmit, void *);
#define WOS_DUMP(n) extern void n##_dump(DumpEmit, void *);
WOS_DUMP(hero) WOS_DUMP(map) WOS_DUMP(scene) WOS_DUMP(battle)
WOS_DUMP(panels) WOS_DUMP(items) WOS_DUMP(missions) WOS_DUMP(minigame)
WOS_DUMP(options) WOS_DUMP(world) WOS_DUMP(front) WOS_DUMP(chat)
WOS_DUMP(editors)
WOS_DUMP(html)
WOS_DUMP(pet)
WOS_DUMP(minigame_asteroids)
WOS_DUMP(minigame_blackjack)
WOS_DUMP(minigame_pi)
WOS_DUMP(minigame_racer)
WOS_DUMP(minigame_slots)
WOS_DUMP(minigame_stocks)
WOS_DUMP(minigame_tetris)
WOS_DUMP(minigame_train)
#undef WOS_DUMP
#define WOS_WEAK __attribute__((weak))
WOS_WEAK void hero_dump(DumpEmit e, void *u) { (void)e; (void)u; }
WOS_WEAK void map_dump(DumpEmit e, void *u) { (void)e; (void)u; }
WOS_WEAK void scene_dump(DumpEmit e, void *u) { (void)e; (void)u; }
WOS_WEAK void battle_dump(DumpEmit e, void *u) { (void)e; (void)u; }
WOS_WEAK void panels_dump(DumpEmit e, void *u) { (void)e; (void)u; }
WOS_WEAK void items_dump(DumpEmit e, void *u) { (void)e; (void)u; }
WOS_WEAK void missions_dump(DumpEmit e, void *u) { (void)e; (void)u; }
WOS_WEAK void minigame_dump(DumpEmit e, void *u) { (void)e; (void)u; }
WOS_WEAK void options_dump(DumpEmit e, void *u) { (void)e; (void)u; }
WOS_WEAK void world_dump(DumpEmit e, void *u) { (void)e; (void)u; }
WOS_WEAK void front_dump(DumpEmit e, void *u) { (void)e; (void)u; }
WOS_WEAK void chat_dump(DumpEmit e, void *u) { (void)e; (void)u; }
WOS_WEAK void editors_dump(DumpEmit e, void *u) { (void)e; (void)u; }
WOS_WEAK void html_dump(DumpEmit e, void *u) { (void)e; (void)u; }
WOS_WEAK void pet_dump(DumpEmit e, void *u) { (void)e; (void)u; }
WOS_WEAK void minigame_asteroids_dump(DumpEmit e, void *u) { (void)e; (void)u; }
WOS_WEAK void minigame_blackjack_dump(DumpEmit e, void *u) { (void)e; (void)u; }
WOS_WEAK void minigame_pi_dump(DumpEmit e, void *u) { (void)e; (void)u; }
WOS_WEAK void minigame_racer_dump(DumpEmit e, void *u) { (void)e; (void)u; }
WOS_WEAK void minigame_slots_dump(DumpEmit e, void *u) { (void)e; (void)u; }
WOS_WEAK void minigame_stocks_dump(DumpEmit e, void *u) { (void)e; (void)u; }
WOS_WEAK void minigame_tetris_dump(DumpEmit e, void *u) { (void)e; (void)u; }
WOS_WEAK void minigame_train_dump(DumpEmit e, void *u) { (void)e; (void)u; }

typedef struct { const char *label; DumpFn fn; } DumpEntry;
static const DumpEntry dump_table[] = {
    { "clock",  0 },
    { "rng",    0 },
    { "hero",   hero_dump },
    { "map",    map_dump },
    { "scene",  scene_dump },
    { "battle", battle_dump },
    { "panels", panels_dump },
    { "items",  items_dump },
    { "missions", missions_dump },
    { "minigame", minigame_dump },
    { "options", options_dump },
    { "world",   world_dump },
    { "front", front_dump },
    { "chat", chat_dump },
    { "editors", editors_dump },
    { "html", html_dump },
    { "pet", pet_dump },
    { "minigame_asteroids", minigame_asteroids_dump },
    { "minigame_blackjack", minigame_blackjack_dump },
    { "minigame_pi", minigame_pi_dump },
    { "minigame_racer", minigame_racer_dump },
    { "minigame_slots", minigame_slots_dump },
    { "minigame_stocks", minigame_stocks_dump },
    { "minigame_tetris", minigame_tetris_dump },
    { "minigame_train", minigame_train_dump },
};
#define DUMP_ENTRIES ((int)(sizeof(dump_table) / sizeof(dump_table[0])))

static void dump_builtin(DumpEmit emit, void *user)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)clock_ms());
    emit("clock.ms", buf, user);
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)clock_time_s());
    emit("clock.time_s", buf, user);
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)crt_rand_state());
    emit("rng.state", buf, user);
    snprintf(buf, sizeof(buf), "%llu", (unsigned long long)crt_rand_calls());
    emit("rng.calls", buf, user);
    snprintf(buf, sizeof(buf), "%llu", (unsigned long long)crt_srand_calls());
    emit("rng.srand_calls", buf, user);
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)crt_boot_base_offset());
    emit("hero.base_offset", buf, user);
}

static void dump_emit_file(const char *key, const char *value, void *user)
{
    fprintf((FILE *)user, "%s=%s\n", key, value);
}

static int run_dumps(const char *path)
{
    FILE *file;
    int i, failed = 0;
    if (!path) return 0;
    file = plat_fopen(path, "wb");
    if (!file) {
        wos_log_event("dump_open_failed", "path=%s", path);
        return -1;
    }
    for (i = 0; i < DUMP_ENTRIES; ++i) {
        const char *label = dump_table[i].label;
        dump_emit_file("label", label, file);
        if (!strcmp(label, "clock") || !strcmp(label, "rng")) dump_builtin(dump_emit_file, file);
        else if (dump_table[i].fn) dump_table[i].fn(dump_emit_file, file);
        else failed = 1;
    }
    if (fclose(file)) failed = 1;
    wos_log_event("dump_written", "path=%s", path);
    return failed;
}

/* `dump <module>` or the tagged `dump <module>@<tag>` (dscript.c validates the tag).
 * The split is at the LAST '@'. The label emitted is exactly "<module>@<tag>" -- one
 * string, because diff_oracle.sh matches labels by file name and the tag is what names
 * the file. The tag does NOT namespace the keys inside: those stay <module>.*. A plain
 * `dump <module>` still emits label=<module>, so this is one convention, not two. */
static void dump_one(const char *spec)
{
    char label[64];
    const char *at = spec ? strrchr(spec, '@') : NULL;
    size_t n = at ? (size_t)(at - spec) : (spec ? strlen(spec) : 0);
    int i;
    if (!spec || !n || n >= sizeof(label)) { wos_log_event("dump_unknown", "label=%s", spec ? spec : ""); return; }
    memcpy(label, spec, n);
    label[n] = 0;
    for (i = 0; i < DUMP_ENTRIES; ++i) {
        if (strcmp(dump_table[i].label, label)) continue;
        wos_log_event("dump", "label=%s", spec);
        if (!strcmp(label, "clock") || !strcmp(label, "rng")) dump_builtin(log_emit, NULL);
        else if (dump_table[i].fn) dump_table[i].fn(log_emit, NULL);
        return;
    }
    wos_log_event("dump_unknown", "label=%s", spec);
}

static int unsigned_arg(const char *s, uint32_t *out)
{
    char *end; unsigned long n;
    if (!*s || *s == '-') return -1;
    errno = 0; n = strtoul(s, &end, 10);
    if (errno || *end || n > UINT32_MAX) return -1;
    *out = (uint32_t)n; return 0;
}

static uint64_t seen(const char *name, uint64_t since, void *user)
{ (void)user; return wos_log_seen_since(name, since); }

static void bmp_u32(unsigned char *p, uint32_t n)
{ p[0]=(unsigned char)n; p[1]=(unsigned char)(n>>8); p[2]=(unsigned char)(n>>16); p[3]=(unsigned char)(n>>24); }

static int save_shot(const Framebuffer *fb, const char *path)
{
    unsigned char header[54]={0}, row[PLAT_SCREEN_W*3+3];
    size_t stride; int x,y,failed=0; FILE *file;
    if(fb->w<=0 || fb->w>PLAT_SCREEN_W || fb->h<=0 || fb->h>PLAT_SCREEN_H) return -1;
    stride=((size_t)fb->w*3+3)&~(size_t)3;
    header[0]='B'; header[1]='M'; bmp_u32(header+2,(uint32_t)(54+stride*fb->h));
    bmp_u32(header+10,54); bmp_u32(header+14,40);
    bmp_u32(header+18,(uint32_t)fb->w); bmp_u32(header+22,(uint32_t)fb->h);
    header[26]=1; header[28]=24; bmp_u32(header+34,(uint32_t)(stride*fb->h));
    file=plat_fopen(path,"wb");
    if(!file) return -1;
    if(fwrite(header,1,sizeof(header),file)!=sizeof(header)) failed=1;
    memset(row,0,stride);
    for(y=fb->h-1;y>=0 && !failed;--y) {
        for(x=0;x<fb->w;++x) {
            uint32_t c=fb->pixels[(size_t)y*fb->w+x];
            row[x*3]=(unsigned char)c; row[x*3+1]=(unsigned char)(c>>8); row[x*3+2]=(unsigned char)(c>>16);
        }
        if(fwrite(row,1,stride,file)!=stride) failed=1;
    }
    if(fclose(file)) failed=1;
    if(failed) return -1;
    wos_log_event("shot","path=%s",path); return 0;
}

/* --dump-shots DIR: at every `dump <label>` also save DIR/<label>.bmp, as the oracle hook
 * does (do_dump writes the text, then repaints and captures the client). The repaint is
 * the screen's own render, so a shot shows what the player would see at that label. */
static const char *g_dump_shot_dir;
static Framebuffer *g_dump_shot_fb;
static void dump_shot(const char *label)
{
    char path[4096]; const Screen *screen = screen_current(); int n;
    if (!g_dump_shot_dir || !g_dump_shot_fb) return;
    fb_reset_clip(g_dump_shot_fb); fb_clear(g_dump_shot_fb, 0);
    if (screen && screen->render) screen->render(g_dump_shot_fb);
    n = snprintf(path, sizeof(path), "%s/%s.bmp", g_dump_shot_dir, label);
    if (n > 0 && (size_t)n < sizeof(path)) (void)save_shot(g_dump_shot_fb, path);
}

static void usage(void)
{
    fputs("Usage: wos --data DIR [--save DIR] [--headless] [--replay FILE] [--script FILE]\n"
          "          [--log FILE] [--dump FILE] [--time EPOCH_S] [--max-frames N]\n"
          "          [--seed N] [--shot-every N DIR] [--dump-shots DIR]\n", stderr);
}

/* --- seeding ----------------------------------------------------------------
 * The original seeds the CRT twice inside FUN_004269AF (0x004269AF), the
 * CWinApp-derived global-constructor class, at all.c:27953 and all.c:27987:
 *     tVar3 = time(NULL); srand(tVar3);   rand();   <- all.c:27953
 *     ... FUN_00427D89(); ...                       (34 lines later)
 *     tVar3 = time(NULL); srand(tVar3);              <- all.c:27987
 * The second seed wins, but the single rand() between them is a real consumer,
 * so the port does exactly: seed, draw once, seed again. --seed pins the
 * `time()` value so a replay reproduces; otherwise it is the virtual time(). */

/* The 20 Hz idle work of FUN_0040A7C7 (0x0040A7C7). Its four callees are the
 * SRNet perf graph FUN_0042895C, a heap probe FUN_00416CD3, a sound poke
 * FUN_00401F8D and the Lag-O-Meter accumulate FUN_0047397B. None is a solo-play
 * rule, and FUN_0042895C's discarded rand() is SRNet-only (docs/re/rng_calls.md
 * section 2.1), so solo play consumes no RNG on the 20 Hz path. */
/* The main frame's id-0x16 100 ms WM_TIMER, FUN_00428360 at 0x00428803 (slot this+0x114),
 * whose handler is FUN_00428C8F at 0x00428C8F:
 *
 *     void __fastcall FUN_00428c8f(CWnd *self) {
 *         if ((DAT_004e48a0 == 0) && (DAT_004e709c == 0)) {
 *             DAT_004e48a0 = 1;          // re-entrancy guard
 *             FUN_0040a7c7();            // the 20 ms gate
 *             DAT_004e48a0 = 0;
 *         }
 *         CWnd::Default(self);
 *     }
 *
 * So the world tick is driven by THIS TIMER, not by the message queue emptying. That
 * matters: the queue is rarely empty on a repainting screen, yet the original still
 * ticks, because 0x00428CAF calls the gate directly. FUN_0040A7C7 has exactly two
 * callers in the whole binary: 0x0040A90F in the Run loop's idle path, and this one.
 * Inside the gate, 0x0040A810 writes _DAT_004dd510 and 0x0040A815 calls FUN_0042895C,
 * the 25 ms sub-gate that draws one rand and runs the world via FUN_0041BDB4.
 *
 * FUN_0042895C has exactly ONE caller, 0x0040A815, so the 13 observed draws prove the
 * gate ran 13 times and therefore that _DAT_004dd510 was written 13 times. A hook
 * reading it as 0 throughout is seeing its own virtualised GetTickCount return 0, not
 * a missing tick. */
#define MAIN_FRAME_TIMER_ID 0x16   /* 22 */
static int frame_timer_owner;
static int world_tick_pending;
/* The 20 Hz tick and the 25 ms world tick, i.e. the real shape of
 * FUN_0040A7C7 (0x0040A7C7) -> FUN_0042895C (0x0042895C) -> FUN_0041BDB4
 * (0x0041BDB4). FUN_0042895C burns ONE discarded rand() on every call, before
 * its 25 ms sub-gate, so the port must consume one crt_rand() per 20 ms tick or
 * the whole stream shifts (docs/re/rng_calls.md 2.1, all.c:30576).
 *
 * It then gates on `GetTickCount() - _DAT_004e48cc < 0x19` (all.c:30578) and,
 * only if `DAT_004e483c` is a live window, runs the entire game world:
 * FUN_0041BDB4. The world is therefore 40 Hz, not 60 Hz and not 20 Hz. */
#define WORLD_GATE_OWNER ((void *)&idle_ticks)
static unsigned idle_ticks;

static int world_tick_due(void);

/* FUN_00428C8F: the 100 ms WM_TIMER is what enters the gate, so the handler is where
 * the world's rand and 25 ms step live. It only DECIDES -- the loop performs the step,
 * so update/render/present stay in one place. */
/* FUN_0040A7C7's 20 ms gate and what it calls first, FUN_0042895C: one discarded rand(),
 * then the 25 ms world sub-gate. AppRun (0x0040A8D9) enters it on every idle pass while the
 * game is focused, and FUN_00428C8F enters it from the 100 ms timer; the gate's own stamp
 * makes both the same 20 ms cadence. Measured on the oracle once its pump stopped
 * stepping the clock mid-drain: one 0x428996 draw on every 20 ms boundary from t=150. */
static int idle_loop_live;   /* AppRun's loop is running: from the first frame-timer delivery */
static void idle_gate(void)
{
    if (!clock_idle_due()) return;
    (void)crt_rand();                       /* FUN_0042895C's discarded draw */
    (void)world_tick_due();
}

static void main_frame_timer(void *owner, void *user)
{
    (void)owner; (void)user;
    idle_loop_live = 1;
    idle_gate();                            /* FUN_00428C8F -> FUN_0040A7C7 */
}

int g_pet_pen_up = 1;
static int world_step_now = 1;
int game_world_step(void) { return world_step_now; }
#define OPTION_CUT_DIALOG_ANIMATIONS 10  /* "My computer is slow, cut animations during dialogs." */
static unsigned half_rate_count;         /* DAT_00559628 */

static int world_tick_due(void)
{
    if (!clock_gate_restamp(WORLD_GATE_OWNER, 0, 25u)) return 0;
    /* NetGraphTick 0x428B1D: with a dialog up (DAT_004E6914, a modal box, is not modelled;
     * DAT_004DEA1C is g_pet_pen_up) and option 10 on, every odd step is stamped but skips
     * the world, so the front end runs at half the step rate. */
    if (g_pet_pen_up && options_get(OPTION_CUT_DIALOG_ANIMATIONS) && (++half_rate_count & 1u))
        return 0;
    if (++idle_ticks % 40u == 0u)      /* one log line per virtual second */
        wos_log_event("world_tick", "ms=%lu count=%u",
                      (unsigned long)clock_ms(), idle_ticks);
    world_tick_pending = 1;
    return 1;
}

/* One `dialog` line is N control assignments followed by the terminating
 * ok|cancel. FrontHero-2 owns the New Soul dialogs (138, 149) and takes the pairs
 * as strings because one of its controls is a text field; Panels-2 owns the item
 * and shop dialogs and takes them as ints. Both are offered every op, and each
 * returns 0 for ids it does not own, so the dispatch order is not significant. */
/* A dialog op whose window is not up yet must NOT be dropped: the original's
 * WM_COMMAND has no "not ready" state, and the oracle's pump keeps such an event
 * pending and retries. The port does the same -- unaccepted ops stay queued and are
 * retried every iteration until something takes them. */
#define DIALOG_PENDING_MAX 32
static DscriptOp g_pending[DIALOG_PENDING_MAX];
static int g_pending_count;

static void apply_dialog_op(const DscriptOp *op)
{
    /* Queue it: dialog_retry_pending() offers it until something takes it, so an op
     * whose window is not up yet is retried rather than dropped. */
    if (g_pending_count < DIALOG_PENDING_MAX) g_pending[g_pending_count++] = *op;
}

/* Returns 1 when at least one pending dialog op was accepted this pass. */
static int dialog_retry_pending(void)
{
    int i = 0, accepted = 0;
    while (i < g_pending_count) {
        const DscriptOp *op = &g_pending[i];
        char keys[DSCRIPT_KV_MAX + 1][48];
        const char *kp[DSCRIPT_KV_MAX + 1];
        DialogOp d;
        int k, n = op->control_count, took = 0;
        for (k = 0; k < n; ++k) {
            if (op->svalue[k][0])
                snprintf(keys[k], sizeof(keys[k]), "%d=%s", op->control[k], op->svalue[k]);
            else
                snprintf(keys[k], sizeof(keys[k]), "%d=%d", op->control[k], op->value[k]);
            kp[k] = keys[k];
        }
        snprintf(keys[n], sizeof(keys[n]), "ok");
        kp[n] = keys[n];
        ++n;
        d.id = op->id; d.ctrl = -1; d.value = op->ok; d.ok = op->ok;
        if (panel_dialog_op(&d)) took = 1;
        if (front_dialog_op(op->id, kp, n, op->ok)) took = 1;
        if (options_dialog_op(op->id, kp, n, op->ok)) took = 1;
        if (took) { accepted = 1; g_pending[i] = g_pending[--g_pending_count]; }
        else ++i;
    }
    return accepted;
}

/* FUN_00409722 (0x00409722): the game's date-validity gate, and the ONLY MessageBoxA
 * call site in the binary (0x004097B7, return address 0x004097BD, caption "Check Your
 * Computer's Clock"). It builds two bounds with mktime on a memset-0 tm, so tm_year is
 * years-since-1900: 0x6B = 107 -> 2007-02-01 and 0x81 = 129 -> 2029-12-01, and it accepts
 * DAT_0053866C (the time(NULL) stamp taken at boot, all.c:5803) only if lo <= t <= hi.
 * Outside the window it shows a modal box and returns 1.
 *
 * The value compared is the WALL CLOCK, not the virtual clock: the original reads
 * DAT_0053866C, which is the time(NULL) stamp taken at boot. So this uses plat_time_s(),
 * and the check behaves the same in a headless replay as it does live.
 *
 * The port logs the verdict and returns it but does NOT abort: the original's caller
 * FUN_004097D5 (0x004097D5) decides what the failure means, and that is not yet read.
 * Note the field order: tm_mday = 1 with tm_mon = 1 is 1 February (tm_mon is 0-based), and
 * tm_mon = 0xb is December. */
static int clock_date_check(uint32_t t_s)
{
    /* Seconds from the epoch to 2007-02-01T00:00:00Z and 2029-12-01T00:00:00Z. */
    static const uint32_t lo = 1170288000u;
    static const uint32_t hi = 1890777600u;
    int ok = (t_s >= lo && t_s <= hi);
    wos_log_event("clock_check", "ms=%lu time=%lu lo=%lu hi=%lu ok=%d",
                  (unsigned long)clock_ms(), (unsigned long)t_s,
                  (unsigned long)lo, (unsigned long)hi, ok);
    return ok;
}

static void apply_script_op(const DscriptOp *op, Input *input)
{
    PlatEvent ev;
    int i;
    memset(&ev, 0, sizeof(ev));
    switch (op->kind) {
    case DS_CLICK: case DS_RCLICK: case DS_DOWN: case DS_UP: case DS_MOVE:
        ev.type = PLAT_EV_MOUSE_MOVE; ev.x = op->x; ev.y = op->y;
        input_event(input, &ev);
        if (op->kind == DS_MOVE) break;
        ev.type = op->kind == DS_UP ? PLAT_EV_MOUSE_UP : PLAT_EV_MOUSE_DOWN;
        ev.button = op->button ? op->button : 1;
        input_event(input, &ev);
        /* `click` is a down AND an up, as the oracle hook posts it (do_mouse). */
        if (op->kind == DS_CLICK || op->kind == DS_RCLICK) {
            ev.type = PLAT_EV_MOUSE_UP;
            input_event(input, &ev);
        }
        break;
    case DS_KEY:
        ev.type = PLAT_EV_KEY_DOWN; ev.key = op->key; input_event(input, &ev);
        ev.type = PLAT_EV_KEY_UP;   ev.key = op->key; input_event(input, &ev);
        break;
    case DS_TEXT: {
        const char *s = op->text;
        while (*s) {
            size_t n = strlen(s); if (n > 31) n = 31;
            /* Do not split UTF-8 continuation bytes across platform events. */
            while (n > 28 && s[n] && ((unsigned char)s[n] & 0xc0) == 0x80) --n;
            memset(&ev, 0, sizeof(ev));
            ev.type = PLAT_EV_TEXT; memcpy(ev.text, s, n); s += n;
            input_event(input, &ev);
        }
        break;
    }
    case DS_DIALOG:
        apply_dialog_op(op);
        break;
    case DS_DUMP: dump_one(op->text); dump_shot(op->text); break;
    case DS_END: game_request_quit(); break;
    default: break;
    }
    (void)i;
}

/* FUN_00409722 (0x00409722) raises the only MessageBoxA in the binary -- 0x004097B7,
 * return address 0x004097BD -- with these two strings, s_Check_Your_Computer_s_Clock_
 * 004dd7e4 and s_It_would_appear_that_your_comput_004dd720. Style 0, so it is modal and
 * the game blocks on it until dismissed. */
static const char CLOCK_BOX_CAPTION[] = "Check Your Computer's Clock";
static const char CLOCK_BOX_BODY[] =
    "It would appear that your computer's calendar is set to the incorrect date.  "
    "Since many features of the game depend upon an accurate time setting, I suggest "
    "you set it now, then restart the game.";

/* The in-framebuffer stand-in for that modal MessageBoxA. Blocks until dismissed, exactly
 * as the Win32 box does, and never returns a code: the original continues either way. */
static void clock_box_modal(Framebuffer *fb)
{
    Input in = {0};
    int waiting = 1;
    wos_log_event("clock_box_shown", "caption=%s", CLOCK_BOX_CAPTION);
    while (waiting) {
        PlatEvent ev;
        input_begin(&in);
        while (plat_poll_event(&ev)) input_event(&in, &ev);
        if (in.quit) { game_request_quit(); return; }
        if (in.released[PLAT_KEY_RETURN] || in.released[PLAT_KEY_ESCAPE] ||
            in.released[PLAT_KEY_SPACE] || in.mouse_released)
            waiting = 0;
        fb_reset_clip(fb);
        fb_clear(fb, 0);
        ui_panel(fb, (Rect){ 60, 120, PLAT_SCREEN_W - 120, 200 },
                 CLOCK_BOX_CAPTION, CLOCK_BOX_BODY);
        plat_present(fb->pixels, fb->w, fb->h);
    }
    wos_log_event("clock_box_dismissed", "ms=%lu", (unsigned long)clock_ms());
}

int game_main(int argc, char **argv)
{
    const char *replay_path=NULL, *script_path=NULL, *log_path=NULL, *dump_path=NULL, *shot_dir=NULL;
    uint32_t seed=0, epoch=0, max_frames=0, frame=0, shot_every=0, phase=0;
    int capped=0, headless=0, i, result=0, initialized=0, have_seed=0, have_epoch=0;
    char *text=NULL; Replay *replay=NULL; Dscript *script=NULL; size_t error_line;
    uint32_t *pixels=NULL; Framebuffer fb; Input input={0};
    data_path=NULL; save_path=NULL; quitting=0;
    for(i=1;i<argc;++i) {
        const char *arg=argv[i];
        if(!strcmp(arg,"--headless")) { headless=1; continue; }
        if(i+1>=argc) { usage(); return 1; }
        if(!strcmp(arg,"--data")) data_path=argv[++i];
        else if(!strcmp(arg,"--save")) { save_path=argv[++i]; if(!*save_path) { usage(); return 1; } }
        else if(!strcmp(arg,"--replay")) replay_path=argv[++i];
        else if(!strcmp(arg,"--script")) script_path=argv[++i];
        else if(!strcmp(arg,"--log")) log_path=argv[++i];
        else if(!strcmp(arg,"--dump")) dump_path=argv[++i];
        else if(!strcmp(arg,"--time")) { if(unsigned_arg(argv[++i],&epoch)) { usage(); return 1; } have_epoch=1; }
        else if(!strcmp(arg,"--seed")) { if(unsigned_arg(argv[++i],&seed)) { usage(); return 1; } have_seed=1; }
        else if(!strcmp(arg,"--max-frames")) { capped=1; if(unsigned_arg(argv[++i],&max_frames)) { usage(); return 1; } }
        else if(!strcmp(arg,"--dump-shots")) {
            if(i+1>=argc || !*argv[i+1]) { usage(); return 1; }
            g_dump_shot_dir=argv[++i];
        }
        else if(!strcmp(arg,"--shot-every")) {
            if(i+2>=argc || unsigned_arg(argv[++i],&shot_every) || !shot_every) { usage(); return 1; }
            shot_dir=argv[++i]; if(!*shot_dir) { usage(); return 1; }
        }
        else { usage(); return 1; }
    }
    if(!data_path || !*data_path) { usage(); return 1; }
    if(!save_path) {
        int n=snprintf(default_save_path,sizeof(default_save_path),"%s/Save",data_path);
        if(n<0 || (size_t)n>=sizeof(default_save_path)) { fputs("Default save path is too long\n",stderr); return 1; }
        save_path=default_save_path;
    }
    if(wos_log_open(log_path)) { fputs("Cannot open event log\n",stderr); return 1; }
    if(replay_path && script_path) { fputs("--replay and --script are exclusive\n",stderr); result=1; goto cleanup; }
    if(replay_path) {
        text=text_read_file(replay_path,NULL); replay=malloc(sizeof(*replay));
        if(!text || !replay) { fputs("Cannot load replay\n",stderr); result=1; goto cleanup; }
        if(replay_parse(replay,text,&error_line)) {
            fprintf(stderr,"REPLAY FAIL parse line %lu\n",(unsigned long)error_line); result=2; goto cleanup;
        }
    } else if(script_path) {
        text=text_read_file(script_path,NULL); script=malloc(sizeof(*script));
        if(!text || !script) { fputs("Cannot load script\n",stderr); result=1; goto cleanup; }
        if(dscript_parse(script,text,&error_line)) {
            fprintf(stderr,"SCRIPT FAIL parse line %lu\n",(unsigned long)error_line); result=2; goto cleanup;
        }
    }
    if(plat_init("Well of Souls",PLAT_SCREEN_W,PLAT_SCREEN_H,headless?PLAT_INIT_HEADLESS:0)) {
        fputs("Platform initialization failed\n",stderr); result=1; goto cleanup;
    }
    initialized=1; pixels=malloc(PLAT_SCREEN_W*PLAT_SCREEN_H*sizeof(*pixels));
    if(!pixels) { result=1; goto cleanup; }
    if(shot_dir && plat_mkdir(shot_dir)) { fputs("Cannot create screenshot directory\n",stderr); result=1; goto cleanup; }
    fb_init(&fb,pixels,PLAT_SCREEN_W,PLAT_SCREEN_H);
    g_dump_shot_fb=&fb;
    if(g_dump_shot_dir && plat_mkdir(g_dump_shot_dir)) { fputs("Cannot create the dump-shot directory\n",stderr); result=1; goto cleanup; }
    clock_reset();
    if(have_epoch) clock_set_time_base(epoch);
    /* The seed step is a BOOT STEP now, at CRT index 41, because the 1408 EncInt
     * draws precede it (Oracle3's trace: calls 1..1408 are the tables, call 1409 is
     * 0x00426B27). Seeding before boot put every table draw on the wrong side of
     * the re-seed. --seed still pins the time() value the step uses. */
    /* --seed no longer touches the CRT seed: the original seeds from time(NULL),
     * and --time is the faithful pin. Leave holdrand at the CRT default of 1 so
     * the 1408 EncInt draws match, and let boot_seed_step() seed from the clock. */
    options_load();
    if (!clock_date_check(clock_time_s())) {
        /* The original blocks here on a modal MessageBoxA and then continues into the
         * same init path, so the port shows the same box and does the same. */
        clock_box_modal(&fb);
    }
    /* The boot sequence, in the original's CRT initialiser order. seed_crt() above
     * already took the two srand calls and the base-offset draw that
     * FUN_004269AF does as a static initialiser (index 0, i.e. before the table);
     * this runs the _initterm steps, table A first. */
    boot_register_core();
    scene_boot_register();
    if (have_seed && !have_epoch) { boot_seed_pin = seed; boot_seed_have_pin = 1; }
    boot_run();
    /* FUN_00428360, the main frame's id-0x16 100 ms timer, armed at the original's
     * point: InitInstance, at t=0 and before the title art loads. It is what actually
     * drives FUN_0040A7C7's gate. The title's cold scene-cache stall (scenecache.h) takes
     * the clock to 150 before the loop runs, so the first WM_TIMER (due at 100) is
     * delivered late at 150 and the next is due at 200 -- the oracle's t_tim. */
    frame_timer_owner = 0;
    clock_set_timer(&frame_timer_owner, MAIN_FRAME_TIMER_ID, 100u, main_frame_timer, NULL);
    if(game_boot()) { result=1; goto cleanup; }
    {
        /* The first WM_PAINT: the original paints its new windows before the loop takes its
         * first script event or timer (oracle: WM_PAINT at 150, ahead of everything). */
        const Screen *first=screen_current();
        if(first && first->render) { fb_reset_clip(&fb); fb_clear(&fb,0); first->render(&fb); }
    }
    if(!replay && !script) clock_attach_realtime();

    /* The original's loop is CWinApp::Run at 0x0040A8D9: a bare PeekMessage pump
     * whose order is INPUT -> TIMER -> IDLE/PAINT, and whose idle path runs only
     * while the message queue is empty. --script makes the virtual clock the only
     * thing that decides when a step happens, so the step count is a function of
     * the script and of nothing else. --replay keeps the legacy per-frame
     * 1000/60 ms virtual step so the existing .rpl acceptance tests are
     * bit-identical. */
    while(!quitting) {
        PlatEvent event; const Screen *screen; int delivered = 0;
        input_begin(&input);
        while(plat_poll_event(&event)) if(!replay || event.type==PLAT_EV_QUIT) input_event(&input,&event);
        if(input.quit) break;

        if(script) {
            /* INPUT first: at an equal timestamp a scheduled event is dispatched
             * before a timer that is also due, which is the original's order.
             *
             * A delivered op MUST be seen by the screen update in THIS iteration.
             * The world step is gated on the 100 ms timer, so without `delivered`
             * forcing it the loop would `continue` past screen->update(), the next
             * iteration's input_begin() would clear the pressed/released edges, and
             * every scripted event would be dropped -- a --script run with no input
             * at all, which is exactly what the diff suite was measuring. */
            /* ONE op per iteration: the oracle's pump fires one .dsc event per step and
             * lets the app run (timers, the idle gate) before the next, even at an equal
             * timestamp (docs/re/oracle.md 3.1). The clock does not move while ops are due. */
            {
                /* A timer that came due BEFORE the op's time (the boot timer due at 100,
                 * delivered late at 150 after the title stall) is already in the queue
                 * when the op is posted, so it is taken first; ties still go to input. */
                uint32_t op_at = dscript_next_time(script, 0), tdl = clock_next_timer_deadline();
                int timer_first = tdl != UINT32_MAX && (int32_t)(tdl - clock_ms()) <= 0 &&
                                  (int32_t)(tdl - op_at) < 0;
                const DscriptOp *op = timer_first ? NULL : dscript_take(script, clock_ms());
                if(op) { apply_script_op(op,&input); delivered = 1; }
            }
            {
                uint32_t next = dscript_next_time(script, clock_ms());
                if(next == UINT32_MAX) break;             /* nothing scheduled remains */
                if(script->has_end && clock_ms() >= script->end_ms) break;
                {
                    /* The oracle pump's step: the earliest of the next op, the next timer
                     * deadline and (once AppRun's loop runs) the idle boundary. */
                    uint32_t target = next, t = clock_next_timer_deadline();
                    if (t != UINT32_MAX && (int32_t)(t - target) < 0) target = t;
                    if (idle_loop_live && !front_modal_up()) {
                        t = clock_idle_next();
                        if ((int32_t)(t - target) < 0) target = t;
                    }
                    /* After a step, start over: an op due at the new time goes before a
                     * timer or the idle gate due at the same millisecond (input wins ties).
                     * Never step in an iteration that delivered an op: the update below must
                     * see its input edges first, and the next iteration steps instead. */
                    if(!delivered && target > clock_ms()) { clock_advance(target - clock_ms()); continue; }
                }
            }
        } else if(replay) {
            PlatEvent events[REPLAY_EVENTS_MAX]; size_t n,j;
            int status=replay_step(replay,events,&n,seen,NULL);
            if(status==2) { fprintf(stderr,"REPLAY FAIL expect %s\n",replay->failed_event); result=2; break; }
            if(status==1 && !n) break;
            for(j=0;j<n;++j) input_event(&input,&events[j]);
            phase += 1000; clock_advance(phase / 60); phase %= 60;
        }
        /* TIMER: every WM_TIMER due now, oldest deadline first (FUN_0040A8D9
         * dispatches the queue, and the timers are the lowest-priority entries). */
        /* Not in an iteration that delivered an op: the input message is handled (by the
         * update below) before any timer the next pump pass takes. The oracle's click at
         * 150 with a timer due since 100 runs the click first (boot_menu.dsc). */
        if (!delivered) clock_dispatch_timers();
        /* IDLE (FUN_0040A7C7, 20 ms) -> world (FUN_0042895C's 25 ms sub-gate ->
         * FUN_0041BDB4). The world step runs once per 25 ms of virtual time in
         * --script mode, which is the original's rate; the legacy --replay mode
         * keeps its per-frame step so the .rpl acceptance tests are unchanged. */
        if (idle_loop_live && !delivered && !front_modal_up()) idle_gate();   /* AppRun's idle path -> FUN_0040A7C7 */
        dialog_retry_pending();
        if (capped && frame >= max_frames) { result = 3; break; }
        if (script && !delivered) {
            /* The 100 ms timer enters the gate; a step happens only when it fired,
             * unless this iteration delivered a scheduled input event, which must be
             * consumed by the update now rather than discarded. */
            if (!world_tick_pending) continue;
            world_tick_pending = 0;
        }
        /* An iteration that only delivers input is a message handler, not FUN_0041BDB4. */
        world_step_now = !(script && delivered);
        scene_tick_if_any();
        env_tick();                 /* FUN_00456AA1, on both arms of the world step FUN_0041BDB4 */
        screen=screen_current(); if(screen && screen->update) screen->update(&input);
        fb_reset_clip(&fb); fb_clear(&fb,0);
        screen=screen_current(); if(screen && screen->render) screen->render(&fb);
        if(replay && replay->shot_path && save_shot(&fb,replay->shot_path)) {
            fprintf(stderr,"Cannot write screenshot: %s\n",replay->shot_path); result=1; break;
        }
        plat_present(fb.pixels,fb.w,fb.h); ++frame;
        if(shot_every && frame%shot_every==0) {
            char path[4096]; int n=snprintf(path,sizeof(path),"%s/frame_%06lu.bmp",shot_dir,(unsigned long)frame);
            if(n<0 || (size_t)n>=sizeof(path) || save_shot(&fb,path)) {
                fputs("Cannot write periodic screenshot\n",stderr); result=1; break;
            }
        }
        if(!replay && !script) {
            uint32_t idle = clock_20hz_next(), now = clock_ms();
            if((int32_t)(idle - now) > 0) plat_sleep_ms(idle - now);
        }
    }
    if(run_dumps(dump_path)) { fputs("Cannot write dump\n",stderr); if(!result) result=1; }
cleanup:
    screen_set(NULL); free(pixels); free(replay); free(script); free(text);
    if(initialized) plat_shutdown();
    wos_log_close(); return result;
}
