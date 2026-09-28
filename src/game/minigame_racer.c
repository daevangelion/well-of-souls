/* GAME 2 — Monster Racer. See minigame_racer.h for the rules and their VAs. */
#include "../engine/clock.h"
#include "minigame_data.h"
#include "hero.h"
#include "world.h"
#include "../engine/font.h"
#include "../engine/log.h"
#include "../engine/rng.h"
#include <stdio.h>
#include <string.h>

#define WIN_W 280
#define WIN_H 170
#define RACERS 4
/* the fee and the refund, from FUN_00405FD1's payout block */
#define ENTRY_COST 100
#define RETURN_PRIZE 100

enum { RACE_PICK, RACE_RUNNING, RACE_DONE };

static struct {
    int open;
    int state;                    /* this+0x12C */
    int id[RACERS];                /* this+0xDC + 0x10*i */
    int speed[RACERS];             /* this+0xEC + 0x10*i */
    int dist[RACERS];              /* this+0xFC + 0x10*i */
    int frame[RACERS];             /* this+0x11C + 0x10*i */
    int finished[RACERS];          /* this+0x10C + 0x10*i */
    int place1, place2;            /* this+0x130 / 0x134, both permanently -1 in A96 */
    int t_start;                   /* this+0x138 */
    int pick;                      /* this+0x13C */
    int winnings;                  /* this+0x140 */
    int player;                    /* which of the four quadrants the player clicked */
    int paid;
    char text[96];
    MgRacers racers;
} g;

/* FUN_004066A9 (0x4066A9): 8 rand() pick the four racers out of racers.txt. */
static void setup(void)
{
    int i;
    for (i = 0; i < RACERS; ++i) {
        int pick = crt_rand() % (g.racers.count ? g.racers.count : 1);
        g.id[i] = g.racers.count ? g.racers.ids[pick] : 0;
        g.speed[i] = 1 + crt_rand() % 100;
    }
    g.state = RACE_PICK;
    g.place1 = -1;
    g.place2 = -1;
    g.pick = 0;
    g.winnings = 0;
    g.paid = 0;
    for (i = 0; i < RACERS; ++i) { g.dist[i] = 0; g.frame[i] = 0; g.finished[i] = 0; }
    snprintf(g.text, sizeof g.text, "Pick your monster, then click to race");
}

int minigame_racer_open(void)
{
    memset(&g, 0, sizeof g);
    g.open = 1;
    mg_racers_load(&g.racers);
    setup();
    wos_log_event("minigame_racer_open", "racers=%d", g.racers.count);
    return 1;
}

void minigame_racer_close(void)
{
    g.open = 0;
    wos_log_event("minigame_racer_close", "racers=%d winnings=%d", g.racers.count, g.winnings);
}

static void quadrant(int q, Rect *out)
{
    int hx = WIN_W / 2, hy = WIN_H / 2;
    int qx = (q & 1) ? hx : 0;
    int qy = (q & 2) ? hy : 0;
    out->x = qx;
    out->y = qy;
    out->w = hx;
    out->h = hy;
}

static void start_race(int player, uint32_t now_ms)
{
    int i, fastest = 0, best = 0;
    g.player = player;
    /* 100 cookies charged on the click (FUN_00405FD1's payout block). */
    hero_add_gold(&g_hero, -ENTRY_COST);
    for (i = 0; i < RACERS; ++i) {
        g.dist[i] = 0;
        g.finished[i] = 0;
        g.frame[i] = 0;
        if (g.speed[i] > best) { best = g.speed[i]; fastest = i; }
    }
    /* 100 back only when the player's racer has the maximum speed; the 500 branch is dead
     * because place1 is only ever written as -1. */
    if (fastest == player) {
        hero_add_gold(&g_hero, RETURN_PRIZE);
        g.winnings += RETURN_PRIZE;
    }
    g.t_start = (int)now_ms;
    g.state = RACE_RUNNING;
    wos_log_event("minigame_racer_start", "player=%d fastest=%d speed=%d winnings=%d",
                  player, fastest, g.speed[player], g.winnings);
}

