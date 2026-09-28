/* Standalone retail-data smoke driver; not linked into wos. */
#include "game/world.h"
#include "engine/fb.h"
#include "platform/platform.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void check_tokenizer(void)
{
    char tokens[8][256];
    char long_token[257];
    /* FUN_0047A0E4: ';' and "//" are ordinary characters; only whole-line ';' comments exist. */
    int n=world_tokenize(" ACTOR 3, \"Green Jelly\", josh2, -1",tokens,8);
    assert(n==5 && !strcmp(tokens[2],"Green Jelly") && !strcmp(tokens[4],"-1"));
    /* ';' ends that token only because a space follows; it is not a comment marker. */
    assert(world_tokenize("MUSIC \"\"; silence",tokens,8)==4 && !tokens[1][0]);
    assert(!strcmp(tokens[2],";") && !strcmp(tokens[3],"silence"));
    assert(world_tokenize("SAY \"a;b\" rest",tokens,8)==3 && !strcmp(tokens[1],"a;b"));
    assert(world_tokenize("SAY half//way",tokens,8)==2 && !strcmp(tokens[1],"half//way"));
    assert(world_tokenize("A \"comma, and space\" B",tokens,8)==3);
    assert(!strcmp(tokens[1],"comma, and space"));
    assert(world_tokenize("A \"unterminated quote",tokens,8)==2);
    assert(world_tokenize("A B",tokens,1)==-1);
    memset(long_token,'a',sizeof(long_token)); long_token[256]=0;
    assert(world_tokenize(long_token,tokens,8)==-1);
}

static void check_sheet(const char *name, int skin)
{
    Sheet sheet={0};
    Framebuffer fb;
    uint32_t *pixels;
    int x,y,dir;
    assert((skin?sheet_load_skin(&sheet,name):sheet_load_monster(&sheet,name))==0);
    assert(sheet.image.indices && sheet.count>0 && sheet.cell==sheet.image.h);
    assert(sheet.key==(int64_t)sheet.image.palette[sheet.image.indices[0]]);
    if(!strcmp(name,"adventurer") || !strcmp(name,"josh2")) assert(sheet.key==0x008080);
    for(x=0;x<sheet.image.w;++x) {
        assert(sheet.image.pixels[x]==sheet.key);
        assert(sheet.image.pixels[(size_t)(sheet.cell-1)*sheet.image.w+x]==sheet.key);
    }
    for(x=0;x<sheet.image.w;x+=sheet.cell) for(y=0;y<sheet.cell;++y)
        assert(sheet.image.pixels[(size_t)y*sheet.image.w+x]==sheet.key);
    pixels=malloc((size_t)sheet.cell*sheet.cell*sizeof(*pixels)); assert(pixels);
    fb_init(&fb,pixels,sheet.cell,sheet.cell);
    fb_clear(&fb,0x123456);
    sheet_draw(&fb,&sheet,1,0,0,1);
    for(y=0;y<sheet.cell;++y) for(x=0;x<sheet.cell;++x) {
        uint32_t source=sheet.image.pixels[(size_t)y*sheet.image.w+2*sheet.cell-1-x];
        assert(pixels[(size_t)y*sheet.cell+x]==(source==sheet.key?0x123456:source));
    }
    if(skin) for(dir=0;dir<9;++dir) {
        int side=sheet.cell/3;
        fb_clear(&fb,0x123456); sheet_draw_map_dir(&fb,&sheet,dir,0,0);
        for(y=0;y<side;++y) for(x=0;x<side;++x) {
            uint32_t expected=0x123456;
            if(x<side-2 && y<side-2) {
                uint32_t source=sheet.image.pixels[(size_t)(y+(dir/3)*side+1)*sheet.image.w+x+(dir%3)*side+1];
                if(source!=sheet.key) expected=source;
            }
            assert(pixels[(size_t)y*sheet.cell+x]==expected);
        }
    }
    printf("sheet %s %dx%d cell=%d frames=%d key=%06lx draw=OK\n",name,sheet.image.w,sheet.image.h,
           sheet.cell,sheet.count,(unsigned long)sheet.key);
    free(pixels); sheet_free(&sheet);
}
static void check_new_sections(void);

