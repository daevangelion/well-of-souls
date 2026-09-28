/* Solo hero model. Creation: boot_flow.md 3c / script.md 8.2.
 * Inventory/equipment: 0x403349, 0x40C694, 0x40D6B4. Consumables live in items.c.
 * Training: hand/element click handlers 0x419107/0x425C30, proficiency
 * consumers 0x419306/0x4258B8; learning spells: 0x44E3BD.
 *
 * The save file is the original's, unchanged: a fixed 0x16CC-byte record
 * written by FUN_00417F1B (all.c:17099) and validated by FUN_004181A2
 * (all.c:17202), with FUN_00416ABB's checksum in the last four bytes. The old
 * portable WSH3 format is gone with no shim; every field of the record is
 * written by hero_record_encode and read by hero_record_decode. */
#include "hero.h"
#include "world.h"
#include "scene.h"
#include "../game_main.h"
#include "../engine/ini.h"
#include "../engine/text.h"
#include "../engine/log.h"
#include "../engine/dump.h"
#include "../platform/platform.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <limits.h>
#include <math.h>

Hero g_hero;

static const ClassDef *hero_class(const Hero *h)
{
    if (!h || h->klass < 1 || h->klass >= WORLD_MAX_CLASSES) return NULL;
    return &g_world.classes[h->klass];
}

static int bounded_stat(int64_t n) { return n < 0 ? 0 : n > INT_MAX ? INT_MAX : (int)n; }

static void maxima(const ClassDef *c, int level, int *hp, int *mp);

/* hero_maxima is the public form of the same computation; see hero.h. */
void hero_maxima(const Hero *h, int *hp, int *mp)
{
    const ClassDef *c = hero_class(h);
    int level;
    if (!h) { if (hp) *hp = 1; if (mp) *mp = 0; return; }
    level = h->level;
    if (!c || !c->used) { if (hp) *hp = 1; if (mp) *mp = 0; return; }
    maxima(c,level,hp,mp);
}

static void maxima(const ClassDef *c, int level, int *hp, int *mp)
{
    int n, dh = 0, dm = 0;
    int64_t h = c->levels[0].d_hp, m = c->levels[0].d_mp;
    /* FUN_00483984: sparse positive increments carry forward. AUTO_MAX
     * replaces them with the capped linear-target curve, not a straight lerp. */
    if (c->auto_max_set) {
        int start = c->auto_max[4];
        int64_t hrange = (int64_t)c->auto_max[1] - c->auto_max[0];
        int64_t mrange = (int64_t)c->auto_max[3] - c->auto_max[2];
        h = c->auto_max[0]; m = 0;
        for (n = 0; n <= level; ++n) {
            if (n >= 2) {
                int64_t inc = (n - 1) * hrange / 99 - h + c->auto_max[0];
                int64_t cap = (hrange / 5000 + 1) * n;
                if (inc > cap) inc = cap;
                if (inc > 0) h += inc;
            }
            if (n == start) m = c->auto_max[2];
            else if (n > start && start < 100) {
                int64_t inc = (n - start) * mrange / (100 - start) - m + c->auto_max[2];
                int64_t cap = (mrange / 5000 + 1) * (n - start + 1);
                if (inc > cap) inc = cap;
                if (inc > 0) m += inc;
            }
        }
    } else {
        for (n = 1; n <= level; ++n) {
            int row = n == 100 ? 99 : n;
            if (c->levels[row].d_hp > 0) dh = c->levels[row].d_hp;
            if (c->levels[row].d_mp > 0) dm = c->levels[row].d_mp;
            h += dh; m += dm;
        }
    }
    *hp = bounded_stat(h); if (*hp < 1) *hp = 1;
    *mp = bounded_stat(m);
}

void hero_create(Hero *h, const char *name, int klass, int gender, const char *skin)
{
    const ClassDef *c;
    int i;
    memset(h, 0, sizeof(*h));
    h->klass = klass; c = hero_class(h);
    if (!c || !c->used) return;
    h->valid = 1; h->level = 1;
    h->gender = gender >= 0 && gender < 4 ? gender : 0;
    snprintf(h->name, sizeof(h->name), "%s", name ? name : "");
    memcpy(h->ability, c->start_ability, sizeof(h->ability));
    memcpy(h->hand_pp, c->start_hand_pp, sizeof(h->hand_pp));
    memcpy(h->element_pp, c->start_element_pp, sizeof(h->element_pp));
    /* levels.txt header / FUN_004207BD: preferred hand starts trained to five. */
    if (c->right_hand >= 1 && c->right_hand <= 8 && h->hand_pp[c->right_hand - 1] < 5000)
        h->hand_pp[c->right_hand - 1] = 5000;
    /* New Soul reads the class's level-zero starting values (0x460A7E),
     * not the first cumulative level increment. */
    h->max_hp = c->auto_max_set ? c->auto_max[0] : c->levels[0].d_hp;
    h->max_mp = c->auto_max_set ? (c->auto_max[4] == 0 ? c->auto_max[2] : 0) : c->levels[0].d_mp;
    if (h->max_hp < 1) h->max_hp = 1;
    if (h->max_mp < 0) h->max_mp = 0;
    h->hp = h->max_hp; h->mp = h->max_mp;
    hero_add_gold(h, g_world.starting_gp);
    h->map = c->start_location_set ? c->start_map : 0;
    h->link = c->start_location_set ? c->start_link : 0;
    if (!skin || !*skin) skin = c->default_skin[h->gender];
    if (skin && *skin && strcmp(skin, ".")) snprintf(h->skin, sizeof(h->skin), "%s", skin);
    else {
        char path[768], section[8];
        char *text;
        Ini ini;
        world_path(path, sizeof(path), "gender.ini");
        text = text_read_file(path, NULL);
        snprintf(section, sizeof(section), "%d", h->gender);
        if (text && ini_parse(&ini, text) == 0)
            snprintf(h->skin, sizeof(h->skin), "%s", ini_get(&ini, section, "adventurer", "adventurer"));
        else snprintf(h->skin, sizeof(h->skin), "adventurer");
        free(text);
    }
    for (i = 0; i < c->start_token_count && i < 8; ++i) {
        int token = c->start_tokens[i];
        if (token >= 0 && token < HERO_TOKENS) h->tokens[token] = 1;
    }
    for (i = 0; i < c->start_spell_count && i < 8; ++i)
        hero_set_spell(h, c->start_spells[i], 1);
    for (i = 0; i < c->start_item_count && i < 8; ++i) {
        int id = c->start_items[i];
        if (hero_give_item(h, id, 1) && !h->right_hand &&
            g_world.items[id].klass == c->right_hand + 11 && hero_can_equip(h, id))
            h->right_hand = id;
    }
    scene_reset_timers();
}

/* ---------------------------------------------------------------- record ---
 * Byte offsets in the 0x16CC record (base DAT_0067FBF8, stride 0x16CC, 100
 * entries). Every constant below carries the decomp site that fixes it. */
