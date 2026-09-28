/* Retail dialogs are adapted to the 364x416 main viewport. OFFER2 ordering and
 * filtering: FUN_0047df.. case 0x12; sell price: gp/2 (FUN_00403349 users).
 * Art class mapping: FUN_00482431; equipment deltas: all.c:9645-9659.
 * Item use lives in items.c (FUN_004A6353); the bag and pen live there too. */
#include "panels.h"
#include "hero.h"
#include "items.h"
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
static const char *const titles[] = {"", "Items", "Spells", "Equipment", "Statistics",
                                     "Training", "Shop", "Trophy Bag", "Pet Pen"};
static const char *const ability_names[] = {"STR","WIS","STA","AGI","DEX"};
static const int slots[] = {HERO_SLOT_HELMET,HERO_SLOT_ARMOR,HERO_SLOT_BOOTS,HERO_SLOT_SHIELD,HERO_SLOT_RING,HERO_SLOT_AMULET,HERO_SLOT_RIGHT_HAND};
/* The item class each of the seven displayed slots holds; -1 is the right hand,
 * which the original does not label (FUN_00482431 forwards classes 2..9 to the
 * HANDS names and the paper doll leaves the slot blank). */
static const int slot_classes[] = {ITEM_HELMET,ITEM_ARMOR,ITEM_BOOTS,ITEM_SHIELD,
                                   ITEM_RING,ITEM_AMULET,-1};
static struct {
    PanelKind kind;
    int selected, first, count, ids[WORLD_MAX_ITEMS];
    int offers[SHOP_MAX], offer_count, selling, training_kind, ability;
    int bag_drag, pen_selected;
    char message[128];
    Image background, border, equip_icons, trophy_art;
    struct { char name[48]; Sheet sheet; } icons[ICON_CACHE];
    int icon_count, next_icon;
} panel;
/* The original opens the trophy bag (dialog 0xE7) and the pet pen
 * (FUN_00412716, petButtons.bmp) from the main button bar; on these in-frame
 * panels they are reachable from the Items/Equipment header. */
