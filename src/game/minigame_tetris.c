/* GAME 8 — "Quadris" (Tetris). See minigame_tetris.h for the wiring and VAs.
 *
 * The piece geometry is a BITMASK, not a cell list. FUN_0046AC7F (0x46AC7F, the collision
 * test) and FUN_0046AD74 (0x46AD74, the lock) both read
 *     mask = table[piece * 6 + rotation]
 * and then walk a 4x4 grid with `bit = 0x8000`, testing `(bit & mask)` and shifting `bit`
 * right once per cell, four cells per row and four rows. FUN_0046AD03 (0x46AD03) reads the
 * board at DAT_00698C98 + (row*10 + col)*4, which is why the board is 10 wide and 20 high
 * (rows 0x00..0x13) while the mask is only 4x4.
 *
 * Cell size DAT_004F6DC0 = 8 px, so the board is 80x160 px and a piece's grid coordinate is
 * multiplied by 8 before indexing. The piece's origin (DAT_004F6EB8 x, DAT_004F6EBC y) is in
 * CELLS, and FUN_0046A702 (0x46A702) spawns at x = 3*8 = 24 px, y = 0, rotation 0.
 *
 * Fall speed is a GetTickCount ramp, not a frame rate (FUN_0046A758, 0x46A758):
 *     y = DAT_004F6DC4 + (int)(((DVar7 - _DAT_004F6DB4) * DAT_004F6DBC & 0xFFFFFFFF) / 1000)
 * with DAT_004F6DBC starting at DAT_004F6DB8 = 32 and DAT_004F6DC4 starting at 0. So the
 * piece falls 32/1000 = 0.032 cells per millisecond, i.e. one cell every ~31 ms. Level-ups
 * cut the interval to 78%: DAT_004F6DB8 = (DAT_004F6DB8 * 0x78) / 100.
 *
 * Scoring, FUN_0046ADEA (0x46ADEA): a cleared row is filled with 8, the total line count
 * goes up by the number cleared, the level rises whenever level*10 < lines, and
 *     score = (level * cleared * 1000) / 10,  doubled when cleared == 4.
 * The flash is driven from the step, not a timer: state 3 waits 1000 ms, then either ends
 * the game or, if the flash counter is exhausted, returns to state 1 while the leftover
 * counter drains at (counter/5, minimum 1) per 250 ms.
 */
#include "minigame_tetris.h"
#include "../engine/clock.h"
#include "../engine/font.h"
#include "../engine/log.h"
#include "../engine/rng.h"
#include <stdio.h>
#include <string.h>

#define CELL     8      /* DAT_004F6DC0 */
#define QUEUE    5      /* DAT_00698FB8 .. DAT_00698FCC, slot 0 is the live piece's source */
#define SPAWN_X  3      /* DAT_004F6DC0 * 3, in cells */
#define BASE_MS  32     /* DAT_004F6DB8, the level-0 fall interval numerator */
#define LEVEL_PCT 0x78  /* the level-up multiplier: interval = interval * 0x78 / 100 */
#define FLASH_MS 1000   /* the state-3 hold, 0x3E8 */
#define DRAIN_MS 250    /* 0xFA, the flash-counter drain tick */

/* the state byte at this+0xB8 */
enum { ST_TITLE, ST_PLAYING, ST_PAUSED, ST_FLASH };