enum {
    R_INUSE      = 0x0000, /* 1 alive, 2 ghost, 4 loaded-but-unincarnated  0x42095E  */
    R_SERIAL     = 0x0004, /* DAT_004DD20C                               0x41F832  */
    R_NAME       = 0x0014, /* char[0x21], strncpy 0x20                   0x4209F7  */
    R_SKIN       = 0x0035, /* char[0x1F]                                 0x438C05  */
    R_CLASS      = 0x0060, /* levels.txt class index                     0x420A3F  */
    R_LEVEL      = 0x0064, /*                                             0x4181A2  */
    R_XP         = 0x0068, /* cumulative XP; -XP mirror at 0x6C8         0x4E3274  */
    R_GOLD       = 0x006C, /* -GOLD mirror at 0x974                      0x42BAF3  */
    R_HP         = 0x0070, /*                                             0x4A6355  */
    R_MAXHP      = 0x0074, /*                                             0x42B7C7  */
    R_MP         = 0x0078, /*                                             0x4A6355  */
    R_MAXMP      = 0x007C, /*                                             0x42B7F8  */
    R_ATTACK     = 0x0084, /* sum of worn +0xa0, mirror -0xA60            0x40C694  */
    R_DEFENCE    = 0x0080, /* sum of worn +0xa4, mirror -0xA64            0x40C694  */
    R_FLAGS      = 0x0A58, /* the hero flag word the script tests as IF Mn:
                             * FUN_004851F1 case 0x4D reads (hero[0xA58] & n) != 0
                             * for n > 0. Bit 2 is the "Modified Quest File
                             * Detected" flag set on the world-CRC mismatch path
                             * (FUN_0044B196); bit 4 is set by FUN_004978D5 when
                             * avoidModifiedQuestFiles is OFF. Mirrored negated
                             * at +0xA5C, and the mirror is checked.            */
    R_FLAGS_MIR  = 0x0A5C, /* -R_FLAGS, a tamper pair like the XP and PP ones  */
    R_FACING     = 0x0088, /* fy*4 + fx with a 5 -> 9 remap, 0x46230E       */
    R_FIXED_X    = 0x0094, /* LIVE position x, 24.8 fixed; all.c:70730 does
                             * FUN_004631C6(<<value>> >> 8, ...) on it, so the
                             * shift is 8, not 16                                 */
    R_FIXED_Y    = 0x0098, /* LIVE position y, 24.8 fixed, ditto              */
    R_TARGET_X   = 0x009C, /* the walk destination x, 24.8 fixed, all.c:70613  */
    R_TARGET_Y   = 0x00A0, /* the walk destination y, 24.8 fixed, all.c:70614  */
    R_SPEED      = 0x00AC, /* walk speed                                      */
    R_WALK_DUR   = 0x00B4, /* walk duration                                   */
    R_MAP        = 0x0090, /* sent in msg 0x46A as the map number        0x420240  */
    R_EQUIP      = 0x01D8, /* 14 ints, slot 8 is the right hand           0x40C694  */
    R_HANDITEM   = 0x01F8, /* equip[8], the right-hand item id           0x490712  */
    R_SPELLS     = 0x0210, /* 0x60 bytes, 768 BITS (spells 0..767)       0x4A4E79  */
    R_ITEMCNT    = 0x0272, /* 0x400 bytes, one count per id 0..0x3FF     0x45FC2B  */
    R_ITEMBIT    = 0x0AC8, /* 0x200 bytes, presence bit per id 0x400+    0x45FC2B  */
    R_LINK       = 0x067C, /* sent in msg 0x46A as the link index        0x420240  */
    R_MAGICRATIO = 0x0694,
    R_PK         = 0x0698,
    R_PP         = 0x069C, /* unspent wallet; -PP mirror at 0x760        0x425854  */
    R_HAND       = 0x06A0, /* preferred hand 0..7                       0x490712  */
    R_HANDITEMID = 0x06A4,
    R_HP_MIRROR  = 0x06A8,
    R_XP_MIRROR  = 0x06C8, /* -XP                                        0x4181A2  */
    R_ELEM_PP    = 0x06CC, /* 8 ints, cap class+0x1AA64                  0x4258B8  */
    R_HAND_PP    = 0x06FC, /* 8 ints, cap class+0x1AAA4                  0x419306  */
    R_ABILITIES  = 0x0680, /* str,wis,sta,agi,dex, each clamped 0..255   0x4207BD  */
    R_DEATHS     = 0x0724, /* ++ on death, FUN_00494FCD              all.c:109382 */
    R_KILLS      = 0x0728, /* += the fight block's tally, all.c:32670           */
    R_INCARN     = 0x0730, /* ++ by FUN_00420240 on every incarnate      0x420240  */
    R_SECONDS    = 0x0734, /* seconds played                             0x417F1B  */
    R_SELECTED   = 0x0738, /* 33 ints, selected element per hand, mirrored
                              to/from DAT_00D831F8                     0x417F1B  */
    R_PP_MIRROR  = 0x0760, /* -PP                                        0x4181A2  */
    R_CKSUM_MODE = 0x0768, /* 0 legacy, 1 current                       0x416ABB  */
    R_TOKENS     = 0x076C, /* 0x200 bytes, 4096 BITS (T0..T4095)        0x44DDEA  */
    R_HAND_RATIO = 0x09E0, /* 100 - magic ratio when 0 on load          0x4181A2  */
    R_MAXHP_RAW  = 0x09E8, /* 0 -> derive from the level                0x4181A2  */
    R_HUNTING    = 0x0A04, /* raw signed hunt field, /10000 threshold   0x46260E  */
    R_HALO       = 0x0A34,
    R_SAVES      = 0x0A4C, /* save counter, ++ per save                 0x417F1B  */
    R_MAXPP      = 0x0EF0, /* lifetime PP earned                        0x425854  */
    R_CAMPED_CRC = 0x06C4, /* the ONE world CRC field (int index 0x1B1). Written from
                              * the live world CRC-1 whenever the soul camps or
                              * incarnates (0x24018, 0x24392, 0x24470, 0x24523, 0x87880)
                              * and compared against it by FUN_0044B196 at 0x53935. */
    R_ENERGY     = 0x0EFC, /* FUN_004142D8(seconds)                     0x417F00  */
    R_GENDER     = 0x0AA0, /* 0..3                                        0x38130  */
    R_TROPHY     = 0x0CE0, /* 128 raw words (FUN_0046F726)               */
    R_TROPHY_GEO = 0x0EE0,
    R_PET0       = 0x0EE8,
    R_PET1       = 0x0EEC, /* NOT a second pet id: a GetTickCount stamp,
                             * written at all.c:42151. There is one pet.  */
    R_CKSUM      = 0x16C8  /* the last four bytes of the file            */
};
/* Fields FUN_004181A2 clears after fread because they only ever hold a live
 * pointer (all.c:17236-17241). 0x5C and 0x58 are the two halves of the pet
 * CWnd* pair. */
static const int ram_only[] = { 0x000C, 0x0058, 0x005C, 0x00CC, 0x06C0, 0x0978 };

static int safe_name(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    if (!p || !*p || strlen(s) >= HERO_NAME_MAX || !strcmp(s,".") || !strcmp(s,"..")) return 0;
    for (; *p; ++p) if (*p < 32 || strchr("/\\:", *p)) return 0;
    return 1;
}

/* FUN_00460962 (all.c:69364): the ONLY hero-path builder. "<save>/<world>" and
 * "<save>/<world>/savedHeroes" are created on demand, then, when `name` is
 * non-empty, "\\" + name + "." + ext are appended. The original passes
 * "her" (DAT_004DF644) for the record and the empty string at DAT_004DCBAC
 * for the per-hero INI, so the INI is "<...>/savedHeroes/<Name>" with no
 * extension. */
static int hero_path(char *path, size_t size, const char *name, const char *ext, int create)
{
    int n;
    if (name && !safe_name(name)) return -1;
    if (!*g_world.name || strchr(g_world.name,'/') || strchr(g_world.name,'\\')) return -1;
    n = snprintf(path,size,"%s",game_save_path());
    if (n < 0 || (size_t)n >= size || (create && plat_mkdir(path))) return -1;
    n = snprintf(path,size,"%s/%s",game_save_path(),g_world.name);
    if (n < 0 || (size_t)n >= size || (create && plat_mkdir(path))) return -1;
    n = snprintf(path,size,"%s/%s/savedHeroes",game_save_path(),g_world.name);
    if (n < 0 || (size_t)n >= size || (create && plat_mkdir(path))) return -1;
    if (name && *name) {
        /* The original always appends "\", name, "." and the extension, but the
         * per-hero INI's extension is the empty string, so its name on disk ends
         * in a dot -- which Windows silently drops. The port has to drop it too
         * or the file would not be the one the original and its own reader see. */
        if (ext && *ext)
            n = snprintf(path,size,"%s/%s/savedHeroes/%s.%s",game_save_path(),g_world.name,name,ext);
        else
            n = snprintf(path,size,"%s/%s/savedHeroes/%s",game_save_path(),g_world.name,name);
        if (n < 0 || (size_t)n >= size) return -1;
    }
    return 0;
}

/* FUN_00484CBC (all.c:97246) sets or clears one bit of a bitmap; FUN_00484D3B
 * (all.c:97277) reads it. The port spells the same little routines so the
 * bit order in tokens, spells and the high item ids is the original's. */
static int bit_get(const uint8_t *map, int index)
{ return (map[index >> 3] >> (index & 7)) & 1; }
static void bit_set(uint8_t *map, int index, int on)
{
    uint8_t m = (uint8_t)(1u << (index & 7));
    if (on) map[index >> 3] |= m; else map[index >> 3] = (uint8_t)(map[index >> 3] & ~m);
}

/* FUN_00416ABB, all.c:16108. `mode` is the record's +0x768 selector. Note the
 * original has no branch for a mode other than 0 or 1: it returns the bare
 * seed, and we reproduce that rather than inventing a third variant. */
uint32_t hero_record_checksum(const uint8_t in[HERO_RECORD_SIZE], int mode)
{
    uint32_t h = UINT32_C(0x379ADE);
    int i;
    if (mode == 0) {
        for (i = 0; i < 0x16C8; ++i) {
            h ^= (uint32_t)(int32_t)(int8_t)in[i];
            if (in[i] != 0) h <<= (uint32_t)(i & 1);
        }
    } else if (mode == 1) {
        for (i = 0; i < 0x16C8; ++i) h ^= (uint32_t)in[i] << (uint32_t)(i % 0x18);
    }
    return h;
}

