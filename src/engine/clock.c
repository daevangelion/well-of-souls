#include "clock.h"
#include "../platform/platform.h"

/* Win32 clamps uElapse below USER_TIMER_MINIMUM (10 ms) up to 10 ms. */
#define TIMER_MIN_INTERVAL 10u
#define TIMER_SLOTS 32

typedef struct {
    void *owner;
    int id;
    uint32_t interval;
    ClockTimerFn fn;
    void *user;
    uint32_t deadline; /* virtual ms at which the WM_TIMER is posted */
    uint32_t seq;      /* registration order; deterministic tie-break */
    int active;
    int pending;       /* a WM_TIMER for this timer is outstanding */
} TimerSlot;

static struct {
    uint32_t now_ms;
    uint32_t epoch_s;
    int time_pinned;   /* --time: time() runs from epoch_s on the virtual clock */
    uint32_t real_base;
    int realtime;
    uint32_t idle_last; /* FUN_0040a7c7's _DAT_004dd510 */
    uint32_t seq;
    TimerSlot slot[TIMER_SLOTS];
} g;

static int find(void *owner, int id)
{
    int i;
    for (i = 0; i < TIMER_SLOTS; ++i)
        if (g.slot[i].active && g.slot[i].owner == owner && g.slot[i].id == id) return i;
    return -1;
}

uint32_t clock_real_ms(void) { return plat_ticks_ms(); }

uint32_t clock_ms(void)
{
    if (g.realtime) {
        uint32_t t = plat_ticks_ms() - g.real_base;
        if ((int32_t)(t - g.now_ms) > 0) g.now_ms = t; /* never run backwards */
        return g.now_ms;
    }
    return g.now_ms;
}

/* The original's time() is the wall clock: FUN_00409722's date check, the two boot srand
 * calls and FUN_0040A7C7's time-vs-GetTickCount drift test all read it. Unpinned it is
 * plat_time_s(). Pinned (--time), it is the pin advanced by the VIRTUAL clock, which is
 * what the oracle hook's time() returns (vtime_s = epoch + ms/1000): a constant would
 * stop the seconds while GetTickCount runs on. The boot seeds read it at 0 ms, where
 * both forms agree. */
uint32_t clock_time_s(void) { return g.time_pinned ? g.epoch_s + g.now_ms / 1000u : plat_time_s(); }

void clock_advance(uint32_t ms)
{
    g.realtime = 0;
    g.now_ms += ms;
}

void clock_set_now(uint32_t now_ms)
{
    if ((int32_t)(now_ms - g.now_ms) >= 0) g.now_ms = now_ms;
}

void clock_attach_realtime(void)
{
    g.real_base = plat_ticks_ms() - g.now_ms;
    g.realtime = 1;
}

void clock_set_time_base(uint32_t epoch_s)
{
    g.epoch_s = epoch_s;
    g.time_pinned = epoch_s != 0;             /* 0 unpins, as plat_time_set_s(0) does */
    plat_time_set_s(epoch_s);
}

void clock_gate_reset(void);
void clock_reset(void)
{
    uint32_t epoch = g.epoch_s;
    int pinned = g.time_pinned, i;
    for (i = 0; i < TIMER_SLOTS; ++i) g.slot[i].active = 0;
    clock_gate_reset();
    g.now_ms = 0;
    g.epoch_s = epoch;
    g.time_pinned = pinned;
    g.realtime = 0;
    g.idle_last = 0;
    g.seq = 0;
}

void clock_set_timer(void *owner, int id, uint32_t interval_ms, ClockTimerFn fn, void *user)
{
    int i, free_slot = -1;
    if (interval_ms < TIMER_MIN_INTERVAL) interval_ms = TIMER_MIN_INTERVAL;
    i = find(owner, id);
    if (i < 0) {
        for (free_slot = 0; free_slot < TIMER_SLOTS; ++free_slot)
            if (!g.slot[free_slot].active) break;
        if (free_slot == TIMER_SLOTS) return; /* the original has at most two live */
        i = free_slot;
    }
    g.slot[i].owner = owner;
    g.slot[i].id = id;
    g.slot[i].interval = interval_ms;
    g.slot[i].fn = fn;
    g.slot[i].user = user;
    g.slot[i].seq = ++g.seq;
    g.slot[i].active = 1;
    g.slot[i].deadline = clock_ms() + interval_ms;
    g.slot[i].pending = 0;
}

void clock_kill_timer(void *owner, int id)
{
    int i = find(owner, id);
    if (i >= 0) g.slot[i].active = 0;
}

int clock_timer_active(void *owner, int id) { return find(owner, id) >= 0; }

