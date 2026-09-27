/* Retail formats: docs/re/script.md sections 1 and 6, maps.md sections 1-5,
 * art.md section 2. Allocations happen only when loading, never when drawing. */
#include "world.h"
#include "../engine/fb.h"
#include "../engine/ini.h"
#include "../engine/text.h"
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
    size_t n = strlen(src);
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

int world_tokenize(const char *line, char tokens[][256], int max)
{
    int count = 0;
    const unsigned char *p = (const unsigned char *)line;
    if (!line || !tokens || max < 0) return -1;
    while (*p) {
        int quoted = 0;
        size_t len = 0;
        while (*p && (*p <= ' ' || *p == ',')) ++p;
        if (!*p || *p == ';' || (p[0] == '/' && p[1] == '/')) break;
        if (count == max) return -1;
        if (*p == '"') { quoted = 1; ++p; }
        while (*p) {
            /* Semicolons terminate even quoted text: script.md section 1.2. */
            if (*p == ';') break;
            if (quoted) {
                if (*p == '"') { ++p; break; }
            } else {
                if (*p <= ' ' || *p == ',' || (p[0] == '/' && p[1] == '/')) break;
                if (*p == '"') { quoted = 1; ++p; continue; }
            }
            if (len == 255) return -1;
            tokens[count][len++] = (char)*p++;
        }
        tokens[count++][len] = 0;
        if (*p == ';' || (!quoted && p[0] == '/' && p[1] == '/')) break;
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

static int read_quest(const char *path, int depth, char **text, size_t *size, size_t *capacity)
{
    char *file, *cursor, *line;
    size_t file_size;
    int result = 0;
    if (depth == INCLUDE_DEPTH_MAX) return -1;
    file = text_read_file(path, &file_size);
    if (!file || file_size > QUEST_BYTES_MAX) { free(file); return -1; }
    cursor = file;
    while ((line = text_next_line(&cursor)) != NULL) {
        char *p = line;
        while (*p && (unsigned char)*p <= ' ') ++p;
        if (!*p || *p == ';' || (p[0] == '/' && p[1] == '/')) continue;
        if (*p == '#') {
            char t[3][256], child[1024];
            const char *slash;
            int n = world_tokenize(p, t, 3);
            if (n > 0 && !text_casecmp(t[0], "#include")) {
                size_t base;
                if (n != 2) { result = -1; break; }
                slash = strrchr(path, '/');
                base = slash ? (size_t)(slash - path + 1) : 0;
                if (base + strlen(t[1]) >= sizeof(child)) { result = -1; break; }
                memcpy(child, path, base);
                memcpy(child + base, t[1], strlen(t[1]) + 1);
                if (read_quest(child, depth + 1, text, size, capacity)) { result = -1; break; }
                continue;
            }
        }
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

enum { SEC_MAPS, SEC_TERRAINS, SEC_MONSTERS, SEC_GROUPS, SEC_LEVELS, SEC_ITEMS,
       SEC_SPELLS, SEC_SCENES, SEC_CREDITS, SEC_STORY, SEC_TOKENS, SEC_ELEMENTS,
       SEC_HANDS, SEC_EQUIP, SEC_THEMES, SEC_TROPHIES, SEC_COUNT };
static const char *const section_names[SEC_COUNT] = {
    "MAPS","TERRAINS","MONSTERS","GROUPS","LEVELS","ITEMS","SPELLS","SCENES",
    "CREDITS","STORY","TOKENS","ELEMENTS","HANDS","EQUIP","THEMES","TROPHIES"
};

static int parse_row(int section, char t[][256], int n, int *current_class)
{
    int id, i;
    if (section==SEC_LEVELS) return parse_levels(t,n,current_class);
    if (!isdigit((unsigned char)t[0][0])) return -1;
    id=number(t[0]);
    switch(section) {
    case SEC_MAPS: {
        MapDef *m;
        if(id>=WORLD_MAX_MAPS || n<5) return -1;
        m=&g_world.maps[id]; m->used=1;
        copy_string(m->image,sizeof(m->image),t[1]);
        copy_string(m->root,sizeof(m->root),t[2]);
        copy_string(m->name,sizeof(m->name),t[3]);
        m->flags=(uint32_t)strtoul(t[4],NULL,0); m->theme=arg(t,n,5);
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
        v=&g_world.spells[id]; v->used=1; copy_string(v->name,sizeof(v->name),t[1]);
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
        if(dotarg(t,n,12,1)>0) v->effects=dotarg(t,n,12,1);
        if(dotarg(t,n,12,2)>0) v->weather=dotarg(t,n,12,2);
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
        if(active<0 || (active>SEC_SCENES && active!=SEC_ELEMENTS &&
           active!=SEC_HANDS && active!=SEC_TROPHIES)) continue;
        if(active==SEC_SCENES) {
            /* Dialogue may exceed a token: only SCENE declarations need lexing. */
            if(strlen(p)<5 || toupper((unsigned char)p[0])!='S' ||
               toupper((unsigned char)p[1])!='C' || toupper((unsigned char)p[2])!='E' ||
               toupper((unsigned char)p[3])!='N' || toupper((unsigned char)p[4])!='E' ||
               (p[5] && p[5]!=' ' && p[5]!='\t' && p[5]!=',')) continue;
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
    return active==-1?0:-1;
}

void world_free(void)
{
    free(g_world.lines); free(g_world.text);
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
    }
}

int world_load(const char *data_dir, const char *name)
{
    char path[1024], *p, *config;
    size_t size=0, capacity=0, offset;
    int count=0, n;
    Ini ini;
    world_free();
    if(!data_dir || !name || strlen(data_dir)>=sizeof(data_root) || strlen(name)>=sizeof(g_world.name)) return -1;
    copy_string(data_root,sizeof(data_root),data_dir); copy_string(g_world.name,sizeof(g_world.name),name);
    n=snprintf(g_world.dir,sizeof(g_world.dir),"%s/worlds/%s",data_dir,name);
    if(n<0 || (size_t)n>=sizeof(g_world.dir)) goto fail;
    if(!world_path(path,sizeof(path),"quest.txt") || read_quest(path,0,&g_world.text,&size,&capacity)) goto fail;
    for(offset=0;offset<size;offset+=strlen(g_world.text+offset)+1)
        if(++count>QUEST_LINES_MAX) goto fail;
    if(!count) goto fail;
    g_world.lines=malloc((size_t)count*sizeof(*g_world.lines));
    if(!g_world.lines) goto fail;
    g_world.line_count=count; p=g_world.text;
    for(n=0;n<count;++n) { g_world.lines[n]=p; p+=strlen(p)+1; }
    if(parse_world()) goto fail;
    complete_spells();
    g_world.starting_gp=500;
    if(!world_path(path,sizeof(path),"config.ini")) goto fail;
    g_world.max_unspent_pp=1000000;
    config=text_read_file(path,NULL);
    if(config) {
        if(ini_parse(&ini,config)) { free(config); goto fail; }
        g_world.starting_gp=number(ini_get(&ini,"General","startingGP","500"));
        g_world.max_unspent_pp=clamp(number(ini_get(&ini,"General","maxUnspentPP","1000000")),0,INT_MAX);
        free(config);
    }
    if(!world_path(path,sizeof(path),"music.ini")) goto fail;
    music_text=text_read_file(path,NULL);
    if(music_text && ini_parse(&music_ini,music_text)) goto fail;
    return 0;
fail:
    fprintf(stderr,"world: cannot load %s\n",name);
    world_free(); return -1;
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

static int load_records(Map *map, const char *path, int type)
{
    unsigned char record[OBL_RECORD_SIZE];
    int count=type==0?OBL_RECORDS:type==1?MON_RECORDS:OBR_RECORDS;
    size_t size=type==0?OBL_RECORD_SIZE:type==1?MON_RECORD_SIZE:OBR_RECORD_SIZE;
    FILE *f=plat_fopen(path,"rb");
    int i;
    if(!f) return -1;
    for(i=0;i<count;++i) {
        size_t got=fread(record,1,size,f);
        /* FUN_00463630 reads up to 1000 OBRs; Evergreen ships only 256. */
        if(type==2 && got==0 && feof(f) && i>0) break;
        if(got!=size) { fclose(f); return -1; }
        if(type==0) {
            Link *v=&map->links[i];
            v->used=le32(record); v->object_id=le32(record+4);
            v->x=le32(record+8); v->y=le32(record+12); v->kind=le32(record+16);
            v->target=le32(record+100); v->difficulty=le32(record+104); v->theme=le32(record+108);
            disk_string(v->background,sizeof(v->background),record+112,80);
            disk_string(v->name,sizeof(v->name),record+192,128);
            v->dest_map=le32(record+404); v->weather=le32(record+408); v->fx=le32(record+412);
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
    fclose(f); return 0;
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
        /* FUN_0041e421 creates a cleared terrain DIB before its optional load.
         * Retail Springwell has no .ter; the cleared cells mean terrain 0. */
        ter->w=(loaded.image.w+3)/4; ter->h=(loaded.image.h+3)/4;
        ter->bpp=8; ter->palette_size=256;
        cells=(size_t)ter->w*ter->h;
        ter->indices=calloc(cells,1); ter->pixels=calloc(cells,sizeof(*ter->pixels));
        if(!ter->indices || !ter->pixels) goto fail;
    }
    if(!loaded.terrain.indices || loaded.terrain.bpp!=8 ||
       loaded.terrain.w!=(loaded.image.w+3)/4 || loaded.terrain.h!=(loaded.image.h+3)/4) goto fail;
    snprintf(rel,sizeof(rel),"maps/%s.obl",loaded.def->root);
    if(!world_path(path,sizeof(path),rel) || load_records(&loaded,path,0)) goto fail;
    snprintf(rel,sizeof(rel),"maps/%s.mon",loaded.def->root);
    if(!world_path(path,sizeof(path),rel) || load_records(&loaded,path,1)) goto fail;
    if(!world_path(path,sizeof(path),"maps/objects.bmp")) goto fail;
    if(image_load(&loaded.objects,path)) {
        if(!world_path(path,sizeof(path),"maps/objects.jpg") || image_load(&loaded.objects,path)) goto fail;
    }
    if(!world_path(path,sizeof(path),"maps/objects.obr") || load_records(&loaded,path,2)) goto fail;
    map_free(map); *map=loaded; return 0;
fail:
    fprintf(stderr,"world: cannot load map %d resource %s\n",id,path);
    map_free(&loaded); return -1;
}

int map_terrain_at(const Map *map, int x, int y)
{
    if(!map || !map->terrain.indices || x<0 || y<0 || x>=map->image.w || y>=map->image.h ||
       x/4>=map->terrain.w || y/4>=map->terrain.h) return 9;
    return map->terrain.indices[(size_t)(y/4)*map->terrain.w+x/4];
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
