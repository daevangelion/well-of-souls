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
#define WORLD_MAX_ELEMENTS  256
#define WORLD_MAX_HANDS       8
#define WORLD_MAX_TROPHIES 4096
#define TROPHY_MAX_RANGES    10
#define GROUP_MAX_MEMBERS   9
#define OBL_RECORDS         256   /* .obl = 256 x 800 bytes */
#define OBL_RECORD_SIZE     800
#define MON_RECORDS         1000  /* .mon = 1000 x 276 bytes */
#define MON_RECORD_SIZE     276
#define OBR_RECORDS         1024 /* FUN_00463630 is called with 0x400 at 0x41F119; Evergreen ships 256 */
#define OBR_RECORD_SIZE     48
#define WORLD_MAX_TOKENS    4096  /* +TOKENS id 0..4095, FUN_004814d2 */
#define WORLD_MAX_CHAPTERS  100   /* FUN_004817de rejects chapter 100 */
#define WORLD_MAX_EQUIP     14    /* +EQUIP ids 0..13, FUN_004824b4 */
#define WORLD_CREDITS_MAX   9999  /* FUN_004861c8 buffer, called with 0x270F */
/* +CREDITS is concatenated with "\r\n" after every line (FUN_004861c8, called with
 * WORLD_CREDITS_MAX by the About-this-world dialog at 0x49751B). +STORY keeps its lines
 * in file order; both are owned by World.text. */
typedef struct {
    const char **lines;
    int count;
} WorldText;



typedef struct {
    int used;
    char image[64];   /* e.g. "evergreen.jpg" */
    char root[64];    /* base name of .obl/.ter/.mon and music.ini section */
    char name[64];    /* display name */
    uint32_t flags;   /* MAP_FLAG_* bitmask, quest.txt +MAPS arg4; FUN_00482BB8 tests it */
    int theme;        /* arg5, optional */
} MapDef;

/* +TOKENS description / diary text: id 0..4095, text truncated to 128 chars
 * (FUN_004814d2: strncpy(dst, src, 0x80); dst[0x80] = 0). FUN_004814a7 returns "" for an
 * unused or out-of-range id, so world_token_text() never returns NULL. */
typedef struct {
    int used;
    char text[129];
} TokenDef;

/* +TOKENS "chapter <a> <b> <c> <title> <link> <text>" rows (FUN_004817de), max 100.
 * A chapter is the quest-diary page; FUN_00481aca lists the tokens in [low, high]. */
typedef struct {
    int used;
    int a, b, c;
    char title[261], link[261], text[261];
} ChapterDef;


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
    /* The four fields below are PARSED FROM REAL monsters.txt COLUMNS AND READ BY NO PORT CODE
     * YET. That is not the same as a phantom field, and it is why they are kept:
     *   a phantom field - no writer and no reader - is a trap and should be deleted;
     *   a parsed-but-unread column is working parsing that the next person to implement monster
     *   AI, sounds or attack paths needs, and deleting it costs them a re-derivation.
     * Do not sweep these on a "no readers" grep without checking for a writer first. */
    int scale, flags; /* arg3 scaleFactor[.monsterFlags]; flags IS read (battle.c) */
    int element;
    int hp, mp, defense, offense, exp, gold, level;
    int strength, stamina, agility, dexterity, wisdom;
    char growl_wav[64], pain_wav[64]; /* arg17, arg18; the combatant record keeps runtime
                                       * copies at +0x046 and +0x079 (FUN_004809a3) */
    int attack_path;                   /* arg19; not to be confused with ItemDef.attack_path,
                                         * which IS live (written world.c:445, read items.c) */
    char ai[256];    /* arg20 optional AI command, FUN_004809a3 */
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
    int hidden;
    int max_wallet;             /* -1 = no class-specific gold cap */
    int hand_ratio;             /* 0 means 100 - magic_ratio */
    int max_ability[5], start_ability_set;
    int start_element_pp[8], max_element_pp[8];
    int start_hand_pp[8], max_hand_pp[8];
    int no_gifts, hidden_start_level;
    int start_spells[8], start_spell_count;
    int start_tokens[8], start_token_count;
    LevelDef levels[WORLD_MAX_LEVELS + 1]; /* sparse 1..99; [0] holds class starting HP/MP */
} ClassDef;

