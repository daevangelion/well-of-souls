/* The local (solo) hero: stats, position, inventory, tokens; creation and save/load.
 * Evidence: docs/re/boot_flow.md (new soul dialog), docs/re/script.md section 8.2 (hero creation from
 * levels.txt), docs/re/battle.md section 9 (XP/level). Owner: hero.c. Additive changes only. */
#ifndef WOS_HERO_H
#define WOS_HERO_H

#include <stdint.h>

#define HERO_NAME_MAX   32
#define HERO_TOKENS     1024   /* T0..T1023 quest tokens */
#define HERO_INVENTORY  5119  /* one entry for every valid retail item id */
#define HERO_ABILITIES  5      /* str, wis, sta, agi, dex (levels.txt START_ABILITY order) */

enum { ABIL_STR, ABIL_WIS, ABIL_STA, ABIL_AGI, ABIL_DEX };
enum {
    HERO_SLOT_HELMET, HERO_SLOT_ARMOR, HERO_SLOT_BOOTS, HERO_SLOT_SHIELD,
    HERO_SLOT_RING, HERO_SLOT_AMULET,
    HERO_SLOT_RIGHT_HAND = 8 /* stored in right_hand, not equip[] */
};
enum { HERO_TRAIN_HAND, HERO_TRAIN_ELEMENT };

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
    int hand_pp[8];             /* hands 1..8; preferred hand begins at 5000 PP */
    int element_pp[8];          /* element indices 0..7 */
    int64_t pp;                 /* unspent proficiency wallet */
    uint32_t ailments;          /* bit n = spell disease -n, n=2..24 */
    HeroItem inventory[HERO_INVENTORY]; /* item id n owns slot n-1; empty = {0,0} */
    unsigned char tokens[HERO_TOKENS];
    unsigned char learned_spells[768]; /* explicit GIVE S / START_SPELLS grants */
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
 * WSH3: magic, fixed NUL-terminated name/skin arrays, little-endian stats,
 * hand/element PP, PP wallet, ailments, inventory, tokens, spells and position.
 * Save/load return 0 on success, -1 on I/O or invalid data; load is transactional. */
int hero_save(const Hero *hero);
int hero_load(Hero *hero, const char *name);
/* List saved hero names for the current world; returns count (<= max). */
int hero_list_saves(char names[][HERO_NAME_MAX], int max);

/* XP needed to reach `level` for this hero's class (levels.txt maths). */
int64_t hero_xp_for_level(const Hero *hero, int level);
/* Add XP/gold; performs level-ups (max HP/MP growth). Returns number of levels gained. */
int hero_award(Hero *hero, int64_t xp, int64_t gold);

/* Inventory operations are all-or-nothing: 1 success, 0 rejected. Equipped items
 * remain in inventory; taking the final copy automatically unequips it. */
int hero_item_count(const Hero *hero, int item_id);
int hero_item_limit(const Hero *hero, int item_id);
int hero_give_item(Hero *hero, int item_id, int count);
int hero_take_item(Hero *hero, int item_id, int count);
int64_t hero_wallet_limit(const Hero *hero);
int64_t hero_add_gold(Hero *hero, int64_t amount); /* actual signed change */
int hero_item_slot(int item_id); /* HERO_SLOT_* or -1 for non-equipment */
int hero_equipped(const Hero *hero, int slot);
int hero_can_equip(const Hero *hero, int item_id);
int hero_equip(Hero *hero, int item_id);
int hero_unequip(Hero *hero, int slot);
/* ability is ABIL_* for class-105 free-choice seeds; otherwise ignored.
 * Rejected uses do not consume the item. Success emits item_use. */
int hero_use_item(Hero *hero, int item_id, int ability);
int hero_offense(const Hero *hero);
int hero_defense(const Hero *hero);
int hero_ability(const Hero *hero, int ability);
int hero_pp_level(int pp); /* documented thresholds, integer level 0..9 */
int hero_train_limit(const Hero *hero, int kind, int index);
/* FUN_00425854 wallet clamp; Battle awards PP on qualifying attacks, not XP payout. */
int64_t hero_add_pp(Hero *hero, int64_t amount); /* actual signed change */
/* Spend exactly 1..2000 wallet PP, or reject without mutation; indices 0..7. */
int hero_train(Hero *hero, int kind, int index, int amount);
/* Combat proficiency gain (1..2000), clamped to class cap; no wallet debit. */
void hero_gain_training(Hero *hero, int kind, int index, int amount);
int hero_spell_known(const Hero *hero, int spell_id);
/* Explicit grant/removal, not a PP purchase; used by quest GIVE/TAKE S. */
int hero_set_spell(Hero *hero, int spell_id, int known);
int hero_can_learn_spell(const Hero *hero, int spell_id);
int hero_learn_spell(Hero *hero, int spell_id); /* debit PP and emit spell_learn */

#endif
