/* Authoring editors: the original's Link Editor, terrain brush, monster placement and the
 * World Editor's world signing (front view states 7 and 9/10, DAT_004df8a4).
 *
 * The original keeps the world binaries in memory as two flat byte arrays and edits them in
 * place, then dumps them verbatim:
 *   DAT_00604808  256 x 800-byte .obl link records    (FUN_00463989 load, FUN_0046393f save)
 *   DAT_00636808 1000 x 276-byte .mon placement records (FUN_00464461 load, FUN_00464421 save)
 *   the .ter DIB  8-bit, one cell per 4x4 map units  (FUN_004867c0 save)
 * This module keeps the same raw records, so a record the user did not touch re-serializes to
 * exactly the bytes that were read and a byte diff against the retail file is empty except at
 * the edited records. Owner: editors.c. */
#ifndef WOS_EDITORS_H
#define WOS_EDITORS_H

#include <stddef.h>
#include <stdint.h>
#include "world.h"
#include "../engine/dump.h"
#include "../engine/screen.h"

/* Offset of the "a hero has traversed this link" flag inside an .obl record (0x1A0). The
 * original sets it in FUN_00462958 at &DAT_006049a8 = DAT_00604808 + 0x1A0. */
#define OBL_OFFSET_TRAVERSED 416
/* Offset of the cursor-distance scratch inside an .obl record (0x190): FUN_004636d3 writes the
 * Manhattan distance there for every link and stores 0 in the one under the cursor. */
#define OBL_OFFSET_DISTANCE  400
/* Offset of the runtime monster-instance pointer inside a .mon record (0x10): FUN_00464461
 * zeroes it on load and FUN_004643ed walks it. */
#define MON_OFFSET_INSTANCE  16
/* Size of <world>/world.ver (the "World Version Info" file). */
#define WORLD_VER_SIZE       1192
#define WORLD_VER_NAME_MAX   64

/* Terrain brush sizes cycled by FUN_004643aa (map units). */
#define EDIT_BRUSH_SMALL  0x20
#define EDIT_BRUSH_MEDIUM 0x30
#define EDIT_BRUSH_LARGE  0x40

/* Editing tool selected in the editor screen (the original's *piVar5 = this + 0x70). */
typedef enum { EDIT_TOOL_LINK, EDIT_TOOL_TERRAIN, EDIT_TOOL_MONSTER, EDIT_TOOL_MOVE } EditorTool;

typedef struct {
    int open;
    int map_id;                     /* DAT_004e0ddc */
    /* Raw world binaries, exactly as the original holds them. */
    unsigned char obl[OBL_RECORDS * OBL_RECORD_SIZE];
    unsigned char mon[MON_RECORDS * MON_RECORD_SIZE];
    unsigned char *ter_file;        /* the whole .ter file, header + palette + cells */
    size_t ter_size;
    int ter_w, ter_h;               /* cells; 1 cell = 4x4 map units */
    int dirty_link, dirty_mon, dirty_terrain;

    /* Editor state: DAT_004f2178 link, DAT_004f225c monster, DAT_004f2250 brush size,
     * DAT_004f0ce0 brush terrain, DAT_004f8830 brush monster, DAT_005394b0/b4 origin. */
    int selected_link, selected_mon;
    int brush_size, brush_terrain, brush_monster;
    int object_id;                   /* objects.obr row a new link claims (DAT_004f2178's palette) */
    int origin_x, origin_y;
    int tool;
    int mouse_x, mouse_y;           /* map units under the cursor */
    char status[128];
} Editor;

/* --- Screen lifecycle (FrontHero-2 calls editors_enter from the world-select state) -------- */

/* Overrides the data root world_load() and world_data_path() resolve against. Empty (the
 * default) means the --data directory from the command line. Set it to a scratch copy when a
 * test must never touch the retail tree. */
void editors_set_data_dir(const char *dir);
const char *editors_data_dir(void);

/* Loads `world` (a folder name under <data>/worlds; NULL or "" keeps the loaded world) and
 * switches to the editor screen. Returns 0 on success, -1 if the world will not load. */
int  editors_enter(const char *world);
/* Saves anything dirty and returns to the front end's world-select list, so a newly created
 * world is rescanned. Called by the editor's Cancel; FrontHero does not call it. */
void editors_leave(void);
int  editors_active(void);

