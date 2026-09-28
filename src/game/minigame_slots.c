/* GAME 1 — Slobber Slots. See minigame_slots.h for the full rule list and the VAs behind it. */
#include "minigame_slots.h"
#include "minigame_data.h"
#include "hero.h"
#include "../engine/font.h"
#include "../engine/image.h"
#include "../engine/log.h"
#include "../engine/rng.h"
#include "world.h"
#include <stdio.h>
#include <string.h>

/* RT_DIALOG 0xA7: 176x111 client, style WS_POPUP|WS_CAPTION|WS_SYSMENU. */
#define WIN_W 176
#define WIN_H 111

/* The three spin controls and the three window rects, read out of the 0xA7 DLGTEMPLATE.
 * The three reel windows are placed by FUN_00466B29 (0x466B29), the WM_SIZE handler:
 *   rect = {0,0,w,h} inflated by -8 on every side, then forced to 48x144, then stepped
 *   right by 0x34 (52) px twice. The stored rects land at this+0x90/0xA0/0xB0. */
#define REEL_W 48
#define REEL_H 144
#define REEL_GAP 52
#define REEL_X0 8 /* 0 client-left margin after the -8 inflate */
#define REEL_Y0 8

/* Control 0x447 (1095), the SPIN button: DLU x=123 y=7 cx=46 cy=12, style 0x50010000
 * (BS_OWNERDRAW with WS_CHILD|WS_VISIBLE). */
#define SPIN_ID   0x447
#define SPIN_X    123
#define SPIN_Y    7
#define SPIN_W    46
#define SPIN_H    12

/* The reel strip art: art/slots.bmp is 48x384, 8 symbols of 48x48 (slots.ini header). */
#define SYM_PX 48
#define ART_STRIP 4800 /* 100 slots * 48 px, the pos space in FUN_004666B1 */

#define BET_COST 10 /* 0x466BC6: hero+0x6C < 10 refuses the spin */
static struct {
    int open;
    int armed;       /* this+0x68: a spin is in flight and owes a payout */
    int pos[3];      /* this+0x6C/0x70/0x74: reel pixel offset 0..4799 */
    int count[3];    /* this+0x78/0x7C/0x80: ticks left before the reel stops */
    int last[3];     /* this+0x84/0x88/0x8C: symbol index last shown (0..99) */
    int last_sound;  /* this+0x64: GetTickCount stamp of the last reel click */
    uint8_t wheel[MG_WHEELS][MG_WHEEL_SLOTS];
    MgSlotsConfig cfg;
    int total_won;   /* DAT_004F240C */
    int total_bet;   /* DAT_004F2408 */
    int spins;       /* DAT_004F2410 */
    char message[96];
    int last_pay;
    int last_match;
    Image strip, cover;
} g;

int minigame_slots_open(void)
{
    int i;
    memset(&g, 0, sizeof g);
    g.open = 1;
    mg_slots_load(&g.cfg);
    /* FUN_004666B1 (0x4666B1): 3 rand(), one per reel, seeding the pixel offsets. */
    for (i = 0; i < MG_WHEELS; ++i) {
        g.pos[i] = crt_rand() % ART_STRIP;
        g.count[i] = 0;
        g.last[i] = -1;
    }
    /* FUN_00466327 (0x466327) rebuilds all three wheels; this is the bulk RNG consumer. */
    mg_slots_build(&g.cfg, g.wheel);
    {
        char rel[64], path[1024];
        snprintf(rel, sizeof rel, "art/slots.bmp");
        if (image_load(&g.strip, world_path(path, sizeof path, rel)))
            image_load(&g.strip, world_data_path(path, sizeof path, rel));
        snprintf(rel, sizeof rel, "art/slotCover.bmp");
        if (image_load(&g.cover, world_path(path, sizeof path, rel)))
            image_load(&g.cover, world_data_path(path, sizeof path, rel));
    }
    snprintf(g.message, sizeof g.message, "Press SPIN to play");
    wos_log_event("minigame_slots_open", "spins=%d won=%d bet=%d", g.spins, g.total_won, g.total_bet);
    return 1;
}

