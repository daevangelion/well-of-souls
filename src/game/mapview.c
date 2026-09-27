/* Offline map screen. Geometry/movement: docs/re/maps.md sections 4, 6 and 8. */
#include "game.h"
#include "hero.h"
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
static int hover = -1, mouse_held, panel = -1;
static char message[128];
static int pending[FIGHT_MAX], pending_count, pending_difficulty;
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

void game_set_pending_fight(const int *ids, int count, int difficulty)
{
    int i;
    pending_count = 0;
    pending_difficulty = difficulty;
    if (!ids) return;
    for (i = 0; i < count && pending_count < FIGHT_MAX; ++i)
        if (valid_monster(ids[i])) pending[pending_count++] = ids[i];
}

int game_take_pending_fight(int *ids, int max, int *difficulty)
{
    int n = clamp(pending_count, 0, max > 0 ? max : 0);
    if (difficulty) *difficulty = pending_difficulty;
    if (ids && n) memcpy(ids, pending, (size_t)n * sizeof(*ids));
    if (!ids) n = 0;
    pending_count = 0;
    pending_difficulty = 0;
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
    int i;
    if (id <= 0) return 1;
    for (i = 0; i < HERO_INVENTORY; ++i)
        if (g_hero.inventory[i].item_id == id && g_hero.inventory[i].count > 0) return 1;
    for (i = 0; i < 8; ++i) if (g_hero.equip[i] == id) return 1;
    return g_hero.right_hand == id;
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
    entry_frames = ENTRY_GRACE;
    encounter_frames = ENCOUNTER_GRACE;
    link_latch = -1;
    panel = -1;
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
    if (!distance) { stop_walk(); return; }
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
    if (!keyboard && pos_x == target_x && pos_y == target_y) stop_walk();
}

static void add_monster(int *ids, int *n, int id)
{
    if (*n < FIGHT_MAX && valid_monster(id)) ids[(*n)++] = id;
}

static int encounter_roster(const Link *link, int distance, int *ids)
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
    } else if (group > 0 && group < WORLD_MAX_GROUPS && g_world.groups[group].used) {
        const GroupDef *g = &g_world.groups[group];
        int pct = clamp(distance, 20, 80);
        for (i = 0; i < g->count && i < GROUP_MAX_MEMBERS; ++i)
            if (rng_bounded(game_rng(), 100) < (unsigned)pct ||
                (g_world.groups[0].count && (g_world.groups[0].members[0] & 1)))
                add_monster(ids, &n, g->members[i]);
        if (!n && g->count > 0 && g->count <= GROUP_MAX_MEMBERS)
            add_monster(ids, &n, g->members[rng_bounded(game_rng(), (unsigned)g->count)]);
    }
    return n;
}

static void encounter_tick(int nearest, int hit, int distance, int moved)
{
    const Link *link;
    int threshold, ids[FIGHT_MAX], n;
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
    n = encounter_roster(link, distance, ids);
    if (!n) return;
    stop_walk();
    g_hero.link = nearest;
    game_set_pending_fight(ids, n, link->difficulty);
    encounter_frames = ENCOUNTER_GRACE;
    game_enter_scene(2, link);
}

static void map_update(const Input *in)
{
    int i, dx, dy, hit, nearest, distance, moved;
    hover = -1;
    mouse_held = (in->mouse_down & (1u << 1)) != 0;
    for (i = 0; i < BUTTON_COUNT; ++i)
        if (contains(button_rect(i), in->mouse_x, in->mouse_y)) hover = i;
    if ((in->mouse_pressed & (1u << 1)) && hover >= 0) {
        stop_walk();
        if (hover == 5) { game_enter_scene(1, NULL); return; }
        if (hover == 6) { hero_save(&g_hero); game_go_well(); return; }
        panel = panel == hover ? -1 : hover;
    }
    if (in->pressed[PLAT_KEY_ESCAPE]) { stop_walk(); panel = -1; }
    if (panel >= 0) return;
    if ((in->mouse_pressed & (1u << 1)) && in->mouse_x >= 0 &&
        in->mouse_x < VIEW_W && in->mouse_y >= 0 && in->mouse_y < VIEW_H) {
        int cx, cy;
        stop_walk();
        camera(&cx, &cy);
        target_x = clamp((cx + in->mouse_x) / 4, 0, map.image.w - 1) * 256;
        target_y = clamp((cy + in->mouse_y) / 4, 0, map.image.h - 1) * 256;
        walking = target_x != pos_x || target_y != pos_y;
    }
    dx = !!in->down[PLAT_KEY_RIGHT] - !!in->down[PLAT_KEY_LEFT];
    dy = !!in->down[PLAT_KEY_DOWN] - !!in->down[PLAT_KEY_UP];
    if ((dx * 256 != blocked_dx) || (dy * 256 != blocked_dy))
        blocked_dx = blocked_dy = 0;
    if ((dx || dy) && !(blocked_dx || blocked_dy)) { walking = keyboard = 1; }
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

static void draw_panel(Framebuffer *fb)
{
    int i, y = 60;
    char text[128];
    fb_fill(fb, (Rect){12, 20, 340, 376}, 0x14202b);
    fb_rect(fb, (Rect){12, 20, 340, 376}, 0xb8a67d);
    font_draw(fb, 24, 34, button_names[panel], 0xffdc94);
    if (panel == 4) {
        /* Minimap is the low-resolution jpg, nearest-neighbour fit to the panel. */
        int x, py, w = 312, h = map.image.h * 312 / map.image.w;
        if (h > 296) { h = 296; w = map.image.w * h / map.image.h; }
        for (py = 0; py < h; ++py)
            for (x = 0; x < w; ++x)
                fb_pixel(fb, 24 + x, 60 + py, map.image.pixels[(py * map.image.h / h) * map.image.w + x * map.image.w / w]);
        fb_fill(fb, (Rect){23 + g_hero.x * w / map.image.w, 59 + g_hero.y * h / map.image.h, 3, 3}, 0xff2020);
    } else if (panel == 0 || panel == 2) {
        int count = panel == 0 ? HERO_INVENTORY : 8;
        for (i = 0; i < count && y < 360; ++i) {
            int id = panel == 0 ? g_hero.inventory[i].item_id : g_hero.equip[i];
            if (id <= 0 || id >= WORLD_MAX_ITEMS || !g_world.items[id].used) continue;
            if (panel == 0 && g_hero.inventory[i].count <= 0) continue;
            snprintf(text, sizeof(text), "%s x%d", g_world.items[id].name,
                     panel == 0 ? g_hero.inventory[i].count : 1);
            font_wrap(fb, (Rect){24, y, 312, 20}, text, 0xffffff);
            y += 24;
        }
        if (y == 60) font_draw(fb, 24, y, "Empty", 0xc0c0c0);
    } else if (panel == 3) {
        static const char *const ability[] = { "Strength", "Wisdom", "Stamina", "Agility", "Dexterity" };
        for (i = 0; i < HERO_ABILITIES; ++i) {
            snprintf(text, sizeof(text), "%s: %d", ability[i], g_hero.ability[i]);
            font_draw(fb, 24, y, text, 0xffffff); y += 24;
        }
        snprintf(text, sizeof(text), "XP: %lld", (long long)g_hero.xp);
        font_draw(fb, 24, y, text, 0xffffff);
    } else font_wrap(fb, (Rect){24, y, 312, 64}, "Cast spells while in a scene.", 0xc0c0c0);
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
    if (panel >= 0) draw_panel(fb);
}
