/* GAME 7 — Stock Market. See minigame_stocks.h for the rules and their VAs. */
#include "minigame_stocks.h"
/* FUN_0049A7E8's constants, read out of .rdata with
 * `objdump -s -j .rdata --start-address=0x4d0858 --stop-address=0x4d08f8`:
 *   0x4D08C0 K_T = 0.016666666666666666   0x4D0878 K_A = 10.0
 *   0x4D08D8 K_B = 0.03125               0x4D0888 K_P = 5.0
 *   0x4D08D0 K_W = 1e-05                 0x4D08E0 K_D = 0.9
 *   0x4D08E8 K_M1 = 0.0002777777777777778   0x4D08F0 K_M2 = 0.00011415525114155251
 *   0x4D0860 K_MIN = 0.0
 * The two mood constants multiply a netgame flag (DAT_004E17FC) that is never set in solo
 * play, so the mood term is identically zero and is written as a literal 0.0 here. */
#define K_T    0.016666666666666666
#define K_A    10.0
#define K_B    0.03125
#define K_P    5.0
#define K_W    1e-05
#define K_D    0.9
#define K_M1   0.0002777777777777778
#define K_M2   0.00011415525114155251
#define K_MIN  0.0
#include "minigame_data.h"
#include "hero.h"
#include "../engine/clock.h"
#include "../engine/font.h"
#include "../engine/log.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define WIN_W 287
#define WIN_H 220

/* the five market buttons in the 0xC6 template, and the portfolio tab's SELL button */
static const Rect btn_buy  = { 8,   24,  60, 20 };
static const Rect btn_sell = { 76,  24,  60, 20 };
static const Rect btn_up   = { 144, 24,  40, 20 };
static const Rect btn_down = { 192, 24,  40, 20 };
static const Rect btn_tab  = { 8,   4,   80, 16 };
static const Rect list_rect = { 8,  48, 271, 150 };

#define MAX_SHARES 10000   /* the buy cap in FUN_0049A3F6 */
#define LOT 10             /* the +/- buttons move the order size by one lot */

static struct {
    int open;
    MgStocks stocks;
    uint32_t ticker[MG_STOCKS_MAX];
    int price[MG_STOCKS_MAX];
    int shares[MG_STOCKS_MAX];
    int selected;
    int order;             /* the pending order size, in shares */
    int tab;               /* 0 = market, 1 = portfolio */
    char text[96];
} g;

/* FUN_00499B1A / 0x499B33: the 32-bit ticker codec. A symbol becomes an integer by folding
 * each character into the accumulator; the low 5 bits of each byte are kept so the price
 * function can walk the set bits. */
static uint32_t ticker_of(const char *symbol)
{
    uint32_t t = 0;
    int i;
    for (i = 0; symbol[i] && i < 8; ++i)
        t = (t << 5) | (uint32_t)(symbol[i] & 0x1F);
    return t;
}

/* FUN_0049A7E8 (0x49A7E8), exactly:
 *   phase = (double)(when / 60) * K_T;      integer division, so MINUTES since epoch
 *   for each set bit b of the ticker:
 *       n   += 1.0;
 *       amp  = pow(32.0 - b, 4.5);
 *       k    = b * K_A * K_B + K_P;
 *       sum += (sin(amp * K_W * phase) + K_D) * k;
 *   sum += n * K_A * 0.0 * K_M1 * K_M2;     the mood term: identically 0 in solo play
 *   if (sum < K_MIN) sum = 0.0;
 * The 32 ticker bits ARE the price personality, so two symbols give two different but
 * perfectly reproducible curves, and the market moves even while the dialog is closed. */
static double price_of(int index, uint32_t when)
{
    uint32_t t = g.ticker[index];
    double phase = (double)(when / 60u) * K_T;
    double sum = 0.0, n = 0.0;
    int b;
    for (b = 0; b < 32; ++b) {
        if (t & (1u << b)) {
            double amp = pow((double)(32 - b), 4.5);
            double k = (double)b * K_A * K_B + K_P;
            n += 1.0;
            sum += (sin(amp * K_W * phase) + K_D) * k;
        }
    }
    sum += n * K_A * 0.0 * K_M1 * K_M2;   /* mood: DAT_004E17FC is 0 in solo play */
    if (sum < K_MIN) sum = 0.0;
    return sum;
}

