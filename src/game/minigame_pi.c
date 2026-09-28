/* GAME 3 — "The Search for Pi at Home". See minigame_pi.h for the rules and their VAs. */
#include "minigame_pi.h"
#include "../engine/clock.h"
#include "../engine/font.h"
#include "../engine/log.h"
#include "../engine/rng.h"
#include <stdio.h>
#include <string.h>

#define WIN_W 142
#define WIN_H 178
/* the plot static's rect, in the dialog's client coordinates (DLGTEMPLATE 0xBF) */
#define PLOT_X 8
#define PLOT_Y 26
#define PLOT_W 126
#define PLOT_H 96

static struct {
    int open;
    double in_circle;   /* this+0x68 */
    double dead_6c;     /* this+0x6C, zeroed and never used */
    double total;       /* this+0x70 */
    double dead_74;     /* this+0x74 */
    uint32_t next_timer;
    int t0;
} g;

/* FUN_0047437E (0x47437E): 1 + 8 rand() folded into a 16-bit value, & 0x7FFFFFFF.
 * Ghidra prints the fold as `u = u << 4 ^ r >> 4` over 8 iterations after one seed rand. */
static unsigned big_number(void)
{
    unsigned u = (unsigned)crt_rand();
    int i;
    for (i = 0; i < 8; ++i) u = (u << 4) ^ ((unsigned)crt_rand() >> 4);
    return u & 0x7FFFFFFFu;
}

int minigame_pi_open(void)
{
    memset(&g, 0, sizeof g);
    g.open = 1;
    /* FUN_004742F0 (0x4742F0): zero the counters and arm SetTimer(id 2, 50 ms). */
    g.next_timer = 0;
    g.t0 = (int)clock_ms();
    wos_log_event("minigame_pi_open", "timer_ms=%d", MG_PI_TIMER_MS);
    return 1;
}

void minigame_pi_close(void)
{
    g.open = 0;
    wos_log_event("minigame_pi_close", "in=%d total=%d", (int)g.in_circle, (int)g.total);
}

/* One 50 ms OnTimer tick: 100 samples, 40 rand() each. */
static void tick(void)
{
    int s;
    for (s = 0; s < MG_PI_SAMPLES_PER_TICK; ++s) {
        /* 2 rand() for the plot point (FUN_004743E8 chooses a cell in the 4-column field) */
        int px = crt_rand() & 0x7FFF;
        int py = crt_rand() & 0x7FFF;
        /* two calls of FUN_0047437E: 18 rand() */
        unsigned a = big_number();
        unsigned b = big_number();
        (void)a; (void)b;
        /* the in-circle test uses two further independent samples, dx = rand - 16383.5 */
        {
            double dx = (double)crt_rand() - 16383.5;
            double dy = (double)crt_rand() - 16383.5;
            g.total += 1.0;
            if (dx * dx + dy * dy <= 1.0) g.in_circle += 1.0;
        }
        (void)px; (void)py;
    }
}

void minigame_pi_update(uint32_t now_ms)
{
    if (!g.open) return;
    /* Win32 SetTimer semantics: fire once per elapsed interval, never replay the backlog. */
    while ((int)(now_ms - g.next_timer) >= MG_PI_TIMER_MS) {
        g.next_timer += MG_PI_TIMER_MS;
        tick();
    }
}

void minigame_pi_click(int x, int y, int pressed)
{
    /* The 0xBF class has no click, key or button handler at all. */
    (void)x; (void)y; (void)pressed;
}

void minigame_pi_key(int vk, int pressed)
{
    (void)vk; (void)pressed;
}

void minigame_pi_render(Framebuffer *fb, int bx, int by)
{
    Rect win = { bx, by, WIN_W, WIN_H };
    char buf[64];
    if (!g.open) return;
    fb_fill(fb, win, 0x101010u);
    fb_rect(fb, win, 0x9d8959u);
    fb_rect(fb, (Rect){bx + PLOT_X, by + PLOT_Y, PLOT_W, PLOT_H}, 0x000000u);
    if (g.total > 0.0)
        snprintf(buf, sizeof buf, "pi ~ %.6f", g.in_circle / g.total);
    else
        snprintf(buf, sizeof buf, "pi ~ 0.000000");
    font_draw(fb, bx + PLOT_X, by + PLOT_Y + PLOT_H + 8, buf, 0xffe6aeu);
    snprintf(buf, sizeof buf, "samples %d", (int)g.total);
    font_draw(fb, bx + PLOT_X, by + PLOT_Y + PLOT_H + 20, buf, 0xffe6aeu);
}

void minigame_pi_dump(DumpEmit emit, void *user)
{
    dump_emit_int(emit, "minigame.pi.in_circle", (long long)g.in_circle, user);
    dump_emit_int(emit, "minigame.pi.total", (long long)g.total, user);
    dump_emit_int(emit, "minigame.pi.estimate",
                  g.total > 0.0 ? (long long)(g.in_circle / g.total * 1000000.0) : 0, user);
}
