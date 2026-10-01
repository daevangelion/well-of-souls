/* Authoring editors: Link Editor, terrain brush, monster placement, world signing.
 * Every rule below is taken from the decompilation; the VA is cited at each site. */
#include "editors.h"
#include "sched.h"
#include "game.h"
#include "game_main.h"
#include "../engine/fb.h"
#include "../engine/font.h"
#include "../engine/image.h"
#include "../engine/log.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Editor ed;
static char data_dir[1024];
/* FUN_0045C7D7's MessageBox, drawn in framebuffer. */
static int save_prompt;
static Rect prompt_yes, prompt_no;
static Map ed_map;          /* decoded view: objects.obr rects, the map jpg, the terrain grid */
static int ed_have_map;

static int le16(const unsigned char *p)
{ return (int)((unsigned)p[0] | ((unsigned)p[1] << 8)); }

static int le32(const unsigned char *p)
{ return (int)((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24)); }

static void put32(unsigned char *p, int v)
{
    uint32_t u = (uint32_t)v;
    p[0] = (unsigned char)u; p[1] = (unsigned char)(u >> 8);
    p[2] = (unsigned char)(u >> 16); p[3] = (unsigned char)(u >> 24);
}

static void disk_get(char *dst, size_t cap, const unsigned char *p, size_t bytes)
{
    size_t n = 0;
    while (n < bytes && n + 1 < cap && p[n]) { dst[n] = (char)p[n]; ++n; }
    dst[n] = 0;
}

static void disk_put(unsigned char *p, const char *src, size_t bytes)
{
    size_t n = 0;
    memset(p, 0, bytes);
    while (n + 1 < bytes && src && src[n]) { p[n] = (unsigned char)src[n]; ++n; }
}

static unsigned char *obl_rec(int i) { return ed.obl + (size_t)i * OBL_RECORD_SIZE; }
static unsigned char *mon_rec(int i) { return ed.mon + (size_t)i * MON_RECORD_SIZE; }

#if defined(__GNUC__)
static void status(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#endif
static void status(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ed.status, sizeof(ed.status), fmt, ap);
    va_end(ap);
}

static int absi(int v) { return v < 0 ? -v : v; }

/* --- raw file access ---------------------------------------------------------------------- */

/* FUN_00463989 (obl) memsets the whole array and then freads; a short file leaves the tail
 * zeroed, so a re-save always produces a full-size file. FUN_00464461 (mon) is the same but
 * additionally zeroes the runtime instance pointer at MON_OFFSET_INSTANCE of every record. */
static int read_obl(const char *path)
{
    FILE *f;
    memset(ed.obl, 0, sizeof(ed.obl));
    f = plat_fopen(path, "rb");
    if (!f) return -1;
    fread(ed.obl, 1, sizeof(ed.obl), f);
    fclose(f);
    return 0;
}

static int read_mon(const char *path)
{
    FILE *f;
    int i;
    memset(ed.mon, 0, sizeof(ed.mon));
    f = plat_fopen(path, "rb");
    if (!f) return -1;
    fread(ed.mon, 1, sizeof(ed.mon), f);
    fclose(f);
    for (i = 0; i < MON_RECORDS; ++i) put32(mon_rec(i) + MON_OFFSET_INSTANCE, 0);
    return 0;
}

static int read_ter(const char *path)
{
    FILE *f;
    long size;
    int w, h, stride;
    free(ed.ter_file);
    ed.ter_file = NULL; ed.ter_size = 0; ed.ter_w = ed.ter_h = 0;
    f = plat_fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 54) { fclose(f); return -1; }
    ed.ter_file = malloc((size_t)size);
    if (!ed.ter_file) { fclose(f); return -1; }
    if (fread(ed.ter_file, 1, (size_t)size, f) != (size_t)size) {
        fclose(f); free(ed.ter_file); ed.ter_file = NULL; return -1;
    }
    fclose(f);
    ed.ter_size = (size_t)size;
    if (ed.ter_file[0] != 'B' || ed.ter_file[1] != 'M' || le16(ed.ter_file + 28) != 8) goto bad;
    w = le32(ed.ter_file + 18);
    h = le32(ed.ter_file + 22);
    if (h < 0) h = -h;                 /* a negative height means a top-down DIB */
    stride = (w + 3) & ~3;
    if (w <= 0 || h <= 0 || stride <= 0) goto bad;
    if ((size_t)le32(ed.ter_file + 10) + (size_t)stride * (size_t)h > ed.ter_size) goto bad;
    ed.ter_w = w; ed.ter_h = h;
    return 0;
bad:
    free(ed.ter_file); ed.ter_file = NULL; ed.ter_size = 0;
    return -1;
}

static int write_blob(const char *path, const void *data, size_t size, size_t chunk)
{
    FILE *f = plat_fopen(path, "wb");
    if (!f) return -1;
    if (chunk) {
        /* FUN_0046393f: fwrite(base, 800, 0x100) */
        size_t done = 0;
        while (done < size) {
            size_t n = size - done < chunk ? size - done : chunk;
            if (fwrite((const unsigned char *)data + done, 1, n, f) != n) { fclose(f); return -1; }
            done += n;
        }
    } else if (size && fwrite(data, 1, size, f) != size) { fclose(f); return -1; }
    if (fclose(f)) return -1;
    return 0;
}

static int map_file(char *buf, size_t size, const char *ext)
{
    const MapDef *def;
    char rel[160];
    if (ed.map_id < 0 || ed.map_id >= WORLD_MAX_MAPS) return -1;
    def = &g_world.maps[ed.map_id];
    if (!def->used) return -1;
    if (snprintf(rel, sizeof(rel), "maps/%s.%s", def->root, ext) < 0) return -1;
    return world_path(buf, (int)size, rel) ? 0 : -1;
}

/* --- world / map loading ------------------------------------------------------------------ */

void editors_set_data_dir(const char *dir)
{
    if (!dir || !*dir) data_dir[0] = 0;
    else snprintf(data_dir, sizeof(data_dir), "%s", dir);
}