/* Little-endian accessors. The original is x86, so the record is native
 * little-endian; these keep the port portable without changing the bytes. */
static void put32(uint8_t *p, uint32_t v)
{ p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24); }
static uint32_t get32(const uint8_t *p)
{ return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24; }
static int32_t get32s(const uint8_t *p) { return (int32_t)get32(p); }

void hero_record_encode(uint8_t out[HERO_RECORD_SIZE])
{
    const Hero *h = &g_hero;
    int i;
    memset(out,0,HERO_RECORD_SIZE);
    put32(out+R_INUSE,(uint32_t)(h->slot_in_use?1:0));
    put32(out+R_SERIAL,(uint32_t)h->serial);
    memcpy(out+R_NAME,h->name,HERO_NAME_MAX);
    out[R_NAME+HERO_NAME_MAX]=0;
    memcpy(out+R_SKIN,h->skin,sizeof(h->skin));
    out[R_SKIN+sizeof(h->skin)]=0;
    put32(out+R_CLASS,(uint32_t)h->klass);
    put32(out+R_LEVEL,(uint32_t)h->level);
    /* XP and gold are 32-bit in the record and each has a negated mirror; the
     * loader rejects the file unless the pairs sum to zero (0x4181A2). */
    put32(out+R_XP,(uint32_t)(int32_t)h->xp);
    put32(out+R_XP_MIRROR,(uint32_t)(0u-(uint32_t)(int32_t)h->xp));
    put32(out+R_GOLD,(uint32_t)(int32_t)h->gold);
    put32(out+0x0974,(uint32_t)(0u-(uint32_t)(int32_t)h->gold));
    put32(out+R_HP,(uint32_t)h->hp);
    put32(out+R_MAXHP,(uint32_t)h->max_hp);
    put32(out+R_MP,(uint32_t)h->mp);
    put32(out+R_MAXMP,(uint32_t)h->max_mp);
    put32(out+R_ATTACK,(uint32_t)hero_offense(h));
    put32(out+0x0A60,(uint32_t)(0u-(uint32_t)hero_offense(h)));
    put32(out+R_DEFENCE,(uint32_t)hero_defense(h));
    put32(out+0x0A64,(uint32_t)(0u-(uint32_t)hero_defense(h)));
    put32(out+R_MAP,(uint32_t)h->map);
    /* The hero's walk state IS in the record, which closes the gap I had
     * reported as "x and y are not in the hero record": Oracle4's map.* audit
     * found it in record 0 of the 44-entry actor array whose base pointer is
     * the global at 0x0067FBF8, the hero table. All six words are written raw,
     * in the original's own 24.8 fixed point, because a save made mid-walk
     * carries the fractional bits. */
    put32(out+R_FIXED_X,(uint32_t)h->x);
    put32(out+R_FIXED_Y,(uint32_t)h->y);
    put32(out+R_TARGET_X,(uint32_t)h->target_x);
    put32(out+R_TARGET_Y,(uint32_t)h->target_y);
    put32(out+R_SPEED,(uint32_t)h->walk_speed);
    put32(out+R_WALK_DUR,(uint32_t)h->walk_duration);
    put32(out+R_FACING,(uint32_t)h->facing);
    put32(out+R_FLAGS,(uint32_t)h->flags);
    put32(out+R_FLAGS_MIR,(uint32_t)(0u-(uint32_t)h->flags));
    for (i=0;i<8;++i) put32(out+R_EQUIP+4*i,(uint32_t)h->equip[i]);
    put32(out+R_HANDITEM,(uint32_t)h->right_hand);
    for (i=0;i<768;++i) bit_set(out+R_SPELLS,i,h->learned_spells[i]?1:0);
    for (i=0;i<HERO_INVENTORY;++i) {
        int id=i+1;
        if (h->inventory[i].item_id!=id || h->inventory[i].count<=0) continue;
        if (id<0x400) out[R_ITEMCNT+id]=(uint8_t)(h->inventory[i].count>100?100:h->inventory[i].count);
        else bit_set(out+R_ITEMBIT,id-0x400,1);
    }
    put32(out+R_LINK,(uint32_t)h->link);
    put32(out+R_PP,(uint32_t)(int32_t)h->pp);
    put32(out+R_PP_MIRROR,(uint32_t)(0u-(uint32_t)(int32_t)h->pp));
    put32(out+R_HAND,(uint32_t)(h->right_hand?1:0));
    put32(out+R_GENDER,(uint32_t)h->gender);
    for (i=0;i<HERO_ABILITIES;++i) put32(out+R_ABILITIES+4*i,(uint32_t)h->ability[i]);
    put32(out+R_DEATHS,(uint32_t)h->deaths);
    put32(out+R_KILLS,(uint32_t)h->kills);
    put32(out+R_INCARN,(uint32_t)h->incarnations);
    /* Both CRC fields come from the loaded world. FUN_00420240 stamps 0x6C4 from
     * DAT_004FA95C (the world CRC-1) on incarnate, and FUN_0044B196 compares the
     * stored stamp against the current g_world.crc1 when a soul is switched; a
     * stored 0 is the "has not made camp in your world" case. */
    put32(out+R_CAMPED_CRC,(uint32_t)g_world.crc1);
    for (i=0;i<8;++i) put32(out+R_ELEM_PP+4*i,(uint32_t)h->element_pp[i]);
    for (i=0;i<8;++i) put32(out+R_HAND_PP+4*i,(uint32_t)h->hand_pp[i]);
    put32(out+R_SECONDS,(uint32_t)h->seconds_played);
    put32(out+R_SAVES,(uint32_t)h->saves);
    put32(out+R_CKSUM_MODE,1);
    for (i=0;i<HERO_TOKENS;++i) if (h->tokens[i]) bit_set(out+R_TOKENS,i,1);
    put32(out+R_HAND_RATIO,(uint32_t)100);
    put32(out+R_MAXHP_RAW,(uint32_t)800000);
    put32(out+R_HUNTING,(uint32_t)h->hunting);
    put32(out+R_HALO,0);
    put32(out+R_MAXPP,(uint32_t)h->pp);
    for (i=0;i<128;++i) put32(out+R_TROPHY+4*i,h->trophy_bag[i]);
    put32(out+R_TROPHY_GEO,h->trophy_bag_geo);
    put32(out+R_PET0,h->pet_ids[0]);
    put32(out+R_PET1,h->pet_ids[1]);
    put32(out+R_CKSUM,hero_record_checksum(out,1));
}