static int pick(void)
{
    int i, best = -1;
    for (i = 0; i < TIMER_SLOTS; ++i) {
        if (!g.slot[i].active) continue;
        if (best < 0) { best = i; continue; }
        if (g.slot[i].deadline < g.slot[best].deadline ||
            (g.slot[i].deadline == g.slot[best].deadline && g.slot[i].seq < g.slot[best].seq))
            best = i;
    }
    return best;
}

int clock_dispatch_timers(void)
{
    int fired = 0;
    for (;;) {
        int i = pick();
        TimerSlot t;
        if (i < 0 || g.slot[i].pending) break;         /* one WM_TIMER outstanding */
        if ((int32_t)(g.slot[i].deadline - clock_ms()) > 0) break;
        /* Stamp the message with its due time, as WM_TIMER carries the queue time. */
        if ((int32_t)(g.slot[i].deadline - g.now_ms) > 0) g.now_ms = g.slot[i].deadline;
        t = g.slot[i];
        if (!g.slot[i].active || g.slot[i].seq != t.seq) continue; /* re-armed in place */
        /* WIN32 COALESCING, and this is the rule that matters. A timer with a WM_TIMER
         * already in the queue does NOT post another, so a stall delivers ONE message
         * and not one per missed period. `pending` models the outstanding message: it is
         * set as the message is posted and cleared when the handler runs, and the next
         * period is measured from the deadline so a busy handler drifts the way Windows
         * does. Walking `deadline += interval` unconditionally, which is what this did
         * before, replays every missed period -- the oracle measured 6 extra draws over a
         * 650 ms script window, and that is this line. */
        g.slot[i].pending = 1;
        g.slot[i].deadline += t.interval;
        t.fn(t.owner, t.user);
        /* The handler ran, so the message was taken: clear it and let the next period
         * fire from wherever the clock now is. */
        if (g.slot[i].active && g.slot[i].seq == t.seq) g.slot[i].pending = 0;
        ++fired;
        if (fired > TIMER_SLOTS * 2) break; /* a handler that re-arms at 10 ms */
    }
    return fired;
}

int clock_idle_due(void)
{
    uint32_t now = clock_ms();
    if ((uint32_t)(now - g.idle_last) <= 0x13u) return 0; /* > 19 ms, i.e. 20 ms */
    /* Re-stamp with the OBSERVED time, exactly as FUN_0040a7c7 does
     * (`_DAT_004dd510 = GetTickCount()`). This matters: the original calls
     * GetTickCount twice per idle pass and never replays the boundaries it slept
     * through, so a 5 s stall costs ONE idle tick, not 250. Re-arming from the
     * previous boundary would make the two disagree on any stall.
     * Determinism is not lost: the script loop in game_main.c steps the clock onto
     * each 20 ms boundary (clock_20hz_next()), so under a .dsc schedule the gate
     * always sees now - last == 20 and fires exactly once per boundary. */
    g.idle_last = now;
    return 1;
}

/* The next 20 ms boundary. It CATCHES the stamp up to the present time first, so
 * a driver that asks for the next boundary always gets one strictly greater than
 * now. Without the catch-up this returned a value <= now once the clock had
 * advanced past the stamp, the script loop's `min(next, 20hz_next())` stopped
 * moving, and the virtual clock froze -- silently starving every ms-gated rule in
 * the port. The catch-up is idempotent for a given now, so the loop still lands on
 * each boundary exactly once. */
uint32_t clock_20hz_next(void)
{
    uint32_t now = clock_ms();
    while ((uint32_t)(now - g.idle_last) >= 20u) g.idle_last += 20u;
    return g.idle_last + 20u;
}

/* --- private rate gates (the `GetTickCount() - last < N` idiom) --------------- */
#define GATE_SLOTS 32
static struct { void *owner; int id; uint32_t interval, last; int used; } g_gate[GATE_SLOTS];

int clock_gate(void *owner, int id, uint32_t interval_ms)
{
    int i, free_slot = -1;
    uint32_t now = clock_ms();
    for (i = 0; i < GATE_SLOTS; ++i) {
        if (g_gate[i].used && g_gate[i].owner == owner && g_gate[i].id == id) break;
        if (free_slot < 0 && !g_gate[i].used) free_slot = i;
    }
    if (i == GATE_SLOTS) {
        if (free_slot < 0) return 0;   /* exhausted; the original has at most two */
        i = free_slot;
        g_gate[i].owner = owner; g_gate[i].id = id; g_gate[i].interval = interval_ms;
        g_gate[i].last = now; g_gate[i].used = 1;
    }
    if ((uint32_t)(now - g_gate[i].last) < interval_ms) return 0;
    g_gate[i].last += interval_ms;      /* re-arm from the boundary, never from now */
    return 1;
}

void clock_gate_reset(void)
{
    int i;
    for (i = 0; i < GATE_SLOTS; ++i) g_gate[i].used = 0;
}