const char *editors_data_dir(void) { return data_dir; }

int editors_open(const char *world)
{
    const char *name = world && *world ? world : g_world.name;
    const char *root = data_dir[0] ? data_dir : game_data_path();
    if (ed.open) editors_close();
    if (!name || !*name) return -1;
    if (world && *world && world_load(root, world)) {
        wos_log_event("editor_error", "reason=world_load world=%s", world);
        return -1;
    }
    if (world && *world) env_world_loaded();
    ed.brush_size = EDIT_BRUSH_MEDIUM;
    ed.brush_terrain = 1;
    ed.brush_monster = -1;
    ed.tool = EDIT_TOOL_LINK;
    ed.selected_link = ed.selected_mon = -1;
    return 0;
}

int editors_load_map(int map_id)
{
    char path[1024];
    int i, first = -1;
    if (map_id < 0 || map_id >= WORLD_MAX_MAPS || !g_world.maps[map_id].used) return -1;
    for (i = 0; i < WORLD_MAX_MAPS; ++i) if (g_world.maps[i].used) { first = i; break; }
    if (ed_have_map) { map_free(&ed_map); ed_have_map = 0; }
    ed.map_id = map_id;
    ed.selected_link = ed.selected_mon = -1;
    ed.dirty_link = ed.dirty_mon = ed.dirty_terrain = 0;
    if (map_file(path, sizeof(path), "obl") || read_obl(path)) return -1;
    if (map_file(path, sizeof(path), "mon") || read_mon(path)) return -1;
    /* FUN_0041e421 loads the terrain before the link and monster tables; a world with no .ter
     * (retail Springwell) still opens, with an all-zero cleared grid. */
    if (map_file(path, sizeof(path), "ter") || read_ter(path)) {
        ed.ter_w = ed.ter_h = 0;
        wos_log_event("editor_terrain", "map=%d state=absent", map_id);
    }
    /* The decoded map supplies the objects.obr rectangles the link hit test needs
     * (FUN_004636d3 reads DAT_005f8808, the same 48-byte rows world.c decodes) plus the map
     * image and the terrain grid the editor paints. */
    ed_have_map = map_load(&ed_map, map_id) == 0;
    /* Default the palettes the way an unconfigured editor does: the first object sprite and the
     * first monster the world defines, so a click always has something to place. */
    ed.object_id = 0;
    if (ed_have_map) {
        int k;
        for (k = 0; k < OBR_RECORDS; ++k) if (ed_map.objrects[k].used) { ed.object_id = k; break; }
    }
    if (ed.brush_monster < 0) {
        int k;
        for (k = 1; k < WORLD_MAX_MONSTERS; ++k)
            if (g_world.monsters[k].used) { ed.brush_monster = k; break; }
    }
    ed.open = 1;
    wos_log_event("editor_open", "world=%s map=%d terrain=%dx%d decoded=%d first_map=%d",
                  g_world.name, map_id, ed.ter_w, ed.ter_h, ed_have_map, first);
    return 0;
}

void editors_close(void)
{
    if (ed_have_map) { map_free(&ed_map); ed_have_map = 0; }
    free(ed.ter_file);
    ed.ter_file = NULL; ed.ter_size = 0; ed.ter_w = ed.ter_h = 0;
    ed.open = 0;
}

const Editor *editors_state(void) { return &ed; }

int editors_dirty(void) { return ed.dirty_link || ed.dirty_mon || ed.dirty_terrain; }

/* --- saving ------------------------------------------------------------------------------- */

/* FUN_0045c9bd: sprintf("%s.obl", <world>\maps\<root>) then FUN_0046393f, which is
 * fopen("wb") + fwrite(base, 800, 0x100) - all 256 records, verbatim. */
int editors_save_links(void)
{
    char path[1024];
    if (map_file(path, sizeof(path), "obl")) return -1;
    if (write_blob(path, ed.obl, sizeof(ed.obl), OBL_RECORD_SIZE)) return -1;
    ed.dirty_link = 0;
    wos_log_event("editor_save", "file=obl map=%d records=%d bytes=%lu",
                  ed.map_id, OBL_RECORDS, (unsigned long)sizeof(ed.obl));
    return 0;
}

/* FUN_00464421: fopen("wb") + fwrite(&DAT_00636808, 1, 0x43620) - the whole array at once. */
int editors_save_monsters(void)
{
    char path[1024];
    if (map_file(path, sizeof(path), "mon")) return -1;
    if (write_blob(path, ed.mon, sizeof(ed.mon), 0)) return -1;
    ed.dirty_mon = 0;
    wos_log_event("editor_save", "file=mon map=%d records=%d bytes=%lu",
                  ed.map_id, MON_RECORDS, (unsigned long)sizeof(ed.mon));
    return 0;
}

/* FUN_004867c0(&DAT_004e0de0, 1) writes the terrain DIB back out. */
int editors_save_terrain(void)
{
    char path[1024];
    if (!ed.ter_file) return -1;
    if (map_file(path, sizeof(path), "ter")) return -1;
    if (write_blob(path, ed.ter_file, ed.ter_size, 0)) return -1;
    ed.dirty_terrain = 0;
    wos_log_event("editor_save", "file=ter map=%d cells=%dx%d bytes=%lu",
                  ed.map_id, ed.ter_w, ed.ter_h, (unsigned long)ed.ter_size);
    return 0;
}

/* FUN_0045dbc5: the terrain and the monsters go out as soon as they are dirty; the links are
 * written when the editor closes (FUN_0045c7d7 -> FUN_0045c9bd). */
int editors_save(void)
{
    int written = 0;
    if (ed.dirty_terrain && editors_save_terrain() == 0) ++written;
    if (ed.dirty_mon && editors_save_monsters() == 0) ++written;
    if (ed.dirty_link && editors_save_links() == 0) ++written;
    return written;
}

/* --- link editor -------------------------------------------------------------------------- */