void minigame_slots_close(void)
{
    g.open = 0;
    wos_log_event("minigame_slots_close", "spins=%d won=%d bet=%d", g.spins, g.total_won, g.total_bet);
}

/* The spin button, FUN_004666BB at 0x466BBB. */
static void spin(void)
{
    int i;
    /* 0x466BC6: `if (hero->gold < 10) { sound; "You lack sufficient funds to bet."; return; }`
     * — the refusal consumes no rand(). */
    if (g_hero.gold < BET_COST) {
        snprintf(g.message, sizeof g.message, "You lack sufficient funds to bet.");
        wos_log_event("minigame_slots_spin", "ok=0 reason=no_funds gold=%lld", (long long)g_hero.gold);
        return;
    }
    /* 6 rand(): per reel, the new offset then the tick count 10 + rand()%10. */
    for (i = 0; i < MG_WHEELS; ++i) {
        g.pos[i] = crt_rand() % ART_STRIP;
        g.count[i] = BET_COST + crt_rand() % 10;
        g.last[i] = -1;
    }
    /* 0x466C03: sprintf(buf, "G%d", 10) then FUN_00484E72(hero, buf, -1) takes the 10 GP. */
    hero_add_gold(&g_hero, -BET_COST);
    g.total_bet += BET_COST;   /* DAT_004F2408 += 0x0A */
    g.spins++;                 /* DAT_004F2410 += 1 */
    g.armed = 1;               /* 0x466C46: this+0x68 = 1 */
    snprintf(g.message, sizeof g.message, "...");
    wos_log_event("minigame_slots_spin", "ok=1 bet=%d gold=%lld", BET_COST, (long long)g_hero.gold);
}

/* The settle, 0x466830 onwards. Verified against the objdump at 0x466830. */
static void settle(void)
{
    int matches = 0, ref = -1, pay = 0, w;
    for (w = 0; w < MG_WHEELS; ++w) {
        /* the original reads wheel[(last[w] + 1) % 100] — the +1 is its own off-by-one */
        int sym = g.wheel[w][(g.last[w] + 1) % MG_WHEEL_SLOTS];
        if (w == 0) ref = sym;
        if (sym == ref && ref != -1) matches++;
    }
    if (matches == 3)      pay = g.cfg.triple[ref < 0 ? 0 : ref];
    else if (matches == 2) pay = g.cfg.double_[ref < 0 ? 0 : ref];
    g.last_match = matches;
    g.last_pay = pay;
    g.armed = 0;
    if (pay > 0) {
        g.total_won += pay;                       /* DAT_004F240C += pay */
        hero_add_gold(&g_hero, pay);              /* FUN_00484E72(hero, "G%d", 1) */
        snprintf(g.message, sizeof g.message, "You won %d GP at the slot machine!", pay);
    } else {
        snprintf(g.message, sizeof g.message, "...");
    }
    wos_log_event("minigame_slots_result", "matches=%d symbol=%d pay=%d gold=%lld",
                  matches, ref, pay, (long long)g_hero.gold);
}

void minigame_slots_update(uint32_t now_ms)
{
    int spinning = 0, i;
    if (!g.open) return;
    for (i = 0; i < MG_WHEELS; ++i) {
        if (g.count[i] > 2) {
            int sym;
            spinning = 1;
            g.pos[i] = (g.pos[i] + g.count[i]) % ART_STRIP;
            sym = g.pos[i] / SYM_PX;
            if (g.last[i] != sym) {
                g.last[i] = sym;
                g.count[i]--;
                /* 0x4667E4: `if (GetTickCount() - this+0x64 > 500) { click; stamp; }` */
                if ((int)(now_ms - (uint32_t)g.last_sound) > 500)
                    g.last_sound = (int)now_ms;
            }
        }
    }
    if (!spinning && g.armed) settle();
}

