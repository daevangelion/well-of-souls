#include "boot.h"
#include "encint.h"
#include "log.h"
#include "rng.h"
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
void boot_register_core(void);

/* These live in src/game/sched.c, which the self-test targets do not link, so they
 * are weak: a target without the game modules still links, and the step simply
 * does nothing there. The selftest asserts the boot registry, not the game. */
__attribute__((weak)) void sched_boot(void);
__attribute__((weak)) void srn_mix(void);
static void sched_boot_step(void *u) { (void)u; if (sched_boot) sched_boot(); }
static void srn_mix_step(void *u) { (void)u; if (srn_mix) srn_mix(); }

void boot_register_core(void)
{
    /* FUN_00401051 / the thunk at 0x00401101: 100 x 0x120, EncInt at +0xE8, ascending. */
    boot_register(BOOT_CRT_TABLE_A, "tableA_100x0x120", construct_table_a, NULL);
    /* FUN_004269AF is a C++ static initialiser, which the CRT runs BEFORE the
     * _initterm table, so its srand/draw/srand happens before anything here --
     * game_main.c's seed_crt() does it, and then calls boot_run(). */

    /* The two remaining offline boot draws. Their exact CRT indices are NOT
     * established: FUN_00456C51 is reached from FUN_00456D2F, which the main
     * window's state machine calls, and FUN_0042B4E0 from 0x004096E7, the serial
     * registration path. Both are placed after the tables, which is where the
     * trace puts them, and both are logged with their crt index so a wrong
     * placement is visible in boot_step rather than silent. */
    boot_register(39, "sched_boot_FUN_00456C51", sched_boot_step, NULL);
    boot_register(40, "srn_mix_FUN_0042B4E0", srn_mix_step, NULL);
}