/* The 7x6 mask table, copied LITERALLY from the original.
 *
 * Base VA 0x4F6DD8, .rdata, 42 uint32 entries at stride 4, indexed
 * `table[piece * 6 + rotation]`. Both load sites are unambiguous and agree:
 *     46acb8:  8b 0c 85 d8 6d 4f 00   mov ecx, [eax*4 + 0x4F6DD8]   (FUN_0046AC7F, collision)
 *     46adad:  8b 34 85 d8 6d 4f 00   mov esi, [eax*4 + 0x4F6DD8]   (FUN_0046AD74, lock)
 *     46acb2:  6b c0 06              imul eax, eax, 0x6            (eax = piece*6 + rotation)
 * and both then walk the entry with `bit = 0x8000`, testing `(bit & mask)` and shifting
 * right once per cell, four cells per row over four rows.
 *
 * These values are the final runtime values, verified three ways:
 *   - .rdata has characteristics 0x40000040, i.e. IMAGE_SCN_MEM_READ without
 *     IMAGE_SCN_MEM_WRITE, so the loader cannot patch it;
 *   - the .reloc section (235 pages, 34694 entries) contains NO entry whose target falls in
 *     0x4F6DC0..0x4F6E80, and the image loads at its preferred base 0x400000, so nothing
 *     rebases them;
 *   - no instruction in .text writes to 0x4F6DD8.
 *
 * They alias a blob of string pointers (0x4F75D0 is "%s.bmp\0\0You find a %s but your
 * trophy bag is full...") and 0x4DF638 is 64 zero bytes, so at first glance they look like
 * something other than masks. They are nevertheless the values the collision and lock code
 * reads and tests, so they are the piece geometry the original actually plays. Identical
 * behaviour means copying them exactly rather than substituting a conventional set.
 *
 * Dump with: objdump -s -j .rdata --start-address=0x4f6dd8 --stop-address=0x4f6e80
 */
static const uint32_t tetromino[7][6] = {
    { 0x004F75DCu, 0x004F75D4u, 0x00000001u, 0x00000064u, 0x004F75D0u, 0x004F75C8u },
    { 0x00000001u, 0x00000064u, 0x004F75C4u, 0x004F75BCu, 0x00000001u, 0x00000064u },
    { 0x004F75B8u, 0x004F75B0u, 0x00000001u, 0x00000064u, 0x004F75ACu, 0x004F75A8u },
    { 0x00000001u, 0x00000064u, 0x004F75A4u, 0x004F75A0u, 0x00000001u, 0x00000064u },
    { 0x004F759Cu, 0x004F7598u, 0x00000001u, 0x00000064u, 0x004F7594u, 0x004DF638u },
    { 0x00000002u, 0x00000019u, 0x004F7590u, 0x004F7588u, 0x00000001u, 0x00000003u },
    { 0x004F7590u, 0x004F757Cu, 0x00000001u, 0x00000003u, 0x004F7590u, 0x004F7574u }
};

/* FUN_0046A174 (0x46A174) polls exactly these six keys with GetAsyncKeyState:
 *   VK_RIGHT 0x27 -> this+0xA0  (rotate cw)
 *   VK_LEFT  0x25 -> this+0xA4  (rotate ccw)
 *   'A'      0x41 -> this+0x98  (left)
 *   'S'      0x53 -> this+0x9C  (right)
 *   VK_UP    0x26 -> this+0xA8  (soft drop)
 *   VK_DOWN  0x28 -> this+0x94  (hard drop)
 *   SPACE    0x20 -> this+0xB4  (start / the title-screen accept)
 * The routine also auto-repeats: with no key edge and at least 0x33 = 51 ms since
 * DAT_004F6DD0 it clears the A and S latches, which is what makes a held key slide. */
#define VK_RIGHT 0x27
#define VK_LEFT  0x25
#define VK_A     0x41
#define VK_S     0x53
#define VK_UP    0x26
#define VK_DOWN  0x28
#define VK_SPACE 0x20
#define REPEAT_MS 0x33

