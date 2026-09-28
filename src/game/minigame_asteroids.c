/* GAME 6 — Asteroids. See minigame_asteroids.h for the rules and their VAs. */
#include "minigame_asteroids.h"
#include "../engine/clock.h"
#include "../engine/font.h"
#include "../engine/log.h"
#include "../engine/rng.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define WIN_W 383
#define WIN_H 277

#define MAX_ASTEROIDS 20   /* 0x698FF0 record array */
#define MAX_BULLETS   20   /* 0x69BA70 */
#define STARS         200  /* the starfield is 200 points, 200 rand() at start */

#define START_LIVES 3

/* FUN_0046E3AA (0x46E3AA), the field, exactly:
 *   count = wave * 4 + 10, capped at 0x14 (20)
 *   per asteroid, 3 rand():  size = rand() % 0x1E + 0x14
 *                            y    = rand() % H
 *                            x    = rand() % W
 *   then FUN_0046E14C(x, y, size, speedBase) with speedBase = (wave + 5) * 10.
 * `wave` here is this+0x7E0. */
#define FIELD_MIN  10
#define FIELD_PER_WAVE 4
#define FIELD_MAX  0x14
#define SIZE_MIN   0x14   /* 20 */
#define SIZE_SPAN  0x1E   /* 30 */

/* FUN_0046E14C (0x46E14C), one asteroid, 16 rand() in this exact order:
 *   1: vx  = (rand() % 1000)               * 0.00628   (0x4CE910)
 *   2: vy  = (rand() % 200 - 100)          * 0.001    (0x4CE918)
 *   3: spin= (size + rand() % (size * 2))   * 0.01     (0x4CE920)
 *   4: g   = rand() % 100 + 0x80, packed as 0xRRGGBB
 *   5..16: 12 vertex rand() in a `sin/cos` loop; the value is `ftol()`ed into the vertex
 *           record, and the loop index advances by 0.5233333333333333 rad (0x4CE928).
 * piVar3[0xF] = 0x0C is the vertex count. */
#define VX_SCALE   0.00628
#define VY_SCALE   0.001
#define SPIN_SCALE 0.01
#define GREY_BASE  0x80
#define VERTEX_COUNT 12
#define VERTEX_STEP  0.5233333333333333

/* The split, in FUN_0046E4D1 (0x46E4D1). It runs only when size > 0x1E, and each of the
 * three branches consumes its OWN rand() whether or not the roll succeeds, so the budget is
 * 1-3 depending on the rolls:
 *   rand() % 100 < 0x50 (80 %) -> spawn at size * 2 / 3
 *   rand() % 100 < 0x32 (50 %) -> spawn at size / 2
 *   rand() % 100 < 0x1E (30 %) -> spawn at size / 3            <- 0x14 = 20, not 30
 * Score: +0x32 (50) when size < 0x1F, else +0x19 (25), tracked into DAT_004F828C as the
 * high score. Kills go to this+0x7D8. */
#define SPLIT_LARGE_PCT 80
#define SPLIT_MED_PCT  50
#define SPLIT_SMALL_PCT 20
#define SPLIT_GATE_SIZE 0x1E
#define SCORE_SMALL 0x32
#define SCORE_LARGE 0x19

/* The collision box: SetRect(x, y, x, y) then InflateRect by (size * 0x50) / 100, i.e. 80%
 * of the size, so the hit box is 1.6 * size across. */
#define HIT_PCT 0x50

/* the key bindings, polled with GetAsyncKeyState in FUN_0046E457 */
#define KEY_LEFT   0x25
#define KEY_RIGHT  0x27
#define KEY_UP     0x26
#define KEY_DOWN   0x28
#define KEY_FIRE   0x20
#define KEY_PAUSE  0x13

/* One 0x100-byte record at 0x698FF0. */
typedef struct {
    double x, y;          /* +0x08, +0x10 */
    double vx, vy;        /* +0x18, +0x24 */
    double spin;          /* +0x20 */
    int size;             /* +0x00, 0 = free */
    unsigned grey;        /* 0xRRGGBB from rand()%100 + 0x80 */
    int vertex[VERTEX_COUNT];
    int alive;
} Asteroid;

typedef struct {
    double x, y, vx, vy;
    int life;
    int alive;
} Bullet;

