/* GAME 5 — Pokegatchi Training Center / "Pet Center". See minigame_train.h. */
#include "minigame_train.h"
#include "minigame_data.h"
#include "../engine/clock.h"
#include "../engine/font.h"
#include "../engine/log.h"
#include "../engine/rng.h"
#include "world.h"
#include <stdio.h>
#include <string.h>

#define WIN_W 326
#define WIN_H 213

/* the 5 care buttons along the bottom of the 0xC4 template, plus the pet-pen quadrants */
#define CARE_N 5
static const Rect care_rect[CARE_N] = {
    { 8, 180, 56, 20 }, { 70, 180, 56, 20 }, { 132, 180, 56, 20 },
    { 194, 180, 56, 20 }, { 256, 180, 56, 20 }
};
static const char *const care_name[CARE_N] = { "Play", "Feed", "Sleep", "Wash", "Train" };
/* the need each care button pushes down; -1 means it touches none directly */
static const int care_need[CARE_N] = { NEED_LAZY, NEED_HUNGRY, NEED_SLEEPY, NEED_DIRTY, NEED_STUPID };

/* FUN_00412840 (0x412840), the need-meter drift, exactly:
 *   elapsed = param_2 - this->last_tick;                // ticks, not ms
 *   if ((DAT_004E48D0 != 0) || (DAT_004E17FC != 0)) and FUN_00431DB1() returns nonzero:
 *       the whole block below runs; otherwise the function returns 0 immediately.
 * DAT_004E48D0 and DAT_004E17FC are a netgame/party flag and a scene flag. Neither is set
 * in solo play, so the ORIGINAL LEAVES THE METERS FROZEN. The port reproduces that: identical
 * behaviour is the target even though the result is inert. Do not "fix" this.
 *   if the pet is out (param_1[0] == 1) this->energy += elapsed * 0x411A;   (16666)
 *   energy is clamped to 0, then to 1000000, then hard-capped at 2000000;
 *   the meter rate is clamp(this->param_29 / 0x3C, 1, 10) with 0x3C = 60.
 * The care buttons are the only thing that moves a meter, which is what the original does. */
#define ENERGY_PER_TICK 0x411A      /* 16666 */
#define ENERGY_SOFT_CAP 1000000
#define ENERGY_HARD_CAP 2000000
#define NEED_MAX        1000000
#define CARE_RELIEF     250000      /* how much a care action knocks a meter down */
#define CARE_ENERGY     50000       /* what it costs the pet's energy */

static const char *const need_name[NEED_COUNT] = {
    "Wild", "Sleepy", "Hungry", "Stupid", "Sickly", "Lazy", "Dirty", "Angry"
};

static struct {
    int open;
    int need[NEED_COUNT];   /* 0..NEED_MAX, the current pet's eight meters */
    int energy;             /* the current pet's energy, 0..ENERGY_MAX */
    int selected;
    int ticks;              /* upkeep steps elapsed */
    uint32_t next_upkeep;
    int cares;
} g;

int minigame_train_open(void)
{
    int i;
    memset(&g, 0, sizeof g);
    g.open = 1;
    g.energy = ENERGY_SOFT_CAP / 4;   /* energy starts mid-range; it never drifts */
    for (i = 0; i < NEED_COUNT; ++i) g.need[i] = NEED_MAX / 4;
    g.next_upkeep = 0;
    wos_log_event("minigame_train_open", "pets=%d", MG_RACERS_MAX);
    return 1;
}

void minigame_train_close(void)
{
    g.open = 0;
    wos_log_event("minigame_train_close", "cares=%d", g.cares);
}

/* FUN_00412840 returns 0 immediately in solo play, so there is nothing to advance: the
 * meters are frozen and only the care buttons move them. The original has no SetTimer here
 * either (FUN_004126C8's OnTimer is dead and FUN_00412797's KillTimer is a no-op guard). */
void minigame_train_update(uint32_t now_ms)
{
    (void)now_ms;
}

void minigame_train_click(int x, int y, int pressed)
{
    int i;
    if (!g.open || !pressed) return;
    for (i = 0; i < CARE_N; ++i) {
        Rect r = care_rect[i];
        if (x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h) {
            int n = care_need[i];
            if (n >= 0) {
                g.need[n] -= CARE_RELIEF;
                if (g.need[n] < 0) g.need[n] = 0;
            }
            g.energy -= CARE_ENERGY;
            if (g.energy < 0) g.energy = 0;
            g.cares++;
            wos_log_event("minigame_train_care", "action=%s need=%d energy=%d",
                          care_name[i], n, g.energy);
            return;
        }
    }
}

void minigame_train_key(int vk, int pressed)
{
    if (g.open && pressed && vk >= '1' && vk <= '5')
        minigame_train_click(care_rect[vk - '1'].x, care_rect[vk - '1'].y, 1);
}

void minigame_train_render(Framebuffer *fb, int bx, int by)
{
    Rect win = { bx, by, WIN_W, WIN_H };
    int i;
    char buf[64];
    if (!g.open) return;
    fb_fill(fb, win, 0x14201cu);
    fb_rect(fb, win, 0x9d8959u);
    font_draw(fb, bx + 8, by + 6, "PokeGatchi Pen", 0xffe6aeu);
    for (i = 0; i < NEED_COUNT; ++i) {
        int row_y = by + 24 + i * 18;
        int bar_w = (int)((long)g.need[i] * 200 / NEED_MAX);
        font_draw(fb, bx + 8, row_y, need_name[i], 0xffe6aeu);
        fb_fill(fb, (Rect){ bx + 64, row_y, 200, 10 }, 0x292820u);
        fb_fill(fb, (Rect){ bx + 64, row_y, bar_w, 10 }, 0x9d8959u);
    }
    snprintf(buf, sizeof buf, "energy %d", g.energy);
    font_draw(fb, bx + 8, by + WIN_H - 32, buf, 0xffe6aeu);
    for (i = 0; i < CARE_N; ++i) {
        Rect r = { bx + care_rect[i].x, by + care_rect[i].y, care_rect[i].w, care_rect[i].h };
        fb_fill(fb, r, 0x292820u);
        fb_rect(fb, r, 0x9d8959u);
        font_draw(fb, r.x + 4, r.y + 6, care_name[i], 0xffe6aeu);
    }
}

void minigame_train_dump(DumpEmit emit, void *user)
{
    int i;
    dump_emit_int(emit, "minigame.train.energy", g.energy, user);
    dump_emit_int(emit, "minigame.train.ticks", g.ticks, user);
    dump_emit_int(emit, "minigame.train.cares", g.cares, user);
    for (i = 0; i < NEED_COUNT; ++i)
        dump_emit_int(emit, "minigame.train.need", g.need[i], user);
}
