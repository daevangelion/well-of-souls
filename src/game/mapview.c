/* Offline map screen. Every rule here is transcribed from work/decomp/all.c; the VA is
 * quoted on each function. Nothing in this file is frame-driven: the original's map world
 * is `GetTickCount`-driven and runs on the paint/idle path, so every timer comparison uses
 * clock_ms() with the original's constant (docs/re/timing.md section 7, MapView-2 rows).
 *
 *   FUN_00462958 0x462958  per-pass actor loop: step, camera, link hit, encounter roll
 *   FUN_0046230e 0x46230e  the movement step (start tick, duration, integer step vector)
 *   FUN_004620f3 0x4620F3  start/aim a walk leg; wander bookkeeping at its tail
 *   FUN_00461948 0x461948  arrive; advance the polyline cursor
 *   FUN_004636d3 0x4636D3  nearest link (Manhattan) and the containing link rect
 *   FUN_0046260e 0x46260E  the random-encounter roll (1 or 2 rand() per passing tick)
 *   FUN_00464daf 0x464DAF  the .mon proximity resolver (0..3 rand() per placement)
 *   FUN_004610d9 0x4610D9  idle auto-wander, pumped from the paint handler
 *   FUN_00461b11 0x461B11  grid snap;  FUN_00461b26 0x461B26 four-neighbour clearance
 *   FUN_00461b93 0x461B93  the line march;  FUN_00461cf7 0x461CF7 direction table
 *   FUN_00461d41 0x461D41  one detour step;  FUN_00461dcc 0x461DCC the path builder
 *   FUN_0046206d 0x46206D  click-target validation (three builder runs)
 *   FUN_004639eb 0x4639EB  link sprite + the +0xC0 name draw rule
 */
#include "game.h"
#include "chat.h"
#include "hero.h"
#include "panels.h"
#include "../engine/clock.h"
#include "../engine/dump.h"
#include "../engine/font.h"
#include "../engine/log.h"
#include "../engine/rng.h"
#include "../engine/screen.h"
#include "../engine/text.h"
#include "../platform/platform.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- Layout at a 640x480 client area (FUN_0041b891's OnSize, all.c:20906-20961) -------
 * client 640x480 -> top area 0..416 (bottom -0x40), right column 620..640 (-0x14),
 * left column 0..19, vertical splitter 344..364 (right-0x114), right pane 364..620, and
 * the map pane 19..344 x 0..416 with a horizontal row splitter whose first row is
 * max(0x118, min((325*256)/360, 416-0x5A)) = 0x119 = 281. So the walk view is 325x281
 * screen pixels = (325+3)/4 x (281+3)/4 = 82 x 71 map units, hero hard-centred. */
#define WALK_X     19
#define WALK_Y     0
#define WALK_W     325
#define WALK_H     281
#define SIDE_X     364      /* right pane, this+0x138C */
#define SIDE_W     256
#define TOP_H      416      /* this+0x84 bottom */
#define BOT_Y      416      /* the status window strip, 64 rows */

#define FIGHT_MAX 143
#define BUTTON_COUNT 7
#define PATH_NODES_MAX 2000 /* FUN_00461DCC: bounded 2000-node detour search. */
#define PATH_NODES_MAX_0 0x7D0
/* FUN_004620F3's step arithmetic uses DWORD ms throughout. */
#define MAP_TICK_MS 25      /* FUN_0042895C's own sub-gate, `DVar1 - _DAT_004e48cc < 0x19` */
#define WANDER_MS  2000     /* FUN_004610D9 / FUN_004620F3 */
#define ENC_COOL_MS 5000    /* FUN_0046260E, both cooldowns */
#define BATTLE_GRACE_MS 2000/* FUN_0046259A */
#define SPEED_MAP0 0xC80    /* 3200, map id 0 (all.c:23356 / 71047) */
#define SPEED_OTHER 0x12C0  /* 4800, any other map (all.c:23370 / 71052) */
#define WANDER_LEG 0x2800   /* 40 map units: FUN_004620F3's short-leg test */

static Map map;
static int loaded;
static Sheet skin;
static Image buttons[BUTTON_COUNT];
static const char *const button_names[BUTTON_COUNT] = {
    "Items", "Spells", "Equip", "Stats", "Map", "Camp", "Well"
};

/* --- the actor record fields FUN_004620F3/FUN_0046230E own ---------------- */
static int32_t w_x, w_y;              /* rec+0x94/+0x98, 24.8 map units      */
static int32_t w_target_x, w_target_y;/* rec+0x9C/+0xA0                    */
static int32_t w_step_x, w_step_y;    /* rec+0xA4/+0xA8, per-leg step       */
static int32_t w_leg_x, w_leg_y;       /* rec+0xBC/+0xC0, leg start         */
static int w_speed;                    /* rec+0xAC, 24.8 map units per ms   */
static uint32_t w_tick;                /* rec+0xB0, GetTickCount at leg start */
static int w_duration;                 /* rec+0xB4, (dist*1000)/speed       */
static int face_x = 1, face_y = 1;     /* per-axis facing code, 0/1/2       */

/* --- path state (DAT_004f2168 count, DAT_004f216c cursor) ---------------- */
typedef struct { int x, y; } PathPoint;
static PathPoint path[PATH_NODES_MAX];
static int path_count;                 /* DAT_004f2168 */
static int path_cursor = -1;           /* DAT_004f216c */
static int waypoints_enabled;          /* option 27, "automatic Way Point calculations" */

/* --- idle auto-wander (FUN_004620F3 tail + FUN_004610D9) ----------------- */
static uint32_t wander_tick;           /* _DAT_004f2184 */
static int32_t wander_x, wander_y;     /* DAT_004f2188 / DAT_004f218c */
static int wander_speed;               /* DAT_004f2190 */
static uint32_t wander_legs;           /* _DAT_004f219C */

/* --- encounter state (FUN_0046260E) -------------------------------------- */
static uint32_t enc_a;                 /* _DAT_004f2220 */
static uint32_t enc_b;                 /* _DAT_004f2224 */
static uint32_t battle_end_tick;       /* DAT_004e70a8, set when a fight ends */
static int no_monsters_here;           /* DAT_004f2228 */
static int hunt_recent;                /* DAT_004e70ac */

/* --- link state --------------------------------------------------------- */
static int nearest_link_idx = -1;      /* DAT_004f2240 */
static int hit_link_idx = -1;          /* DAT_004f2244 */
static int link_latched;               /* DAT_004f222c */
static int link_active;                /* DAT_004f2230 */

static int hover = -1, mouse_held, minimap;
static char message[128];
static int pending[FIGHT_MAX], pending_count, pending_difficulty, pending_distance = 20;
static uint32_t last_map_tick;
static int music_index, music_count, music_running;
static int map_flags;

