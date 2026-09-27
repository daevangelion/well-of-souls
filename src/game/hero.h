/* The local (solo) hero: stats, position, inventory, tokens; creation and save/load.
 * Evidence: docs/re/boot_flow.md (new soul dialog), docs/re/script.md section 8.2 (hero creation from
 * levels.txt), docs/re/battle.md section 9 (XP/level). Owner: hero.c. Additive changes only. */
#ifndef WOS_HERO_H
#define WOS_HERO_H

#include <stdint.h>

#define HERO_NAME_MAX   32
#define HERO_TOKENS     1024   /* T0..T1023 quest tokens */
#define HERO_INVENTORY  64
#define HERO_ABILITIES  5      /* str, wis, sta, agi, dex (levels.txt START_ABILITY order) */

enum { ABIL_STR, ABIL_WIS, ABIL_STA, ABIL_AGI, ABIL_DEX };

typedef struct {
    int item_id;
    int count;
} HeroItem;

typedef struct {
    int valid;
    char name[HERO_NAME_MAX];
    char skin[64];
    int gender;                 /* 0..3 (gender.ini) */
    int klass;                  /* levels.txt class number (1..88) */
    int level;
    int64_t xp;
    int64_t gold;
    int hp, max_hp, mp, max_mp;
    int ability[HERO_ABILITIES];
    int right_hand;             /* equipped right-hand item id (0 = bare hands) */
    int equip[8];               /* helmet, armor, boots, shield, ring, amulet, ... (item ids) */
    HeroItem inventory[HERO_INVENTORY];
    unsigned char tokens[HERO_TOKENS];
    /* map position */
    int map;                    /* current map id */
    int link;                   /* last link visited (resurrection/incarnate point) */
    int x, y;                   /* map units */
} Hero;

extern Hero g_hero;

/* Initialise a new level-1 hero of class `klass` from g_world.classes (START_ABILITY, AUTO_MAX,
 * DEFAULT_SKIN, START_ITEMS, START_LOCATION, config.ini startingGP). skin NULL = class default. */
void hero_create(Hero *hero, const char *name, int klass, int gender, const char *skin);

/* Save file: game_save_path()/<world>/savedHeroes/<name>.wsh.
 * WSH1: magic, fixed NUL-terminated name/skin arrays, little-endian stats,
 * inventory, token bytes and position; not the retail .her record.
 * Save/load return 0 on success, -1 on I/O or invalid data; load is transactional. */
int hero_save(const Hero *hero);
int hero_load(Hero *hero, const char *name);
/* List saved hero names for the current world; returns count (<= max). */
int hero_list_saves(char names[][HERO_NAME_MAX], int max);

/* XP needed to reach `level` for this hero's class (levels.txt maths). */
int64_t hero_xp_for_level(const Hero *hero, int level);
/* Add XP/gold; performs level-ups (max HP/MP growth). Returns number of levels gained. */
int hero_award(Hero *hero, int64_t xp, int64_t gold);

#endif