int editors_link_get(int index, Link *out)
{
    const unsigned char *r;
    if (index < 0 || index >= OBL_RECORDS || !out) return -1;
    r = obl_rec(index);
    memset(out, 0, sizeof(*out));
    out->used = le32(r);
    out->object_id = le32(r + 4);
    out->x = le32(r + 8);
    out->y = le32(r + 12);
    out->kind = le32(r + 16);
    out->target = le32(r + 100);
    out->difficulty = le32(r + 104);
    out->theme = le32(r + 108);
    disk_get(out->background, sizeof(out->background), r + 112, 80);
    disk_get(out->name, sizeof(out->name), r + 192, 128);
    out->dest_map = le32(r + 404);
    out->weather = le32(r + 408);
    out->fx = le32(r + 412);
    out->required_item = le32(r + 420);
    return 0;
}

/* FUN_0045db67 writes the link kind straight into (&DAT_00604818)[n*200] = record + 0x10 and
 * raises both the "record changed" and "file changed" flags. Only the fields the Link model
 * names are rewritten; every other byte of the 800-byte record is left as it was read, which
 * is what makes an unedited record re-serialize byte for byte. */
int editors_link_set(int index, const Link *in)
{
    unsigned char *r;
    if (index < 0 || index >= OBL_RECORDS || !in) return -1;
    r = obl_rec(index);
    put32(r + 0, in->used);
    put32(r + 4, in->object_id);
    put32(r + 8, in->x);
    put32(r + 12, in->y);
    put32(r + 16, in->kind);
    put32(r + 100, in->target);
    put32(r + 104, in->difficulty);
    put32(r + 108, in->theme);
    disk_put(r + 112, in->background, 80);
    disk_put(r + 192, in->name, 128);
    put32(r + 404, in->dest_map);
    put32(r + 408, in->weather);
    put32(r + 412, in->fx);
    put32(r + 420, in->required_item);
    ed.dirty_link = 1;
    wos_log_event("editor_link_set", "index=%d kind=%d x=%d y=%d dest_map=%d target=%d",
                  index, in->kind, in->x, in->y, in->dest_map, in->target);
    return 0;
}

/* FUN_004636d3. The half extents are the object sprite's size in objects.obr divided by 8 (the
 * objects sheet is drawn at 8x in the editor), rounded toward zero. Every record gets its
 * Manhattan distance to the cursor written at OBL_OFFSET_DISTANCE; the record the cursor is
 * inside gets 0 there and ends the scan. */
int editors_link_at(int x, int y)
{
    int i, best = 1000000000, nearest = -1, usable = -1, hit = -1;
    for (i = 0; i < OBL_RECORDS; ++i) {
        unsigned char *r = obl_rec(i);
        if (le32(r) != 0 && le32(r + 4) > -1) {
            int id = le32(r + 4), hw = 0, hh = 0, d, in;
            if (ed_have_map && id >= 0 && id < OBR_RECORDS && ed_map.objrects[id].used) {
                int w = ed_map.objrects[id].r - ed_map.objrects[id].l;
                int h = ed_map.objrects[id].b - ed_map.objrects[id].t;
                hw = (w + (w >> 31 & 7)) >> 3;
                hh = (h + (h >> 31 & 7)) >> 3;
            }
            in = x >= le32(r + 8) - hw && x < le32(r + 8) + hw &&
                 y >= le32(r + 12) - hh && y < le32(r + 12) + hh;
            if (in) {
                put32(r + OBL_OFFSET_DISTANCE, 0);
                nearest = i;
                hit = i;
                /* kind < 1 or a required item the hero does not carry is not usable; the
                 * required-item test is FUN_0044de18 and lives with the inventory. */
                if (le32(r + 16) >= 1) usable = i;
                break;
            }
            d = absi(y - le32(r + 12)) + absi(x - le32(r + 8));
            put32(r + OBL_OFFSET_DISTANCE, d);
            if (d < best) { best = d; nearest = i; }
        }
    }
    if (hit < 0) hit = nearest;
    ed.selected_link = hit;
    if (hit >= 0) {
        const unsigned char *r = obl_rec(hit);
        status("link %d  M%dL%d  kind=%d dest=%d target=%d", hit, ed.map_id, hit,
               le32(r + 16), le32(r + 404), le32(r + 100));
        wos_log_event("editor_link_select", "index=%d x=%d y=%d kind=%d dest_map=%d target=%d usable=%d",
                      hit, le32(r + 8), le32(r + 12), le32(r + 16), le32(r + 404), le32(r + 100), usable);
    } else {
        status("no link under the cursor");
    }
    return hit;
}

/* FUN_004636d3's fallback: the record with the smallest Manhattan distance, whether or not the
 * point is inside its sprite rectangle. */
int editors_link_nearest(int x, int y)
{
    int i, best = 1000000000, nearest = -1;
    for (i = 0; i < OBL_RECORDS; ++i) {
        const unsigned char *r = obl_rec(i);
        if (le32(r) != 0 && le32(r + 4) > -1) {
            int d = absi(y - le32(r + 12)) + absi(x - le32(r + 8));
            if (d < best) { best = d; nearest = i; }
        }
    }
    return nearest;
}

/* FUN_0045c8e2: (&DAT_00604808)[n*200] = 0, i.e. only the used flag is cleared. */
int editors_link_remove(int index)
{
    if (index < 0 || index >= OBL_RECORDS) return -1;
    put32(obl_rec(index), 0);
    ed.dirty_link = 1;
    if (ed.selected_link == index) ed.selected_link = -1;
    wos_log_event("editor_link_remove", "index=%d", index);
    status("removed link %d", index);
    return 0;
}

int editors_link_add(int x, int y, int object_id, int kind, int dest_map, int target)
{
    int i;
    for (i = 0; i < OBL_RECORDS; ++i) {
        unsigned char *r;
        if (le32(obl_rec(i)) != 0) continue;
        r = obl_rec(i);
        put32(r + 0, 1);
        put32(r + 4, object_id);
        put32(r + 8, x);
        put32(r + 12, y);
        put32(r + 16, kind);
        put32(r + 100, target);
        put32(r + 104, 0);
        put32(r + 108, -1);
        put32(r + 404, dest_map);
        ed.dirty_link = 1;
        ed.selected_link = i;
        wos_log_event("editor_link_add", "index=%d x=%d y=%d object=%d kind=%d dest_map=%d target=%d",
                      i, x, y, object_id, kind, dest_map, target);
        status("added link %d at %d,%d", i, x, y);
        return i;
    }
    wos_log_event("editor_link_add", "index=-1 reason=full");
    return -1;
}