static struct {
    int open;
    double sx, sy;          /* this+0x3C0 / 0x3C8 */
    double heading;         /* this+0x3D0, radians */
    double vx, vy;          /* this+0x3E0 / 0x3E4 */
    int w, h;               /* this+0x3D8 / 0x3DC, from GetClientRect */
    int key[6];             /* this+0x3A0..0x3BC latches */
    int score, high, kills, shots, wave, ships, shields, invuln, gameover;
    int star_x[STARS], star_y[STARS];
    Asteroid rock[MAX_ASTEROIDS];
    Bullet shot[MAX_BULLETS];
    uint32_t last_step;
    char banner[64];
    uint32_t banner_t0;
} g;

/* FUN_0046E14C (0x46E14C): one asteroid, 16 rand() in the exact order documented above. */
static void spawn_asteroid(int x, int y, int size, int speed_base)
{
    Asteroid *a = NULL;
    int i;
    for (i = 0; i < MAX_ASTEROIDS; ++i) if (!g.rock[i].alive) { a = &g.rock[i]; break; }
    if (!a) return;
    a->alive = 1;
    a->size = size;
    a->x = x;
    a->y = y;
    a->vx = (double)(crt_rand() % 1000) * VX_SCALE;                  /* 1 */
    a->vy = (double)(crt_rand() % 200 - 100) * VY_SCALE;             /* 2 */
    a->spin = (double)(speed_base + crt_rand() % (speed_base * 2)) * SPIN_SCALE;  /* 3 */
    {                                                                /* 4 */
        unsigned g8 = (unsigned)(crt_rand() % 100) + GREY_BASE;
        a->grey = (g8 << 16) | (g8 << 8) | g8;
    }
    /* 5..16: the 12 vertex rand() in the sin/cos loop, all of which must be consumed. */
    for (i = 0; i < VERTEX_COUNT; ++i) a->vertex[i] = crt_rand();
}

/* FUN_0046E3AA (0x46E3AA): the level start. count = wave*4 + 10 capped at 20, 3 rand() per
 * asteroid, and speedBase = (wave + 5) * 10. */
static void level_start(void)
{
    int count = g.wave * FIELD_PER_WAVE + FIELD_MIN;
    int i;
    if (count > FIELD_MAX) count = FIELD_MAX;
    g.shields++;
    g.invuln = 3000;                       /* FUN_0046F1E5, 3 s of teleport-in */
    memset(g.rock, 0, sizeof g.rock);
    memset(g.shot, 0, sizeof g.shot);
    for (i = 0; i < count; ++i) {
        int size = crt_rand() % SIZE_SPAN + SIZE_MIN;
        int y    = crt_rand() % (g.h ? g.h : 1);
        int x    = crt_rand() % (g.w ? g.w : 1);
        spawn_asteroid(x, y, size, (g.wave + 5) * 10);
    }
    snprintf(g.banner, sizeof g.banner, "WAVE %d", g.wave);
    g.banner_t0 = clock_ms();
    wos_log_event("minigame_asteroids_wave", "wave=%d count=%d shields=%d ships=%d",
                  g.wave, count, g.shields, g.ships);
}

int minigame_asteroids_open(void)
{
    int i;
    memset(&g, 0, sizeof g);
    g.open = 1;
    g.w = WIN_W;
    g.h = WIN_H;
    g.sx = g.w / 2.0;
    g.sy = g.h / 2.0;
    g.ships = START_LIVES;
    g.wave = 0;
    /* 200 rand() for the starfield (FUN_0046E29B) */
    for (i = 0; i < STARS; ++i) {
        g.star_x[i] = crt_rand() % g.w;
        g.star_y[i] = crt_rand() % g.h;
    }
    level_start();
    wos_log_event("minigame_asteroids_open", "ships=%d wave=%d", g.ships, g.wave);
    return 1;
}

void minigame_asteroids_close(void)
{
    g.open = 0;
    wos_log_event("minigame_asteroids_close", "score=%d wave=%d ships=%d",
                  g.score, g.wave, g.ships);
}

static void fire(void)
{
    int i;
    for (i = 0; i < MAX_BULLETS; ++i) {
        if (!g.shot[i].alive) {
            g.shot[i].alive = 1;
            g.shot[i].life = 60;
            g.shot[i].x = g.sx;
            g.shot[i].y = g.sy;
            g.shot[i].vx = cos(g.heading) * 6.0;
            g.shot[i].vy = sin(g.heading) * 6.0;
            g.shots++;
            return;
        }
    }
}

