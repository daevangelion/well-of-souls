/* GAME 4 — Blackjack. See minigame_cards.h for the rules and their VAs. */
#include "minigame_cards.h"
#include "hero.h"
#include "../engine/clock.h"
#include "../engine/font.h"
#include "../engine/log.h"
#include "../engine/rng.h"
#include <stdio.h>
#include <string.h>

#define WIN_W 196
#define WIN_H 167

/* Control rects out of the 0xC0 DLGTEMPLATE: "Hit Me!" (id 0x79), "Stay!" (id 0x3a),
 * "Place Bet" (id 0x61) and the wager CEdit. */
#define BTN_HIT_X 74
#define BTN_HIT_Y 128
#define BTN_HIT_W 30
#define BTN_HIT_H 12
#define BTN_STAY_X 40
#define BTN_STAY_Y 128
#define BTN_STAY_W 30
#define BTN_STAY_H 12
#define BTN_BET_X 8
#define BTN_BET_Y 8
#define BTN_BET_W 40
#define BTN_BET_H 12
#define EDIT_X 100
#define EDIT_Y 8
#define EDIT_W 60
#define EDIT_H 12

/* the wager ceiling, from the "G%d" sprintf the Deal handler builds */
#define BET_MAX 1000
/* the original sleeps 250 ms per dealt card and holds the result for 3000 ms */
#define DEAL_STEP_MS 250
#define RESULT_HOLD_MS 3000

/* Deck layout: a 52-card shoe, card value 0..51, suit = v/13, rank = v%13 (0 = ace). */
#define SHOE_SIZE 52

enum { ST_IDLE, ST_DEALING, ST_PLAYER, ST_DEALER, ST_RESULT };

static struct {
    int open;
    int shoe[SHOE_SIZE];
    int cursor;          /* DAT_004DE038 */
    int player[101];     /* this+0x68 */
    int player_count;    /* this+0x1F8 */
    int dealer[100];     /* this+0x1FC */
    int dealer_count;    /* this+0x38C */
    int wager;           /* this+0x60 */
    int state;           /* this+0x64 */
    int payout;          /* this+0x778 */
    int result_t0;       /* this+0x780 */
    int profit;          /* this+0x784 */
    int next_deal;       /* when the next card is due (DEAL_STEP_MS cadence) */
    int pending_card;    /* a card already drawn but not yet placed */
    int pending_to;      /* 0 = player, 1 = dealer */
    int player_score;
    int dealer_score;
    char status[128];
} g;

static const char *const rank_name[13] = {
    "A", "2", "3", "4", "5", "6", "7", "8", "9", "10", "J", "Q", "K"
};

/* The shuffle at 0x40ADBB: 52000 iterations, each drawing j = (rand() >> 4) % 52. */
static void shuffle(void)
{
    int i;
    for (i = 0; i < SHOE_SIZE; ++i) g.shoe[i] = i;
    for (i = 0; i < 52000; ++i) {
        int j = (crt_rand() >> 4) % SHOE_SIZE;
        int t = g.shoe[i % SHOE_SIZE];
        g.shoe[i % SHOE_SIZE] = g.shoe[j];
        g.shoe[j] = t;
    }
    g.cursor = 0;
}

/* FUN_0040AE6C (0x40AE6C) scores a hand: aces are 11 or 1, so a natural 21 is an ace plus a
 * ten and the dealer stands on 17. Returns the hand total with `soft` set for a natural. */
static int score(const int *cards, int n, int *soft)
{
    int total = 0, aces = 0, i;
    *soft = 0;
    for (i = 0; i < n; ++i) {
        int r = cards[i] % 13;
        if (r == 0) { aces++; total += 11; }
        else if (r >= 10) total += 10;
        else total += r + 1;
    }
    while (total > 21 && aces > 0) { total -= 10; aces--; }
    if (total == 21 && n == 2) *soft = 1;
    return total;
}

static int draw(void)
{
    if (g.cursor >= SHOE_SIZE) shuffle();
    return g.shoe[g.cursor++];
}

/* FUN_00484E72 with a "G%d" string: the only way this dialog moves gold. */
static void move_gold(int amount)
{
    hero_add_gold(&g_hero, amount);
}

static void set_status(const char *text)
{
    snprintf(g.status, sizeof g.status, "%s", text);
}