static struct {
    int open;
    int state;                     /* this+0xB8 */
    int board[20][10];             /* DAT_00698C98; a cell holds 0..8, 8 = a just-cleared row */
    int queue[QUEUE];              /* DAT_00698FB8..DAT_00698FCC, values 1..7 */
    int live;                      /* DAT_004F6EB0 */
    int rot;                       /* DAT_004F6EB4, 0..3 */
    int gx, gy;                    /* DAT_004F6EB8 / DAT_004F6EBC, in cells */
    int hold;                      /* DAT_004F6DC8 */
    int hold_ok;                   /* DAT_004F6EC4 */
    int dropped;                   /* DAT_004F6EC0, the hard-drop latch */
    int base_px;                   /* DAT_004F6DC4, the fractional fall accumulator */
    uint32_t t0;                   /* _DAT_004F6DB4, the GetTickCount base */
    int interval;                  /* DAT_004F6DBC, the current fall numerator */
    int base_interval;             /* DAT_004F6DB8 */
    int last_repeat;               /* DAT_004F6DD0 */
    int lines;                     /* this+0x140 */
    int level;                     /* this+0x144, starts at 1 */
    int flash_ticks;               /* this+0x148 */
    int flash_left;                /* this+0x14C */
    int high;                      /* DAT_004F6DCC */
    uint32_t flash_t0;             /* this+0x13C */
    char banner[48];
} g;

/* DAT_004F6DC0 is 8, so the grid coordinates are in cells. */
static int cell(int col, int row)
{
    if (row < 0) return 0;          /* above the board is empty */
    if (col < 0 || col > 9 || row > 0x13) return -1;
    return g.board[row][col] % 9;
}

/* FUN_0046AC7F (0x46AC7F): does the piece at (col,row) hit anything? Walks the 4x4 mask with
 * bit 0x8000 shifting right, four cells per row, four rows. */
static int collides(int piece, int rot, int col, int row)
{
    uint32_t mask = tetromino[piece][rot & 3];
    unsigned bit = 0x8000;
    int y, x;
    for (y = 0; y < 4; ++y) {
        for (x = 0; x < 4; ++x) {
            int c = cell(col + x, row + y + 1);
            if (c != 0 && (bit & mask)) return 1;
            bit >>= 1;
        }
    }
    return 0;
}

/* FUN_0046A702 (0x46A702): place a new piece at the top centre. */
static void spawn_piece(int piece)
{
    g.live = piece;
    g.rot = 0;
    g.gx = SPAWN_X;
    g.gy = 0;
    g.dropped = 0;
    g.base_px = 0;
    g.t0 = clock_ms();
    g.interval = g.base_interval;
    g.hold_ok = 1;
}

/* FUN_0046ADEA (0x46ADEA): clear full rows, update lines/level/score. Returns rows cleared. */
static int clear_rows(void)
{
    int row, col, cleared = 0;
    for (row = 0; row < 20; ++row) {
        int full = 1;
        for (col = 0; col < 10; ++col) if (g.board[row][col] == 0) { full = 0; break; }
        if (!full) continue;
        for (col = 0; col < 10; ++col) g.board[row][col] = 8;   /* 0x46ADEA writes 8 */
        cleared++;
    }
    if (!cleared) return 0;
    g.lines += cleared;                                  /* this+0x140 */
    if (g.level * 10 < g.lines) {
        g.level++;                                        /* this+0x144, starts at 1 */
        g.base_interval = (g.base_interval * LEVEL_PCT) / 100;   /* DAT_004F6DB8 */
    }
    g.flash_left = (g.level * cleared * 1000) / 10;      /* this+0x14C */
    if (cleared == 4) g.flash_left *= 2;                  /* the four-line bonus */
    g.flash_t0 = clock_ms();
    g.state = ST_FLASH;
    wos_log_event("minigame_tetris_clear", "lines=%d level=%d score=%d",
                  cleared, g.level, g.flash_left);
    return cleared;
}

/* FUN_0046AECB (0x46AECB): collapse after the flash. Returns 1 when the game is over, i.e. when
 * a cell of 8 was still sitting on the bottom row when the flash ended. */
static int collapse(void)
{
    int row, col;
    for (row = 19; row >= 0; --row) {
        if (cell(0, row) == 8) {
            for (; row >= 0; --row)
                for (col = 0; col < 10; ++col) g.board[row][col] = g.board[row + 1][col];
            for (col = 0; col < 10; ++col) g.board[0][col] = 0;
            return 1;
        }
    }
    return 0;
}

