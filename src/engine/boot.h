/* The boot sequence, in the original's CRT initialiser order.
 *
 * The original's 52-entry initialiser table lives at 0x004DC000..0x004DC0D0, passed
 * to `_initterm(0x4DC000, 0x4DC0D0)` from the entry point at 0x004C5AF0 (the call is
 * at 0x004C5BC8). It runs in ASCENDING index order, and that order is a contract:
 * the EncInt constructions at indices 2 and 38 consume 1408 of the 1409 boot draws
 * between them, so moving any of them shifts every later value in the session. A
 * module therefore cannot build its own EncInts inside its own initialiser -- the
 * order is global, and this registry is where it is expressed.
 *
 * Modules register a step against the CRT index the original uses, and boot_run()
 * executes the registered steps in ascending index. Owner: Core. */
#ifndef WOS_BOOT_H
#define WOS_BOOT_H

#include <stddef.h>
#include <stdint.h>

/* The CRT indices we have identified. Anything not listed is deliberately
 * unregistered rather than guessed at a wrong index. */
#define BOOT_CRT_TABLE_A     2    /* the 100 x 0x120 EncInt array          */
#define BOOT_CRT_TABLE_B    38    /* the 4 x 9 x 7 EncInt object groups     */
#define BOOT_CRT_GLOBAL_CTOR 1    /* FUN_004269AF: srand, the base draw, srand */

#define BOOT_STEPS_MAX 64

typedef void (*BootFn)(void *user);

/* Register a step. Two steps may share an index, in which case they run in
 * registration order. Returns 0, or -1 if the registry is full. */
int  boot_register(int crt_index, const char *name, BootFn fn, void *user);
/* Execute every registered step in ascending CRT index, then by registration
 * order within an index. Idempotent: a second call does nothing. */
void boot_run(void);
int  boot_ran(void);
int  boot_step_count(void);
const char *boot_step_name(int i);
/* crt_rand_calls() after boot_run() returns; logged as `boot.rng_calls`. */
unsigned long long boot_rng_calls(void);
/* Registers the Core-owned steps (currently only table A). Call once, before
 * boot_run(). Safe to call more than once. */
void boot_register_core(void);
/* Set before boot_run(): --seed pins the time() VALUE the seed step uses. It does
 * not move the step -- FUN_004269AF's srand pair runs at CRT index 41, after both
 * EncInt tables, because the 1408 table draws precede it and run on the CRT's
 * default holdrand of 1. */
extern uint32_t boot_seed_pin;
extern int boot_seed_have_pin;
void boot_seed_step(void);

/* Table A, CRT index 2. The eh-vector-constructor thunk at 0x00401101 (inside
 * FUN_00401051) walks ASCENDING over 100 elements of 0x120 bytes based at
 * 0x0052C978, with the EncInt at element offset +0xE8. `FUN_00401137` memsets
 * 0x7080 = 100 x 288 at the same base, which is where the 100 comes from. */
#define BOOT_TABLE_A_COUNT  100
#define BOOT_TABLE_A_STRIDE 0x120
#define BOOT_TABLE_A_OFFSET 0x0E8
void *boot_table_a_base(void);

#endif
