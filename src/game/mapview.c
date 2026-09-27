/* Offline map screen. Geometry/movement: docs/re/maps.md sections 4, 6 and 8. */
#include "game.h"
#include "hero.h"
#include "panels.h"
#include "../engine/font.h"
#include "../engine/log.h"
#include "../engine/rng.h"
#include "../engine/screen.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VIEW_W 364
#define VIEW_H 416
#define FIGHT_MAX 143
#define BUTTON_COUNT 7
#define ENTRY_GRACE 60
#define ENCOUNTER_GRACE 300
#define PATH_NODES_MAX 2000 /* FUN_00461dcc: bounded 2000-node detour search. */

static Map map;
static int loaded;
static Sheet skin;
static Image buttons[BUTTON_COUNT];
static const char *const button_names[BUTTON_COUNT] = {
    "Items", "Spells", "Equip", "Stats", "Map", "Camp", "Well"
};
static int32_t pos_x, pos_y, target_x, target_y;
static int walking, keyboard, facing = 7, step_remainder;
static int blocked_dx, blocked_dy;
static int entry_frames, encounter_frames, link_latch = -1;
static int hover = -1, mouse_held, minimap;
static char message[128];
static int pending[FIGHT_MAX], pending_count, pending_difficulty, pending_distance = 20;
typedef struct { int x, y; } PathPoint;
static PathPoint path[PATH_NODES_MAX], path_work[PATH_NODES_MAX];
static int path_count, path_next;
static int music_index, music_count, music_running;
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

/* FUN_004636d3: source-image dimensions / 8 give map-unit half extents. */
static void link_extent(const Link *link, int *w, int *h)
{
    const ObjRect *r = object_rect(link);
    *w = r && r->r > r->l ? (r->r - r->l + 7) / 8 : 1;
    *h = r && r->b > r->t ? (r->b - r->t + 7) / 8 : 1;
}

static int nearest_link(int *hit, int *distance)
{
    int i, nearest = -1, best = 0x7fffffff;
    *hit = -1;
    for (i = 0; i < OBL_RECORDS; ++i) {
        const Link *l = &map.links[i];
        int d, w, h;
        if (!l->used) continue;
        d = abs(g_hero.x - l->x) + abs(g_hero.y - l->y);
        if (d < best) { best = d; nearest = i; }
        link_extent(l, &w, &h);
        if (*hit < 0 && l->kind >= 1 && owns_item(l->required_item) &&
            g_hero.x >= l->x - w && g_hero.x < l->x + w &&
            g_hero.y >= l->y - h && g_hero.y < l->y + h) *hit = i;
    }
    *distance = best;
    return nearest;
}

static void stop_walk(void)
{
    if (walking) wos_log_event("hero_move", "x=%d y=%d", g_hero.x, g_hero.y);
    walking = keyboard = 0;
    step_remainder = 0;
    blocked_dx = blocked_dy = 0;
    path_count = path_next = 0;
}

