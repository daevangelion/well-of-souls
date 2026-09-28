/* Mini-game chrome shared by all eight GAME games: the armed-game button, the open-window
 * bookkeeping, the input pump and the differential dump.
 *
 * VAs: FUN_00421529 (0x421529) arms; FUN_00478E04 (0x478E04) registers the GAME button;
 * FUN_00421563 (0x421563) opens/closes; DAT_004E483C is the "a game is open" guard. */
#include "minigame.h"
#include "minigame_data.h"
#include "minigame_slots.h"
#include "minigame_racer.h"
#include "minigame_cards.h"
#include "minigame_asteroids.h"
#include "minigame_stocks.h"
#include "minigame_tetris.h"
#include "minigame_train.h"
#include "minigame_pi.h"
#include "../engine/clock.h"
#include "../engine/log.h"
#include "../engine/rng.h"
#include <stdio.h>
#include <string.h>

/* The per-game state. Each sub-module owns one slot; the chrome below only knows which is live. */
static struct {
    int armed;        /* DAT_004E18A4: the armed game number, 0 = no GAME button */
    int open;         /* DAT_004E483C != 0: a mini-game window exists */
    int finished;     /* sticky: a window closed this tick */
    int result;       /* the game number that closed */
    int timer_id;     /* the live SetTimer id, 0 = none (Win32 returns the same id) */
    char label[64];   /* script text of the line after `GAME n` (log/dump only) */
} mg;

/* SetTimer ids. The original has exactly eight SetTimer sites (docs/re/timing.md table 2);
 * FUN_0046666E/0x4666B1 slots uses id 1 with a 0 ms period, so we keep our own per-game ids. */
#define TIMER_SPIN 1

int minigame_exists(int game)
{
    return game >= MINIGAME_SLOTS && game <= MINIGAME_TETRIS;
}

int minigame_start(int game, const char *label)
{
    if (game < 0 || !minigame_exists(game)) {
        /* The original's FUN_00421529 sends every n<1 down the close path and has no default
         * case in FUN_00421563, so an out-of-range number silently does nothing. */
        if (game < 1) {
            minigame_close();
            mg.armed = 0;
        }
        wos_log_event("minigame_start", "game=%d armed=0 ok=0", game);
        return 0;
    }
    if (mg.open) {
        wos_log_event("minigame_start", "game=%d armed=0 ok=0 reason=already_open", game);
        return 0;
    }
    mg.armed = game;
    snprintf(mg.label, sizeof mg.label, "%s", label ? label : "");
    /* FUN_00478E04 shows the GAME button (slot 6, "buttonGame.bmp", message id 0x493). */
    wos_log_event("minigame_arm", "game=%d label=%s", game, mg.label);
    return 1;
}

void minigame_disarm(void)
{
    if (!mg.armed) return;
    mg.armed = 0;
    wos_log_event("minigame_disarm", "game=%d", mg.result);
}

int minigame_armed(void) { return mg.armed; }
int minigame_active(void) { return mg.open; }
int minigame_finished(void) { return mg.finished; }
void minigame_finished_clear(void) { mg.finished = 0; }
int minigame_result(void) { return mg.result; }

/* The GAME button click: FUN_00478E04 registered the button with message id 0x493; the scene's
 * button handler dispatches to FUN_00421563 with the armed number. Opening happens HERE, not at
 * the opcode. */
static void open_armed(void)
{
    int game = mg.armed;
    int ok = 1;
    if (!game || mg.open) return;
    switch (game) {
    case MINIGAME_SLOTS:      ok = minigame_slots_open(); break;
    case MINIGAME_RACER:      ok = minigame_racer_open(); break;
    case MINIGAME_PI:         ok = minigame_pi_open(); break;
    case MINIGAME_BLACKJACK:  ok = minigame_blackjack_open(); break;
    case MINIGAME_POKEGATCHI: ok = minigame_train_open(); break;
    case MINIGAME_ASTEROIDS:  ok = minigame_asteroids_open(); break;
    case MINIGAME_STOCKS:     ok = minigame_stocks_open(); break;
    case MINIGAME_TETRIS:     ok = minigame_tetris_open(); break;
    default: ok = 0; break;
    }
    if (!ok) {
        wos_log_event("minigame_open", "game=%d ok=0", game);
        return;
    }
    mg.open = game;
    wos_log_event("minigame_open", "game=%d ok=1", game);
}

