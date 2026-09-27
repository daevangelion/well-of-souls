/* World data model: quest.txt tables, scene script index, per-map binaries, sprite sheets.
 * Evidence: docs/re/script.md (loader FUN_00479594..FUN_00479a02), docs/re/maps.md (map loader
 * FUN_0041e421), docs/re/art.md (filmstrip loader FUN_0048df3c).
 * Owner: world.c. Other modules read these structs; additive changes only. */
#ifndef WOS_WORLD_H
#define WOS_WORLD_H

#include <stdint.h>
#include "../engine/image.h"

#define WORLD_MAX_MAPS      1000  /* maps.txt id range, FUN_00482c54 rejects id > 999 */
#define WORLD_MAX_MONSTERS  4096  /* monsters.txt arg0 1..4095 */
#define WORLD_MAX_GROUPS    4096  /* groups.txt, FUN_00483686 */
#define WORLD_MAX_CLASSES   89    /* levels.txt class < 89, FUN_00483984 */
#define WORLD_MAX_LEVELS    100
#define WORLD_MAX_ITEMS     5120  /* items.txt 1..5119 */
#define WORLD_MAX_SPELLS    768
#define WORLD_MAX_TERRAINS  256
#define WORLD_MAX_SCENES    10000
#define GROUP_MAX_MEMBERS   9
#define OBL_RECORDS         256   /* .obl = 256 x 800 bytes */
#define OBL_RECORD_SIZE     800
#define MON_RECORDS         1000  /* .mon = 1000 x 276 bytes */
#define MON_RECORD_SIZE     276
#define OBR_RECORDS         1000  /* objects.obr = 1000 x 48 bytes */
#define OBR_RECORD_SIZE     48

typedef struct {
    int used;
    char image[64];   /* e.g. "evergreen.jpg" */
    char root[64];    /* base name of .obl/.ter/.mon and music.ini section */
    char name[64];    /* display name */
    uint32_t flags;   /* MAP_FLAG_* bitmask (quest.txt +MAPS docs) */
    int theme;
} MapDef;

typedef struct {
    int used;
    char name[32];
    int damage;
    int token;        /* token that permits crossing (0 = none) */
} TerrainDef;

typedef struct {
    int used;
    char name[32];
    char skin[64];    /* sprite sheet base name (monsters/ folder) */
    int scale, flags; /* arg3 scaleFactor[.monsterFlags] */
    int element;
    int hp, mp, defense, offense, exp, gold, level;
    int strength, stamina, agility, dexterity, wisdom;
    char growl_wav[64], pain_wav[64];
    int attack_path;
    int spells[16];
    int spell_count;
} MonsterDef;

typedef struct {
    int used;
    int members[GROUP_MAX_MEMBERS];
    int count;
} GroupDef;

typedef struct {
    int used;
    int d_hp, d_mp;
    char name[64];    /* level title (first gender variant) */
} LevelDef;

typedef struct {
    int used;
    char name[64];
    char description[256];
    int magic_ratio, right_hand;
    int start_ability[5];       /* str, wis, sta, agi, dex */
    int auto_max[5];            /* minHP, maxHP, minMP, maxMP, startMPLevel; auto_max_set flags presence */
    int auto_max_set;
    int start_map, start_link, start_drop_in, start_location_set;
    char default_skin[4][64];   /* per gender 0..3 */
    int start_items[8];
    int start_item_count;
    LevelDef levels[WORLD_MAX_LEVELS + 1]; /* index = level 1..99 */
} ClassDef;

typedef struct {
    int used;
    char name[64];
    int klass;        /* arg2 class code */
    int image, image_ext;
    int gp;
    int level;
    int element, defense, attack, hp, mp;
} ItemDef;

typedef struct {
    int used;
    char name[64];
    int pp_cost, element, damage;
} SpellDef;

/* One quest.txt scene: script lines [first_line, end_line) in World.lines. */
typedef struct {
    int used;
    int first_line;   /* the SCENE line itself */
    int end_line;
} SceneDef;