/* --- terrain brush ----------------------------------------------------------------------- */

static unsigned char *ter_cell(int cx, int cy)
{
    int stride, row, off;
    if (!ed.ter_file || cx < 0 || cy < 0 || cx >= ed.ter_w || cy >= ed.ter_h) return NULL;
    stride = (ed.ter_w + 3) & ~3;
    off = le32(ed.ter_file + 10);
    row = ed.ter_h - 1 - cy;                 /* a BMP's rows run bottom-up */
    return ed.ter_file + off + (size_t)row * (size_t)stride + (size_t)cx;
}

int editors_terrain_get(int cell_x, int cell_y)
{
    const unsigned char *p = ter_cell(cell_x, cell_y);
    return p ? (int)*p : -1;
}

/* FUN_004642c7. The value is the current brush terrain when a button is down and 0 otherwise;
 * the cell is the cursor shifted right by two twice with the editor origin added before the
 * second shift, exactly as the decompilation reads. Both shifts are kept: the second one is
 * what the binary does, and the editor status line prints the resulting cell so the mapping
 * stays observable. */
int editors_terrain_paint(int x, int y, int button)
{
    int cx, cy;
    unsigned char *p;
    unsigned char value = button ? (unsigned char)ed.brush_terrain : 0;
    cx = (x + (x >> 31 & 3)) / 4 + ed.origin_x;
    cx = (cx + (cx >> 31 & 3)) / 4;
    cy = (y + (y >> 31 & 3)) / 4 + ed.origin_y;
    cy = (cy + (cy >> 31 & 3)) / 4;
    p = ter_cell(cx, cy);
    if (!p) {
        wos_log_event("editor_terrain", "x=%d y=%d cell_x=%d cell_y=%d state=outside", x, y, cx, cy);
        return 0;
    }
    if (*p == value) return 0;
    *p = value;
    ed.dirty_terrain = 1;
    wos_log_event("editor_terrain", "x=%d y=%d cell_x=%d cell_y=%d terrain=%d changed=1", x, y, cx, cy, value);
    return 1;
}

int editors_terrain_paint_brush(int x, int y, int button)
{
    int half = ed.brush_size / 2, dx, dy, changed = 0;
    for (dy = -half; dy <= half; ++dy)
        for (dx = -half; dx <= half; ++dx)
            changed += editors_terrain_paint(x + dx, y + dy, button);
    return changed;
}

/* FUN_004643aa: 0x20 -> 0x30 -> 0x40 -> 0x20, anything else -> 0x40. */
void editors_brush_cycle(void)
{
    if (ed.brush_size == EDIT_BRUSH_SMALL) ed.brush_size = EDIT_BRUSH_MEDIUM;
    else if (ed.brush_size == EDIT_BRUSH_MEDIUM) ed.brush_size = EDIT_BRUSH_LARGE;
    else ed.brush_size = EDIT_BRUSH_SMALL;
    wos_log_event("editor_brush", "size=%d", ed.brush_size);
    status("brush %d", ed.brush_size);
}

void editors_set_brush_terrain(int terrain_id) { ed.brush_terrain = terrain_id & 0xff; }
void editors_set_brush_monster(int monster_id) { ed.brush_monster = monster_id; }
int  editors_brush_monster(void) { return ed.brush_monster; }

/* --- monster placement -------------------------------------------------------------------- */

int editors_monster_free(void)
{
    int i;
    for (i = 0; i < MON_RECORDS; ++i) if (le32(mon_rec(i)) < 1) return i;
    return -1;
}

int editors_monster_get(int index, MonPlace *out)
{
    const unsigned char *r;
    if (index < 0 || index >= MON_RECORDS || !out) return -1;
    r = mon_rec(index);
    out->monster_id = le32(r);
    out->x = le32(r + 4);
    out->y = le32(r + 8);
    out->radius = le32(r + 12);
    return 0;
}

/* FUN_00464b06. A record is hit when the cursor cell is inside its rectangle grown by 4 on
 * every side; the winner is the smallest value of ((cy - top) - left) + cx, which is a
 * rotated distance with a constant +8 offset from the rectangle's half-width. */
int editors_monster_at(int x, int y)
{
    int i, best = 100000, found = -1;
    int cx = (x + (x >> 31 & 3)) / 4 + ed.origin_x;
    int cy = (y + (y >> 31 & 3)) / 4 + ed.origin_y;
    for (i = 0; i < MON_RECORDS; ++i) {
        const unsigned char *r = mon_rec(i);
        if (le32(r) >= 1) {
            int left = le32(r + 4) - 4, top = le32(r + 8) - 4;
            if (cx >= left && cx < le32(r + 4) + 4 && cy >= top && cy < le32(r + 8) + 4) {
                int d = ((cy - top) - left) + cx;
                if (d < best) { best = d; found = i; }
            }
        }
    }
    return found;
}

/* FUN_00464b06. With nothing under the cursor it claims the lowest free record, copies the
 * current brush monster into it and adopts the brush size as the radius; otherwise it moves
 * the record it found, and with `cycle_brush` set it first advances the brush size
 * (FUN_004643aa) and drags the radius along. Either way the file is marked dirty. */