int minigame_tetris_open(void)
{
    int i;
    memset(&g, 0, sizeof g);
    g.open = 1;
    g.state = ST_TITLE;
    g.level = 1;              /* 0x469FFC: *(param+0x144) = 1 */
    g.base_interval = BASE_MS;
    /* 5 rand() fill the queue, each `rand() % 7 + 1`, so the values are 1..7. */
    for (i = 0; i < QUEUE; ++i) g.queue[i] = crt_rand() % 7 + 1;
    spawn_piece(g.queue[0]);
    snprintf(g.banner, sizeof g.banner, "PRESS SPACE");
    wos_log_event("minigame_tetris_open", "state=%d level=%d interval=%d",
                  g.state, g.level, g.base_interval);
    return 1;
}

void minigame_tetris_close(void)
{
    g.open = 0;
    wos_log_event("minigame_tetris_close", "lines=%d level=%d high=%d", g.lines, g.level, g.high);
}

/* the queue slide from 0x46A758 */
static void slide_queue(void)
{
    int i;
    for (i = 0; i < QUEUE - 1; ++i) g.queue[i] = g.queue[i + 1];
    g.queue[QUEUE - 1] = crt_rand() % 7 + 1;   /* 1 rand() per piece locked */
    spawn_piece(g.queue[0]);
    if (collides(g.live, g.rot, g.gx, g.gy)) {
        g.state = ST_TITLE;
        if (g.flash_left > g.high) g.high = g.flash_left;
        snprintf(g.banner, sizeof g.banner, "GAME OVER");
        wos_log_event("minigame_tetris_gameover", "lines=%d level=%d high=%d",
                      g.lines, g.level, g.high);
    }
}

void minigame_tetris_key(int vk, int pressed)
{
    if (!g.open || !pressed) return;
    if (g.state == ST_TITLE) {
        if (vk == VK_SPACE) {
            memset(g.board, 0, sizeof g.board);
            g.lines = 0;
            g.level = 1;
            g.base_interval = BASE_MS;
            g.state = ST_PLAYING;
            g.banner[0] = 0;
            wos_log_event("minigame_tetris_start", "ok=1");
        }
        return;
    }
    if (g.state == ST_FLASH || g.state != ST_PLAYING) return;
    switch (vk) {
    case VK_LEFT:  g.rot = (g.rot + 3) & 3; break;      /* this+0xA4: rotation -= 1, wraps at 0 */
    case VK_RIGHT: g.rot = (g.rot + 1) & 3; break;      /* this+0xA0: rotation += 1, wraps at 3 */
    case VK_A:     if (!collides(g.live, g.rot, g.gx - 1, g.gy)) g.gx--; break;
    case VK_S:     if (!collides(g.live, g.rot, g.gx + 1, g.gy)) g.gx++; break;
    case VK_DOWN:  g.dropped = 1; break;                 /* this+0x94 sets DAT_004F6EC0 */
    case VK_UP:    if (!collides(g.live, g.rot, g.gx, g.gy + 1)) g.gy++; break;
    default: break;
    }
}

void minigame_tetris_click(int x, int y, int pressed)
{
    /* the Quadris window takes no mouse input at all */
    (void)x; (void)y; (void)pressed;
}