/* --- Data (usable without the screen) ---------------------------------------------------- */

/* Loads <data>/worlds/<world> if `world` is non-empty. 0 on success. */
int  editors_open(const char *world);
/* Loads the .obl/.mon/.ter of map `map_id` out of the currently loaded world. 0 on success. */
int  editors_load_map(int map_id);
void editors_close(void);
const Editor *editors_state(void);
int  editors_dirty(void);
/* Writes every dirty file (FUN_0045dbc5). Returns the number of files written, -1 on error. */
int  editors_save(void);
/* <world>/maps/<root>.obl only (FUN_0045c9bd -> FUN_0046393f). 0 on success. */
int  editors_save_links(void);
/* <world>/maps/<root>.mon only (FUN_00464421). 0 on success. */
int  editors_save_monsters(void);
/* <world>/maps/<root>.ter only (FUN_004867c0). 0 on success. */
int  editors_save_terrain(void);

/* --- Link editor (FUN_004636d3 hit test, FUN_0045db67 record edit, FUN_0045c8e2 remove) ---- */

/* Decodes .obl record `index` into the caller's Link. 0 on success, -1 if out of range. */
int  editors_link_get(int index, Link *out);
/* Encodes the known fields of `in` over .obl record `index` and marks the file dirty.
 * 0 on success, -1 if out of range. */
int  editors_link_set(int index, const Link *in);
/* Record under map unit (x, y): FUN_004636d3's sprite-rectangle test, stopping at the first
 * hit. Also writes the distance scratch of every record and updates the selection.
 * Returns the record index or -1. */
int  editors_link_at(int x, int y);
/* Nearest record to (x, y) whether or not the point is inside its sprite (FUN_004636d3's
 * Manhattan-distance fallback). Returns the index or -1. */
int  editors_link_nearest(int x, int y);
/* Clears the used flag of record `index` (FUN_0045c8e2) and marks the file dirty. */
int  editors_link_remove(int index);
/* Claims the lowest free record and writes a new link at (x, y). Returns the new record index,
 * or -1 when all 256 records are used. */
int  editors_link_add(int x, int y, int object_id, int kind, int dest_map, int target);

/* --- Terrain brush (FUN_004642c7 paint, FUN_004643aa size cycle) -------------------------- */

/* Paints terrain at map unit (x, y). `button` non-zero paints brush_terrain, zero erases to 0.
 * Returns 1 if a cell changed. */
int  editors_terrain_paint(int x, int y, int button);
/* Paints the whole current brush square centred on (x, y). Returns the cells changed. */
int  editors_terrain_paint_brush(int x, int y, int button);
/* FUN_004643aa: 0x20 -> 0x30 -> 0x40 -> 0x20, anything else -> 0x40. */
void editors_brush_cycle(void);
/* The terrain id the brush paints (DAT_004f0ce0) and the monster id a new placement claims
 * (DAT_004f8830). The monster id of -1 means "no monster selected", which is FUN_00464b06's
 * early return. */
void editors_set_brush_terrain(int terrain_id);
void editors_set_brush_monster(int monster_id);
int  editors_brush_monster(void);
/* Terrain id at editor cell (cell_x, cell_y), or -1 outside the map. */
int  editors_terrain_get(int cell_x, int cell_y);

/* --- Monster placement (FUN_00464b06) ---------------------------------------------------- */

/* Lowest record index whose monster id is < 1, or -1 (FUN_00464b06's local_c). */
int  editors_monster_free(void);
/* Decodes .mon record `index`. 0 on success, -1 if out of range. */
int  editors_monster_get(int index, MonPlace *out);
/* Record under map unit (x, y), using FUN_00464b06's +/-4 cell hit box and its rotated
 * distance. Returns the index or -1. */
int  editors_monster_at(int x, int y);
/* Places or moves a monster: FUN_00464b06. With `move_mode` non-zero an existing record under
 * the cursor is moved instead of a new one claimed; with `cycle_brush` non-zero the brush size
 * is advanced first (FUN_004643aa) and the record's radius follows it. `monster_id` < 0 uses
 * the current brush monster. Returns the record index, or -1 when there is no free record. */
int  editors_monster_place(int x, int y, int monster_id, int move_mode, int cycle_brush);
/* Clears the monster id of record `index` and marks the file dirty. */
int  editors_monster_remove(int index);