int hero_record_decode(const uint8_t in[HERO_RECORD_SIZE], Hero *out)
{
    Hero v;
    int i, mode;
    size_t k;
    /* FUN_004181A2, all.c:17228-17247, in the original's order: the checksum
     * must match, and both mirror pairs must sum to zero. A record that fails
     * either is not a hero file and is rejected outright. */
    mode = get32s(in+R_CKSUM_MODE);
    if (hero_record_checksum(in,mode)!=get32(in+R_CKSUM)) return -1;
    if (get32s(in+R_XP)+get32s(in+R_XP_MIRROR)!=0) return -1;
    /* The same mirror discipline applies to the flag word, and the original
     * checks it (all.c:16485). */
    if (get32s(in+R_FLAGS)+get32s(in+R_FLAGS_MIR)!=0) return -1;
    if (get32s(in+R_PP)+get32s(in+R_PP_MIRROR)!=0) return -1;
    memset(&v,0,sizeof(v));
    v.serial = get32s(in+R_SERIAL);
    memcpy(v.name,in+R_NAME,HERO_NAME_MAX); v.name[HERO_NAME_MAX]=0;
    memcpy(v.skin,in+R_SKIN,sizeof(v.skin)); v.skin[sizeof(v.skin)]=0;
    v.klass = get32s(in+R_CLASS);
    v.level = get32s(in+R_LEVEL);
    v.xp    = get32s(in+R_XP);
    v.gold  = get32s(in+R_GOLD);
    v.hp = get32s(in+R_HP); v.max_hp = get32s(in+R_MAXHP);
    v.mp = get32s(in+R_MP); v.max_mp = get32s(in+R_MAXMP);
    v.map  = get32s(in+R_MAP);
    v.x = get32s(in+R_FIXED_X);
    v.y = get32s(in+R_FIXED_Y);
    v.target_x = get32s(in+R_TARGET_X);
    v.target_y = get32s(in+R_TARGET_Y);
    v.walk_speed = get32s(in+R_SPEED);
    v.walk_duration = get32s(in+R_WALK_DUR);
    v.facing = get32s(in+R_FACING);
    v.flags = get32s(in+R_FLAGS);
    v.link = get32s(in+R_LINK);
    v.pp   = get32s(in+R_PP);
    v.kills  = get32s(in+R_KILLS);
    v.deaths = get32s(in+R_DEATHS);
    v.incarnations = get32s(in+R_INCARN);
    v.saves = get32s(in+R_SAVES);
    v.hunting = get32s(in+R_HUNTING);
    v.world_crc = get32s(in+R_CAMPED_CRC);
    v.seconds_played = get32s(in+R_SECONDS);
    for (i=0;i<8;++i) {
        v.equip[i]=get32s(in+R_EQUIP+4*i);
        v.element_pp[i]=get32s(in+R_ELEM_PP+4*i);
        v.hand_pp[i]=get32s(in+R_HAND_PP+4*i);
    }
    v.right_hand = get32s(in+R_HANDITEM);
    v.gender = get32s(in+R_GENDER);
    for (i=0;i<HERO_ABILITIES;++i) v.ability[i]=get32s(in+R_ABILITIES+4*i);
    for (i=0;i<768;++i) v.learned_spells[i]=(unsigned char)bit_get(in+R_SPELLS,i);
    for (i=0;i<HERO_TOKENS;++i) v.tokens[i]=(unsigned char)bit_get(in+R_TOKENS,i);
    for (i=0;i<HERO_INVENTORY;++i) {
        int id=i+1;
        int n = id<0x400 ? in[R_ITEMCNT+id] : bit_get(in+R_ITEMBIT,id-0x400);
        v.inventory[i].item_id=n?id:0;
        v.inventory[i].count=n;
    }
    for (i=0;i<128;++i) v.trophy_bag[i]=get32(in+R_TROPHY+4*i);
    v.trophy_bag_geo=get32(in+R_TROPHY_GEO);
    v.pet_ids[0]=get32(in+R_PET0);
    v.pet_ids[1]=get32(in+R_PET1);
    /* FUN_004181A2 zeroes the six RAM-only fields the moment the bytes land, so
     * a decoded hero never carries a stale pointer. Nothing here ever held one
     * (Hero has no pointer members), so the loop exists to keep the list and
     * the rule in one place. */
    for (k=0;k<sizeof(ram_only)/sizeof(ram_only[0]);++k) (void)ram_only[k];
    if (!memchr(v.name,0,HERO_NAME_MAX) || !memchr(v.skin,0,sizeof(v.skin))) return -1;
    if (!safe_name(v.name) || v.level<1 || v.level>100 || v.xp<0 || v.gold<0 || v.pp<0) return -1;
    if (v.max_hp<1 || v.hp<0 || v.hp>v.max_hp || v.max_mp<0 || v.mp<0 || v.mp>v.max_mp) return -1;
    if (v.map<0 || v.map>=WORLD_MAX_MAPS || v.link<0 || v.link>=OBL_RECORDS) return -1;
    if (v.gender<0 || v.gender>3) return -1;
    if (v.right_hand<0 || v.right_hand>=WORLD_MAX_ITEMS) return -1;
    for (i=0;i<HERO_ABILITIES;++i) if (v.ability[i]<0 || v.ability[i]>255) return -1;
    for (i=0;i<8;++i) {
        if (v.equip[i]<0 || v.equip[i]>=WORLD_MAX_ITEMS) return -1;
        if (v.hand_pp[i]<0 || v.hand_pp[i]>5000000) return -1;
        if (v.element_pp[i]<0 || v.element_pp[i]>5000000) return -1;
    }
    for (i=0;i<HERO_INVENTORY;++i) {
        const HeroItem *item=&v.inventory[i];
        if (item->count<0 || item->count>(i+1>=0x400?1:100)) return -1;
        if (item->item_id!=(item->count?i+1:0)) return -1;
    }
    if (!hero_class(&v) || !hero_class(&v)->used) return -1;
    v.valid=1;
    for (i=HERO_SLOT_HELMET;i<=HERO_SLOT_RIGHT_HAND;++i) {
        int id=hero_equipped(&v,i);
        if (id && (hero_item_slot(id)!=i || !hero_item_count(&v,id))) return -1;
    }
    *out=v;
    return 0;
}

int hero_save(const Hero *h)
{
    char path[1024]; FILE *f; uint8_t rec[HERO_RECORD_SIZE]; size_t n; int failed;
    if (!h->valid || hero_path(path,sizeof(path),h->name,HERO_FILE_EXT,1)) return -1;
    /* FUN_00417F1B mirrors the 33-int selected-element block out of the global,
     * bumps the seconds and the save counter, forces the checksum mode to 1 and
     * stores the checksum, all before the single 0x16CC-byte fwrite. The block
     * and the counter are already current in g_hero, so only the mode and the
     * checksum are set here; the record is built from the caller's hero when it
     * is g_hero and from a temporary copy otherwise. */
    if (h==&g_hero) hero_record_encode(rec);
    else { Hero save=g_hero; g_hero=*h; hero_record_encode(rec); g_hero=save; }
    f = plat_fopen(path,"wb"); if (!f) return -1;
    n = fwrite(rec,1,HERO_RECORD_SIZE,f);
    failed = (n!=HERO_RECORD_SIZE) || ferror(f); if (fclose(f)) failed = 1;
    if (!failed) { g_hero.saves++; wos_log_event("hero_save","name=%s bytes=%d",h->name,(int)n); }
    return failed ? -1 : 0;
}

int hero_load(Hero *h, const char *name)
{
    char path[1024]; FILE *f; uint8_t rec[HERO_RECORD_SIZE]; Hero v; size_t n; int failed;
    if (hero_path(path,sizeof(path),name,HERO_FILE_EXT,0)) return -1;
    f = plat_fopen(path,"rb"); if (!f) return -1;
    n = fread(rec,1,HERO_RECORD_SIZE,f);
    failed = ferror(f) || n!=HERO_RECORD_SIZE;
    fclose(f);
    if (failed || hero_record_decode(rec,&v)) return -1;
    if (text_casecmp(v.name,name)) return -1;
    *h=v; scene_reset_timers(); return 0;
}

int hero_delete(const char *name)
{
    char path[1024];
    if (hero_path(path,sizeof(path),name,HERO_FILE_EXT,0)) return -1;
    if (remove(path)) return -1;
    wos_log_event("hero_purge","name=%s",name);
    return 0;
}

typedef struct { char (*names)[HERO_NAME_MAX]; int count, max; } SaveList;
/* FUN_00477060 (all.c:0x477060) lists savedHeroes and truncates each entry at
 * its LAST '.', which is how "Bob.her" becomes "Bob". */
static void save_entry(const char *name,int is_dir,void *user)
{
    SaveList *l=user; const char *dot;
    size_t n=strlen(name);
    if (is_dir || l->count>=l->max) return;
    dot = strrchr(name,'.');
    if (!dot || dot==name) return;
    n = (size_t)(dot-name);
    if (n>=HERO_NAME_MAX) return;
    memcpy(l->names[l->count],name,n); l->names[l->count][n]=0;
    if (safe_name(l->names[l->count])) ++l->count;
}
static int compare_names(const void *a,const void *b) { return text_casecmp(a,b); }
int hero_list_saves(char names[][HERO_NAME_MAX],int max)
{
    char path[1024]; SaveList l={names,0,max};
    if (max<=0 || hero_path(path,sizeof(path),NULL,NULL,0)) return 0;
    plat_list_dir(path,save_entry,&l);
    qsort(names,(size_t)l.count,HERO_NAME_MAX,compare_names); return l.count;
}

int64_t hero_xp_for_level(const Hero *h,int level)
{
    int n; int64_t total=0, step=20;
    (void)h;
    if(level>100) level=100;
    /* FUN_00483984: cumulative shared-class curve. The decomp drops the
     * FMUL at 0x4846F3: doubles 0x4D0220=.01, 0x4D0228=450000. */
    for(n=1;n<level;++n) {
        int64_t cap=(int64_t)(pow(n * 0.01,2.5) * 450000.0)+10;
        step=step*150/100; if(step>cap) step=cap; total+=step;
    }
    return total;
}

int hero_award(Hero *h,int64_t xp,int64_t gold)
{
    const ClassDef *c=hero_class(h); int gained=0;
    if(!c || !h->valid) return 0;
    if(xp>0) h->xp=xp>INT64_MAX-h->xp ? INT64_MAX : h->xp+xp;
    while(h->level<100 && h->xp>=hero_xp_for_level(h,h->level+1)) {
        int old_hp=h->max_hp, old_mp=h->max_mp;
        ++h->level; ++gained; maxima(c,h->level,&h->max_hp,&h->max_mp);
        h->hp=bounded_stat((int64_t)h->hp+h->max_hp-old_hp);
        h->mp=bounded_stat((int64_t)h->mp+h->max_mp-old_mp);
        wos_log_event("level_up","level=%d",h->level);
    }
    if(gold>0) hero_add_gold(h,gold);
    return gained;
}

static const ItemDef *hero_item(int id)
{
    return id > 0 && id < WORLD_MAX_ITEMS && g_world.items[id].used ? &g_world.items[id] : NULL;
}