void minigame_slots_click(int x, int y, int pressed)
{
    Rect spin_rect = { SPIN_X, SPIN_Y, SPIN_W, SPIN_H };
    if (!g.open || !pressed) return;
    /* Win32 client coordinates relative to the dialog's own origin. */
    if (x >= spin_rect.x && y >= spin_rect.y &&
        x < spin_rect.x + spin_rect.w && y < spin_rect.y + spin_rect.h)
        spin();
}

void minigame_slots_key(int vk, int pressed)
{
    /* The 0xA7 class has no keyboard handler; the only control is the SPIN button. */
    (void)vk; (void)pressed;
}

void minigame_slots_render(Framebuffer *fb, int bx, int by)
{
    Rect win = { bx, by, WIN_W, WIN_H };
    int w, row;
    if (!g.open) return;
    fb_fill(fb, win, 0x101010u);
    fb_rect(fb, win, 0x9d8959u);
    /* Three reels. FUN_00466931 (0x466931) draws 4 cells per reel starting at
     * -(pos % 48) and stepping 48 px; the payline symbol is cell 1, i.e. row pos/48 + 1. */
    for (w = 0; w < MG_WHEELS; ++w) {
        int rx = bx + REEL_X0 + w * REEL_GAP;
        int ry = by + REEL_Y0;
        int base = g.pos[w] / SYM_PX;
        fb_fill(fb, (Rect){rx,ry,REEL_W,REEL_H}, 0x000000u);
        for (row = 0; row < 4; ++row) {
            int slot = (base + row) % MG_WHEEL_SLOTS;
            uint8_t sym = g.wheel[w][slot];
            if (sym >= MG_SYMBOLS) continue;
            /* art/slots.bmp is a vertical 8-cell strip, cell `sym` at y = sym*48. */
            fb_blit_sub(fb, &g.strip, (Rect){0, sym * SYM_PX, REEL_W, SYM_PX},
                        rx, ry + row * SYM_PX - (g.pos[w] % SYM_PX), 0, -1);
        }
        fb_rect(fb, (Rect){rx,ry,REEL_W,REEL_H}, 0x67512eu);
    }
    font_draw(fb, bx + 8, by + WIN_H - 26, g.message, 0xffe6aeu);
    {
        char buf[64];
        snprintf(buf, sizeof buf, "W %d  B %d", g.total_won, g.total_bet);
        font_draw(fb, bx + 8, by + WIN_H - 16, buf, 0xffe6aeu);
    }
}

void minigame_slots_dump(DumpEmit emit, void *user)
{
    dump_emit_int(emit, "minigame.slots.armed", g.armed, user);
    dump_emit_int(emit, "minigame.slots.pos0", g.pos[0], user);
    dump_emit_int(emit, "minigame.slots.pos1", g.pos[1], user);
    dump_emit_int(emit, "minigame.slots.pos2", g.pos[2], user);
    dump_emit_int(emit, "minigame.slots.count0", g.count[0], user);
    dump_emit_int(emit, "minigame.slots.count1", g.count[1], user);
    dump_emit_int(emit, "minigame.slots.count2", g.count[2], user);
    dump_emit_int(emit, "minigame.slots.sym0", g.wheel[0][(g.last[0] + 1) % MG_WHEEL_SLOTS], user);
    dump_emit_int(emit, "minigame.slots.sym1", g.wheel[1][(g.last[1] + 1) % MG_WHEEL_SLOTS], user);
    dump_emit_int(emit, "minigame.slots.sym2", g.wheel[2][(g.last[2] + 1) % MG_WHEEL_SLOTS], user);
    dump_emit_int(emit, "minigame.slots.matches", g.last_match, user);
    dump_emit_int(emit, "minigame.slots.pay", g.last_pay, user);
    dump_emit_int(emit, "minigame.slots.total_won", g.total_won, user);
    dump_emit_int(emit, "minigame.slots.total_bet", g.total_bet, user);
    dump_emit_int(emit, "minigame.slots.spins", g.spins, user);
}