static void check_gap_tables(void)
{
    int i,trophies=0;
    const SpellDef *s=&g_world.spells[1];
    const ClassDef *c=&g_world.classes[9];
    assert(s->pp_cost==250 && s->mp_cost==2 && s->damage==30 && s->effects_row==63);
    assert(g_world.spells[3].damage==-11 && g_world.spells[3].pp_cost==562);
    assert(g_world.spells[6].all_targets && g_world.spells[6].req_affinity==5);
    /* FUN_0047fced swaps the dotted arg12 overrides: part 1 fills the record slot the packed
     * head fills with the AAA (weather) group and part 2 the BBB (effects) one. */
    assert(g_world.spells[5].effects==1055 && !g_world.spells[5].weather);
    assert(g_world.spells[7].weather==3 && g_world.spells[7].effects==3055);
    assert(g_world.spells[8].pp_cost==-1 && g_world.spells[78].flags==2);
    assert(g_world.items[1].find_probability==80 && !strcmp(g_world.items[1].sound,"petGulp.wav"));
    assert(g_world.items[3].ability_points==-2 && g_world.items[8].ability_points==5);
    assert(g_world.items[97].travel_map_scene==5 && g_world.items[97].travel_link==11 && g_world.items[97].travel_drop_in==1);
    assert(!strcmp(g_world.items[96].sound,"http://www.synthetic-reality.com/wosquest.htm"));
    assert(c->magic_ratio==45 && c->hand_ratio==45 && c->no_gifts);
    for(i=0;i<5;++i) assert(c->max_ability[i]==100);
    for(i=0;i<8;++i) assert(c->start_hand_pp[i]==1000 && c->max_hand_pp[i]==5000);
    assert(g_world.classes[1].start_ability_set && g_world.classes[1].max_element_pp[0]==5000000);
    assert(!strcmp(g_world.elements[5].name,"Fire") && !strcmp(g_world.elements[7].name,"Air"));
    assert(!strcmp(g_world.hands[0].name,"Sword") && g_world.hands[0].damage_weight==22);
    assert(g_world.hands[2].damage_weight==50 && !strcmp(g_world.hands[2].sound,"sword8.wav"));
    assert(g_world.trophies[1].monster_first[0]==1 && g_world.trophies[1].probability==80);
    assert(g_world.trophies[1000].stack_size==99 && g_world.trophies[1014].flags==8);
    for(i=0;i<WORLD_MAX_TROPHIES;++i) trophies+=g_world.trophies[i].used!=0;
    assert(!strcmp(world_music("rustrock","fight"),"ahad_glasse~1"));
    assert(!strcmp(world_music("evergreen","victory"),"orbwon.mid"));
    assert(!strcmp(world_music("missing-map","midi1"),"scrn_overworld"));
    assert(!*world_music("evergreen","levelup") && g_world.max_unspent_pp==100000);
    assert(!g_world.spells[0].used && g_world.spells[1].effect_seed);
    check_new_sections();
    printf("gap tables: Dirt pp=%d mp=%d damage=%d; item1 find=80; class9 caps=100/5000; trophies=%d; music inferno=3/common victory=orbwon.mid; PP cap=100000\n",
           s->pp_cost,s->mp_cost,s->damage,trophies);
}

/* +TOKENS (table and the TOKEN rows inside +SCENES), +EQUIP, +CREDITS/+STORY, config.ini and
 * the two world CRCs, all against the retail Evergreen data. */