int hero_item_count(const Hero *h, int id)
{
    if (!h || !hero_item(id)) return 0;
    return h->inventory[id - 1].count;
}

int hero_item_limit(const Hero *h, int id)
{
    const ItemDef *item = hero_item(id);
    int limit;
    if (!h || !h->valid || !item) return 0;
    /* FUN_00403349: the "wallet size" in items.txt is level+1, not current GP. */
    limit = id >= 1024 ? 1 : h->level + 1;
    if (limit > 100) limit = 100;
    if (id < 1024 && item->max_count > 0 && item->max_count < limit) limit = item->max_count;
    /* FUN_00484E72: zero-price quest objects never accumulate multiple copies. */
    if (!item->gp && limit > 1) limit = 1;
    return limit;
}

int hero_give_item(Hero *h, int id, int count)
{
    int have = hero_item_count(h,id), limit = hero_item_limit(h,id);
    if (!h || count <= 0 || count > limit - have) return 0;
    h->inventory[id - 1].item_id = id;
    h->inventory[id - 1].count += count;
    return 1;
}

int hero_take_item(Hero *h, int id, int count)
{
    int slot;
    if (!h || count <= 0 || hero_item_count(h,id) < count) return 0;
    h->inventory[id - 1].count -= count;
    if (!h->inventory[id - 1].count) {
        h->inventory[id - 1].item_id = 0;
        slot = hero_item_slot(id);
        if (slot >= 0 && hero_equipped(h,slot) == id) hero_unequip(h,slot);
    }
    return 1;
}

int64_t hero_wallet_limit(const Hero *h)
{
    const ClassDef *c = hero_class(h);
    int64_t cap;
    if (!c || !h->valid) return 0;
    /* FUN_0042BAA8, then the class's optional MAX_WALLET. */
    cap = ((int64_t)h->level + 1) * 10000;
    if (cap > 1000000) cap = 1000000;
    if (c->max_wallet >= 0 && cap > c->max_wallet) cap = c->max_wallet;
    return cap < 0 ? 0 : cap;
}

static int64_t add_wallet(int64_t *wallet, int64_t amount, int64_t limit)
{
    int64_t old = *wallet;
    if (amount > 0 && amount > limit - old) *wallet = limit;
    else if (amount < 0 && amount < -old) *wallet = 0;
    else *wallet += amount;
    return *wallet - old;
}

int64_t hero_add_gold(Hero *h, int64_t amount)
{
    if (!h || !h->valid) return 0;
    return add_wallet(&h->gold,amount,hero_wallet_limit(h));
}

int hero_item_slot(int id)
{
    const ItemDef *item = hero_item(id);
    if (!item) return -1;
    if (item->klass == 10) return HERO_SLOT_HELMET;
    if (item->klass == 11) return HERO_SLOT_ARMOR;
    if (item->klass >= 12 && item->klass <= 19) return HERO_SLOT_RIGHT_HAND;
    if (item->klass >= 20 && item->klass <= 23) return HERO_SLOT_BOOTS + item->klass - 20;
    return -1;
}

int hero_equipped(const Hero *h, int slot)
{
    if (!h) return 0;
    if (slot == HERO_SLOT_RIGHT_HAND) return h->right_hand;
    return slot >= HERO_SLOT_HELMET && slot <= HERO_SLOT_AMULET ? h->equip[slot] : 0;
}

static int item_requirements(const Hero *h, const ItemDef *item)
{
    if (!h || !h->valid || !item || h->level < item->level) return 0;
    return !item->equip_token ||
        (item->equip_token > 0 && item->equip_token < HERO_TOKENS && h->tokens[item->equip_token]);
}

int hero_can_equip(const Hero *h, int id)
{
    const ItemDef *item = hero_item(id);
    const ClassDef *c = hero_class(h);
    int hand;
    if (!c || hero_item_slot(id) < 0 || !item_requirements(h,item) || !hero_item_count(h,id)) return 0;
    hand = item->klass - 12;
    /* FUN_0040D6B4: preferred hand, or level-five proficiency in another hand. */
    if (hand >= 0 && hand < 8 && c->right_hand != hand + 1 && hero_pp_level(h->hand_pp[hand]) < 5)
        return 0;
    return 1;
}

int hero_equip(Hero *h, int id)
{
    int slot = hero_item_slot(id);
    if (!hero_can_equip(h,id) || hero_equipped(h,slot) == id) return 0;
    if (slot == HERO_SLOT_RIGHT_HAND) h->right_hand = id;
    else h->equip[slot] = id;
    wos_log_event("equip","slot=%d item=%d",slot,id);
    return 1;
}

int hero_unequip(Hero *h, int slot)
{
    int id = hero_equipped(h,slot);
    if (!h || !h->valid || !id) return 0;
    if (slot == HERO_SLOT_RIGHT_HAND) h->right_hand = 0;
    else h->equip[slot] = 0;
    wos_log_event("unequip","slot=%d item=%d",slot,id);
    return 1;
}

static int equipment_stat(const Hero *h, int offense)
{
    int slot;
    int64_t value = 0;
    if (!h || !h->valid) return 0;
    /* FUN_0040C694 sums every equipped item's signed attack/defense, clamps
     * once at the end. Cursed items can cancel positive equipment bonuses. */
    for (slot = HERO_SLOT_HELMET; slot <= HERO_SLOT_RIGHT_HAND; ++slot) {
        int id = hero_equipped(h,slot);
        const ItemDef *item = hero_item(id);
        if (item && hero_can_equip(h,id)) value += offense ? item->attack : item->defense;
    }
    return value > 32767 ? 32767 : value > 0 ? (int)value : 0;
}

int hero_offense(const Hero *h) { return equipment_stat(h,1); }
int hero_defense(const Hero *h) { return equipment_stat(h,0); }
int hero_ability(const Hero *h, int ability)
{
    const ClassDef *c = hero_class(h);
    int value, cap;
    if (!c || ability < 0 || ability >= HERO_ABILITIES) return 0;
    /* FUN_00449006: equipment changes attack/defense, not ability points. */
    value = h->ability[ability]; cap = c->max_ability[ability];
    if (cap > 255) cap = 255;
    if (value > cap) value = cap;
    return value < 0 ? 0 : value;
}

int hero_pp_level(int pp)
{
    /* levels.txt PP Levels, L0..L9; integer boundaries are part of the API. */
    static const int threshold[10] = {0,555,1250,2142,3333,5000,7500,11666,20000,45000};
    int level = 9;
    while (level > 0 && pp < threshold[level]) --level;
    return level;
}

int hero_train_limit(const Hero *h, int kind, int index)
{
    const ClassDef *c = hero_class(h);
    int cap;
    if (!c || index < 0 || index >= 8 || (kind != HERO_TRAIN_HAND && kind != HERO_TRAIN_ELEMENT)) return 0;
    cap = kind == HERO_TRAIN_HAND ? c->max_hand_pp[index] : c->max_element_pp[index];
    return cap < 0 ? 0 : cap > 5000000 ? 5000000 : cap;
}

int64_t hero_add_pp(Hero *h, int64_t amount)
{
    if (!h || !h->valid) return 0;
    return add_wallet(&h->pp,amount,g_world.max_unspent_pp);
}

void hero_gain_training(Hero *h, int kind, int index, int amount)
{
    int cap = hero_train_limit(h,kind,index), *pp, i;
    if (!h || !h->valid || amount <= 0 || amount > 2000 || cap <= 0) return;
    pp = kind == HERO_TRAIN_HAND ? &h->hand_pp[index] : &h->element_pp[index];
    if (amount > cap - *pp) amount = cap - *pp;
    if (amount <= 0) return;
    /* FUN_00419306 / FUN_004258B8; combat training does not debit the wallet. */
    *pp += amount;
    if (kind == HERO_TRAIN_ELEMENT) {
        h->element_pp[(index+4)&7] -= amount*15/100;
        h->element_pp[(index+3)&7] -= amount*5/100;
        h->element_pp[(index+5)&7] -= amount*5/100;
        h->element_pp[(index+2)&7] -= amount/100;
        h->element_pp[(index+6)&7] -= amount/100;
        for (i=0;i<8;++i) if (h->element_pp[i]<20) h->element_pp[i]=20;
    }
}

int hero_train(Hero *h, int kind, int index, int amount)
{
    int cap = hero_train_limit(h,kind,index), *pp;
    if (!h || !h->valid || amount <= 0 || amount > 2000 || cap < amount || h->pp < amount) return 0;
    pp = kind == HERO_TRAIN_HAND ? &h->hand_pp[index] : &h->element_pp[index];
    if (*pp > cap - amount) return 0;
    /* 0x419107/0x425C30: wallet purchase uses the same update as combat. */
    h->pp -= amount;
    hero_gain_training(h,kind,index,amount);
    wos_log_event("train","kind=%s index=%d pp=%d level=%d",
        kind == HERO_TRAIN_HAND ? "hand" : "element",index,*pp,hero_pp_level(*pp));
    return 1;
}