/* FUN_00478E04 (0x478E04) only arms the GAME button; the window is created when the player
 * clicks it, which routes through minigame_click below. Nothing to advance while closed. */
void minigame_update(uint32_t now_ms)
{
    (void)now_ms;
    if (!mg.open) return;
    switch (mg.open) {
    case MINIGAME_SLOTS:      minigame_slots_update(clock_ms()); break;
    case MINIGAME_RACER:      minigame_racer_update(clock_ms()); break;
    case MINIGAME_PI:         minigame_pi_update(clock_ms()); break;
    case MINIGAME_BLACKJACK:  minigame_blackjack_update(clock_ms()); break;
    case MINIGAME_POKEGATCHI: minigame_train_update(clock_ms()); break;
    case MINIGAME_ASTEROIDS:  minigame_asteroids_update(clock_ms()); break;
    case MINIGAME_STOCKS:     minigame_stocks_update(clock_ms()); break;
    case MINIGAME_TETRIS:     minigame_tetris_update(clock_ms()); break;
    default: break;
    }
}

void minigame_render(Framebuffer *fb, int base_x, int base_y)
{
    if (!mg.open) return;
    switch (mg.open) {
    case MINIGAME_SLOTS:      minigame_slots_render(fb, base_x, base_y); break;
    case MINIGAME_RACER:      minigame_racer_render(fb, base_x, base_y); break;
    case MINIGAME_PI:         minigame_pi_render(fb, base_x, base_y); break;
    case MINIGAME_BLACKJACK:  minigame_blackjack_render(fb, base_x, base_y); break;
    case MINIGAME_POKEGATCHI: minigame_train_render(fb, base_x, base_y); break;
    case MINIGAME_ASTEROIDS:  minigame_asteroids_render(fb, base_x, base_y); break;
    case MINIGAME_STOCKS:     minigame_stocks_render(fb, base_x, base_y); break;
    case MINIGAME_TETRIS:     minigame_tetris_render(fb, base_x, base_y); break;
    default: break;
    }
}

/* The scene button bar. FUN_004787B2 (0x4787B2) lays out 10 slots across the top-right of the
 * client area, right to left, each 48x48 with a 51 px pitch, the first starting 3 px in from
 * the right edge and 8 px down:
 *     left = right - 51 - 51*slot,  top = top + 8,  w = h = 48.
 * FUN_0047876B (0x47876B) hit-tests with Win32 PtInRect (left/top inclusive, right/bottom
 * exclusive) over slots 0..9. FUN_00478E04 registers the GAME button in slot 6, whose command
 * id 0x493 goes to the main frame; the view then calls FUN_00421563 with the armed number.
 * At 640x480 slot 6 is x in [283,330], y in [8,55]. */
#define BTN_COUNT      10
#define BTN_W          48
#define BTN_H          48
#define BTN_PITCH      51
#define BTN_INSET      3
#define BTN_TOP        8
#define BTN_GAME_SLOT  6
#define BTN_CLIENT_W   640

static void game_button_rect(int slot, Rect *out)
{
    out->w = BTN_W;
    out->h = BTN_H;
    out->x = BTN_CLIENT_W - BTN_W - BTN_INSET - slot * BTN_PITCH;
    out->y = BTN_TOP;
}

