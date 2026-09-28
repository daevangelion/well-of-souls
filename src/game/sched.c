/* The scheduled-event table and the SRN boot mixer. Both run in offline solo play,
 * so the port has them regardless of what the code was originally written for.
 * Owner: Core. */
#include "sched.h"
#include "../engine/clock.h"
#include "../engine/log.h"
#include "../engine/rng.h"

/* FUN_00456B87 (0x00456B87) appends into a table whose count lives at obj+8, capped
 * at 0x80 = 128 entries, each entry 0x610 bytes. The fields the port needs are the
 * ones the function writes: id at +0x0C, the GetTickCount stamp at +0x20C, the
 * `count` argument at +0x40C, and the random delay at +0x60C. Returns -1 when the
 * table is full, so it saturates and silently stops consuming RNG from then on. */
static SchedEntry g_sched[SCHED_ENTRIES_MAX];
static int g_sched_count;

/* FUN_00456C51 (0x00456C51) is the seeder the offline trace actually runs: exactly
 * four appends, ids 0x0E..0x11 = 14..17, all with count 0x1E = 30. */
int sched_append(int id, int count)
{
    SchedEntry *e;
    if (g_sched_count >= SCHED_ENTRIES_MAX) return -1;      /* FUN_00456B87's -1 */
    e = &g_sched[g_sched_count];
    e->id = id;
    e->tick_ms = clock_ms();                                 /* GetTickCount at +0x20C */
    e->count = count;                                        /* +0x40C */
    e->delay_ms = (crt_rand() % (count * 2)) * 1000;         /* +0x60C */
    ++g_sched_count;
    return g_sched_count - 1;
}

void sched_boot(void)
{
    int i;
    static const int ids[4] = { 0x0E, 0x0F, 0x10, 0x11 };
    g_sched_count = 0;
    for (i = 0; i < 4; ++i) sched_append(ids[i], 0x1E);       /* FUN_00456C51 */
    wos_log_event("sched_boot", "entries=%d", g_sched_count);
}

/* The consumer is FUN_00456EC1 (0x00456EC1), reached from FUN_00456D2F (0x00456D2F).
 * Its exact test has NOT been read -- what is implemented here is the shape the
 * writer implies: an entry is due when clock_ms() - tick_ms has passed its delay_ms.
 * See docs/re/timing.md section 8.2b; this is a known-open item, not a claim of parity. */
int sched_due(int *fired, int max)
{
    uint32_t now = clock_ms();
    int n = 0, i;
    for (i = 0; i < g_sched_count && n < max; ++i) {
        uint32_t due = g_sched[i].tick_ms + (uint32_t)g_sched[i].delay_ms;
        if ((int32_t)(now - due) < 0) continue;
        g_sched[i].fired = 1;
        if (fired) fired[n] = g_sched[i].id;
        ++n;
    }
    return n;
}

int sched_count(void) { return g_sched_count; }

const SchedEntry *sched_at(int i) { return (i >= 0 && i < g_sched_count) ? &g_sched[i] : NULL; }

/* FUN_0042B4E0 (0x0042B4E0): the SRN warm-up mixer. Five rand() and one GetTickCount
 * per iteration, folded together, repeating while the fold is exactly zero:
 *
 *   do {
 *     iVar1 = rand(); uVar2 = rand(); uVar3 = rand(); iVar4 = rand();
 *     DVar5 = GetTickCount(); uVar6 = rand();
 *   } while (((((iVar1<<4 ^ uVar2)<<4 ^ uVar3)<<4 ^ iVar4<<16 ^ DVar5 ^ uVar6) & 0x3FFFFFFF) == 0);
 *
 * Five draws, not a fixed count: the loop repeats if the fold lands on zero, which is
 * rare but not impossible, so the port must not hardcode five. It mixes the clock and
 * the RNG into one value, which is why clock_ms() rather than the host clock is used:
 * the harness's virtual clock is what the fold must see. */
void srn_mix(void)
{
    uint32_t fold;
    unsigned draws = 0;
    do {
        /* FOUR draws, not five. The oracle's trace has exactly four buckets in this
         * function -- 0x42B4EA / 0x42B4F5 / 0x42B4FC / 0x42B50E -- and the
         * GetTickCount at 0x42B504 returns to 0x42B50A, which is NOT one of them, so
         * the clock read is not a draw. The decompilation's five `rand()` calls are
         * Ghidra's reading; the trace is the authority on the count. */
        uint32_t a = (uint32_t)crt_rand();
        uint32_t b = (uint32_t)crt_rand();
        uint32_t c = (uint32_t)crt_rand();
        uint32_t t = clock_ms();               /* GetTickCount, not a draw */
        uint32_t d = (uint32_t)crt_rand();
        fold = ((((a << 4) ^ b) << 4 ^ c) ^ t ^ d) & 0x3FFFFFFFu;
        draws += 4;
    } while (fold == 0);
    wos_log_event("srn_mix", "draws=%u fold=%lu", draws, (unsigned long)fold);
}