int editors_monster_place(int x, int y, int monster_id, int move_mode, int cycle_brush)
{
    int cx = (x + (x >> 31 & 3)) / 4 + ed.origin_x;
    int cy = (y + (y >> 31 & 3)) / 4 + ed.origin_y;
    int under = editors_monster_at(x, y);
    int index, id = monster_id >= 0 ? monster_id : ed.brush_monster;
    unsigned char *r;
    if (move_mode) under = under >= 0 ? under : ed.selected_mon;
    else if (under < 0) under = -1;
    if (under < 0) {
        index = editors_monster_free();
        if (index < 0) {
            wos_log_event("editor_monster", "state=full");
            return -1;
        }
        if (id < 0) {
            wos_log_event("editor_monster", "state=no_brush_monster");
            return -1;
        }
        r = mon_rec(index);
        put32(r + 0, id);
        put32(r + 4, cx);
        put32(r + 8, cy);
        put32(r + 12, ed.brush_size);
    } else {
        index = under;
        r = mon_rec(index);
        ed.brush_size = le32(r + 12);
        if (cycle_brush) {
            editors_brush_cycle();
            put32(r + 12, ed.brush_size);
        }
        put32(r + 4, cx);
        put32(r + 8, cy);
    }
    ed.selected_mon = index;
    ed.brush_monster = le32(r);
    ed.dirty_mon = 1;
    wos_log_event("editor_monster", "index=%d id=%d x=%d y=%d radius=%d mode=%s",
                  index, le32(r), le32(r + 4), le32(r + 8), le32(r + 12),
                  (under >= 0 && move_mode) ? "move" : "place");
    status("monster %d id=%d at %d,%d r=%d", index, le32(r), le32(r + 4), le32(r + 8), le32(r + 12));
    return index;
}

int editors_monster_remove(int index)
{
    if (index < 0 || index >= MON_RECORDS) return -1;
    put32(mon_rec(index), 0);
    ed.dirty_mon = 1;
    if (ed.selected_mon == index) ed.selected_mon = -1;
    wos_log_event("editor_monster_remove", "index=%d", index);
    status("removed monster %d", index);
    return 0;
}

/* --- world signing ------------------------------------------------------------------------ */

/* FUN_00497106, instruction for instruction:
 *   497106  mov $0x402c8e17,%eax
 *   49710b  mov 0x4(%esp),%ecx
 *   497112  je done
 *   497114  movsbl (%ecx),%edx        ; signed char
 *   497117  add %eax,%eax             ; h * 2, wrapping
 *   497119  inc %ecx
 *   49711a  imul %eax,%edx
 *   49711d  mov %edx,%eax
 *   49711f  cmpb $0x0,(%ecx) ; jne loop
 * The multiply is signed 32x32 and only the low 32 bits survive, so the C form is unsigned. */
uint32_t editors_ver_hash(const char *s)
{
    uint32_t h = WORLD_VER_HASH_SEED;
    for (; s && *s; ++s) {
        int32_t c = (int32_t)(int8_t)*s;
        h = (uint32_t)c * (h * 2u);
    }
    return h;
}

/* FUN_00497080, instruction for instruction:
 *   49708c-49709c  sprintf(path, "%s\\worlds\\%s\\world.ver", <root>, DAT_004E0BD0)
 *   4970a9-4970af  f = fopen(path, "wb")
 *   4970be-4970ce  memset(rec, 0, 0x4A4)
 *   4970d6-4970df  fwrite(rec, 0x4A8, 1, f)
 *   4970e8-4970e9  fclose(f)
 * The 0x4A8..0x4AF window is never written: the struct is zeroed to 0x4A4 and 0x4A8 bytes are
 * written, so bytes 0x4A4..0x4A7 are whatever was already in the buffer. In the original that
 * buffer is a fixed global and 0x4970D1 stores memset's own return value (base + 0x4A4) there, so
 * the original leaks an address into its own world.ver. It is not reproducible, so the port
 * leaves those four bytes zero and says so in the log. */
static int ver_write_path(char *buf, size_t size)
{
    char rel[256];
    const char *root = data_dir[0] ? data_dir : game_data_path();
    if (!g_world.name[0]) return -1;
    if (snprintf(rel, sizeof(rel), "worlds/%s/world.ver", g_world.name) < 0) return -1;
    if (snprintf(buf, size, "%s/%s", root, rel) < 0) return -1;
    return 0;
}

int editors_sign_world(const char *name, const char *version_text, uint32_t id, uint32_t author_serial)
{
    unsigned char rec[WORLD_VER_SIZE];
    char path[1024];
    FILE *f;
    if (ver_write_path(path, sizeof(path))) return -1;
    memset(rec, 0, sizeof(rec));
    /* The field stores, all inside FUN_00497200-0x497540:
     *   497250 mov %eax,0xd30c80  -> base+0x00 is the id, and 0x49732A base+0x58 again
     *   49732F strncpy(base+0x18, name, 0x3F) ; 0x497348 movb $0,base+0x57
     *   497339 mov %ecx,base+0x14  and  0x49736B mov %ecx,base+0x5C   (DAT_004DD20C)
     *   49732A mov %eax,base+0x58  and  0x49734E strncpy(base+0x60, name, 0x3F)
     *   49738F mov %eax,base+0xA0  (FUN_00497106's return)
     *   497371 movl $0xA97,base+0xA4
     *   497200 mov %eax,base+0x10  (the id again; FUN_0049747C compares it with the player) */
    put32(rec + WORLD_VER_ID, (int)id);
    put32(rec + WORLD_VER_ID2, (int)id);
    put32(rec + WORLD_VER_ID3, (int)id);
    put32(rec + WORLD_VER_AUTHOR, (int)author_serial);
    disk_put(rec + WORLD_VER_CREATOR, name, WORLD_VER_NAME_MAX);
    put32(rec + WORLD_VER_SERIAL, (int)author_serial);
    disk_put(rec + WORLD_VER_LASTMOD, name, WORLD_VER_NAME_MAX);
    put32(rec + WORLD_VER_HASH, (int)editors_ver_hash(version_text));
    put32(rec + WORLD_VER_FORMAT_AT, (int)WORLD_VER_FORMAT);
    f = plat_fopen(path, "wb");
    if (!f) return -1;
    if (fwrite(rec, 1, sizeof(rec), f) != sizeof(rec) || fclose(f)) return -1;
    wos_log_event("world_sign", "state=ok name=%s id=%08X serial=%08X hash=%08X format=%04X "
                  "version_text=%s", name, id, author_serial,
                  editors_ver_hash(version_text), WORLD_VER_FORMAT, version_text);
    return 0;
}