static int spell_requirements(const Hero *h, int id)
{
    const SpellDef *spell;
    if (!h || !h->valid || id <= 0 || id >= WORLD_MAX_SPELLS) return 0;
    spell = &g_world.spells[id];
    if (!spell->used || spell->element < 0 || spell->element >= 8 || h->level < spell->min_level) return 0;
    if (spell->token && (spell->token < 0 || spell->token >= HERO_TOKENS || !h->tokens[spell->token])) return 0;
    return hero_pp_level(h->element_pp[spell->element]) >= spell->req_affinity;
}

int hero_spell_known(const Hero *h, int id)
{
    return spell_requirements(h,id) && h->learned_spells[id];
}

int hero_can_learn_spell(const Hero *h, int id)
{
    if (!spell_requirements(h,id) || h->learned_spells[id]) return 0;
    return !(g_world.spells[id].flags & 2) && g_world.spells[id].pp_cost >= 0 &&
        h->pp >= g_world.spells[id].pp_cost;
}

int hero_learn_spell(Hero *h, int id)
{
    /* FUN_0044E3BD: eligibility first, debit cost once, then set learned bit. */
    if (!hero_can_learn_spell(h,id)) return 0;
    h->pp -= g_world.spells[id].pp_cost;
    h->learned_spells[id] = 1;
    wos_log_event("spell_learn","spell=%d pp=%lld",id,(long long)h->pp);
    return 1;
}

int hero_set_spell(Hero *h, int id, int known)
{
    if (!h || !h->valid || id <= 0 || id >= WORLD_MAX_SPELLS || !g_world.spells[id].used) return 0;
    h->learned_spells[id] = known != 0;
    return 1;
}
/* ------------------------------------------------- the per-hero INI --------
 * One file per soul, "<save>/<world>/savedHeroes/<Name>" (FUN_00460962 with the
 * empty extension in DAT_004DCBAC, so Windows drops the trailing dot). It holds
 * the quest cookie jar in [cookies] (FUN_0047A9F5 reads it, FUN_0047AB07 writes
 * it, both _stricmp on the key) and the per-monster tallies in the three
 * sections FUN_0043B0FD names: "monsters killed", "killed by monster" and
 * "monsters seen", each keyed by the decimal monster id (sprintf("%0d", id)).
 *
 * The port has no Win32 profile API, so the file is read whole, patched line by
 * line in memory and rewritten. That is a storage detail, not a behaviour one:
 * the section names, the key spelling and the case-insensitive lookup are the
 * original's, so a file written here and a file written by the original are
 * interchangeable. */
#define INI_MAX_BYTES (64*1024)

static int ini_file_path(char *path, size_t size)
{
    return g_hero.valid ? hero_path(path,size,g_hero.name,"",0) : -1;
}

/* Reads the whole INI into `buf` (always NUL-terminated) and returns its length,
 * or -1 when the file does not exist. */
static int ini_read(char *buf, size_t cap)
{
    char path[1024]; FILE *f; size_t n;
    buf[0]=0;
    if (ini_file_path(path,sizeof(path))) return -1;
    f = plat_fopen(path,"rb"); if (!f) return -1;
    n = fread(buf,1,cap-1,f);
    if (ferror(f)) n = 0;
    fclose(f);
    buf[n]=0;
    return (int)n;
}

static int ini_write(const char *buf, int len)
{
    char path[1024]; FILE *f; size_t n;
    if (ini_file_path(path,sizeof(path))) return -1;
    f = plat_fopen(path,"wb"); if (!f) return -1;
    n = fwrite(buf,1,(size_t)len,f);
    if (fclose(f) || n!=(size_t)len) return -1;
    return 0;
}

/* Rewrites `key` inside [section]. An empty value deletes the key, which is what
 * WritePrivateProfileStringA(section,key,NULL,path) does in FUN_0047AB07. */
static int ini_patch(const char *section, const char *key, const char *value)
{
    static char buf[INI_MAX_BYTES];
    char out[INI_MAX_BYTES];
    char line[1200];
    int len, olen=0, in_section=0, done=0;
    const char *p, *nl;
    len = ini_read(buf,sizeof(buf));
    if (len<0) len=0;
    out[0]=0;
    p = buf;
    while (*p && !done) {
        nl = strchr(p,'\n');
        if (!nl) nl = p+strlen(p);
        if (nl>p && p[0]!=';' && p[0]!='#') {
            const char *t = p;
            while (t<nl && (*t==' ' || *t=='\t')) ++t;
            if (*t=='[') {
                char name[128]; size_t n = (size_t)(nl-t);
                if (n>2 && t[n-1]==']') { n-=2; if (n>=sizeof(name)) n=sizeof(name)-1;
                    memcpy(name,t+1,n); name[n]=0; in_section = !text_casecmp(name,section); }
            } else if (in_section) {
                const char *eq = memchr(t,'=',(size_t)(nl-t));
                if (eq) {
                    char k[256]; size_t n = (size_t)(eq-t);
                    while (n && (t[n-1]==' ' || t[n-1]=='\t')) --n;
                    if (n>=sizeof(k)) n=sizeof(k)-1;
                    memcpy(k,t,n); k[n]=0;
                    if (!text_casecmp(k,key)) {
                        done = 1;
                        if (*value) {
                            snprintf(line,sizeof(line),"%s=%s\n",key,value);
                            if (olen+(int)strlen(line)<(int)sizeof(out))
                                olen += snprintf(out+olen,sizeof(out)-olen,"%s",line);
                        }
                        continue; /* the old line is dropped, with or without a replacement */
                    }
                }
            }
        }
        if (olen+(int)(nl-p)+1<(int)sizeof(out))
            olen += snprintf(out+olen,sizeof(out)-olen,"%.*s\n",(int)(nl-p),p);
        p = nl;
        while (*p=='\n' || *p=='\r') ++p;
    }
    if (!done && *value) {
        /* A key that was not there is appended, creating the section if it is
         * missing. GetPrivateProfileString's empty-string default means a key
         * with no value reads back as unset, which is what the port returns. */
        const char *q; int have_section = 0;
        for (q=out; (q=strchr(q,'['))!=NULL; ++q)
            if (!text_casecmp(q+1,section)) { have_section = 1; break; }
        if (!have_section)
            olen += snprintf(out+olen,sizeof(out)-olen,"[%s]\n",section);
        snprintf(line,sizeof(line),"%s=%s\n",key,value);
        if (olen+(int)strlen(line)<(int)sizeof(out))
            olen += snprintf(out+olen,sizeof(out)-olen,"%s",line);
    }
    return ini_write(out,olen);
}

/* FUN_0047A9F5: GetPrivateProfileString("cookies", key, "", out, size, path).
 * The lookup is _stricmp on the key and the default is the empty string, so a
 * key that is absent and a key with an empty value both read back as unset. */
const char *hero_cookie_get(const char *key)
{
    static char buf[INI_MAX_BYTES];
    static char out[1024];
    const char *p = buf;
    int in_section = 0;
    if (!key || !*key || ini_read(buf,sizeof(buf))<0) return NULL;
    while (*p) {
        const char *nl = strchr(p,'\n');
        size_t len = nl ? (size_t)(nl-p) : strlen(p);
        while (len && (p[len-1]=='\r' || p[len-1]==' ' || p[len-1]=='\t')) --len;
        if (len && p[0]=='[' && p[len-1]==']') {
            char name[128]; size_t n = len-2;
            if (n>=sizeof(name)) n=sizeof(name)-1;
            memcpy(name,p+1,n); name[n]=0;
            in_section = !text_casecmp(name,"cookies");
        } else if (in_section && len && p[0]!=';' && p[0]!='#') {
            const char *eq = memchr(p,'=',len);
            if (eq) {
                char k[256]; size_t n = (size_t)(eq-p);
                if (n<sizeof(k)) {
                    memcpy(k,p,n); k[n]=0;
                    while (n && (k[n-1]==' '||k[n-1]=='\t')) k[--n]=0;
                    if (!text_casecmp(k,key)) {
                        const char *v = eq+1; size_t vn = (size_t)(p+len-v);
                        while (vn && (v[vn-1]==' '||v[vn-1]=='\t')) --vn;
                        if (!vn) return NULL;   /* the empty default, i.e. unset */
                        if (vn>=sizeof(out)) vn=sizeof(out)-1;
                        memcpy(out,v,vn); out[vn]=0;
                        return out;
                    }
                }
            }
        }
        if (!nl) break;
        p = nl+1;
    }
    return NULL;
}