static void check_new_sections(void)
{
    int i, tokens=0;
    for(i=0;i<WORLD_MAX_TOKENS;++i) tokens+=g_world.tokens[i].used!=0;
    assert(tokens>0 && !*world_token_text(-1) && !*world_token_text(WORLD_MAX_TOKENS));
    assert(!strcmp(world_token_text(1),"You have agreed to obey the golden rule."));
    assert(!strcmp(world_token_text(55),"The town recovers"));
    /* QuestScenes150.txt declares these inside +SCENES, not in the +TOKENS table. */
    assert(!strcmp(world_token_text(150),"You rescued Princess Lyssa from her dangling predicament."));
    assert(!world_chapter(0) && g_world.chapter_count==0);
    assert(!strcmp(world_equip_name(0),"Helmet") && !strcmp(world_equip_name(1),"Armor"));
    assert(!strcmp(world_equip_name(10),"Boots") && !strcmp(world_equip_name(11),"Shield"));
    assert(!strcmp(world_equip_name(12),"Ring") && !strcmp(world_equip_name(13),"Amulet"));
    assert(!strcmp(world_equip_name(2),"Sword") && !strcmp(world_equip_name(9),"Spirit"));
    assert(!strcmp(world_equip_name(-1),"Right-Hand") && !strcmp(world_equip_name(14),"Right-Hand"));
    assert(!strcmp(world_equip_name(2),world_hand_name(0)) && !strcmp(world_hand_name(8),"Right-Hand"));
    assert(world_equip_slot_by_name("amulet")==13 && world_equip_slot_by_name("Sword")==2);
    assert(world_equip_slot_by_name("nonesuch")==-1);
    assert(!strcmp(world_hero_slot_name(8,3),"Music") && !strcmp(world_hero_slot_name(2,0),"Boots"));
    assert(world_story_count()>10 && !strcmp(world_story_line(0),":S 200"));
    assert(!strcmp(world_story_line(1),":C 255,255,0") && !strcmp(world_story_line(2),"|Evergreen"));
    assert(!world_story_line(-1) && !world_story_line(world_story_count()));
    assert(!strncmp(world_credits_text(),"Evergreen\r\nStory by Dan Samuel\r\n",31));
    assert(strstr(world_credits_text(),"Contact us on the web at:\r\nhttp://www.synthetic-reality.com"));
    /* config.ini [General]: the retail file comments spellSuccessPercent out, so the call
     * falls back to the string 0x41E922 pushes - "0" (0x4DCAF4) - not the 100 that 0x41E315
     * pre-inits DAT_004e0ff8 to. */
    assert(!strcmp(world_gold_name(),"GP") && g_world.spell_success_percent==0);
    assert(g_world.starting_gp==500 && g_world.max_unspent_pp==100000);
    assert(g_world.pk_hand_percent==100 && g_world.pk_magic_percent==100);
    assert(g_world.cookie_protection==1 && !g_world.no_giving_gp && !g_world.pets_can_bite_people);
    assert(g_world.max_pk_attack_advantage==10 && g_world.pk_trophy==1000);
    assert(g_world.karma_points_are_also_war_points==234);
    assert(g_world.monster_xp_are_also_war_points==1000 && g_world.tactics_win_gives_war_points==1000);
    assert(!strcmp(g_world.world_home_url,"http://www.synthetic-reality.com/wosHome.htm"));
    assert(!strcmp(g_world.tactics_source_url,"http://www.synthetic-reality.com/tactics"));
    /* CRC-1 (FUN_0047977a: rotate-left-1 + XOR over the raw quest.txt stream, then XOR the
     * summed per-file byte counts) and CRC-2 (FUN_0047983e, dword XOR) are pinned for retail
     * Evergreen: both land in the hero record (+0x6F0/+0x6C4/+0x1B1), where a mismatch blocks
     * soul switching with "Modified Quest File Detected" (FUN_0044B196). Both values were
     * reproduced independently from the reconstructed raw byte stream. */
    assert(g_world.crc1==0xfe2166a6u && g_world.crc2==0x5902bf6du);
    printf("new sections: tokens=%d equip[0,1,10..13]=%s/%s/%s/%s/%s/%s story=%d credits=%d bytes"
           " startingGP=%d pp=%d cookie=%d crc1=%08lx crc2=%08lx\n",
           tokens,world_equip_name(0),world_equip_name(1),world_equip_name(10),world_equip_name(11),
           world_equip_name(12),world_equip_name(13),world_story_count(),
           (int)strlen(world_credits_text()),g_world.starting_gp,g_world.max_unspent_pp,
           g_world.cookie_protection,(unsigned long)g_world.crc1,(unsigned long)g_world.crc2);
}