static int ver_read(const char *world, unsigned char *rec)
{
    char path[1024], rel[256];
    const char *root = data_dir[0] ? data_dir : game_data_path();
    const char *name = world && *world ? world : g_world.name;
    FILE *f;
    if (!name || !*name) return -1;
    if (snprintf(rel, sizeof(rel), "worlds/%s/world.ver", name) < 0) return -1;
    if (snprintf(path, sizeof(path), "%s/%s", root, rel) < 0) return -1;
    f = plat_fopen(path, "rb");
    if (!f) return -1;
    if (fread(rec, 1, WORLD_VER_SIZE, f) != WORLD_VER_SIZE) { fclose(f); return -1; }
    fclose(f);
    return 0;
}

int editors_world_version(const char *world, char creator[WORLD_VER_NAME_MAX],
                          char last_mod[WORLD_VER_NAME_MAX], uint32_t *serial)
{
    unsigned char rec[WORLD_VER_SIZE];
    if (ver_read(world, rec)) return -1;
    disk_get(creator, WORLD_VER_NAME_MAX, rec + WORLD_VER_CREATOR, WORLD_VER_NAME_MAX - 1);
    disk_get(last_mod, WORLD_VER_NAME_MAX, rec + WORLD_VER_LASTMOD, WORLD_VER_NAME_MAX - 1);
    if (serial) *serial = (uint32_t)le32(rec + WORLD_VER_SERIAL);
    wos_log_event("world_version", "world=%s creator=%s last_mod=%s serial=%08X id=%08X format=%04X",
                  (world && *world) ? world : g_world.name, creator, last_mod,
                  (unsigned)((serial) ? *serial : 0u),
                  (unsigned)le32(rec + WORLD_VER_ID), (unsigned)le32(rec + WORLD_VER_FORMAT_AT));
    return 0;
}

/* FUN_0049747C: `cmp %ecx,0xd307e8` where ecx is DAT_004FA95C, the player's own id. Equal means
 * the official version, otherwise 0x4974DE formats "Seems to be a MOD (%04X)" with the low 16
 * bits of the player's id. */
int editors_world_is_mod(uint32_t player_id)
{
    unsigned char rec[WORLD_VER_SIZE];
    if (ver_read(NULL, rec)) return -1;
    return (uint32_t)le32(rec + WORLD_VER_ID2) != player_id;
}

uint32_t editors_world_format(const char *world)
{
    unsigned char rec[WORLD_VER_SIZE];
    if (ver_read(world, rec)) return 0;
    return (uint32_t)le32(rec + WORLD_VER_FORMAT_AT);
}

/* --- screen ------------------------------------------------------------------------------- */

static void editors_prompt_answer(int answer);
static void editors_finish_leave(int written);

static const char *const tool_names[] = { "link", "terrain", "monster", "move" };

static void editor_enter_screen(void);

static int in_rect(Rect r, int x, int y)
{ return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h; }

static void editor_update(const Input *in)
{
    int i;
    if (save_prompt) {
        if ((in->mouse_released & 2u) && in_rect(prompt_yes, in->mouse_x, in->mouse_y)) {
            editors_prompt_answer(IDYES); return;
        }
        if ((in->mouse_released & 2u) && in_rect(prompt_no, in->mouse_x, in->mouse_y)) {
            editors_prompt_answer(IDNO); return;
        }
        if (in->pressed['y'] || in->pressed['Y'] || in->pressed[PLAT_KEY_RETURN]) {
            editors_prompt_answer(IDYES); return;
        }
        if (in->pressed['n'] || in->pressed['N'] || in->pressed[PLAT_KEY_ESCAPE]) {
            editors_prompt_answer(IDNO); return;
        }
        return;   /* the prompt is modal: it swallows everything else */
    }
    ed.mouse_x = in->mouse_x;
    ed.mouse_y = in->mouse_y;
    if (in->pressed[PLAT_KEY_ESCAPE]) { editors_leave(); return; }
    if (in->pressed['1']) { ed.tool = EDIT_TOOL_LINK;   wos_log_event("editor_tool", "tool=link"); }
    if (in->pressed['2']) { ed.tool = EDIT_TOOL_TERRAIN; wos_log_event("editor_tool", "tool=terrain"); }
    if (in->pressed['3']) { ed.tool = EDIT_TOOL_MONSTER; wos_log_event("editor_tool", "tool=monster"); }
    if (in->pressed['4']) { ed.tool = EDIT_TOOL_MOVE;    wos_log_event("editor_tool", "tool=move"); }
    /* N places a new link at the cursor whatever is under it, the editor's "Add" entry
     * (FUN_0045c9BD claims a record, FUN_0045ca09 refreshes the list). */
    if (in->pressed['n'])
        editors_link_add(ed.mouse_x, ed.mouse_y, ed.object_id, 2, ed.map_id, 0);
    if (in->pressed['b']) editors_brush_cycle();
    if (in->pressed['s']) editors_save();
    if (in->pressed['d'] && ed.selected_link >= 0) editors_link_remove(ed.selected_link);
    if (in->pressed['x'] && ed.selected_mon >= 0) editors_monster_remove(ed.selected_mon);
    if (in->pressed[PLAT_KEY_TAB]) {
        int next = -1;
        for (i = ed.map_id + 1; i < WORLD_MAX_MAPS; ++i)
            if (g_world.maps[i].used) { next = i; break; }
        if (next < 0) for (i = 0; i < ed.map_id; ++i) if (g_world.maps[i].used) { next = i; break; }
        if (next >= 0) { editors_save(); editors_load_map(next); }
    }
    if (in->mouse_pressed & 2u) {   /* button 1 = left, so bit 1<<1 */
        switch (ed.tool) {
        case EDIT_TOOL_LINK: {
            /* Select what is under the cursor, otherwise claim a free record for a new link. */
            int hit = editors_link_at(ed.mouse_x, ed.mouse_y);
            if (hit < 0) editors_link_add(ed.mouse_x, ed.mouse_y, ed.object_id, 2, ed.map_id, 0);
            break;
        }
        case EDIT_TOOL_TERRAIN:
            editors_terrain_paint_brush(ed.mouse_x, ed.mouse_y, 1);
            break;
        case EDIT_TOOL_MONSTER:
            editors_monster_place(ed.mouse_x, ed.mouse_y, -1, 0, 0);
            break;
        case EDIT_TOOL_MOVE:
            if (ed.selected_link >= 0) {
                Link l;
                if (editors_link_get(ed.selected_link, &l) == 0) {
                    l.x = ed.mouse_x; l.y = ed.mouse_y;
                    editors_link_set(ed.selected_link, &l);
                }
            }
            break;
        default: break;
        }
    } else if (in->mouse_pressed & 8u) {   /* button 3 = right, so bit 1<<3 */
        /* right button erases, matching FUN_004642c7's zero value */
        if (ed.tool == EDIT_TOOL_TERRAIN) editors_terrain_paint_brush(ed.mouse_x, ed.mouse_y, 0);
        else if (ed.tool == EDIT_TOOL_MONSTER && ed.selected_mon >= 0) editors_monster_remove(ed.selected_mon);
        else if (ed.selected_link >= 0) editors_link_remove(ed.selected_link);
    }
}

