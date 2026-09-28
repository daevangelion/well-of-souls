/* Retail formats: docs/re/script.md sections 1 and 6, maps.md sections 1-5,
 * art.md section 2. Allocations happen only when loading, never when drawing. */
#include "world.h"
#include "../engine/fb.h"
#include "../engine/ini.h"
#include "../engine/text.h"
#include "../engine/rng.h"
#include "../platform/platform.h"
#include <ctype.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define QUEST_BYTES_MAX (32u * 1024u * 1024u)
#define QUEST_LINES_MAX 262144
#define INCLUDE_DEPTH_MAX 32
#define TABLE_TOKENS_MAX 64
#define MAP_PIXELS_MAX (64u * 1024u * 1024u)

World g_world;
static char data_root[512];
static Ini music_ini;
static char *music_text;

static void copy_string(char *dst, size_t cap, const char *src)
{
    size_t n;
    if (!src) { dst[0] = 0; return; }
    n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

static const char *join_path(char *buf, int size, const char *base, const char *rel)
{
    int n;
    if (!buf || size <= 0) return NULL;
    n = snprintf(buf, (size_t)size, "%s/%s", base, rel);
    if (n < 0 || n >= size) { buf[0] = 0; return NULL; }
    return buf;
}

const char *world_data_path(char *buf, int size, const char *rel)
{ return join_path(buf, size, data_root, rel); }
const char *world_path(char *buf, int size, const char *rel)
{ return join_path(buf, size, g_world.dir, rel); }

/* FUN_0047A0E4. Exact rules, in order:
 *   - characters <= 0x20 and ',' separate tokens, both inside and outside quotes;
 *   - '"' opens and closes a quoted run; an unterminated quote ends at end of line;
 *   - ';' and "//" are ORDINARY characters. Comments are whole-line only: FUN_004798A9 drops
 *     a line whose first non-blank character is ';' before any of this runs. The old
 *     "';' terminates even quoted text" rule (docs/re/script.md section 1.2) is not in the
 *     binary and truncated dialogue that legitimately contains ';' or "//".
 *   - the original's slots are 0x104 (260) bytes and it writes with NO length check, so a
 *     token longer than the slot overwrites the following ones. We return -1 instead, which
 *     is the same observable result for every retail row (no Evergreen token exceeds 255).
 * `max` is the caller's slot count; the original passes 32 for every table row. */
int world_tokenize(const char *line, char tokens[][256], int max)
{
    int count = 0;
    const unsigned char *p = (const unsigned char *)line;
    if (!line || !tokens || max < 0) return -1;
    while (*p) {
        int quoted = 0;
        size_t len = 0;
        while (*p && (*p <= ' ' || *p == ',')) ++p;
        if (!*p) break;
        if (count == max) return -1;
        if (*p == '"') { quoted = 1; ++p; }
        while (*p) {
            if (quoted) {
                if (*p == '"') { ++p; break; }
            } else {
                if (*p <= ' ' || *p == ',') break;
                if (*p == '"') { quoted = 1; ++p; continue; }
            }
            if (len == 255) return -1;
            tokens[count][len++] = (char)*p++;
        }
        tokens[count++][len] = 0;
        if (!quoted) {
            while (*p && (*p <= ' ' || *p == ',')) ++p;
            if (!*p) break;
        }
    }
    return count;
}

/* Decimal heads intentionally accept dotted suffixes (script.md section 6). */
static int number(const char *s)
{
    long v = strtol(s, NULL, 10);
    return v > INT_MAX ? INT_MAX : v < INT_MIN ? INT_MIN : (int)v;
}
static int dotted(const char *s, int part)
{
    while (part-- > 0) {
        s = strchr(s, '.');
        if (!s) return 0;
        ++s;
    }
    return number(s);
}

/* FUN_00479594: leading whitespace is skipped; a line starting with ';' or shorter than two
 * characters (terminator included) is dropped; an "#include " line splices the named file and
 * leaves "\r\n" behind. Comments are whole-line only - FUN_0047a0e4 keeps ';' as an ordinary
 * token character, so `SAY "a;b"` keeps its semicolon. */
static int arg(char t[][256], int n, int i) { return i < n ? number(t[i]) : 0; }

static int clamp(int value, int low, int high)
{ return value < low ? low : value > high ? high : value; }

static int dotarg(char t[][256], int n, int arg_index, int part)
{ return arg_index < n ? dotted(t[arg_index],part) : 0; }

static int append_line(char **text, size_t *size, size_t *capacity, const char *line)
{
    size_t n = strlen(line), needed;
    char *grown;
    if (*size >= QUEST_BYTES_MAX || n >= QUEST_BYTES_MAX - *size) return -1;
    needed = *size + n + 1;
    if (needed > *capacity) {
        size_t cap = *capacity ? *capacity : 16384;
        while (cap < needed) cap = cap > QUEST_BYTES_MAX / 2 ? QUEST_BYTES_MAX : cap * 2;
        grown = realloc(*text, cap);
        if (!grown) return -1;
        *text = grown; *capacity = cap;
    }
    memcpy(*text + *size, line, n + 1);
    *size = needed;
    return 0;
}

static int prefix_ci(const char *s, const char *prefix, size_t n);
/* FUN_00479594 matches _strnicmp(line, "#include ", 9). The trailing space is part of the
 * literal, so "#include<file>" is NOT an include and falls through to the normal line path. */
static int include_line(const char *line) { return prefix_ci(line, "#include ", 9); }
static int prefix_ci(const char *s, const char *prefix, size_t n)
{
    size_t i;
    for (i = 0; i < n; ++i) {
        int a = tolower((unsigned char)s[i]);
        if (!a || a != tolower((unsigned char)prefix[i])) return 0;
    }
    return 1;
}

/* The two world CRCs are taken over FUN_0047977a's raw buffer, which is NOT the line array:
 * fgets keeps the line terminator, so a kept line lands as "<trimmed>\r\n\0", and an #include
 * line is overwritten with the literal "\r\n" (0x4EBC50) so it lands as "\r\n\0" whatever its
 * own terminator was. FUN_004798a9 turns those control bytes into NULs only afterwards, so
 * the CRCs see the terminators. `raw` reproduces that stream; `text` is the clean line array. */
typedef struct { char *data; size_t size, capacity; } RawBuf;

static int raw_append(RawBuf *raw, const char *bytes, size_t n)
{
    char *grown;
    size_t needed;
    if (raw->size + n >= QUEST_BYTES_MAX) return -1;
    needed = raw->size + n;
    if (needed + 1 > raw->capacity) {
        size_t cap = raw->capacity ? raw->capacity : 16384;
        while (cap < needed + 1) cap = cap > QUEST_BYTES_MAX / 2 ? QUEST_BYTES_MAX : cap * 2;
        grown = realloc(raw->data, cap);
        if (!grown) return -1;
        raw->data = grown; raw->capacity = cap;
    }
    memcpy(raw->data + raw->size, bytes, n);
    raw->size = needed;
    raw->data[raw->size] = 0;
    return 0;
}

static int read_quest(const char *path, int depth, char **text, size_t *size, size_t *capacity,
                      RawBuf *raw)
{
    char *file, *line;
    const char *next;
    size_t file_size;
    int result = 0;
    if (depth == INCLUDE_DEPTH_MAX) return -1;
    file = text_read_file(path, &file_size);
    if (!file || file_size > QUEST_BYTES_MAX) { free(file); return -1; }
    next = file;
    while (next < file + file_size) {
        const char *term, *arg, *nl;
        char *p;
        size_t len, span;
        nl = memchr(next, '\n', (size_t)(file + file_size - next));
        term = (!nl) ? "" : (nl > next && nl[-1] == '\r') ? "\r\n" : "\n";
        line = (char *)next; /* the buffer is ours; the terminator is removed below */
        next = nl ? nl + 1 : file + file_size;
        /* fgets keeps the terminator, so terminate in place and remember it separately. */
        span = nl ? (size_t)((const char *)nl - line) : (size_t)(file + file_size - line);
        if (span && line[span-1] == '\r') --span;
        line[span] = 0;
        p = line;
        while (*p && (unsigned char)*p <= ' ') ++p;
        len = strlen(p) + strlen(term);
        if (len < 2 || *p == ';') continue;   /* the original's (1 < strlen) && line[0] != ';' */
        if (include_line(p)) {
            char child[1024];
            size_t base, k;
            char *q, *slash;
            arg = p + 8;
            while (*arg && (unsigned char)*arg <= ' ') ++arg;
            slash = strrchr(path, '/');
            base = slash ? (size_t)(slash - path + 1) : 0;
            if (!*arg || base + strlen(arg) >= sizeof(child)) { result = -1; break; }
            memcpy(child, path, base);
            k = strlen(arg);
            memcpy(child + base, arg, k + 1);
            /* The original truncates the copied tail at the LAST '"' of the line, then turns
             * every remaining control character into a NUL. */
            if ((q = strrchr(child + base, '"')) != NULL) *q = 0;
            for (q = child + base; *q; ++q) if ((unsigned char)*q < ' ') *q = 0;
            if (read_quest(child, depth + 1, text, size, capacity, raw)) { result = -1; break; }
            if (raw_append(raw, "\r\n", 3)) { result = -1; break; }
            if (append_line(text, size, capacity, "")) { result = -1; break; }
            continue;
        }
        /* A blank line still lands in the CRC stream ("\r\n\0"), but FUN_004798a9's splitter
         * never produces a line pointer for it, so the line array must not gain an entry. */
        if (raw_append(raw, p, strlen(p)) || raw_append(raw, term, strlen(term)) ||
            raw_append(raw, "", 1)) { result = -1; break; }
        if (!*p) continue;
        if (append_line(text, size, capacity, p)) { result = -1; break; }
    }
    free(file);
    return result;
}

static int parse_levels(char t[][256], int n, int *current)
{
    ClassDef *c;
    int i;
    if (isdigit((unsigned char)t[0][0])) {
        int key = number(t[0]), id = key / 100, level = key % 100;
        LevelDef *l;
        char *pipe;
        if (id >= WORLD_MAX_CLASSES || n < 5) return -1;
        c = &g_world.classes[id];
        l = &c->levels[level];
        l->used = 1; l->d_hp = arg(t,n,2); l->d_mp = arg(t,n,3);
        copy_string(l->name,sizeof(l->name),t[4]);
        pipe = strchr(l->name, '|'); if (pipe) *pipe = 0;
        if (!level) {
            c->used = 1; *current = id;
            copy_string(c->name,sizeof(c->name),l->name);
            c->magic_ratio = clamp(arg(t,n,5),0,100); c->right_hand = arg(t,n,6);
            c->hand_ratio = 100-c->magic_ratio;
            c->max_wallet = -1;
            for(i=0;i<5;++i) c->max_ability[i]=255;
            /* FUN_00483984 defaults, not the smaller example caps in levels.txt. */
            for(i=0;i<8;++i) c->max_element_pp[i]=c->max_hand_pp[i]=5000000;
        }
        return 0;
    }
    if (*current < 0) return -1;
    c = &g_world.classes[*current];
    if (!text_casecmp(t[0],"DESCRIPTION") && n > 1)
        copy_string(c->description,sizeof(c->description),t[1]);
    else if (!text_casecmp(t[0],"AUTO_MAX")) {
        if (n < 6) return -1;
        for (i=0;i<5;++i) c->auto_max[i]=number(t[i+1]);
        c->auto_max_set=1;
    } else if (!text_casecmp(t[0],"START_ABILITY")) {
        c->start_ability_set=1;
        for (i=0;i<5;++i) c->start_ability[i]=clamp(arg(t,n,i+1),0,255);
    } else if (!text_casecmp(t[0],"MAX_ABILITY")) {
        for(i=0;i<5;++i) c->max_ability[i]=clamp(arg(t,n,i+1),0,255);
    } else if (!text_casecmp(t[0],"START_ELEMENT_PP")) {
        for(i=0;i<8;++i) c->start_element_pp[i]=arg(t,n,i+1);
    } else if (!text_casecmp(t[0],"MAX_ELEMENT_PP")) {
        for(i=0;i<8;++i) c->max_element_pp[i]=arg(t,n,i+1);
    } else if (!text_casecmp(t[0],"START_HAND_PP")) {
        for(i=0;i<8;++i) c->start_hand_pp[i]=arg(t,n,i+1);
    } else if (!text_casecmp(t[0],"MAX_HAND_PP")) {
        for(i=0;i<8;++i) c->max_hand_pp[i]=arg(t,n,i+1);
    } else if (!text_casecmp(t[0],"START_LOCATION")) {
        if (n<3) return -1;
        c->start_map=arg(t,n,1); c->start_link=arg(t,n,2); c->start_drop_in=arg(t,n,3);
        c->start_location_set=1;
    } else if (!text_casecmp(t[0],"DEFAULT_SKIN")) {
        for (i=0;i<4 && i+1<n;++i) copy_string(c->default_skin[i],sizeof(c->default_skin[i]),t[i+1]);
    } else if (!text_casecmp(t[0],"START_ITEMS")) {
        c->start_item_count=0;
        for (i=1;i<n && i<=8;++i) c->start_items[c->start_item_count++]=clamp(number(t[i]),0,WORLD_MAX_ITEMS-1);
    } else if (!text_casecmp(t[0],"START_SPELLS")) {
        c->start_spell_count=0;
        for(i=1;i<n && i<=8;++i) c->start_spells[c->start_spell_count++]=clamp(number(t[i]),0,WORLD_MAX_SPELLS-1);
    } else if (!text_casecmp(t[0],"START_TOKENS")) {
        c->start_token_count=0;
        for(i=1;i<n && i<=8;++i) c->start_tokens[c->start_token_count++]=clamp(number(t[i]),0,4095);
    } else if (!text_casecmp(t[0],"MAGIC_RATIO")) c->magic_ratio=clamp(arg(t,n,1),0,100);
    else if (!text_casecmp(t[0],"HAND_RATIO")) c->hand_ratio=clamp(arg(t,n,1),0,100);
    else if (!text_casecmp(t[0],"NO_GIFTS")) c->no_gifts=1;
    else if (!text_casecmp(t[0],"HIDDEN_CLASS")) {
        c->hidden=1; c->hidden_start_level=clamp(arg(t,n,1),0,100);
    }
    else if (!text_casecmp(t[0],"MAX_WALLET")) {
        c->max_wallet=arg(t,n,1); if(c->max_wallet<1) c->max_wallet=-1;
    }
    return 0;
}

/* Section index order does not matter (each section is seen at most once); the names and
 * their set are FUN_004799b3's 16-entry table at 0x4FA858 (CREDITS, STORY, TOKENS, ITEMS,
 * SPELLS, MONSTERS, SCENES, GROUPS, LEVELS, TERRAINS, MAPS, ELEMENTS, HANDS, EQUIP,
 * THEMES, TROPHIES). */
enum { SEC_MAPS, SEC_TERRAINS, SEC_MONSTERS, SEC_GROUPS, SEC_LEVELS, SEC_ITEMS,
       SEC_SPELLS, SEC_SCENES, SEC_CREDITS, SEC_STORY, SEC_TOKENS, SEC_ELEMENTS,
       SEC_HANDS, SEC_EQUIP, SEC_THEMES, SEC_TROPHIES, SEC_COUNT };
static const char *const section_names[SEC_COUNT] = {
    "MAPS","TERRAINS","MONSTERS","GROUPS","LEVELS","ITEMS","SPELLS","SCENES",
    "CREDITS","STORY","TOKENS","ELEMENTS","HANDS","EQUIP","THEMES","TROPHIES"
};

/* FUN_004817de: a +TOKENS row whose first token is "chapter" is a diary page, not a token.
 * Needs at least five tokens and stops at 100 chapters; the values land in
 * {a, b, c, title, link, text} in that order. */
static int parse_chapter(char t[][256], int n)
{
    ChapterDef *ch;
    if (n < 5 || g_world.chapter_count >= WORLD_MAX_CHAPTERS) return -1;
    ch = &g_world.chapters[g_world.chapter_count];
    ch->used = 1;
    ch->a = number(t[1]); ch->b = number(t[2]); ch->c = number(t[3]);
    copy_string(ch->title, sizeof ch->title, t[4]);
    if (n > 5) copy_string(ch->link, sizeof ch->link, t[5]);
    if (n > 6) copy_string(ch->text, sizeof ch->text, t[6]);
    g_world.chapter_count++;
    return 0;
}

/* FUN_004814d2 / FUN_00481654: "n,text" for a token id 0..4095, text truncated to 128 chars. */
static int set_token(char t[][256], int n)
{
    TokenDef *v;
    int id;
    if (n < 2) return -1;
    if (!isdigit((unsigned char)t[0][0])) return -1;
    id = number(t[0]);
    if (id < 0 || id >= WORLD_MAX_TOKENS) return -1;
    v = &g_world.tokens[id];
    if (v->used) return -1;                    /* duplicate token id: the original errors out */
    v->used = 1;
    copy_string(v->text, sizeof v->text, t[1]);
    return 0;
}

static int parse_row(int section, char t[][256], int n, int *current_class)
{
    int id, i;
    if (section==SEC_LEVELS) return parse_levels(t,n,current_class);
    if (section==SEC_TOKENS) {
        /* FUN_004814d2 tries the "chapter" form first, then the token form. */
        if (!text_casecmp(t[0],"chapter")) return parse_chapter(t,n);
        return set_token(t,n);
    }
    if (section==SEC_EQUIP) {
        /* FUN_004824b4: id 0..13, name truncated to 39 chars, no duplicate check. Ids 2..9 are
         * the eight hand classes and are never named here - FUN_00482431 forwards them to
         * FUN_004825bf, which is what world_equip_name() does. */
        if (n<2 || !isdigit((unsigned char)t[0][0])) return -1;
        id=number(t[0]);
        if (id<0 || id>=WORLD_MAX_EQUIP) return -1;
        copy_string(g_world.equip_names[id],sizeof(g_world.equip_names[id]),t[1]);
        return 0;
    }
    if (!isdigit((unsigned char)t[0][0])) return -1;
    id=number(t[0]);
    switch(section) {
    case SEC_MAPS: {
        MapDef *m;
        /* FUN_00482c54 wants four tokens (id, image, root, name); the flag word and the theme
         * are only read when the row actually has them. */
        if(id>=WORLD_MAX_MAPS || n<4) return -1;
        m=&g_world.maps[id]; m->used=1;
        copy_string(m->image,sizeof(m->image),t[1]);
        copy_string(m->root,sizeof(m->root),t[2]);
        copy_string(m->name,sizeof(m->name),t[3]);
        if(n>4) m->flags=(uint32_t)strtoul(t[4],NULL,0);
        m->theme=arg(t,n,5);
        break;
    }
    case SEC_TERRAINS: {
        TerrainDef *v;
        if(id>=WORLD_MAX_TERRAINS || n<3) return -1;
        v=&g_world.terrains[id]; v->used=1;
        copy_string(v->name,sizeof(v->name),t[1]);
        v->damage=arg(t,n,2); v->token=arg(t,n,3);
        break;
    }
    case SEC_MONSTERS: {
        MonsterDef *m;
        if(id>=WORLD_MAX_MONSTERS || n<12) return -1;
        m=&g_world.monsters[id]; m->used=1;
        copy_string(m->name,sizeof(m->name),t[1]); copy_string(m->skin,sizeof(m->skin),t[2]);
        m->scale=dotted(t[3],0); m->flags=dotted(t[3],1); m->element=arg(t,n,4);
        m->hp=arg(t,n,5); m->mp=arg(t,n,6); m->defense=arg(t,n,7); m->offense=arg(t,n,8);
        m->exp=arg(t,n,9); m->gold=arg(t,n,10); m->level=arg(t,n,11);
        m->strength=arg(t,n,12); m->stamina=arg(t,n,13); m->agility=arg(t,n,14);
        m->dexterity=arg(t,n,15); m->wisdom=arg(t,n,16);
        if(n>17) copy_string(m->growl_wav,sizeof(m->growl_wav),t[17]);
        if(n>18) copy_string(m->pain_wav,sizeof(m->pain_wav),t[18]);
        m->attack_path=arg(t,n,19);
        /* Arg20 is an AI command, not a spell ID: FUN_004809a3. */
        if(n>20) copy_string(m->ai,sizeof(m->ai),t[20]);
        break;
    }
    case SEC_GROUPS: {
        GroupDef *g;
        if(id>=WORLD_MAX_GROUPS) return -1;
        g=&g_world.groups[id]; g->used=1; g->count=0;
        for(i=1;i<n && i<=GROUP_MAX_MEMBERS;++i) g->members[g->count++]=number(t[i]);
        break;
    }
    case SEC_ITEMS: {
        ItemDef *v;
        if(id>=WORLD_MAX_ITEMS || n<15) return -1;
        v=&g_world.items[id]; v->used=1; copy_string(v->name,sizeof(v->name),t[1]);
        v->klass=arg(t,n,2); v->image=dotted(t[3],0);
        v->image_ext=t[3][0]=='+'?1:dotted(t[3],1);
        v->gp=arg(t,n,4); v->level=arg(t,n,5);
        v->equip_token=clamp(dotted(t[5],1),0,INT_MAX);
        v->flags=dotted(t[5],2); v->max_count=dotted(t[5],3);
        v->trophy_needed=dotted(t[5],4); v->trophy_made=dotted(t[5],5);
        /* FUN_00482fc1: <2 becomes ONE, not two as the RE prose claimed. */
        v->trophy_count_needed=clamp(dotted(t[5],6),1,INT_MAX);
        v->trophy_count_made=clamp(dotted(t[5],7),1,INT_MAX);
        v->spell_binding=arg(t,n,6); v->movement=dotted(t[6],1);
        if(v->klass==20 && !v->movement) v->movement=v->spell_binding;
        v->element=arg(t,n,7)&255;
        v->defense=clamp(arg(t,n,8),INT_MIN,255); v->attack=clamp(arg(t,n,9),INT_MIN,255);
        v->hp=arg(t,n,10); v->mp=arg(t,n,11);
        /* The 20 ceiling applies to arg12 (ability), NOT arg13 (find probability). */
        v->ability_points=clamp(arg(t,n,12),INT_MIN,20);
        v->find_probability=arg(t,n,13); v->find_monster=dotted(t[13],1);
        copy_string(v->description,sizeof(v->description),t[14]);
        if(n>15) copy_string(v->sound,sizeof(v->sound),t[15]);
        v->attack_path=clamp(arg(t,n,16),-1,63);
        v->attack_image=clamp(dotarg(t,n,16,1),0,199);
        v->attack_flags=clamp(dotarg(t,n,16,2),0,63);
        v->attack_weather=clamp(dotarg(t,n,16,3),0,63);
        v->attack_effect=clamp(dotarg(t,n,16,4),0,63);
        if(v->klass==4) {
            v->travel_mode=dotted(t[2],1); v->travel_map_scene=dotted(t[2],2);
            v->travel_link=dotted(t[2],3); v->travel_drop_in=dotted(t[2],4);
            v->travel_scene=dotted(t[2],5);
        }
        break;
    }
    case SEC_SPELLS: {
        SpellDef *v;
        int affinity, packed;
        if(id>=WORLD_MAX_SPELLS || n<7) return -1;
        /* FUN_0047fced 0x47FE06: spell 0 is the algorithm-tweak template and is never marked
         * used, so the post-pass skips it. Every other field is still copied. */
        v=&g_world.spells[id]; if(id) v->used=1; copy_string(v->name,sizeof(v->name),t[1]);
        v->pp_cost=arg(t,n,2); v->element=arg(t,n,3); v->damage=arg(t,n,4);
        v->summon_id=dotted(t[4],1); v->mp_cost=arg(t,n,5);
        affinity=arg(t,n,6); v->req_affinity=affinity%100; v->all_targets=affinity>99;
        v->flags=dotted(t[6],1); v->min_level=dotted(t[6],2); v->token=dotted(t[6],3);
        v->trophy_needed=dotted(t[6],4); v->trophy_made=dotted(t[6],5);
        v->trophy_count_needed=clamp(dotted(t[6],6),1,INT_MAX);
        v->trophy_count_made=clamp(dotted(t[6],7),1,INT_MAX);
        v->path=arg(t,n,7); v->effects_row=arg(t,n,8);
        v->max_cols=n>9?arg(t,n,9):16; v->max_fx=n>10?arg(t,n,10):32;
        if(v->max_fx<0 || v->max_fx>1023) v->max_fx=1023;
        v->ms_per_col=n>11?arg(t,n,11):100;
        packed=arg(t,n,12); v->gravity=packed%1000;
        v->effects=(packed%1000000)/1000; v->weather=(packed%1000000000)/1000000;
        /* FUN_0047fced 0x4800F2/0x480108: the dotted part 1 lands in the record slot the
         * packed head fills with the AAA (weather) group and part 2 in the BBB (effects) one,
         * i.e. the two dotted overrides are swapped with respect to spells.txt's
         * "gravity.effects.weather" wording. Reproduced as the original computes it. */
        if(dotarg(t,n,12,1)>0) v->weather=dotarg(t,n,12,1);
        if(dotarg(t,n,12,2)>0) v->effects=dotarg(t,n,12,2);
        v->loop=arg(t,n,13);
        if(n>14) copy_string(v->sfx_summon,sizeof(v->sfx_summon),t[14]);
        if(n>15) copy_string(v->sfx_travel,sizeof(v->sfx_travel),t[15]);
        if(n>16) copy_string(v->sfx_strike,sizeof(v->sfx_strike),t[16]);
        v->extra[0]=dotarg(t,n,17,0); v->extra[1]=dotarg(t,n,17,1);
        break;
    }
    case SEC_ELEMENTS: {
        ElementDef *v;
        if(id>=WORLD_MAX_ELEMENTS || n<2) return -1;
        v=&g_world.elements[id]; v->used=1;
        copy_string(v->name,sizeof(v->name),t[1]); break;
    }
    case SEC_HANDS: {
        HandDef *v;
        if(id>=WORLD_MAX_HANDS || n<2) return -1;
        v=&g_world.hands[id]; v->used=1;
        copy_string(v->name,sizeof(v->name),t[1]);
        v->strength_percent=n>2?clamp(arg(t,n,2),0,100):50;
        v->damage_weight=v->strength_percent*40/100+10;
        if(n>3) copy_string(v->sound,sizeof(v->sound),t[3]);
        break;
    }
    case SEC_TROPHIES: {
        TrophyDef *v;
        const char *part;
        if(id>=WORLD_MAX_TROPHIES || n<8) return -1;
        v=&g_world.trophies[id]; v->used=1;
        copy_string(v->name,sizeof(v->name),t[1]); copy_string(v->image,sizeof(v->image),t[2]);
        v->image_index=arg(t,n,3); v->stack_size=clamp(arg(t,n,4),0,99); v->gp=arg(t,n,5);
        v->probability=arg(t,n,7); v->token=arg(t,n,8); v->flags=arg(t,n,9);
        part=t[6];
        while(*part && v->monster_range_count<TROPHY_MAX_RANGES) {
            const char *end=strchr(part,'.'), *dash=strchr(part,'-');
            int r=v->monster_range_count++;
            v->monster_first[r]=number(part);
            v->monster_last[r]=dash && (!end || dash<end)?number(dash+1):v->monster_first[r];
            if(!end) break;
            part=end+1;
        }
        break;
    }
    default: break;
    }
    return 0;
}

/* FUN_004798a9 splits the buffer into line pointers once; FUN_00479a02 pairs the +/- markers
 * and each table parser then walks its own line range. +STORY and +CREDITS have no table
 * parser: FUN_004861c8 concatenates the CREDITS lines and the front-end scroller walks the
 * STORY lines, so we only record the pointers (they point into g_world.text). */
static int push_line(WorldText *out, const char *line)
{
    const char **grown = realloc(out->lines, (size_t)(out->count + 1) * sizeof(*grown));
    if (!grown) return -1;
    out->lines = grown;
    out->lines[out->count++] = line;
    return 0;
}

static int parse_world(void)
{
    int line, active=-1, seen[SEC_COUNT]={0}, current_class=-1, scene=-1;
    char t[TABLE_TOKENS_MAX][256];
    for(line=0;line<g_world.line_count;++line) {
        const char *p=g_world.lines[line];
        int n, section;
        if(*p=='+' || *p=='-') {
            n=world_tokenize(p,t,TABLE_TOKENS_MAX);
            if(n<1) return -1;
            for(section=0;section<SEC_COUNT;++section)
                if(!text_casecmp(t[0]+1,section_names[section])) break;
            if(section==SEC_COUNT) return -1;
            if(*p=='+') {
                if(active!=-1 || seen[section]) return -1;
                active=section; seen[section]=1;
            } else {
                if(active!=section) return -1;
                if(active==SEC_SCENES && scene>=0) g_world.scenes[scene].end_line=line;
                active=-1;
            }
            continue;
        }
        if(active==SEC_STORY) { if(push_line(&g_world.story,p)) return -1; continue; }
        if(active==SEC_CREDITS) { if(push_line(&g_world.credits,p)) return -1; continue; }
        if(active<0 || (active>SEC_SCENES && active!=SEC_TOKENS && active!=SEC_ELEMENTS &&
           active!=SEC_HANDS && active!=SEC_EQUIP && active!=SEC_TROPHIES)) continue;
        if(active==SEC_SCENES) {
            /* Dialogue may exceed a token: only SCENE and TOKEN declarations need lexing
             * (FUN_00481654). */
            if(strlen(p)<5 || toupper((unsigned char)p[0])!='S' ||
               toupper((unsigned char)p[1])!='C' || toupper((unsigned char)p[2])!='E' ||
               toupper((unsigned char)p[3])!='N' || toupper((unsigned char)p[4])!='E' ||
               (p[5] && p[5]!=' ' && p[5]!='\t' && p[5]!=',')) {
                /* "TOKEN <id> <text>": the keyword is t[0], the row starts at t[1]. */
                if(!prefix_ci(p,"TOKEN",5)) continue;
                n=world_tokenize(p,t,TABLE_TOKENS_MAX);
                if(n<3 || set_token(t+1,n-1)) {
                    fprintf(stderr,"world: invalid TOKEN row at expanded line %d: %.80s\n",line,p);
                    return -1;
                }
                continue;
            }
            n=world_tokenize(p,t,TABLE_TOKENS_MAX);
            if(n<2) return -1;
            if(scene>=0) g_world.scenes[scene].end_line=line;
            scene=number(t[1]);
            if(scene<0 || scene>=WORLD_MAX_SCENES || g_world.scenes[scene].used) return -1;
            g_world.scenes[scene].used=1; g_world.scenes[scene].first_line=line;
        } else {
            n=world_tokenize(p,t,TABLE_TOKENS_MAX);
            if(n<0 || (n && parse_row(active,t,n,&current_class))) {
                fprintf(stderr,"world: invalid %s row at expanded line %d: %.80s\n",section_names[active],line,p);
                return -1;
            }
        }
    }
    if(active!=-1) return -1;
    return 0;
}

void world_free(void)
{
    free(g_world.lines); free(g_world.text);
    free((void *)g_world.story.lines); free((void *)g_world.credits.lines);
    free(music_text); music_text=NULL; memset(&music_ini,0,sizeof(music_ini));
    memset(&g_world,0,sizeof(g_world)); data_root[0]=0;
}

const char *world_music(const char *root, const char *key)
{
    const char *fallback;
    if(!key) return "";
    fallback=ini_get(&music_ini,"common",key,"");
    return root && *root?ini_get(&music_ini,root,key,fallback):fallback;
}

int world_music_count(const char *root)
{
    return clamp(number(world_music(root,"numMidi")),0,INI_MAX_ENTRIES);
}

/* The x86 integer loop at 0x480274 is absent from Ghidra's pseudocode. */
static int scaled(int value, int percent, int divisor)
{
    int64_t result=(int64_t)value*percent/divisor;
    return result>INT_MAX?INT_MAX:result<INT_MIN?INT_MIN:(int)result;
}

static int spell_auto(int value, int percent)
{
    if(percent>=1 && percent<=500) value=scaled(value,percent,100);
    return value<1?1:value;
}

/* FUN_0047fced 0x480255..0x4803F1, the spell post-pass. Three accumulators seeded 200/5000/3000
 * are grown once per required affinity level (x145/x150/x130) and, for an all-targets spell,
 * multiplied by 120/200 before the costs are derived. The pass also draws ONE rand() per used
 * spell into record +0x154 (0x4803E9, rand_calls.md line 93973) while that field is 0. */
static void complete_spells(void)
{
    int id;
    const SpellDef *zero=&g_world.spells[0];
    for(id=1;id<WORLD_MAX_SPELLS;++id) {
        SpellDef *s=&g_world.spells[id];
        int mp=200, pp=5000, damage=3000, level;
        if(!s->used) continue;
        for(level=0;level<s->req_affinity;++level) {
            mp=scaled(mp,145,100); pp=scaled(pp,150,100); damage=scaled(damage,130,100);
        }
        if(s->all_targets) { mp=scaled(mp,120,100); pp=scaled(pp,200,100); }
        if(!s->mp_cost) s->mp_cost=spell_auto(mp/100,zero->mp_cost);
        if(!s->pp_cost) s->pp_cost=spell_auto(pp/20,zero->pp_cost);
        if(!s->damage && s->element) s->damage=spell_auto(damage/100,zero->damage);
        if(!s->effect_seed) s->effect_seed=crt_rand();
    }
}

/* FUN_004823c4: the built-in +EQUIP names, installed before the section is read so a world
 * that renames only some slots keeps the rest. Slots 2..9 are the hand classes, not their own
 * defaults (FUN_00482431 forwards them to FUN_004825bf). */
static void default_equip_names(void)
{
    copy_string(g_world.equip_names[0], sizeof g_world.equip_names[0], "Helmet");
    copy_string(g_world.equip_names[1], sizeof g_world.equip_names[1], "Armor");
    copy_string(g_world.equip_names[10], sizeof g_world.equip_names[10], "Boots");
    copy_string(g_world.equip_names[11], sizeof g_world.equip_names[11], "Shield");
    copy_string(g_world.equip_names[12], sizeof g_world.equip_names[12], "Ring");
    copy_string(g_world.equip_names[13], sizeof g_world.equip_names[13], "Amulet");
}

/* FUN_0048219a: only installed when +ELEMENTS defined no rows at all. */
static void default_element_names(void)
{
    static const char *const names[8]={"Life","Water","Nature","Earth","Death","Fire","Spirit","Air"};
    int i;
    for(i=0;i<8;++i) {
        g_world.elements[i].used=1;
        copy_string(g_world.elements[i].name,sizeof g_world.elements[i].name,names[i]);
    }
}

/* FUN_004825df: the 4th..8th classes have no retail name and are RH5..RH8. */
static void default_hand_names(void)
{
    static const char *const names[8]={"Sword","Staff","Bow","Music","Fist","Dart","Book","Spirit"};
    int i;
    for(i=0;i<8;++i) {
        g_world.hands[i].used=1;
        copy_string(g_world.hands[i].name,sizeof g_world.hands[i].name,names[i]);
    }
}

/* FUN_0047983e: seed 0x075BCD15, then XOR the quest.txt buffer one dword at a time
 * (little-endian; the loop count is the byte length divided by four, rounded up). */
static uint32_t quest_crc2(const char *text, size_t size)
{
    uint32_t crc=0x075bcd15u;
    size_t words=(size+3)/4, i;
    for(i=0;i<words;++i) {
        uint32_t w=(uint32_t)(unsigned char)text[i*4];
        if(i*4+1<size) w|=(uint32_t)(unsigned char)text[i*4+1]<<8;
        if(i*4+2<size) w|=(uint32_t)(unsigned char)text[i*4+2]<<16;
        if(i*4+3<size) w|=(uint32_t)(unsigned char)text[i*4+3]<<24;
        crc^=w;
    }
    return crc;
}

/* FUN_0047977a: seed 0x175A3E2D, then for every byte of the #include-expanded quest.txt
 * rotate left by one (the old bit31 is folded into bit 0) and XOR the byte in. */
static uint32_t quest_crc1(const char *text, size_t size)
{
    uint32_t crc=0x175a3e2du;
    size_t i;
    for(i=0;i<size;++i) {
        uint32_t old=crc;
        crc<<=1;
        if(old&0x80000000u) crc|=1u;   /* FUN_0047977a: the bit shifted out of 31 folds back in */
        crc^=(unsigned char)text[i];
    }
    return crc;
}
/* FUN_00437b15: a plain additive sum of every byte; a missing file contributes 0. */
static uint32_t file_byte_sum(const char *path)
{
    unsigned char buffer[8192];
    uint32_t sum=0;
    size_t got;
    FILE *f=plat_fopen(path,"rb");
    if(!f) return 0;
    while((got=fread(buffer,1,sizeof buffer,f))>0) {
        size_t i;
        for(i=0;i<got;++i) sum+=buffer[i];
    }
    fclose(f);
    return sum;
}

/* FUN_00479a02 0x479E0F..0x479F22: the per-file byte sums XORed into CRC-1. Every map row's
 * .obl/.mon/.ter, then slots.ini, config.ini, gender.ini and missions.ini (the three INIs only
 * when the world is "Evergreen" or the online build number reaches 0xA86/0xA91, which offline
 * retail always is), then devTable00..99.txt when present. */
static uint32_t world_file_sum(void)
{
    char path[1024];
    uint32_t sum=0;
    int i, n;
    for(i=0;i<WORLD_MAX_MAPS;++i) {
        const MapDef *m=&g_world.maps[i];
        static const char *const exts[3]={".obl",".mon",".ter"};
        int e;
        if(!m->used || !*m->root) continue;
        for(e=0;e<3;++e) {
            n=snprintf(path,sizeof(path),"%s/maps/%s%s",g_world.dir,m->root,exts[e]);
            if(n>0 && (size_t)n<sizeof(path)) sum+=file_byte_sum(path);
        }
    }
    if(world_path(path,sizeof(path),"slots.ini")) sum+=file_byte_sum(path);
    if(world_path(path,sizeof(path),"config.ini")) sum+=file_byte_sum(path);
    if(world_path(path,sizeof(path),"gender.ini")) sum+=file_byte_sum(path);
    if(world_path(path,sizeof(path),"missions.ini")) sum+=file_byte_sum(path);
    for(i=0;i<100;++i) {
        if(snprintf(path,sizeof(path),"%s/devTable%02d.txt",g_world.dir,i)<(int)sizeof(path))
            sum+=file_byte_sum(path);
    }
    return sum;
}

/* FUN_004861c8: the +CREDITS lines joined with "\r\n", stopping five bytes short of the limit. */
static void build_credits(void)
{
    size_t used=0;
    int i;
    g_world.credits_text[0]=0;
    for(i=0;i<g_world.credits.count;++i) {
        const char *line=g_world.credits.lines[i];
        size_t n=line?strlen(line):0;
        if(!n) continue;
        if(used+n+2>WORLD_CREDITS_MAX-5) break;
        memcpy(g_world.credits_text+used,line,n); used+=n;
        g_world.credits_text[used++]='\r';
        g_world.credits_text[used++]='\n';
    }
    g_world.credits_text[used]=0;
}

/* GetPrivateProfileString (FUN_0047C5C5) semantics: only a ';' at the start of a line is a
 * comment, the key is everything before the first '=', and both sides are trimmed. A "//" in a
 * value is ordinary text, so config.ini cannot go through the quest INI lexer, which treats "//"
 * as a comment and would truncate http://... to "http:". One pass fills the whole table because
 * the scan destroys the separators in place. */
#define CONFIG_KEYS 16
static void read_profile(char *text, const char *section, const char *const *keys,
                         const char **values)
{
    char *cursor = text, *line;
    int i, inside = 0;
    for (i = 0; i < CONFIG_KEYS; ++i) values[i] = NULL;
    while ((line = text_next_line(&cursor)) != NULL) {
        char *p, *eq, *key, *value;
        while (*line && (unsigned char)*line <= ' ') ++line;
        if (!*line || *line == ';') continue;
        if (*line == '[') {
            p = strchr(line, ']');
            if (!p) continue;
            *p = 0;
            inside = text_casecmp(text_trim(line + 1), section) == 0;
            continue;
        }
        if (!inside) continue;
        eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        key = text_trim(line);
        value = text_trim(eq + 1);
        for (i = 0; i < CONFIG_KEYS; ++i)
            if (!text_casecmp(key, keys[i])) values[i] = value;
    }
}

/* FUN_0041e421 0x41E8A5..0x41EB3D, one FUN_0047c5c5 call per key. Every numeric key is
 * passed through atoi with no range check, and the DEFAULT is the string the call site pushes,
 * which is not always the InitInstance pre-init value at 0x41E300. */
static void read_config(void)
{
    static const char *const keys[CONFIG_KEYS]={
        "goldName","pkHandPercent","pkMagicPercent","spellSuccessPercent",
        "karmaPointsAreAlsoWarPoints","pkTrophy","monsterXPAreAlsoWarPoints",
        "tacticsWinGivesWarPoints","noGivingGP","cookieProtection",
        "petsCanBitePeople","maxUnspentPP","maxPKAttackAdvantage","startingGP",
        "worldHomeUrl","tacticsSourceUrl"
    };
    /* The DEFAULT column is the FIRST argument pushed at each FUN_0047C5C5 call site, read off
     * 0x41E8A5..0x41EB3D. It is sometimes an immediate and sometimes `push %ebx`, and %ebx only
     * changes twice in the whole run: `mov ebx,0x4e246c` ("100") at 0x41E8B1, then
     * `mov ebx,0x4dcaf4` ("0") at 0x41E93A. So spellSuccessPercent, pushed at 0x41E922, still
     * gets "100" - the 0x41E93A reload serves karmaPointsAreAlsoWarPoints onwards. Verified by
     * walking the push sequence, not by reading the nearest mov. */
    static const char *const fallback[CONFIG_KEYS]={
        "GP","100","100","100","0","0","0","0","0","0","1","1000000","80","1000","",""
    };
    const char *value[CONFIG_KEYS];
    char path[1024], *text;
    int i;
    for(i=0;i<CONFIG_KEYS;++i) value[i]=fallback[i];
    if(!world_path(path,sizeof(path),"config.ini")) return;
    text=text_read_file(path,NULL);
    if(!text) return;
    read_profile(text,"General",keys,value);
    for(i=0;i<CONFIG_KEYS;++i) if(!value[i]) value[i]=fallback[i];
    copy_string(g_world.gold_name,sizeof g_world.gold_name,value[0]);
    g_world.pk_hand_percent=number(value[1]);
    g_world.pk_magic_percent=number(value[2]);
    g_world.spell_success_percent=number(value[3]);
    g_world.karma_points_are_also_war_points=number(value[4]);
    g_world.pk_trophy=number(value[5]);
    g_world.monster_xp_are_also_war_points=number(value[6]);
    g_world.tactics_win_gives_war_points=number(value[7]);
    g_world.no_giving_gp=number(value[8]);
    g_world.cookie_protection=number(value[9]);
    g_world.pets_can_bite_people=number(value[10]);
    g_world.max_unspent_pp=number(value[11]);
    g_world.max_pk_attack_advantage=number(value[12]);
    g_world.starting_gp=number(value[13]);
    copy_string(g_world.world_home_url,sizeof g_world.world_home_url,value[14]);
    copy_string(g_world.tactics_source_url,sizeof g_world.tactics_source_url,value[15]);
    free(text);
}

int world_load(const char *data_dir, const char *name)
{
    char path[1024], *p;
    size_t size=0, capacity=0, offset;
    int count=0, n, i, elements=0, hands=0;
    RawBuf raw={0};
    world_free();
    if(!data_dir || !name || strlen(data_dir)>=sizeof(data_root) || strlen(name)>=sizeof(g_world.name)) return -1;
    copy_string(data_root,sizeof(data_root),data_dir); copy_string(g_world.name,sizeof(g_world.name),name);
    n=snprintf(g_world.dir,sizeof(g_world.dir),"%s/worlds/%s",data_dir,name);
    if(n<0 || (size_t)n>=sizeof(g_world.dir)) goto fail;
    if(!world_path(path,sizeof(path),"quest.txt") ||
       read_quest(path,0,&g_world.text,&size,&capacity,&raw)) goto fail;
    for(offset=0;offset<size;offset+=strlen(g_world.text+offset)+1)
        if(++count>QUEST_LINES_MAX) goto fail;
    if(!count) goto fail;
    g_world.lines=malloc((size_t)count*sizeof(*g_world.lines));
    if(!g_world.lines) goto fail;
    g_world.line_count=count; p=g_world.text;
    for(n=0;n<count;++n) { g_world.lines[n]=p; p+=strlen(p)+1; }
    default_equip_names();
    if(parse_world()) goto fail;
    for(i=0;i<WORLD_MAX_ELEMENTS;++i) elements+=g_world.elements[i].used!=0;
    for(i=0;i<WORLD_MAX_HANDS;++i) hands+=g_world.hands[i].used!=0;
    if(!elements) default_element_names();
    if(!hands) default_hand_names();
    complete_spells();
    read_config();
    build_credits();
    g_world.crc1=quest_crc1(raw.data,raw.size);
    g_world.crc2=quest_crc2(raw.data,raw.size);
    g_world.crc1^=world_file_sum();
    free(raw.data);
    if(!world_path(path,sizeof(path),"music.ini")) goto fail;
    music_text=text_read_file(path,NULL);
    if(music_text && ini_parse(&music_ini,music_text)) goto fail;
    return 0;
fail:
    fprintf(stderr,"world: cannot load %s\n",name);
    world_free(); return -1;
}

/* FUN_004814a7: an unused or out-of-range id answers the shared empty string, not NULL. */
const char *world_token_text(int id)
{
    if(id<0 || id>=WORLD_MAX_TOKENS || !g_world.tokens[id].used) return "";
    return g_world.tokens[id].text;
}

const ChapterDef *world_chapter(int index)
{
    if(index<0 || index>=g_world.chapter_count || !g_world.chapters[index].used) return NULL;
    return &g_world.chapters[index];
}

/* FUN_004825bf: a bad hand id answers "Right-Hand". */
const char *world_hand_name(int hand)
{
    if(hand<0 || hand>=WORLD_MAX_HANDS || !g_world.hands[hand].used) return "Right-Hand";
    return g_world.hands[hand].name;
}

/* FUN_00482431: 0/1 are the +EQUIP table itself, 2..9 forward to the hand classes, 10..13 are
 * the remaining +EQUIP slots, and anything else answers "Right-Hand". */
const char *world_equip_name(int slot)
{
    if(slot>=2 && slot<=9) return world_hand_name(slot-2);
    if(slot<0 || slot>=WORLD_MAX_EQUIP) return "Right-Hand";
    return g_world.equip_names[slot];
}

const char *world_hero_slot_name(int hero_slot, int hand)
{
    switch(hero_slot) {
    case 0: return world_equip_name(0);   /* HERO_SLOT_HELMET  */
    case 1: return world_equip_name(1);   /* HERO_SLOT_ARMOR   */
    case 2: return world_equip_name(10);  /* HERO_SLOT_BOOTS   */
    case 3: return world_equip_name(11);  /* HERO_SLOT_SHIELD  */
    case 4: return world_equip_name(12);  /* HERO_SLOT_RING    */
    case 5: return world_equip_name(13);  /* HERO_SLOT_AMULET  */
    case 8: return world_hand_name(hand); /* HERO_SLOT_RIGHT_HAND */
    default: return "";
    }
}

/* The original only ever walks index -> name (FUN_00482431); this is the reverse for UIs that
 * have to match a localised label. Hand slots are named by the hand class. */
int world_equip_slot_by_name(const char *name)
{
    int slot, hand;
    if(!name || !*name) return -1;
    for(slot=0;slot<WORLD_MAX_EQUIP;++slot) {
        if(slot>=2 && slot<=9) continue;
        if(!text_casecmp(name,g_world.equip_names[slot])) return slot;
    }
    for(hand=0;hand<WORLD_MAX_HANDS;++hand)
        if(!text_casecmp(name,g_world.hands[hand].name)) return hand+2;
    return -1;
}

const char *world_story_line(int index)
{
    if(index<0 || index>=g_world.story.count) return NULL;
    return g_world.story.lines[index];
}

int world_story_count(void) { return g_world.story.count; }
const char *world_credits_text(void) { return g_world.credits_text; }
const char *world_gold_name(void) { return g_world.gold_name[0]?g_world.gold_name:"GP"; }

void world_dump(void (*emit)(const char *key, const char *value, void *user), void *user)
{
    char value[64];
    int i, used=0;
#define WORLD_DUMP_INT(key,field) \
    do { snprintf(value,sizeof value,"%ld",(long)(field)); emit((key),value,user); } while (0)
    WORLD_DUMP_INT("world.crc1",(long)g_world.crc1);
    WORLD_DUMP_INT("world.crc2",(long)g_world.crc2);
    WORLD_DUMP_INT("world.line_count",g_world.line_count);
    WORLD_DUMP_INT("world.starting_gp",g_world.starting_gp);
    WORLD_DUMP_INT("world.max_unspent_pp",g_world.max_unspent_pp);
    WORLD_DUMP_INT("world.spell_success_percent",g_world.spell_success_percent);
    WORLD_DUMP_INT("world.pk_hand_percent",g_world.pk_hand_percent);
    WORLD_DUMP_INT("world.pk_magic_percent",g_world.pk_magic_percent);
    WORLD_DUMP_INT("world.cookie_protection",g_world.cookie_protection);
    WORLD_DUMP_INT("world.max_pk_attack_advantage",g_world.max_pk_attack_advantage);
    WORLD_DUMP_INT("world.no_giving_gp",g_world.no_giving_gp);
    WORLD_DUMP_INT("world.pets_can_bite_people",g_world.pets_can_bite_people);
    emit("world.gold_name",world_gold_name(),user);
    emit("world.world_home_url",g_world.world_home_url,user);
    emit("world.tactics_source_url",g_world.tactics_source_url,user);
    for(i=0;i<WORLD_MAX_TOKENS;++i) used+=g_world.tokens[i].used!=0;
    WORLD_DUMP_INT("world.tokens.defined",(long)used);
    WORLD_DUMP_INT("world.chapters",g_world.chapter_count);
    WORLD_DUMP_INT("world.story_lines",g_world.story.count);
    WORLD_DUMP_INT("world.credits_bytes",(long)strlen(g_world.credits_text));
    for(i=0;i<WORLD_MAX_EQUIP;++i) {
        char key[32];
        if(i>=2 && i<=9) continue;
        snprintf(key,sizeof key,"world.equip.%d",i);
        emit(key,world_equip_name(i),user);
    }
#undef WORLD_DUMP_INT
}

/* Explicit little-endian decoding: maps.md sections 4-5; never native struct dumps. */
static int le32(const unsigned char *p)
{
    uint32_t u=(uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);
    return u<=INT32_MAX?(int)u:-1-(int)(UINT32_MAX-u);
}
static void disk_string(char *dst, size_t cap, const unsigned char *p, size_t bytes)
{
    size_t n=0;
    while(n<bytes && n+1<cap && p[n]) { dst[n]=(char)p[n]; ++n; }
    dst[n]=0;
}

/* FUN_00463989 (.obl, 0x100 x 800), FUN_00464461 (.mon, 0x43620 bytes) and FUN_00463630
 * (objects.obr, count x 0x30) are the same loader written three times: memset the whole table
 * to zero FIRST, then fopen, then fread as many whole records as the file actually has, and
 * return the record count. A missing or short file is NOT an error - the table is simply left
 * empty. That is how retail Evergreen ships castle1, petarena, pkarena, stonetree and shrimpee
 * (no .mon) and NorthUmbrage, grotto, isleLight, springwell (no .ter), and the map still loads.
 * FUN_00464461 also returns whether the read was exactly 0x43620 bytes, but its caller at
 * 0x41F126 only stores that in DAT_004F224C for the debug placement overlay; it never fails.
 * Returns the number of whole records read, 0 for a missing file. */
static int load_records(Map *map, const char *path, int type)
{
    unsigned char record[OBL_RECORD_SIZE];
    int count=type==0?OBL_RECORDS:type==1?MON_RECORDS:OBR_RECORDS;
    size_t size=type==0?OBL_RECORD_SIZE:type==1?MON_RECORD_SIZE:OBR_RECORD_SIZE;
    FILE *f=plat_fopen(path,"rb");
    int i, read=0;
    if(!f) return 0;
    for(i=0;i<count;++i) {
        size_t got=fread(record,1,size,f);
        if(got!=size) break;      /* short or missing tail: the rest of the table stays zeroed */
        ++read;
        if(type==0) {
            Link *v=&map->links[i];
            v->used=le32(record); v->object_id=le32(record+4);
            v->x=le32(record+8); v->y=le32(record+12); v->kind=le32(record+16);
            v->target=le32(record+100); v->difficulty=le32(record+104); v->theme=le32(record+108);
            disk_string(v->background,sizeof(v->background),record+112,80);
            disk_string(v->name,sizeof(v->name),record+192,128);
            v->dest_map=le32(record+404); v->weather=le32(record+408); v->fx=le32(record+412);
            v->has_been_used=le32(record+416);
            v->required_item=le32(record+420);
        } else if(type==1) {
            MonPlace *v=&map->mons[i];
            v->monster_id=le32(record); v->x=le32(record+4); v->y=le32(record+8); v->radius=le32(record+12);
            if(v->monster_id>0) ++map->mon_count;
        } else {
            ObjRect *v=&map->objrects[i];
            v->used=le32(record); disk_string(v->name,sizeof(v->name),record+4,24);
            v->l=le32(record+32); v->t=le32(record+36); v->r=le32(record+40); v->b=le32(record+44);
        }
    }
    fclose(f);
    return read;
}

void map_free(Map *map)
{
    image_free(&map->image); image_free(&map->image_x4); image_free(&map->terrain); image_free(&map->objects);
    memset(map,0,sizeof(*map));
}

int map_load(Map *map, int id)
{
    Map loaded={0};
    char path[1024], rel[160];
    int x,y;
    if(id<0 || id>=WORLD_MAX_MAPS || !g_world.maps[id].used) return -1;
    loaded.id=id; loaded.def=&g_world.maps[id];
    snprintf(rel,sizeof(rel),"maps/%s",loaded.def->image);
    if(!world_path(path,sizeof(path),rel) || image_load(&loaded.image,path)) goto fail;
    if(loaded.image.w>INT_MAX/4 || loaded.image.h>INT_MAX/4 ||
       (size_t)loaded.image.w*loaded.image.h>MAP_PIXELS_MAX/16) goto fail;
    snprintf(rel,sizeof(rel),"maps/%sX4.jpg",loaded.def->root);
    if(!world_path(path,sizeof(path),rel)) goto fail;
    if(image_load(&loaded.image_x4,path)) {
        Image *hi=&loaded.image_x4;
        hi->w=loaded.image.w*4; hi->h=loaded.image.h*4; hi->bpp=32;
        hi->pixels=malloc((size_t)hi->w*hi->h*sizeof(*hi->pixels));
        if(!hi->pixels) goto fail;
        for(y=0;y<hi->h;++y) for(x=0;x<hi->w;++x)
            hi->pixels[(size_t)y*hi->w+x]=loaded.image.pixels[(size_t)(y/4)*loaded.image.w+x/4];
    }
    if(loaded.image_x4.w!=loaded.image.w*4 || loaded.image_x4.h!=loaded.image.h*4) goto fail;
    snprintf(rel,sizeof(rel),"maps/%s.ter",loaded.def->root);
    if(!world_path(path,sizeof(path),rel)) goto fail;
    if(image_load(&loaded.terrain,path)) {
        FILE *existing=plat_fopen(path,"rb");
        Image *ter=&loaded.terrain;
        size_t cells;
        if(existing) { fclose(existing); goto fail; }
        /* FUN_0041e421 creates a cleared terrain DIB before its optional load, so a world with no
         * .ter (retail NorthUmbrage, grotto, isleLight, springwell) walks on terrain 0. */
        ter->w=(loaded.image.w+3)/4; ter->h=(loaded.image.h+3)/4;
        ter->bpp=8; ter->palette_size=256;
        cells=(size_t)ter->w*ter->h;
        ter->indices=calloc(cells,1); ter->pixels=calloc(cells,sizeof(*ter->pixels));
        if(!ter->indices || !ter->pixels) goto fail;
    }
    /* Only the 8-bit form is required. The dimensions are whatever the .ter says: FUN_00486690
     * decodes the BMP and nothing compares it with the jpg, and retail castle1.ter is 82x87 where
     * its 328x350 jpg implies 82x88. map_terrain_at answers terrain 9 outside the grid the file
     * actually provides, which is the defined answer to what the original would read past the end
     * of the buffer. */
    if(!loaded.terrain.indices || loaded.terrain.bpp!=8 ||
       loaded.terrain.w<1 || loaded.terrain.h<1) goto fail;
    /* .obl, .mon and objects.obr are optional: the three loaders zero their table and read what
     * exists, and the original checks none of their return values (0x41F0BF, 0x41F11A, 0x41F126).
     * objects.bmp/.jpg is optional for the same reason - FUN_00486690's result is stored in
     * DAT_00549898 and only matters when a link sprite is actually drawn. */
    snprintf(rel,sizeof(rel),"maps/%s.obl",loaded.def->root);
    if(!world_path(path,sizeof(path),rel)) goto fail;
    load_records(&loaded,path,0);
    snprintf(rel,sizeof(rel),"maps/%s.mon",loaded.def->root);
    if(!world_path(path,sizeof(path),rel)) goto fail;
    load_records(&loaded,path,1);
    if(world_path(path,sizeof(path),"maps/objects.bmp")) image_load(&loaded.objects,path);
    if(!loaded.objects.pixels && world_path(path,sizeof(path),"maps/objects.jpg"))
        image_load(&loaded.objects,path);
    if(world_path(path,sizeof(path),"maps/objects.obr")) load_records(&loaded,path,2);
    map_free(map); *map=loaded; return 0;
fail:
    fprintf(stderr,"world: cannot load map %d resource %s\n",id,path);
    map_free(&loaded); return -1;
}

/* FUN_0046186F (0x46186F), the terrain lookup, has TWO different out-of-range answers:
 *   1. x>>2 / y>>2 outside the JPG-derived grid DAT_004DF8AC/DAT_004DF8B0 - which FUN_0041E421
 *      sets to ((jpg width)+3)/4 and ((jpg height)+3)/4 at 0x41E4B4/0x41E4B5 - returns 9.
 *   2. inside that grid but past the .ter's OWN extent, `if (*(int*)(DAT_00549B7C+4) <= cx)
 *      return 0;` and the `if (cy < FUN_004864C0())` inner read both leave the block, and the
 *      function's remaining `return 0` answers TERRAIN 0 - open ground, NOT impassable.
 * That distinction is load-bearing because retail .ter files are smaller than their jpg implies:
 * castle1 is 82x87 against 82x88, floodedMaze 89x89 against 90x89, wormCave 88x88 against 89x89.
 * Answering 9 past the file would wall off the last row of those three maps; the original leaves
 * them walkable.
 * The raw pixel bound is FUN_004631C6's own guard (0x4631C6, `param_1 < DAT_005494B8 &&
 * param_2 < DAT_00549990`, both from the JPG), whose answer is "not walkable". */
int map_terrain_at(const Map *map, int x, int y)
{
    int cx, cy;
    if(!map || !map->terrain.indices || x<0 || y<0 || x>=map->image.w || y>=map->image.h) return 9;
    cx=x>>2; cy=y>>2;
    if(cx>=(map->image.w+3)/4 || cy>=(map->image.h+3)/4) return 9;
    if(cx>=map->terrain.w || cy>=map->terrain.h) return 0;
    return map->terrain.indices[(size_t)cy*map->terrain.w+cx];
}
int map_walkable(const Map *map, int x, int y, const unsigned char *tokens)
{
    int t=map_terrain_at(map,x,y), token;
    if(t==0) return 1;
    if(t==9) return 0;
    token=g_world.terrains[t].token;
    return tokens && token>0 && token<4096 && tokens[token]!=0;
}

void sheet_free(Sheet *sheet)
{ image_free(&sheet->image); memset(sheet,0,sizeof(*sheet)); }

static void key_pixel(Sheet *s, int x, int y, unsigned char index)
{
    size_t at=(size_t)y*s->image.w+x;
    s->image.pixels[at]=(uint32_t)s->key;
    if(s->image.indices) s->image.indices[at]=index;
}
static int load_sheet(Sheet *out, const char *path, int skin)
{
    Sheet s={0};
    int x,y,end;
    unsigned char key_index=0;
    if(image_load(&s.image,path)) return -1;
    s.cell=s.cell_h=s.image.h; s.count=s.image.w/s.cell;
    /* FUN_0048df3c floors width/height; Adventurer has a trailing separator. */
    if(!s.count || (skin && (s.cell<=35 || s.count<5))) {
        sheet_free(&s); return -1;
    }
    /* FUN_0048dad9 samples row h-1 of the original bottom-up DIB.
     * Image normalizes to top-down, so that original pixel is indices[0]. */
    if(s.image.indices) { key_index=s.image.indices[0]; s.key=s.image.palette[key_index]; }
    else s.key=s.image.pixels[0];
    /* FUN_0048dad9: normalize filmstrip boundaries and the shadow separators. */
    for(x=0;x<s.image.w;++x) { key_pixel(&s,x,0,key_index); key_pixel(&s,x,s.cell-1,key_index); }
    for(x=0;x<s.image.w;x+=s.cell) for(y=0;y<s.cell;++y) key_pixel(&s,x,y,key_index);
    y=s.cell-s.cell/6;
    end=skin?s.cell*5:s.image.w;
    for(x=s.cell;x<end;++x) { key_pixel(&s,x,y-1,key_index); key_pixel(&s,x,y,key_index); }
    for(x=6*s.cell;x<s.image.w;++x) { key_pixel(&s,x,y-1,key_index); key_pixel(&s,x,y,key_index); }
    sheet_free(out); *out=s; return 0;
}

int sheet_load_skin(Sheet *sheet, const char *name)
{
    char path[1024],rel[256];
    int n=snprintf(rel,sizeof(rel),"skins/%s.bmp",name);
    if(n<0 || (size_t)n>=sizeof(rel) || !world_data_path(path,sizeof(path),rel)) return -1;
    return load_sheet(sheet,path,1);
}
int sheet_load_monster(Sheet *sheet, const char *name)
{
    char path[1024],rel[256];
    int n=snprintf(rel,sizeof(rel),"monsters/%s.bmp",name);
    if(n<0 || (size_t)n>=sizeof(rel)) return -1;
    if(world_path(path,sizeof(path),rel) && !load_sheet(sheet,path,0)) return 0;
    if(world_data_path(path,sizeof(path),rel) && !load_sheet(sheet,path,0)) return 0;
    if(!world_data_path(path,sizeof(path),"monsters/josh1.bmp")) return -1;
    return load_sheet(sheet,path,0);
}

int sheet_load_art(Sheet *out, const char *name, int cell_w, int cell_h)
{
    Sheet s={0};
    char path[1024],rel[256];
    size_t len;
    int n,columns,rows;
    if(!name || cell_w<=0 || cell_h<=0) return -1;
    len=strlen(name);
    n=snprintf(rel,sizeof(rel),"art/%s%s",name,
               len>=4 && !text_casecmp(name+len-4,".bmp")?"":".bmp");
    if(n<0 || (size_t)n>=sizeof(rel)) return -1;
    if(!world_path(path,sizeof(path),rel) || image_load(&s.image,path)) {
        if(!world_data_path(path,sizeof(path),rel) || image_load(&s.image,path)) return -1;
    }
    columns=s.image.w/cell_w; rows=s.image.h/cell_h;
    if(!columns || !rows) { sheet_free(&s); return -1; }
    s.cell=cell_w; s.cell_h=cell_h; s.count=columns*rows;
    s.key=s.image.indices?s.image.palette[s.image.indices[0]]:s.image.pixels[0];
    /* Raw UI/effects atlases have no character shadow/separator normalization. */
    sheet_free(out); *out=s;
    return 0;
}

void sheet_draw(struct Framebuffer *fb, const Sheet *sheet, int index, int x, int y, int flip)
{
    Rect src;
    int columns;
    if(!sheet || index<0 || index>=sheet->count || sheet->cell<=0 || sheet->cell_h<=0) return;
    columns=sheet->image.w/sheet->cell;
    if(!columns) return;
    src=(Rect){(index%columns)*sheet->cell,(index/columns)*sheet->cell_h,sheet->cell,sheet->cell_h};
    fb_blit_sub(fb,&sheet->image,src,x,y,flip,sheet->key);
}
void sheet_draw_map_dir(struct Framebuffer *fb, const Sheet *sheet, int dir, int x, int y)
{
    int cell;
    Rect src;
    if(!sheet || !sheet->count || dir<0 || dir>8) return;
    cell=sheet->cell/3;
    if(cell<=2) return;
    /* Map painter 0x4639eb calls 0x416426: its MAP branch insets all four
     * source edges by one (InflateRect -1,-1), excluding authoring guides. */
    src=(Rect){(dir%3)*cell+1,(dir/3)*cell+1,cell-2,cell-2};
    fb_blit_sub(fb,&sheet->image,src,x,y,0,sheet->key);
}
