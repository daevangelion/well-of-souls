/* Standalone retail-data smoke driver; not linked into wos. */
#include "game/world.h"
#include "engine/fb.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void check_tokenizer(void)
{
    char tokens[8][256];
    char long_token[257];
    int n=world_tokenize(" ACTOR 3, \"Green Jelly\", josh2, -1 // comment",tokens,8);
    assert(n==5 && !strcmp(tokens[2],"Green Jelly") && !strcmp(tokens[4],"-1"));
    assert(world_tokenize("MUSIC \"\"; silence",tokens,8)==2 && !tokens[1][0]);
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
            uint32_t source=sheet.image.pixels[(size_t)(y+(dir/3)*side)*sheet.image.w+x+(dir%3)*side];
            assert(pixels[(size_t)y*sheet.cell+x]==(source==sheet.key?0x123456:source));
        }
    }
    printf("sheet %s %dx%d cell=%d frames=%d key=%06lx draw=OK\n",name,sheet.image.w,sheet.image.h,
           sheet.cell,sheet.count,(unsigned long)sheet.key);
    free(pixels); sheet_free(&sheet);
}

int main(int argc, char **argv)
{
    Map map={0};
    int i, maps=0, monsters=0, groups=0, classes=0, items=0, spells=0, scenes=0, terrains=0;
    static const int scene_ids[]={0,2,3};
    check_tokenizer();
    assert(world_load(argc>1?argv[1]:"extracted","Evergreen")==0);
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
    map_free(&map); world_free();
    puts("world_selftest: PASS");
    return 0;
}