static void settle(void)
{
    int psoft, dsoft;
    int p = score(g.player, g.player_count, &psoft);
    int d = score(g.dealer, g.dealer_count, &dsoft);
    g.player_score = p;
    g.dealer_score = d;
    g.payout = 0;
    if (psoft && !dsoft) {
        /* a natural pays 3:2; the bet is returned plus three halves of it */
        g.payout = g.wager + g.wager * 3 / 2;
        set_status("Blackjack! You win.");
    } else if (psoft && dsoft) {
        g.payout = g.wager;          /* push: the wager comes back */
        set_status("Push. Both have blackjack.");
    } else if (p > 21) {
        g.payout = 0;
        set_status("Bust. You lose your bet.");
    } else if (d > 21 || p > d) {
        g.payout = g.wager * 2;
        set_status("You win!");
    } else if (p < d) {
        g.payout = 0;
        set_status("Dealer wins.");
    } else {
        g.payout = g.wager;          /* push */
        set_status("Push.");
    }
    move_gold(g.payout - g.wager);
    g.profit += g.payout - g.wager;
    g.state = ST_RESULT;
    g.result_t0 = (int)clock_ms();
    wos_log_event("minigame_blackjack_result", "wager=%d player=%d dealer=%d payout=%d profit=%d",
                  g.wager, p, d, g.payout, g.profit);
}

int minigame_blackjack_open(void)
{
    memset(&g, 0, sizeof g);
    g.open = 1;
    shuffle();
    g.state = ST_IDLE;
    g.wager = 10;
    set_status("Place your bet, then Hit Me!");
    wos_log_event("minigame_blackjack_open", "wager=%d", g.wager);
    return 1;
}

void minigame_blackjack_close(void)
{
    g.open = 0;
    wos_log_event("minigame_blackjack_close", "profit=%d", g.profit);
}

/* FUN_0040B8DE (0x40B8DE): take the wager and deal two cards each, player first. */
static void deal(void)
{
    if (g.wager <= 0 || g_hero.gold < g.wager) {
        set_status("You lack sufficient funds to bet.");
        wos_log_event("minigame_blackjack_deal", "ok=0 reason=no_funds gold=%lld",
                      (long long)g_hero.gold);
        return;
    }
    move_gold(-g.wager);
    g.player_count = 0;
    g.dealer_count = 0;
    g.payout = 0;
    g.state = ST_DEALING;
    g.pending_card = draw();
    g.pending_to = 0;
    g.next_deal = (int)clock_ms() + DEAL_STEP_MS;
    wos_log_event("minigame_blackjack_deal", "ok=1 wager=%d gold=%lld", g.wager, (long long)g_hero.gold);
}

static void place_pending(void)
{
    if (g.pending_to == 0) {
        if (g.player_count < (int)(sizeof g.player / sizeof g.player[0]))
            g.player[g.player_count++] = g.pending_card;
    } else {
        if (g.dealer_count < (int)(sizeof g.dealer / sizeof g.dealer[0]))
            g.dealer[g.dealer_count++] = g.pending_card;
    }
}

/* FUN_0040B783 (0x40B783): the dealer draws to 17, then the hands are compared. */
static void stand(void)
{
    int soft;
    g.state = ST_DEALER;
    /* the dealer reveals the hole card, then draws while under 17 */
    while (score(g.dealer, g.dealer_count, &soft) < 17) {
        if (g.dealer_count >= (int)(sizeof g.dealer / sizeof g.dealer[0])) break;
        g.dealer[g.dealer_count++] = draw();
    }
    settle();
}

void minigame_blackjack_update(uint32_t now_ms)
{
    int soft;
    if (!g.open) return;
    switch (g.state) {
    case ST_DEALING:
        /* Sleep(250) per card in the original; the port keeps the same cadence on the
         * virtual clock so a replay lands on the same card at the same virtual time. */
        if ((int)(now_ms - (uint32_t)g.next_deal) < 0) break;
        place_pending();
        if (g.player_count < 2) {
            g.pending_card = draw();
            g.pending_to = 1;
        } else if (g.dealer_count < 2) {
            g.pending_card = draw();
            g.pending_to = 0;
        } else {
            g.state = ST_PLAYER;
            set_status("Your turn: Hit Me! or Stay!");
            break;
        }
        g.next_deal = (int)now_ms + DEAL_STEP_MS;
        break;
    case ST_PLAYER: {
        int p = score(g.player, g.player_count, &soft);
        if (p > 21) { settle(); break; }
        break;
    }
    case ST_RESULT:
        /* the 3000 ms hold, checked from OnIdle in the original */
        if ((int)(now_ms - (uint32_t)g.result_t0) >= RESULT_HOLD_MS) {
            g.state = ST_IDLE;
            set_status("Place your bet, then Hit Me!");
        }
        break;
    default:
        break;
    }
}