static int in_rect(int x, int y, Rect r)
{
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

void minigame_click(int x, int y, int pressed)
{
    if (mg.open) {
        /* `x`,`y` are already in mini-game client coordinates (the caller translated them). */
        switch (mg.open) {
        case MINIGAME_SLOTS:      minigame_slots_click(x, y, pressed); break;
        case MINIGAME_RACER:      minigame_racer_click(x, y, pressed); break;
        case MINIGAME_PI:         minigame_pi_click(x, y, pressed); break;
        case MINIGAME_BLACKJACK:  minigame_blackjack_click(x, y, pressed); break;
        case MINIGAME_POKEGATCHI: minigame_train_click(x, y, pressed); break;
        case MINIGAME_ASTEROIDS:  minigame_asteroids_click(x, y, pressed); break;
        case MINIGAME_STOCKS:     minigame_stocks_click(x, y, pressed); break;
        case MINIGAME_TETRIS:     minigame_tetris_click(x, y, pressed); break;
        default: break;
        }
        return;
    }
    if (!pressed || !mg.armed) return;
    {
        Rect r;
        int slot;
        for (slot = 0; slot < BTN_COUNT; slot++) {
            game_button_rect(slot, &r);
            if (in_rect(x, y, r)) break;
        }
        if (slot < BTN_COUNT && slot == 6) open_armed();
    }
}

void minigame_key(int vk, int pressed)
{
    if (!mg.open) return;
    switch (mg.open) {
    case MINIGAME_SLOTS:      minigame_slots_key(vk, pressed); break;
    case MINIGAME_RACER:      minigame_racer_key(vk, pressed); break;
    case MINIGAME_PI:         minigame_pi_key(vk, pressed); break;
    case MINIGAME_BLACKJACK:  minigame_blackjack_key(vk, pressed); break;
    case MINIGAME_POKEGATCHI: minigame_train_key(vk, pressed); break;
    case MINIGAME_ASTEROIDS:  minigame_asteroids_key(vk, pressed); break;
    case MINIGAME_STOCKS:     minigame_stocks_key(vk, pressed); break;
    case MINIGAME_TETRIS:     minigame_tetris_key(vk, pressed); break;
    default: break;
    }
}

void minigame_close(void)
{
    int game = mg.open;
    if (!game) return;
    switch (game) {
    case MINIGAME_SLOTS:      minigame_slots_close(); break;
    case MINIGAME_RACER:      minigame_racer_close(); break;
    case MINIGAME_PI:         minigame_pi_close(); break;
    case MINIGAME_BLACKJACK:  minigame_blackjack_close(); break;
    case MINIGAME_POKEGATCHI: minigame_train_close(); break;
    case MINIGAME_ASTEROIDS:  minigame_asteroids_close(); break;
    case MINIGAME_STOCKS:     minigame_stocks_close(); break;
    case MINIGAME_TETRIS:     minigame_tetris_close(); break;
    default: break;
    }
    mg.open = 0;
    mg.result = game;
    mg.finished = 1;
    /* FUN_00421CB9 (0x421CB9) re-arms the GAME button when the scene regains focus. */
    if (mg.armed) {
        wos_log_event("minigame_rearm", "game=%d", game);
    } else {
        mg.armed = 0;
    }
    wos_log_event("minigame_close", "game=%d", game);
}

void minigame_dump(DumpEmit emit, void *user)
{
    dump_emit_int(emit, "minigame.armed", mg.armed, user);
    dump_emit_int(emit, "minigame.open", mg.open, user);
    dump_emit_int(emit, "minigame.result", mg.result, user);
    emit("minigame.label", mg.label, user);
    dump_emit_int(emit, "rng.calls", (long long)crt_rand_calls(), user);
    switch (mg.open) {
    case MINIGAME_SLOTS:      minigame_slots_dump(emit, user); break;
    case MINIGAME_RACER:      minigame_racer_dump(emit, user); break;
    case MINIGAME_PI:         minigame_pi_dump(emit, user); break;
    case MINIGAME_BLACKJACK:  minigame_blackjack_dump(emit, user); break;
    case MINIGAME_POKEGATCHI: minigame_train_dump(emit, user); break;
    case MINIGAME_ASTEROIDS:  minigame_asteroids_dump(emit, user); break;
    case MINIGAME_STOCKS:     minigame_stocks_dump(emit, user); break;
    case MINIGAME_TETRIS:     minigame_tetris_dump(emit, user); break;
    default: break;
    }
}
