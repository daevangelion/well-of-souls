/* Retail dialogs are adapted to the 364x416 main viewport. OFFER2 ordering and
 * filtering: FUN_0047df.. case 0x12; sell price: FUN_0040c.. (all.c:9673).
 * Art class mapping: FUN_00482431; equipment deltas: all.c:9645-9659. */
#include "panels.h"
#include "hero.h"
#include "world.h"
#include "../engine/font.h"
#include "../engine/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SHOP_MAX 100
#define ICON_CACHE 32
#define INK 0xffe6aeu
static const Rect close_rect = {284,384,68,24};
static const char *const titles[] = {"", "Items", "Spells", "Equipment", "Statistics", "Training", "Shop"};
static const char *const ability_names[] = {"STR","WIS","STA","AGI","DEX"};
static const int slots[] = {HERO_SLOT_HELMET,HERO_SLOT_ARMOR,HERO_SLOT_BOOTS,HERO_SLOT_SHIELD,HERO_SLOT_RING,HERO_SLOT_AMULET,HERO_SLOT_RIGHT_HAND};
static const char *const slot_names[] = {"Head","Body","Feet","Guard","Ring","Charm","Hand"};
static struct {
    PanelKind kind;
    int selected, first, count, ids[WORLD_MAX_ITEMS];
    int offers[SHOP_MAX], offer_count, selling, training_kind, ability;
    char message[128];
    Image background, border, equip_icons;
    struct { char name[48]; Sheet sheet; } icons[ICON_CACHE];
    int icon_count, next_icon;
} panel;
static int clicked(const Input *in, Rect r)
{
    return (in->mouse_pressed & 2u) && in->mouse_x>=r.x && in->mouse_y>=r.y &&
           in->mouse_x<r.x+r.w && in->mouse_y<r.y+r.h;
}
static void button(Framebuffer *fb, Rect r, const char *text, int selected)
{
    fb_fill(fb,r,selected?0x67512e:0x292820);
    fb_rect(fb,r,selected?0xffd477:0x9d8959);
    font_draw(fb,r.x+(r.w-font_width(text))/2,r.y+(r.h-8)/2,text,INK);
}
static void art_load(Image *image, const char *name)
{
    char rel[96],path[1024];
    snprintf(rel,sizeof rel,"art/%s",name);
    if(image_load(image,world_path(path,sizeof path,rel)))
        image_load(image,world_data_path(path,sizeof path,rel));
}
static void message(const char *text)
{
    snprintf(panel.message,sizeof panel.message,"%s",text);
}
static int page_size(void) { return panel.kind==PANEL_EQUIP?12:panel.kind==PANEL_SPELLS?8:18; }
static void selection_bound(void)
{
    int size=page_size();
    if(panel.selected>=panel.count) panel.selected=panel.count-1;
    if(panel.selected<0) panel.selected=0;
    panel.first=panel.selected/size*size;
}
static void rebuild(void)
{
    int i;
    panel.count=0;
    if(panel.kind==PANEL_SHOP && !panel.selling) {
        for(i=0;i<panel.offer_count;i++) panel.ids[panel.count++]=panel.offers[i];
    } else if(panel.kind==PANEL_SPELLS) {
        for(i=1;i<WORLD_MAX_SPELLS;i++)
            if(g_world.spells[i].used && (hero_spell_known(&g_hero,i) || hero_can_learn_spell(&g_hero,i))) panel.ids[panel.count++]=i;
    } else if(panel.kind==PANEL_TRAIN) panel.count=8;
    else if(panel.kind==PANEL_ITEMS || panel.kind==PANEL_EQUIP || panel.kind==PANEL_SHOP) {
        for(i=0;i<HERO_INVENTORY;i++) {
            int id=g_hero.inventory[i].item_id;
            if(g_hero.inventory[i].count<=0 || id<=0 || id>=WORLD_MAX_ITEMS || !g_world.items[id].used) continue;
            if(panel.kind==PANEL_EQUIP && hero_item_slot(id)<0) continue;
            if(panel.kind==PANEL_SHOP && g_world.items[id].gp<=0) continue;
            panel.ids[panel.count++]=id;
        }
    }
    selection_bound();
}
void panel_close(void)
{
    int i;
    image_free(&panel.background);image_free(&panel.border);image_free(&panel.equip_icons);
    for(i=0;i<panel.icon_count;i++) sheet_free(&panel.icons[i].sheet);
    memset(&panel,0,sizeof panel);
}
int panel_active(void) { return panel.kind!=PANEL_NONE; }
void panel_open(PanelKind kind)
{
    panel_close();
    if(kind<=PANEL_NONE || kind>PANEL_SHOP) return;
    panel.kind=kind;
    art_load(&panel.background,kind==PANEL_EQUIP?"bkEquip.jpg":kind==PANEL_SPELLS?"bkSpell.jpg":
             kind==PANEL_ITEMS || kind==PANEL_SHOP?"itemTable.jpg":"bkBook.jpg");
    art_load(&panel.border,"shopBorder256.bmp");
    if(kind==PANEL_EQUIP) art_load(&panel.equip_icons,"equipIcons.bmp");
    rebuild();
    wos_log_event("panel_open","kind=%s",titles[kind]);
}
static void offer_add(int id)
{
    int i;
    if(id<=0 || id>=WORLD_MAX_ITEMS || !g_world.items[id].used || g_world.items[id].gp<=0) return;
    for(i=0;i<panel.offer_count;i++) if(panel.offers[i]==id) return;
    if(panel.offer_count<SHOP_MAX) panel.offers[panel.offer_count++]=id;
}
void panel_open_shop(const char *const *args,int argc,int offer2)
{
    int i,j;
    panel_open(PANEL_SHOP);
    if(args && argc>0) {
        if(!offer2) for(i=0;i<argc;i++) offer_add(atoi(args[i]));
        else for(i=0;i+1<argc && panel.offer_count<SHOP_MAX;i+=3) {
            int min=atoi(args[i]),max=atoi(args[i+1]),klass=i+2<argc?atoi(args[i+2]):-1;
            /* OFFER2 minLevel,maxLevel[,class], repeated in groups of three. */
            for(j=1;j<WORLD_MAX_ITEMS && panel.offer_count<SHOP_MAX;j++) {
                const ItemDef *item=&g_world.items[j];
                if(item->used && item->find_probability!=0 && item->klass<100 &&
                   item->level>=min && item->level<=max && (klass<0 || item->klass==klass)) offer_add(j);
            }
        }
    }
    rebuild();
}
static void icon(Framebuffer *fb,int id,int x,int y)
{
    static const char *const names[]={"helmets","armor","swords","staffs","bows","music","right5","right6","right7","right8","boots","shields","rings","amulets"};
    const ItemDef *item;
    const char *base;
    char name[48];int i;
    if(id<=0 || id>=WORLD_MAX_ITEMS) return;
    item=&g_world.items[id];
    base=item->klass>=10 && item->klass<=23?names[item->klass-10]:item->klass==5?"darts":"items";
    if(item->image_ext) snprintf(name,sizeof name,"%s_%d.bmp",base,item->image_ext);
    else snprintf(name,sizeof name,"%s.bmp",base);
    for(i=0;i<panel.icon_count;i++) if(!strcmp(name,panel.icons[i].name)) break;
    if(i==panel.icon_count) {
        if(i==ICON_CACHE) {
            i=panel.next_icon;
            panel.next_icon=(panel.next_icon+1)%ICON_CACHE;
            sheet_free(&panel.icons[i].sheet);
        } else panel.icon_count++;
        snprintf(panel.icons[i].name,sizeof panel.icons[i].name,"%s",name);
        if(!sheet_load_art(&panel.icons[i].sheet,name,48,48)) {
            Sheet *s=&panel.icons[i].sheet;
            /* Item filmstrips have one row. Retail items.bmp is 64x64 per
             * cell; equipment is 48 wide, with 48/64-high cells. */
            s->cell=!strcmp(base,"items")?s->image.h:48;
            s->cell_h=s->image.h;
            s->count=s->image.w/s->cell;
        }
    }
    {
        const Sheet *s=&panel.icons[i].sheet;
        int dx,dy,w,h;
        if(!s->image.pixels || item->image<0 || item->image>=s->count) return;
        if(s->cell==48 && s->cell_h==48) {sheet_draw(fb,s,item->image,x,y,0);return;}
        w=s->cell>=s->cell_h?48:48*s->cell/s->cell_h;
        h=s->cell_h>=s->cell?48:48*s->cell_h/s->cell;
        x+=(48-w)/2;y+=(48-h)/2;
        for(dy=0;dy<h;dy++) for(dx=0;dx<w;dx++) {
            uint32_t pixel=s->image.pixels[(dy*s->cell_h/h)*s->image.w+item->image*s->cell+dx*s->cell/w];
            if((int64_t)pixel!=s->key) fb_pixel(fb,x+dx,y+dy,pixel);
        }
    }
}
static void equip_selected(int id)
{
    int slot=hero_item_slot(id),ok;
    if(slot<0) {message("This item cannot be equipped.");return;}
    if(hero_equipped(&g_hero,slot)==id) ok=hero_unequip(&g_hero,slot);
    else ok=hero_equip(&g_hero,id);
    message(ok?"Equipment changed.":"Level, class or training prevents equip.");
}
static void transact(int id)
{
    int price=g_world.items[id].gp;
    if(price<=0) {message("This item cannot be traded.");return;}
    if(panel.selling) {
        price/=2;
        if(hero_wallet_limit(&g_hero)-g_hero.gold<price) {message("Your gold wallet is full.");return;}
        if(!hero_take_item(&g_hero,id,1)) {message("You do not own that item.");return;}
        hero_add_gold(&g_hero,price);
        wos_log_event("shop_sell","item=%d gold=%lld",id,(long long)g_hero.gold);
        message("Sold one item.");
    } else {
        if(g_hero.gold<price) {message("Not enough gold.");return;}
        if(!hero_give_item(&g_hero,id,1)) {message("You cannot carry another of this item.");return;}
        hero_add_gold(&g_hero,-price);
        wos_log_event("shop_buy","item=%d gold=%lld",id,(long long)g_hero.gold);
        message("Bought one item.");
    }
    rebuild();
}
static void use_selected(int id)
{
    message(hero_use_item(&g_hero,id,panel.ability)?"Item used.":"This item cannot be used now.");
    rebuild();
}
static void train_selected(int requested)
{
    int index=panel.selected,pp=panel.training_kind==HERO_TRAIN_HAND?g_hero.hand_pp[index]:g_hero.element_pp[index];
    int amount=hero_train_limit(&g_hero,panel.training_kind,index)-pp;
    if(amount>requested) amount=requested;
    if(g_hero.pp<amount) amount=(int)g_hero.pp;
    message(amount>0 && hero_train(&g_hero,panel.training_kind,index,amount)?"Training improved.":"No PP available, or class cap reached.");
}
void panel_update(const Input *in)
{
    int i,id=0,activate=0,cols=panel.kind==PANEL_TRAIN || panel.kind==PANEL_SPELLS?1:6;
    if(!panel_active()) return;
    if(in->pressed[PLAT_KEY_ESCAPE] || clicked(in,close_rect)) {panel_close();return;}
    if(panel.kind!=PANEL_SHOP) {
        for(i=1;i<=5;i++) if(clicked(in,(Rect){8+(i-1)*70,32,68,24})) {panel_open((PanelKind)i);return;}
    } else if(in->pressed[PLAT_KEY_TAB] || clicked(in,(Rect){8,32,100,24}) || clicked(in,(Rect){112,32,100,24})) {
        panel.selling=clicked(in,(Rect){8,32,100,24})?0:clicked(in,(Rect){112,32,100,24})?1:!panel.selling;
        panel.selected=0;message("");rebuild();return;
    }
    if(panel.kind==PANEL_STATS) {
        if(in->pressed['t'] || clicked(in,(Rect){8,384,120,24})) panel_open(PANEL_TRAIN);
        return;
    }
    if(in->pressed[PLAT_KEY_UP]) panel.selected-=cols;
    if(in->pressed[PLAT_KEY_DOWN]) panel.selected+=cols;
    if(in->pressed[PLAT_KEY_LEFT]) panel.selected--;
    if(in->pressed[PLAT_KEY_RIGHT]) panel.selected++;
    if(in->pressed[PLAT_KEY_PAGEUP] || in->wheel>0 || clicked(in,(Rect){8,260,44,20})) panel.selected-=page_size();
    if(in->pressed[PLAT_KEY_PAGEDOWN] || in->wheel<0 || clicked(in,(Rect){308,260,44,20})) panel.selected+=page_size();
    selection_bound();
    if(panel.kind==PANEL_TRAIN) {
        if(in->pressed[PLAT_KEY_TAB] || clicked(in,(Rect){8,96,168,24}) || clicked(in,(Rect){184,96,168,24}))
            panel.training_kind=clicked(in,(Rect){8,96,168,24})?HERO_TRAIN_HAND:clicked(in,(Rect){184,96,168,24})?HERO_TRAIN_ELEMENT:!panel.training_kind;
        for(i=0;i<8;i++) if(clicked(in,(Rect){8,128+i*26,344,24})) panel.selected=i;
        if(in->pressed['t'] || in->pressed[PLAT_KEY_RETURN] || clicked(in,(Rect){8,384,120,24})) train_selected(100);
        if(clicked(in,(Rect){136,384,120,24})) train_selected(1);
        return;
    }
    if(panel.kind==PANEL_EQUIP) for(i=0;i<7;i++) if(clicked(in,(Rect){8+i*49,64,48,60})) {
        message(hero_unequip(&g_hero,slots[i])?"Slot unequipped.":"That slot is empty.");return;
    }
    for(i=0;i<page_size() && panel.first+i<panel.count;i++) {
        Rect r=panel.kind==PANEL_SPELLS?(Rect){8,64+i*24,344,23}:
               (Rect){8+(i%6)*58,(panel.kind==PANEL_EQUIP?130:64)+(i/6)*64,56,62};
        if(clicked(in,r)) {panel.selected=panel.first+i;activate=1;}
    }
    if(panel.selected<panel.count) id=panel.ids[panel.selected];
    if(!id) return;
    if(panel.kind==PANEL_SPELLS) {
        if(in->pressed['l'] || in->pressed[PLAT_KEY_RETURN] || clicked(in,(Rect){8,384,160,24})) {
            message(hero_learn_spell(&g_hero,id)?"Spell learned.":"Already known, or cannot learn yet.");
            rebuild();
        }
        return;
    }
    if(panel.kind==PANEL_SHOP) {
        if(in->pressed[PLAT_KEY_RETURN] || in->pressed[panel.selling?'s':'b'] || clicked(in,(Rect){8,384,120,24})) transact(id);
    } else {
        if(in->pressed['e'] || clicked(in,(Rect){96,384,80,24}) || (panel.kind==PANEL_EQUIP && (activate || in->pressed[PLAT_KEY_RETURN]))) equip_selected(id);
        else if(in->pressed['d'] || clicked(in,(Rect){184,384,80,24})) {message(hero_take_item(&g_hero,id,1)?"Dropped one item.":"Nothing to drop.");rebuild();}
        else if(in->pressed['u'] || in->pressed[PLAT_KEY_RETURN] || clicked(in,(Rect){8,384,80,24}) || activate) use_selected(id);
        if(in->pressed['c'] || clicked(in,(Rect){8,358,256,20})) panel.ability=(panel.ability+1)%HERO_ABILITIES;
    }
}
static void background(Framebuffer *fb)
{
    int x,y;
    fb_fill(fb,(Rect){0,0,364,416},0x242019);
    if(panel.background.pixels) for(y=0;y<416;y+=panel.background.h) for(x=0;x<364;x+=panel.background.w) fb_blit(fb,&panel.background,x,y,-1);
    fb_blend(fb,(Rect){4,4,356,408},0x080a10,170);
    /* Keep the original border's corner and edge pixels, tiling rather than
     * scaling the artwork into the fixed-size overlay. */
    if(panel.border.pixels && panel.border.w>=16 && panel.border.h>=16) {
        const Image *im=&panel.border;
        for(x=0;x<364;x+=8) {
            fb_blit_sub(fb,im,(Rect){8,0,8,4},x,0,0,im->pixels[0]);
            fb_blit_sub(fb,im,(Rect){8,im->h-4,8,4},x,412,0,im->pixels[0]);
        }
        for(y=4;y<412;y+=8) {
            fb_blit_sub(fb,im,(Rect){0,8,4,8},0,y,0,im->pixels[0]);
            fb_blit_sub(fb,im,(Rect){im->w-4,8,4,8},360,y,0,im->pixels[0]);
        }
    }
    fb_rect(fb,(Rect){3,3,358,410},0xa8915d);
}
static void stats_render(Framebuffer *fb)
{
    char text[256];int i;
    int64_t tnl=hero_xp_for_level(&g_hero,g_hero.level+1)-g_hero.xp;
    if(tnl<0 || g_hero.level>=100) tnl=0;
    snprintf(text,sizeof text,"%.30s\n\n%.30s  Level %d\n\nHP %d / %d     MP %d / %d\n\nXP %lld   TNL %lld\n\nGold %lld   PP %lld",g_hero.name,g_world.classes[g_hero.klass].name,g_hero.level,g_hero.hp,g_hero.max_hp,g_hero.mp,g_hero.max_mp,(long long)g_hero.xp,(long long)tnl,(long long)g_hero.gold,(long long)g_hero.pp);
    font_wrap(fb,(Rect){16,72,332,128},text,INK);
    for(i=0;i<HERO_ABILITIES;i++) {
        snprintf(text,sizeof text,"%s  %3d / %3d",ability_names[i],hero_ability(&g_hero,i),g_world.classes[g_hero.klass].max_ability[i]);
        font_draw(fb,24,218+i*22,text,INK);
    }
    snprintf(text,sizeof text,"Offense %d   Defense %d",hero_offense(&g_hero),hero_defense(&g_hero));font_draw(fb,16,342,text,INK);
    button(fb,(Rect){8,384,120,24},"T: Training",0);
}
static void train_render(Framebuffer *fb)
{
    int i;char text[160];
    snprintf(text,sizeof text,"Wallet: %lld PP",(long long)g_hero.pp);font_draw(fb,16,72,text,INK);
    button(fb,(Rect){8,96,168,24},"Hands",panel.training_kind==HERO_TRAIN_HAND);
    button(fb,(Rect){184,96,168,24},"Elements",panel.training_kind==HERO_TRAIN_ELEMENT);
    for(i=0;i<8;i++) {
        int pp=panel.training_kind==HERO_TRAIN_HAND?g_hero.hand_pp[i]:g_hero.element_pp[i];
        const char *name=panel.training_kind==HERO_TRAIN_HAND?g_world.hands[i].name:g_world.elements[i].name;
        int cap=hero_train_limit(&g_hero,panel.training_kind,i);
        Rect r={8,128+i*26,344,24};
        fb_fill(fb,r,i==panel.selected?0x57482b:0x18191c);fb_rect(fb,r,0x74613e);
        snprintf(text,sizeof text,"%.12s L%d %d/%d",*name?name:"None",hero_pp_level(pp),pp,cap);
        font_draw(fb,12,r.y+8,text,INK);
    }
    button(fb,(Rect){8,384,120,24},"T: +100 PP",0);
    button(fb,(Rect){136,384,120,24},"+1 PP",0);
}
static void item_description(Framebuffer *fb,int id)
{
    const ItemDef *item=&g_world.items[id];char text[192];int slot=hero_item_slot(id);
    snprintf(text,sizeof text,"%.40s",item->name);font_draw(fb,12,286,text,INK);
    snprintf(text,sizeof text,"Level %d  Buy %d  Sell %d  Own %d",item->level,item->gp,item->gp/2,hero_item_count(&g_hero,id));font_draw(fb,12,300,text,0xd7caac);
    if(slot>=0) {
        int old=hero_equipped(&g_hero,slot),attack=item->attack,defense=item->defense;
        if(old==id) {attack=-attack;defense=-defense;}
        else if(old>0 && old<WORLD_MAX_ITEMS) {attack-=g_world.items[old].attack;defense-=g_world.items[old].defense;}
        snprintf(text,sizeof text,"%s: Off %+d  Def %+d",old==id?"Unequip delta":"Equip delta",attack,defense);
        font_draw(fb,12,314,text,INK);
    }
    font_wrap(fb,(Rect){12,328,340,28},item->description,0xd7caac);
}
void panel_render(Framebuffer *fb)
{
    Rect clip;int i;char text[192];
    if(!panel_active()) return;
    clip=fb->clip;fb_clip_intersect(fb,(Rect){0,0,364,416});background(fb);
    font_draw(fb,12,14,titles[panel.kind],INK);
    if(panel.kind==PANEL_SHOP) {
        snprintf(text,sizeof text,"Gold: %lld",(long long)g_hero.gold);font_draw(fb,164,14,text,INK);
        button(fb,(Rect){8,32,100,24},"Buy",!panel.selling);button(fb,(Rect){112,32,100,24},"Sell",panel.selling);
    } else {
        static const char *const tabs[]={"Items","Spells","Equip","Stats","Train"};
        for(i=0;i<5;i++) button(fb,(Rect){8+i*70,32,68,24},tabs[i],panel.kind==(PanelKind)(i+1));
    }
    if(panel.kind==PANEL_STATS) stats_render(fb);
    else if(panel.kind==PANEL_TRAIN) train_render(fb);
    else {
        if(panel.kind==PANEL_EQUIP) for(i=0;i<7;i++) {
            static const int glyphs[]={1,2,11,12,13,14,3};
            int id=hero_equipped(&g_hero,slots[i]);Rect r={8+i*49,64,48,60};
            fb_fill(fb,r,0x202128);fb_rect(fb,r,0xa8915d);
            if(id) icon(fb,id,r.x,r.y);
            else if(panel.equip_icons.pixels) fb_blit_sub(fb,&panel.equip_icons,(Rect){glyphs[i]*16,0,16,16},r.x+16,r.y+16,0,panel.equip_icons.pixels[0]);
            font_draw(fb,r.x,r.y+50,slot_names[i],INK);
        }
        for(i=0;i<page_size() && panel.first+i<panel.count;i++) {
            int id=panel.ids[panel.first+i];
            if(panel.kind==PANEL_SPELLS) {
                const SpellDef *spell=&g_world.spells[id];Rect r={8,64+i*24,344,23};
                fb_fill(fb,r,panel.first+i==panel.selected?0x57482b:0x18191c);
                snprintf(text,sizeof text,"%c %-25.25s %4d MP",hero_spell_known(&g_hero,id)?'*':'+',spell->name,spell->mp_cost);font_draw(fb,12,r.y+8,text,INK);
            } else {
                Rect r={8+(i%6)*58,(panel.kind==PANEL_EQUIP?130:64)+(i/6)*64,56,62};
                fb_fill(fb,r,0x1a1c23);fb_rect(fb,r,panel.first+i==panel.selected?0xffd477:0x665638);
                icon(fb,id,r.x+4,r.y+2);
                snprintf(text,sizeof text,"x%d",hero_item_count(&g_hero,id));font_draw(fb,r.x+2,r.y+52,text,INK);
            }
        }
        if(!panel.count) font_draw(fb,24,174,panel.kind==PANEL_SPELLS?"No known spells.":"No items.",INK);
        button(fb,(Rect){8,260,44,20},"<",0);button(fb,(Rect){308,260,44,20},">",0);
        snprintf(text,sizeof text,"%d / %d",panel.count?panel.selected+1:0,panel.count);font_draw(fb,128,266,text,INK);
        if(panel.kind==PANEL_SPELLS) {
            if(panel.count) {
                const SpellDef *s=&g_world.spells[panel.ids[panel.selected]];
                const char *element=s->element>=0 && s->element<WORLD_MAX_ELEMENTS?g_world.elements[s->element].name:"";
                snprintf(text,sizeof text,"%.40s\n\n%.20s  MP %d  PP %d\n\nPower %d  Minimum level %d\n%s\n\nSpells are cast during battle.",s->name,element,s->mp_cost,s->pp_cost,s->damage,s->min_level,s->all_targets?"Affects all targets.":(s->flags&4)?"Self only.":"One target.");
                font_wrap(fb,(Rect){12,288,340,82},text,INK);
                button(fb,(Rect){8,384,160,24},hero_spell_known(&g_hero,panel.ids[panel.selected])?"Known":"L: Learn spell",0);
            } else font_wrap(fb,(Rect){12,288,340,64},"Earn PP and train elements to learn spells. Known spells are marked *; available spells are marked +.",INK);
        } else {
            if(panel.count) item_description(fb,panel.ids[panel.selected]);
            if(panel.kind==PANEL_SHOP) button(fb,(Rect){8,384,120,24},panel.selling?"S: Sell one":"B: Buy one",0);
            else {
                snprintf(text,sizeof text,"C: Seed choice %s",ability_names[panel.ability]);font_draw(fb,12,360,text,0xd7caac);
                button(fb,(Rect){8,384,80,24},"U: Use",0);button(fb,(Rect){96,384,80,24},"E: Equip",0);button(fb,(Rect){184,384,80,24},"D: Drop",0);
            }
        }
    }
    if(panel.message[0]) {fb_fill(fb,(Rect){8,368,344,12},0x151515);font_wrap(fb,(Rect){12,370,336,10},panel.message,0xffd477);}
    button(fb,close_rect,"Close",0);fb_clip(fb,clip);
}