int hero_cookie_set(const char *key, const char *value)
{
    if (!key || !*key) return -1;
    return ini_patch("cookies",key,value && *value?value:"");
}

void hero_cookie_del(const char *key)
{
    if (key && *key) (void)ini_patch("cookies",key,"");
}

uint32_t hero_kills_total(void)  { return (uint32_t)g_hero.kills; }
uint32_t hero_deaths_total(void) { return (uint32_t)g_hero.deaths; }
int hero_add_kill(Hero *h)       { return h ? (h->kills += 1) : 0; }
int hero_add_kills(Hero *h, int n) { return h ? (h->kills += n) : 0; }
int hero_add_death(Hero *h)      { return h ? (h->deaths += 1) : 0; }

static int monster_key(char *key, size_t size, int monster_id)
{ return snprintf(key,size,"%0d",monster_id)<0?-1:0; }

static int tally_get(const char *section, int monster_id, int fallback)
{
    char key[32], path[1024]; FILE *f; char line[512]; int value = fallback;
    if (monster_key(key,sizeof(key),monster_id) || ini_file_path(path,sizeof(path))) return fallback;
    f = plat_fopen(path,"rb"); if (!f) return fallback;
    while (fgets(line,sizeof(line),f)) {
        char *eq, *ke;
        if (line[0]!='[') continue;
        eq = strchr(line,']');
        if (!eq) continue;
        line[eq-line]=0;
        if (text_casecmp(line+1,section)) continue;
        while (fgets(line,sizeof(line),f)) {
            if (line[0]=='[') break;
            eq = strchr(line,'=');
            if (!eq) continue;
            *eq=0; ke = eq;
            while (ke>line && (ke[-1]==' '||ke[-1]=='\t')) --ke;
            *ke=0;
            if (text_casecmp(line,key)) continue;
            value = (int)strtol(eq+1,NULL,10);
            break;
        }
        break;
    }
    fclose(f);
    return value;
}

static int tally_add(const char *section, int monster_id)
{
    char key[32]; int value;
    if (monster_key(key,sizeof(key),monster_id)) return 0;
    value = tally_get(section,monster_id,0)+1;
    { char text[32]; snprintf(text,sizeof(text),"%d",value); (void)ini_patch(section,key,text); }
    return value;
}

int hero_kills_of_monster(int m)  { return tally_get("monsters killed",m,0); }
int hero_deaths_by_monster(int m) { return tally_get("killed by monster",m,0); }
int hero_monsters_seen(int m)      { return tally_get("monsters seen",m,-1); }
int hero_kill_monster(int m)       { return tally_add("monsters killed",m); }
int hero_killed_by_monster(int m)  { return tally_add("killed by monster",m); }

/* ------------------------------------------------------------- the dump ---
 * hero.record is the comparison unit: the whole 0x16CC record as lowercase
 * hex, exactly as it sits in the file. The decoded fields exist so a human can
 * read a diff; hero.abil.* are separate keys because Core asked for them by
 * name. Nothing here allocates: one 0x16CC buffer and two small ones. */
void hero_dump(DumpEmit emit, void *user)
{
    static uint8_t rec[HERO_RECORD_SIZE];
    static char hex[HERO_RECORD_SIZE*2+1];
    static char bag[1024];
    static const char *const abil_name[HERO_ABILITIES] = {"str","wis","sta","agi","dex"};
    const Hero *h = &g_hero;
    int i, n = 0;
    hero_record_encode(rec);
    for (i=0;i<HERO_RECORD_SIZE;++i) {
        static const char d[] = "0123456789abcdef";
        hex[i*2]=d[rec[i]>>4]; hex[i*2+1]=d[rec[i]&15];
    }
    hex[HERO_RECORD_SIZE*2]=0;
    emit("hero.record",hex,user);
    /* The original has a slot allocated from startup, so the named keys are
     * emitted whenever the slot exists, not only when a soul is loaded: at the
     * title screen FUN_00427D89 has already set in_use=1 and the serial, with
     * every other field still zero. Emitting hero.valid=0 and nothing else
     * there was a port-only shape the original never has. */
    emit("hero.valid",h->valid?"1":"0",user);
    dump_emit_int(emit,"hero.in_use",h->slot_in_use?1:0,user);
    emit("hero.name",h->name,user);
    dump_emit_int(emit,"hero.serial",h->serial,user);
    dump_emit_int(emit,"hero.class",h->klass,user);
    dump_emit_int(emit,"hero.level",h->level,user);
    dump_emit_int(emit,"hero.gender",h->gender,user);
    dump_emit_int(emit,"hero.xp",(long long)h->xp,user);
    dump_emit_int(emit,"hero.gold",(long long)h->gold,user);
    dump_emit_int(emit,"hero.pp",(long long)h->pp,user);
    dump_emit_int(emit,"hero.hp",h->hp,user);
    dump_emit_int(emit,"hero.max_hp",h->max_hp,user);
    dump_emit_int(emit,"hero.mp",h->mp,user);
    dump_emit_int(emit,"hero.max_mp",h->max_mp,user);
    dump_emit_int(emit,"hero.map",h->map,user);
    dump_emit_int(emit,"hero.link",h->link,user);
    dump_emit_int(emit,"hero.x",h->x,user);
    dump_emit_int(emit,"hero.y",h->y,user);
    dump_emit_int(emit,"hero.kills",h->kills,user);
    dump_emit_int(emit,"hero.deaths",h->deaths,user);
    dump_emit_int(emit,"hero.incarnations",h->incarnations,user);
    dump_emit_int(emit,"hero.hunting",h->hunting,user);
    dump_emit_int(emit,"hero.base_offset",(long long)hero_base_offset(),user);
    for (i=0;i<HERO_ABILITIES;++i) {
        static char key[32];
        snprintf(key,sizeof(key),"hero.abil.%s",abil_name[i]);
        dump_emit_int(emit,key,h->ability[i],user);
    }
    snprintf(hex,sizeof(hex),"%08x",(unsigned)get32(rec+R_CKSUM));
    emit("hero.checksum",hex,user);
    /* The bag words are stored obfuscated; the dump reports what the owner sees. */
    bag[0]=0;
    for (i=0;i<128 && n<(int)sizeof(bag)-32;++i) {
        uint32_t w = h->trophy_bag[i] ^ UINT32_C(0x1D43E217);
        if (!w) continue;
        n += snprintf(bag+n,sizeof(bag)-n,"%s%u:%u",n?",":"",
                      (unsigned)((w>>16)&0xFFFF),(unsigned)((w>>8)&0xFF));
    }
    if (!n) snprintf(bag,sizeof(bag),"0");
    emit("hero.trophy_bag",bag,user);
    if (trophy_bag_absent(h)) snprintf(hex,sizeof(hex),"0x0");
    else { uint32_t g = h->trophy_bag_geo ^ UINT32_C(0x1D43E217);
           snprintf(hex,sizeof(hex),"%ux%u",(unsigned)(g>>16),(unsigned)(g&0xFFFF)); }
    emit("hero.trophy_bag_size",hex,user);
    snprintf(hex,sizeof(hex),"%u",(unsigned)h->pet_ids[0]);
    emit("hero.pet_id",hex,user);
}

/* --------------------------------------------------- the personal BIO ----
 * FUN_00452107 is the bio editor's commit handler, reached from FUN_00450E94,
 * FUN_00450DBC and FUN_00452DF0. Its +0x94 == 0 path reads the bio edit
 * (CWnd at +0x338) and writes "%s\BIO\%08X.txt" verbatim when the text is
 * non-empty, unlinks it when it is empty, and then unlinks the name-only
 * "%s\BIO\%s.txt" and "%s\BIO\%s.ini" siblings (all.c:58447-58470). The
 * structured half (all.c:58040-58070) writes "%s\bio\%08X-%s.ini" with the
 * section "Bio" (0x4EB538) and the keys serNum, className, levelName,
 * worldLocation and skin.
 *
 * The original keys both files on the hero's SERIAL and writes them under the
 * INSTALL ROOT, not the save directory. The port has no install root, so it
 * writes them under game_save_path()/bio/ - the same save root the .her and the
 * per-hero INI live in - and keeps the original's "%08X" keying and "%08X-%s"
 * file names so the two are the same documents. */
static int bio_path(char *path, size_t size, const char *name, int serial)
{
    int n;
    if (!g_hero.valid) return -1;
    n = snprintf(path,size,"%s/bio",game_save_path());
    if (n < 0 || (size_t)n >= size) return -1;
    if (plat_mkdir(path)) return -1;
    n = name && *name
      ? snprintf(path,size,"%s/bio/%08X-%s.ini",game_save_path(),serial,name)
      : snprintf(path,size,"%s/bio/%08X.ini",game_save_path(),serial);
    return n < 0 || (size_t)n >= size ? -1 : 0;
}
static int bio_text_path(char *path, size_t size, int serial)
{
    int n = snprintf(path,size,"%s/bio/%08X.txt",game_save_path(),serial);
    return n < 0 || (size_t)n >= size ? -1 : 0;
}