static void check_art(const char *name, int cell_w, int cell_h)
{
    Sheet sheet={0};
    Framebuffer fb;
    uint32_t pixels[48*64];
    int x,y,columns,index;
    assert(sheet_load_art(&sheet,name,cell_w,cell_h)==0);
    assert(sheet.cell==cell_w && sheet.cell_h==cell_h);
    columns=sheet.image.w/cell_w;
    index=sheet.count>columns?columns:sheet.count-1;
    fb_init(&fb,pixels,cell_w,cell_h); fb_clear(&fb,0x123456);
    sheet_draw(&fb,&sheet,index,0,0,0);
    for(y=0;y<cell_h;++y) for(x=0;x<cell_w;++x) {
        uint32_t source=sheet.image.pixels[(size_t)(index/columns*cell_h+y)*sheet.image.w+(index%columns)*cell_w+x];
        assert(pixels[y*cell_w+x]==(source==sheet.key?0x123456:source));
    }
    assert(sheet_load_art(&sheet,name,0,cell_h)==-1 && sheet.cell==cell_w);
    printf("art %s %dx%d cell=%dx%d frames=%d row-major draw=OK\n",name,sheet.image.w,sheet.image.h,cell_w,cell_h,sheet.count);
    sheet_free(&sheet);
}

static void fixture_file(const char *dir, const char *name, const char *contents)
{
    char path[1024];
    FILE *f;
    assert(snprintf(path,sizeof(path),"%s/%s",dir,name)>0);
    f=plat_fopen(path,"wb"); assert(f);
    assert(fwrite(contents,1,strlen(contents),f)==strlen(contents));
    assert(fclose(f)==0);
}

/* Optional argv[2] is a caller-owned temporary root, never the retail directory. */
static void check_parser_edges(const char *root)
{
    char dir[1024];
    const ItemDef *item;
    const SpellDef *spell;
    assert(plat_mkdir(root)==0);
    assert(snprintf(dir,sizeof(dir),"%s/worlds",root)>0 && plat_mkdir(dir)==0);
    assert(snprintf(dir,sizeof(dir),"%s/worlds/ParserEdges",root)>0 && plat_mkdir(dir)==0);
    fixture_file(dir,"quest.txt",
        "+ITEMS\n1 Boots 20 +3 50 7.9.1.4.10.11.0.3 12 258 -7 900 1 2 99 80.24 \"test item\" \"s.wav\" 4.220.90.2.3\n-ITEMS\n"
        "+SPELLS\n0 Scale 50 0 200 150 0\n"
        "1 Test 0 3 0 0 102.5.6.7.8.9.0.3 123 2 4 2000 50 2003004.5.6 1 summon.wav travel.wav strike.wav 4.5\n"
        "2 Heal -1 0 0 4 0\n-SPELLS\n"
        "+LEVELS\n100 0 20 0 Test 10 1\nMAX_ABILITY 999 9 8 7 6\nSTART_ELEMENT_PP 1 2 3 4 5 6 7 8\n"
        "MAX_ELEMENT_PP 10 20 30 40 50 60 70 80\nSTART_HAND_PP 8 7 6 5 4 3 2 1\n"
        "MAX_HAND_PP 80 70 60 50 40 30 20 10\nSTART_SPELLS 1 2\nSTART_TOKENS 7 8\n"
        "START_ITEMS 1\nNO_GIFTS\nHAND_RATIO 0\nHIDDEN_CLASS 120\n101 0 1 0 Learner\n-LEVELS\n"
        "+ELEMENTS\n255 Chaos\n-ELEMENTS\n+HANDS\n0 Sword 200 sword.wav\n-HANDS\n"
        "+TROPHIES\n1 Test trophy 2 200 5 1.3-5.9 80 7 8\n-TROPHIES\n");
    fixture_file(dir,"music.ini","[common]\nfight=common\nnumMidi=2\nmidi1=one\n[place]\nfight=\nnumMidi=0\n");
    assert(world_load(root,"ParserEdges")==0);
    item=&g_world.items[1]; spell=&g_world.spells[1];
    assert(item->image_ext==1 && item->movement==12 && item->element==2);
    assert(item->defense==-7 && item->attack==255 && item->ability_points==20);
    assert(item->find_probability==80 && item->find_monster==24 && item->equip_token==9 && item->flags==1);
    assert(item->max_count==4 && item->trophy_count_needed==1 && item->trophy_count_made==3);
    assert(item->attack_image==199 && item->attack_flags==63);
    assert(spell->all_targets && spell->flags==5 && spell->min_level==6 && spell->token==7);
    assert(spell->mp_cost==7 && spell->pp_cost==562 && spell->damage==100);
    assert(spell->trophy_needed==8 && spell->trophy_made==9 && spell->trophy_count_needed==1);
    assert(spell->gravity==4 && spell->weather==5 && spell->effects==6 && spell->max_fx==1023);
    assert(!strcmp(spell->sfx_strike,"strike.wav") && spell->extra[1]==5);
    assert(g_world.spells[2].damage==0 && g_world.spells[2].pp_cost==-1);
    assert(g_world.classes[1].max_ability[0]==255 && g_world.classes[1].start_element_pp[7]==8);
    assert(g_world.classes[1].max_element_pp[7]==80 && g_world.classes[1].max_hand_pp[7]==10);
    assert(g_world.classes[1].start_spell_count==2 && g_world.classes[1].start_tokens[1]==8);
    assert(g_world.classes[1].hidden_start_level==100 && g_world.classes[1].hand_ratio==0);
    assert(g_world.hands[0].damage_weight==50 && g_world.trophies[1].stack_size==99);
    assert(g_world.trophies[1].monster_range_count==3 && g_world.trophies[1].monster_last[1]==5);
    assert(!*world_music("place","fight") && world_music_count("place")==0);
    assert(!strcmp(world_music("other","fight"),"common") && world_music_count("other")==2);
    world_free();
    assert(!*world_music("other","fight"));
    puts("parser edges: dotted costs/caps/negative values/ranges/empty music overrides PASS");
}