typedef struct {
    int used;
    char name[64];
    int klass;        /* arg2 class code */
    int image, image_ext;
    int gp;
    int level;
    int element, defense, attack, hp, mp;
    int movement;    /* arg6 dotted bootEffect; maps.md section 3 */
    int equip_token, flags, max_count;
    int trophy_needed, trophy_made, trophy_count_needed, trophy_count_made;
    int spell_binding, ability_points, find_probability, find_monster;
    int travel_mode, travel_map_scene, travel_link, travel_drop_in, travel_scene;
    int attack_path, attack_image, attack_flags, attack_weather, attack_effect;
    char description[256], sound[256];
} ItemDef;

typedef struct {
    int used;
    char name[64];
    int pp_cost, element, damage;
    int mp_cost, summon_id;
    int req_affinity, all_targets, flags, min_level, token;
    int trophy_needed, trophy_made, trophy_count_needed, trophy_count_made;
    int path, effects_row, max_cols, max_fx, ms_per_col;
    int gravity, effects, weather, loop;
    int extra[2];     /* optional arg17 dotted pair, FUN_0047fced +0x180/+0x184 */
    /* FUN_0047fced 0x4803E9 draws one rand() per used spell into record +0x154 while it is
     * still 0; FUN_0048????? reads it back as the effect-animation seed. Keep the draw even
     * if the value is never displayed: it shifts every later crt_rand(). */
    int effect_seed;
    char sfx_summon[80], sfx_travel[80], sfx_strike[80];
} SpellDef;

typedef struct { int used; char name[32]; } ElementDef;
typedef struct {
    int used;
    /* 8 entries, 79-char names; FUN_004825df installs Sword/Staff/Bow/Music/Fist/Dart/Book/
     * RH5/RH6/RH7/RH8 when the +HANDS section defines no rows at all. */
    char name[80], sound[80];
    int strength_percent; /* arg2, clamped 0..100 (>99 becomes 100, <1 becomes 0) */
    int damage_weight;    /* strength_percent * 40 / 100 + 10, FUN_004825df */
} HandDef;
typedef struct {
    int used;
    char name[33], image[33];
    int image_index, stack_size, gp, probability, token, flags;
    int monster_first[TROPHY_MAX_RANGES], monster_last[TROPHY_MAX_RANGES];
    int monster_range_count;
} TrophyDef;

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
    int has_been_used; /* +0x1A0; FUN_00462958 sets it when the link fires and FUN_004639EB
                        * draws the +0xC0 name only when it is set (or map flag 32) */
    char name[64];
    char background[80]; /* +0x70, inherited by SCENE (FUN_0047a1bc) */
    int fx, weather;     /* +0x19c, +0x198 */
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

/* Row-major atlas; character filmstrips have one row of height-sized square cells. */
typedef struct {
    Image image;
    int cell;         /* cell width in pixels; character sheets use image height */
    int count;        /* (image.w / cell) * (image.h / cell_h) */
    int64_t key;      /* RGB key: original bottom-up DIB (0,h-1), normalized Image (0,0) */
    int cell_h;       /* rectangular art cells; skins/monsters use cell_h == cell */
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
    ElementDef elements[WORLD_MAX_ELEMENTS];
    TrophyDef trophies[WORLD_MAX_TROPHIES];
    HandDef hands[WORLD_MAX_HANDS];
    TokenDef tokens[WORLD_MAX_TOKENS];
    ChapterDef chapters[WORLD_MAX_CHAPTERS];
    int chapter_count;
    char equip_names[WORLD_MAX_EQUIP][40]; /* +EQUIP, FUN_004823c4 built-in names first */
    WorldText story;                       /* +STORY body lines, file order */
    WorldText credits;                     /* +CREDITS body lines, file order */
    char credits_text[WORLD_CREDITS_MAX+1]; /* lines joined with "\r\n", FUN_004861c8 */
    /* config.ini [General], read by FUN_0047c5c5 into a 100-byte buffer. The value in
     * brackets is the original's DEFAULT STRING argument, not the InitInstance pre-init:
     * startingGP is 1000 (0x4e2364) and spellSuccessPercent is "0" (0x4dcaf4). */
    char gold_name[100];                   /* [GP] */
    int pk_hand_percent;                   /* [100] */
    int pk_magic_percent;                  /* [100] */
    int spell_success_percent;             /* [100] FUN_004A7456 0x4A750C: != 100 scales */
    int karma_points_are_also_war_points;  /* [0] */
    int pk_trophy;                         /* [0] */
    int monster_xp_are_also_war_points;    /* [0] */
    int tactics_win_gives_war_points;      /* [0] */
    int no_giving_gp;                      /* [0] */
    int cookie_protection;                 /* [0] */
    int pets_can_bite_people;              /* [1] */
    int starting_gp;                       /* [1000] */
    int max_unspent_pp;                    /* [1000000] */
    int max_pk_attack_advantage;           /* [80] */
    char world_home_url[200];              /* [] */
    char tactics_source_url[200];          /* [] */
    /* DAT_004fa95c: rotate-left-1 + XOR over the whole #include-expanded quest.txt
     * (FUN_0047977a), then XOR the summed per-file byte counts (FUN_00479a02). Stored in
     * the hero record at +0x6F0/+0x6C4/+0x1B1; a mismatch blocks soul switching. */
    uint32_t crc1;
    /* DAT_004fa960: seed 0x075BCD15 XOR the quest.txt buffer as dwords (FUN_0047983e). */
    uint32_t crc2;
} World;