int hero_bio_save(const char *text)
{
    char path[1024], text_path[1024], sibling[1024];
    FILE *f; int failed = 0;
    if (!g_hero.valid) return -1;
    if (bio_text_path(text_path,sizeof(text_path),g_hero.serial)) return -1;
    if (bio_path(path,sizeof(path),g_hero.name,g_hero.serial)) return -1;
    if (!text || !*text) {
        /* FUN_00452107's empty-text branch: _unlink the text and the siblings. */
        remove(text_path);
    } else {
        size_t len = strlen(text);
        f = plat_fopen(text_path,"wb");
        if (!f) return -1;
        if (fwrite(text,1,len,f)!=len) failed = 1;
        if (fclose(f)) failed = 1;
    }
    if (snprintf(sibling,sizeof(sibling),"%s/bio/%s.txt",game_save_path(),g_hero.name) < (int)sizeof(sibling)) remove(sibling);
    if (snprintf(sibling,sizeof(sibling),"%s/bio/%s.ini",game_save_path(),g_hero.name) < (int)sizeof(sibling)) remove(sibling);
    {
        const ClassDef *c = hero_class(&g_hero);
        char body[2048];
        int n = snprintf(body,sizeof(body),
            "[Bio]\r\n"
            "serNum=%d\r\n"
            "className=%s\r\n"
            "levelName=%s\r\n"
            "worldLocation=%s\r\n"
            "skin=%s\r\n",
            g_hero.serial,
            c && c->used ? c->name : "",
            c && c->used && g_hero.level>=0 && g_hero.level<100 ? c->levels[g_hero.level].name : "",
            g_world.name, g_hero.skin);
        if (n < 0 || (size_t)n >= sizeof(body)) failed = 1;
        f = plat_fopen(path,"wb");
        if (!f) return -1;
        if (fwrite(body,1,(size_t)n,f)!=(size_t)n) failed = 1;
        if (fclose(f)) failed = 1;
    }
    wos_log_event("hero_bio_save","name=%s serial=%08x",g_hero.name,(unsigned)g_hero.serial);
    return failed ? -1 : 0;
}

char *hero_bio_text(size_t *size)
{
    char path[1024]; FILE *f; char *buf; size_t n;
    if (size) *size = 0;
    if (!g_hero.valid || bio_text_path(path,sizeof(path),g_hero.serial)) return NULL;
    f = plat_fopen(path,"rb"); if (!f) return NULL;
    buf = (char *)malloc(HERO_BIO_TEXT_MAX);
    if (!buf) { fclose(f); return NULL; }
    n = fread(buf,1,HERO_BIO_TEXT_MAX-1,f);
    if (ferror(f)) n = 0;
    fclose(f);
    buf[n]=0;
    if (size) *size = n;
    return buf;
}

/* FUN_0044B196, the soul-switch world check, at all.c:0x53935:
 *     if ((DAT_004fa95c != hero[0x1B1]) && (DAT_004e70c8 != 0)) {
 *         if (hero[0x1B1] == 0)  "You are Cautious" / "This soul has not made camp in your world"
 *         else                   "Modified Quest File Detected" / "This soul marches to ..."
 *         FUN_00458343(...); return 0; }
 * It is a HARD BLOCK in both worded cases - the return value of FUN_00458343 is
 * discarded and the next instruction returns 0 - so there is no override. Only
 * the MESSAGE differs between "never camped here" (a stored 0) and "camped in a
 * different world version". `*mismatch` is 0 for a match, 1 for a genuine
 * mismatch and 2 for the never-camped case, so the caller picks the wording
 * without re-deriving it.
 *
 * `avoid` is DAT_004E70C8, the WIN.INI Preferences\avoidModifiedQuestFiles int,
 * default 1. The port has no profile store, so callers pass 1, which is the
 * default install. */
int hero_world_crc_check(int avoid, int *mismatch)
{
    uint32_t stored = (uint32_t)g_hero.world_crc;
    if (mismatch) *mismatch = 0;
    if (!g_hero.valid) return 0;
    if (stored == g_world.crc1) return 1;
    if (mismatch) *mismatch = stored ? 1 : 2;
    return !avoid;
}

/* FUN_0046230E, 0x46230E (all.c:70725-70728): the facing is fy*4 + fx, and the
 * value 5 is remapped to 9 before packing, so the nine legal packings are not
 * contiguous. The port stores this encoding NATIVELY -- a raw-word diff against
 * the original's record then compares like with like, instead of reporting a
 * false mismatch on every facing change, which is the failure mode that hides
 * a real one. */
int hero_facing_encode(int fx, int fy)
{
    int v = fy*4 + fx;
    if (v == 5) return 9;
    if (v == 9) return 5;   /* the remap is its own inverse, so decode round-trips */
    return v;
}
void hero_facing_decode(int facing, int *fx, int *fy)
{
    int v = facing;
    if (v == 5) v = 9; else if (v == 9) v = 5;
    if (fx) *fx = v & 3;
    if (fy) *fy = v >> 2;
}

/* Set by Core at the point in the boot sequence where FUN_004269AF consumes
 * draw 1409. See hero.h for why this is not the seed. */
static uint32_t base_offset_value;
uint32_t hero_base_offset(void) { return base_offset_value; }
void hero_set_base_offset(uint32_t value) { base_offset_value = value; }

/* The hero flag word, hero+0xA58. Bit 2 is the script's IF M2 condition - the
 * "Modified Quest File Detected" flag, which FUN_004142F2's pet-call gate reads
 * as `(*(byte *)(hero + 0xA58) & 2) != 0`. The whole word is exposed so a
 * caller can mask any bit rather than being handed a pre-selected one. */
int hero_flag(const Hero *hero, unsigned bit)
{ return hero ? ((hero->flags >> (bit & 31u)) & 1u) : 0; }

/* Set or clear a flag bit. front.c owns the policy: it calls this when
 * hero_world_crc_check() reports a genuine mismatch (bit 2), which is the only
 * place the original sets it, and when avoidModifiedQuestFiles is off
 * (bit 4, FUN_004978D5). It is STORED in the record and round-trips, rather than
 * recomputed on read, because the original stores it -- but it is derived from
 * the world-CRC comparison, so a caller holding the two crc fields may equally
 * compute it rather than call this. */
void hero_set_flag(Hero *hero, unsigned bit, int on)
{
    if (!hero) return;
    if (on) hero->flags |= (uint32_t)1u << (bit & 31u);
    else   hero->flags &= ~((uint32_t)1u << (bit & 31u));
}

/* 24.8 -> map units, as the ORIGINAL does it. Seven sites use the same idiom,
 * and it is not a plain shift:
 *   all.c:23512  (int)(piVar4[0x25] + (piVar4[0x25] >> 0x1f & 0xffU)) >> 8
 *   all.c:23513, 70882, 70885, 70971, 70972, 72525, 72527 -- the same
 * `(x + (x >> 31 & 0xFF)) >> 8` is MSVC's round-toward-zero for a signed divide
 * by 256: the +255 bias applies only when x is negative, then the arithmetic
 * shift runs. A plain `x >> 8` FLOORS instead, so for any negative coordinate
 * the two disagree by exactly one map unit. The walk produces negative y
 * routinely, so this is not a corner case -- Oracle4.ItemsPanelsVAs caught it
 * and it is a real defect in this helper, not in the record layout. */
static int fixed24_8_to_units(int32_t v) { return (int)((v + (v >> 31 & 0xFF)) >> 8); }
int hero_x_units(const Hero *hero) { return hero ? fixed24_8_to_units(hero->x) : 0; }
int hero_y_units(const Hero *hero) { return hero ? fixed24_8_to_units(hero->y) : 0; }
void hero_set_x_units(Hero *hero, int units) { if (hero) hero->x = (int32_t)units << 8; }
void hero_set_y_units(Hero *hero, int units) { if (hero) hero->y = (int32_t)units << 8; }

void hero_allocate_slot(int serial)
{
    g_hero.slot_in_use = 1;
    g_hero.serial = serial;
}

int trophy_bag_absent(const Hero *hero) { return !hero || hero->trophy_bag_geo == 0; }

const WalkLeg *hero_walk_leg(const Hero *hero)
{
    static WalkLeg leg;              /* one view; mapview.c reads it immediately */
    if (!hero) return NULL;
    leg.x = hero->x; leg.y = hero->y;
    leg.target_x = hero->target_x; leg.target_y = hero->target_y;
    leg.speed = hero->walk_speed;
    leg.start_tick = hero->walk_start_tick;
    leg.duration = hero->walk_duration;
    leg.facing = hero->facing;
    return &leg;
}