int main(int argc, char **argv)
{
    Map map={0};
    int i, maps=0, monsters=0, groups=0, classes=0, items=0, spells=0, scenes=0, terrains=0;
    static const int scene_ids[]={0,2,3};
    check_tokenizer();
    assert(world_load(argc>1?argv[1]:"extracted","Evergreen")==0);
    check_gap_tables();
    check_art("items",48,64);
    check_art("buttonBar.bmp",48,48);
    check_art("attack00",48,48);
    check_art("effects00",48,48);
    for(i=0;i<WORLD_MAX_MAPS;++i) maps+=g_world.maps[i].used!=0;
    for(i=0;i<WORLD_MAX_MONSTERS;++i) monsters+=g_world.monsters[i].used!=0;
    for(i=0;i<WORLD_MAX_GROUPS;++i) groups+=g_world.groups[i].used!=0;
    for(i=0;i<WORLD_MAX_CLASSES;++i) classes+=g_world.classes[i].used!=0;
    for(i=0;i<WORLD_MAX_ITEMS;++i) items+=g_world.items[i].used!=0;
    for(i=0;i<WORLD_MAX_SPELLS;++i) spells+=g_world.spells[i].used!=0;
    for(i=0;i<WORLD_MAX_SCENES;++i) scenes+=g_world.scenes[i].used!=0;
    for(i=0;i<WORLD_MAX_TERRAINS;++i) terrains+=g_world.terrains[i].used!=0;
    printf("world %s maps=%d terrains=%d monsters=%d groups=%d classes=%d items=%d spells=%d scenes=%d lines=%d startingGP=%d\n",
           g_world.name,maps,terrains,monsters,groups,classes,items,spells,scenes,g_world.line_count,g_world.starting_gp);
    assert(maps==18 && !strcmp(g_world.monsters[1].name,"Green Jelly"));
    assert(!strcmp(g_world.classes[1].name,"Sword-User") && !strcmp(g_world.classes[2].name,"Magic-User"));
    assert(g_world.classes[1].levels[0].d_hp==20 && g_world.classes[1].auto_max[1]==3759);
    assert(g_world.groups[0].used && g_world.groups[0].members[0]==1);
    assert(map_load(&map,0)==0);
    printf("map 0 %s low=%dx%d x4=%dx%d terrain=%dx%d placements=%d objects=%dx%d\n",map.def->name,
           map.image.w,map.image.h,map.image_x4.w,map.image_x4.h,map.terrain.w,map.terrain.h,map.mon_count,map.objects.w,map.objects.h);
    printf("link 0 used=%d object=%d x=%d y=%d kind=%d target=%d difficulty=%d name=\"%s\" background=%s\n",
           map.links[0].used,map.links[0].object_id,map.links[0].x,map.links[0].y,map.links[0].kind,
           map.links[0].target,map.links[0].difficulty,map.links[0].name,map.links[0].background);
    assert(map.links[0].used && map.links[0].x==179 && map.links[0].y==219);
    assert(map.links[0].x<map.image.w && map.links[0].y<map.image.h);
    assert(map.links[0].kind==1 && map.links[0].target==3 && !strcmp(map.links[0].background,"greek1"));
    assert(map_terrain_at(&map,-1,0)==9 && !map_walkable(&map,-1,0,NULL));
    assert(map_terrain_at(&map,map.image.w,0)==9);
    assert(map_load(&map,-1)==-1 && map.id==0 && map.image.w==768);
    for(i=0;i<map.terrain.w*map.terrain.h;++i) {
        int x=(i%map.terrain.w)*4, y=(i/map.terrain.w)*4;
        assert(map_terrain_at(&map,x,y)==map.terrain.indices[i]);
        assert(map_walkable(&map,x,y,NULL)==(map.terrain.indices[i]==0));
    }
    for(i=1;i<=10;++i) {
        const MonsterDef *m=&g_world.monsters[i];
        printf("monster %d %-18s skin=%-8s level=%d hp=%d mp=%d xp=%d gold=%d\n",i,m->name,m->skin,m->level,m->hp,m->mp,m->exp,m->gold);
    }
    for(i=0;i<WORLD_MAX_CLASSES;++i) if(g_world.classes[i].used) {
        const ClassDef *c=&g_world.classes[i];
        printf("class %d %s hidden=%d START_ABILITY=%d,%d,%d,%d,%d AUTO_MAX=%d:%d,%d,%d,%d,%d DEFAULT_SKIN=%s,%s,%s,%s\n",
               i,c->name,c->hidden,c->start_ability[0],c->start_ability[1],c->start_ability[2],c->start_ability[3],c->start_ability[4],
               c->auto_max_set,c->auto_max[0],c->auto_max[1],c->auto_max[2],c->auto_max[3],c->auto_max[4],
               c->default_skin[0],c->default_skin[1],c->default_skin[2],c->default_skin[3]);
    }
    for(i=0;i<3;++i) {
        const SceneDef *s=&g_world.scenes[scene_ids[i]];
        char tokens[16][256];
        assert(s->used && s->first_line<s->end_line && s->end_line<g_world.line_count);
        assert(world_tokenize(g_world.lines[s->first_line],tokens,16)>=2 && atoi(tokens[1])==scene_ids[i]);
        printf("scene %d lines=[%d,%d) %s\n",scene_ids[i],s->first_line,s->end_line,g_world.lines[s->first_line]);
    }
    check_sheet("adventurer",1); check_sheet(g_world.monsters[1].skin,0);
    check_sheet("not-a-retail-monster",0);
    /* Rune Ruins has no X4; Springwell also omits its optional terrain file. */
    assert(map_load(&map,1)==0);
    for(i=0;i<map.image_x4.w*map.image_x4.h;++i) {
        int x=i%map.image_x4.w, y=i/map.image_x4.w;
        assert(map.image_x4.pixels[i]==map.image.pixels[(size_t)(y/4)*map.image.w+x/4]);
    }
    printf("map 1 synthesized X4=%dx%d nearest-neighbor=OK\n",map.image_x4.w,map.image_x4.h);
    assert(map_load(&map,13)==0);
    for(i=0;i<map.terrain.w*map.terrain.h;++i) assert(map.terrain.indices[i]==0);
    printf("map 13 missing terrain=%dx%d clear=OK\n",map.terrain.w,map.terrain.h);
    map_free(&map); world_free();
    if(argc>2) check_parser_edges(argv[2]);
    puts("world_selftest: PASS");
    return 0;
}