static void resample(void)
{
    /* re-sampled on every paint, every sell and every portfolio recompute */
    uint32_t minutes = clock_time_s() / 60u;
    int i;
    for (i = 0; i < g.stocks.count; ++i) g.price[i] = price_of(i, minutes);
}

int minigame_stocks_open(void)
{
    int i;
    memset(&g, 0, sizeof g);
    g.open = 1;
    mg_stocks_load(&g.stocks);
    for (i = 0; i < g.stocks.count && i < MG_STOCKS_MAX; ++i)
        g.ticker[i] = ticker_of(g.stocks.stock[i].symbol);
    g.order = LOT;
    g.tab = 0;
    resample();
    snprintf(g.text, sizeof g.text, "Pick a stock, set the size, then Buy or Sell");
    wos_log_event("minigame_stocks_open", "stocks=%d", g.stocks.count);
    return 1;
}

void minigame_stocks_close(void)
{
    int i, held = 0;
    for (i = 0; i < g.stocks.count; ++i) held += g.shares[i];
    g.open = 0;
    wos_log_event("minigame_stocks_close", "held=%d gold=%lld", held, (long long)g_hero.gold);
}

/* FUN_0049A3F6: buy `order` shares of the selected stock at its current price. */
static void buy(void)
{
    int cost, n, i;
    if (g.selected < 0 || g.selected >= g.stocks.count || g.order <= 0) return;
    resample();
    cost = g.price[g.selected] * g.order;
    if (cost > g_hero.gold) {
        snprintf(g.text, sizeof g.text, "Not enough gold.");
        wos_log_event("minigame_stocks_buy", "ok=0 reason=no_funds need=%d gold=%lld",
                      cost, (long long)g_hero.gold);
        return;
    }
    if (g.shares[g.selected] + g.order > MAX_SHARES) {
        snprintf(g.text, sizeof g.text, "Position limit reached.");
        return;
    }
    hero_add_gold(&g_hero, -cost);
    g.shares[g.selected] += g.order;
    snprintf(g.text, sizeof g.text, "Bought %d %s at %d", g.order,
             g.stocks.stock[g.selected].symbol, g.price[g.selected]);
    wos_log_event("minigame_stocks_buy", "ok=1 stock=%d shares=%d price=%d cost=%d gold=%lld",
                  g.selected, g.shares[g.selected], g.price[g.selected], cost,
                  (long long)g_hero.gold);
    (void)n; (void)i;
}

/* FUN_0049A5A2: sell at the current price. */
static void sell(void)
{
    int value, n;
    if (g.selected < 0 || g.selected >= g.stocks.count || g.shares[g.selected] <= 0) {
        snprintf(g.text, sizeof g.text, "You hold none of that stock.");
        wos_log_event("minigame_stocks_sell", "ok=0 reason=no_shares stock=%d", g.selected);
        return;
    }
    resample();
    n = g.order;
    if (n > g.shares[g.selected]) n = g.shares[g.selected];
    value = g.price[g.selected] * n;
    hero_add_gold(&g_hero, value);
    g.shares[g.selected] -= n;
    snprintf(g.text, sizeof g.text, "Sold %d %s at %d", n,
             g.stocks.stock[g.selected].symbol, g.price[g.selected]);
    wos_log_event("minigame_stocks_sell", "ok=1 stock=%d shares=%d price=%d value=%d gold=%lld",
                  g.selected, g.shares[g.selected], g.price[g.selected], value,
                  (long long)g_hero.gold);
}