/* --- World signing (the World Editor's "Set Version", dialog "World Version Info") ---------
 *
 * FUN_00497080 (0x497080) is the writer and FUN_00497106 (0x497106) is the hash, both
 * disassembled instruction by instruction:
 *   497080  memset(rec, 0, 0x4A4); f = fopen(path, "wb");
 *           if (f) { fwrite(rec, 0x4A8, 1, f); fclose(f); }
 *   497106  h = 0x402C8E17; for (; *s; ++s) h = (int8)*s * (h * 2); return h;
 * The record is 0x4A8 = 1192 bytes; only the fields below are ever stored, everything else stays
 * zero from the memset. */
#define WORLD_VER_SIZE_BODY 0x4A4        /* what FUN_00497080 memsets */
#define WORLD_VER_FORMAT    0x00000A97u  /* struct+0xA4, this build's world-format version */
#define WORLD_VER_HASH_SEED 0x402C8E17u  /* struct+0xA0's initial value */

/* Field offsets inside <world>/world.ver. */
#define WORLD_VER_ID        0x00  /* u32, the dialog's id (its -0x14(%ebp) argument) */
#define WORLD_VER_ID2       0x10  /* u32, the same id; FUN_0049747C compares it with the player's */
#define WORLD_VER_AUTHOR    0x14  /* u32, DAT_004DD20C, the signing user's id */
#define WORLD_VER_CREATOR   0x18  /* 64 bytes, NUL padded (strncpy with 0x3F) */
#define WORLD_VER_ID3       0x58  /* u32, the same id again */
#define WORLD_VER_SERIAL    0x5C  /* u32, DAT_004DD20C again */
#define WORLD_VER_LASTMOD   0x60  /* 64 bytes, NUL padded */
#define WORLD_VER_HASH      0xA0  /* u32, FUN_00497106 over the dialog's -0x9C(%ebp) string */
#define WORLD_VER_FORMAT_AT 0xA4  /* u32, WORLD_VER_FORMAT */

/* FUN_00497106 exactly: h = 0x402C8E17, then h = (signed char)c * (h * 2) per character, 32-bit. */
uint32_t editors_ver_hash(const char *s);

/* Writes <world>/world.ver exactly as FUN_00497080 does.
 *   name          - the author, strncpy'd into WORLD_VER_CREATOR and WORLD_VER_LASTMOD
 *   version_text  - the string FUN_00497106 hashes into WORLD_VER_HASH
 *   id            - the dialog's id, stored at WORLD_VER_ID, _ID2 and _ID3
 *   author_serial - DAT_004DD20C, stored at WORLD_VER_AUTHOR and WORLD_VER_SERIAL
 * Returns 0 on success, -1 if the file cannot be written. */
int editors_sign_world(const char *name, const char *version_text, uint32_t id, uint32_t author_serial);
/* Decodes the readable fields of <data>/worlds/<world>/world.ver. Returns 0 on success. */
int editors_world_version(const char *world, char creator[WORLD_VER_NAME_MAX],
                          char last_mod[WORLD_VER_NAME_MAX], uint32_t *serial);
/* FUN_0049747C: the file is a MOD when its WORLD_VER_ID2 differs from the player's id. */
int editors_world_is_mod(uint32_t player_id);
/* The world-format version stored at WORLD_VER_FORMAT_AT, or 0 when the file is unreadable. */
uint32_t editors_world_format(const char *world);

/* --- The "Save your link changes?" prompt (FUN_0045C7D7) ---------------------------------- */

/* FUN_0045C7D7 asks CWnd::MessageBoxA("Save your link changes for map %s?", "Hmmm..", 4),
 * where 4 is MB_YESNO. The port draws the same question in framebuffer and answers with the
 * same ids, so a replay can drive it through the `dialog` op. */
#define EDITOR_DIALOG_SAVE_LINKS 0xF001
#define EDITOR_MB_YESNO          4
#define IDYES 6
#define IDNO  7
/* 1 while the prompt is up. */
int editors_save_prompt_active(void);
/* The `dialog` op entry point: `id` is EDITOR_DIALOG_SAVE_LINKS, `ok` non-zero is IDYES.
 * Returns 1 when the prompt consumed the operation. */
int editors_dialog_op(int dialog_id, const char *const *kv, int n, int ok);

void editors_dump(DumpEmit emit, void *user);

#endif