/* One paint's worth of RNG: 4 + Binomial(4, 0.2) rand() for the racer sprite frames. */
static void paint_rand(void)
{
    int i, k;
    for (i = 0; i < RACERS; ++i) g.frame[i] = crt_rand() % 8;
    for (k = 0; k < 4; ++k) if (crt_rand() % 5 == 0) g.frame[crt_rand() % RACERS] = crt_rand() % 8;
}

void minigame_racer_update(uint32_t now_ms)
{
    int i, all_done = 1;
    if (!g.open) return;
    /* The original only steps while OnIdle invalidates the window, i.e. while racing. */
    if (g.state != RACE_RUNNING) return;
    for (i = 0; i < RACERS; ++i) {
        /* x = speed * elapsed / 1000: the LOWEST speed finishes first. */
        long elapsed = (long)now_ms - g.t_start;
        if (elapsed < 0) elapsed = 0;
        g.dist[i] = (int)((long)g.speed[i] * elapsed / 1000);
        if (g.dist[i] < WIN_W) all_done = 0;
        else if (!g.finished[i]) g.finished[i] = 1;
    }
    paint_rand();
    if (all_done) {
        g.state = RACE_DONE;
        snprintf(g.text, sizeof g.text, "Race over. Winnings: %d GP", g.winnings);
        wos_log_event("minigame_racer_result", "player=%d winnings=%d", g.player, g.winnings);
    }
}

void minigame_racer_click(int x, int y, int pressed)
{
    int q;
    if (!g.open || !pressed) return;
    if (g.state == RACE_PICK) {
        for (q = 0; q < RACERS; ++q) {
            Rect r;
            quadrant(q, &r);
            if (x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h) {
                g.pick = q;
                start_race(q, (uint32_t)clock_ms());
                return;
            }
        }
        return;
    }
    if (g.state == RACE_DONE) setup();
}

void minigame_racer_key(int vk, int pressed)
{
    if (g.open && pressed && g.state != RACE_RUNNING && vk >= '1' && vk <= '4')
        minigame_racer_click((vk - '1') % 2 * (WIN_W / 2), (vk - '1') / 2 * (WIN_H / 2), 1);
}

void minigame_racer_render(Framebuffer *fb, int bx, int by)
{
    Rect win = { bx, by, WIN_W, WIN_H };
    int i;
    char buf[64];
    if (!g.open) return;
    fb_fill(fb, win, 0x0d1418u);
    fb_rect(fb, win, 0x9d8959u);
    for (i = 0; i < RACERS; ++i) {
        Rect r;
        char name[32];
        quadrant(i, &r);
        fb_rect(fb, (Rect){bx + r.x, by + r.y, r.w, r.h}, i == g.pick ? 0xffd477u : 0x292820u);
        snprintf(name, sizeof name, "M%d spd %d", g.id[i], g.speed[i]);
        font_draw(fb, bx + r.x + 4, by + r.y + 4, name, 0xffe6aeu);
        if (g.state == RACE_RUNNING || g.state == RACE_DONE) {
            int x = bx + r.x + (g.dist[i] % (r.w ? r.w : 1));
            int y = by + r.y + r.h - 24;
            fb_fill(fb, (Rect){x, y, 16, 10}, i == g.player ? 0xffd477u : 0x9d8959u);
            snprintf(buf, sizeof buf, "%d", g.dist[i]);
            font_draw(fb, bx + r.x + 4, by + r.y + 16, buf, 0xffe6aeu);
        }
    }
    font_draw(fb, bx + 4, by + WIN_H - 12, g.text, 0xffe6aeu);
}

void minigame_racer_dump(DumpEmit emit, void *user)
{
    int i;
    dump_emit_int(emit, "minigame.racer.state", g.state, user);
    dump_emit_int(emit, "minigame.racer.pick", g.pick, user);
    dump_emit_int(emit, "minigame.racer.player", g.player, user);
    dump_emit_int(emit, "minigame.racer.winnings", g.winnings, user);
    dump_emit_int(emit, "minigame.racer.t_start", g.t_start, user);
    for (i = 0; i < RACERS; ++i) {
        dump_emit_int(emit, "minigame.racer.id", g.id[i], user);
        dump_emit_int(emit, "minigame.racer.speed", g.speed[i], user);
        dump_emit_int(emit, "minigame.racer.dist", g.dist[i], user);
        dump_emit_int(emit, "minigame.racer.finished", g.finished[i], user);
    }
}