static int inside(int x, int y, Rect r)
{
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

void minigame_stocks_click(int x, int y, int pressed)
{
    int row;
    if (!g.open || !pressed) return;
    if (inside(x, y, btn_tab)) { g.tab = g.tab ? 0 : 1; return; }
    if (inside(x, y, btn_up))   { g.order += LOT; if (g.order > MAX_SHARES) g.order = MAX_SHARES; return; }
    if (inside(x, y, btn_down)) { g.order -= LOT; if (g.order < LOT) g.order = LOT; return; }
    if (inside(x, y, btn_buy))  { buy(); return; }
    if (inside(x, y, btn_sell)) { sell(); return; }
    if (g.tab == 0 && inside(x, y, list_rect)) {
        row = (y - list_rect.y) / 10;
        if (row >= 0 && row < g.stocks.count) g.selected = row;
    }
}

void minigame_stocks_key(int vk, int pressed)
{
    if (!g.open || !pressed) return;
    if (vk == 0x21 /*PgUp*/ || vk == 0x26) { if (g.selected > 0) g.selected--; }
    else if (vk == 0x22 /*PgDn*/ || vk == 0x28) { if (g.selected + 1 < g.stocks.count) g.selected++; }
}

void minigame_stocks_update(uint32_t now_ms)
{
    (void)now_ms;
    if (!g.open) return;
    resample();
}

void minigame_stocks_render(Framebuffer *fb, int bx, int by)
{
    Rect win = { bx, by, WIN_W, WIN_H };
    int i, rows;
    char buf[80];
    if (!g.open) return;
    fb_fill(fb, win, 0x101c10u);
    fb_rect(fb, win, 0x9d8959u);
    font_draw(fb, bx + 8, by + 6, g.tab ? "Portfolio" : "Stock Market", 0xffe6aeu);
    if (g.tab == 0) {
        for (i = 0; i < 4; ++i) {
            Rect r = i == 0 ? btn_buy : i == 1 ? btn_sell : i == 2 ? btn_up : btn_down;
            fb_fill(fb, (Rect){bx + r.x, by + r.y, r.w, r.h}, 0x292820u);
            fb_rect(fb, (Rect){bx + r.x, by + r.y, r.w, r.h}, 0x9d8959u);
        }
        font_draw(fb, bx + btn_buy.x + 6, by + btn_buy.y + 6, "Buy", 0xffe6aeu);
        font_draw(fb, bx + btn_sell.x + 6, by + btn_sell.y + 6, "Sell", 0xffe6aeu);
        font_draw(fb, bx + btn_up.x + 6, by + btn_up.y + 6, "+", 0xffe6aeu);
        font_draw(fb, bx + btn_down.x + 6, by + btn_down.y + 6, "-", 0xffe6aeu);
        snprintf(buf, sizeof buf, "order %d", g.order);
        font_draw(fb, bx + 8, by + WIN_H - 12, buf, 0xffe6aeu);
    }
    rows = list_rect.h / 10;
    for (i = 0; i < g.stocks.count && i < rows; ++i) {
        int ry = by + list_rect.y + i * 10;
        if (i == g.selected) fb_fill(fb, (Rect){bx + list_rect.x, ry, list_rect.w, 10}, 0x2b2410u);
        if (g.tab == 0)
            snprintf(buf, sizeof buf, "%-6s %-28s %6d", g.stocks.stock[i].symbol,
                     g.stocks.stock[i].name, g.price[i]);
        else
            snprintf(buf, sizeof buf, "%-6s held %6d  value %8d", g.stocks.stock[i].symbol,
                     g.shares[i], g.shares[i] * g.price[i]);
        font_draw(fb, bx + list_rect.x + 2, ry + 1, buf, 0xffe6aeu);
    }
    font_draw(fb, bx + 8, by + WIN_H - 24, g.text, 0xffe6aeu);
}

void minigame_stocks_dump(DumpEmit emit, void *user)
{
    int i;
    dump_emit_int(emit, "minigame.stocks.count", g.stocks.count, user);
    dump_emit_int(emit, "minigame.stocks.selected", g.selected, user);
    dump_emit_int(emit, "minigame.stocks.order", g.order, user);
    dump_emit_int(emit, "minigame.stocks.tab", g.tab, user);
    for (i = 0; i < g.stocks.count; ++i) {
        dump_emit_int(emit, "minigame.stocks.price", g.price[i], user);
        dump_emit_int(emit, "minigame.stocks.shares", g.shares[i], user);
    }
}