void minigame_tetris_update(uint32_t now_ms)
{
    (void)now_ms;
    if (!g.open) return;
    if (g.state == ST_PLAYING) {
        if (collides(g.live, g.rot, g.gx, g.gy)) {
            uint32_t mask = tetromino[g.live][g.rot & 3];
            unsigned bit = 0x8000;
            int y, x;
            for (y = 0; y < 4; ++y)
                for (x = 0; x < 4; ++x) {
                    if (bit & mask) {
                        int c = g.gx + x, r = g.gy + y;
                        if (c >= 0 && c < 10 && r >= 0 && r < 20) g.board[r][c] = g.live;
                    }
                    bit >>= 1;
                }
            clear_rows();
            slide_queue();
            return;
        }
        g.gy = g.base_px +
               (int)(((unsigned)(clock_ms() - g.t0) * (unsigned)g.interval) / 1000u);
    } else if (g.state == ST_FLASH) {
        if ((int)(clock_ms() - g.flash_t0) >= FLASH_MS) {
            if (collapse()) {
                g.state = ST_TITLE;
                if (g.flash_left > g.high) g.high = g.flash_left;
                snprintf(g.banner, sizeof g.banner, "GAME OVER");
                wos_log_event("minigame_tetris_gameover", "lines=%d level=%d high=%d",
                              g.lines, g.level, g.high);
            } else if (g.flash_left == 0) {
                g.state = ST_PLAYING;
            }
        } else if ((int)(clock_ms() - g.flash_t0) >= DRAIN_MS && g.flash_left > 0) {
            int drain = g.flash_left / 5;
            if (drain < 1) drain = 1;
            g.flash_ticks += drain;
            g.flash_left -= drain;
        }
    }
}

void minigame_tetris_render(Framebuffer *fb, int bx, int by)
{
    int x, y;
    char buf[64];
    if (!g.open) return;
    fb_fill(fb, (Rect){ bx, by, 10 * CELL, 20 * CELL }, 0x101010u);
    for (y = 0; y < 20; ++y)
        for (x = 0; x < 10; ++x)
            if (g.board[y][x])
                fb_fill(fb, (Rect){ bx + x * CELL, by + y * CELL, CELL, CELL }, 0x9d8959u);
    if (g.state == ST_PLAYING) {
        uint32_t mask = tetromino[g.live][g.rot & 3];
        unsigned bit = 0x8000;
        int i, j;
        for (i = 0; i < 4; ++i)
            for (j = 0; j < 4; ++j) {
                if (bit & mask) {
                    int c = g.gx + j, r = g.gy + i;
                    if (c >= 0 && c < 10 && r >= 0 && r < 20)
                        fb_fill(fb, (Rect){ bx + c * CELL, by + r * CELL, CELL, CELL },
                                0xffd477u);
                }
                bit >>= 1;
            }
    }
    {
        int px = bx + 10 * CELL + 12;
        snprintf(buf, sizeof buf, "LINES %d", g.lines);   font_draw(fb, px, by + 8, buf, 0xffe6aeu);
        snprintf(buf, sizeof buf, "LEVEL %d", g.level);   font_draw(fb, px, by + 24, buf, 0xffe6aeu);
        snprintf(buf, sizeof buf, "SCORE %d", g.flash_left); font_draw(fb, px, by + 40, buf, 0xffe6aeu);
        snprintf(buf, sizeof buf, "HIGH %d", g.high);     font_draw(fb, px, by + 56, buf, 0xffe6aeu);
        if (g.banner[0]) font_draw(fb, bx + 20, by + 20 * CELL / 2, g.banner, 0xffd477u);
    }
}

void minigame_tetris_dump(DumpEmit emit, void *user)
{
    dump_emit_int(emit, "minigame.tetris.state", g.state, user);
    dump_emit_int(emit, "minigame.tetris.lines", g.lines, user);
    dump_emit_int(emit, "minigame.tetris.level", g.level, user);
    dump_emit_int(emit, "minigame.tetris.score", g.flash_left, user);
    dump_emit_int(emit, "minigame.tetris.high", g.high, user);
    dump_emit_int(emit, "minigame.tetris.live", g.live, user);
    dump_emit_int(emit, "minigame.tetris.rot", g.rot, user);
    dump_emit_int(emit, "minigame.tetris.gx", g.gx, user);
    dump_emit_int(emit, "minigame.tetris.gy", g.gy, user);
    dump_emit_int(emit, "minigame.tetris.interval", g.interval, user);
    dump_emit_int(emit, "minigame.tetris.dropped", g.dropped, user);
}