static void map_update(const Input *input);
static void map_render(Framebuffer *fb);
static const Screen map_screen = { "map", NULL, map_update, map_render, NULL };

static int clamp(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static int valid_monster(int id)
{
    return id > 0 && id < WORLD_MAX_MONSTERS && g_world.monsters[id].used;
}

/* FUN_00482B69 (0x482B69) over the map's +0xF8 flag word. Offline the PK-session overlay
 * (DAT_004FB030) and the two PK visibility bits are all zero, so this is `def->flags & m`. */
static int map_flag(unsigned mask)
{
    return (map_flags & mask) != 0;
}

void game_set_pending_fight(const int *ids, int count, int difficulty, int distance_pct)
{
    int i;
    pending_count = 0;
    pending_difficulty = difficulty;
    pending_distance = clamp(distance_pct, 0, 100);
    if (!ids) return;
    for (i = 0; i < count && pending_count < FIGHT_MAX; ++i)
        if (valid_monster(ids[i])) pending[pending_count++] = ids[i];
}

int game_take_pending_fight(int *ids, int max, int *difficulty, int *distance_pct)
{
    int n = clamp(pending_count, 0, max > 0 ? max : 0);
    if (difficulty) *difficulty = pending_difficulty;
    if (distance_pct) *distance_pct = pending_distance;
    if (ids && n) memcpy(ids, pending, (size_t)n * sizeof(*ids));
    if (!ids) n = 0;
    pending_count = 0;
    pending_difficulty = 0;
    pending_distance = 20;
    return n;
}

const Map *game_current_map(void)
{
    return loaded ? &map : NULL;
}

void map_set_waypoints(int on)
{
    waypoints_enabled = on ? 1 : 0;
}

void map_note_battle_end(void)
{
    battle_end_tick = clock_ms();
}

static const ObjRect *object_rect(const Link *link)
{
    int id = link->object_id;
    if (id < 0 || id >= OBR_RECORDS || !map.objrects[id].used) return NULL;
    return &map.objrects[id];
}

static int owns_item(int id)
{
    return id <= 0 || hero_item_count(&g_hero, id) > 0;
}

/* FUN_00479FC4 (0x479FC4) out-flag bit 1 (item+0x2E4 == 12) doubles the walk speed and
 * bit 2 (item+0x2E4 == 11) makes encounters ten times rarer. */
static int movement_effect(int effect)
{
    int i;
    for (i = 0; i < 8; ++i) {
        int id = g_hero.equip[i];
        if (id > 0 && id < WORLD_MAX_ITEMS && g_world.items[id].used &&
            g_world.items[id].movement == effect) return 1;
    }
    return 0;
}

/* FUN_004636D3 (0x4636D3): halfW = (obr.right - obr.left + 7) / 8. */
static void link_extent(const Link *link, int *w, int *h)
{
    const ObjRect *r = object_rect(link);
    *w = r && r->r > r->l ? (r->r - r->l + 7) / 8 : 1;
    *h = r && r->b > r->t ? (r->b - r->t + 7) / 8 : 1;
}

/* FUN_004636D3 (0x4636D3), transcribed. It returns the link whose rect CONTAINS the hero
 * and is usable (kind >= 1 and the party owns +0x1A4) -- that is the `iVar5` FUN_00462958
 * latches on -- while DAT_004f2240 (nearest, Manhattan) and DAT_004f2244 (the hit, which
 * is STICKY across passes when the hero is not inside a usable link) are the globals. The
 * link the hero is standing in also becomes the nearest, with its stored distance +0x190
 * zeroed, which is what FUN_0049099B reads. */
static int map_scan_links(int *distance)
{
    int i, nearest = -1, hit = -1, sticky = hit_link_idx, inside = 0;
    int best = 0x3fffffff, hx = w_x >> 8, hy = w_y >> 8;
    for (i = 0; i < OBL_RECORDS; ++i) {
        const Link *l = &map.links[i];
        int w, h, d;
        if (!l->used || l->object_id < 0) continue;
        link_extent(l, &w, &h);
        if (hx >= l->x - w && hx < l->x + w && hy >= l->y - h && hy < l->y + h) {
            nearest = i; inside = 1;
            if (l->kind >= 1 && owns_item(l->required_item)) { hit = i; sticky = i; }
            break;                     /* (&DAT_00604998)[i*200] = 0; the scan stops here */
        }
        d = abs(hx - l->x) + abs(hy - l->y);
        if (d < best) { best = d; nearest = i; }
    }
    hit_link_idx = sticky;
    nearest_link_idx = nearest;
    *distance = nearest < 0 ? 0 : (inside ? 0 : best);
    return hit;
}

/* FUN_00461DA7 (0x461DA7): count 0, cursor -1, polyline cleared. */
static void path_reset(void)
{
    path_count = 0;
    path_cursor = -1;
    memset(path, 0, sizeof(path));
}

/* FUN_00461B11 (0x461B11): `((v + (v>>31 & 3)) >> 2) * 4 + 2` -- a truncating divide by
 * four with a sign correction, NOT a +3 round-up. Cell centres are 4k+2. */
static int grid_snap(int v)
{
    return ((v + (v >> 31 & 3)) >> 2) * 4 + 2;
}

/* FUN_00461B26 (0x461B26): the cell and all four diagonal neighbours must be walkable.
 * Short-circuits in the original's order, which has no observable effect. */
static int path_clear4(int x, int y)
{
    if (!map_walkable(&map, x, y, g_hero.tokens)) return 0;
    return map_walkable(&map, x - 1, y - 1, g_hero.tokens) &&
           map_walkable(&map, x + 1, y - 1, g_hero.tokens) &&
           map_walkable(&map, x - 1, y + 1, g_hero.tokens) &&
           map_walkable(&map, x + 1, y + 1, g_hero.tokens);
}

/* FUN_00461B93 (0x461B93) / FUN_00461CD7 (0x461CD7). Marches FROM (x0,y0) TO (x1,y1) in
 * 2*max(|dx|,|dy|) half-steps (minimum 1), tests walkability after every advance and the
 * optional four-neighbour clearance BEFORE the goal test at every sample. On failure the
 * outputs are snapped back to the last cell known to be walkable. `clearance` is the
 * original's param_7 and is the ONLY thing that turns the four-neighbour test on; the
 * decomp has no unconditional clearance anywhere, so the port has none either. */
static int line_march(int x1, int y1, int x0, int y0, int *ox, int *oy, int clearance)
{
    int dx = x1 - x0, dy = y1 - y0;
    int steps = 2 * (abs(dy) < abs(dx) ? abs(dx) : abs(dy));
    int accx = 0, accy = 0, i = 0, lastx, lasty, ok = 0;
    if (steps < 1) steps = 1;
    lastx = x0; lasty = y0;
    *ox = x0; *oy = y0;
    if (map_walkable(&map, x0, y0, g_hero.tokens)) {
        for (;;) {
            if (clearance && !path_clear4(*ox, *oy)) break;
            if (*ox == x1 && *oy == y1) { ok = 1; break; }
            if (steps < i) break;
            lastx = *ox; lasty = *oy;
            ++i;
            *ox = x0 + accx / steps; accx += dx;
            *oy = y0 + accy / steps; accy += dy;
            if (!map_walkable(&map, *ox, *oy, g_hero.tokens)) break;
        }
    }
    if (!ok) { *ox = grid_snap(lastx); *oy = grid_snap(lasty); }
    return ok;
}

/* Retail direction tables: 0x4F21A0 / 0x4F21C0 (eight compass steps, south first) and the
 * 4x4 index table 0x4F21E0. FUN_00461CF7 is `(&DAT_004f21e0)[(dy+1&3)<<2 | dx+1&3]` on
 * sign(param_1-param_3), sign(param_2-param_4). The stored table is used verbatim. */
static const int path_dx[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };
static const int path_dy[8] = { 1, 1, 0, -1, -1, -1, 0, 1 };
static const int path_dir3x3[16] = {
    5, 4, 3, 0,
    6, 0, 2, 0,
    7, 0, 1, 0,
    0, 0, 0, 0
};

static int path_direction(int x1, int y1, int x0, int y0)
{
    int x = (x1 > x0) - (x1 < x0);
    int y = (y1 > y0) - (y1 < y0);
    return path_dir3x3[((y + 1) & 3) << 2 | ((x + 1) & 3)];
}

/* FUN_00461D41 (0x461D41): step one cell in `dir` and march to it. Returns 1 when the
 * step is BLOCKED, i.e. the original's `iVar2 == 0` with iVar2 the march result. */
static int path_step(int x, int y, int dir, int *ox, int *oy)
{
    int nx = grid_snap(x + path_dx[dir] * 4);
    int ny = grid_snap(y + path_dy[dir] * 4);
    int lx, ly;
    *ox = nx; *oy = ny;
    return line_march(nx, ny, x, y, &lx, &ly, 0) ? 0 : 1;
}

/* FUN_00461DCC (0x461DCC). Arguments are (target, start, turn) exactly as the original
 * receives them from FUN_0046206D; node[0] is the start, the last node is the target, and
 * the four-neighbour clearance is used in exactly two places: the "drop the previous node"
 * shortcut test after a detour, and the string-pulling pass at the end. */
static int path_build(int x1, int y1, int x0, int y0, int turn)
{
    int budget = PATH_NODES_MAX_0;   /* [ebp-0x14], the detour-step budget */
    int prev = 0;                    /* iVar10, last kept node */
    int count = 1;
    int cx = x0, cy = y0;            /* current node */
    int px = x1, py = y1;            /* previous candidate */
    int dir, lx, ly, l2x, l2y, straight;
    path_reset();
    path[0].x = x0; path[0].y = y0;
    if (line_march(x1, y1, x0, y0, &lx, &ly, 0)) {
        path[1].x = x1; path[1].y = y1;
        path_count = 2;
        return 2;
    }
    dir = path_direction(x1, y1, x0, y0);
    for (;;) {
        straight = line_march(x1, y1, cx, cy, &lx, &ly, 0);
        if (!straight) {
            int tries = 10, blocked, bx = 0, by = 0;
            blocked = path_step(cx, cy, dir, &bx, &by);
            while (blocked && tries > 0) {
                px = bx; py = by;
                dir = (dir + (turn ? 1 : 7)) & 7;
                blocked = path_step(cx, cy, dir, &bx, &by);
                --tries;
            }
            if (count > 1 && line_march(bx, by, path[prev].x, path[prev].y, &l2x, &l2y, 1))
                --count;                       /* the previous node is redundant */
            else
                prev = count - 1;
            if (count >= PATH_NODES_MAX) { path_count = 0; return PATH_NODES_MAX_0; }
            path[count].x = bx; path[count].y = by;
            ++count;
            cx = bx; cy = by;
            dir = path_direction(px, py, cx, cy);
        } else {
            if (count >= PATH_NODES_MAX) { path_count = 0; return PATH_NODES_MAX_0; }
            path[count].x = x1; path[count].y = y1;
            ++count;
        }
        if (straight) {
            int at = 0;
            while (at < count - 2) {
                int far = count - 1, got = 0;
                while (at + 1 < far) {
                    if (line_march(path[at].x, path[at].y, path[far].x, path[far].y,
                                   &l2x, &l2y, 1)) { got = 1; break; }
                    --far;
                }
                if (got) {
                    int removed = far - at - 1;
                    if (far < count)
                        memmove(&path[at + 1], &path[far], (size_t)(count - far) * sizeof(*path));
                    count -= removed;
                }
                ++at;
            }
            path_count = count;
            return count;
        }
        if (count >= PATH_NODES_MAX_0 || budget <= 0) { path_count = 0; return PATH_NODES_MAX_0; }
        --budget;
    }
}

/* FUN_0046206D (0x46206D): start and target are snapped, the builder runs with turn 1,
 * then turn 0, and turn 1 again whenever the first run was not strictly longer. The
 * builder's globals are overwritten by every run, so the LAST run is the one that counts. */
static void path_validate(int hx, int hy, int tx, int ty)
{
    int sx = grid_snap(hx), sy = grid_snap(hy);
    int a = path_build(tx, ty, sx, sy, 1);
    int b = path_build(tx, ty, sx, sy, 0);
    if (a <= b) b = path_build(tx, ty, sx, sy, 1);
    if (b < PATH_NODES_MAX && path_count > 2) path_cursor = 1;
}

/* FUN_004620F3 (0x4620F3). The leg is a straight interpolated line from the leg start at a
 * fixed integer step, with a duration in whole milliseconds; FUN_0046230E interpolates
 * from that. `player` is the original's param_5 and gates the waypoint re-validation. */
static void walk_to(int32_t tx, int32_t ty, int speed, int player)
{
    int dx, dy, dist;
    uint32_t now = clock_ms();
    if (!map_walkable(&map, tx >> 8, ty >> 8, g_hero.tokens)) return;
    if (player && waypoints_enabled && !map_flag(0x80000u)) {
        path_validate(w_x >> 8, w_y >> 8, tx >> 8, ty >> 8);
        if (path_cursor > 0) {
            tx = path[path_cursor].x << 8;
            ty = path[path_cursor].y << 8;
            wos_log_event("path_found", "nodes=%d x=%d y=%d", path_count,
                          (tx >> 8), (ty >> 8));
        } else {
            wos_log_event("path_none", "x=%d y=%d", tx >> 8, ty >> 8);
        }
    }
    if (speed < 1) speed = 1;
    w_target_x = tx; w_target_y = ty;
    dx = (int)(tx - w_x); dy = (int)(ty - w_y);
    dist = (int)sqrt((double)dx * dx + (double)dy * dy);
    if (dist < 1) dist = 1;
    w_leg_x = w_x; w_leg_y = w_y;
    w_step_x = speed * dx / dist;
    w_step_y = speed * dy / dist;
    w_speed = speed;
    w_tick = now;
    w_duration = dist * 1000 / speed;
    /* FUN_004620F3 tail: a short leg that follows another within two seconds becomes the
     * idle wander target; anything else clears the wander and re-stamps the timer. */
    if ((uint32_t)(now - wander_tick) < WANDER_MS && dist < WANDER_LEG) {
        wander_x = tx; wander_y = ty; wander_speed = speed;
        ++wander_legs;
    } else {
        wander_tick = now;
        wander_x = wander_y = 0; wander_speed = 0;
        wos_log_event("walk_reset", "x=%d y=%d", w_x >> 8, w_y >> 8);
    }
}

/* FUN_00461948 (0x461948) with param_4 == 1. */
static void walk_arrive(int32_t tx, int32_t ty, int advance)
{
    int speed = w_speed;
    w_x = tx; w_y = ty;
    w_leg_x = tx; w_leg_y = ty;
    w_speed = 0; w_tick = 0;
    g_hero.x = w_x >> 8; g_hero.y = w_y >> 8;
    if (!advance || path_cursor < 1 || path_count - 1 <= path_cursor) {
        wos_log_event("hero_move", "x=%d y=%d", w_x >> 8, w_y >> 8);
        return;
    }
    ++path_cursor;
    walk_to(path[path_cursor].x << 8, path[path_cursor].y << 8, speed, 0);
}

/* FUN_0046230E's per-axis facing rule: |delta| < 0x14 keeps the axis neutral (1), else
 * increasing is 2 and decreasing is 0. The y axis reuses the x code when it does not move
 * at all, which is what the decomp's `if (iVar5 <= iVar2) uVar4 = local_8` does. */
static int face_axis(int oldv, int newv, int other)
{
    if (abs(newv - oldv) < 0x14) return 1;
    if (oldv < newv) return 2;
    if (newv < oldv) return 0;
    return other;
}

/* FUN_0046230E (0x46230E). Returns `speed > 0` AFTER the step, which is the "moving" flag
 * FUN_00462958 hands to the encounter roll. */
static int walk_step(uint32_t now)
{
    if (w_speed > 0) {
        int32_t elapsed = (int32_t)(uint32_t)(now - w_tick);
        if (w_duration < elapsed) {
            walk_arrive(w_target_x, w_target_y, 1);
        } else {
            int32_t nx = w_leg_x + (int32_t)((int64_t)w_step_x * elapsed / 1000);
            int32_t ny = w_leg_y + (int32_t)((int64_t)w_step_y * elapsed / 1000);
            int fx, fy;
            if (nx < 0) nx = 0;
            if (ny < 0) ny = 0;
            fx = face_axis((int)w_x, (int)nx, face_x);
            fy = face_axis((int)w_y, (int)ny, fx);
            face_x = fx; face_y = fy;
            if (!map_walkable(&map, nx >> 8, ny >> 8, g_hero.tokens))
                walk_arrive(w_x, w_y, 1);
            else { w_x = nx; w_y = ny; }
        }
    }
    g_hero.x = w_x >> 8; g_hero.y = w_y >> 8;
    return w_speed > 0;
}

/* FUN_00461A07 (0x461A07) = FUN_00461948(..., param_4 = 0): place and stop, no advance. */
static void walk_stop(int32_t x, int32_t y)
{
    w_x = x; w_y = y;
    w_leg_x = x; w_leg_y = y;
    w_speed = 0; w_tick = 0;
    g_hero.x = x >> 8; g_hero.y = y >> 8;
}

static void load_art(void)
{
    char rel[96], path[1024];
    int i;
    sheet_free(&skin);
    sheet_load_skin(&skin, g_hero.skin);
    for (i = 0; i < BUTTON_COUNT; ++i) {
        image_free(&buttons[i]);
        snprintf(rel, sizeof(rel), "art/button%s.bmp", button_names[i]);
        if (image_load(&buttons[i], world_path(path, sizeof(path), rel)) != 0)
            image_load(&buttons[i], world_data_path(path, sizeof(path), rel));
    }
}

/* --- sounds -------------------------------------------------------------
 * FUN_00429C9C (0x429C9C) -> FUN_00429A31 (0x429A31) -> "sfx\%s" (0x4E7A88) resolved by
 * FUN_0042445E (0x42445E) as <data>/<world>/sfx/<name>, then <data>/themes/<theme>/<name>,
 * then <data>/<name>. Played on channel 1 with mode 0, which in FUN_0046890D (0x46890D)
 * dispatches to the mixer's own play (vtable +0x48): there is NO pan or volume argument
 * anywhere on this path, so walk.wav is not positional. */
static void play_sfx(const char *name)
{
    char rel[128], path[1024];
    unsigned char *bytes;
    size_t len;
    snprintf(rel, sizeof(rel), "SFX/%s", name);
    bytes = (unsigned char *)text_read_file(world_path(path, sizeof(path), rel), &len);
    if (!bytes) bytes = (unsigned char *)text_read_file(world_data_path(path, sizeof(path), rel), &len);
    if (!bytes) { wos_log_event("sound_missing", "name=%s", name); return; }
    plat_sound_play(bytes, len);
    free(bytes);
    wos_log_event("sound", "name=%s", name);
}

/* --- map music: FUN_00436FFC (0x436FFC) + FUN_00468F4F (0x468F4F) -------------
 * The playlist cursor DAT_004e70b8 is zeroed by the map loader (all.c:23179), wraps with
 * `% numMidi` at lookup time and is advanced by the MCI notify thread when a track ends
 * (all.c:76196) unless the caller passed loop = 0. Map music always passes loop = 1. */
static void map_music_track(void)
{
    char key[32], fallback[96];
    const char *name;
    music_index %= music_count;
    snprintf(key, sizeof(key), "midi%d", music_index + 1);
    name = world_music(map.def->root, key);
    if (!name || !*name) {
        snprintf(fallback, sizeof(fallback), "%s.MID", map.def->root);
        name = fallback;
    }
    game_music(name);
    music_running = plat_music_playing();
}

static void map_music_start(void)
{
    music_count = world_music_count(map.def->root);
    if (music_count < 1) music_count = 1;
    map_music_track();
}

static void map_music_update(void)
{
    if (music_running && !plat_music_playing()) {
        music_index = (music_index + 1) % music_count;
        map_music_track();
    }
}

/* FUN_00463853 (0x463853): record the link, drop any wander and any path, then dispatch. */
static void activate_link(int index)
{
    Link link = map.links[index];
    w_speed = 0; w_tick = 0;
    wander_x = wander_y = 0; wander_speed = 0;
    path_reset();
    g_hero.link = index;
    if (link.kind == 2) game_enter_map(link.dest_map, link.target, 0);
    else if (link.kind == 1 || link.kind == 3 || link.kind == 4)
        game_enter_scene(link.target, &map.links[index]);
}

static void place_at_link(int link)
{
    int w, h, fx, fy, radius, found = 0;
    if (link < 0 || link >= OBL_RECORDS || !map.links[link].used) {
        fx = clamp(g_hero.x, 0, map.image.w - 1);
        fy = clamp(g_hero.y, 0, map.image.h - 1);
    } else {
        g_hero.link = link;
        link_extent(&map.links[link], &w, &h);
        fx = map.links[link].x;
        fy = map.links[link].y - (h + 1);
    }
    /* Above-link placement must not strand the hero in impassable terrain. */
    if (!map_walkable(&map, fx, fy, g_hero.tokens))
        for (radius = 1; radius <= 64 && !found; ++radius) {
            int dx, dy;
            for (dy = -radius; dy <= radius && !found; ++dy)
                for (dx = -radius; dx <= radius; ++dx) {
                    if (abs(dx) != radius && abs(dy) != radius) continue;
                    if (map_walkable(&map, fx + dx, fy + dy, g_hero.tokens)) {
                        fx += dx; fy += dy; found = 1; break;
                    }
                }
        }
    fx = clamp(fx, 0, map.image.w - 1);
    fy = clamp(fy, 0, map.image.h - 1);
    walk_stop((int32_t)fx << 8, (int32_t)fy << 8);
}

static void map_reset_runtime(int link)
{
    w_speed = 0; w_tick = 0;
    path_reset();
    wander_x = wander_y = 0; wander_speed = 0;
    wander_tick = clock_ms();
    /* DAT_004f222c is never cleared explicitly by the original; it falls out on its own
     * when the hero is not standing in a link, so the map entry does not touch it either. */
    nearest_link_idx = hit_link_idx = -1;
    no_monsters_here = 0;
    enc_a = enc_b = 0;
    face_x = face_y = 1;
    minimap = 0;
    battle_end_tick = clock_ms();
    last_map_tick = clock_ms();
    place_at_link(link);
    snprintf(message, sizeof(message), "%s", map.def->name);
}

void game_enter_map(int map_id, int link, int drop_in)
{
    if (map_id < 0 || map_id >= WORLD_MAX_MAPS || !g_world.maps[map_id].used) {
        wos_log_event("map_error", "map=%d reason=undefined", map_id);
        game_go_well();
        return;
    }
    if (!loaded || map.id != map_id) {
        if (loaded) map_free(&map);
        loaded = 0;
        if (map_load(&map, map_id) != 0) {
            wos_log_event("map_error", "map=%d reason=load", map_id);
            game_go_well();
            return;
        }
        loaded = 1;
    }
    g_hero.map = map_id;
    map_flags = (int)map.def->flags;
    pending_count = pending_difficulty = 0;
    music_index = 0;              /* DAT_004e70b8 = 0 in the map loader */
    map_reset_runtime(drop_in ? -1 : link);
    load_art();
    screen_set(&map_screen);
    wos_log_event("map_enter", "map=%d x=%d y=%d", map_id, g_hero.x, g_hero.y);
    map_music_start();
    if (drop_in && link >= 0 && link < OBL_RECORDS && map.links[link].used)
        activate_link(link);
}

void game_return_to_map(void)
{
    if (!loaded || map.id != g_hero.map) { game_enter_map(g_hero.map, g_hero.link, 0); return; }
    map_reset_runtime(g_hero.link);
    screen_set(&map_screen);
    wos_log_event("map_enter", "map=%d x=%d y=%d", map.id, g_hero.x, g_hero.y);
    map_music_start();
}

/* FUN_00461138 (0x461138): viewW = (clientW + 3) / 4 map units, origin = hero - viewW/2. */
static void camera(int *x, int *y)
{
    int vw = (WALK_W + 3) / 4, vh = (WALK_H + 3) / 4;
    *x = (w_x >> 8) - vw / 2;
    *y = (w_y >> 8) - vh / 2;
}

static Rect button_rect(int i)
{
    Rect r = { 619 - 48 - 3 - (i % 5) * 51, 8 + (i / 5) * 51, 48, 48 };
    return r;
}

static int contains(Rect r, int x, int y)
{
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

/* FUN_0046259A (0x46259A): the post-battle grace window. 2 s after a fight ends it clears
 * the stamp and, if the last roll said "no monsters here", prints that line. Returns
 * (elapsed*100)/2000, i.e. non-zero once 20 ms have passed. */
static int battle_grace(uint32_t now, uint32_t window)
{
    uint32_t elapsed;
    if (battle_end_tick == 0) return 0;
    elapsed = now - battle_end_tick;
    if (elapsed < BATTLE_GRACE_MS) {
        if (window <= elapsed) return (int)((uint32_t)elapsed * 100u / BATTLE_GRACE_MS);
    } else {
        battle_end_tick = 0;
        if (no_monsters_here)
            wos_log_event("message", "text=There are no monsters here, hunt somewhere else.");
    }
    return 0;
}

/* FUN_00436B03 (0x436B03) / FUN_00436B15 (0x436B15). */
static int hunting_level(int raw)
{
    int bits = 0, v = raw;
    while (v > 0) { ++bits; v >>= 1; }
    bits -= 3;
    return bits < 0 ? 0 : bits;
}

/* FUN_0046260E (0x46260E), the hero tail of the actor loop. Returns the original's
 * *param_3 state (2 = "the hero's own random encounter fired") or 0. Consumes exactly one
 * crt_rand() for the first roll and, only when the first clause did not win, one more for
 * the second -- 0, 1 or 2 rands per passing tick, in that order. */
static int encounter_roll(int nearest, int moving, uint32_t now)
{
    const Link *link;
    int base, level, r, hit;
    if (!(moving || battle_grace(now, 1000))) return 0;
    if (nearest < 0) return 0;
    link = &map.links[nearest];
    /* link[+104] < 2 and either no .mon table, or an empty one, or the hero is standing
     * on a group-0 link -> "There are no monsters here". */
    if (link->difficulty < 2 && (!map.mon_count || link->difficulty != 0 || hit_link_idx == nearest)) {
        no_monsters_here = 1;
        return 0;
    }
    no_monsters_here = 0;
    base = movement_effect(11) ? 20 : 200;
    level = hunting_level(g_hero.hunting);
    r = crt_rand();
    hit = (uint32_t)(r % 10000) < (uint32_t)base * 3u / (uint32_t)(level + 3) &&
          (uint32_t)(now - enc_a) > ENC_COOL_MS && moving;
    if (!hit) {
        r = crt_rand();
        hit = ((uint32_t)(r % 10000) < (uint32_t)g_hero.hunting || hunt_recent > 10) &&
              ((uint32_t)(now - enc_b) > ENC_COOL_MS) && battle_grace(now, 0);
    }
    if (!hit) return 0;
    enc_a = now; enc_b = now;
    hunt_recent = 0;
    wos_log_event("encounter_roll", "nearest=%d base=%d level=%d", nearest, base, level);
    return 2;
}

static void add_monster(int *ids, int *n, int id)
{
    if (*n < FIGHT_MAX && valid_monster(id)) ids[(*n)++] = id;
}

/* FUN_00464DAF (0x464DAF), the .mon proximity resolver. Distance is EUCLIDEAN in doubles
 * and the radius is a plain double compare; the two inner bands use the .rdata doubles
 * 0.5 (0x4CD548) and 0.25 (0x4CD578). Rands: one for the 25 % gate, one inside the 0.5
 * band, one inside the 0.25 band -- each consumed only when its band is entered. */
static int encounter_mon_roster(int *ids)
{
    int n = 0, i, hx = w_x >> 8, hy = w_y >> 8;
    for (i = 0; i < MON_RECORDS; ++i) {
        const MonPlace *m = &map.mons[i];
        double d, radius;
        if (!valid_monster(m->monster_id) || m->radius <= 0) continue;
        radius = (double)m->radius;
        d = sqrt((double)(m->x - hx) * (m->x - hx) + (double)(m->y - hy) * (m->y - hy));
        if (d >= radius) continue;
        if (crt_rand() % 100 < 25) add_monster(ids, &n, m->monster_id);
        if (d < radius * 0.5) {
            add_monster(ids, &n, m->monster_id);
            if (crt_rand() % 100 < 15) add_monster(ids, &n, m->monster_id);
        }
        if (d < radius * 0.25) {
            add_monster(ids, &n, m->monster_id);
            if (crt_rand() % 100 < 5) add_monster(ids, &n, m->monster_id);
        }
    }
    return n;
}

/* FUN_0049099B (0x49099B) group selection lives in the battle module; the .mon fallback
 * and the distance percentage are the map's. */
static void encounter_start(int nearest, int distance, const Link *link)
{
    int ids[FIGHT_MAX], n, pct, group = link->difficulty;
    n = encounter_mon_roster(ids);
    if (!n && (!group || group >= WORLD_MAX_GROUPS ||
               !g_world.groups[group].used || g_world.groups[group].count < 1)) return;
    pct = clamp(distance, 20, 80);
    if (group < 0) { group = -group; pct = 100 - pct; }
    w_speed = 0; w_tick = 0;
    g_hero.link = nearest;
    game_set_pending_fight(ids, n, link->difficulty, pct);
    wos_log_event("encounter", "group=%d distance=%d monsters=%d", link->difficulty, pct, n);
    play_sfx("fight.wav");
    game_enter_scene(2, link);
}

/* FUN_00462958 (0x462958), the hero's slice. `hit` is the link the hero is standing in,
 * `state` is FUN_0046260E's return (0, or "self" when the hero's own roll fired; the
 * original's self id is DAT_004dd20c, which is always nonzero because of its 0x40000000
 * tag at 0x4C3xxx, so a fired roll is non-zero and takes the PostMessage branch). */
static void map_tick(uint32_t now)
{
    int moving, distance = 0, hit, state, nearest;
    moving = walk_step(now);
    hit = map_scan_links(&distance);
    nearest = nearest_link_idx;
    state = encounter_roll(nearest, moving, now);
    /* FUN_00462958: `if (nearest < 0 || hero[+0xAC] != 0) latch = 0;` -- +0xAC is the
     * leg speed, so a hero still walking has its latch cleared and re-fires on arrival. */
    if (hit < 0 || w_speed != 0) link_latched = 0;
    else if (!link_latched && (map.links[hit].kind != 4 || state == 0)) {
        link_latched = 1; link_active = 1;
        wos_log_event("link_used", "map=%d link=%d", g_hero.map, hit);
        map.links[hit].has_been_used = 1;
        play_sfx("link.wav");
        activate_link(hit);
    }
    /* `if (state == 0 || latched) active = 0; else if (!active) { stop; latch; fight; }` */
    if (state == 0 || link_latched) link_active = 0;
    else if (!link_active) {
        walk_stop(w_x, w_y);                 /* FUN_00461A07 */
        link_active = 1; link_latched = 1;
        encounter_start(nearest, distance, &map.links[nearest]);
    }
}

/* FUN_004610D9 (0x4610D9): pumped from the paint handler. */
static void wander_pump(void)
{
    uint32_t now;
    if (wander_speed == 0) return;
    now = clock_ms();
    if ((uint32_t)(now - wander_tick) > WANDER_MS) {
        ++wander_legs;
        walk_to(wander_x, wander_y, wander_speed, 0);
        wander_tick = now;
        wander_x = wander_y = 0; wander_speed = 0;
    }
}

/* FUN_00462B5D (0x462B5D) + FUN_004622F4 (0x4622F4): the one and only way the hero is
 * made to walk. walk.wav plays only when the target cell passes FUN_004631C6. */
static void click_walk(int mx, int my)
{
    int speed;
    mx = clamp(mx, 0, map.image.w - 1);
    my = clamp(my, 0, map.image.h - 1);
    if (!map_walkable(&map, mx, my, g_hero.tokens)) return;
    speed = map.id == 0 ? SPEED_MAP0 : SPEED_OTHER;
    if (movement_effect(12)) speed *= 2;
    play_sfx("walk.wav");
    walk_to((int32_t)mx << 8, (int32_t)my << 8, speed, 1);
}

static char map_gate_owner[8];
#define MAP_GATE_ID 7

static void map_update(const Input *in)
{
    int i, dx, dy;
    map_music_update();
    if (chat_update(in)) return;
    if (panel_active()) { panel_update(in); return; }
    hover = -1;
    mouse_held = (in->mouse_down & (1u << 1)) != 0;
    for (i = 0; i < BUTTON_COUNT; ++i)
        if (contains(button_rect(i), in->mouse_x, in->mouse_y)) hover = i;
    if ((in->mouse_pressed & (1u << 1)) && hover >= 0) {
        w_speed = 0; w_tick = 0;
        if (hover == 5) { game_enter_scene(1, NULL); return; }
        if (hover == 6) { hero_save(&g_hero); game_go_well(); return; }
        if (hover < 4) {
            static const PanelKind kinds[] = { PANEL_ITEMS, PANEL_SPELLS, PANEL_EQUIP, PANEL_STATS };
            minimap = 0;
            panel_open(kinds[hover]);
            return;
        }
        minimap = !minimap;
    }
    if (in->pressed[PLAT_KEY_ESCAPE]) { w_speed = 0; w_tick = 0; minimap = 0; }
    if (minimap) return;
    /* The click is converted with a truncating divide by four against the view origin. */
    if ((in->mouse_pressed & (1u << 1)) &&
        in->mouse_x >= WALK_X && in->mouse_x < WALK_X + WALK_W &&
        in->mouse_y >= WALK_Y && in->mouse_y < WALK_Y + WALK_H) {
        int cx, cy, px = in->mouse_x - WALK_X, py = in->mouse_y - WALK_Y;
        camera(&cx, &cy);
        click_walk(cx + (px + (px >> 31 & 3)) / 4, cy + (py + (py >> 31 & 3)) / 4);
    }
    /* FUN_00462958 runs from the map pass, which the original gates at 25 ms
     * (FUN_0042895C, `GetTickCount() - _DAT_004e48cc < 0x19`). Nothing here is per-frame. */
    {
        uint32_t now = clock_ms();
        if (clock_gate(map_gate_owner, MAP_GATE_ID, MAP_TICK_MS)) {
            last_map_tick = now;
            map_tick(now);
        }
    }
    /* The original has no keyboard walking (maps.md section 6: click-to-walk only). The
     * arrow keys stay as pure extra input, as the parity plan allows: while one is held we
     * re-issue the click the equivalent mouse click would make, once per map tick, through
     * exactly the same FUN_004631C6 gate and FUN_004622F4 call. It adds no rule the original
     * does not have and changes none of the timings. */
    dx = !!in->down[PLAT_KEY_RIGHT] - !!in->down[PLAT_KEY_LEFT];
    dy = !!in->down[PLAT_KEY_DOWN] - !!in->down[PLAT_KEY_UP];
    if (dx || dy) {
        int32_t tx = (int32_t)((w_x >> 8) + dx) << 8;
        int32_t ty = (int32_t)((w_y >> 8) + dy) << 8;
        /* Re-issue the click only when the leg is finished or is aimed elsewhere, so a
         * held key walks cell by cell at the original's leg speed instead of restarting
         * the interpolation every frame. */
        if (w_speed == 0 || w_target_x != tx || w_target_y != ty)
            click_walk((w_x >> 8) + dx, (w_y >> 8) + dy);
    }
}

static void meter(Framebuffer *fb, int x, int y, const char *label, int cur, int max, uint32_t color)
{
    char text[48];
    int width = max > 0 ? (int)((int64_t)clamp(cur, 0, max) * 128 / max) : 0;
    fb_fill(fb, (Rect){x, y, 132, 16}, 0x080808);
    fb_fill(fb, (Rect){x + 2, y + 2, width, 12}, color);
    snprintf(text, sizeof(text), "%s %d/%d", label, cur, max);
    font_draw(fb, x + 3, y + 4, text, 0xffffff);
}

static void draw_minimap(Framebuffer *fb)
{
    int x, py, iw = map.image.w, ih = map.image.h, w = 312, h;
    if (iw < 1) iw = 1;
    if (ih < 1) ih = 1;
    h = ih * 312 / iw;
    fb_fill(fb, (Rect){12, 20, 340, 376}, 0x14202b);
    fb_rect(fb, (Rect){12, 20, 340, 376}, 0xb8a67d);
    font_draw(fb, 24, 34, "Map", 0xffdc94);
    if (h > 296) { h = 296; w = iw * h / ih; }
    for (py = 0; py < h; ++py)
        for (x = 0; x < w; ++x)
            fb_pixel(fb, 24 + x, 60 + py, map.image.pixels[(py * ih / h) * iw + x * iw / w]);
    fb_fill(fb, (Rect){23 + g_hero.x * w / iw, 59 + g_hero.y * h / ih, 3, 3}, 0xff2020);
    font_draw(fb, 24, 380, "Escape to return", 0xb8a67d);
}

/* FUN_00461684 (0x461684): the /terrain debug overlay, DAT_004df8b4, one 16x16 screen-pixel
 * FillSolidRect per non-zero 4x4-map-unit cell, drawn after the objects. */
static void draw_terrain_grid(Framebuffer *fb)
{
    int tx, ty, cx, cy;
    if (!map.terrain.indices) return;
    camera(&cx, &cy);
    for (ty = 0; ty < map.terrain.h; ++ty)
        for (tx = 0; tx < map.terrain.w; ++tx) {
            unsigned char t = map.terrain.indices[(size_t)ty * map.terrain.w + tx];
            if (!t) continue;
            fb_fill(fb, (Rect){tx * 4 - cx * 4, ty * 4 - cy * 4, 16, 16},
                    map.terrain.palette[t & 0xff]);
        }
}

/* FUN_004644EE (0x4644EE): the /monsters debug overlay, DAT_004df8b8. */
static void draw_monster_blobs(Framebuffer *fb)
{
    int i, cx, cy;
    camera(&cx, &cy);
    for (i = 0; i < MON_RECORDS; ++i) {
        const MonPlace *m = &map.mons[i];
        int sx, sy, r;
        if (!valid_monster(m->monster_id)) continue;
        sx = m->x * 4 - cx * 4;
        sy = m->y * 4 - cy * 4;
        r = m->radius * 4;
        if (r < 4) r = 4;
        fb_rect(fb, (Rect){sx - r, sy - r, r * 2, r * 2}, 0x40ff40);
    }
}

/* FUN_004639EB (0x4639EB): the link sprite is the objects.obr rect stretched 4x and centred
 * on the link; the +0xC0 name is drawn above it (OffsetRect 0,-0x20; InflateRect 200,0)
 * only when hasBeenUsed (+0x1A0) is set, or the link is record 0, or map flag 32 is set
 * (FUN_00482BB8(0x20)), AND the party owns the required item (+0x1A4). */
static void draw_links(Framebuffer *fb, int cx, int cy)
{
    int i;
    for (i = 0; i < OBL_RECORDS; ++i) {
        const Link *l = &map.links[i];
        const ObjRect *r = object_rect(l);
        int w, h, sx, sy, text_w;
        if (!l->used || !r || !map.objects.pixels) continue;
        w = r->r - r->l; h = r->b - r->t;
        if (w <= 0 || h <= 0) continue;
        sx = l->x * 4 - cx * 4;
        sy = l->y * 4 - cy * 4;
        if (sx + w < 0 || sx > WALK_W || sy + h < 0 || sy > WALK_H) continue;
        fb_blit_sub(fb, &map.objects, (Rect){r->l, r->t, w, h}, sx, sy, 0, map.objects.pixels[0]);
        if (!l->name[0]) continue;
        if (!(l->has_been_used || i == 0 || map_flag(32u))) continue;
        if (!owns_item(l->required_item)) continue;
        text_w = font_width(l->name);
        font_draw(fb, sx + w / 2 - text_w / 2, sy - 0x20, l->name, 0xffffff);
    }
}

static void map_render(Framebuffer *fb)
{
    int cx, cy, i, sx, sy, size;
    char text[128];
    fb_clear(fb, 0x121820);
    camera(&cx, &cy);
    fb_clip(fb, (Rect){WALK_X, WALK_Y, WALK_W, WALK_H});
    fb_fill(fb, (Rect){WALK_X, WALK_Y, WALK_W, WALK_H}, 0);
    fb_blit_sub(fb, &map.image_x4, (Rect){cx * 4, cy * 4, WALK_W, WALK_H}, WALK_X, WALK_Y, 0, -1);
    draw_links(fb, cx, cy);
    if (chat_overlay_terrain()) draw_terrain_grid(fb);
    if (chat_overlay_monsters()) draw_monster_blobs(fb);
    sx = (w_x >> 8) * 4 - cx * 4;
    sy = (w_y >> 8) * 4 - cy * 4;
    size = skin.cell / 3;
    for (i = -3; i <= 3; ++i) {
        int half = 8 - i * i / 2;
        fb_fill(fb, (Rect){sx - half, sy + size / 2 - 3 + i, half * 2 + 1, 1}, 0x263124);
    }
    sheet_draw_map_dir(fb, &skin, face_y * 3 + face_x, sx - size / 2, sy - size / 2);
    fb_reset_clip(fb);
    fb_fill(fb, (Rect){SIDE_X, 0, SIDE_W, TOP_H}, 0x202934);
    for (i = 0; i < BUTTON_COUNT; ++i) {
        Rect r = button_rect(i);
        int state = hover == i ? (mouse_held ? 2 : 3) : 1;
        const Image *img = &buttons[i];
        fb_fill(fb, r, 0x34414d);
        if (img->pixels && img->w >= (state + 1) * 48 && img->h >= 48)
            fb_blit_sub(fb, img, (Rect){state * 48, 0, 48, 48}, r.x, r.y, 0, img->pixels[0]);
        else font_draw(fb, r.x + 2, r.y + 20, button_names[i], 0xffffff);
    }
    fb_fill(fb, (Rect){SIDE_X, 116, 256, 256}, 0x151e28);
    fb_rect(fb, (Rect){SIDE_X, 116, 256, 256}, 0x82795f);
    font_wrap(fb, (Rect){SIDE_X + 12, 128, 232, 20}, g_hero.name, 0xffdc94);
    snprintf(text, sizeof(text), "Level %d", g_hero.level);
    font_draw(fb, SIDE_X + 12, 156, text, 0xffffff);
    fb_clip(fb, (Rect){SIDE_X + 12, 176, 232, 128});
    sheet_draw(fb, &skin, 1, SIDE_X + 12, 176, 0);
    fb_reset_clip(fb);
    snprintf(text, sizeof(text), "HP %d/%d  MP %d/%d", g_hero.hp, g_hero.max_hp, g_hero.mp, g_hero.max_mp);
    font_wrap(fb, (Rect){SIDE_X + 12, 310, 232, 24}, text, 0xffffff);
    snprintf(text, sizeof(text), "Gold %lld", (long long)g_hero.gold);
    font_draw(fb, SIDE_X + 12, 346, text, 0xffdc94);
    if (hover >= 0) font_draw(fb, SIDE_X + 12, 390, button_names[hover], 0xffffff);
    fb_fill(fb, (Rect){0, BOT_Y, 640, 64}, 0x101820);
    meter(fb, 8, BOT_Y + 7, "HP", g_hero.hp, g_hero.max_hp, 0x2100a5);
    meter(fb, 148, BOT_Y + 7, "MP", g_hero.mp, g_hero.max_mp, 0xff8080);
    snprintf(text, sizeof(text), "%s (%d,%d)", map.def->name, g_hero.x, g_hero.y);
    font_wrap(fb, (Rect){292, BOT_Y + 9, 336, 16}, text, 0xc8cfdb);
    font_wrap(fb, (Rect){8, BOT_Y + 35, 624, 24}, message, 0xffffff);
    if (minimap) draw_minimap(fb);
    chat_render(fb);
    if (panel_active()) panel_render(fb);
    /* FUN_00461138's last act: the wander pump runs on the paint path. */
    wander_pump();
}

void map_dump(DumpEmit emit, void *user)
{
    uint32_t now = clock_ms();
    dump_emit_int(emit, "map.id", loaded ? map.id : -1, user);
    dump_emit_int(emit, "map.x", w_x >> 8, user);
    dump_emit_int(emit, "map.y", w_y >> 8, user);
    dump_emit_int(emit, "map.fx", w_x, user);
    dump_emit_int(emit, "map.fy", w_y, user);
    dump_emit_int(emit, "map.tx", w_target_x >> 8, user);
    dump_emit_int(emit, "map.ty", w_target_y >> 8, user);
    dump_emit_int(emit, "map.speed", w_speed, user);
    dump_emit_int(emit, "map.duration", w_duration, user);
    dump_emit_int(emit, "map.path_cursor", path_cursor, user);
    dump_emit_int(emit, "map.path_count", path_count, user);
    dump_emit_int(emit, "map.waypoints", waypoints_enabled, user);
    dump_emit_int(emit, "map.nearest", nearest_link_idx, user);
    dump_emit_int(emit, "map.hit", hit_link_idx, user);
    dump_emit_int(emit, "map.latched", link_latched, user);
    dump_emit_int(emit, "map.enc_a", (long long)(now - enc_a), user);
    dump_emit_int(emit, "map.enc_b", (long long)(now - enc_b), user);
    dump_emit_int(emit, "map.enc_grace", battle_end_tick ? (long long)(now - battle_end_tick) : -1, user);
    dump_emit_int(emit, "map.no_monsters", no_monsters_here, user);
    dump_emit_int(emit, "map.wander", wander_speed != 0, user);
    dump_emit_int(emit, "map.wander_legs", (long long)wander_legs, user);
    dump_emit_int(emit, "map.facing", face_y * 3 + face_x, user);
    dump_emit_int(emit, "map.music", music_index, user);
}