static void respawn(void)
{
    g.sx = g.w / 2.0;
    g.sy = g.h / 2.0;
    g.vx = g.vy = 0.0;
    g.heading = -1.5707963267948966;
    g.invuln = 3000;
    g.ships--;
    if (g.ships <= 0) {
        g.gameover = 1;
        snprintf(g.banner, sizeof g.banner, "GAME OVER  score %d", g.score);
        g.banner_t0 = clock_ms();
        wos_log_event("minigame_asteroids_gameover", "score=%d wave=%d", g.score, g.wave);
    }
}

/* FUN_0046E4D1 (0x46E4D1): the whole simulation step. */
void minigame_asteroids_update(uint32_t now_ms)
{
    uint32_t dt;
    int i, j, alive = 0;
    if (!g.open || g.gameover) return;
    dt = g.last_step ? now_ms - g.last_step : 0;
    g.last_step = now_ms;
    if (g.invuln > 0) g.invuln = (int)((uint32_t)g.invuln > dt ? (uint32_t)g.invuln - dt : 0);
    if (dt == 0) return;
    {
        double step = (double)dt / 16.0;   /* the original steps once per idle pass */
        /* thrust / turn / drag */
        if (g.key[2]) {                    /* up */
            g.vx += cos(g.heading) * 0.08 * step;
            g.vy += sin(g.heading) * 0.08 * step;
        }
        if (g.key[0]) g.heading -= 0.05 * step;   /* left  */
        if (g.key[1]) g.heading += 0.05 * step;   /* right */
        g.vx *= 0.995;
        g.vy *= 0.995;
        g.sx += g.vx * step;
        g.sy += g.vy * step;
        /* wrap into [0,w) x [0,h) — FUN_0046E0E6 / 0x46EB43 */
        while (g.sx < 0) g.sx += g.w;
        while (g.sx >= g.w) g.sx -= g.w;
        while (g.sy < 0) g.sy += g.h;
        while (g.sy >= g.h) g.sy -= g.h;
        /* bullets */
        for (i = 0; i < MAX_BULLETS; ++i) {
            if (!g.shot[i].alive) continue;
            g.shot[i].x += g.shot[i].vx * step;
            g.shot[i].y += g.shot[i].vy * step;
            if (--g.shot[i].life <= 0) g.shot[i].alive = 0;
            while (g.shot[i].x < 0) g.shot[i].x += g.w;
            while (g.shot[i].x >= g.w) g.shot[i].x -= g.w;
            while (g.shot[i].y < 0) g.shot[i].y += g.h;
            while (g.shot[i].y >= g.h) g.shot[i].y -= g.h;
            /* bullet/asteroid collision. The hit box is SetRect(x,y,x,y) inflated by
             * (size * 0x50) / 100, i.e. 80% of the size on each side. */
            for (j = 0; j < MAX_ASTEROIDS; ++j) {
                double dx, dy, box;
                if (!g.rock[j].alive) continue;
                dx = g.shot[i].x - g.rock[j].x;
                dy = g.shot[i].y - g.rock[j].y;
                box = (double)((g.rock[j].size * HIT_PCT) / 100);
                if (dx * dx + dy * dy > box * box) continue;
                g.shot[i].alive = 0;
                g.rock[j].alive = 0;
                g.kills++;                                  /* this+0x7D8 */
                /* +0x32 (50) below 0x1F, else +0x19 (25); the high score is DAT_004F828C */
                g.score += (g.rock[j].size < 0x1F) ? SCORE_SMALL : SCORE_LARGE;
                if (g.score > g.high) g.high = g.score;
                /* the split: each branch consumes its own rand() whether or not it passes,
                 * so this costs 1, 2 or 3 draws depending on the rolls. */
                if (g.rock[j].size > SPLIT_GATE_SIZE) {
                    double cx = g.rock[j].x, cy = g.rock[j].y;
                    if (crt_rand() % 100 < SPLIT_LARGE_PCT)
                        spawn_asteroid((int)cx, (int)cy, g.rock[j].size * 2 / 3,
                                       (g.wave + 5) * 10);
                    if (crt_rand() % 100 < SPLIT_MED_PCT)
                        spawn_asteroid((int)cx, (int)cy, g.rock[j].size / 2,
                                       (g.wave + 5) * 10);
                    if (crt_rand() % 100 < SPLIT_SMALL_PCT)
                        spawn_asteroid((int)cx, (int)cy, g.rock[j].size / 3,
                                       (g.wave + 5) * 10);
                }
                break;
            }
        }
        /* ship/asteroid collision, skipped while invulnerable */
        if (g.invuln == 0) {
            for (j = 0; j < MAX_ASTEROIDS; ++j) {
                double dx, dy, r;
                if (!g.rock[j].alive) continue;
                dx = g.sx - g.rock[j].x;
                dy = g.sy - g.rock[j].y;
                r = (double)((g.rock[j].size * HIT_PCT) / 100) + 4.0;
                if (dx * dx + dy * dy <= r * r) { respawn(); break; }
            }
        }
    }
    for (i = 0; i < MAX_ASTEROIDS; ++i) if (g.rock[i].alive) alive = 1;
    if (!alive) { g.wave++; level_start(); }
}