static const Rect bag_button = {150,8,100,20};
static const Rect pen_button = {254,8,102,20};
static int clicked(const Input *in, Rect r)
{
    return (in->mouse_pressed & 2u) && in->mouse_x>=r.x && in->mouse_y>=r.y &&
           in->mouse_x<r.x+r.w && in->mouse_y<r.y+r.h;
}
static int held(const Input *in, Rect r)
{
    return (in->mouse_down & 2u) && in->mouse_x>=r.x && in->mouse_y>=r.y &&
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
    image_free(&panel.trophy_art);
    for(i=0;i<panel.icon_count;i++) sheet_free(&panel.icons[i].sheet);
    memset(&panel,0,sizeof panel);
}
int panel_active(void) { return panel.kind!=PANEL_NONE; }
void panel_open(PanelKind kind)
{
    panel_close();
    if(kind<=PANEL_NONE || kind>=PANEL_COUNT) return;
    panel.kind=kind;
    art_load(&panel.background,kind==PANEL_EQUIP?"bkEquip.jpg":kind==PANEL_SPELLS?"bkSpell.jpg":
             (kind==PANEL_ITEMS || kind==PANEL_SHOP)?"itemTable.jpg":
             kind==PANEL_TROPHY?"petPen.jpg":"bkBook.jpg");
    art_load(&panel.border,"shopBorder256.bmp");
    if(kind==PANEL_EQUIP) art_load(&panel.equip_icons,"equipIcons.bmp");
    if(kind==PANEL_TROPHY) art_load(&panel.trophy_art,"trophy.bmp");
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
void panel_open_pets(void) { panel_open(PANEL_PET); }
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
/* FUN_0048BA..: the seller's class NO_GIFTS (levels.txt, class record +0x1ABD8)
 * refuses every sale, and the original's menu is greyed out with 0x16. */
static int gifts_forbidden(void)
{
    return g_hero.klass>=0 && g_hero.klass<WORLD_MAX_CLASSES && g_world.classes[g_hero.klass].no_gifts;
}
static void transact(int id)
{
    int price=g_world.items[id].gp;
    if(price<=0) {message("This item cannot be traded.");return;}
    if(panel.selling) {
        price/=2;
        /* FUN_0042BAA8: the wallet clamps silently, so the original refuses the
         * sale outright when the proceeds would not fit. */
        if(hero_wallet_limit(&g_hero)-g_hero.gold<price) {message("Your gold wallet is full.");return;}
        if(gifts_forbidden()) {message("Your class may not give items away.");return;}
        if(!hero_take_item(&g_hero,id,1)) {message("You do not own that item.");return;}
        hero_add_gold(&g_hero,price);
        wos_log_event("shop_sell","item=%d gold=%lld %s",id,(long long)g_hero.gold,world_gold_name());
        message("Sold one item.");
    } else {
        /* FUN_0042BAF3(hero,0) < price is the original's "not enough gold". */
        if(g_hero.gold<price) {message("Not enough gold.");return;}
        /* FUN_00403349: the carry limit is level+1 (capped 100) or maxCount. */
        if(!hero_give_item(&g_hero,id,1)) {message("You cannot carry another of this item.");return;}
        hero_add_gold(&g_hero,-price);
        wos_log_event("shop_buy","item=%d gold=%lld %s",id,(long long)g_hero.gold,world_gold_name());
        message("Bought one item.");
    }
    rebuild();
}
/* FUN_004A6353 through items.c. Class 5 arms a throw; class 4 travels; class
 * 201 opens the page; 0/1/100..105 apply and spend a copy. */
static void use_selected(int id)
{
    int armed=items_throw_armed();
    if(items_use(id,panel.ability)) {
        const char *text=items_last_message();
        if(panel.kind==PANEL_ITEMS && id>0 && id<WORLD_MAX_ITEMS && g_world.items[id].klass==ITEM_THROWABLE)
            message("Item readied. Attack to throw it.");
        else message(text[0]?text:"Item used.");
    } else message("This item cannot be used now.");
    if(armed && !items_throw_armed()) items_clear_throw();
}
static void train_selected(int requested)
{
    int index=panel.selected,pp=panel.training_kind==HERO_TRAIN_HAND?g_hero.hand_pp[index]:g_hero.element_pp[index];
    int amount=hero_train_limit(&g_hero,panel.training_kind,index)-pp;
    if(amount>requested) amount=requested;
    if(g_hero.pp<amount) amount=(int)g_hero.pp;
    message(amount>0 && hero_train(&g_hero,panel.training_kind,index,amount)?"Training improved.":"No PP available, or class cap reached.");
}
/* ------------------------------------------------------- trophy bag (0xE7) */
static Rect bag_cell_rect(int slot)
{
    int width=0,height=0,cell;
    trophy_bag_size(&width,&height);
    cell=TROPHY_BAG_CELL;
    if(width<=0) width=1;
    return (Rect){20+(slot%width)*cell, 28+(slot/width)*cell, cell, cell};
}
static void bag_update(const Input *in)
{
    int width=0,height=0,cells,slot;
    trophy_bag_size(&width,&height);
    cells=width*height;
    for(slot=0;slot<cells && slot<TROPHY_BAG_SLOTS;slot++) {
        Rect r=bag_cell_rect(slot);
        if(held(in,r)) { if(panel.bag_drag<0) panel.bag_drag=slot; continue; }
    }
    if(!(in->mouse_down & 2u) && panel.bag_drag>=0) {
        int target=trophy_bag_slot_at(in->mouse_x-20,in->mouse_y-28);
        /* FUN_0047075A: a drop onto a different cell swaps the two stacks. */
        if(target>=0 && target<cells && target!=panel.bag_drag) {
            uint32_t tmp=g_hero.trophy_bag[panel.bag_drag];
            g_hero.trophy_bag[panel.bag_drag]=g_hero.trophy_bag[target];
            g_hero.trophy_bag[target]=tmp;
            wos_log_event("trophy_slot_moved","from=%d to=%d",panel.bag_drag,target);
        }
        panel.bag_drag=-1;
    }
    if(in->pressed['c']) {trophy_bag_pack();message("Trophy bag tidied.");}
}
static void bag_render(Framebuffer *fb)
{
    char text[64];
    int width=0,height=0,cells,slot,id,count,drag=panel.bag_drag;
    trophy_bag_size(&width,&height);
    cells=width*height;
    if(cells<0) cells=0;
    if(!cells) {
        font_wrap(fb,(Rect){20,120,324,48},"Your trophy bag is empty. num.TrophyBagWidth and num.TrophyBagHeight decide how big it is.",INK);
        return;
    }
    for(slot=0;slot<cells && slot<TROPHY_BAG_SLOTS;slot++) {
        Rect r=bag_cell_rect(slot);
        if(r.y+r.h>400) break;
        fb_fill(fb,r,slot==drag?0x6b5a2c:0x1b1d24);
        fb_rect(fb,r,slot==drag?0xffd477:0x404040);
        if(trophy_bag_get(slot,&id,&count) && id>0 && id<WORLD_MAX_TROPHIES) {
            const TrophyDef *t=&g_world.trophies[id];
            if(t->used && panel.trophy_art.pixels && t->image_index>=0) {
                int cell_w=panel.trophy_art.h;
                if(cell_w>0 && t->image_index < panel.trophy_art.w/cell_w)
                    fb_blit_sub(fb,&panel.trophy_art,(Rect){t->image_index*cell_w,0,cell_w,cell_w},
                                r.x+4,r.y+4,0,panel.trophy_art.pixels[0]);
            }
            if(count>1) {
                snprintf(text,sizeof text,"x%d",count);
                font_draw(fb,r.x+r.w-8-font_width(text),r.y+r.h-10,text,INK);
            }
        }
    }
    snprintf(text,sizeof text,"%d of %d slots used, %d free",trophy_bag_used(),cells,trophy_bag_free());
    font_draw(fb,20,412-font_width(""),text,0xd7caac);
    button(fb,(Rect){8,384,120,24},"C: Compact",0);
}
/* ------------------------------------------------------------ pet pen */
static void pen_update(const Input *in)
{
    int i,n=pet_count(),index=0;
    for(i=0;i<PET_PEN_SLOTS;i++) {
        Pet *p=pet_at(i);
        if(!p) continue;
        if(clicked(in,(Rect){20,64+index*40,240,36})) {panel.pen_selected=i;return;}
        ++index;
    }
    if(n==0) return;
    if(in->pressed['u'] || in->pressed[PLAT_KEY_RETURN] || clicked(in,(Rect){8,384,120,24})) {
        /* FUN_004142F2: Call. 0 = call, the arm that sounds petCall.wav and starts
         * the 200-second lead timer. */
        if(panel.pen_selected!=0) { message("Call the first pet in the pen."); return; }
        if(pet_trigger(PET_CALL)<0) message(items_last_message());
        else message(items_last_message()[0]?items_last_message():"Pet called.");
    } else if(in->pressed['d'] || clicked(in,(Rect){136,384,120,24})) {
        /* FUN_004142F2: Recall, the arm that drops the pet out of the fight. */
        if(panel.pen_selected!=0) { message("Recall the first pet in the pen."); return; }
        if(pet_trigger(PET_RECALL)<0) message("Your pet is not in battle.");
        else message("Pet recalled.");
    } else if(in->pressed['x'] || clicked(in,(Rect){264,384,92,24})) {
        if(!pet_release(panel.pen_selected)) message("No pet selected.");
        else message("Pet released back to the wild.");
    }
}
static void pen_render(Framebuffer *fb)
{
    char text[160];
    int i,n=0;
    if(!pet_count()) {
        font_wrap(fb,(Rect){20,120,324,48},"No pets in the pen. A class-200 item bought from a shop is a pet voucher.",INK);
    } else {
        for(i=0;i<PET_PEN_SLOTS;i++) {
            Pet *p=pet_at(i);
            Rect r;
            if(!p) continue;
            r=(Rect){20,64+n*40,240,36};
            if(r.y+r.h>380) break;
            fb_fill(fb,r,i==panel.pen_selected?0x57482b:0x18191c);
            fb_rect(fb,r,i==panel.pen_selected?0xffd477:0x74613e);
            snprintf(text,sizeof text,"%s  L%d  %d/%d",g_world.monsters[p->monster_id].name,
                     p->level,p->hp,p->max_hp);
            font_draw(fb,r.x+8,r.y+14,text,INK);
            ++n;
        }
    }
    snprintf(text,sizeof text,"Pets out: %d, %d",(int)g_hero.pet_ids[0],(int)g_hero.pet_ids[1]);
    font_draw(fb,20,352,text,0xd7caac);
    button(fb,(Rect){8,384,120,24},"U: Call pet",0);
    button(fb,(Rect){136,384,120,24},"D: Recall",0);
    button(fb,(Rect){264,384,92,24},"X: Release",0);
}
void panel_update(const Input *in)
{
    int i,id=0,activate=0,cols=panel.kind==PANEL_TRAIN || panel.kind==PANEL_SPELLS?1:6;
    if(!panel_active()) return;
    if(in->pressed[PLAT_KEY_ESCAPE] || clicked(in,close_rect)) {panel_close();return;}
    if(panel.kind==PANEL_TROPHY) {bag_update(in);return;}
    if(panel.kind==PANEL_PET) {pen_update(in);return;}
    if((panel.kind==PANEL_ITEMS || panel.kind==PANEL_EQUIP) &&
       (in->pressed['g'] || clicked(in,bag_button))) {panel_open(PANEL_TROPHY);return;}
    if((panel.kind==PANEL_ITEMS || panel.kind==PANEL_EQUIP) &&
       (in->pressed['p'] || clicked(in,pen_button))) {panel_open(PANEL_PET);return;}
    if(panel.kind!=PANEL_SHOP) {
        for(i=1;i<=5;i++) if(clicked(in,(Rect){8+(i-1)*70,32,68,24})) {panel_open((PanelKind)i);return;}
    } else if(in->pressed[PLAT_KEY_TAB] || clicked(in,(Rect){8,32,100,24}) || clicked(in,(Rect){112,32,100,24})) {
        panel.selling=clicked(in,(Rect){8,32,100,24})?0:clicked(in,(Rect){112,32,100,24})?1:!panel.selling;
        panel.selected=0;message("");rebuild();return;
    }
    if(panel.kind==PANEL_STATS) {
        if(in->pressed['t'] || clicked(in,(Rect){8,384,120,24})) panel_open(PANEL_TRAIN);
        /* Class-105 seeds leave points in the pool; spend them here. */
        if(items_attr_pool()>0) {
            for(i=0;i<HERO_ABILITIES;i++) if(clicked(in,(Rect){24,218+i*22,332,20})) {
                if(items_attr_assign(i)) message("Ability assigned.");
                else message("That ability is already at its cap.");
            }
        }
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
        else if(in->pressed['d'] || clicked(in,(Rect){184,384,80,24})) {
            /* FUN_0040E134: an equipped item must come off before it is sold. */
            int slot=hero_item_slot(id);
            if(slot>=0 && hero_equipped(&g_hero,slot)==id) message("You will have to un-equip this item first.");
            else message(hero_take_item(&g_hero,id,1)?"Dropped one item.":"Nothing to drop.");
            rebuild();
        }
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
    snprintf(text,sizeof text,"%.30s\n\n%.30s  Level %d\n\nHP %d / %d     MP %d / %d\n\nXP %lld   TNL %lld\n\n%s %lld   PP %lld",g_hero.name,g_world.classes[g_hero.klass].name,g_hero.level,g_hero.hp,g_hero.max_hp,g_hero.mp,g_hero.max_mp,(long long)g_hero.xp,(long long)tnl,world_gold_name(),(long long)g_hero.gold,(long long)g_hero.pp);
    font_wrap(fb,(Rect){16,72,332,128},text,INK);
    for(i=0;i<HERO_ABILITIES;i++) {
        snprintf(text,sizeof text,"%s  %3d / %3d",ability_names[i],hero_ability(&g_hero,i),g_world.classes[g_hero.klass].max_ability[i]);
        font_draw(fb,24,218+i*22,text,INK);
    }
    if(items_attr_pool()>0) {
        snprintf(text,sizeof text,"%d unassigned - click an ability",items_attr_pool());
        font_draw(fb,150,210,text,0xffd477);
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
    snprintf(text,sizeof text,"Level %d  Buy %d  Sell %d  Own %d/%d",item->level,item->gp,item->gp/2,
             hero_item_count(&g_hero,id),hero_item_limit(&g_hero,id));
    font_draw(fb,12,300,text,0xd7caac);
    if(slot>=0) {
        int old=hero_equipped(&g_hero,slot),attack=item->attack,defense=item->defense;
        if(old==id) {attack=-attack;defense=-defense;}
        else if(old>0 && old<WORLD_MAX_ITEMS) {attack-=g_world.items[old].attack;defense-=g_world.items[old].defense;}
        snprintf(text,sizeof text,"%s: Off %+d  Def %+d",old==id?"Unequip delta":"Equip delta",attack,defense);
        font_draw(fb,12,314,text,INK);
    } else {
        /* FUN_004A6353: say what using it will actually do. */
        const char *kind=item->klass==ITEM_POTION?"Restores HP/MP":
                         item->klass==ITEM_ANTIDOTE?"Cures a disease":
                         item->klass==ITEM_TRAVEL?"Travel ticket":
                         item->klass==ITEM_THROWABLE?"Thrown in battle":
                         item->klass==ITEM_HTML?"Opens a page":
                         item->klass==ITEM_PET?"Pet voucher":
                         (item->klass>=ITEM_ATTR_STRENGTH&&item->klass<=ITEM_ATTR_ALL)?"Raises an ability":
                         item->klass>=ITEM_HELMET&&item->klass<=ITEM_AMULET?"Worn":"Quest item";
        snprintf(text,sizeof text,"Class %d  %s",item->klass,kind);
        font_draw(fb,12,314,text,0xd7caac);
    }
    font_wrap(fb,(Rect){12,328,340,28},item->description,0xd7caac);
}
void panel_render(Framebuffer *fb)
{
    Rect clip;int i;char text[192];
    if(!panel_active()) return;
    clip=fb->clip;fb_clip_intersect(fb,(Rect){0,0,364,416});background(fb);
    font_draw(fb,12,14,titles[panel.kind],INK);
    if(panel.kind==PANEL_TROPHY) {bag_render(fb);}
    else if(panel.kind==PANEL_PET) {pen_render(fb);}
    else if(panel.kind==PANEL_SHOP) {
        snprintf(text,sizeof text,"%s: %lld",world_gold_name(),(long long)g_hero.gold);font_draw(fb,164,14,text,INK);
        button(fb,(Rect){8,32,100,24},"Buy",!panel.selling);button(fb,(Rect){112,32,100,24},"Sell",panel.selling);
    } else {
        static const char *const tabs[]={"Items","Spells","Equip","Stats","Train"};
        for(i=0;i<5;i++) button(fb,(Rect){8+i*70,32,68,24},tabs[i],panel.kind==(PanelKind)(i+1));
        if(panel.kind==PANEL_ITEMS || panel.kind==PANEL_EQUIP) {
            button(fb,bag_button,"G: Trophies",0);
            button(fb,pen_button,"P: Pet pen",0);
        }
    }
    if(panel.kind==PANEL_STATS) stats_render(fb);
    else if(panel.kind==PANEL_TRAIN) train_render(fb);
    else if(panel.kind!=PANEL_TROPHY && panel.kind!=PANEL_PET) {
        if(panel.kind==PANEL_EQUIP) for(i=0;i<7;i++) {
            static const int glyphs[]={1,2,11,12,13,14,3};
            int id=hero_equipped(&g_hero,slots[i]);Rect r={8+i*49,64,48,60};
            fb_fill(fb,r,0x202128);fb_rect(fb,r,0xa8915d);
            if(id) icon(fb,id,r.x,r.y);
            else if(panel.equip_icons.pixels) fb_blit_sub(fb,&panel.equip_icons,(Rect){glyphs[i]*16,0,16,16},r.x+16,r.y+16,0,panel.equip_icons.pixels[0]);
            /* FUN_00482431: the +EQUIP name for this slot, right hands unnamed. */
            font_draw(fb,r.x,r.y+50,slot_classes[i]<0?"Hand":items_class_slot_name(slot_classes[i]),INK);
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

/* ------------------------------------------------- semantic dialog ops */
/* Original control ids, for Core's `at <ms> dialog` op and the oracle harness.
 *
 *   dialog 179 (0xB3) EQUIPMENT   FUN_0040420E / FUN_0040D872 / FUN_0040DBBA
 *     2224 (0x8B0) popup EQUIP / UNEQUIP          FUN_0040DBBA
 *     2225 (0x8B1) popup SELL one                 FUN_0040E134
 *     2226 (0x8B2) popup SELL all of this item    FUN_0040DBBA
 *     1337 (0x539) item list control id
 *     list notifications (WM_NOTIFY 0x536 NMHDR.code):
 *        -2 NM_CLICK, -3 NM_DBLCLK, -5 NM_RBUTTONUP,
 *      -101 LVN_ITEMCHANGED, -108 LVN_GETINFOTIPW
 *   dialog 200 (0xC8) ITEMS       FUN_0045E493 / FUN_0045E8B3
 *     1337 (0x539) item list control id
 *     2225 (0x8B1) popup Sell It                  FUN_0045E8B3
 *     2226 (0x8B2) popup Sell All                 FUN_0045E8B3
 *   dialog 212 (0xD4) SHOP        FUN_00402803 / FUN_00402FBF
 *     1337 (0x539) item-for-sale list (columns 140/90/60)
 *   dialog 231 (0xE7) TROPHY BAG  FUN_0046FF98, no controls: 40 px cells
 *   dialog 149 (0x95) STATS/TRAIN FUN_00448E0F
 *   the class-105 ability chooser  FUN_004A6353 case 0x69 (DoModal, no id read)
 */
enum {
    DLG_EQUIP = 179, DLG_ITEMS = 200, DLG_SHOP = 212, DLG_TROPHY = 231, DLG_STATS = 149
};
enum {
    IDC_ITEM_LIST = 1337,
    IDC_EQUIP = 2224, IDC_SELL_ONE = 2225, IDC_SELL_ALL = 2226
};

static void select_by_id(int item_id)
{
    int i;
    for(i=0;i<panel.count;i++) if(panel.ids[i]==item_id) {panel.selected=i;selection_bound();return;}
}

int panel_dialog_op(const DialogOp *op)
{
    if(!op) return -1;
    switch(op->id) {
    case DLG_ITEMS:
    case DLG_EQUIP:
        if(!panel_active() || (panel.kind!=PANEL_ITEMS && panel.kind!=PANEL_EQUIP)) panel_open((PanelKind)op->id);
        if(op->ctrl==IDC_ITEM_LIST) { select_by_id(op->value); return 0; }
        switch(op->ctrl) {
        case IDC_EQUIP:
            if(!op->ok) return 0;
            if(panel.selected<panel.count) equip_selected(panel.ids[panel.selected]);
            return 0;
        case IDC_SELL_ONE:
            if(!op->ok) return 0;
            if(panel.selected<panel.count) transact(panel.ids[panel.selected]);
            return 0;
        case IDC_SELL_ALL: {
            int id=panel.selected<panel.count?panel.ids[panel.selected]:0,guard=0;
            if(!op->ok || !id) return 0;
            panel.selling=1;
            while(hero_item_count(&g_hero,id)>0 && guard++<HERO_INVENTORY) transact(id);
            panel.selling=0;
            rebuild();
            return 0;
        }
        default: break;
        }
        return -1;
    case DLG_SHOP:
        if(!panel_active() || panel.kind!=PANEL_SHOP) panel_open(PANEL_SHOP);
        if(op->ctrl==IDC_ITEM_LIST) { select_by_id(op->value); return 0; }
        if(op->ok && (op->ctrl==IDC_SELL_ONE || op->ctrl==IDC_SELL_ALL)) {
            int i;
            if(panel.selected<panel.count) transact(panel.ids[panel.selected]);
            if(op->ctrl==IDC_SELL_ALL)
                for(i=0;i<panel.count;i++) if(hero_item_count(&g_hero,panel.ids[i])>0) transact(panel.ids[i]);
            return 0;
        }
        return -1;
    case DLG_TROPHY:
        panel_open(PANEL_TROPHY);
        if(op->ctrl==0 && op->ok) trophy_bag_pack();
        return 0;
    case DLG_STATS:
        panel_open(PANEL_STATS);
        if(op->ctrl>=0 && op->ctrl<HERO_ABILITIES) items_attr_assign(op->ctrl);
        return 0;
    default:
        return -1;
    }
}

void panel_dialog_register(int (*fn)(const DialogOp *)) { (void)fn; }

void panels_dump(DumpEmit emit, void *user)
{
    char text[128];
    snprintf(text,sizeof text,"%s",panel.kind>PANEL_NONE&&panel.kind<PANEL_COUNT?titles[panel.kind]:"none");
    emit("panels.kind",text,user);
    dump_emit_int(emit,"panels.selected",panel.selected,user);
    dump_emit_int(emit,"panels.count",panel.count,user);
    dump_emit_int(emit,"panels.first",panel.first,user);
    dump_emit_int(emit,"panels.selling",panel.selling,user);
    dump_emit_int(emit,"panels.training_kind",panel.training_kind,user);
    dump_emit_int(emit,"panels.ability",panel.ability,user);
    dump_emit_int(emit,"panels.offers",panel.offer_count,user);
    emit("panels.message",panel.message,user);
    items_dump(emit,user);
}