static void editor_render(Framebuffer *fb)
{
    static char line[160];
    int i;
    fb_clear(fb, 0x101018);
    if (ed_have_map && ed_map.image.pixels)
        fb_blit(fb, &ed_map.image, 0, 0, -1);
    if (ed.tool == EDIT_TOOL_TERRAIN && ed_have_map && ed_map.terrain.indices) {
        /* one .ter cell covers 4x4 map units */
        for (i = 0; i < ed.ter_w * ed.ter_h && i < 4096; ++i) {
            int cx = i % ed.ter_w, cy = i / ed.ter_w;
            fb_fill(fb, (Rect){ cx * 4, cy * 4, 4, 4 },
                    0x00ff00u | ((unsigned)ed_map.terrain.indices[i] << 8));
        }
    }
    for (i = 0; i < OBL_RECORDS; ++i) {
        const unsigned char *r = obl_rec(i);
        if (le32(r) != 0) {
            uint32_t c = i == ed.selected_link ? 0xffff40u : 0x40c0ffu;
            fb_rect(fb, (Rect){ le32(r + 8) - 2, le32(r + 12) - 2, 5, 5 }, c);
        }
    }
    for (i = 0; i < MON_RECORDS; ++i) {
        const unsigned char *r = mon_rec(i);
        if (le32(r) >= 1)
            fb_rect(fb, (Rect){ le32(r + 4) * 4 - 2, le32(r + 8) * 4 - 2, 5, 5 },
                    i == ed.selected_mon ? 0xffff40u : 0x40ff60u);
    }
    fb_fill(fb, (Rect){ 0, PLAT_SCREEN_H - 40, PLAT_SCREEN_W, 40 }, 0x000018);
    snprintf(line, sizeof(line), "Link Editor - map%d: %s  [1 link 2 terrain 3 monster 4 move  B brush  S save  TAB map  ESC exit]",
             ed.map_id, g_world.name);
    font_draw(fb, 4, PLAT_SCREEN_H - 38, line, 0xffffff);
    font_draw(fb, 4, PLAT_SCREEN_H - 26, ed.status, 0xdfd080);
    if (save_prompt) {
        const MapDef *def = (ed.map_id >= 0 && ed.map_id < WORLD_MAX_MAPS) ? &g_world.maps[ed.map_id] : NULL;
        fb_fill(fb, (Rect){ 120, 200, 400, 120 }, 0x1c1c28);
        fb_rect(fb, (Rect){ 120, 200, 400, 120 }, 0xffdc80);
        snprintf(line, sizeof(line), "Save your link changes for map %s?", def ? def->root : "?");
        font_draw(fb, 140, 214, line, 0xffffff);
        font_draw(fb, 140, 232, "Hmmm..", 0xa0a8b0);
        fb_fill(fb, prompt_yes, 0x353a50);
        fb_rect(fb, prompt_yes, 0xffdc80);
        font_draw(fb, prompt_yes.x + 30, prompt_yes.y + 10, "Yes", 0xffffff);
        fb_fill(fb, prompt_no, 0x353a50);
        fb_rect(fb, prompt_no, 0xa0a8b0);
        font_draw(fb, prompt_no.x + 30, prompt_no.y + 10, "No", 0xffffff);
        return;
    }
    snprintf(line, sizeof(line), "tool=%s brush=%d terrain=%d monster=%d at %d,%d  links=%d mons=%d",
             tool_names[ed.tool], ed.brush_size, ed.brush_terrain, ed.brush_monster,
             ed.mouse_x, ed.mouse_y, ed.selected_link, ed.selected_mon);
    font_draw(fb, 4, PLAT_SCREEN_H - 14, line, 0x8080c0);
}

static const Screen editor_screen = { "editor", editor_enter_screen, editor_update, editor_render, NULL };

static void editor_enter_screen(void)
{
    status("editor ready");
}

/* The World Editor's map/world combo is FUN_0040f688 over "%s\\worlds\\%s" (string
 * 0x4de674): every folder under <data>/worlds, skipping the ones without a quest.txt. With no
 * world chosen yet - the "... or Create Your Own World ..." path - take the first one, which is
 * the only world an offline install has to edit. */
typedef struct { const char *base; char name[64]; int found; } WorldPick;
static void pick_world_entry(const char *name, int is_dir, void *user)
{
    WorldPick *pick = (WorldPick *)user;
    char path[1024];
    FILE *f;
    if (pick->found || !is_dir || !*name || name[0] == '.') return;
    if (snprintf(path, sizeof(path), "%s/worlds/%s/quest.txt", pick->base, name) < 0) return;
    f = plat_fopen(path, "rb");
    if (!f) return;
    fclose(f);
    snprintf(pick->name, sizeof(pick->name), "%s", name);
    pick->found = 1;
}