/* .obl link record, decoded from the 800-byte on-disk layout (docs/re/maps.md section 4). */
typedef struct {
    int used;
    int object_id;    /* index into objects.obr / objects.bmp */
    int x, y;         /* map units (low-res jpg pixels) */
    int kind;         /* 1/4 scene, 2 map link, 3 back (FUN_00463853) */
    int target;       /* +0x64 scene number (kind 1/4) or link number on dest map (kind 2) */
    int difficulty;   /* +0x68 signed monster group level */
    int theme;        /* +0x6c */
    int dest_map;     /* +0x194 */
    int required_item;/* +0x1a4 */
    char name[64];
} Link;

typedef struct {
    int monster_id;
    int x, y, radius; /* map units */
} MonPlace;

typedef struct {
    int used;
    char name[24];
    int l, t, r, b;   /* source rect in objects.bmp */
} ObjRect;

typedef struct {
    int id;
    const MapDef *def;
    Image image;      /* low-res map jpg (1 px = 1 map unit) */
    Image image_x4;   /* hi-res 4x jpg; if absent, world.c synthesizes it by 4x upscale of image */
    Image terrain;    /* 8-bit .ter; terrain.indices[(y/4)*terrain.w + x/4] = terrain id */
    Image objects;    /* objects.bmp/jpg link sprite sheet */
    Link links[OBL_RECORDS];
    MonPlace mons[MON_RECORDS];
    int mon_count;
    ObjRect objrects[OBR_RECORDS];
} Map;

/* A filmstrip sprite sheet (skin, monster/villager sheet, item icons): cells are h x h squares. */
typedef struct {
    Image image;
    int cell;         /* cell side in pixels (= image height, or forced 48) */
    int count;        /* width / cell */
    int64_t key;      /* transparent color (0x00RRGGBB) from pixel (0, h-1), FUN_0048dad9 */
} Sheet;

typedef struct {
    char name[64];            /* world folder name, e.g. "Evergreen" */
    char dir[512];            /* "<data>/worlds/<name>" */
    char *text;               /* concatenated quest.txt with #includes expanded */
    char **lines;             /* line pointers into text (comments NOT stripped) */
    int line_count;
    MapDef maps[WORLD_MAX_MAPS];
    TerrainDef terrains[WORLD_MAX_TERRAINS];
    MonsterDef monsters[WORLD_MAX_MONSTERS];
    GroupDef groups[WORLD_MAX_GROUPS];
    ClassDef classes[WORLD_MAX_CLASSES];
    ItemDef items[WORLD_MAX_ITEMS];
    SpellDef spells[WORLD_MAX_SPELLS];
    SceneDef scenes[WORLD_MAX_SCENES];
    int starting_gp;          /* config.ini startingGP, default 500 */
} World;

extern World g_world;

/* Load <data>/worlds/<name>: quest.txt (+#include), all tables, config.ini. 0 on success. */
int world_load(const char *data_dir, const char *name);
void world_free(void);

/* Load map id (jpg, X4 jpg, .ter, .obl, .mon, objects). 0 on success. Caller owns Map. */
int map_load(Map *map, int id);
void map_free(Map *map);
/* Terrain id at map-unit coordinate, 9 (impassable) outside the map. */
int map_terrain_at(const Map *map, int x, int y);
/* 1 if a hero may stand at map-unit (x, y) (FUN_004631c6 rules: terrain 0 always OK, 9 never,
 * others need their token; tokens may be NULL meaning "none held"). */
int map_walkable(const Map *map, int x, int y, const unsigned char *tokens);

/* Sprite sheets. Skins come from <data>/skins; monsters from world monsters/ then <data>/monsters.
 * Falls back to "josh1" (monsters) like FUN_0048df3c. 0 on success. */
int sheet_load_skin(Sheet *sheet, const char *name);
int sheet_load_monster(Sheet *sheet, const char *name);
void sheet_free(Sheet *sheet);
/* Draw cell `index` (or a sub-rect of it) with transparency. */
struct Framebuffer;
void sheet_draw(struct Framebuffer *fb, const Sheet *sheet, int index, int x, int y, int flip);
/* Hero skin map sprite: the MAP cell is a 3x3 grid of (cell/3) squares; dir 0..8 row-major
 * (4 = centre/camp). */
void sheet_draw_map_dir(struct Framebuffer *fb, const Sheet *sheet, int dir, int x, int y);

/* Path helpers: "<data>/<rel>" and "<world dir>/<rel>" into buf. */
const char *world_data_path(char *buf, int size, const char *rel);
const char *world_path(char *buf, int size, const char *rel);

#endif