extern World g_world;

/* Load <data>/worlds/<name>: quest.txt (+#include), all tables, config.ini. 0 on success. */
int world_load(const char *data_dir, const char *name);
void world_free(void);
/* Comma/whitespace lexer with quotes and comments; -1 on token/count overflow. */
int world_tokenize(const char *line, char tokens[][256], int max);
/* Borrowed music.ini values, map section then [common]; absent value is "". */
const char *world_music(const char *root, const char *key);
int world_music_count(const char *root);

/* Token description text (+TOKENS rows and `TOKEN n,"text"` lines inside +SCENES).
 * FUN_004814a7 answers "" for an unused or out-of-range id; never NULL. */
const char *world_token_text(int id);
/* Diary chapter i (0..chapter_count-1) or NULL. FUN_004817de order: a, b, c, title, link, text. */
const ChapterDef *world_chapter(int index);
/* +EQUIP display name for the ORIGINAL slot id 0..13 (FUN_00482431):
 * 0 helmet, 1 armor, 2..9 the eight hand classes, 10 boots, 11 shield, 12 ring, 13 amulet.
 * Out of range answers "Right-Hand" exactly like FUN_004825bf does for a bad hand id. */
const char *world_equip_name(int slot);
/* +HANDS name for hand 0..7 (FUN_004825bf); "Right-Hand" out of range. */
const char *world_hand_name(int hand);
/* Name for the port's hero.h HERO_SLOT_* value: 0..5 map to the original slots
 * 0,1,10,11,12,13 and HERO_SLOT_RIGHT_HAND (8) answers the hero's hand class name. */
const char *world_hero_slot_name(int hero_slot, int hand);
/* Inverse of the +EQUIP table: name (case-insensitive) -> original slot id, -1 if unknown. */
int world_equip_slot_by_name(const char *name);
/* +STORY back-story lines in file order; *lines is NULL when the section is absent. */
const char *world_story_line(int index);
int world_story_count(void);
/* +CREDITS joined with "\r\n" (FUN_004861c8); "" when the section is absent. */
const char *world_credits_text(void);
/* config.ini [General] goldName, the string the original prints in place of "GP". */
const char *world_gold_name(void);

/* State dump: world.crc1, world.crc2, world.cfg.*, world.tokens, world.chapters,
 * world.equip.<slot>, world.music. Signature from engine/dump.h. */
void world_dump(void (*emit)(const char *key, const char *value, void *user), void *user);

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
/* World art/ overrides root art/. Name may include .bmp; cells tile row-major. */
int sheet_load_art(Sheet *sheet, const char *name, int cell_w, int cell_h);
void sheet_free(Sheet *sheet);
/* Draw cell `index` (or a sub-rect of it) with transparency. */
struct Framebuffer;
void sheet_draw(struct Framebuffer *fb, const Sheet *sheet, int index, int x, int y, int flip);
/* Hero skin map sprite: the MAP cell is a 3x3 grid of (cell/3) squares; dir 0..8 row-major
 * (4 = centre/camp). One-pixel guides on all four subcell edges are excluded (0x4165ba). */
void sheet_draw_map_dir(struct Framebuffer *fb, const Sheet *sheet, int dir, int x, int y);

/* Path helpers: "<data>/<rel>" and "<world dir>/<rel>" into buf. */
const char *world_data_path(char *buf, int size, const char *rel);
const char *world_path(char *buf, int size, const char *rel);

#endif