static int editors_pick_world(char *out, size_t size)
{
    char root[1024];
    WorldPick pick;
    const char *base = data_dir[0] ? data_dir : game_data_path();
    pick.base = base;
    pick.found = 0;
    pick.name[0] = 0;
    if (snprintf(root, sizeof(root), "%s/worlds", base) < 0) return -1;
    if (plat_list_dir(root, pick_world_entry, &pick) < 0 || !pick.found) return -1;
    if (snprintf(out, size, "%s", pick.name) < 0) return -1;
    return 0;
}

int editors_enter(const char *world)
{
    int i, map_id = -1;
    char chosen[64];
    if (!world || !*world) {
        if (!g_world.name[0]) {
            if (editors_pick_world(chosen, sizeof(chosen))) {
                wos_log_event("editor_error", "reason=no_world");
                return -1;
            }
            world = chosen;
        }
    }
    if (editors_open(world)) return -1;
    for (i = 0; i < WORLD_MAX_MAPS; ++i) if (g_world.maps[i].used) { map_id = i; break; }
    if (map_id < 0) return -1;
    if (editors_load_map(map_id)) return -1;
    screen_set(&editor_screen);
    wos_log_event("editor_screen", "state=enter world=%s map=%d", g_world.name, map_id);
    return 0;
}

/* FUN_0045C7D7: FUN_0045DBC5 first (terrain and monsters go out at once), then, if the link
 * changed flag at this+0x74 is still set, CWnd::MessageBoxA(this, "Save your link changes for
 * map %s?", "Hmmm..", 4) with MB_YESNO. IDYES runs FUN_0045C9BD (the .obl write) and either
 * answer then clears the flag. */
static void editors_finish_leave(int written)
{
    wos_log_event("editor_screen", "state=leave saved=%d", written);
    wos_log_event("editor_leave", "saved=%d", written);
    editors_close();
    game_go_front();
}

static void editors_prompt_answer(int answer)
{
    int written = 0;
    save_prompt = 0;
    if (answer == IDYES) written = editors_save();
    else { ed.dirty_link = ed.dirty_mon = ed.dirty_terrain = 0; }
    wos_log_event("editor_prompt", "id=%d answer=%s saved=%d",
                  EDITOR_DIALOG_SAVE_LINKS, answer == IDYES ? "IDYES" : "IDNO", written);
    editors_finish_leave(written);
}

int editors_save_prompt_active(void) { return save_prompt; }

int editors_dialog_op(int dialog_id, const char *const *kv, int n, int ok)
{
    (void)kv; (void)n;
    if (dialog_id != EDITOR_DIALOG_SAVE_LINKS || !save_prompt) return 0;
    /* The `dialog` script op ends in ok|cancel; the MessageBox has no Cancel, so cancel is
     * IDNO, which is what MB_YESNO's second button returns. */
    editors_prompt_answer(ok ? IDYES : IDNO);
    return 1;
}

void editors_leave(void)
{
    const MapDef *def;
    if (save_prompt) return;
    /* FUN_0045DBC5 writes the terrain and the monsters at once; the .obl is held back for
     * the MessageBox below. */
    if (ed.dirty_terrain) editors_save_terrain();
    if (ed.dirty_mon) editors_save_monsters();
    if (!ed.dirty_link) { editors_finish_leave(0); return; }
    def = (ed.map_id >= 0 && ed.map_id < WORLD_MAX_MAPS) ? &g_world.maps[ed.map_id] : NULL;
    save_prompt = 1;
    prompt_yes = (Rect){ 200, 258, 100, 28 };
    prompt_no = (Rect){ 340, 258, 100, 28 };
    wos_log_event("editor_prompt", "id=%d state=open map=%s dirty_link=1 dirty_mon=%d dirty_ter=%d",
                  EDITOR_DIALOG_SAVE_LINKS, def ? def->root : "?", ed.dirty_mon, ed.dirty_terrain);
}

int editors_active(void) { return ed.open; }

/* --- dump ---------------------------------------------------------------------------------- */

void editors_dump(DumpEmit emit, void *user)
{
    char buf[64];
    int i, used = 0, mons = 0;
    if (!ed.open) { emit("editor.open", "0", user); return; }
    emit("editor.open", "1", user);
    dump_emit_int(emit, "editor.map", ed.map_id, user);
    dump_emit_int(emit, "editor.dirty_link", ed.dirty_link, user);
    dump_emit_int(emit, "editor.dirty_mon", ed.dirty_mon, user);
    dump_emit_int(emit, "editor.dirty_terrain", ed.dirty_terrain, user);
    dump_emit_int(emit, "editor.brush_size", ed.brush_size, user);
    dump_emit_int(emit, "editor.brush_terrain", ed.brush_terrain, user);
    dump_emit_int(emit, "editor.tool", ed.tool, user);
    dump_emit_int(emit, "editor.selected_link", ed.selected_link, user);
    dump_emit_int(emit, "editor.selected_mon", ed.selected_mon, user);
    for (i = 0; i < OBL_RECORDS; ++i) if (le32(obl_rec(i)) != 0) ++used;
    for (i = 0; i < MON_RECORDS; ++i) if (le32(mon_rec(i)) >= 1) ++mons;
    dump_emit_int(emit, "editor.links_used", used, user);
    dump_emit_int(emit, "editor.monsters_placed", mons, user);
    for (i = 0; i < OBL_RECORDS; ++i) {
        const unsigned char *r = obl_rec(i);
        if (le32(r) == 0) continue;
        snprintf(buf, sizeof(buf), "%d,%d,%d,%d,%d,%d", i, le32(r + 8), le32(r + 12),
                 le32(r + 16), le32(r + 404), le32(r + 100));
        {
            char key[32];
            snprintf(key, sizeof(key), "editor.link.%d", i);
            emit(key, buf, user);
        }
    }
}