static void place_hero(int link, int drop_in)
{
    int x = clamp(g_hero.x, 0, map.image.w - 1);
    int y = clamp(g_hero.y, 0, map.image.h - 1);
    if (link >= 0 && link < OBL_RECORDS && map.links[link].used) {
        int w, h;
        link_extent(&map.links[link], &w, &h);
        x = map.links[link].x;
        y = map.links[link].y - (drop_in ? 0 : h + 1);
        g_hero.link = link;
    }
    x = clamp(x, 0, map.image.w - 1);
    y = clamp(y, 0, map.image.h - 1);
    /* Above-link placement must not strand the hero in impassable terrain. */
    if (!map_walkable(&map, x, y, g_hero.tokens)) {
        int radius, found = 0;
        for (radius = 1; radius <= 64 && !found; ++radius) {
            int dx, dy;
            for (dy = -radius; dy <= radius && !found; ++dy)
                for (dx = -radius; dx <= radius; ++dx) {
                    if (abs(dx) != radius && abs(dy) != radius) continue;
                    if (map_walkable(&map, x + dx, y + dy, g_hero.tokens)) {
                        x += dx; y += dy; found = 1; break;
                    }
                }
        }
    }
    g_hero.x = x; g_hero.y = y;
    pos_x = x * 256; pos_y = y * 256;
    walking = keyboard = step_remainder = 0;
    blocked_dx = blocked_dy = path_count = path_next = 0;
    entry_frames = ENTRY_GRACE;
    encounter_frames = ENCOUNTER_GRACE;
    link_latch = -1;
    minimap = 0;
    panel_close();
    snprintf(message, sizeof(message), "%s", map.def->name);
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

static void map_music_track(void)
{
    char key[32], fallback[80];
    const char *name;
    snprintf(key, sizeof(key), "midi%d", music_index);
    name = world_music(map.def->root, key);
    if (!name || !*name) {
        snprintf(fallback, sizeof(fallback), "%s.mid", map.def->root);
        name = fallback;
    }
    game_music(name);
    /* No backend (or rejected track): never spin through the playlist. */
    music_running = plat_music_playing();
}

static void map_music_start(void)
{
    music_count = world_music_count(map.def->root);
    if (music_count < 1) music_count = 1;
    music_index = 1;
    map_music_track();
}

static void map_music_update(void)
{
    if (music_running && !plat_music_playing()) {
        music_index = music_index % music_count + 1;
        map_music_track();
    }
}

static void activate_link(int index)
{
    Link link = map.links[index];
    stop_walk();
    g_hero.link = index;
    if (link.kind == 2) game_enter_map(link.dest_map, link.target, 0);
    else if (link.kind == 1 || link.kind == 3 || link.kind == 4)
        game_enter_scene(link.target, &map.links[index]);
}

void game_enter_map(int map_id, int link, int drop_in)
{
    stop_walk();
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
    pending_count = pending_difficulty = 0;
    place_hero(link, drop_in);
    load_art();
    screen_set(&map_screen);
    wos_log_event("map_enter", "map=%d x=%d y=%d", map_id, g_hero.x, g_hero.y);
    map_music_start();
    if (drop_in && link >= 0 && link < OBL_RECORDS && map.links[link].used)
        activate_link(link);
}

void game_return_to_map(void)
{
    if (!loaded || map.id != g_hero.map) {
        game_enter_map(g_hero.map, g_hero.link, 0);
        return;
    }
    place_hero(g_hero.link, 0);
    screen_set(&map_screen);
    wos_log_event("map_enter", "map=%d x=%d y=%d", map.id, g_hero.x, g_hero.y);
    map_music_start();
}

static void camera(int *x, int *y)
{
    *x = clamp(pos_x / 64 - VIEW_W / 2, 0,
               map.image_x4.w > VIEW_W ? map.image_x4.w - VIEW_W : 0);
    *y = clamp(pos_y / 64 - VIEW_H / 2, 0,
               map.image_x4.h > VIEW_H ? map.image_x4.h - VIEW_H : 0);
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

static uint32_t integer_sqrt(uint64_t n)
{
    uint64_t bit = UINT64_C(1) << 62, result = 0;
    while (bit > n) bit >>= 2;
    while (bit) {
        if (n >= result + bit) { n -= result + bit; result = (result >> 1) + bit; }
        else result >>= 1;
        bit >>= 2;
    }
    return (uint32_t)result;
}

/* FUN_00461b11 truncates to the containing four-unit grid cell, then centres.
 * The sign correction in the decomp is for negative inputs, not a +3 offset. */
static int grid_snap(int v)
{
    return (v / 4) * 4 + 2;
}

static int path_clear(int x, int y, int clearance)
{
    if (!map_walkable(&map, x, y, g_hero.tokens)) return 0;
    return !clearance ||
        (map_walkable(&map, x - 1, y - 1, g_hero.tokens) &&
         map_walkable(&map, x + 1, y - 1, g_hero.tokens) &&
         map_walkable(&map, x - 1, y + 1, g_hero.tokens) &&
         map_walkable(&map, x + 1, y + 1, g_hero.tokens));
}

/* FUN_00461b93: half-unit line march, optionally checking four neighbours.
 * Diagonal legs retain one-unit clearance so fixed-point truncation cannot
 * put the actor on the blocked side of an exact terrain-cell corner. */
static int line_march(PathPoint from, PathPoint to, PathPoint *last, int clearance)
{
    int dx = to.x - from.x, dy = to.y - from.y;
    int steps = 2 * (abs(dx) > abs(dy) ? abs(dx) : abs(dy)), i;
    PathPoint previous = from;
    *last = from;
    if (!steps) return path_clear(from.x, from.y, clearance);
    for (i = 0; i <= steps; ++i) {
        PathPoint p = { from.x + (int)((int64_t)dx * i / steps),
                        from.y + (int)((int64_t)dy * i / steps) };
        if (!path_clear(p.x, p.y, clearance || (dx && dy)) ||
            (p.x / 4 != previous.x / 4 && p.y / 4 != previous.y / 4 &&
             (!path_clear(p.x, previous.y, 0) || !path_clear(previous.x, p.y, 0)))) {
            last->x = grid_snap(last->x);
            last->y = grid_snap(last->y);
            return 0;
        }
        *last = previous = p;
    }
    return 1;
}

/* Retail direction tables at 0x4f21a0/0x4f21c0, beginning south. */
static const int path_dx[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };
static const int path_dy[8] = { 1, 1, 0, -1, -1, -1, 0, 1 };

static int path_direction(PathPoint from, PathPoint to)
{
    int x = (to.x > from.x) - (to.x < from.x);
    int y = (to.y > from.y) - (to.y < from.y), i;
    for (i = 0; i < 8; ++i) if (path_dx[i] == x && path_dy[i] == y) return i;
    return 0;
}

/* FUN_00461dcc: march to the obstruction, then follow either edge using the
 * eight-direction table. Each blocked neighbour rotates the search one step;
 * after advancing, aim back toward the last blocked neighbour. */
static int path_build(PathPoint start, PathPoint goal, int turn)
{
    PathPoint current, last, blocked, next;
    int count = 1, direction, attempts, iterations;
    path_work[0] = start;
    if (line_march(start, goal, &current, 0)) {
        path_work[1] = goal;
        return 2;
    }
    if (!path_clear(current.x, current.y, 0) ||
        !line_march(start, current, &last, 0)) return 0;
    if (current.x != start.x || current.y != start.y) path_work[count++] = current;
    direction = path_direction(current, goal);
    for (iterations = 0; iterations < PATH_NODES_MAX && count < PATH_NODES_MAX - 1; ++iterations) {
        if (line_march(current, goal, &last, 0)) {
            path_work[count++] = goal;
            break;
        }
        blocked = goal;
        for (attempts = 0; attempts < 10; ++attempts) {
            next.x = grid_snap(current.x + path_dx[direction] * 4);
            next.y = grid_snap(current.y + path_dy[direction] * 4);
            if (line_march(current, next, &last, 0)) break;
            blocked = next;
            direction = (direction + turn + 8) % 8;
        }
        if (attempts == 10) return 0;
        path_work[count++] = next;
        direction = path_direction(next, blocked);
        current = next;
    }
    if (path_work[count - 1].x != goal.x || path_work[count - 1].y != goal.y) return 0;
    /* Original pruning scans backward for the furthest clearance-safe shortcut. */
    {
        int at = 0, out = 1;
        while (at < count - 1) {
            int far = count - 1;
            while (far > at + 1 && !line_march(path_work[at], path_work[far], &last, 1)) --far;
            path_work[out++] = path_work[far];
            at = far;
        }
        count = out;
    }
    return count;
}

static void start_path(int x, int y)
{
    PathPoint start = { g_hero.x, g_hero.y }, goal = { grid_snap(x), grid_snap(y) };
    int first, second;
    stop_walk();
    if (!map_walkable(&map, goal.x, goal.y, g_hero.tokens)) {
        wos_log_event("path_none", "x=%d y=%d", goal.x, goal.y);
        return;
    }
    first = path_build(start, goal, 1);
    if (first) memcpy(path, path_work, (size_t)first * sizeof(*path));
    second = path_build(start, goal, -1);
    if (second && (!first || second < first)) {
        first = second;
        memcpy(path, path_work, (size_t)first * sizeof(*path));
    }
    if (!first) {
        wos_log_event("path_none", "x=%d y=%d", goal.x, goal.y);
        return;
    }
    path_count = first;
    path_next = 1;
    target_x = path[1].x * 256;
    target_y = path[1].y * 256;
    walking = 1;
    wos_log_event("path_found", "nodes=%d x=%d y=%d", first, goal.x, goal.y);
}

static void path_arrive(void)
{
    if (path_next + 1 < path_count) {
        ++path_next;
        target_x = path[path_next].x * 256;
        target_y = path[path_next].y * 256;
    } else stop_walk();
}

static void move_step(int dx, int dy)
{
    int speed = map.id == 0 ? 3200 : 4800; /* maps.md section 6: 24.8 units/second. */
    int step, nx, ny;
    uint32_t distance;
    if (movement_effect(12)) speed *= 2;
    step_remainder += speed;
    step = step_remainder / 60;
    step_remainder %= 60;
    distance = integer_sqrt((uint64_t)((int64_t)dx * dx + (int64_t)dy * dy));
    if (!distance) { path_arrive(); return; }
    facing = (dy < 0 ? 0 : dy > 0 ? 2 : 1) * 3 + (dx < 0 ? 0 : dx > 0 ? 2 : 1);
    if (!keyboard && distance <= (uint32_t)step) { nx = target_x; ny = target_y; }
    else {
        nx = pos_x + (int)((int64_t)dx * step / distance);
        ny = pos_y + (int)((int64_t)dy * step / distance);
    }
    if (!map_walkable(&map, nx / 256, ny / 256, g_hero.tokens) || nx < 0 || ny < 0 ||
        (nx / 256 != pos_x / 256 && ny / 256 != pos_y / 256 &&
         (!map_walkable(&map, nx / 256, pos_y / 256, g_hero.tokens) ||
          !map_walkable(&map, pos_x / 256, ny / 256, g_hero.tokens)))) {
        int was_keyboard = keyboard;
        stop_walk();
        if (was_keyboard) { blocked_dx = dx; blocked_dy = dy; }
        return;
    }
    pos_x = nx; pos_y = ny;
    g_hero.x = nx / 256; g_hero.y = ny / 256;
    if (!keyboard && pos_x == target_x && pos_y == target_y) path_arrive();
}

static void add_monster(int *ids, int *n, int id)
{
    if (*n < FIGHT_MAX && valid_monster(id)) ids[(*n)++] = id;
}

static int encounter_roster(const Link *link, int *ids)
{
    int n = 0, i, group = link->difficulty;
    if (!group && map.mon_count) {
        for (i = 0; i < MON_RECORDS && n < FIGHT_MAX; ++i) {
            const MonPlace *m = &map.mons[i];
            int64_t dx = (int64_t)g_hero.x - m->x, dy = (int64_t)g_hero.y - m->y;
            uint64_t d2 = (uint64_t)(dx * dx + dy * dy);
            uint64_t r2;
            if (!valid_monster(m->monster_id) || m->radius <= 0) continue;
            r2 = (uint64_t)m->radius * m->radius;
            if (d2 >= r2) continue;
            if (rng_bounded(game_rng(), 100) < 25) add_monster(ids, &n, m->monster_id);
            /* Souls.exe doubles 0x4cd548 = .5, 0x4cd578 = .25 (PE .rdata). */
            if (d2 < (r2 + 3) / 4) {
                add_monster(ids, &n, m->monster_id);
                if (rng_bounded(game_rng(), 100) < 15) add_monster(ids, &n, m->monster_id);
            }
            if (d2 < (r2 + 15) / 16) {
                add_monster(ids, &n, m->monster_id);
                if (rng_bounded(game_rng(), 100) < 5) add_monster(ids, &n, m->monster_id);
            }
        }
    }
    return n;
}

static void encounter_tick(int nearest, int hit, int distance, int moved)
{
    const Link *link;
    int threshold, ids[FIGHT_MAX], n, pct, group;
    if (encounter_frames > 0) { --encounter_frames; return; }
    if (nearest < 0 || g_hero.hp <= 0) return;
    link = &map.links[nearest];
    /* FUN_0046260e: .mon group zero is suppressed ON a link, not outside it. */
    if (link->kind == 4 || (link->difficulty < 2 &&
        (!map.mon_count || link->difficulty != 0 || hit == nearest))) return;
    /* FUN_00436b15 uses hunting training, NOT the link's group. Fresh solo
     * heroes have rating zero: max(bitlen(0),3) = 3, and idle threshold = 0.
     * The port has no hunting-training action yet. One roll per fixed 60 Hz tick. */
    threshold = movement_effect(11) ? 20 : 200;
    if (!moved || rng_bounded(game_rng(), 10000) >= (unsigned)threshold) return;
    n = encounter_roster(link, ids);
    group = link->difficulty < 0 ? -link->difficulty : link->difficulty;
    if (!n && (!group || group >= WORLD_MAX_GROUPS ||
               !g_world.groups[group].used || g_world.groups[group].count < 1)) return;
    pct = clamp(distance, 20, 80);
    if (link->difficulty < 0) pct = 100 - pct;
    stop_walk();
    g_hero.link = nearest;
    game_set_pending_fight(ids, n, link->difficulty, pct);
    wos_log_event("encounter", "group=%d distance=%d", link->difficulty, pct);
    encounter_frames = ENCOUNTER_GRACE;
    game_enter_scene(2, link);
}

static void map_update(const Input *in)
{
    int i, dx, dy, hit, nearest, distance, moved;
    map_music_update();
    if (panel_active()) { panel_update(in); return; }
    hover = -1;
    mouse_held = (in->mouse_down & (1u << 1)) != 0;
    for (i = 0; i < BUTTON_COUNT; ++i)
        if (contains(button_rect(i), in->mouse_x, in->mouse_y)) hover = i;
    if ((in->mouse_pressed & (1u << 1)) && hover >= 0) {
        stop_walk();
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
    if (in->pressed[PLAT_KEY_ESCAPE]) { stop_walk(); minimap = 0; }
    if (minimap) return;
    if ((in->mouse_pressed & (1u << 1)) && in->mouse_x >= 0 &&
        in->mouse_x < VIEW_W && in->mouse_y >= 0 && in->mouse_y < VIEW_H) {
        int cx, cy;
        camera(&cx, &cy);
        start_path(clamp((cx + in->mouse_x) / 4, 0, map.image.w - 1),
                   clamp((cy + in->mouse_y) / 4, 0, map.image.h - 1));
    }
    dx = !!in->down[PLAT_KEY_RIGHT] - !!in->down[PLAT_KEY_LEFT];
    dy = !!in->down[PLAT_KEY_DOWN] - !!in->down[PLAT_KEY_UP];
    if ((dx * 256 != blocked_dx) || (dy * 256 != blocked_dy))
        blocked_dx = blocked_dy = 0;
    if ((dx || dy) && !(blocked_dx || blocked_dy)) {
        path_count = path_next = 0;
        walking = keyboard = 1;
    }
    else if (keyboard) stop_walk();
    moved = walking;
    if (walking) move_step(keyboard ? dx * 256 : target_x - pos_x,
                           keyboard ? dy * 256 : target_y - pos_y);
    nearest = nearest_link(&hit, &distance);
    if (entry_frames > 0) --entry_frames;
    else if (hit >= 0 && hit != link_latch) {
        link_latch = hit;
        activate_link(hit);
        return;
    }
    if (hit < 0) link_latch = -1;
    encounter_tick(nearest, hit, distance, moved);
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
    int x, py, w = 312, h = map.image.h * 312 / map.image.w;
    fb_fill(fb, (Rect){12, 20, 340, 376}, 0x14202b);
    fb_rect(fb, (Rect){12, 20, 340, 376}, 0xb8a67d);
    font_draw(fb, 24, 34, "Map", 0xffdc94);
    if (h > 296) { h = 296; w = map.image.w * h / map.image.h; }
    for (py = 0; py < h; ++py)
        for (x = 0; x < w; ++x)
            fb_pixel(fb, 24 + x, 60 + py, map.image.pixels[(py * map.image.h / h) * map.image.w + x * map.image.w / w]);
    fb_fill(fb, (Rect){23 + g_hero.x * w / map.image.w, 59 + g_hero.y * h / map.image.h, 3, 3}, 0xff2020);
    font_draw(fb, 24, 380, "Escape to return", 0xb8a67d);
}

static void map_render(Framebuffer *fb)
{
    int cx, cy, i, sx, sy, size;
    char text[128];
    fb_clear(fb, 0x121820);
    camera(&cx, &cy);
    fb_clip(fb, (Rect){0, 0, VIEW_W, VIEW_H});
    fb_blit_sub(fb, &map.image_x4, (Rect){cx, cy, VIEW_W, VIEW_H}, 0, 0, 0, -1);
    for (i = 0; i < OBL_RECORDS; ++i) {
        const Link *l = &map.links[i];
        const ObjRect *r;
        int w, h;
        int64_t key;
        if (!l->used || !(r = object_rect(l)) || !map.objects.pixels) continue;
        w = r->r - r->l; h = r->b - r->t;
        if (w <= 0 || h <= 0) continue;
        /* Original DIB row h-1 is top-left after Image's top-down conversion. */
        key = map.objects.pixels[0];
        fb_blit_sub(fb, &map.objects, (Rect){r->l, r->t, w, h},
                    l->x * 4 - cx - w / 2, l->y * 4 - cy - h / 2, 0, key);
    }
    sx = pos_x / 64 - cx; sy = pos_y / 64 - cy;
    size = skin.cell / 3;
    /* Small opaque oval underneath the colour-keyed MAP subcell. */
    for (i = -3; i <= 3; ++i) {
        int half = 8 - i * i / 2;
        fb_fill(fb, (Rect){sx - half, sy + size / 2 - 3 + i, half * 2 + 1, 1}, 0x263124);
    }
    sheet_draw_map_dir(fb, &skin, facing, sx - size / 2, sy - size / 2);
    fb_reset_clip(fb);
    fb_fill(fb, (Rect){364, 0, 276, VIEW_H}, 0x202934);
    for (i = 0; i < BUTTON_COUNT; ++i) {
        Rect r = button_rect(i);
        int state = hover == i ? (mouse_held ? 2 : 3) : 1;
        const Image *img = &buttons[i];
        fb_fill(fb, r, 0x34414d);
        if (img->pixels && img->w >= (state + 1) * 48 && img->h >= 48)
            fb_blit_sub(fb, img, (Rect){state * 48, 0, 48, 48}, r.x, r.y, 0,
                        img->pixels[0]);
        else font_draw(fb, r.x + 2, r.y + 20, button_names[i], 0xffffff);
    }
    fb_fill(fb, (Rect){364, 116, 256, 256}, 0x151e28);
    fb_rect(fb, (Rect){364, 116, 256, 256}, 0x82795f);
    font_wrap(fb, (Rect){376, 128, 232, 20}, g_hero.name, 0xffdc94);
    snprintf(text, sizeof(text), "Level %d", g_hero.level);
    font_draw(fb, 376, 156, text, 0xffffff);
    fb_clip(fb, (Rect){376, 176, 232, 128});
    sheet_draw(fb, &skin, 1, 492 - skin.cell / 2, 238 - skin.cell / 2, 0);
    fb_reset_clip(fb);
    snprintf(text, sizeof(text), "HP %d/%d  MP %d/%d", g_hero.hp, g_hero.max_hp, g_hero.mp, g_hero.max_mp);
    font_wrap(fb, (Rect){376, 310, 232, 24}, text, 0xffffff);
    snprintf(text, sizeof(text), "Gold %lld", (long long)g_hero.gold);
    font_draw(fb, 376, 346, text, 0xffdc94);
    if (hover >= 0) font_draw(fb, 376, 390, button_names[hover], 0xffffff);
    fb_fill(fb, (Rect){0, 416, 640, 64}, 0x101820);
    meter(fb, 8, 423, "HP", g_hero.hp, g_hero.max_hp, 0x2100a5);
    meter(fb, 148, 423, "MP", g_hero.mp, g_hero.max_mp, 0xff8080);
    snprintf(text, sizeof(text), "%s (%d,%d)", map.def->name, g_hero.x, g_hero.y);
    font_wrap(fb, (Rect){292, 425, 336, 16}, text, 0xc8cfdb);
    font_wrap(fb, (Rect){8, 451, 624, 24}, message, 0xffffff);
    if (minimap) draw_minimap(fb);
    if (panel_active()) panel_render(fb);
}