void minigame_asteroids_key(int vk, int pressed)
{
    int idx = -1;
    if (!g.open) return;
    switch (vk) {
    case KEY_LEFT:  idx = 0; break;
    case KEY_RIGHT: idx = 1; break;
    case KEY_UP:    idx = 2; break;
    case KEY_DOWN:  idx = 3; break;
    case KEY_FIRE:  idx = 4; break;
    case KEY_PAUSE: idx = 5; break;
    default: return;
    }
    g.key[idx] = pressed;
    /* FUN_0046E457 polls GetAsyncKeyState; the original has no key-message handler, so the
     * port treats a key press as the poll edge and fires on the press only. */
    if (idx == 4 && pressed && !g.gameover) fire();
    if (idx == 5 && pressed) {
        if (g.gameover) minigame_asteroids_close();
    }
}

void minigame_asteroids_click(int x, int y, int pressed)
{
    /* The 0xC5 class has no mouse handler at all. */
    (void)x; (void)y; (void)pressed;
}

void minigame_asteroids_render(Framebuffer *fb, int bx, int by)
{
    Rect win = { bx, by, WIN_W, WIN_H };
    int i;
    char buf[64];
    if (!g.open) return;
    fb_fill(fb, win, 0x000000u);
    for (i = 0; i < STARS; ++i)
        fb_pixel(fb, bx + g.star_x[i], by + g.star_y[i], 0x808080u);
    for (i = 0; i < MAX_ASTEROIDS; ++i) {
        int r;
        if (!g.rock[i].alive) continue;
        r = (int)((g.rock[i].size * HIT_PCT) / 100);
        if (r < 1) r = 1;
        fb_fill(fb, (Rect){ bx + (int)g.rock[i].x - r, by + (int)g.rock[i].y - r, r * 2, r * 2 },
               g.rock[i].grey);
    }
    for (i = 0; i < MAX_BULLETS; ++i) {
        if (!g.shot[i].alive) continue;
        fb_pixel(fb, bx + (int)g.shot[i].x, by + (int)g.shot[i].y, 0xffffffu);
    }
    if (!g.gameover) {
        fb_fill(fb, (Rect){ bx + (int)g.sx - 4, by + (int)g.sy - 4, 8, 8 },
               g.invuln > 0 ? 0x8080ffu : 0x00ff00u);
    }
    snprintf(buf, sizeof buf, "SCORE %d   WAVE %d   SHIPS %d", g.score, g.wave, g.ships);
    font_draw(fb, bx + 6, by + 6, buf, 0xffe6aeu);
    if (g.banner[0]) font_draw(fb, bx + WIN_W / 2 - 40, by + WIN_H / 2, g.banner, 0xffd477u);
}

void minigame_asteroids_dump(DumpEmit emit, void *user)
{
    dump_emit_int(emit, "minigame.asteroids.score", g.score, user);
    dump_emit_int(emit, "minigame.asteroids.high", g.high, user);
    dump_emit_int(emit, "minigame.asteroids.kills", g.kills, user);
    dump_emit_int(emit, "minigame.asteroids.shots", g.shots, user);
    dump_emit_int(emit, "minigame.asteroids.wave", g.wave, user);
    dump_emit_int(emit, "minigame.asteroids.ships", g.ships, user);
    dump_emit_int(emit, "minigame.asteroids.shields", g.shields, user);
    dump_emit_int(emit, "minigame.asteroids.gameover", g.gameover, user);
    dump_emit_int(emit, "minigame.asteroids.sx", (int)g.sx, user);
    dump_emit_int(emit, "minigame.asteroids.sy", (int)g.sy, user);
}
