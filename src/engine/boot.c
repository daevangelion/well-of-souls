#include "boot.h"
#include "encint.h"
#include "log.h"
#include "rng.h"
#include "clock.h"
#include <string.h>

typedef struct { int crt_index; const char *name; BootFn fn; void *user; } BootStep;
static BootStep g_steps[BOOT_STEPS_MAX];
static int g_count, g_ran;
static unsigned long long g_rng_after;

int boot_register(int crt_index, const char *name, BootFn fn, void *user)
{
    if (g_count == BOOT_STEPS_MAX || !fn) return -1;
    g_steps[g_count].crt_index = crt_index;
    g_steps[g_count].name = name ? name : "?";
    g_steps[g_count].fn = fn;
    g_steps[g_count].user = user;
    ++g_count;
    return 0;
}

void boot_run(void)
{
    int i, j;
    if (g_ran) return;
    g_ran = 1;
    /* Ascending CRT index, then registration order within an index. The table is
     * tiny and boot runs once, so an insertion sort is the right size of tool. */
    for (i = 1; i < g_count; ++i) {
        BootStep key = g_steps[i];
        for (j = i - 1; j >= 0 && g_steps[j].crt_index > key.crt_index; --j)
            g_steps[j + 1] = g_steps[j];
        g_steps[j + 1] = key;
    }
    for (i = 0; i < g_count; ++i) {
        wos_log_event("boot_step", "crt=%d name=%s calls=%llu", g_steps[i].crt_index,
                      g_steps[i].name, (unsigned long long)crt_rand_calls());
        g_steps[i].fn(g_steps[i].user);
    }
    g_rng_after = crt_rand_calls();
    wos_log_event("boot_done", "steps=%d rng_calls=%llu", g_count,
                  (unsigned long long)g_rng_after);
}

int  boot_ran(void) { return g_ran; }
int  boot_step_count(void) { return g_count; }
const char *boot_step_name(int i) { return (i >= 0 && i < g_count) ? g_steps[i].name : NULL; }
unsigned long long boot_rng_calls(void) { return g_rng_after; }

/* --- Table A, CRT index 2 ----------------------------------------------------
 * The original's array is at 0x0052C978 in .bss: 100 elements of 0x120 bytes with
 * the EncInt at +0xE8 inside each, walked ASCENDING by the eh vector constructor
 * iterator at 0x00401101. The port keeps the same shape -- same element size, same
 * EncInt offset, same order -- so the draws land identically. */
static unsigned char g_table_a[BOOT_TABLE_A_COUNT * BOOT_TABLE_A_STRIDE];

void *boot_table_a_base(void) { return g_table_a; }

static void construct_table_a(void *user)
{
    (void)user;
    enc_construct_array(g_table_a, BOOT_TABLE_A_OFFSET, BOOT_TABLE_A_STRIDE,
                        BOOT_TABLE_A_COUNT, ENC_ASCENDING);
}

/* The port's own registration, in one place so the order is auditable. */
/* --seed pins the time() value the seed step uses; it does NOT move the step. */
uint32_t boot_seed_pin;
int boot_seed_have_pin;
void boot_seed_step(void)
{
    /* The original is `srand(time(NULL))` -- the seed IS the wall clock, and the
     * wall clock is what --time pins. The legacy --seed flag is the .rpl replay
     * pin and must NOT override this: with --seed 1 --time 1234567890 the port was
     * seeding from 1 while the oracle seeded from 1234567890, which is why the boot
     * COUNT matched and the state did not. */
    /* The legacy .rpl replays pass --seed N without --time; for them N stands in
     * for time(NULL) here so a replay run is deterministic. --time wins when given. */
    uint32_t t = boot_seed_have_pin ? boot_seed_pin : clock_time_s();
    crt_srand(t);
    /* 1409: 0x00426B27, the base-offset draw. abs(draw) & 0x3FFF & ~15 is 0..0x3FF0;
     * DAT_004E4870 adds a malloc block and DAT_0067FBF8 is that + 0x1560A5C. Only
     * the masked draw is comparable, since the block is a heap address. */
    { int draw = crt_rand(); crt_boot_base_offset_set((uint32_t)(draw & 0x3fff) & ~0xfu); }
    /* 1410: 0x0048E1CF, inside FUN_0048E19A, which FUN_00427D89 calls before
     * allocating hero slot 0. It is the SAME boot draw, not a second one --
     * FrontHero-2 had it in hero_allocate_slot(), which runs in game_boot() after
     * boot_run(), so it was being taken a step too late as well as duplicated. */
    (void)crt_rand();
    crt_srand(t);
    wos_log_event("rng_seeded", "seed=%lu", (unsigned long)t);
}

void boot_register_core(void);

/* These live in src/game/sched.c, which the self-test targets do not link, so they
 * are weak: a target without the game modules still links, and the step simply
 * does nothing there. The selftest asserts the boot registry, not the game. */
__attribute__((weak)) void srn_mix(void);
static void srn_mix_step(void *u) { (void)u; if (srn_mix) srn_mix(); }

/* FUN_004269AF: tVar3 = time(NULL); srand(tVar3); uVar1 = rand(); ... srand(tVar3).
 * The one draw between the two seeds places the hero block:
 * DAT_004E4870 = (draw & 0x3FFF & ~15) + <malloc block>, DAT_0067FBF8 = that + 0x1560A5C.
 * The masked draw is what is published, since the block is a heap address. */
static void seed_step(void *u) { (void)u; boot_seed_step(); }

void boot_register_core(void)
{
    /* FUN_00401051 / the thunk at 0x00401101: 100 x 0x120, EncInt at +0xE8, ascending. */
    boot_register(BOOT_CRT_TABLE_A, "tableA_100x0x120", construct_table_a, NULL);
    /* FUN_004269AF's srand pair, at index 41 -- i.e. AFTER both EncInt tables.
     * Oracle3's trace is decisive: calls 1..1408 are the two tables, contiguous
     * in FUN_0049B6C7, and draw #1409 is 0x00426B27, the base-offset draw inside
     * FUN_004269AF. So the 1408 table draws run on the CRT's DEFAULT holdrand of
     * 1, not on the time seed, and the seed happens after them. Running the
     * srand pair first (as the port did) put every table draw on the wrong side
     * of a re-seed: the call COUNT still matched while rng.state was generated
     * from a different seed entirely. crt_rand_state() after N calls is a
     * function of the seed and N alone, so equal count + different state is
     * arithmetically only possible if the seed sits at a different point.
     *
     * FUN_004269AF is not itself one of the 52 entries in the table at
     * 0x004DC000 -- it is reached from a separate _initterm range -- so 41 is
     * "somewhere above 38", which is all the ordering requires. Index 41 is a
     * real entry (0x0045ACD0), so the number is not invented.
     *
     * FUN_00456C51 is NOT registered at all: the scheduler is reached from
     * FUN_00456D2F via the main window's state machine, which is runtime, and
     * removing it moved the boot count by exactly 4 as predicted. */
    boot_register(41, "seed_FUN_004269AF", seed_step, NULL);
    /* FUN_0042B4E0, the SRN warm-up mixer, called from 0x004096E7 on the serial
     * path. Its exact position is not established either; it is after the seed
     * pair, which is all that is known and all that is claimed. */
    boot_register(42, "srn_mix_FUN_0042B4E0", srn_mix_step, NULL);
}