static int hit(int x, int y, Rect r)
{
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

void minigame_blackjack_click(int x, int y, int pressed)
{
    if (!g.open || !pressed) return;
    if (g.state == ST_IDLE) {
        if (hit(x, y, (Rect){BTN_BET_X, BTN_BET_Y, BTN_BET_W, BTN_BET_H}))
            g.wager = g.wager >= BET_MAX ? 10 : g.wager * 2;
        else if (hit(x, y, (Rect){BTN_HIT_X, BTN_HIT_Y, BTN_HIT_W, BTN_HIT_H}) ||
                 hit(x, y, (Rect){BTN_STAY_X, BTN_STAY_Y, BTN_STAY_W, BTN_STAY_H}))
            deal();
        return;
    }
    if (g.state == ST_PLAYER) {
        int soft, p;
        if (hit(x, y, (Rect){BTN_HIT_X, BTN_HIT_Y, BTN_HIT_W, BTN_HIT_H})) {
            if (g.player_count >= (int)(sizeof g.player / sizeof g.player[0])) return;
            g.player[g.player_count++] = draw();
            p = score(g.player, g.player_count, &soft);
            if (p > 21) settle();
            else set_status("Your turn: Hit Me! or Stay!");
        } else if (hit(x, y, (Rect){BTN_STAY_X, BTN_STAY_Y, BTN_STAY_W, BTN_STAY_H})) {
            stand();
        }
    }
}

void minigame_blackjack_key(int vk, int pressed)
{
    if (!g.open || !pressed) return;
    if (g.state == ST_PLAYER && (vk == 'H' || vk == 'S' || vk == 13))
        minigame_blackjack_click(vk == 'S' ? BTN_STAY_X : BTN_HIT_X, BTN_HIT_Y, 1);
}

static void draw_hand(Framebuffer *fb, int bx, int by, const int *cards, int n, int y)
{
    int i;
    for (i = 0; i < n; ++i) {
        Rect card = { bx + 10 + i * 22, by + y, 20, 28 };
        char buf[8];
        fb_fill(fb, card, 0xf0f0f0u);
        fb_rect(fb, card, 0x202020u);
        snprintf(buf, sizeof buf, "%s", rank_name[cards[i] % 13]);
        font_draw(fb, card.x + 2, card.y + 2, buf, 0x101010u);
    }
}

void minigame_blackjack_render(Framebuffer *fb, int bx, int by)
{
    Rect win = { bx, by, WIN_W, WIN_H };
    char buf[64];
    if (!g.open) return;
    fb_fill(fb, win, 0x0d2b12u);
    fb_rect(fb, win, 0x9d8959u);
    font_draw(fb, bx + 8, by + 4, "Blackjack", 0xffe6aeu);
    draw_hand(fb, bx, by, g.dealer, g.dealer_count, 34);
    draw_hand(fb, bx, by, g.player, g.player_count, 76);
    snprintf(buf, sizeof buf, "bet %d   you %d   dealer %d", g.wager, g.player_score, g.dealer_score);
    font_draw(fb, bx + 8, by + WIN_H - 30, buf, 0xffe6aeu);
    font_draw(fb, bx + 8, by + WIN_H - 18, g.status, 0xffe6aeu);
    fb_fill(fb, (Rect){bx + BTN_HIT_X, by + BTN_HIT_Y, BTN_HIT_W, BTN_HIT_H}, 0x292820u);
    fb_rect(fb, (Rect){bx + BTN_HIT_X, by + BTN_HIT_Y, BTN_HIT_W, BTN_HIT_H}, 0x9d8959u);
    font_draw(fb, bx + BTN_HIT_X + 2, by + BTN_HIT_Y + 2, "Hit", 0xffe6aeu);
    fb_fill(fb, (Rect){bx + BTN_STAY_X, by + BTN_STAY_Y, BTN_STAY_W, BTN_STAY_H}, 0x292820u);
    fb_rect(fb, (Rect){bx + BTN_STAY_X, by + BTN_STAY_Y, BTN_STAY_W, BTN_STAY_H}, 0x9d8959u);
    font_draw(fb, bx + BTN_STAY_X + 2, by + BTN_STAY_Y + 2, "Stay", 0xffe6aeu);
}

void minigame_blackjack_dump(DumpEmit emit, void *user)
{
    int i;
    dump_emit_int(emit, "minigame.blackjack.state", g.state, user);
    dump_emit_int(emit, "minigame.blackjack.wager", g.wager, user);
    dump_emit_int(emit, "minigame.blackjack.payout", g.payout, user);
    dump_emit_int(emit, "minigame.blackjack.profit", g.profit, user);
    dump_emit_int(emit, "minigame.blackjack.cursor", g.cursor, user);
    dump_emit_int(emit, "minigame.blackjack.player_count", g.player_count, user);
    dump_emit_int(emit, "minigame.blackjack.dealer_count", g.dealer_count, user);
    dump_emit_int(emit, "minigame.blackjack.player_score", g.player_score, user);
    dump_emit_int(emit, "minigame.blackjack.dealer_score", g.dealer_score, user);
    for (i = 0; i < g.player_count && i < 16; ++i)
        dump_emit_int(emit, "minigame.blackjack.player_card", g.player[i], user);
    for (i = 0; i < g.dealer_count && i < 16; ++i)
        dump_emit_int(emit, "minigame.blackjack.dealer_card", g.dealer[i], user);
}
